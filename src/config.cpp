#include "config.hpp"

#include <Windows.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace {
std::mutex g_log_mutex;

bool read_bool(const wchar_t* section, const wchar_t* key, bool fallback,
               const std::wstring& path) {
    return GetPrivateProfileIntW(section, key, fallback ? 1 : 0, path.c_str()) != 0;
}

float read_float(const wchar_t* section, const wchar_t* key, float fallback,
                 const std::wstring& path) {
    wchar_t value[64]{};
    wchar_t default_value[64]{};
    swprintf_s(default_value, L"%.4f", fallback);
    GetPrivateProfileStringW(section, key, default_value, value, 64, path.c_str());
    wchar_t* end{};
    const float result = wcstof(value, &end);
    return end == value ? fallback : result;
}
}  // namespace

std::wstring module_directory() {
    wchar_t path[MAX_PATH]{};
    HMODULE self{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&module_directory), &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring result(path);
    const auto slash = result.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : result.substr(0, slash);
}

Config load_config() {
    Config c;
    const auto path = module_directory() + L"\\NierHaptics.ini";
    c.haptics_enabled = read_bool(L"General", L"HapticsEnabled", c.haptics_enabled, path);
    c.hitstop_enabled = read_bool(L"General", L"HitstopEnabled", c.hitstop_enabled, path);
    c.hitstop_speed = std::clamp(read_float(L"Hitstop", L"HitstopSpeed", c.hitstop_speed, path), 0.01f, 1.0f);
    const int duration = static_cast<int>(GetPrivateProfileIntW(
        L"Hitstop", L"HitstopDurationMs", c.hitstop_duration_ms, path.c_str()));
    c.hitstop_duration_ms = static_cast<unsigned>(std::clamp(duration, 10, 2000));
    const int interval = static_cast<int>(GetPrivateProfileIntW(
        L"Hitstop", L"HitstopMinIntervalMs", c.hitstop_min_interval_ms, path.c_str()));
    c.hitstop_min_interval_ms = static_cast<unsigned>(std::clamp(interval, 0, 5000));
    c.hitstop_affects_enemies = read_bool(L"Hitstop", L"HitstopAffectsEnemies", c.hitstop_affects_enemies, path);
    const int window = static_cast<int>(GetPrivateProfileIntW(
        L"Haptics", L"MeleeAttributionWindowMs", c.melee_attribution_window_ms, path.c_str()));
    c.melee_attribution_window_ms = static_cast<unsigned>(std::clamp(window, 50, 5000));
    c.menu_enabled = read_bool(L"Haptics", L"MenuEnabled", c.menu_enabled, path);
    c.menu_strength = std::clamp(read_float(L"Haptics", L"MenuStrength", c.menu_strength, path), 0.0f, 1.0f);
    c.footsteps_enabled = read_bool(L"Haptics", L"FootstepsEnabled", c.footsteps_enabled, path);
    c.footstep_strength = std::clamp(read_float(L"Haptics", L"FootstepStrength", c.footstep_strength, path), 0.0f, 1.0f);
    c.footstep_player_only = read_bool(L"Haptics", L"FootstepPlayerOnly", c.footstep_player_only, path);
    c.footstep_require_moving = read_bool(L"Haptics", L"FootstepRequireMoving", c.footstep_require_moving, path);
    const int footstep_gap = static_cast<int>(GetPrivateProfileIntW(
        L"Haptics", L"FootstepMinIntervalMs", c.footstep_min_interval_ms, path.c_str()));
    c.footstep_min_interval_ms = static_cast<unsigned>(std::clamp(footstep_gap, 0, 1000));
    c.log_sound_names = read_bool(L"Diagnostics", L"LogSoundNames", c.log_sound_names, path);
    c.enemy_hit_enabled = read_bool(L"Haptics", L"EnemyHitEnabled", c.enemy_hit_enabled, path);
    c.enemy_hit_strength = std::clamp(read_float(L"Haptics", L"EnemyHitStrength", c.enemy_hit_strength, path), 0.0f, 1.0f);
    c.player_hit_enabled = read_bool(L"Haptics", L"PlayerHitEnabled", c.player_hit_enabled, path);
    c.player_hit_strength = std::clamp(read_float(L"Haptics", L"PlayerHitStrength", c.player_hit_strength, path), 0.0f, 1.0f);
    return c;
}

void log_line(const char* format, ...) {
    std::lock_guard lock(g_log_mutex);
    const auto path = module_directory() + L"\\NierHaptics.log";
    FILE* file{};
    _wfopen_s(&file, path.c_str(), L"a, ccs=UTF-8");
    if (!file) return;
    SYSTEMTIME time{};
    GetLocalTime(&time);
    fwprintf(file, L"[%02u:%02u:%02u.%03u] ", time.wHour, time.wMinute,
             time.wSecond, time.wMilliseconds);
    char message[1024]{};
    va_list args;
    va_start(args, format);
    vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);
    fwprintf(file, L"%S\n", message);
    fclose(file);
}
