@echo off
rem Launch Dr. Robotnik's Ring Racers in VR (OpenXR).
rem Put the headset on first: Quest 3 via Link / Air Link / Virtual Desktop
rem (make sure its OpenXR runtime is the active one), Pimax via Pimax Play.

setlocal

rem Game assets (bios.pk3 etc.). Set RINGRACERSWADDIR to an installed copy
rem before running, or leave it unset if the assets sit next to this script.
if not defined RINGRACERSWADDIR if exist "%~dp0bios.pk3" set "RINGRACERSWADDIR=%~dp0."

set "EXE=%~dp0build\ninja-x64_windows_vcpkg-develop\bin\ringracers_vr-support.exe"
if not exist "%EXE%" (
    rem Branch-suffixed name not found - take whatever ringracers exe the build produced.
    for %%F in ("%~dp0build\ninja-x64_windows_vcpkg-develop\bin\ringracers*.exe") do set "EXE=%%~fF"
)
if not exist "%EXE%" (
    echo No built executable found. Run build_vr.bat first.
    pause
    exit /b 1
)

start "" "%EXE%" -vr -skipintro -win
endlocal
