param(
    [string]$OutDir = (Join-Path $env:TEMP 'ApexExteriorLookupChecks'),
    [Parameter(Mandatory=$true)][string]$ReferenceSource
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$source = Get-Content -LiteralPath (Join-Path $repo 'features/level_light_share.cpp') -Raw
$reference = Get-Content -LiteralPath $ReferenceSource -Raw
function Get-Lookup([string]$text) {
    $wall = $text.IndexOf('float WallPassImpl(')
    $start = $text.IndexOf('const uintptr_t mgrNext = StoryManager(tracker, floor + 1);', $wall)
    $end = $text.IndexOf('const uintptr_t wb =', $start)
    if ($wall -lt 0 -or $start -lt 0 -or $end -lt $start) { throw 'Exterior floor lookup not found' }
    return 'uintptr_t Lookup(uintptr_t tracker, int floor) { static uintptr_t s_nextMgr=0, s_nextLevel=0; ' +
        $text.Substring($start,$end-$start) + ' return mgrNext ? s_nextLevel : 0; }'
}
$cacheStart = $source.IndexOf('struct LevelEntry {')
$cacheEnd = $source.IndexOf('// World -> lot:', $cacheStart)
if ($cacheStart -lt 0 -or $cacheEnd -lt $cacheStart) { throw 'Production per-point cache not found' }
$extracted = 'namespace Production {' + $source.Substring($cacheStart,$cacheEnd-$cacheStart) +
    (Get-Lookup $source) + '} namespace Reference {' + (Get-Lookup $reference) + '}'
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
Set-Content -LiteralPath (Join-Path $OutDir 'extracted_lookup.h') -Value $extracted -Encoding UTF8
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
    & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/I'+$OutDir) /Feexterior_lookup_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'Exterior lookup checks compilation failed' }
    & './exterior_lookup_checks.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Exterior lookup checks failed' }
} finally { Pop-Location }
