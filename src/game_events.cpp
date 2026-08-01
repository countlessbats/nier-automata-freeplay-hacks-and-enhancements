#include "game_events.hpp"
#include "config.hpp"
#include "damage_hook.hpp"
#include "sound_hook.hpp"
#include "timescale.hpp"

#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {
struct Vec3 { float x{}, y{}, z{}; };

// Pl0000 layout, confirmed against the unpacked runtime image:
//   behavior + 0x50   position (Vector4f)
//   behavior + 0x858  current health
//   behavior + 0x85C  maximum health
//   behavior + 0xCA0  CharacterController, whose +0x794 is the movement speed
//     the game itself computes, so 0xCA0 + 0x794 = 0x1434.
constexpr uintptr_t kControllerSpeed = 0x1434;

template <typename T> bool safe_read(uintptr_t address, T& value) {
    __try {
        value = *reinterpret_cast<const T*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readable(uintptr_t address, size_t bytes) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const auto end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return address <= end && bytes <= end - address;
}

uintptr_t find_pattern(const unsigned char* pattern, const char* mask, size_t length) {
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        auto* start = base + section->VirtualAddress;
        const size_t size = section->Misc.VirtualSize;
        for (size_t i = 0; i + length <= size; ++i) {
            bool match = true;
            for (size_t p = 0; p < length; ++p) {
                if (mask[p] != '?' && start[i + p] != pattern[p]) { match = false; break; }
            }
            if (match) return reinterpret_cast<uintptr_t>(start + i);
        }
    }
    return 0;
}

uintptr_t find_entity_list_global() {
    static constexpr unsigned char pattern[] = {
        0x44,0x8B,0x0D,0x00,0x00,0x00,0x00,0x44,
        0x39,0x0D,0x00,0x00,0x00,0x00,0x75,0x24};
    const auto found = find_pattern(pattern, "xxx????xxx????xx", sizeof(pattern));
    if (!found) return 0;
    int32_t displacement{};
    if (!safe_read(found + 3, displacement)) return 0;
    return found + 7 + displacement;
}

bool get_name(uintptr_t entity, std::string& out) {
    char name[33]{};
    if (!readable(entity + 8, 32)) return false;
    memcpy(name, reinterpret_cast<void*>(entity + 8), 32);
    name[32] = 0;
    out.assign(name, strnlen_s(name, 32));
    return !out.empty();
}

enum class SoundKind { Ignored, Footstep, MenuTick, MenuConfirm, MenuCancel };

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

// The game's sound event names are stable, lowercase-ish identifiers. Menu
// names were read out of the executable; footsteps are matched by shape because
// their names live in the packed data files rather than the executable.
SoundKind classify(const std::string& lowered) {
    if (lowered.empty()) return SoundKind::Ignored;
    if (contains(lowered, "foot")) return SoundKind::Footstep;
    if (contains(lowered, "step") && !contains(lowered, "stepup")) return SoundKind::Footstep;

    const bool menu_namespace = lowered.rfind("core_", 0) == 0 || lowered.rfind("se_", 0) == 0;
    if (!menu_namespace) return SoundKind::Ignored;
    if (contains(lowered, "cancel")) return SoundKind::MenuCancel;
    if (contains(lowered, "error") || contains(lowered, "alart")) return SoundKind::MenuCancel;
    // "disicion" is the game's own spelling of the decision/confirm sound.
    if (contains(lowered, "disicion") || contains(lowered, "decide") ||
        contains(lowered, "decision")) return SoundKind::MenuConfirm;
    if (contains(lowered, "cursor") || contains(lowered, "toptab") ||
        contains(lowered, "menu_slide")) return SoundKind::MenuTick;
    return SoundKind::Ignored;
}

std::string lowercase(const char* text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}
}  // namespace

GameEvents::GameEvents(const Config& config, Haptics& haptics)
    : config_(config), haptics_(haptics) {}

void GameEvents::run(std::atomic_bool& stop_requested) {
    uintptr_t list_global{};
    ULONGLONG last_signature_scan{};
    bool damage_hook_attempted{};
    bool sound_hook_attempted{};
    bool sound_hook_active{};

    std::unordered_map<uintptr_t, uint32_t> health_by_entity;
    std::unordered_set<std::string> catalogued_names;
    uint32_t player_health{};
    bool have_player_health{};
    bool logged_player{};
    bool logged_speed_sample{};
    float player_speed{};
    bool left_foot{};
    unsigned suppressed_hitstops{};

    while (!stop_requested.load()) {
        const ULONGLONG loop_time = GetTickCount64();
        if (!list_global && loop_time - last_signature_scan >= 500) {
            last_signature_scan = loop_time;
            list_global = find_entity_list_global();
            if (list_global)
                log_line("Game events: entity list located at NieRAutomata.exe+0x%llX",
                         static_cast<unsigned long long>(list_global -
                         reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))));
        }
        // The entity-list signature only resolves once the runtime image is
        // unpacked, which is also when it is safe to scan for anything else.
        if (list_global && !sound_hook_attempted) {
            sound_hook_attempted = true;
            sound_hook_active = install_sound_hook();
        }
        if (list_global && !damage_hook_attempted) {
            damage_hook_attempted = true;
            install_enemy_damage_hook();
        }

        Vec3 player_position{};
        bool have_player{};
        bool enemy_damaged = consume_enemy_damage_event();
        bool player_damaged{};

        if (list_global) {
            uintptr_t list{};
            uint32_t count{};
            if (safe_read(list_global + 0x10, list) && safe_read(list_global + 0x04, count) &&
                list && count > 0 && count <= 4096 && readable(list, count * 16ULL)) {
                std::unordered_map<uintptr_t, uint32_t> current_health;
                for (uint32_t i = 0; i < count; ++i) {
                    uintptr_t entity{};
                    if (!safe_read(list + i * 16ULL + 8, entity) || !entity) continue;
                    std::string name;
                    if (!get_name(entity, name)) continue;
                    uintptr_t behavior{};
                    if (!safe_read(entity + 0x48, behavior) || !behavior) continue;
                    uint32_t health{}, max_health{};
                    const bool valid_health = safe_read(behavior + 0x858, health) &&
                        safe_read(behavior + 0x85C, max_health) && max_health > 0 &&
                        max_health <= 100000000 && health <= max_health;
                    if (name == "Player") {
                        if (safe_read(behavior + 0x50, player_position) &&
                            std::isfinite(player_position.x) && std::isfinite(player_position.y) &&
                            std::isfinite(player_position.z)) have_player = true;
                        float speed{};
                        if (safe_read(behavior + kControllerSpeed, speed) &&
                            std::isfinite(speed) && speed >= 0.0f && speed < 1000.0f)
                            player_speed = speed;
                        if (valid_health) {
                            if (have_player_health && health < player_health) {
                                player_damaged = true;
                                if (config_.haptics_enabled && config_.player_hit_enabled)
                                    haptics_.play(HapticEffect::PlayerHit, config_.player_hit_strength);
                                log_line("Event: player damaged (%u -> %u); PlayerHit waveform queued",
                                         player_health, health);
                            }
                            player_health = health;
                            have_player_health = true;
                        }
                    } else if (valid_health) {
                        current_health[entity] = health;
                        const auto old = health_by_entity.find(entity);
                        if (old != health_by_entity.end() && health < old->second && old->second > 0) {
                            log_line("Event: %s damaged (%u -> %u)", name.c_str(), old->second, health);
                            enemy_damaged = true;
                        }
                    }
                }
                health_by_entity.swap(current_health);
            }
        }

        if (enemy_damaged || player_damaged) {
            const bool applied = config_.hitstop_enabled &&
                begin_hitstop(config_.hitstop_speed, config_.hitstop_duration_ms,
                              config_.hitstop_min_interval_ms);
            if (applied) log_line("Event: hit connected; hitstop applied");
            else if (config_.hitstop_enabled && ++suppressed_hitstops % 20 == 1)
                log_line("Event: hit connected; hitstop still cooling down");
            if (enemy_damaged && !player_damaged && config_.haptics_enabled &&
                config_.enemy_hit_enabled)
                haptics_.play(HapticEffect::EnemyHit, config_.enemy_hit_strength);
        }

        if (have_player && !logged_player) {
            log_line("Game events: local player acquired (HP %u, position %.2f/%.2f/%.2f, "
                     "%llu other health-bearing entities)", player_health, player_position.x,
                     player_position.y, player_position.z,
                     static_cast<unsigned long long>(health_by_entity.size()));
            logged_player = true;
        }
        if (have_player && !logged_speed_sample && player_speed > 0.5f) {
            log_line("Game events: player controller speed reads %.2f", player_speed);
            logged_speed_sample = true;
        }

        // Everything below is driven by the sounds the game actually plays.
        SoundEvent event{};
        while (sound_hook_active && pop_sound_event(event)) {
            if (!event.name[0]) continue;
            const std::string lowered = lowercase(event.name);
            const SoundKind kind = classify(lowered);
            if (config_.log_sound_names && catalogued_names.size() < 400 &&
                catalogued_names.insert(lowered).second) {
                static const char* kKindNames[] = {"-", "footstep", "menu tick",
                                                   "menu confirm", "menu cancel"};
                log_line("Sound: %s (id 0x%08X) [%s]", event.name, event.id,
                         kKindNames[static_cast<int>(kind)]);
            }
            if (!config_.haptics_enabled) continue;
            switch (kind) {
            case SoundKind::Footstep: {
                if (!config_.footsteps_enabled) break;
                if (config_.footstep_require_moving &&
                    (!have_player || player_speed < config_.footstep_speed_threshold)) break;
                left_foot = !left_foot;
                haptics_.play(left_foot ? HapticEffect::FootLeft : HapticEffect::FootRight,
                              config_.footstep_strength);
                break;
            }
            case SoundKind::MenuTick:
                if (config_.menu_enabled)
                    haptics_.play(HapticEffect::MenuTick, config_.menu_strength);
                break;
            case SoundKind::MenuConfirm:
                if (config_.menu_enabled)
                    haptics_.play(HapticEffect::MenuConfirm, config_.menu_strength);
                break;
            case SoundKind::MenuCancel:
                if (config_.menu_enabled)
                    haptics_.play(HapticEffect::MenuCancel, config_.menu_strength);
                break;
            case SoundKind::Ignored:
                break;
            }
        }
        Sleep(4);
    }
    reset_timescale();
}
