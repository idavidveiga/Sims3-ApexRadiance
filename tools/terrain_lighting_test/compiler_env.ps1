function Initialize-TerrainCompiler {
    # Nested suites share the already initialized x86 toolchain. Repeated VsDevCmd
    # calls append PATH entries until cmd.exe's environment handling fails.
    $compiler = Get-Command cl.exe -ErrorAction SilentlyContinue
    if ($compiler -and $compiler.Source -match '[\\/]bin[\\/]Host(?:x64|x86)[\\/]x86[\\/]cl\.exe$' -and $env:INCLUDE -and $env:LIB) { return }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installation) { throw 'No installed x86 MSVC toolchain found' }
    $vsDevCmd = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
    $environmentLines = & cmd.exe /d /c ('call "' + $vsDevCmd + '" -no_logo -arch=x86 -host_arch=x64 >nul && set')
    if ($LASTEXITCODE -ne 0) { throw 'Compiler initialization failed' }
    foreach ($line in $environmentLines) {
        if ($line -match '^(PATH|INCLUDE|LIB|LIBPATH)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
    }
}
