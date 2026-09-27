/*
 * fanyi — translator.c
 * llama.cpp 封装：加载 Hy-MT2(GGUF)，多段并行批量解码，段级流式回调。
 *
 * 提示词采用腾讯混元 MT2 官方格式（无 system prompt）：
 *   user: 将以下文本翻译为 {目标语言}，注意只需要输出翻译后的结果，不要额外解释：\n\n{原文}
 * 聊天模板特殊 token 直接从词表精确匹配定位，避免依赖 jinja 模板解析。
 */
#include "fanyi/translator.h"

#include "llama.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FANYI_VERSION
#define FANYI_VERSION "1.0.0"
#endif

/* ---------- portable mutex ---------- */
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
typedef CRITICAL_SECTION fanyi_mutex_t;
static void mx_init(fanyi_mutex_t *m)   { InitializeCriticalSection(m); }
static void mx_free(fanyi_mutex_t *m)   { DeleteCriticalSection(m); }
static void mx_lock(fanyi_mutex_t *m)   { EnterCriticalSection(m); }
static void mx_unlock(fanyi_mutex_t *m) { LeaveCriticalSection(m); }
#else
#  include <pthread.h>
#  include <unistd.h>
typedef pthread_mutex_t fanyi_mutex_t;
static void mx_init(fanyi_mutex_t *m)   { pthread_mutex_init(m, NULL); }
static void mx_free(fanyi_mutex_t *m)   { pthread_mutex_destroy(m); }
static void mx_lock(fanyi_mutex_t *m)   { pthread_mutex_lock(m); }
static void mx_unlock(fanyi_mutex_t *m) { pthread_mutex_unlock(m); }
#endif

/* ---------- growable string buffer ---------- */
typedef struct { char *data; size_t len, cap; } sbuf;

static void sbuf_grow(sbuf *b, size_t need) {
    if (b->len + need + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + need + 1) cap *= 2;
    char *d = realloc(b->data, cap);
    if (!d) { perror("fanyi: realloc"); abort(); }
    b->data = d;
    b->cap = cap;
}
static void sbuf_append(sbuf *b, const char *s, size_t n) {
    sbuf_grow(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}
static void sbuf_free(sbuf *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

/* 尾部不完整 UTF-8 序列的字节数（挂起，等下一个 piece 拼齐再输出） */
static size_t utf8_pending_tail(const char *s, size_t n) {
    size_t i = n;
    while (i > 0 && n - i < 3 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;
    if (i == 0) return 0;
    unsigned char b = (unsigned char)s[i - 1];
    int need = (b & 0x80) == 0    ? 1
             : (b & 0xE0) == 0xC0 ? 2
             : (b & 0xF0) == 0xE0 ? 3
             : (b & 0xF8) == 0xF0 ? 4 : 1;
    size_t have = n - (i - 1);
    return have < need ? have : 0;
}

/* ---------- hunyuan 聊天模板特殊 token（字节与 llama.cpp llama-chat.cpp 一致） ---------- */
#define HY_USER   "<｜hy_User｜>"
#define HY_ASSIST "<｜hy_Assistant｜>"
#define HY_END    "<｜hy_place▁holder▁no▁2｜>"

struct fanyi_translator {
    fanyi_params             p;
    struct llama_model      *model;
    const struct llama_vocab *vocab;
    struct llama_context    *ctx;
    llama_token        tok_user, tok_assist, tok_end;
    int32_t            n_vocab;
    fanyi_mutex_t      mtx;
};

void fanyi_params_default(fanyi_params *p) {
    memset(p, 0, sizeof *p);
    p->ctx_tokens     = 16384;
    p->max_parallel   = 8;
    p->gpu_layers     = -1;
    p->greedy         = false;
    p->temperature    = 0.7f;
    p->top_p          = 0.6f;
    p->top_k          = 20;
    p->repeat_penalty = 1.05f;
}

const char *fanyi_build_info(void) { return "fanyi " FANYI_VERSION; }

static int auto_threads(void) {
#ifdef _WIN32
    SYSTEM_INFO si; GetSystemInfo(&si);
    long n = (long)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (n < 2) n = 2;
    long half = n / 2;
    return (int)(half > 2 ? half : 2);
}

/* 从词表精确匹配特殊 token（special=true 让控制 token 渲染为文本）；找不到返回 -1 */
static llama_token find_token(const struct llama_vocab *vocab, int32_t n_vocab, const char *s) {
    char buf[160];
    size_t slen = strlen(s);
    for (int32_t i = 0; i < n_vocab; i++) {
        int32_t n = llama_token_to_piece(vocab, i, buf, (int32_t)sizeof buf - 1, 0, true);
        if (n == (int32_t)slen && n > 0) {
            buf[n] = 0;
            if (strcmp(buf, s) == 0) return i;
        }
    }
    return -1;
}

fanyi_translator *fanyi_create(const fanyi_params *p, char *err, size_t err_len) {
    if (!p || !p->model_path) {
        if (err) snprintf(err, err_len, "缺少模型路径");
        return NULL;
    }
    static bool backend_ready = false;
    if (!backend_ready) { llama_backend_init(); backend_ready = true; }

    fanyi_translator *t = calloc(1, sizeof *t);
    if (!t) { if (err) snprintf(err, err_len, "内存不足"); return NULL; }
    t->p = *p;
    mx_init(&t->mtx);

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = p->gpu_layers;
    t->model = llama_model_load_from_file(p->model_path, mp);
    if (!t->model) {
        snprintf(err, err_len, "模型加载失败: %s", p->model_path);
        goto fail;
    }
    t->vocab   = llama_model_get_vocab(t->model);
    t->n_vocab = llama_vocab_n_tokens(t->vocab);


    t->tok_user   = find_token(t->vocab, t->n_vocab, HY_USER);
    t->tok_assist = find_token(t->vocab, t->n_vocab, HY_ASSIST);
    t->tok_end    = find_token(t->vocab, t->n_vocab, HY_END);
    if (t->tok_user < 0 || t->tok_assist < 0) {
        snprintf(err, err_len, "词表中未找到混元模板 token，这可能不是 Hy-MT 系列模型");
        goto fail;
    }

    {
        int threads = p->threads > 0 ? p->threads : auto_threads();
        for (int kv_q = 1; kv_q >= 0 && !t->ctx; kv_q--) { /* 先 q8_0 KV（省内存），失败退 F16 */
            struct llama_context_params cp = llama_context_default_params();
            cp.n_ctx           = p->ctx_tokens > 0 ? (uint32_t)p->ctx_tokens : 16384;
            cp.n_seq_max       = p->max_parallel > 0 ? (uint32_t)p->max_parallel : 8;
            cp.n_batch         = 4096;
            cp.n_ubatch        = 1024;
            cp.n_threads       = threads;
            cp.n_threads_batch = threads;
            cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_AUTO;
            cp.kv_unified      = true;
            if (kv_q) { cp.type_k = GGML_TYPE_Q8_0; cp.type_v = GGML_TYPE_Q8_0; }
            t->ctx = llama_init_from_model(t->model, cp);
        }
    }
    if (!t->ctx) {
        snprintf(err, err_len, "上下文创建失败（ctx/并行数过大？）");
        goto fail;
    }
    if (err) err[0] = 0;
    return t;
fail:
    if (t->model) llama_model_free(t->model);
    mx_free(&t->mtx);
    free(t);
    return NULL;
}

void fanyi_destroy(fanyi_translator *t) {
    if (!t) return;
    mx_lock(&t->mtx);
    if (t->ctx) llama_free(t->ctx);
    if (t->model) llama_model_free(t->model);
    mx_unlock(&t->mtx);
    mx_free(&t->mtx);
    free(t);
}

bool fanyi_loaded(const fanyi_translator *t) { return t && t->ctx; }

void fanyi_strfree(char *s) { free(s); }

/* 拼完整提示词并 tokenize：[user_tag] 指令+原文 [assist_tag]，返回 token 数 */
static int32_t build_prompt(const fanyi_translator *t, const char *target, const char *seg,
                            llama_token *out, int32_t cap, char *err, size_t err_len) {
    sbuf b = {0};
    char head[192];
    int hn = snprintf(head, sizeof head,
                      "将以下文本翻译为 %s，注意只需要输出翻译后的结果，不要额外解释：\n\n",
                      target);
    if (hn < 0 || (size_t)hn >= sizeof head) { snprintf(err, err_len, "语言名过长"); return -1; }
    sbuf_append(&b, head, (size_t)hn);
    sbuf_append(&b, seg, strlen(seg));

    out[0] = t->tok_user;
    int32_t n = llama_tokenize(t->vocab, b.data, (int32_t)b.len,
                               out + 1, cap - 2, /*add_special*/ false, /*parse_special*/ false);
    sbuf_free(&b);
    if (n < 0) { snprintf(err, err_len, "分词溢出"); return -1; }
    out[1 + n] = t->tok_assist;
    return n + 2;
}

static struct llama_sampler *make_sampler(const fanyi_translator *t, bool greedy) {
    struct llama_sampler *s = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (greedy) {
        llama_sampler_chain_add(s, llama_sampler_init_greedy());
    } else {
        llama_sampler_chain_add(s, llama_sampler_init_penalties(
            t->n_vocab, 64,
            t->p.repeat_penalty > 0 ? t->p.repeat_penalty : 1.05f, 0.0f, 0.0f));
        llama_sampler_chain_add(s, llama_sampler_init_top_k(t->p.top_k > 0 ? t->p.top_k : 20));
        llama_sampler_chain_add(s, llama_sampler_init_top_p(t->p.top_p > 0 ? t->p.top_p : 0.6f, 1));
        llama_sampler_chain_add(s, llama_sampler_init_temp(t->p.temperature > 0 ? t->p.temperature : 0.7f));
        llama_sampler_chain_add(s, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
    }
    return s;
}

/* 单个解码槽位 */
typedef struct {
    llama_token   *toks;      /* 本段完整提示 token（含 user/assist 标记） */
    int32_t        plen;
    int32_t        max_gen;
    int32_t        gen_count;
    size_t         gi;        /* 全局输出下标 */
    sbuf              out;
    struct llama_sampler *sm;
    int64_t        batch_idx; /* 该段最近一次带 logits 的 token 在 batch 中的下标 */
    bool           done;
    bool           empty;     /* 空段/分词失败：直接空结果 */
} slot_t;

int fanyi_translate(fanyi_translator *t, const char *const *segs, size_t n,
                    const char *target_lang_zh, char **out,
                    fanyi_seg_done_cb cb, void *userdata,
                    char *err, size_t err_len) {
    if (!t || !segs || !out || !target_lang_zh) { if (err) snprintf(err, err_len, "参数错误"); return -1; }
    if (!t->ctx) { if (err) snprintf(err, err_len, "模型未加载"); return -2; }
    if (n == 0) { if (err) err[0] = 0; return 0; }
    for (size_t i = 0; i < n; i++) out[i] = NULL;
    if (err) err[0] = 0;

    mx_lock(&t->mtx);
    const uint32_t n_ctx     = llama_n_ctx(t->ctx);
    const uint32_t n_seq_max = llama_n_seq_max(t->ctx);
    int rc = 0;

    for (size_t wave0 = 0; wave0 < n && rc == 0; ) {
        size_t wave = n - wave0 < n_seq_max ? n - wave0 : n_seq_max;
        slot_t *slots = calloc(wave, sizeof(slot_t));
        llama_token *scratch = malloc(8192 * sizeof(llama_token));
        if (!slots || !scratch) { snprintf(err, err_len, "内存不足"); rc = -3; free(slots); free(scratch); break; }

        /* tokenize 并按 KV 预算决定本波段数 */
        size_t used = 0, take = 0;
        for (; take < wave; take++) {
            slots[take].gi = wave0 + take;
            int32_t plen = build_prompt(t, target_lang_zh, segs[wave0 + take],
                                        scratch, 8192, err, err_len);
            if (plen <= 2) {                       /* 空段或分词失败 */
                slots[take].done = true;
                slots[take].empty = true;
                out[slots[take].gi] = strdup("");
                if (cb) cb(userdata, slots[take].gi, out[slots[take].gi]);
                continue;
            }
            int32_t max_gen = (plen - 2) * 4 + 24;
            if (max_gen < 32)   max_gen = 32;
            if (max_gen > 768)  max_gen = 768;
            if (take > 0 && used + (uint32_t)(plen + max_gen) > n_ctx) break;
            llama_token *buf = malloc((size_t)plen * sizeof(llama_token));
            if (!buf) { snprintf(err, err_len, "内存不足"); rc = -3; break; }
            build_prompt(t, target_lang_zh, segs[wave0 + take], buf, plen, err, err_len);
            slots[take].toks   = buf;
            slots[take].plen   = plen;
            slots[take].max_gen = max_gen;
            used += (uint32_t)(plen + max_gen);
        }
        free(scratch);
        if (rc != 0 && take == 0) { free(slots); break; }
        wave = take > 0 ? take : 1;

        /* 清 KV，整波一次 prefill（每段只在最后一个 token 上取 logits） */
        if (rc == 0) {
            llama_memory_clear(llama_get_memory(t->ctx), false);
            size_t total = 0;
            for (size_t s = 0; s < wave; s++) total += (size_t)slots[s].plen;
            llama_batch batch = llama_batch_init((int32_t)(total > 0 ? total : 1), 0, 1);
            size_t bi = 0;
            for (size_t s = 0; s < wave; s++) {
                if (slots[s].done) continue;
                for (int32_t j = 0; j < slots[s].plen; j++) {
                    batch.token[bi]    = slots[s].toks[j];
                    batch.pos[bi]      = j;
                    batch.n_seq_id[bi] = 1;
                    batch.seq_id[bi][0] = (int32_t)s;
                    batch.logits[bi]   = (j == slots[s].plen - 1);
                    if (batch.logits[bi]) slots[s].batch_idx = (int64_t)bi;
                    bi++;
                }
            }
            batch.n_tokens = (int32_t)bi;
            if (batch.n_tokens > 0 && llama_decode(t->ctx, batch) != 0) {
                snprintf(err, err_len, "prefill 失败（上下文溢出？）");
                rc = -3;
            }
            llama_batch_free(batch);
        }

        /* 逐 token 并行解码 */
        if (rc == 0) {
            llama_batch batch = llama_batch_init((int32_t)wave, 0, 1);
            size_t active = 0;
            for (size_t s = 0; s < wave; s++) {
                if (slots[s].done) continue;
                slots[s].sm = make_sampler(t, t->p.greedy);
                active++;
            }
            while (active > 0 && rc == 0) {
                size_t bi = 0;
                for (size_t s = 0; s < wave; s++) {
                    if (slots[s].done) continue;
                    llama_token tok = llama_sampler_sample(slots[s].sm, t->ctx, (int32_t)slots[s].batch_idx);
                    llama_sampler_accept(slots[s].sm, tok);
                    slots[s].gen_count++;
                    if (tok == llama_vocab_eos(t->vocab) || tok == t->tok_end ||
                        slots[s].gen_count >= slots[s].max_gen) {
                        sbuf_append(&slots[s].out, "", 0);  /* 补 NUL（data[len]=0） */
                        out[slots[s].gi] = slots[s].out.data ? slots[s].out.data : strdup("");
                        slots[s].out.data = NULL;           /* 所有权移交 out */
                        slots[s].done = true;
                        active--;
                        if (cb) cb(userdata, slots[s].gi, out[slots[s].gi]);
                        continue;
                    }
                    char piece[64];
                    int32_t pn = llama_token_to_piece(t->vocab, tok, piece, (int32_t)sizeof piece, 0, false);
                    if (pn > 0) {
                        size_t tail = utf8_pending_tail(piece, (size_t)pn);
                        sbuf_append(&slots[s].out, piece, (size_t)pn - tail);
                    }
                    batch.token[bi]    = tok;
                    batch.pos[bi]      = slots[s].plen + slots[s].gen_count - 1;
                    batch.n_seq_id[bi] = 1;
                    batch.seq_id[bi][0] = (int32_t)s;
                    batch.logits[bi]   = true;
                    slots[s].batch_idx = (int64_t)bi;
                    bi++;
                }
                if (bi == 0) break;
                batch.n_tokens = (int32_t)bi;
                int32_t ret = llama_decode(t->ctx, batch);
                if (ret != 0) { snprintf(err, err_len, "解码失败 ret=%d", ret); rc = -3; }
            }
            llama_batch_free(batch);
        }

        for (size_t s = 0; s < wave; s++) {
            if (slots[s].sm) llama_sampler_free(slots[s].sm);
            free(slots[s].toks);
            sbuf_free(&slots[s].out);   /* 未移交的缓冲在此释放；已移交的 data 为 NULL */
        }
        free(slots);
        wave0 += wave;
    }

    mx_unlock(&t->mtx);
    return rc;
}
