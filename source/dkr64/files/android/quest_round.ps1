# The whole Quest round-trip in one command: native build -> stage libs -> gradle APK ->
# adb install -> launch -> tail the log. A silent install failure reads exactly like a code bug,
# so every step gates the next and says what failed.
#
#   .\quest_round.ps1                    immersive debug APK, install + launch + log
#   .\quest_round.ps1 -Panel             the 2D shell instead: boots with the controllers off
#                                        and the headset on a desk, which is the only way to
#                                        check "does it come up and render" without wearing it
#   .\quest_round.ps1 -Release           release APK (signed when keystore.properties is set up)
#   .\quest_round.ps1 -NoRun             build + install only
#   .\quest_round.ps1 -SkipNative        gradle onward, when the .so is already current
param(
    [switch]$Panel,
    [switch]$Release,
    [switch]$NoRun,
    [switch]$SkipNative,
    [int]$LogSeconds = 15
)
$ErrorActionPreference = 'Stop'
$adb = 'C:\Android\sdk\platform-tools\adb.exe'

$flavor = if ($Panel) { 'panel' } else { 'immersive' }
$pkg    = if ($Panel) { 'com.rayrod.goldenballoon.panel' } else { 'com.rayrod.goldenballoon' }

if (-not $SkipNative) {
    Write-Host '== 1/5 native build =='
    & (Join-Path $PSScriptRoot 'build_native.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'NATIVE BUILD FAILED' }

    Write-Host '== 2/5 stage libs =='
    & (Join-Path $PSScriptRoot 'stage_native.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'STAGE FAILED' }
}

Write-Host "== 3/5 gradle ($flavor) =="
$build = if ($Release) { 'Release' } else { 'Debug' }
$task  = 'assemble' + $flavor.Substring(0,1).ToUpper() + $flavor.Substring(1) + $build
Push-Location $PSScriptRoot
try {
    & .\gradlew.bat $task
    if ($LASTEXITCODE -ne 0) { throw 'GRADLE FAILED' }
} finally { Pop-Location }

$buildDir = if ($Release) { 'release' } else { 'debug' }
$apk = Join-Path $PSScriptRoot "app\build\outputs\apk\$flavor\$buildDir\app-$flavor-$buildDir.apk"
if (-not (Test-Path $apk)) { throw "APK NOT PRODUCED at $apk" }
Write-Host "APK: $apk ($([math]::Round((Get-Item $apk).Length / 1MB, 1)) MB)"

Write-Host '== 4/5 adb install =='
& $adb install -r $apk
if ($LASTEXITCODE -ne 0) { throw 'ADB INSTALL FAILED' }

if ($NoRun) { Write-Host 'installed (NoRun)'; exit 0 }

Write-Host '== 5/5 launch + log =='
# A headset that is merely asleep queues the launch behind vrshell and never spawns the process,
# which reads exactly like a crash. Say so instead of handing back an empty log.
$wake = (& $adb shell dumpsys power 2>$null | Select-String 'mWakefulness=' | Select-Object -First 1)
Write-Host "device: $wake"

& $adb logcat -c
& $adb shell am start -n "$pkg/com.rayrod.goldenballoon.GoldenBalloonActivity"
Start-Sleep -Seconds $LogSeconds
$pid_ = (& $adb shell pidof $pkg)
if (-not $pid_) {
    Write-Host 'NO PROCESS. Either the headset is asleep, or an immersive launch was refused'
    Write-Host 'because the controllers are off. Try -Panel to boot it from the desk.'
}
& $adb logcat -d -s mdkr64:V SDL:V "SDL/APP:V" AndroidRuntime:E DEBUG:I libc:F
