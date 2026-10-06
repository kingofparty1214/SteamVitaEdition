#pragma once

#include <cstdint>
#include <string>

enum class CompatState {
    NotInstalled,
    MissingExecutable,
    UnsupportedBinary,
    ReadyForTranslator,
};

struct CompatReport {
    CompatState state = CompatState::NotInstalled;
    std::uint32_t app_id = 0;
    std::string game_name;
    std::string install_dir;
    std::string executable_path;
    std::string detail;
    bool pe32_x86 = false;
};

CompatReport inspect_compat_game(std::uint32_t app_id, const std::string& game_name);
std::string compat_state_label(CompatState state);
