@echo off
REM ============================================================
REM  One-click SRB2 VR launch (OpenXR, head-tracked in-game render).
REM  Before running:
REM    - Headset connected + awake (Quest 3 via Link/Air Link/
REM      Virtual Desktop, or Pimax Dream Air).
REM    - An OpenXR runtime active: SteamVR running, OR Meta/Pimax/
REM      VDXR set as the active runtime.
REM  Launches SRB2 in OpenGL with vr_mode on. Start a level to see
REM  the world in the headset; console (~): chasecam 0/1 toggles
REM  first/third person, vr_scale tunes world scale.
REM ============================================================
cd /d "%~dp0build\vs2026\bin\Release"
(echo vr_mode 1)> autoexec.cfg
srb2win.exe -opengl -skipintro
del autoexec.cfg 2>nul
