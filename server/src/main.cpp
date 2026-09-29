/*
 * fanyi — main.cpp
 * 服务入口：加载配置、启动 HTTP 服务、托盘事件循环。
 */
#include "http_server.h"
#include "tray.h"
#include "config.h"
#include "fanyi/translator.h"
#include "capture/capture.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using fanyi::Service;

static Service *g_svc = nullptr;
static bool tray_enabled_fn()        { return g_svc->enabled(); }
static void tray_toggle_fn()         { g_svc->toggle_enabled(); }
static void tray_quit_fn()           { g_svc->request_shutdown(); }

/* 划词/截图翻译：独立线程执行（取词 → 翻译 → 浮窗），不阻塞托盘 */
static void run_select_translate_async() {
    Service *svc = g_svc;
    std::thread([svc] {
        std::string err;
        std::string text = fanyi::capture_selected_text(&err);
        if (text.empty()) {
            fanyi::result_window_show("划词翻译", err.empty() ? "未取到选中文本，请先选中文字" : err, "提示");
            return;
        }
        std::string terr;
        std::string out = svc->translate_text(text, &terr);
        if (out.empty()) out = terr.empty() ? "翻译失败" : terr;
        fanyi::result_window_show(text, out, svc->target_lang_name());
    }).detach();
}
static void tray_select_fn()         { run_select_translate_async(); }
static void tray_capture_fn()        {
    /* 截图翻译在 v1.1-beta 接入 OCR，此处先提示 */
    fanyi::result_window_show("截图翻译", "截图翻译将在 v1.1-beta 版本提供，敬请期待", "提示");
}

#if defined(__APPLE__) || defined(_WIN32)
static int run_with_tray(Service *svc) {
    g_svc = svc;
    static const std::string url = svc->settings_url();
    fanyi::TrayActions actions;
    actions.settings_url   = url.c_str();
    actions.enabled        = tray_enabled_fn;
    actions.toggle_enabled = tray_toggle_fn;
    actions.quit           = tray_quit_fn;
    actions.select_translate  = tray_select_fn;
    actions.capture_translate = tray_capture_fn;

    /* 信号（Ctrl-C/服务管理器）→ 停止托盘事件循环 */
    std::thread watcher([svc] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            if (svc->shutdown_requested()) break;
        }
        fanyi::tray_request_stop();
    });
    fanyi::tray_run(actions);
    watcher.join();
    svc->stop();
    return 0;
}
#endif

int main(int argc, char **argv) {
    bool no_tray = false;
    int port_override = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-tray") == 0)      no_tray = true;
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port_override = atoi(argv[++i]);
        else if (strcmp(argv[i], "--version") == 0) { printf("%s\n", fanyi_build_info()); return 0; }
        else if (strcmp(argv[i], "--help") == 0) {
            printf("fanyi-server — 本地模型网页翻译服务\n"
                   "  --port N      覆盖端口（默认 8765）\n"
                   "  --no-tray     不显示托盘图标\n"
                   "  --version     版本信息\n");
            return 0;
        }
    }

    Service *svc = Service::instance();
    if (port_override > 0) svc->override_port(port_override);   /* 临时覆盖，不写回配置 */

    std::string err;
    if (!svc->start(&err)) {
        fprintf(stderr, "[fanyi] 启动失败: %s\n", err.c_str());
        return 1;
    }
    fprintf(stderr, "[fanyi] %s 已启动，设置页: %s\n", fanyi_build_info(), svc->settings_url().c_str());

#if defined(__APPLE__) || defined(_WIN32)
    if (!no_tray) return run_with_tray(svc);
#endif
    svc->run_until_shutdown();   /* headless：Ctrl-C / SIGTERM 由 HTTP 层信号处理器触发 */
    return 0;
}
