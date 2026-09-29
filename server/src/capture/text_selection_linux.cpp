/*
 * fanyi — text_selection_linux.cpp
 * Linux 划词（X11）：xdotool 模拟 Ctrl+C + xclip 读取（需安装两工具，文档说明）。
 */
#include "capture.h"

#include <cstdio>
#include <string>

namespace fanyi {

static std::string run_cmd(const char *cmd) {
    FILE *p = popen(cmd, "r");
    if (!p) return "";
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

std::string capture_selected_text(std::string *err) {
    run_cmd("xdotool key --clearmodifiers ctrl+c 2>/dev/null");
    std::string out = run_cmd("xclip -selection clipboard -o 2>/dev/null");
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    if (out.empty()) {
        if (err) *err = "未取到选中文本（需安装 xdotool 与 xclip：sudo apt install xdotool xclip）";
        return "";
    }
    return out;
}

} // namespace fanyi
