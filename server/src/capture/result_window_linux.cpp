/*
 * fanyi — result_window_linux.cpp
 * 结果浮窗（Linux v1.1 简化）：通过系统通知展示译文。
 * （后续版本可升级为 GTK 浮窗 + 复制按钮）
 */
#include "capture.h"

#include <cstdio>
#include <string>

namespace fanyi {

void result_window_show(const std::string &original,
                        const std::string &translated,
                        const std::string &target_name) {
    std::string cmd = "notify-send -a fanyi -t 10000 '翻译结果 (" + target_name + ")' '" +
                      translated + "' 2>/dev/null &";
    // 简单转义单引号
    std::string safe;
    for (char c : cmd) {
        if (c == '\'') safe += "'\\''";
        else safe += c;
    }
    std::string full = "sh -c '" + safe + "'";
    FILE *p = popen(full.c_str(), "r");
    if (p) pclose(p);
}

} // namespace fanyi
