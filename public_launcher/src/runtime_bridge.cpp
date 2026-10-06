#include "runtime_bridge.h"

RuntimeLaunchResult launch_installed_game(
        std::uint32_t app_id,
        const std::string& install_dir) {
    (void)app_id;
    (void)install_dir;

    // PRIVATE COMPATIBILITY IMPLEMENTATION REMOVED.
    //
    // Internal builds replace this bridge with the experimental runtime.
    // Public source intentionally contains no executable handoff, translator,
    // emulator, Win32 shim, or compatibility backend.
    return {
        RuntimeLaunchState::Blocked,
        "Compatibility runtime is disabled in the public launcher build."
    };
}

bool public_runtime_launching_enabled() {
    return false;
}
