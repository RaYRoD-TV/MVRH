# Stage the native libraries gradle packages into the APK, then verify the staged bytes match
# the built bytes. A staged copy that silently failed reads exactly like a code bug on device.
$ErrorActionPreference = 'Stop'
$bld = 'C:\g\dkrvr\goldenballoon\build-android'
$dst = Join-Path $PSScriptRoot 'app\src\main\jniLibs\arm64-v8a'
New-Item -ItemType Directory -Force $dst | Out-Null

$main = Join-Path $bld 'libmain.so'
$sdl  = Join-Path $bld '_deps\sdl2-build\libSDL2.so'
# THE LOADER TRAVELS WITH THE APP. libmain.so links against it, so leaving it out gives a dlopen
# failure at launch and nothing else: the app dies before a single line of the game runs, with no
# mention of VR anywhere in the log.
$xr   = Join-Path $PSScriptRoot 'openxr\android\arm64-v8a\libopenxr_loader.so'

$parts = [ordered]@{ 'libmain.so' = $main; 'libSDL2.so' = $sdl; 'libopenxr_loader.so' = $xr }
foreach ($name in $parts.Keys) {
    if (-not (Test-Path $parts[$name])) { Write-Host "STAGE FAILED: missing $($parts[$name])"; exit 1 }
}
Copy-Item @($parts.Values) $dst -Force

$sizes = @()
foreach ($name in $parts.Keys) {
    & "$env:SystemRoot\System32\fc.exe" /b $parts[$name] (Join-Path $dst $name) > $null
    if ($LASTEXITCODE -ne 0) { Write-Host "STAGE FAILED: $name differs from the built file"; exit 1 }
    $sizes += '{0} {1:n0}' -f $name, (Get-Item (Join-Path $dst $name)).Length
}
Write-Host "== STAGED OK: $($sizes -join ', ') =="
