# NieR:Automata Haptics + Hitstop — Agent Handoff

## Current state

- Repository: `D:\Documents\NieRAutomata`
- Game install: `C:\Program Files (x86)\Steam\steamapps\common\NieRAutomata`
- Target executable: Steam build 7020666; installer validates SHA-256
  `5171BED09E6FEC7B21BF0EA479DBD2E1B228695C67D1F0B478549A9BE2F5726A`.
- Installed build: **1.0.29**, commit pending.
- Installed DLL SHA-256:
  `FF9EE49CB2E99484608BA79C3DFB4530955F5FBAC2E06D783472884568121D2B`.
- Remote: `origin` ->
  https://github.com/countlessbats/nier-automata-freeplay-hacks-and-enhancements
  (public). Push every commit before calling work complete.
- **History was rewritten once, before the first push**, to purge bulk strings
  extracted from the game binary and four screen captures that had caught the
  desktop rather than the game window. `.gitignore` keeps them out. Never commit
  `analysis/exe_strings.txt`, `analysis/nier_dump.bin`, or a capture that was not
  taken with `PrintWindow` against the game window.
- **1.0.17 is installed, launched and verified.** The next build must be
  **1.0.18** with a matching commit.
- Footstep gaits, established from logging a whole session rather than guessing:
  the player raises **only** `_step_walk_` and `_step_run_`. `_step_dash_` exists
  in the bank but was never raised once, so it cannot represent sprinting.
  `FootstepsSprintOnly` therefore drops only walk.
- **`Pl0000 + 0x1434` is not a speedometer. Do not use it.** It read ~570 in one
  session and pinned at exactly 1054.1 for a dozen consecutive footsteps in
  another. Speed is now measured from position deltas instead, which is ground
  truth.
- **`Pl0000 + 0x106F4` is the animation *request* slot, not the live state.** It
  read 0 for all 33 footsteps of a session containing both running and
  sprinting, because it is only written when a transition is requested. The
  AnimationState enum below is still correct, but the live state field has not
  been found. The jump-counter probe is the way to find it if wanted.
- **Measured speeds, from position deltas over a real session:** jogging is
  4.5-7.2 units/s and sprinting is 7.7-14.3, clustering near 10. They overlap
  only between 7.22 and 7.66, so `FootstepMinSpeed` sits at 8. That is a genuine
  2x separation; anything claiming these differ by a few percent is measuring
  the wrong thing.
- **The event names cannot separate jogging from sprinting** — both are
  `_step_run_`. `FootstepMinSpeed` gates on the game's own movement speed
  (`Pl0000 + 0x1434`). Measured from real play: running peaks at **1000-1025**,
  sprinting at **1082-1115**, so the default sits at 1050. Those bands are only
  ~6% apart, so treat the threshold as something the user tunes by feel, not a
  constant. The field intermittently reads 0.0 when polled mid-update, so the
  gate uses a 250 ms peak hold rather than the instantaneous value. Combat suppression was removed: the game's
  battle flag only tracks what the battle music tracks, so wildlife never
  counted, and lock-on stuck on after a death. The battle flag itself is still
  documented in the internals doc if it is ever wanted.
- **Do not wrap the game's DirectInput objects by swapping vtable pointers.**
  Three attempts crashed the game on startup, including one with a 128-entry
  clone that rules out the short-copy fault which broke the DXGI factory. The
  panel therefore still leaks keyboard input to the game: window messages,
  GetAsyncKeyState, GetKeyState and XInput are blocked, DirectInput is not.
  Doing it properly needs a real forwarding COM wrapper object rather than
  patching the object the game already holds.

## The in-game overlay works and is on by default

`src/overlay.cpp` draws an ImGui panel on the game's own renderer, toggled with
F10 by default. It was verified by launching the game, sending a synthetic key
press and capturing the window: the panel renders over the game with every
control working. `docs/NIER-INTERNALS.md` records the four failures it took to
get there, including the two that crashed the game, so none of them get
repeated.

The toggle key is `[Overlay] ToggleKeyVirtualCode`, because the user has another
tool bound to F10. `tools/ControlPanel.ps1` still works as an external panel.- Settings now hot-reload: the mod watches the INI's last-write time and
  reloads within a poll. `tools/ControlPanel.ps1` (launcher `ControlPanel.bat`)
  edits the INI live and toggles with F10 from anywhere.
- **Keeping plug-in chips on death is live**, contributed by a parallel session
  and shipped in the same 1.0.13 build. `src/chip_keeper.cpp` signature-scans
  the game-over state machine's single call to the death-penalty routine,
  verifies the callee's prologue byte for byte, and NOPs the 5-byte call. The
  corpse still spawns and recovery returns nothing, so there is no duplication.
  Setting is `[Gameplay] KeepChipsOnDeath`. In the live run it logged:
  `death penalty disabled (call at +0x83CC1C to +0x81A460 replaced with nop)`.
  The reverse-engineering is written up in `docs/NIER-INTERNALS.md`.
  **This is the only code patch in the mod** — everything else reads, hooks, or
  calls the game's own functions, so treat it as the first place to look if a
  game update breaks something.
- **PowerShell tools must stay pure ASCII.** An em dash in a string broke the
  parser and the control panel failed to start; both tools are ASCII-only now.
- The user prefers the agent to launch the game for them once a build is ready,
  since it is slow to start.
- **Auto-load walks the title menu with synthetic pad presses and is OFF by
  default.** It is blind: it does not read the menu, so a layout other than the
  expected one lands on the wrong entry. Real input cancels the queue. The
  proper fix is the game's own load path — `UITitleMenu`, `UITitleMenuItem`,
  `ContinueState` and the `@Continue` token category all exist and are the place
  to look; `TITLE_MENU_00..12` are the entry labels.
- Boot logos are disabled by renaming `data/movie_logo/*.usm` to `.usm.disabled`.
  That is game data rather than a mod file, so `tools/Toggle-Logos.ps1` restores
  them and the uninstaller does too. Verified: the game boots to the title
  screen with them gone.

## Hitstop is removed — do not revive it

The user asked for it to be dropped after four failed approaches. `timescale.cpp`
is deleted and every hitstop setting is out of the INI. The history is in
`docs/NIER-INTERNALS.md` so it is not re-attempted from scratch: process-clock
scaling cannot work because the game paces frames off the clocks it simulates
from, the engine's `AccelTime` is player-scoped, and writing enemy animation
rates crashed the game.

## 1.0.7 crashed the game — do not reintroduce what caused it

1.0.7 wrote `anim_spd_rate` (`behavior + 0xC40`) on every non-player entity
during hitstop, to slow enemies alongside the player. The game crashed after a
couple of hits. That write was the mod's **only** write into game memory, and
the offset came from a struct definition for `BehaviorAppBase` that was never
verified against the varied classes actually present in the entity list, so it
was landing somewhere unintended on at least some entities. `safe_write` did not
help: an SEH guard catches a fault, not a valid-looking write to the wrong
field, which corrupts state and crashes later.

1.0.8 removes it. **The mod now writes nothing into game memory** — it only
reads, hooks, and calls the game's own functions. Keep it that way unless an
offset has been confirmed for every class it will be applied to.

## User feedback so far

After 1.0.4: menu haptics good; footsteps fired repeatedly when moving slowly
"as if buffered" with no dodge pause; hitstop imperceptible; hits fired on the
companion's damage. Fixed in 1.0.5.

After 1.0.5: **footsteps confirmed good.** Hitstop was tanking performance again
and firing for companion hits; companion gunfire still vibrated; the user wants
pod *fire* to vibrate but pod *impacts* not to, and hitstop on melee only.
Addressed in 1.0.6. Nothing in 1.0.6 has been played yet.

The user asked whether a CheatEngine-style whole-clock deceleration would be
cheaper. It would not: that is exactly what 1.0.5 already did, and it is the
cause of the frame-rate collapse. See the timing section of the internals doc.

After 1.0.7: **the game crashed** (see below); 9S's sword swings still vibrated
because `wpf000_*` weapon sounds were treated as the player's and the companion
plays them too. Both fixed in 1.0.8.

After 1.0.6: drone/pod vibration to be removed entirely — it fires often enough
to drown out everything else. 9S's melee hits were still vibrating. Hitstop
appeared to slow only 2B's own animations. The melee haptic felt blunt and was
asked to be "shimmerier". All four addressed in 1.0.7.

**Hitstop has now failed three times and is the weak point of this project.**
1.0.7's approach — the engine's player-scoped time acceleration plus scaling
enemy `anim_spd_rate` — is the fourth attempt and is not confirmed. If it still
does not read as a real hitstop, stop iterating on it and either locate the
engine's per-frame delta properly or drop the feature; do not ship a fifth
variation on the same guess.

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

## How the mod decides what to do

### Everything is driven by the game's own sound events

`src/sound_hook.cpp` locates the Wwise post-event family and breakpoints all
four descriptor-taking variants, so every posted sound yields its name and id.
`game_events.cpp` classifies those names. This replaced the movement-distance
footstep heuristic and the XInput menu-state inference; both are gone.

**The `_pl` suffix is the key discriminator.** The character sound player
appends `_pl` for the character the player is controlling and falls back to the
bare name for companions and enemies, so:

- `pl0000_step_walk_L_pl` is 2B, whom the player controls — fires.
- `pl0200_step_walk_L` is 9S as companion — ignored.
- `em0000_step_L` is a machine — ignored.

The model prefix is learned from any `_pl` sound, so it follows story sections
that swap the playable character. Footstep names also state the foot and the
gait, so left/right and walk/run come from the game rather than being inferred.

Outgoing hits come from the game's own hit-confirm sounds
(`core_small_sword_hit`, `core_shot_hit`, `core_shot_bullet_hit`), because a
health decrease in the entity list is equally true when a companion lands the
blow. Entity polling now exists only to notice the player taking damage.

`LogSoundNames=1` records each distinct event name once and marks the ones
attributed to the player, so unmapped events can be identified from an ordinary
play session with no timed test. This is how the footstep names were found.

### Hitstop

Hitstop drives the engine's own `AccelTime` system (`docs/NIER-INTERNALS.md`),
not the process clock. Two clock-scaling attempts failed, and the second one —
scaling every clock the game imports, which is what a speedhack does — proved
the approach is unusable here: NieR paces frames off the same clocks it
simulates from, so a slowed clock makes the frame limiter wait proportionally
longer in real time. The frame rate collapses and nothing slows down.

`AccelTime_request(singleton, rate, duration_frames, delay_frames, flags)` is
the call the game itself uses for slow motion. Durations are in frames at 60 Hz;
the config still takes milliseconds and converts.

Hitstop fires on the melee hit-confirm sound only. Pod round impacts
(`core_shot_hit`, `core_shot_bullet_hit`) are ignored entirely — they fire for
the companion's pod as readily as the player's, and an impact is not an action
the player took.

**The hitstop hook must be installed from the event loop, not at DLL load.** The
executable is still encrypted when the DLL attaches, so the signature cannot be
found yet. 1.0.6's first build had exactly this bug and logged "call site was
not found"; it is installed alongside the sound hook now.

## Verified without the user playing

- The post-event address resolved at runtime matches the offline analysis
  exactly, and all captured event ids reproduce from the documented FNV-1 hash.
- The shipped classifier was replayed against the full list of names captured
  from a real session: it fires on 2B's four footstep events, the three
  hit-confirm sounds and the menu sounds, and suppresses all seven companion and
  machine footstep events.

## Not yet confirmed by play

1. Whether clock virtualization produces visible slow motion. If the frame rate
   still suffers instead, the next lead is the engine's own delta time — do not
   simply lengthen the duration or lower the speed further.
2. Whether the hit-confirm sounds are exclusive to the player's attacks. They
   are `core_` feedback sounds, which is why they were chosen, but if hits still
   register for the companion, the log will show which name fired.
3. Whether footsteps survive surfaces or areas where the `_pl` variant may not
   exist in the sound bank. If footsteps go silent, the log will show a
   `pl0000_step_*` name without the suffix; set `FootstepPlayerOnly=0` as an
   immediate workaround.
4. Whether menu classification covers nested menus and the pause screen.

## Known risks

`sound_hook.cpp` uses INT3 breakpoints with a vectored exception handler, which
leaves those pages RWX. It is proven to work but is not production-grade; a
rewrite onto a real mid-function hook library such as SafetyHook is the eventual
fix. 1.0.5 deleted `damage_hook.cpp`, which used the same technique on five
health-subtraction instructions, so this is now the only such hook.

The breakpoints sit on hot functions and every posted sound costs two
exceptions. Observed rates are low enough not to matter, but a scene that posts
sounds far more aggressively is the thing to watch if frame pacing regresses.

## Architecture

Native x64 `dinput8.dll` proxy loaded from the game directory.

- `src/dllmain.cpp` — proxy forwarding and the mod worker thread.
- `src/haptics.cpp` — WASAPI PCM stream to DualSense actuator channels 3/4.
- `src/sound_hook.cpp` — Wwise post-event observation; the source of
  footstep, menu and outgoing-hit events.
- `src/game_events.cpp` — entity/player polling, sound classification, dispatch.
- `src/timescale.cpp` — virtual clock across every imported time source.
- `src/config.cpp` / `.hpp` — INI configuration.
- `tools/dump_image.cpp` — external decrypted-image dumper.
- `tools/waveform_player.cpp`, `tools/WaveformStudio.ps1` — haptic design
  sandbox; see `docs/WAVEFORM-CODES.md` for the NHW1 code format.
- `analysis/nier.py` — dump analysis toolkit.
- `docs/NIER-INTERNALS.md` — game internals reference.
- `docs/WAVEFORM-CODES.md` — waveform studio and the NHW1 interchange format.
- `Build.ps1`, `Install.ps1`, `Watch-Deploy.ps1`, `tests/smoke_loader.cpp`.

Runtime log and install manifest live in the game directory as
`NierHaptics.log` and `NierHaptics.install.json`.

## Build and deployment

```powershell
& .\Build.ps1
& .\tests\smoke_loader.exe .\dist\dinput8.dll
& .\Install.ps1
```

`Build-Tools.ps1` builds the standalone tools; they are not part of the mod.

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
