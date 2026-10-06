#include "steam_client.h"

#include <curl/curl.h>
#include <psp2/kernel/rng.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr const char* DATA_DIR = "ux0:data/SteamVita";
constexpr const char* DEVICE_ID_PATH = "ux0:data/SteamVita/device_id.txt";
constexpr const char* CA_PATH = "ux0:data/SteamVita/cacert.pem";
constexpr const char* BEGIN_QR_URL =
    "https://api.steampowered.com/IAuthenticationService/BeginAuthSessionViaQR/v1/";
constexpr const char* POLL_QR_URL =
    "https://api.steampowered.com/IAuthenticationService/PollAuthSessionStatus/v1/";
constexpr const char* OWNED_GAMES_URL =
    "https://api.steampowered.com/IPlayerService/GetOwnedGames/v1/";
constexpr std::size_t NET_MEMORY_SIZE = 4u * 1024u * 1024u;
constexpr std::size_t AUTH_RESPONSE_LIMIT = 256u * 1024u;
constexpr std::size_t LIBRARY_RESPONSE_LIMIT = 8u * 1024u * 1024u;
constexpr std::size_t MAX_LIBRARY_GAMES = 10000u;

constexpr const char* LOG_PATH = "ux0:data/SteamVita/steamvita.log";
constexpr const char* LIBRARY_CACHE_PATH = "ux0:data/SteamVita/library.cache";
constexpr const char* LIBRARY_CACHE_TMP_PATH = "ux0:data/SteamVita/library.cache.tmp";
constexpr const char* LIBRARY_CACHE_BAK_PATH = "ux0:data/SteamVita/library.cache.bak";

std::mutex g_log_mutex;

void append_log(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::ofstream output(LOG_PATH, std::ios::app);
    if (output) output << line << "\n";
}

struct CurlBuffer {
    std::string* output = nullptr;
    std::size_t limit = 0;
    bool overflow = false;
};

struct HttpResult {
    CURLcode curl_code = CURLE_FAILED_INIT;
    long status = 0;
    std::string body;
    bool overflow = false;
};

std::size_t curl_write_limited(void* data,
                               std::size_t size,
                               std::size_t count,
                               void* userdata) {
    if (!userdata) return 0;

    const std::size_t bytes = size * count;
    auto* buffer = static_cast<CurlBuffer*>(userdata);
    if (!buffer->output) return 0;

    if (bytes > buffer->limit ||
        buffer->output->size() > buffer->limit - bytes) {
        buffer->overflow = true;
        return 0;
    }

    buffer->output->append(static_cast<const char*>(data), bytes);
    return bytes;
}

int curl_cancel_progress(void* userdata,
                         curl_off_t,
                         curl_off_t,
                         curl_off_t,
                         curl_off_t) {
    auto* cancelled = static_cast<std::atomic<bool>*>(userdata);
    return cancelled && cancelled->load() ? 1 : 0;
}

bool configure_curl(CURL* curl,
                    const std::string& ca_bundle,
                    CurlBuffer* buffer,
                    std::atomic<bool>* cancelled,
                    long timeout_seconds) {
    if (!curl || !buffer || !buffer->output) return false;

    return
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "SteamVita/0.12") == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle.c_str()) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity") == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_limited) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, buffer) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_cancel_progress) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancelled) == CURLE_OK;
}

HttpResult http_post(const std::string& url,
                     const std::string& body,
                     const std::vector<std::string>& headers,
                     const std::string& ca_bundle,
                     std::size_t response_limit,
                     std::atomic<bool>* cancelled) {
    HttpResult result;
    result.body.reserve(std::min<std::size_t>(response_limit, 64u * 1024u));

    CURL* curl = curl_easy_init();
    if (!curl) return result;

    CurlBuffer buffer{&result.body, response_limit, false};
    curl_slist* header_list = nullptr;
    for (const auto& header : headers) {
        header_list = curl_slist_append(header_list, header.c_str());
        if (!header_list) {
            curl_easy_cleanup(curl);
            result.curl_code = CURLE_OUT_OF_MEMORY;
            return result;
        }
    }

    if (!configure_curl(curl, ca_bundle, &buffer, cancelled, 30L) ||
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POST, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str()) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(body.size())) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list) != CURLE_OK) {
        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);
        result.curl_code = CURLE_FAILED_INIT;
        return result;
    }

    result.curl_code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    result.overflow = buffer.overflow;

    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);
    return result;
}

HttpResult http_get(const std::string& url,
                    const std::string& ca_bundle,
                    std::size_t response_limit,
                    std::atomic<bool>* cancelled,
                    long timeout_seconds) {
    HttpResult result;
    result.body.reserve(std::min<std::size_t>(response_limit, 256u * 1024u));

    CURL* curl = curl_easy_init();
    if (!curl) return result;

    CurlBuffer buffer{&result.body, response_limit, false};
    if (!configure_curl(curl, ca_bundle, &buffer, cancelled, timeout_seconds) ||
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) != CURLE_OK) {
        curl_easy_cleanup(curl);
        result.curl_code = CURLE_FAILED_INIT;
        return result;
    }

    result.curl_code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    result.overflow = buffer.overflow;

    curl_easy_cleanup(curl);
    return result;
}

std::size_t find_json_member(const std::string& object, const char* member) {
    const std::string needle = std::string("\"") + member + "\"";
    std::size_t pos = object.find(needle);
    if (pos == std::string::npos) return pos;

    pos = object.find(':', pos + needle.size());
    if (pos == std::string::npos) return pos;

    ++pos;
    while (pos < object.size() &&
           std::isspace(static_cast<unsigned char>(object[pos]))) {
        ++pos;
    }
    return pos;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void append_utf8(std::string& out, unsigned codepoint) {
    if (codepoint <= 0x7f) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0u | (codepoint >> 6u)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
    } else if (codepoint <= 0xffff) {
        out.push_back(static_cast<char>(0xe0u | (codepoint >> 12u)));
        out.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
    } else if (codepoint <= 0x10ffff) {
        out.push_back(static_cast<char>(0xf0u | (codepoint >> 18u)));
        out.push_back(static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3fu)));
        out.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
    }
}

std::string json_string_member(const std::string& object, const char* member) {
    std::size_t pos = find_json_member(object, member);
    if (pos == std::string::npos || pos >= object.size() || object[pos] != '"')
        return {};
    ++pos;

    std::string out;
    while (pos < object.size()) {
        char c = object[pos++];
        if (c == '"') return out;

        if (c != '\\') {
            if (static_cast<unsigned char>(c) < 0x20) return {};
            out.push_back(c);
            continue;
        }

        if (pos >= object.size()) return {};
        const char escaped = object[pos++];
        switch (escaped) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (pos + 4 > object.size()) return {};
                unsigned cp = 0;
                for (int i = 0; i < 4; ++i) {
                    const int digit = hex_digit(object[pos++]);
                    if (digit < 0) return {};
                    cp = (cp << 4u) | static_cast<unsigned>(digit);
                }

                if (cp >= 0xd800 && cp <= 0xdbff &&
                    pos + 6 <= object.size() &&
                    object[pos] == '\\' && object[pos + 1] == 'u') {
                    pos += 2;
                    unsigned low = 0;
                    for (int i = 0; i < 4; ++i) {
                        const int digit = hex_digit(object[pos++]);
                        if (digit < 0) return {};
                        low = (low << 4u) | static_cast<unsigned>(digit);
                    }
                    if (low >= 0xdc00 && low <= 0xdfff) {
                        cp = 0x10000u + ((cp - 0xd800u) << 10u) +
                             (low - 0xdc00u);
                    }
                }

                append_utf8(out, cp);
                break;
            }
            default:
                return {};
        }
    }

    return {};
}

std::uint64_t json_u64_member(const std::string& object, const char* member) {
    std::size_t pos = find_json_member(object, member);
    if (pos == std::string::npos || pos >= object.size()) return 0;

    bool quoted = false;
    if (object[pos] == '"') {
        quoted = true;
        ++pos;
    }

    if (pos >= object.size() ||
        !std::isdigit(static_cast<unsigned char>(object[pos]))) {
        return 0;
    }

    std::uint64_t value = 0;
    while (pos < object.size() &&
           std::isdigit(static_cast<unsigned char>(object[pos]))) {
        const unsigned digit = static_cast<unsigned>(object[pos] - '0');
        if (value > (UINT64_MAX - digit) / 10u) return 0;
        value = value * 10u + digit;
        ++pos;
    }

    if (quoted && (pos >= object.size() || object[pos] != '"')) return 0;
    return value;
}

std::uint32_t json_uint_member(const std::string& object, const char* member) {
    const std::uint64_t value = json_u64_member(object, member);
    return value <= 0xffffffffull ? static_cast<std::uint32_t>(value) : 0u;
}

double json_double_member(const std::string& object,
                          const char* member,
                          double fallback) {
    std::size_t pos = find_json_member(object, member);
    if (pos == std::string::npos || pos >= object.size()) return fallback;

    char* end = nullptr;
    const double value = std::strtod(object.c_str() + pos, &end);
    if (!end || end == object.c_str() + pos || value <= 0.0) return fallback;
    return value;
}

bool json_bool_member(const std::string& object,
                      const char* member,
                      bool fallback = false) {
    const std::size_t pos = find_json_member(object, member);
    if (pos == std::string::npos || pos >= object.size()) return fallback;
    if (object.compare(pos, 4, "true") == 0) return true;
    if (object.compare(pos, 5, "false") == 0) return false;
    return fallback;
}

int base64url_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}

std::string decode_base64url(const std::string& input) {
    std::string output;
    output.reserve((input.size() * 3u) / 4u + 4u);

    unsigned accumulator = 0;
    int bits = 0;

    for (char c : input) {
        if (c == '=') break;
        const int value = base64url_value(c);
        if (value < 0) return {};

        accumulator = (accumulator << 6u) | static_cast<unsigned>(value);
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<char>((accumulator >> bits) & 0xffu));
        }
    }

    return output;
}

std::uint64_t steam_id_from_token(const std::string& token) {
    const std::size_t first = token.find('.');
    if (first == std::string::npos) return 0;

    const std::size_t second = token.find('.', first + 1);
    if (second == std::string::npos || second <= first + 1) return 0;

    const std::string payload =
        decode_base64url(token.substr(first + 1, second - first - 1));
    if (payload.empty()) return 0;

    std::uint64_t steam_id = json_u64_member(payload, "sub");
    if (steam_id == 0) steam_id = json_u64_member(payload, "steamid");
    return steam_id;
}

void sleep_interruptible(double seconds, std::atomic<bool>* cancelled) {
    int milliseconds = static_cast<int>(seconds * 1000.0);
    milliseconds = std::max(250, std::min(milliseconds, 10000));

    while (milliseconds > 0) {
        if (cancelled && cancelled->load()) return;
        const int slice = std::min(milliseconds, 100);
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        milliseconds -= slice;
    }
}

bool copy_ca_bundle() {
    std::ifstream input("app0:/cacert.pem", std::ios::binary);
    if (!input) return false;

    std::ofstream output(CA_PATH, std::ios::binary | std::ios::trunc);
    if (!output) return false;

    char buffer[8192];
    std::size_t total = 0;
    while (input.good()) {
        input.read(buffer, sizeof(buffer));
        const std::streamsize bytes = input.gcount();
        if (bytes > 0) {
            output.write(buffer, bytes);
            total += static_cast<std::size_t>(bytes);
        }
    }

    output.flush();
    return output.good() && total > 1024u;
}

void parse_games(const std::string& json, std::vector<SteamGame>* loaded) {
    if (!loaded) return;

    const std::size_t games_key = json.find("\"games\"");
    if (games_key == std::string::npos) return;

    const std::size_t array_begin = json.find('[', games_key);
    if (array_begin == std::string::npos) return;

    bool in_string = false;
    bool escaped = false;
    int depth = 0;
    std::size_t object_begin = std::string::npos;

    for (std::size_t i = array_begin + 1; i < json.size(); ++i) {
        const char c = json[i];

        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }

        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            if (depth == 0) object_begin = i;
            ++depth;
        } else if (c == '}') {
            if (depth > 0) --depth;

            if (depth == 0 && object_begin != std::string::npos) {
                const std::string object =
                    json.substr(object_begin, i - object_begin + 1);
                object_begin = std::string::npos;

                SteamGame game;
                game.app_id = json_uint_member(object, "appid");
                game.name = json_string_member(object, "name");
                game.playtime_minutes =
                    json_uint_member(object, "playtime_forever");
                game.icon_hash = json_string_member(object, "img_icon_url");

                if (game.name.size() > 512u) game.name.resize(512u);
                if (game.icon_hash.size() > 128u) game.icon_hash.resize(128u);

                if (game.app_id != 0 && !game.name.empty()) {
                    loaded->push_back(std::move(game));
                    if (loaded->size() >= MAX_LIBRARY_GAMES) return;
                }
            }
        } else if (c == ']' && depth == 0) {
            break;
        }
    }
}

bool json_parser_self_test() {
    const std::string sample =
        "{\"response\":{\"client_id\":\"12345\","
        "\"request_id\":\"YWJjZA==\","
        "\"challenge_url\":\"https:\\/\\/s.team\\/q\\/test\","
        "\"interval\":5.0},"
        "\"games\":[{\"appid\":21000,"
        "\"name\":\"LEGO Batman: The Videogame\","
        "\"playtime_forever\":42}]}";

    if (json_u64_member(sample, "client_id") != 12345u) return false;
    if (json_string_member(sample, "request_id") != "YWJjZA==") return false;
    if (json_string_member(sample, "challenge_url") !=
        "https://s.team/q/test") return false;

    std::vector<SteamGame> games;
    parse_games(sample, &games);
    return games.size() == 1u &&
           games[0].app_id == 21000u &&
           games[0].name == "LEGO Batman: The Videogame" &&
           games[0].playtime_minutes == 42u;
}

bool write_bytes(std::ofstream& out, const void* data, std::size_t size) {
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}

bool read_bytes(std::ifstream& in, void* data, std::size_t size) {
    in.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    return in.gcount() == static_cast<std::streamsize>(size);
}

bool write_u16(std::ofstream& out, std::uint16_t value) {
    unsigned char bytes[2] = {
        static_cast<unsigned char>(value & 0xffu),
        static_cast<unsigned char>((value >> 8u) & 0xffu)
    };
    return write_bytes(out, bytes, sizeof(bytes));
}

bool write_u32(std::ofstream& out, std::uint32_t value) {
    unsigned char bytes[4] = {
        static_cast<unsigned char>(value & 0xffu),
        static_cast<unsigned char>((value >> 8u) & 0xffu),
        static_cast<unsigned char>((value >> 16u) & 0xffu),
        static_cast<unsigned char>((value >> 24u) & 0xffu)
    };
    return write_bytes(out, bytes, sizeof(bytes));
}

bool write_u64(std::ofstream& out, std::uint64_t value) {
    unsigned char bytes[8];
    for (int i = 0; i < 8; ++i) {
        bytes[i] = static_cast<unsigned char>((value >> (i * 8)) & 0xffu);
    }
    return write_bytes(out, bytes, sizeof(bytes));
}

bool read_u16(std::ifstream& in, std::uint16_t* value) {
    unsigned char bytes[2];
    if (!read_bytes(in, bytes, sizeof(bytes))) return false;
    *value = static_cast<std::uint16_t>(bytes[0]) |
             (static_cast<std::uint16_t>(bytes[1]) << 8u);
    return true;
}

bool read_u32(std::ifstream& in, std::uint32_t* value) {
    unsigned char bytes[4];
    if (!read_bytes(in, bytes, sizeof(bytes))) return false;
    *value = static_cast<std::uint32_t>(bytes[0]) |
             (static_cast<std::uint32_t>(bytes[1]) << 8u) |
             (static_cast<std::uint32_t>(bytes[2]) << 16u) |
             (static_cast<std::uint32_t>(bytes[3]) << 24u);
    return true;
}

bool read_u64(std::ifstream& in, std::uint64_t* value) {
    unsigned char bytes[8];
    if (!read_bytes(in, bytes, sizeof(bytes))) return false;
    std::uint64_t result = 0;
    for (int i = 0; i < 8; ++i) {
        result |= static_cast<std::uint64_t>(bytes[i]) << (i * 8);
    }
    *value = result;
    return true;
}

} // namespace

SteamClient::SteamClient() = default;

SteamClient::~SteamClient() {
    sign_out();
    shutdown_network();
}

bool SteamClient::initialize(std::string* error_message) {
    mkdir(DATA_DIR, 0777);

    {
        std::ofstream clear_log(LOG_PATH, std::ios::trunc);
        if (clear_log) clear_log << "SteamVita startup\n";
    }

    if (!json_parser_self_test()) {
        const std::string message =
            "SteamVita JSON parser self-test failed.";
        append_log(message);
        if (error_message) *error_message = message;
        set_error(message);
        return false;
    }
    append_log("JSON parser self-test passed.");

    const bool cache_loaded = load_library_cache();
    if (cache_loaded) {
        append_log("Offline library cache loaded.");
    } else {
        append_log("No usable offline library cache.");
    }

    auto offline_fallback = [&](const std::string& reason) {
        append_log(std::string("Network unavailable: ") + reason);
        network_ready_ = false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!games_.empty()) {
            state_ = SteamState::Ready;
            offline_mode_ = true;
            std::ostringstream message;
            message << "Offline Mode - " << games_.size()
                    << " cached games.";
            status_ = message.str();
        } else {
            state_ = SteamState::SignedOut;
            offline_mode_ = true;
            status_ = "Offline Mode - no cached library yet.";
        }
        if (error_message) error_message->clear();
        return true;
    };

    const int module_result = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    if (module_result < 0) {
        return offline_fallback("could not load Vita network module");
    }
    net_module_loaded_ = true;

    net_memory_ = std::malloc(NET_MEMORY_SIZE);
    if (!net_memory_) {
        shutdown_network();
        return offline_fallback("could not allocate network memory");
    }

    SceNetInitParam net_param{};
    net_param.memory = net_memory_;
    net_param.size = NET_MEMORY_SIZE;
    net_param.flags = 0;

    if (sceNetInit(&net_param) < 0) {
        shutdown_network();
        return offline_fallback("sceNetInit failed");
    }
    net_initialized_ = true;

    if (sceNetCtlInit() < 0) {
        shutdown_network();
        return offline_fallback("sceNetCtlInit failed");
    }
    netctl_initialized_ = true;

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        shutdown_network();
        return offline_fallback("libcurl initialization failed");
    }
    curl_initialized_ = true;

    if (!copy_ca_bundle()) {
        shutdown_network();
        return offline_fallback("TLS certificate bundle unavailable");
    }
    ca_bundle_ = CA_PATH;

    std::string device_error;
    if (!load_or_create_device_id(&device_id_, &device_error)) {
        shutdown_network();
        return offline_fallback(device_error);
    }

    network_ready_ = true;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!games_.empty()) {
            state_ = SteamState::Ready;
            offline_mode_ = true;
            std::ostringstream message;
            message << "Cached library loaded (" << games_.size()
                    << " games). Triangle: sign in to refresh.";
            status_ = message.str();
        } else {
            state_ = SteamState::SignedOut;
            offline_mode_ = false;
            status_ = "Sign in to Steam to load your real game library.";
        }
    }

    if (error_message) error_message->clear();
    return true;
}

bool SteamClient::load_or_create_device_id(std::string* value,
                                           std::string* error_message) {
    if (!value) return false;

    {
        std::ifstream input(DEVICE_ID_PATH);
        std::string existing;
        std::getline(input, existing);
        if (existing.size() >= 16 && existing.size() <= 128) {
            *value = existing;
            return true;
        }
    }

    unsigned char random_bytes[32]{};
    if (sceKernelGetRandomNumber(random_bytes, sizeof(random_bytes)) < 0) {
        if (error_message) {
            *error_message = "Could not generate a Steam device identity.";
        }
        return false;
    }

    static const char hex[] = "0123456789abcdef";
    std::string generated;
    generated.resize(sizeof(random_bytes) * 2);

    for (std::size_t i = 0; i < sizeof(random_bytes); ++i) {
        generated[i * 2] = hex[(random_bytes[i] >> 4) & 0x0f];
        generated[i * 2 + 1] = hex[random_bytes[i] & 0x0f];
    }

    std::ofstream output(DEVICE_ID_PATH, std::ios::trunc);
    if (!output) {
        if (error_message) *error_message = "Could not save Steam device ID.";
        return false;
    }

    output << generated << "\n";
    output.close();

    *value = std::move(generated);
    return true;
}


bool SteamClient::load_library_cache() {
    std::ifstream in(LIBRARY_CACHE_PATH, std::ios::binary);
    if (!in) return false;

    char magic[8]{};
    if (!read_bytes(in, magic, sizeof(magic)) ||
        std::memcmp(magic, "SVLIB01", 7) != 0) {
        return false;
    }

    std::uint32_t count = 0;
    std::uint64_t cached_steam_id = 0;
    std::uint16_t account_len = 0;
    if (!read_u32(in, &count) ||
        !read_u64(in, &cached_steam_id) ||
        !read_u16(in, &account_len)) {
        return false;
    }

    if (count > MAX_LIBRARY_GAMES || account_len > 128u) return false;

    std::string account(account_len, '\0');
    if (account_len > 0 &&
        !read_bytes(in, &account[0], account_len)) {
        return false;
    }

    std::vector<SteamGame> loaded;
    loaded.reserve(count);

    for (std::uint32_t i = 0; i < count; ++i) {
        SteamGame game;
        std::uint16_t name_len = 0;
        std::uint16_t icon_len = 0;

        if (!read_u32(in, &game.app_id) ||
            !read_u32(in, &game.playtime_minutes) ||
            !read_u16(in, &name_len) ||
            !read_u16(in, &icon_len)) {
            return false;
        }

        if (game.app_id == 0 || name_len == 0 ||
            name_len > 512u || icon_len > 128u) {
            return false;
        }

        game.name.assign(name_len, '\0');
        if (!read_bytes(in, &game.name[0], name_len)) return false;

        game.icon_hash.assign(icon_len, '\0');
        if (icon_len > 0 &&
            !read_bytes(in, &game.icon_hash[0], icon_len)) {
            return false;
        }

        loaded.push_back(std::move(game));
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        games_.swap(loaded);
        steam_id_ = cached_steam_id;
        account_name_ = std::move(account);
        offline_mode_ = true;
    }
    return true;
}

bool SteamClient::save_library_cache(const std::vector<SteamGame>& games,
                                     const std::string& account_name,
                                     std::uint64_t steam_id) {
    if (games.size() > MAX_LIBRARY_GAMES ||
        account_name.size() > 128u) {
        return false;
    }

    std::ofstream out(LIBRARY_CACHE_TMP_PATH,
                      std::ios::binary | std::ios::trunc);
    if (!out) return false;

    char magic[8] = {'S','V','L','I','B','0','1','\0'};
    if (!write_bytes(out, magic, sizeof(magic)) ||
        !write_u32(out, static_cast<std::uint32_t>(games.size())) ||
        !write_u64(out, steam_id) ||
        !write_u16(out, static_cast<std::uint16_t>(account_name.size())) ||
        (!account_name.empty() &&
         !write_bytes(out, account_name.data(), account_name.size()))) {
        return false;
    }

    for (const SteamGame& game : games) {
        if (game.app_id == 0 || game.name.empty() ||
            game.name.size() > 512u || game.icon_hash.size() > 128u) {
            return false;
        }

        if (!write_u32(out, game.app_id) ||
            !write_u32(out, game.playtime_minutes) ||
            !write_u16(out, static_cast<std::uint16_t>(game.name.size())) ||
            !write_u16(out, static_cast<std::uint16_t>(game.icon_hash.size())) ||
            !write_bytes(out, game.name.data(), game.name.size()) ||
            (!game.icon_hash.empty() &&
             !write_bytes(out, game.icon_hash.data(),
                          game.icon_hash.size()))) {
            return false;
        }
    }

    out.flush();
    if (!out.good()) return false;
    out.close();

    std::remove(LIBRARY_CACHE_BAK_PATH);
    std::rename(LIBRARY_CACHE_PATH, LIBRARY_CACHE_BAK_PATH);

    if (std::rename(LIBRARY_CACHE_TMP_PATH, LIBRARY_CACHE_PATH) != 0) {
        std::rename(LIBRARY_CACHE_BAK_PATH, LIBRARY_CACHE_PATH);
        return false;
    }

    std::remove(LIBRARY_CACHE_BAK_PATH);
    append_log("Library cache updated atomically.");
    return true;
}

bool SteamClient::start_qr_login() {
    cancel_login_.store(true);

    if (auth_thread_.joinable() &&
        auth_thread_.get_id() != std::this_thread::get_id()) {
        auth_thread_.join();
    }

    if (library_thread_.joinable() &&
        library_thread_.get_id() != std::this_thread::get_id()) {
        library_thread_.join();
    }

    cancel_login_.store(false);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        access_token_.clear();
        qr_url_.clear();
    }

    if (!network_ready_ || !net_initialized_ ||
        device_id_.empty() || ca_bundle_.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!games_.empty()) {
            state_ = SteamState::Ready;
            offline_mode_ = true;
            status_ = "Offline Mode - Steam networking is unavailable.";
        } else {
            state_ = SteamState::SignedOut;
            offline_mode_ = true;
            status_ = "Offline Mode - Steam networking is unavailable.";
        }
        return false;
    }

    cancel_login_.store(false);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = SteamState::Connecting;
        status_ = "Connecting to Steam authentication...";
        qr_url_.clear();
    }

    auth_thread_ = std::thread(&SteamClient::authentication_worker, this);
    return true;
}

void SteamClient::authentication_worker() {
    const std::vector<std::string> begin_headers = {
        "Content-Type: application/x-www-form-urlencoded; charset=UTF-8",
        "Accept: application/json",
        "Origin: https://steamcommunity.com",
        "Referer: https://steamcommunity.com/login/home/?goto="
    };

    // Steam's Unified Web API accepts request messages through the
    // input_json form field. Encoding the whole message this way is more
    // reliable than posting individual fields on Vita.
    CURL* begin_escape = curl_easy_init();
    if (!begin_escape) {
        set_error("Could not prepare the Steam QR request.");
        return;
    }

    const std::string begin_json =
        "{\"device_friendly_name\":\"SteamVita\",\"platform_type\":2}";
    char* encoded_begin = curl_easy_escape(
        begin_escape, begin_json.c_str(), static_cast<int>(begin_json.size()));
    if (!encoded_begin) {
        curl_easy_cleanup(begin_escape);
        set_error("Could not encode the Steam QR request.");
        return;
    }

    const std::string begin_body =
        std::string("input_json=") + encoded_begin;
    curl_free(encoded_begin);
    curl_easy_cleanup(begin_escape);

    HttpResult begin = http_post(
        BEGIN_QR_URL, begin_body, begin_headers, ca_bundle_,
        AUTH_RESPONSE_LIMIT, &cancel_login_);

    if (cancel_login_.load()) return;

    {
        std::ostringstream log;
        log << "QR begin: curl=" << static_cast<int>(begin.curl_code)
            << " http=" << begin.status
            << " bytes=" << begin.body.size();
        append_log(log.str());
    }

    if (begin.overflow) {
        set_error("Steam authentication response was unexpectedly large.");
        return;
    }
    if (begin.curl_code != CURLE_OK) {
        set_error(std::string("Steam QR request failed: ") +
                  curl_easy_strerror(begin.curl_code));
        return;
    }
    if (begin.status != 200) {
        std::ostringstream message;
        message << "Steam QR request returned HTTP " << begin.status << ".";
        set_error(message.str());
        return;
    }

    std::uint64_t client_id = json_u64_member(begin.body, "client_id");
    const std::string request_id =
        json_string_member(begin.body, "request_id");
    std::string challenge_url =
        json_string_member(begin.body, "challenge_url");
    double interval = json_double_member(begin.body, "interval", 5.0);
    interval = std::max(1.0, std::min(interval, 10.0));

    if (client_id == 0 || request_id.empty() || challenge_url.empty()) {
        {
            std::ostringstream log;
            log << "QR fields: client_id=" << (client_id != 0 ? "yes" : "no")
                << " request_id=" << (!request_id.empty() ? "yes" : "no")
                << " challenge_url=" << (!challenge_url.empty() ? "yes" : "no");
            append_log(log.str());
        }
        std::ostringstream message;
        message << "Steam QR session missing ";
        bool first = true;
        if (client_id == 0) {
            message << "client_id";
            first = false;
        }
        if (request_id.empty()) {
            if (!first) message << ", ";
            message << "request_id";
            first = false;
        }
        if (challenge_url.empty()) {
            if (!first) message << ", ";
            message << "challenge_url";
        }
        message << ".";
        set_error(message.str());
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        qr_url_ = challenge_url;
        state_ = SteamState::WaitingForQr;
        status_ = "Scan the QR code with the Steam mobile app.";
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::minutes(5);
    int consecutive_failures = 0;

    while (!cancel_login_.load() &&
           std::chrono::steady_clock::now() < deadline) {
        std::ostringstream poll_json;
        poll_json << "{\"client_id\":\"" << client_id
                  << "\",\"request_id\":\"" << request_id << "\"}";

        CURL* poll_escape = curl_easy_init();
        if (!poll_escape) {
            set_error("Could not prepare Steam QR polling.");
            return;
        }

        const std::string poll_json_text = poll_json.str();
        char* encoded_poll = curl_easy_escape(
            poll_escape,
            poll_json_text.c_str(),
            static_cast<int>(poll_json_text.size()));
        if (!encoded_poll) {
            curl_easy_cleanup(poll_escape);
            set_error("Could not encode Steam QR polling.");
            return;
        }

        const std::string poll_body =
            std::string("input_json=") + encoded_poll;
        curl_free(encoded_poll);
        curl_easy_cleanup(poll_escape);

        const std::vector<std::string> form_headers = {
            "Content-Type: application/x-www-form-urlencoded; charset=UTF-8",
            "Accept: application/json",
            "Origin: https://steamcommunity.com",
            "Referer: https://steamcommunity.com/login/home/?goto="
        };

        HttpResult poll = http_post(
            POLL_QR_URL, poll_body, form_headers, ca_bundle_,
            AUTH_RESPONSE_LIMIT, &cancel_login_);

        if (cancel_login_.load()) return;

        if (poll.overflow) {
            set_error("Steam QR polling response was unexpectedly large.");
            return;
        }

        if (poll.curl_code != CURLE_OK || poll.status != 200) {
            ++consecutive_failures;
            if (consecutive_failures >= 3) {
                if (poll.curl_code != CURLE_OK) {
                    set_error(std::string("Steam QR polling failed: ") +
                              curl_easy_strerror(poll.curl_code));
                } else {
                    std::ostringstream message;
                    message << "Steam QR polling returned HTTP "
                            << poll.status << ".";
                    set_error(message.str());
                }
                return;
            }

            sleep_interruptible(interval, &cancel_login_);
            continue;
        }

        consecutive_failures = 0;

        const std::uint64_t new_client_id =
            json_u64_member(poll.body, "new_client_id");
        if (new_client_id != 0) client_id = new_client_id;

        const std::string new_challenge =
            json_string_member(poll.body, "new_challenge_url");
        if (!new_challenge.empty() && new_challenge != challenge_url) {
            challenge_url = new_challenge;
            std::lock_guard<std::mutex> lock(mutex_);
            qr_url_ = challenge_url;
            state_ = SteamState::WaitingForQr;
            status_ = "Steam refreshed the QR code. Scan the new code.";
        }

        if (json_bool_member(poll.body, "had_remote_interaction", false)) {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = SteamState::Authorizing;
            status_ = "Steam saw the QR scan. Approve the sign-in.";
        }

        const std::string access_token =
            json_string_member(poll.body, "access_token");
        const std::string refresh_token =
            json_string_member(poll.body, "refresh_token");

        if (!access_token.empty() && !refresh_token.empty()) {
            append_log("QR auth approved; token fields present.");
            std::uint64_t steam_id =
                json_u64_member(poll.body, "steamid");
            if (steam_id == 0) {
                steam_id = steam_id_from_token(access_token);
            }
            if (steam_id == 0) {
                steam_id = steam_id_from_token(refresh_token);
            }

            if (steam_id == 0) {
                set_error(
                    "Steam signed in, but SteamVita could not read the SteamID.");
                return;
            }

            std::string account_name =
                json_string_member(poll.body, "account_name");
            if (account_name.empty()) account_name = "Steam account";

            {
                std::lock_guard<std::mutex> lock(mutex_);
                steam_id_ = steam_id;
                account_name_ = std::move(account_name);
                access_token_ = access_token;
                qr_url_.clear();
                state_ = SteamState::LoadingLibrary;
                status_ =
                    "Signed in. Loading your real Steam library...";
            }

            begin_library_fetch();
            return;
        }

        sleep_interruptible(interval, &cancel_login_);
    }

    if (!cancel_login_.load()) {
        set_error("Steam QR sign-in timed out. Press X to try again.");
    }
}

void SteamClient::update() {
    // Network work stays on worker threads. The render loop only reads
    // synchronized state, so a large Steam library cannot stall each frame.
}

void SteamClient::begin_library_fetch() {
    if (library_thread_.joinable() &&
        library_thread_.get_id() != std::this_thread::get_id()) {
        library_thread_.join();
    }

    std::string token;
    std::uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        token = access_token_;
        id = steam_id_;
        state_ = SteamState::LoadingLibrary;
        status_ = "Loading your owned games from Steam...";
    }

    if (token.empty() || id == 0) {
        set_error("Steam session is missing account information.");
        return;
    }

    library_thread_ = std::thread(
        &SteamClient::fetch_library_worker, this, std::move(token), id);
}

void SteamClient::fetch_library_worker(std::string access_token,
                                       std::uint64_t steam_id) {
    CURL* escape = curl_easy_init();
    if (!escape) {
        set_error("Could not prepare the Steam library request.");
        return;
    }

    char* escaped_token = curl_easy_escape(
        escape, access_token.c_str(), static_cast<int>(access_token.size()));
    if (!escaped_token) {
        curl_easy_cleanup(escape);
        set_error("Could not encode the Steam library token.");
        return;
    }

    std::ostringstream url;
    url << OWNED_GAMES_URL
        << "?access_token=" << escaped_token
        << "&steamid=" << steam_id
        << "&include_appinfo=1"
        << "&include_played_free_games=1"
        << "&include_free_sub=1"
        << "&skip_unvetted_apps=0"
        << "&format=json";

    curl_free(escaped_token);
    curl_easy_cleanup(escape);

    HttpResult result = http_get(
        url.str(), ca_bundle_, LIBRARY_RESPONSE_LIMIT,
        &cancel_login_, 60L);

    if (cancel_login_.load()) return;

    {
        std::ostringstream log;
        log << "Library request: curl=" << static_cast<int>(result.curl_code)
            << " http=" << result.status
            << " bytes=" << result.body.size();
        append_log(log.str());
    }

    if (result.overflow) {
        set_error(
            "Your Steam library response exceeded SteamVita's 8 MB safety limit.");
        return;
    }
    if (result.curl_code != CURLE_OK) {
        set_error(std::string("Steam library request failed: ") +
                  curl_easy_strerror(result.curl_code));
        return;
    }
    if (result.status != 200) {
        std::ostringstream message;
        message << "Steam library request returned HTTP "
                << result.status << ".";
        set_error(message.str());
        return;
    }

    const std::uint64_t reported_count =
        json_u64_member(result.body, "game_count");

    std::vector<SteamGame> loaded;
    const std::size_t reserve_count = static_cast<std::size_t>(
        std::min<std::uint64_t>(
            reported_count == 0 ? 1024u : reported_count,
            MAX_LIBRARY_GAMES));
    loaded.reserve(reserve_count);

    parse_games(result.body, &loaded);

    if (reported_count > 0 && loaded.empty()) {
        set_error("Steam returned games, but SteamVita could not parse them.");
        return;
    }

    if (reported_count > MAX_LIBRARY_GAMES ||
        loaded.size() >= MAX_LIBRARY_GAMES) {
        set_error(
            "Steam library is larger than SteamVita's 10,000-game safety limit.");
        return;
    }

    std::sort(loaded.begin(), loaded.end(),
              [](const SteamGame& a, const SteamGame& b) {
                  return a.name < b.name;
              });

    std::string cache_account;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_account = account_name_;
    }
    if (!save_library_cache(loaded, cache_account, steam_id)) {
        append_log("WARN: failed to save library cache.");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        games_.swap(loaded);
        state_ = SteamState::Ready;
        offline_mode_ = false;

        {
            std::ostringstream log;
            log << "Library parsed games=" << games_.size()
                << " reported=" << reported_count;
            append_log(log.str());
        }

        std::ostringstream message;
        message << "Loaded " << games_.size()
                << " games from your Steam account.";
        status_ = message.str();
    }
}

void SteamClient::refresh_library() {
    std::string token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        token = access_token_;
    }

    if (token.empty()) {
        start_qr_login();
        return;
    }

    begin_library_fetch();
}

void SteamClient::sign_out() {
    cancel_login_.store(true);

    if (auth_thread_.joinable() &&
        auth_thread_.get_id() != std::this_thread::get_id()) {
        auth_thread_.join();
    }

    if (library_thread_.joinable() &&
        library_thread_.get_id() != std::this_thread::get_id()) {
        library_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        games_.clear();
        qr_url_.clear();
        account_name_.clear();
        access_token_.clear();
        steam_id_ = 0;
        state_ = SteamState::SignedOut;
        status_ = "Sign in to Steam to load your real game library.";
    }
}

SteamState SteamClient::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::string SteamClient::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

std::string SteamClient::qr_url() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return qr_url_;
}

std::string SteamClient::account_name() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return account_name_;
}

std::uint64_t SteamClient::steam_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return steam_id_;
}

std::vector<SteamGame> SteamClient::games_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return games_;
}

bool SteamClient::network_ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return network_ready_;
}

bool SteamClient::offline_mode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return offline_mode_;
}

bool SteamClient::has_session() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !access_token_.empty() && steam_id_ != 0;
}

void SteamClient::set_error(const std::string& message) {
    const std::string safe_message =
        message.empty() ? "SteamVita encountered an error." : message;
    append_log(std::string("ERROR: ") + safe_message);

    std::lock_guard<std::mutex> lock(mutex_);
    if (!games_.empty()) {
        state_ = SteamState::Ready;
        offline_mode_ = true;
        status_ = "Offline Mode - " + safe_message;
    } else {
        state_ = SteamState::Error;
        status_ = safe_message;
    }
}

void SteamClient::shutdown_network() {
    network_ready_ = false;
    if (curl_initialized_) {
        curl_global_cleanup();
        curl_initialized_ = false;
    }

    if (netctl_initialized_) {
        sceNetCtlTerm();
        netctl_initialized_ = false;
    }

    if (net_initialized_) {
        sceNetTerm();
        net_initialized_ = false;
    }

    if (net_memory_) {
        std::free(net_memory_);
        net_memory_ = nullptr;
    }

    if (net_module_loaded_) {
        sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
        net_module_loaded_ = false;
    }
}
