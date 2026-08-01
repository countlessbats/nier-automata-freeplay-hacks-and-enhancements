#include "timescale.hpp"
#include "config.hpp"

#include <Windows.h>
#include <winnt.h>
#include <algorithm>

namespace {
using QpcFn = BOOL(WINAPI*)(LARGE_INTEGER*);
QpcFn g_real_qpc = ::QueryPerformanceCounter;
SRWLOCK g_clock_lock = SRWLOCK_INIT;
LONGLONG g_real_anchor{};
LONGLONG g_virtual_anchor{};
LONGLONG g_end_real{};
LONGLONG g_frequency{};
double g_scale{1.0};

LONGLONG virtual_now_locked(LONGLONG real) {
    return g_virtual_anchor + static_cast<LONGLONG>((real - g_real_anchor) * g_scale);
}

BOOL WINAPI scaled_qpc(LARGE_INTEGER* value) {
    LARGE_INTEGER real{};
    if (!g_real_qpc(&real)) return FALSE;
    AcquireSRWLockExclusive(&g_clock_lock);
    if (g_scale != 1.0 && real.QuadPart >= g_end_real) {
        g_virtual_anchor = virtual_now_locked(g_end_real);
        g_real_anchor = g_end_real;
        g_scale = 1.0;
    }
    value->QuadPart = virtual_now_locked(real.QuadPart);
    ReleaseSRWLockExclusive(&g_clock_lock);
    return TRUE;
}

bool patch_main_import(const char* dll_name, const char* function_name, void* replacement,
                       void** original) {
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!base) return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const char* imported_dll = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(imported_dll, dll_name) != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(import->Name), function_name) != 0) continue;
            DWORD old_protection{};
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old_protection)) return false;
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
    g_real_qpc(&now);
    g_frequency = frequency.QuadPart;
    g_real_anchor = g_virtual_anchor = now.QuadPart;
    void* original{};
    if (!patch_main_import("KERNEL32.dll", "QueryPerformanceCounter",
                           reinterpret_cast<void*>(&scaled_qpc), &original)) {
        log_line("Hitstop: QueryPerformanceCounter import was not found");
        return false;
    }
    g_real_qpc = reinterpret_cast<QpcFn>(original);
    log_line("Hitstop: performance-counter hook installed");
    return true;
}

void begin_hitstop(float scale, unsigned duration_ms) {
    LARGE_INTEGER real{};
    g_real_qpc(&real);
    AcquireSRWLockExclusive(&g_clock_lock);
    const auto current_virtual = virtual_now_locked(real.QuadPart);
    g_real_anchor = real.QuadPart;
    g_virtual_anchor = current_virtual;
    g_scale = std::clamp<double>(scale, 0.01, 1.0);
    g_end_real = std::max(g_end_real, real.QuadPart +
        (g_frequency * static_cast<LONGLONG>(duration_ms)) / 1000);
    ReleaseSRWLockExclusive(&g_clock_lock);
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

