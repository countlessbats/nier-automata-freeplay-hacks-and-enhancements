#pragma once

#include <string>

struct Config {
    bool haptics_enabled{true};
    bool hitstop_enabled{true};
    float hitstop_speed{0.08f};
    unsigned hitstop_duration_ms{1000};
    float enemy_range{80.0f};

    bool menu_enabled{true};
    float menu_strength{0.22f};
    bool footsteps_enabled{true};
    float footstep_strength{0.08f};
    float footstep_distance{1.35f};
    bool enemy_hit_enabled{true};
    float enemy_hit_strength{0.62f};
    bool player_hit_enabled{true};
    float player_hit_strength{0.90f};
};

std::wstring module_directory();
Config load_config();
void log_line(const char* format, ...);
