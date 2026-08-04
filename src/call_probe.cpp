#include "call_probe.hpp"
#include "config.hpp"

#include <Windows.h>
#include <cstring>
#include <vector>

namespace {

// The two open questions, and the addresses that answer them.
//
// +0x9870BA draws the auto-chip readout: it is the one place choosing between
// CORE_AUTO_CHIP_01 and its _KEY variant, which matches the keyboard prompt on
// screen. Whatever calls it decides whether the widget exists at all, and that
// is the gate worth patching.
//
// The rest are the candidate owners of the chip storage grid, picked because
// they write the selected index at +0x38. Only one of them should fire while
// the cursor moves in the centre-right grid.
struct Site {
    unsigned rva;
    const char* label;
};

constexpr Site kSites[] = {
    {0x9870BA, "auto-chip readout draw"},
    {0x988BFF, "chip grid candidate A"},
    {0x98AA30, "chip grid candidate B"},
    {0x98BBFC, "chip grid candidate C"},
    {0x991E00, "chip grid candidate D"},
};

constexpr size_t kRingSize = 128;
constexpr size_t kMaxCallersPerSite = 12;

struct Breakpoint {
    uintptr_t address{};
    unsigned char original{};
    const char* label{};
    // Callers already reported, so a per-frame draw does not flood the log.
    uintptr_t seen[kMaxCallersPerSite]{};
    size_t seen_count{};
};

struct Hit {
    const char* label{};
    unsigned long long caller_rva{};
};

std::vector<Breakpoint> g_breakpoints;
Hit g_ring[kRingSize];
volatile LONG g_write_index{};
LONG g_read_index{};
uintptr_t g_base{};
thread_local Breakpoint* g_rearm{};

Breakpoint* breakpoint_at(uintptr_t address) {
    for (auto& bp : g_breakpoints)
        if (bp.address == address) return &bp;
    return nullptr;
}

// Runs inside the exception handler: no allocation, no locks, no logging.
void record(Breakpoint& bp, const CONTEXT* context) {
    uintptr_t caller = 0;
    __try {
        // The return address sits at the top of the stack on entry, because the
        // breakpoint is on the function's first byte.
        caller = *reinterpret_cast<const uintptr_t*>(context->Rsp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    for (size_t i = 0; i < bp.seen_count; ++i)
        if (bp.seen[i] == caller) return;
    if (bp.seen_count < kMaxCallersPerSite) bp.seen[bp.seen_count++] = caller;

    const LONG slot = InterlockedIncrement(&g_write_index) - 1;
    Hit& hit = g_ring[static_cast<size_t>(slot) % kRingSize];
    hit.label = bp.label;
    hit.caller_rva = static_cast<unsigned long long>(caller - g_base);
}

LONG CALLBACK call_probe_handler(EXCEPTION_POINTERS* exception) {
    auto* info = exception->ExceptionRecord;
    auto* context = exception->ContextRecord;
    if (info->ExceptionCode == EXCEPTION_BREAKPOINT) {
        auto* bp = breakpoint_at(reinterpret_cast<uintptr_t>(info->ExceptionAddress));
        if (!bp) return EXCEPTION_CONTINUE_SEARCH;
        record(*bp, context);
        *reinterpret_cast<volatile unsigned char*>(bp->address) = bp->original;
        context->Rip = bp->address;
        context->EFlags |= 0x100;
        g_rearm = bp;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (info->ExceptionCode == EXCEPTION_SINGLE_STEP && g_rearm) {
        *reinterpret_cast<volatile unsigned char*>(g_rearm->address) = 0xCC;
        context->EFlags &= ~0x100UL;
        g_rearm = nullptr;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

bool install_call_probe() {
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

    for (const Site& site : kSites) {
        const uintptr_t address = g_base + site.rva;
        unsigned char first{};
        __try {
            first = *reinterpret_cast<const unsigned char*>(address);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            log_line("Call probe: +0x%X is unreadable; skipped", site.rva);
            continue;
        }
        // Already a breakpoint means something else owns it; leave it alone.
        if (first == 0xCC) {
            log_line("Call probe: +0x%X already has a breakpoint; skipped", site.rva);
            continue;
        }
        Breakpoint bp{};
        bp.address = address;
        bp.original = first;
        bp.label = site.label;
        g_breakpoints.push_back(bp);
    }
    if (g_breakpoints.empty()) return false;
    if (!AddVectoredExceptionHandler(1, &call_probe_handler)) {
        g_breakpoints.clear();
        log_line("Call probe: failed to register the exception handler");
        return false;
    }
    for (auto& bp : g_breakpoints) {
        DWORD protection{};
        if (!VirtualProtect(reinterpret_cast<void*>(bp.address), 1,
                            PAGE_EXECUTE_READWRITE, &protection)) continue;
        *reinterpret_cast<volatile unsigned char*>(bp.address) = 0xCC;
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    log_line("Call probe: watching %llu function%s for their callers",
             static_cast<unsigned long long>(g_breakpoints.size()),
             g_breakpoints.size() == 1 ? "" : "s");
    return true;
}

bool pop_call_site(const char*& label, unsigned long long& caller_rva) {
    const LONG written = g_write_index;
    if (g_read_index >= written) return false;
    if (written - g_read_index > static_cast<LONG>(kRingSize))
        g_read_index = written - static_cast<LONG>(kRingSize);
    const Hit& hit = g_ring[static_cast<size_t>(g_read_index) % kRingSize];
    label = hit.label;
    caller_rva = hit.caller_rva;
    ++g_read_index;
    return true;
}
