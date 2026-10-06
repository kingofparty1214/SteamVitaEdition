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
    bool receive_frame(std::vector<unsigned char>* payload,
                       std::atomic<bool>* cancelled,
                       std::string* error_message);
    bool receive_exact(void* data,
                       std::size_t size,
                       std::atomic<bool>* cancelled);

    int socket_ = -1;
    SteamCmEndpoint endpoint_;
    std::array<unsigned char, 32> session_key_{};
    std::array<unsigned char, 16> hmac_secret_{};
};
