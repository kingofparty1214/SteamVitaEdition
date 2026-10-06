#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "runtime_dependencies.h"
#include "x86_runtime.h"

enum class VitaProtonState {
    NotReady,
    ReadyForLoad,
    ReadyForTranslation,
    MissingRuntimePieces,
};

enum class VitaGraphicsBackend {
    None,
    OpenGL,
    Direct3D9,
    Direct3D11,
};

struct VitaProtonProfile {
    std::uint32_t app_id = 0;
    std::string game_name;
    std::string executable_path;
    std::string install_dir;

    GuestArchitecture architecture = GuestArchitecture::Unknown;
    VitaGraphicsBackend graphics = VitaGraphicsBackend::None;

    bool unity = false;
    bool mono = false;
    bool steamworks = false;
    bool needs_audio = false;
    bool needs_input = false;
    bool needs_network = false;

    PeImageInfo image;
    std::vector<RuntimeDependency> dependencies;
};

struct VitaProtonLaunchPlan {
    VitaProtonState state = VitaProtonState::NotReady;
    VitaProtonProfile profile;
    std::vector<std::string> missing_components;
    std::string detail;
};

VitaProtonLaunchPlan build_vita_proton_launch_plan(
    const VitaProtonProfile& profile);

const char* vita_proton_state_label(VitaProtonState state);
const char* vita_graphics_backend_label(VitaGraphicsBackend backend);
