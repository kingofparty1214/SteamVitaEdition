#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

enum class RuntimePackState {
    Disabled,
    Checking,
    Missing,
    UpToDate,
    Available,
    Downloading,
    Installing,
    Ready,
    Error,
};

class RuntimePackManager {
public:
    RuntimePackManager();
    ~RuntimePackManager();

    RuntimePackManager(const RuntimePackManager&) = delete;
    RuntimePackManager& operator=(const RuntimePackManager&) = delete;

    void initialize(bool network_ready);
    bool force_check(bool network_ready);
    bool start_install();

    RuntimePackState state() const;
    std::string status() const;
    std::string installed_version() const;
    std::string remote_version() const;
    bool runtime_ready() const;
    bool update_available() const;

    static const char* runtime_root();

private:
    void check_worker();
    void install_worker();
    void set_state(RuntimePackState state, const std::string& status);

    mutable std::mutex mutex_;
    RuntimePackState state_ = RuntimePackState::Disabled;
    std::string status_;
    std::string installed_version_;
    std::string remote_version_;
    std::string download_url_;
    std::string expected_sha256_;

    std::atomic<bool> cancel_{false};
    std::thread check_thread_;
    std::thread install_thread_;
};

const char* runtime_pack_state_label(RuntimePackState state);
