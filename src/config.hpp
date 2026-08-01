#pragma once

#include <string>

struct Config {
    bool haptics_enabled{true};
    bool hitstop_enabled{true};
    float hitstop_speed{0.35f};
    unsigned hitstop_duration_ms{130};
    unsigned hitstop_min_interval_ms{240};

    bool menu_enabled{true};
    float menu_strength{0.20f};
    bool footsteps_enabled{true};
    float footstep_strength{0.035f};
    // Footsteps follow the game's own footstep sounds. This gate only rejects
    // footstep sounds belonging to other characters while the player is still.
    bool footstep_require_moving{true};
    float footstep_speed_threshold{0.35f};
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
