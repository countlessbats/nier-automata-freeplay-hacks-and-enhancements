#include "state_system.hpp"
#include "config.hpp"

#include <Windows.h>
#include <cstring>
#include <string>

// The `hap` token categories are a singly linked list of named objects. The head
// is found the way praydog's AutomataMP finds it: the registration of the first
// category, "GlobalPhase", is followed by the store into the head pointer.
//
//   class TokenCategory {
//       void* vftable;      // +0x00
//       uint32_t crc;       // +0x08
//       uint32_t unknown;   // +0x0C
//       const char* name;   // +0x10
//       TokenCategory* next;// +0x18
//   };                      // 0x20 bytes

namespace {
constexpr size_t kNameOffset = 0x10;
constexpr size_t kNextOffset = 0x18;

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

bool read_name(uintptr_t category, std::string& out) {
    uintptr_t pointer{};
    if (!safe_read(category + kNameOffset, pointer) || !pointer) return false;
    if (!readable(pointer, 1)) return false;
    char text[65]{};
    __try {
        memcpy(text, reinterpret_cast<const void*>(pointer), 64);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    text[64] = 0;
    out.assign(text, strnlen_s(text, 64));
    // Category names are short printable identifiers, often with a leading '@'.
    if (out.empty() || out.size() > 48) return false;
    for (unsigned char c : out) if (c < 0x20 || c > 0x7E) return false;
    return true;
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
            for (size_t p = 0; p < length; ++p)
                if (mask[p] != '?' && start[i + p] != pattern[p]) { match = false; break; }
            if (match) return reinterpret_cast<uintptr_t>(start + i);
        }
    }
    return 0;
}

// Finds the head by locating any category whose name we can read, then walking
// the whole list. Rather than depend on one instruction sequence, this scans
// writable data for something shaped like a TokenCategory whose name matches a
// category the game always registers, and walks from there.
uintptr_t find_list_head() {
    static uintptr_t cached{};
    if (cached) return cached;

    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section) {
        if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        if (!(section->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        const auto start = reinterpret_cast<uintptr_t>(base + section->VirtualAddress);
        const size_t size = section->Misc.VirtualSize;
        for (size_t offset = 0; offset + 0x20 <= size; offset += sizeof(void*)) {
            const uintptr_t candidate = start + offset;
            std::string name;
            if (!read_name(candidate, name)) continue;
            if (name != "GlobalPhase") continue;
            uintptr_t next{};
            if (!safe_read(candidate + kNextOffset, next)) continue;
            if (next && !readable(next, 0x20)) continue;
            cached = candidate;
            return cached;
        }
    }
    return 0;
}
}  // namespace

uintptr_t find_token_category(const char* wanted, bool log_all) {
    const uintptr_t head = find_list_head();
    if (!head) {
        log_line("State system: the token category list was not found");
        return 0;
    }
    uintptr_t found{};
    unsigned count{};
    for (uintptr_t node = head; node && count < 512; ++count) {
        std::string name;
        if (!read_name(node, name)) break;
        if (log_all) log_line("State system: category '%s' at +0x%llX", name.c_str(),
                              static_cast<unsigned long long>(
                                  node - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))));
        if (wanted && name == wanted) found = node;
        uintptr_t next{};
        if (!safe_read(node + kNextOffset, next) || !next || !readable(next, 0x20)) break;
        node = next;
    }
    log_line("State system: walked %u token categories%s", count,
             found ? "; the wanted one was found" : "");
    return found;
}

uintptr_t find_scene_state_system() {
    const uintptr_t category = find_token_category("@SceneState", false);
    if (!category) {
        log_line("State system: @SceneState is not registered");
        return 0;
    }
    // SceneStateSystem derives from StateObject (0x40) then TokenCategory, so
    // the category address is the object's base plus 0x40.
    const uintptr_t system = category - 0x40;
    uintptr_t vtable{};
    if (!safe_read(system, vtable) || !vtable || !readable(vtable, sizeof(void*))) {
        log_line("State system: @SceneState at +0x%llX but the object below it does not "
                 "look valid",
                 static_cast<unsigned long long>(
                     category - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))));
        return 0;
    }
    log_line("State system: SceneStateSystem at +0x%llX (category +0x%llX)",
             static_cast<unsigned long long>(
                 system - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))),
             static_cast<unsigned long long>(
                 category - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))));
    return system;
}
