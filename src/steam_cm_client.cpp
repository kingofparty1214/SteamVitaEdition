#include "steam_cm_client.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace {

constexpr const char* CA_PATH = "ux0:data/SteamVita/cacert.pem";
constexpr const char* CM_LIST_URL =
    "https://api.steampowered.com/ISteamDirectory/GetCMList/v1/"
    "?cellid=0&maxcount=32&format=json";
constexpr std::size_t RESPONSE_LIMIT = 256u * 1024u;

struct CurlBuffer {
    std::string data;
    bool overflow = false;
};

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
