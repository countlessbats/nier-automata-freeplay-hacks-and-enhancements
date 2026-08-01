#pragma once

#include <cstdint>

// Observes the game's own Wwise post-event calls so haptics can be driven by
// real sound events instead of inferred from movement or controller input.
struct SoundEvent {
    uint32_t id{};
    char name[56]{};
};

bool install_sound_hook();
bool pop_sound_event(SoundEvent& out);
