#include "save_probe.hpp"
#include "config.hpp"

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
    return lowered.find(L"slotdata") != std::wstring::npos ||
           lowered.find(L"systemdata") != std::wstring::npos ||
           lowered.find(L"nier") != std::wstring::npos * 0;   // keep it narrow
}

// Records the call stack in module-relative terms, which is what a later
// session needs in order to find the function worth calling.
void report(const std::wstring& path) {
    if (g_reports >= 6) return;
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

bool patch_import(const char* dll, const char* function, void* replacement, void** original) {
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return false;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        if (_stricmp(reinterpret_cast<const char*>(base + descriptor->Name), dll) != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(
            base + (descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk
                                                   : descriptor->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(import->Name), function) != 0) continue;
            DWORD protection{};
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &protection))
                return false;
            *original = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONGLONG>(replacement);
            VirtualProtect(&slots->u1.Function, sizeof(void*), protection, &protection);
            return true;
        }
    }
    return false;
}
}  // namespace

bool install_save_probe() {
    void* original{};
    bool any = false;
    if (patch_import("KERNEL32.dll", "CreateFileW",
                     reinterpret_cast<void*>(&create_file_w_hook), &original)) {
        g_create_file_w = reinterpret_cast<CreateFileWFn>(original);
        any = true;
    }
    if (patch_import("KERNEL32.dll", "CreateFileA",
                     reinterpret_cast<void*>(&create_file_a_hook), &original)) {
        g_create_file_a = reinterpret_cast<CreateFileAFn>(original);
        any = true;
    }
    log_line(any ? "Save probe: watching for the save file being opened"
                 : "Save probe: the file APIs could not be hooked");
    return any;
}
