#include "save_probe.hpp"
#include "config.hpp"
#include "iat.hpp"

#include <process.h>

#include <Windows.h>
#include <Psapi.h>
#include <cstdint>
#include <cstring>
#include <string>

namespace {
using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                      DWORD, DWORD, HANDLE);
using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                      DWORD, DWORD, HANDLE);

CreateFileWFn g_create_file_w{};
CreateFileAFn g_create_file_a{};
unsigned g_reports{};

bool looks_like_save(const std::wstring& path) {
    std::wstring lowered;
    lowered.reserve(path.size());
    for (wchar_t c : path) lowered.push_back(static_cast<wchar_t>(towlower(c)));
    // Only the save files themselves. An earlier version also tested for
    // "nier" and got the comparison wrong, so every .cpk under the game folder
    // matched and the report budget was gone before a save was ever opened.
    return lowered.find(L"slotdata") != std::wstring::npos ||
           lowered.find(L"systemdata") != std::wstring::npos ||
           lowered.find(L".sav") != std::wstring::npos;
}

// Records the call stack in module-relative terms, which is what a later
// session needs in order to find the function worth calling.
void report(const std::wstring& path) {
    if (g_reports >= 200) return;
    ++g_reports;
    void* frames[24]{};
    const USHORT captured = RtlCaptureStackBackTrace(1, 24, frames, nullptr);
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto low = reinterpret_cast<uintptr_t>(base);
    const auto high = low + nt->OptionalHeader.SizeOfImage;

    std::string trace;
    for (USHORT i = 0; i < captured; ++i) {
        const auto address = reinterpret_cast<uintptr_t>(frames[i]);
        if (address < low || address >= high) continue;
        char entry[32]{};
        _snprintf_s(entry, sizeof(entry), _TRUNCATE, " +0x%llX",
                    static_cast<unsigned long long>(address - low));
        trace += entry;
    }
    log_line("Save probe: '%ls' opened from%s", path.c_str(),
             trace.empty() ? " (no game frames captured)" : trace.c_str());
}

HANDLE WINAPI create_file_w_hook(LPCWSTR name, DWORD access, DWORD share,
                                 LPSECURITY_ATTRIBUTES security, DWORD disposition,
                                 DWORD flags, HANDLE template_file) {
    if (name && looks_like_save(name)) report(name);
    return g_create_file_w(name, access, share, security, disposition, flags, template_file);
}

HANDLE WINAPI create_file_a_hook(LPCSTR name, DWORD access, DWORD share,
                                 LPSECURITY_ATTRIBUTES security, DWORD disposition,
                                 DWORD flags, HANDLE template_file) {
    if (name) {
        const std::string narrow(name);
        std::wstring wide(narrow.begin(), narrow.end());
        if (looks_like_save(wide)) report(wide);
    }
    return g_create_file_a(name, access, share, security, disposition, flags, template_file);
}

}  // namespace

namespace {
// The save and load drivers at +0x9C7B70 and +0x9C7FC0 both switch on one
// state word, and the load driver reads a second word right after it. Watching
// the pair while a save is chosen normally gives the exact sequence a
// quick-load has to reproduce, which no amount of file hooking could show:
// the slots are all read before the list is drawn.
constexpr unsigned kStateRva = 0x1421EF4;   // 0..6, shared by both drivers
constexpr unsigned kRequestRva = 0x1421EF0;
constexpr unsigned kSubStateRva = 0x1421EF8;
constexpr unsigned kStagingRva = 0x14220C0;  // the buffer a slot is read into
constexpr unsigned kLiveRva = 0x145BF60;     // the live game data block

unsigned WINAPI watch_save_state(void*) {
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    auto* state = reinterpret_cast<volatile int*>(base + kStateRva);
    auto* sub = reinterpret_cast<volatile int*>(base + kSubStateRva);
    auto* request = reinterpret_cast<volatile int*>(base + kRequestRva);
    auto* staging = reinterpret_cast<volatile unsigned*>(base + kStagingRva);
    auto* live = reinterpret_cast<volatile unsigned*>(base + kLiveRva);
    int last_state = -999, last_sub = -999, last_request = -999;
    unsigned last_staging = 0, last_live = 0;
    for (;;) {
        const int s = *state, u = *sub, r = *request;
        const unsigned g = *staging, v = *live;
        if (s != last_state || u != last_sub || r != last_request) {
            log_line("Save state: step %d->%d slot %d->%d request %d->%d (staging %08X, live %08X)",
                     last_state, s, last_sub, u, last_request, r, g, v);
            last_state = s; last_sub = u; last_request = r;
        }
        if (g != last_staging || v != last_live) {
            log_line("Save buffers: staging %08X -> %08X, live %08X -> %08X",
                     last_staging, g, last_live, v);
            last_staging = g; last_live = v;
        }
        Sleep(1);
    }
}
}  // namespace

bool install_save_probe() {
    _beginthreadex(nullptr, 0, &watch_save_state, nullptr, 0, nullptr);
    log_line("Save probe: watching the save state machine at +0x%X", kStateRva);
    void* original{};
    bool any = false;
    if (patch_game_import("KERNEL32.dll", "CreateFileW",
                     reinterpret_cast<void*>(&create_file_w_hook), &original)) {
        g_create_file_w = reinterpret_cast<CreateFileWFn>(original);
        any = true;
    }
    if (patch_game_import("KERNEL32.dll", "CreateFileA",
                     reinterpret_cast<void*>(&create_file_a_hook), &original)) {
        g_create_file_a = reinterpret_cast<CreateFileAFn>(original);
        any = true;
    }
    log_line(any ? "Save probe: watching for the save file being opened"
                 : "Save probe: the file APIs could not be hooked");
    return any;
}
