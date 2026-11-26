// UIApplication, UIApplicationMain and the main event loop.
#import <emu/UIKit.h>
#include <emu/host.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <emu/msg.h>
extern void* objc_autoreleasePoolPush(void);
extern void objc_autoreleasePoolPop(void*);
extern void _uikit_set_pixel_size(int w, int h);
extern void _uikit_pixel_size(int* w, int* h);
extern NSTimeInterval _emu_runloop_next_timer(void);
extern int _dispatch_main_queue_pending(void);
extern void _uikit_text_input_char(unsigned ch);
extern void _uikit_text_input_key(int vk);
extern void _uikit_poll_image_picker(void);

#define NOTE(n) EMU_EXPORT NSString* const n = @#n
NOTE(UIApplicationDidBecomeActiveNotification);
NOTE(UIApplicationDidEnterBackgroundNotification);
NOTE(UIApplicationDidFinishLaunchingNotification);
NOTE(UIApplicationDidReceiveMemoryWarningNotification);
NOTE(UIApplicationWillEnterForegroundNotification);
NOTE(UIApplicationWillResignActiveNotification);
NOTE(UIApplicationWillTerminateNotification);
NOTE(UIApplicationDidChangeStatusBarOrientationNotification);
NOTE(UIApplicationLaunchOptionsURLKey);
NOTE(UIApplicationLaunchOptionsSourceApplicationKey);
NOTE(UIApplicationLaunchOptionsRemoteNotificationKey);
NOTE(UIApplicationOpenSettingsURLString);
NOTE(UIDeviceOrientationDidChangeNotification);
NOTE(UIKeyboardWillShowNotification);
NOTE(UIKeyboardDidShowNotification);
NOTE(UIKeyboardWillHideNotification);
NOTE(UIKeyboardDidHideNotification);
NOTE(UIKeyboardWillChangeFrameNotification);
NOTE(UIKeyboardDidChangeFrameNotification);
NOTE(UIKeyboardFrameBeginUserInfoKey);
NOTE(UIKeyboardFrameEndUserInfoKey);
NOTE(UIKeyboardAnimationDurationUserInfoKey);
NOTE(UIKeyboardAnimationCurveUserInfoKey);
NOTE(UITextViewTextDidChangeNotification);
NOTE(UITextViewTextDidBeginEditingNotification);
NOTE(UITextViewTextDidEndEditingNotification);
NOTE(UITextFieldTextDidChangeNotification);
NOTE(UITextFieldTextDidBeginEditingNotification);
NOTE(UITextFieldTextDidEndEditingNotification);
NOTE(UIScreenDidConnectNotification);
NOTE(UIScreenDidDisconnectNotification);
NOTE(UIContentSizeCategoryDidChangeNotification);
NOTE(UIAccessibilityVoiceOverStatusChanged);
NOTE(UIImagePickerControllerOriginalImage);
NOTE(UIImagePickerControllerEditedImage);
NOTE(UIImagePickerControllerMediaType);
EMU_EXPORT NSString* const NSFontAttributeName = @"NSFont";
EMU_EXPORT NSString* const NSForegroundColorAttributeName = @"NSColor";
EMU_EXPORT NSString* const NSParagraphStyleAttributeName = @"NSParagraphStyle";
EMU_EXPORT const NSUInteger UIBackgroundTaskInvalid = 0;
EMU_EXPORT const double UIWindowLevelNormal = 0, UIWindowLevelAlert = 2000, UIWindowLevelStatusBar = 1000;
EMU_EXPORT const UIEdgeInsets UIEdgeInsetsZero = {0, 0, 0, 0};
EMU_EXPORT const CGRect CGRectZero = {{0, 0}, {0, 0}};
EMU_EXPORT const CGRect CGRectNull = {{1e300, 1e300}, {0, 0}};
EMU_EXPORT const CGRect CGRectInfinite = {{-8.98846567431158e307, -8.98846567431158e307}, {1.79769313486232e308, 1.79769313486232e308}};
EMU_EXPORT const CGPoint CGPointZero = {0, 0};
EMU_EXPORT const CGSize CGSizeZero = {0, 0};
EMU_EXPORT const CGAffineTransform CGAffineTransformIdentity = {1, 0, 0, 1, 0, 0};

// accessibility queries
EMU_EXPORT BOOL UIAccessibilityIsVoiceOverRunning(void) { return NO; }
EMU_EXPORT BOOL UIAccessibilityIsClosedCaptioningEnabled(void) { return NO; }
EMU_EXPORT BOOL UIAccessibilityIsGuidedAccessEnabled(void) { return NO; }
EMU_EXPORT BOOL UIAccessibilityIsInvertColorsEnabled(void) { return NO; }
EMU_EXPORT BOOL UIAccessibilityIsMonoAudioEnabled(void) { return NO; }
EMU_EXPORT BOOL UIAccessibilityIsReduceMotionEnabled(void) { return NO; }
EMU_EXPORT BOOL UIAccessibilityIsReduceTransparencyEnabled(void) { return NO; }
EMU_EXPORT BOOL UIAccessibilityIsSpeakScreenEnabled(void) { return NO; }
EMU_EXPORT BOOL UIAccessibilityIsSpeakSelectionEnabled(void) { return NO; }
EMU_EXPORT BOOL UIAccessibilityIsSwitchControlRunning(void) { return NO; }
EMU_EXPORT BOOL UIAccessibilityIsBoldTextEnabled(void) { return NO; }
EMU_EXPORT BOOL UIAccessibilityIsGrayscaleEnabled(void) { return NO; }
EMU_EXPORT BOOL UIAccessibilityDarkerSystemColorsEnabled(void) { return NO; }
EMU_EXPORT void UIAccessibilityPostNotification(uint32_t n, id a) { (void)n; (void)a; }

// ===================================================================== UIApplication
@interface UIApplication : UIResponder {
    id _delegate;
    BOOL _idle;
    NSInteger _state;
    NSInteger _badge;
}
@end
static UIApplication* g_app;
@implementation UIApplication
+ (UIApplication*)sharedApplication { return g_app; }
- (id)delegate { return _delegate; }
- (void)setDelegate:(id)d { _delegate = d; }
- (UIWindow*)keyWindow { return [UIWindow performSelector:@selector(_keyWindow)]; }
- (NSArray*)windows { return [UIWindow performSelector:@selector(_windows)]; }
- (NSInteger)applicationState { return _state; }
- (void)_setState:(NSInteger)s { _state = s; }
- (NSInteger)statusBarOrientation { return 4; }  // UIInterfaceOrientationLandscapeRight... (Left == 4 in UIInterfaceOrientation)
- (void)setStatusBarOrientation:(NSInteger)o { (void)o; }
- (void)setStatusBarOrientation:(NSInteger)o animated:(BOOL)a { (void)o; (void)a; }
- (BOOL)isStatusBarHidden { return YES; }
- (void)setStatusBarHidden:(BOOL)h { (void)h; }
- (void)setStatusBarHidden:(BOOL)h withAnimation:(NSInteger)a { (void)h; (void)a; }
- (void)setStatusBarHidden:(BOOL)h animated:(BOOL)a { (void)h; (void)a; }
- (void)setStatusBarStyle:(NSInteger)s { (void)s; }
- (void)setStatusBarStyle:(NSInteger)s animated:(BOOL)a { (void)s; (void)a; }
- (CGRect)statusBarFrame { return CGRectZero; }
- (BOOL)isIdleTimerDisabled { return _idle; }
- (void)setIdleTimerDisabled:(BOOL)b { _idle = b; }
- (BOOL)openURL:(NSURL*)u { NSLog(@"[UIKit] openURL: %@ (ignored)", u); return NO; }
- (void)openURL:(NSURL*)u options:(NSDictionary*)o completionHandler:(void (^)(BOOL))c { (void)o; [self openURL:u]; if (c) c(NO); }
- (BOOL)canOpenURL:(NSURL*)u { (void)u; return NO; }
- (void)registerUserNotificationSettings:(id)s { (void)s; }
- (void)registerForRemoteNotifications {}
- (void)unregisterForRemoteNotifications {}
- (BOOL)isRegisteredForRemoteNotifications { return NO; }
- (void)registerForRemoteNotificationTypes:(NSUInteger)t { (void)t; }
- (id)currentUserNotificationSettings { return nil; }
- (void)scheduleLocalNotification:(id)n { (void)n; }
- (void)cancelAllLocalNotifications {}
- (void)presentLocalNotificationNow:(id)n { (void)n; }
- (NSInteger)applicationIconBadgeNumber { return _badge; }
- (void)setApplicationIconBadgeNumber:(NSInteger)n { _badge = n; }
- (NSUInteger)beginBackgroundTaskWithExpirationHandler:(void (^)(void))h { (void)h; return 1; }
- (NSUInteger)beginBackgroundTaskWithName:(NSString*)n expirationHandler:(void (^)(void))h { (void)n; (void)h; return 1; }
- (void)endBackgroundTask:(NSUInteger)t { (void)t; }
- (NSTimeInterval)backgroundTimeRemaining { return 600; }
- (void)setMinimumBackgroundFetchInterval:(NSTimeInterval)i { (void)i; }
- (void)setNetworkActivityIndicatorVisible:(BOOL)b { (void)b; }
- (BOOL)isNetworkActivityIndicatorVisible { return NO; }
- (void)beginReceivingRemoteControlEvents {}
- (void)endReceivingRemoteControlEvents {}
- (void)beginIgnoringInteractionEvents {}
- (void)endIgnoringInteractionEvents {}
- (BOOL)isIgnoringInteractionEvents { return NO; }
- (BOOL)sendAction:(SEL)a to:(id)t from:(id)s forEvent:(UIEvent*)e { (void)e; if (t) objc_msgSend(t, a, s); return t != nil; }
- (void)sendEvent:(UIEvent*)e { (void)e; }
- (NSInteger)userInterfaceLayoutDirection { return 0; }
- (NSUInteger)supportedInterfaceOrientationsForWindow:(UIWindow*)w { (void)w; return 0x18; }
- (BOOL)isProtectedDataAvailable { return YES; }
- (NSString*)preferredContentSizeCategory { return @"UICTContentSizeCategoryL"; }
- (void)setApplicationSupportsShakeToEdit:(BOOL)b { (void)b; }
@end

// ===================================================================== display links
static NSMutableArray* g_links;
EMU_EXPORT void _uikit_register_display_link(id l) {
    if (!g_links) g_links = [[NSMutableArray alloc] init];
    if (![g_links containsObject:l]) [g_links addObject:l];
}
EMU_EXPORT void _uikit_unregister_display_link(id l) { [g_links removeObjectIdenticalTo:l]; }

// ===================================================================== touches
#define MAX_POINTERS 10
static UITouch* g_touch[MAX_POINTERS];

static UIWindow* key_window(void) { return [UIWindow performSelector:@selector(_keyWindow)]; }

// iPadOS asks the root view controller for prefersPointerLocked whenever it lays out. The
// game only calls setNeedsUpdateOfPrefersPointerLocked from its GCMouse moved handler, which
// runs only after the mouse is locked: without asking here the mouse never gets captured in
// the world (no mouse look). Minecraft's answer is a flag of its platform object (cheap).
// (no cache of the last value here: -setNeedsUpdateOfPrefersPointerLocked tells the emulator
// too, and a cache here would then miss the next change. The emulator ignores repeats.)
static void update_pointer_lock(void) {
    UIWindow* kw = key_window();
    id vc = kw ? [kw rootViewController] : nil;
    int on = vc && ((BOOL(*)(id, SEL))objc_msgSend)(vc, @selector(prefersPointerLocked));
    emu_set_pointer_locked(on);
}

static void deliver(UITouch* t, NSInteger phase) {
    t->_phase = phase;
    UIEvent* ev = [[[UIEvent alloc] init] autorelease];
    NSMutableSet* all = [NSMutableSet set];
    for (int i = 0; i < MAX_POINTERS; i++) if (g_touch[i]) [all addObject:g_touch[i]];
    ev->_touches = [all retain];
    ev->_time = t->_time;
    NSSet* s = [NSSet setWithObject:t];
    UIView* v = t->_view;
    if (getenv("EMU_TRACE_INPUT")) NSLog(@"[UIKit] touch phase %ld at %.0f,%.0f -> %@ (next %@)", (long)phase, t->_loc.x, t->_loc.y, v, [v nextResponder]);
    if (!v) return;
    switch (phase) {
    case 0: [v touchesBegan:s withEvent:ev]; break;
    case 1: [v touchesMoved:s withEvent:ev]; break;
    case 3: [v touchesEnded:s withEvent:ev]; break;
    default: [v touchesCancelled:s withEvent:ev]; break;
    }
}

static void pointer_event(const emu_event* e) {
    int pid = e->id;
    if (pid < 0 || pid >= MAX_POINTERS) return;
    CGFloat sc = _uikit_scale();
    CGPoint p = CGPointMake(e->x / sc, e->y / sc);
    UIWindow* w = key_window();
    if (!w) return;
    if (e->type == EMU_EV_POINTER_DOWN) {
        if (g_touch[pid]) { deliver(g_touch[pid], 4); [g_touch[pid] release]; g_touch[pid] = nil; }
        UITouch* t = [[UITouch alloc] init];
        t->_loc = t->_prev = p;
        t->_time = e->time;
        t->_window = w;
        t->_view = [w hitTest:p withEvent:nil];
        t->_taps = 1;
        g_touch[pid] = t;
        deliver(t, 0);
    } else if (e->type == EMU_EV_POINTER_MOVE) {
        UITouch* t = g_touch[pid];
        if (!t) return;
        if (t->_loc.x == p.x && t->_loc.y == p.y) return;
        t->_prev = t->_loc;
        t->_loc = p;
        t->_time = e->time;
        deliver(t, 1);
    } else if (e->type == EMU_EV_POINTER_UP) {
        UITouch* t = g_touch[pid];
        if (!t) return;
        t->_prev = t->_loc;
        t->_loc = p;
        t->_time = e->time;
        deliver(t, 3);
        g_touch[pid] = nil;
        [t release];
    }
}

// ===================================================================== lifecycle helpers
static void post(NSString* name) { [[NSNotificationCenter defaultCenter] postNotificationName:name object:g_app]; }
static void call_delegate(SEL s) {
    id d = [g_app delegate];
    if ([d respondsToSelector:s]) objc_msgSend(d, s, g_app);
}
static BOOL g_active = YES;
static void resign_active(void) {
    if (!g_active) return;
    g_active = NO;
    call_delegate(@selector(applicationWillResignActive:));
    post(UIApplicationWillResignActiveNotification);
}
static void become_active(void) {
    if (g_active) return;
    g_active = YES;
    call_delegate(@selector(applicationDidBecomeActive:));
    post(UIApplicationDidBecomeActiveNotification);
}
static void terminate(void) {
    resign_active();
    [g_app _setState:2];
    call_delegate(@selector(applicationDidEnterBackground:));
    post(UIApplicationDidEnterBackgroundNotification);
    call_delegate(@selector(applicationWillTerminate:));
    post(UIApplicationWillTerminateNotification);
    [[NSUserDefaults standardUserDefaults] synchronize];
    exit(0);
}

// Minecraft sets its render size and GUI size once, in -[minecraftpeViewControllerBase initView]
// (an iPhone's screen never changes size), so a resized / fullscreen window (or a phone's render
// size) is passed on the same way initView does it:
//   game = _minecraftGame (unique_ptr<IMinecraftGame>); w, h = [self width], [self height]
//   game->vtable[0x300 / 8](game, w, h); game->vtable[0x310 / 8](game, w, h)
// (MinecraftGame's render size and screen size; both queue the change to the game's thread).
// The platform's native size (+0x15c / +0x160 of _platform, from nativeWidth / nativeHeight) as well.
extern void* class_getInstanceVariable(Class c, const char* name);
extern ptrdiff_t ivar_getOffset(void* v);
static BOOL resize_game(void) {
    UIWindow* kw = key_window();
    id vc = kw ? [kw rootViewController] : nil;
    if (!vc || ![vc respondsToSelector:@selector(width)] || ![vc respondsToSelector:@selector(height)]) return NO;
    void* gv = class_getInstanceVariable([vc class], "_minecraftGame");
    void* pv = class_getInstanceVariable([vc class], "_platform");
    if (!gv) return NO;
    void* game = *(void**)((char*)vc + ivar_getOffset(gv));
    if (!game) return NO;  // (not started yet: initView will use the new size)
    // (the rendered pixels; the controller's width / height would use the scale it saw at start-up)
    int w, h;
    _uikit_pixel_size(&w, &h);
    if (h > w) { int t = w; w = h; h = t; }
    if (w <= 0 || h <= 0) return NO;
    void* platform = pv ? *(void**)((char*)vc + ivar_getOffset(pv)) : NULL;
    if (platform && [vc respondsToSelector:@selector(nativeWidth)] && [vc respondsToSelector:@selector(nativeHeight)]) {
        *(int*)((char*)platform + 0x15c) = ((int (*)(id, SEL))objc_msgSend)(vc, @selector(nativeWidth));
        *(int*)((char*)platform + 0x160) = ((int (*)(id, SEL))objc_msgSend)(vc, @selector(nativeHeight));
    }
    void** vt = *(void***)game;
    NSLog(@"[UIKit] game resized to %dx%d", w, h);
    ((void (*)(void*, int, int))vt[0x300 / 8])(game, w, h);
    ((void (*)(void*, int, int))vt[0x310 / 8])(game, w, h);
    return YES;
}

// (the windows follow the size, so that touches anywhere on the screen find their view; the
// game resizes its drawable itself in setRenderingSize - MTKView leaves it alone, see Metal.m)
static void apply_resize(int w, int h) {
    _uikit_set_pixel_size(w, h);
    CGSize s = _uikit_screen_points();
    for (UIWindow* win in [UIWindow performSelector:@selector(_windows)]) [win setFrame:CGRectMake(0, 0, s.width, s.height)];
    resize_game();
}

// A burst of size changes (dragging the window's border; a phone's Safari showing / hiding its
// bars) is passed on as one: once the size has not changed for 0.3 s.
static int g_resize_w, g_resize_h;
static double g_resize_at, g_resized_at = -100;
static void handle_resize(int w, int h) {
    if (w <= 0 || h <= 0) return;
    g_resize_w = w;
    g_resize_h = h;
    g_resize_at = _emu_now();
    if (getenv("EMU_TRACE_RESIZE")) NSLog(@"[UIKit] size change to %dx%d received", w, h);
}
static void apply_pending_resize(void) {
    if (!g_resize_w) return;
    double now = _emu_now();
    if (now - g_resize_at < 0.3 || now - g_resized_at < 0.3) return;
    int w = g_resize_w, h = g_resize_h;
    g_resize_w = g_resize_h = 0;
    g_resized_at = now;
    int cw, ch;
    _uikit_pixel_size(&cw, &ch);
    if (cw == w && ch == h) return;  // (back to the size the game has)
    apply_resize(w, h);
}

void _gc_mouse_position(float px, float py);
void _gc_mouse_moved(float dx, float dy);
void _gc_mouse_button(int button, int down);
void _gc_mouse_scroll(float dx, float dy);
void _gc_key(int vk, int down);
void _gc_disconnect_mouse(void);

// ===================================================================== bootstrap from the main nib
@interface _EmuMainNib : NSObject
@end
@implementation _EmuMainNib
+ (void)loadMainNib:(NSString*)nib into:(UIApplication*)app delegateClass:(NSString*)forced {
    CGSize s = _uikit_screen_points();
    NSString* delegateName = forced;
    NSString* vcName = nil;
    NSString* vcNib = nil;
    if ([nib isEqualToString:@"MainWindow"] && NSClassFromString(@"minecraftpeAppDelegate")) {
        if (!delegateName) delegateName = @"minecraftpeAppDelegate";
        // MainWindow.nib: the view controller is minecraftpeViewControllerImpl (1.2x), minecraftpeViewController before
        vcName = NSClassFromString(@"minecraftpeViewControllerImpl") ? @"minecraftpeViewControllerImpl" : @"minecraftpeViewController";
        vcNib = @"minecraftpeViewController";
    }
    if (!delegateName) {
        NSLog(@"[UIKit] main nib %@ is not known and no delegate class was given", nib);
        return;
    }
    UIWindow* window = [[UIWindow alloc] initWithFrame:CGRectMake(0, 0, s.width, s.height)];
    id delegate = [[NSClassFromString(delegateName) alloc] init];
    [app setDelegate:delegate];
    if ([delegate respondsToSelector:@selector(setWindow:)]) objc_msgSend(delegate, @selector(setWindow:), window);
    if (vcName) {
        id coder = [[NSClassFromString(@"_EmuNibCoder") alloc] init];
        objc_msgSend(coder, @selector(setNib:), vcNib);
        UIViewController* vc = [[NSClassFromString(vcName) alloc] initWithCoder:coder];
        [coder release];
        if ([delegate respondsToSelector:@selector(setViewController:)]) objc_msgSend(delegate, @selector(setViewController:), vc);
        [vc awakeFromNib];
    }
    [window awakeFromNib];
    if ([delegate respondsToSelector:@selector(awakeFromNib)]) [delegate awakeFromNib];
    // MainWindow.nib marks the window "visible at launch"
    [window makeKeyAndVisible];
}
@end

// EMU_TRACE_XAL=1: the trace of libHttpClient and the Xbox sign-in (Xal) in the log
extern void* dlsym(void* h, const char* name);
// The errors and warnings of libHttpClient / Xal always go to emulator.log (sign-in problems);
// EMU_TRACE_XAL=1: everything, on the console
void emu_log_level(int level, const char* msg);
static void xal_trace_cb(const char* area, int level, uint64_t thread, uint64_t ts, const char* msg) {
    (void)thread; (void)ts;
    // (level 3: the steps of the sign-in, without the ones repeated for every request)
    static const char* const kNoisy[] = {"GetDtoken", "LoadDeviceIdentity", "InitializeTokenStack", "GetXtoken", "TokenAndSignature",
                                         "GetTokenInternal", "Returning requested cache", "Handle ", "with CV", "UploadTelemetry",
                                         "to user ", "Telemetry"};
    BOOL noisy = NO;
    if (level == 3 && msg)
        for (unsigned i = 0; i < sizeof kNoisy / sizeof kNoisy[0] && !noisy; i++) noisy = strstr(msg, kNoisy[i]) != NULL;
    if (level <= 2 || (level == 3 && !noisy && area && !strcmp(area, "XAL"))) {
        char buf[1024];
        snprintf(buf, sizeof buf, "[%s] %s", area ? area : "?", msg ? msg : "");
        static char last[1024];  // (each message is reported twice)
        if (strcmp(buf, last) != 0) emu_log_level(level, buf);
        memcpy(last, buf, sizeof last);
    }
    if (getenv("EMU_TRACE_XAL")) NSLog(@"[XAL %s %d] %s", area ? area : "?", level, msg ? msg : "");
}
static void enable_xal_trace(void) {
    void (*set_level)(int) = (void (*)(int))dlsym((void*)-2, "HCSettingsSetTraceLevel");
    void (*set_cb)(void*) = (void (*)(void*))dlsym((void*)-2, "HCTraceSetClientCallback");
    if (set_level) set_level(getenv("EMU_TRACE_XAL") ? 5 : 3);  // HCTraceLevel_Verbose / _Important
    if (set_cb) set_cb((void*)xal_trace_cb);
}

// ===================================================================== UIApplicationMain
EMU_EXPORT int UIApplicationMain(int argc, char** argv, NSString* principal, NSString* delegateName) {
    (void)argc; (void)argv;
    void* pool = objc_autoreleasePoolPush();
    int w = 1280, h = 720;
    const char* sz = getenv("EMU_WINDOW_SIZE");
    if (sz) sscanf(sz, "%dx%d", &w, &h);
    _uikit_set_pixel_size(w, h);
    if (!emu_window_create(w, h)) {
        NSLog(@"[UIKit] could not create the host window");
        return 1;
    }
    emu_window_size(&w, &h);
    _uikit_set_pixel_size(w, h);
    Class pc = principal ? NSClassFromString(principal) : Nil;
    g_app = [[(pc ? pc : [UIApplication class]) alloc] init];
    NSDictionary* info = [[NSBundle mainBundle] infoDictionary];
    NSString* mainNib = [info objectForKey:@"NSMainNibFile"];
    if (mainNib) [_EmuMainNib loadMainNib:mainNib into:g_app delegateClass:delegateName];
    else if (delegateName) [g_app setDelegate:[[NSClassFromString(delegateName) alloc] init]];

    id d = [g_app delegate];
    enable_xal_trace();
    NSLog(@"[UIKit] launching with delegate %@", d);
    if ([d respondsToSelector:@selector(application:willFinishLaunchingWithOptions:)])
        objc_msgSend(d, @selector(application:willFinishLaunchingWithOptions:), g_app, nil);
    if ([d respondsToSelector:@selector(application:didFinishLaunchingWithOptions:)])
        objc_msgSend(d, @selector(application:didFinishLaunchingWithOptions:), g_app, nil);
    else if ([d respondsToSelector:@selector(applicationDidFinishLaunching:)])
        objc_msgSend(d, @selector(applicationDidFinishLaunching:), g_app);
    post(UIApplicationDidFinishLaunchingNotification);
    enable_xal_trace();
    UIWindow* kw = key_window();
    if (kw) [kw makeKeyAndVisible];
    call_delegate(@selector(applicationDidBecomeActive:));
    post(UIApplicationDidBecomeActiveNotification);
    {
        extern void _uikit_test_wkwebview(UIView* parent);  // (debug: EMU_TEST_WKWEBVIEW)
        _uikit_test_wkwebview(key_window());
    }
    objc_autoreleasePoolPop(pool);

    // ---- main loop ----
    emu_event evs[64];
    double stat_t = _emu_now();
    unsigned long frames = 0, iters = 0;
    const int loop_stats = getenv("EMU_LOOP_STATS") != NULL;
    for (;;) {
        iters++;
        if (loop_stats && _emu_now() - stat_t > 5) {
            NSLog(@"[UIKit] loop: %lu iterations, %lu display-link fires, %lu links", iters, frames, (unsigned long)[g_links count]);
            stat_t = _emu_now();
        }
        void* p = objc_autoreleasePoolPush();
        int n;
        while ((n = emu_poll_events(evs, 64)) > 0) {
            for (int i = 0; i < n; i++) {
                const emu_event* e = &evs[i];
                switch (e->type) {
                case EMU_EV_POINTER_DOWN:
                case EMU_EV_POINTER_MOVE:
                case EMU_EV_POINTER_UP: pointer_event(e); break;
                case EMU_EV_RESIZE: handle_resize((int)e->x, (int)e->y); break;
                case EMU_EV_CLOSE: terminate(); break;
                case EMU_EV_FOCUS: become_active(); break;
                case EMU_EV_BLUR: break;
                case EMU_EV_CHAR: _uikit_text_input_char(e->chr); break;
                case EMU_EV_KEY_DOWN:
                    _uikit_text_input_key(e->id);
                    if (!(e->mods & 0x100)) _gc_key(e->id, 1);  // (not the key repeat)
                    break;
                case EMU_EV_KEY_UP: _gc_key(e->id, 0); break;
                // the PC's mouse (GameController's GCMouse, UIPointerInteraction)
                case 12: _gc_mouse_moved(e->x, e->y); break;
                case 13:
                    if (e->id >= 1 && e->id <= 3) _gc_mouse_button(e->id, e->chr != 0);
                    else if (e->id == 4) {
                        // Minecraft's scroll handler reads the x value (> 0: wheel up, < 0: down) for the
                        // wheel: scroll bars of screens, the hotbar in the world
                        float v = (int32_t)e->chr > 0 ? 1.0f : -1.0f;
                        _gc_mouse_scroll(v, v);
                    }
                    break;
                case 14: _gc_mouse_position(e->x, e->y); break;
                case 15: if (e->id) _gc_disconnect_mouse(); break;  // a phone connected
                default: break;
                }
            }
        }
        _uikit_poll_image_picker();
        _emu_runloop_iteration(0);
        _uikit_layout_pass();
        update_pointer_lock();
        apply_pending_resize();
        double now = _emu_now();
        double next = now + 0.05;
        NSArray* links = [[g_links copy] autorelease];
        for (id l in links) {
            if (((BOOL(*)(id, SEL))objc_msgSend)(l, @selector(isPaused))) continue;
            double t = ((double (*)(id, SEL))objc_msgSend)(l, @selector(_emuNext));
            NSInteger fi = ((NSInteger(*)(id, SEL))objc_msgSend)(l, @selector(frameInterval));
            if (now + 0.0005 >= t) {
                double iv = emu_frame_interval();  // [window] max_fps in controls.ini
                double nt = t + fi * iv;
                if (nt < now) nt = now + fi * iv;
                ((void (*)(id, SEL, double))objc_msgSend)(l, @selector(_setEmuNext:), nt);
                ((void (*)(id, SEL, double))objc_msgSend)(l, @selector(_fire:), now);
                frames++;
                t = nt;
            }
            if (t < next) next = t;
        }
        objc_autoreleasePoolPop(p);
        double nt = _emu_runloop_next_timer();
        double wait = next - _emu_now();
        if (nt < wait) wait = nt;
        if (_dispatch_main_queue_pending()) wait = 0;
        if (wait > 0.0005) emu_wait_events(wait);
    }
    return 0;
}
