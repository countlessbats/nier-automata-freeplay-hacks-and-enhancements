# ControlPanel.ps1 - live control over the mod's settings.
#
# Writes NierHaptics.ini in the game folder; the mod notices the file changing
# and reloads within a few milliseconds, so changes apply while you play.
# F10 shows and hides this window from anywhere, including while the game has
# focus. The game must be windowed or borderless for an overlay window to be
# visible over it.
[CmdletBinding()]
param([string]$IniPath)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace Native -Name Keys -MemberDefinition @'
[DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vKey);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
'@

if (-not $IniPath) {
    $steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    $roots = @()
    if ($steam) { $roots += $steam }
    $libraries = if ($steam) { Join-Path $steam 'steamapps\libraryfolders.vdf' } else { $null }
    if ($libraries -and (Test-Path -LiteralPath $libraries)) {
        foreach ($line in Get-Content -LiteralPath $libraries) {
            if ($line -match '"path"\s+"([^"]+)"') { $roots += ($Matches[1] -replace '\\\\', '\') }
        }
    }
    foreach ($root in $roots) {
        $candidate = Join-Path $root 'steamapps\common\NieRAutomata\NierHaptics.ini'
        if (Test-Path -LiteralPath $candidate) { $IniPath = $candidate; break }
    }
}
if (-not $IniPath -or -not (Test-Path -LiteralPath $IniPath)) {
    [Windows.Forms.MessageBox]::Show(
        "NierHaptics.ini was not found. Install the mod first, or pass -IniPath.",
        'NieR Haptics Control Panel') | Out-Null
    return
}

# ------------------------------------------------------------------ ini access
function Get-IniValue([string]$key, [string]$fallback) {
    foreach ($line in Get-Content -LiteralPath $IniPath) {
        if ($line -match "^\s*$([regex]::Escape($key))\s*=\s*(.*?)\s*$") { return $Matches[1] }
    }
    return $fallback
}

function Set-IniValues([hashtable]$values) {
    $lines = Get-Content -LiteralPath $IniPath
    $remaining = @{}
    foreach ($k in $values.Keys) { $remaining[$k] = $true }
    for ($i = 0; $i -lt $lines.Count; $i++) {
        foreach ($k in @($values.Keys)) {
            if ($lines[$i] -match "^\s*$([regex]::Escape($k))\s*=") {
                $lines[$i] = "$k=$($values[$k])"
                $remaining.Remove($k)
            }
        }
    }
    foreach ($k in $remaining.Keys) { $lines += "$k=$($values[$k])" }
    Set-Content -LiteralPath $IniPath -Value $lines -Encoding UTF8
}

# ------------------------------------------------------------------ definitions
$toggles = @(
    @{ Key = 'HapticsEnabled';       Text = 'Haptics on' },
    @{ Key = 'FootstepsEnabled';     Text = 'Footsteps' },
    @{ Key = 'FootstepsInCombat';    Text = 'Footsteps during combat' },
    @{ Key = 'FootstepPlayerOnly';   Text = 'Only your own footsteps' },
    @{ Key = 'MenuEnabled';          Text = 'Menu haptics' },
    @{ Key = 'EnemyHitEnabled';      Text = 'Landing a hit' },
    @{ Key = 'PlayerHitEnabled';     Text = 'Taking a hit' },
    @{ Key = 'MultiJumpEnabled';     Text = 'Unlimited jumps' }
)
$sliders = @(
    @{ Key = 'FootstepStrength';  Text = 'Footstep intensity'; Scale = 100 },
    @{ Key = 'MenuStrength';      Text = 'Menu intensity';     Scale = 100 },
    @{ Key = 'EnemyHitStrength';  Text = 'Hit intensity';      Scale = 100 },
    @{ Key = 'PlayerHitStrength'; Text = 'Damage intensity';   Scale = 100 },
    @{ Key = 'CombatWindowMs';    Text = 'Combat lasts (ms)';  Scale = 1; Max = 15000 }
)

# ------------------------------------------------------------------ form
$form = [Windows.Forms.Form]::new()
$form.Text = 'NieR Haptics - Control Panel  (F10 to hide)'
$form.Size = [Drawing.Size]::new(470, 610)
$form.StartPosition = 'Manual'
$form.Location = [Drawing.Point]::new(40, 40)
$form.TopMost = $true
$form.FormBorderStyle = 'FixedToolWindow'

$y = 12
$boxes = @{}
foreach ($t in $toggles) {
    $cb = [Windows.Forms.CheckBox]::new()
    $cb.Text = $t.Text
    $cb.Location = [Drawing.Point]::new(16, $y)
    $cb.Size = [Drawing.Size]::new(420, 22)
    $cb.Checked = (Get-IniValue $t.Key '1') -eq '1'
    $form.Controls.Add($cb)
    $boxes[$t.Key] = $cb
    $y += 26
}

$y += 8
$bars = @{}
foreach ($s in $sliders) {
    $label = [Windows.Forms.Label]::new()
    $label.Location = [Drawing.Point]::new(16, $y)
    $label.Size = [Drawing.Size]::new(420, 18)
    $form.Controls.Add($label)
    $bar = [Windows.Forms.TrackBar]::new()
    $bar.Location = [Drawing.Point]::new(12, ($y + 18))
    $bar.Size = [Drawing.Size]::new(430, 34)
    $bar.Minimum = 0
    $bar.Maximum = if ($s.ContainsKey('Max')) { $s.Max } else { 100 }
    $bar.TickFrequency = [int]($bar.Maximum / 20)
    $raw = [double](Get-IniValue $s.Key '0')
    $value = [int][Math]::Round($raw * $s.Scale)
    $bar.Value = [Math]::Min($bar.Maximum, [Math]::Max(0, $value))
    $form.Controls.Add($bar)
    $bars[$s.Key] = @{ Bar = $bar; Label = $label; Spec = $s }
    $y += 58
}

$status = [Windows.Forms.Label]::new()
$status.Location = [Drawing.Point]::new(16, $y)
$status.Size = [Drawing.Size]::new(430, 40)
$status.ForeColor = 'DimGray'
$form.Controls.Add($status)

function Update-Labels {
    foreach ($k in $bars.Keys) {
        $entry = $bars[$k]
        $shown = if ($entry.Spec.Scale -eq 1) { "$($entry.Bar.Value)" }
                 else { [Math]::Round($entry.Bar.Value / $entry.Spec.Scale, 2) }
        $entry.Label.Text = "$($entry.Spec.Text): $shown"
    }
}

function Save-Settings {
    $values = @{}
    foreach ($k in $boxes.Keys) { $values[$k] = if ($boxes[$k].Checked) { '1' } else { '0' } }
    foreach ($k in $bars.Keys) {
        $entry = $bars[$k]
        $values[$k] = if ($entry.Spec.Scale -eq 1) { "$($entry.Bar.Value)" }
                      else { [Math]::Round($entry.Bar.Value / $entry.Spec.Scale, 3) }
    }
    Set-IniValues $values
    $status.Text = "Saved at $(Get-Date -Format HH:mm:ss) - the mod reloads on its own."
}

foreach ($k in $boxes.Keys) { $boxes[$k].Add_CheckedChanged({ Save-Settings }) }
foreach ($k in $bars.Keys) {
    $bars[$k].Bar.Add_ValueChanged({ Update-Labels })
    $bars[$k].Bar.Add_MouseUp({ Save-Settings })
    $bars[$k].Bar.Add_KeyUp({ Save-Settings })
}

# F10 anywhere toggles the window. Polling GetAsyncKeyState keeps this working
# while the game has focus, which a form-level key handler would not.
$hotkey = [Windows.Forms.Timer]::new()
$hotkey.Interval = 120
$script:wasDown = $false
$hotkey.Add_Tick({
    $down = ([Native.Keys]::GetAsyncKeyState(0x79) -band 0x8000) -ne 0   # VK_F10
    if ($down -and -not $script:wasDown) {
        if ($form.Visible) { $form.Hide() }
        else {
            $form.Show(); $form.TopMost = $true
            [Native.Keys]::SetForegroundWindow($form.Handle) | Out-Null
        }
    }
    $script:wasDown = $down
})
$hotkey.Start()

Update-Labels
$status.Text = "Editing $IniPath"
$form.Add_FormClosing({
    param($sender, $e)
    # Closing would lose the hotkey, so hide instead.
    if ($e.CloseReason -eq 'UserClosing') { $e.Cancel = $true; $form.Hide() }
})
[void][Windows.Forms.Application]::Run($form)
