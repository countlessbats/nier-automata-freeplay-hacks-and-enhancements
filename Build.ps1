[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Build Tools were not found.'
}
$installation = & $vswhere -latest -products * -property installationPath
if (-not $installation) { throw 'A C++ Visual Studio installation was not found.' }
$developerShell = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null

$sources = @(
    'src\dllmain.cpp', 'src\config.cpp', 'src\timescale.cpp',
    'src\haptics.cpp', 'src\game_events.cpp'
) | ForEach-Object { '"' + (Join-Path $root $_) + '"' }
$output = Join-Path $dist 'dinput8.dll'
$def = Join-Path $root 'src\dinput8.def'
$command = '"{0}" -arch=amd64 -host_arch=amd64 >nul && cl /nologo /std:c++20 /O2 /EHsc /MD /LD /DUNICODE /D_UNICODE /DNOMINMAX {1} /link /OUT:"{2}" /DEF:"{3}" Ole32.lib Propsys.lib Setupapi.lib' -f $developerShell, ($sources -join ' '), $output, $def
& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "Native build failed with exit code $LASTEXITCODE." }

Copy-Item -LiteralPath (Join-Path $root 'NierHaptics.ini') -Destination $dist -Force
Copy-Item -LiteralPath (Join-Path $root 'Uninstall-NierHaptics.ps1') -Destination $dist -Force
Write-Host "Built $output"
