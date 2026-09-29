/*
 * fanyi — ocr.h
 * 截图 OCR（RapidOCR / PP-OCRv4 mobile ONNX，onnxruntime CPU 推理）。
 * 模型按需懒加载、闲置卸载，与翻译模型互不影响。
 */
#ifndef FANYI_OCR_H
#define FANYI_OCR_H

#include <string>

namespace fanyi {

/* 三件套（det/rec/dict）是否都已存在于用户模型目录 */
bool ocr_models_installed(const std::string &user_models_dir);

/* 懒加载（文件齐才会成功）；已加载直接返回 true */
bool ocr_ensure_loaded(const std::string &user_models_dir, std::string *err);
void ocr_unload();               /* 释放会话内存 */
bool ocr_loaded();

/* 识别 PNG 图片中的文字，按行拼接（UTF-8） */
bool ocr_image_file(const std::string &png_path, std::string *text, std::string *err);

/* 最近一次识别的诊断信息（尺寸/框数/最大概率） */
std::string ocr_last_debug();

/* OCR 三件套的文件名 */
extern const char *const ocr_files[3];   /* det onnx / rec onnx / dict txt */

} // namespace fanyi
#endif
