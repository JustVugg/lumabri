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
    $flags = '/nologo /std:c11 /O2 /W4 /WX /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_WARNINGS /I.'
    $compile = 'call "' + $setup + '" >nul && pushd "' + $root + '" && cl ' + $flags + ' /c /FIsrc\runtime\lumabri_engine_memory_override.h tests\c\test_engine_memory_windows_hooks.c /Fobuild\tests\engine-hooks.obj && cl ' + $flags + ' tests\c\test_engine_memory_windows.c src\runtime\lumabri_engine_memory.c build\tests\engine-hooks.obj /Fobuild\tests\ /Febuild\tests\engine-memory.exe /link psapi.lib && popd'
    & $env:ComSpec /d /s /c $compile
    if ($LASTEXITCODE -ne 0) { throw 'Native engine-memory build failed' }
    foreach ($mode in @('ordinary', 'ownership', 'denied')) {
        & .\build\tests\engine-memory.exe $mode
        if ($LASTEXITCODE -ne 0) { throw "Native engine-memory test failed: $mode" }
    }
} finally {
    Pop-Location
}
