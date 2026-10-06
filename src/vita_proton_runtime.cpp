#include "vita_proton_runtime.h"
#include "win32_shims.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace {

bool imports_dll(const PeImageInfo& image, const char* dll) {
    return std::find(
        image.imported_dlls.begin(),
        image.imported_dlls.end(),
        std::string(dll)) != image.imported_dlls.end();
}

bool dependency_is_unresolved(const RuntimeDependency& dependency) {
    return dependency.kind == RuntimeDependencyKind::Unsupported;
}

bool has_game_local_dependency(
    const std::vector<RuntimeDependency>& dependencies) {
    for (const RuntimeDependency& dependency : dependencies) {
        if (dependency.kind == RuntimeDependencyKind::GameLocal) return true;
    }
    return false;
}

bool runtime_capability(const char* key) {
    std::ifstream input("ux0:data/SteamVita/runtime/capabilities.ini");
    if (!input) return false;

    const std::string prefix = std::string(key) + "=";
    std::string line;
    while (std::getline(input, line)) {
        if (line.compare(0, prefix.size(), prefix) == 0) {
            const std::string value = line.substr(prefix.size());
            return value == "1" || value == "true" || value == "yes";
        }
    }
    return false;
}

bool runtime_pack_present() {
    std::ifstream version("ux0:data/SteamVita/runtime/runtime.version");
    std::ifstream manifest("ux0:data/SteamVita/runtime/runtime.manifest");
    std::ifstream capabilities("ux0:data/SteamVita/runtime/capabilities.ini");
    return static_cast<bool>(version) &&
           static_cast<bool>(manifest) &&
           static_cast<bool>(capabilities);
}

void require_capability(VitaProtonLaunchPlan* plan,
                        const char* capability,
                        const char* label) {
    if (!plan || runtime_capability(capability)) return;
    plan->missing_components.push_back(
        std::string(label) + " (not implemented in installed runtime)");
}

} // namespace

VitaProtonLaunchPlan build_vita_proton_launch_plan(
    const VitaProtonProfile& profile) {
    VitaProtonLaunchPlan plan;
    plan.profile = profile;

    if (!profile.image.valid ||
        profile.architecture == GuestArchitecture::Unknown ||
        profile.architecture == GuestArchitecture::Other) {
        plan.state = VitaProtonState::NotReady;
        plan.detail = "No supported Windows PE image is available.";
        return plan;
    }

    if (!runtime_pack_present()) {
        plan.missing_components.push_back(
            "Verified Vita Proton runtime pack (not installed or incomplete)");
    }

    for (const RuntimeDependency& dependency : profile.dependencies) {
        if (dependency_is_unresolved(dependency)) {
            plan.missing_components.push_back(dependency.name);
        }
    }

    PeLoadedImage loaded = load_pe_image(profile.executable_path);
    if (loaded.valid) {
        const Win32ImportBindResult import_bind =
            bind_win32_imports(&loaded);
        plan.bound_import_count = import_bind.bound.size();
        plan.unresolved_import_count = import_bind.unresolved.size();

        if (!import_bind.unresolved.empty()) {
            plan.missing_components.push_back(
                "Win32 import shims (" +
                std::to_string(import_bind.unresolved.size()) +
                " unresolved)");
        }
    } else {
        plan.missing_components.push_back("PE runtime image loader");
    }

    if (has_game_local_dependency(profile.dependencies)) {
        require_capability(
            &plan, "pe_dll_linker", "PE DLL loader/linker");
    }

    if (profile.architecture == GuestArchitecture::X86_64) {
        require_capability(
            &plan, "x64_armv7_execution",
            "x64-to-ARMv7 execution backend");
    } else if (profile.architecture == GuestArchitecture::X86_32) {
        require_capability(
            &plan, "x86_armv7_execution",
            "x86-to-ARMv7 execution backend");
    }

    if (profile.graphics == VitaGraphicsBackend::OpenGL) {
        require_capability(
            &plan, "opengl_backend",
            "OpenGL-to-Vita graphics backend");
    } else if (profile.graphics == VitaGraphicsBackend::Direct3D9) {
        require_capability(
            &plan, "d3d9_backend",
            "D3D9-to-Vita graphics backend");
    } else if (profile.graphics == VitaGraphicsBackend::Direct3D11) {
        require_capability(
            &plan, "d3d11_backend",
            "D3D11-to-Vita graphics backend");
    }

    if (imports_dll(profile.image, "user32.dll")) {
        require_capability(
            &plan, "user32_backend",
            "USER32 compatibility layer");
    }
    if (imports_dll(profile.image, "winmm.dll")) {
        require_capability(
            &plan, "audio_backend",
            "WinMM audio/timing layer");
    }
    if (imports_dll(profile.image, "ws2_32.dll")) {
        require_capability(
            &plan, "winsock_backend",
            "Winsock compatibility layer");
    }

    if (!plan.missing_components.empty()) {
        plan.state = VitaProtonState::MissingRuntimePieces;
        plan.detail =
            "Runtime files verified; " +
            std::to_string(plan.bound_import_count) +
            " imports bound, " +
            std::to_string(plan.unresolved_import_count) +
            " unresolved. " +
            std::to_string(plan.missing_components.size()) +
            " compatibility features are not implemented yet.";
        return plan;
    }

    plan.state = VitaProtonState::ReadyForTranslation;
    plan.detail = "Vita Proton launch plan is ready for guest execution.";
    return plan;
}

const char* vita_proton_state_label(VitaProtonState state) {
    switch (state) {
        case VitaProtonState::NotReady:
            return "Not ready";
        case VitaProtonState::ReadyForLoad:
            return "Ready for load";
        case VitaProtonState::ReadyForTranslation:
            return "Ready for translation";
        case VitaProtonState::MissingRuntimePieces:
            return "Runtime components missing";
    }
    return "Unknown";
}

const char* vita_graphics_backend_label(VitaGraphicsBackend backend) {
    switch (backend) {
        case VitaGraphicsBackend::None:
            return "None";
        case VitaGraphicsBackend::OpenGL:
            return "OpenGL";
        case VitaGraphicsBackend::Direct3D9:
            return "Direct3D 9";
        case VitaGraphicsBackend::Direct3D11:
            return "Direct3D 11";
    }
    return "Unknown";
}
