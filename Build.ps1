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

$imgui = Join-Path $root 'third_party\imgui'
$imguiBackends = Join-Path $imgui 'backends'

$sources = @(
    (Join-Path $root 'src\dllmain.cpp'),
    (Join-Path $root 'src\config.cpp'),
    (Join-Path $root 'src\haptics.cpp'),
    (Join-Path $root 'src\sound_hook.cpp'),
    (Join-Path $root 'src\game_events.cpp'),
    (Join-Path $root 'src\chip_keeper.cpp'),
    (Join-Path $root 'src\easy_chips.cpp'),
    (Join-Path $root 'src\auto_chips.cpp'),
    (Join-Path $root 'src\quick_load.cpp'),
    (Join-Path $root 'src\iat.cpp'),
    (Join-Path $root 'src\save_probe.cpp'),
    (Join-Path $root 'src\overlay.cpp'),
    (Join-Path $root 'src\state_system.cpp'),
    (Join-Path $imgui 'imgui.cpp'),
    (Join-Path $imgui 'imgui_draw.cpp'),
    (Join-Path $imgui 'imgui_tables.cpp'),
    (Join-Path $imgui 'imgui_widgets.cpp'),
    (Join-Path $imguiBackends 'imgui_impl_dx11.cpp'),
    (Join-Path $imguiBackends 'imgui_impl_win32.cpp')
) | ForEach-Object { '"' + $_ + '"' }

$includes = '/I"{0}" /I"{1}"' -f $imgui, $imguiBackends
$output = Join-Path $dist 'dinput8.dll'
$def = Join-Path $root 'src\dinput8.def'
$command = '"{0}" -arch=amd64 -host_arch=amd64 >nul && cl /nologo /std:c++20 /O2 /EHsc /MD /LD /DUNICODE /D_UNICODE /DNOMINMAX {4} {1} /link /OUT:"{2}" /DEF:"{3}" Ole32.lib Psapi.lib Propsys.lib Setupapi.lib d3d11.lib dxgi.lib' -f `
    $developerShell, ($sources -join ' '), $output, $def, $includes
& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "Native build failed with exit code $LASTEXITCODE." }

Copy-Item -LiteralPath (Join-Path $root 'NierHaptics.ini') -Destination $dist -Force
Copy-Item -LiteralPath (Join-Path $root 'Uninstall-NierHaptics.ps1') -Destination $dist -Force
Write-Host "Built $output"
