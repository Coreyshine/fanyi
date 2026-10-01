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

/* 官方量化的文件名清单（探测顺序：Q8_0 > Q6_K > Q4_K_M） */
extern const char *const fanyi_model_files[];
extern const int fanyi_model_file_count;

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
    char model_file[64];       /* 所选量化文件名（Hy-MT2-1.8B-Q8_0.gguf 等），空 = 自动 */
    bool enabled;              /* 总开关（托盘/设置页可切换） */
    bool video_subtitle;       /* 视频字幕翻译开关（托盘/设置页/扩展弹窗三处同步） */
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

/* 设备物理内存 GB（向上取整，失败返回 0） */
int fanyi_device_ram_gb(void);

/* 列出已安装的量化文件名，返回数量（out 为 out_cap 个 64 字节槽位） */
int fanyi_cfg_installed_models(char out[][64], int out_cap);

#ifdef __cplusplus
}
#endif
#endif
