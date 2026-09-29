/*
 * fanyi — tray_linux.cpp
 * Linux 托盘（libayatana-appindicator，可选编译：-DFANYI_TRAY=ON）。
 * 由于 appindicator 自带主循环集成，这里用 glib timeout 轮询退出标志。
 */
#include "tray.h"

#ifdef FANYI_HAS_TRAY
#include <libayatana-appindicator/app-indicator.h>
#include <atomic>

namespace fanyi {

static TrayActions g_actions;
static std::atomic<bool> g_quit{false};

static void on_settings(GtkMenu *, gpointer) {
    gchar *cmd = g_strdup_printf("xdg-open %s", g_actions.settings_url);
    int ignored = system(cmd);
    (void)ignored;
    g_free(cmd);
}
static void on_toggle(GtkMenu *, gpointer) { g_actions.toggle_enabled(); }
static void on_select(GtkMenu *, gpointer) { g_actions.select_translate(); }
static void on_capture(GtkMenu *, gpointer) { g_actions.capture_translate(); }
static void on_quit(GtkMenu *, gpointer) {
    g_actions.quit();
    g_quit.store(true);
    exit(0);
}

int tray_run(const TrayActions &actions) {
    g_actions = actions;
    gtk_init(0, nullptr);

    AppIndicator *ind = app_indicator_new("fanyi", "preferences-desktop-locale",
                                          APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    app_indicator_set_status(ind, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_title(ind, "fanyi 本地翻译");

    GtkWidget *menu = gtk_menu_new();
    struct { const char *label; GCallback cb; } items[] = {
        { "打开设置…", G_CALLBACK(on_settings) },
        { "翻译 开/暂停", G_CALLBACK(on_toggle) },
        { "划词翻译", G_CALLBACK(on_select) },
        { "截图翻译", G_CALLBACK(on_capture) },
        { "退出 fanyi", G_CALLBACK(on_quit) },
    };
    for (auto &it : items) {
        GtkWidget *mi = gtk_menu_item_new_with_label(it.label);
        g_signal_connect(mi, "activate", it.cb, nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
    gtk_widget_show_all(menu);
    app_indicator_set_menu(ind, GTK_MENU(menu));

    gtk_main();   /* 退出依赖 on_quit 的 exit(0) */
    return 0;
}

void tray_request_stop() { exit(0); }

} // namespace fanyi
#endif
