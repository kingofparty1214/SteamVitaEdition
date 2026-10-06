#include "steam_cm_client.h"

#include <curl/curl.h>
#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <psp2/kernel/rng.h>
#include <psp2/net/net.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <chrono>
#include <sstream>
#include <thread>

namespace {

constexpr const char* CM_TRACE_PATH = "ux0:data/SteamVita/cm_trace.log";
constexpr const char* CA_PATH = "ux0:data/SteamVita/cacert.pem";
constexpr const char* CM_LIST_URL =
    "https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/"
    "?cellid=0&cmtype=websockets&realm=steamglobal&maxcount=32&format=json";
constexpr std::size_t RESPONSE_LIMIT = 256u * 1024u;
constexpr std::size_t CM_FRAME_LIMIT = 2u * 1024u * 1024u;

constexpr std::uint32_t EMSG_MULTI = 1u;
constexpr std::uint32_t EMSG_CHANNEL_ENCRYPT_REQUEST = 1303u;
constexpr std::uint32_t EMSG_CHANNEL_ENCRYPT_RESPONSE = 1304u;
constexpr std::uint32_t EMSG_CHANNEL_ENCRYPT_RESULT = 1305u;
constexpr std::uint32_t EMSG_CLIENT_HELLO = 9805u;
constexpr std::uint32_t EMSG_CLIENT_LOGON = 5514u;
constexpr std::uint32_t EMSG_CLIENT_LOGON_RESPONSE = 751u;
constexpr std::uint32_t EMSG_CLIENT_LICENSE_LIST = 780u;
constexpr std::uint32_t EMSG_CLIENT_PICS_PRODUCT_INFO_REQUEST = 8903u;
constexpr std::uint32_t EMSG_CLIENT_PICS_PRODUCT_INFO_RESPONSE = 8904u;
constexpr std::uint32_t PROTO_MASK = 0x80000000u;
constexpr std::size_t MSG_HEADER_SIZE = 20u;

constexpr const char* STEAM_PUBLIC_KEY_PEM =
    "-----BEGIN PUBLIC KEY-----\n"
    "MIGdMA0GCSqGSIb3DQEBAQUAA4GLADCBhwKBgQDf7BrWLBBmLBc1OhSwfFkRf53T\n"
    "2Ct64+AVzRkeRuh7h3SiGEYxqQMUeYKO6UWiSRKpI2hzic9pobFhRr3Bvr/WARvY\n"
    "gdTckPv+T1JzZsuVcNfFjrocejN1oWI0Rrtgt4Bo+hOneoo3S57G9F1fOpn5nsQ6\n"
    "6WOiu4gZKODnFMBCiQIBEQ==\n"
    "-----END PUBLIC KEY-----\n";

struct CurlBuffer {
    std::string data;
    bool overflow = false;
};

void append_cm_trace(const std::string& line) {
    std::ofstream out(CM_TRACE_PATH, std::ios::app);
    if (out) out << line << "\n";
}

std::uint32_t read_le32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8u) |
           (static_cast<std::uint32_t>(p[2]) << 16u) |
           (static_cast<std::uint32_t>(p[3]) << 24u);
}

void append_le32(std::vector<unsigned char>* out, std::uint32_t value) {
    out->push_back(static_cast<unsigned char>(value & 0xffu));
    out->push_back(static_cast<unsigned char>((value >> 8u) & 0xffu));
    out->push_back(static_cast<unsigned char>((value >> 16u) & 0xffu));
    out->push_back(static_cast<unsigned char>((value >> 24u) & 0xffu));
}

void append_le64(std::vector<unsigned char>* out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out->push_back(static_cast<unsigned char>((value >> (i * 8u)) & 0xffu));
    }
}

void append_msg_header(std::vector<unsigned char>* out, std::uint32_t emsg) {
    append_le32(out, emsg);
    append_le64(out, UINT64_MAX);
    append_le64(out, UINT64_MAX);
}

std::uint64_t read_le64(const unsigned char* p) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(p[i]) << (i * 8u);
    }
    return value;
}

void append_varint(std::vector<unsigned char>* out, std::uint64_t value) {
    while (value >= 0x80u) {
        out->push_back(static_cast<unsigned char>((value & 0x7fu) | 0x80u));
        value >>= 7u;
    }
    out->push_back(static_cast<unsigned char>(value));
}

void append_proto_varint(std::vector<unsigned char>* out,
                         std::uint32_t field,
                         std::uint64_t value) {
    append_varint(out, (static_cast<std::uint64_t>(field) << 3u) | 0u);
    append_varint(out, value);
}

void append_proto_fixed64(std::vector<unsigned char>* out,
                          std::uint32_t field,
                          std::uint64_t value) {
    append_varint(out, (static_cast<std::uint64_t>(field) << 3u) | 1u);
    append_le64(out, value);
}

void append_proto_bytes(std::vector<unsigned char>* out,
                        std::uint32_t field,
                        const unsigned char* data,
                        std::size_t size) {
    append_varint(out, (static_cast<std::uint64_t>(field) << 3u) | 2u);
    append_varint(out, size);
    if (size > 0) out->insert(out->end(), data, data + size);
}

void append_proto_string(std::vector<unsigned char>* out,
                         std::uint32_t field,
                         const std::string& value) {
    append_proto_bytes(
        out, field,
        reinterpret_cast<const unsigned char*>(value.data()),
        value.size());
}

bool read_varint(const unsigned char* data,
                 std::size_t size,
                 std::size_t* offset,
                 std::uint64_t* value) {
    if (!data || !offset || !value) return false;
    std::uint64_t result = 0;
    unsigned shift = 0;
    while (*offset < size && shift <= 63u) {
        const unsigned char byte = data[(*offset)++];
        result |= static_cast<std::uint64_t>(byte & 0x7fu) << shift;
        if ((byte & 0x80u) == 0) {
            *value = result;
            return true;
        }
        shift += 7u;
    }
    return false;
}

bool skip_proto_field(const unsigned char* data,
                      std::size_t size,
                      std::size_t* offset,
                      unsigned wire_type) {
    if (!offset) return false;
    if (wire_type == 0u) {
        std::uint64_t ignored = 0;
        return read_varint(data, size, offset, &ignored);
    }
    if (wire_type == 1u) {
        if (*offset + 8u > size) return false;
        *offset += 8u;
        return true;
    }
    if (wire_type == 2u) {
        std::uint64_t length = 0;
        if (!read_varint(data, size, offset, &length) ||
            length > size - *offset) {
            return false;
        }
        *offset += static_cast<std::size_t>(length);
        return true;
    }
    if (wire_type == 5u) {
        if (*offset + 4u > size) return false;
        *offset += 4u;
        return true;
    }
    return false;
}

bool hmac_sha1(const unsigned char* key,
               std::size_t key_size,
               const unsigned char* data,
               std::size_t data_size,
               unsigned char output[20]) {
    const mbedtls_md_info_t* info =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    return info &&
           mbedtls_md_hmac(
               info, key, key_size, data, data_size, output) == 0;
}

bool symmetric_encrypt_hmac(
        const std::vector<unsigned char>& plain,
        const std::array<unsigned char, 32>& key,
        const std::array<unsigned char, 16>& hmac_secret,
        std::vector<unsigned char>* encrypted) {
    if (!encrypted) return false;

    unsigned char prefix[3]{};
    if (sceKernelGetRandomNumber(prefix, sizeof(prefix)) < 0) return false;

    std::vector<unsigned char> hmac_input;
    hmac_input.reserve(sizeof(prefix) + plain.size());
    hmac_input.insert(hmac_input.end(), prefix, prefix + sizeof(prefix));
    hmac_input.insert(hmac_input.end(), plain.begin(), plain.end());

    unsigned char digest[20]{};
    if (!hmac_sha1(
            hmac_secret.data(), hmac_secret.size(),
            hmac_input.data(), hmac_input.size(), digest)) {
        return false;
    }

    unsigned char iv[16]{};
    std::memcpy(iv, digest, 13u);
    std::memcpy(iv + 13u, prefix, 3u);

    std::vector<unsigned char> padded = plain;
    const unsigned char pad =
        static_cast<unsigned char>(16u - (padded.size() % 16u));
    padded.insert(padded.end(), pad, pad);

    encrypted->assign(16u + padded.size(), 0);

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_enc(&aes, key.data(), 256u) != 0 ||
        mbedtls_aes_crypt_ecb(
            &aes, MBEDTLS_AES_ENCRYPT, iv, encrypted->data()) != 0) {
        mbedtls_aes_free(&aes);
        return false;
    }

    unsigned char cbc_iv[16]{};
    std::memcpy(cbc_iv, iv, sizeof(cbc_iv));
    const int rc = mbedtls_aes_crypt_cbc(
        &aes, MBEDTLS_AES_ENCRYPT,
        padded.size(), cbc_iv,
        padded.data(), encrypted->data() + 16u);
    mbedtls_aes_free(&aes);
    return rc == 0;
}

bool symmetric_decrypt_hmac(
        const std::vector<unsigned char>& encrypted,
        const std::array<unsigned char, 32>& key,
        const std::array<unsigned char, 16>& hmac_secret,
        std::vector<unsigned char>* plain) {
    if (!plain || encrypted.size() < 32u ||
        ((encrypted.size() - 16u) % 16u) != 0u) {
        return false;
    }

    unsigned char iv[16]{};
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_dec(&aes, key.data(), 256u) != 0 ||
        mbedtls_aes_crypt_ecb(
            &aes, MBEDTLS_AES_DECRYPT,
            encrypted.data(), iv) != 0) {
        mbedtls_aes_free(&aes);
        return false;
    }

    std::vector<unsigned char> padded(encrypted.size() - 16u);
    unsigned char cbc_iv[16]{};
    std::memcpy(cbc_iv, iv, sizeof(cbc_iv));
    const int rc = mbedtls_aes_crypt_cbc(
        &aes, MBEDTLS_AES_DECRYPT,
        padded.size(), cbc_iv,
        encrypted.data() + 16u, padded.data());
    mbedtls_aes_free(&aes);
    if (rc != 0 || padded.empty()) return false;

    const unsigned char pad = padded.back();
    if (pad == 0u || pad > 16u || pad > padded.size()) return false;
    for (std::size_t i = 0; i < pad; ++i) {
        if (padded[padded.size() - 1u - i] != pad) return false;
    }
    padded.resize(padded.size() - pad);

    std::vector<unsigned char> hmac_input;
    hmac_input.reserve(3u + padded.size());
    hmac_input.insert(hmac_input.end(), iv + 13u, iv + 16u);
    hmac_input.insert(hmac_input.end(), padded.begin(), padded.end());

    unsigned char digest[20]{};
    if (!hmac_sha1(
            hmac_secret.data(), hmac_secret.size(),
            hmac_input.data(), hmac_input.size(), digest) ||
        std::memcmp(iv, digest, 13u) != 0) {
        return false;
    }

    plain->swap(padded);
    return true;
}

bool symmetric_encrypt_legacy(
        const std::vector<unsigned char>& plain,
        const std::array<unsigned char, 32>& key,
        std::vector<unsigned char>* encrypted) {
    if (!encrypted) return false;

    unsigned char iv[16]{};
    if (sceKernelGetRandomNumber(iv, sizeof(iv)) < 0) return false;

    std::vector<unsigned char> padded = plain;
    const unsigned char pad =
        static_cast<unsigned char>(16u - (padded.size() % 16u));
    padded.insert(padded.end(), pad, pad);

    encrypted->assign(16u + padded.size(), 0);

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_enc(&aes, key.data(), 256u) != 0 ||
        mbedtls_aes_crypt_ecb(
            &aes, MBEDTLS_AES_ENCRYPT, iv, encrypted->data()) != 0) {
        mbedtls_aes_free(&aes);
        return false;
    }

    unsigned char cbc_iv[16]{};
    std::memcpy(cbc_iv, iv, sizeof(cbc_iv));
    const int rc = mbedtls_aes_crypt_cbc(
        &aes, MBEDTLS_AES_ENCRYPT,
        padded.size(), cbc_iv,
        padded.data(), encrypted->data() + 16u);
    mbedtls_aes_free(&aes);
    return rc == 0;
}

bool symmetric_decrypt_legacy(
        const std::vector<unsigned char>& encrypted,
        const std::array<unsigned char, 32>& key,
        std::vector<unsigned char>* plain) {
    if (!plain || encrypted.size() < 32u ||
        ((encrypted.size() - 16u) % 16u) != 0u) {
        return false;
    }

    unsigned char iv[16]{};
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_dec(&aes, key.data(), 256u) != 0 ||
        mbedtls_aes_crypt_ecb(
            &aes, MBEDTLS_AES_DECRYPT,
            encrypted.data(), iv) != 0) {
        mbedtls_aes_free(&aes);
        return false;
    }

    std::vector<unsigned char> padded(encrypted.size() - 16u);
    unsigned char cbc_iv[16]{};
    std::memcpy(cbc_iv, iv, sizeof(cbc_iv));
    const int rc = mbedtls_aes_crypt_cbc(
        &aes, MBEDTLS_AES_DECRYPT,
        padded.size(), cbc_iv,
        encrypted.data() + 16u, padded.data());
    mbedtls_aes_free(&aes);
    if (rc != 0 || padded.empty()) return false;

    const unsigned char pad = padded.back();
    if (pad == 0u || pad > 16u || pad > padded.size()) return false;
    for (std::size_t i = 0; i < pad; ++i) {
        if (padded[padded.size() - 1u - i] != pad) return false;
    }
    padded.resize(padded.size() - pad);
    plain->swap(padded);
    return true;
}


std::vector<unsigned char> make_proto_message(
        std::uint32_t emsg,
        std::uint64_t steam_id,
        std::int32_t session_id,
        const std::vector<unsigned char>& body,
        std::uint64_t source_job_id = 0) {
    std::vector<unsigned char> header;
    if (steam_id != 0) append_proto_fixed64(&header, 1u, steam_id);
    if (session_id != 0) {
        append_proto_varint(
            &header, 2u,
            static_cast<std::uint32_t>(session_id));
    }
    if (source_job_id != 0) {
        append_proto_fixed64(&header, 10u, source_job_id);
    }

    std::vector<unsigned char> message;
    message.reserve(8u + header.size() + body.size());
    append_le32(&message, emsg | PROTO_MASK);
    append_le32(&message, static_cast<std::uint32_t>(header.size()));
    message.insert(message.end(), header.begin(), header.end());
    message.insert(message.end(), body.begin(), body.end());
    return message;
}

bool split_proto_message(
        const std::vector<unsigned char>& message,
        std::uint32_t* emsg,
        const unsigned char** header,
        std::size_t* header_size,
        const unsigned char** body,
        std::size_t* body_size) {
    if (message.size() < 8u || !emsg || !header || !header_size ||
        !body || !body_size) {
        return false;
    }

    const std::uint32_t raw = read_le32(message.data());
    if ((raw & PROTO_MASK) == 0) return false;

    const std::uint32_t proto_size = read_le32(message.data() + 4u);
    if (proto_size > message.size() - 8u) return false;

    *emsg = raw & ~PROTO_MASK;
    *header = message.data() + 8u;
    *header_size = proto_size;
    *body = message.data() + 8u + proto_size;
    *body_size = message.size() - 8u - proto_size;
    return true;
}

bool parse_proto_header_session(
        const unsigned char* data,
        std::size_t size,
        std::uint64_t* steam_id,
        std::int32_t* session_id) {
    std::size_t offset = 0;
    while (offset < size) {
        std::uint64_t tag = 0;
        if (!read_varint(data, size, &offset, &tag)) return false;
        const std::uint32_t field = static_cast<std::uint32_t>(tag >> 3u);
        const unsigned wire = static_cast<unsigned>(tag & 7u);

        if (field == 1u && wire == 1u) {
            if (offset + 8u > size) return false;
            if (steam_id) *steam_id = read_le64(data + offset);
            offset += 8u;
        } else if (field == 2u && wire == 0u) {
            std::uint64_t value = 0;
            if (!read_varint(data, size, &offset, &value)) return false;
            if (session_id) {
                *session_id =
                    static_cast<std::int32_t>(
                        static_cast<std::uint32_t>(value));
            }
        } else if (!skip_proto_field(data, size, &offset, wire)) {
            return false;
        }
    }
    return true;
}

bool parse_eresult(
        const unsigned char* data,
        std::size_t size,
        std::uint32_t* eresult) {
    std::size_t offset = 0;
    while (offset < size) {
        std::uint64_t tag = 0;
        if (!read_varint(data, size, &offset, &tag)) return false;
        const std::uint32_t field = static_cast<std::uint32_t>(tag >> 3u);
        const unsigned wire = static_cast<unsigned>(tag & 7u);
        if (field == 1u && wire == 0u) {
            std::uint64_t value = 0;
            if (!read_varint(data, size, &offset, &value)) return false;
            if (eresult) *eresult = static_cast<std::uint32_t>(value);
            return true;
        }
        if (!skip_proto_field(data, size, &offset, wire)) return false;
    }
    return false;
}

bool parse_license(
        const unsigned char* data,
        std::size_t size,
        SteamCmLicense* license) {
    if (!license) return false;
    std::size_t offset = 0;
    while (offset < size) {
        std::uint64_t tag = 0;
        if (!read_varint(data, size, &offset, &tag)) return false;
        const std::uint32_t field = static_cast<std::uint32_t>(tag >> 3u);
        const unsigned wire = static_cast<unsigned>(tag & 7u);

        if (wire == 0u &&
            (field == 1u || field == 9u || field == 12u ||
             field == 17u || field == 18u)) {
            std::uint64_t value = 0;
            if (!read_varint(data, size, &offset, &value)) return false;
            if (field == 1u) license->package_id = static_cast<std::uint32_t>(value);
            else if (field == 9u) license->license_type = static_cast<std::uint32_t>(value);
            else if (field == 12u) license->owner_id = static_cast<std::uint32_t>(value);
            else if (field == 17u) license->access_token = value;
            else if (field == 18u) license->master_package_id = static_cast<std::uint32_t>(value);
        } else if (!skip_proto_field(data, size, &offset, wire)) {
            return false;
        }
    }
    return license->package_id != 0;
}

bool parse_license_list(
        const unsigned char* data,
        std::size_t size,
        std::vector<SteamCmLicense>* licenses,
        std::uint32_t* eresult) {
    if (!licenses) return false;
    licenses->clear();

    std::size_t offset = 0;
    while (offset < size) {
        std::uint64_t tag = 0;
        if (!read_varint(data, size, &offset, &tag)) return false;
        const std::uint32_t field = static_cast<std::uint32_t>(tag >> 3u);
        const unsigned wire = static_cast<unsigned>(tag & 7u);

        if (field == 1u && wire == 0u) {
            std::uint64_t value = 0;
            if (!read_varint(data, size, &offset, &value)) return false;
            if (eresult) *eresult = static_cast<std::uint32_t>(value);
        } else if (field == 2u && wire == 2u) {
            std::uint64_t length = 0;
            if (!read_varint(data, size, &offset, &length) ||
                length > size - offset) {
                return false;
            }
            SteamCmLicense license;
            if (parse_license(
                    data + offset,
                    static_cast<std::size_t>(length),
                    &license)) {
                licenses->push_back(license);
            }
            offset += static_cast<std::size_t>(length);
        } else if (!skip_proto_field(data, size, &offset, wire)) {
            return false;
        }
    }

    return true;
}


bool parse_multi_body(
        const unsigned char* data,
        std::size_t size,
        std::vector<unsigned char>* packed,
        std::uint32_t* size_unzipped) {
    if (!data || !packed || !size_unzipped) return false;
    packed->clear();
    *size_unzipped = 0;

    std::size_t offset = 0;
    while (offset < size) {
        std::uint64_t tag = 0;
        if (!read_varint(data, size, &offset, &tag)) return false;

        const std::uint32_t field =
            static_cast<std::uint32_t>(tag >> 3u);
        const unsigned wire =
            static_cast<unsigned>(tag & 7u);

        if (field == 1u && wire == 0u) {
            std::uint64_t value = 0;
            if (!read_varint(data, size, &offset, &value) ||
                value > 0xffffffffull) {
                return false;
            }
            *size_unzipped = static_cast<std::uint32_t>(value);
        } else if (field == 2u && wire == 2u) {
            std::uint64_t length = 0;
            if (!read_varint(data, size, &offset, &length) ||
                length > size - offset) {
                return false;
            }
            packed->assign(
                data + offset,
                data + offset + static_cast<std::size_t>(length));
            offset += static_cast<std::size_t>(length);
        } else if (!skip_proto_field(data, size, &offset, wire)) {
            return false;
        }
    }

    return !packed->empty();
}

bool gunzip_multi_payload(
        const std::vector<unsigned char>& compressed,
        std::uint32_t expected_size,
        std::vector<unsigned char>* output) {
    if (!output || expected_size == 0 ||
        expected_size > CM_FRAME_LIMIT * 8u) {
        return false;
    }

    output->assign(expected_size, 0);

    z_stream stream{};
    stream.next_in =
        const_cast<Bytef*>(
            reinterpret_cast<const Bytef*>(compressed.data()));
    stream.avail_in =
        static_cast<uInt>(compressed.size());
    stream.next_out =
        reinterpret_cast<Bytef*>(output->data());
    stream.avail_out =
        static_cast<uInt>(output->size());

    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
        output->clear();
        return false;
    }

    const int rc = inflate(&stream, Z_FINISH);
    const bool ok =
        rc == Z_STREAM_END &&
        stream.total_out == expected_size;
    inflateEnd(&stream);

    if (!ok) output->clear();
    return ok;
}

bool unpack_multi_records(
        const std::vector<unsigned char>& message,
        std::deque<std::vector<unsigned char>>* out) {
    if (!out) return false;

    std::uint32_t emsg = 0;
    const unsigned char* header = nullptr;
    std::size_t header_size = 0;
    const unsigned char* body = nullptr;
    std::size_t body_size = 0;

    if (!split_proto_message(
            message,
            &emsg,
            &header,
            &header_size,
            &body,
            &body_size) ||
        emsg != EMSG_MULTI) {
        out->push_back(message);
        return true;
    }

    std::vector<unsigned char> packed;
    std::uint32_t size_unzipped = 0;
    if (!parse_multi_body(
            body, body_size,
            &packed, &size_unzipped)) {
        return false;
    }

    std::vector<unsigned char> records;
    if (size_unzipped > 0) {
        if (!gunzip_multi_payload(
                packed, size_unzipped, &records)) {
            return false;
        }
    } else {
        records.swap(packed);
    }

    std::size_t offset = 0;
    while (offset + 4u <= records.size()) {
        const std::uint32_t length =
            read_le32(records.data() + offset);
        offset += 4u;

        if (length == 0u ||
            length > records.size() - offset ||
            length > CM_FRAME_LIMIT) {
            return false;
        }

        std::vector<unsigned char> inner(
            records.begin() + offset,
            records.begin() + offset + length);
        offset += length;

        if (!unpack_multi_records(inner, out)) {
            return false;
        }
    }

    return offset == records.size();
}


bool read_cstring(
        const unsigned char* data,
        std::size_t size,
        std::size_t* offset,
        std::string* value) {
    if (!data || !offset || !value || *offset >= size) return false;
    const std::size_t start = *offset;
    while (*offset < size && data[*offset] != 0) ++(*offset);
    if (*offset >= size) return false;
    value->assign(
        reinterpret_cast<const char*>(data + start),
        *offset - start);
    ++(*offset);
    return true;
}

bool skip_wide_cstring(
        const unsigned char* data,
        std::size_t size,
        std::size_t* offset) {
    if (!data || !offset) return false;
    while (*offset + 1u < size) {
        if (data[*offset] == 0 && data[*offset + 1u] == 0) {
            *offset += 2u;
            return true;
        }
        *offset += 2u;
    }
    return false;
}

bool parse_binary_vdf_object(
        const unsigned char* data,
        std::size_t size,
        std::size_t* offset,
        int depth,
        bool in_appids,
        std::vector<std::uint32_t>* app_ids) {
    if (!data || !offset || !app_ids || depth > 16) return false;

    while (*offset < size) {
        const unsigned char type = data[(*offset)++];
        if (type == 0x08u) return true;

        std::string key;
        if (!read_cstring(data, size, offset, &key)) return false;

        if (type == 0x00u) {
            if (!parse_binary_vdf_object(
                    data, size, offset, depth + 1,
                    in_appids || key == "appids",
                    app_ids)) {
                return false;
            }
        } else if (type == 0x01u) {
            std::string ignored;
            if (!read_cstring(data, size, offset, &ignored)) return false;
        } else if (type == 0x02u || type == 0x04u || type == 0x06u) {
            if (*offset + 4u > size) return false;
            const std::uint32_t value = read_le32(data + *offset);
            if (in_appids && type == 0x02u && value != 0u) {
                app_ids->push_back(value);
            }
            *offset += 4u;
        } else if (type == 0x03u) {
            if (*offset + 4u > size) return false;
            *offset += 4u;
        } else if (type == 0x05u) {
            if (!skip_wide_cstring(data, size, offset)) return false;
        } else if (type == 0x07u || type == 0x0au) {
            if (*offset + 8u > size) return false;
            *offset += 8u;
        } else {
            return false;
        }
    }

    return depth == 0;
}

bool extract_package_app_ids(
        const unsigned char* buffer,
        std::size_t buffer_size,
        std::vector<std::uint32_t>* app_ids) {
    if (!buffer || !app_ids || buffer_size <= 4u) return false;
    app_ids->clear();

    std::size_t offset = 4u;
    if (!parse_binary_vdf_object(
            buffer, buffer_size, &offset, 0, false, app_ids)) {
        return false;
    }

    std::sort(app_ids->begin(), app_ids->end());
    app_ids->erase(
        std::unique(app_ids->begin(), app_ids->end()),
        app_ids->end());
    return true;
}

struct PicsPackageResult {
    std::uint32_t package_id = 0;
    bool missing_token = false;
    std::vector<std::uint32_t> app_ids;
};

bool parse_pics_package_info(
        const unsigned char* data,
        std::size_t size,
        PicsPackageResult* result) {
    if (!data || !result) return false;

    std::size_t offset = 0;
    const unsigned char* buffer = nullptr;
    std::size_t buffer_size = 0;

    while (offset < size) {
        std::uint64_t tag = 0;
        if (!read_varint(data, size, &offset, &tag)) return false;
        const std::uint32_t field =
            static_cast<std::uint32_t>(tag >> 3u);
        const unsigned wire = static_cast<unsigned>(tag & 7u);

        if ((field == 1u || field == 3u) && wire == 0u) {
            std::uint64_t value = 0;
            if (!read_varint(data, size, &offset, &value)) return false;
            if (field == 1u) {
                result->package_id =
                    static_cast<std::uint32_t>(value);
            } else {
                result->missing_token = value != 0;
            }
        } else if (field == 5u && wire == 2u) {
            std::uint64_t length = 0;
            if (!read_varint(data, size, &offset, &length) ||
                length > size - offset) {
                return false;
            }
            buffer = data + offset;
            buffer_size = static_cast<std::size_t>(length);
            offset += buffer_size;
        } else if (!skip_proto_field(data, size, &offset, wire)) {
            return false;
        }
    }

    if (result->missing_token || !buffer || buffer_size <= 4u) {
        return result->package_id != 0;
    }

    return result->package_id != 0 &&
           extract_package_app_ids(
               buffer, buffer_size, &result->app_ids);
}

bool parse_pics_product_response(
        const unsigned char* data,
        std::size_t size,
        std::vector<PicsPackageResult>* packages,
        bool* response_pending) {
    if (!data || !packages || !response_pending) return false;
    packages->clear();
    *response_pending = false;

    std::size_t offset = 0;
    while (offset < size) {
        std::uint64_t tag = 0;
        if (!read_varint(data, size, &offset, &tag)) return false;
        const std::uint32_t field =
            static_cast<std::uint32_t>(tag >> 3u);
        const unsigned wire = static_cast<unsigned>(tag & 7u);

        if (field == 3u && wire == 2u) {
            std::uint64_t length = 0;
            if (!read_varint(data, size, &offset, &length) ||
                length > size - offset) {
                return false;
            }

            PicsPackageResult result;
            if (!parse_pics_package_info(
                    data + offset,
                    static_cast<std::size_t>(length),
                    &result)) {
                return false;
            }
            packages->push_back(std::move(result));
            offset += static_cast<std::size_t>(length);
        } else if (field == 6u && wire == 0u) {
            std::uint64_t value = 0;
            if (!read_varint(data, size, &offset, &value)) return false;
            *response_pending = value != 0;
        } else if (!skip_proto_field(data, size, &offset, wire)) {
            return false;
        }
    }

    return true;
}


std::string parse_quoted_value(
        const std::string& text,
        std::size_t quote_pos) {
    if (quote_pos >= text.size() || text[quote_pos] != '"') return {};
    std::string value;
    bool escape = false;
    for (std::size_t i = quote_pos + 1u; i < text.size(); ++i) {
        const char ch = text[i];
        if (escape) {
            if (ch == 'n') value.push_back('\n');
            else if (ch == 't') value.push_back('\t');
            else value.push_back(ch);
            escape = false;
        } else if (ch == '\\') {
            escape = true;
        } else if (ch == '"') {
            return value;
        } else {
            value.push_back(ch);
        }
    }
    return {};
}

std::string extract_text_vdf_common_name(
        const unsigned char* buffer,
        std::size_t size) {
    if (!buffer || size == 0) return {};

    std::string text(
        reinterpret_cast<const char*>(buffer),
        size);
    while (!text.empty() && text.back() == '\0') text.pop_back();

    const std::size_t common = text.find("\"common\"");
    if (common == std::string::npos) return {};

    const std::size_t open = text.find('{', common);
    if (open == std::string::npos) return {};

    bool in_string = false;
    bool escape = false;
    int depth = 1;
    std::size_t close = std::string::npos;

    for (std::size_t i = open + 1u; i < text.size(); ++i) {
        const char ch = text[i];
        if (in_string) {
            if (escape) escape = false;
            else if (ch == '\\') escape = true;
            else if (ch == '"') in_string = false;
            continue;
        }
        if (ch == '"') in_string = true;
        else if (ch == '{') ++depth;
        else if (ch == '}') {
            --depth;
            if (depth == 0) {
                close = i;
                break;
            }
        }
    }

    if (close == std::string::npos || close <= open + 1u) return {};

    const std::string common_block =
        text.substr(open + 1u, close - open - 1u);
    const std::size_t name_key = common_block.find("\"name\"");
    if (name_key == std::string::npos) return {};

    const std::size_t value_quote =
        common_block.find('"', name_key + 6u);
    if (value_quote == std::string::npos) return {};

    return parse_quoted_value(common_block, value_quote);
}

struct PicsAppResult {
    std::uint32_t app_id = 0;
    bool missing_token = false;
    std::string name;
};

bool parse_pics_app_info(
        const unsigned char* data,
        std::size_t size,
        PicsAppResult* result) {
    if (!data || !result) return false;

    std::size_t offset = 0;
    const unsigned char* buffer = nullptr;
    std::size_t buffer_size = 0;

    while (offset < size) {
        std::uint64_t tag = 0;
        if (!read_varint(data, size, &offset, &tag)) return false;
        const std::uint32_t field =
            static_cast<std::uint32_t>(tag >> 3u);
        const unsigned wire = static_cast<unsigned>(tag & 7u);

        if ((field == 1u || field == 3u) && wire == 0u) {
            std::uint64_t value = 0;
            if (!read_varint(data, size, &offset, &value)) return false;
            if (field == 1u) {
                result->app_id = static_cast<std::uint32_t>(value);
            } else {
                result->missing_token = value != 0;
            }
        } else if (field == 5u && wire == 2u) {
            std::uint64_t length = 0;
            if (!read_varint(data, size, &offset, &length) ||
                length > size - offset) {
                return false;
            }
            buffer = data + offset;
            buffer_size = static_cast<std::size_t>(length);
            offset += buffer_size;
        } else if (!skip_proto_field(data, size, &offset, wire)) {
            return false;
        }
    }

    if (buffer && buffer_size > 0) {
        result->name =
            extract_text_vdf_common_name(buffer, buffer_size);
    }
    return result->app_id != 0;
}

bool parse_pics_app_response(
        const unsigned char* data,
        std::size_t size,
        std::vector<PicsAppResult>* apps,
        bool* response_pending) {
    if (!data || !apps || !response_pending) return false;
    apps->clear();
    *response_pending = false;

    std::size_t offset = 0;
    while (offset < size) {
        std::uint64_t tag = 0;
        if (!read_varint(data, size, &offset, &tag)) return false;
        const std::uint32_t field =
            static_cast<std::uint32_t>(tag >> 3u);
        const unsigned wire = static_cast<unsigned>(tag & 7u);

        if (field == 1u && wire == 2u) {
            std::uint64_t length = 0;
            if (!read_varint(data, size, &offset, &length) ||
                length > size - offset) {
                return false;
            }

            PicsAppResult result;
            if (!parse_pics_app_info(
                    data + offset,
                    static_cast<std::size_t>(length),
                    &result)) {
                return false;
            }
            apps->push_back(std::move(result));
            offset += static_cast<std::size_t>(length);
        } else if (field == 6u && wire == 0u) {
            std::uint64_t value = 0;
            if (!read_varint(data, size, &offset, &value)) return false;
            *response_pending = value != 0;
        } else if (!skip_proto_field(data, size, &offset, wire)) {
            return false;
        }
    }

    return true;
}

int vita_rng(void*, unsigned char* output, std::size_t len) {
    return sceKernelGetRandomNumber(output, static_cast<int>(len));
}

std::size_t write_response(void* ptr,
                           std::size_t size,
                           std::size_t count,
                           void* userdata) {
    auto* buffer = static_cast<CurlBuffer*>(userdata);
    if (!buffer) return 0;

    const std::size_t bytes = size * count;
    if (bytes > RESPONSE_LIMIT ||
        buffer->data.size() > RESPONSE_LIMIT - bytes) {
        buffer->overflow = true;
        return 0;
    }

    buffer->data.append(static_cast<const char*>(ptr), bytes);
    return bytes;
}

int cancel_progress(void* userdata,
                    curl_off_t,
                    curl_off_t,
                    curl_off_t,
                    curl_off_t) {
    auto* cancelled = static_cast<std::atomic<bool>*>(userdata);
    return cancelled && cancelled->load() ? 1 : 0;
}

bool parse_endpoint(const std::string& value, SteamCmEndpoint* endpoint) {
    if (!endpoint || value.empty()) return false;

    const std::size_t colon = value.find_last_of(':');
    if (colon == std::string::npos || colon == 0 ||
        colon + 1 >= value.size()) {
        return false;
    }

    char* end = nullptr;
    const long parsed_port =
        std::strtol(value.c_str() + colon + 1, &end, 10);
    if (!end || *end != '\0' || parsed_port <= 0 ||
        parsed_port > 65535) {
        return false;
    }

    endpoint->host = value.substr(0, colon);
    endpoint->port = static_cast<std::uint16_t>(parsed_port);
    return !endpoint->host.empty();
}

bool parse_server_list(const std::string& json,
                       std::vector<SteamCmEndpoint>* servers) {
    if (!servers) return false;

    const std::size_t key = json.find("\"serverlist\"");
    if (key == std::string::npos) return false;

    const std::size_t begin = json.find('[', key);
    if (begin == std::string::npos) return false;

    const std::size_t end = json.find(']', begin + 1);
    if (end == std::string::npos) return false;

    std::size_t pos = begin + 1;
    while (pos < end) {
        const std::size_t endpoint_key =
            json.find("\"endpoint\"", pos);
        if (endpoint_key == std::string::npos ||
            endpoint_key >= end) {
            break;
        }

        const std::size_t colon = json.find(':', endpoint_key + 10u);
        if (colon == std::string::npos || colon >= end) return false;

        const std::size_t quote = json.find('"', colon + 1u);
        if (quote == std::string::npos || quote >= end) return false;

        const std::size_t close = json.find('"', quote + 1u);
        if (close == std::string::npos || close > end) return false;

        SteamCmEndpoint endpoint;
        if (parse_endpoint(
                json.substr(quote + 1u, close - quote - 1u),
                &endpoint)) {
            servers->push_back(std::move(endpoint));
        }

        pos = close + 1u;
    }

    return !servers->empty();
}

bool rsa_encrypt_session(const unsigned char session_key[32],
                         const unsigned char* challenge,
                         std::size_t challenge_size,
                         unsigned char encrypted[128],
                         std::string* error_message) {
    if (challenge_size > 64u) {
        if (error_message) *error_message = "Steam CM challenge was unexpectedly large.";
        return false;
    }

    unsigned char plaintext[96]{};
    std::memcpy(plaintext, session_key, 32u);
    if (challenge_size > 0) {
        std::memcpy(plaintext + 32u, challenge, challenge_size);
    }
    const std::size_t plaintext_size = 32u + challenge_size;

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    const int parse = mbedtls_pk_parse_public_key(
        &pk,
        reinterpret_cast<const unsigned char*>(STEAM_PUBLIC_KEY_PEM),
        std::strlen(STEAM_PUBLIC_KEY_PEM) + 1u);
    if (parse != 0 || !mbedtls_pk_can_do(&pk, MBEDTLS_PK_RSA)) {
        mbedtls_pk_free(&pk);
        if (error_message) *error_message = "Could not load Steam CM public key.";
        return false;
    }

    mbedtls_rsa_context* rsa = mbedtls_pk_rsa(pk);
    mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);

    std::size_t encrypted_size = 0;
    const int encrypted_result = mbedtls_pk_encrypt(
        &pk,
        plaintext,
        plaintext_size,
        encrypted,
        &encrypted_size,
        128u,
        vita_rng,
        nullptr);

    mbedtls_pk_free(&pk);

    if (encrypted_result != 0 || encrypted_size != 128u) {
        if (error_message) *error_message = "Could not encrypt the Steam CM session key.";
        return false;
    }
    return true;
}

} // namespace

bool discover_steam_cm_servers(std::vector<SteamCmEndpoint>* servers,
                               std::atomic<bool>* cancelled,
                               std::string* error_message) {
    if (!servers) return false;
    servers->clear();

    CURL* curl = curl_easy_init();
    if (!curl) {
        if (error_message) {
            *error_message = "Could not initialize Steam CM discovery.";
        }
        return false;
    }

    CurlBuffer buffer;
    const bool configured =
        curl_easy_setopt(curl, CURLOPT_URL, CM_LIST_URL) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "SteamVita/0.13") == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_CAINFO, CA_PATH) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity") == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cancel_progress) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancelled) == CURLE_OK;

    CURLcode result = CURLE_FAILED_INIT;
    long status = 0;
    if (configured) {
        result = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }
    curl_easy_cleanup(curl);

    if (cancelled && cancelled->load()) {
        if (error_message) *error_message = "Install cancelled.";
        return false;
    }

    if (!configured || result != CURLE_OK ||
        status != 200 || buffer.overflow) {
        if (error_message) {
            std::ostringstream message;
            message << "Steam CM discovery failed (HTTP "
                    << status << ").";
            *error_message = message.str();
        }
        return false;
    }

    if (!parse_server_list(buffer.data, servers)) {
        if (error_message) {
            *error_message = "Steam returned no usable CM servers.";
        }
        return false;
    }

    return true;
}

SteamCmConnection::SteamCmConnection() = default;

SteamCmConnection::~SteamCmConnection() {
    close();
}

void SteamCmConnection::close() {
    if (websocket_) {
        curl_easy_cleanup(static_cast<CURL*>(websocket_));
        websocket_ = nullptr;
    }
    if (socket_ >= 0) {
        sceNetSocketClose(socket_);
        socket_ = -1;
    }
    endpoint_ = {};
    session_key_.fill(0);
    hmac_secret_.fill(0);
    hmac_mode_ = false;
    steam_id_ = 0;
    session_id_ = 0;
    pending_messages_.clear();
}

bool SteamCmConnection::connected() const {
    return websocket_ != nullptr || socket_ >= 0;
}

const SteamCmEndpoint& SteamCmConnection::endpoint() const {
    return endpoint_;
}

const std::array<unsigned char, 32>& SteamCmConnection::session_key() const {
    return session_key_;
}

const std::array<unsigned char, 16>& SteamCmConnection::hmac_secret() const {
    return hmac_secret_;
}

bool SteamCmConnection::connect_secure(
        const std::vector<SteamCmEndpoint>& servers,
        std::atomic<bool>* cancelled,
        std::string* error_message) {
    close();

    std::string last_error = "No Steam CM servers were available.";
    const std::size_t attempts = std::min<std::size_t>(servers.size(), 4u);
    for (std::size_t i = 0; i < attempts; ++i) {
        if (cancelled && cancelled->load()) {
            if (error_message) *error_message = "Install cancelled.";
            return false;
        }

        close();
        std::string attempt_error;
        if (connect_one(servers[i], cancelled, &attempt_error)) {
            return true;
        }
        if (!attempt_error.empty()) last_error = attempt_error;
    }

    if (error_message) *error_message = last_error;
    return false;
}

bool SteamCmConnection::logon_and_fetch_licenses(
        const std::string& access_token,
        const std::string& account_name,
        std::uint64_t steam_id,
        std::vector<SteamCmLicense>* licenses,
        std::atomic<bool>* cancelled,
        std::string* error_message) {
    if (!licenses) return false;
    licenses->clear();

    if (!connected() || access_token.empty() || steam_id == 0) {
        if (error_message) {
            *error_message = "Steam CM session is not ready for account logon.";
        }
        return false;
    }

    // Modern Steam clients introduce themselves before account logon.
    std::vector<unsigned char> hello_body;
    append_proto_varint(&hello_body, 1u, 65581u);
    const std::vector<unsigned char> hello =
        make_proto_message(EMSG_CLIENT_HELLO, 0, 0, hello_body);
    append_cm_trace("Sending ClientHello");
    if (!send_encrypted(hello, error_message)) {
        if (error_message && !error_message->empty()) {
            *error_message = "ClientHello failed: " + *error_message;
        }
        return false;
    }

    unsigned char client_seed[16]{};
    if (sceKernelGetRandomNumber(
            client_seed, sizeof(client_seed)) < 0) {
        if (error_message) {
            *error_message = "Could not create Steam client login identity.";
        }
        return false;
    }
    std::uint64_t client_instance_id = read_le64(client_seed);
    if (client_instance_id == 0) client_instance_id = 1;

    std::uint32_t login_id = read_le32(client_seed + 8u);
    if (login_id == 0) login_id = 0x53565401u;

    std::vector<unsigned char> body;
    append_proto_varint(&body, 1u, 65580u);
    append_proto_varint(&body, 3u, 0u);
    append_proto_varint(&body, 5u, 1771u);
    append_proto_string(&body, 6u, "english");
    append_proto_varint(&body, 7u, 16u);
    append_proto_varint(&body, 8u, 1u);
    append_proto_varint(&body, 21u, 2u);
    append_proto_fixed64(&body, 22u, steam_id);

    static const unsigned char machine_id[] = "SteamVita";
    append_proto_bytes(
        &body, 30u,
        machine_id, sizeof(machine_id) - 1u);

    // Match the known-good WSS client login identity fields.
    append_proto_varint(&body, 31u, login_id);
    std::vector<unsigned char> private_ip;
    append_proto_varint(&private_ip, 1u, login_id);
    append_proto_bytes(
        &body, 95u,
        private_ip.data(), private_ip.size());

    append_proto_varint(&body, 32u, 7u);
    append_proto_varint(&body, 33u, 2u);
    if (!account_name.empty()) {
        append_proto_string(&body, 50u, account_name);
    }
    append_proto_string(&body, 96u, "SteamVita");
    append_proto_varint(&body, 100u, client_instance_id);
    append_proto_varint(&body, 102u, 1u);
    append_proto_string(&body, 108u, access_token);

    // Steam uses this placeholder in the envelope until ClientLogOnResponse
    // assigns the real session identity.
    constexpr std::uint64_t LOGON_HEADER_STEAM_ID =
        0x0110000100000000ull;
    const std::vector<unsigned char> logon =
        make_proto_message(
            EMSG_CLIENT_LOGON,
            LOGON_HEADER_STEAM_ID,
            0,
            body);
    append_cm_trace("Sending ClientLogon");
    if (!send_encrypted(logon, error_message)) {
        if (error_message && !error_message->empty()) {
            *error_message = "ClientLogon send failed: " + *error_message;
        }
        return false;
    }

    bool logged_on = false;
    bool got_licenses = false;
    const std::uint32_t own_account_id =
        static_cast<std::uint32_t>(steam_id & 0xffffffffull);

    for (int message_index = 0; message_index < 64; ++message_index) {
        if (cancelled && cancelled->load()) {
            if (error_message) *error_message = "Install cancelled.";
            return false;
        }

        std::vector<unsigned char> message;
        if (!receive_encrypted(
                &message, cancelled, error_message)) {
            if (error_message && !error_message->empty()) {
                *error_message =
                    std::string(logged_on
                        ? "Steam CM disconnected while waiting for licenses: "
                        : "Steam CM disconnected during account logon: ") +
                    *error_message;
            }
            return false;
        }

        std::uint32_t emsg = 0;
        const unsigned char* header = nullptr;
        std::size_t header_size = 0;
        const unsigned char* message_body = nullptr;
        std::size_t body_size = 0;

        if (!split_proto_message(
                message, &emsg,
                &header, &header_size,
                &message_body, &body_size)) {
            append_cm_trace("Inbound non-protobuf/parse-failed message");
            continue;
        }

        {
            std::ostringstream trace;
            trace << "Inbound EMsg " << emsg;
            append_cm_trace(trace.str());
        }

        if (emsg == EMSG_CLIENT_LOGON_RESPONSE) {
            std::uint32_t eresult = 0;
            if (!parse_eresult(
                    message_body, body_size, &eresult)) {
                if (error_message) {
                    *error_message =
                        "Steam CM logon response could not be parsed.";
                }
                return false;
            }
            if (eresult != 1u) {
                if (error_message) {
                    std::ostringstream out;
                    out << "Steam CM account logon failed (EResult "
                        << eresult << ").";
                    *error_message = out.str();
                }
                return false;
            }

            std::uint64_t returned_steam_id = steam_id;
            std::int32_t returned_session_id = 0;
            if (!parse_proto_header_session(
                    header, header_size,
                    &returned_steam_id,
                    &returned_session_id)) {
                if (error_message) {
                    *error_message =
                        "Steam CM logon header could not be parsed.";
                }
                return false;
            }

            steam_id_ = returned_steam_id != 0
                ? returned_steam_id
                : steam_id;
            session_id_ = returned_session_id;
            logged_on = true;
        } else if (emsg == EMSG_CLIENT_LICENSE_LIST) {
            std::uint32_t eresult = 0;
            if (!parse_license_list(
                    message_body, body_size,
                    licenses, &eresult)) {
                if (error_message) {
                    *error_message =
                        "Steam CM license list could not be parsed.";
                }
                return false;
            }
            if (eresult != 0u && eresult != 1u) {
                if (error_message) {
                    std::ostringstream out;
                    out << "Steam CM license list failed (EResult "
                        << eresult << ").";
                    *error_message = out.str();
                }
                return false;
            }
            got_licenses = true;
        }

        if (logged_on && got_licenses) {
            std::size_t shared_count = 0;
            for (const SteamCmLicense& license : *licenses) {
                if (license.owner_id != 0 &&
                    license.owner_id != own_account_id) {
                    ++shared_count;
                }
            }

            if (error_message) {
                std::ostringstream out;
                out << "Steam CM returned "
                    << licenses->size() << " package licenses, "
                    << shared_count << " shared.";
                *error_message = out.str();
            }
            return true;
        }
    }

    if (error_message) {
        *error_message = logged_on
            ? "Steam CM logged in but did not send a license list."
            : "Steam CM did not complete account logon.";
    }
    return false;
}

bool SteamCmConnection::fetch_shared_package_apps(
        const std::vector<SteamCmLicense>& licenses,
        std::uint64_t steam_id,
        std::vector<SteamCmSharedApp>* apps,
        std::atomic<bool>* cancelled,
        std::string* error_message,
        bool shared_only) {
    if (!apps) return false;
    apps->clear();

    if (!connected() || steam_id_ == 0 || session_id_ == 0) {
        if (error_message) {
            *error_message =
                "Steam CM account session is not ready for PICS.";
        }
        return false;
    }

    const std::uint32_t own_account_id =
        static_cast<std::uint32_t>(steam_id & 0xffffffffull);

    std::vector<SteamCmLicense> shared;
    for (const SteamCmLicense& license : licenses) {
        if (license.package_id == 0 ||
            license.access_token == 0) {
            continue;
        }

        const bool is_shared =
            license.owner_id != 0 &&
            license.owner_id != own_account_id;
        if (!shared_only || is_shared) {
            shared.push_back(license);
        }
    }

    if (shared.empty()) {
        if (error_message) {
            *error_message = shared_only
                ? "Steam returned no Family Shared package licenses."
                : "Steam returned no package licenses with access tokens.";
        }
        return true;
    }

    constexpr std::size_t BATCH_SIZE = 48u;
    std::uint64_t job_id = 0x5356504943530000ull;

    for (std::size_t batch_start = 0;
         batch_start < shared.size();
         batch_start += BATCH_SIZE) {
        if (cancelled && cancelled->load()) {
            if (error_message) *error_message = "Install cancelled.";
            return false;
        }

        const std::size_t batch_end =
            std::min(batch_start + BATCH_SIZE, shared.size());

        std::vector<unsigned char> body;
        for (std::size_t i = batch_start; i < batch_end; ++i) {
            std::vector<unsigned char> package;
            append_proto_varint(
                &package, 1u, shared[i].package_id);
            append_proto_varint(
                &package, 2u, shared[i].access_token);
            append_proto_bytes(
                &body, 1u,
                package.data(), package.size());
        }
        append_proto_varint(&body, 5u, 1u);

        const std::uint64_t this_job = ++job_id;
        const std::vector<unsigned char> request =
            make_proto_message(
                EMSG_CLIENT_PICS_PRODUCT_INFO_REQUEST,
                steam_id_,
                session_id_,
                body,
                this_job);

        if (!send_encrypted(request, error_message)) {
            return false;
        }

        bool pending = true;
        int responses = 0;
        while (pending && responses < 64) {
            if (cancelled && cancelled->load()) {
                if (error_message) *error_message = "Install cancelled.";
                return false;
            }

            std::vector<unsigned char> message;
            if (!receive_encrypted(
                    &message, cancelled, error_message)) {
                return false;
            }

            std::uint32_t emsg = 0;
            const unsigned char* header = nullptr;
            std::size_t header_size = 0;
            const unsigned char* message_body = nullptr;
            std::size_t body_size = 0;

            if (!split_proto_message(
                    message, &emsg,
                    &header, &header_size,
                    &message_body, &body_size)) {
                continue;
            }

            if (emsg != EMSG_CLIENT_PICS_PRODUCT_INFO_RESPONSE) {
                continue;
            }

            std::vector<PicsPackageResult> packages;
            if (!parse_pics_product_response(
                    message_body, body_size,
                    &packages, &pending)) {
                if (error_message) {
                    *error_message =
                        "Steam PICS package response could not be parsed.";
                }
                return false;
            }

            ++responses;

            for (const PicsPackageResult& package : packages) {
                if (package.package_id == 0 ||
                    package.missing_token) {
                    continue;
                }

                auto license_it = std::find_if(
                    shared.begin(), shared.end(),
                    [&](const SteamCmLicense& license) {
                        return license.package_id == package.package_id;
                    });
                if (license_it == shared.end()) continue;

                for (std::uint32_t app_id : package.app_ids) {
                    if (app_id == 0) continue;

                    const auto duplicate = std::find_if(
                        apps->begin(), apps->end(),
                        [&](const SteamCmSharedApp& existing) {
                            return existing.app_id == app_id &&
                                   existing.owner_id == license_it->owner_id;
                        });
                    if (duplicate != apps->end()) continue;

                    SteamCmSharedApp app;
                    app.app_id = app_id;
                    app.package_id = package.package_id;
                    app.owner_id = license_it->owner_id;
                    apps->push_back(app);
                }
            }
        }

        if (pending) {
            if (error_message) {
                *error_message =
                    "Steam PICS package response did not finish.";
            }
            return false;
        }
    }

    std::sort(
        apps->begin(), apps->end(),
        [](const SteamCmSharedApp& a,
           const SteamCmSharedApp& b) {
            if (a.app_id != b.app_id) return a.app_id < b.app_id;
            return a.owner_id < b.owner_id;
        });

    if (error_message) {
        std::ostringstream out;
        out << "Resolved " << apps->size()
            << (shared_only ? " Family Shared AppIDs from " : " AppIDs from ")
            << shared.size()
            << (shared_only ? " shared packages." : " package licenses.");
        *error_message = out.str();
    }
    return true;
}

bool SteamCmConnection::fetch_shared_app_names(
        std::vector<SteamCmSharedApp>* apps,
        std::atomic<bool>* cancelled,
        std::string* error_message) {
    if (!apps) return false;
    if (apps->empty()) {
        if (error_message) *error_message = "No Family Shared apps to name.";
        return true;
    }

    if (!connected() || steam_id_ == 0 || session_id_ == 0) {
        if (error_message) {
            *error_message =
                "Steam CM account session is not ready for app metadata.";
        }
        return false;
    }

    constexpr std::size_t BATCH_SIZE = 48u;
    std::uint64_t job_id = 0x5356504943531000ull;

    for (std::size_t batch_start = 0;
         batch_start < apps->size();
         batch_start += BATCH_SIZE) {
        const std::size_t batch_end =
            std::min(batch_start + BATCH_SIZE, apps->size());

        std::vector<unsigned char> body;
        for (std::size_t i = batch_start; i < batch_end; ++i) {
            std::vector<unsigned char> app;
            append_proto_varint(&app, 1u, (*apps)[i].app_id);
            append_proto_bytes(
                &body, 2u,
                app.data(), app.size());
        }

        const std::uint64_t this_job = ++job_id;
        const std::vector<unsigned char> request =
            make_proto_message(
                EMSG_CLIENT_PICS_PRODUCT_INFO_REQUEST,
                steam_id_,
                session_id_,
                body,
                this_job);

        if (!send_encrypted(request, error_message)) {
            return false;
        }

        bool pending = true;
        int responses = 0;
        while (pending && responses < 64) {
            if (cancelled && cancelled->load()) {
                if (error_message) *error_message = "Install cancelled.";
                return false;
            }

            std::vector<unsigned char> message;
            if (!receive_encrypted(
                    &message, cancelled, error_message)) {
                return false;
            }

            std::uint32_t emsg = 0;
            const unsigned char* header = nullptr;
            std::size_t header_size = 0;
            const unsigned char* message_body = nullptr;
            std::size_t body_size = 0;
            if (!split_proto_message(
                    message, &emsg,
                    &header, &header_size,
                    &message_body, &body_size)) {
                continue;
            }
            if (emsg != EMSG_CLIENT_PICS_PRODUCT_INFO_RESPONSE) {
                continue;
            }

            std::vector<PicsAppResult> results;
            if (!parse_pics_app_response(
                    message_body, body_size,
                    &results, &pending)) {
                if (error_message) {
                    *error_message =
                        "Steam PICS app response could not be parsed.";
                }
                return false;
            }
            ++responses;

            for (const PicsAppResult& result : results) {
                if (result.app_id == 0 || result.name.empty()) continue;
                for (SteamCmSharedApp& app : *apps) {
                    if (app.app_id == result.app_id && app.name.empty()) {
                        app.name = result.name;
                    }
                }
            }
        }

        if (pending) {
            if (error_message) {
                *error_message =
                    "Steam PICS app metadata response did not finish.";
            }
            return false;
        }
    }

    std::size_t named = 0;
    for (SteamCmSharedApp& app : *apps) {
        if (!app.name.empty()) {
            ++named;
        } else {
            app.name = "Steam App " + std::to_string(app.app_id);
        }
    }

    if (error_message) {
        std::ostringstream out;
        out << "Resolved names for " << named
            << " of " << apps->size()
            << " Family Shared apps.";
        *error_message = out.str();
    }
    return true;
}

bool SteamCmConnection::connect_one(
        const SteamCmEndpoint& endpoint,
        std::atomic<bool>* cancelled,
        std::string* error_message) {
    if (cancelled && cancelled->load()) {
        if (error_message) *error_message = "Install cancelled.";
        return false;
    }

    CURL* ws = curl_easy_init();
    if (!ws) {
        if (error_message) {
            *error_message = "Could not initialize Steam CM WebSocket.";
        }
        return false;
    }

    std::ostringstream url;
    url << "wss://" << endpoint.host << ":" << endpoint.port
        << "/cmsocket/";

    char curl_error[CURL_ERROR_SIZE]{};
    const bool configured =
        curl_easy_setopt(ws, CURLOPT_URL, url.str().c_str()) == CURLE_OK &&
        curl_easy_setopt(ws, CURLOPT_USERAGENT, "SteamVita/0.13") == CURLE_OK &&
        curl_easy_setopt(ws, CURLOPT_CAINFO, CA_PATH) == CURLE_OK &&
        curl_easy_setopt(ws, CURLOPT_SSL_VERIFYPEER, 1L) == CURLE_OK &&
        curl_easy_setopt(ws, CURLOPT_SSL_VERIFYHOST, 2L) == CURLE_OK &&
        curl_easy_setopt(ws, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1) == CURLE_OK &&
        curl_easy_setopt(ws, CURLOPT_CONNECTTIMEOUT, 12L) == CURLE_OK &&
        curl_easy_setopt(ws, CURLOPT_TIMEOUT, 20L) == CURLE_OK &&
        curl_easy_setopt(ws, CURLOPT_CONNECT_ONLY, 2L) == CURLE_OK &&
        curl_easy_setopt(ws, CURLOPT_ERRORBUFFER, curl_error) == CURLE_OK;

    if (!configured) {
        curl_easy_cleanup(ws);
        if (error_message) {
            *error_message = "Could not configure Steam CM WebSocket.";
        }
        return false;
    }

    const CURLcode result = curl_easy_perform(ws);
    if (cancelled && cancelled->load()) {
        curl_easy_cleanup(ws);
        if (error_message) *error_message = "Install cancelled.";
        return false;
    }

    if (result != CURLE_OK) {
        if (error_message) {
            std::ostringstream message;
            message << "Steam CM WebSocket connection failed: "
                    << (curl_error[0] ? curl_error
                                      : curl_easy_strerror(result));
            *error_message = message.str();
        }
        curl_easy_cleanup(ws);
        return false;
    }

    websocket_ = ws;
    endpoint_ = endpoint;
    {
        std::ostringstream trace;
        trace << "WSS connected " << endpoint.host << ":" << endpoint.port;
        append_cm_trace(trace.str());
    }
    return true;
}

bool SteamCmConnection::receive_exact(
        void* data,
        std::size_t size,
        std::atomic<bool>* cancelled) {
    auto* out = static_cast<unsigned char*>(data);
    std::size_t received = 0;
    while (received < size) {
        if (cancelled && cancelled->load()) return false;
        const int result = sceNetRecv(
            socket_,
            out + received,
            size - received,
            0);
        if (result <= 0) return false;
        received += static_cast<std::size_t>(result);
    }
    return true;
}

bool SteamCmConnection::receive_frame(
        std::vector<unsigned char>* payload,
        std::atomic<bool>* cancelled,
        std::string* error_message) {
    if (!payload || socket_ < 0) return false;

    unsigned char header[8]{};
    if (!receive_exact(header, sizeof(header), cancelled)) {
        if (error_message) *error_message = "Steam CM closed the connection.";
        return false;
    }

    const std::uint32_t length = read_le32(header);
    if (std::memcmp(header + 4, "VT01", 4) != 0) {
        if (error_message) *error_message = "Steam CM sent an invalid TCP frame.";
        return false;
    }
    if (length == 0 || length > CM_FRAME_LIMIT) {
        if (error_message) *error_message = "Steam CM frame exceeded the safety limit.";
        return false;
    }

    payload->assign(length, 0);
    if (!receive_exact(payload->data(), payload->size(), cancelled)) {
        if (error_message) *error_message = "Steam CM response was incomplete.";
        return false;
    }
    return true;
}

bool SteamCmConnection::send_encrypted(
        const std::vector<unsigned char>& payload,
        std::string* error_message) {
    if (!websocket_ || payload.empty()) {
        if (error_message) {
            *error_message = "Steam CM WebSocket is not connected.";
        }
        return false;
    }

    CURL* ws = static_cast<CURL*>(websocket_);
    std::size_t offset = 0;
    while (offset < payload.size()) {
        std::size_t sent = 0;
        const CURLcode result = curl_ws_send(
            ws,
            payload.data() + offset,
            payload.size() - offset,
            &sent,
            0,
            CURLWS_BINARY);

        if (result == CURLE_AGAIN) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(10));
            continue;
        }
        if (result != CURLE_OK) {
            if (error_message) {
                *error_message =
                    std::string("Steam CM WebSocket send failed: ") +
                    curl_easy_strerror(result);
            }
            return false;
        }
        if (sent == 0) {
            if (error_message) {
                *error_message =
                    "Steam CM WebSocket sent zero bytes.";
            }
            return false;
        }
        offset += sent;
    }

    return true;
}

bool SteamCmConnection::receive_encrypted(
        std::vector<unsigned char>* payload,
        std::atomic<bool>* cancelled,
        std::string* error_message) {
    if (!payload || !websocket_) return false;

    if (!pending_messages_.empty()) {
        *payload = std::move(pending_messages_.front());
        pending_messages_.pop_front();
        return true;
    }

    payload->clear();

    CURL* ws = static_cast<CURL*>(websocket_);
    bool message_started = false;

    for (;;) {
        if (cancelled && cancelled->load()) {
            if (error_message) *error_message = "Install cancelled.";
            return false;
        }

        unsigned char buffer[16u * 1024u];
        std::size_t received = 0;
        const struct curl_ws_frame* meta = nullptr;
        const CURLcode result = curl_ws_recv(
            ws,
            buffer,
            sizeof(buffer),
            &received,
            &meta);

        if (result == CURLE_AGAIN) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(10));
            continue;
        }
        if (result != CURLE_OK) {
            std::ostringstream trace;
            trace << "WSS receive error " << static_cast<int>(result)
                  << ": " << curl_easy_strerror(result);
            append_cm_trace(trace.str());
            if (error_message) {
                *error_message =
                    std::string("Steam CM WebSocket receive failed: ") +
                    curl_easy_strerror(result);
            }
            return false;
        }
        if (!meta) {
            if (error_message) {
                *error_message =
                    "Steam CM WebSocket returned no frame metadata.";
            }
            return false;
        }

        if (meta->flags & CURLWS_CLOSE) {
            std::ostringstream trace;
            trace << "WSS CLOSE frame, payload bytes=" << received;
            if (received >= 2) {
                const unsigned code =
                    (static_cast<unsigned>(buffer[0]) << 8u) |
                    static_cast<unsigned>(buffer[1]);
                trace << ", code=" << code;
            }
            append_cm_trace(trace.str());
            if (error_message) {
                *error_message = "Steam CM closed the WebSocket.";
            }
            return false;
        }

        if (meta->flags & CURLWS_PING) {
            std::size_t pong_offset = 0;
            while (pong_offset < received) {
                std::size_t pong_sent = 0;
                const CURLcode pong_result = curl_ws_send(
                    ws,
                    buffer + pong_offset,
                    received - pong_offset,
                    &pong_sent,
                    0,
                    CURLWS_PONG);
                if (pong_result == CURLE_AGAIN) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(10));
                    continue;
                }
                if (pong_result != CURLE_OK || pong_sent == 0) {
                    if (error_message) {
                        *error_message =
                            "Could not answer Steam CM WebSocket ping.";
                    }
                    return false;
                }
                pong_offset += pong_sent;
            }
            continue;
        }

        if (meta->flags & CURLWS_PONG) {
            continue;
        }

        if ((meta->flags & CURLWS_BINARY) == 0 &&
            !message_started) {
            continue;
        }

        message_started = true;
        if (received > 0) {
            if (payload->size() > CM_FRAME_LIMIT - received) {
                if (error_message) {
                    *error_message =
                        "Steam CM WebSocket message exceeded safety limit.";
                }
                return false;
            }
            payload->insert(
                payload->end(),
                buffer,
                buffer + received);
        }

        if (meta->bytesleft == 0 &&
            (meta->flags & CURLWS_CONT) == 0) {
            if (payload->empty()) return false;

            std::deque<std::vector<unsigned char>> unpacked;
            if (!unpack_multi_records(*payload, &unpacked)) {
                if (error_message) {
                    *error_message =
                        "Steam CM Multi message could not be unpacked.";
                }
                return false;
            }

            if (unpacked.empty()) {
                if (error_message) {
                    *error_message =
                        "Steam CM Multi message contained no records.";
                }
                return false;
            }

            *payload = std::move(unpacked.front());
            unpacked.pop_front();
            while (!unpacked.empty()) {
                pending_messages_.push_back(
                    std::move(unpacked.front()));
                unpacked.pop_front();
            }
            return true;
        }
    }
}

bool SteamCmConnection::send_frame(
        const std::vector<unsigned char>& payload) {
    if (socket_ < 0 || payload.empty() ||
        payload.size() > 0xffffffffu) {
        return false;
    }

    std::vector<unsigned char> packet;
    packet.reserve(8u + payload.size());
    append_le32(&packet, static_cast<std::uint32_t>(payload.size()));
    packet.insert(packet.end(), {'V', 'T', '0', '1'});
    packet.insert(packet.end(), payload.begin(), payload.end());

    std::size_t sent = 0;
    while (sent < packet.size()) {
        const int result = sceNetSend(
            socket_,
            packet.data() + sent,
            packet.size() - sent,
            0);
        if (result <= 0) return false;
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

bool SteamCmConnection::secure_channel(
        std::atomic<bool>* cancelled,
        std::string* error_message) {
    std::vector<unsigned char> request;
    if (!receive_frame(&request, cancelled, error_message)) return false;

    if (request.size() < MSG_HEADER_SIZE + 8u) {
        if (error_message) *error_message = "Steam CM encryption request was too short.";
        return false;
    }

    const std::uint32_t emsg = read_le32(request.data());
    if (emsg != EMSG_CHANNEL_ENCRYPT_REQUEST) {
        if (error_message) *error_message = "Steam CM did not start channel encryption.";
        return false;
    }

    const unsigned char* body = request.data() + MSG_HEADER_SIZE;
    const std::size_t body_size = request.size() - MSG_HEADER_SIZE;
    const std::uint32_t protocol_version = read_le32(body);
    const std::uint32_t universe = read_le32(body + 4);
    if (protocol_version != 1u || universe != 1u) {
        if (error_message) *error_message = "Steam CM requested an unsupported encryption mode.";
        return false;
    }

    const unsigned char* challenge = body + 8u;
    const std::size_t challenge_size = body_size - 8u;

    if (sceKernelGetRandomNumber(
            session_key_.data(),
            static_cast<int>(session_key_.size())) < 0) {
        if (error_message) *error_message = "Could not generate Steam CM session key.";
        return false;
    }

    unsigned char encrypted[128]{};
    if (!rsa_encrypt_session(
            session_key_.data(),
            challenge,
            challenge_size,
            encrypted,
            error_message)) {
        return false;
    }

    hmac_mode_ = challenge_size > 0;
    hmac_secret_.fill(0);
    if (hmac_mode_) {
        std::copy(
            session_key_.begin(),
            session_key_.begin() + hmac_secret_.size(),
            hmac_secret_.begin());
    }

    std::vector<unsigned char> response;
    response.reserve(MSG_HEADER_SIZE + 144u);
    append_msg_header(&response, EMSG_CHANNEL_ENCRYPT_RESPONSE);
    append_le32(&response, 1u);
    append_le32(&response, 128u);
    response.insert(response.end(), encrypted, encrypted + sizeof(encrypted));

    const std::uint32_t crc =
        static_cast<std::uint32_t>(crc32(0L, encrypted, sizeof(encrypted)));
    append_le32(&response, crc);
    append_le32(&response, 0u);

    if (!send_frame(response)) {
        if (error_message) *error_message = "Could not send Steam CM encryption response.";
        return false;
    }

    std::vector<unsigned char> result;
    if (!receive_frame(&result, cancelled, error_message)) return false;
    if (result.size() < MSG_HEADER_SIZE + 4u ||
        read_le32(result.data()) != EMSG_CHANNEL_ENCRYPT_RESULT) {
        if (error_message) *error_message = "Steam CM encryption result was invalid.";
        return false;
    }

    const std::uint32_t eresult =
        read_le32(result.data() + MSG_HEADER_SIZE);
    if (eresult != 1u) {
        if (error_message) {
            std::ostringstream message;
            message << "Steam CM channel encryption failed (EResult "
                    << eresult << ").";
            *error_message = message.str();
        }
        return false;
    }

    return true;
}
