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
    $destination = Join-Path $Target $name
    # The settings file belongs to whoever is playing. Upgrades leave an
    # existing one alone; new keys fall back to their built-in defaults, so a
    # settings file from an older build stays valid.
    # The settings file belongs to whoever is playing, so an existing one is
    # never replaced. Keys the build has added since are appended, otherwise a
    # new option would be invisible in every install that already had a file.
    if ($name -eq 'NierHaptics.ini' -and (Test-Path -LiteralPath $destination)) {
        $existing = Get-Content -LiteralPath $destination
        $addition = @()
        foreach ($line in (Get-Content -LiteralPath (Join-Path $Stage $name))) {
            if ($line -match '^\s*([A-Za-z0-9_]+)\s*=') {
                $key = $Matches[1]
                if (-not ($existing -match ('^\s*' + [regex]::Escape($key) + '\s*='))) { $addition += $line }
            }
        }
        if ($addition.Count) {
            Add-Content -LiteralPath $destination -Value (@('') + $addition)
            Write-Host "Added $($addition.Count) new setting(s) to NierHaptics.ini"
        }
        continue
    }
    $source = Join-Path $Stage $name
    $temporary = Join-Path $Target ($name + '.nierhaptics-new')
    Copy-Item -LiteralPath $source -Destination $temporary -Force
    Move-Item -LiteralPath $temporary -Destination $destination -Force
}
@{ version='1.0.55'; dllHash=$sourceHash; backup=(Test-Path -LiteralPath $backup) } |
    ConvertTo-Json | Set-Content -LiteralPath $manifestPath -Encoding UTF8
if ($Stage -like (Join-Path $Target '.nierhaptics-stage-*')) {
    Remove-Item -LiteralPath $Stage -Recurse -Force
}
