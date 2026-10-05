#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct sd_context;
struct sd_auth_operation;
struct sd_auth_event_v1;
struct sd_session;

struct SteamGame {
    std::uint32_t app_id = 0;
    std::string name;
    std::uint32_t playtime_minutes = 0;
    std::string icon_hash;
};

enum class SteamState {
    SignedOut,
    Connecting,
    WaitingForQr,
    Authorizing,
    LoadingLibrary,
    Ready,
    Error,
};

class SteamClient {
public:
    SteamClient();
    ~SteamClient();

    SteamClient(const SteamClient&) = delete;
    SteamClient& operator=(const SteamClient&) = delete;

    bool initialize(std::string* error_message);
    bool start_qr_login();
    void update();
    void refresh_library();
    void sign_out();

    SteamState state() const;
    std::string status() const;
    std::string qr_url() const;
    std::string account_name() const;
    std::uint64_t steam_id() const;
    std::vector<SteamGame> games_snapshot() const;

private:
    static void auth_event_bridge(void* context, const sd_auth_event_v1* event);
    void handle_auth_event(const sd_auth_event_v1* event);
    bool finish_authentication();
    bool load_or_create_device_id(std::string* value, std::string* error_message);
    void begin_library_fetch();
    void fetch_library_worker(std::string access_token, std::uint64_t steam_id);
    void set_error(const std::string& message);
    void shutdown_network();

    mutable std::mutex mutex_;
    SteamState state_ = SteamState::SignedOut;
    std::string status_ = "Sign in to Steam to view your library.";
    std::string qr_url_;
    std::string account_name_;
    std::string access_token_;
    std::uint64_t steam_id_ = 0;
    std::vector<SteamGame> games_;

    sd_context* context_ = nullptr;
    sd_auth_operation* auth_operation_ = nullptr;
    sd_session* session_ = nullptr;

    std::thread library_thread_;

    void* net_memory_ = nullptr;
    bool net_module_loaded_ = false;
    bool net_initialized_ = false;
    bool netctl_initialized_ = false;
    bool curl_initialized_ = false;
};
