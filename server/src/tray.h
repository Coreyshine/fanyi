/*
 * fanyi — tray.h
 * 托盘图标（各平台实现）。run_* 阻塞调用线程直到退出被请求。
 */
#ifndef FANYI_TRAY_H
#define FANYI_TRAY_H

namespace fanyi {
struct TrayActions {
    const char *settings_url;   /* 打开设置页 */
    bool (*enabled)();          /* 当前总开关状态 */
    void (*toggle_enabled)();   /* 切换总开关 */
    void (*quit)();             /* 请求退出 */
};

/* 返回后进程可以退出。无托盘环境的实现只等待 quit。 */
int tray_run(const TrayActions &actions);

/* 从其它线程请求托盘事件循环退出（mac: 停 NSApp；win: 关窗口；headless: no-op） */
void tray_request_stop();
} // namespace fanyi
#endif
