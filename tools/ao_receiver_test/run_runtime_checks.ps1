param(
    [string]$Repo = (Join-Path $PSScriptRoot '../..'),
    [string]$OutDir = (Join-Path $env:TEMP 'ApexReceiverReplayChecks'),
    [string]$SourcePath = ''
)
$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path -LiteralPath $Repo).Path
if (-not $SourcePath) { $SourcePath = $Repo }
$SourcePath = (Resolve-Path -LiteralPath $SourcePath).Path
if (Test-Path -LiteralPath $SourcePath -PathType Container) {
    $patchPath = Join-Path $SourcePath 'patches/ambient_occlusion_patch.cpp'
    $shaderPath = Join-Path $SourcePath 'features/shader_patches.cpp'
} else {
    $patchPath = $SourcePath
    $shaderPath = Join-Path (Split-Path -Parent (Split-Path -Parent $SourcePath)) 'features/shader_patches.cpp'
}
$source = Get-Content -LiteralPath $patchPath -Raw
$start = $source.IndexOf('struct SimMaskCopy {', [StringComparison]::Ordinal)
$end = $source.IndexOf('void ReleaseResources() {', $start, [StringComparison]::Ordinal)
if ($start -lt 0 -or $end -le $start) { throw 'Production receiver replay boundaries changed; update fixture extraction' }
if (-not (Test-Path -LiteralPath $shaderPath -PathType Leaf)) { throw 'Matching shader-patch source is missing' }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $OutDir 'runtime_replay.generated.h'), $source.Substring($start, $end-$start))
. (Join-Path $Repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
    & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/I' + (Join-Path $Repo 'features')) ('/I' + (Join-Path $Repo 'shaders')) ('/I' + $OutDir) `
        (Join-Path $PSScriptRoot 'runtime_replay_check.cpp') $shaderPath d3d9.lib d3dcompiler.lib user32.lib /Fe:receiver_replay_checks.exe
    if ($LASTEXITCODE -ne 0) { throw 'Receiver replay checks failed to compile' }
    & './receiver_replay_checks.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Receiver replay checks failed' }
} finally { Pop-Location }
