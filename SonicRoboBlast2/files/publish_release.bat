@echo off
REM ============================================================
REM  One-command release: publish_release.bat 2.2.15-vr2
REM  Tags the current vr HEAD and pushes; GitHub Actions builds,
REM  packages and publishes the release automatically.
REM ============================================================
setlocal
if "%~1"=="" (
	echo usage: publish_release.bat ^<version, e.g. 2.2.15-vr2^>
	exit /b 1
)
cd /d "%~dp0"
git diff --quiet && git diff --cached --quiet || (
	echo Working tree is not clean - commit or stash first.
	exit /b 2
)
git tag "v%~1" || exit /b 3
git push fork vr --tags || exit /b 4
echo.
echo Tagged v%~1 and pushed. CI is building the release now:
echo   https://github.com/RaYRoD-TV/SRB2-VR/actions
endlocal
