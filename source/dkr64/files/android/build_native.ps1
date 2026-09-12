# Diddy Kong Racing VR on Quest: configure + build libmain.so for arm64-v8a.
#
# Always builds through the short junction C:\g\dkrvr. The NDK toolchain dies at 0xc0000005 on
# long object paths and the real tree sits deep under Documents; the junction keeps every object
# path short. Log: native_build.log beside this script.
#   .\build_native.ps1           incremental
#   .\build_native.ps1 -Fresh    wipe the build dir first
param([switch]$Fresh)
$ErrorActionPreference = 'Continue'

$env:ANDROID_NDK_HOME = 'C:\Android\sdk\ndk\27.0.12077973'
$src = 'C:\g\dkrvr\goldenballoon'
$bld = 'C:\g\dkrvr\goldenballoon\build-android'
$xr  = 'C:\g\dkrvr\goldenballoon\android\openxr'

if (-not (Test-Path 'C:\g\dkrvr')) {
    Write-Host '== CONFIGURE FAILED: C:\g\dkrvr junction is missing =='
    Write-Host '   New-Item -ItemType Junction -Path C:\g\dkrvr -Target <...>\Games\N64\DiddyKongRacing'
    exit 1
}
if ($Fresh -and (Test-Path $bld)) { Remove-Item -Recurse -Force $bld }

# MDKR_APP=OFF     - the ImGui launcher shell is a desktop lane; on Android SDLActivity is the
#                    shell and platform/main_pc.c owns main().
# MDKR_WEBGPU=OFF  - wgpu-native ships no arm64-android binary and the GL backend is the VR one.
# MDKR_VR=ON       - builds platform/vr/vr_openxr.cpp; the Android arm binds OpenXR to SDL's
#                    GLES context and links the prebuilt Khronos loader instead of fetching one.
cmake -G Ninja -S $src -B $bld `
  -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_TOOLCHAIN_FILE=$env:ANDROID_NDK_HOME\build\cmake\android.toolchain.cmake" `
  -DANDROID_ABI=arm64-v8a `
  -DANDROID_PLATFORM=android-26 `
  -DMDKR_APP=OFF `
  -DMDKR_WEBGPU_BACKEND=OFF `
  -DMDKR_VR=ON `
  -DBUILD_TESTING=OFF `
  "-DMDKR_ANDROID_OPENXR_ROOT=$xr"
if ($LASTEXITCODE -ne 0) { Write-Host '== CONFIGURE FAILED =='; exit 1 }

cmake --build $bld -j
if ($LASTEXITCODE -ne 0) { Write-Host '== BUILD FAILED =='; exit 1 }

$out = Get-ChildItem -Recurse $bld -Filter 'libmain.so' -ErrorAction SilentlyContinue | Select-Object -First 1
if ($out) { Write-Host "== OK: $($out.FullName) $((Get-Item $out.FullName).Length) bytes ==" }
else { Write-Host '== BUILD FAILED: no libmain.so produced =='; exit 1 }
