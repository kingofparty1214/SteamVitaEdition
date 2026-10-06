#pragma once

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
