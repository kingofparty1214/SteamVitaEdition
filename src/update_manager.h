#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

enum class UpdateState {
    Disabled,
    Checking,
    UpToDate,
    Available,
    Downloading,
    ReadyToInstall,
    Error,
};

class UpdateManager {
public:
    UpdateManager();
    ~UpdateManager();

    UpdateManager(const UpdateManager&) = delete;
    UpdateManager& operator=(const UpdateManager&) = delete;

    void initialize(bool network_ready);
    void update();
    void start_update();
    bool launch_installer(std::string* error_message);

    UpdateState state() const;
    std::string status() const;
    std::string remote_version() const;
    bool update_available() const;

private:
    void check_worker();
    void download_worker();
    void set_state(UpdateState state, const std::string& status);

    mutable std::mutex mutex_;
    UpdateState state_ = UpdateState::Disabled;
    std::string status_;
    std::string remote_version_;
    std::string download_url_;
    std::string expected_sha256_;

    std::atomic<bool> cancel_{false};
    std::thread check_thread_;
    std::thread download_thread_;
};
