$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot
$shaderHeader = Get-Content -Raw -LiteralPath (Join-Path $repoRoot "src/hardware/hw_shaders.h")
$renderer = Get-Content -Raw -LiteralPath (Join-Path $repoRoot "src/hardware/hw_main.c")
$shaderBackend = Get-Content -Raw -LiteralPath (Join-Path $repoRoot "src/hardware/r_opengl/r_opengl.c")
$shaderApi = Get-Content -Raw -LiteralPath (Join-Path $repoRoot "src/hardware/hw_defs.h")

function Assert-Match([string] $Text, [string] $Pattern, [string] $Message)
{
	if ($Text -notmatch $Pattern)
	{
		throw "FAIL: $Message"
	}
}

Assert-Match $shaderHeader 'uniform float vr_uwcaustictime;' `
	"the underwater shader declares a dedicated caustic clock"
Assert-Match $shaderHeader 'cwa = sin\(cp\.x \+ vr_uwcaustictime \* 1\.15\)' `
	"the first caustic wave uses the dedicated clock"
Assert-Match $shaderHeader 'cwb = sin\(\(cp\.x \+ cp\.y\) \* 0\.73 - vr_uwcaustictime \* 0\.83\)' `
	"the second caustic wave uses the dedicated clock"
Assert-Match $shaderApi 'HWD_SHADERINFO_VRUWCAUSTICTIME' `
	"the renderer API exposes the caustic clock"
Assert-Match $renderer 'HWD_SHADERINFO_VRUWCAUSTICTIME,\s*g_vrActive \? -1 : \(INT32\)leveltime' `
	"VR freezes caustics while flat rendering keeps the animated clock"
Assert-Match $shaderBackend 'case HWD_SHADERINFO_VRUWCAUSTICTIME:' `
	"the OpenGL backend accepts the caustic clock"
Assert-Match $shaderBackend 'shader_vruwcaustictime\s*=\s*value < 0 \? 0\.0f : \(\(\(float\)\(value-1\)\) \+ FIXED_TO_FLOAT\(rendertimefrac\)\) / TICRATE;' `
	"the OpenGL backend preserves flat interpolation and freezes VR at zero"
Assert-Match $shaderBackend 'GETUNI\("vr_uwcaustictime"\)' `
	"the OpenGL backend locates the caustic clock uniform"
Assert-Match $shaderBackend 'gluniform_vr_uwcaustictime.*shader_vruwcaustictime' `
	"the OpenGL backend uploads the caustic clock uniform"

Write-Output "PASS: VR caustics are temporally stable and flat caustics stay animated"
