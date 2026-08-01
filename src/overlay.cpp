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
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT,
                                                    DXGI_FORMAT, UINT);

PresentFn g_present{};
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

void write_setting(const wchar_t* section, const wchar_t* key, const wchar_t* value) {
    WritePrivateProfileStringW(section, key, value, config_path().c_str());
}

void write_bool(const wchar_t* section, const wchar_t* key, bool value) {
    write_setting(section, key, value ? L"1" : L"0");
}

void write_float(const wchar_t* section, const wchar_t* key, float value) {
    wchar_t text[32]{};
    swprintf_s(text, L"%.3f", value);
    write_setting(section, key, text);
}

void write_unsigned(const wchar_t* section, const wchar_t* key, unsigned value) {
    wchar_t text[32]{};
    swprintf_s(text, L"%u", value);
    write_setting(section, key, text);
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
        const ImGuiIO& io = ImGui::GetIO();
        // Swallow the input the panel is using so it does not also reach the
        // game, but let everything else through.
        const bool mouse = message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
        const bool keyboard = message >= WM_KEYFIRST && message <= WM_KEYLAST;
        if ((mouse && io.WantCaptureMouse) || (keyboard && io.WantCaptureKeyboard)) return 1;
    }
    return CallWindowProcW(g_original_wndproc, window, message, w, l);
}

void draw_panel() {
    if (!g_settings_loaded) { g_settings = load_config(); g_settings_loaded = true; }

    ImGui::SetNextWindowSize(ImVec2(430, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);
    ImGui::Begin("NieR Haptics", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextDisabled("F10 closes this panel. Changes apply immediately.");
    ImGui::Separator();

    if (ImGui::Checkbox("Haptics on", &g_settings.haptics_enabled))
        write_bool(L"General", L"HapticsEnabled", g_settings.haptics_enabled);

    if (ImGui::CollapsingHeader("Footsteps", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("Enabled##foot", &g_settings.footsteps_enabled))
            write_bool(L"Haptics", L"FootstepsEnabled", g_settings.footsteps_enabled);
        if (ImGui::SliderFloat("Intensity##foot", &g_settings.footstep_strength, 0.0f, 0.3f, "%.3f"))
            write_float(L"Haptics", L"FootstepStrength", g_settings.footstep_strength);
        if (ImGui::Checkbox("During combat", &g_settings.footsteps_in_combat))
            write_bool(L"Haptics", L"FootstepsInCombat", g_settings.footsteps_in_combat);
        if (ImGui::Checkbox("Yours only", &g_settings.footstep_player_only))
            write_bool(L"Haptics", L"FootstepPlayerOnly", g_settings.footstep_player_only);
        int combat = static_cast<int>(g_settings.combat_window_ms);
        if (ImGui::SliderInt("Combat lasts (ms)", &combat, 250, 15000)) {
            g_settings.combat_window_ms = static_cast<unsigned>(combat);
            write_unsigned(L"Haptics", L"CombatWindowMs", g_settings.combat_window_ms);
        }
    }

    if (ImGui::CollapsingHeader("Combat and menus", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("Landing a hit", &g_settings.enemy_hit_enabled))
            write_bool(L"Haptics", L"EnemyHitEnabled", g_settings.enemy_hit_enabled);
        if (ImGui::SliderFloat("Hit intensity", &g_settings.enemy_hit_strength, 0.0f, 1.0f, "%.2f"))
            write_float(L"Haptics", L"EnemyHitStrength", g_settings.enemy_hit_strength);
        if (ImGui::Checkbox("Taking a hit", &g_settings.player_hit_enabled))
            write_bool(L"Haptics", L"PlayerHitEnabled", g_settings.player_hit_enabled);
        if (ImGui::SliderFloat("Damage intensity", &g_settings.player_hit_strength, 0.0f, 1.0f, "%.2f"))
            write_float(L"Haptics", L"PlayerHitStrength", g_settings.player_hit_strength);
        if (ImGui::Checkbox("Menu haptics", &g_settings.menu_enabled))
            write_bool(L"Haptics", L"MenuEnabled", g_settings.menu_enabled);
        if (ImGui::SliderFloat("Menu intensity", &g_settings.menu_strength, 0.0f, 1.0f, "%.2f"))
            write_float(L"Haptics", L"MenuStrength", g_settings.menu_strength);
    }

    if (ImGui::CollapsingHeader("Gameplay", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("Unlimited jumps", &g_settings.multi_jump_enabled))
            write_bool(L"Gameplay", L"MultiJumpEnabled", g_settings.multi_jump_enabled);
        if (ImGui::Checkbox("Keep chips on death", &g_settings.keep_chips_on_death))
            write_bool(L"Gameplay", L"KeepChipsOnDeath", g_settings.keep_chips_on_death);
        ImGui::TextDisabled("Chip setting applies on the next launch.");
    }

    if (ImGui::Button("Reload from file")) { g_settings = load_config(); }
    ImGui::End();
}

HRESULT STDMETHODCALLTYPE present_hook(IDXGISwapChain* swap_chain, UINT interval, UINT flags) {
    // A Present hook must never run inside itself. 1.0.14 crashed the game with
    // a stack overflow on the first frame, and whatever re-entered, this guard
    // turns that class of failure into a dropped frame instead of a crash.
    static thread_local bool inside = false;
    if (inside) return g_present(swap_chain, interval, flags);
    inside = true;
    struct Leave { ~Leave() { inside = false; } } leave;

    if (!g_failed && !g_initialised) {
        if (SUCCEEDED(swap_chain->GetDevice(__uuidof(ID3D11Device),
                                            reinterpret_cast<void**>(&g_device))) && g_device) {
            g_device->GetImmediateContext(&g_context);
            DXGI_SWAP_CHAIN_DESC desc{};
            swap_chain->GetDesc(&desc);
            g_window = desc.OutputWindow;
            if (g_window && g_context && create_target(swap_chain)) {
                IMGUI_CHECKVERSION();
                ImGui::CreateContext();
                ImGui::GetIO().IniFilename = nullptr;
                ImGui::StyleColorsDark();
                if (ImGui_ImplWin32_Init(g_window) && ImGui_ImplDX11_Init(g_device, g_context)) {
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
        const bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (down && !held) g_visible = !g_visible;
        held = down;

        if (g_visible && g_target) {
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            draw_panel();
            ImGui::Render();
            g_context->OMSetRenderTargets(1, &g_target, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
    }
    return g_present(swap_chain, interval, flags);
}

HRESULT STDMETHODCALLTYPE resize_buffers_hook(IDXGISwapChain* swap_chain, UINT count, UINT width,
                                              UINT height, DXGI_FORMAT format, UINT flags) {
    release_target();
    const HRESULT hr = g_resize_buffers(swap_chain, count, width, height, format, flags);
    if (g_initialised) create_target(swap_chain);
    return hr;
}

bool patch_vtable_entry(void** vtable, size_t index, void* replacement, void** original) {
    DWORD protection{};
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &protection)) return false;
    *original = vtable[index];
    vtable[index] = replacement;
    VirtualProtect(&vtable[index], sizeof(void*), protection, &protection);
    return true;
}
}  // namespace

bool install_overlay() {
    // A throwaway swap chain purely to read the vtable DXGI shares with the
    // game's own; nothing is ever rendered through it.
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NierHapticsOverlayProbe";
    RegisterClassExW(&wc);
    HWND probe = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                                 nullptr, nullptr, wc.hInstance, nullptr);
    if (!probe) {
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        log_line("Overlay: could not create the probe window");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = probe;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swap_chain{};
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    const HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
        &desc, &swap_chain, &device, &level, &context);

    bool hooked = false;
    if (SUCCEEDED(hr) && swap_chain) {
        auto** vtable = *reinterpret_cast<void***>(swap_chain);
        hooked = patch_vtable_entry(vtable, 8, reinterpret_cast<void*>(&present_hook),
                                    reinterpret_cast<void**>(&g_present)) &&
                 patch_vtable_entry(vtable, 13, reinterpret_cast<void*>(&resize_buffers_hook),
                                    reinterpret_cast<void**>(&g_resize_buffers));
    }
    if (swap_chain) swap_chain->Release();
    if (context) context->Release();
    if (device) device->Release();
    DestroyWindow(probe);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    if (!hooked) {
        log_line("Overlay: could not hook the swap chain (0x%08lX); the panel is unavailable", hr);
        return false;
    }
    log_line("Overlay: swap chain hooked; the panel will appear on the first frame");
    return true;
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
