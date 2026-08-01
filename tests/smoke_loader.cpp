#include <Windows.h>
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    HMODULE module = LoadLibraryW(argv[1]);
    if (!module) {
        std::printf("LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    const char* exports[] = {"DirectInput8Create", "DllCanUnloadNow", "DllGetClassObject"};
    for (const char* name : exports) {
        if (!GetProcAddress(module, name)) {
            std::printf("Missing export: %s\n", name);
            return 1;
        }
    }
    Sleep(1500);
    std::puts("ok");
    return 0;
}
