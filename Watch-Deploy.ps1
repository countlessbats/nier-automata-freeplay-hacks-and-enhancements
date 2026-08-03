[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Stage,
    [Parameter(Mandatory)][string]$Target,
    [switch]$DeployNow
)

$ErrorActionPreference = 'Stop'
$Stage = [IO.Path]::GetFullPath($Stage.Trim().Trim('"'))
$Target = [IO.Path]::GetFullPath($Target.Trim().Trim('"'))
$exe = Join-Path $Target 'NieRAutomata.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw 'Deployment target is no longer a valid game folder.' }

if (-not $DeployNow) {
    while (Get-Process -Name 'NieRAutomata','NieRAutomataCompat' -ErrorAction SilentlyContinue) {
        Start-Sleep -Milliseconds 500
    }
}

$destinationDll = Join-Path $Target 'dinput8.dll'
for ($attempt = 0; $attempt -lt 40; $attempt++) {
    try {
        if (Test-Path -LiteralPath $destinationDll) {
            $stream = [IO.File]::Open($destinationDll, 'Open', 'ReadWrite', 'None')
            $stream.Dispose()
        }
        break
    } catch {
        if ($attempt -eq 39) { throw }
        Start-Sleep -Milliseconds 250
    }
}

$sourceDll = Join-Path $Stage 'dinput8.dll'
$sourceHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $sourceDll).Hash
$backup = Join-Path $Target 'dinput8.nierhaptics.backup.dll'
$manifestPath = Join-Path $Target 'NierHaptics.install.json'
if ((Test-Path -LiteralPath $destinationDll) -and -not (Test-Path -LiteralPath $manifestPath)) {
    if (Test-Path -LiteralPath $backup) { throw 'A dinput8.dll backup already exists; refusing to overwrite it.' }
    Move-Item -LiteralPath $destinationDll -Destination $backup
}

foreach ($name in @('dinput8.dll','NierHaptics.ini','Uninstall-NierHaptics.ps1')) {
    $source = Join-Path $Stage $name
    $temporary = Join-Path $Target ($name + '.nierhaptics-new')
    Copy-Item -LiteralPath $source -Destination $temporary -Force
    Move-Item -LiteralPath $temporary -Destination (Join-Path $Target $name) -Force
}
@{ version='1.0.24'; dllHash=$sourceHash; backup=(Test-Path -LiteralPath $backup) } |
    ConvertTo-Json | Set-Content -LiteralPath $manifestPath -Encoding UTF8
if ($Stage -like (Join-Path $Target '.nierhaptics-stage-*')) {
    Remove-Item -LiteralPath $Stage -Recurse -Force
}
