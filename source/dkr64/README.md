# Diddy Kong Racing VR source

The files I added or modified for the Golden Balloon 1.7.0 VR preview are in
[files](files). They are ordinary source files, with the upstream directory
layout preserved.

Start with the [VR layer](files/platform/vr),
[camera integration](files/game/src/camera.c),
[render loop](files/platform/platform_sdl_min.c), or
[display-list renderer](files/platform/fast3d/gfx_pc_dkr.c).

## Get a complete working checkout

Install Python 3.9 or later and Git. Clone the `dkr-source` branch, then run
the setup command:

```powershell
git clone --branch dkr-source --single-branch https://github.com/RaYRoD-TV/MVRH.git MVRH-DKR-source
cd MVRH-DKR-source
python source/dkr64/prepare-source.py C:/src/dkr-vr
```

Choose a new destination folder. Existing folders are left alone. The command
checks out the exact upstream base, copies these source files over it, and
downloads the stock OpenXR headers and SDL Android support files separately.
Both archives and every selected dependency file are checked against pinned
SHA-256 hashes in [source.json](source.json).

The resulting folder is the complete game source. Its Git HEAD stays at the
upstream base, so the VR changes can be reviewed as normal text diffs:

```powershell
git -C C:/src/dkr-vr diff --stat
git -C C:/src/dkr-vr diff -- game/src/camera.c
```

New files, including `platform/vr`, are present as ordinary untracked files.
Stage them before making a commit or exporting a complete diff.

## Upstream base

- Repository: [akratch/goldenballoon](https://github.com/akratch/goldenballoon)
- Tag: `v1.7.0`
- Commit: `106bad37244a2f8829bab21c655d25ff4b4dcdbf`

The source files here are an overlay on that base. The setup command assembles
the full checkout without requiring the earlier single-file source patch.

## Build on Windows

Use a MinGW-w64 environment with CMake, Ninja, pkg-config and SDL2 available.
From the assembled checkout:

```sh
cmake -S . -B build-vr -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DMDKR_VR=ON -DMDKR_ENABLE_ONLINE_BETA=ON -DMDKR_ENABLE_ONLINE_ROOM_PREVIEW=OFF
cmake --build build-vr --target mdkr64
```

CMake fetches its other pinned build dependencies. The game reads a supported
US 1.1 or European 1.1 ROM supplied locally. No ROM, save, game capture or
standalone game executable is included here.

## Current status

The Windows preview passed flat and VR builds and 26 selected checks.
Synthetic-headset captures covered stereo, first person, head roll, the HUD
and Rice-format texture replacements. Direct headset review and the actual
community texture pack remain pending.

The Android source is carried forward from the earlier Quest port. The 1.7.0
Android build and headset behavior have not been qualified. The existing
[Quest notes](files/android/QUEST_PORT.md) describe that boundary.

Source publication does not replace the game downloads in the hub. The
[earlier source patch](../../patches/dkr64-v1.7.0-vr.patch) remains a snapshot
of the same preview; use this directory for browsing and further work.

## Source terms

The assembled checkout retains the upstream license and third-party notices.
An unchanged copy of the upstream MIT license is included as
[LICENSE.upstream](LICENSE.upstream). See [NOTICE.md](NOTICE.md) for the
upstream terms that apply to the different parts of the code.

The setup helper can be checked with:

```sh
python source/dkr64/test_prepare_source.py
```
