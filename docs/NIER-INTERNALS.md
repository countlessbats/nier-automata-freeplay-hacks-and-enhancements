# NieR:Automata internals — working notes

Everything here was recovered from the Steam build whose `NieRAutomata.exe` has
SHA-256 `5171BED0…5726A` (build 7020666). All addresses are **RVAs** (offsets
from the module base), so add the runtime module base to use them.

## The single most important fact: the executable is encrypted on disk

`NieRAutomata.exe` ships with a SteamStub DRM wrapper. The extra `.bind`
section is the stub, and `.text` is encrypted until the stub decrypts it in
memory at startup.

Consequences, which cost earlier iterations a lot of time:

- **Strings are readable on disk.** `.rdata` is *not* encrypted, so any string
  scan of the shipped executable works. This is why `core_main_cursor`,
  `AnimationState_*` and friends are greppable directly.
- **Code signatures are not.** Any byte pattern in `.text` is absent from the
  file and appears only in the running process. Never conclude a code signature
  is wrong because an offline scan of the shipped executable misses it.

### Getting a workable image

`tools/dump_image.cpp` builds `dump_image.exe`, which waits for the process,
waits until the decrypted entity-list signature is present, and writes the main
module in **image layout** (file offset == RVA) with section headers fixed up.

```powershell
# Move any dinput8.dll proxy aside first so the dump is not polluted with hooks.
Start-Process .\tools\dump_image.exe -ArgumentList 'NieRAutomata.exe','.\analysis\nier_dump.bin','300'
Start-Process "steam://rungameid/524220"
```

The game loads at its preferred base (`0x7FF7F9AC0000` observed), so
RVA + `IMAGE_BASE` is the runtime virtual address, and the dump needs no
rebasing.

### Analysis toolkit

`analysis/nier.py` wraps the dump: `find_sig` (wildcard byte patterns),
`find_string`, `find_refs` (numpy-accelerated RIP-relative displacement scan),
`find_calls_to`, `func_of` (exact function bounds from the `.pdata` exception
directory — 53,667 functions), and `disasm`/`print_func` via capstone.

`func_of` is the single most useful helper: linear disassembly desynchronizes if
you start mid-instruction, so always disassemble from a `.pdata` function start.

## Entity model

Confirmed against praydog's [AutomataMP](https://github.com/praydog/AutomataMP)
SDK, whose signatures all resolve in the dump — a good cross-check that a dump
is genuinely decrypted.

Entity list global, via signature `44 8B 0D ?? ?? ?? ?? 44 39 0D ?? ?? ?? ?? 75 24`
(resolves the RIP-relative operand to the `CEntityList` object itself, not a
pointer to it):

| Field | Offset |
| --- | --- |
| entity count | list + `0x04` |
| pair array | list + `0x10` (stride 16) |
| `CEntityInfo*` | pair + `0x08` |

`CEntityInfo`: name at `+0x08` (≤32 bytes, player is exactly `Player`),
behavior pointer at `+0x48`.

Behavior (`BehaviorAppBase` / `Pl0000`):

| Field | Offset |
| --- | --- |
| position (`Vector4f`) | `0x50` |
| facing | `0x94` |
| current health | `0x858` |
| maximum health | `0x85C` |
| `anim_spd_rate` | `0xC40` |
| `CharacterController` | `0xCA0` |
| → controller speed | `0xCA0 + 0x794` = **`0x1434`** |
| → controller facing | `0xCA0 + 0x79C` = `0x143C` |

The controller speed is the game's own movement speed. Prefer it over
differentiating position samples, but note it is not in world units per second:
it reads around 570 while running, so treat it as a relative magnitude and
calibrate against a logged sample rather than assuming a scale.

## Sound: the Wwise event layer

This is the highest-value subsystem for modding, because nearly every
interesting gameplay moment plays a sound.

### Event ids are FNV-1 32-bit hashes

`0xACA3F0` lowercases the name and hashes it with basis `0x811C9DC5` and prime
`0x01000193`, multiply-then-xor (FNV-1, not FNV-1a). So any event id can be
computed offline from its name:

```python
def wwise_id(name):
    h = 0x811C9DC5
    for c in name.lower().encode():
        h = ((h * 0x01000193) ^ c) & 0xFFFFFFFF
    return h
```

### The post-event family

Every sound is posted through a function taking a descriptor in `RCX`:

```c
struct PostDescriptor { const char* name; uint32_t id; uint32_t flags; };
//                      +0x00           +0x08         +0x0C
```

`name` is null when the caller posts a precomputed id. Four descriptor-taking
variants sit at fixed offsets:

| RVA | Offset from first | Role |
| --- | --- | --- |
| `0x140200` | `+0x000` | post, no game object |
| `0x1403C0` | `+0x1C0` | post on a game object |
| `0x140560` | `+0x360` | post with position |
| `0x140890` | `+0x690` | post, fourth variant |

Locate the first one from this signature on the by-name wrapper `0x1387A0` and
follow its trailing `call`:

```
53 48 83 EC 30 8B DA 48 89 4C 24 20 33 D2 E8 ?? ?? ?? ??
48 8D 4C 24 20 89 44 24 28 89 5C 24 2C E8
```

Wrappers above them:

| RVA | Signature | Notes |
| --- | --- | --- |
| `0x1387A0` | `(const char* name, uint32_t flags)` | global/system SE — **630 call sites** |
| `0x1387D0` | `(const char* name, obj, …)` | SE on an entity |
| `0x138890` | `(uint32_t id, uint32_t flags)` | post by id, name is null |
| `0x138910` | `(playing_id, const char* rtpc, float value)` | set RTPC by name |
| `0x8662C0` | character SE; appends `_pl` / `_npc` / `_ot` then sets `SE_CharaType` |
| `0x866830` | sets the `SE_FloorCollisionType` switch |

Note `0x8662C0` builds its name in a **stack buffer**, so a hook must copy the
string immediately rather than retain the pointer.

### Known event names

Menu vocabulary (recovered by resolving the `lea rcx` before each of the 630
calls to `0x1387A0`; count = call sites):

| Name | Sites | Meaning |
| --- | --- | --- |
| `core_main_cursor` | 159 | cursor movement |
| `core_main_disicion` | 65 | confirm (the game's own spelling) |
| `core_main_cancel` | 21 | cancel |
| `core_menu_error` | 17 | invalid action |
| `core_menu_cursor_toptab` | 5 | tab change |
| `core_back_menu_slide` | 6 | menu slide |
| `core_menu_item_general_decide` | 1 | item confirm |

`Set_State_*` and `RTPC_*` names go through the same call, so a name-based hook
sees switch and RTPC traffic too — filter it out.

Footstep event names are **not** in the executable; they live in the packed CPK
data and arrive as runtime strings, which is exactly why hooking the post
function (rather than pattern-matching the executable) is the right approach.
`pl_L_FootStep_R` in `.rdata` is a footprint **VFX attachment**, not a sound.

Footstep and hit names recovered by logging a live session:

| Name | Source |
| --- | --- |
| `pl0000_step_walk_L_pl`, `..._R_pl`, `pl0000_step_run_L_pl`, `..._R_pl` | 2B, player-controlled |
| `pl0200_step_walk_L`, `pl0200_step_run_L`, … | 9S as companion |
| `em0000_step_L`, `em0000_step_R`, `em0000_step_move` | machines |
| `core_small_sword_hit`, `core_shot_hit`, `core_shot_bullet_hit` | attacks connecting |

### Melee hit-confirms are per weapon class

There is no single "you hit something" sound. Each of the four weapon classes
has its own, so matching one name covers only one class:

| Class | Names |
| --- | --- |
| Small sword | `core_small_sword_hit`, `core_small_sword_hit_em7000` |
| Large sword | `core_big_sword_hit`, `core_blade_hit` |
| Spear | `core_spear_hit`, `core_pl_AS_black_spear_hit(_em)` |
| Combat bracer | `core_nackle_hit(_animal)`, `core_bare_hand_hit(_animal)` |

Plus `core_pl_blow_hit`, `core_pl_PS_critical_hit`, `core_pl_AS_wire_hit` and
`core_sword_hit_animal`.

Names carrying "hit" that are **not** the player's melee: anything with `shot`
or `bullet` (pod and enemy rounds), `gun`, `pod` (`core_pl_AS_pod_throw_hit`),
`hak`/`hack` (the hacking minigame), `emil` (a merchant NPC's own attacks) and
`drug`.

`core_pl_` is a reliable player marker for the ones that carry it.

### The `_pl` suffix is the player marker

`0x8662C0` appends `_pl`, `_npc` or `_ot` to a character sound name, checks
whether that variant exists, and falls back to the bare name when it does not.
In practice the character the player controls gets `_pl` and everyone else falls
back — so **`_pl` is how you tell the player's own sounds from a companion's or
an enemy's**. The model prefix before the first underscore (`pl0000` for 2B,
`pl0200` for 9S) identifies the character, and learning it from any `_pl` sound
lets it follow the story's character swaps.

Footstep names also state the foot (`_L_` / `_R_`) and the gait (`walk` / `run`),
so neither has to be inferred.

Related Wwise switches/RTPCs: `SE_FloorCollisionType`, `SE_CharaType`,
`SE_PlayerType`, `SE_Speed`, `SE_Doppler`, `SE_Occlusion`.

## Player movement state machine

`0x702700` maps a movement-state enum to its name — a jump table of 24 entries
at `0x7027E8`. The enum is 1-based:

| # | State | # | State |
| --- | --- | --- | --- |
| 1 | `Idle` | 13 | `JumpStart_Idle` |
| 2 | `Walk` | 14 | `JumpStart_Run` |
| 3 | `RunStart` | 15 | `JumpStart_Dash` |
| 4 | `RunLoop` | 16 | `JumpUp_Idle` |
| 5 | `RunStop_Early` | 17 | `JumpUp_Run` |
| 6 | `RunStop` | 18 | `JumpUp_Dash` |
| 7 | `Dash` | 19 | `JumpLoop_Idle` |
| 8 | `DashStop` | 20 | `JumpLoop_Run` |
| 9 | `EscapeStart_Front` | 21 | `JumpLoop_Dash` |
| 10 | `EscapeLoop_Front` | 22 | `JumpEnd_Idle` |
| 11 | `EscapeEnd_Front` | 23 | `JumpEnd_Run` |
| 12 | `EscapeToDash_Front` | 24 | `JumpEnd_Dash` |

`Escape*` are the dodge. `Jump*` are split into Start → Up → Loop → End, each
with Idle/Run/Dash variants.

A debug overlay at `0x71A3BD` prints a request struct, giving its layout:

| Offset | Field |
| --- | --- |
| `+0x28` | `reqState` (the enum above) |
| `+0x30` | `ignore` |
| `+0x34` | **`reqJump`** |
| `+0x38` | `reqMove` |
| `+0x3C` | `reqMoveStp` |

## Multi-jump: what is known and what is not

The pieces above are the start of an infinite-jump mod; this is a plan, not a
finished feature.

What is already established:

- The jump is a request (`reqJump`, `+0x34` of the request struct) consumed by a
  state machine whose states are fully enumerated, including the three
  `JumpLoop_*` airborne states.
- `0x71A3BD` is a virtual function with no direct callers, so the request struct
  must be reached through the object's vtable rather than a call site.

Additionally established:

- The animation request block lives at **`Pl0000 + 0x106F0`**:
  `+0x106F0` request-pending flag, `+0x106F4` the movement state from the enum
  above, `+0x106F8` a sub-state, `+0x106FC` an animation id, `+0x10700` a float,
  `+0x10704`, `+0x10708`. Jump starts write `0xE` (`JumpStart_Run`) or `0xF`
  (`JumpStart_Dash`) to `+0x106F4` and then call `0x47EE80(Pl0000*)`, which
  appears to commit the request.
- Jump-start sites: `0x4AF89A`, `0x4B3B4C`, `0x4C8E38`, `0x4D11E2`.
- The game plays `pl0000_jump_first` and `pl0200_jump_second`, so the engine
  distinguishes the first jump from the second and a counter exists.

### The air-jump counter: `Pl0000 + 0x14A8`

Found with the probe rather than by reading code. Across five logged jumps only
one field behaved like a count: `+0x14A8` read `1 -> 2` on jumps taken while
already airborne — the only candidate reaching **2**, which is the double-jump
limit. `+0x165F0` stepped `0 -> 1` on every jump and is an airborne flag, not a
count; the rest appeared once or twice and did not recur.

`Pl0000 + 0x14A8` is `CharacterController + 0x808` (the controller lives at
`Pl0000 + 0xCA0`), a plausible home for it.

Repeated jumps are implemented by holding that dword at zero from the polling
loop, so the game always believes a jump is available. The write is guarded: the
player's object only, this one dword, and only when it already holds a plausible
count. The offset is exposed in the INI so a game update can be handled without
a rebuild.

**Historical note — the counter was not findable statically.** Searches that came up
empty: the debug overlay at `0x71A3BD` is unreferenced and not in any vtable, so
its request struct cannot be located that way; no field in the player region is
both incremented and compared against a small limit; and the fields written
during a jump are animation parameters rather than a count.

Because of that, 1.0.11 ships a **read-only probe** instead of a guess. It keeps
a grounded baseline of the whole `Pl0000` block, and when a player jump sound
fires it reports every dword that stepped up by exactly one and stayed at four
or less. The counter identifies itself by reading 0 -> 1 on the first jump and
1 -> 2 on the second. Once the offset is known, the implementation is to hold it
at zero from the polling loop — a write to one field of one entity whose class
is known, which is a far narrower risk than the 1.0.7 crash.

## Timing and hitstop

### Do not scale the process clock

The game reads several clocks — `QueryPerformanceCounter` (39 call sites),
`timeGetTime` (17), `GetTickCount` (3) — and the IAT is at RVA `0xC1B000`.

Two attempts were made and both failed:

1. Scaling only `QueryPerformanceCounter` desynchronized the simulation from the
   frame limiter.
2. Scaling **every** clock from one virtual timeline — the classic speedhack
   approach — failed the same way, which is the useful result: NieR paces its
   frames off the same clocks it simulates from, so telling it that 8% as much
   time has passed makes the frame limiter wait about twelve times longer in
   real time. The frame rate collapses and the action does not slow. No choice
   of scale avoids this, because the limiter and the simulation read the same
   value.

Engine startup at `0x26C710` stores the performance frequency at `0x1769F80` and
a derived seconds-per-count at `0x1769FA8`.

### Use the engine's own time acceleration

The engine already implements slow motion, and it is what the game uses for its
own dramatic moments.

```c
void AccelTime_request(void* self, float rate, float duration_frames,
                       float delay_frames, int flags);   // RVA 0x800CD0
```

The singleton lives at RVA **`0x1250F68`**. The object's own fields:
`+0x00` active flag, `+0x14` rate, `+0x18`/`+0x20` duration, `+0xC0` a critical
section (so calling from a mod thread is safe), `+0xE8` enabled.

The eight in-game call sites pass rates of **0.1, 0.4 and 0.5** over **8, 16 or
30 frames**, with a delay of 5 frames — durations are in **frames at 60 Hz**,
not milliseconds. `0x81A330` is the inner start function, which also plays
`core_AccelTime_In`.

Locate both the function and the singleton from one call-site signature:

```
F3 0F 10 1D ?? ?? ?? ??   movss xmm3, <delay>
48 8D 0D ?? ?? ?? ??      lea   rcx,  <singleton>
F3 0F 10 15 ?? ?? ?? ??   movss xmm2, <duration>
F3 0F 10 0D ?? ?? ?? ??   movss xmm1, <rate>
C7 44 24 20 00 00 00 00   mov   [rsp+0x20], 0
E8 ?? ?? ?? ??            call  AccelTime_request
```

Two sites match and both resolve to the same pair. Singleton is at
match + 8 + 7 + disp32(match + 11); the function at match + 39 + 5 +
disp32(match + 40).

**AccelTime is player-scoped, not a global game speed.** All 25 functions that
read the object's active flag live in the player code region (`0x42xxxx` to
`0x61xxxx`, the same range as `Pl0000`), and in play only the player character
visibly slows. It is the "finisher slow" effect — the same update function that
drives it also posts `State_FinishSlowOff`.

A true global timescale has **not** been located. Leads that did not pan out:
`GURADHITSTOPSCALE_` is a per-entity guard parameter from the object parameter
table, not a global; the frequency and seconds-per-count globals stored at
engine init have no direct code references. Slowing everything therefore needs
either the engine's per-frame delta or a per-entity approach.

The per-entity approach was tried and **crashed the game**: writing
`anim_spd_rate` (`BehaviorAppBase + 0xC40`) on every non-player entity assumes
an offset that does not hold for every class in the entity list. Do not write
into entity memory without confirming the offset for each class involved.

### Hunting the native hitstop — what has been ruled out

The engine does have a hitstop concept: `GURADHITSTOPSCALE_` binds to a float at
`+0x120` of a player parameter struct (parser at `0x408EEC`, which also maps
`GURADHITBACKSPEED_` to `+0x68`, `TURNRATE_` `+0x6C`, `CAMANG_*` `+0x8C..+0x9C`,
`PODJUMPTIME_` `+0x1F0`). A *scale* implies a base hitstop exists. The consumer
of `+0x120` has not been found because the struct's address is not yet known.

Dead ends, so the next attempt does not repeat them:

- **`applyDamage` (`0x66D1F0`, virtual, reached via vtable)** is pure damage
  arithmetic: resistances, multipliers, clamping, then `sub [rdi+0x858], ebx`.
  It sets no timer and no hitstop.
- **`anim_spd_rate`** is not the native mechanism. Its only writer is a
  one-instruction virtual setter at `0x4E62A0` with no direct callers, and it
  has just two readers, which pass it to the animation-advance virtual.
- **`+0xBE8`** looked like a hitstop countdown in the update at `0x420090`
  (loaded from `+0xB44`, decremented, gates a block while positive) but it is a
  per-state delay, and the same offset is unrelated `qword` data in other
  classes.

The promising next step is **empirical rather than static**: the sound hook
already knows the exact moment a melee hit lands, so snapshot the player's
behavior block before and after that moment and diff it. Whatever field the
engine sets for its own hitstop will show up as a change correlated with hits
and nothing else, which finds the mechanism without guessing at offsets.

## Plug-in chips, corpses and the death penalty

Recovered for the chip-keeper feature. All addresses are RVAs in build 7020666.

### Chip inventory

The save-data block is reached through a **pointer** global at `0xF5D0C0`
(deref once; the code below calls it `save`). Chip inventory:

- array base `save + 0x1F50`, **300 entries**, stride `0x30`
- active chip-set index (0..2) at `save + 0x1F48`

Chip entry fields (offsets within an entry):

| Offset | Field |
| --- | --- |
| `+0x04` | chip id; `-1` marks an empty slot |
| `+0x14` / `+0x18` / `+0x1C` | slot position in chip set A / B / C; `-1` = unequipped |
| `+0x20` / `+0x24` / `+0x28` | backup of the three set slots, written at death, restored on body recovery |
| `+0x2C` | status: `0` = in the player's possession, nonzero = lying on the corpse |

Chip ids `0xD1A..0xD1E` are the protected family the penalty never takes (the
OS chip and friends). A chip catalog with stride `0x30` hangs off the global at
`0x13C94E0` (`+8` is the first id); the penalty routine looks up a category
there and a jump table exempts certain categories.

### The corpse record

The player's own corpse is **not** an inventory: it is a `0x120`-byte record at
`0x1494790` (name, map string at `+0x10`, appearance/loadout words, position at
`+0xF0`) plus a count/valid dword at `0x14948B0` and an exists byte at
`0x1494780`. It holds **no chip data** — "chips on the corpse" is entirely the
per-chip `+0x2C` status flag above. The world entity is spawned by name
(`"Corpse"`, entity type id `0x21080`/`0x21081`) from the chunk at `0x7C30D5`;
`BehaviorSetItemCorpse` is its behavior class.

### The death-penalty routine — `0x81A460`

Called from **exactly one place**: `0x83CC1C`, inside the game-over/continue
state machine at `0x83C840` (which also builds the corpse record via
`0x7C3AA0` and commits it). The routine:

1. wipes for good every chip entry whose status is nonzero — chips still on a
   previous corpse are permanently destroyed here (the double-death loss);
2. copies each entry's set slots `+0x14/+0x18/+0x1C` into the backups
   `+0x20/+0x24/+0x28`, then for every chip equipped in the **active** set —
   skipping ids `0xD1A..0xD1E` and jump-table-exempt categories — sets status
   `= 1` and clears all three set slots;
3. calls `0x814A40(save_ptr, active_set)` to recompute the set.

Body recovery (writer at `0x80A525`, part of the `UICorpseMenu` flow) is the
inverse: status back to `0`, set slots restored from the backups. The
second-death cleaner at `0x7C3EA0` (callers `0x7D9350`, `0x8359E0`) deletes
flagged chips and memsets the corpse record.

### The chip-keeper patch

`src/chip_keeper.cpp` finds the unique 36-byte call-site signature at
`0x83CC15` (`lea rcx,[rip+..]; call ..; xor edx,edx; lea r8d,[rdx+0x60];
lea rcx,[rip+..]; call ..; mov [rip+..],ebx`), resolves the `call` target,
verifies the penalty routine's 17-byte prologue byte-for-byte, and replaces
the 5-byte `call` with one 5-byte NOP. Chips are then never flagged or
unequipped; the corpse still spawns for achievements and the repair/ally
options but returns nothing, so recovery cannot duplicate anything. Skipping
the routine also skips its phase-1 destruction of previously flagged chips,
which only matters for a save that already had chips on a corpse when the mod
was first enabled — those stay flagged until recovered normally.

## Miscellaneous leads

- `is_loading` global, via `48 83 ec 28 e8 ? ? ? ? c7 05 ? ? ? ? 01 00 00 00`,
  absolute at instruction + 11.
- ~1,331 script-export names of the form `Class.method` exist as plain strings,
  including 248 `Pl0000.*` entries — a broad map of the scriptable surface.
- `hap::` is the engine namespace; `TokenCategory` / `StateObject` linked lists
  are searchable by name (`@SceneState`, `GlobalPhase`).
- Sound banks are Wwise (`.bnk`, `.wsp`); `WwiseInfo.wai` indexes them but holds
  no event names. `SoundMacro.bxm` is a data-driven name→event macro table.


## The in-game overlay: hooking this game's renderer

The panel is Dear ImGui drawn from the game's own present call. Getting there
took four failures, each worth recording because they are all easy to repeat.

The game imports exactly two graphics entry points: `D3D11CreateDevice` from
d3d11.dll and `CreateDXGIFactory` from dxgi.dll. That is the whole surface.

1. **Do not patch DXGI's shared vtable.** Creating a throwaway swap chain to
   read `IDXGISwapChain`'s vtable and patching slot 8 there is the technique
   most guides describe. It killed the game instantly with `0xc00000fd`, a stack
   overflow, and the hook never even ran. That vtable is shared far more widely
   than this process's swap chain. A probe run that created the dummy device but
   left the vtable alone was perfectly stable, which isolated the cause.
2. **Hook the object, not the class.** The working approach walks the game's own
   path: patch the IAT entry for `CreateDXGIFactory`, give the returned factory a
   *private copy* of its vtable, and hook swap-chain creation there. Only that
   one object is affected and dxgi.dll is never written to.
3. **Copy the whole vtable, generously.** A 16-entry copy of the factory crashed
   the game: it asks for `IDXGIFactory2` and calls slots past the 1.0 interface,
   which then read past the copy. The factory now clones 48 entries and the swap
   chain 64.
4. **This game presents through `Present1`, not `Present`.** The swap chain comes
   from `CreateSwapChainForHwnd` (factory slot 15), so it is an
   `IDXGISwapChain1` and the frame goes through slot **22**. Hooking only slot 8
   is why the hook stayed silent through several attempts. Both are hooked now,
   along with `ResizeBuffers` at slot 13.

The window is 1600x900 windowed, so a `PrintWindow` call with
`PW_RENDERFULLCONTENT` captures it; a plain GDI screen grab does not, which
matters when verifying the panel without a person watching.

Settings are written back to the INI, which the mod already watches and reloads,
so the panel needs no shared state with the polling thread.


## Loading the most recent save without synthetic input

1.0.29 walks the title menu with fake pad presses, which is blind and therefore
off by default. Everything needed to replace it with a direct call exists; this
is what has been established so far.

### What is known

| Symbol | Where |
| --- | --- |
| `UITitleMenu` RTTI | `0xFB58D8` |
| `UITitleMenuItem` RTTI | `0xFB58B0` |
| `ContinueState` RTTI | `0xC35028` |
| `@Continue` token category, registered at | `0xBFC50` |
| `TITLE_MENU_00`..`TITLE_MENU_12` labels | `0xD0ACE8` onward |
| `COMN_CONTINUE` label | `0xCF1D2E`, `0xD08EB8` |

A menu builder sits at **`0x928FE0`**. It resolves `TITLE_MENU_00` through
`0x14C4C0` (label lookup) and installs it via `0x8F28B0`, then loops installing
further entries at stride `0xA0`, and finishes by zeroing what look like the
selection fields at **`+0x450`** and `+0x45C` of the menu object in `rbp`. It has
no direct callers, so it is virtual.

### Why it is not finished

`find_refs` locates nothing for the RTTI names because MSVC references type
descriptors through image-relative offsets in the COL structures rather than
RIP-relative operands, so the class has to be reached by walking vtables rather
than by a reference scan. That is the next step: find `UITitleMenu`'s vtable
through its complete-object locator, then the live instance, then confirm
whether `+0x450` really is the highlighted index and which index Continue holds.

### Two viable finishes, in order of preference

1. **Call the load path directly.** `ContinueState` is a `hap` StateObject, so
   the AutomataMP approach applies: walk the StateObject list to find it by name
   and invoke its script function. No input, no menu assumptions.
2. **Verify before pressing.** Keep the synthetic confirm but read the menu's
   highlighted index first and only press when it is Continue. Much less work
   than option 1 and removes the entire risk of landing on New Game.

Either removes the reason `AutoLoadLastSave` defaults to off.


### The state system is reachable; `@Continue` is not a token category

`src/state_system.cpp` walks the `hap` token category list and works. Rather
than depend on one instruction sequence, it scans writable sections for an
object shaped like a `TokenCategory` whose name reads `GlobalPhase`, then walks
`next`. Confirmed live:

| Category | Address |
| --- | --- |
| `GlobalPhase` | `+0xF2B048` |
| `SubPhase` | `+0xF2A900` |
| `Phase` | `+0xF2A8C8` |
| `GlobalRoom` | `+0xF2A680` |
| `Room` | `+0xF2A328` |
| `Grid` | `+0xF29D40` |
| `Hacking` | `+0xF04CC8` |
| `@SceneState` | `+0xFC23B0` |
| `Quest` | `+0xF04620` |

Layout confirmed by this working: name pointer at `+0x10`, next at `+0x18`,
object stride `0x20`.

**`@Continue` is not among them**, so the load path is not a token category
lookup. The string at `0xC3FBF0` referenced from `0xBFC50` is something else —
possibly a scene-state name rather than a category, or registered by a system
that is not running at the title screen.

**`SceneStateSystem` is now located: `+0xFC2370`**, confirmed live, from the
`@SceneState` category at `+0xFC23B0` minus `0x40`. That is the object
AutomataMP drives, so `has` / `set` / `reset` are reachable on it.

What remains is identifying *which* scene state means continue. A state is
addressed only by a CRC32 of its name, so it cannot be enumerated — but it can
be observed: hook `set` and log the CRC it receives while choosing Continue from
the menu by hand, then call `set` with that CRC at the title screen. AutomataMP
locates these script functions by PUID rather than fixed offsets, which is the
approach to copy since its published offsets are from a different build.

The next thing to try is `@SceneState`, which *is* present and is the system
AutomataMP drives: `SceneStateSystem` sits at the category address minus 0x40
and exposes `has`, `set` and `reset`, each taking a `SceneStateName` that is
only a CRC32 of a string. If continuing is expressed as a scene state, setting
it is a single call with no synthetic input. Enumerating which scene states
exist, or watching which one flips when Continue is chosen by hand, would
identify it.


## Easy-mode-only chips in other difficulties

The auto chips are ids **`0xD1A`..`0xD1E`** (five of them), the same family the
death penalty never takes. Every gate on them tests that range the same way, as
`lea reg, [id - 0xD1A]` followed by `cmp reg, 4`, which makes them easy to find:
ten such sites exist.

| Site | Function |
| --- | --- |
| `0x7CCE86` | `0x7CCE80` |
| `0x7DBF65`, `0x7DBF74` | `0x7DBEE8` |
| `0x7E69DA` | `0x7E69D0` |
| `0x7E6B0E` | `0x7E6AD0` (help text) |
| `0x7F2E02` | no pdata entry |
| `0x7F4FB3` | no pdata entry |
| `0x80ED9C`, `0x80EF48` | `0x80E530` |
| `0x81A576` | `0x81A464` |

**`0x9CA170` returns the current difficulty.** The help-text function calls it,
keeps the result, and picks `CORE_AUTO_CHIP_99` when it equals 3 — so the
difficulty is a small integer and 3 is the restricted case. It has many callers,
listed by `find_calls_to`.

**`0x7F2E10` takes (something, chip id) and returns a flag**, and the help text
uses it together with the difficulty to decide which explanation to show. It is
the strongest candidate for the legality check, so the next step is to
disassemble it and see whether it consults `0x9CA170`.

**The gate is `isChipUsable` at `0x7CCE80`.** It reads:

    eax = chipId - 0xD1A
    if (eax <= 4)              // one of the five auto chips
        if (difficulty() != 0) // anything but Easy
            return 0;          // not usable
    ...
    return 1;

The `ja` at `+0x11` skips the difficulty test for ordinary chips. Making it
unconditional sends auto chips down the same path, so they are never rejected.
One byte, `0x77` to `0xEB`.

Found by intersecting the ten sites that test the `0xD1A..0xD1E` range with the
45 callers of the difficulty getter: only five functions do both, and this is
the smallest, doing nothing else.

**A first attempt patched `0x7F4F80` and did nothing**, because that function
only reports whether a set contains an auto chip. Superseded note follows.

Superseded: the gate is `0x7F4F80`: it walks the chip array at stride
`0x30` and returns 1 as soon as a chip's id falls in `0xD1A..0xD1E`. Overwriting
its first three bytes with `xor eax, eax; ret` makes it answer "no auto chips
here", which is what the restriction hangs on. Three bytes, reversible in place,
nothing written to the save, so the option can be switched while playing and
turning it off leaves equipped chips alone.

Original shape of the feature: make the legality check answer
"allowed" for ids `0xD1A`..`0xD1E` regardless of difficulty. The user's
requirement is that turning the option off must not disturb chips already
equipped — they simply cannot be re-equipped once removed — which a check-time
hook satisfies naturally, since nothing is written to the save.

## Quick-load from the title screen, recap

Established already: `SceneStateSystem` is at `+0xFC2370`, `@Continue` is not a
token category, the title menu builder is `0x928FE0` and zeroes what look like
selection fields at `+0x450`/`+0x45C`, and `UITitleMenu`'s RTTI is at `0xFB58D8`
but cannot be found by a reference scan because MSVC uses image-relative offsets
in the complete-object locators.

Two ways forward, unchanged: drive the state system once the state that means
continue is identified by hooking `set` and watching the CRC while choosing
Continue by hand; or read the highlighted menu index and only send a confirm
when it is already on Continue, which removes the risk of landing on New Game
without needing the state system at all.


### StateObject list: walk works, but it starts mid-list

`find_state_object` in `src/state_system.cpp` walks the StateObject chain (name
at `+0x20`, next at `+0x38`, stride `0x40`) and reads real objects out of a
running game:

| State object | Address |
| --- | --- |
| `DEAD` | `+0xF372D8` |
| `@EnemySet` | `+0xFC6AD0` |
| `@SCENE` | `+0xFC6200` |

Only three, which means the scan latched onto a node partway down the chain
rather than its head — the same problem the token category walk solved by
anchoring on `GlobalPhase`. **`ContinueState` was not in the reachable portion.**

Two things to settle next, in this order:

1. **Find the real head.** The token category walk anchors on a name the game
   always registers first. The StateObject list needs the same treatment, either
   by finding an equivalent always-present name to anchor on, or by locating the
   head pointer properly — AutomataMP has a `StateObject::get_first`, and its
   approach of scanning for the registration of the first object is the model.
2. **Only then decide whether `ContinueState` is even reachable at the title.**
   It may only be constructed once a save is being continued, in which case it
   is the wrong handle entirely and the answer is the save-load call itself
   rather than a state object.

The goal remains a single call that lands in the game, with no synthetic input:
the user specifically does not want button emulation, partly because Start Game
is a second confirmation after the file is chosen.


### Scanning for StateObjects: a dead end as written

Walking the list needs a head that has not been found, so the next attempt
scanned writable memory for anything of the right shape instead. The filter — a
pointer at `+0x00` into the image, a readable short printable string at `+0x20`,
and a null-or-valid pointer at `+0x38` — is far too loose. It matched string
tables and text markup tokens: `GER`, `ESP`, `KOR`, `BGCOLOR_`, `/COL`, `NOISE`,
`B`, `/I`. `ContinueState` was not among them, at startup or fifteen and thirty
seconds later once the title screen had settled.

That leaves three honest possibilities, and they should be distinguished before
more code is written:

1. `ContinueState` is not a StateObject at all. Its RTTI exists, but nothing has
   yet shown it living in the StateObject chain.
2. It exists only while a continue is in progress, so it can never be found from
   the title screen — making it the wrong handle for this feature entirely.
3. The shape assumption is wrong. The 0x40 layout comes from AutomataMP and is
   for a different build.

The way to settle it without guessing is to stop looking for the object and look
for the code instead: find what writes or reads `ContinueState`'s vtable, or
breakpoint the save-load path once and see what actually runs when Start Game is
chosen. Both are observation rather than inference, which is what has worked
every other time on this project.
