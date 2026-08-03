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
    // Measured: ordinary running peaks around 1000-1025, sprinting around
    // 1082-1115, so the threshold sits between them. Only ~6% apart, so this is
    // meant to be tuned live rather than trusted blindly; 0 disables the gate.
    float footstep_min_speed{1050.0f};
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

    // In-game panel. Off by default: the first attempt at hooking the swap
    // chain crashed the game with a stack overflow and is not yet trusted.
    bool overlay_enabled{false};

    // Writes every distinct sound-event name the game posts to the log, so new
    // events can be mapped without a timed in-game test.
    bool log_sound_names{true};
    // Read-only probe that finds the air-jump counter from real play.
    bool probe_jump_fields{true};
};

std::wstring module_directory();
std::wstring config_path();
unsigned long long config_stamp();
Config load_config();
void log_line(const char* format, ...);
