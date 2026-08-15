# Quest round-trip: build the APK, install it, make sure the game data is on
# the headset, launch, and pull the log. Run from anywhere; paths are pinned.
# Every step fails loudly. Usage:
#   .\deploy_quest.ps1            build + install + launch + log
#   .\deploy_quest.ps1 -SkipBuild reuse the last APK
param(
    [switch]$SkipBuild,
    [int]$LogWaitSeconds = 10
)

$ErrorActionPreference = 'Stop'
$adb = 'C:\Android\sdk\platform-tools\adb.exe'
$proj = 'C:\g\srb2vr\android'
$apk = "$proj\app\build\outputs\apk\debug\srb2-vr-quest-v2215-debug.apk"
$pkg = 'com.rayrod.srb2vr'
$activity = "$pkg/org.stjr.srb2.SRB2Game"
$dataDir = "/sdcard/Android/data/$pkg/files"
$assetSrc = 'C:\Users\RaYRoD\Documents\Projects\VR_Dev\Games\SonicRoboBlast2\SRB2\build\vs2026\bin\Release'
$wads = @('srb2.pk3', 'zones.pk3', 'characters.pk3', 'music.pk3')

function Fail($msg) { Write-Host "DEPLOY_FAILED: $msg"; exit 1 }

# 1. Device present?
$devices = (& $adb devices) -match "device$"
if (-not $devices) { Fail 'no Quest on adb (wake the headset / check USB)' }

# 2. Build
if (-not $SkipBuild) {
    $env:JAVA_HOME = 'C:\Program Files\Android\openjdk\jdk-21.0.8'
    Push-Location $proj
    .\gradlew.bat assembleDebug --no-daemon 2>&1 | Out-File -Encoding utf8 "$proj\deploy_build.log"
    $code = $LASTEXITCODE
    Pop-Location
    if ($code -ne 0) { Fail "gradle exit $code (see $proj\deploy_build.log)" }
}
if (-not (Test-Path $apk)) { Fail "APK missing at $apk" }

# 3. Install
$out = & $adb install -r $apk 2>&1
if ($LASTEXITCODE -ne 0) { Fail "adb install: $out" }

# 4. Game data (pushed once; app-scoped dir needs no permission)
foreach ($w in $wads) {
    $have = & $adb shell "ls $dataDir/$w" 2>$null
    if (-not $have -or $have -match 'No such file') {
        $src = Join-Path $assetSrc $w
        if (-not (Test-Path $src)) { Fail "missing local asset $src" }
        Write-Host "pushing $w..."
        & $adb push $src "$dataDir/$w" | Out-Null
        if ($LASTEXITCODE -ne 0) { Fail "adb push $w" }
    }
}

# 5. Launch fresh
& $adb shell am force-stop $pkg
& $adb logcat -c
& $adb shell am start -n $activity | Out-Null
Start-Sleep -Seconds $LogWaitSeconds

# 6. Pull evidence: logcat + the game's own log file
& $adb logcat -d -v brief 2>$null |
    Select-String -Pattern 'srb2|SRB2|SDL|FATAL|AndroidRuntime|DEBUG' |
    Select-Object -First 60 | ForEach-Object { $_.Line.Trim() }
Write-Host '---- latest-log.txt ----'
& $adb shell "cat $dataDir/latest-log.txt" 2>$null | Select-Object -First 40
Write-Host 'DEPLOY_DONE'
