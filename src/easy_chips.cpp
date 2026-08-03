#include "easy_chips.hpp"
#include "config.hpp"

#include <Windows.h>
#include <cstdint>
#include <cstring>

// The gate is a small function that walks the chip array, stride 0x30, and
// returns 1 as soon as it finds a chip whose id falls in 0xD1A..0xD1E, the five
// auto chips. Replacing its first three bytes with `xor eax, eax; ret` makes it
// answer "this set has no auto chips", which is what the restriction hangs on.
//
// Chosen over patching the difficulty check because this touches one function
// with a single, well-understood job, and because it is reversible byte for
// byte while the game runs.

namespace {
constexpr unsigned char kPatch[3] = {0x31, 0xC0, 0xC3};   // xor eax, eax ; ret

uintptr_t g_gate{};
unsigned char g_original[3]{};
bool g_patched{};

uintptr_t find_gate() {
    if (g_gate) return g_gate;
    // 83 FA 02 77 4A 4C 8B 11 45 33 C0 49 8D 8A 7C 1F 00 00 44 8B
    static constexpr unsigned char pattern[] = {
        0x83,0xFA,0x02,0x77,0x4A,0x4C,0x8B,0x11,0x45,0x33,
        0xC0,0x49,0x8D,0x8A,0x7C,0x1F,0x00,0x00,0x44,0x8B};
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        auto* start = base + section->VirtualAddress;
        const size_t size = section->Misc.VirtualSize;
        for (size_t i = 0; i + sizeof(pattern) <= size; ++i) {
            if (memcmp(start + i, pattern, sizeof(pattern)) == 0) {
                g_gate = reinterpret_cast<uintptr_t>(start + i);
                memcpy(g_original, start + i, sizeof(g_original));
                log_line("Easy chips: auto-chip gate found at NieRAutomata.exe+0x%llX",
                         static_cast<unsigned long long>(g_gate -
                         reinterpret_cast<uintptr_t>(base)));
                return g_gate;
            }
        }
    }
    return 0;
}

bool write_bytes(uintptr_t address, const unsigned char* bytes, size_t count) {
    DWORD protection{};
    if (!VirtualProtect(reinterpret_cast<void*>(address), count,
                        PAGE_EXECUTE_READWRITE, &protection)) return false;
    memcpy(reinterpret_cast<void*>(address), bytes, count);
    VirtualProtect(reinterpret_cast<void*>(address), count, protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), count);
    return true;
}
}  // namespace

bool set_easy_chips_anywhere(bool enabled) {
    if (!find_gate()) {
        static bool complained = false;
        if (!complained) {
            complained = true;
            log_line("Easy chips: the auto-chip gate was not found; the option does nothing");
        }
        return false;
    }
    if (enabled == g_patched) return true;
    const bool ok = enabled ? write_bytes(g_gate, kPatch, sizeof(kPatch))
                            : write_bytes(g_gate, g_original, sizeof(g_original));
    if (ok) {
        g_patched = enabled;
        log_line("Easy chips: auto chips are %s at any difficulty",
                 enabled ? "now usable" : "restricted again");
    }
    return ok;
}
