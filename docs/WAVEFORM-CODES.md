# Waveform studio and NHW1 codes

A sandbox for designing DualSense haptics by ear, and a compact code you can
hand over to have a waveform used for a particular event.

## Running it

```powershell
& .\Build-Tools.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\WaveformStudio.ps1
```

`Build-Tools.ps1` builds `tools\waveform_player.exe` (which talks to the
controller) and `tools\dump_image.exe`. The studio drives the player, so the
waveform you hear is real output on the actuators, not a simulation.

It shares the controller with the mod, so it works while the game is running.
If both fire at once the two simply mix.

The window has a preset picker, sliders for the overall shape, four layers, and
a code box. **Copy code** puts the current design on the clipboard; **Load code**
reads a code back into the sliders.

## The code format

```
NHW1;dur=0.130;gain=0.62;bal=0.30@23;L=90,0.55,55;L=430,0.17,11;L=880,0.11,24
```

| Field | Meaning |
| --- | --- |
| `NHW1` | format marker, required first |
| `dur` | total length in seconds (0.005 – 3.0) |
| `gain` | overall intensity, 0 – 1 |
| `bal` | stereo balance, -1 (left actuator) to 1 (right); `@hz` sweeps it |
| `L` | a layer: `frequency,amplitude,decay[,tremoloHz,tremoloDepth]` |

Each layer is

```
amplitude * exp(-decay * t) * sin(2*pi*frequency*t)
```

optionally multiplied by `(1 + depth * sin(2*pi*tremolo*t))`, and the layers are
summed then scaled by `gain`.

`decay` is the exponential falloff per second: 0 rings for the whole duration,
around 10–20 gives a ringing tail, 50+ is a sharp tap.

## Designing by feel

- **Low frequencies read as weight, high as detail.** Under about 80 Hz the
  actuators feel like a rumble; above roughly 300 Hz they feel like texture.
- **Shimmer comes from two close frequencies**, not from one bright tone. The
  melee hit uses 430 and 505 Hz together so they beat against each other.
- **Short beats loud.** Anything repeated often — footsteps especially — wants a
  low gain and a fast decay, or it masks everything else.
- **Balance separates events.** Footsteps swing hard left and right; hits sit
  centred or sweep.

## Current mod waveforms as codes

| Event | Code |
| --- | --- |
| Footstep | `NHW1;dur=0.032;gain=0.035;bal=-0.75;L=95,0.80,46;L=190,0.20,46` |
| Menu tick | `NHW1;dur=0.022;gain=0.20;bal=0;L=190,1.0,0` |
| Menu confirm | `NHW1;dur=0.045;gain=0.20;bal=0;L=150,0.70,0;L=300,0.30,0` |
| Menu cancel | `NHW1;dur=0.038;gain=0.20;bal=0;L=96,1.0,0` |
| Melee hit | `NHW1;dur=0.130;gain=0.62;bal=0.30@23;L=90,0.55,55;L=430,0.17,11;L=505,0.17,11;L=880,0.11,24` |
| Player hit | `NHW1;dur=0.220;gain=0.90;bal=0.25@9;L=68,0.55,0;L=47,0.45,0,173,1.0` |

The footstep balance is written as left; the mod mirrors it for the right foot.

## Handing a code over

Say which event it is for and paste the code, for example: "use
`NHW1;dur=0.09;gain=0.5;L=55,1.0,30` for the player hit". The events currently
available are footstep, menu tick, menu confirm, menu cancel, melee hit and
player hit.

The mod's waveforms are compiled in today, so a code becomes a small edit to
`synthesize()` in `src/haptics.cpp`. Wiring the INI to accept NHW1 strings
directly — so a code takes effect on relaunch with no rebuild — is a
straightforward follow-up and the obvious next step for this tool.
