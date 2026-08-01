# NieR:Automata Haptics + Hitstop — Agent Handoff

## Current state

- Repository: `D:\Documents\NieRAutomata`
- Game install: `C:\Program Files (x86)\Steam\steamapps\common\NieRAutomata`
- Target executable: Steam build 7020666; installer validates SHA-256
  `5171BED09E6FEC7B21BF0EA479DBD2E1B228695C67D1F0B478549A9BE2F5726A`.
- Installed build: **1.0.4**, commit `86df7f1`.
- Installed DLL SHA-256:
  `2FCCAEB976893F4602C1E4150B0A815D7119FF62D6994F99016E43F65E3C765A`.
- Git has **no configured remote**, so nothing has been pushed. Every commit is
  local only. This must be reported whenever work is called complete.
- **1.0.4 has been built, smoke-loaded, installed, hash-verified, and launched
  in the real game to confirm every hook installs and captures live events. The
  user has not yet played it.** The next build shown to the user must be
  **1.0.5** with a matching commit.

The user runs a real DualSense wirelessly through **DSX beta wireless haptics**,
not a wired controller. DSX exposes a virtual four-channel 48-kHz DualSense
audio endpoint. The controller may turn itself off while work is underway; this
is expected and not an error.

## Read this first

`docs/NIER-INTERNALS.md` is the durable knowledge base: the SteamStub
encryption problem and how to work around it, the entity and player layouts, the
full sound-event API, the movement state machine, the timing situation, and
groundwork for a multi-jump mod. Read it before touching game internals.

The executable is **encrypted on disk**. Strings survive in `.rdata` and are
greppable, but no `.text` code signature exists in the shipped file. Do not
conclude a signature is wrong because an offline scan of the shipped executable
misses it. Use `tools/dump_image.exe` to capture the decrypted runtime image and
`analysis/nier.py` to work with it.

## What 1.0.4 changed

### Footsteps and menus now follow real sound events

`src/sound_hook.cpp` locates the game's Wwise post-event family and places
breakpoints on all four descriptor-taking variants. Every posted sound yields
its name and id, which `game_events.cpp` classifies:

- names containing `foot`, or `step` without `stepup`, are footsteps;
- `core_*` / `se_*` names containing `cursor`, `disicion`, `decide`, `cancel`,
  `error` and similar are menu actions.

This replaces the movement-distance footstep heuristic and the XInput menu-state
inference, both of which are gone. Menu pulses can no longer fire before a menu
exists, and footstep cadence is the game's own.

`FootstepRequireMoving` gates footstep sounds against the player's real
controller speed (behavior + `0x1434`) so other characters' footsteps do not
buzz while the player stands still. The trigger is still the sound; this is only
a filter.

`LogSoundNames=1` records each distinct event name once, so unmapped events can
be identified from an ordinary play session with no timed test.

### Hitstop

1.0.3 scaled only `QueryPerformanceCounter`. The game also reads `timeGetTime`
(17 sites) and `GetTickCount` (3 sites), so the frame limiter waited on real
time while the simulation used the slowed clock — the frame rate collapsed and
nothing slowed down, exactly as reported. All three clocks now come from one
virtual timeline.

Repeated hits also used to extend the stop indefinitely via `max()`. With hits
landing roughly ten times a second against a one-second stop, the game never
came out of it. `HitstopMinIntervalMs` now enforces a refractory period and the
end time is assigned rather than extended.

Diagnostic values (1000 ms at 8%) are replaced with 130 ms at 35%.

### Haptic waveforms

Footsteps are a short exponentially damped tap at 95 Hz instead of a 50 ms
low-frequency thud, with default strength lowered from 0.08 to 0.035. Menus have
three distinct effects — tick, confirm, cancel — instead of a left/right pair.

## Verified in the live game

The 1.0.4 launch produced:

```text
Hitstop: virtual clock installed over 3 game time sources
Sound hook: post-event entry at NieRAutomata.exe+0x140200
Sound hook: watching 4 post-event entries
Outgoing hits: armed 5 health-subtraction instructions
Sound: core_title_wsh (id 0x073E9868) [-]
Sound: se_movie_in (id 0xFB50D662) [-]
```

The resolved post-event address matched the offline analysis exactly, and all
six captured event ids reproduce from the documented FNV-1 hash of their names.

## Not yet confirmed by play

1. Whether footstep sound names actually contain `foot` or `step`. The names
   live in the packed CPK data, not the executable, so they could not be read
   offline. If no footsteps fire, read the `Sound:` lines in the log from a
   session with walking and extend `classify()` in `game_events.cpp`; the log
   now makes this a single quick pass with no timed test.
2. Whether full clock virtualization produces visible slow motion. If the frame
   rate still suffers, the next lead is the engine's own delta time rather than
   a larger clock scale — do not simply lengthen the duration.
3. Whether `behavior + 0x1434` is really the controller speed. The log prints
   one sample (`player controller speed reads …`) the first time it exceeds 0.5.
   If that value looks wrong, set `FootstepRequireMoving=0` to bypass the gate.
4. Whether menu classification covers nested menus and the pause screen.

## Known risks

`damage_hook.cpp` still uses INT3 breakpoints with a vectored handler on five
health-subtraction instructions, leaving those pages RWX. `sound_hook.cpp` uses
the same technique with its own handler; the two coexist because each ignores
addresses it does not own and each tracks its single-step state in thread-local
storage. Both are proven to work but neither is production-grade. A rewrite onto
a real mid-function hook library such as SafetyHook is the eventual fix.

The sound-hook breakpoints sit on hot functions. Every posted sound costs two
exceptions. Observed rates are low enough not to matter, but a scene that posts
sounds far more aggressively is the thing to watch if frame pacing regresses.

## Architecture

Native x64 `dinput8.dll` proxy loaded from the game directory.

- `src/dllmain.cpp` — proxy forwarding and the mod worker thread.
- `src/haptics.cpp` — WASAPI PCM stream to DualSense actuator channels 3/4.
- `src/sound_hook.cpp` — Wwise post-event observation (new in 1.0.4).
- `src/game_events.cpp` — entity/player polling, sound classification, dispatch.
- `src/damage_hook.cpp` — outgoing-damage breakpoints.
- `src/timescale.cpp` — virtual clock across every imported time source.
- `src/config.cpp` / `.hpp` — INI configuration.
- `tools/dump_image.cpp` — external decrypted-image dumper.
- `analysis/nier.py` — dump analysis toolkit.
- `docs/NIER-INTERNALS.md` — game internals reference.
- `Build.ps1`, `Install.ps1`, `Watch-Deploy.ps1`, `tests/smoke_loader.cpp`.

Runtime log and install manifest live in the game directory as
`NierHaptics.log` and `NierHaptics.install.json`.

## Build and deployment

```powershell
& .\Build.ps1
& .\tests\smoke_loader.exe .\dist\dinput8.dll
& .\Install.ps1
```

`Install.ps1` stages the build and starts a hidden watcher if the game is
running, deploying as soon as it exits. Note that deployment **overwrites**
`NierHaptics.ini`, so ship intended defaults in the repository copy.

## Requested behavior, for reference

1. Advanced waveform haptics for menu navigation only while actually navigating
   menus; left/right footsteps with real walk/sprint cadence; hitting an enemy;
   being hit.
2. Hitstop whenever a hit connects.

The user dislikes approximations: haptics should be tied to real game events
rather than inferred from movement or stick position.

## Standing constraints

- Every build presented to the user increments Z and is committed at the same
  time. No remote exists, so the inability to push must be reported explicitly.
- Never put the user on a countdown or require them to act on the agent's clock.
- Do not spawn additional agents without explicit approval.
- Read `C:\DeepThought\shared-instructions.md` before further work.
