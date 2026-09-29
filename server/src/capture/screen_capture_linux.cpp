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

} // namespace fanyi
