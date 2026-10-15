param([Parameter(Mandatory=$true)][string]$Precomp, [string[]]$Captures = @(), [string]$OutDir = (Join-Path $env:TEMP 'ApexRoomMapSmoothCheck'))
# Smooth light maps, offline check (read-only: prints to stdout; the compiler's outputs stay in $OutDir)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$exe = Join-Path $OutDir 'room_map_smooth_check.exe'
Push-Location $OutDir
try {
    & cl.exe /nologo /std:c++20 /utf-8 /EHsc /W4 /O2 /MT ('/Fe' + $exe) (Join-Path $PSScriptRoot 'check.cpp') (Join-Path $repo 'features/shader_patches.cpp') d3d9.lib d3dcompiler.lib user32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Room map smooth check did not compile' }
    & $exe (Resolve-Path -LiteralPath $Precomp).Path @($Captures | ForEach-Object { (Resolve-Path -LiteralPath $_).Path })
    if ($LASTEXITCODE -ne 0) { throw 'Room map smooth check failed' }
} finally { Pop-Location }
