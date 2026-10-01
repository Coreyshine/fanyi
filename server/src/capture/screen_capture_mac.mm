/*
 * fanyi — screen_capture_mac.mm
 * macOS 截图：screencapture（交互 -i / 区域 -R），鼠标位置 CGEvent。
 */
#include "screen_capture.h"

#import <AppKit/AppKit.h>

#include <cstdio>
#include <cstdlib>

namespace fanyi {

bool screen_capture_to_file(const std::string &png_path, std::string *err) {
    std::string cmd = "screencapture -x -i '" + png_path + "'";
    int rc = system(cmd.c_str());
    FILE *f = fopen(png_path.c_str(), "rb");
    bool ok = (rc == 0) && f;
    long sz = 0;
    if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); fclose(f); }
    if (!ok || sz < 100) {
        if (err) *err = "已取消截图";
        return false;
    }
    return true;
}

bool capture_region_to_file(int x, int y, int w, int h,
                            const std::string &png_path, std::string *err) {
    /* 收拢到主屏范围（点坐标，与 screencapture -R 一致） */
    NSScreen *screen = [NSScreen mainScreen];
    NSRect sf = screen.frame;
    if (x < sf.origin.x) x = (int)sf.origin.x;
    if (y < sf.origin.y) y = (int)sf.origin.y;
    if (x + w > sf.origin.x + sf.size.width)  w = (int)(sf.origin.x + sf.size.width)  - x;
    if (y + h > sf.origin.y + sf.size.height) h = (int)(sf.origin.y + sf.size.height) - y;
    if (w <= 0 || h <= 0) { if (err) *err = "区域无效"; return false; }

    std::string cmd = "screencapture -x -R" + std::to_string(x) + "," +
                      std::to_string(y) + "," + std::to_string(w) + "," +
                      std::to_string(h) + " '" + png_path + "'";
    int rc = system(cmd.c_str());
    FILE *f = fopen(png_path.c_str(), "rb");
    bool ok = (rc == 0) && f;
    long sz = 0;
    if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); fclose(f); }
    if (!ok || sz < 100) { if (err) *err = "区域截图失败"; return false; }
    return true;
}

bool mouse_position(int *x, int *y) {
    NSPoint p = [NSEvent mouseLocation];   /* 左下原点 */
    NSScreen *screen = [NSScreen mainScreen];
    NSRect sf = screen.frame;
    *x = (int)p.x;
    *y = (int)(sf.origin.y + sf.size.height - p.y);   /* 转为左上原点 */
    return true;
}

} // namespace fanyi
