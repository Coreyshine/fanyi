/*
 * fanyi — translator.h
 * 纯 C 接口：封装 llama.cpp，加载本地 Hy-MT2 翻译模型并批量翻译文本段。
 *
 * 用法：
 *   fanyi_params p; fanyi_params_default(&p); p.model_path = "...";
 *   fanyi_translator *t = fanyi_create(&p, err, sizeof err);
 *   char *out[2];
 *   const char *segs[2] = {"Hello", "World"};
 *   fanyi_translate(t, segs, 2, "中文", out, NULL, NULL, err, sizeof err);
 *   ...
 *   fanyi_strfree(out[i]);
 *   fanyi_destroy(t);
 */
#ifndef FANYI_TRANSLATOR_H
#define FANYI_TRANSLATOR_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fanyi_translator fanyi_translator;

typedef struct fanyi_params {
    const char *model_path;      /* GGUF 文件路径（必填） */
    int         ctx_tokens;      /* KV 总上下文，0 => 16384 */
    int         max_parallel;    /* 单波并行段数，0 => 8 */
    int         threads;         /* 0 => 自动（物理核数的一半，至少 2） */
    int         gpu_layers;      /* -1 => 全部层（有 GPU 后端时生效） */
    bool        greedy;          /* 精准模式：贪心解码，输出确定 */
    float       temperature;     /* <=0 => 0.7（官方推荐） */
    float       top_p;           /* <=0 => 0.6 */
    int         top_k;           /* <=0 => 20 */
    float       repeat_penalty;  /* <=0 => 1.05 */
} fanyi_params;

/* 每完成一段回调一次（在 fanyi_translate 调用线程上） */
typedef void (*fanyi_seg_done_cb)(void *userdata, size_t index, const char *text);

void fanyi_params_default(fanyi_params *p);

/* 失败返回 NULL 并填 err。线程安全：内部有互斥锁，翻译请求串行执行。 */
fanyi_translator *fanyi_create(const fanyi_params *p, char *err, size_t err_len);
void             fanyi_destroy(fanyi_translator *t);
bool             fanyi_loaded(const fanyi_translator *t);

/*
 * 批量翻译。target_lang_zh 用中文语言名（模型提示词要求），如 "中文"、"英语"。
 * out 必须、且只需要提供 n 个指针槽位；成功后 out[i] 为 malloc 的 UTF-8 文本，
 * 用 fanyi_strfree 释放。cb 可为 NULL。
 * 返回 0 成功；-1 参数错；-2 模型未加载；-3 推理失败（err 有说明）。
 */
int  fanyi_translate(fanyi_translator *t,
                     const char *const *segs, size_t n,
                     const char *target_lang_zh,
                     char **out,
                     fanyi_seg_done_cb cb, void *userdata,
                     char *err, size_t err_len);

void        fanyi_strfree(char *s);
const char *fanyi_build_info(void);

#ifdef __cplusplus
}
#endif
#endif /* FANYI_TRANSLATOR_H */
