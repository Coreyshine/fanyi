/*
 * fanyi — screen_capture_mac.mm
 * macOS：系统自带 screencapture -i（用户框选任意区域），零依赖。
 */
#include "screen_capture.h"

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

} // namespace fanyi
