#include "game_installer.h"
#include "steam_cm_client.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace {

constexpr const char* GAME_ROOT = "ux0:data/SteamVita/games";
constexpr const char* CA_PATH = "ux0:data/SteamVita/cacert.pem";
constexpr std::size_t SERVER_RESPONSE_LIMIT = 512u * 1024u;

struct CurlBuffer {
    std::string data;
    std::size_t limit = 0;
    bool overflow = false;
};

std::size_t write_limited(void* ptr, std::size_t size,
                          std::size_t count, void* userdata) {
    if (!userdata) return 0;
    auto* buffer = static_cast<CurlBuffer*>(userdata);
    const std::size_t bytes = size * count;
    if (bytes > buffer->limit ||
        buffer->data.size() > buffer->limit - bytes) {
        buffer->overflow = true;
        return 0;
    }
    buffer->data.append(static_cast<const char*>(ptr), bytes);
    return bytes;
}

int cancel_progress(void* userdata,
                    curl_off_t, curl_off_t,
                    curl_off_t, curl_off_t) {
    auto* cancelled = static_cast<std::atomic<bool>*>(userdata);
    return cancelled && cancelled->load() ? 1 : 0;
}

bool mkdir_if_needed(const std::string& path) {
    struct stat info {};
    if (stat(path.c_str(), &info) == 0) return S_ISDIR(info.st_mode);
    return mkdir(path.c_str(), 0777) == 0;
}

std::string json_string_after(const std::string& text,
                              const std::string& key,
                              std::size_t start) {
    const std::string needle = "\"" + key + "\"";
    std::size_t pos = text.find(needle, start);
    if (pos == std::string::npos) return {};
    pos = text.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    ++pos;
    while (pos < text.size() &&
           std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
    if (pos >= text.size() || text[pos] != '"') return {};
    ++pos;

    std::string value;
    bool escape = false;
    while (pos < text.size()) {
        const char c = text[pos++];
        if (escape) {
            switch (c) {
                case '"': value.push_back('"'); break;
                case '\\': value.push_back('\\'); break;
                case '/': value.push_back('/'); break;
                default: value.push_back(c); break;
            }
            escape = false;
        } else if (c == '\\') {
            escape = true;
        } else if (c == '"') {
            return value;
        } else {
            value.push_back(c);
        }
    }
    return {};
}

long json_int_after(const std::string& text,
                    const std::string& key,
                    std::size_t start,
                    long fallback = 0) {
    const std::string needle = "\"" + key + "\"";
    std::size_t pos = text.find(needle, start);
    if (pos == std::string::npos) return fallback;
    pos = text.find(':', pos + needle.size());
    if (pos == std::string::npos) return fallback;
    ++pos;
    while (pos < text.size() &&
           std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;

    bool negative = false;
    if (pos < text.size() && text[pos] == '-') {
        negative = true;
        ++pos;
    }

    long value = 0;
    bool saw_digit = false;
    while (pos < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[pos]))) {
        saw_digit = true;
        value = value * 10 + (text[pos++] - '0');
    }
    if (!saw_digit) return fallback;
    return negative ? -value : value;
}

} // namespace

GameInstaller::GameInstaller() = default;

GameInstaller::~GameInstaller() {
    cancel();
    if (worker_.joinable()) worker_.join();
}

bool GameInstaller::start_install(
        std::uint32_t app_id,
        const std::string& game_name,
        const SteamSessionCredentials& credentials) {
    if (!credentials.valid()) {
        fail("Steam session is not ready for game downloads.");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshot_.active()) return false;
    }

    if (worker_.joinable()) worker_.join();

    cancel_.store(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = {};
        snapshot_.state = InstallState::Preparing;
        snapshot_.app_id = app_id;
        snapshot_.game_name = game_name;
        snapshot_.status = "Preparing Steam install...";
    }

    worker_ = std::thread(
        &GameInstaller::worker, this, app_id, game_name, credentials);
    return true;
}

void GameInstaller::cancel() {
    cancel_.store(true);
}

void GameInstaller::update() {
    // Network/install work runs on the worker thread.
}

InstallSnapshot GameInstaller::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void GameInstaller::set_state(InstallState state,
                              const std::string& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.state = state;
    snapshot_.status = status;
}

void GameInstaller::set_progress(
        std::uint64_t downloaded_bytes,
        std::uint64_t total_bytes,
        std::uint64_t bytes_per_second) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.downloaded_bytes = downloaded_bytes;
    snapshot_.total_bytes = total_bytes;
    snapshot_.bytes_per_second = bytes_per_second;
    if (bytes_per_second > 0 && total_bytes > downloaded_bytes) {
        snapshot_.eta_seconds =
            (total_bytes - downloaded_bytes + bytes_per_second - 1) /
            bytes_per_second;
    } else {
        snapshot_.eta_seconds = 0;
    }
}

void GameInstaller::fail(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.state = InstallState::Error;
    snapshot_.status = message;
}

bool GameInstaller::discover_content_servers(
        std::vector<ContentServer>* servers,
        std::string* error) {
    if (!servers) return false;
    servers->clear();

    const char* url =
        "https://api.steampowered.com/"
        "IContentServerDirectoryService/GetServersForSteamPipe/v1/"
        "?cell_id=0&max_servers=20&format=json";

    CURL* curl = curl_easy_init();
    if (!curl) {
        if (error) *error = "Could not initialize Steam CDN discovery.";
        return false;
    }

    CurlBuffer buffer;
    buffer.limit = SERVER_RESPONSE_LIMIT;

    const bool configured =
        curl_easy_setopt(curl, CURLOPT_URL, url) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "SteamVita/0.13") == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_CAINFO, CA_PATH) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity") == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_limited) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cancel_progress) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancel_) == CURLE_OK;

    if (!configured) {
        curl_easy_cleanup(curl);
        if (error) *error = "Could not configure Steam CDN discovery.";
        return false;
    }

    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if (cancel_.load()) {
        if (error) *error = "Install cancelled.";
        return false;
    }

    if (result != CURLE_OK || status != 200 || buffer.overflow) {
        if (error) {
            std::ostringstream out;
            out << "Steam CDN discovery failed (HTTP " << status << ").";
            *error = out.str();
        }
        return false;
    }

    std::size_t pos = 0;
    while ((pos = buffer.data.find("\"host\"", pos)) != std::string::npos) {
        ContentServer server;
        server.host = json_string_after(buffer.data, "host", pos);
        server.vhost = json_string_after(buffer.data, "vhost", pos);
        const std::string https =
            json_string_after(buffer.data, "https_support", pos);
        server.port = static_cast<int>(
            json_int_after(buffer.data, "port", pos, 443));
        server.weighted_load = static_cast<int>(
            json_int_after(buffer.data, "weighted_load", pos, 0));
        server.https = https == "mandatory" || server.port == 443;

        if (!server.host.empty()) servers->push_back(std::move(server));
        ++pos;
    }

    std::stable_sort(servers->begin(), servers->end(),
        [](const ContentServer& a, const ContentServer& b) {
            return a.weighted_load < b.weighted_load;
        });

    if (servers->empty()) {
        if (error) *error = "Steam returned no usable content servers.";
        return false;
    }

    return true;
}

void GameInstaller::worker(
        std::uint32_t app_id,
        std::string game_name,
        SteamSessionCredentials credentials) {
    (void)credentials;

    if (!mkdir_if_needed(GAME_ROOT)) {
        fail("Could not create SteamVita game storage.");
        return;
    }

    const std::string app_dir =
        std::string(GAME_ROOT) + "/" + std::to_string(app_id);
    if (!mkdir_if_needed(app_dir)) {
        fail("Could not create the game's install folder.");
        return;
    }

    set_state(InstallState::DiscoveringContentServers,
              "Finding the best Steam content servers...");

    std::vector<ContentServer> servers;
    std::string error;
    if (!discover_content_servers(&servers, &error)) {
        if (cancel_.load()) {
            set_state(InstallState::Idle, "Install cancelled.");
        } else {
            fail(error);
        }
        return;
    }

    if (cancel_.load()) {
        set_state(InstallState::Idle, "Install cancelled.");
        return;
    }

    set_state(
        InstallState::ResolvingApp,
        "Steam CDN is reachable. Finding Steam connection managers...");

    std::vector<SteamCmEndpoint> cm_servers;
    if (!discover_steam_cm_servers(&cm_servers, &cancel_, &error)) {
        if (cancel_.load()) {
            set_state(InstallState::Idle, "Install cancelled.");
        } else {
            fail(error);
        }
        return;
    }

    if (cancel_.load()) {
        set_state(InstallState::Idle, "Install cancelled.");
        return;
    }

    {
        std::ostringstream status;
        status << "Found " << cm_servers.size()
               << " Steam CM server"
               << (cm_servers.size() == 1 ? "" : "s")
               << ". Securing a Steam connection...";
        set_state(InstallState::ResolvingApp, status.str());
    }

    SteamCmConnection cm;
    if (!cm.connect_secure(cm_servers, &cancel_, &error)) {
        if (cancel_.load()) {
            set_state(InstallState::Idle, "Install cancelled.");
        } else {
            fail(error);
        }
        return;
    }

    {
        std::ostringstream status;
        status << "Secure Steam CM channel established via "
               << cm.endpoint().host << ":" << cm.endpoint().port
               << ". Preparing account logon...";
        set_state(InstallState::ResolvingApp, status.str());
    }

    // Next protocol layer:
    // ClientHello + encrypted protobuf ClientLogon using the QR-issued
    // refresh token -> ClientLicenseList -> PICS app info -> Windows depot
    // selection -> depot key + manifest request code -> CDN manifest/chunks.
    fail(
        "Secure Steam CM transport is working. "
        "Encrypted account logon and license retrieval are next.");
}
