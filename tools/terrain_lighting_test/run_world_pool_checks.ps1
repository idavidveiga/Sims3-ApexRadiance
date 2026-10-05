param([string]$Repo = (Join-Path $PSScriptRoot '../..'), [string]$OutDir = (Join-Path $env:TEMP 'ApexWorldLampTests'), [string]$Baseline = '')
$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path -LiteralPath $Repo).Path
$source = Get-Content -LiteralPath (Join-Path $Repo 'features/lot_light_bridge.cpp') -Raw
function Read-Function([string]$text, [string]$signature) {
    $start = $text.IndexOf($signature, [StringComparison]::Ordinal)
    if ($start -lt 0) { throw "Missing function: $signature" }
    $tail = $text.Substring($start)
    $firstEnd = $tail.IndexOf("`n", [StringComparison]::Ordinal)
    if ($firstEnd -lt 0) { $firstEnd = $tail.Length }
    $first = $tail.Substring(0, $firstEnd).TrimEnd("`r")
    if ($first.EndsWith('}')) { return $first }
    $end = [regex]::Match($tail, '(?m)^}\r?$')
    if (-not $end.Success) { throw "Missing function end: $signature" }
    return $tail.Substring(0, $end.Index + $end.Length)
}
function Read-Struct([string]$text, [string]$signature) {
    $start = $text.IndexOf($signature, [StringComparison]::Ordinal)
    if ($start -lt 0) { throw "Missing struct: $signature" }
    $tail = $text.Substring($start)
    $end = [regex]::Match($tail, '(?m)^};\r?$')
    if (-not $end.Success) { throw "Missing struct end: $signature" }
    return $tail.Substring(0, $end.Index + $end.Length)
}
$structs = @('struct LotLampState {', 'struct LotSeen {', 'struct LampMemo {') | ForEach-Object { Read-Struct $source $_ }
$helpers = @('bool IsPlainType(', 'bool ObservedEnableSwitch(', 'float LampLight(', 'bool InBake(', 'bool MovedApart(', 'bool LightDiffers(', 'bool LightChanged(', 'bool RawChanged(', 'std::string LampChangeText(', 'void AddDetail(') | ForEach-Object { Read-Function $source $_ }
$functions = @('void OnWorldChanged(', 'uint32_t FloatBits(', 'int SelectLamps(', 'int SelectLampsScan(float x, float z, float maxScore) {', 'void ReadEnumeratedLamps(', 'void TrackLotLampEdits(') | ForEach-Object { Read-Function $source $_ }
$present = Read-Function $source 'void OnPresent('
$refreshStart = $present.IndexOf('    const bool editReady =', [StringComparison]::Ordinal)
if ($refreshStart -lt 0) { throw 'Missing production lamp refresh fragment' }
$refresh = "void RefreshPresentFixture() {`n" + $present.Substring($refreshStart)
$fixture = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'world_pool_fixture.cpp') -Raw
$fixture = $fixture.Replace('/* PRODUCTION_STRUCTS */', ($structs -join "`n`n"))
$fixture = $fixture.Replace('/* PRODUCTION_HELPERS */', ($helpers -join "`n`n"))
$fixture = $fixture.Replace('/* PRODUCTION_FUNCTIONS */', ($functions -join "`n`n"))
$fixture = $fixture.Replace('/* PRODUCTION_REFRESH */', $refresh)
$oldFunction = ''
if ($Baseline) {
    $oldSource = Get-Content -LiteralPath $Baseline -Raw
    $oldFunction = (Read-Function $oldSource 'void OnWorldChanged(').Replace('void OnWorldChanged(', 'void OldOnWorldChanged(')
    $fixture = "#define HAVE_BASELINE 1`n" + $fixture
}
$fixture = $fixture.Replace('/* BASELINE_FUNCTION */', $oldFunction)
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $OutDir 'world_lamp.generated.cpp'), $fixture)
. (Join-Path $PSScriptRoot 'compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
    & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT "/I$Repo" /Fe:world_lamp_checks.exe world_lamp.generated.cpp
    if ($LASTEXITCODE -ne 0) { throw 'World lamp checks failed to compile' }
    & './world_lamp_checks.exe'
    if ($LASTEXITCODE -ne 0) { throw 'World lamp checks failed' }
    Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $Repo 'features/lot_light_bridge.cpp')
} finally { Pop-Location }
