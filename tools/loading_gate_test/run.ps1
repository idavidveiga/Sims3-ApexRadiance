param([string]$OutDir=(Join-Path $env:TEMP 'ApexLoadingGateChecks'))
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$gui=Get-Content -LiteralPath (Join-Path $repo 'apex_gui.cpp') -Raw
$blur=Get-Content -LiteralPath (Join-Path $repo 'patches/depth_blur_patch.cpp') -Raw
$hint=[regex]::Match($gui,'(?s)void UpdateHint\(\) \{.*?\r?\n\}')
$guard=[regex]::Match($blur,'(?s)void BlurEffect\(IDirect3DDevice9\* dev\) \{.*?(?=    const float dt = StepTime\(\);)')
if(!$hint.Success -or !$guard.Success){throw 'Production loading guards not found'}
$body=$guard.Value.Replace('void BlurEffect(IDirect3DDevice9* dev)','void ExtractedBlurGuard(void*)')+"    ++passes;`n}`n"
Set-Content -LiteralPath (Join-Path $OutDir 'extracted_loading_guards.h') -Value ($hint.Value+"`n"+$body) -Encoding UTF8
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
    & cl.exe /nologo /std:c++20 /EHsc /W4 /Od /RTC1 /MT ('/I'+$repo) ('/I'+(Join-Path $repo 'framework')) ('/I'+$OutDir) /Feloading_gate_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
    if($LASTEXITCODE -ne 0){throw 'Loading guard checks compilation failed'}
    & './loading_gate_checks.exe'
    if($LASTEXITCODE -ne 0){throw 'Loading guard checks failed'}
}finally{Pop-Location}
