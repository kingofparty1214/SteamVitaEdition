#include "compat_runtime.h"
#include "x86_runtime.h"

#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <cstdio>
#include <functional>
#include <sys/stat.h>
#include <unistd.h>
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

bool file_exists(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool directory_exists(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool tree_contains_name(const std::string& directory,
                        const std::string& target_lower,
                        int depth = 0) {
    if (depth > MAX_SCAN_DEPTH) return false;

    DIR* dir = opendir(directory.c_str());
    if (!dir) return false;

    bool found = false;
    while (!found) {
        dirent* entry = readdir(dir);
        if (!entry) break;

        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;

        const std::string path = directory + "/" + name;
        struct stat info {};
        if (stat(path.c_str(), &info) != 0) continue;

        const std::string lower = lowercase_ascii(name);
        if (lower == target_lower) {
            found = true;
            break;
        }

        if (S_ISDIR(info.st_mode) &&
            !should_skip_directory(name) &&
            tree_contains_name(path, target_lower, depth + 1)) {
            found = true;
            break;
        }
    }

    closedir(dir);
    return found;
}

std::string join_dependency_summary(const CompatReport& report) {
    std::vector<std::string> items;
    if (report.unity) items.push_back("Unity");
    if (report.unity_mono) items.push_back("Mono");
    if (report.steamworks) items.push_back("Steamworks");
    if (report.d3d11_hint) items.push_back("D3D11");
    if (report.xinput_hint) items.push_back("XInput");

    if (items.empty()) return "No common runtime markers detected.";

    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) out += ", ";
        out += items[i];
    }
    return out;
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

bool uninstall_compat_game(std::uint32_t app_id, std::string* error_message) {
    if (app_id == 0) {
        if (error_message) *error_message = "Invalid Steam AppID.";
        return false;
    }

    const std::string install_dir =
        std::string(GAME_ROOT) + "/" + std::to_string(app_id);

    struct stat info {};
    if (stat(install_dir.c_str(), &info) != 0) {
        if (error_message) error_message->clear();
        return true;
    }
    if (!S_ISDIR(info.st_mode)) {
        if (error_message) *error_message = "Installed game path is not a directory.";
        return false;
    }

    std::function<bool(const std::string&)> remove_tree =
        [&](const std::string& path) -> bool {
            struct stat node {};
            if (stat(path.c_str(), &node) != 0) return true;

            if (!S_ISDIR(node.st_mode)) {
                return std::remove(path.c_str()) == 0;
            }

            DIR* dir = opendir(path.c_str());
            if (!dir) return false;

            bool ok = true;
            while (dirent* entry = readdir(dir)) {
                const std::string name = entry->d_name;
                if (name == "." || name == "..") continue;
                if (!remove_tree(path + "/" + name)) ok = false;
            }
            closedir(dir);

            if (rmdir(path.c_str()) != 0) ok = false;
            return ok;
        };

    if (!remove_tree(install_dir)) {
        if (error_message) {
            *error_message = "Could not fully remove the installed game files.";
        }
        return false;
    }

    if (error_message) error_message->clear();
    return true;
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

    report.unity =
        tree_contains_name(report.install_dir, "unityplayer.dll") ||
        tree_contains_name(report.install_dir, "unitycrashhandler32.exe") ||
        tree_contains_name(report.install_dir, "unitycrashhandler64.exe");

    report.unity_mono =
        tree_contains_name(report.install_dir, "monobleedingedge") ||
        tree_contains_name(report.install_dir, "mscorlib.dll") ||
        tree_contains_name(report.install_dir, "assembly-csharp.dll");

    report.steamworks =
        tree_contains_name(report.install_dir, "steam_api.dll") ||
        tree_contains_name(report.install_dir, "steam_api64.dll") ||
        tree_contains_name(report.install_dir, "facepunch.steamworks.win32.dll") ||
        tree_contains_name(report.install_dir, "facepunch.steamworks.win64.dll");

    report.d3d11_hint =
        tree_contains_name(report.install_dir, "d3d11.dll") ||
        tree_contains_name(report.install_dir, "unityplayer.dll");

    report.xinput_hint =
        tree_contains_name(report.install_dir, "xinput1_3.dll") ||
        tree_contains_name(report.install_dir, "xinput1_4.dll") ||
        tree_contains_name(report.install_dir, "unity.inputsystem.dll");

    if (report.unity) {
        report.engine = "Unity";
        if (report.unity_mono) {
            report.managed_runtime = "Mono";
        }
    }

    report.dependency_summary = join_dependency_summary(report);

    if (report.pe32_x86) {
        report.state = CompatState::ReadyForTranslator;
        report.detail =
            "PE32 x86 selected. " + report.dependency_summary +
            ". Ready for the x86 decoder/ARMv7 backend.";
    } else if (report.pe64_x86) {
        report.state = CompatState::UnsupportedBinary;
        report.detail =
            "PE32+ x86-64 selected. " + report.dependency_summary +
            ". x64 CPU translation is required before execution.";
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
