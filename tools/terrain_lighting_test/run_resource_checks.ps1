param([string]$Repo = (Join-Path $PSScriptRoot '../..'), [string]$OutDir = (Join-Path $env:TEMP 'ApexLightingResourceTests'))
$ErrorActionPreference='Stop'
$Repo = (Resolve-Path -LiteralPath $Repo).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$sourcePath=Join-Path $Repo 'features/lot_light_bridge.cpp'
$source=Get-Content -LiteralPath $sourcePath -Raw
function Extract-Block([string]$pattern) {
    $m=[regex]::Match($source,$pattern)
    if(-not $m.Success) { throw ('Production block missing: '+$pattern) }
    return $m.Value
}
$sampler=Extract-Block '(?s)struct SamplerBind \{.*?\r?\n\};'
$gain=Extract-Block '(?s)struct ConstGain \{.*?\r?\n\};'
$weighted=Extract-Block '(?m)^float NightWeighted\(.*\r?\nfloat GroundGain\(.*\r?\nfloat RoadGain\(.*\r?\nfloat LotMapGain\(.*'
$wall=Extract-Block '(?s)template <typename DrawFn> bool DrawWallGain\(.*?\r?\n\}'
$object=Extract-Block '(?s)template <typename DrawFn> bool DrawObjectLamp\(.*?\r?\n\}'
$instanced=Extract-Block '(?s)template <typename DrawFn> bool DrawInstanced\(.*?\r?\n\}'
$core=$source.IndexOf('template <typename DrawFn> D3D9Hooks::HookAction OnDrawInnerCore(')
if($core -lt 0) { throw 'OnDrawInnerCore not found' }
$lotStart=$source.IndexOf('    EnsureReplacement(dev);',$core)
$lotEnd=$source.IndexOf('// Foliage vertex shaders are swapped',$lotStart)
if($lotStart -lt 0 -or $lotEnd -lt 0) { throw 'Lot branch boundaries missing' }
$lot=$source.Substring($lotStart,$lotEnd-$lotStart).TrimEnd()
if(-not $lot.StartsWith('    EnsureReplacement(dev);') -or -not $lot.EndsWith('}')) { throw 'Unexpected lot branch bounds' }
$prefix="template <typename DrawFn> D3D9Hooks::HookAction ExtractedLotBranch(IDirect3DDevice9* dev, DrawFn draw) {`n    constexpr auto kContinue = D3D9Hooks::HookAction::Continue;`n"
$extracted=$sampler+"`n"+$gain+"`n"+$weighted+"`n"+$wall+"`n"+$object+"`n"+$instanced+"`n"+$prefix+$lot+"`n"
$header=Join-Path $OutDir 'extracted_resource_paths.h'
Set-Content -LiteralPath $header -Value $extracted -Encoding UTF8
$sourceHash=(Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash
$extractedHash=(Get-FileHash -LiteralPath $header -Algorithm SHA256).Hash
Write-Output ('Production lot_light_bridge SHA256: '+$sourceHash)
Write-Output ('Extracted resource paths SHA256: '+$extractedHash)
. (Join-Path $PSScriptRoot 'compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
    & cl.exe /nologo /std:c++20 /EHsc /W4 /Od /RTC1 /MT ('/I'+$Repo) ('/I'+$OutDir) /Feresource_paths_audit.exe (Join-Path $PSScriptRoot 'resource_paths_fixture.cpp')
    if($LASTEXITCODE -ne 0) { throw 'Resource path fixture compilation failed' }
    & './resource_paths_audit.exe'
    if($LASTEXITCODE -ne 0) { throw 'Resource path checks failed' }
} finally { Pop-Location }
