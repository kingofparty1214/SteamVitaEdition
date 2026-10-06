#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "steam_client.h"

enum class InstallState {
    Idle,
    Preparing,
    DiscoveringContentServers,
    ResolvingApp,
    DownloadingManifest,
    DownloadingFiles,
    Installed,
    Error,
};

struct InstallSnapshot {
    InstallState state = InstallState::Idle;
    std::uint32_t app_id = 0;
    std::string game_name;
    std::string status;
    std::uint64_t downloaded_bytes = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t bytes_per_second = 0;
    std::uint64_t eta_seconds = 0;

    bool active() const {
        return state == InstallState::Preparing ||
               state == InstallState::DiscoveringContentServers ||
               state == InstallState::ResolvingApp ||
               state == InstallState::DownloadingManifest ||
               state == InstallState::DownloadingFiles;
    }
};

class GameInstaller {
public:
    GameInstaller();
    ~GameInstaller();

    GameInstaller(const GameInstaller&) = delete;
    GameInstaller& operator=(const GameInstaller&) = delete;

    bool start_install(std::uint32_t app_id,
                       const std::string& game_name,
                       const SteamSessionCredentials& credentials);
    void cancel();
    void update();

    InstallSnapshot snapshot() const;

private:
    struct ContentServer {
        std::string host;
        std::string vhost;
        int port = 443;
        bool https = true;
        int weighted_load = 0;
    };

    void worker(std::uint32_t app_id,
                std::string game_name,
                SteamSessionCredentials credentials);
    bool discover_content_servers(std::vector<ContentServer>* servers,
                                  std::string* error);
    void set_state(InstallState state, const std::string& status);
    void set_progress(std::uint64_t downloaded_bytes,
                      std::uint64_t total_bytes,
                      std::uint64_t bytes_per_second);
    void fail(const std::string& message);

    mutable std::mutex mutex_;
    InstallSnapshot snapshot_;
    std::atomic<bool> cancel_{false};
    std::thread worker_;
};
