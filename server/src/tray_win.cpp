/*
 * fanyi — tray_win.cpp
 * Windows 托盘图标（Shell_NotifyIcon）。
 */
#include "tray.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

namespace fanyi {

static TrayActions g_actions;
static HWND g_hwnd = nullptr;
static NOTIFYICONDATAW g_nid = {};
static const UINT WM_FANYI_TRAY = WM_APP + 1;
static UINT g_wm_taskbar_created = 0;

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_FANYI_TRAY:
            if (lp == WM_LBUTTONUP || lp == WM_CONTEXTMENU) {
                HMENU menu = CreatePopupMenu();
                AppendMenuA(menu, MF_STRING, 1, "打开设置…");
                AppendMenuA(menu, MF_STRING | (g_actions.enabled() ? MF_CHECKED : 0), 2,
                            g_actions.enabled() ? "翻译：开（点此暂停）" : "翻译：暂停（点此开启）");
                AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
                AppendMenuA(menu, MF_STRING, 3, "退出 fanyi");
                POINT p;
                GetCursorPos(&p);
                SetForegroundWindow(h);
                int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, p.x, p.y, 0, h, NULL);
                DestroyMenu(menu);
                switch (cmd) {
                    case 1: ShellExecuteA(h, "open", g_actions.settings_url, NULL, NULL, SW_SHOWNORMAL); break;
                    case 2: g_actions.toggle_enabled(); break;
                    case 3: g_actions.quit(); DestroyWindow(h); break;
                }
            }
            return 0;
        default:
            if (msg == g_wm_taskbar_created && g_wm_taskbar_created) {
                Shell_NotifyIconW(NIM_ADD, &g_nid);
            }
            return DefWindowProcW(h, msg, wp, lp);
    }
}

int tray_run(const TrayActions &actions) {
    g_actions = actions;
    g_wm_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wnd_proc;
    wc.lpszClassName = L"fanyi_tray";
    wc.hInstance = GetModuleHandleW(NULL);
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, L"fanyi_tray", L"fanyi", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);

    wchar_t tip[64];
    MultiByteToWideChar(CP_UTF8, 0, "fanyi 本地翻译", -1, tip, 64);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_FANYI_TRAY;
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    lstrcpynW(g_nid.szTip, tip, 64);
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    return 0;
}

void tray_request_stop() {
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

} // namespace fanyi
