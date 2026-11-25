// GameController: the PC's keyboard and mouse as GCKeyboard / GCMouse (what iPadOS gives an
// app for a hardware keyboard and mouse); UIPointerInteraction for the cursor position.
// Minecraft (1.2x) switches to its keyboard/mouse mode and screens when these report input.
// No game controllers. StoreKit placeholders: no store.
#import <emu/UIKit.h>
#include <emu/host.h>
#include <emu/msg.h>

#include <stdlib.h>

extern void objc_setAssociatedObject(id o, const void* key, id v, uintptr_t policy);
extern id objc_getAssociatedObject(id o, const void* key);
#define OBJC_ASSOCIATION_RETAIN 01401
static int trace_input(void);

EMU_EXPORT NSString* const GCControllerDidConnectNotification = @"GCControllerDidConnectNotification";
EMU_EXPORT NSString* const GCControllerDidDisconnectNotification = @"GCControllerDidDisconnectNotification";
EMU_EXPORT NSString* const GCControllerDidBecomeCurrentNotification = @"GCControllerDidBecomeCurrentNotification";
EMU_EXPORT NSString* const GCControllerDidStopBeingCurrentNotification = @"GCControllerDidStopBeingCurrentNotification";
EMU_EXPORT NSString* const GCKeyboardDidConnectNotification = @"GCKeyboardDidConnectNotification";
EMU_EXPORT NSString* const GCKeyboardDidDisconnectNotification = @"GCKeyboardDidDisconnectNotification";
EMU_EXPORT NSString* const GCMouseDidConnectNotification = @"GCMouseDidConnectNotification";
EMU_EXPORT NSString* const GCMouseDidDisconnectNotification = @"GCMouseDidDisconnectNotification";
EMU_EXPORT NSString* const GCMouseDidBecomeCurrentNotification = @"GCMouseDidBecomeCurrentNotification";
EMU_EXPORT NSString* const GCMouseDidStopBeingCurrentNotification = @"GCMouseDidStopBeingCurrentNotification";
EMU_EXPORT NSString* const GCProductCategoryKeyboard = @"Keyboard";
EMU_EXPORT NSString* const GCProductCategoryMouse = @"Mouse";

// GCKeyCode (= USB HID keyboard usage)
#define KEY(name, v) EMU_EXPORT const NSInteger GCKeyCode##name = v
KEY(KeyA, 0x04); KEY(KeyB, 0x05); KEY(KeyC, 0x06); KEY(KeyD, 0x07); KEY(KeyE, 0x08); KEY(KeyF, 0x09); KEY(KeyG, 0x0A);
KEY(KeyH, 0x0B); KEY(KeyI, 0x0C); KEY(KeyJ, 0x0D); KEY(KeyK, 0x0E); KEY(KeyL, 0x0F); KEY(KeyM, 0x10); KEY(KeyN, 0x11);
KEY(KeyO, 0x12); KEY(KeyP, 0x13); KEY(KeyQ, 0x14); KEY(KeyR, 0x15); KEY(KeyS, 0x16); KEY(KeyT, 0x17); KEY(KeyU, 0x18);
KEY(KeyV, 0x19); KEY(KeyW, 0x1A); KEY(KeyX, 0x1B); KEY(KeyY, 0x1C); KEY(KeyZ, 0x1D);
KEY(One, 0x1E); KEY(Two, 0x1F); KEY(Three, 0x20); KEY(Four, 0x21); KEY(Five, 0x22); KEY(Six, 0x23); KEY(Seven, 0x24);
KEY(Eight, 0x25); KEY(Nine, 0x26); KEY(Zero, 0x27);
KEY(ReturnOrEnter, 0x28); KEY(Escape, 0x29); KEY(DeleteOrBackspace, 0x2A); KEY(Tab, 0x2B); KEY(Spacebar, 0x2C);
KEY(Hyphen, 0x2D); KEY(EqualSign, 0x2E); KEY(OpenBracket, 0x2F); KEY(CloseBracket, 0x30); KEY(Backslash, 0x31);
KEY(NonUSPound, 0x32); KEY(Semicolon, 0x33); KEY(Quote, 0x34); KEY(GraveAccentAndTilde, 0x35); KEY(Comma, 0x36);
KEY(Period, 0x37); KEY(Slash, 0x38); KEY(CapsLock, 0x39);
KEY(F1, 0x3A); KEY(F2, 0x3B); KEY(F3, 0x3C); KEY(F4, 0x3D); KEY(F5, 0x3E); KEY(F6, 0x3F); KEY(F7, 0x40); KEY(F8, 0x41);
KEY(F9, 0x42); KEY(F10, 0x43); KEY(F11, 0x44); KEY(F12, 0x45);
KEY(PrintScreen, 0x46); KEY(ScrollLock, 0x47); KEY(Pause, 0x48); KEY(Insert, 0x49); KEY(Home, 0x4A); KEY(PageUp, 0x4B);
KEY(DeleteForward, 0x4C); KEY(End, 0x4D); KEY(PageDown, 0x4E); KEY(RightArrow, 0x4F); KEY(LeftArrow, 0x50);
KEY(DownArrow, 0x51); KEY(UpArrow, 0x52);
KEY(KeypadNumLock, 0x53); KEY(KeypadSlash, 0x54); KEY(KeypadAsterisk, 0x55); KEY(KeypadHyphen, 0x56); KEY(KeypadPlus, 0x57);
KEY(KeypadEnter, 0x58); KEY(Keypad1, 0x59); KEY(Keypad2, 0x5A); KEY(Keypad3, 0x5B); KEY(Keypad4, 0x5C); KEY(Keypad5, 0x5D);
KEY(Keypad6, 0x5E); KEY(Keypad7, 0x5F); KEY(Keypad8, 0x60); KEY(Keypad9, 0x61); KEY(Keypad0, 0x62); KEY(KeypadPeriod, 0x63);
KEY(NonUSBackslash, 0x64); KEY(Application, 0x65); KEY(Power, 0x66); KEY(KeypadEqualSign, 0x67);
KEY(LeftControl, 0xE0); KEY(LeftShift, 0xE1); KEY(LeftAlt, 0xE2); KEY(LeftGUI, 0xE3);
KEY(RightControl, 0xE4); KEY(RightShift, 0xE5); KEY(RightAlt, 0xE6); KEY(RightGUI, 0xE7);
KEY(International1, 0x87); KEY(International2, 0x88); KEY(International3, 0x89); KEY(International4, 0x8A);
KEY(International5, 0x8B); KEY(LANG1, 0x90); KEY(LANG2, 0x91);

// Windows virtual key -> HID usage
static int vk_to_hid(int vk) {
    if (vk >= 'A' && vk <= 'Z') return 0x04 + (vk - 'A');
    if (vk >= '1' && vk <= '9') return 0x1E + (vk - '1');
    if (vk == '0') return 0x27;
    if (vk >= 0x70 && vk <= 0x7B) return 0x3A + (vk - 0x70);  // F1..F12
    if (vk >= 0x61 && vk <= 0x69) return 0x59 + (vk - 0x61);  // numpad 1..9
    switch (vk) {
    case 0x0D: return 0x28;  // Enter
    case 0x1B: return 0x29;  // Esc
    case 0x08: return 0x2A;  // Backspace
    case 0x09: return 0x2B;  // Tab
    case 0x20: return 0x2C;  // Space
    case 0xBD: return 0x2D;  // -
    case 0xBB: return 0x2E;  // = (US) / ; (JP)
    case 0xDB: return 0x2F;  // [
    case 0xDD: return 0x30;  // ]
    case 0xDC: return 0x31;  // backslash
    case 0xBA: return 0x33;  // ;
    case 0xDE: return 0x34;  // '
    case 0xC0: return 0x35;  // `
    case 0xBC: return 0x36;  // ,
    case 0xBE: return 0x37;  // .
    case 0xBF: return 0x38;  // /
    case 0x14: return 0x39;  // Caps Lock
    case 0x2C: return 0x46;  // Print Screen
    case 0x91: return 0x47;  // Scroll Lock
    case 0x13: return 0x48;  // Pause
    case 0x2D: return 0x49;  // Insert
    case 0x24: return 0x4A;  // Home
    case 0x21: return 0x4B;  // Page Up
    case 0x2E: return 0x4C;  // Delete
    case 0x23: return 0x4D;  // End
    case 0x22: return 0x4E;  // Page Down
    case 0x27: return 0x4F;  // Right
    case 0x25: return 0x50;  // Left
    case 0x28: return 0x51;  // Down
    case 0x26: return 0x52;  // Up
    case 0x90: return 0x53;  // Num Lock
    case 0x6F: return 0x54;  // numpad /
    case 0x6A: return 0x55;  // numpad *
    case 0x6D: return 0x56;  // numpad -
    case 0x6B: return 0x57;  // numpad +
    case 0x60: return 0x62;  // numpad 0
    case 0x6E: return 0x63;  // numpad .
    case 0xE2: return 0x64;  // non-US backslash (JP: _ ろ)
    case 0x5D: return 0x65;  // Application
    case 0x10: case 0xA0: return 0xE1;  // Shift
    case 0xA1: return 0xE5;
    case 0x11: case 0xA2: return 0xE0;  // Ctrl
    case 0xA3: return 0xE4;
    case 0x12: case 0xA4: return 0xE2;  // Alt
    case 0xA5: return 0xE6;
    case 0x5B: return 0xE3;  // Win
    case 0x5C: return 0xE7;
    case 0x1C: return 0x8A;  // 変換 (International4)
    case 0x1D: return 0x8B;  // 無変換 (International5)
    case 0xF2: case 0x15: return 0x88;  // かな (International2)
    case 0xF3: case 0xF4: case 0x19: return 0x35;  // 半角/全角 -> grave
    default: return 0;
    }
}

// ===================================================================== inputs
@interface GCControllerElement : NSObject {
@public
    id _collection;
}
@end
@implementation GCControllerElement
- (id)collection { return _collection; }
- (BOOL)isAnalog { return NO; }
- (BOOL)isBoundToSystemGesture { return NO; }
- (NSUInteger)preferredSystemGestureState { return 0; }
- (void)setPreferredSystemGestureState:(NSUInteger)s { (void)s; }
- (NSString*)localizedName { return nil; }
- (NSString*)sfSymbolsName { return nil; }
- (NSSet*)aliases { return [NSSet set]; }
@end

@interface GCControllerButtonInput : GCControllerElement {
@public
    float _value;
    BOOL _pressed;
    id _valueChanged, _pressedChanged, _touchedChanged;
}
@end
@implementation GCControllerButtonInput
- (void)dealloc { [_valueChanged release]; [_pressedChanged release]; [_touchedChanged release]; [super dealloc]; }
- (float)value { return _value; }
- (BOOL)isPressed { return _pressed; }
- (BOOL)isTouched { return _pressed; }
- (id)valueChangedHandler { return _valueChanged; }
- (void)setValueChangedHandler:(id)h {
    [_valueChanged release];
    _valueChanged = [h copy];
    if (trace_input()) NSLog(@"[GC] button %p valueChangedHandler %p", self, h);
}
- (id)pressedChangedHandler { return _pressedChanged; }
- (void)setPressedChangedHandler:(id)h { [_pressedChanged release]; _pressedChanged = [h copy]; }
- (id)touchedChangedHandler { return _touchedChanged; }
- (void)setTouchedChangedHandler:(id)h { [_touchedChanged release]; _touchedChanged = [h copy]; }
- (void)setValue:(float)v {
    BOOL p = v > 0.5f;
    BOOL changed = p != _pressed;
    _value = v;
    _pressed = p;
    if (_valueChanged) ((void (^)(id, float, BOOL))_valueChanged)(self, v, p);
    if (changed && _pressedChanged) ((void (^)(id, float, BOOL))_pressedChanged)(self, v, p);
    if (changed && _touchedChanged) ((void (^)(id, float, BOOL, BOOL))_touchedChanged)(self, v, p, p);
}
@end

@interface GCControllerAxisInput : GCControllerElement {
@public
    float _value;
    id _valueChanged;
}
@end
@implementation GCControllerAxisInput
- (void)dealloc { [_valueChanged release]; [super dealloc]; }
- (float)value { return _value; }
- (void)setValue:(float)v {
    _value = v;
    if (_valueChanged) ((void (^)(id, float))_valueChanged)(self, v);
}
- (id)valueChangedHandler { return _valueChanged; }
- (void)setValueChangedHandler:(id)h { [_valueChanged release]; _valueChanged = [h copy]; }
- (BOOL)isAnalog { return YES; }
@end

@interface GCControllerDirectionPad : GCControllerElement {
@public
    GCControllerAxisInput *_x, *_y;
    GCControllerButtonInput *_up, *_down, *_left, *_right;
    id _valueChanged;
}
@end
@implementation GCControllerDirectionPad
- (instancetype)init {
    _x = [[GCControllerAxisInput alloc] init];
    _y = [[GCControllerAxisInput alloc] init];
    _up = [[GCControllerButtonInput alloc] init];
    _down = [[GCControllerButtonInput alloc] init];
    _left = [[GCControllerButtonInput alloc] init];
    _right = [[GCControllerButtonInput alloc] init];
    return self;
}
- (void)dealloc {
    [_x release]; [_y release]; [_up release]; [_down release]; [_left release]; [_right release]; [_valueChanged release];
    [super dealloc];
}
- (id)xAxis { return _x; }
- (id)yAxis { return _y; }
- (id)up { return _up; }
- (id)down { return _down; }
- (id)left { return _left; }
- (id)right { return _right; }
- (id)valueChangedHandler { return _valueChanged; }
- (void)setValueChangedHandler:(id)h { [_valueChanged release]; _valueChanged = [h copy]; }
- (void)setValueForXAxis:(float)x yAxis:(float)y {
    _x->_value = x;
    _y->_value = y;
    if (_valueChanged) ((void (^)(id, float, float))_valueChanged)(self, x, y);
}
- (BOOL)isAnalog { return YES; }
@end
@interface GCDeviceCursor : GCControllerDirectionPad
@end
@implementation GCDeviceCursor
@end

// ---- mouse ----
@interface GCMouseInput : NSObject {
@public
    GCControllerButtonInput *_left, *_right, *_middle;
    GCDeviceCursor* _scroll;
    id _moved;
}
@end
@implementation GCMouseInput
- (instancetype)init {
    _left = [[GCControllerButtonInput alloc] init];
    _right = [[GCControllerButtonInput alloc] init];
    _middle = [[GCControllerButtonInput alloc] init];
    _scroll = [[GCDeviceCursor alloc] init];
    return self;
}
- (id)leftButton { return _left; }
- (id)rightButton { return _right; }
- (id)middleButton { return _middle; }
- (NSArray*)auxiliaryButtons { return [NSArray array]; }
- (id)scroll { return _scroll; }
- (id)mouseMovedHandler { return _moved; }
- (void)setMouseMovedHandler:(id)h {
    [_moved release];
    _moved = [h copy];
    if (trace_input()) NSLog(@"[GC] mouseMovedHandler %p", h);
}
- (id)device { return nil; }
- (double)lastEventTimestamp { return emu_time(); }
@end

@interface GCMouse : NSObject {
@public
    GCMouseInput* _input;
    id _queue;
}
@end
static GCMouse* g_mouse;
static BOOL g_mouse_connected;
@implementation GCMouse
+ (void)_create {
    if (!g_mouse) {
        g_mouse = [[GCMouse alloc] init];
        g_mouse->_input = [[GCMouseInput alloc] init];
    }
}
+ (GCMouse*)current { [self _create]; return g_mouse_connected ? g_mouse : nil; }
+ (NSArray*)mice { GCMouse* m = [self current]; return m ? [NSArray arrayWithObject:m] : [NSArray array]; }
- (id)mouseInput { return _input; }
- (NSString*)vendorName { return @"Mouse"; }
- (NSString*)productCategory { return GCProductCategoryMouse; }
- (id)handlerQueue { return _queue; }
- (void)setHandlerQueue:(id)q { _queue = q; }
- (id)physicalInputProfile { return nil; }
- (id)battery { return nil; }
@end

// ---- keyboard ----
@interface GCKeyboardInput : NSObject {
@public
    id _keyChanged;
    NSMutableDictionary* _buttons;
    int _down;
}
@end
@implementation GCKeyboardInput
- (instancetype)init { _buttons = [[NSMutableDictionary alloc] init]; return self; }
- (id)keyChangedHandler { return _keyChanged; }
- (void)setKeyChangedHandler:(id)h { [_keyChanged release]; _keyChanged = [h copy]; }
- (GCControllerButtonInput*)buttonForKeyCode:(NSInteger)code {
    NSNumber* k = [NSNumber numberWithInteger:code];
    GCControllerButtonInput* b = [_buttons objectForKey:k];
    if (!b) {
        b = [[[GCControllerButtonInput alloc] init] autorelease];
        [_buttons setObject:b forKey:k];
    }
    return b;
}
- (BOOL)isAnyKeyPressed { return _down > 0; }
- (id)device { return nil; }
- (double)lastEventTimestamp { return emu_time(); }
@end

@interface GCKeyboard : NSObject {
@public
    GCKeyboardInput* _input;
    id _queue;
}
@end
static GCKeyboard* g_keyboard;
static BOOL g_keyboard_connected;
@implementation GCKeyboard
+ (void)_create {
    if (!g_keyboard) {
        g_keyboard = [[GCKeyboard alloc] init];
        g_keyboard->_input = [[GCKeyboardInput alloc] init];
    }
}
+ (GCKeyboard*)coalescedKeyboard { [self _create]; return g_keyboard_connected ? g_keyboard : nil; }
- (id)keyboardInput { return _input; }
- (NSString*)vendorName { return @"Keyboard"; }
- (NSString*)productCategory { return GCProductCategoryKeyboard; }
- (id)handlerQueue { return _queue; }
- (void)setHandlerQueue:(id)q { _queue = q; }
- (id)physicalInputProfile { return nil; }
@end

// ---- connection: on the first input from the PC (not while a phone drives the game) ----
static int trace_input(void) { static int t = -1; if (t < 0) t = getenv("EMU_TRACE_INPUT") != NULL; return t; }
static void connect_mouse(void) {
    if (g_mouse_connected) return;
    if (trace_input()) NSLog(@"[GC] mouse connected");
    [GCMouse _create];
    g_mouse_connected = YES;
    [[NSNotificationCenter defaultCenter] postNotificationName:GCMouseDidConnectNotification object:g_mouse];
    [[NSNotificationCenter defaultCenter] postNotificationName:GCMouseDidBecomeCurrentNotification object:g_mouse];
}
static void connect_keyboard(void) {
    if (g_keyboard_connected) return;
    [GCKeyboard _create];
    g_keyboard_connected = YES;
    [[NSNotificationCenter defaultCenter] postNotificationName:GCKeyboardDidConnectNotification object:g_keyboard];
}

// ===================================================================== UIPointerInteraction
@interface UIPointerRegion : NSObject {
@public
    CGRect _rect;
    id _identifier;
}
@end
@implementation UIPointerRegion
+ (instancetype)regionWithRect:(CGRect)r identifier:(id)i {
    UIPointerRegion* p = [[[self alloc] init] autorelease];
    p->_rect = r;
    p->_identifier = [i retain];
    return p;
}
- (void)dealloc { [_identifier release]; [super dealloc]; }
- (CGRect)rect { return _rect; }
- (id)identifier { return _identifier; }
- (void)setLatchingAxes:(NSUInteger)a { (void)a; }
@end

@interface UIPointerRegionRequest : NSObject {
@public
    CGPoint _location;
    NSInteger _modifiers;
}
@end
@implementation UIPointerRegionRequest
- (CGPoint)location { return _location; }
- (NSInteger)modifiers { return _modifiers; }
@end

@interface UIPointerInteraction : NSObject {
@public
    id _delegate;
    UIView* _view;
    BOOL _enabled;
}
@end
// (not retained: an interaction removes itself when it goes away)
#define MAX_PI 32
static UIPointerInteraction* g_pi[MAX_PI];
static int g_npi;
@implementation UIPointerInteraction
- (instancetype)initWithDelegate:(id)d {
    _delegate = d;
    _enabled = YES;
    if (g_npi < MAX_PI) g_pi[g_npi++] = self;
    return self;
}
- (void)dealloc {
    for (int k = 0; k < g_npi; k++)
        if (g_pi[k] == self) { g_pi[k] = g_pi[--g_npi]; break; }
    [super dealloc];
}
- (id)delegate { return _delegate; }
- (UIView*)view { return _view; }
- (void)willMoveToView:(UIView*)v { (void)v; }
- (void)didMoveToView:(UIView*)v {
    _view = v;
    if (trace_input()) NSLog(@"[GC] pointer interaction on %@", v);
}
- (BOOL)isEnabled { return _enabled; }
- (void)setEnabled:(BOOL)e { _enabled = e; }
- (void)invalidate {}
@end

@interface UIView (EmuInteractions)
@end
@implementation UIView (EmuInteractions)
- (void)addInteraction:(id)i {
    if ([i respondsToSelector:@selector(didMoveToView:)]) [i didMoveToView:self];
    NSArray* cur = objc_getAssociatedObject(self, "emu_interactions");
    NSArray* now = cur ? [cur arrayByAddingObject:i] : [NSArray arrayWithObject:i];
    objc_setAssociatedObject(self, "emu_interactions", now, OBJC_ASSOCIATION_RETAIN);
}
- (void)removeInteraction:(id)i {
    NSMutableArray* a = [[objc_getAssociatedObject(self, "emu_interactions") mutableCopy] autorelease];
    [a removeObjectIdenticalTo:i];
    objc_setAssociatedObject(self, "emu_interactions", a, OBJC_ASSOCIATION_RETAIN);
    if ([i respondsToSelector:@selector(didMoveToView:)]) [i didMoveToView:nil];
}
- (NSArray*)interactions {
    NSArray* a = objc_getAssociatedObject(self, "emu_interactions");
    return a ? a : [NSArray array];
}
@end

// ===================================================================== events from the emulator (UIApplication.m)
EMU_EXPORT void _gc_mouse_position(float px, float py) {
    connect_mouse();
    if (trace_input()) NSLog(@"[GC] position %.0f,%.0f (%d interactions)", px, py, g_npi);
    CGFloat sc = _uikit_scale();
    CGPoint p = CGPointMake(px / sc, py / sc);
    for (int k = 0; k < g_npi; k++) {
        UIPointerInteraction* i = g_pi[k];
        if (!i->_enabled || !i->_view || ![i->_delegate respondsToSelector:@selector(pointerInteraction:regionForRequest:defaultRegion:)]) continue;
        UIPointerRegionRequest* r = [[UIPointerRegionRequest alloc] init];
        r->_location = [i->_view convertPoint:p fromView:nil];
        UIPointerRegion* def = [UIPointerRegion regionWithRect:[i->_view bounds] identifier:nil];
        ((id(*)(id, SEL, id, id, id))objc_msgSend)(i->_delegate, @selector(pointerInteraction:regionForRequest:defaultRegion:), i, r, def);
        [r release];
    }
}
EMU_EXPORT void _gc_mouse_moved(float dx, float dy) {
    connect_mouse();
    GCMouseInput* m = g_mouse->_input;
    if (trace_input()) NSLog(@"[GC] moved %.0f,%.0f handler %p", dx, dy, m->_moved);
    // GCMouse: y grows upwards
    if (m->_moved) ((void (^)(id, float, float))m->_moved)(m, dx, -dy);
}
EMU_EXPORT void _gc_mouse_button(int button, int down) {
    connect_mouse();
    if (trace_input()) NSLog(@"[GC] button %d %d (handler %p)", button, down, (button == 1 ? g_mouse->_input->_left : g_mouse->_input->_right)->_valueChanged);
    GCMouseInput* m = g_mouse->_input;
    GCControllerButtonInput* b = button == 1 ? m->_left : button == 2 ? m->_right : m->_middle;
    [b setValue:down ? 1.0f : 0.0f];
}
EMU_EXPORT void _gc_mouse_scroll(float dx, float dy) {
    connect_mouse();
    [g_mouse->_input->_scroll setValueForXAxis:dx yAxis:dy];
}
EMU_EXPORT void _gc_key(int vk, int down) {
    int code = vk_to_hid(vk);
    if (!code) return;
    connect_keyboard();
    GCKeyboardInput* k = g_keyboard->_input;
    GCControllerButtonInput* b = [k buttonForKeyCode:code];
    if ((b->_pressed != 0) == (down != 0)) return;  // (no key repeat)
    k->_down += down ? 1 : -1;
    if (k->_down < 0) k->_down = 0;
    [b setValue:down ? 1.0f : 0.0f];
    if (trace_input()) NSLog(@"[GC] key %d (hid %d) %d handler %p", vk, code, down, k->_keyChanged);
    if (k->_keyChanged) ((void (^)(id, id, NSInteger, BOOL))k->_keyChanged)(k, b, code, down != 0);
}
// a phone took over (touch input): the PC's mouse goes away so that the game shows its touch controls
EMU_EXPORT void _gc_disconnect_mouse(void) {
    if (!g_mouse_connected) return;
    g_mouse_connected = NO;
    [[NSNotificationCenter defaultCenter] postNotificationName:GCMouseDidDisconnectNotification object:g_mouse];
}

// ===================================================================== GCController (none)
@interface GCController : NSObject
@end
@implementation GCController
+ (NSArray*)controllers { return [NSArray array]; }
+ (id)current { return nil; }
+ (BOOL)shouldMonitorBackgroundEvents { return NO; }
+ (void)setShouldMonitorBackgroundEvents:(BOOL)b { (void)b; }
+ (void)startWirelessControllerDiscoveryWithCompletionHandler:(void (^)(void))h { if (h) h(); }
+ (void)stopWirelessControllerDiscovery {}
- (id)extendedGamepad { return nil; }
- (id)gamepad { return nil; }
- (id)microGamepad { return nil; }
- (NSString*)vendorName { return @""; }
- (BOOL)isAttachedToDevice { return NO; }
- (NSInteger)playerIndex { return -1; }
- (void)setPlayerIndex:(NSInteger)i { (void)i; }
- (void)setControllerPausedHandler:(id)h { (void)h; }
@end

@interface SKPaymentQueue : NSObject
@end
@implementation SKPaymentQueue
+ (instancetype)defaultQueue { static SKPaymentQueue* q; if (!q) q = [[SKPaymentQueue alloc] init]; return q; }
+ (BOOL)canMakePayments { return NO; }
- (void)addTransactionObserver:(id)o { (void)o; }
- (void)removeTransactionObserver:(id)o { (void)o; }
- (void)addPayment:(id)p { (void)p; }
- (void)restoreCompletedTransactions {}
- (void)finishTransaction:(id)t { (void)t; }
- (NSArray*)transactions { return [NSArray array]; }
@end
@interface SKRequest : NSObject {
@public
    id _delegate;
}
@end
@implementation SKRequest
- (void)setDelegate:(id)d { _delegate = d; }
- (id)delegate { return _delegate; }
- (void)start {
    if ([_delegate respondsToSelector:@selector(request:didFailWithError:)])
        ((void (*)(id, SEL, id, id))objc_msgSend)(_delegate, @selector(request:didFailWithError:), self,
                                                  [NSError errorWithDomain:@"SKErrorDomain" code:0 userInfo:nil]);
}
- (void)cancel {}
@end
@interface SKProductsRequest : SKRequest
@end
@implementation SKProductsRequest
- (instancetype)initWithProductIdentifiers:(NSSet*)s { (void)s; return self; }
@end
@interface SKReceiptRefreshRequest : SKRequest
@end
@implementation SKReceiptRefreshRequest
- (instancetype)initWithReceiptProperties:(NSDictionary*)p { (void)p; return self; }
@end
