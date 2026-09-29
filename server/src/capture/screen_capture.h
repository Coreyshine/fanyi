/*
 * fanyi — screen_capture.h
 * 交互式屏幕截图（用户框选区域），保存为 PNG。
 */
#ifndef FANYI_SCREEN_CAPTURE_H
#define FANYI_SCREEN_CAPTURE_H

#include <string>

namespace fanyi {

/* 弹出系统截图交互（用户框选区域），保存为 PNG。
   用户取消返回 false；系统工具缺失也返回 false（err 说明）。 */
bool screen_capture_to_file(const std::string &png_path, std::string *err);

} // namespace fanyi
#endif
