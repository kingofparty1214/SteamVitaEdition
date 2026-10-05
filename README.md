# SteamVita Edition

SteamVita is an **unofficial, experimental** PlayStation Vita PC-game compatibility project. It is not affiliated with Valve or Sony.

The goal is a normal Vita homebrew VPK that opens from LiveArea and presents a console-friendly game library. It does **not** require opening the Vita or booting Linux.

## Current status: v0.1 launcher shell

The first build is intentionally small. It proves the native Vita app, controls, game-folder scanning, and per-game profiles before we add emulator/translation backends.

### What v0.1 does

- Runs as a normal VPK from LiveArea.
- Scans `ux0:data/SteamVita/games/`.
- Shows one entry per game folder.
- Reads an optional `game.ini`.
- D-pad Up/Down selects a game.
- X attempts to launch the selected backend (placeholder in v0.1).
- Triangle rescans the library.
- Circle exits.

Example layout:

```text
ux0:data/SteamVita/
└── games/
    └── DOOM/
        ├── game.ini
        ├── DOOM.EXE
        └── ...
```

Example `game.ini`:

```ini
name=DOOM
backend=dosbox
entry=DOOM.EXE
```

## Roadmap

### v0.2 - DOS backend
- Embed/port a DOSBox-compatible core.
- Mount each game's folder as the emulated C: drive.
- Launch the configured `entry=` executable.
- Add Vita controller-to-keyboard/mouse mappings.
- Add audio and per-game settings.

### v0.3 - polished library
- Cover images.
- Favorites and recent games.
- Controller profiles.
- Virtual keyboard.
- Compatibility status per game.

### v0.4 - experimental x86 backend
- Investigate a narrow 32-bit x86 -> ARMv7 translation layer.
- Start with tiny, software-rendered games.
- Explore lightweight Win32/Linux compatibility only after the translator works.

A full desktop Steam client is **not** the first target. The project is aimed at old/simple games that can realistically fit the Vita's CPU, RAM, and GPU limits.

## Automatic builds

Every push to `main` runs the GitHub Actions workflow in `.github/workflows/build.yml`. A successful run uploads `SteamVita.vpk` as a workflow artifact.

## Build locally

With VitaSDK installed:

```bash
vdpm install libvita2d
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The VPK will be at `build/SteamVita.vpk`.
