/*
 * fanyi — screen_capture.h
 * 屏幕截图：交互式全屏框选 / 指定区域截取 / 鼠标位置。
 */
#ifndef FANYI_SCREEN_CAPTURE_H
#define FANYI_SCREEN_CAPTURE_H

#include <string>

namespace fanyi {

/* 弹出系统截图交互（用户框选区域），保存为 PNG。
   用户取消返回 false；系统工具缺失也返回 false（err 说明）。 */
bool screen_capture_to_file(const std::string &png_path, std::string *err);

/* 截取屏幕指定区域（屏幕像素坐标），保存为 PNG。 */
bool capture_region_to_file(int x, int y, int w, int h,
                            const std::string &png_path, std::string *err);

/* 鼠标当前屏幕坐标（失败返回 false） */
bool mouse_position(int *x, int *y);

} // namespace fanyi
#endif
