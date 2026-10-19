/* macOS platform layer: Cocoa app on the main thread, game code on its own thread.
 * Keyboard events become Windows messages and DirectInput key state. */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <GameController/GameController.h>
#include "rt.h"

#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_CLOSE 0x0010
#define WM_ACTIVATEAPP 0x001C

/* mac virtual key code -> Windows VK and set-1 scan code (DirectInput DIK value) */
typedef struct { uint8_t vk; uint8_t scan; const char *name; } KeyMap;
static KeyMap g_keys[128];
static void km(int mac, int vk, int scan, const char *name) {
    g_keys[mac].vk = (uint8_t)vk;
    g_keys[mac].scan = (uint8_t)scan;
    g_keys[mac].name = name;
}
static void init_keys(void) {
    km(0x00, 'A', 0x1E, "A"); km(0x01, 'S', 0x1F, "S"); km(0x02, 'D', 0x20, "D"); km(0x03, 'F', 0x21, "F");
    km(0x04, 'H', 0x23, "H"); km(0x05, 'G', 0x22, "G"); km(0x06, 'Z', 0x2C, "Z"); km(0x07, 'X', 0x2D, "X");
    km(0x08, 'C', 0x2E, "C"); km(0x09, 'V', 0x2F, "V"); km(0x0B, 'B', 0x30, "B"); km(0x0C, 'Q', 0x10, "Q");
    km(0x0D, 'W', 0x11, "W"); km(0x0E, 'E', 0x12, "E"); km(0x0F, 'R', 0x13, "R"); km(0x10, 'Y', 0x15, "Y");
    km(0x11, 'T', 0x14, "T"); km(0x12, '1', 0x02, "1"); km(0x13, '2', 0x03, "2"); km(0x14, '3', 0x04, "3");
    km(0x15, '4', 0x05, "4"); km(0x16, '6', 0x07, "6"); km(0x17, '5', 0x06, "5"); km(0x18, 0xBB, 0x0D, "=");
    km(0x19, '9', 0x0A, "9"); km(0x1A, '7', 0x08, "7"); km(0x1B, 0xBD, 0x0C, "-"); km(0x1C, '8', 0x09, "8");
    km(0x1D, '0', 0x0B, "0"); km(0x1E, 0xDD, 0x1B, "]"); km(0x1F, 'O', 0x18, "O"); km(0x20, 'U', 0x16, "U");
    km(0x21, 0xDB, 0x1A, "["); km(0x22, 'I', 0x17, "I"); km(0x23, 'P', 0x19, "P"); km(0x24, 0x0D, 0x1C, "Enter");
    km(0x25, 'L', 0x26, "L"); km(0x26, 'J', 0x24, "J"); km(0x27, 0xDE, 0x28, "'"); km(0x28, 'K', 0x25, "K");
    km(0x29, 0xBA, 0x27, ";"); km(0x2A, 0xDC, 0x2B, "\\"); km(0x2B, 0xBC, 0x33, ","); km(0x2C, 0xBF, 0x35, "/");
    km(0x2D, 'N', 0x31, "N"); km(0x2E, 'M', 0x32, "M"); km(0x2F, 0xBE, 0x34, "."); km(0x30, 0x09, 0x0F, "Tab");
    km(0x31, 0x20, 0x39, "Space"); km(0x32, 0xC0, 0x29, "`"); km(0x33, 0x08, 0x0E, "Backspace");
    km(0x35, 0x1B, 0x01, "Esc"); km(0x38, 0x10, 0x2A, "Shift"); km(0x39, 0x14, 0x3A, "Caps Lock");
    km(0x3A, 0x12, 0x38, "Alt"); km(0x3B, 0x11, 0x1D, "Ctrl"); km(0x3C, 0x10, 0x36, "Right Shift");
    km(0x3D, 0x12, 0xB8, "Right Alt"); km(0x3E, 0x11, 0x9D, "Right Ctrl");
    km(0x41, 0x6E, 0x53, "Num Del"); km(0x43, 0x6A, 0x37, "Num *"); km(0x45, 0x6B, 0x4E, "Num +");
    km(0x4B, 0x6F, 0xB5, "Num /"); km(0x4C, 0x0D, 0x9C, "Num Enter"); km(0x4E, 0x6D, 0x4A, "Num -");
    km(0x52, 0x60, 0x52, "Num 0"); km(0x53, 0x61, 0x4F, "Num 1"); km(0x54, 0x62, 0x50, "Num 2");
    km(0x55, 0x63, 0x51, "Num 3"); km(0x56, 0x64, 0x4B, "Num 4"); km(0x57, 0x65, 0x4C, "Num 5");
    km(0x58, 0x66, 0x4D, "Num 6"); km(0x59, 0x67, 0x47, "Num 7"); km(0x5B, 0x68, 0x48, "Num 8");
    km(0x5C, 0x69, 0x49, "Num 9");
    km(0x60, 0x74, 0x3F, "F5"); km(0x61, 0x75, 0x40, "F6"); km(0x62, 0x76, 0x41, "F7"); km(0x63, 0x72, 0x3D, "F3");
    km(0x64, 0x77, 0x42, "F8"); km(0x65, 0x78, 0x43, "F9"); km(0x67, 0x7A, 0x57, "F11"); km(0x6D, 0x79, 0x44, "F10");
    km(0x6F, 0x7B, 0x58, "F12"); km(0x73, 0x24, 0xC7, "Home"); km(0x74, 0x21, 0xC9, "Page Up");
    km(0x75, 0x2E, 0xD3, "Delete"); km(0x76, 0x73, 0x3E, "F4"); km(0x77, 0x23, 0xCF, "End");
    km(0x78, 0x71, 0x3C, "F2"); km(0x79, 0x22, 0xD1, "Page Down"); km(0x7A, 0x70, 0x3B, "F1");
    km(0x7B, 0x25, 0xCB, "Left"); km(0x7C, 0x27, 0xCD, "Right"); km(0x7D, 0x28, 0xD0, "Down");
    km(0x7E, 0x26, 0xC8, "Up");
}

static volatile uint8_t g_vk_state[256], g_scan_state[256];
bool plat_vk_down(int vk) { return vk >= 0 && vk < 256 && g_vk_state[vk]; }
bool plat_scan_down(int scan) { return scan >= 0 && scan < 256 && g_scan_state[scan]; }
uint32_t plat_map_key(uint32_t code, uint32_t type) {
    for (int i = 0; i < 128; i++) {
        if (!g_keys[i].vk) continue;
        if (type == 0 && g_keys[i].vk == code) return g_keys[i].scan & 0x7F;
        if ((type == 1 || type == 3) && (g_keys[i].scan & 0x7F) == code) return g_keys[i].vk;
    }
    return 0;
}
const char *plat_scan_name(uint32_t scan) {
    for (int i = 0; i < 128; i++)
        if (g_keys[i].vk && ((g_keys[i].scan & 0x7F) == (scan & 0x7F)) &&
            (((scan & 0x100) != 0) == ((g_keys[i].scan & 0x80) != 0)))
            return g_keys[i].name;
    return "";
}

static void key_event(unsigned short code, bool down, bool repeat) {
    if (code >= 128 || !g_keys[code].vk) return;
    KeyMap k = g_keys[code];
    g_vk_state[k.vk] = down;
    g_scan_state[k.scan] = down;
    if (k.vk == 0x10) g_vk_state[code == 0x3C ? 0xA1 : 0xA0] = down;
    if (k.vk == 0x11) g_vk_state[code == 0x3E ? 0xA3 : 0xA2] = down;
    if (k.vk == 0x12) g_vk_state[code == 0x3D ? 0xA5 : 0xA4] = down;
    uint32_t lp = 1u | ((uint32_t)(k.scan & 0x7F) << 16) | ((k.scan & 0x80) ? 1u << 24 : 0) |
                  (repeat ? 1u << 30 : 0) | (down ? 0 : 3u << 30);
    if (!repeat) dinput_key_event(k.scan, down);
    bool sys = k.vk == 0x12 || g_vk_state[0x12];
    user32_post_input(down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP), k.vk, lp);
}

/* MGS_PEEK="10:716e1c:16,..." logs guest memory (hex) at those times, for debugging */
static void *peeker(void *arg) {
    (void)arg;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", getenv("MGS_PEEK"));
    uint64_t t0 = plat_ticks_us();
    char *save = NULL;   /* strtok_r: the key script thread tokenises at the same time */
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        double at;
        unsigned a, n;
        if (sscanf(tok, "%lf:%x:%u", &at, &a, &n) != 3) continue;
        while (plat_ticks_us() < t0 + (uint64_t)(at * 1e6)) usleep(10000);
        char line[1024];
        int k = 0;
        for (unsigned i = 0; i < n && k < 1000; i++) k += snprintf(line + k, sizeof line - (size_t)k, "%02x ", M[a + i]);
        rt_log("PEEK %08x: %s", a, line);
    }
    return NULL;
}

static void *trace_at(void *arg) {
    (void)arg;
    double at = 0;
    int n = 1000;
    sscanf(getenv("MGS_TRACE_AT"), "%lf:%d", &at, &n);
    uint64_t t0 = plat_ticks_us();
    while (plat_ticks_us() < t0 + (uint64_t)(at * 1e6)) usleep(10000);
    rt_log("tracing %d import calls", n);
    g_trace = n + 1;
    return NULL;
}

static void toggle_fullscreen(void);
/* MGS_KEYS="25:Enter,30.5:Down" presses keys at those times (seconds after start) for testing;
 * "FullScreen" switches full screen like Cmd+F, "Quit" closes the window */
static void *autokeys(void *arg) {
    (void)arg;
    const char *spec = getenv("MGS_KEYS");
    uint64_t t0 = g_plat_t0;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", spec);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *colon = strchr(tok, ':');
        if (!colon) continue;
        *colon = 0;
        double at = atof(tok);
        char name[64];
        snprintf(name, sizeof name, "%s", colon + 1);
        double hold = 0.15;   /* "Up*2.5" holds the key for 2.5 s */
        char *star = strchr(name, '*');
        if (star) { *star = 0; hold = atof(star + 1); }
        if (!strcasecmp(name, "Quit")) {
            uint64_t when = t0 + (uint64_t)(at * 1e6);
            while (plat_ticks_us() < when) usleep(5000);
            rt_log("MGS_KEYS: window close at %.1fs", at);
            user32_post_input(WM_CLOSE, 0, 0);
            continue;
        }
        if (!strcasecmp(name, "FullScreen")) {
            uint64_t when = t0 + (uint64_t)(at * 1e6);
            while (plat_ticks_us() < when) usleep(5000);
            rt_log("MGS_KEYS: full screen on/off at %.1fs", at);
            toggle_fullscreen();
            continue;
        }
        int code = -1;
        for (int i = 0; i < 128; i++)
            if (g_keys[i].name && !strcasecmp(g_keys[i].name, name)) { code = i; break; }
        if (code < 0) { rt_log("MGS_KEYS: unknown key %s", name); continue; }
        uint64_t when = t0 + (uint64_t)(at * 1e6);
        while (plat_ticks_us() < when) usleep(5000);
        rt_log("MGS_KEYS: press %s at %.1fs", name, at);
        key_event((unsigned short)code, true, false);
        usleep((useconds_t)(hold * 1e6));
        key_event((unsigned short)code, false, false);
    }
    return NULL;
}

/* ------------------------------------------------------------------ window */
static NSWindow *g_win;
static int g_game_w = 640, g_game_h = 480;
static CAMetalLayer *g_layer;
static bool g_cursor_visible = true;

@interface GameView : NSView
@end
@implementation GameView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer *)makeBackingLayer {
    CAMetalLayer *l = [CAMetalLayer layer];
    l.device = MTLCreateSystemDefaultDevice();
    l.pixelFormat = MTLPixelFormatBGRA8Unorm;
    l.framebufferOnly = NO;
    l.contentsScale = [NSScreen mainScreen].backingScaleFactor;
    g_layer = l;
    return l;
}
- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    if (g_layer) g_layer.contentsScale = self.window.backingScaleFactor;
}
- (void)keyDown:(NSEvent *)e {
    NSUInteger mods = e.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    if ((mods & NSEventModifierFlagCommand)) {
        if (e.keyCode == 0x03 || e.keyCode == 0x24) toggle_fullscreen();               /* Cmd+F, Cmd+Enter */
        if (e.keyCode == 0x0C) user32_post_input(WM_CLOSE, 0, 0);                    /* Cmd+Q */
        return;
    }
    key_event(e.keyCode, true, e.isARepeat);
}
- (void)keyUp:(NSEvent *)e { key_event(e.keyCode, false, false); }
- (void)flagsChanged:(NSEvent *)e {
    unsigned short code = e.keyCode;
    NSUInteger f = e.modifierFlags;
    bool down = false;
    switch (code) {
    case 0x38: case 0x3C: down = (f & NSEventModifierFlagShift) != 0; break;
    case 0x3B: case 0x3E: down = (f & NSEventModifierFlagControl) != 0; break;
    case 0x3A: case 0x3D: down = (f & NSEventModifierFlagOption) != 0; break;
    case 0x39: down = (f & NSEventModifierFlagCapsLock) != 0; break;
    default: return;
    }
    key_event(code, down, false);
}
- (void)resetCursorRects {
    if (!g_cursor_visible) {
        static NSCursor *blank;
        if (!blank) {
            NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(1, 1)];
            blank = [[NSCursor alloc] initWithImage:img hotSpot:NSZeroPoint];
        }
        [self addCursorRect:self.bounds cursor:blank];
    }
}
@end

/* ------------------------------------------------------------------ full screen */
/* The game asks DirectDraw for exclusive full screen; on the Mac that is the window in macOS full
 * screen. Cmd+F (or the green button) switches, and the next start follows the player's last choice
 * instead of the game. MGS_FULLSCREEN=0/1 (tests) overrides both and is not remembered. */
static NSString *const kFullScreenKey = @"MGSFullScreen";
static bool g_fs_decided;
static void fs_remember(bool on) {
    if (getenv("MGS_FULLSCREEN")) return;
    [[NSUserDefaults standardUserDefaults] setBool:on forKey:kFullScreenKey];
}
static void toggle_fullscreen(void) {
    dispatch_async(dispatch_get_main_queue(), ^{ [g_win toggleFullScreen:nil]; });
}
void plat_window_fullscreen(bool game_wants) {
    if (!g_win || g_fs_decided) return;
    g_fs_decided = true;
    const char *e = getenv("MGS_FULLSCREEN");
    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    bool on = e ? atoi(e) != 0 : [ud objectForKey:kFullScreenKey] ? [ud boolForKey:kFullScreenKey] : game_wants;
    rt_log("full screen: the game %s it, %s", game_wants ? "asks for" : "does not ask for",
           on ? "entering it" : "staying in a window");
    if (on)
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!(g_win.styleMask & NSWindowStyleMaskFullScreen)) [g_win toggleFullScreen:nil];
        });
}

@interface WinDelegate : NSObject <NSWindowDelegate>
@end
@implementation WinDelegate
- (BOOL)windowShouldClose:(NSWindow *)w {
    user32_post_input(WM_CLOSE, 0, 0);
    return NO;
}
- (void)windowDidBecomeKey:(NSNotification *)n { user32_post_input(WM_ACTIVATEAPP, 1, 0); }
- (void)windowDidEnterFullScreen:(NSNotification *)n {
    rt_log("window entered full screen");
    fs_remember(true);
}
- (void)windowDidExitFullScreen:(NSNotification *)n {
    rt_log("window left full screen");
    fs_remember(false);
}
- (void)windowDidResignKey:(NSNotification *)n {
    memset((void *)g_vk_state, 0, sizeof g_vk_state);
    memset((void *)g_scan_state, 0, sizeof g_scan_state);
}
@end
static WinDelegate *g_delegate;

static void on_main(void (^b)(void)) {
    if ([NSThread isMainThread]) b();
    else dispatch_sync(dispatch_get_main_queue(), b);
}

void plat_window_create(int w, int h, const char *title) {
    g_game_w = w;
    g_game_h = h;
    if (getenv("MGS_HEADLESS")) return;
    NSString *t = [NSString stringWithUTF8String:title[0] ? title : "Metal Gear Solid"];
    on_main(^{
        NSRect vis = [NSScreen mainScreen].visibleFrame;
        /* a 4:3 window filling most of the screen height */
        CGFloat ch = floor(vis.size.height * 0.85), cw = floor(ch * 4 / 3);
        NSRect r = NSMakeRect(vis.origin.x + (vis.size.width - cw) / 2, vis.origin.y + (vis.size.height - ch) / 2, cw, ch);
        g_win = [[NSWindow alloc] initWithContentRect:r
                                            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                      NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                              backing:NSBackingStoreBuffered
                                                defer:NO];
        g_win.title = t;
        g_win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        g_win.backgroundColor = NSColor.blackColor;
        GameView *v = [[GameView alloc] initWithFrame:NSMakeRect(0, 0, cw, ch)];
        v.wantsLayer = YES;
        g_win.contentView = v;
        g_delegate = [WinDelegate new];
        g_win.delegate = g_delegate;
        [g_win makeFirstResponder:v];
    });
}
static bool g_shown;
void plat_window_show(void) {
    if (!g_win || g_shown) return;
    g_shown = true;
    rt_log("window shown");
    on_main(^{
        [g_win makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    });
}
void plat_window_set_title(const char *t) {
    if (!g_win) return;
    NSString *s = [NSString stringWithUTF8String:t];
    dispatch_async(dispatch_get_main_queue(), ^{ g_win.title = s; });
}
void plat_client_size(int *w, int *h) {
    *w = g_game_w;
    *h = g_game_h;
}
void plat_set_game_size(int w, int h) {
    g_game_w = w;
    g_game_h = h;
}
void plat_screen_size(int *w, int *h) {
    /* the desktop the game sees; it renders into our window at any size */
    *w = 1920;
    *h = 1440;
}
void plat_cursor_visible(bool v) {
    g_cursor_visible = v;
    if (!g_win) return;
    dispatch_async(dispatch_get_main_queue(), ^{ [g_win invalidateCursorRectsForView:g_win.contentView]; });
}
void *plat_metal_layer(void) { return (__bridge void *)g_layer; }
void plat_drawable_size(int *w, int *h) {
    if (!g_win) {
        /* MGS_HEADLESS_H=2880: headless runs render (and capture) at that height, like a big window */
        static int hh = -1;
        if (hh < 0) hh = getenv("MGS_HEADLESS_H") ? atoi(getenv("MGS_HEADLESS_H")) : 0;
        *h = hh > 0 ? hh : g_game_h;
        *w = hh > 0 ? hh * g_game_w / g_game_h : g_game_w;
        return;
    }
    __block CGSize s;
    on_main(^{
        NSView *v = g_win.contentView;
        CGFloat k = g_win.backingScaleFactor;
        s = CGSizeMake(v.bounds.size.width * k, v.bounds.size.height * k);
        if (g_layer) g_layer.drawableSize = s;
    });
    *w = (int)s.width;
    *h = (int)s.height;
}

/* ------------------------------------------------------------------ licence */
/* The GOG build asks EBUEula.dll whether the licence was accepted. Ask once, natively, and remember. */
bool plat_eula_accepted(const char *eula_path) {
    if (getenv("MGS_HEADLESS") || getenv("MGS_ACCEPT_EULA")) return true;
    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    if ([ud boolForKey:@"MGSLicenceAccepted"]) return true;
    NSString *path = eula_path ? [NSString stringWithUTF8String:eula_path] : nil;
    __block NSInteger r = 0;
    dispatch_sync(dispatch_get_main_queue(), ^{
        for (;;) {
            NSAlert *a = [NSAlert new];
            a.messageText = @"Metal Gear Solid licence agreement";
            a.informativeText = @"Playing the game means accepting the licence agreement that came with it (EULA.RTF in the game folder).";
            [a addButtonWithTitle:@"Accept"];
            [a addButtonWithTitle:@"Decline"];
            if (path) [a addButtonWithTitle:@"Read Licence"];
            [NSApp activateIgnoringOtherApps:YES];
            r = [a runModal];
            if (r == NSAlertThirdButtonReturn && path) {
                [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:path]];
                continue;
            }
            break;
        }
    });
    if (r != NSAlertFirstButtonReturn) return false;
    [ud setBool:YES forKey:@"MGSLicenceAccepted"];
    return true;
}

/* ------------------------------------------------------------------ game controller */
void plat_pad(PadState *s) {
    memset(s, 0, sizeof *s);
    GCController *pad = GCController.controllers.firstObject;
    GCExtendedGamepad *g = pad.extendedGamepad;
    if (!g) return;
    s->connected = true;
    s->lx = g.leftThumbstick.xAxis.value;
    s->ly = -g.leftThumbstick.yAxis.value;  /* DirectInput: +y is down */
    s->rx = g.rightThumbstick.xAxis.value;
    s->ry = -g.rightThumbstick.yAxis.value;
    s->lt = g.leftTrigger.value;
    s->rt = g.rightTrigger.value;
    uint32_t b = 0;
    if (g.buttonA.pressed) b |= PAD_A;
    if (g.buttonB.pressed) b |= PAD_B;
    if (g.buttonX.pressed) b |= PAD_X;
    if (g.buttonY.pressed) b |= PAD_Y;
    if (g.leftShoulder.pressed) b |= PAD_LB;
    if (g.rightShoulder.pressed) b |= PAD_RB;
    if (g.leftTrigger.pressed) b |= PAD_LT;
    if (g.rightTrigger.pressed) b |= PAD_RT;
    if (g.buttonOptions.pressed) b |= PAD_BACK;
    if (g.buttonMenu.pressed) b |= PAD_START;
    if (g.leftThumbstickButton.pressed) b |= PAD_LS;
    if (g.rightThumbstickButton.pressed) b |= PAD_RS;
    if (g.dpad.up.pressed) b |= PAD_UP;
    if (g.dpad.down.pressed) b |= PAD_DOWN;
    if (g.dpad.left.pressed) b |= PAD_LEFT;
    if (g.dpad.right.pressed) b |= PAD_RIGHT;
    s->buttons = b;
}

/* ------------------------------------------------------------------ app */
@interface AppDelegate : NSObject <NSApplicationDelegate>
@end
@implementation AppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)a {
    user32_post_input(WM_CLOSE, 0, 0);
    return NSTerminateCancel;
}
@end
static AppDelegate *g_app_delegate;

uint64_t g_plat_t0;
void plat_init(void) {
    g_plat_t0 = plat_ticks_us();
    init_keys();
    if (getenv("MGS_KEYS")) {
        pthread_t t;
        pthread_create(&t, NULL, autokeys, NULL);
    }
    if (getenv("MGS_TRACE_AT")) {
        pthread_t t;
        pthread_create(&t, NULL, trace_at, NULL);
    }
    if (getenv("MGS_PEEK")) {
        pthread_t t;
        pthread_create(&t, NULL, peeker, NULL);
    }
    if (getenv("MGS_HEADLESS")) return;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    g_app_delegate = [AppDelegate new];
    NSApp.delegate = g_app_delegate;
    NSMenu *bar = [NSMenu new], *app = [NSMenu new];
    NSMenuItem *item = [NSMenuItem new];
    [bar addItem:item];
    [app addItemWithTitle:@"Quit Metal Gear Solid" action:@selector(terminate:) keyEquivalent:@"q"];
    item.submenu = app;
    NSApp.mainMenu = bar;
}

static void *game_main(void *f) {
    ((void (*)(void))f)();
    return NULL;
}

void plat_run_main_loop(void (*game)(void)) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 512u << 20); /* recompiled code recurses on the host stack */
    pthread_t t;
    pthread_create(&t, &a, game_main, (void *)game);
    if (getenv("MGS_HEADLESS")) {
        pthread_join(t, NULL);
        exit(0);
    }
    [NSApp run];
}
