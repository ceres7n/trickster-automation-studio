#include "app/notify.h"
#include <windows.h>
#include <shellapi.h>
#include <cwchar>

namespace nj::notify {
namespace {

HWND g_hwnd = nullptr;
bool g_tray = false;                                // the tray icon is there (added at the first notification)

std::wstring wide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

NOTIFYICONDATAW base()
{
    NOTIFYICONDATAW d{};
    d.cbSize = sizeof(d);
    d.hWnd = g_hwnd;
    d.uID = 1;
    return d;
}

} // namespace

void init(void* hwnd) { g_hwnd = static_cast<HWND>(hwnd); }

void show(const std::string& title, const std::string& text, bool warning)
{
    if (!g_hwnd) return;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!g_tray) {                                  // the tray icon (the app's, res/tas.rc id 1)
        NOTIFYICONDATAW d = base();
        d.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
        d.uCallbackMessage = kTrayMsg;
        d.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), 0);
        if (!d.hIcon) d.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        wcsncpy_s(d.szTip, L"Trickster Automation Studio", _TRUNCATE);
        g_tray = Shell_NotifyIconW(NIM_ADD, &d) != FALSE;
        if (g_tray) {
            d.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &d);
        }
    }
    if (g_tray) {
        NOTIFYICONDATAW d = base();
        d.uFlags = NIF_INFO;
        wcsncpy_s(d.szInfoTitle, wide(title).c_str(), _TRUNCATE);
        wcsncpy_s(d.szInfo, wide(text).c_str(), _TRUNCATE);
        if (warning) {
            d.dwInfoFlags = NIIF_WARNING | NIIF_LARGE_ICON;
        } else {
            d.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
            d.hBalloonIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                               GetSystemMetrics(SM_CYICON), 0);
        }
        Shell_NotifyIconW(NIM_MODIFY, &d);
    }
    if (GetForegroundWindow() != g_hwnd) {          // the taskbar button flashes until the window is in front
        FLASHWINFO f{};
        f.cbSize = sizeof(f);
        f.hwnd = g_hwnd;
        f.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
        FlashWindowEx(&f);
    }
}

void shutdown()
{
    if (!g_tray) return;
    NOTIFYICONDATAW d = base();
    Shell_NotifyIconW(NIM_DELETE, &d);
    g_tray = false;
}

void bring_back()
{
    if (!g_hwnd) return;
    if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
    else ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
}

} // namespace nj::notify
