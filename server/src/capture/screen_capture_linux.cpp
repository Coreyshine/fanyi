/*
 * fanyi — screen_capture_linux.cpp
 * Linux：优先 gnome-screenshot -a（GNOME 框选），退化 import（ImageMagick）。
 */
#include "screen_capture.h"

#include <cstdio>
#include <cstdlib>

namespace fanyi {

bool screen_capture_to_file(const std::string &png_path, std::string *err) {
    std::string cmd1 = "gnome-screenshot -a -f '" + png_path + "' 2>/dev/null";
    int rc = system(cmd1.c_str());
    FILE *f = fopen(png_path.c_str(), "rb");
    long sz = 0;
    if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); fclose(f); }
    if (rc == 0 && sz > 100) return true;

    std::string cmd2 = "import '" + png_path + "' 2>/dev/null";
    rc = system(cmd2.c_str());
    f = fopen(png_path.c_str(), "rb");
    sz = 0;
    if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); fclose(f); }
    if (rc == 0 && sz > 100) return true;

    if (err) *err = "截图失败：请安装 gnome-screenshot 或 ImageMagick";
    return false;
}

bool capture_region_to_file(int x, int y, int w, int h,
                            const std::string &png_path, std::string *err) {
    std::string cmd = "import -window root -crop " + std::to_string(w) + "x" +
                      std::to_string(h) + "+" + std::to_string(x) + "+" +
                      std::to_string(y) + " '" + png_path + "' 2>/dev/null";
    int rc = system(cmd.c_str());
    FILE *f = fopen(png_path.c_str(), "rb");
    bool ok = (rc == 0) && f;
    long sz = 0;
    if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); fclose(f); }
    if (!ok || sz < 100) { if (err) *err = "区域截图失败（需 ImageMagick）"; return false; }
    return true;
}

bool mouse_position(int *x, int *y) {
    std::string out = run_cmd("xdotool getmouselocation 2>/dev/null");
    /* 输出形如 x:123 y:456 screen:0 window:xxx */
    int px = -1, py = -1;
    if (sscanf(out.c_str(), "x:%d y:%d", &px, &py) == 2) { *x = px; *y = py; return true; }
    return false;
}

} // namespace fanyi
