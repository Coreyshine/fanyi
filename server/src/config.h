/*
 * fanyi — config.h
 * 服务配置：JSON 持久化 + 默认值 + 平台配置目录定位。
 */
#ifndef FANYI_CONFIG_H
#define FANYI_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FANYI_DEFAULT_PORT 8765

typedef struct fanyi_cfg {
    char source_lang[16];      /* 源语言代码，如 "en"；"auto" 交给前端检测 */
    char target_lang[16];      /* 目标语言代码，如 "zh" */
    bool auto_translate;       /* 外文占比超阈值时自动翻译 */
    double foreign_ratio;      /* 自动翻译阈值，默认 0.6 */
    bool precision_mode;       /* 精准模式：贪心解码 */
    int  ctx_tokens;           /* KV 上下文，默认 16384 */
    int  max_parallel;         /* 并行段数，默认 8 */
    int  idle_unload_min;      /* 空闲 N 分钟卸载模型，0=不卸载，默认 10 */
    int  port;                 /* HTTP 端口，默认 8765 */
    char model_path[512];      /* GGUF 路径，空 = 自动探测 */
    bool enabled;              /* 总开关（托盘/设置页可切换） */
} fanyi_cfg;

void fanyi_cfg_default(fanyi_cfg *c);

/* path 为空时使用平台默认路径；返回 false = 读不到则用默认值（不算致命） */
bool fanyi_cfg_load(fanyi_cfg *c, const char *path, char *err, size_t err_len);
bool fanyi_cfg_save(const fanyi_cfg *c, const char *path, char *err, size_t err_len);

/* 平台配置文件默认绝对路径（调用者 free()） */
char *fanyi_cfg_default_path(void);

/* 用户模型目录（下载的模型放这里，调用者 free()，自动创建） */
char *fanyi_cfg_user_models_dir(void);

/* 探测模型路径：配置值 > 可执行文件旁 models/ > 工作目录 models/ > 用户模型目录；
   找不到返回 NULL */
char *fanyi_cfg_find_model(const fanyi_cfg *c);

/* 可执行文件所在目录（调用者 free()），失败返回 NULL */
char *fanyi_exe_dir(void);

#ifdef __cplusplus
}
#endif
#endif
