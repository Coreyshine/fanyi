/*
 * fanyi — http_server.h
 * 本地翻译服务：配置 + 模型生命周期 + HTTP API + 设置页。
 */
#ifndef FANYI_HTTP_SERVER_H
#define FANYI_HTTP_SERVER_H

#include <string>

namespace fanyi {

class Service {
public:
    static Service *instance();

    /* 启动 HTTP 监听（非阻塞，失败返回 false 并填 err） */
    bool start(std::string *err);
    void stop();

    /* 阻塞主线程直到 request_shutdown（托盘菜单/信号触发） */
    void run_until_shutdown();

    /* 托盘调用 */
    void request_shutdown();
    bool shutdown_requested() const;
    bool toggle_enabled();          /* 返回切换后的状态 */
    bool enabled() const;
    std::string settings_url() const;

    /* --port 命令行临时覆盖（不写回配置文件） */
    void override_port(int port);

    /* 供划词/截图翻译使用：翻译单段文本（阻塞，返回译文；失败置 err） */
    std::string translate_text(const std::string &source, std::string *err);

    /* 当前目标语言的中文名（用于结果浮窗标题） */
    std::string target_lang_name() const;

    ~Service();

private:
    Service();
    struct Impl;
    Impl *impl_;
};

} // namespace fanyi
#endif
