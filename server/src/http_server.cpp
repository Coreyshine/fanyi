/*
 * fanyi — http_server.cpp
 * 本地服务实现：HTTP API、配置热更新、模型懒加载/空闲卸载。
 *
 * API：
 *   GET  /              → 302 /settings
 *   GET  /settings      → 设置页面（静态文件）
 *   GET  /v1/status     → {version,model_loaded,model_path,enabled,...}
 *   GET  /v1/config     → 当前配置
 *   POST /v1/config     → 更新配置（持久化；换模型/参数自动重载）
 *   POST /v1/translate  → {"segments":[..],"target":"zh"} → NDJSON 流 {"i":n,"text":".."}
 *
 * 安全：仅绑定 127.0.0.1；浏览器跨源请求只放行扩展 Origin。
 */
#include "http_server.h"

#include "config.h"
#include "fanyi/translator.h"
#include "model_download.h"

#define CPPHTTPLIB_THREAD_POOL_COUNT 8
#include "httplib.h"

#include "cJSON.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <signal.h>
#endif

namespace fanyi {

static std::string lang_name_zh(const std::string &code) {
    /* 模型提示词要求目标语言用中文名 */
    if (code == "zh") return "中文";
    if (code == "zh-TW") return "繁体中文";
    if (code == "en") return "英语";
    if (code == "ja") return "日语";
    if (code == "ko") return "韩语";
    if (code == "fr") return "法语";
    if (code == "de") return "德语";
    if (code == "es") return "西班牙语";
    if (code == "ru") return "俄语";
    if (code == "pt") return "葡萄牙语";
    if (code == "it") return "意大利语";
    if (code == "ar") return "阿拉伯语";
    if (code == "th") return "泰语";
    if (code == "vi") return "越南语";
    if (code == "id") return "印尼语";
    if (code == "ms") return "马来语";
    if (code == "hi") return "印地语";
    if (code == "tr") return "土耳其语";
    if (code == "pl") return "波兰语";
    if (code == "nl") return "荷兰语";
    if (code == "uk") return "乌克兰语";
    return code; /* 未知代码原样传入 */
}

/* 段完成回调（fanyi_translate 要求 C 函数指针） */
static void seg_done_cb(void *ud, size_t idx, const char *text) {
    auto *sink = static_cast<httplib::DataSink *>(ud);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "i", (double)idx);
    cJSON_AddStringToObject(o, "text", text);
    char *line = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (line) {
        std::string l = line;
        l += '\n';
        cJSON_free(line);
        sink->write(l.data(), l.size());
    }
}

struct Service::Impl {
    fanyi_cfg cfg;
    std::mutex cfg_mtx;            /* 保护 cfg */
    std::mutex model_mtx;          /* 保护模型生命周期（创建/销毁） */
    fanyi_translator *tr = nullptr;
    std::atomic<bool> model_failed{false};
    std::string model_err;

    httplib::Server svr;
    std::thread http_thread;
    std::thread idle_thread;
    std::string cfg_path;
    std::string model_path_resolved;

    std::atomic<bool> running{false};
    std::atomic<bool> shutdown_req{false};
    std::atomic<long long> last_use_ms{0};

    static long long now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    /* 懒加载模型（model_mtx 内调用） */
    bool ensure_model_locked(std::string *err) {
        if (tr) { last_use_ms.store(now_ms()); return true; }
        fanyi_params p;
        fanyi_params_default(&p);
        {
            std::lock_guard<std::mutex> g(cfg_mtx);
            char *mp = fanyi_cfg_find_model(&cfg);
            if (!mp) {
                model_failed.store(true);
                model_err = "模型未安装：请打开设置页（/settings）下载模型，或在设置里指定模型路径";
                if (err) *err = model_err;
                return false;
            }
            model_path_resolved = mp;
            free(mp);
            p.model_path     = model_path_resolved.c_str();
            p.ctx_tokens     = cfg.ctx_tokens;
            p.max_parallel   = cfg.max_parallel;
            p.greedy         = cfg.precision_mode;
        }
        char merr[512] = {0};
        tr = fanyi_create(&p, merr, sizeof merr);
        if (!tr) {
            model_failed.store(true);
            model_err = merr;
            if (err) *err = std::string("模型加载失败: ") + merr;
            return false;
        }
        model_failed.store(false);
        last_use_ms.store(now_ms());
        fprintf(stderr, "[fanyi] 模型已加载: %s\n", model_path_resolved.c_str());
        return true;
    }

    void unload_if_idle() {
        int idle_min;
        { std::lock_guard<std::mutex> g(cfg_mtx); idle_min = cfg.idle_unload_min; }
        if (idle_min <= 0) return;
        if (now_ms() - last_use_ms.load() <= (long long)idle_min * 60000) return;
        std::lock_guard<std::mutex> g(model_mtx);
        if (tr && now_ms() - last_use_ms.load() > (long long)idle_min * 60000) {
            fanyi_destroy(tr);
            tr = nullptr;
            fprintf(stderr, "[fanyi] 空闲超时，已卸载模型\n");
        }
    }

    void idle_watchdog() {
        while (!shutdown_req.load()) {
            /* 1 秒粒度轮询：总周期 15 秒，但退出响应 ≤1 秒 */
            for (int i = 0; i < 15 && !shutdown_req.load(); i++)
                std::this_thread::sleep_for(std::chrono::seconds(1));
            if (shutdown_req.load()) return;
            unload_if_idle();
        }
    }

    void save_cfg(char *err, size_t n) { fanyi_cfg_save(&cfg, cfg_path.c_str(), err, n); }
};

Service::Service() : impl_(new Impl) {
    char *path = fanyi_cfg_default_path();
    if (path) { impl_->cfg_path = path; free(path); }
    char err[256];
    fanyi_cfg_load(&impl_->cfg, impl_->cfg_path.c_str(), err, sizeof err);
}

Service::~Service() { stop(); delete impl_; }
Service *Service::instance() { static Service s; return &s; }

static bool origin_allowed(const char *origin) {
    if (!origin || !*origin) return true;    /* curl/CLI 无 Origin */
    /* 浏览器扩展 */
    if (strstr(origin, "chrome-extension://") == origin ||
        strstr(origin, "moz-extension://") == origin ||
        strstr(origin, "safari-web-extension://") == origin) return true;
    /* 本机回环页面（本地工具/测试床）。外部网站无法伪造 Origin，
       而公网页面 Origin 是 https://域名，不会命中这里。 */
    if (strstr(origin, "http://127.0.0.1:") == origin ||
        strstr(origin, "http://localhost:") == origin) return true;
    return false;
}

static void add_cors(httplib::Response &res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
}

static std::string json_str(const char *s) {
    cJSON *o = cJSON_CreateString(s ? s : "");
    char *r = cJSON_PrintUnformatted(o);
    std::string out(r ? r : "\"\"");
    cJSON_free(r);
    cJSON_Delete(o);
    return out;
}

bool Service::start(std::string *err) {
    Impl &s = *impl_;
    if (s.running.load()) return true;

    auto &svr = s.svr;
    int port;
    { std::lock_guard<std::mutex> g(s.cfg_mtx); port = s.cfg.port; }

    svr.set_payload_max_length(8 * 1024 * 1024);

    svr.Get("/", [](const httplib::Request &, httplib::Response &res) {
        res.set_redirect("/settings");
    });

    svr.Get("/settings", [](const httplib::Request &, httplib::Response &res) {
        std::ifstream f("settings.html", std::ios::binary);
        if (!f) {
            char *dir = fanyi_exe_dir();
            if (dir) {
                f.open(std::string(dir) + "/settings.html", std::ios::binary);
                free(dir);
            }
        }
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            res.set_content(ss.str(), "text/html; charset=utf-8");
        } else {
            res.status = 404;
            res.set_content("settings.html 缺失（请重新构建）", "text/plain; charset=utf-8");
        }
    });

    svr.Options(R"(/v1/.*)", [](const httplib::Request &, httplib::Response &res) {
        add_cors(res);
        res.status = 204;
    });

    svr.Get("/v1/status", [&s](const httplib::Request &, httplib::Response &res) {
        add_cors(res);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "version", fanyi_build_info());
        {
            char *mp = fanyi_cfg_find_model(&s.cfg);   /* 只读探测，无需锁 */
            cJSON_AddBoolToObject(o, "model_installed", mp != nullptr);
            if (mp) { cJSON_AddStringToObject(o, "model_installed_path", mp); free(mp); }
        }
        cJSON_AddBoolToObject(o, "model_loaded", s.tr && fanyi_loaded(s.tr));
        cJSON_AddBoolToObject(o, "model_failed", s.model_failed.load());
        if (s.model_failed.load()) cJSON_AddStringToObject(o, "model_error", s.model_err.c_str());
        {
            std::lock_guard<std::mutex> g(s.cfg_mtx);
            cJSON_AddBoolToObject(o, "enabled", s.cfg.enabled);
            cJSON_AddStringToObject(o, "source_lang", s.cfg.source_lang);
            cJSON_AddStringToObject(o, "target_lang", s.cfg.target_lang);
            cJSON_AddBoolToObject(o, "auto_translate", s.cfg.auto_translate);
            cJSON_AddNumberToObject(o, "foreign_ratio", s.cfg.foreign_ratio);
            cJSON_AddBoolToObject(o, "precision_mode", s.cfg.precision_mode);
            cJSON_AddStringToObject(o, "model_path", s.model_path_resolved.c_str());
        }
        {   /* 下载进度 */
            auto st = model_download_status();
            cJSON *d = cJSON_CreateObject();
            cJSON_AddBoolToObject(d, "active", st.active);
            cJSON_AddNumberToObject(d, "progress", st.progress);
            cJSON_AddStringToObject(d, "mirror", st.mirror.c_str());
            cJSON_AddStringToObject(d, "error", st.error.c_str());
            cJSON_AddBoolToObject(d, "succeeded", st.succeeded);
            cJSON_AddItemToObject(o, "model_download", d);
        }
        char *txt = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        res.set_content(txt ? txt : "{}", "application/json");
        cJSON_free(txt);
    });

    svr.Get("/v1/config", [&s](const httplib::Request &req, httplib::Response &res) {
        if (!origin_allowed(req.get_header_value("Origin").c_str())) { res.status = 403; return; }
        add_cors(res);
        std::lock_guard<std::mutex> g(s.cfg_mtx);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "source_lang", s.cfg.source_lang);
        cJSON_AddStringToObject(o, "target_lang", s.cfg.target_lang);
        cJSON_AddBoolToObject(o, "auto_translate", s.cfg.auto_translate);
        cJSON_AddNumberToObject(o, "foreign_ratio", s.cfg.foreign_ratio);
        cJSON_AddBoolToObject(o, "precision_mode", s.cfg.precision_mode);
        cJSON_AddNumberToObject(o, "ctx_tokens", s.cfg.ctx_tokens);
        cJSON_AddNumberToObject(o, "max_parallel", s.cfg.max_parallel);
        cJSON_AddNumberToObject(o, "idle_unload_min", s.cfg.idle_unload_min);
        cJSON_AddNumberToObject(o, "port", s.cfg.port);
        cJSON_AddStringToObject(o, "model_path", s.cfg.model_path);
        cJSON_AddBoolToObject(o, "enabled", s.cfg.enabled);
        char *txt = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        res.set_content(txt ? txt : "{}", "application/json");
        cJSON_free(txt);
    });

    svr.Post("/v1/config", [&s](const httplib::Request &req, httplib::Response &res) {
        if (!origin_allowed(req.get_header_value("Origin").c_str())) { res.status = 403; return; }
        add_cors(res);
        cJSON *in = cJSON_Parse(req.body.c_str());
        if (!in) { res.status = 400; res.set_content("{\"error\":\"bad json\"}", "application/json"); return; }
        bool model_dirty = false;
        {
            std::lock_guard<std::mutex> g(s.cfg_mtx);
            cJSON *v;
            if ((v = cJSON_GetObjectItem(in, "source_lang")) && cJSON_IsString(v))
                snprintf(s.cfg.source_lang, sizeof s.cfg.source_lang, "%s", v->valuestring);
            if ((v = cJSON_GetObjectItem(in, "target_lang")) && cJSON_IsString(v))
                snprintf(s.cfg.target_lang, sizeof s.cfg.target_lang, "%s", v->valuestring);
            if ((v = cJSON_GetObjectItem(in, "auto_translate")) && cJSON_IsBool(v))
                s.cfg.auto_translate = cJSON_IsTrue(v);
            if ((v = cJSON_GetObjectItem(in, "foreign_ratio")) && cJSON_IsNumber(v)) {
                double r = v->valuedouble;
                if (r > 0 && r <= 1) s.cfg.foreign_ratio = r;
            }
            if ((v = cJSON_GetObjectItem(in, "precision_mode")) && cJSON_IsBool(v)) {
                if (s.cfg.precision_mode != cJSON_IsTrue(v)) model_dirty = true;
                s.cfg.precision_mode = cJSON_IsTrue(v);
            }
            if ((v = cJSON_GetObjectItem(in, "ctx_tokens")) && cJSON_IsNumber(v)) {
                int nv = (int)v->valuedouble;
                if (nv >= 2048 && nv <= 65536 && nv != s.cfg.ctx_tokens) { s.cfg.ctx_tokens = nv; model_dirty = true; }
            }
            if ((v = cJSON_GetObjectItem(in, "max_parallel")) && cJSON_IsNumber(v)) {
                int nv = (int)v->valuedouble;
                if (nv >= 1 && nv <= 32 && nv != s.cfg.max_parallel) { s.cfg.max_parallel = nv; model_dirty = true; }
            }
            if ((v = cJSON_GetObjectItem(in, "idle_unload_min")) && cJSON_IsNumber(v))
                s.cfg.idle_unload_min = (int)v->valuedouble;
            if ((v = cJSON_GetObjectItem(in, "port")) && cJSON_IsNumber(v)) {
                int nv = (int)v->valuedouble;
                if (nv > 0 && nv <= 65535) s.cfg.port = nv;
            }
            if ((v = cJSON_GetObjectItem(in, "model_path")) && cJSON_IsString(v)) {
                if (strcmp(s.cfg.model_path, v->valuestring) != 0) {
                    snprintf(s.cfg.model_path, sizeof s.cfg.model_path, "%s", v->valuestring);
                    model_dirty = true;
                }
            }
            if ((v = cJSON_GetObjectItem(in, "enabled")) && cJSON_IsBool(v))
                s.cfg.enabled = cJSON_IsTrue(v);
            char serr[256] = {0};
            s.save_cfg(serr, sizeof serr);
        }
        if (model_dirty) {   /* 锁外销毁，下次请求重载；不与 cfg_mtx 嵌套 */
            std::lock_guard<std::mutex> g(s.model_mtx);
            if (s.tr) { fanyi_destroy(s.tr); s.tr = nullptr; }
            s.model_failed.store(false);
        }
        cJSON_Delete(in);
        res.set_content("{\"ok\":true,\"note\":\"换模型/并行参数已重载；端口修改需重启服务\"}", "application/json");
    });

    /* 模型下载：POST /v1/model/download
       {"cancel":true} 取消；{"url":"自定义镜像"} 自定义源优先；否则自动镜像表 */
    svr.Post("/v1/model/download", [&s](const httplib::Request &req, httplib::Response &res) {
        if (!origin_allowed(req.get_header_value("Origin").c_str())) { res.status = 403; return; }
        add_cors(res);
        cJSON *in = cJSON_Parse(req.body.c_str());
        bool cancel = false;
        std::string custom_url;
        if (in) {
            cJSON *v;
            if ((v = cJSON_GetObjectItem(in, "cancel")) && cJSON_IsTrue(v)) cancel = true;
            if ((v = cJSON_GetObjectItem(in, "url")) && cJSON_IsString(v) && v->valuestring[0])
                custom_url = v->valuestring;
        }
        if (cancel) {
            model_download_cancel();
            cJSON_Delete(in);
            res.set_content("{\"ok\":true,\"cancelled\":true}", "application/json");
            return;
        }
        if (model_download_status().active) {
            cJSON_Delete(in);
            res.set_content("{\"ok\":false,\"error\":\"下载已在进行中\"}", "application/json");
            return;
        }
        {
            char *mp = fanyi_cfg_find_model(&s.cfg);
            if (mp) {
                free(mp);
                cJSON_Delete(in);
                res.set_content("{\"ok\":false,\"error\":\"模型已安装，无需下载\"}", "application/json");
                return;
            }
        }
        std::vector<std::string> mirrors = model_default_mirrors();
        if (!custom_url.empty()) mirrors.insert(mirrors.begin(), custom_url);   /* 自定义源优先，失败自动回退内置表 */

        char *dir = fanyi_cfg_user_models_dir();
        if (!dir) {
            cJSON_Delete(in);
            res.status = 500;
            res.set_content("{\"ok\":false,\"error\":\"无法确定用户模型目录\"}", "application/json");
            return;
        }
        std::string user_dir(dir);
        free(dir);
        cJSON_Delete(in);
        Impl *impl = &s;
        /* 内置镜像用已知精确大小做完整性校验；自定义镜像按其实际 Content-Length 校验 */
        long long expect = custom_url.empty() ? 1908528192LL : 0;
        model_download_start(user_dir, "Hy-MT2-1.8B-Q8_0.gguf", mirrors,
                             expect,
                             [](void *ud) {   /* 下载成功后立即加载 */
                                 auto *p = static_cast<Impl *>(ud);
                                 std::lock_guard<std::mutex> g(p->model_mtx);
                                 std::string e;
                                 p->ensure_model_locked(&e);
                             }, impl);
        res.set_content("{\"ok\":true,\"started\":true}", "application/json");
    });

    svr.Post("/v1/translate", [&s](const httplib::Request &req, httplib::Response &res) {
        if (!origin_allowed(req.get_header_value("Origin").c_str())) { res.status = 403; return; }
        cJSON *in = cJSON_Parse(req.body.c_str());
        if (!in) { res.status = 400; res.set_content("{\"error\":\"bad json\"}", "application/json"); return; }
        cJSON *jsegs = cJSON_GetObjectItem(in, "segments");
        if (!cJSON_IsArray(jsegs) || cJSON_GetArraySize(jsegs) == 0) {
            cJSON_Delete(in);
            res.status = 400;
            res.set_content("{\"error\":\"segments required\"}", "application/json");
            return;
        }
        std::string target;
        {
            std::lock_guard<std::mutex> g(s.cfg_mtx);
            cJSON *jt = cJSON_GetObjectItem(in, "target");
            target = lang_name_zh(jt && cJSON_IsString(jt) ? jt->valuestring : s.cfg.target_lang);
        }
        std::vector<std::string> segtexts;
        segtexts.reserve((size_t)cJSON_GetArraySize(jsegs));
        cJSON *it = nullptr;
        cJSON_ArrayForEach(it, jsegs) {
            const char *v = cJSON_IsString(it) ? it->valuestring : "";
            segtexts.emplace_back(v ? v : "");
        }
        cJSON_Delete(in);
        size_t n = segtexts.size();

        std::string merr;
        {
            std::lock_guard<std::mutex> g(s.model_mtx);
            if (!s.ensure_model_locked(&merr)) {
                res.status = 503;
                res.set_content("{\"error\":" + json_str(merr.c_str()) + "}", "application/json");
                return;
            }
        }

        add_cors(res);
        res.set_chunked_content_provider(
            "application/x-ndjson",
            [&s, segtexts, n, target](size_t, httplib::DataSink &sink) {
                std::vector<const char *> segs(n);
                for (size_t i = 0; i < n; i++) segs[i] = segtexts[i].c_str();
                auto t0 = std::chrono::steady_clock::now();

                char **out = (char **)calloc(n ? n : 1, sizeof(char *));
                char terr[512] = {0};
                int rc = fanyi_translate(s.tr, segs.data(), n, target.c_str(), out,
                                         seg_done_cb, &sink, terr, sizeof terr);
                if (rc != 0) {
                    std::string line = "{\"error\":" + json_str(terr) + "}\n";
                    sink.write(line.data(), line.size());
                }
                if (out) {
                    for (size_t k = 0; k < n; k++) fanyi_strfree(out[k]);
                    free(out);
                }
                s.last_use_ms.store(Impl::now_ms());
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                fprintf(stderr, "[fanyi] 批量 %zu 段，耗时 %lld ms\n", n, (long long)ms);
                /* 本版 httplib：必须显式 done() 发送 chunked 终帧 */
                sink.done();
                return false;
            });
    });

    if (!svr.bind_to_port("127.0.0.1", port)) {
        if (err) *err = "端口被占用: " + std::to_string(port);
        return false;
    }
    s.http_thread = std::thread([&svr] { svr.listen_after_bind(); });
    s.running.store(true);
    s.idle_thread = std::thread([impl = &s] { impl->idle_watchdog(); });

#ifdef _WIN32
    SetConsoleCtrlHandler([](DWORD t) -> BOOL {
        if (t == CTRL_C_EVENT || t == CTRL_CLOSE_EVENT) {
            Service::instance()->request_shutdown();
            return TRUE;
        }
        return FALSE;
    }, TRUE);
#else
    signal(SIGINT, [](int) { Service::instance()->request_shutdown(); });
    signal(SIGTERM, [](int) { Service::instance()->request_shutdown(); });
    signal(SIGPIPE, SIG_IGN);
#endif
    if (err) err->clear();
    return true;
}

void Service::stop() {
    Impl &s = *impl_;
    if (!s.running.exchange(false)) return;
    s.shutdown_req.store(true);
    s.svr.stop();
    if (s.http_thread.joinable()) s.http_thread.join();
    if (s.idle_thread.joinable()) s.idle_thread.join();
    { std::lock_guard<std::mutex> g(s.model_mtx); if (s.tr) { fanyi_destroy(s.tr); s.tr = nullptr; } }
}

void Service::run_until_shutdown() {
    while (!impl_->shutdown_req.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop();
}

void Service::request_shutdown() { impl_->shutdown_req.store(true); }

bool Service::shutdown_requested() const { return impl_->shutdown_req.load(); }

void Service::override_port(int port) {
    std::lock_guard<std::mutex> g(impl_->cfg_mtx);
    if (port > 0 && port <= 65535) impl_->cfg.port = port;
}

bool Service::toggle_enabled() {
    Impl &s = *impl_;
    std::lock_guard<std::mutex> g(s.cfg_mtx);
    s.cfg.enabled = !s.cfg.enabled;
    char err[256] = {0};
    s.save_cfg(err, sizeof err);
    return s.cfg.enabled;
}

bool Service::enabled() const {
    std::lock_guard<std::mutex> g(impl_->cfg_mtx);
    return impl_->cfg.enabled;
}

std::string Service::settings_url() const {
    Impl &s = *impl_;
    int port;
    { std::lock_guard<std::mutex> g(s.cfg_mtx); port = s.cfg.port; }
    return "http://127.0.0.1:" + std::to_string(port) + "/settings";
}

} // namespace fanyi
