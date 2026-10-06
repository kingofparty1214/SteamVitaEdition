#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct SteamSessionCredentials {
    std::string access_token;
    std::string refresh_token;
    std::uint64_t steam_id = 0;
    bool valid() const {
        return !access_token.empty() && !refresh_token.empty() && steam_id != 0;
    }
};

enum class SteamOwnership : std::uint8_t {
    Direct = 0,
    FamilyShared = 1,
};

struct SteamGame {
    std::uint32_t app_id = 0;
    std::string name;
    std::uint32_t playtime_minutes = 0;
    std::string icon_hash;
    SteamOwnership ownership = SteamOwnership::Direct;
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
    bool network_ready() const;
    bool offline_mode() const;
    bool has_session() const;
    SteamSessionCredentials session_credentials_snapshot() const;

private:
    void authentication_worker();
    void begin_library_fetch();
    void fetch_library_worker(std::string access_token, std::uint64_t steam_id);
    bool load_or_create_device_id(std::string* value, std::string* error_message);
    bool load_library_cache();
    bool save_library_cache(const std::vector<SteamGame>& games,
                            const std::string& account_name,
                            std::uint64_t steam_id);
    void set_error(const std::string& message);
    void shutdown_network();

    mutable std::mutex mutex_;
    SteamState state_ = SteamState::SignedOut;
    std::string status_ = "Sign in to Steam to view your library.";
    std::string qr_url_;
    std::string account_name_;
    std::string access_token_;
    std::string refresh_token_;
    std::string device_id_;
    std::string ca_bundle_;
    std::uint64_t steam_id_ = 0;
    std::vector<SteamGame> games_;

    std::atomic<bool> cancel_login_{false};
    std::thread auth_thread_;
    std::thread library_thread_;

    void* net_memory_ = nullptr;
    bool net_module_loaded_ = false;
    bool net_initialized_ = false;
    bool netctl_initialized_ = false;
    bool curl_initialized_ = false;
    bool network_ready_ = false;
    bool offline_mode_ = false;
};
