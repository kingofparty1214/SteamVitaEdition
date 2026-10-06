#include "update_manager.h"

#include "vpk_install.h"

#include <curl/curl.h>
#include <psp2/appmgr.h>
#include <psp2/kernel/processmgr.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace {

constexpr const char* CURRENT_VERSION = "0.11";
constexpr const char* CA_PATH = "ux0:data/SteamVita/cacert.pem";
constexpr const char* MANIFEST_URL =
    "https://github.com/kingofparty1214/SteamVitaEditon/releases/latest/download/update.txt";
constexpr const char* UPDATE_DIR = "ux0:data/SteamVita/update";
constexpr const char* UPDATE_PART = "ux0:data/SteamVita/update/SteamVita.vpk.part";
constexpr const char* UPDATE_VPK = "ux0:data/SteamVita/update/SteamVita.vpk";
constexpr const char* EXPECTED_SHA = "ux0:data/SteamVita/update/expected.sha256";
constexpr const char* HELPER_VPK = "app0:/updater/SteamVitaUpdater.vpk";
constexpr const char* HELPER_STAGE = "ux0:data/SteamVita/updater_pkg";
constexpr const char* HELPER_EBOOT = "ux0:app/STMVUPD01/eboot.bin";
constexpr std::size_t MANIFEST_LIMIT = 16u * 1024u;
constexpr std::size_t UPDATE_LIMIT = 32u * 1024u * 1024u;

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

std::size_t memory_write(void* ptr,
                         std::size_t size,
                         std::size_t count,
                         void* userdata) {
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

std::size_t file_write(void* ptr,
                       std::size_t size,
                       std::size_t count,
                       void* userdata) {
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

int progress_cancel(void* userdata,
                    curl_off_t,
                    curl_off_t,
                    curl_off_t,
                    curl_off_t) {
    auto* cancel = static_cast<std::atomic<bool>*>(userdata);
    return cancel && cancel->load() ? 1 : 0;
}

bool configure_curl(CURL* curl, std::atomic<bool>* cancel) {
    return curl &&
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "SteamVita/0.11") == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_CAINFO, CA_PATH) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 6L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L) == CURLE_OK &&
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
    sink.data.reserve(std::min<std::size_t>(limit, 4096u));

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
        status != 200 || sink.overflow) {
        return false;
    }

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
               !std::isdigit(static_cast<unsigned char>(version[i]))) {
            ++i;
        }
        if (i >= version.size()) break;

        unsigned value = 0;
        while (i < version.size() &&
               std::isdigit(static_cast<unsigned char>(version[i]))) {
            value = value * 10u +
                    static_cast<unsigned>(version[i] - '0');
            ++i;
        }
        parts.push_back(value);
    }
    return parts;
}

int compare_version(const std::string& left,
                    const std::string& right) {
    std::vector<unsigned> a = version_parts(left);
    std::vector<unsigned> b = version_parts(right);
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

} // namespace

UpdateManager::UpdateManager() = default;

UpdateManager::~UpdateManager() {
    cancel_.store(true);
    if (check_thread_.joinable()) check_thread_.join();
    if (download_thread_.joinable()) download_thread_.join();
}

void UpdateManager::initialize(bool network_ready) {
    if (!network_ready) {
        set_state(UpdateState::Disabled,
                  "Update check skipped while offline.");
        return;
    }

    cancel_.store(false);
    set_state(UpdateState::Checking,
              "Checking for SteamVita updates...");
    check_thread_ = std::thread(&UpdateManager::check_worker, this);
}

void UpdateManager::update() {
    // Workers update synchronized state directly. No blocking work runs here.
}

void UpdateManager::check_worker() {
    std::string manifest;
    if (!get_text(MANIFEST_URL, MANIFEST_LIMIT, &cancel_, &manifest)) {
        if (!cancel_.load()) {
            // Update checks must never block normal/offline use.
            set_state(UpdateState::Error,
                      "Update check unavailable; continuing normally.");
        }
        return;
    }

    const std::string version = manifest_value(manifest, "version");
    const std::string url = manifest_value(manifest, "url");
    const std::string sha = manifest_value(manifest, "sha256");

    if (version.empty() || url.empty() || !valid_sha256(sha)) {
        set_state(UpdateState::Error,
                  "Update manifest was invalid; continuing normally.");
        return;
    }

    if (compare_version(CURRENT_VERSION, version) >= 0) {
        set_state(UpdateState::UpToDate,
                  "SteamVita is up to date.");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        remote_version_ = version;
        download_url_ = url;
        expected_sha256_ = sha;
        state_ = UpdateState::Available;
        status_ = "SteamVita v" + version +
                  " available - press START to update.";
    }
}

void UpdateManager::start_update() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != UpdateState::Available) return;
        state_ = UpdateState::Downloading;
        status_ = "Downloading SteamVita v" +
                  remote_version_ + "...";
    }

    if (download_thread_.joinable()) download_thread_.join();
    download_thread_ =
        std::thread(&UpdateManager::download_worker, this);
}

void UpdateManager::download_worker() {
    std::string url;
    std::string sha;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url = download_url_;
        sha = expected_sha256_;
    }

    steamvita::ensure_directory(UPDATE_DIR);
    std::remove(UPDATE_PART);
    std::remove(UPDATE_VPK);

    if (!download_file(url, UPDATE_PART, UPDATE_LIMIT, &cancel_)) {
        if (!cancel_.load()) {
            set_state(UpdateState::Error,
                      "Update download failed. Current version is unchanged.");
        }
        return;
    }

    std::string error;
    if (!steamvita::verify_sha256_file(UPDATE_PART, sha, &error)) {
        std::remove(UPDATE_PART);
        set_state(UpdateState::Error, error);
        return;
    }

    if (std::rename(UPDATE_PART, UPDATE_VPK) != 0) {
        std::remove(UPDATE_PART);
        set_state(UpdateState::Error,
                  "Could not finalize the downloaded update.");
        return;
    }

    {
        std::ofstream out(EXPECTED_SHA,
                          std::ios::trunc);
        if (!out) {
            std::remove(UPDATE_VPK);
            set_state(UpdateState::Error,
                      "Could not save update verification data.");
            return;
        }
        out << sha << "\n";
    }

    set_state(UpdateState::ReadyToInstall,
              "Update verified. Starting SteamVita Updater...");
}

bool UpdateManager::launch_installer(std::string* error_message) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != UpdateState::ReadyToInstall) {
            if (error_message) *error_message = "Update is not ready to install.";
            return false;
        }
    }

    std::string error;
    if (!steamvita::path_exists(HELPER_EBOOT)) {
        if (!steamvita::extract_vpk(
                HELPER_VPK, HELPER_STAGE, &error)) {
            if (error_message) *error_message = error;
            set_state(UpdateState::Error, error);
            return false;
        }

        if (!steamvita::promote_directory(
                HELPER_STAGE, &error)) {
            if (error_message) *error_message = error;
            set_state(UpdateState::Error, error);
            return false;
        }
    }

    const int result = sceAppMgrLaunchAppByUri(
        0xFFFFF, "psgm:play?titleid=STMVUPD01");
    if (result < 0) {
        std::ostringstream message;
        message << "Could not launch SteamVita Updater: 0x"
                << std::hex << static_cast<unsigned>(result);
        if (error_message) *error_message = message.str();
        set_state(UpdateState::Error, message.str());
        return false;
    }

    return true;
}

UpdateState UpdateManager::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::string UpdateManager::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

std::string UpdateManager::remote_version() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return remote_version_;
}

bool UpdateManager::update_available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == UpdateState::Available;
}

void UpdateManager::set_state(UpdateState state,
                              const std::string& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = state;
    status_ = status;
}
