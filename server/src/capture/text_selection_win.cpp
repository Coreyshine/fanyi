/*
 * fanyi — text_selection_win.cpp
 * Windows 划词：模拟 Ctrl+C 读剪贴板（先备份原剪贴板文本，读取后还原）。
 */
#include "capture.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <vector>

namespace fanyi {

static std::wstring read_clipboard_text() {
    std::wstring out;
    if (!OpenClipboard(NULL)) return out;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t *p = (const wchar_t *)GlobalLock(h);
        if (p) { out = p; GlobalUnlock(h); }
    }
    CloseClipboard();
    return out;
}

static void write_clipboard_text(const std::wstring &s) {
    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();
    if (!s.empty()) {
        SIZE_T bytes = (s.size() + 1) * sizeof(wchar_t);
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (g) {
            wchar_t *p = (wchar_t *)GlobalLock(g);
            if (p) { memcpy(p, s.c_str(), bytes); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
        }
    }
    CloseClipboard();
}

static void send_ctrl_c() {
    INPUT inp[4] = {};
    inp[0].type = INPUT_KEYBOARD; inp[0].ki.wVk = VK_CONTROL;
    inp[1].type = INPUT_KEYBOARD; inp[1].ki.wVk = 'C';
    inp[2].type = INPUT_KEYBOARD; inp[2].ki.wVk = 'C'; inp[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inp[3].type = INPUT_KEYBOARD; inp[3].ki.wVk = VK_CONTROL; inp[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, inp, sizeof(INPUT));
}

std::string capture_selected_text(std::string *err) {
    // 备份当前剪贴板文本
    std::wstring backup = read_clipboard_text();
    DWORD seq0 = GetClipboardSequenceNumber();

    send_ctrl_c();

    std::wstring got;
    for (int i = 0; i < 20; i++) {   // 最多 2s：等剪贴板变化
        Sleep(100);
        if (GetClipboardSequenceNumber() != seq0) {
            got = read_clipboard_text();
            if (!got.empty()) break;
        }
    }

    std::string out;
    if (!got.empty()) {
        int len = WideCharToMultiByte(CP_UTF8, 0, got.c_str(), (int)got.size(),
                                      NULL, 0, NULL, NULL);
        out.resize(len);
        WideCharToMultiByte(CP_UTF8, 0, got.c_str(), (int)got.size(), out.data(), len, NULL, NULL);
        write_clipboard_text(backup);   // 还原原剪贴板
    } else if (err) {
        *err = "未取到选中文本：请先选中文字，或确认当前应用支持复制操作";
    }
    return out;
}

} // namespace fanyi
