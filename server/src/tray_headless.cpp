/*
 * fanyi — tray_headless.cpp
 * 无托盘构建（Linux 默认 / --no-tray）：不进入事件循环，
 * main 用 run_until_shutdown() 等待信号。
 */
#include "tray.h"

namespace fanyi {
int tray_run(const TrayActions &) { return 0; }   /* 不会被调用 */
void tray_request_stop() {}
} // namespace fanyi
