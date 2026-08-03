<#
    Turns the Square Enix and PlatinumGames boot movies off or on.

    They are game data, not mod files, so this renames rather than deletes:
    logo_01.usm becomes logo_01.usm.disabled and back. The game boots straight
    to the title screen without them.

        .\Toggle-Logos.ps1            # skip the logos
        .\Toggle-Logos.ps1 -Restore   # put them back
#>
[CmdletBinding()]
param([switch]$Restore, [string]$GamePath)

$ErrorActionPreference = 'Stop'
if (-not $GamePath) {
    $steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    $roots = @(); if ($steam) { $roots += $steam }
    $vdf = if ($steam) { Join-Path $steam 'steamapps\libraryfolders.vdf' } else { $null }
    if ($vdf -and (Test-Path -LiteralPath $vdf)) {
        foreach ($line in Get-Content -LiteralPath $vdf) {
            if ($line -match '"path"\s+"([^"]+)"') { $roots += ($Matches[1] -replace '\\', '\') }
        }
    }
    foreach ($r in $roots) {
        $c = Join-Path $r 'steamapps\common\NieRAutomata'
        if (Test-Path -LiteralPath (Join-Path $c 'NieRAutomata.exe')) { $GamePath = $c; break }
    }
}
if (-not $GamePath -or -not (Test-Path -LiteralPath $GamePath)) {
    throw 'NieR:Automata was not found. Pass -GamePath with the game folder.'
}

$dir = Join-Path $GamePath 'data\movie_logo'
if (-not (Test-Path -LiteralPath $dir)) { throw "No movie_logo folder under $GamePath." }

$changed = 0
if ($Restore) {
    Get-ChildItem -LiteralPath $dir -Filter '*.usm.disabled' | ForEach-Object {
        $to = $_.Name -replace '\.disabled$', ''
        if (-not (Test-Path -LiteralPath (Join-Path $dir $to))) {
            Rename-Item -LiteralPath $_.FullName -NewName $to; $changed++
        }
    }
    Write-Host "Restored $changed logo movie(s)."
} else {
    Get-ChildItem -LiteralPath $dir -Filter '*.usm' | ForEach-Object {
        Rename-Item -LiteralPath $_.FullName -NewName "$($_.Name).disabled"; $changed++
    }
    Write-Host "Disabled $changed logo movie(s). The game now boots to the title screen."
}
