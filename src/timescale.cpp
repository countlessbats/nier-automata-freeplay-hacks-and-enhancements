#include "timescale.hpp"
#include "config.hpp"

#include <Windows.h>
#include <winnt.h>
#include <algorithm>

// Hitstop works by handing the game a slowed virtual clock.
//
// 1.0.3 scaled only QueryPerformanceCounter. The game also reads timeGetTime
// and GetTickCount, so the simulation and the frame limiter disagreed about how
// much time had passed: the limiter waited for real time to catch up with the
// slowed clock, which starved the frame rate instead of slowing the action.
// Every clock the game imports is now driven from one virtual timeline.

namespace {
using QpcFn = BOOL(WINAPI*)(LARGE_INTEGER*);
using TimeGetTimeFn = DWORD(WINAPI*)();
using GetTickCountFn = DWORD(WINAPI*)();

QpcFn g_real_qpc = ::QueryPerformanceCounter;
TimeGetTimeFn g_real_time_get_time{};
GetTickCountFn g_real_get_tick_count = ::GetTickCount;

SRWLOCK g_clock_lock = SRWLOCK_INIT;
LONGLONG g_real_anchor{};
LONGLONG g_virtual_anchor{};
LONGLONG g_end_real{};
LONGLONG g_frequency{1};
LONGLONG g_last_trigger_real{};
double g_scale{1.0};

// Bases that keep the millisecond clocks continuous with the virtual timeline.
LONGLONG g_ms_virtual_base{};
DWORD g_time_get_time_base{};
DWORD g_get_tick_count_base{};

LONGLONG virtual_now_locked(LONGLONG real) {
    return g_virtual_anchor + static_cast<LONGLONG>((real - g_real_anchor) * g_scale);
}

// Expires a finished hitstop and returns the current virtual counter.
LONGLONG advance_locked(LONGLONG real) {
    if (g_scale != 1.0 && real >= g_end_real) {
        g_virtual_anchor = virtual_now_locked(g_end_real);
        g_real_anchor = g_end_real;
        g_scale = 1.0;
    }
    return virtual_now_locked(real);
}

LONGLONG virtual_counter() {
    LARGE_INTEGER real{};
    if (!g_real_qpc(&real)) return 0;
    AcquireSRWLockExclusive(&g_clock_lock);
    const LONGLONG value = advance_locked(real.QuadPart);
    ReleaseSRWLockExclusive(&g_clock_lock);
    return value;
}

DWORD virtual_milliseconds(DWORD base) {
    const LONGLONG elapsed = virtual_counter() - g_ms_virtual_base;
    return base + static_cast<DWORD>((elapsed * 1000) / g_frequency);
}

BOOL WINAPI scaled_qpc(LARGE_INTEGER* value) {
    LARGE_INTEGER real{};
    if (!g_real_qpc(&real)) return FALSE;
    AcquireSRWLockExclusive(&g_clock_lock);
    value->QuadPart = advance_locked(real.QuadPart);
    ReleaseSRWLockExclusive(&g_clock_lock);
    return TRUE;
}

DWORD WINAPI scaled_time_get_time() { return virtual_milliseconds(g_time_get_time_base); }

DWORD WINAPI scaled_get_tick_count() { return virtual_milliseconds(g_get_tick_count_base); }

bool patch_main_import(const char* dll_name, const char* function_name, void* replacement,
                       void** original) {
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!base) return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return false;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const char* imported_dll = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(imported_dll, dll_name) != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(
            base + (descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk
                                                   : descriptor->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(import->Name), function_name) != 0) continue;
            DWORD old_protection{};
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old_protection))
                return false;
            *original = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONGLONG>(replacement);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old_protection, &old_protection);
            FlushInstructionCache(GetCurrentProcess(), &slots->u1.Function, sizeof(void*));
            return true;
        }
    }
    return false;
}
}  // namespace

bool install_timescale_hook() {
    LARGE_INTEGER frequency{}, now{};
    QueryPerformanceFrequency(&frequency);
    g_frequency = frequency.QuadPart ? frequency.QuadPart : 1;
    g_real_qpc(&now);
    g_real_anchor = g_virtual_anchor = g_ms_virtual_base = now.QuadPart;

    void* original{};
    if (!patch_main_import("KERNEL32.dll", "QueryPerformanceCounter",
                           reinterpret_cast<void*>(&scaled_qpc), &original)) {
        log_line("Hitstop: QueryPerformanceCounter import was not found");
        return false;
    }
    g_real_qpc = reinterpret_cast<QpcFn>(original);

    // The millisecond clocks only matter if the game actually imports them.
    if (HMODULE winmm = GetModuleHandleW(L"WINMM.dll")) {
        g_real_time_get_time =
            reinterpret_cast<TimeGetTimeFn>(GetProcAddress(winmm, "timeGetTime"));
    }
    g_time_get_time_base = g_real_time_get_time ? g_real_time_get_time() : 0;
    g_get_tick_count_base = g_real_get_tick_count();

    unsigned scaled_clocks = 1;
    void* replaced{};
    if (patch_main_import("WINMM.dll", "timeGetTime",
                          reinterpret_cast<void*>(&scaled_time_get_time), &replaced)) {
        g_real_time_get_time = reinterpret_cast<TimeGetTimeFn>(replaced);
        g_time_get_time_base = g_real_time_get_time();
        ++scaled_clocks;
    }
    if (patch_main_import("KERNEL32.dll", "GetTickCount",
                          reinterpret_cast<void*>(&scaled_get_tick_count), &replaced)) {
        g_real_get_tick_count = reinterpret_cast<GetTickCountFn>(replaced);
        g_get_tick_count_base = g_real_get_tick_count();
        ++scaled_clocks;
    }
    log_line("Hitstop: virtual clock installed over %u game time source%s",
             scaled_clocks, scaled_clocks == 1 ? "" : "s");
    return true;
}

bool begin_hitstop(float scale, unsigned duration_ms, unsigned minimum_interval_ms) {
    LARGE_INTEGER real{};
    g_real_qpc(&real);
    AcquireSRWLockExclusive(&g_clock_lock);
    const LONGLONG minimum_gap = (g_frequency * minimum_interval_ms) / 1000;
    if (g_last_trigger_real && real.QuadPart - g_last_trigger_real < minimum_gap) {
        ReleaseSRWLockExclusive(&g_clock_lock);
        return false;
    }
    const LONGLONG current_virtual = advance_locked(real.QuadPart);
    g_real_anchor = real.QuadPart;
    g_virtual_anchor = current_virtual;
    g_scale = std::clamp<double>(scale, 0.01, 1.0);
    // Assignment, not max(): repeated hits must not accumulate into a stall.
    g_end_real = real.QuadPart + (g_frequency * static_cast<LONGLONG>(duration_ms)) / 1000;
    g_last_trigger_real = real.QuadPart;
    ReleaseSRWLockExclusive(&g_clock_lock);
    return true;
}

void reset_timescale() {
    LARGE_INTEGER real{};
    g_real_qpc(&real);
    AcquireSRWLockExclusive(&g_clock_lock);
    g_virtual_anchor = virtual_now_locked(real.QuadPart);
    g_real_anchor = real.QuadPart;
    g_scale = 1.0;
    g_end_real = 0;
    ReleaseSRWLockExclusive(&g_clock_lock);
}
