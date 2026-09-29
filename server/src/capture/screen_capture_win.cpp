/*
 * fanyi — screen_capture_win.cpp
 * Windows：触发系统 Win+Shift+S 截图（用户框选），从剪贴板取图转存 PNG。
 */
#include "screen_capture.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <string>
#include <thread>

namespace fanyi {

static void send_win_shift_s() {
    INPUT inp[6] = {};
    int i = 0;
    inp[i].type = INPUT_KEYBOARD; inp[i].ki.wVk = VK_LWIN; i++;
    inp[i].type = INPUT_KEYBOARD; inp[i].ki.wVk = VK_SHIFT; i++;
    inp[i].type = INPUT_KEYBOARD; inp[i].ki.wVk = 'S'; i++;
    inp[i].type = INPUT_KEYBOARD; inp[i].ki.wVk = 'S'; inp[i].ki.dwFlags = KEYEVENTF_KEYUP; i++;
    inp[i].type = INPUT_KEYBOARD; inp[i].ki.wVk = VK_SHIFT; inp[i].ki.dwFlags = KEYEVENTF_KEYUP; i++;
    inp[i].type = INPUT_KEYBOARD; inp[i].ki.wVk = VK_LWIN; inp[i].ki.dwFlags = KEYEVENTF_KEYUP; i++;
    SendInput((UINT)i, inp, sizeof(INPUT));
}

/* 从剪贴板 CF_DIB 提取 RGB 像素 */
static bool clipboard_dib_to_rgb(std::vector<unsigned char> &rgb, int &w, int &h) {
    if (!OpenClipboard(NULL)) return false;
    bool ok = false;
    HANDLE h = GetClipboardData(CF_DIB);
    if (h) {
        BITMAPINFO *bi = (BITMAPINFO *)GlobalLock(h);
        if (bi && bi->bmiHeader.biCompression == BI_RGB &&
            (bi->bmiHeader.biBitCount == 24 || bi->bmiHeader.biBitCount == 32)) {
            w = bi->bmiHeader.biWidth;
            h = abs(bi->bmiHeader.biHeight);
            int bpp = bi->bmiHeader.biBitCount / 8;
            const unsigned char *bits = (const unsigned char *)bi + bi->bmiHeader.biSize
                                        + (bi->bmiHeader.biClrUsed ? bi->bmiHeader.biClrUsed * 4 : 0);
            bool flip = bi->bmiHeader.biHeight > 0;
            rgb.resize((size_t)w * h * 3);
            for (int y = 0; y < h; y++) {
                int sy = flip ? h - 1 - y : y;
                const unsigned char *row = bits + (size_t)sy * ((w * bpp + 3) & ~3);
                for (int x = 0; x < w; x++) {
                    rgb[((size_t)y * w + x) * 3 + 0] = row[x * bpp + 2];
                    rgb[((size_t)y * w + x) * 3 + 1] = row[x * bpp + 1];
                    rgb[((size_t)y * w + x) * 3 + 2] = row[x * bpp + 0];
                }
            }
            ok = true;
        }
        GlobalUnlock(h);
    }
    CloseClipboard();
    return ok;
}

bool screen_capture_to_file(const std::string &png_path, std::string *err) {
    if (!OpenClipboard(NULL)) { if (err) *err = "剪贴板被占用"; return false; }
    EmptyClipboard();
    CloseClipboard();
    send_win_shift_s();

    /* 等用户框选完成（剪贴板出现图像），上限 25s */
    bool got = false;
    std::vector<unsigned char> rgb;
    int w = 0, h = 0;
    for (int i = 0; i < 250 && !got; i++) {
        Sleep(100);
        if (!OpenClipboard(NULL)) continue;
        got = IsClipboardFormatAvailable(CF_DIB);
        if (got) got = clipboard_dib_to_rgb(rgb, w, h);
        CloseClipboard();
    }
    if (!got) {
        if (err) *err = "已取消截图或超时";
        return false;
    }
    if (!stbi_write_png(png_path.c_str(), w, h, 3, rgb.data(), w * 3)) {
        if (err) *err = "保存截图失败";
        return false;
    }
    return true;
}

} // namespace fanyi
