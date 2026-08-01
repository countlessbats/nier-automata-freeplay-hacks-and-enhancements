# NieR:Automata DualSense Haptics + Hitstop

Native haptic feedback and configurable hitstop for the Steam version of
NieR:Automata (build 7020666).

## Features

- True DualSense waveform haptics over the controller's USB audio endpoint.
- Distinct pulses for menu movement, footsteps, outgoing hits, and damage taken.
- Alternating left/right footstep texture paced by movement-stick intensity.
- Hitstop whenever a health-bearing non-player entity loses health: 8% game speed for 1000 ms by
  default.
- Plain-text configuration in `NierHaptics.ini`.

## Requirements

- A wired Sony DualSense controller, or DSX's beta wireless-haptics virtual
  audio device.
- The Steam release of NieR:Automata, build 7020666.
- The Windows playback device named `DualSense Wireless Controller` must be
  enabled. It does not need to be the default audio device.

Steam Input may remain enabled. The haptic stream is sent to the controller (or
DSX virtual controller) audio endpoint and does not depend on XInput rumble.

## Install

Run `Install.ps1` from PowerShell. The installer finds Steam's registered game
folder, validates it, and installs the proxy DLL, configuration, and uninstaller.

For this source checkout, run `Build.ps1` first.

## Configuration

Edit `NierHaptics.ini` beside `NieRAutomata.exe`, then restart the game. Strength
values range from 0.0 to 1.0. Set any feature's `Enabled` value to 0 to disable it.

`HitstopDurationMs` is real-world time. The diagnostic default is currently 1000 ms even
though the game itself advances at only 8% speed during the effect. Menu pulses
are enabled before a save is loaded and after Start/Back opens a menu; pressing
Start/Back again or Cancel returns haptics to gameplay mode.

## Troubleshooting

`NierHaptics.log` beside the game executable reports the detected game hooks and
DualSense audio format. If haptics are unavailable, confirm the controller is
wired and that **Speakers (DualSense Wireless Controller)** is enabled under
Windows sound devices. When using DSX wireless haptics, keep its virtual
DualSense audio playback device enabled.

The mod is intentionally build-specific. If Steam updates the executable and the
entity signature no longer matches, gameplay continues normally and the log
reports that hit/footstep detection was disabled.

## Uninstall

Run `Uninstall-NierHaptics.ps1` from the game folder. It removes only files owned
by this mod and restores a pre-existing `dinput8.dll` if the installer backed one
up.

## Technical note

DualSense high-definition haptics use channels 3 and 4 of the controller's
four-channel USB audio device. This implementation synthesizes the actuator PCM
stream locally. Entity layout and signature work was cross-checked against the
MIT-licensed [AutomataMP SDK](https://github.com/praydog/AutomataMP). DualSense
USB/audio behavior was cross-checked against the public
[DualSense hardware notes](https://github.com/nondebug/dualsense) and the
[Linux hid-playstation driver](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c).
