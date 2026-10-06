#pragma once

#include <string>
#include <vector>

enum class RuntimeDependencyKind {
    WindowsShim,
    GameLocal,
    SteamRedistributableCandidate,
    Unsupported,
};

struct RuntimeDependency {
    std::string name;
    RuntimeDependencyKind kind = RuntimeDependencyKind::Unsupported;
    bool satisfied = false;
    std::string detail;
};

std::vector<RuntimeDependency> resolve_runtime_dependencies(
    const std::string& install_dir,
    const std::vector<std::string>& imported_dlls);
