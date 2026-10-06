param([string]$OutDir = (Join-Path $env:TEMP 'ApexLotUvChecks'))
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $PSScriptRoot 'compiler_env.ps1')
Initialize-TerrainCompiler
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
Push-Location -LiteralPath $OutDir
try {
    & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/I'+$repo) /Felot_uv_checks.exe (Join-Path $PSScriptRoot 'lot_uv_fixture.cpp') (Join-Path $repo 'features/shader_patches.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'Lot UV checks compilation failed' }
    & './lot_uv_checks.exe' (Join-Path $repo 'features/lot_light_bridge.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'Lot UV checks failed' }
} finally { Pop-Location }
