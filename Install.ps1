[CmdletBinding()]
param([string]$GamePath)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$dist = Join-Path $root 'dist'
$payload = @('dinput8.dll', 'NierHaptics.ini', 'Uninstall-NierHaptics.ps1')
foreach ($file in $payload) {
    if (-not (Test-Path -LiteralPath (Join-Path $dist $file))) {
        throw "Missing $file. Run Build.ps1 first."
    }
}

function Normalize-GamePath([string]$Path) {
    if (-not $Path) { return $null }
    $clean = $Path.Trim().Trim('"')
    return [IO.Path]::GetFullPath($clean)
}

function Find-GamePath {
    $steamRoots = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if ($steam -and (Test-Path -LiteralPath $steam)) { [void]$steamRoots.Add((Normalize-GamePath $steam)) }
    foreach ($base in @($steamRoots)) {
        $libraries = Join-Path $base 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $libraries) {
            foreach ($line in Get-Content -LiteralPath $libraries) {
                if ($line -match '"path"\s+"([^"]+)"') {
                    $candidate = $Matches[1] -replace '\\\\', '\'
                    if (Test-Path -LiteralPath $candidate) { [void]$steamRoots.Add((Normalize-GamePath $candidate)) }
                }
            }
        }
    }
    $matches = @()
    foreach ($base in $steamRoots) {
        $candidate = Join-Path $base 'steamapps\common\NieRAutomata'
        if (Test-Path -LiteralPath (Join-Path $candidate 'NieRAutomata.exe')) { $matches += $candidate }
    }
    $matches = @($matches | Select-Object -Unique)
    if ($matches.Count -eq 1) { return $matches[0] }
    if ($matches.Count -eq 0) { throw 'NieR:Automata was not found. Pass -GamePath with the game folder.' }
    throw "Multiple NieR:Automata installs were found. Pass -GamePath with the intended folder: $($matches -join ', ')"
}

$target = if ($GamePath) { Normalize-GamePath $GamePath } else { Find-GamePath }
$exe = Join-Path $target 'NieRAutomata.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "The selected folder is not a NieR:Automata install: $target" }
$expectedHash = '5171BED09E6FEC7B21BF0EA479DBD2E1B228695C67D1F0B478549A9BE2F5726A'
$actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash
if ($actualHash -ne $expectedHash) {
    throw "Unsupported NieRAutomata.exe build ($actualHash). This release targets Steam build 7020666."
}

$writeProbe = Join-Path $target '.nierhaptics-write-test'
try { [IO.File]::WriteAllText($writeProbe, 'test') } finally { Remove-Item -LiteralPath $writeProbe -Force -ErrorAction SilentlyContinue }

$running = @(Get-Process -Name 'NieRAutomata','NieRAutomataCompat' -ErrorAction SilentlyContinue)
if ($running.Count) {
    $stage = Join-Path $target '.nierhaptics-stage-1.0.29'
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    foreach ($file in $payload) { Copy-Item -LiteralPath (Join-Path $dist $file) -Destination $stage -Force }
    Copy-Item -LiteralPath (Join-Path $root 'Watch-Deploy.ps1') -Destination $stage -Force
    Start-Process powershell.exe -WindowStyle Hidden -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $stage 'Watch-Deploy.ps1'),'-Stage',$stage,'-Target',$target)
    Write-Host "NieR:Automata is running. Deployment is staged and will complete automatically when it closes."
    return
}

& (Join-Path $root 'Watch-Deploy.ps1') -Stage $dist -Target $target -DeployNow
Write-Host "Installed NieR Haptics + Hitstop 1.0.29 to $target"
