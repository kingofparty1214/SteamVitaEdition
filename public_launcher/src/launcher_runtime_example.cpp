#include "runtime_bridge.h"

#include <cstdint>
#include <string>

// Example integration point for the public launcher.
//
// The launcher may call this after a game is installed, but the public
// runtime bridge cannot execute the game. Internal compatibility code is
// intentionally replaced by a blocked result.
std::string public_launch_request(
        std::uint32_t app_id,
        const std::string& install_dir) {
    const RuntimeLaunchResult result =
        launch_installed_game(app_id, install_dir);

    if (result.state == RuntimeLaunchState::Blocked) {
        return result.detail;
    }

    return "Compatibility runtime is unavailable.";
}
