#include "steam_cm_client.h"

#include <curl/curl.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <psp2/kernel/rng.h>
#include <psp2/net/net.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace {

constexpr const char* CA_PATH = "ux0:data/SteamVita/cacert.pem";
constexpr const char* CM_LIST_URL =
    "https://api.steampowered.com/ISteamDirectory/GetCMList/v1/"
    "?cellid=0&maxcount=32&format=json";
constexpr std::size_t RESPONSE_LIMIT = 256u * 1024u;
constexpr std::size_t CM_FRAME_LIMIT = 2u * 1024u * 1024u;

constexpr std::uint32_t EMSG_CHANNEL_ENCRYPT_REQUEST = 1303u;
constexpr std::uint32_t EMSG_CHANNEL_ENCRYPT_RESPONSE = 1304u;
constexpr std::uint32_t EMSG_CHANNEL_ENCRYPT_RESULT = 1305u;
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
        pos = json.find('"', pos);
        if (pos == std::string::npos || pos >= end) break;
        const std::size_t close = json.find('"', pos + 1);
        if (close == std::string::npos || close > end) return false;

        SteamCmEndpoint endpoint;
        if (parse_endpoint(json.substr(pos + 1, close - pos - 1),
                           &endpoint)) {
            servers->push_back(std::move(endpoint));
        }

        pos = close + 1;
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
    if (socket_ >= 0) {
        sceNetSocketClose(socket_);
        socket_ = -1;
    }
    endpoint_ = {};
    session_key_.fill(0);
    hmac_secret_.fill(0);
}

bool SteamCmConnection::connected() const {
    return socket_ >= 0;
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

bool SteamCmConnection::connect_one(
        const SteamCmEndpoint& endpoint,
        std::atomic<bool>* cancelled,
        std::string* error_message) {
    socket_ = sceNetSocket(
        "SteamVitaCM",
        SCE_NET_AF_INET,
        SCE_NET_SOCK_STREAM,
        SCE_NET_IPPROTO_TCP);
    if (socket_ < 0) {
        if (error_message) *error_message = "Could not create Steam CM socket.";
        return false;
    }

    SceNetSockaddrIn address{};
    address.sin_family = SCE_NET_AF_INET;
    address.sin_port = sceNetHtons(endpoint.port);
    if (sceNetInetPton(
            SCE_NET_AF_INET,
            endpoint.host.c_str(),
            &address.sin_addr) != 1) {
        close();
        if (error_message) *error_message = "Steam CM returned an invalid IP address.";
        return false;
    }

    if (cancelled && cancelled->load()) {
        close();
        if (error_message) *error_message = "Install cancelled.";
        return false;
    }

    const int connected_result = sceNetConnect(
        socket_,
        reinterpret_cast<SceNetSockaddr*>(&address),
        sizeof(address));
    if (connected_result < 0) {
        close();
        if (error_message) {
            std::ostringstream message;
            message << "Could not connect to Steam CM "
                    << endpoint.host << ":" << endpoint.port << ".";
            *error_message = message.str();
        }
        return false;
    }

    endpoint_ = endpoint;
    if (!secure_channel(cancelled, error_message)) {
        close();
        return false;
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

    std::copy(
        session_key_.begin(),
        session_key_.begin() + hmac_secret_.size(),
        hmac_secret_.begin());

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
