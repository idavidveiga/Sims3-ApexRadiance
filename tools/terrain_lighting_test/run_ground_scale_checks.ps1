param([Parameter(Mandatory=$true)][string]$Captures, [string]$OutDir = (Join-Path $env:TEMP 'ApexGroundScaleTests'))
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $PSScriptRoot 'compiler_env.ps1')
Initialize-TerrainCompiler
$Captures = (Resolve-Path -LiteralPath $Captures).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$exe = Join-Path $OutDir 'ground_scale_check.exe'
Push-Location $OutDir
try {
    & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/I' + $repo) ('/Fe' + $exe) (Join-Path $PSScriptRoot 'ground_scale_check.cpp') (Join-Path $repo 'features/shader_patches.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'Ground scale checks did not compile' }
    & $exe $Captures
    if ($LASTEXITCODE -ne 0) { throw 'Ground scale checks failed' }
} finally { Pop-Location }
