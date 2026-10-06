param([string]$Repo = (Join-Path $PSScriptRoot '../..'), [string]$OutDir = (Join-Path $env:TEMP 'ApexTerrainQueueTests'))
$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path -LiteralPath $Repo).Path
$source = Get-Content -LiteralPath (Join-Path $Repo 'features/terrain_chunk_relight.cpp') -Raw
function Read-Block([string]$signature, [bool]$structure = $false) {
    $start = $source.IndexOf($signature, [StringComparison]::Ordinal)
    if ($start -lt 0) { throw "Missing production block: $signature" }
    $tail = $source.Substring($start)
    $pattern = if ($structure) { '(?m)^};\r?$' } else { '(?m)^}\r?$' }
    $ending = [regex]::Match($tail, $pattern)
    if (-not $ending.Success) { throw "Missing end of production block: $signature" }
    return $tail.Substring(0, $ending.Index + $ending.Length)
}
$types = @('struct Entry {', 'struct Batch {', 'struct View {', 'struct ChunkRaw {', 'struct Pending {') | ForEach-Object { Read-Block $_ $true }
$limits = @('kMaxPerSecond', 'kIdleTimeout', 'kHardTimeout') | ForEach-Object {
    $match = [regex]::Match($source, ('(?m)^constexpr [^\r\n]*\b' + $_ + '\s*=\s*[^\r\n]*'))
    if (-not $match.Success) { throw "Missing production limit: $_" }
    $match.Value
}
$functions = @('int Refuse(', 'void ClearQueue(', 'bool Queued(', 'bool Attach(', 'void FinishFlight(', 'void FlushDone(', 'void Fail(', 'size_t ReleaseLimit(', 'int QueueSweep(', 'void OnPresent(') | ForEach-Object { Read-Block $_ }
$fixture = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'queue_check_fixture.cpp') -Raw
$fixture = $fixture.Replace('/* PRODUCTION_LIMITS */', ($limits -join "`n"))
$fixture = $fixture.Replace('/* PRODUCTION_TYPES */', ($types -join "`n`n"))
$fixture = $fixture.Replace('/* PRODUCTION_FUNCTIONS */', ($functions -join "`n`n"))
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $OutDir 'queue_checks.generated.cpp'), $fixture)
. (Join-Path $PSScriptRoot 'compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
    & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/I' + $Repo) /Fe:terrain_queue_checks.exe queue_checks.generated.cpp
    if ($LASTEXITCODE -ne 0) { throw 'Queue checks failed to compile' }
    & './terrain_queue_checks.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Queue checks failed' }
    Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $Repo 'features/terrain_chunk_relight.cpp')
} finally { Pop-Location }
