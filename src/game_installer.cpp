#include "game_installer.h"
#include "steam_cm_client.h"

#include <curl/curl.h>
#include "miniz.h"

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
constexpr std::size_t MANIFEST_RESPONSE_LIMIT = 64u * 1024u * 1024u;

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

struct FileWriteContext {
    std::ofstream* out = nullptr;
    std::uint64_t bytes = 0;
    std::uint64_t limit = 0;
    bool overflow = false;
};

std::size_t write_file_limited(void* ptr, std::size_t size,
                               std::size_t count, void* userdata) {
    if (!userdata) return 0;
    auto* ctx = static_cast<FileWriteContext*>(userdata);
    if (!ctx->out || !*ctx->out) return 0;

    const std::size_t bytes = size * count;
    if (ctx->limit != 0 &&
        (bytes > ctx->limit ||
         ctx->bytes > ctx->limit - bytes)) {
        ctx->overflow = true;
        return 0;
    }

    ctx->out->write(
        static_cast<const char*>(ptr),
        static_cast<std::streamsize>(bytes));
    if (!*ctx->out) return 0;

    ctx->bytes += static_cast<std::uint64_t>(bytes);
    return bytes;
}

bool file_starts_with_zip_signature(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    unsigned char sig[4]{};
    in.read(reinterpret_cast<char*>(sig), sizeof(sig));
    if (in.gcount() != 4) return false;
    return sig[0] == 0x50u &&
           sig[1] == 0x4bu &&
           (sig[2] == 0x03u || sig[2] == 0x05u || sig[2] == 0x07u) &&
           (sig[3] == 0x04u || sig[3] == 0x06u || sig[3] == 0x08u);
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

struct DepotManifestStats {
    std::uint32_t depot_id = 0;
    std::uint64_t manifest_id = 0;
    bool filenames_encrypted = false;
    std::uint64_t total_uncompressed = 0;
    std::uint64_t total_compressed = 0;
    std::uint32_t unique_chunks = 0;
    std::uint64_t files = 0;
    std::uint64_t chunks = 0;
    std::uint64_t chunk_compressed = 0;
    std::uint64_t chunk_uncompressed = 0;
};

std::uint32_t read_le32_local(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8u) |
           (static_cast<std::uint32_t>(p[2]) << 16u) |
           (static_cast<std::uint32_t>(p[3]) << 24u);
}

bool read_varint_local(
        const unsigned char* data,
        std::size_t size,
        std::size_t* offset,
        std::uint64_t* value) {
    if (!data || !offset || !value) return false;
    std::uint64_t result = 0;
    unsigned shift = 0;
    while (*offset < size && shift < 64u) {
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

bool skip_proto_local(
        const unsigned char* data,
        std::size_t size,
        std::size_t* offset,
        unsigned wire) {
    if (!data || !offset) return false;
    switch (wire) {
        case 0: {
            std::uint64_t ignored = 0;
            return read_varint_local(data, size, offset, &ignored);
        }
        case 1:
            if (*offset + 8u > size) return false;
            *offset += 8u;
            return true;
        case 2: {
            std::uint64_t length = 0;
            if (!read_varint_local(data, size, offset, &length) ||
                length > size - *offset) {
                return false;
            }
            *offset += static_cast<std::size_t>(length);
            return true;
        }
        case 5:
            if (*offset + 4u > size) return false;
            *offset += 4u;
            return true;
        default:
            return false;
    }
}

bool parse_manifest_chunk(
        const unsigned char* data,
        std::size_t size,
        DepotManifestStats* stats) {
    if (!data || !stats) return false;
    std::size_t offset = 0;
    std::uint64_t compressed = 0;
    std::uint64_t original = 0;
    while (offset < size) {
        std::uint64_t tag = 0;
        if (!read_varint_local(data, size, &offset, &tag)) return false;
        const std::uint32_t field =
            static_cast<std::uint32_t>(tag >> 3u);
        const unsigned wire =
            static_cast<unsigned>(tag & 7u);

        if ((field == 4u || field == 5u) && wire == 0u) {
            std::uint64_t value = 0;
            if (!read_varint_local(data, size, &offset, &value)) {
                return false;
            }
            if (field == 4u) original = value;
            else compressed = value;
        } else if (!skip_proto_local(data, size, &offset, wire)) {
            return false;
        }
    }

    ++stats->chunks;
    stats->chunk_compressed += compressed;
    stats->chunk_uncompressed += original;
    return true;
}

bool parse_manifest_file(
        const unsigned char* data,
        std::size_t size,
        DepotManifestStats* stats) {
    if (!data || !stats) return false;
    std::size_t offset = 0;
    while (offset < size) {
        std::uint64_t tag = 0;
        if (!read_varint_local(data, size, &offset, &tag)) return false;
        const std::uint32_t field =
            static_cast<std::uint32_t>(tag >> 3u);
        const unsigned wire =
            static_cast<unsigned>(tag & 7u);

        if (field == 6u && wire == 2u) {
            std::uint64_t length = 0;
            if (!read_varint_local(data, size, &offset, &length) ||
                length > size - offset) {
                return false;
            }
            if (!parse_manifest_chunk(
                    data + offset,
                    static_cast<std::size_t>(length),
                    stats)) {
                return false;
            }
            offset += static_cast<std::size_t>(length);
        } else if (!skip_proto_local(data, size, &offset, wire)) {
            return false;
        }
    }

    ++stats->files;
    return true;
}

bool parse_manifest_payload(
        const unsigned char* data,
        std::size_t size,
        DepotManifestStats* stats) {
    if (!data || !stats) return false;
    std::size_t offset = 0;
    while (offset < size) {
        std::uint64_t tag = 0;
        if (!read_varint_local(data, size, &offset, &tag)) return false;
        const std::uint32_t field =
            static_cast<std::uint32_t>(tag >> 3u);
        const unsigned wire =
            static_cast<unsigned>(tag & 7u);

        if (field == 1u && wire == 2u) {
            std::uint64_t length = 0;
            if (!read_varint_local(data, size, &offset, &length) ||
                length > size - offset) {
                return false;
            }
            if (!parse_manifest_file(
                    data + offset,
                    static_cast<std::size_t>(length),
                    stats)) {
                return false;
            }
            offset += static_cast<std::size_t>(length);
        } else if (!skip_proto_local(data, size, &offset, wire)) {
            return false;
        }
    }
    return true;
}

bool parse_manifest_metadata(
        const unsigned char* data,
        std::size_t size,
        DepotManifestStats* stats) {
    if (!data || !stats) return false;
    std::size_t offset = 0;
    while (offset < size) {
        std::uint64_t tag = 0;
        if (!read_varint_local(data, size, &offset, &tag)) return false;
        const std::uint32_t field =
            static_cast<std::uint32_t>(tag >> 3u);
        const unsigned wire =
            static_cast<unsigned>(tag & 7u);

        if ((field == 1u || field == 2u || field == 4u ||
             field == 5u || field == 6u || field == 7u) &&
            wire == 0u) {
            std::uint64_t value = 0;
            if (!read_varint_local(data, size, &offset, &value)) {
                return false;
            }
            switch (field) {
                case 1u:
                    stats->depot_id =
                        static_cast<std::uint32_t>(value);
                    break;
                case 2u:
                    stats->manifest_id = value;
                    break;
                case 4u:
                    stats->filenames_encrypted = value != 0;
                    break;
                case 5u:
                    stats->total_uncompressed = value;
                    break;
                case 6u:
                    stats->total_compressed = value;
                    break;
                case 7u:
                    stats->unique_chunks =
                        static_cast<std::uint32_t>(value);
                    break;
            }
        } else if (!skip_proto_local(data, size, &offset, wire)) {
            return false;
        }
    }
    return true;
}

bool parse_depot_manifest_binary(
        const std::string& path,
        DepotManifestStats* stats,
        std::string* error) {
    if (!stats) return false;
    *stats = {};

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        if (error) *error = "Could not open extracted depot manifest.";
        return false;
    }

    const std::streamoff end = in.tellg();
    if (end <= 0 ||
        static_cast<std::uint64_t>(end) >
            MANIFEST_RESPONSE_LIMIT) {
        if (error) *error = "Extracted depot manifest has an invalid size.";
        return false;
    }

    std::vector<unsigned char> data(
        static_cast<std::size_t>(end));
    in.seekg(0, std::ios::beg);
    in.read(
        reinterpret_cast<char*>(data.data()),
        static_cast<std::streamsize>(data.size()));
    if (!in) {
        if (error) *error = "Could not read extracted depot manifest.";
        return false;
    }

    constexpr std::uint32_t PAYLOAD_MAGIC = 0x71F617D0u;
    constexpr std::uint32_t METADATA_MAGIC = 0x1F4812BEu;
    constexpr std::uint32_t SIGNATURE_MAGIC = 0x1B81B817u;
    constexpr std::uint32_t END_MAGIC = 0x32C415ABu;

    bool saw_payload = false;
    bool saw_metadata = false;
    bool saw_signature = false;
    bool saw_end = false;

    std::size_t offset = 0;
    while (offset + 4u <= data.size()) {
        const std::uint32_t magic =
            read_le32_local(data.data() + offset);
        offset += 4u;

        if (magic == END_MAGIC) {
            saw_end = true;
            break;
        }

        if (offset + 4u > data.size()) {
            if (error) *error = "Steam manifest section length is missing.";
            return false;
        }

        const std::uint32_t length =
            read_le32_local(data.data() + offset);
        offset += 4u;
        if (length > data.size() - offset) {
            if (error) *error = "Steam manifest section is truncated.";
            return false;
        }

        const unsigned char* section = data.data() + offset;
        if (magic == PAYLOAD_MAGIC) {
            if (!parse_manifest_payload(section, length, stats)) {
                if (error) *error = "Steam manifest payload protobuf is invalid.";
                return false;
            }
            saw_payload = true;
        } else if (magic == METADATA_MAGIC) {
            if (!parse_manifest_metadata(section, length, stats)) {
                if (error) *error = "Steam manifest metadata protobuf is invalid.";
                return false;
            }
            saw_metadata = true;
        } else if (magic == SIGNATURE_MAGIC) {
            saw_signature = true;
        } else {
            if (error) {
                std::ostringstream out;
                out << "Steam manifest has unknown section 0x"
                    << std::hex << magic << ".";
                *error = out.str();
            }
            return false;
        }

        offset += length;
    }

    if (!saw_payload || !saw_metadata ||
        !saw_signature || !saw_end) {
        if (error) {
            *error =
                "Steam manifest is missing required protobuf sections.";
        }
        return false;
    }

    return true;
}

bool extract_single_manifest_zip(
        const std::string& zip_path,
        const std::string& raw_path,
        std::string* error) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zip_path.c_str(), 0)) {
        if (error) *error = "Could not open Steam manifest ZIP.";
        return false;
    }

    const mz_uint files = mz_zip_reader_get_num_files(&zip);
    if (files != 1u) {
        mz_zip_reader_end(&zip);
        if (error) *error = "Steam manifest ZIP did not contain exactly one file.";
        return false;
    }

    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&zip, 0, &stat) ||
        stat.m_uncomp_size == 0 ||
        stat.m_uncomp_size > MANIFEST_RESPONSE_LIMIT) {
        mz_zip_reader_end(&zip);
        if (error) *error = "Steam manifest ZIP entry has an invalid size.";
        return false;
    }

    const bool ok =
        mz_zip_reader_extract_to_file(
            &zip, 0, raw_path.c_str(), 0);
    mz_zip_reader_end(&zip);

    if (!ok) {
        std::remove(raw_path.c_str());
        if (error) *error = "Could not extract Steam depot manifest.";
        return false;
    }

    return true;
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
               << ". Logging into your Steam account...";
        set_state(InstallState::ResolvingApp, status.str());
    }

    std::vector<SteamCmLicense> licenses;
    std::string license_status;
    if (!cm.logon_and_fetch_licenses(
            credentials.refresh_token,
            credentials.account_name,
            credentials.steam_id,
            &licenses,
            &cancel_,
            &license_status)) {
        if (cancel_.load()) {
            set_state(InstallState::Idle, "Install cancelled.");
        } else {
            fail(license_status.empty()
                     ? "Steam CM account logon failed."
                     : license_status);
        }
        return;
    }

    const std::uint32_t own_account_id =
        static_cast<std::uint32_t>(
            credentials.steam_id & 0xffffffffull);
    std::size_t shared_count = 0;
    for (const SteamCmLicense& license : licenses) {
        if (license.owner_id != 0 &&
            license.owner_id != own_account_id) {
            ++shared_count;
        }
    }

    {
        std::ostringstream status;
        status << "Steam licenses loaded: "
               << licenses.size() << " packages, "
               << shared_count << " Family Shared. "
               << "Resolving package contents...";
        set_state(InstallState::ResolvingApp, status.str());
    }

    std::vector<SteamCmSharedApp> package_apps;
    std::string package_status;
    if (!cm.fetch_shared_package_apps(
            licenses,
            credentials.steam_id,
            &package_apps,
            &cancel_,
            &package_status,
            false)) {
        if (cancel_.load()) {
            set_state(InstallState::Idle, "Install cancelled.");
        } else {
            fail(package_status.empty()
                     ? "Steam package contents could not be resolved."
                     : package_status);
        }
        return;
    }

    const auto app_it = std::find_if(
        package_apps.begin(),
        package_apps.end(),
        [app_id](const SteamCmSharedApp& app) {
            return app.app_id == app_id;
        });

    if (app_it == package_apps.end()) {
        std::ostringstream message;
        message << "Steam licenses loaded, but no package resolved AppID "
                << app_id << ".";
        fail(message.str());
        return;
    }

    {
        std::ostringstream status;
        status << "Resolved AppID " << app_id
               << " to package " << app_it->package_id
               << ". Preparing depot metadata...";
        set_state(InstallState::ResolvingApp, status.str());
    }

    std::vector<SteamCmDepotInfo> depots;
    std::string depot_status;
    if (!cm.fetch_app_depots(
            app_id,
            &depots,
            &cancel_,
            &depot_status)) {
        if (cancel_.load()) {
            set_state(InstallState::Idle, "Install cancelled.");
        } else {
            fail(depot_status.empty()
                     ? "Steam depot metadata could not be resolved."
                     : depot_status);
        }
        return;
    }

    std::vector<SteamCmDepotInfo> windows_depots;
    for (const SteamCmDepotInfo& depot : depots) {
        std::string os = depot.os_list;
        std::transform(
            os.begin(), os.end(), os.begin(),
            [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });

        if (os.empty() ||
            os.find("windows") != std::string::npos) {
            windows_depots.push_back(depot);
        }
    }

    if (windows_depots.empty()) {
        fail("Steam returned depot manifests, but none target Windows.");
        return;
    }

    {
        std::ostringstream status;
        status << "Resolved " << windows_depots.size()
               << " Windows/common depot"
               << (windows_depots.size() == 1 ? "" : "s")
               << ". Requesting depot keys...";
        set_state(InstallState::ResolvingApp, status.str());
    }

    std::size_t keyed_depots = 0;
    SteamCmDepotInfo first_keyed;
    for (const SteamCmDepotInfo& depot : windows_depots) {
        if (cancel_.load()) {
            set_state(InstallState::Idle, "Install cancelled.");
            return;
        }

        std::vector<unsigned char> depot_key;
        std::string key_status;
        if (!cm.get_depot_decryption_key(
                app_id,
                depot.depot_id,
                &depot_key,
                &cancel_,
                &key_status)) {
            continue;
        }

        if (keyed_depots == 0) {
            first_keyed = depot;
        }
        ++keyed_depots;
    }

    if (keyed_depots == 0) {
        fail(
            "Steam resolved Windows depots, but did not grant a "
            "decryption key for any of them.");
        return;
    }

    std::uint64_t manifest_request_code = 0;
    std::string manifest_code_status;
    if (!cm.get_manifest_request_code(
            app_id,
            first_keyed.depot_id,
            first_keyed.manifest_id,
            &manifest_request_code,
            &cancel_,
            &manifest_code_status)) {
        if (cancel_.load()) {
            set_state(InstallState::Idle, "Install cancelled.");
        } else {
            fail(manifest_code_status.empty()
                     ? "Steam manifest request code could not be obtained."
                     : manifest_code_status);
        }
        return;
    }

    set_state(
        InstallState::DownloadingManifest,
        "Downloading Steam depot manifest...");

    const ContentServer* selected_server = nullptr;
    for (const ContentServer& server : servers) {
        if (server.https) {
            selected_server = &server;
            break;
        }
    }
    if (!selected_server && !servers.empty()) {
        selected_server = &servers.front();
    }
    if (!selected_server) {
        fail("No Steam CDN server remained available for the manifest.");
        return;
    }

    std::ostringstream manifest_url;
    manifest_url
        << (selected_server->https ? "https://" : "http://")
        << selected_server->host;
    if ((selected_server->https && selected_server->port != 443) ||
        (!selected_server->https && selected_server->port != 80)) {
        manifest_url << ":" << selected_server->port;
    }
    manifest_url
        << "/depot/" << first_keyed.depot_id
        << "/manifest/" << first_keyed.manifest_id
        << "/5/" << manifest_request_code;

    const std::string manifest_path =
        app_dir + "/depot_" +
        std::to_string(first_keyed.depot_id) + "_" +
        std::to_string(first_keyed.manifest_id) +
        ".manifest.zip";

    std::ofstream manifest_file(
        manifest_path,
        std::ios::binary | std::ios::trunc);
    if (!manifest_file) {
        fail("Could not create the local Steam depot manifest file.");
        return;
    }

    CURL* manifest_curl = curl_easy_init();
    if (!manifest_curl) {
        manifest_file.close();
        fail("Could not initialize the Steam CDN manifest request.");
        return;
    }

    FileWriteContext manifest_write;
    manifest_write.out = &manifest_file;
    manifest_write.limit = MANIFEST_RESPONSE_LIMIT;

    struct curl_slist* manifest_headers = nullptr;
    if (!selected_server->vhost.empty() &&
        selected_server->vhost != selected_server->host) {
        const std::string host_header =
            "Host: " + selected_server->vhost;
        manifest_headers =
            curl_slist_append(
                manifest_headers,
                host_header.c_str());
    }

    const bool manifest_configured =
        curl_easy_setopt(
            manifest_curl,
            CURLOPT_URL,
            manifest_url.str().c_str()) == CURLE_OK &&
        curl_easy_setopt(
            manifest_curl,
            CURLOPT_USERAGENT,
            "SteamVita/0.13") == CURLE_OK &&
        curl_easy_setopt(
            manifest_curl,
            CURLOPT_CAINFO,
            CA_PATH) == CURLE_OK &&
        curl_easy_setopt(
            manifest_curl,
            CURLOPT_SSL_VERIFYPEER,
            1L) == CURLE_OK &&
        curl_easy_setopt(
            manifest_curl,
            CURLOPT_SSL_VERIFYHOST,
            2L) == CURLE_OK &&
        curl_easy_setopt(
            manifest_curl,
            CURLOPT_FOLLOWLOCATION,
            1L) == CURLE_OK &&
        curl_easy_setopt(
            manifest_curl,
            CURLOPT_CONNECTTIMEOUT,
            15L) == CURLE_OK &&
        curl_easy_setopt(
            manifest_curl,
            CURLOPT_TIMEOUT,
            90L) == CURLE_OK &&
        curl_easy_setopt(
            manifest_curl,
            CURLOPT_ACCEPT_ENCODING,
            "identity") == CURLE_OK &&
        curl_easy_setopt(
            manifest_curl,
            CURLOPT_WRITEFUNCTION,
            write_file_limited) == CURLE_OK &&
        curl_easy_setopt(
            manifest_curl,
            CURLOPT_WRITEDATA,
            &manifest_write) == CURLE_OK &&
        curl_easy_setopt(
            manifest_curl,
            CURLOPT_NOPROGRESS,
            0L) == CURLE_OK &&
        curl_easy_setopt(
            manifest_curl,
            CURLOPT_XFERINFOFUNCTION,
            cancel_progress) == CURLE_OK &&
        curl_easy_setopt(
            manifest_curl,
            CURLOPT_XFERINFODATA,
            &cancel_) == CURLE_OK &&
        (!manifest_headers ||
         curl_easy_setopt(
             manifest_curl,
             CURLOPT_HTTPHEADER,
             manifest_headers) == CURLE_OK);

    CURLcode manifest_result = CURLE_FAILED_INIT;
    long manifest_http = 0;
    if (manifest_configured) {
        manifest_result = curl_easy_perform(manifest_curl);
        curl_easy_getinfo(
            manifest_curl,
            CURLINFO_RESPONSE_CODE,
            &manifest_http);
    }

    curl_easy_cleanup(manifest_curl);
    if (manifest_headers) {
        curl_slist_free_all(manifest_headers);
    }
    manifest_file.close();

    if (cancel_.load()) {
        std::remove(manifest_path.c_str());
        set_state(InstallState::Idle, "Install cancelled.");
        return;
    }

    if (!manifest_configured ||
        manifest_result != CURLE_OK ||
        manifest_http != 200 ||
        manifest_write.overflow ||
        manifest_write.bytes == 0) {
        std::remove(manifest_path.c_str());
        std::ostringstream message;
        message << "Steam CDN manifest download failed (HTTP "
                << manifest_http << ", curl "
                << static_cast<int>(manifest_result) << ").";
        fail(message.str());
        return;
    }

    if (!file_starts_with_zip_signature(manifest_path)) {
        std::remove(manifest_path.c_str());
        fail("Steam CDN returned a manifest payload that was not a ZIP.");
        return;
    }

    const std::string raw_manifest_path =
        app_dir + "/depot_" +
        std::to_string(first_keyed.depot_id) + "_" +
        std::to_string(first_keyed.manifest_id) +
        ".manifest";

    std::string parse_error;
    if (!extract_single_manifest_zip(
            manifest_path,
            raw_manifest_path,
            &parse_error)) {
        fail(parse_error);
        return;
    }

    DepotManifestStats manifest_stats;
    if (!parse_depot_manifest_binary(
            raw_manifest_path,
            &manifest_stats,
            &parse_error)) {
        fail(parse_error);
        return;
    }

    if (manifest_stats.depot_id != 0 &&
        manifest_stats.depot_id != first_keyed.depot_id) {
        fail("Steam manifest depot ID did not match the requested depot.");
        return;
    }
    if (manifest_stats.manifest_id != 0 &&
        manifest_stats.manifest_id != first_keyed.manifest_id) {
        fail("Steam manifest GID did not match the requested manifest.");
        return;
    }

    set_progress(
        0,
        manifest_stats.total_compressed != 0
            ? manifest_stats.total_compressed
            : manifest_stats.chunk_compressed,
        0);

    {
        std::ostringstream status;
        status << "Parsed depot " << first_keyed.depot_id
               << ": " << manifest_stats.files << " files, "
               << manifest_stats.chunks << " chunks, "
               << manifest_stats.total_compressed
               << " compressed bytes, "
               << manifest_stats.total_uncompressed
               << " installed bytes. Chunk downloads are next.";
        fail(status.str());
    }
}
