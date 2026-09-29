/*
 * fanyi — rapidocr.cpp
 * PP-OCRv4 mobile ONNX 推理（onnxruntime CPU）：
 *   det（DBNet 概率图 → 连通域轴对齐框，屏幕文字皆为横向，无需多边形）
 *   rec（CTC 解码 + 字典映射），简化 cls（屏幕文字基本正向）。
 * det 输入形状约束因图而异：三级尺寸重试（直接 → 方形衬底 → 960×960）
 * 确保任意框选比例都能推理。依赖 stb_image / stb_image_resize2。
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
Ort::Env     *g_env     = nullptr;
Ort::Session *g_det     = nullptr;
Ort::Session *g_rec     = nullptr;
std::vector<std::string> g_dict;
std::string   g_models_dir;
std::string   g_debug;
int           g_threads = 2;

/* CHW 归一化：det 用 PaddleOCR mean/std，rec 用 (x/255-0.5)/0.5 */
void chw_norm(const unsigned char *rgb, int w, int h, bool det, std::vector<float> &out) {
    const float mean[3] = {123.675f, 116.28f, 103.53f};
    const float stdv[3] = {58.395f, 57.12f, 57.375f};
    out.resize((size_t)3 * w * h);
    for (int c = 0; c < 3; c++)
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                float v = rgb[((size_t)y * w + x) * 3 + c];
                out[((size_t)c * h + y) * w + x] =
                    det ? (v - mean[c]) / stdv[c] : (v / 255.0f - 0.5f) / 0.5f;
            }
}

bool resize_rgb(const unsigned char *src, int sw, int sh,
                unsigned char *dst, int dw, int dh) {
    return stbir_resize_uint8_linear(src, sw, sh, sw * 3,
                                     dst, dw, dh, dw * 3, STBIR_RGB);
}

struct Box { int x, y, w, h; };

/* det 概率图 → 连通域轴对齐框（BFS + 扩边 + 噪点过滤） */
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
            int pad = 4;
            int bx = std::max(0, minx - pad), by = std::max(0, miny - pad);
            int bw = std::min(w, maxx + pad) - bx, bh = std::min(h, maxy + pad) - by;
            if (bw < 12 || bh < 8 || area < 40) continue;
            boxes.push_back({bx, by, bw, bh});
        }
    }
    std::sort(boxes.begin(), boxes.end(), [](const Box &a, const Box &b) {
        int la = a.y / std::max(1, a.h), lb = b.y / std::max(1, b.h);
        return la != lb ? la < lb : a.x < b.x;
    });
    return boxes;
}

/* CTC 解码（过滤控制字符） */
std::string ctc_decode(const float *logits, int steps, int classes) {
    int last = -1;
    std::string res;
    for (int t = 0; t < steps; t++) {
        int best = 0;
        float bv = -1e30f;
        for (int c = 0; c < classes; c++) {
            float v = logits[(size_t)t * classes + c];
            if (v > bv) { bv = v; best = c; }
        }
        if (best != 0 && best != last) {
            if (best == (int)g_dict.size() + 1) res += ' ';
            else if (best >= 1 && best <= (int)g_dict.size()) {
                std::string ch = g_dict[best - 1];
                bool bad = ch.empty() || (unsigned char)ch[0] < 0x20 || (unsigned char)ch[0] == 0x7f;
                if (!bad) res += ch;
            }
        }
        last = best;
    }
    return res;
}

/* 一级 det：直接按当前尺寸推理；任何异常静默失败（由重试梯级兜底） */
bool run_det(const std::vector<unsigned char> &img, int w, int h,
             std::vector<Box> &boxes) {
    try {
        std::vector<float> input;
        chw_norm(img.data(), w, h, true, input);
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<int64_t> shape = {1, 3, h, w};
        Ort::Value tin = Ort::Value::CreateTensor<float>(
            mem, input.data(), input.size(), shape.data(), shape.size());
        auto in_name = g_det->GetInputNameAllocated(0, Ort::AllocatorWithDefaultOptions());
        auto out_name = g_det->GetOutputNameAllocated(0, Ort::AllocatorWithDefaultOptions());
        const char *in_c = in_name.get();
        const char *out_c = out_name.get();
        auto outs = g_det->Run(Ort::RunOptions{}, &in_c, &tin, 1, &out_c, 1);
        float *prob = outs[0].GetTensorMutableData<float>();
        auto pshape = outs[0].GetTensorTypeAndShapeInfo().GetShape();
        int pw = (int)pshape[3], ph = (int)pshape[2];
        boxes = boxes_from_probmap(prob, pw, ph, 0.3f);
        float pmax = 0;
        size_t pn = 1;
        for (auto d : pshape) pn *= (size_t)d;
        for (size_t i = 0; i < pn; i++) pmax = std::max(pmax, prob[i]);
        g_debug = "img=" + std::to_string(w) + "x" + std::to_string(h) +
                  " probmap=" + std::to_string(pw) + "x" + std::to_string(ph) +
                  " maxprob=" + std::to_string(pmax).substr(0, 5) +
                  " boxes=" + std::to_string(boxes.size());
        return true;
    } catch (...) {
        return false;   /* 形状约束等原因失败 → 上层换尺寸重试 */
    }
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

std::string ocr_last_debug() {
    std::lock_guard<std::mutex> g(g_mtx);
    return g_debug;
}

bool ocr_ensure_loaded(const std::string &dir, std::string *err) {
    std::lock_guard<std::mutex> g(g_mtx);
    if (g_det) return true;

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
        auto to_w = [](const std::string &s) { return std::wstring(s.begin(), s.end()); };
        std::wstring d = to_w(dir) + L"/" + to_w(ocr_files[0]);
        g_det = new Ort::Session(*g_env, d.c_str(), so);
        std::wstring r = to_w(dir) + L"/" + to_w(ocr_files[1]);
        g_rec = new Ort::Session(*g_env, r.c_str(), so);
#else
        g_det = new Ort::Session(*g_env, (dir + "/" + ocr_files[0]).c_str(), so);
        g_rec = new Ort::Session(*g_env, (dir + "/" + ocr_files[1]).c_str(), so);
#endif
        g_dict.clear();
        std::ifstream df(dir + "/" + ocr_files[2]);
        std::string line;
        while (std::getline(df, line)) {
            while (!line.empty() && line.back() == '\r') line.pop_back();
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

bool ocr_image_file(const std::string &png_path, std::string *text, std::string *err) {
    if (!ocr_loaded()) {
        if (err) *err = "OCR 未加载";
        return false;
    }
    int w = 0, h = 0, ch = 0;
    unsigned char *rgb = stbi_load(png_path.c_str(), &w, &h, &ch, 3);
    if (!rgb) {
        if (err) *err = "图片解码失败";
        return false;
    }

    std::string result;
    std::string diag;
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    try {
        /* ---- det 尺寸重试梯级 ----
           变体 1：长边 ≤960、对齐 32、短边 ≥64（最常见，质量最好）
           变体 2：方形白底衬底（侧边 ≥256、对齐 32）
           变体 3：960×960 固定衬底（兜底任意怪比例）      */
        int dw = w, dh = h;
        if (std::max(dw, dh) > 960) {
            double sc = 960.0 / std::max(dw, dh);
            dw = std::max(32, ((int)(dw * sc)) / 32 * 32);
            dh = std::max(32, ((int)(dh * sc)) / 32 * 32);
        }
        dw = std::max(dw, 64);
        dh = std::max(dh, 64);
        std::vector<unsigned char> v1((size_t)dw * dh * 3);
        if (dw != w || dh != h) resize_rgb(rgb, w, h, v1.data(), dw, dh);
        else memcpy(v1.data(), rgb, (size_t)dw * dh * 3);

        std::vector<Box> boxes;
        std::vector<unsigned char> det_img;
        int det_w = 0, det_h = 0;

        std::vector<Box> b1;
        if (run_det(v1, dw, dh, b1) && !b1.empty()) {
            boxes = std::move(b1);
            det_img = std::move(v1); det_w = dw; det_h = dh;
        } else {
            int side = std::max(256, ((std::max(dw, dh) + 31) / 32) * 32);
            std::vector<unsigned char> v2((size_t)side * side * 3, 255);
            for (int y = 0; y < dh && y < side; y++)
                memcpy(v2.data() + (size_t)y * side * 3,
                       v1.data() + (size_t)y * dw * 3, (size_t)std::min(dw, side) * 3);
            std::vector<Box> b2;
            if (run_det(v2, side, side, b2) && !b2.empty()) {
                boxes = std::move(b2);
                det_img = std::move(v2); det_w = side; det_h = side;
            } else {
                std::vector<unsigned char> v3((size_t)960 * 960 * 3, 255);
                double sc = std::min(1.0, 940.0 / std::max(dw, dh));
                int pw2 = std::max(1, (int)(dw * sc)), ph2 = std::max(1, (int)(dh * sc));
                std::vector<unsigned char> tmp((size_t)pw2 * ph2 * 3);
                resize_rgb(v1.data(), dw, dh, tmp.data(), pw2, ph2);
                for (int y = 0; y < ph2 && y < 960; y++)
                    memcpy(v3.data() + (size_t)y * 960 * 3,
                           tmp.data() + (size_t)y * pw2 * 3, (size_t)std::min(pw2, 960) * 3);
                std::vector<Box> b3;
                run_det(v3, 960, 960, b3);
                boxes = std::move(b3);
                det_img = std::move(v3); det_w = 960; det_h = 960;
            }
        }
        diag = "boxes=" + std::to_string(boxes.size());

        /* ---- rec：逐框识别 ---- */
        for (auto &b : boxes) {
            std::vector<unsigned char> crop((size_t)b.w * b.h * 3);
            for (int y = 0; y < b.h; y++) {
                int sy = std::min(b.y + y, det_h - 1);
                int cx = std::min(b.x, det_w - 1);
                int cw = std::min(b.w, det_w - cx);
                memcpy(crop.data() + (size_t)y * b.w * 3,
                       det_img.data() + ((size_t)sy * det_w + cx) * 3, (size_t)cw * 3);
                if (cw < b.w)
                    memset(crop.data() + (size_t)y * b.w * 3 + (size_t)cw * 3, 255,
                           (size_t)(b.w - cw) * 3);
            }

            int rw2 = std::min(640, std::max(16, (int)std::round((double)b.w * 48 / b.h)));
            std::vector<unsigned char> scaled((size_t)rw2 * 48 * 3);
            resize_rgb(crop.data(), b.w, b.h, scaled.data(), rw2, 48);

            std::vector<float> rin;
            chw_norm(scaled.data(), rw2, 48, false, rin);
            std::vector<int64_t> rshape = {1, 3, 48, rw2};
            Ort::Value rin_v = Ort::Value::CreateTensor<float>(
                mem, rin.data(), rin.size(), rshape.data(), rshape.size());

            auto rin_name = g_rec->GetInputNameAllocated(0, Ort::AllocatorWithDefaultOptions());
            auto rout_name = g_rec->GetOutputNameAllocated(0, Ort::AllocatorWithDefaultOptions());
            const char *rin_c = rin_name.get();
            const char *rout_c = rout_name.get();
            auto routs = g_rec->Run(Ort::RunOptions{}, &rin_c, &rin_v, 1, &rout_c, 1);
            float *logits = routs[0].GetTensorMutableData<float>();
            auto rsh = routs[0].GetTensorTypeAndShapeInfo().GetShape();
            std::string line = ctc_decode(logits, (int)rsh[1], (int)rsh[2]);
            if (!line.empty()) {
                if (!result.empty()) result += "\n";
                result += line;
            }
        }
        stbi_image_free(rgb);
        g_debug = diag;
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
