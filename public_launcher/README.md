# SteamVita Public Launcher

This directory contains the public-facing launcher and installer portion of SteamVita.

The compatibility/runtime implementation is intentionally **not included** here yet. The private compatibility code is still experimental and has caused hard crashes, complete system hangs, and other unknown issues on real PlayStation Vita hardware during testing.

## Included here

- Public launcher-facing source structure
- Installer-facing interfaces
- Safe runtime bridge API
- Explicit launch blocking
- Placeholder compatibility hooks showing where the private implementation would normally connect

## Intentionally removed

The following are **not** included in this public source section:

- x86/ARM compatibility runtime
- executable translation or emulation code
- private compatibility shims
- experimental launch backends
- any code path capable of starting a Windows game

The public runtime bridge always returns `Blocked`. This is intentional.

## Why launching is blocked

The compatibility layer is not ready for public testing. Internal builds have caused hard crashes on PS Vita hardware and other behavior that has not been fully diagnosed.

Until that code is considered safe enough to distribute, this public section can be used to inspect or contribute to launcher and installer work without exposing or accidentally invoking the private runtime.

## Private implementation boundary

Internal builds eventually hand an installed title to the private compatibility runtime.

In this public tree, that handoff is replaced with:

```cpp
RuntimeLaunchResult launch_installed_game(
    std::uint32_t app_id,
    const std::string& install_dir) {
    (void)app_id;
    (void)install_dir;

    return {
        RuntimeLaunchState::Blocked,
        "Compatibility runtime removed from the public launcher build."
    };
}
```

Do not replace this stub with a direct executable launch path. The block is deliberate.
