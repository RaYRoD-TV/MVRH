@echo off
REM === SRB2 2.2.15 Visual Studio solution generator + build (vcpkg, Debug+Release) ===
setlocal

if not defined VCPKG_ROOT set "VCPKG_ROOT=C:\vcpkg"
cd /d "%~dp0"

echo ===== CONFIGURE (generate VS 2026 solution + build vcpkg deps: Debug+Release) =====
cmake -S . -B build/vs2026 -G "Visual Studio 18 2026" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows
if errorlevel 1 ( echo CONFIGURE_FAILED & exit /b 2 )

echo ===== BUILD Debug =====
cmake --build build/vs2026 --config Debug
if errorlevel 1 ( echo BUILD_DEBUG_FAILED & exit /b 3 )

echo ===== BUILD Release =====
cmake --build build/vs2026 --config Release
if errorlevel 1 ( echo BUILD_RELEASE_FAILED & exit /b 4 )

echo ALL_DONE
endlocal
