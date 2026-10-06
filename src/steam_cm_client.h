#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct SteamCmEndpoint {
    std::string host;
    std::uint16_t port = 0;
};

struct SteamCmLicense {
    std::uint32_t package_id = 0;
    std::uint32_t owner_id = 0;
    std::uint32_t license_type = 0;
    std::uint64_t access_token = 0;
    std::uint32_t master_package_id = 0;
};

struct SteamCmSharedApp {
    std::uint32_t app_id = 0;
    std::uint32_t package_id = 0;
    std::uint32_t owner_id = 0;
    std::string name;
};

bool discover_steam_cm_servers(std::vector<SteamCmEndpoint>* servers,
                               std::atomic<bool>* cancelled,
                               std::string* error_message);

class SteamCmConnection {
public:
    SteamCmConnection();
    ~SteamCmConnection();

    SteamCmConnection(const SteamCmConnection&) = delete;
    SteamCmConnection& operator=(const SteamCmConnection&) = delete;

    bool connect_secure(const std::vector<SteamCmEndpoint>& servers,
                        std::atomic<bool>* cancelled,
                        std::string* error_message);
    bool logon_and_fetch_licenses(const std::string& access_token,
                                  const std::string& account_name,
                                  std::uint64_t steam_id,
                                  std::vector<SteamCmLicense>* licenses,
                                  std::atomic<bool>* cancelled,
                                  std::string* error_message);
    bool fetch_shared_package_apps(
        const std::vector<SteamCmLicense>& licenses,
        std::uint64_t steam_id,
        std::vector<SteamCmSharedApp>* apps,
        std::atomic<bool>* cancelled,
        std::string* error_message);
    bool fetch_shared_app_names(
        std::vector<SteamCmSharedApp>* apps,
        std::atomic<bool>* cancelled,
        std::string* error_message);
    void close();

    bool connected() const;
    const SteamCmEndpoint& endpoint() const;
    const std::array<unsigned char, 32>& session_key() const;
    const std::array<unsigned char, 16>& hmac_secret() const;

private:
    bool connect_one(const SteamCmEndpoint& endpoint,
                     std::atomic<bool>* cancelled,
                     std::string* error_message);
    bool secure_channel(std::atomic<bool>* cancelled,
                        std::string* error_message);
    bool send_frame(const std::vector<unsigned char>& payload);
    bool send_encrypted(const std::vector<unsigned char>& payload,
                        std::string* error_message);
    bool receive_encrypted(std::vector<unsigned char>* payload,
                           std::atomic<bool>* cancelled,
                           std::string* error_message);
    bool receive_frame(std::vector<unsigned char>* payload,
                       std::atomic<bool>* cancelled,
                       std::string* error_message);
    bool receive_exact(void* data,
                       std::size_t size,
                       std::atomic<bool>* cancelled);

    int socket_ = -1;
    void* websocket_ = nullptr;
    SteamCmEndpoint endpoint_;
    std::array<unsigned char, 32> session_key_{};
    std::array<unsigned char, 16> hmac_secret_{};
    bool hmac_mode_ = false;
    std::uint64_t steam_id_ = 0;
    std::int32_t session_id_ = 0;
};
