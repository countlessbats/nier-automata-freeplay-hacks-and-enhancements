#include "overlay.hpp"
#include "config.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <string>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// The panel is drawn from the game's render thread by way of a hook on
// IDXGISwapChain::Present. The hook is a vtable pointer swap rather than a
// patched instruction: DXGI hands every swap chain the same vtable, so writing
// one pointer reaches the game's swap chain without modifying any code.
//
// Settings are written straight back to NierHaptics.ini, which the mod already
// watches and reloads. That keeps one source of truth and avoids sharing
// mutable state between the render thread and the polling thread.

namespace {
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
// IDXGISwapChain1::Present1, vtable slot 22. CreateSwapChainForHwnd hands back a
// swap chain 1, and that is the entry point this game actually presents through.
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, const void*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT,
                                                    DXGI_FORMAT, UINT);

PresentFn g_present{};
Present1Fn g_present1{};
ResizeBuffersFn g_resize_buffers{};
ID3D11Device* g_device{};
ID3D11DeviceContext* g_context{};
ID3D11RenderTargetView* g_target{};
HWND g_window{};
WNDPROC g_original_wndproc{};
bool g_initialised{};
bool g_visible{};
bool g_failed{};
Config g_settings;
bool g_settings_loaded{};
bool g_passthrough_only{};
int g_toggle_key = VK_F10;
volatile LONG g_present_calls{};

void write_setting(const wchar_t* section, const wchar_t* key, const wchar_t* value) {
    WritePrivateProfileStringW(section, key, value, config_path().c_str());
}

void write_bool(const wchar_t* section, const wchar_t* key, bool value) {
    write_setting(section, key, value ? L"1" : L"0");
}

// Step-by-step breadcrumbs through the one-time initialisation. log_line closes
// the file every call, so the last line written survives a crash.
void step(const char* what) {
    static const char* last = nullptr;
    if (last == what) return;
    last = what;
    log_line("Overlay: %s", what);
}

void release_target() {
    if (g_target) { g_target->Release(); g_target = nullptr; }
}

bool create_target(IDXGISwapChain* swap_chain) {
    ID3D11Texture2D* back_buffer{};
    if (FAILED(swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                     reinterpret_cast<void**>(&back_buffer))) || !back_buffer)
        return false;
    const HRESULT hr = g_device->CreateRenderTargetView(back_buffer, nullptr, &g_target);
    back_buffer->Release();
    return SUCCEEDED(hr);
}

LRESULT CALLBACK overlay_wndproc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (g_visible) {
        ImGui_ImplWin32_WndProcHandler(window, message, w, l);
        // While the panel is open every keyboard and mouse message is consumed,
        // not just the ones ImGui asks for. Navigating with the arrow keys must
        // not also walk the player around.
        const bool mouse = message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
        const bool keyboard = message >= WM_KEYFIRST && message <= WM_KEYLAST;
        if (mouse || keyboard) return 1;
    }
    return CallWindowProcW(g_original_wndproc, window, message, w, l);
}

// The game does not rely on window messages alone: it polls GetAsyncKeyState and
// GetKeyState, reads the pad through XInput, and reads the keyboard through
// DirectInput. Each is answered with "nothing is pressed" while the panel is
// open, which is what actually keeps the game still.

using GetDeviceStateFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, void*);
using GetDeviceDataFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, void*, DWORD*, DWORD);
using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(void*, const GUID&, void**, void*);

CreateDeviceFn g_create_device{};

// Every DirectInput device gets its own cloned vtable, so its original functions
// have to be remembered per device rather than shared: two devices with
// different implementations would otherwise be sent to the wrong original.
struct DeviceHooks {
    void** clone{};
    GetDeviceStateFn state{};
    GetDeviceDataFn data{};
};
DeviceHooks g_devices[16]{};
volatile LONG g_device_count{};

const DeviceHooks* hooks_for(void* device) {
    auto** vtable = *reinterpret_cast<void***>(device);
    const LONG count = g_device_count;
    for (LONG i = 0; i < count && i < 16; ++i)
        if (g_devices[i].clone == vtable) return &g_devices[i];
    return nullptr;
}

HRESULT STDMETHODCALLTYPE get_device_state_hook(void* device, DWORD size, void* data) {
    const DeviceHooks* hooks = hooks_for(device);
    if (!hooks || !hooks->state) return E_FAIL;
    const HRESULT hr = hooks->state(device, size, data);
    if (g_visible && SUCCEEDED(hr) && data) memset(data, 0, size);
    return hr;
}

HRESULT STDMETHODCALLTYPE get_device_data_hook(void* device, DWORD size, void* data,
                                               DWORD* count, DWORD flags) {
    const DeviceHooks* hooks = hooks_for(device);
    if (!hooks || !hooks->data) return E_FAIL;
    const HRESULT hr = hooks->data(device, size, data, count, flags);
    if (g_visible && SUCCEEDED(hr) && count) *count = 0;
    return hr;
}

using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
using GetKeyStateFn = SHORT(WINAPI*)(int);
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, void*);
using XInputSetStateFn = DWORD(WINAPI*)(DWORD, void*);
// The game drives the rumble motors through XInputSetState. Our haptics use the
// controller's actuators instead, and having both run at once feels like mush,
// so exactly one of them is live at a time.
volatile LONG g_suppress_native_rumble{1};

GetAsyncKeyStateFn g_get_async_key_state{};
GetKeyStateFn g_get_key_state{};
XInputGetStateFn g_xinput_get_state{};
XInputSetStateFn g_xinput_set_state{};

DWORD WINAPI xinput_set_state_hook(DWORD user, void* vibration) {
    if (g_suppress_native_rumble && vibration) {
        // XINPUT_VIBRATION is two WORDs; hand the driver a silent copy rather
        // than editing the game's own struct.
        unsigned char silent[4]{};
        return g_xinput_set_state(user, silent);
    }
    return g_xinput_set_state(user, vibration);
}
SHORT WINAPI get_async_key_state_hook(int key) {
    if (g_visible) return 0;
    return g_get_async_key_state(key);
}

SHORT WINAPI get_key_state_hook(int key) {
    if (g_visible) return 0;
    return g_get_key_state(key);
}

// Synthetic confirm presses for the title menu. These ride on the same XInput
// call the game already polls, so the game cannot tell them from a real press.
// Anything the player does cancels the rest of the sequence, because a queued
// press arriving while they are navigating by hand is how a menu automation
// picks the wrong entry.
volatile LONG g_confirm_remaining{};
ULONGLONG g_confirm_next_time{};
unsigned g_confirm_gap_ms = 700;
bool g_confirm_holding{};

void queue_confirm(unsigned presses, unsigned first_delay_ms) {
    g_confirm_next_time = GetTickCount64() + first_delay_ms;
    g_confirm_holding = false;
    InterlockedExchange(&g_confirm_remaining, static_cast<LONG>(presses));
}

DWORD WINAPI xinput_get_state_hook(DWORD user, void* state) {
    const DWORD result = g_xinput_get_state(user, state);
    if (result != ERROR_SUCCESS || !state) return result;
    if (g_visible) {
        // Keep the packet number, blank the buttons and sticks.
        memset(static_cast<char*>(state) + 4, 0, 12);
        return result;
    }
    if (g_confirm_remaining > 0) {
        auto* buttons = reinterpret_cast<WORD*>(static_cast<char*>(state) + 4);
        auto* sticks = reinterpret_cast<char*>(state) + 8;
        const bool player_acted = *buttons != 0 && !g_confirm_holding;
        bool stick_moved = false;
        for (int i = 0; i < 8; ++i) if (sticks[i] != 0) stick_moved = true;
        if (player_acted || stick_moved) {
            InterlockedExchange(&g_confirm_remaining, 0);
            log_line("Auto-load: cancelled, the player is using the pad");
            return result;
        }
        const ULONGLONG now = GetTickCount64();
        if (g_confirm_holding) {
            if (now >= g_confirm_next_time) {
                g_confirm_holding = false;
                g_confirm_next_time = now + g_confirm_gap_ms;
                InterlockedDecrement(&g_confirm_remaining);
            } else {
                *buttons |= 0x1000;   // XINPUT_GAMEPAD_A
            }
        } else if (now >= g_confirm_next_time) {
            g_confirm_holding = true;
            g_confirm_next_time = now + 80;   // hold long enough to register
            *buttons |= 0x1000;
            log_line("Auto-load: confirm press (%ld remaining)", g_confirm_remaining);
        }
    }
    return result;
}


void draw_panel() {
    if (!g_settings_loaded) { g_settings = load_config(); g_settings_loaded = true; }

    ImGui::SetNextWindowSize(ImVec2(430, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);
    ImGui::Begin("NieR:Automata", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextDisabled("Press the toggle key to close. Changes apply immediately.");
    ImGui::Separator();

    if (ImGui::Checkbox("DualSense haptics", &g_settings.haptics_enabled)) {
        write_bool(L"General", L"HapticsEnabled", g_settings.haptics_enabled);
        InterlockedExchange(&g_suppress_native_rumble, g_settings.haptics_enabled ? 1 : 0);
    }
    ImGui::TextDisabled(g_settings.haptics_enabled
        ? "On: footsteps, hits and menus on the controller's actuators."
        : "Off: the game's own rumble motors, as shipped.");

    ImGui::Spacing();
    ImGui::Separator();

    if (ImGui::Checkbox("Unlimited jumps", &g_settings.multi_jump_enabled))
        write_bool(L"Gameplay", L"MultiJumpEnabled", g_settings.multi_jump_enabled);
    ImGui::TextDisabled("Jump as often as you like without landing.");

    ImGui::Spacing();
    if (ImGui::Checkbox("Keep plug-in chips on death", &g_settings.keep_chips_on_death))
        write_bool(L"Gameplay", L"KeepChipsOnDeath", g_settings.keep_chips_on_death);
    ImGui::TextDisabled("Applies on the next launch; the corpse still spawns.");

    ImGui::Spacing();
    if (ImGui::Checkbox("Easy-mode chips at any difficulty",
                        &g_settings.easy_chips_anywhere))
        write_bool(L"Gameplay", L"EasyChipsAnywhere", g_settings.easy_chips_anywhere);
    ImGui::TextDisabled("Turning this off leaves equipped chips alone.");

    ImGui::Spacing();
    if (ImGui::Checkbox("Auto-chips always on, L2 keeps lock-on",
                        &g_settings.auto_chips_always_on))
        write_bool(L"Gameplay", L"AutoChipsAlwaysOn", g_settings.auto_chips_always_on);
    ImGui::TextDisabled("Equipped auto-chips stop stealing the targeting button.");

    ImGui::Spacing();
    if (ImGui::Checkbox("Jump straight into the newest save",
                        &g_settings.auto_load_last_save))
        write_bool(L"Gameplay", L"AutoLoadLastSave", g_settings.auto_load_last_save);
    ImGui::TextDisabled("Applies on the next launch; skips the title menu entirely.");

    ImGui::Spacing();
    if (ImGui::Button("Reload from file")) { g_settings = load_config(); }
    ImGui::End();
}

// Everything the panel does per frame, shared by Present and Present1.
void render_overlay(IDXGISwapChain* swap_chain) {
    // A Present hook must never run inside itself. 1.0.14 crashed the game with
    // a stack overflow on the first frame, and whatever re-entered, this guard
    // turns that class of failure into a dropped frame instead of a crash.
    static thread_local bool inside = false;
    if (inside) return;
    inside = true;
    struct Leave { ~Leave() { inside = false; } } leave;

    const LONG call = InterlockedIncrement(&g_present_calls);
    if (call == 1) log_line("Overlay: present hook fired (swap chain %p)", swap_chain);
    if (g_passthrough_only) {
        if (call == 1) log_line("Overlay: passthrough only; not drawing");
        return;
    }

    if (!g_failed && !g_initialised) {
        step("present reached");
        if (SUCCEEDED(swap_chain->GetDevice(__uuidof(ID3D11Device),
                                            reinterpret_cast<void**>(&g_device))) && g_device) {
            step("device acquired");
            g_device->GetImmediateContext(&g_context);
            DXGI_SWAP_CHAIN_DESC desc{};
            swap_chain->GetDesc(&desc);
            g_window = desc.OutputWindow;
            log_line("Overlay: window %p, %ux%u, format %d, buffers %u, windowed %d",
                     desc.OutputWindow, desc.BufferDesc.Width, desc.BufferDesc.Height,
                     static_cast<int>(desc.BufferDesc.Format), desc.BufferCount,
                     static_cast<int>(desc.Windowed));
            if (g_window && g_context && create_target(swap_chain)) {
                step("render target created");
                IMGUI_CHECKVERSION();
                ImGui::CreateContext();
                ImGuiIO& io = ImGui::GetIO();
                io.IniFilename = nullptr;
                // Arrow keys move between controls, Enter and Space activate
                // them, Escape backs out. The game never sees any of it.
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
                // The game hides the system cursor, so ImGui draws its own.
                io.MouseDrawCursor = true;
                ImGui::StyleColorsDark();
                step("imgui context created");
                const bool win32_ready = ImGui_ImplWin32_Init(g_window);
                step(win32_ready ? "win32 backend ready" : "win32 backend failed");
                const bool dx11_ready = win32_ready && ImGui_ImplDX11_Init(g_device, g_context);
                step(dx11_ready ? "dx11 backend ready" : "dx11 backend failed");
                if (dx11_ready) {
                    g_original_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                        g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&overlay_wndproc)));
                    g_initialised = true;
                    log_line("Overlay: panel ready on the game's swap chain; press F10");
                }
            }
            if (!g_initialised) {
                g_failed = true;
                log_line("Overlay: could not attach to the swap chain; the panel is unavailable");
            }
        } else {
            g_failed = true;
            log_line("Overlay: swap chain is not Direct3D 11; the panel is unavailable");
        }
    }

    if (g_initialised) {
        static bool held = false;
        const bool down = (GetAsyncKeyState(g_toggle_key) & 0x8000) != 0;
        if (down && !held) g_visible = !g_visible;
        held = down;

        if (g_visible && g_target) {
            step("first frame drawn");
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            draw_panel();
            ImGui::Render();
            g_context->OMSetRenderTargets(1, &g_target, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
    }
}

HRESULT STDMETHODCALLTYPE present_hook(IDXGISwapChain* swap_chain, UINT interval, UINT flags) {
    render_overlay(swap_chain);
    return g_present(swap_chain, interval, flags);
}

HRESULT STDMETHODCALLTYPE present1_hook(IDXGISwapChain* swap_chain, UINT interval, UINT flags,
                                        const void* parameters) {
    render_overlay(swap_chain);
    return g_present1(swap_chain, interval, flags, parameters);
}

HRESULT STDMETHODCALLTYPE resize_buffers_hook(IDXGISwapChain* swap_chain, UINT count, UINT width,
                                              UINT height, DXGI_FORMAT format, UINT flags) {
    release_target();
    const HRESULT hr = g_resize_buffers(swap_chain, count, width, height, format, flags);
    if (g_initialised) create_target(swap_chain);
    return hr;
}

// Hooking DXGI's shared vtable killed the game: patching the Present slot in
// dxgi.dll reached far more than this process's swap chain. Instead each object
// we care about gets a private copy of its vtable, so only that one object is
// affected and dxgi.dll is never written to.
void** clone_vtable(void* object, size_t entries) {
    auto** original = *reinterpret_cast<void***>(object);
    auto** copy = new void*[entries];
    memcpy(copy, original, entries * sizeof(void*));
    *reinterpret_cast<void***>(object) = copy;
    return copy;
}

using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*,
                                                      DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
// IDXGIFactory2::CreateSwapChainForHwnd, vtable slot 15. The game only imports
// the DXGI 1.0 entry point but may still ask the factory for the newer
// interface, which returns the same object and so the same cloned vtable.
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(void*, IUnknown*, HWND, const void*,
                                                             const void*, IDXGIOutput*, void**);
using CreateDXGIFactoryFn = HRESULT(WINAPI*)(REFIID, void**);

CreateSwapChainFn g_create_swap_chain{};
CreateSwapChainForHwndFn g_create_swap_chain_for_hwnd{};
CreateDXGIFactoryFn g_create_factory{};
bool g_swap_chain_hooked{};

void hook_swap_chain(IDXGISwapChain* swap_chain) {
    if (g_swap_chain_hooked || !swap_chain) return;
    // Cloned well past IDXGISwapChain1's 29 entries: a short copy meant the game
    // called through memory that was not ours, which is what crashed it before.
    auto** vtable = clone_vtable(swap_chain, 64);
    g_present = reinterpret_cast<PresentFn>(vtable[8]);
    g_resize_buffers = reinterpret_cast<ResizeBuffersFn>(vtable[13]);
    g_present1 = reinterpret_cast<Present1Fn>(vtable[22]);
    vtable[8] = reinterpret_cast<void*>(&present_hook);
    vtable[13] = reinterpret_cast<void*>(&resize_buffers_hook);
    vtable[22] = reinterpret_cast<void*>(&present1_hook);
    g_swap_chain_hooked = true;
    log_line("Overlay: hooked this swap chain (Present %p, Present1 %p)", g_present, g_present1);
}

HRESULT STDMETHODCALLTYPE create_swap_chain_hook(IDXGIFactory* factory, IUnknown* device,
                                                 DXGI_SWAP_CHAIN_DESC* desc,
                                                 IDXGISwapChain** out) {
    const HRESULT hr = g_create_swap_chain(factory, device, desc, out);
    if (SUCCEEDED(hr) && out && *out) {
        log_line("Overlay: swap chain came from CreateSwapChain");
        hook_swap_chain(*out);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE create_swap_chain_for_hwnd_hook(void* factory, IUnknown* device,
                                                          HWND window, const void* desc,
                                                          const void* fullscreen,
                                                          IDXGIOutput* restrict_output,
                                                          void** out) {
    const HRESULT hr = g_create_swap_chain_for_hwnd(factory, device, window, desc, fullscreen,
                                                    restrict_output, out);
    if (SUCCEEDED(hr) && out && *out) {
        log_line("Overlay: swap chain came from CreateSwapChainForHwnd");
        hook_swap_chain(reinterpret_cast<IDXGISwapChain*>(*out));
    }
    return hr;
}

HRESULT WINAPI create_factory_hook(REFIID riid, void** out) {
    const HRESULT hr = g_create_factory(riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        static bool done = false;
        if (!done) {
            done = true;
            // Cloned generously: if the game asks for IDXGIFactory2 it will call
            // through slots past the 1.0 interface, and those must still be valid.
            auto** vtable = clone_vtable(*out, 48);
            g_create_swap_chain = reinterpret_cast<CreateSwapChainFn>(vtable[10]);
            vtable[10] = reinterpret_cast<void*>(&create_swap_chain_hook);
            g_create_swap_chain_for_hwnd =
                reinterpret_cast<CreateSwapChainForHwndFn>(vtable[15]);
            vtable[15] = reinterpret_cast<void*>(&create_swap_chain_for_hwnd_hook);
            log_line("Overlay: factory created; watching CreateSwapChain and "
                     "CreateSwapChainForHwnd");
        }
    }
    return hr;
}

// XInput is imported by ordinal rather than by name, so it needs its own lookup.
HRESULT STDMETHODCALLTYPE create_device_hook(void* self, const GUID& guid, void** device,
                                             void* outer) {
    const HRESULT hr = g_create_device(self, guid, device, outer);
    if (SUCCEEDED(hr) && device && *device && g_device_count < 16) {
        // Cloned generously. A 16-entry copy of the DirectInput object is what
        // crashed 1.0.17: the game calls through slots past the interface a
        // short copy covers, exactly as it did with the DXGI factory.
        auto** vtable = clone_vtable(*device, 128);
        DeviceHooks& entry = g_devices[g_device_count];
        entry.state = reinterpret_cast<GetDeviceStateFn>(vtable[9]);
        entry.data = reinterpret_cast<GetDeviceDataFn>(vtable[10]);
        entry.clone = vtable;
        // Published only once the originals are stored, so a device polled from
        // another thread never reaches a half-built entry.
        InterlockedIncrement(&g_device_count);
        vtable[9] = reinterpret_cast<void*>(&get_device_state_hook);
        vtable[10] = reinterpret_cast<void*>(&get_device_data_hook);
        log_line("Overlay: DirectInput device %ld will report idle while the panel is open",
                 g_device_count);
    }
    return hr;
}

bool patch_main_import_ordinal(const char* dll_name, WORD ordinal, void* replacement,
                               void** original) {
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return false;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        if (_stricmp(reinterpret_cast<const char*>(base + descriptor->Name), dll_name) != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(
            base + (descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk
                                                   : descriptor->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (!IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            if (IMAGE_ORDINAL64(names->u1.Ordinal) != ordinal) continue;
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

bool patch_main_import(const char* dll_name, const char* function_name, void* replacement,
                       void** original) {
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return false;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        if (_stricmp(reinterpret_cast<const char*>(base + descriptor->Name), dll_name) != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(
            base + (descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk
                                                   : descriptor->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(import->Name), function_name) != 0) continue;
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

bool install_overlay() {
    g_passthrough_only = GetPrivateProfileIntW(L"Overlay", L"PassthroughOnly", 0,
                                               config_path().c_str()) != 0;
    g_toggle_key = static_cast<int>(GetPrivateProfileIntW(L"Overlay", L"ToggleKeyVirtualCode",
                                                          VK_F10, config_path().c_str()));
    InterlockedExchange(&g_suppress_native_rumble, load_config().haptics_enabled ? 1 : 0);
    // The game creates its device through CreateDXGIFactory, so intercepting that
    // import leads to the factory, then to the swap chain it makes. No device of
    // our own is created and nothing shared is modified.
    void* original{};
    if (!patch_main_import("dxgi.dll", "CreateDXGIFactory",
                           reinterpret_cast<void*>(&create_factory_hook), &original)) {
        log_line("Overlay: the game does not import CreateDXGIFactory; panel unavailable");
        return false;
    }
    g_create_factory = reinterpret_cast<CreateDXGIFactoryFn>(original);

    // Input paths the game uses that bypass window messages entirely.
    if (patch_main_import("USER32.dll", "GetAsyncKeyState",
                          reinterpret_cast<void*>(&get_async_key_state_hook), &original))
        g_get_async_key_state = reinterpret_cast<GetAsyncKeyStateFn>(original);
    if (patch_main_import("USER32.dll", "GetKeyState",
                          reinterpret_cast<void*>(&get_key_state_hook), &original))
        g_get_key_state = reinterpret_cast<GetKeyStateFn>(original);
    if (patch_main_import_ordinal("XINPUT1_4.dll", 2,
                                  reinterpret_cast<void*>(&xinput_get_state_hook), &original))
        g_xinput_get_state = reinterpret_cast<XInputGetStateFn>(original);
    if (patch_main_import_ordinal("XINPUT1_4.dll", 3,
                                  reinterpret_cast<void*>(&xinput_set_state_hook), &original)) {
        g_xinput_set_state = reinterpret_cast<XInputSetStateFn>(original);
        log_line("Haptics: the game's own rumble is suppressed while ours is on");
    }
    log_line("Overlay: input will be held back from the game while the panel is open");

    log_line("Overlay: waiting for the game to create its swap chain");
    return true;
}

bool overlay_is_open() { return g_visible; }

void request_menu_confirm(unsigned presses, unsigned first_delay_ms) {
    queue_confirm(presses, first_delay_ms);
}

bool menu_confirm_pending() { return g_confirm_remaining > 0; }

void hook_direct_input(void* direct_input) {
    if (!direct_input) return;
    static bool done = false;
    if (done) return;
    done = true;
    auto** vtable = clone_vtable(direct_input, 128);
    g_create_device = reinterpret_cast<CreateDeviceFn>(vtable[3]);
    vtable[3] = reinterpret_cast<void*>(&create_device_hook);
    log_line("Overlay: watching DirectInput device creation");
}


void shutdown_overlay() {
    if (!g_initialised) return;
    if (g_original_wndproc && g_window)
        SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original_wndproc));
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    release_target();
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
    g_initialised = false;
}
