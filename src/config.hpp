#pragma once

#include <string>

struct Config {
    bool haptics_enabled{true};
    // A shared hit-confirm sound only counts as the player's if one of the
    // player's own swings happened this recently.
    unsigned melee_attribution_window_ms{700};

    bool menu_enabled{true};
    float menu_strength{0.20f};
    bool footsteps_enabled{true};
    float footstep_strength{0.035f};
    // Footsteps follow the game's own footstep sounds. Companions and machines
    // walk constantly, so only sounds belonging to the player's own character
    // are used; the remaining settings guard against layered duplicates.
    // The footstep event names state the gait, so sprinting can be told from
    // walking exactly rather than inferred. Walking is the quiet, ambient case
    // where a pulse per step is mostly clutter.
    bool footsteps_sprint_only{true};
    // The game names only two gaits in play, walk and run, so jogging and
    // sprinting share an event. Its own movement speed is the only thing that
    // tells them apart. Zero disables the gate until a threshold is calibrated
    // from the speeds the log reports.
    // World units per second, measured from the position the game reports.
    // Measured over a session: jogging runs 4.5-7.2 and sprinting 7.7-14.3,
    // clustering near 10, so the line sits at 8. Set 0 to feel every step.
    float footstep_min_speed{8.0f};
    bool footstep_require_moving{true};
    unsigned footstep_min_interval_ms{70};
    bool enemy_hit_enabled{true};
    float enemy_hit_strength{0.62f};
    bool player_hit_enabled{true};
    float player_hit_strength{0.90f};

    // Repeated air jumps. The counter reaches 2 on a double jump, so holding it
    // down leaves the game believing a jump is always available. It lives in the
    // player's CharacterController (Pl0000 + 0xCA0, so 0x14A8 is +0x808 into it)
    // and was identified by the jump probe rather than assumed.
    bool multi_jump_enabled{true};
    unsigned multi_jump_offset{0x14A8};
    unsigned multi_jump_hold_value{0};
    unsigned multi_jump_sane_max{8};

    // Keep plug-in chips through death. Disables the game's death-penalty
    // routine, so equipped chips are never moved onto the corpse; the body
    // still spawns and recovering it returns nothing, so nothing duplicates.
    bool keep_chips_on_death{true};
    // Stops an equipped auto chip from taking L2 over as its own toggle. Off by
    // default: what has been patched so far does not reach the readout or the
    // button, so switching it on currently changes nothing.
    bool auto_chips_always_on{false};
    // Lets the Easy-only auto chips be equipped at any difficulty. Applied in
    // memory, so switching it off leaves anything already equipped alone.
    bool easy_chips_anywhere{false};
    // Walks the title menu with synthetic confirm presses to reach the most
    // recent save. Off by default: the presses are blind, so a menu that is not
    // laid out as expected would land on the wrong entry. Any real input
    // cancels the rest of the sequence.
    bool auto_load_last_save{false};
    unsigned auto_load_presses{2};
    unsigned auto_load_delay_ms{2500};

    // In-game panel. Off by default: the first attempt at hooking the swap
    // chain crashed the game with a stack overflow and is not yet trusted.
    bool overlay_enabled{false};

    // Reports who calls the auto-chip readout and the chip grid candidates,
    // which is the only way left to find the gate behind either.
    bool log_call_sites{true};
    // Writes every distinct sound-event name the game posts to the log, so new
    // events can be mapped without a timed in-game test.
    bool log_sound_names{true};
    // Logs every change to the save system's request, step and slot words.
    // Read-only, and the only way to learn which request loads a slot without
    // calling codes at random: several of them write to save files.
    bool log_save_state{true};
    // Read-only probe that finds the air-jump counter from real play.
    bool probe_jump_fields{true};
};

std::wstring module_directory();
std::wstring config_path();
unsigned long long config_stamp();
Config load_config();
void log_line(const char* format, ...);
