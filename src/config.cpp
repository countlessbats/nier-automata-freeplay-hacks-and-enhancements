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

unsigned read_number(const wchar_t* section, const wchar_t* key, unsigned fallback,
                    const std::wstring& path) {
    wchar_t value[64]{}, fallback_text[64]{};
    swprintf_s(fallback_text, L"0x%X", fallback);
    GetPrivateProfileStringW(section, key, fallback_text, value, 64, path.c_str());
    wchar_t* end{};
    const unsigned long result = wcstoul(value, &end, 0);  // base 0 accepts 0x
    return end == value ? fallback : static_cast<unsigned>(result);
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

std::wstring config_path() { return module_directory() + L"\\NierHaptics.ini"; }

// Last-write time of the INI, so the mod can pick up edits while running.
unsigned long long config_stamp() {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(config_path().c_str(), GetFileExInfoStandard, &data)) return 0;
    ULARGE_INTEGER value{};
    value.LowPart = data.ftLastWriteTime.dwLowDateTime;
    value.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return value.QuadPart;
}

Config load_config() {
    Config c;
    const auto path = config_path();
    c.haptics_enabled = read_bool(L"General", L"HapticsEnabled", c.haptics_enabled, path);
    const int window = static_cast<int>(GetPrivateProfileIntW(
        L"Haptics", L"MeleeAttributionWindowMs", c.melee_attribution_window_ms, path.c_str()));
    c.melee_attribution_window_ms = static_cast<unsigned>(std::clamp(window, 50, 5000));
    c.menu_enabled = read_bool(L"Haptics", L"MenuEnabled", c.menu_enabled, path);
    c.menu_strength = std::clamp(read_float(L"Haptics", L"MenuStrength", c.menu_strength, path), 0.0f, 1.0f);
    c.footsteps_enabled = read_bool(L"Haptics", L"FootstepsEnabled", c.footsteps_enabled, path);
    c.footstep_strength = std::clamp(read_float(L"Haptics", L"FootstepStrength", c.footstep_strength, path), 0.0f, 1.0f);
    c.footsteps_sprint_only = read_bool(L"Haptics", L"FootstepsSprintOnly", c.footsteps_sprint_only, path);
    c.footstep_min_speed = std::clamp(read_float(L"Haptics", L"FootstepMinSpeed", c.footstep_min_speed, path), 0.0f, 100000.0f);
    c.footstep_require_moving = read_bool(L"Haptics", L"FootstepRequireMoving", c.footstep_require_moving, path);
    const int footstep_gap = static_cast<int>(GetPrivateProfileIntW(
        L"Haptics", L"FootstepMinIntervalMs", c.footstep_min_interval_ms, path.c_str()));
    c.footstep_min_interval_ms = static_cast<unsigned>(std::clamp(footstep_gap, 0, 1000));
    c.multi_jump_enabled = read_bool(L"Gameplay", L"MultiJumpEnabled", c.multi_jump_enabled, path);
    c.multi_jump_offset = read_number(L"Gameplay", L"MultiJumpCounterOffset", c.multi_jump_offset, path);
    c.multi_jump_hold_value = read_number(L"Gameplay", L"MultiJumpHoldValue", c.multi_jump_hold_value, path);
    c.multi_jump_sane_max = read_number(L"Gameplay", L"MultiJumpSaneMax", c.multi_jump_sane_max, path);
    c.keep_chips_on_death = read_bool(L"Gameplay", L"KeepChipsOnDeath", c.keep_chips_on_death, path);
    c.easy_chips_anywhere = read_bool(L"Gameplay", L"EasyChipsAnywhere", c.easy_chips_anywhere, path);
    c.log_save_state = read_bool(L"Diagnostics", L"LogSaveState", c.log_save_state, path);
    c.log_camera_fields = read_bool(L"Diagnostics", L"LogCameraFields", c.log_camera_fields, path);
    c.auto_load_last_save = read_bool(L"Gameplay", L"AutoLoadLastSave", c.auto_load_last_save, path);
    c.auto_load_presses = read_number(L"Gameplay", L"AutoLoadPresses", c.auto_load_presses, path);
    c.auto_load_delay_ms = read_number(L"Gameplay", L"AutoLoadDelayMs", c.auto_load_delay_ms, path);
    c.overlay_enabled = read_bool(L"Overlay", L"Enabled", c.overlay_enabled, path);
    c.log_sound_names = read_bool(L"Diagnostics", L"LogSoundNames", c.log_sound_names, path);
    c.probe_jump_fields = read_bool(L"Diagnostics", L"ProbeJumpFields", c.probe_jump_fields, path);
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
