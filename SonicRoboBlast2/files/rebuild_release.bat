@echo off
REM === Focused Release reconfigure + build (applies CMakeLists changes) ===
setlocal
if not defined VCPKG_ROOT set "VCPKG_ROOT=C:\vcpkg"
cd /d "%~dp0"

REM Free the exe in case a previous run is still open (prevents LNK1104 lock errors).
taskkill /F /IM srb2win.exe >nul 2>&1

cmake -S . -B build/vs2026 -G "Visual Studio 18 2026" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows ^
  -DSRB2_SDL2_EXE_NAME=srb2win
if errorlevel 1 ( echo CONFIGURE_FAILED & exit /b 2 )

cmake --build build/vs2026 --config Release
if errorlevel 1 ( echo BUILD_RELEASE_FAILED & exit /b 4 )

echo RELEASE_DONE
endlocal
