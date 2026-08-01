#include "game_events.hpp"
#include "config.hpp"
#include "timescale.hpp"

#include <Windows.h>
#include <Xinput.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

namespace {
struct Vec3 { float x{}, y{}, z{}; };

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

bool enemy_name(const std::string& name) {
    if (name.size() < 2 || !((name[0] == 'E' && name[1] == 'm') ||
                             (name[0] == 'e' && name[1] == 'm'))) return false;
    static constexpr const char* non_characters[] = {
        "Shooting", "Bullet", "Laser", "Weapon", "Effect", "Eff"
    };
    for (const char* marker : non_characters) {
        if (name.find(marker) != std::string::npos) return false;
    }
    return true;
}

float horizontal_distance(const Vec3& a, const Vec3& b) {
    return std::hypot(a.x - b.x, a.z - b.z);
}

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

XInputGetStateFn load_xinput() {
    HMODULE module = LoadLibraryW(L"XINPUT1_4.dll");
    if (!module) return nullptr;
    return reinterpret_cast<XInputGetStateFn>(GetProcAddress(module, MAKEINTRESOURCEA(2)));
}
}  // namespace

GameEvents::GameEvents(const Config& config, Haptics& haptics)
    : config_(config), haptics_(haptics) {}

void GameEvents::run(std::atomic_bool& stop_requested) {
    uintptr_t list_global{};
    ULONGLONG last_signature_scan{};

    const auto xinput = load_xinput();
    if (xinput) log_line("Menu haptics: XInput state reader active");
    else log_line("Menu haptics: XInput 1.4 unavailable");

    std::unordered_map<uint32_t, uint32_t> health_by_handle;
    uint32_t player_health{};
    bool have_player_health{};
    Vec3 previous_position{};
    bool have_position{};
    bool have_tick{};
    bool logged_player{};
    float previous_tick{};
    ULONGLONG last_tick_advance{};
    float step_progress{};
    bool left_foot{};
    bool menu_likely{};
    WORD previous_buttons{};
    int previous_stick_direction{};
    ULONGLONG last_menu_pulse{};

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
        uintptr_t player_behavior{};
        Vec3 player_position{};
        float player_tick{};
        bool player_tick_valid{};
        bool have_player{};
        bool enemy_damaged{};

        if (list_global) {
            uintptr_t list{};
            uint32_t count{};
            if (safe_read(list_global + 0x10, list) && safe_read(list_global + 0x04, count) &&
                list && count > 0 && count <= 4096 && readable(list, count * 16ULL)) {
                std::unordered_map<uint32_t, uint32_t> current_health;
                for (uint32_t i = 0; i < count && !have_player; ++i) {
                    uintptr_t entity{}, behavior{};
                    std::string name;
                    if (safe_read(list + i * 16ULL + 8, entity) && entity &&
                        get_name(entity, name) && name == "Player" &&
                        safe_read(entity + 0x48, behavior) && behavior &&
                        safe_read(behavior + 0x50, player_position) &&
                        std::isfinite(player_position.x) && std::isfinite(player_position.y) &&
                        std::isfinite(player_position.z)) {
                        player_behavior = behavior;
                        have_player = true;
                        player_tick_valid = safe_read(behavior + 0x9C, player_tick);
                    }
                }
                for (uint32_t i = 0; i < count; ++i) {
                    uintptr_t entity{};
                    if (!safe_read(list + i * 16ULL + 8, entity) || !entity) continue;
                    std::string name;
                    if (!get_name(entity, name)) continue;
                    uintptr_t behavior{};
                    if (!safe_read(entity + 0x48, behavior) || !behavior) continue;
                    uint32_t handle{}, health{};
                    safe_read(entity + 0x30, handle);
                    if (name == "Player") {
                        player_behavior = behavior;
                        if (safe_read(behavior + 0x50, player_position) &&
                            std::isfinite(player_position.x) && std::isfinite(player_position.y) &&
                            std::isfinite(player_position.z)) have_player = true;
                        player_tick_valid = safe_read(behavior + 0x9C, player_tick) || player_tick_valid;
                        if (safe_read(behavior + 0x858, health) && health <= 100000000) {
                            if (have_player_health && health < player_health) {
                                if (config_.haptics_enabled && config_.player_hit_enabled)
                                    haptics_.play(HapticEffect::PlayerHit, config_.player_hit_strength);
                                log_line("Event: player damaged (%u -> %u)", player_health, health);
                            }
                            player_health = health;
                            have_player_health = true;
                        }
                    } else if (enemy_name(name) && handle && safe_read(behavior + 0x858, health) &&
                               health <= 100000000) {
                        current_health[handle] = health;
                        const auto old = health_by_handle.find(handle);
                        if (old != health_by_handle.end() && health < old->second && old->second > 0) {
                            Vec3 enemy_position{};
                            const bool nearby = have_player && safe_read(behavior + 0x50, enemy_position) &&
                                horizontal_distance(player_position, enemy_position) <= config_.enemy_range;
                            enemy_damaged = enemy_damaged || nearby;
                        }
                    }
                }
                health_by_handle.swap(current_health);
            }
        }

        if (enemy_damaged) {
            log_line("Event: nearby enemy damaged; applying hitstop");
            if (config_.hitstop_enabled)
                begin_hitstop(config_.hitstop_speed, config_.hitstop_duration_ms);
            if (config_.haptics_enabled && config_.enemy_hit_enabled)
                haptics_.play(HapticEffect::EnemyHit, config_.enemy_hit_strength);
        }

        if (have_player) {
            const ULONGLONG now = GetTickCount64();
            if (!logged_player) {
                log_line("Game events: local player acquired (HP %u, position %.2f/%.2f/%.2f)",
                         player_health, player_position.x, player_position.y, player_position.z);
                logged_player = true;
            }
            if (player_tick_valid &&
                (!have_tick || std::abs(player_tick - previous_tick) > 0.00001f)) {
                previous_tick = player_tick;
                last_tick_advance = now;
                have_tick = true;
            }
            menu_likely = player_tick_valid && have_tick && now - last_tick_advance >= 100;
            if (have_position) {
                const float horizontal = horizontal_distance(player_position, previous_position);
                const float vertical = std::abs(player_position.y - previous_position.y);
                if (horizontal < 0.18f && vertical < 0.045f)
                    step_progress += horizontal;
                else if (horizontal >= 3.0f) step_progress = 0.0f;
                if (step_progress >= config_.footstep_distance) {
                    step_progress = std::fmod(step_progress, config_.footstep_distance);
                    left_foot = !left_foot;
                    if (config_.haptics_enabled && config_.footsteps_enabled)
                        haptics_.play(left_foot ? HapticEffect::FootLeft : HapticEffect::FootRight,
                                      config_.footstep_strength);
                }
            }
            previous_position = player_position;
            have_position = true;
        } else {
            have_position = false;
            have_tick = false;
            step_progress = 0.0f;
            menu_likely = true;
        }

        if (xinput && config_.haptics_enabled && config_.menu_enabled) {
            XINPUT_STATE state{};
            bool connected{};
            for (DWORD user = 0; user < 4; ++user) {
                if (xinput(user, &state) == ERROR_SUCCESS) { connected = true; break; }
            }
            if (connected) {
                const WORD buttons = state.Gamepad.wButtons;
                const WORD rising = buttons & ~previous_buttons;
                int stick_direction{};
                if (state.Gamepad.sThumbLX < -18000 || state.Gamepad.sThumbLY < -18000) stick_direction = -1;
                else if (state.Gamepad.sThumbLX > 18000 || state.Gamepad.sThumbLY > 18000) stick_direction = 1;
                const ULONGLONG now = GetTickCount64();
                const bool digital_nav = (rising & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_DOWN |
                    XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_DPAD_UP)) != 0;
                const bool stick_nav = stick_direction && stick_direction != previous_stick_direction &&
                                       now - last_menu_pulse > 120;
                const bool action = menu_likely && (rising & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B));
                if (menu_likely && (digital_nav || stick_nav || action)) {
                    const bool right = (buttons & (XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_DPAD_UP)) ||
                                       stick_direction > 0 || (rising & XINPUT_GAMEPAD_A);
                    haptics_.play(right ? HapticEffect::MenuRight : HapticEffect::MenuLeft,
                                  config_.menu_strength);
                    last_menu_pulse = now;
                }
                previous_buttons = buttons;
                previous_stick_direction = stick_direction;
            }
        }
        Sleep(8);
    }
    reset_timescale();
}
