#include "sound_hook.hpp"
#include "config.hpp"

#include <Windows.h>
#include <cstring>
#include <vector>

// The game posts every sound through a small family of functions that all take
// a descriptor in RCX:
//
//     struct PostDescriptor { const char* name; uint32_t id; uint32_t flags; };
//
// `name` is null when a sound is posted by precomputed event id. The four post
// variants sit at fixed offsets from the first one; we locate that one from a
// unique signature in the by-name wrapper rather than trusting a raw address,
// then derive the rest.
//
// Wwise event ids are FNV-1a-style FNV-1 32-bit hashes of the lowercased name
// (basis 0x811C9DC5, prime 0x01000193), so ids stay comparable across runs.

namespace {
constexpr size_t kRingSize = 256;

struct PostDescriptor {
    const char* name;
    uint32_t id;
    uint32_t flags;
};

struct Breakpoint {
    uintptr_t address{};
    unsigned char original{};
};

SoundEvent g_ring[kRingSize];
volatile LONG g_write_index{};
LONG g_read_index{};
std::vector<Breakpoint> g_breakpoints;
thread_local Breakpoint* g_rearm{};

Breakpoint* breakpoint_at(uintptr_t address) {
    for (auto& bp : g_breakpoints)
        if (bp.address == address) return &bp;
    return nullptr;
}

// Copies at most `limit - 1` characters out of a pointer that may be invalid or
// may point at a caller's stack buffer. Runs inside the exception handler, so it
// must not allocate or take locks.
void copy_name(const char* source, char* destination, size_t limit) {
    destination[0] = 0;
    if (!source) return;
    __try {
        size_t i = 0;
        for (; i + 1 < limit; ++i) {
            const char c = source[i];
            if (!c) break;
            destination[i] = c;
        }
        destination[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        destination[0] = 0;
    }
}

void record(const PostDescriptor* descriptor) {
    if (!descriptor) return;
    uint32_t id{};
    const char* name{};
    __try {
        id = descriptor->id;
        name = descriptor->name;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    const LONG slot = InterlockedIncrement(&g_write_index) - 1;
    SoundEvent& event = g_ring[static_cast<size_t>(slot) % kRingSize];
    event.id = id;
    copy_name(name, event.name, sizeof(event.name));
}

LONG CALLBACK sound_exception_handler(EXCEPTION_POINTERS* exception) {
    auto* record_info = exception->ExceptionRecord;
    auto* context = exception->ContextRecord;
    if (record_info->ExceptionCode == EXCEPTION_BREAKPOINT) {
        auto* bp = breakpoint_at(reinterpret_cast<uintptr_t>(record_info->ExceptionAddress));
        if (!bp) return EXCEPTION_CONTINUE_SEARCH;
        record(reinterpret_cast<const PostDescriptor*>(context->Rcx));
        *reinterpret_cast<volatile unsigned char*>(bp->address) = bp->original;
        context->Rip = bp->address;
        context->EFlags |= 0x100;
        g_rearm = bp;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (record_info->ExceptionCode == EXCEPTION_SINGLE_STEP && g_rearm) {
        *reinterpret_cast<volatile unsigned char*>(g_rearm->address) = 0xCC;
        context->EFlags &= ~0x100UL;
        g_rearm = nullptr;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
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
            for (size_t p = 0; p < length; ++p) {
                if (mask[p] != '?' && start[i + p] != pattern[p]) { match = false; break; }
            }
            if (match) return reinterpret_cast<uintptr_t>(start + i);
        }
    }
    return 0;
}

// push rbx / sub rsp,0x30 / mov ebx,edx / mov [rsp+0x20],rcx / xor edx,edx /
// call hash / lea rcx,[rsp+0x20] / mov [rsp+0x28],eax / mov [rsp+0x2C],ebx / call post
uintptr_t find_post_by_descriptor() {
    static constexpr unsigned char pattern[] = {
        0x53,0x48,0x83,0xEC,0x30,0x8B,0xDA,0x48,0x89,0x4C,0x24,0x20,0x33,0xD2,
        0xE8,0x00,0x00,0x00,0x00,0x48,0x8D,0x4C,0x24,0x20,0x89,0x44,0x24,0x28,
        0x89,0x5C,0x24,0x2C,0xE8};
    static constexpr char mask[] = "xxxxxxxxxxxxxx?????xxxxxxxxxxxxxx";
    static_assert(sizeof(mask) - 1 == sizeof(pattern), "signature mask must cover every byte");
    const uintptr_t anchor = find_pattern(pattern, mask, sizeof(pattern));
    if (!anchor) return 0;
    const uintptr_t call_site = anchor + 32;           // the trailing call
    int32_t displacement{};
    memcpy(&displacement, reinterpret_cast<const void*>(call_site + 1), sizeof(displacement));
    return call_site + 5 + displacement;
}
}  // namespace

bool install_sound_hook() {
    const uintptr_t post = find_post_by_descriptor();
    if (!post) {
        log_line("Sound hook: post-event signature was not found; sound-driven haptics are off");
        return false;
    }
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    log_line("Sound hook: post-event entry at NieRAutomata.exe+0x%llX",
             static_cast<unsigned long long>(post - reinterpret_cast<uintptr_t>(base)));

    // The four descriptor-taking post variants, at fixed offsets from the first.
    // Each prologue is verified before it is armed so a different game build
    // cannot leave a breakpoint in the middle of an unrelated instruction.
    struct Variant { ptrdiff_t delta; unsigned char first; unsigned char second; };
    static constexpr Variant kVariants[] = {
        {0x000, 0x40, 0x56}, {0x1C0, 0x40, 0x57},
        {0x360, 0x40, 0x57}, {0x690, 0x40, 0x57}};

    for (const Variant& variant : kVariants) {
        const uintptr_t address = post + variant.delta;
        unsigned char bytes[2]{};
        __try {
            bytes[0] = reinterpret_cast<const unsigned char*>(address)[0];
            bytes[1] = reinterpret_cast<const unsigned char*>(address)[1];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (bytes[0] != variant.first || bytes[1] != variant.second) {
            log_line("Sound hook: variant +0x%llX has an unexpected prologue (%02X %02X); skipped",
                     static_cast<unsigned long long>(variant.delta), bytes[0], bytes[1]);
            continue;
        }
        g_breakpoints.push_back({address, bytes[0]});
    }
    if (g_breakpoints.empty()) {
        log_line("Sound hook: no post-event variant matched; sound-driven haptics are off");
        return false;
    }
    if (!AddVectoredExceptionHandler(1, &sound_exception_handler)) {
        g_breakpoints.clear();
        log_line("Sound hook: failed to register the exception handler");
        return false;
    }
    for (auto& bp : g_breakpoints) {
        DWORD old_protection{};
        if (!VirtualProtect(reinterpret_cast<void*>(bp.address), 1,
                            PAGE_EXECUTE_READWRITE, &old_protection)) continue;
        *reinterpret_cast<volatile unsigned char*>(bp.address) = 0xCC;
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    log_line("Sound hook: watching %llu post-event entr%s",
             static_cast<unsigned long long>(g_breakpoints.size()),
             g_breakpoints.size() == 1 ? "y" : "ies");
    return true;
}

bool pop_sound_event(SoundEvent& out) {
    const LONG written = g_write_index;
    if (g_read_index >= written) return false;
    // If the producer lapped us, skip ahead to the oldest entry still present.
    if (written - g_read_index > static_cast<LONG>(kRingSize))
        g_read_index = written - static_cast<LONG>(kRingSize);
    out = g_ring[static_cast<size_t>(g_read_index) % kRingSize];
    ++g_read_index;
    return true;
}
