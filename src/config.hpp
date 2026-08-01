#pragma once

#include <string>

struct Config {
    bool haptics_enabled{true};
    bool hitstop_enabled{true};
    float hitstop_speed{0.35f};
    unsigned hitstop_duration_ms{130};
    unsigned hitstop_min_interval_ms{240};
    // The engine's own time acceleration only slows the player, so enemies are
    // slowed alongside by scaling their animation rate for the same window.
    bool hitstop_affects_enemies{true};
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
    bool footstep_player_only{true};
    bool footstep_require_moving{true};
    unsigned footstep_min_interval_ms{70};
    bool enemy_hit_enabled{true};
    float enemy_hit_strength{0.62f};
    bool player_hit_enabled{true};
    float player_hit_strength{0.90f};

    // Writes every distinct sound-event name the game posts to the log, so new
    // events can be mapped without a timed in-game test.
    bool log_sound_names{true};
};

std::wstring module_directory();
Config load_config();
void log_line(const char* format, ...);
