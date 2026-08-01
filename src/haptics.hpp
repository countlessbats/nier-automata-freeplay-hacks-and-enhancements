#pragma once

#include <atomic>
#include <memory>

enum class HapticEffect {
    MenuTick,     // cursor movement
    MenuConfirm,  // decision / accept
    MenuCancel,   // cancel, back, error
    FootLeft,
    FootRight,
    EnemyHit,
    PlayerHit,
};

class Haptics {
public:
    Haptics();
    ~Haptics();
    bool start();
    void stop();
    void play(HapticEffect effect, float strength);
    bool active() const { return active_.load(); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic_bool active_{false};
};

