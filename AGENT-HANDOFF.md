# NieR:Automata Haptics + Hitstop — Agent Handoff

## Current state

- Repository: `D:\Documents\NieRAutomata`
- Game install: `C:\Program Files (x86)\Steam\steamapps\common\NieRAutomata`
- Target executable: Steam build 7020666; installer validates SHA-256
  `5171BED09E6FEC7B21BF0EA479DBD2E1B228695C67D1F0B478549A9BE2F5726A`.
- Installed build: **1.0.3**, commit `61cc706`.
- Installed DLL SHA-256:
  `F01D99A1786883BB71F4B3B96B0DD6CFBE075CE5D4BC4B2CA87AE0026A9B59C3`.
- Git has no configured remote. All commits are local.
- Working tree was clean immediately before this handoff file was created.
- **1.0.3 has been built, smoke-loaded, installed, and hash-verified, but has not
  yet been tested by the user in the game.** The next build shown to the user
  must be **1.0.4** and committed at the same time, per the machine's X.Y.Z
  versioning rules.

The user runs a real DualSense wirelessly through **DSX beta wireless haptics**,
not a physically wired controller. DSX exposes a virtual four-channel,
48-kHz DualSense audio endpoint. The controller may turn itself off while work
is underway; this is expected and not an error.

## Requested behavior

1. Advanced waveform haptics for:
   - menu navigation only while actually navigating menus;
   - left/right footsteps tied to real movement, with walk/sprint cadence;
   - hitting an enemy;
   - being hit.
2. Hitstop whenever a hit connects. It is intentionally set to an exaggerated
   diagnostic value of one real-world second at 8% game speed until operation is
   visually confirmed.

## What the user has actually observed

### 1.0.0 and 1.0.1

- D-pad, X, and O produced genuine waveform pulses on every press, including
  gameplay. They were not restricted to menus.
- No footstep feedback.
- No outgoing enemy-hit feedback.
- No visible hitstop.

The 1.0.1 log proved that the revised entity-list layout could acquire the
player and read player HP, but its menu inference used `behavior+0x9C` as if it
were an advancing gameplay clock. It was effectively static and caused idle
gameplay to be classified as a menu. Do not restore that mechanism.

### 1.0.2

- Footsteps existed, but were too strong and were driven only by left-stick
  magnitude. Holding the stick during a cutscene produced fake footsteps.
- Hitting an enemy still produced no haptic feedback.
- Getting hit produced a rumble. The user was unsure whether it came from the
  mod or the game.
- The log conclusively shows our incoming-hit detector firing at those times:

```text
[00:28:20.058] Event: player damaged (1439 -> 1332)
[00:28:20.139] Event: hit connected; applying hitstop
[00:28:21.614] Event: player damaged (1332 -> 1225)
[00:28:21.693] Event: hit connected; applying hitstop
[00:28:23.125] Event: player damaged (1225 -> 1118)
[00:28:23.210] Event: hit connected; applying hitstop
```

Those incoming pulses were ours: the `PlayerHit` waveform is queued on the same
health decrease. Whether the QPC-based hitstop was visually effective was not
confirmed.

The same run acquired the player at a valid nonzero location and reported three
other health-bearing entities, but it never logged a non-player health decrease.
Thus entity polling did not identify the enemies the user struck.

No user report has yet confirmed whether 1.0.2's conservative menu gating fixed
the always-pulsing D-pad/X/O issue. Do not assume silence means confirmation.

## Release history

```text
61cc706  Tie footsteps to movement and hook outgoing damage v1.0.3
b5aff0e  Fix gameplay haptic and hitstop detection v1.0.2
c426a12  Fix live entity detection and menu haptic gating v1.0.1
ef6239e  Add DualSense haptics and hitstop mod v1.0.0
```

## Architecture

The mod is a native x64 `dinput8.dll` proxy loaded from the game directory.

- `src/dllmain.cpp`: proxy forwarding and mod worker thread.
- `src/haptics.cpp`: WASAPI PCM stream to DualSense actuator channels 3/4.
- `src/game_events.cpp`: entity/player polling, input/menu state, footsteps,
  incoming/outgoing event dispatch.
- `src/damage_hook.cpp`: new in 1.0.3; runtime outgoing-damage hook.
- `src/timescale.cpp`: imports/IAT hook for `QueryPerformanceCounter` and virtual
  clock scaling.
- `src/config.cpp` / `src/config.hpp`: INI configuration.
- `Build.ps1`: MSVC native build into `dist`.
- `Install.ps1`, `Watch-Deploy.ps1`: validated install and deferred deployment.
- `tests/smoke_loader.cpp`: verifies DLL loading and proxy exports.
- `tests/entity_probe.cpp`: external entity-list probe; useful only while the
  game process exists.

Runtime log:
`C:\Program Files (x86)\Steam\steamapps\common\NieRAutomata\NierHaptics.log`

Install manifest:
`C:\Program Files (x86)\Steam\steamapps\common\NieRAutomata\NierHaptics.install.json`

## Entity knowledge established so far

The current Steam build is packed on disk. Useful signatures are absent from the
file and appear only in the unpacked runtime image. Do not reject a signature
solely because an offline scan of `NieRAutomata.exe` finds nothing.

Working runtime entity-list signature:

```text
44 8B 0D ?? ?? ?? ?? 44 39 0D ?? ?? ?? ?? 75 24
```

It resolves to the direct `CEntityList` object, not a pointer-to-pointer:

- count: list object `+0x04`
- pair array: list object `+0x10`
- pair stride: 16 bytes
- `CEntityInfo*`: pair `+0x08`
- entity name: `CEntityInfo+0x08`, up to 32 bytes
- behavior pointer: `CEntityInfo+0x48`
- behavior position: `+0x50`
- current HP: `+0x858`
- presumed maximum HP: `+0x85C`

The player name is exactly `Player`. Player HP and position reads have been
validated by logs. Enemy filtering originally required names beginning with
`Em`; that was removed because it could exclude the actual combat entities.
Current polling accepts sane current/max-health pairs on any non-player entity
and keeps polling as a fallback.

## 1.0.3 changes awaiting user test

### Footsteps

`game_events.cpp` now samples actual `behavior+0x50` displacement every 25 ms or
more. Stick position no longer generates steps. It:

- rejects teleports (`horizontal >= 1.5` per sample), large vertical changes
  (`abs(dy) >= 0.12`), speeds below `0.7`, and speeds above `30`;
- accumulates horizontal distance;
- uses a walk stride of `FootstepDistance * 0.90` below speed `7.0`;
- uses a sprint stride of `FootstepDistance * 1.35` at or above speed `7.0`;
- alternates `FootLeft` and `FootRight`.

Default `FootstepStrength` was reduced from `0.18` to `0.08`.

These thresholds are educated guesses and need tuning from an actual game run.
Because cadence is distance-based, sprinting should still sound faster despite
the longer sprint stride. Watch for slopes, jumping, frame/update cadence, and
cutscenes that physically move the player entity.

### Outgoing damage hook

The old candidate signature:

```text
29 BB ?? ?? ?? ?? 8B 83 ?? ?? ?? ?? 79 0A
```

was absent from the packed disk executable and was not shipped as the sole
detector. Instead `damage_hook.cpp` waits until the working entity-list signature
proves the runtime image is unpacked, then scans executable sections for x64
`sub` instructions whose destination is `[register+0x858]`:

- opcode `29 /r` (`sub r/m32, r32`)
- opcode `81 /5` or `83 /5` (`sub r/m32, immediate`)

Each candidate is armed with an `INT3` breakpoint. A vectored exception handler
records a hit, restores the original byte, single-steps the original instruction,
then rearms the breakpoint. The polling loop consumes the atomic event and queues
`EnemyHit` plus hitstop. Entity-health polling remains a fallback.

Expected startup log on success:

```text
Outgoing hits: armed N health-subtraction instruction(s)
Outgoing hits: breakpoint at NieRAutomata.exe+0x...
```

If it logs `no subtract-from-health instructions were found`, the next task is to
capture/disassemble the unpacked runtime around all references to displacement
`0x858`, not to retry the obsolete packed-file signature.

This VEH hook is the highest-risk new code. It has passed compilation and generic
DLL loading only; it has not executed inside NieR:Automata. Check first for:

- startup crash or first-hit crash;
- no candidates or an implausibly high candidate count;
- repeated/false hit events from health drains, friendly damage, or regeneration;
- breakpoint rearm races across game threads;
- outgoing haptic being suppressed when `player_damaged` is true in the same
  polling cycle.

The hook leaves candidate code pages writable/executable because the exception
handler must restore and reinsert the breakpoint byte. A production-quality
replacement should preferably use a proven mid-hook library such as SafetyHook
after the correct runtime instruction is established.

### Incoming damage

Player HP polling is confirmed operational. 1.0.3 changes the log to:

```text
Event: player damaged (old -> new); PlayerHit waveform queued
```

This lets the user distinguish our haptic from native rumble. If necessary,
temporarily set `PlayerHitEnabled=0` to perform an A/B test without a timed agent
capture.

### Menu gating

Current logic is deliberately conservative:

- menu mode is true before the player entity is acquired;
- acquiring the player enters gameplay mode;
- Start/Back toggles menu mode;
- O/B queues a cancel pulse while in menu mode, then returns to gameplay mode;
- D-pad, stick-nav, X/A, and O/B pulses only occur while menu mode is true.

This is heuristic. Nested menus, menus opened by scripted events, or closing a
menu with X may desynchronize it. The invalid `+0x9C` tick heuristic must not be
reintroduced. A future robust fix needs the game's real UI/menu state.

## Hitstop status and risk

`timescale.cpp` replaces the main executable's imported
`QueryPerformanceCounter` with a scaled virtual clock. Logs confirm the import
hook installs. On hit, `begin_hitstop(0.08, 1000)` is called.

There is not yet positive visual confirmation that NieR's gameplay timestep uses
that import. The absence of visible hitstop in early builds may have resulted
from the outgoing-hit event never firing, but the mechanism itself remains
unverified. Use the incoming-hit log timestamps as the first known trigger when
evaluating it. If the log says `applying hitstop` but the game does not visibly
slow for one second, replace the QPC approach with the game's actual timescale or
frame-step mechanism rather than lengthening the duration further.

## Haptic transport status

The actual advanced haptic transport is working. Logs show:

```text
Haptics: candidate endpoint 'Speakers (3- DualSense Wireless Controller)', 48000 Hz, 4 channels, 32-bit
Haptics: advanced PCM stream started on actuator channels 3/4
```

The user's menu pulses and incoming-hit response prove real actuator PCM reaches
DSX's wireless virtual device. Do not spend the next iteration re-debugging basic
WASAPI/DSX transport unless those log lines change.

## Recommended next test sequence

Do not use a countdown or require the user to act on the agent's clock.

1. Launch 1.0.3 and inspect the startup log for the outgoing-hook candidate count.
2. Confirm idle/stick input during a cutscene produces no footsteps.
3. Walk, then sprint on level ground; evaluate strength and cadence separately.
4. Hit one ordinary machine once and inspect the log for `hit connected`.
5. Take one hit and verify the explicit `PlayerHit waveform queued` log.
6. Compare both logged triggers against visible one-second hitstop.
7. Re-check D-pad/X/O in gameplay and in at least one nested menu.

If 1.0.3 crashes, immediately restore/install 1.0.2 from commit `b5aff0e` for a
safe baseline, then replace the experimental breakpoint hook. Do not use
destructive Git reset/checkout commands over the user's working tree.

## Build and deployment commands

From `D:\Documents\NieRAutomata`:

```powershell
& .\Build.ps1
& .\tests\smoke_loader.exe .\dist\dinput8.dll
& .\Install.ps1
```

If the game is running, `Install.ps1` stages the build and starts the hidden
watcher; it deploys after the game exits and file locks clear. Verify the manifest
and installed DLL hash afterward. The user's INI currently has diagnostic
hitstop values:

```ini
HitstopSpeed=0.08
HitstopDurationMs=1000
FootstepStrength=0.08
```

Read `C:\DeepThought\shared-instructions.md` before doing further work. In
particular, every build presented to the user increments Z and gets a matching
commit; no remote currently exists, so the inability to push must be reported.
