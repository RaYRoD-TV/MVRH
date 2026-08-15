# Test harness for headless SRB2 runs.
#
# The point of this script is one thing the obvious approach gets wrong:
# telling a CRASH apart from a CLEAN EXIT. SRB2 boots into an attract demo and
# will quit on its own, so "the process ended" proves nothing. Start-Process
# -PassThru also tends to hand back a Process object whose ExitCode never
# populates, which is how a whole run of tests can come back blank and be
# misread as crashes.
#
# So: launch through System.Diagnostics.Process directly (ExitCode is reliable
# there), and classify the result instead of guessing.
#
#   .\run_test.ps1 -GameArgs '-opengl','-warp','1','+gr_models On' -Seconds 40
#
param(
    [string[]] $GameArgs = @(),
    [int]      $Seconds  = 40,
    [string]   $Exe      = "$PSScriptRoot\build\ninja-vcpkg-release\bin\srb2win_vr.exe",
    [string]   $Assets   = "$PSScriptRoot\..\SRB2-assets",
    [string]   $Label    = "run"
)

$ErrorActionPreference = "Stop"

$home_ = Join-Path $env:TEMP "srb2_test_$Label"
New-Item -ItemType Directory -Path $home_ -Force | Out-Null
$out = Join-Path $home_ "stdout.txt"
$err = Join-Path $home_ "stderr.txt"

# Note the time so we only count crash dumps this run produced.
$started = Get-Date
$dumpDir = Join-Path $env:LOCALAPPDATA "CrashDumps"

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName               = $Exe
$psi.WorkingDirectory       = $Assets
$psi.Arguments              = (@("-home", "`"$home_`"") + $GameArgs) -join ' '
$psi.UseShellExecute         = $false
$psi.RedirectStandardOutput  = $true
$psi.RedirectStandardError   = $true

$proc = [System.Diagnostics.Process]::Start($psi)
# Read the pipes on background tasks; a full pipe buffer will otherwise wedge
# the game and look exactly like a hang.
$tOut = $proc.StandardOutput.ReadToEndAsync()
$tErr = $proc.StandardError.ReadToEndAsync()

$exited = $proc.WaitForExit($Seconds * 1000)
if (-not $exited)
{
    $verdict = "ALIVE"        # still running when time ran out: it did not crash
    $code = $null
    $proc.Kill(); $proc.WaitForExit()
}
else
{
    $code = $proc.ExitCode
    # Windows returns the exception code as the exit status. Anything in the
    # 0xC0000000 range is a hardware/OS fault, not the program deciding to stop.
    $u = [uint32]([int64]$code -band 0xFFFFFFFFL)
    if ($code -eq 0)            { $verdict = "CLEAN EXIT" }
    elseif ($u -ge 0xC0000000)  { $verdict = "CRASH" }
    else                        { $verdict = "EXIT($code)" }
}

Set-Content -Path $out -Value $tOut.Result -Encoding utf8
Set-Content -Path $err -Value $tErr.Result -Encoding utf8

$dumps = @()
if (Test-Path $dumpDir)
{
    $dumps = @(Get-ChildItem $dumpDir -Filter "srb2*" -ErrorAction SilentlyContinue |
               Where-Object { $_.LastWriteTime -ge $started })
}

$name = switch ([uint32]([int64]$code -band 0xFFFFFFFFL)) {
    0xC0000005 { "ACCESS VIOLATION" }
    0xC0000374 { "HEAP CORRUPTION" }
    0xC000041D { "FAULT IN CALLBACK" }
    0xC0000409 { "STACK BUFFER OVERRUN (/GS fast-fail)" }
    0x80000003 { "BREAKPOINT / assert" }
    default    { "" }
}

"{0,-22} {1}{2}" -f $Label, $verdict, $(if ($name) { " - $name" } else { "" })
if ($code -ne $null) { "   exit code : {0} (0x{1:X8})" -f $code, ([uint32]([int64]$code -band 0xFFFFFFFFL)) }
if ($dumps.Count)    { "   crash dump: $($dumps[0].Name)" }
"   last line : " + (@(Get-Content $out -ErrorAction SilentlyContinue) | Select-Object -Last 1)
"   log       : $out"
