/*
 * fanyi — text_selection_mac.mm
 * macOS 划词：优先辅助功能 API 直读选区；否则模拟 Cmd+C 读剪贴板（并还原原剪贴板）。
 * 需要用户在「系统设置 → 隐私与安全性 → 辅助功能」中授权（首次自动弹引导）。
 */
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

#include <chrono>
#include <thread>

std::string fanyi_capture_selected_text_impl(std::string *err);

namespace fanyi {

std::string capture_selected_text(std::string *err) {
    if (!AXIsProcessTrustedWithOptions(
            (CFDictionaryRef)@{(id)kAXTrustedCheckOptionPrompt: @YES})) {
        if (err) *err = "需要在「系统设置 → 隐私与安全性 → 辅助功能」中勾选 fanyi，然后重试";
        return "";
    }

    // 1) 直接读焦点控件选区（对多数原生 App 有效，且不碰剪贴板）
    @try {
        AXUIElementRef sys = AXUIElementCreateSystemWide();
        AXUIElementRef app = nullptr, el = nullptr;
        CFTypeRef sel = nullptr;
        if (AXUIElementCopyAttributeValue(sys, kAXFocusedApplicationAttribute,
                                          (CFTypeRef *)&app) == kAXErrorSuccess && app) {
            if (AXUIElementCopyAttributeValue(app, kAXFocusedUIElementAttribute,
                                              (CFTypeRef *)&el) == kAXErrorSuccess && el) {
                AXUIElementCopyAttributeValue(el, kAXSelectedTextAttribute, &sel);
                if (el) CFRelease(el);
            }
            if (app) CFRelease(app);
        }
        if (sel) {
            NSString *s = (__bridge NSString *)sel;
            CFRelease(sel);
            CFRelease(sys);
            std::string out(s.UTF8String ? s.UTF8String : "");
            if (!out.empty()) return out;
        }
        if (sys) CFRelease(sys);
    } @catch (...) {}

    // 2) 模拟 Cmd+C，读剪贴板（读取后还原原剪贴板内容，不覆盖用户数据）
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    NSArray *oldItems = [pb pasteboardItems];   // 浅拷贝保留旧内容
    NSString *oldStr = [pb stringForType:NSPasteboardTypeString];
    NSInteger oldChange = [pb changeCount];

    CGEventRef kd = CGEventCreateKeyboardEvent(NULL, 8 /*kVK_ANSI_C*/, true);
    CGEventSetFlags(kd, kCGEventFlagMaskCommand);
    CGEventRef ku = CGEventCreateKeyboardEvent(NULL, 8, false);
    CGEventSetFlags(ku, kCGEventFlagMaskCommand);
    CGEventPost(kCGHIDEventTap, kd);
    CGEventPost(kCGHIDEventTap, ku);
    CFRelease(kd); CFRelease(ku);

    NSString *got = nil;
    for (int i = 0; i < 20; i++) {   // 最多等 2s
        [NSThread sleepForTimeInterval:0.1];
        if ([pb changeCount] != oldChange) {
            got = [pb stringForType:NSPasteboardTypeString];
            if (got.length) break;
        }
    }

    std::string out = got.UTF8String ? got.UTF8String : "";
    // 还原原剪贴板
    [pb clearContents];
    if ([oldItems count]) [pb writeObjects:oldItems];
    else if (oldStr) [pb setString:oldStr forType:NSPasteboardTypeString];

    if (!out.empty()) return out;
    if (err) *err = "未取到选中文本：请先选中文字，或确认当前应用支持复制操作";
    return "";
}

} // namespace fanyi
