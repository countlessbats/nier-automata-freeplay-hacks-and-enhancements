// dump_image.exe <process-name> <output-file> [timeout-seconds]
// Waits for the process, waits until its main module contains the known
// unpacked-runtime signature, then writes the module in image layout
// (file offset == RVA) with section headers rewritten to match.
#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
DWORD find_process(const wchar_t* name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{sizeof(entry)};
    DWORD pid{};
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, name) == 0) { pid = entry.th32ProcessID; break; }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pid;
}

bool contains_signature(const std::vector<unsigned char>& image) {
    static constexpr unsigned char pattern[] = {
        0x44,0x8B,0x0D,0x00,0x00,0x00,0x00,0x44,
        0x39,0x0D,0x00,0x00,0x00,0x00,0x75,0x24};
    static constexpr char mask[] = "xxx????xxx????xx";
    if (image.size() < sizeof(pattern)) return false;
    for (size_t i = 0; i + sizeof(pattern) <= image.size(); ++i) {
        bool match = true;
        for (size_t p = 0; p < sizeof(pattern); ++p) {
            if (mask[p] != '?' && image[i + p] != pattern[p]) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        fwprintf(stderr, L"usage: dump_image.exe <process-name> <output-file> [timeout-seconds]\n");
        return 2;
    }
    const wchar_t* process_name = argv[1];
    const wchar_t* output_path = argv[2];
    const int timeout_seconds = argc > 3 ? _wtoi(argv[3]) : 120;
    const ULONGLONG deadline = GetTickCount64() + 1000ULL * timeout_seconds;

    DWORD pid{};
    while (!(pid = find_process(process_name))) {
        if (GetTickCount64() > deadline) { fwprintf(stderr, L"timeout waiting for process\n"); return 3; }
        Sleep(250);
    }
    wprintf(L"pid %lu\n", pid);

    HANDLE process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!process) { fwprintf(stderr, L"OpenProcess failed (%lu)\n", GetLastError()); return 4; }

    HMODULE main_module{};
    DWORD needed{};
    while (!EnumProcessModulesEx(process, &main_module, sizeof(main_module), &needed,
                                 LIST_MODULES_64BIT) || !main_module) {
        if (GetTickCount64() > deadline) { fwprintf(stderr, L"timeout enumerating modules\n"); return 5; }
        Sleep(250);
    }
    MODULEINFO info{};
    if (!GetModuleInformation(process, main_module, &info, sizeof(info))) {
        fwprintf(stderr, L"GetModuleInformation failed (%lu)\n", GetLastError());
        return 6;
    }
    const auto base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
    wprintf(L"base %p size 0x%lX\n", info.lpBaseOfDll, info.SizeOfImage);

    std::vector<unsigned char> image(info.SizeOfImage);
    while (true) {
        for (size_t offset = 0; offset < image.size(); offset += 0x1000) {
            SIZE_T read{};
            ReadProcessMemory(process, reinterpret_cast<void*>(base + offset),
                              image.data() + offset,
                              std::min<size_t>(0x1000, image.size() - offset), &read);
        }
        if (contains_signature(image)) break;
        if (GetTickCount64() > deadline) { fwprintf(stderr, L"timeout waiting for unpack\n"); return 7; }
        Sleep(500);
    }
    wprintf(L"unpacked signature present; dumping\n");

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image.data() + dos->e_lfanew);
    auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section) {
        section->PointerToRawData = section->VirtualAddress;
        section->SizeOfRawData = section->Misc.VirtualSize;
    }

    FILE* file{};
    if (_wfopen_s(&file, output_path, L"wb") || !file) {
        fwprintf(stderr, L"cannot open output file\n");
        return 8;
    }
    fwrite(image.data(), 1, image.size(), file);
    fclose(file);
    wprintf(L"wrote %llu bytes; runtime base 0x%llX preferred 0x%llX\n",
            static_cast<unsigned long long>(image.size()),
            static_cast<unsigned long long>(base),
            static_cast<unsigned long long>(nt->OptionalHeader.ImageBase));
    CloseHandle(process);
    return 0;
}
