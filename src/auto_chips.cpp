#include "auto_chips.hpp"

#include <windows.h>

#include <iterator>

#include "config.hpp"

namespace {

// The game asks one question, "does the equipped set contain an auto chip", and
// changes what L2 does when the answer is yes. The query lives at +0x7F4F80 and
// is asked from a dozen places, most of them menus that genuinely need the real
// answer, so it is answered locally at the two sites that take the button away
// rather than patched at the function itself:
//
//   +0x42CD54  gameplay. On a yes it jumps over the lock-on call at +0x42CD6E,
//              which is L2 being taken for the auto chip toggle.
//   +0x7E0F7F  the button label, choosing OPTION_CONFIG_20 (toggle auto chips)
//              over OPTION_CONFIG_05 (target enemy).
//   +0x89135F  the button availability query at +0x891240. On a yes it returns
//              zero, which is what actually takes the button away. Patching only
//              the first two left the toggle working exactly as before, because
//              neither of them is what the button itself is asked about.
//
// Answering no at both leaves L2 doing what it always did. Every other caller
// still gets the truth, so the chips keep working and the menus still show them
// normally. They also stay switched on, because L2 was the only way to turn
// them off and it no longer reaches the toggle.
constexpr unsigned kCallSites[] = {0x42CD54, 0x7E0F7F, 0x89135F};
constexpr unsigned kQueryRva = 0x7F4F80;

// A second, separate predicate: isAutoChip(id), testing the id against the
// 0xD1A..0xD1E range directly rather than scanning a set. Its callers are all
// in the HUD and menu range, and patching the set-scanning query at three call
// sites changed nothing about the corner readout or the button, so this is the
// more likely source of both. Stubbed to return false rather than answered per
// call site, because it is a leaf predicate with no other meaning.
constexpr unsigned kIsAutoChipRva = 0x7E69D0;
// xor eax, eax ; ret
constexpr unsigned char kReturnFalse[] = {0x31, 0xC0, 0xC3};
unsigned char g_is_auto_original[sizeof(kReturnFalse)]{};
constexpr size_t kCallLength = 5;

// xor eax, eax ; nop ; nop ; nop  -- same length as the call it replaces, and
// leaves the flags the following `test eax, eax` expects.
constexpr unsigned char kAnswerNo[kCallLength] = {0x31, 0xC0, 0x90, 0x90, 0x90};

unsigned char g_original[std::size(kCallSites)][kCallLength]{};
bool g_captured = false;
bool g_applied = false;

bool write_bytes(unsigned char* at, const unsigned char* bytes, size_t length) {
    DWORD protection{};
    if (!VirtualProtect(at, length, PAGE_EXECUTE_READWRITE, &protection)) return false;
    memcpy(at, bytes, length);
    VirtualProtect(at, length, protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), at, length);
    return true;
}

// Confirms the site really is a call to the query before touching it, so a
// wrong address fails loudly instead of corrupting whatever is there.
bool verify(const unsigned char* base, unsigned rva) {
    const unsigned char* at = base + rva;
    if (at[0] != 0xE8) return false;
    int displacement{};
    memcpy(&displacement, at + 1, sizeof(displacement));
    return rva + kCallLength + displacement == kQueryRva;
}

}  // namespace

bool set_auto_chips_always_on(bool enabled) {
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));

    if (!g_captured) {
        for (size_t i = 0; i < std::size(kCallSites); ++i) {
            if (!verify(base, kCallSites[i])) {
                log_line("Auto chips: +0x%X is not a call to the auto-chip query; "
                         "leaving L2 alone", kCallSites[i]);
                return false;
            }
            memcpy(g_original[i], base + kCallSites[i], kCallLength);
        }
        g_captured = true;
    }

    if (enabled == g_applied) return true;

    // Captured on the first pass so switching the option off restores exactly
    // what was there.
    static bool captured_predicate = false;
    if (!captured_predicate) {
        memcpy(g_is_auto_original, base + kIsAutoChipRva, sizeof(kReturnFalse));
        captured_predicate = true;
    }
    write_bytes(base + kIsAutoChipRva,
                enabled ? kReturnFalse : g_is_auto_original, sizeof(kReturnFalse));

    for (size_t i = 0; i < std::size(kCallSites); ++i) {
        if (!write_bytes(base + kCallSites[i],
                         enabled ? kAnswerNo : g_original[i], kCallLength))
            return false;
    }
    g_applied = enabled;
    log_line(enabled ? "Auto chips: L2 keeps lock-on and equipped auto chips stay on"
                     : "Auto chips: L2 restored to the game's own behaviour");
    return true;
}
