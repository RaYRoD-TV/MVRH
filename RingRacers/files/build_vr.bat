@echo off
rem Build Dr. Robotnik's Ring Racers with OpenXR VR support.
rem Uses the VS 2026 clang-cl toolchain + vcpkg (deps install on first run).

setlocal
set "VCPKG_ROOT=C:\vcpkg"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d "%~dp0"

cmake --preset ninja-x64_windows_vcpkg-develop
if errorlevel 1 goto :fail

rem Capture the build log: a locked exe can fail the link while the exit
rem code still comes back clean, so scan for ninja's failure markers too.
set "BUILDLOG=%TEMP%\ringracers_vr_build.log"
cmake --build --preset ninja-x64_windows_vcpkg-develop > "%BUILDLOG%" 2>&1
set "BUILDRC=%ERRORLEVEL%"
type "%BUILDLOG%"
if not "%BUILDRC%"=="0" goto :fail
findstr /C:"FAILED:" /C:"ninja: build stopped" "%BUILDLOG%" >nul && goto :fail

echo.
echo Build OK: build\ninja-x64_windows_vcpkg-develop\bin
exit /b 0

:fail
echo.
echo Build FAILED - see output above.
pause
exit /b 1
