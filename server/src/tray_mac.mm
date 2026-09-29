/*
 * fanyi — tray_mac.mm
 * macOS 状态栏图标（NSStatusItem）。tray_run 阻塞主线程直到退出。
 * 注意：Objective-C 类必须在全局作用域（不能放进 namespace）。
 */
#import <Cocoa/Cocoa.h>

#include "tray.h"
#include <atomic>

/* 供 ObjC 类访问的动作表（全局） */
static fanyi::TrayActions g_actions;
static std::atomic<bool> g_quit{false};

@interface FanyiTarget : NSObject
- (void)onSettings:(id)sender;
- (void)onToggle:(id)sender;
- (void)onSelectTranslate:(id)sender;
- (void)onCaptureTranslate:(id)sender;
- (void)onQuit:(id)sender;
@end

@implementation FanyiTarget
- (void)onSettings:(id)sender {
    (void)sender;
    NSString *url = [NSString stringWithUTF8String:g_actions.settings_url];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:url]];
}
- (void)onToggle:(id)sender {
    (void)sender;
    g_actions.toggle_enabled();
    NSMenuItem *it = (NSMenuItem *)sender;
    it.state = g_actions.enabled() ? NSControlStateValueOn : NSControlStateValueOff;
    it.title  = g_actions.enabled() ? @"翻译：开（点此暂停）" : @"翻译：暂停（点此开启）";
}
- (void)onSelectTranslate:(id)sender {
    (void)sender;
    g_actions.select_translate();
}
- (void)onCaptureTranslate:(id)sender {
    (void)sender;
    g_actions.capture_translate();
}
- (void)onQuit:(id)sender {
    (void)sender;
    g_actions.quit();
    g_quit.store(true);
    [NSApp terminate:nil];
}
@end

namespace fanyi {

int tray_run(const TrayActions &actions) {
    g_actions = actions;
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];

        NSStatusItem *item = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
        item.button.title = @"译";

        NSMenu *menu = [[NSMenu alloc] init];
        FanyiTarget *target = [[FanyiTarget alloc] init];

        NSMenuItem *settings = [[NSMenuItem alloc] initWithTitle:@"打开设置…"
                                                          action:@selector(onSettings:) keyEquivalent:@"s"];
        settings.target = target;

        NSMenuItem *toggle = [[NSMenuItem alloc] initWithTitle:@"翻译：开（点此暂停）"
                                                        action:@selector(onToggle:) keyEquivalent:@"t"];
        toggle.target = target;
        toggle.state = actions.enabled() ? NSControlStateValueOn : NSControlStateValueOff;

        [menu addItem:settings];
        [menu addItem:toggle];
        [menu addItem:[NSMenuItem separatorItem]];

        NSMenuItem *sel = [[NSMenuItem alloc] initWithTitle:@"划词翻译（先选中文字再点）"
                                                     action:@selector(onSelectTranslate:) keyEquivalent:@"e"];
        sel.target = target;
        [menu addItem:sel];

        NSMenuItem *cap = [[NSMenuItem alloc] initWithTitle:@"截图翻译（框选屏幕区域）"
                                                     action:@selector(onCaptureTranslate:) keyEquivalent:@"g"];
        cap.target = target;
        [menu addItem:cap];

        [menu addItem:[NSMenuItem separatorItem]];

        NSMenuItem *quit = [[NSMenuItem alloc] initWithTitle:@"退出 fanyi"
                                                      action:@selector(onQuit:) keyEquivalent:@"q"];
        quit.target = target;

        item.menu = menu;

        [app run];   /* 阻塞；返回条件：quit 菜单或外部 tray_request_stop → terminate */
        if (!g_quit.load()) actions.quit();
    }
    return 0;
}

void tray_request_stop() {
    dispatch_async(dispatch_get_main_queue(), ^{
        if ([NSApp respondsToSelector:@selector(terminate:)]) [NSApp terminate:nil];
    });
}

} // namespace fanyi
