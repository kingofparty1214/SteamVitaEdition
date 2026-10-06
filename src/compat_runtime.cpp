#include "compat_runtime.h"
#include "x86_runtime.h"

#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <sys/stat.h>
#include <vector>

namespace {

constexpr const char* GAME_ROOT = "ux0:data/SteamVita/games";
constexpr int MAX_SCAN_DEPTH = 5;
constexpr std::size_t MAX_EXECUTABLES = 512;

bool is_directory(const std::string& path) {
    struct stat info {};
    if (stat(path.c_str(), &info) != 0) return false;
    return S_ISDIR(info.st_mode);
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

bool ends_with_exe(const std::string& name) {
    const std::string lower = lowercase_ascii(name);
    return lower.size() >= 4 &&
           lower.compare(lower.size() - 4, 4, ".exe") == 0;
}

bool should_skip_directory(const std::string& name) {
    const std::string lower = lowercase_ascii(name);
    return lower == "." || lower == ".." ||
           lower == "redist" || lower == "_redist" ||
           lower == "redistributables" ||
           lower == "directx" || lower == "vcredist";
}

void collect_executables(const std::string& directory,
                         int depth,
                         std::vector<std::string>* output) {
    if (!output || depth > MAX_SCAN_DEPTH ||
        output->size() >= MAX_EXECUTABLES) {
        return;
    }

    DIR* dir = opendir(directory.c_str());
    if (!dir) return;

    while (dirent* entry = readdir(dir)) {
        if (output->size() >= MAX_EXECUTABLES) break;

        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;

        const std::string path = directory + "/" + name;
        struct stat info {};
        if (stat(path.c_str(), &info) != 0) continue;

        if (S_ISDIR(info.st_mode)) {
            if (!should_skip_directory(name)) {
                collect_executables(path, depth + 1, output);
            }
        } else if (S_ISREG(info.st_mode) && ends_with_exe(name)) {
            output->push_back(path);
        }
    }

    closedir(dir);
}

int executable_score(const std::string& path, const std::string& game_name) {
    const std::string lower = lowercase_ascii(path);
    int score = 0;

    auto penalize = [&](const char* token, int value) {
        if (lower.find(token) != std::string::npos) score -= value;
    };

    penalize("unins", 120);
    penalize("uninstall", 120);
    penalize("setup", 100);
    penalize("crash", 80);
    penalize("report", 60);
    penalize("launcher", 35);
    penalize("server", 25);
    penalize("editor", 25);
    penalize("helper", 20);

    std::string compact_name;
    for (unsigned char c : lowercase_ascii(game_name)) {
        if (std::isalnum(c)) compact_name.push_back(static_cast<char>(c));
    }

    std::string compact_path;
    for (unsigned char c : lower) {
        if (std::isalnum(c)) compact_path.push_back(static_cast<char>(c));
    }

    if (!compact_name.empty() &&
        compact_path.find(compact_name) != std::string::npos) {
        score += 100;
    }

    const std::size_t slash_count =
        static_cast<std::size_t>(std::count(path.begin(), path.end(), '/'));
    score -= static_cast<int>(slash_count) * 2;
    return score;
}

} // namespace

bool is_compat_game_installed(std::uint32_t app_id) {
    if (app_id == 0) return false;
    const std::string install_dir =
        std::string(GAME_ROOT) + "/" + std::to_string(app_id);
    return is_directory(install_dir);
}

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

    std::vector<std::string> executables;
    collect_executables(report.install_dir, 0, &executables);

    if (executables.empty()) {
        report.state = CompatState::MissingExecutable;
        report.detail = "No Windows executable was found in the installed game.";
        return report;
    }

    struct Candidate {
        std::string path;
        PeImageInfo image;
        int score = 0;
    };

    std::vector<Candidate> candidates;
    candidates.reserve(executables.size());

    for (const auto& path : executables) {
        const PeImageInfo image = probe_pe_image(path);
        if (!image.valid) continue;
        candidates.push_back({path, image, executable_score(path, game_name)});
    }

    if (candidates.empty()) {
        report.state = CompatState::UnsupportedBinary;
        report.detail = "EXE files were found, but none contained a valid PE image.";
        return report;
    }

    std::stable_sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) {
            const bool a32 = a.image.architecture == GuestArchitecture::X86_32;
            const bool b32 = b.image.architecture == GuestArchitecture::X86_32;
            if (a32 != b32) return a32;
            return a.score > b.score;
        });

    const Candidate& best = candidates.front();
    report.executable_path = best.path;
    report.pe32_x86 = best.image.architecture == GuestArchitecture::X86_32;
    report.pe64_x86 = best.image.architecture == GuestArchitecture::X86_64;

    if (report.pe32_x86) {
        report.state = CompatState::ReadyForTranslator;
        report.detail =
            "Generic PE32 x86 candidate selected automatically. "
            "Ready for the x86 decoder/ARMv7 backend.";
    } else if (report.pe64_x86) {
        report.state = CompatState::UnsupportedBinary;
        report.detail =
            "Only an x86-64 candidate was selected. SteamVita's first runtime target is 32-bit x86.";
    } else {
        report.state = CompatState::UnsupportedBinary;
        report.detail = best.image.detail;
    }

    return report;
}

std::string compat_state_label(CompatState state) {
    switch (state) {
        case CompatState::NotInstalled: return "Not installed";
        case CompatState::MissingExecutable: return "No executable found";
        case CompatState::UnsupportedBinary: return "Unsupported binary";
        case CompatState::ReadyForTranslator: return "PE32 x86 ready";
    }
    return "Unknown";
}
