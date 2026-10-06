#include "compat_runtime.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <vector>

namespace {

constexpr const char* GAME_ROOT = "ux0:data/SteamVita/games";

bool path_exists(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0;
}

bool is_directory(const std::string& path) {
    struct stat info {};
    if (stat(path.c_str(), &info) != 0) return false;
    return S_ISDIR(info.st_mode);
}

std::uint16_t read_u16_le(const unsigned char* data) {
    return static_cast<std::uint16_t>(data[0]) |
           (static_cast<std::uint16_t>(data[1]) << 8u);
}

std::uint32_t read_u32_le(const unsigned char* data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8u) |
           (static_cast<std::uint32_t>(data[2]) << 16u) |
           (static_cast<std::uint32_t>(data[3]) << 24u);
}

bool is_pe32_x86(const std::string& path, std::string* detail) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        if (detail) *detail = "Could not open the executable.";
        return false;
    }

    unsigned char dos[64] {};
    if (std::fread(dos, 1, sizeof(dos), file) != sizeof(dos)) {
        std::fclose(file);
        if (detail) *detail = "Executable is too small to be a PE file.";
        return false;
    }

    if (dos[0] != 'M' || dos[1] != 'Z') {
        std::fclose(file);
        if (detail) *detail = "Executable does not have an MZ header.";
        return false;
    }

    const std::uint32_t pe_offset = read_u32_le(dos + 0x3c);
    if (std::fseek(file, static_cast<long>(pe_offset), SEEK_SET) != 0) {
        std::fclose(file);
        if (detail) *detail = "PE header offset is invalid.";
        return false;
    }

    unsigned char pe[24] {};
    if (std::fread(pe, 1, sizeof(pe), file) != sizeof(pe)) {
        std::fclose(file);
        if (detail) *detail = "PE header is incomplete.";
        return false;
    }

    std::fclose(file);

    if (std::memcmp(pe, "PE\0\0", 4) != 0) {
        if (detail) *detail = "Executable does not have a valid PE signature.";
        return false;
    }

    constexpr std::uint16_t IMAGE_FILE_MACHINE_I386 = 0x014c;
    const std::uint16_t machine = read_u16_le(pe + 4);
    if (machine != IMAGE_FILE_MACHINE_I386) {
        if (detail) *detail = "Executable is PE, but not 32-bit x86 (i386).";
        return false;
    }

    if (detail) *detail = "PE32 x86 executable detected. Ready for the translator layer.";
    return true;
}

std::vector<std::string> entry_candidates(std::uint32_t app_id) {
    switch (app_id) {
        case 400:
            return {
                "hl2.exe",
                "Portal/hl2.exe",
            };
        case 21000:
            return {
                "LEGOBatman.exe",
                "LEGO Batman.exe",
                "LEGO Batman/LEGOBatman.exe",
            };
        default:
            return {};
    }
}

} // namespace

CompatReport inspect_compat_game(std::uint32_t app_id, const std::string& game_name) {
    CompatReport report;
    report.app_id = app_id;
    report.game_name = game_name;
    report.install_dir = std::string(GAME_ROOT) + "/" + std::to_string(app_id);

    if (!is_directory(report.install_dir)) {
        report.state = CompatState::NotInstalled;
        report.detail = "Game files are not installed on the Vita yet.";
        return report;
    }

    const auto candidates = entry_candidates(app_id);
    if (candidates.empty()) {
        report.state = CompatState::MissingExecutable;
        report.detail = "No executable profile exists for this game yet.";
        return report;
    }

    for (const auto& relative : candidates) {
        const std::string candidate = report.install_dir + "/" + relative;
        if (!path_exists(candidate)) continue;

        report.executable_path = candidate;
        std::string detail;
        report.pe32_x86 = is_pe32_x86(candidate, &detail);
        report.detail = detail;
        report.state = report.pe32_x86
            ? CompatState::ReadyForTranslator
            : CompatState::UnsupportedBinary;
        return report;
    }

    report.state = CompatState::MissingExecutable;
    report.detail = "Game folder exists, but the expected Windows executable was not found.";
    return report;
}

std::string compat_state_label(CompatState state) {
    switch (state) {
        case CompatState::NotInstalled: return "Not installed";
        case CompatState::MissingExecutable: return "Needs executable profile";
        case CompatState::UnsupportedBinary: return "Unsupported binary";
        case CompatState::ReadyForTranslator: return "PE32 x86 ready";
    }
    return "Unknown";
}
