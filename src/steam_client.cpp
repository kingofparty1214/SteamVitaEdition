#include "steam_client.h"

#include <steamdepot/steamdepot.h>

#include <curl/curl.h>
#include <jansson.h>
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

extern "C" sd_status sd_session_copy_access_token_steamvita(
    const sd_session* session,
    sd_secret** output,
    sd_error_v1* error);

namespace {

constexpr const char* DATA_DIR = "ux0:data/SteamVita";
constexpr const char* DEVICE_ID_PATH = "ux0:data/SteamVita/device_id.txt";
constexpr const char* CA_PATH = "ux0:data/SteamVita/cacert.pem";
constexpr std::size_t NET_MEMORY_SIZE = 4u * 1024u * 1024u;

sd_string_view view_of(const std::string& value) {
    sd_string_view view{};
    view.data = value.data();
    view.size = value.size();
    return view;
}

std::string from_view(sd_string_view value) {
    if (!value.data || value.size == 0) return {};
    return std::string(value.data, value.size);
}

std::string steam_error(const sd_error_v1& error, const char* fallback) {
    if (error.message[0]) return error.message;
    return fallback ? fallback : "Steam error";
}

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

std::string account_name_from_session(const sd_session* session) {
    sd_error_v1 error{};
    sd_error_init_v1(&error);

    std::size_t size = 0;
    if (sd_session_copy_account_name(session, nullptr, &size, &error) != SD_OK || size == 0)
        return {};

    std::vector<char> buffer(size);
    if (sd_session_copy_account_name(session, buffer.data(), &size, &error) != SD_OK)
        return {};

    return std::string(buffer.data());
}

std::string access_token_from_session(const sd_session* session) {
    sd_error_v1 error{};
    sd_error_init_v1(&error);

    sd_secret* secret = nullptr;
    if (sd_session_copy_access_token_steamvita(session, &secret, &error) != SD_OK || !secret)
        return {};

    const std::size_t token_size = sd_secret_size(secret);
    std::string token(token_size, '\0');
    std::size_t written = token.size();

    const sd_status status = token_size == 0
        ? SD_NOT_READY
        : sd_secret_copy(secret, token.data(), &written, &error);

    sd_secret_destroy(secret);

    if (status != SD_OK) return {};
    token.resize(written);
    return token;
}

} // namespace

SteamClient::SteamClient() = default;

SteamClient::~SteamClient() {
    sign_out();

    if (context_) {
        sd_context_destroy(context_);
        context_ = nullptr;
    }

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

    std::string device_id;
    std::string device_error;
    if (!load_or_create_device_id(&device_id, &device_error)) {
        if (error_message) *error_message = device_error;
        set_error(device_error);
        return false;
    }

    const std::string ca_bundle = CA_PATH;
    const std::string user_agent = "SteamVita/0.3";
    const std::string client_label = "SteamVita";

    sd_context_config_v1 config{};
    config.struct_size = sizeof(config);
    config.abi_version = SD_ABI_VERSION_1;
    config.ca_bundle_path = view_of(ca_bundle);
    config.device_id = view_of(device_id);
    config.user_agent = view_of(user_agent);
    config.client_label = view_of(client_label);

    sd_error_v1 error{};
    sd_error_init_v1(&error);
    if (sd_context_create_v1(&config, &context_, &error) != SD_OK) {
        const std::string message = steam_error(error, "Could not initialize Steam.");
        if (error_message) *error_message = message;
        set_error(message);
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

    sd_error_v1 error{};
    sd_error_init_v1(&error);

    std::size_t size = 0;
    (void)sd_generate_device_id(nullptr, &size, &error);
    if (size == 0) {
        if (error_message) *error_message = steam_error(error, "Could not create Steam device ID.");
        return false;
    }

    std::vector<char> buffer(size);
    if (sd_generate_device_id(buffer.data(), &size, &error) != SD_OK) {
        if (error_message) *error_message = steam_error(error, "Could not create Steam device ID.");
        return false;
    }

    std::string generated(buffer.data());
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
    if (!context_) {
        set_error("Steam networking is not initialized.");
        return false;
    }

    sign_out();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = SteamState::Connecting;
        status_ = "Connecting to Steam...";
        qr_url_.clear();
    }

    sd_auth_callbacks_v1 callbacks{};
    callbacks.struct_size = sizeof(callbacks);
    callbacks.abi_version = SD_ABI_VERSION_1;
    callbacks.on_event = &SteamClient::auth_event_bridge;
    callbacks.context = this;

    sd_error_v1 error{};
    sd_error_init_v1(&error);

    const sd_status result =
        sd_auth_start_qr_v1(context_, &callbacks, &auth_operation_, &error);

    if (result != SD_OK) {
        auth_operation_ = nullptr;
        set_error(steam_error(error, "Steam QR login could not start."));
        return false;
    }

    return true;
}

void SteamClient::auth_event_bridge(void* context, const sd_auth_event_v1* event) {
    if (context && event) {
        static_cast<SteamClient*>(context)->handle_auth_event(event);
    }
}

void SteamClient::handle_auth_event(const sd_auth_event_v1* event) {
    const std::string message = from_view(event->message);
    const std::string challenge = from_view(event->challenge_url);

    std::lock_guard<std::mutex> lock(mutex_);

    switch (event->type) {
        case SD_AUTH_EVENT_CONNECTING:
            state_ = SteamState::Connecting;
            status_ = message.empty() ? "Connecting to Steam..." : message;
            break;

        case SD_AUTH_EVENT_QR_CHALLENGE:
            qr_url_ = challenge;
            state_ = SteamState::WaitingForQr;
            status_ = "Scan the QR code with the Steam mobile app.";
            break;

        case SD_AUTH_EVENT_WAITING_FOR_MOBILE_APPROVAL:
            state_ = SteamState::Authorizing;
            status_ = message.empty() ? "Approve the sign-in in Steam." : message;
            break;

        case SD_AUTH_EVENT_AUTHENTICATED:
            state_ = SteamState::Authorizing;
            status_ = "Steam approved the sign-in. Loading your account...";
            break;

        case SD_AUTH_EVENT_FAILED:
            state_ = SteamState::Error;
            status_ = message.empty() ? "Steam sign-in failed." : message;
            break;

        case SD_AUTH_EVENT_CANCELLED:
            state_ = SteamState::SignedOut;
            status_ = "Steam sign-in cancelled.";
            break;

        default:
            break;
    }
}

void SteamClient::update() {
    if (!auth_operation_) return;

    sd_error_v1 error{};
    sd_error_init_v1(&error);
    const sd_status result = sd_auth_wait(auth_operation_, 0, &error);

    if (result == SD_TIMED_OUT) return;

    if (result != SD_OK) {
        const std::string message = steam_error(error, "Steam sign-in failed.");
        sd_auth_operation_destroy(auth_operation_);
        auth_operation_ = nullptr;
        set_error(message);
        return;
    }

    if (!finish_authentication()) {
        if (auth_operation_) {
            sd_auth_operation_destroy(auth_operation_);
            auth_operation_ = nullptr;
        }
    }
}

bool SteamClient::finish_authentication() {
    sd_error_v1 error{};
    sd_error_init_v1(&error);

    sd_session* new_session = nullptr;
    if (sd_auth_take_session(auth_operation_, &new_session, &error) != SD_OK || !new_session) {
        set_error(steam_error(error, "Steam session was not available."));
        return false;
    }

    sd_auth_operation_destroy(auth_operation_);
    auth_operation_ = nullptr;

    if (session_) sd_session_destroy(session_);
    session_ = new_session;

    const std::uint64_t new_steam_id = sd_session_steam_id(session_);
    const std::string new_account_name = account_name_from_session(session_);
    const std::string new_access_token = access_token_from_session(session_);

    if (new_steam_id == 0 || new_access_token.empty()) {
        set_error("Steam signed in, but SteamVita could not read the account token.");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        steam_id_ = new_steam_id;
        account_name_ = new_account_name;
        access_token_ = new_access_token;
        qr_url_.clear();
        status_ = "Signed in. Loading your Steam library...";
        state_ = SteamState::LoadingLibrary;
    }

    begin_library_fetch();
    return true;
}

void SteamClient::begin_library_fetch() {
    if (library_thread_.joinable()) library_thread_.join();

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

            json_t* playtime_value =
                json_object_get(game_object, "playtime_forever");
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

    if (current_state == SteamState::Ready) {
        begin_library_fetch();
    }
}

void SteamClient::sign_out() {
    if (auth_operation_) {
        sd_auth_cancel(auth_operation_);
        sd_auth_operation_destroy(auth_operation_);
        auth_operation_ = nullptr;
    }

    if (library_thread_.joinable()) library_thread_.join();

    if (session_) {
        sd_session_destroy(session_);
        session_ = nullptr;
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
