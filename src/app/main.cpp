// TricksterAutomationStudio.exe — Win32 + DX11 host for the ImGui panel.
#include "app/neo_app.h"
#include "app/gfx.h"
#include "app/notify.h"
#include "app/ui_theme.h"
#include "app/text_util.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include <d3d11.h>
#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#pragma comment(lib, "dwmapi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

ID3D11Device*           g_device = nullptr;
ID3D11DeviceContext*    g_context = nullptr;
IDXGISwapChain*         g_swapChain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT                    g_resizeW = 0, g_resizeH = 0;
bool                    g_occluded = false;

void create_rtv()
{
    ID3D11Texture2D* back = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

void release_rtv()
{
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

bool create_device(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &got, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)   // no hardware device: WARP
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &got, &g_context);
    if (FAILED(hr)) return false;
    create_rtv();
    return true;
}

// --shot <file.png>: the back buffer of a rendered frame -> PNG (WIC), for UI checks without a screen grab.
bool save_back_buffer(const std::string& path)
{
    ID3D11Texture2D* back = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (!back) return false;
    D3D11_TEXTURE2D_DESC d{};
    back->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    bool ok = SUCCEEDED(g_device->CreateTexture2D(&d, nullptr, &staging));
    if (ok) g_context->CopyResource(staging, back);
    back->Release();
    if (!ok) return false;
    D3D11_MAPPED_SUBRESOURCE m{};
    ok = SUCCEEDED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &m));
    IWICImagingFactory* wic = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    const std::wstring wpath(path.begin(), path.end());
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    ok = ok && SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
         SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(wpath.c_str(), GENERIC_WRITE)) &&
         SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
         SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
         SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(d.Width, d.Height)) &&
         SUCCEEDED(frame->SetPixelFormat(&fmt));
    std::vector<BYTE> px;
    if (ok) {                                   // the PNG encoder may only take BGRA: swap R and B then
        px.assign((BYTE*)m.pData, (BYTE*)m.pData + size_t(m.RowPitch) * d.Height);
        if (fmt != GUID_WICPixelFormat32bppRGBA)
            for (size_t i = 0; i + 3 < px.size(); i += 4) std::swap(px[i], px[i + 2]);
    }
    ok = ok && SUCCEEDED(frame->WritePixels(d.Height, m.RowPitch, UINT(px.size()), px.data())) &&
         SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
    if (frame) frame->Release();
    if (enc) enc->Release();
    if (stream) stream->Release();
    if (wic) wic->Release();
    g_context->Unmap(staging, 0);
    staging->Release();
    return ok;
}

void destroy_device()
{
    release_rtv();
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) return 0;
        g_resizeW = LOWORD(lp);
        g_resizeH = HIWORD(lp);
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU) return 0;   // no ALT menu
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case nj::notify::kTrayMsg: {                    // the notification / the tray icon clicked: the window back
        const UINT ev = LOWORD(lp);
        if (ev == NIN_BALLOONUSERCLICK || ev == NIN_SELECT || ev == NIN_KEYSELECT || ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK)
            nj::notify::bring_back();
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmdLine, int)
{
    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, wnd_proc, 0, 0, inst, nullptr, nullptr, nullptr, nullptr, L"TricksterAutomationStudio", nullptr };
    // the app icon (res/tas.rc, id 1): the large one for Alt+Tab, the small one for the title bar / taskbar
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                 GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    if (!wc.hIcon) wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);
    int winW = 1440, winH = 900;                // --size <w>x<h> (with --shot: a long page in one picture)
    if (const char* sz = cmdLine ? strstr(cmdLine, "--size ") : nullptr) {
        int a = 0, b = 0;
        if (sscanf_s(sz + 7, "%dx%d", &a, &b) == 2 && a >= 640 && b >= 480 && a <= 8000 && b <= 8000) { winW = a; winH = b; }
    }
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Trickster Automation Studio", WS_OVERLAPPEDWINDOW, 100, 80, winW, winH,
                              nullptr, nullptr, inst, nullptr);
    if (!create_device(hwnd)) {
        destroy_device();
        UnregisterClassW(wc.lpszClassName, inst);
        MessageBoxA(nullptr, "Direct3D 11 is not available.", "Trickster Automation Studio", MB_ICONERROR);
        return 1;
    }
    {   // a dark title bar to match the theme (Windows 10 20H1+: DWMWA_USE_IMMERSIVE_DARK_MODE = 20)
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    static std::string iniPath = nj::exe_dir() + "tas_ui.ini";
    io.IniFilename = iniPath.c_str();
    nj::ui::apply_theme();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    nj::notify::init(hwnd);

    {
        nj::NeoApp app;
        nj::ui::load_fonts();
        app.monoFont = nj::ui::fonts.mono;
        {
            std::string err;
            if (!nj::gfx::init(g_device, nj::exe_dir() + "data\\icons", err)) app.gfxStatus = err;
        }
        app.handle_command_line(cmdLine);

        // --shot <file.png>: render a few frames, save the last one, quit
        std::string shotPath;
        {
            const std::string cl = cmdLine ? cmdLine : "";
            const size_t at = cl.find("--shot ");
            if (at != std::string::npos) {
                size_t b = cl.find_first_not_of(' ', at + 7), e = std::string::npos;
                if (b != std::string::npos && cl[b] == '"') { ++b; e = cl.find('"', b); }
                else if (b != std::string::npos) e = cl.find(' ', b);
                if (b != std::string::npos) shotPath = cl.substr(b, e == std::string::npos ? std::string::npos : e - b);
            }
        }
        int frames = 0;
        bool done = false;
        while (!done) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (msg.message == WM_QUIT) done = true;
            }
            if (done) break;
            app.update();                           // links + bots run even while the window is hidden

            if (g_occluded && g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
                Sleep(10);
                continue;
            }
            g_occluded = false;
            if (g_resizeW && g_resizeH) {
                release_rtv();
                g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
                g_resizeW = g_resizeH = 0;
                create_rtv();
            }

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            app.frame();
            ImGui::Render();

            const float clear[4] = { nj::ui::kBg.x, nj::ui::kBg.y, nj::ui::kBg.z, 1.0f };
            g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
            g_context->ClearRenderTargetView(g_rtv, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            if (!shotPath.empty() && ++frames == 45) {
                save_back_buffer(shotPath);
                PostQuitMessage(0);
            }
            g_occluded = g_swapChain->Present(1, 0) == DXGI_STATUS_OCCLUDED;
        }
    }   // sessions close here, before the device goes
    nj::gfx::shutdown();
    nj::notify::shutdown();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroy_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    return 0;
}
