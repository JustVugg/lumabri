param()
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $visualStudio) { throw 'Native MSVC toolchain not found' }
    $setup = Join-Path $visualStudio 'VC\Auxiliary\Build\vcvars64.bat'
    New-Item -ItemType Directory -Path build\tests -Force | Out-Null
    $compile = 'call "' + $setup + '" >nul && pushd "' + $root + '" && cl /nologo /std:c11 /O2 /W4 /WX /I. tests\c\test_process_memory_windows.c /Fobuild\tests\process-memory.obj /Febuild\tests\process-memory.exe /link psapi.lib && popd'
    & $env:ComSpec /d /s /c $compile
    if ($LASTEXITCODE -ne 0) { throw 'Native process-memory build failed' }
    & .\build\tests\process-memory.exe
    if ($LASTEXITCODE -ne 0) { throw 'Native process-memory test failed' }
} finally {
    Pop-Location
}
