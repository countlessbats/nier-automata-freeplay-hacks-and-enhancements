# WaveformStudio.ps1 - sliders for DualSense haptic waveforms.
#
# Drives tools\waveform_player.exe, which plays on the controller's actuators.
# The "code" box holds an NHW1 string: paste one in to load it, or copy one out
# and hand it over to have it used for a particular event.
[CmdletBinding()]
param([string]$PlayerPath)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $PlayerPath) { $PlayerPath = Join-Path $root 'waveform_player.exe' }
if (-not (Test-Path -LiteralPath $PlayerPath)) {
    [Windows.Forms.MessageBox]::Show(
        "waveform_player.exe was not found at`n$PlayerPath`n`nRun Build-Tools.ps1 first.",
        'Waveform Studio') | Out-Null
    return
}

# ---------------------------------------------------------------- player process
$psi = [Diagnostics.ProcessStartInfo]::new()
$psi.FileName = $PlayerPath
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$player = [Diagnostics.Process]::Start($psi)
$firstLine = $player.StandardOutput.ReadLine()
$status = if ($firstLine -like 'error*') { $firstLine } else { $firstLine }

function Send-Play([string]$code, [int]$repeats) {
    if ($player.HasExited) { return 'player exited' }
    $player.StandardInput.WriteLine("play $code $repeats")
    $player.StandardInput.Flush()
    return $player.StandardOutput.ReadLine()
}

# ---------------------------------------------------------------- presets
# These match the waveforms the mod currently uses, so they are a starting point
# rather than an invention.
$presets = [ordered]@{
    'Footstep (current)'   = 'NHW1;dur=0.032;gain=0.035;bal=-0.75;L=95,0.80,46;L=190,0.20,46'
    'Menu tick (current)'  = 'NHW1;dur=0.022;gain=0.20;bal=0;L=190,1.0,0'
    'Menu confirm'         = 'NHW1;dur=0.045;gain=0.20;bal=0;L=150,0.70,0;L=300,0.30,0'
    'Menu cancel'          = 'NHW1;dur=0.038;gain=0.20;bal=0;L=96,1.0,0'
    'Melee hit (current)'  = 'NHW1;dur=0.130;gain=0.62;bal=0.30@23;L=90,0.55,55;L=430,0.17,11;L=505,0.17,11;L=880,0.11,24'
    'Player hit (current)' = 'NHW1;dur=0.220;gain=0.90;bal=0.25@9;L=68,0.55,0;L=47,0.45,0,173,1.0'
    'Sharp tick'           = 'NHW1;dur=0.020;gain=0.5;bal=0;L=260,1.0,90'
    'Soft thump'           = 'NHW1;dur=0.090;gain=0.5;bal=0;L=55,1.0,30'
    'Metallic ring'        = 'NHW1;dur=0.250;gain=0.5;bal=0.4@17;L=420,0.5,9;L=497,0.5,9;L=1200,0.15,20'
    'Buzz'                 = 'NHW1;dur=0.200;gain=0.4;bal=0;L=120,1.0,0,35,0.8'
}

# ---------------------------------------------------------------- form
$form = [Windows.Forms.Form]::new()
$form.Text = 'DualSense Waveform Studio'
$form.Size = [Drawing.Size]::new(760, 690)
$form.StartPosition = 'CenterScreen'

function New-Label($text, $x, $y, $w = 90) {
    $l = [Windows.Forms.Label]::new()
    $l.Text = $text; $l.Location = [Drawing.Point]::new($x, $y)
    $l.Size = [Drawing.Size]::new($w, 18)
    $form.Controls.Add($l); return $l
}

# preset picker
New-Label 'Preset' 12 16 50 | Out-Null
$presetBox = [Windows.Forms.ComboBox]::new()
$presetBox.Location = [Drawing.Point]::new(66, 12)
$presetBox.Size = [Drawing.Size]::new(300, 24)
$presetBox.DropDownStyle = 'DropDownList'
$presetBox.Items.AddRange(@($presets.Keys))
$form.Controls.Add($presetBox)

# global controls: duration, gain, balance, balance sweep
$globals = @(
    @{ Name = 'dur';    Text = 'Duration ms'; Min = 5;    Max = 600; Default = 130 },
    @{ Name = 'gain';   Text = 'Intensity';   Min = 0;    Max = 100; Default = 62  },
    @{ Name = 'bal';    Text = 'Balance L/R'; Min = -100; Max = 100; Default = 30  },
    @{ Name = 'balhz';  Text = 'Bal sweep Hz'; Min = 0;   Max = 60;  Default = 23  }
)
$globalBars = @{}
$y = 52
foreach ($g in $globals) {
    New-Label $g.Text 12 ($y + 4) 90 | Out-Null
    $bar = [Windows.Forms.TrackBar]::new()
    $bar.Location = [Drawing.Point]::new(106, $y)
    $bar.Size = [Drawing.Size]::new(430, 34)
    $bar.Minimum = $g.Min; $bar.Maximum = $g.Max; $bar.Value = $g.Default
    $bar.TickFrequency = [Math]::Max(1, [int](($g.Max - $g.Min) / 20))
    $form.Controls.Add($bar)
    $val = New-Label "$($g.Default)" 548 ($y + 4) 60
    $globalBars[$g.Name] = @{ Bar = $bar; Label = $val }
    $y += 36
}

# four layers
$layerBars = @()
for ($i = 0; $i -lt 4; $i++) {
    $group = [Windows.Forms.GroupBox]::new()
    $group.Text = "Layer $($i + 1)"
    $group.Location = [Drawing.Point]::new(12, $y)
    $group.Size = [Drawing.Size]::new(620, 96)
    $form.Controls.Add($group)

    $enable = [Windows.Forms.CheckBox]::new()
    $enable.Text = 'on'
    $enable.Location = [Drawing.Point]::new(560, 0)
    $enable.Size = [Drawing.Size]::new(50, 20)
    $enable.Checked = ($i -lt 2)
    $group.Controls.Add($enable)

    $specs = @(
        @{ Name = 'freq';  Text = 'Hz';      Min = 20; Max = 1400; Default = if ($i -eq 0) { 90 } else { 430 } },
        @{ Name = 'amp';   Text = 'Amp';     Min = 0;  Max = 100;  Default = if ($i -eq 0) { 55 } else { 17 } },
        @{ Name = 'decay'; Text = 'Decay';   Min = 0;  Max = 120;  Default = if ($i -eq 0) { 55 } else { 11 } }
    )
    $entry = @{ Enable = $enable }
    $ly = 18
    foreach ($s in $specs) {
        $l = [Windows.Forms.Label]::new()
        $l.Text = $s.Text; $l.Location = [Drawing.Point]::new(8, ($ly + 4)); $l.Size = [Drawing.Size]::new(46, 18)
        $group.Controls.Add($l)
        $bar = [Windows.Forms.TrackBar]::new()
        $bar.Location = [Drawing.Point]::new(56, $ly)
        $bar.Size = [Drawing.Size]::new(440, 30)
        $bar.Minimum = $s.Min; $bar.Maximum = $s.Max; $bar.Value = $s.Default
        $bar.TickFrequency = [Math]::Max(1, [int](($s.Max - $s.Min) / 20))
        $group.Controls.Add($bar)
        $val = [Windows.Forms.Label]::new()
        $val.Text = "$($s.Default)"; $val.Location = [Drawing.Point]::new(504, ($ly + 4))
        $val.Size = [Drawing.Size]::new(60, 18)
        $group.Controls.Add($val)
        $entry[$s.Name] = @{ Bar = $bar; Label = $val }
        $ly += 26
    }
    $layerBars += $entry
    $y += 100
}

# code box and buttons
New-Label 'Code' 12 $y 40 | Out-Null
$codeBox = [Windows.Forms.TextBox]::new()
$codeBox.Location = [Drawing.Point]::new(56, ($y - 3))
$codeBox.Size = [Drawing.Size]::new(576, 24)
$form.Controls.Add($codeBox)
$y += 32

$playButton = [Windows.Forms.Button]::new()
$playButton.Text = 'Play'
$playButton.Location = [Drawing.Point]::new(56, $y)
$playButton.Size = [Drawing.Size]::new(90, 30)
$form.Controls.Add($playButton)

$repeatButton = [Windows.Forms.Button]::new()
$repeatButton.Text = 'Play x5'
$repeatButton.Location = [Drawing.Point]::new(152, $y)
$repeatButton.Size = [Drawing.Size]::new(90, 30)
$form.Controls.Add($repeatButton)

$loadButton = [Windows.Forms.Button]::new()
$loadButton.Text = 'Load code'
$loadButton.Location = [Drawing.Point]::new(248, $y)
$loadButton.Size = [Drawing.Size]::new(96, 30)
$form.Controls.Add($loadButton)

$copyButton = [Windows.Forms.Button]::new()
$copyButton.Text = 'Copy code'
$copyButton.Location = [Drawing.Point]::new(350, $y)
$copyButton.Size = [Drawing.Size]::new(96, 30)
$form.Controls.Add($copyButton)

$statusLabel = New-Label $status 56 ($y + 38) 600
$statusLabel.ForeColor = if ($status -like 'error*') { 'Firebrick' } else { 'DimGray' }

# ---------------------------------------------------------------- behaviour
function Get-Code {
    $dur   = [Math]::Round($globalBars['dur'].Bar.Value / 1000.0, 3)
    $gain  = [Math]::Round($globalBars['gain'].Bar.Value / 100.0, 3)
    $bal   = [Math]::Round($globalBars['bal'].Bar.Value / 100.0, 2)
    $balhz = $globalBars['balhz'].Bar.Value
    $balText = if ($balhz -gt 0) { "$bal@$balhz" } else { "$bal" }
    $parts = @("NHW1", "dur=$dur", "gain=$gain", "bal=$balText")
    foreach ($l in $layerBars) {
        if (-not $l.Enable.Checked) { continue }
        $f = $l['freq'].Bar.Value
        $a = [Math]::Round($l['amp'].Bar.Value / 100.0, 2)
        $d = $l['decay'].Bar.Value
        $parts += "L=$f,$a,$d"
    }
    return ($parts -join ';')
}

function Update-Labels {
    $globalBars['dur'].Label.Text   = "$($globalBars['dur'].Bar.Value) ms"
    $globalBars['gain'].Label.Text  = "$([Math]::Round($globalBars['gain'].Bar.Value / 100.0, 2))"
    $globalBars['bal'].Label.Text   = "$([Math]::Round($globalBars['bal'].Bar.Value / 100.0, 2))"
    $globalBars['balhz'].Label.Text = "$($globalBars['balhz'].Bar.Value) Hz"
    foreach ($l in $layerBars) {
        $l['freq'].Label.Text  = "$($l['freq'].Bar.Value) Hz"
        $l['amp'].Label.Text   = "$([Math]::Round($l['amp'].Bar.Value / 100.0, 2))"
        $l['decay'].Label.Text = "$($l['decay'].Bar.Value)"
    }
    $codeBox.Text = Get-Code
}

function Set-FromCode([string]$code) {
    $fields = $code.Split(';') | ForEach-Object { $_.Trim() } | Where-Object { $_ }
    $layerIndex = 0
    foreach ($l in $layerBars) { $l.Enable.Checked = $false }
    foreach ($f in $fields) {
        if ($f -match '^(?i)NHW1$') { continue }
        $kv = $f.Split('=', 2)
        if ($kv.Count -lt 2) { continue }
        $key = $kv[0].Trim().ToLower(); $value = $kv[1].Trim()
        switch ($key) {
            'dur'  { $globalBars['dur'].Bar.Value = [Math]::Min(600, [Math]::Max(5, [int]([double]$value * 1000))) }
            'gain' { $globalBars['gain'].Bar.Value = [Math]::Min(100, [Math]::Max(0, [int]([double]$value * 100))) }
            'bal'  {
                $b = $value.Split('@')
                $globalBars['bal'].Bar.Value = [Math]::Min(100, [Math]::Max(-100, [int]([double]$b[0] * 100)))
                $globalBars['balhz'].Bar.Value = if ($b.Count -gt 1) { [Math]::Min(60, [int][double]$b[1]) } else { 0 }
            }
            'l' {
                if ($layerIndex -ge $layerBars.Count) { break }
                $n = $value.Split(',')
                $entry = $layerBars[$layerIndex]
                $entry.Enable.Checked = $true
                $entry['freq'].Bar.Value  = [Math]::Min(1400, [Math]::Max(20, [int][double]$n[0]))
                $entry['amp'].Bar.Value   = [Math]::Min(100, [Math]::Max(0, [int]([double]$n[1] * 100)))
                $entry['decay'].Bar.Value = [Math]::Min(120, [Math]::Max(0, [int][double]$n[2]))
                $layerIndex++
            }
        }
    }
    Update-Labels
}

foreach ($g in $globalBars.Values) { $g.Bar.Add_ValueChanged({ Update-Labels }) }
foreach ($l in $layerBars) {
    $l.Enable.Add_CheckedChanged({ Update-Labels })
    foreach ($k in @('freq', 'amp', 'decay')) { $l[$k].Bar.Add_ValueChanged({ Update-Labels }) }
}

$presetBox.Add_SelectedIndexChanged({
    $name = $presetBox.SelectedItem
    if ($name -and $presets.Contains($name)) { Set-FromCode $presets[$name] }
})

$playButton.Add_Click({
    $codeBox.Text = Get-Code
    $statusLabel.Text = Send-Play $codeBox.Text 1
})
$repeatButton.Add_Click({
    $codeBox.Text = Get-Code
    $statusLabel.Text = Send-Play $codeBox.Text 5
})
$loadButton.Add_Click({
    try { Set-FromCode $codeBox.Text; $statusLabel.Text = 'code loaded' }
    catch { $statusLabel.Text = "could not read that code: $($_.Exception.Message)" }
})
$copyButton.Add_Click({
    $codeBox.Text = Get-Code
    Set-Clipboard -Value $codeBox.Text
    $statusLabel.Text = 'code copied to the clipboard'
})

$form.Add_FormClosed({
    try { $player.StandardInput.WriteLine('quit'); $player.StandardInput.Flush() } catch {}
    try { if (-not $player.WaitForExit(1500)) { $player.Kill() } } catch {}
})

$presetBox.SelectedIndex = 4   # melee hit, the one being tuned
Update-Labels
[void]$form.ShowDialog()
