#include "game_events.hpp"
#include "chip_keeper.hpp"
#include "config.hpp"
#include "sound_hook.hpp"

#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
struct Vec3 { float x{}, y{}, z{}; };

// Pl0000 layout, confirmed against the unpacked runtime image:
//   behavior + 0x50   position (Vector4f)
//   behavior + 0x858  current health
//   behavior + 0x85C  maximum health
//   behavior + 0xCA0  CharacterController, whose +0x794 is the movement speed
//     the game itself computes, so 0xCA0 + 0x794 = 0x1434.
constexpr uintptr_t kControllerSpeed = 0x1434;
// Pl0000 is 0x17920 bytes. The probe walks this much of it looking for the
// air-jump counter; it only ever reads.
constexpr size_t kPlayerBlockBytes = 0x17920;

template <typename T> bool safe_read(uintptr_t address, T& value) {
    __try {
        value = *reinterpret_cast<const T*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Kept separate because SEH cannot be used in a function that needs C++ object
// unwinding, which the event loop does.
bool copy_block(uintptr_t address, void* destination, size_t bytes) {
    __try {
        memcpy(destination, reinterpret_cast<const void*>(address), bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool write_dword(uintptr_t address, uint32_t value) {
    __try {
        *reinterpret_cast<volatile uint32_t*>(address) = value;
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

enum class SoundKind { Ignored, Footstep, MenuTick, MenuConfirm, MenuCancel, MeleeHit };

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

bool ends_with(const std::string& text, const char* suffix) {
    const size_t length = strlen(suffix);
    return text.size() >= length && text.compare(text.size() - length, length, suffix) == 0;
}

// The character sound player appends `_pl` to the name when the sound belongs to
// the character the player is controlling, and falls back to the bare name for
// companions and enemies. That suffix is how 2B's own footsteps are told apart
// from 9S's and from every machine walking nearby.
bool is_player_sound(const std::string& lowered) { return ends_with(lowered, "_pl"); }

bool is_footstep_name(const std::string& lowered) {
    if (contains(lowered, "stepup")) return false;
    return contains(lowered, "_step") || contains(lowered, "foot");
}

// Menu names were read out of the executable; footstep names live in the packed
// data files and were recovered by logging what the game actually posts.
SoundKind classify(const std::string& lowered) {
    if (lowered.empty()) return SoundKind::Ignored;
    if (is_footstep_name(lowered)) return SoundKind::Footstep;

    const bool menu_namespace = lowered.rfind("core_", 0) == 0 || lowered.rfind("se_", 0) == 0;
    if (!menu_namespace) return SoundKind::Ignored;
    // Melee hit-confirms. The game has one per weapon class rather than a
    // single shared sound, which is why matching only "sword" dropped unarmed
    // attacks entirely. Across the four classes:
    //   small sword   core_small_sword_hit, core_small_sword_hit_em7000
    //   large sword   core_big_sword_hit, core_blade_hit
    //   spear         core_spear_hit, core_pl_AS_black_spear_hit
    //   combat bracer core_nackle_hit, core_bare_hand_hit (+ _animal variants)
    //
    // Everything else carrying "hit" is deliberately rejected: pod and gun
    // rounds are impacts rather than actions the player took, hacking hits
    // belong to the minigame, and Emil is not the player.
    if (contains(lowered, "hit")) {
        static const char* const kNotMelee[] = {
            "shot", "bullet", "gun", "pod", "hak", "hack", "emil", "drug"};
        bool melee = true;
        for (const char* reject : kNotMelee)
            if (contains(lowered, reject)) { melee = false; break; }
        if (melee) return SoundKind::MeleeHit;
    }
    if (contains(lowered, "cancel")) return SoundKind::MenuCancel;
    if (contains(lowered, "error") || contains(lowered, "alart")) return SoundKind::MenuCancel;
    // "disicion" is the game's own spelling of the decision/confirm sound.
    if (contains(lowered, "disicion") || contains(lowered, "decide") ||
        contains(lowered, "decision")) return SoundKind::MenuConfirm;
    if (contains(lowered, "cursor") || contains(lowered, "toptab") ||
        contains(lowered, "menu_slide")) return SoundKind::MenuTick;
    return SoundKind::Ignored;
}

// `core_small_sword_hit` is a shared hit-confirm with no owner in its name, so
// the only way to tell 2B's hit from 9S's is that a hit follows the attacker's
// own swing.
//
// Only sounds carrying the player's own model prefix count. Weapon sounds such
// as `wpf000_combo_swing_01` were tried and are not usable: the companion plays
// them too, so they let 9S's swings through.
bool is_jump_sound(const std::string& lowered, const std::string& player_prefix) {
    if (player_prefix.empty() || lowered.rfind(player_prefix + "_", 0) != 0) return false;
    return contains(lowered, "jump");
}

// `core_pl_` names the player outright, so those hits need no corroboration.
bool is_player_namespaced(const std::string& lowered) {
    return lowered.rfind("core_pl_", 0) == 0;
}

bool is_attack_sound(const std::string& lowered, const std::string& player_prefix) {
    if (player_prefix.empty()) return false;
    if (lowered.rfind(player_prefix + "_", 0) != 0) return false;
    return contains(lowered, "atk") || contains(lowered, "swing");
}

// `pl0000_step_walk_L_pl` names the foot outright, so alternation is only a
// fallback for names that do not.
enum class Foot { Unknown, Left, Right };

Foot foot_of(const std::string& lowered) {
    if (contains(lowered, "_l_")) return Foot::Left;
    if (contains(lowered, "_r_")) return Foot::Right;
    if (ends_with(lowered, "_l")) return Foot::Left;
    if (ends_with(lowered, "_r")) return Foot::Right;
    return Foot::Unknown;
}

// The model prefix of whatever the player is currently controlling, e.g.
// "pl0000" for 2B. Learned from any `_pl` sound so it follows character swaps.
std::string model_prefix(const std::string& lowered) {
    const size_t underscore = lowered.find('_');
    return underscore == std::string::npos ? std::string() : lowered.substr(0, underscore);
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
    bool sound_hook_attempted{};
    bool sound_hook_active{};

    std::unordered_set<std::string> catalogued_names;
    std::string player_prefix;
    uint32_t player_health{};
    unsigned other_entities{};
    bool have_player_health{};
    bool logged_player{};
    bool left_foot{};
    ULONGLONG last_footstep{};
    ULONGLONG last_player_attack{};
    ULONGLONG last_jump_sound{};

    unsigned long long config_seen = config_stamp();
    uintptr_t player_behavior{};
    std::vector<uint32_t> grounded_snapshot;
    std::vector<uint32_t> live_snapshot;
    unsigned jump_probe_reports{};
    bool logged_multi_jump{};
    unsigned foreign_melee_hits{};

    while (!stop_requested.load()) {
        const ULONGLONG loop_time = GetTickCount64();
        // Pick up edits to the INI while the game runs, so the control panel
        // can change settings without a restart.
        if (const unsigned long long stamp = config_stamp(); stamp && stamp != config_seen) {
            config_seen = stamp;
            config_ = load_config();
            log_line("Config: reloaded settings from NierHaptics.ini");
        }
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
            // Same timing constraint as the sound hook: the code signature
            // only exists once the executable has decrypted itself.
            if (config_.keep_chips_on_death) install_chip_keeper();
        }
        Vec3 player_position{};
        bool have_player{};


        // Entity polling now exists only to notice the player taking damage.
        // Outgoing hits are read from the game's own hit-confirm sound, because
        // a health decrease somewhere in the entity list is equally true when a
        // companion or another machine lands the blow.
        if (list_global) {
            uintptr_t list{};
            uint32_t count{};
            if (safe_read(list_global + 0x10, list) && safe_read(list_global + 0x04, count) &&
                list && count > 0 && count <= 4096 && readable(list, count * 16ULL)) {
                unsigned others{};
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
                    if (name != "Player") {
                        if (valid_health) ++others;
                        continue;
                    }
                    player_behavior = behavior;
                    // Repeated air jumps: clear the jump counter so the game
                    // always believes another jump is available. Only ever the
                    // player's own object, only this one dword, and only when
                    // it holds a plausible count.
                    if (config_.multi_jump_enabled) {
                        const uintptr_t counter = behavior + config_.multi_jump_offset;
                        uint32_t jumps{};
                        if (safe_read(counter, jumps) && jumps > config_.multi_jump_hold_value &&
                            jumps <= config_.multi_jump_sane_max) {
                            write_dword(counter, config_.multi_jump_hold_value);
                            if (!logged_multi_jump) {
                                log_line("Multi-jump: jump counter at +0x%X read %u and is being "
                                         "held at %u", config_.multi_jump_offset, jumps,
                                         config_.multi_jump_hold_value);
                                logged_multi_jump = true;
                            }
                        }
                    }
                    if (safe_read(behavior + 0x50, player_position) &&
                        std::isfinite(player_position.x) && std::isfinite(player_position.y) &&
                        std::isfinite(player_position.z)) have_player = true;
                    if (!valid_health) continue;
                    if (have_player_health && health < player_health) {
                        if (config_.haptics_enabled && config_.player_hit_enabled)
                            haptics_.play(HapticEffect::PlayerHit, config_.player_hit_strength);
                        log_line("Event: player damaged (%u -> %u); PlayerHit waveform queued",
                                 player_health, health);
                    }
                    player_health = health;
                    have_player_health = true;
                }
                other_entities = others;
            }
        }

        if (have_player && !logged_player) {
            log_line("Game events: local player acquired (HP %u, position %.2f/%.2f/%.2f, "
                     "%u other health-bearing entities)", player_health, player_position.x,
                     player_position.y, player_position.z, other_entities);
            logged_player = true;
        }

        // Everything below is driven by the sounds the game actually plays.
        SoundEvent event{};
        while (sound_hook_active && pop_sound_event(event)) {
            if (!event.name[0]) continue;
            const std::string lowered = lowercase(event.name);
            const SoundKind kind = classify(lowered);
            if (is_attack_sound(lowered, player_prefix)) last_player_attack = GetTickCount64();
            if (config_.probe_jump_fields && is_jump_sound(lowered, player_prefix))
                last_jump_sound = GetTickCount64();

            // Any `_pl` sound identifies the character the player is currently
            // controlling, so the prefix follows story sections that swap it.
            if (is_player_sound(lowered)) {
                const std::string prefix = model_prefix(lowered);
                if (!prefix.empty() && prefix != player_prefix) {
                    player_prefix = prefix;
                    log_line("Game events: player character sounds are '%s'", prefix.c_str());
                }
            }
            const bool mine = is_player_sound(lowered) ||
                (!player_prefix.empty() && lowered.rfind(player_prefix + "_", 0) == 0);

            if (config_.log_sound_names && catalogued_names.size() < 400 &&
                catalogued_names.insert(lowered).second) {
                static const char* kKindNames[] = {"-", "footstep", "menu tick", "menu confirm",
                                                   "menu cancel", "melee hit"};
                log_line("Sound: %s (id 0x%08X) [%s%s]", event.name, event.id,
                         kKindNames[static_cast<int>(kind)], mine ? ", player" : "");
            }
            if (!config_.haptics_enabled) continue;
            switch (kind) {
            case SoundKind::Footstep: {
                if (!config_.footsteps_enabled) break;
                // Companions and machines walk constantly; only the player's own
                // steps ever reach the controller.
                if (!mine) break;
                if (config_.footstep_require_moving && !have_player) break;
                // The event name states the gait, so this is the game's own
                // answer rather than a guess from speed: walking steps are
                // `_step_walk_`, sprinting is `_step_run_` or `_step_dash_`.
                if (config_.footsteps_sprint_only && contains(lowered, "_walk")) break;
                const ULONGLONG now = GetTickCount64();
                if (now - last_footstep < config_.footstep_min_interval_ms) break;
                last_footstep = now;
                // The event name states which foot; alternate only when it does not.
                const Foot foot = foot_of(lowered);
                if (foot == Foot::Unknown) left_foot = !left_foot;
                else left_foot = foot == Foot::Left;
                const float strength = contains(lowered, "walk")
                    ? config_.footstep_strength * 0.8f
                    : config_.footstep_strength;
                haptics_.play(left_foot ? HapticEffect::FootLeft : HapticEffect::FootRight,
                              strength);
                break;
            }
            case SoundKind::MeleeHit: {
                const ULONGLONG now = GetTickCount64();
                const ULONGLONG age = last_player_attack ? now - last_player_attack : ~0ULL;
                if (!is_player_namespaced(lowered) &&
                    age > config_.melee_attribution_window_ms) {
                    if (++foreign_melee_hits % 10 == 1)
                        log_line("Event: melee hit ignored; no player swing in the last %llu ms",
                                 static_cast<unsigned long long>(
                                     config_.melee_attribution_window_ms));
                    break;
                }
                if (config_.enemy_hit_enabled)
                    haptics_.play(HapticEffect::EnemyHit, config_.enemy_hit_strength);
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
        // Read-only search for the air-jump counter. While the player has not
        // jumped recently the block is kept as a grounded baseline; on a jump
        // sound the same block is compared against it and any small integer
        // that stepped up by one is reported. The counter shows itself by
        // going 0 -> 1 on the first jump and 1 -> 2 on the second.
        if (config_.probe_jump_fields && player_behavior && jump_probe_reports < 12) {
            const ULONGLONG now = GetTickCount64();
            const size_t words = kPlayerBlockBytes / sizeof(uint32_t);
            if (live_snapshot.size() != words) live_snapshot.resize(words);
            const bool readable_block = readable(player_behavior, kPlayerBlockBytes) &&
                copy_block(player_behavior, live_snapshot.data(), kPlayerBlockBytes);
            if (readable_block) {
                if (last_jump_sound && now - last_jump_sound < 120 &&
                    grounded_snapshot.size() == words) {
                    std::string found;
                    unsigned hits{};
                    for (size_t i = 0; i < words && hits < 12; ++i) {
                        const uint32_t before = grounded_snapshot[i], after = live_snapshot[i];
                        if (after != before + 1 || after > 4) continue;
                        char entry[48]{};
                        _snprintf_s(entry, sizeof(entry), _TRUNCATE, " +0x%llX:%u->%u",
                                    static_cast<unsigned long long>(i * sizeof(uint32_t)),
                                    before, after);
                        found += entry;
                        ++hits;
                    }
                    if (hits) {
                        ++jump_probe_reports;
                        log_line("Jump probe: fields that stepped up on this jump:%s",
                                 found.c_str());
                    }
                    last_jump_sound = 0;
                } else if (!last_jump_sound || now - last_jump_sound > 900) {
                    grounded_snapshot = live_snapshot;
                }
            }
        }
        Sleep(4);
    }
}
