@echo off
REM === SRB2 2.2.15 MSVC build helper (vcpkg + Ninja, release) ===
setlocal

call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64
if errorlevel 1 ( echo VSDEVCMD_FAILED & exit /b 1 )

set "VCPKG_ROOT=C:\Users\RaYRoD\vcpkg"
set "VCPKG_OVERLAY_TRIPLETS=C:\Users\RaYRoD\srb2-vcpkg-overlays\triplets"

cd /d "%~dp0"

echo ===== CONFIGURE (vcpkg deps + cmake) =====
cmake --preset ninja-vcpkg-release -DVCPKG_TARGET_TRIPLET=x64-windows-rel -DVCPKG_HOST_TRIPLET=x64-windows-rel
if errorlevel 1 ( echo CONFIGURE_FAILED & exit /b 2 )

echo ===== BUILD (SRB2) =====
cmake --build build/ninja-vcpkg-release
if errorlevel 1 ( echo BUILD_FAILED & exit /b 3 )

echo ALL_DONE
endlocal
