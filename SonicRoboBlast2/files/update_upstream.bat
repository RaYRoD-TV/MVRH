@echo off
REM ============================================================
REM  Rebase the vr branch onto a new upstream SRB2 release:
REM    update_upstream.bat SRB2_release_2.2.16
REM  On conflicts: fix, "git add", "git rebase --continue".
REM  The VR layer is src/vr/ plus small hooks - conflicts should stay tiny.
REM  Afterwards: update .github/UPSTREAM_BASE, build, test,
REM  then publish_release.bat <new version>.
REM ============================================================
setlocal
if "%~1"=="" (
	echo usage: update_upstream.bat ^<upstream tag, e.g. SRB2_release_2.2.16^>
	exit /b 1
)
cd /d "%~dp0"
git fetch origin --tags || exit /b 2
git rebase "%~1" vr || (
	echo.
	echo Rebase stopped on conflicts - resolve, then: git rebase --continue
	exit /b 3
)
echo.
echo vr is now based on %~1. Next: update .github/UPSTREAM_BASE, rebuild, test.
endlocal
