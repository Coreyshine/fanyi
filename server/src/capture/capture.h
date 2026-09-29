/*
 * fanyi — capture.h
 * 划词取词与结果浮窗的跨平台接口（各平台分别实现）。
 */
#ifndef FANYI_CAPTURE_H
#define FANYI_CAPTURE_H

#include <string>

namespace fanyi {

/* 取当前选中的文本（模拟复制 + 读剪贴板 + 还原剪贴板）。
   失败返回空串，err 说明原因（含权限引导）。 */
std::string capture_selected_text(std::string *err);

/* 在光标附近显示结果浮窗（原文 + 译文 + 复制按钮），非阻塞。
   窗口自行管理生命周期（失焦/Esc/超时关闭）。 */
void result_window_show(const std::string &original,
                        const std::string &translated,
                        const std::string &target_name);

} // namespace fanyi
#endif
