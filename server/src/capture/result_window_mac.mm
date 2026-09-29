/*
 * fanyi — result_window_mac.mm
 * 结果浮窗：光标附近的非激活面板（不打断当前输入焦点），
 * 原文对照 + 译文 + 复制按钮；Esc/失焦/25s 超时自动关闭。
 */
#import <AppKit/AppKit.h>

#include <string>

// 无边框面板要成为 key window（接收 Esc）必须重写 canBecomeKey
@interface FanyiPanel : NSPanel
@end
@implementation FanyiPanel
- (BOOL)canBecomeKey { return YES; }
@end

@interface FanyiResultBox : NSObject
@property(strong) NSPanel *win;
@property(copy) NSString *trans;
@property(strong) NSTimer *timer;
- (instancetype)initWithPanel:(NSPanel *)win translate:(NSString *)t;
- (void)copyTrans:(id)sender;
- (void)close;
- (void)installObservers;
@end

@implementation FanyiResultBox
- (instancetype)initWithPanel:(NSPanel *)win translate:(NSString *)t {
    if ((self = [super init])) { _win = win; _trans = t; }
    return self;
}
- (void)copyTrans:(id)sender {
    (void)sender;
    [[NSPasteboard generalPasteboard] clearContents];
    [[NSPasteboard generalPasteboard] setString:self.trans forType:NSPasteboardTypeString];
}
- (void)close {
    [self.timer invalidate];
    [self.win orderOut:nil];
}
- (void)installObservers {
    __weak FanyiResultBox *me = self;
    NSTimer *t = [NSTimer timerWithTimeInterval:25.0
                                        repeats:NO
                                          block:^(NSTimer *) { [me close]; }];
    [[NSRunLoop mainRunLoop] addTimer:t forMode:NSRunLoopCommonModes];
    self.timer = t;
    // 失焦自动关
    [[NSNotificationCenter defaultCenter]
        addObserverForName:NSWindowDidResignKeyNotification object:self.win queue:nil
        usingBlock:^(NSNotification *n) { [[me win] orderOut:nil]; }];
}
@end

@interface FanyiPanelEsc : FanyiPanel
@end
@implementation FanyiPanelEsc
- (void)keyDown:(NSEvent *)e {
    if (e.keyCode == 53) {   // Esc
        [NSApp sendAction:@selector(fanyiClose:) to:nil from:self];
    } else {
        [super keyDown:e];
    }
}
@end

// Esc 关闭走 first-responder 链找不到目标时的兜底：直接在 contentView 上装本地监听
@interface FanyiEscView : NSView
@property(weak) FanyiResultBox *box;
@end
@implementation FanyiEscView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent *)e {
    if (e.keyCode == 53) { [self.box close]; return; }
    [super keyDown:e];
}
@end

namespace fanyi {

void result_window_show(const std::string &original,
                        const std::string &translated,
                        const std::string &target_name) {
    NSString *nsOrig = [NSString stringWithUTF8String:original.c_str()];
    NSString *nsTrans = [NSString stringWithUTF8String:translated.c_str()];
    NSString *nsTarget = [NSString stringWithUTF8String:target_name.c_str()];

    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            FanyiPanel *w = [[FanyiPanelEsc alloc]
                initWithContentRect:NSMakeRect(0, 0, 340, 150)
                          styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                            backing:NSBackingStoreBuffered
                              defer:NO];
            [w setLevel:NSFloatingWindowLevel];
            [w setOpaque:NO];
            [w setBackgroundColor:[NSColor colorWithCalibratedWhite:0.11 alpha:0.96]];
            [w setHasShadow:YES];
            w.releasedWhenClosed = NO;

            FanyiResultBox *box = [[FanyiResultBox alloc] initWithPanel:w translate:nsTrans];

            FanyiEscView *content = [[FanyiEscView alloc] initWithFrame:NSMakeRect(0, 0, 340, 150)];
            content.box = box;
            w.contentView = content;

            NSTextField *title = [[NSTextField alloc] initWithFrame:NSMakeRect(14, 128, 312, 16)];
            title.stringValue = [NSString stringWithFormat:@"已翻译为 %@", nsTarget];
            title.editable = NO; title.bordered = NO; title.drawsBackground = NO;
            title.textColor = [NSColor colorWithCalibratedRed:0.35 green:0.62 blue:1 alpha:1];
            title.font = [NSFont boldSystemFontOfSize:11];
            [content addSubview:title];

            NSTextField *orig = [[NSTextField alloc] initWithFrame:NSMakeRect(14, 104, 312, 22)];
            orig.stringValue = nsOrig;
            orig.editable = NO; orig.bordered = NO; orig.drawsBackground = NO;
            orig.textColor = [NSColor colorWithCalibratedWhite:0.62 alpha:1];
            orig.font = [NSFont systemFontOfSize:11];
            [content addSubview:orig];

            NSTextField *trans = [[NSTextField alloc] initWithFrame:NSMakeRect(14, 36, 312, 64)];
            trans.stringValue = nsTrans;
            trans.editable = NO; trans.bordered = NO; trans.drawsBackground = NO;
            trans.textColor = [NSColor whiteColor];
            trans.font = [NSFont systemFontOfSize:14];
            [content addSubview:trans];

            NSButton *copy = [[NSButton alloc] initWithFrame:NSMakeRect(14, 6, 96, 24)];
            copy.title = @"复制译文";
            copy.bezelStyle = NSBezelStyleRounded;
            copy.font = [NSFont systemFontOfSize:12];
            copy.target = box;
            copy.action = @selector(copyTrans:);
            [content addSubview:copy];

            // 定位：鼠标附近，屏内收拢
            NSPoint m = [NSEvent mouseLocation];
            NSScreen *screen = [NSScreen mainScreen];
            CGFloat x = MIN(MAX(8, m.x + 12), NSMaxX(screen.frame) - 356);
            CGFloat y = MIN(MAX(8, m.y - 170), NSMaxY(screen.frame) - 165);
            [w setFrameTopLeftPoint:NSMakePoint(x, y)];

            [box installObservers];
            [w makeKeyAndOrderFront:nil];
            [w orderFrontRegardless];
        }
    });
}

} // namespace fanyi
