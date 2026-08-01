#include "timescale.hpp"
#include "config.hpp"

#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>

// Hitstop drives the game's own time-acceleration system.
//
// Earlier builds scaled the process clock instead — first QueryPerformanceCounter
// alone, then every clock the game imports, which is the technique a speedhack
// uses. Both collapsed the frame rate rather than slowing the action, and the
// second attempt showed why the idea cannot work here: NieR paces its frames off
// the same clocks it simulates from, so telling it that 8% as much time has
// passed makes the frame limiter wait roughly twelve times longer in real time.
// No choice of scale escapes that, because the limiter and the simulation read
// the same value.
//
// The engine already has the effect built in. `AccelTime` is what the game uses
// for its own slow-motion moments:
//
//     void AccelTime_request(AccelTime* self, float rate, float duration_frames,
//                            float delay_frames, int flags);
//
// with the game's own calls passing rates of 0.1 to 0.5 over 8, 16 or 30 frames.
// It takes the object's critical section, so calling it from the mod thread is
// safe, and it slows the simulation without touching frame pacing.

namespace {
using AccelTimeRequestFn = void(__fastcall*)(void*, float, float, float, int);

AccelTimeRequestFn g_accel_time_request{};
void* g_accel_time{};
LONGLONG g_frequency{1};
LONGLONG g_last_trigger{};

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

int32_t displacement_at(uintptr_t address) {
    int32_t value{};
    memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}
}  // namespace

bool install_timescale_hook() {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    g_frequency = frequency.QuadPart ? frequency.QuadPart : 1;

    // A call site of AccelTime_request, which yields both the singleton and the
    // function: movss xmm3 / lea rcx,<singleton> / movss xmm2 / movss xmm1 /
    // mov [rsp+0x20],0 / call <request>.
    static constexpr unsigned char pattern[] = {
        0xF3,0x0F,0x10,0x1D,0,0,0,0, 0x48,0x8D,0x0D,0,0,0,0,
        0xF3,0x0F,0x10,0x15,0,0,0,0, 0xF3,0x0F,0x10,0x0D,0,0,0,0,
        0xC7,0x44,0x24,0x20,0x00,0x00,0x00,0x00, 0xE8,0,0,0,0};
    static constexpr char kMask[] = "xxxx????xxx????xxxx????xxxx????xxxxxxxxx????";
    static_assert(sizeof(kMask) - 1 == sizeof(pattern), "mask must cover every byte");

    const uintptr_t site = find_pattern(pattern, kMask, sizeof(pattern));
    if (!site) {
        log_line("Hitstop: the game's time-acceleration call site was not found");
        return false;
    }
    g_accel_time = reinterpret_cast<void*>(site + 8 + 7 + displacement_at(site + 11));
    g_accel_time_request =
        reinterpret_cast<AccelTimeRequestFn>(site + 39 + 5 + displacement_at(site + 40));

    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    log_line("Hitstop: using the game's time acceleration (object +0x%llX, request +0x%llX)",
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_accel_time) -
                                             reinterpret_cast<uintptr_t>(base)),
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_accel_time_request) -
                                             reinterpret_cast<uintptr_t>(base)));
    return true;
}

bool begin_hitstop(float scale, unsigned duration_ms, unsigned minimum_interval_ms) {
    if (!g_accel_time_request || !g_accel_time) return false;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const LONGLONG gap = (g_frequency * minimum_interval_ms) / 1000;
    if (g_last_trigger && now.QuadPart - g_last_trigger < gap) return false;
    g_last_trigger = now.QuadPart;

    // The engine counts the duration in frames at 60 Hz, not milliseconds.
    const float frames = std::clamp(duration_ms * 60.0f / 1000.0f, 1.0f, 240.0f);
    g_accel_time_request(g_accel_time, std::clamp(scale, 0.01f, 1.0f), frames, 0.0f, 0);
    return true;
}

void reset_timescale() {
    // Restores normal speed immediately: rate 1.0 for a single frame.
    if (g_accel_time_request && g_accel_time)
        g_accel_time_request(g_accel_time, 1.0f, 1.0f, 0.0f, 0);
}
