@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set VCPKG_ROOT=C:\vcpkg
cd /d C:\Users\RaYRoD\Documents\Projects\VR_Dev\Games\DrRobotniksRingRacers\RingRacers
cmake --build --preset ninja-x64_windows_vcpkg-develop
