#include "chip_keeper.hpp"
#include "config.hpp"

#include <Windows.h>

// The death penalty is a single routine (RVA 0x81A460 in build 7020666) with
// exactly one caller, in the game-over state machine. It first deletes for
// good any chip still flagged as lying on a previous corpse, then flags every
// chip equipped in the active set as "on the corpse" (chip entry +0x2C = 1)
// and unequips it, sparing only the OS-chip family (ids 0xD1A..0xD1E).
//
// The corpse the game leaves in the world carries no chip payload of its own:
// its record holds name, map, position and cosmetics, and recovering the body
// merely clears those +0x2C flags and re-equips from the entry's backup slots.
// NOPing the one call therefore keeps every chip through death, and the body
// - which still spawns normally - returns nothing when recovered, because
// nothing was ever flagged. Duplication is structurally impossible.
//
// Layouts and addresses are documented in docs/NIER-INTERNALS.md.

namespace {
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

int32_t read_rel32(uintptr_t address) {
    int32_t value{};
    memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}
}  // namespace

bool install_chip_keeper() {
    // The unique call site in the game-over state machine:
    //   lea rcx,[rip+..]   ; the save-data pointer global
    //   call death_penalty
    //   xor edx,edx
    //   lea r8d,[rdx+0x60]
    //   lea rcx,[rip+..]
    //   call memset
    //   mov [rip+..],ebx
    static constexpr unsigned char site_pattern[] = {
        0x48,0x8D,0x0D,0x00,0x00,0x00,0x00, 0xE8,0x00,0x00,0x00,0x00,
        0x33,0xD2, 0x44,0x8D,0x42,0x60, 0x48,0x8D,0x0D,0x00,0x00,0x00,0x00,
        0xE8,0x00,0x00,0x00,0x00, 0x89,0x1D,0x00,0x00,0x00,0x00};
    static constexpr char site_mask[] = "xxx????x????xxxxxxxxx????x????xx????";
    const uintptr_t site = find_pattern(site_pattern, site_mask, sizeof(site_pattern));
    if (!site) {
        log_line("Chip keeper: death-penalty call site was not found; chips will "
                 "still be lost on death");
        return false;
    }

    // The call must resolve to the death-penalty routine itself; its prologue
    // is checked byte for byte so a false signature match patches nothing.
    const uintptr_t call_at = site + 7;
    const uintptr_t target = call_at + 5 + read_rel32(call_at + 1);
    static constexpr unsigned char penalty_prologue[] = {
        0x48,0x83,0xEC,0x48, 0x48,0x89,0x5C,0x24,0x60, 0x4C,0x8B,0xC9,
        0x48,0x89,0x6C,0x24,0x68};
    if (memcmp(reinterpret_cast<const void*>(target), penalty_prologue,
               sizeof(penalty_prologue)) != 0) {
        log_line("Chip keeper: call target +0x%llX does not match the "
                 "death-penalty prologue; leaving the game untouched",
                 static_cast<unsigned long long>(
                     target - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))));
        return false;
    }

    DWORD old_protect{};
    if (!VirtualProtect(reinterpret_cast<void*>(call_at), 5, PAGE_EXECUTE_READWRITE,
                        &old_protect)) {
        log_line("Chip keeper: VirtualProtect failed (%lu)", GetLastError());
        return false;
    }
    // A single five-byte NOP in place of the call.
    static constexpr unsigned char nop5[] = {0x0F, 0x1F, 0x44, 0x00, 0x00};
    memcpy(reinterpret_cast<void*>(call_at), nop5, sizeof(nop5));
    VirtualProtect(reinterpret_cast<void*>(call_at), 5, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(call_at), 5);

    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    log_line("Chip keeper: death penalty disabled (call at +0x%llX to +0x%llX "
             "replaced with nop); chips now survive death and corpses hold none",
             static_cast<unsigned long long>(call_at - base),
             static_cast<unsigned long long>(target - base));
    return true;
}
