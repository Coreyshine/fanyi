/*
 * fanyi — cli.c
 * 翻译测试工具：
 *   fanyi-cli [-m model.gguf] [-t 中文] [-g] "Hello world" "Another segment"
 *   echo "Hello" | fanyi-cli -m model.gguf     （每行一段）
 *   -g  精准模式（贪心解码）
 */
#include "fanyi/translator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_MODEL "models/Hy-MT2-1.8B-Q8_0.gguf"
#define MAX_SEGS 4096

static char *lines[MAX_SEGS];
static const char *argv_segs[MAX_SEGS];

static int run(const char *model, const char *target, bool greedy,
               const char **segs, size_t n) {
    char err[512] = {0};
    fanyi_params p;
    fanyi_params_default(&p);
    p.model_path = model;
    p.greedy = greedy;
    p.max_parallel = 8;

    fprintf(stderr, "[fanyi-cli] 模型: %s  目标语言: %s%s\n", model, target, greedy ? "  [精准模式]" : "");
    fanyi_translator *t = fanyi_create(&p, err, sizeof err);
    if (!t) { fprintf(stderr, "[fanyi-cli] 初始化失败: %s\n", err); return 1; }

    char **out = calloc(n ? n : 1, sizeof(char *));
    int rc = fanyi_translate(t, segs, n, target, out, NULL, NULL, err, sizeof err);
    if (rc != 0) { fprintf(stderr, "[fanyi-cli] 翻译失败: %s\n", err); free(out); fanyi_destroy(t); return 1; }
    for (size_t i = 0; i < n; i++) {
        printf("%s\n", out[i] ? out[i] : "");
        fanyi_strfree(out[i]);
    }
    free(out);
    fanyi_destroy(t);
    return 0;
}

int main(int argc, char **argv) {
    const char *model = DEFAULT_MODEL;
    const char *target = "中文";
    bool greedy = false;
    size_t n = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc)      model = argv[++i];
        else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) target = argv[++i];
        else if (strcmp(argv[i], "-g") == 0)                 greedy = true;
        else if (n < MAX_SEGS)                               argv_segs[n++] = argv[i];
    }

    if (n > 0) return run(model, target, greedy, argv_segs, n);

    /* stdin 模式：每行一段 */
    char buf[16384];
    while (fgets(buf, sizeof buf, stdin) && n < MAX_SEGS) {
        buf[strcspn(buf, "\r\n")] = 0;
        if (buf[0]) lines[n++] = strdup(buf);
    }
    if (n == 0) {
        fprintf(stderr, "用法: fanyi-cli [-m model.gguf] [-t 中文] [-g] \"文本\" ...\n");
        return 2;
    }
    for (size_t i = 0; i < n; i++) argv_segs[i] = lines[i];
    int rc = run(model, target, greedy, argv_segs, n);
    for (size_t i = 0; i < n; i++) free(lines[i]);
    return rc;
}
