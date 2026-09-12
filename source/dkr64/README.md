# Diddy Kong Racing VR source

My VR changes for Golden Balloon 1.7.0 are in [files](files), using the
same paths as upstream.

Start with the [VR layer](files/platform/vr),
[camera integration](files/game/src/camera.c),
[render loop](files/platform/platform_sdl_min.c), or
[display-list renderer](files/platform/fast3d/gfx_pc_dkr.c).

## Get the full source

With Python 3.9 or later and Git installed:

```powershell
git clone --branch dkr-source --single-branch https://github.com/RaYRoD-TV/MVRH.git MVRH-DKR-source
cd MVRH-DKR-source
python source/dkr64/prepare-source.py C:/src/dkr-vr
```

Choose a folder that doesn't already exist. The script downloads the pinned
upstream source, adds my changes, then fetches the stock OpenXR and SDL files.
Their versions and checksums are in [source.json](source.json).

The new folder is a complete source checkout. HEAD stays at the upstream
commit, so you can review changes to existing files with:

```powershell
git -C C:/src/dkr-vr diff --stat
git -C C:/src/dkr-vr diff -- game/src/camera.c
```

New files such as `platform/vr` are untracked. Include them when staging a
commit or exporting a diff.

## Upstream base

- Repository: [akratch/goldenballoon](https://github.com/akratch/goldenballoon)
- Tag: `v1.7.0`
- Commit: `106bad37244a2f8829bab21c655d25ff4b4dcdbf`

## Build on Windows

Use a MinGW-w64 environment with CMake, Ninja, pkg-config and SDL2 available.
From the assembled checkout:

```sh
cmake -S . -B build-vr -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DMDKR_VR=ON -DMDKR_ENABLE_ONLINE_BETA=ON -DMDKR_ENABLE_ONLINE_ROOM_PREVIEW=OFF
cmake --build build-vr --target mdkr64
```

CMake downloads the remaining build dependencies. Bring your own US 1.1 or
European 1.1 ROM. This repository contains source, not a PC game download.

## Current status

This is the Windows 1.7.0 preview. Flat and VR builds passed, along with 26
selected tests. Headset testing and testing with the actual community texture
pack are still pending.

The Android code comes from the earlier Quest port. I haven't built or tested
this 1.7.0 version on Quest yet. See the [Quest notes](files/android/QUEST_PORT.md).

## Licenses

Keep the upstream license and third-party notices. [LICENSE.upstream](LICENSE.upstream)
is a copy of the upstream MIT license; [NOTICE.md](NOTICE.md) links the terms
for the other parts of the source.

To test the setup script:

```sh
python source/dkr64/test_prepare_source.py
```
