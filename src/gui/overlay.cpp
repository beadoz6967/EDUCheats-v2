#include "overlay.hpp"
#include "esp_render.hpp"
#include "menu_ui.hpp"
#include "../theme.hpp"
#include "../offsets.hpp"

#include <d3d11.h>
#include <dxgi1_5.h>   // IDXGIFactory2/5, IDXGISwapChain1, DXGI_SWAP_CHAIN_DESC1, tearing
#include <dwmapi.h>
#include <dcomp.h>
#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include <cstdio>
#include <cmath>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

// ImGui Win32 backend exports its own WndProc handler — forward to it for
// keyboard/mouse before our own logic runs.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static constexpr wchar_t kOverlayClass[] = L"EDUCheats_DX11";
static Overlay* g_overlay = nullptr;

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (g_overlay && g_overlay->m_device && wp != SIZE_MINIMIZED) {
            g_overlay->DestroyRenderTarget();
            auto* sc = static_cast<IDXGISwapChain1*>(g_overlay->m_swapchain);
            sc->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
            g_overlay->CreateRenderTarget();
            g_overlay->m_winW = LOWORD(lp);
            g_overlay->m_winH = HIWORD(lp);
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

Overlay::Overlay(const Memory& mem, uintptr_t clientBase,
                 ESPConfig& cfg, AimbotConfig& ab, GameState& state, Config& persist)
    : m_mem(mem), m_clientBase(clientBase),
      m_cfg(cfg), m_ab(ab), m_state(state), m_persist(persist) {}

Overlay::~Overlay() {
    if (m_device) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        DestroyDevice();
    }
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        UnregisterClassW(kOverlayClass, GetModuleHandleW(nullptr));
    }
}

bool Overlay::CreateOverlayWindow() {
    // Find CS2 by both class and title for robustness.
    // CS2 uses class "SDL_app" — title alone fails on some configurations.
    HWND gameWnd = FindWindowW(L"SDL_app", L"Counter-Strike 2");
    if (!gameWnd) gameWnd = FindWindowW(nullptr, L"Counter-Strike 2");

    RECT gameRect{};
    if (gameWnd) {
        GetWindowRect(gameWnd, &gameRect);
        m_winX = gameRect.left;
        m_winY = gameRect.top;
        m_winW = gameRect.right  - gameRect.left;
        m_winH = gameRect.bottom - gameRect.top;
        printf("[Overlay] CS2 window found at %d,%d %dx%d\n", m_winX, m_winY, m_winW, m_winH);
    } else {
        m_winX = 0;
        m_winY = 0;
        m_winW = GetSystemMetrics(SM_CXSCREEN);
        m_winH = GetSystemMetrics(SM_CYSCREEN);
        gameRect = { 0, 0, m_winW, m_winH };
        printf("[Overlay] CS2 window not found — falling back to virtual screen %dx%d\n", m_winW, m_winH);
    }

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = OverlayWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kOverlayClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        printf("[Overlay] RegisterClassExW failed (0x%lX)\n", GetLastError());
        return false;
    }

    m_hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        kOverlayClass, L"EDUCheats",
        WS_POPUP,
        gameRect.left, gameRect.top, m_winW, m_winH,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!m_hwnd) {
        printf("[Overlay] CreateWindowExW failed (0x%lX)\n", GetLastError());
        return false;
    }

    // WS_EX_NOREDIRECTIONBITMAP + DirectComposition handles transparency.
    // WS_EX_TRANSPARENT starts set — HTTRANSPARENT passes all clicks to CS2.
    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
    return true;
}

bool Overlay::CreateDeviceAndSwapchain() {
    // DXGI_SWAP_EFFECT_FLIP_DISCARD requires IDXGIFactory2::CreateSwapChainForHwnd —
    // it is NOT supported through D3D11CreateDeviceAndSwapChain (DXGI 1.0 API).
    // We create the device first, then obtain the factory through the device's adapter.

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL achieved{};
    ID3D11Device*        dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &dev, &achieved, &ctx);

    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
            &dev, &achieved, &ctx);
    }

    if (FAILED(hr)) {
        printf("[Overlay] D3D11CreateDevice failed (0x%lX)\n", hr);
        return false;
    }

    // Walk device → IDXGIDevice → IDXGIAdapter → IDXGIFactory2
    IDXGIDevice*   dxgiDev  = nullptr;
    IDXGIAdapter*  adapter  = nullptr;
    IDXGIFactory2* factory2 = nullptr;

    hr  = dev->QueryInterface(IID_PPV_ARGS(&dxgiDev));
    if (SUCCEEDED(hr)) hr = dxgiDev->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory2));

    if (adapter)  adapter->Release();

    if (FAILED(hr) || !factory2) {
        printf("[Overlay] Failed to obtain IDXGIFactory2 (0x%lX)\n", hr);
        dev->Release(); ctx->Release();
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width              = static_cast<UINT>(m_winW);
    sd.Height             = static_cast<UINT>(m_winH);
    sd.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.Stereo             = FALSE;
    sd.SampleDesc.Count   = 1;
    sd.SampleDesc.Quality = 0;
    sd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount        = 2;
    sd.Scaling            = DXGI_SCALING_STRETCH;
    sd.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode          = DXGI_ALPHA_MODE_PREMULTIPLIED;
    sd.Flags              = 0;

    // CreateSwapChainForComposition supports PREMULTIPLIED alpha (unlike ForHwnd).
    // DirectComposition binds the swap chain to the HWND.
    IDXGISwapChain1* swap1 = nullptr;
    hr = factory2->CreateSwapChainForComposition(dev, &sd, nullptr, &swap1);
    factory2->Release();

    if (FAILED(hr)) {
        printf("[Overlay] CreateSwapChainForComposition failed (0x%lX)\n", hr);
        if (dxgiDev) dxgiDev->Release();
        dev->Release(); ctx->Release();
        return false;
    }

    IDCompositionDevice* dcompDev = nullptr;
    hr = DCompositionCreateDevice(dxgiDev, IID_PPV_ARGS(&dcompDev));
    dxgiDev->Release();

    if (FAILED(hr)) {
        printf("[Overlay] DCompositionCreateDevice failed (0x%lX)\n", hr);
        swap1->Release(); dev->Release(); ctx->Release();
        return false;
    }

    IDCompositionTarget* dcompTarget = nullptr;
    hr = dcompDev->CreateTargetForHwnd(m_hwnd, TRUE, &dcompTarget);
    if (FAILED(hr)) {
        printf("[Overlay] DComp CreateTargetForHwnd failed (0x%lX)\n", hr);
        dcompDev->Release(); swap1->Release(); dev->Release(); ctx->Release();
        return false;
    }

    IDCompositionVisual* dcompVisual = nullptr;
    hr = dcompDev->CreateVisual(&dcompVisual);
    if (FAILED(hr)) {
        printf("[Overlay] DComp CreateVisual failed (0x%lX)\n", hr);
        dcompTarget->Release(); dcompDev->Release();
        swap1->Release(); dev->Release(); ctx->Release();
        return false;
    }

    dcompVisual->SetContent(swap1);
    dcompTarget->SetRoot(dcompVisual);
    dcompDev->Commit();

    m_device      = dev;
    m_deviceCtx   = ctx;
    m_swapchain   = swap1;
    m_dcompDevice = dcompDev;
    m_dcompTarget = dcompTarget;
    m_dcompVisual = dcompVisual;

    CreateRenderTarget();
    return true;
}

void Overlay::CreateRenderTarget() {
    auto* swap = static_cast<IDXGISwapChain*>(m_swapchain);
    auto* dev  = static_cast<ID3D11Device*>(m_device);

    ID3D11Texture2D* back = nullptr;
    if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return;

    ID3D11RenderTargetView* rtv = nullptr;
    if (FAILED(dev->CreateRenderTargetView(back, nullptr, &rtv))) {
        back->Release();
        printf("[Overlay] CreateRenderTargetView failed\n");
        return;
    }
    back->Release();
    m_renderTargetView = rtv;
}

void Overlay::DestroyRenderTarget() {
    if (m_renderTargetView) {
        static_cast<ID3D11RenderTargetView*>(m_renderTargetView)->Release();
        m_renderTargetView = nullptr;
    }
}

void Overlay::DestroyDevice() {
    DestroyRenderTarget();
    if (m_dcompVisual) { static_cast<IDCompositionVisual*>(m_dcompVisual)->Release(); m_dcompVisual = nullptr; }
    if (m_dcompTarget) { static_cast<IDCompositionTarget*>(m_dcompTarget)->Release(); m_dcompTarget = nullptr; }
    if (m_dcompDevice) { static_cast<IDCompositionDevice*>(m_dcompDevice)->Release(); m_dcompDevice = nullptr; }
    if (m_swapchain)   { static_cast<IDXGISwapChain1*>(m_swapchain)->Release();       m_swapchain   = nullptr; }
    if (m_deviceCtx)   { static_cast<ID3D11DeviceContext*>(m_deviceCtx)->Release();   m_deviceCtx   = nullptr; }
    if (m_device)      { static_cast<ID3D11Device*>(m_device)->Release();             m_device      = nullptr; }
}

void Overlay::RefreshGameWindowBounds() {
    HWND gameWnd = FindWindowW(L"SDL_app", L"Counter-Strike 2");
    if (!gameWnd) gameWnd = FindWindowW(nullptr, L"Counter-Strike 2");
    if (!gameWnd) return;

    RECT r{};
    if (!GetWindowRect(gameWnd, &r)) return;

    int x = r.left;
    int y = r.top;
    int w = r.right  - r.left;
    int h = r.bottom - r.top;

    // Track both position and size — window may move without resizing
    if (x == m_winX && y == m_winY && w == m_winW && h == m_winH) return;

    m_winX = x;
    m_winY = y;
    m_winW = w;
    m_winH = h;
    if (m_hwnd)
        SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
}

void Overlay::ApplyClickThrough(bool clickThrough) {
    if (!m_hwnd) return;
    if (m_clickThrough == clickThrough) return;

    LONG_PTR style = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    if (clickThrough) {
        style |= (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    } else {
        style &= ~(LONG_PTR)(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    }
    SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, style);
    if (!clickThrough) SetForegroundWindow(m_hwnd);
    m_clickThrough = clickThrough;
}

void Overlay::ToggleMenu() {
    m_menuVisible = !m_menuVisible;
    ApplyClickThrough(!m_menuVisible);
}

void Overlay::PushPlayers(const PlayerESPData players[64], int count, int localTeam) {
    std::lock_guard<std::mutex> lk(m_lock);
    for (int i = 0; i < count; ++i) m_players[i] = players[i];
    m_playerCount = count;
    m_localTeam   = localTeam;
}

void Overlay::RenderFrame() {
    auto* ctx  = static_cast<ID3D11DeviceContext*>(m_deviceCtx);
    auto* swap = static_cast<IDXGISwapChain1*>(m_swapchain);
    auto* rtv  = static_cast<ID3D11RenderTargetView*>(m_renderTargetView);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Copy entity snapshot under lock — render without holding it
    PlayerESPData snapPlayers[64];
    int snapCount, snapTeam;
    {
        std::lock_guard<std::mutex> lk(m_lock);
        for (int i = 0; i < m_playerCount; ++i) snapPlayers[i] = m_players[i];
        snapCount = m_playerCount;
        snapTeam  = m_localTeam;
    }

    // Read view matrix fresh every render frame — camera changes at mouse speed,
    // so a stale matrix from the entity scan thread causes visible lag on fast turns.
    ViewMatrix liveView{};
    m_mem.ReadBuffer(m_clientBase + offsets::dwViewMatrix, &liveView, sizeof(liveView));

    if (m_cfg.enabled.load()) {
        esp_render::DrawAll(snapPlayers, snapCount, snapTeam,
                            liveView, m_winW, m_winH, m_cfg);
    }

    // FOV circle — drawn when aimbot is enabled, independent of ESP master toggle.
    // Radius derived from the aimbot angular FOV (degrees, Euclidean pitch+yaw)
    // projected onto screen using CS2's fixed vertical FOV of ~73.74°.
    if (m_ab.enabled.load()) {
        constexpr float kGameVFOV = 73.74f;
        constexpr float kPI       = 3.14159265f;
        float fov    = m_ab.fov.load();
        float radius = std::tan(fov * kPI / 180.f)
                     / std::tan(kGameVFOV * 0.5f * kPI / 180.f)
                     * (m_winH * 0.5f);
        ImVec2 center{ m_winW * 0.5f, m_winH * 0.5f };
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        dl->AddCircle(center, radius, IM_COL32(0, 0, 0, 110), 0, 3.0f);
        dl->AddCircle(center, radius, IM_COL32(255, 255, 255, 200), 0, 1.5f);
    }

    if (m_menuVisible) {
        menu_ui::Draw(m_cfg, m_ab, m_state, m_persist, m_menuVisible);
    }

    ImGui::Render();

    float clear[4]{ 0.f, 0.f, 0.f, 0.f }; // fully transparent
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->ClearRenderTargetView(rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    // syncInterval=0: no vsync wait. Tearing flag omitted — incompatible with layered windows.
    swap->Present(0, 0);
    static_cast<IDCompositionDevice*>(m_dcompDevice)->Commit();
}

void Overlay::Run(std::atomic<bool>& running) {
    g_overlay = this;

    if (!CreateOverlayWindow())       { g_overlay = nullptr; return; }
    if (!CreateDeviceAndSwapchain())  { g_overlay = nullptr; return; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Replace the blurry 13px bitmap default with Segoe UI at DPI-aware size
    {
        UINT dpi = GetDpiForWindow(m_hwnd);
        if (dpi < 72) dpi = 96;
        float sz = floorf(16.f * static_cast<float>(dpi) / 96.f);
        ImFontConfig fc;
        fc.OversampleH = 3;
        fc.OversampleV = 1;
        char path[MAX_PATH];
        ExpandEnvironmentStringsA("%SystemRoot%\\Fonts\\segoeui.ttf", path, MAX_PATH);
        if (!io.Fonts->AddFontFromFileTTF(path, sz, &fc))
            io.Fonts->AddFontDefault();
    }

    theme::ApplyEducanetStyle();

    ImGui_ImplWin32_Init(m_hwnd);
    ImGui_ImplDX11_Init(static_cast<ID3D11Device*>(m_device),
                        static_cast<ID3D11DeviceContext*>(m_deviceCtx));

    printf("[Overlay] DX11 + ImGui initialized — entering render loop\n");

    MSG msg{};
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        // INSERT toggles menu — polled here so it works regardless of focus
        bool curIns = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        if (curIns && !m_prevInsert) ToggleMenu();
        m_prevInsert = curIns;

        if (GetAsyncKeyState(VK_END) & 0x8000) {
            running = false;
            break;
        }

        RefreshGameWindowBounds();
        RenderFrame();
    }

    printf("[Overlay] Render loop exited\n");
}
