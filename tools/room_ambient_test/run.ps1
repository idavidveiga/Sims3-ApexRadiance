param(
    [string]$OutDir = (Join-Path $env:TEMP 'ApexRoomAmbientTests'),
    [string]$VsDevCmd = '',
    [string]$CompareTag = ''
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (-not $VsDevCmd) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Build Tools are required' }
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installation) { throw 'No installed x86 MSVC toolchain found' }
    $VsDevCmd = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
}
if (-not (Test-Path -LiteralPath $VsDevCmd)) { throw 'VsDevCmd.bat not found' }
# Load only compiler search paths; do not print or persist the user's environment.
$environmentLines = & cmd.exe /d /c ('call "' + $VsDevCmd + '" -no_logo -arch=x86 -host_arch=x64 >nul && set')
if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the x86 compiler environment' }
foreach ($line in $environmentLines) {
    if ($line -match '^(PATH|INCLUDE|LIB|LIBPATH)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}
$compiler = (Get-Command cl.exe -ErrorAction Stop).Source
$include = Join-Path $repo 'vcpkg_installed/x86-windows-static/include'
if (-not (Test-Path -LiteralPath (Join-Path $include 'detours/detours.h'))) { throw 'Use the existing project vcpkg dependencies' }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
foreach ($test in @('room_ambient_test', 'unlit_rooms_recovery_test')) {
    $exe = Join-Path $OutDir ($test + '.exe')
    $object = Join-Path $OutDir ($test + '.obj')
    $source = Join-Path $PSScriptRoot ($test + '.cpp')
    $arguments = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/O2', '/DNOMINMAX',
        ('/I' + $repo), ('/I' + (Join-Path $repo 'framework')), ('/I' + (Join-Path $repo 'features')),
        ('/I' + $include), ('/Fo' + $object), ('/Fe' + $exe), $source)
    & $compiler @arguments
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $test" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "Checks failed: $test" }
}
if ($CompareTag) {
    $oldSource = Join-Path $OutDir 'unlit_rooms_reference.cpp'
    $gitOutput = & git -c ('safe.directory=' + $repo.Replace('\', '/')) show ($CompareTag + ':features/unlit_rooms.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'Could not read reference source from Git' }
    [IO.File]::WriteAllText($oldSource, ($gitOutput -join "`n"), [Text.UTF8Encoding]::new($false))
    $wrapper = Join-Path $OutDir 'room_reference_wrapper.cpp'
    $testSource = Join-Path $PSScriptRoot 'unlit_rooms_recovery_test.cpp'
    $wrapperText = '#define APEX_ROOM_REFERENCE_SOURCE "' + $oldSource.Replace('\', '/') + '"' + "`n" +
        '#include "' + $testSource.Replace('\', '/') + '"' + "`n"
    [IO.File]::WriteAllText($wrapper, $wrapperText, [Text.UTF8Encoding]::new($false))
    $referenceExe = Join-Path $OutDir 'room_reference.exe'
    $arguments = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/O2', '/DNOMINMAX',
        ('/I' + $repo), ('/I' + (Join-Path $repo 'framework')), ('/I' + (Join-Path $repo 'features')),
        ('/I' + $include), ('/Fo' + (Join-Path $OutDir 'room_reference.obj')), ('/Fe' + $referenceExe), $wrapper)
    & $compiler @arguments
    if ($LASTEXITCODE -ne 0) { throw 'Reference compilation failed' }
    Write-Output ('Reference brightness response: ' + $CompareTag)
    & $referenceExe --brightness-reference
    if ($LASTEXITCODE -ne 0) { throw 'Reference brightness checks failed' }
}
