[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$target = Split-Path -Parent $MyInvocation.MyCommand.Path
$manifestPath = Join-Path $target 'NierHaptics.install.json'
if (-not (Test-Path -LiteralPath $manifestPath)) { throw 'The NieR Haptics install manifest is missing; nothing was removed.' }
$manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
$dll = Join-Path $target 'dinput8.dll'
if (Get-Process -Name 'NieRAutomata','NieRAutomataCompat' -ErrorAction SilentlyContinue) {
    throw 'Close NieR:Automata before uninstalling.'
}
if (Test-Path -LiteralPath $dll) {
    $currentHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $dll).Hash
    if ($currentHash -ne $manifest.dllHash) { throw 'dinput8.dll has changed since installation; nothing was removed.' }
    Remove-Item -LiteralPath $dll -Force
}
$backup = Join-Path $target 'dinput8.nierhaptics.backup.dll'
if ($manifest.backup -and (Test-Path -LiteralPath $backup)) { Move-Item -LiteralPath $backup -Destination $dll }
foreach ($name in @('NierHaptics.ini','NierHaptics.log','NierHaptics.install.json')) {
    Remove-Item -LiteralPath (Join-Path $target $name) -Force -ErrorAction SilentlyContinue
}
Write-Host 'NieR Haptics + Hitstop was removed. This uninstaller may now be deleted.'

# Boot logos, if they were disabled, are game data rather than mod files, so
# removal puts them back.
$logoDir = Join-Path $target 'data\movie_logo'
if (Test-Path -LiteralPath $logoDir) {
    Get-ChildItem -LiteralPath $logoDir -Filter '*.usm.disabled' -ErrorAction SilentlyContinue |
        ForEach-Object {
            $restored = $_.FullName -replace '\.disabled$', ''
            if (-not (Test-Path -LiteralPath $restored)) {
                Rename-Item -LiteralPath $_.FullName -NewName (Split-Path $restored -Leaf)
                Write-Host "Restored $(Split-Path $restored -Leaf)"
            }
        }
}
