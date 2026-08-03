# NieR:Automata — combined haptics and quality of life

A native `dinput8.dll` mod for the Steam release of NieR:Automata. It adds
DualSense haptics driven by the game's own sound events, an in-game settings
panel, unlimited air jumps, and keeps your plug-in chips when you die.

Built for Steam build 7020666. The installer refuses to touch any other
executable.

## What it does

**Haptics on the DualSense's actuators.** Not the rumble motors — the mod opens
the controller's four-channel audio endpoint and synthesises waveforms onto the
haptic channels. Every effect fires from an event the game itself raises:

- **Footsteps** follow the game's footstep sound events, so the timing, the
  cadence and which foot it was all come from the game. Only your own
  character's steps fire, never a companion's or a machine's. By default only
  sprinting and dashing are felt; walking stays quiet.
- **Menu feedback** follows the real menu sounds, so it only happens on an
  actual cursor move, confirm or cancel.
- **Landing a hit** fires on the hit-confirm sound of every weapon class — small
  sword, large sword, spear and combat bracer. Pod fire and pod impacts
  deliberately do not.
- **Taking damage** fires on your health actually dropping.

**In-game settings panel.** Press F10. It draws on the game's own renderer, takes
arrow keys and Enter as well as the mouse, and every change applies immediately.
It holds three switches and nothing else: haptics, unlimited jumps, and keeping
chips. Turning haptics off restores the game's own rumble motors, so the two
never run at once.

**Unlimited jumps.** Jump as many times as you like in the air.

**Keep plug-in chips on death.** Your corpse still spawns; it just no longer
takes your chips with it, and recovering it returns nothing to duplicate.

## Requirements

- The Steam release of NieR:Automata, build 7020666.
- A DualSense controller, either wired or through DSX's wireless-haptics virtual
  audio device. The Windows playback device named `DualSense Wireless
  Controller` must be enabled, though it need not be the default.

Steam Input can stay enabled; the haptic stream does not use XInput rumble.

## Installing

```powershell
& .\Build.ps1
& .\tests\smoke_loader.exe .\dist\dinput8.dll
& .\Install.ps1
```

`Install.ps1` finds the game through Steam's library folders, verifies the
executable, and backs up any existing `dinput8.dll`. If the game is running it
stages the build and deploys as soon as it exits.
`Uninstall-NierHaptics.ps1` removes only what the installer put there and
restores the backup.

Settings live in `NierHaptics.ini` beside the game. The mod watches that file and
reloads it while you play, so the panel and any hand edits take effect at once.
`NierHaptics.log` in the same folder records what was detected and hooked.

## Extra tools

- **`WaveformStudio.bat`** — design haptic waveforms by ear against the real
  controller and copy them out as a compact `NHW1` code. See
  [docs/WAVEFORM-CODES.md](docs/WAVEFORM-CODES.md).
- **`ControlPanel.bat`** — the same settings as an external window, for when you
  would rather not use the in-game panel.

## Known limitation

While the in-game panel is open, window messages, `GetAsyncKeyState`,
`GetKeyState` and XInput are all held back from the game, but **DirectInput is
not**, so keyboard input can still reach the game behind the panel. Patching
DirectInput's vtables to fix this crashed the game every time it was tried;
doing it properly needs a forwarding COM wrapper. Rebinding the panel's toggle
key with `ToggleKeyVirtualCode` avoids most of the annoyance meanwhile.

## How it was built

[docs/NIER-INTERNALS.md](docs/NIER-INTERNALS.md) is the reverse-engineering
reference: the SteamStub encryption and how to work around it, the entity and
player layouts, the Wwise sound API, the movement state machine, the renderer
hook, and a frank list of approaches that failed and why.

`tools/dump_image.cpp` captures the decrypted runtime image and
`analysis/nier.py` is a small toolkit for working with it. No game data ships in
this repository — those tools regenerate what they need from your own copy.

## Licence and credits

MIT; see [LICENSE](LICENSE). Vendors
[Dear ImGui](https://github.com/ocornut/imgui) (MIT) under `third_party/imgui`.

Entity and player structure offsets were cross-checked against
[AutomataMP](https://github.com/praydog/AutomataMP) by praydog (MIT), which was
invaluable for confirming a decrypted dump was correct. DualSense audio
behaviour was cross-checked against the public
[DualSense hardware notes](https://github.com/nondebug/dualsense) and the
[Linux hid-playstation driver](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c).

NieR:Automata is © Square Enix. This is an unofficial, non-commercial mod, not
affiliated with or endorsed by Square Enix or PlatinumGames, and it contains no
game assets or game code.
