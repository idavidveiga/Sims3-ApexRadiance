param([string]$OutDir=(Join-Path $env:TEMP 'ApexOutdoorDeltaChecks'))
# Outdoor lamp edits by delta (features/outdoor_delta_model.h): compiles check.cpp against the production header and runs it.
# Read-only: writes only the compiler outputs to -OutDir.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT /fp:precise ('/I'+$repo) /Feoutdoor_delta_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
 if($LASTEXITCODE -ne 0){throw 'Outdoor delta checks compilation failed'}
 & './outdoor_delta_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'Outdoor delta checks failed'}
}finally{Pop-Location}
