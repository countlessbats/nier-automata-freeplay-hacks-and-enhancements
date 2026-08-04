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
    // A and D never fired and are dropped. B turned out to be setup, called once
    // when the screen opens; C handles mouse hover and page scrolling. Neither
    // moves the cursor, so the up and down handler is elsewhere.
    {0x98AA30, "chip grid setup"},
    {0x98BBFC, "chip grid mouse/page"},
    // Already known to wrap both ways, and known to serve the left hand list. If
    // it also serves the storage grid then the wrap is present there too and the
    // panel not looping is a different bug entirely, which is worth knowing
    // before writing a patch for the wrong thing.
    {0x98C347, "list nav (wraps already)"},
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

// A breakpoint is only useful on a real function entry. Several of these
// addresses are chained chunks, reached by a jump from elsewhere in the same
// function, so nothing has been pushed and the top of the stack is not a return
// address; that is what produced the nonsense caller in the first run. The
// unwind data knows where each chunk's owning function starts, so it is asked.
uintptr_t primary_entry(uintptr_t address) {
    for (int hop = 0; hop < 8; ++hop) {
        DWORD64 image_base = 0;
        auto* function = RtlLookupFunctionEntry(address, &image_base, nullptr);
        if (!function || !image_base) return address;
        const uintptr_t begin = static_cast<uintptr_t>(image_base) + function->BeginAddress;
        auto* unwind = reinterpret_cast<const unsigned char*>(image_base + function->UnwindData);
        const unsigned char flags = static_cast<unsigned char>(unwind[0] >> 3);
        if (!(flags & 0x4)) return begin;   // UNW_FLAG_CHAININFO
        // The chained RUNTIME_FUNCTION follows the unwind codes, which are
        // padded to an even count.
        const unsigned char codes = unwind[2];
        const size_t offset = 4 + ((static_cast<size_t>(codes) + 1) & ~size_t{1}) * 2;
        const auto* chained =
            reinterpret_cast<const RUNTIME_FUNCTION*>(unwind + offset);
        const uintptr_t parent = static_cast<uintptr_t>(image_base) + chained->BeginAddress;
        if (parent == begin || !chained->BeginAddress) return begin;
        address = parent;
    }
    return address;
}

bool install_call_probe() {
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

    for (const Site& site : kSites) {
        const uintptr_t requested = g_base + site.rva;
        const uintptr_t address = primary_entry(requested);
        if (address != requested)
            log_line("Call probe: %s at +0x%X belongs to the function at +0x%llX; "
                     "watching that instead", site.label, site.rva,
                     static_cast<unsigned long long>(address - g_base));
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
