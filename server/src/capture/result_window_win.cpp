/*
 * fanyi — result_window_win.cpp
 * 结果浮窗：光标附近的置顶小窗（原文 + 译文 + 复制译文按钮），
 * 失焦/30s 超时自动关闭。窗口在独立线程上自建自销（自带消息循环）。
 */
#include "capture.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <string>
#include <thread>

namespace fanyi {

struct ResultArgs {
    std::wstring original, translated, target;
};

static const wchar_t *CLASS_NAME = L"fanyi_result_wnd";

static std::wstring to_wide(const std::string &u8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), NULL, 0);
    std::wstring w(n, 0);
    if (n) MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), w.data(), n);
    return w;
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wp) == 102) { DestroyWindow(h); return 0; }   // ✕ 关闭
            if (LOWORD(wp) == 100) {   // 复制译文
                HWND hEdit = GetDlgItem(h, 101);
                int len = GetWindowTextLengthW(hEdit);
                std::wstring t(len + 1, 0);
                GetWindowTextW(hEdit, t.data(), len + 1);
                if (OpenClipboard(h)) {
                    EmptyClipboard();
                    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
                    if (g) {
                        wchar_t *p = (wchar_t *)GlobalLock(g);
                        if (p) { wcscpy_s(p, t.size() + 1, t.c_str()); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
                    }
                    CloseClipboard();
                    SetWindowTextW(GetDlgItem(h, 100), L"已复制 ✓");
                }
            }
            return 0;
        case WM_KILLFOCUS:
        case WM_TIMER:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void set_font(HWND c, int size) {
    HFONT f = CreateFontW(-size, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
}

static DWORD WINAPI window_thread(LPVOID param) {
    ResultArgs *a = (ResultArgs *)param;

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = CreateSolidBrush(RGB(28, 32, 40));
    RegisterClassW(&wc);

    POINT pt; GetCursorPos(&pt);
    int W = 360, H = 150;
    int x = pt.x + 12, y = pt.y - H - 10;
    x = max(8, min(x, GetSystemMetrics(SM_CXSCREEN) - W - 8));
    y = max(8, min(y, GetSystemMetrics(SM_CYSCREEN) - H - 8));

    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, CLASS_NAME, NULL,
                             WS_POPUP | WS_BORDER, x, y, W, H, NULL, NULL,
                             GetModuleHandleW(NULL), NULL);
    HWND hTitle = CreateWindowExW(0, L"STATIC", (a->target + L" · 已翻译").c_str(),
                                  WS_CHILD | WS_VISIBLE, 14, 8, W - 28, 18, h, NULL, NULL, NULL);
    HWND hOrig = CreateWindowExW(0, L"STATIC", a->original.c_str(),
                                 WS_CHILD | WS_VISIBLE, 14, 30, W - 28, 22, h, NULL, NULL, NULL);
    HWND hTrans = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", a->translated.c_str(),
                                  WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
                                  14, 56, W - 28, 48, h, (HMENU)101, NULL, NULL);
    HWND hBtn = CreateWindowExW(0, L"BUTTON", L"复制译文",
                                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                14, 110, 100, 26, h, (HMENU)100, NULL, NULL);
    HWND hClose = CreateWindowExW(0, L"BUTTON", L"✕",
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  W - 34, 8, 24, 22, h, (HMENU)102, NULL, NULL);
    set_font(hTitle, 12); set_font(hOrig, 11); set_font(hTrans, 14); set_font(hBtn, 13); set_font(hClose, 12);
    SendMessageW(hOrig, WM_SETTEXT, 0, (LPARAM)a->original.c_str());

    ShowWindow(h, SW_SHOWNOACTIVATE);
    SetTimer(h, 1, 15000, NULL);       // 15s 超时自动关（✕ 可随时手动关）
    SetFocus(hTrans);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    delete a;
    return 0;
}

void result_window_show(const std::string &original,
                        const std::string &translated,
                        const std::string &target_name) {
    auto *a = new ResultArgs{to_wide(original), to_wide(translated), to_wide(target_name)};
    HANDLE th = CreateThread(NULL, 0, window_thread, a, 0, NULL);
    if (th) CloseHandle(th);
}

} // namespace fanyi
