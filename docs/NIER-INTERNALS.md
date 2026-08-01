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
differentiating position samples.

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

## Groundwork for a multi-jump mod

The pieces above are the start of an infinite-jump mod; this is a plan, not a
finished feature.

What is already established:

- The jump is a request (`reqJump`, `+0x34` of the request struct) consumed by a
  state machine whose states are fully enumerated, including the three
  `JumpLoop_*` airborne states.
- `0x71A3BD` is a virtual function with no direct callers, so the request struct
  must be reached through the object's vtable rather than a call site.

The remaining work, in order:

1. Find the owner of the request struct. Locate `0x71A3BD` in a vtable by
   scanning `.rdata` for its address, then identify the class from the adjacent
   RTTI, which gives the offset of the struct inside `Pl0000`.
2. Find the air-jump gate. With the struct offset known, look for reads of
   `reqJump` near a comparison against a small constant, or a ground/air flag
   test near the `JumpStart_*` transitions. NieR already allows one air jump, so
   a counter compared against 1 is the likely shape.
3. Neutralize the gate. Preferred order: write the counter back to zero each
   frame from the polling thread (fully reversible, no code patching); if that
   is insufficient, patch the comparison constant; use a mid-function hook only
   as a last resort.

Reading and resetting a counter from the existing polling loop is strongly
preferred over patching code — it is reversible, survives a game update far more
gracefully, and cannot corrupt instruction boundaries.

## Timing and hitstop

The game reads **more than one clock**. Import call-site counts:

| Import | Call sites |
| --- | --- |
| `QueryPerformanceCounter` | 39 |
| `timeGetTime` (WINMM) | 17 |
| `QueryPerformanceFrequency` | 8 |
| `GetTickCount` | 3 |
| `Sleep` | 14 |

Scaling only `QueryPerformanceCounter` desynchronizes the simulation from the
frame limiter: the limiter waits for real time to catch up with the slowed
clock, so the frame rate collapses instead of the action slowing down. Any
timescale work must drive **every** imported clock from one virtual timeline.
The IAT is at RVA `0xC1B000` (size `0x7F8`).

Engine startup at `0x26C710` stores the performance frequency at `0x1769F80` and
a derived seconds-per-count at `0x1769FA8`.

If full clock virtualization ever proves insufficient, the next lead is the
engine's own delta time rather than a larger clock scale.

## Miscellaneous leads

- `is_loading` global, via `48 83 ec 28 e8 ? ? ? ? c7 05 ? ? ? ? 01 00 00 00`,
  absolute at instruction + 11.
- ~1,331 script-export names of the form `Class.method` exist as plain strings,
  including 248 `Pl0000.*` entries — a broad map of the scriptable surface.
- `hap::` is the engine namespace; `TokenCategory` / `StateObject` linked lists
  are searchable by name (`@SceneState`, `GlobalPhase`).
- Sound banks are Wwise (`.bnk`, `.wsp`); `WwiseInfo.wai` indexes them but holds
  no event names. `SoundMacro.bxm` is a data-driven name→event macro table.
