#include "runtime_pack.h"

#include "vpk_install.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace {

#ifndef STEAMVITA_RUNTIME_MANIFEST_URL
#define STEAMVITA_RUNTIME_MANIFEST_URL \
    "https://github.com/kingofparty1214/SteamVitaEdition/releases/download/dev-latest/runtime.txt"
#endif

#ifndef STEAMVITA_VERSION
#define STEAMVITA_VERSION "0.13.0"
#endif

constexpr const char* MANIFEST_URL = STEAMVITA_RUNTIME_MANIFEST_URL;
constexpr const char* CA_PATH = "ux0:data/SteamVita/cacert.pem";
constexpr const char* RUNTIME_DIR = "ux0:data/SteamVita/runtime";
constexpr const char* RUNTIME_STAGE = "ux0:data/SteamVita/runtime.stage";
constexpr const char* DOWNLOAD_DIR = "ux0:data/SteamVita/runtime_download";
constexpr const char* DOWNLOAD_PART = "ux0:data/SteamVita/runtime_download/SteamVitaRuntime.zip.part";
constexpr const char* DOWNLOAD_ZIP = "ux0:data/SteamVita/runtime_download/SteamVitaRuntime.zip";
constexpr const char* VERSION_PATH = "ux0:data/SteamVita/runtime/runtime.version";
constexpr const char* STAGE_VERSION_PATH = "ux0:data/SteamVita/runtime.stage/runtime.version";
constexpr std::size_t MANIFEST_LIMIT = 16u * 1024u;
constexpr std::size_t RUNTIME_LIMIT = 128u * 1024u * 1024u;

struct MemorySink {
    std::string data;
    std::size_t limit = 0;
    bool overflow = false;
};

struct FileSink {
    FILE* file = nullptr;
    std::size_t total = 0;
    std::size_t limit = 0;
    bool overflow = false;
};

std::size_t memory_write(void* ptr, std::size_t size,
                         std::size_t count, void* userdata) {
    auto* sink = static_cast<MemorySink*>(userdata);
    const std::size_t bytes = size * count;
    if (!sink || bytes > sink->limit ||
        sink->data.size() > sink->limit - bytes) {
        if (sink) sink->overflow = true;
        return 0;
    }
    sink->data.append(static_cast<const char*>(ptr), bytes);
    return bytes;
}

std::size_t file_write(void* ptr, std::size_t size,
                       std::size_t count, void* userdata) {
    auto* sink = static_cast<FileSink*>(userdata);
    const std::size_t bytes = size * count;
    if (!sink || !sink->file || bytes > sink->limit ||
        sink->total > sink->limit - bytes) {
        if (sink) sink->overflow = true;
        return 0;
    }
    const std::size_t written = std::fwrite(ptr, 1, bytes, sink->file);
    sink->total += written;
    return written;
}

int progress_cancel(void* userdata, curl_off_t, curl_off_t,
                    curl_off_t, curl_off_t) {
    auto* cancel = static_cast<std::atomic<bool>*>(userdata);
    return cancel && cancel->load() ? 1 : 0;
}

bool configure_curl(CURL* curl, std::atomic<bool>* cancel) {
    return curl &&
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "SteamVita/" STEAMVITA_VERSION) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_CAINFO, CA_PATH) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 6L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity") == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cancel) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel) == CURLE_OK;
}

bool get_text(const std::string& url,
              std::size_t limit,
              std::atomic<bool>* cancel,
              std::string* output) {
    if (!output) return false;
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    MemorySink sink;
    sink.limit = limit;

    const bool configured =
        configure_curl(curl, cancel) &&
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, memory_write) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink) == CURLE_OK;

    CURLcode result = CURLE_FAILED_INIT;
    long status = 0;
    if (configured) {
        result = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }
    curl_easy_cleanup(curl);

    if (!configured || result != CURLE_OK ||
        status != 200 || sink.overflow) return false;

    *output = std::move(sink.data);
    return true;
}

bool download_file(const std::string& url,
                   const std::string& destination,
                   std::size_t limit,
                   std::atomic<bool>* cancel) {
    FILE* file = std::fopen(destination.c_str(), "wb");
    if (!file) return false;

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::fclose(file);
        std::remove(destination.c_str());
        return false;
    }

    FileSink sink;
    sink.file = file;
    sink.limit = limit;

    const bool configured =
        configure_curl(curl, cancel) &&
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, file_write) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink) == CURLE_OK;

    CURLcode result = CURLE_FAILED_INIT;
    long status = 0;
    if (configured) {
        result = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }

    curl_easy_cleanup(curl);
    std::fclose(file);

    if (!configured || result != CURLE_OK ||
        status != 200 || sink.overflow || sink.total == 0) {
        std::remove(destination.c_str());
        return false;
    }
    return true;
}

std::string trim(std::string value) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string manifest_value(const std::string& manifest,
                           const std::string& key) {
    std::istringstream input(manifest);
    std::string line;
    const std::string prefix = key + "=";
    while (std::getline(input, line)) {
        if (line.compare(0, prefix.size(), prefix) == 0) {
            return trim(line.substr(prefix.size()));
        }
    }
    return {};
}

std::vector<unsigned> version_parts(const std::string& version) {
    std::vector<unsigned> parts;
    std::size_t i = 0;
    while (i < version.size() && parts.size() < 4u) {
        while (i < version.size() &&
               !std::isdigit(static_cast<unsigned char>(version[i]))) ++i;
        if (i >= version.size()) break;
        unsigned value = 0;
        while (i < version.size() &&
               std::isdigit(static_cast<unsigned char>(version[i]))) {
            value = value * 10u + static_cast<unsigned>(version[i] - '0');
            ++i;
        }
        parts.push_back(value);
    }
    return parts;
}

int compare_version(const std::string& left,
                    const std::string& right) {
    const auto a = version_parts(left);
    const auto b = version_parts(right);
    const std::size_t count = std::max(a.size(), b.size());
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned av = i < a.size() ? a[i] : 0u;
        const unsigned bv = i < b.size() ? b[i] : 0u;
        if (av < bv) return -1;
        if (av > bv) return 1;
    }
    return 0;
}

bool valid_sha256(const std::string& value) {
    if (value.size() != 64u) return false;
    for (char c : value) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

std::string read_first_line(const char* path) {
    std::ifstream input(path);
    std::string value;
    std::getline(input, value);
    return trim(value);
}

} // namespace

RuntimePackManager::RuntimePackManager() = default;

RuntimePackManager::~RuntimePackManager() {
    cancel_.store(true);
    if (check_thread_.joinable()) check_thread_.join();
    if (install_thread_.joinable()) install_thread_.join();
}

void RuntimePackManager::initialize(bool network_ready) {
    installed_version_ = read_first_line(VERSION_PATH);
    steamvita::remove_tree(RUNTIME_STAGE);
    std::remove(DOWNLOAD_PART);

    if (!network_ready) {
        set_state(installed_version_.empty()
            ? RuntimePackState::Missing
            : RuntimePackState::Ready,
            installed_version_.empty()
                ? "Vita Proton runtime pack is not installed."
                : "Vita Proton runtime pack ready (offline).");
        return;
    }

    cancel_.store(false);
    set_state(RuntimePackState::Checking,
              "Checking Vita Proton runtime pack...");
    check_thread_ = std::thread(&RuntimePackManager::check_worker, this);
}

bool RuntimePackManager::force_check(bool network_ready) {
    if (!network_ready) {
        set_state(RuntimePackState::Disabled,
                  "Cannot check runtime pack while offline.");
        return false;
    }
    cancel_.store(true);
    if (check_thread_.joinable()) check_thread_.join();
    cancel_.store(false);
    set_state(RuntimePackState::Checking,
              "Checking Vita Proton runtime pack...");
    check_thread_ = std::thread(&RuntimePackManager::check_worker, this);
    return true;
}

void RuntimePackManager::check_worker() {
    std::string manifest;
    if (!get_text(MANIFEST_URL, MANIFEST_LIMIT, &cancel_, &manifest)) {
        if (!cancel_.load()) {
            set_state(installed_version_.empty()
                ? RuntimePackState::Missing
                : RuntimePackState::Ready,
                installed_version_.empty()
                    ? "Runtime pack check unavailable; pack is not installed."
                    : "Runtime pack check unavailable; using installed pack.");
        }
        return;
    }

    const std::string version = manifest_value(manifest, "version");
    const std::string url = manifest_value(manifest, "url");
    const std::string sha = manifest_value(manifest, "sha256");

    if (version.empty() || url.empty() || !valid_sha256(sha)) {
        set_state(RuntimePackState::Error,
                  "Runtime pack manifest is invalid.");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        remote_version_ = version;
        download_url_ = url;
        expected_sha256_ = sha;
    }

    if (!installed_version_.empty() &&
        compare_version(installed_version_, version) >= 0) {
        set_state(RuntimePackState::UpToDate,
                  "Vita Proton runtime pack is up to date.");
        return;
    }

    set_state(RuntimePackState::Available,
              installed_version_.empty()
                  ? "Vita Proton runtime pack is available."
                  : "Vita Proton runtime pack update is available.");
}

bool RuntimePackManager::start_install() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != RuntimePackState::Available) return false;
        state_ = RuntimePackState::Downloading;
        status_ = "Downloading Vita Proton runtime pack...";
    }

    if (install_thread_.joinable()) install_thread_.join();
    install_thread_ = std::thread(&RuntimePackManager::install_worker, this);
    return true;
}

void RuntimePackManager::install_worker() {
    std::string url;
    std::string sha;
    std::string version;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url = download_url_;
        sha = expected_sha256_;
        version = remote_version_;
    }

    steamvita::ensure_directory(DOWNLOAD_DIR);
    std::remove(DOWNLOAD_PART);
    std::remove(DOWNLOAD_ZIP);

    if (!download_file(url, DOWNLOAD_PART, RUNTIME_LIMIT, &cancel_)) {
        if (!cancel_.load()) {
            set_state(RuntimePackState::Error,
                      "Runtime pack download failed.");
        }
        return;
    }

    std::string error;
    if (!steamvita::verify_sha256_file(DOWNLOAD_PART, sha, &error)) {
        std::remove(DOWNLOAD_PART);
        set_state(RuntimePackState::Error, error);
        return;
    }

    if (std::rename(DOWNLOAD_PART, DOWNLOAD_ZIP) != 0) {
        std::remove(DOWNLOAD_PART);
        set_state(RuntimePackState::Error,
                  "Could not finalize runtime pack download.");
        return;
    }

    set_state(RuntimePackState::Installing,
              "Installing Vita Proton runtime pack...");

    steamvita::remove_tree(RUNTIME_STAGE);
    if (!steamvita::extract_vpk(DOWNLOAD_ZIP, RUNTIME_STAGE, &error)) {
        steamvita::remove_tree(RUNTIME_STAGE);
        set_state(RuntimePackState::Error, error);
        return;
    }

    const std::string staged_version = read_first_line(STAGE_VERSION_PATH);
    if (staged_version.empty() || compare_version(staged_version, version) != 0) {
        steamvita::remove_tree(RUNTIME_STAGE);
        set_state(RuntimePackState::Error,
                  "Runtime pack version verification failed.");
        return;
    }

    steamvita::remove_tree(RUNTIME_DIR);
    if (std::rename(RUNTIME_STAGE, RUNTIME_DIR) != 0) {
        steamvita::remove_tree(RUNTIME_STAGE);
        set_state(RuntimePackState::Error,
                  "Could not activate Vita Proton runtime pack.");
        return;
    }

    std::remove(DOWNLOAD_ZIP);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        installed_version_ = version;
    }
    set_state(RuntimePackState::Ready,
              "Vita Proton runtime pack installed.");
}

RuntimePackState RuntimePackManager::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::string RuntimePackManager::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

std::string RuntimePackManager::installed_version() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return installed_version_;
}

std::string RuntimePackManager::remote_version() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return remote_version_;
}

bool RuntimePackManager::runtime_ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !installed_version_.empty() &&
           (state_ == RuntimePackState::Ready ||
            state_ == RuntimePackState::UpToDate ||
            state_ == RuntimePackState::Available);
}

bool RuntimePackManager::update_available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == RuntimePackState::Available;
}

const char* RuntimePackManager::runtime_root() {
    return RUNTIME_DIR;
}

void RuntimePackManager::set_state(RuntimePackState state,
                                   const std::string& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = state;
    status_ = status;
}

const char* runtime_pack_state_label(RuntimePackState state) {
    switch (state) {
        case RuntimePackState::Disabled: return "Disabled";
        case RuntimePackState::Checking: return "Checking";
        case RuntimePackState::Missing: return "Missing";
        case RuntimePackState::UpToDate: return "Up to date";
        case RuntimePackState::Available: return "Available";
        case RuntimePackState::Downloading: return "Downloading";
        case RuntimePackState::Installing: return "Installing";
        case RuntimePackState::Ready: return "Ready";
        case RuntimePackState::Error: return "Error";
    }
    return "Unknown";
}
