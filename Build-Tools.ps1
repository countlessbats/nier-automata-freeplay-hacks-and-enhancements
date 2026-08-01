[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Build Tools were not found.' }
$installation = & $vswhere -latest -products * -property installationPath
if (-not $installation) { throw 'A C++ Visual Studio installation was not found.' }
$developerShell = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'

$targets = @(
    @{ Source = 'tools\waveform_player.cpp'; Output = 'tools\waveform_player.exe'
       Libraries = 'Ole32.lib Propsys.lib' },
    @{ Source = 'tools\dump_image.cpp';      Output = 'tools\dump_image.exe'
       Libraries = 'Psapi.lib' }
)

foreach ($target in $targets) {
    $source = Join-Path $root $target.Source
    $output = Join-Path $root $target.Output
    $command = '"{0}" -arch=amd64 -host_arch=amd64 >nul && cl /nologo /std:c++20 /O2 /EHsc /MT /DUNICODE /D_UNICODE /DNOMINMAX "{1}" /link /OUT:"{2}" {3}' -f `
        $developerShell, $source, $output, $target.Libraries
    & cmd.exe /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw "Build failed for $($target.Source) with exit code $LASTEXITCODE." }
    Write-Host "Built $output"
}

Get-ChildItem -LiteralPath $root -Filter '*.obj' -ErrorAction SilentlyContinue | Remove-Item -Force
Write-Host ''
Write-Host 'Start the studio with:'
Write-Host '  powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\WaveformStudio.ps1'
