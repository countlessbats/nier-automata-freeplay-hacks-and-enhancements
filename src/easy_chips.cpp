#include "easy_chips.hpp"
#include "config.hpp"

#include <Windows.h>
#include <cstdint>
#include <cstring>

// The gate is `isChipUsable` at 0x7CCE80:
//
//     eax = chipId - 0xD1A
//     if (eax <= 4)              // one of the five auto chips
//         if (difficulty() != 0) // anything but Easy
//             return 0;          // not usable
//     ...
//     return 1;
//
// The branch that skips the difficulty test for ordinary chips is a `ja` at
// +0x11. Making it unconditional sends auto chips down the same path, so they
// are never rejected for the difficulty.
//
// One byte, restored exactly when switched off, and nothing is written to the
// save: a chip already equipped stays equipped, and only becomes unequippable
// once removed.
//
// An earlier attempt patched 0x7F4F80, which merely reports whether a set
// contains an auto chip. That is not what the restriction hangs on, and it did
// nothing.

namespace {
constexpr size_t kBranchOffset = 0x11;                 // the `ja` past the check
constexpr unsigned char kPatch = 0xEB;                 // jmp, taken always
constexpr unsigned char kOriginalBranch = 0x77;        // ja

uintptr_t g_gate{};
bool g_patched{};

uintptr_t find_gate() {
    if (g_gate) return g_gate;
    // 83 FA 02 77 4A 4C 8B 11 45 33 C0 49 8D 8A 7C 1F 00 00 44 8B
    // push rbx / sub rsp,20 / lea eax,[rdx-0xD1A] / mov ebx,edx / cmp eax,4 / ja
    static constexpr unsigned char pattern[] = {
        0x40,0x53,0x48,0x83,0xEC,0x20,0x8D,0x82,0xE6,0xF2,
        0xFF,0xFF,0x8B,0xDA,0x83,0xF8,0x04,0x77};
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
                log_line("Easy chips: isChipUsable found at NieRAutomata.exe+0x%llX",
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
    const unsigned char byte = enabled ? kPatch : kOriginalBranch;
    const bool ok = write_bytes(g_gate + kBranchOffset, &byte, 1);
    if (ok) {
        g_patched = enabled;
        log_line("Easy chips: auto chips are %s at any difficulty",
                 enabled ? "now usable" : "restricted again");
    }
    return ok;
}
