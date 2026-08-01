# NieR:Automata Haptics + Hitstop — Agent Handoff

## Current state

- Repository: `D:\Documents\NieRAutomata`
- Game install: `C:\Program Files (x86)\Steam\steamapps\common\NieRAutomata`
- Target executable: Steam build 7020666; installer validates SHA-256
  `5171BED09E6FEC7B21BF0EA479DBD2E1B228695C67D1F0B478549A9BE2F5726A`.
- Installed build: **1.0.6**, commit 834e8b4.
- Installed DLL SHA-256:
  `49C41F1D72FCD05AAD0701E91BC3694C47E97E13BC1888CC8A2B0C4420CE9F07`.
- Git has **no configured remote**, so nothing has been pushed. Every commit is
  local only. This must be reported whenever work is called complete.
- **1.0.6 has been built, smoke-loaded, installed, hash-verified, and launched
  in the real game to confirm every hook resolves. The user has not yet played
  it.** The next build shown to the user must be **1.0.7** with a matching
  commit.

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
