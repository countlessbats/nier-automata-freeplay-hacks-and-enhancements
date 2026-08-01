#include "config.hpp"
#include "game_events.hpp"
#include "haptics.hpp"
#include "timescale.hpp"

#include <Windows.h>
#include <atomic>
#include <memory>

namespace {
std::atomic_bool g_stop{};
HMODULE g_real_dinput{};
INIT_ONCE g_proxy_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK initialize_proxy(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t system_path[MAX_PATH]{};
    GetSystemDirectoryW(system_path, MAX_PATH);
    wcscat_s(system_path, L"\\dinput8.dll");
    g_real_dinput = LoadLibraryW(system_path);
    return g_real_dinput != nullptr;
}

FARPROC real_export(const char* name) {
    InitOnceExecuteOnce(&g_proxy_once, initialize_proxy, nullptr, nullptr);
    return g_real_dinput ? GetProcAddress(g_real_dinput, name) : nullptr;
}

DWORD WINAPI mod_thread(void*) {
    const Config config = load_config();
    log_line("NieR Haptics + Hitstop 1.0.6 starting");
    // The hitstop hook is installed from the event loop instead of here: the
    // executable's code is still encrypted at DLL load and no signature can be
    // found until the Steam stub has decrypted it.
    Haptics haptics;
    if (config.haptics_enabled) haptics.start();
    GameEvents events(config, haptics);
    events.run(g_stop);
    haptics.stop();
    log_line("NieR Haptics + Hitstop stopped");
    return 0;
}
}  // namespace

extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(
    HINSTANCE instance, DWORD version, REFIID iid, LPVOID* output, LPUNKNOWN outer) {
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    const auto function = reinterpret_cast<Fn>(real_export("DirectInput8Create"));
    return function ? function(instance, version, iid, output, outer) : E_FAIL;
}

STDAPI DllCanUnloadNow() {
    using Fn = HRESULT(WINAPI*)();
    const auto function = reinterpret_cast<Fn>(real_export("DllCanUnloadNow"));
    return function ? function() : S_FALSE;
}

STDAPI DllGetClassObject(
    REFCLSID clsid, REFIID iid, LPVOID* output) {
    using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    const auto function = reinterpret_cast<Fn>(real_export("DllGetClassObject"));
    return function ? function(clsid, iid, output) : CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllRegisterServer() {
    using Fn = HRESULT(WINAPI*)();
    const auto function = reinterpret_cast<Fn>(real_export("DllRegisterServer"));
    return function ? function() : E_NOTIMPL;
}

STDAPI DllUnregisterServer() {
    using Fn = HRESULT(WINAPI*)();
    const auto function = reinterpret_cast<Fn>(real_export("DllUnregisterServer"));
    return function ? function() : E_NOTIMPL;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (HANDLE thread = CreateThread(nullptr, 0, mod_thread, nullptr, 0, nullptr)) CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH && !reserved) {
        g_stop.store(true);
    }
    return TRUE;
}
