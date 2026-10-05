#include "steam_client.h"

#include "steam_cm_core.h"
#include <steamdepot/discovery.hpp>

#include <curl/curl.h>
#include <jansson.h>
#include <openssl/rand.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace {

constexpr const char* DATA_DIR = "ux0:data/SteamVita";
constexpr const char* DEVICE_ID_PATH = "ux0:data/SteamVita/device_id.txt";
constexpr const char* CA_PATH = "ux0:data/SteamVita/cacert.pem";
constexpr std::size_t NET_MEMORY_SIZE = 4u * 1024u * 1024u;

std::size_t curl_write(void* data, std::size_t size, std::size_t count, void* userdata) {
    if (!userdata) return 0;
    const std::size_t bytes = size * count;
    auto* output = static_cast<std::string*>(userdata);
    output->append(static_cast<const char*>(data), bytes);
    return bytes;
}

bool copy_ca_bundle() {
    std::ifstream input("app0:/cacert.pem", std::ios::binary);
    if (!input) return false;

    std::ofstream output(CA_PATH, std::ios::binary | std::ios::trunc);
    if (!output) return false;

    output << input.rdbuf();
    return output.good();
}

} // namespace

SteamClient::SteamClient() = default;

SteamClient::~SteamClient() {
    sign_out();
    shutdown_network();
}

bool SteamClient::initialize(std::string* error_message) {
    mkdir(DATA_DIR, 0777);

    const int module_result = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    if (module_result < 0) {
        const std::string message = "Could not load the Vita network module.";
        if (error_message) *error_message = message;
        set_error(message);
        return false;
    }
    net_module_loaded_ = true;

    net_memory_ = std::malloc(NET_MEMORY_SIZE);
    if (!net_memory_) {
        const std::string message = "Could not allocate network memory.";
        if (error_message) *error_message = message;
        set_error(message);
        return false;
    }

    SceNetInitParam net_param{};
    net_param.memory = net_memory_;
    net_param.size = NET_MEMORY_SIZE;
    net_param.flags = 0;

    if (sceNetInit(&net_param) < 0) {
        const std::string message = "sceNetInit failed.";
        if (error_message) *error_message = message;
        set_error(message);
        return false;
    }
    net_initialized_ = true;

    if (sceNetCtlInit() < 0) {
        const std::string message = "sceNetCtlInit failed.";
        if (error_message) *error_message = message;
        set_error(message);
        return false;
    }
    netctl_initialized_ = true;

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        const std::string message = "libcurl initialization failed.";
        if (error_message) *error_message = message;
        set_error(message);
        return false;
    }
    curl_initialized_ = true;

    if (!copy_ca_bundle()) {
        const std::string message = "SteamVita could not prepare its TLS certificate bundle.";
        if (error_message) *error_message = message;
        set_error(message);
        return false;
    }
    ca_bundle_ = CA_PATH;

    std::string device_error;
    if (!load_or_create_device_id(&device_id_, &device_error)) {
        if (error_message) *error_message = device_error;
        set_error(device_error);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = SteamState::SignedOut;
        status_ = "Sign in to Steam to load your real game library.";
    }

    if (error_message) error_message->clear();
    return true;
}

bool SteamClient::load_or_create_device_id(std::string* value, std::string* error_message) {
    if (!value) return false;

    {
        std::ifstream input(DEVICE_ID_PATH);
        std::string existing;
        std::getline(input, existing);
        if (existing.size() >= 16) {
            *value = existing;
            return true;
        }
    }

    unsigned char random_bytes[32]{};
    if (RAND_bytes(random_bytes, sizeof(random_bytes)) != 1) {
        if (error_message) *error_message = "Could not generate a Steam device identity.";
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

bool SteamClient::start_qr_login() {
    sign_out();

    if (!net_initialized_ || device_id_.empty() || ca_bundle_.empty()) {
        set_error("Steam networking is not initialized.");
        return false;
    }

    cancel_login_.store(false);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = SteamState::Connecting;
        status_ = "Connecting to Steam...";
        qr_url_.clear();
    }

    auth_thread_ = std::thread(&SteamClient::authentication_worker, this);
    return true;
}

bool SteamClient::cancel_callback(void* context) {
    auto* self = static_cast<SteamClient*>(context);
    return self && self->cancel_login_.load();
}

void SteamClient::authentication_worker() {
    try {
        const auto endpoints =
            steamdepot::discover_cm_endpoints(0, ca_bundle_, 8, "SteamVita/0.3");

        std::string last_error = "Steam returned no usable connection servers.";

        for (const std::string& endpoint : endpoints) {
            if (cancel_login_.load()) return;

            try {
                steamdepot::LoginRequest request;
                request.cm_server = endpoint;
                request.ca_bundle = ca_bundle_;
                request.confirmation_method = "qr";
                request.device_id = device_id_;
                request.client_label = "SteamVita";
                request.control.is_cancelled = &SteamClient::cancel_callback;
                request.control.context = this;

                request.qr_challenge_url_changed = [this](const std::string& url) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    qr_url_ = url;
                    state_ = SteamState::WaitingForQr;
                    status_ = "Scan the QR code with the Steam mobile app.";
                };

                request.qr_remote_interaction = [this]() {
                    std::lock_guard<std::mutex> lock(mutex_);
                    state_ = SteamState::Authorizing;
                    status_ = "Steam saw the QR scan. Approve the sign-in.";
                };

                steamdepot::LoginResult result = steamdepot::login_account(request);
                if (cancel_login_.load()) return;

                if (result.eresult != 1) {
                    last_error = result.message.empty()
                        ? "Steam rejected the sign-in."
                        : result.message;
                    continue;
                }

                if (result.steam_id == 0 || result.access_token.empty()) {
                    last_error = "Steam signed in but did not provide the library token.";
                    continue;
                }

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    steam_id_ = result.steam_id;
                    account_name_ = result.account_name;
                    access_token_ = result.access_token;
                    qr_url_.clear();
                    state_ = SteamState::LoadingLibrary;
                    status_ = "Signed in. Loading your real Steam library...";
                }

                begin_library_fetch();
                return;
            } catch (const std::exception& exception) {
                last_error = exception.what();
            }
        }

        if (!cancel_login_.load()) set_error(last_error);
    } catch (const std::exception& exception) {
        if (!cancel_login_.load()) set_error(exception.what());
    }
}

void SteamClient::update() {
    // Steam auth and library requests run on worker threads.
    // The render loop only consumes their synchronized state.
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
    CURL* curl = curl_easy_init();
    if (!curl) {
        set_error("Could not create a Steam web request.");
        return;
    }

    char* escaped_token =
        curl_easy_escape(curl, access_token.c_str(), static_cast<int>(access_token.size()));
    if (!escaped_token) {
        curl_easy_cleanup(curl);
        set_error("Could not encode the Steam access token.");
        return;
    }

    std::ostringstream url;
    url << "https://api.steampowered.com/IPlayerService/GetOwnedGames/v1/"
        << "?access_token=" << escaped_token
        << "&steamid=" << steam_id
        << "&include_appinfo=1"
        << "&include_played_free_games=1"
        << "&format=json";

    curl_free(escaped_token);

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.str().c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "SteamVita/0.3");
    curl_easy_setopt(curl, CURLOPT_CAINFO, CA_PATH);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 45L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    const CURLcode request_result = curl_easy_perform(curl);

    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_easy_cleanup(curl);

    if (request_result != CURLE_OK) {
        set_error(std::string("Steam library request failed: ") +
                  curl_easy_strerror(request_result));
        return;
    }

    if (http_status != 200) {
        std::ostringstream message;
        message << "Steam library request returned HTTP " << http_status << ".";
        set_error(message.str());
        return;
    }

    json_error_t json_error{};
    json_t* root = json_loadb(response.data(), response.size(), 0, &json_error);
    if (!root) {
        std::ostringstream message;
        message << "Steam returned invalid library data near line "
                << json_error.line << ".";
        set_error(message.str());
        return;
    }

    std::vector<SteamGame> loaded;

    json_t* response_object = json_object_get(root, "response");
    json_t* game_array =
        response_object ? json_object_get(response_object, "games") : nullptr;

    if (game_array && json_is_array(game_array)) {
        const std::size_t count = json_array_size(game_array);
        loaded.reserve(count);

        for (std::size_t index = 0; index < count; ++index) {
            json_t* game_object = json_array_get(game_array, index);
            if (!json_is_object(game_object)) continue;

            json_t* appid_value = json_object_get(game_object, "appid");
            json_t* name_value = json_object_get(game_object, "name");
            if (!json_is_integer(appid_value) || !json_is_string(name_value)) continue;

            SteamGame game;
            game.app_id = static_cast<std::uint32_t>(json_integer_value(appid_value));
            game.name = json_string_value(name_value);

            json_t* playtime_value = json_object_get(game_object, "playtime_forever");
            if (json_is_integer(playtime_value)) {
                game.playtime_minutes =
                    static_cast<std::uint32_t>(json_integer_value(playtime_value));
            }

            json_t* icon_value = json_object_get(game_object, "img_icon_url");
            if (json_is_string(icon_value)) {
                game.icon_hash = json_string_value(icon_value);
            }

            if (game.app_id != 0 && !game.name.empty()) {
                loaded.push_back(std::move(game));
            }
        }
    }

    json_decref(root);

    std::sort(loaded.begin(), loaded.end(),
              [](const SteamGame& a, const SteamGame& b) {
                  return a.name < b.name;
              });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        games_ = std::move(loaded);
        state_ = SteamState::Ready;

        std::ostringstream message;
        message << "Loaded " << games_.size() << " games from your Steam account.";
        status_ = message.str();
    }
}

void SteamClient::refresh_library() {
    SteamState current_state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_state = state_;
    }

    if (current_state == SteamState::Ready) begin_library_fetch();
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

void SteamClient::set_error(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = SteamState::Error;
    status_ = message.empty() ? "SteamVita encountered an error." : message;
}

void SteamClient::shutdown_network() {
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
