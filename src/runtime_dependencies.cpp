#include "runtime_dependencies.h"

#include <algorithm>
#include <cctype>
#include <sys/stat.h>

namespace {

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
    return value;
}

bool file_exists(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool is_windows_system_dll(const std::string& dll) {
    static const char* names[] = {
        "kernel32.dll", "user32.dll", "advapi32.dll", "shell32.dll",
        "ole32.dll", "oleaut32.dll", "gdi32.dll", "winmm.dll",
        "ws2_32.dll", "bcrypt.dll", "ntdll.dll", "comdlg32.dll",
        "shlwapi.dll", "version.dll", "imm32.dll", "setupapi.dll",
        "d3d11.dll", "dxgi.dll", "d3dcompiler_47.dll",
        "xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll"
    };

    for (const char* name : names) {
        if (dll == name) return true;
    }
    return false;
}

bool looks_like_vc_runtime(const std::string& dll) {
    return dll.find("msvcp") == 0 ||
           dll.find("vcruntime") == 0 ||
           dll.find("api-ms-win-crt-") == 0 ||
           dll == "ucrtbase.dll";
}

} // namespace

std::vector<RuntimeDependency> resolve_runtime_dependencies(
    const std::string& install_dir,
    const std::vector<std::string>& imported_dlls) {
    std::vector<RuntimeDependency> result;
    result.reserve(imported_dlls.size());

    for (const std::string& raw : imported_dlls) {
        RuntimeDependency dependency;
        dependency.name = lower_ascii(raw);

        if (file_exists(install_dir + "/" + dependency.name)) {
            dependency.kind = RuntimeDependencyKind::GameLocal;
            dependency.satisfied = true;
            dependency.detail = "Provided by the game.";
        } else if (is_windows_system_dll(dependency.name)) {
            dependency.kind = RuntimeDependencyKind::WindowsShim;
            dependency.satisfied = false;
            dependency.detail = "Requires a SteamVita compatibility shim.";
        } else if (looks_like_vc_runtime(dependency.name)) {
            dependency.kind =
                RuntimeDependencyKind::SteamRedistributableCandidate;
            dependency.satisfied = false;
            dependency.detail =
                "Visual C++ runtime candidate; Steam prerequisite resolution can provide supporting files when usable.";
        } else {
            dependency.kind = RuntimeDependencyKind::Unsupported;
            dependency.satisfied = false;
            dependency.detail = "No dependency provider selected yet.";
        }

        result.push_back(dependency);
    }

    return result;
}
