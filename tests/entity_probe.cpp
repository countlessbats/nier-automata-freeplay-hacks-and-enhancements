#include <Windows.h>
#include <TlHelp32.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

template <typename T> bool read(HANDLE process, uintptr_t address, T& value) {
    SIZE_T done{};
    return ReadProcessMemory(process, reinterpret_cast<void*>(address), &value,
                             sizeof(value), &done) && done == sizeof(value);
}

int main() {
    PROCESSENTRY32W process_entry{sizeof(process_entry)};
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    DWORD pid{};
    for (BOOL ok = Process32FirstW(snapshot, &process_entry); ok;
         ok = Process32NextW(snapshot, &process_entry)) {
        if (_wcsicmp(process_entry.szExeFile, L"NieRAutomata.exe") == 0) {
            pid = process_entry.th32ProcessID;
            break;
        }
    }
    CloseHandle(snapshot);
    if (!pid) { std::puts("NieRAutomata.exe is not running"); return 2; }
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) { std::printf("OpenProcess failed: %lu\n", GetLastError()); return 3; }

    MODULEENTRY32W module{sizeof(module)};
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (!Module32FirstW(snapshot, &module)) return 4;
    CloseHandle(snapshot);
    const uintptr_t base = reinterpret_cast<uintptr_t>(module.modBaseAddr);
    std::vector<unsigned char> image(module.modBaseSize);
    SIZE_T bytes{};
    if (!ReadProcessMemory(process, module.modBaseAddr, image.data(), image.size(), &bytes)) return 5;

    const unsigned char pattern[] = {0x44,0x8B,0x0D,0,0,0,0,0x44,0x39,0x0D,0,0,0,0,0x75,0x24};
    uintptr_t found{};
    for (size_t i = 0; i + sizeof(pattern) <= bytes; ++i) {
        if (memcmp(image.data() + i, pattern, 3) == 0 &&
            memcmp(image.data() + i + 7, pattern + 7, 3) == 0 &&
            memcmp(image.data() + i + 14, pattern + 14, 2) == 0) {
            found = base + i;
            break;
        }
    }
    if (!found) { std::puts("signature not found"); return 6; }
    int32_t displacement{};
    memcpy(&displacement, image.data() + (found - base) + 3, sizeof(displacement));
    const uintptr_t global = found + 7 + displacement;
    uintptr_t list{};
    uint32_t count{};
    read(process, global + 0x10, list);
    read(process, global + 0x04, count);
    std::printf("pid=%lu base=%p signature=+0x%llX global=+0x%llX list=%p count=%u\n",
                pid, reinterpret_cast<void*>(base), found-base, global-base,
                reinterpret_cast<void*>(list), count);
    count = std::min(count, 4096u);
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t entity{}, behavior{};
        uint32_t handle{}, health{};
        char name[33]{};
        float position[4]{};
        if (!read(process, list + i * 16ULL + 8, entity) || !entity) continue;
        SIZE_T got{};
        ReadProcessMemory(process, reinterpret_cast<void*>(entity + 8), name, 32, &got);
        read(process, entity + 0x30, handle);
        read(process, entity + 0x48, behavior);
        const bool hp_ok = behavior && read(process, behavior + 0x858, health);
        if (behavior) ReadProcessMemory(process, reinterpret_cast<void*>(behavior + 0x50),
                                        position, sizeof(position), &got);
        std::printf("[%4u] %-32s handle=%08X entity=%p behavior=%p hp=%s%u pos=(%.2f,%.2f,%.2f)\n",
                    i, name, handle, reinterpret_cast<void*>(entity), reinterpret_cast<void*>(behavior),
                    hp_ok ? "" : "?", health, position[0], position[1], position[2]);
    }
    CloseHandle(process);
    return 0;
}
