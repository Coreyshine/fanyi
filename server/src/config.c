/*
 * fanyi — config.c
 */
#include "config.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <shlobj.h>
#else
#  include <limits.h>
#  include <unistd.h>
#  ifdef __APPLE__
#    include <mach-o/dyld.h>
#  else
#    include <libgen.h>
#  endif
#endif

void fanyi_cfg_default(fanyi_cfg *c) {
    memset(c, 0, sizeof *c);
    snprintf(c->source_lang, sizeof c->source_lang, "en");
    snprintf(c->target_lang, sizeof c->target_lang, "zh");
    c->auto_translate = true;
    c->foreign_ratio  = 0.6;
    c->precision_mode = false;
    c->ctx_tokens     = 16384;
    c->max_parallel   = 8;
    c->idle_unload_min = 10;
    c->port           = FANYI_DEFAULT_PORT;
    c->model_path[0]  = 0;
    c->enabled        = true;
}

static void jset_str(cJSON *o, const char *k, const char *v) { cJSON_AddStringToObject(o, k, v); }

bool fanyi_cfg_save(const fanyi_cfg *c, const char *path, char *err, size_t err_len) {
    cJSON *o = cJSON_CreateObject();
    jset_str(o, "source_lang", c->source_lang);
    jset_str(o, "target_lang", c->target_lang);
    cJSON_AddBoolToObject(o, "auto_translate", c->auto_translate);
    cJSON_AddNumberToObject(o, "foreign_ratio", c->foreign_ratio);
    cJSON_AddBoolToObject(o, "precision_mode", c->precision_mode);
    cJSON_AddNumberToObject(o, "ctx_tokens", c->ctx_tokens);
    cJSON_AddNumberToObject(o, "max_parallel", c->max_parallel);
    cJSON_AddNumberToObject(o, "idle_unload_min", c->idle_unload_min);
    cJSON_AddNumberToObject(o, "port", c->port);
    jset_str(o, "model_path", c->model_path);
    cJSON_AddBoolToObject(o, "enabled", c->enabled);

    char *s = cJSON_Print(o);
    cJSON_Delete(o);
    if (!s) { if (err) snprintf(err, err_len, "JSON 序列化失败"); return false; }

    FILE *f = fopen(path, "wb");
    if (!f) { if (err) snprintf(err, err_len, "无法写入 %s", path); cJSON_free(s); return false; }
    fputs(s, f);
    fputc('\n', f);
    fclose(f);
    cJSON_free(s);
    if (err) err[0] = 0;
    return true;
}

bool fanyi_cfg_load(fanyi_cfg *c, const char *path, char *err, size_t err_len) {
    fanyi_cfg_default(c);
    if (!path) { if (err) err[0] = 0; return false; }
    FILE *f = fopen(path, "rb");
    if (!f) { if (err) err[0] = 0; return false; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 1 << 20) { fclose(f); if (err) err[0] = 0; return false; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = 0;

    cJSON *o = cJSON_Parse(buf);
    free(buf);
    if (!o) { if (err) err[0] = 0; return false; }

    cJSON *v;
    if ((v = cJSON_GetObjectItem(o, "source_lang")) && cJSON_IsString(v))
        snprintf(c->source_lang, sizeof c->source_lang, "%s", v->valuestring);
    if ((v = cJSON_GetObjectItem(o, "target_lang")) && cJSON_IsString(v))
        snprintf(c->target_lang, sizeof c->target_lang, "%s", v->valuestring);
    if ((v = cJSON_GetObjectItem(o, "auto_translate")) && cJSON_IsBool(v))
        c->auto_translate = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(o, "foreign_ratio")) && cJSON_IsNumber(v))
        c->foreign_ratio = v->valuedouble;
    if ((v = cJSON_GetObjectItem(o, "precision_mode")) && cJSON_IsBool(v))
        c->precision_mode = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(o, "ctx_tokens")) && cJSON_IsNumber(v))
        c->ctx_tokens = (int)v->valuedouble;
    if ((v = cJSON_GetObjectItem(o, "max_parallel")) && cJSON_IsNumber(v))
        c->max_parallel = (int)v->valuedouble;
    if ((v = cJSON_GetObjectItem(o, "idle_unload_min")) && cJSON_IsNumber(v))
        c->idle_unload_min = (int)v->valuedouble;
    if ((v = cJSON_GetObjectItem(o, "port")) && cJSON_IsNumber(v))
        c->port = (int)v->valuedouble;
    if ((v = cJSON_GetObjectItem(o, "model_path")) && cJSON_IsString(v))
        snprintf(c->model_path, sizeof c->model_path, "%s", v->valuestring);
    if ((v = cJSON_GetObjectItem(o, "enabled")) && cJSON_IsBool(v))
        c->enabled = cJSON_IsTrue(v);
    cJSON_Delete(o);

    if (c->foreign_ratio <= 0 || c->foreign_ratio > 1) c->foreign_ratio = 0.6;
    if (c->port <= 0 || c->port > 65535) c->port = FANYI_DEFAULT_PORT;
    if (c->max_parallel < 1) c->max_parallel = 1;
    if (c->max_parallel > 32) c->max_parallel = 32;
    if (c->ctx_tokens < 2048) c->ctx_tokens = 2048;
    if (err) err[0] = 0;
    return true;
}

/* ---- 平台路径 ---- */
static void mkdir_p(const char *dir) {
#ifdef _WIN32
    /* 逐级创建（CreateDirectoryA 不支持中间目录） */
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *p = tmp; *p; p++) {
        if (*p == '\\' && p != tmp && *(p - 1) != ':') {
            *p = 0;
            CreateDirectoryA(tmp, NULL);
            *p = '\\';
        }
    }
    CreateDirectoryA(tmp, NULL);
#else
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
    int ignored = system(cmd);
    (void)ignored;
#endif
}

char *fanyi_cfg_default_path(void) {
    char dir[900], path[1024];
#ifdef _WIN32
    char appdata[MAX_PATH];
    if (!SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata)) return NULL;
    snprintf(dir, sizeof dir, "%s\\fanyi", appdata);
    mkdir_p(dir);
    snprintf(path, sizeof path, "%s\\config.json", dir);
#elif defined(__APPLE__)
    const char *home = getenv("HOME");
    if (!home) return NULL;
    snprintf(dir, sizeof dir, "%s/Library/Application Support/fanyi", home);
    mkdir_p(dir);
    snprintf(path, sizeof path, "%s/config.json", dir);
#else
    const char *home = getenv("HOME");
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg) snprintf(dir, sizeof dir, "%s/fanyi", xdg);
    else if (home) snprintf(dir, sizeof dir, "%s/.config/fanyi", home);
    else return NULL;
    mkdir_p(dir);
    snprintf(path, sizeof path, "%s/config.json", dir);
#endif
    return strdup(path);
}

/* 可执行文件所在目录（用于探测 models/） */
static bool exe_dir(char *out, size_t cap) {
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (n == 0 || n >= cap) return false;
    char *slash = strrchr(out, '\\');
    if (slash) *slash = 0;
    return true;
#elif defined(__APPLE__)
    uint32_t sz = (uint32_t)cap;
    if (_NSGetExecutablePath(out, &sz) != 0) return false;
    char *slash = strrchr(out, '/');
    if (slash) *slash = 0;
    return true;
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) return false;
    out[n] = 0;
    char *slash = strrchr(out, '/');
    if (slash) *slash = 0;
    return true;
#endif
}

/* 可执行文件所在目录（用于探测 models/），调用者 free() */
char *fanyi_exe_dir(void) {
    char exe[1024];
    if (!exe_dir(exe, sizeof exe)) return NULL;
    return strdup(exe);
}

/* 用户模型目录（下载的模型放这里），调用者 free() */
char *fanyi_cfg_user_models_dir(void) {
    char base[900];
#ifdef _WIN32
    char appdata[MAX_PATH];
    if (!SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata)) return NULL;
    snprintf(base, sizeof base, "%s\\fanyi\\models", appdata);
    mkdir_p(base);
    return strdup(base);
#elif defined(__APPLE__)
    const char *home = getenv("HOME");
    if (!home) return NULL;
    snprintf(base, sizeof base, "%s/Library/Application Support/fanyi/models", home);
    mkdir_p(base);
    return strdup(base);
#else
    const char *home = getenv("HOME");
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg) snprintf(base, sizeof base, "%s/fanyi/models", xdg);
    else if (home) snprintf(base, sizeof base, "%s/.local/share/fanyi/models", home);
    else return NULL;
    mkdir_p(base);
    return strdup(base);
#endif
}

char *fanyi_cfg_find_model(const fanyi_cfg *c) {
    char exe[1024];
    char cand[1200];
    if (c->model_path[0]) {
        FILE *f = fopen(c->model_path, "rb");
        if (f) { fclose(f); return strdup(c->model_path); }
    }
    if (exe_dir(exe, sizeof exe)) {
        snprintf(cand, sizeof cand, "%s/models/Hy-MT2-1.8B-Q8_0.gguf", exe);
        FILE *f = fopen(cand, "rb");
        if (f) { fclose(f); return strdup(cand); }
        /* CMake 构建布局：build/ 在项目根下 */
        snprintf(cand, sizeof cand, "%s/../models/Hy-MT2-1.8B-Q8_0.gguf", exe);
        f = fopen(cand, "rb");
        if (f) { fclose(f); return strdup(cand); }
        /* macOS .app 包布局：Contents/Resources/models/ */
        snprintf(cand, sizeof cand, "%s/../Resources/models/Hy-MT2-1.8B-Q8_0.gguf", exe);
        f = fopen(cand, "rb");
        if (f) { fclose(f); return strdup(cand); }
    }
    /* 用户模型目录（设置页下载的模型） */
    {
        char *udir = fanyi_cfg_user_models_dir();
        if (udir) {
            snprintf(cand, sizeof cand, "%s/Hy-MT2-1.8B-Q8_0.gguf", udir);
            free(udir);
            FILE *f = fopen(cand, "rb");
            if (f) { fclose(f); return strdup(cand); }
        }
    }
    snprintf(cand, sizeof cand, "models/Hy-MT2-1.8B-Q8_0.gguf");
    FILE *f = fopen(cand, "rb");
    if (f) {
        fclose(f);
#ifdef _WIN32
        _fullpath(cand, cand, sizeof cand);
#else
        char abs[1200];
        if (realpath(cand, abs)) snprintf(cand, sizeof cand, "%s", abs);
#endif
        return strdup(cand);
    }
    return NULL;
}
