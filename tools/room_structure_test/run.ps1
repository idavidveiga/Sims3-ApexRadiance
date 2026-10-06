param([string]$OutDir=(Join-Path $env:TEMP 'ApexRoomStructureChecks'))
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$source=Get-Content -LiteralPath (Join-Path $repo 'features/level_light_share.cpp') -Raw
$start=$source.IndexOf('void NoteRoomStructure(')
$end=$source.IndexOf('uintptr_t g_floorSetTarget', $start)
if($start -lt 0 -or $end -lt $start){throw 'Structure observer not found'}
Set-Content -LiteralPath (Join-Path $OutDir 'extracted_structure.h') -Value $source.Substring($start,$end-$start) -Encoding UTF8
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/I'+$repo) ('/I'+$OutDir) /Feroom_structure_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
 if($LASTEXITCODE -ne 0){throw 'Structure checks compilation failed'}
 & './room_structure_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'Structure checks failed'}
}finally{Pop-Location}
