/*
 * fanyi — rapidocr.cpp
 * PP-OCRv4 mobile ONNX 推理（onnxruntime CPU）：
 *   det（DBNet 概率图 → 连通域轴对齐框，屏幕文字皆为横向，无需多边形）
 *   rec（CRTC 解码 + 字典映射），简化 cls（屏幕文字基本正向）。
 * 依赖：stb_image 解码、stb_image_resize2 缩放（均已在 third_party/stb）。
 */
#include "ocr.h"

#include <onnxruntime_cxx_api.h>

#include "stb_image.h"
#include "stb_image_resize2.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

namespace fanyi {

const char *const ocr_files[3] = {
    "ch_pp-ocrv4_det_infer.onnx",
    "ch_pp-ocrv4_rec_infer.onnx",
    "ppocr_keys_v1.txt",
};

namespace {

std::mutex g_mtx;
std::string g_debug;
Ort::Env     *g_env     = nullptr;
Ort::Session *g_det     = nullptr;
Ort::Session *g_rec     = nullptr;
std::vector<std::string> g_dict;       /* 字典：第 i 行 → 类别 i+1；类别 0 = CTC blank */
std::string   g_models_dir;
int           g_threads = 2;

/* ---------- 工具 ---------- */

std::string read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<std::string> image_lines_to_rgb(const unsigned char *rgb, int w, int h) {
    (void)rgb; (void)w; (void)h;
    return {};
}

/* CHW float 归一化（PaddleOCR det：mean/std） */
void chw_det(const unsigned char *rgb, int w, int h, std::vector<float> &out) {
    const float mean[3] = {123.675f, 116.28f, 103.53f};
    const float stdv[3] = {58.395f, 57.12f, 57.375f};
    out.resize((size_t)3 * w * h);
    for (int c = 0; c < 3; c++)
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                out[((size_t)c * h + y) * w + x] =
                    ((rgb[((size_t)y * w + x) * 3 + c] - mean[c]) / stdv[c]);
}

/* CHW float 归一化（rec：(x/255-0.5)/0.5） */
void chw_rec(const unsigned char *rgb, int w, int h, std::vector<float> &out) {
    out.resize((size_t)3 * w * h);
    for (int c = 0; c < 3; c++)
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                out[((size_t)c * h + y) * w + x] =
                    (rgb[((size_t)y * w + x) * 3 + c] / 255.0f - 0.5f) / 0.5f;
}

/* 双线性缩放 RGB（用 stb resize2） */
bool resize_rgb(const unsigned char *src, int sw, int sh,
                unsigned char *dst, int dw, int dh) {
    return stbir_resize_uint8_linear(src, sw, sh, sw * 3,
                                     dst, dw, dh, dw * 3, STBIR_RGB);
}

/* ---------- det 后处理：概率图 → 轴对齐文本框（连通域 BFS） ---------- */

struct Box { int x, y, w, h; };

std::vector<Box> boxes_from_probmap(const float *prob, int w, int h, float thresh) {
    std::vector<Box> boxes;
    std::vector<unsigned char> bin((size_t)w * h, 0);
    for (size_t i = 0; i < bin.size(); i++) bin[i] = prob[i] > thresh;
    std::vector<int> visited((size_t)w * h, 0);
    std::vector<int> stack;
    for (int y0 = 0; y0 < h; y0++) {
        for (int x0 = 0; x0 < w; x0++) {
            size_t idx = (size_t)y0 * w + x0;
            if (!bin[idx] || visited[idx]) continue;
            /* BFS 收集连通域包围盒 */
            int minx = x0, maxx = x0, miny = y0, maxy = y0, area = 0;
            stack.clear();
            stack.push_back((int)idx);
            visited[idx] = 1;
            while (!stack.empty()) {
                int cur = stack.back(); stack.pop_back();
                int cy = cur / w, cx = cur % w;
                area++;
                if (cx < minx) minx = cx;
                if (cx > maxx) maxx = cx;
                if (cy < miny) miny = cy;
                if (cy > maxy) maxy = cy;
                const int nx[4] = {cx - 1, cx + 1, cx, cx};
                const int ny[4] = {cy, cy, cy - 1, cy + 1};
                for (int k = 0; k < 4; k++) {
                    int px = nx[k], py = ny[k];
                    if (px < 0 || px >= w || py < 0 || py >= h) continue;
                    size_t pi = (size_t)py * w + px;
                    if (bin[pi] && !visited[pi]) { visited[pi] = 1; stack.push_back((int)pi); }
                }
            }
            /* 扩边 + 过滤噪点 */
            int pad = 4;
            int bx = std::max(0, minx - pad), by = std::max(0, miny - pad);
            int bw = std::min(w, maxx + pad) - bx, bh = std::min(h, maxy + pad) - by;
            if (bw < 12 || bh < 8 || area < 40) continue;
            boxes.push_back({bx, by, bw, bh});
        }
    }
    /* 按从上到下、从左到右排序（阅读顺序） */
    std::sort(boxes.begin(), boxes.end(), [](const Box &a, const Box &b) {
        int la = a.y / std::max(1, a.h), lb = b.y / std::max(1, b.h);
        return la != lb ? la < lb : a.x < b.x;
    });
    return boxes;
}

/* ---------- rec 后处理：CTC 解码 ---------- */

std::string ctc_decode(const float *logits, int steps, int classes) {
    std::string out;         /* UTF-8 由字典串拼接 */
    int last = -1;
    std::vector<int> ids;
    for (int t = 0; t < steps; t++) {
        int best = 0;
        float bv = -1e30f;
        for (int c = 0; c < classes; c++) {
            float v = logits[(size_t)t * classes + c];
            if (v > bv) { bv = v; best = c; }
        }
        if (best != 0 && best != last) ids.push_back(best);   /* 0 = blank */
        last = best;
    }
    std::string res;
    for (int id : ids) {
        if (id == (int)g_dict.size() + 1) { res += ' '; continue; }   /* 空格类 */
        if (id >= 1 && id <= (int)g_dict.size()) res += g_dict[id - 1];
    }
    (void)out;
    return res;
}

} // namespace

bool ocr_models_installed(const std::string &dir) {
    for (int i = 0; i < 3; i++) {
        std::ifstream f(dir + "/" + ocr_files[i]);
        if (!f.good()) return false;
    }
    return true;
}

bool ocr_loaded() {
    std::lock_guard<std::mutex> g(g_mtx);
    return g_det != nullptr;
}

bool ocr_ensure_loaded(const std::string &dir, std::string *err) {
    std::lock_guard<std::mutex> g(g_mtx);
    if (g_det) { if (g_models_dir != dir) g_models_dir = dir; return true; }

    for (int i = 0; i < 3; i++) {
        std::ifstream f(dir + "/" + ocr_files[i]);
        if (!f.good()) {
            if (err) *err = std::string("OCR 模型未下载：") + ocr_files[i] +
                            "（设置页可下载，约 15MB）";
            return false;
        }
    }
    g_models_dir = dir;

    try {
        if (!g_env) g_env = new Ort::Env(ORT_LOGGING_LEVEL_ERROR, "fanyi-ocr");
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(g_threads);
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

#ifdef _WIN32
        std::wstring wdir(dir.begin(), dir.end());
        std::wstring d = wdir + L"/" + ocr_files[0];
        g_det = new Ort::Session(*g_env, d.c_str(), so);
        std::wstring r = wdir + L"/" + ocr_files[1];
        g_rec = new Ort::Session(*g_env, r.c_str(), so);
#else
        g_det = new Ort::Session(*g_env, (dir + "/" + ocr_files[0]).c_str(), so);
        g_rec = new Ort::Session(*g_env, (dir + "/" + ocr_files[1]).c_str(), so);
#endif
        /* 字典：每行一个字符（UTF-8） */
        g_dict.clear();
        std::ifstream df(dir + "/" + ocr_files[2]);
        std::string line;
        while (std::getline(df, line)) {
            while (!line.empty() && (line.back() == '\r')) line.pop_back();
            if (!line.empty()) g_dict.push_back(line);
        }
        if (g_dict.empty()) {
            delete g_det; delete g_rec; g_det = g_rec = nullptr;
            if (err) *err = "OCR 字典为空";
            return false;
        }
        if (err) err->clear();
        return true;
    } catch (const Ort::Exception &e) {
        delete g_det; delete g_rec; g_det = g_rec = nullptr;
        if (err) *err = std::string("OCR 初始化失败：") + e.what();
        return false;
    }
}

void ocr_unload() {
    std::lock_guard<std::mutex> g(g_mtx);
    delete g_det; delete g_rec;
    g_det = g_rec = nullptr;
}

std::string ocr_last_debug() { std::lock_guard<std::mutex> g(g_mtx); return g_debug; }

bool ocr_image_file(const std::string &png_path, std::string *text, std::string *err) {
    if (!ocr_loaded()) {
        /* 调用方需先 ocr_ensure_loaded */
        if (err) *err = "OCR 未加载";
        return false;
    }
    int w = 0, h = 0, ch = 0;
    unsigned char *rgb = stbi_load(png_path.c_str(), &w, &h, &ch, 3);
    if (!rgb) {
        if (err) *err = "图片解码失败";
        return false;
    }
    try {
        /* ---- det：长边 960（对齐 32）---- */
        int dw = w, dh = h;
        if (std::max(dw, dh) > 960) {
            double sc = 960.0 / std::max(dw, dh);
            dw = std::max(32, ((int)(dw * sc)) / 32 * 32);
            dh = std::max(32, ((int)(dh * sc)) / 32 * 32);
        }
        std::vector<unsigned char> small;
        if (dw != w || dh != h) {
            small.resize((size_t)dw * dh * 3);
            resize_rgb(rgb, w, h, small.data(), dw, dh);
        }
        const unsigned char *det_img = (dw != w || dh != h) ? small.data() : rgb;

        std::vector<float> input;
        chw_det(det_img, dw, dh, input);

        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<int64_t> shape = {1, 3, dh, dw};
        Ort::Value tin = Ort::Value::CreateTensor<float>(
            mem, input.data(), input.size(), shape.data(), shape.size());

        auto din_owner = g_det->GetInputNameAllocated(0, Ort::AllocatorWithDefaultOptions());
        auto dout_owner = g_det->GetOutputNameAllocated(0, Ort::AllocatorWithDefaultOptions());
        const char *din = din_owner.get();
        const char *dout = dout_owner.get();
        auto outs = g_det->Run(Ort::RunOptions{}, &din, &tin, 1, &dout, 1);

        float *prob = outs[0].GetTensorMutableData<float>();
        auto dshape = outs[0].GetTensorTypeAndShapeInfo().GetShape();
        int ph = (int)dshape[2], pw = (int)dshape[3];
        /* 概率图与 det 输入同尺寸（DBNet 输出 1/1 尺度） */
        auto boxes = boxes_from_probmap(prob, pw, ph, 0.3f);
        /* 概率图与 det 输入同尺寸（DBNet 输出 1/1 尺度） */
        float pmax = 0; for (size_t i = 0; i < (size_t)pw * ph; i++) pmax = std::max(pmax, prob[i]);
        g_debug = "img=" + std::to_string(w) + "x" + std::to_string(h) +
                  " det=" + std::to_string(dw) + "x" + std::to_string(dh) +
                  " probmap=" + std::to_string(pw) + "x" + std::to_string(ph) +
                  " maxprob=" + std::to_string(pmax).substr(0, 5) +
                  " boxes=" + std::to_string(boxes.size());

        /* ---- rec：逐框识别 ---- */
        std::string result;
        for (auto &b : boxes) {
            int rw = b.w, rh = b.h;
            std::vector<unsigned char> crop((size_t)rw * rh * 3);
            for (int y = 0; y < rh; y++)
                memcpy(crop.data() + (size_t)y * rw * 3,
                       det_img + ((size_t)(b.y + y) * dw + b.x) * 3, (size_t)rw * 3);

            int rh48 = 48;
            int rw2 = std::min(1280, std::max(16, (int)std::round((double)rw * 48 / rh)));
            std::vector<unsigned char> scaled((size_t)rw2 * rh48 * 3);
            resize_rgb(crop.data(), rw, rh, scaled.data(), rw2, rh48);

            std::vector<float> rin;
            chw_rec(scaled.data(), rw2, rh48, rin);
            std::vector<int64_t> rshape = {1, 3, rh48, rw2};
            Ort::Value rin_v = Ort::Value::CreateTensor<float>(
                mem, rin.data(), rin.size(), rshape.data(), rshape.size());

            auto rin_name = g_rec->GetInputNameAllocated(0, Ort::AllocatorWithDefaultOptions());
            auto rout_name = g_rec->GetOutputNameAllocated(0, Ort::AllocatorWithDefaultOptions());
            const char *rin_c = rin_name.get();
            const char *rout_c = rout_name.get();
            auto routs = g_rec->Run(Ort::RunOptions{}, &rin_c, &rin_v, 1, &rout_c, 1);
            float *logits = routs[0].GetTensorMutableData<float>();
            auto rout_shape = routs[0].GetTensorTypeAndShapeInfo().GetShape();
            int steps = (int)rout_shape[1], classes = (int)rout_shape[2];
            if (!result.empty()) result += "\n";
            result += ctc_decode(logits, steps, classes);
        }
        stbi_image_free(rgb);
        if (text) *text = result;
        return true;
    } catch (const Ort::Exception &e) {
        stbi_image_free(rgb);
        if (err) *err = std::string("OCR 推理失败：") + e.what();
        return false;
    } catch (const std::exception &e) {
        stbi_image_free(rgb);
        if (err) *err = std::string("OCR 失败：") + e.what();
        return false;
    }
}

} // namespace fanyi
