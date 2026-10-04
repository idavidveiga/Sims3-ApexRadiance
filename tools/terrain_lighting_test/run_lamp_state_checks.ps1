param([string]$Repo = (Join-Path $PSScriptRoot '../..'), [string]$OutDir = (Join-Path $env:TEMP 'ApexLampStateTests'), [string]$Baseline = '')
$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path -LiteralPath $Repo).Path
$source = Get-Content -LiteralPath (Join-Path $Repo 'patches/night_terrain_relight_patch.cpp') -Raw
function Read-Function([string]$text, [string]$signature) {
    $start = $text.IndexOf($signature, [StringComparison]::Ordinal)
    if ($start -lt 0) { throw "Missing function: $signature" }
    $tail = $text.Substring($start)
    $end = [regex]::Match($tail, '(?m)^}\r?$')
    if (-not $end.Success) { throw "Missing function end: $signature" }
    return $tail.Substring(0, $end.Index + $end.Length)
}
$functions = @('void NoteEdit(', 'void RefreshWorldRigs(', 'void FinishEdit(', 'bool EditReady(') | ForEach-Object { Read-Function $source $_ }
$fixture = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'lamp_state_fixture.cpp') -Raw
$fixture = $fixture.Replace('/* PRODUCTION_FUNCTIONS */', ($functions -join "`n`n"))
$oldFunction = ''
if ($Baseline) {
    $oldSource = Get-Content -LiteralPath $Baseline -Raw
    $oldFunction = (Read-Function $oldSource 'void FinishEdit(').Replace('void FinishEdit(', 'void OldFinishEdit(')
    $fixture = "#define HAVE_BASELINE 1`n" + $fixture
}
$fixture = $fixture.Replace('/* BASELINE_FUNCTION */', $oldFunction)
# A world reset intentionally discards the old world's request, rather than dirtying the new world.
$reset = [regex]::Match($source, '(?s)if \(s\.cells != g_lastCells\).*?g_editKickPending = false;\s*g_worldRigRefreshPending = false;')
if (-not $reset.Success) { throw 'World reset no longer discards both pending requests; update fixture coverage' }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $OutDir 'lamp_state.generated.cpp'), $fixture)
. (Join-Path $PSScriptRoot 'compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
    & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT /Fe:lamp_state_checks.exe lamp_state.generated.cpp
    if ($LASTEXITCODE -ne 0) { throw 'Lamp state checks failed to compile' }
    & './lamp_state_checks.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Lamp state checks failed' }
    Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $Repo 'patches/night_terrain_relight_patch.cpp')
} finally { Pop-Location }
