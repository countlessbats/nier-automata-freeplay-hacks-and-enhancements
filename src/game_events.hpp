#pragma once

#include "config.hpp"
#include "haptics.hpp"
#include <atomic>

class GameEvents {
public:
    GameEvents(const Config& config, Haptics& haptics);
    void run(std::atomic_bool& stop_requested);

private:
    const Config& config_;
    Haptics& haptics_;
};

