#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class CompatState {
    NotInstalled,
    MissingExecutable,
    UnsupportedBinary,
    ReadyForTranslator,
    ReadyForX64Translator,
};

struct CompatReport {
    CompatState state = CompatState::NotInstalled;
    std::uint32_t app_id = 0;
    std::string game_name;
    std::string install_dir;
    std::string executable_path;
    std::string detail;
    bool pe32_x86 = false;
    bool pe64_x86 = false;

    bool unity = false;
    bool unity_mono = false;
    bool steamworks = false;
    bool d3d11_hint = false;
    bool xinput_hint = false;

    std::string engine;
    std::string managed_runtime;
    std::string dependency_summary;
    std::vector<std::string> imported_dlls;
};

bool is_compat_game_installed(std::uint32_t app_id);
bool uninstall_compat_game(std::uint32_t app_id, std::string* error_message);
CompatReport inspect_compat_game(std::uint32_t app_id, const std::string& game_name);
std::string compat_state_label(CompatState state);
