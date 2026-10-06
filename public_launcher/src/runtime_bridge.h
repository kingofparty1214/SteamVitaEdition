#pragma once

#include <cstdint>
#include <string>

enum class RuntimeLaunchState {
    Blocked,
    Unavailable,
};

struct RuntimeLaunchResult {
    RuntimeLaunchState state = RuntimeLaunchState::Blocked;
    std::string detail;
};

// Public builds must never start the private compatibility/runtime layer.
RuntimeLaunchResult launch_installed_game(
    std::uint32_t app_id,
    const std::string& install_dir);

bool public_runtime_launching_enabled();
