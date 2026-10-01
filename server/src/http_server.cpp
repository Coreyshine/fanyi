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
#ifdef FANYI_OCR_ENABLED
#  include "ocr/ocr.h"
#endif

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
#include <filesystem>

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
    std::atomic<bool> ocr_dl{false};

    /* OCR 三件套（det/rec/dict）串行下载，复用单一下载通道与多镜像降级 */
    void ocr_start_download() {
        if (ocr_dl.exchange(true)) return;
        std::thread([this]() {
            struct Item { std::string file; std::vector<std::string> mirrors; long long size; };
            std::vector<Item> items = {
                {"ch_pp-ocrv4_det_infer.onnx",
                 {"https://hf-mirror.com/SWHL/RapidOCR/resolve/main/PP-OCRv4/ch_PP-OCRv4_det_infer.onnx",
                  "https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv4/ch_PP-OCRv4_det_infer.onnx"},
                 4745517},
                {"ch_pp-ocrv4_rec_infer.onnx",
                 {"https://hf-mirror.com/SWHL/RapidOCR/resolve/main/PP-OCRv4/ch_PP-OCRv4_rec_infer.onnx",
                  "https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv4/ch_PP-OCRv4_rec_infer.onnx"},
                 10857958},
                {"ppocr_keys_v1.txt",
                 {"https://cdn.jsdelivr.net/gh/PaddlePaddle/PaddleOCR@main/ppocr/utils/ppocr_keys_v1.txt",
                  "https://raw.githubusercontent.com/PaddlePaddle/PaddleOCR/main/ppocr/utils/ppocr_keys_v1.txt",
                  "https://gitee.com/paddlepaddle/PaddleOCR/raw/main/ppocr/utils/ppocr_keys_v1.txt"},
                 26250},
            };
            char *ud = fanyi_cfg_user_models_dir();
            if (!ud) { ocr_dl = false; return; }
            std::string dir = std::string(ud) + "/ocr";
            free(ud);
            std::filesystem::create_directories(dir);   /* 子目录必须先建，否则 curl 落盘失败 */

            for (auto &it : items) {
                std::string path = dir + "/" + it.file;
                std::ifstream f(path, std::ios::binary);
                if (f.good()) continue;   /* 已存在 */
                model_download_start(dir, it.file, it.mirrors, it.size, nullptr, nullptr);
                for (;;) {   /* 等当前文件结束 */
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    auto st = model_download_status();
                    if (!st.active) break;
                }
                if (!model_download_status().succeeded) { ocr_dl = false; return; }
            }
            ocr_dl = false;
        }).detach();
    }
    bool ocr_downloading() const { return ocr_dl.load(); }
    void ocr_cancel() {
        model_download_cancel();
        ocr_dl.store(false);
    }

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
            cJSON_AddBoolToObject(o, "video_subtitle", s.cfg.video_subtitle);
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
        cJSON_AddStringToObject(o, "model_file", s.cfg.model_file);
        cJSON_AddBoolToObject(o, "enabled", s.cfg.enabled);
        cJSON_AddBoolToObject(o, "video_subtitle", s.cfg.video_subtitle);
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
            if ((v = cJSON_GetObjectItem(in, "model_file")) && cJSON_IsString(v)) {
                if (strcmp(s.cfg.model_file, v->valuestring) != 0) {
                    snprintf(s.cfg.model_file, sizeof s.cfg.model_file, "%s", v->valuestring);
                    model_dirty = true;   /* 切换量化：卸载后按新文件名重载 */
                }
            }
            if ((v = cJSON_GetObjectItem(in, "video_subtitle")) && cJSON_IsBool(v))
                s.cfg.video_subtitle = cJSON_IsTrue(v);
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

    /* 模型目录：GET /v1/models —— 三档量化 + 安装状态 + 按设备内存推荐 */
    svr.Get("/v1/models", [&s](const httplib::Request &, httplib::Response &res) {
        add_cors(res);
        int ram_gb = fanyi_device_ram_gb();
        const char *recommend =
            ram_gb >= 16 ? "Q8_0" :
            ram_gb >= 8  ? "Q6_K" : "Q4_K_M";

        char installed[3][64];
        int n_installed = fanyi_cfg_installed_models(installed, 3);

        /* 当前激活文件名：配置指定 > 自动探测到的 */
        std::string active;
        {
            std::lock_guard<std::mutex> g(s.cfg_mtx);
            if (s.cfg.model_file[0]) active = s.cfg.model_file;
            else {
                char *mp = fanyi_cfg_find_model(&s.cfg);
                if (mp) {
                    active = mp;
                    free(mp);
                    auto pos = active.find_last_of("/\\");
                    if (pos != std::string::npos) active = active.substr(pos + 1);
                }
            }
        }

        struct Meta { const char *quality, *speed, *fit; };
        Meta meta[3] = {
            {"近无损",  "标准", "16GB+ 内存，追求精准"},
            {"接近无损", "标准", "8–16GB 内存，均衡之选"},
            {"有折损",  "最快", "8GB 以下低配电脑"},
        };

        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "device_ram_gb", ram_gb);
        cJSON_AddStringToObject(o, "recommended", recommend);
        cJSON *arr = cJSON_CreateArray();
        auto cat = model_catalog();
        for (size_t i = 0; i < cat.size(); i++) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "id", cat[i].id.c_str());
            cJSON_AddStringToObject(m, "file", cat[i].file.c_str());
            cJSON_AddNumberToObject(m, "size_gb", atof(cat[i].size_text.c_str()));
            cJSON_AddStringToObject(m, "size_text", cat[i].size_text.c_str());
            cJSON_AddStringToObject(m, "ram_text", cat[i].ram_text.c_str());
            cJSON_AddStringToObject(m, "quality", meta[i].quality);
            cJSON_AddStringToObject(m, "speed", meta[i].speed);
            cJSON_AddStringToObject(m, "fit", meta[i].fit);
            bool inst = false, act = false;
            for (int k = 0; k < n_installed; k++)
                if (cat[i].file == installed[k]) inst = true;
            if (active == cat[i].file) { act = true; inst = true; }
            cJSON_AddBoolToObject(m, "installed", inst);
            cJSON_AddBoolToObject(m, "active", act);
            cJSON_AddBoolToObject(m, "recommended", strcmp(cat[i].id.c_str(), recommend) == 0);
            cJSON_AddItemToArray(arr, m);
        }
        cJSON_AddItemToObject(o, "models", arr);
        char *txt = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        res.set_content(txt ? txt : "{}", "application/json");
        cJSON_free(txt);
    });

    /* OCR 模型下载（三件套串行，复用单一下载通道）：POST /v1/ocr/download {"cancel":true} */
    svr.Get("/v1/ocr/status", [&s](const httplib::Request &, httplib::Response &res) {
        add_cors(res);
        char *udir = fanyi_cfg_user_models_dir();
        cJSON *o = cJSON_CreateObject();
        bool installed = false;
        if (udir) {
            std::string dir = std::string(udir) + "/ocr";
            free(udir);
            const char *needed[3] = { "ch_pp-ocrv4_det_infer.onnx",
                                      "ch_pp-ocrv4_rec_infer.onnx",
                                      "ppocr_keys_v1.txt" };
            installed = true;
            for (int i = 0; i < 3; i++) {
                std::ifstream f(dir + "/" + needed[i]);
                if (!f.good()) { installed = false; break; }
            }
        }
        auto st = model_download_status();
        cJSON_AddBoolToObject(o, "installed", installed);
        cJSON_AddBoolToObject(o, "dl_active", st.active);
        cJSON_AddNumberToObject(o, "progress", st.progress);
        cJSON_AddStringToObject(o, "mirror", st.mirror.c_str());
        cJSON_AddStringToObject(o, "error", st.error.c_str());
        cJSON_AddBoolToObject(o, "succeeded", st.succeeded);
        char *txt = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        res.set_content(txt ? txt : "{}", "application/json");
        cJSON_free(txt);
    });

    svr.Post("/v1/ocr/download", [&s](const httplib::Request &req, httplib::Response &res) {
        if (!origin_allowed(req.get_header_value("Origin").c_str())) { res.status = 403; return; }
        add_cors(res);
        cJSON *in = cJSON_Parse(req.body.c_str());
        bool cancel = in && cJSON_IsTrue(cJSON_GetObjectItem(in, "cancel"));
        cJSON_Delete(in);
        if (cancel) {
            s.ocr_cancel();
            res.set_content("{\"ok\":true,\"cancelled\":true}", "application/json");
            return;
        }
        if (s.ocr_downloading()) {
            res.set_content("{\"ok\":false,\"error\":\"下载已在进行中\"}", "application/json");
            return;
        }
        s.ocr_start_download();
        res.set_content("{\"ok\":true,\"started\":true}", "application/json");
    });

    /* 调试：POST /v1/ocr/test {"png":"/path/x.png"} → 识别文本（仅本机） */
    svr.Post("/v1/ocr/test", [&s](const httplib::Request &req, httplib::Response &res) {
        if (!origin_allowed(req.get_header_value("Origin").c_str())) { res.status = 403; return; }
#ifdef FANYI_OCR_ENABLED
        cJSON *in = cJSON_Parse(req.body.c_str());
        cJSON *jp = in ? cJSON_GetObjectItem(in, "png") : nullptr;
        if (!cJSON_IsString(jp)) { cJSON_Delete(in); res.status = 400; res.set_content("{\"error\":\"png path required\"}", "application/json"); return; }
        char *udir = fanyi_cfg_user_models_dir();
        std::string dir = std::string(udir ? udir : "") + "/ocr";
        free(udir);
        std::string oerr, text;
        if (!fanyi::ocr_ensure_loaded(dir, &oerr)) {
            cJSON_Delete(in);
            res.set_content("{\"error\":" + json_str(oerr.c_str()) + "}", "application/json");
            return;
        }
        bool ok = fanyi::ocr_image_file(jp->valuestring, &text, &oerr);
        cJSON_Delete(in);
        std::string dbg = fanyi::ocr_last_debug();
        if (ok) res.set_content("{\"text\":" + json_str(text.c_str()) + ",\"debug\":" + json_str(dbg.c_str()) + "}", "application/json");
        else { res.status = 500; res.set_content("{\"error\":" + json_str(oerr.c_str()) + "}", "application/json"); }
#else
        res.set_content("{\"error\":\"本构建未启用 OCR\"}", "application/json");
#endif
    });

    /* 模型下载：POST /v1/model/download
       {"model":"Q6_K"} 按目录下载；{"url":"自定义镜像"} 自定义源优先；
       {"cancel":true} 取消 */
    svr.Post("/v1/model/download", [&s](const httplib::Request &req, httplib::Response &res) {
        if (!origin_allowed(req.get_header_value("Origin").c_str())) { res.status = 403; return; }
        add_cors(res);
        cJSON *in = cJSON_Parse(req.body.c_str());
        bool cancel = false;
        std::string custom_url, model_id;
        if (in) {
            cJSON *v;
            if ((v = cJSON_GetObjectItem(in, "cancel")) && cJSON_IsTrue(v)) cancel = true;
            if ((v = cJSON_GetObjectItem(in, "url")) && cJSON_IsString(v) && v->valuestring[0])
                custom_url = v->valuestring;
            if ((v = cJSON_GetObjectItem(in, "model")) && cJSON_IsString(v) && v->valuestring[0])
                model_id = v->valuestring;
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

        std::string filename;
        long long expect = 0;
        std::vector<std::string> mirrors;
        if (!model_id.empty()) {
            for (auto &e : model_catalog())
                if (e.id == model_id) { filename = e.file; expect = e.size; mirrors = model_mirrors_for(e); break; }
            if (filename.empty()) {
                cJSON_Delete(in);
                res.set_content("{\"ok\":false,\"error\":\"未知模型: " + model_id + "\"}", "application/json");
                return;
            }
            {   /* 该量化已安装则不重复下载 */
                char installed[3][64];
                int n = fanyi_cfg_installed_models(installed, 3);
                for (int k = 0; k < n; k++)
                    if (filename == installed[k]) {
                        cJSON_Delete(in);
                        res.set_content("{\"ok\":false,\"error\":\"该模型已安装，可在设置里切换使用\"}", "application/json");
                        return;
                    }
            }
        } else if (!custom_url.empty()) {
            filename = "Hy-MT2-1.8B-Q8_0.gguf";           /* 自定义源按 Q8_0 处理，大小实测校验 */
            mirrors.push_back(custom_url);
            auto fallback = model_default_mirrors();
            mirrors.insert(mirrors.end(), fallback.begin(), fallback.end());
        } else {
            auto e = model_catalog()[0];                   /* 默认：Q8_0 */
            filename = e.file; expect = e.size; mirrors = model_mirrors_for(e);
        }

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

        struct DoneCtx { Impl *impl; std::string file; };
        DoneCtx *ctx = new DoneCtx{&s, filename};
        Impl *impl = &s;
        model_download_start(user_dir, filename, mirrors, expect,
                             [](void *ud) {   /* 下载成功：登记所选量化 → 保存配置 → 立即加载 */
                                 auto *c = static_cast<DoneCtx *>(ud);
                                 {
                                     std::lock_guard<std::mutex> g(c->impl->cfg_mtx);
                                     snprintf(c->impl->cfg.model_file, sizeof c->impl->cfg.model_file,
                                              "%s", c->file.c_str());
                                     char e2[256] = {0};
                                     c->impl->save_cfg(e2, sizeof e2);
                                 }
                                 std::lock_guard<std::mutex> g(c->impl->model_mtx);
                                 std::string e;
                                 c->impl->ensure_model_locked(&e);
                                 delete c;
                             }, ctx);
        res.set_content("{\"ok\":true,\"started\":true,\"model\":" + json_str(filename.c_str()) + "}", "application/json");
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

void Service::override_port(int port) {
    std::lock_guard<std::mutex> g(impl_->cfg_mtx);
    if (port > 0 && port <= 65535) impl_->cfg.port = port;
}

void Service::request_shutdown() { impl_->shutdown_req.store(true); }

std::string Service::translate_text(const std::string &source, std::string *err) {
    Impl &s = *impl_;
    std::lock_guard<std::mutex> g(s.model_mtx);
    std::string merr;
    if (!s.ensure_model_locked(&merr)) {
        if (err) *err = merr;
        return "";
    }
    std::string target;
    {
        std::lock_guard<std::mutex> g2(s.cfg_mtx);
        target = lang_name_zh(s.cfg.target_lang);
    }
    const char *seg = source.c_str();
    char *out = nullptr;
    char terr[512] = {0};
    int rc = fanyi_translate(s.tr, &seg, 1, target.c_str(), &out, nullptr, nullptr, terr, sizeof terr);
    std::string result;
    if (rc == 0 && out) result = out;
    else if (err) *err = terr[0] ? terr : "翻译失败";
    fanyi_strfree(out);
    s.last_use_ms.store(Impl::now_ms());
    return result;
}

bool Service::shutdown_requested() const { return impl_->shutdown_req.load(); }

std::string Service::target_lang_name() const {
    std::lock_guard<std::mutex> g(impl_->cfg_mtx);
    return lang_name_zh(impl_->cfg.target_lang);
}

bool Service::toggle_video() {
    Impl &s = *impl_;
    std::lock_guard<std::mutex> g(s.cfg_mtx);
    s.cfg.video_subtitle = !s.cfg.video_subtitle;
    char err[256] = {0};
    s.save_cfg(err, sizeof err);
    return s.cfg.video_subtitle;
}

bool Service::video_enabled() const {
    std::lock_guard<std::mutex> g(impl_->cfg_mtx);
    return impl_->cfg.video_subtitle;
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
