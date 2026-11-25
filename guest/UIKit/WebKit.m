// WebKit: WKWebView and the classes around it. The Xbox sign-in shows the second step of a
// sign-in (the Xbox profile / consent pages, XALWebKitViewControllerIos) in a WKWebView.
//
// The page is shown in a window of the emulator (WebView2, iOS MCPE(v1.26.62) Emulator/web_view.cpp).
// Its events are polled on the main thread and passed to the navigation delegate like WebKit
// does: decidePolicyForNavigationAction (http(s) pages are already loading; a "cancel"
// answer closes the window), didStartProvisionalNavigation, didCommit / didFinishNavigation,
// didFailProvisionalNavigation. Closing the window is the user's cancel (the controller's
// cancel: action, like the Cancel button of its iOS screen).
#import <emu/UIKit.h>
#include <emu/host.h>
#include <emu/msg.h>

#include <dispatch/dispatch.h>
#include <stdlib.h>
#include <string.h>

uint64_t emu_webview_open(const char* url, const char* headers);
int emu_webview_poll(uint64_t id, int* kind, int* arg, char* out, uint64_t cap);
void emu_webview_eval(uint64_t id, int token, const char* script);
void emu_webview_close(uint64_t id);
extern void* _Block_copy(const void*);

// (Foundation classes this file uses, declared here: they are implemented in Foundation)
@interface NSURLRequest : NSObject
+ (instancetype)requestWithURL:(NSURL*)u;
- (NSURL*)URL;
- (NSDictionary*)allHTTPHeaderFields;
@end

EMU_EXPORT NSString* const WKErrorDomain = @"WKErrorDomain";
EMU_EXPORT NSString* const WKWebsiteDataTypeCookies = @"WKWebsiteDataTypeCookies";
EMU_EXPORT NSString* const WKWebsiteDataTypeLocalStorage = @"WKWebsiteDataTypeLocalStorage";
EMU_EXPORT NSString* const WKWebsiteDataTypeSessionStorage = @"WKWebsiteDataTypeSessionStorage";
EMU_EXPORT NSString* const WKWebsiteDataTypeDiskCache = @"WKWebsiteDataTypeDiskCache";
EMU_EXPORT NSString* const WKWebsiteDataTypeMemoryCache = @"WKWebsiteDataTypeMemoryCache";
EMU_EXPORT NSString* const WKWebsiteDataTypeIndexedDBDatabases = @"WKWebsiteDataTypeIndexedDBDatabases";
EMU_EXPORT NSString* const WKWebsiteDataTypeWebSQLDatabases = @"WKWebsiteDataTypeWebSQLDatabases";

// ===================================================================== small value classes
@interface WKFrameInfo : NSObject {
@public
    NSURLRequest* _req;
}
@end
@implementation WKFrameInfo
- (void)dealloc { [_req release]; [super dealloc]; }
- (BOOL)isMainFrame { return YES; }
- (NSURLRequest*)request { return _req; }
- (id)securityOrigin { return nil; }
- (id)webView { return nil; }
@end

@interface WKNavigationAction : NSObject {
@public
    NSURLRequest* _req;
    WKFrameInfo* _frame;
}
@end
@implementation WKNavigationAction
- (void)dealloc { [_req release]; [_frame release]; [super dealloc]; }
- (NSURLRequest*)request { return _req; }
- (WKFrameInfo*)targetFrame { return _frame; }
- (WKFrameInfo*)sourceFrame { return _frame; }
- (NSInteger)navigationType { return -1; }  // WKNavigationTypeOther
- (NSInteger)buttonNumber { return 0; }
- (NSInteger)modifierFlags { return 0; }
- (BOOL)shouldPerformDownload { return NO; }
@end

@interface WKNavigation : NSObject
@end
@implementation WKNavigation
- (id)effectiveContentMode { return nil; }
@end

@interface WKWebpagePreferences : NSObject
@end
@implementation WKWebpagePreferences
- (BOOL)allowsContentJavaScript { return YES; }
- (void)setAllowsContentJavaScript:(BOOL)b { (void)b; }
- (NSInteger)preferredContentMode { return 0; }
- (void)setPreferredContentMode:(NSInteger)m { (void)m; }
@end

@interface WKPreferences : NSObject
@end
@implementation WKPreferences
- (BOOL)javaScriptEnabled { return YES; }
- (void)setJavaScriptEnabled:(BOOL)b { (void)b; }
- (BOOL)javaScriptCanOpenWindowsAutomatically { return NO; }
- (void)setJavaScriptCanOpenWindowsAutomatically:(BOOL)b { (void)b; }
- (void)setMinimumFontSize:(double)s { (void)s; }
- (void)setFraudulentWebsiteWarningEnabled:(BOOL)b { (void)b; }
- (void)setValue:(id)v forKey:(NSString*)k { (void)v; (void)k; }
@end

@interface WKUserContentController : NSObject
@end
@implementation WKUserContentController
- (void)addUserScript:(id)s { (void)s; }
- (void)removeAllUserScripts {}
- (void)addScriptMessageHandler:(id)h name:(NSString*)n { (void)h; (void)n; }
- (void)removeScriptMessageHandlerForName:(NSString*)n { (void)n; }
- (void)removeAllScriptMessageHandlers {}
- (NSArray*)userScripts { return [NSArray array]; }
@end

@interface WKUserScript : NSObject
@end
@implementation WKUserScript
- (instancetype)initWithSource:(NSString*)s injectionTime:(NSInteger)t forMainFrameOnly:(BOOL)m { (void)s; (void)t; (void)m; return self; }
@end

@interface WKProcessPool : NSObject
@end
@implementation WKProcessPool
@end

@interface WKHTTPCookieStore : NSObject
@end
@implementation WKHTTPCookieStore
- (void)getAllCookies:(void (^)(NSArray*))h { if (h) h([NSArray array]); }
- (void)setCookie:(id)c completionHandler:(void (^)(void))h { (void)c; if (h) h(); }
- (void)deleteCookie:(id)c completionHandler:(void (^)(void))h { (void)c; if (h) h(); }
- (void)addObserver:(id)o { (void)o; }
- (void)removeObserver:(id)o { (void)o; }
@end

// (the web view's data lives in the emulator's WebView2 profile: nothing to list or remove here)
@interface WKWebsiteDataStore : NSObject
@end
@implementation WKWebsiteDataStore
+ (instancetype)defaultDataStore { static WKWebsiteDataStore* s; if (!s) s = [[WKWebsiteDataStore alloc] init]; return s; }
+ (instancetype)nonPersistentDataStore { return [[[WKWebsiteDataStore alloc] init] autorelease]; }
+ (NSSet*)allWebsiteDataTypes {
    return [NSSet setWithObjects:WKWebsiteDataTypeCookies, WKWebsiteDataTypeLocalStorage, WKWebsiteDataTypeSessionStorage, WKWebsiteDataTypeDiskCache,
                                 WKWebsiteDataTypeMemoryCache, nil];
}
- (BOOL)isPersistent { return YES; }
- (WKHTTPCookieStore*)httpCookieStore { return [[[WKHTTPCookieStore alloc] init] autorelease]; }
- (void)fetchDataRecordsOfTypes:(NSSet*)t completionHandler:(void (^)(NSArray*))h {
    (void)t;
    if (!h) return;
    void (^c)(NSArray*) = (void (^)(NSArray*))_Block_copy((const void*)h);
    dispatch_async(dispatch_get_main_queue(), ^{ c([NSArray array]); });
}
- (void)removeDataOfTypes:(NSSet*)t forDataRecords:(NSArray*)r completionHandler:(void (^)(void))h {
    (void)t; (void)r;
    if (!h) return;
    void (^c)(void) = (void (^)(void))_Block_copy((const void*)h);
    dispatch_async(dispatch_get_main_queue(), ^{ c(); });
}
- (void)removeDataOfTypes:(NSSet*)t modifiedSince:(NSDate*)d completionHandler:(void (^)(void))h {
    (void)d;
    [self removeDataOfTypes:t forDataRecords:nil completionHandler:h];
}
@end

@interface WKWebViewConfiguration : NSObject <NSCopying> {
@public
    WKWebsiteDataStore* _store;
    WKPreferences* _prefs;
    WKUserContentController* _content;
    WKProcessPool* _pool;
    WKWebpagePreferences* _pagePrefs;
    NSString* _appName;
}
@end
@implementation WKWebViewConfiguration
- (instancetype)init {
    _store = [[WKWebsiteDataStore defaultDataStore] retain];
    _prefs = [[WKPreferences alloc] init];
    _content = [[WKUserContentController alloc] init];
    _pool = [[WKProcessPool alloc] init];
    _pagePrefs = [[WKWebpagePreferences alloc] init];
    return self;
}
- (void)dealloc { [_store release]; [_prefs release]; [_content release]; [_pool release]; [_pagePrefs release]; [_appName release]; [super dealloc]; }
- (id)copyWithZone:(NSZone*)z { (void)z; return [self retain]; }
- (WKWebsiteDataStore*)websiteDataStore { return _store; }
- (void)setWebsiteDataStore:(WKWebsiteDataStore*)s { [_store release]; _store = [s retain]; }
- (WKPreferences*)preferences { return _prefs; }
- (void)setPreferences:(WKPreferences*)p { [_prefs release]; _prefs = [p retain]; }
- (WKUserContentController*)userContentController { return _content; }
- (void)setUserContentController:(WKUserContentController*)c { [_content release]; _content = [c retain]; }
- (WKProcessPool*)processPool { return _pool; }
- (void)setProcessPool:(WKProcessPool*)p { [_pool release]; _pool = [p retain]; }
- (WKWebpagePreferences*)defaultWebpagePreferences { return _pagePrefs; }
- (void)setDefaultWebpagePreferences:(WKWebpagePreferences*)p { [_pagePrefs release]; _pagePrefs = [p retain]; }
- (NSString*)applicationNameForUserAgent { return _appName; }
- (void)setApplicationNameForUserAgent:(NSString*)n { [_appName release]; _appName = [n copy]; }
- (void)setAllowsInlineMediaPlayback:(BOOL)b { (void)b; }
- (void)setMediaTypesRequiringUserActionForPlayback:(NSUInteger)t { (void)t; }
- (void)setSuppressesIncrementalRendering:(BOOL)b { (void)b; }
- (void)setDataDetectorTypes:(NSUInteger)t { (void)t; }
- (void)setLimitsNavigationsToAppBoundDomains:(BOOL)b { (void)b; }
- (void)setUpgradeKnownHostsToHTTPS:(BOOL)b { (void)b; }
@end

// ===================================================================== WKWebView
@interface WKWebView : UIView {
@public
    WKWebViewConfiguration* _config;
    id _navDelegate, _uiDelegate;
    uint64_t _host;   // the emulator's window (0: none)
    NSURL* _url;
    NSString* _title;
    NSString* _agent;
    BOOL _loading, _polling, _shown;
    double _lostWindowAt;
    WKNavigation* _nav;   // the navigation in progress
    NSMutableDictionary* _scripts;  // token -> completion handler
    int _nextToken;
}
@end

static NSString* header_lines(NSURLRequest* r) {
    NSMutableString* s = [NSMutableString string];
    NSDictionary* h = [r allHTTPHeaderFields];
    for (NSString* k in h) [s appendFormat:@"%@: %@\r\n", k, [h objectForKey:k]];
    return s;
}

@implementation WKWebView
- (instancetype)initWithFrame:(CGRect)f configuration:(WKWebViewConfiguration*)c {
    if ((self = [super initWithFrame:f])) {
        _config = c ? [c retain] : [[WKWebViewConfiguration alloc] init];
        _scripts = [[NSMutableDictionary alloc] init];
    }
    return self;
}
- (instancetype)initWithFrame:(CGRect)f { return [self initWithFrame:f configuration:nil]; }
- (instancetype)initWithCoder:(id)c { (void)c; return [self initWithFrame:CGRectMake(0, 0, 0, 0) configuration:nil]; }
- (void)dealloc {
    [self _closeHost];
    [_config release]; [_url release]; [_title release]; [_agent release]; [_nav release]; [_scripts release];
    [super dealloc];
}
- (WKWebViewConfiguration*)configuration { return _config; }
- (id)navigationDelegate { return _navDelegate; }
- (void)setNavigationDelegate:(id)d { _navDelegate = d; }  // (weak, like WebKit)
- (id)UIDelegate { return _uiDelegate; }
- (void)setUIDelegate:(id)d { _uiDelegate = d; }
- (NSURL*)URL { return _url; }
- (NSString*)title { return _title; }
- (BOOL)isLoading { return _loading; }
- (double)estimatedProgress { return _loading ? 0.5 : 1.0; }
- (BOOL)hasOnlySecureContent { return YES; }
- (BOOL)canGoBack { return NO; }
- (BOOL)canGoForward { return NO; }
- (id)goBack { return nil; }
- (id)goForward { return nil; }
- (id)reload { return nil; }
- (void)stopLoading {}
- (id)backForwardList { return nil; }
- (UIView*)scrollView { return nil; }
- (NSString*)customUserAgent { return _agent; }
- (void)setCustomUserAgent:(NSString*)a { [_agent release]; _agent = [a copy]; }
- (void)setAllowsBackForwardNavigationGestures:(BOOL)b { (void)b; }
- (void)setAllowsLinkPreview:(BOOL)b { (void)b; }
- (void)setInspectable:(BOOL)b { (void)b; }
- (void)setOpaque:(BOOL)b { (void)b; }

- (void)_closeHost {
    if (!_host) return;
    emu_webview_close(_host);
    _host = 0;
    _loading = NO;
}

- (id)loadRequest:(NSURLRequest*)r {
    if (![r URL]) return nil;
    [self _closeHost];
    [_url release];
    _url = [[r URL] retain];
    NSLog(@"[WebKit] loading %@ in a window", [[r URL] host]);
    _host = emu_webview_open([[[r URL] absoluteString] UTF8String], [header_lines(r) UTF8String]);
    _loading = YES;
    _shown = NO;
    _lostWindowAt = 0;
    [self _startPolling];
    return [[[WKNavigation alloc] init] autorelease];
}
- (id)loadHTMLString:(NSString*)s baseURL:(NSURL*)u {
    (void)u;
    NSString* data = [NSString stringWithFormat:@"data:text/html;charset=utf-8,%@",
                                                [s stringByAddingPercentEncodingWithAllowedCharacters:[NSCharacterSet URLQueryAllowedCharacterSet]]];
    return [self loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:data]]];
}
- (id)loadFileURL:(NSURL*)u allowingReadAccessToURL:(NSURL*)a { (void)a; return [self loadRequest:[NSURLRequest requestWithURL:u]]; }

- (void)evaluateJavaScript:(NSString*)js completionHandler:(void (^)(id, NSError*))h {
    if (!_host) {
        if (h) h(nil, [NSError errorWithDomain:WKErrorDomain code:1 userInfo:nil]);
        return;
    }
    int token = ++_nextToken;
    if (h) [_scripts setObject:(id)_Block_copy((const void*)h) forKey:[NSNumber numberWithInt:token]];  // (one reference: the dictionary's is extra, leaked)
    emu_webview_eval(_host, token, [js UTF8String]);
    [self _startPolling];
}
- (void)evaluateJavaScript:(NSString*)js inFrame:(id)f inContentWorld:(id)w completionHandler:(void (^)(id, NSError*))h {
    (void)f; (void)w;
    [self evaluateJavaScript:js completionHandler:h];
}

- (void)_startPolling {
    if (_polling) return;
    _polling = YES;
    [self retain];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        self->_polling = NO;
        [self _poll];
        [self release];
    });
}

// asks the navigation delegate about a navigation; YES = allowed
- (BOOL)_decide:(NSURL*)u {
    id d = _navDelegate;
    WKNavigationAction* a = [[[WKNavigationAction alloc] init] autorelease];
    a->_req = [[NSURLRequest requestWithURL:u] retain];
    a->_frame = [[WKFrameInfo alloc] init];
    a->_frame->_req = [a->_req retain];
    // (the answer goes through a heap cell, not a __block variable)
    NSInteger* policy = (NSInteger*)malloc(sizeof(NSInteger));
    *policy = 1;  // WKNavigationActionPolicyAllow
    if ([d respondsToSelector:@selector(webView:decidePolicyForNavigationAction:preferences:decisionHandler:)]) {
        WKWebpagePreferences* p = [[[WKWebpagePreferences alloc] init] autorelease];
        ((void (*)(id, SEL, id, id, id, id))objc_msgSend)(d, @selector(webView:decidePolicyForNavigationAction:preferences:decisionHandler:), self, a, p,
                                                       (id)_Block_copy((const void*)^(NSInteger pol, id prefs) { (void)prefs; *policy = pol; }));
    } else if ([d respondsToSelector:@selector(webView:decidePolicyForNavigationAction:decisionHandler:)]) {
        ((void (*)(id, SEL, id, id, id))objc_msgSend)(d, @selector(webView:decidePolicyForNavigationAction:decisionHandler:), self, a,
                                                   (id)_Block_copy((const void*)^(NSInteger pol) { *policy = pol; }));
    }
    NSInteger r = *policy;
    // (the handler and its cell are not freed: a delegate may keep the handler and call it later)
    return r != 0;
}

- (void)_event:(int)kind arg:(int)arg text:(const char*)text {
    id d = _navDelegate;
    if (kind == 1) {  // a navigation starts
        NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:text]];
        if (!u) return;
        [self retain];
        BOOL allowed = [self _decide:u];
        if (!_host) { [self release]; return; }  // (finished by the delegate)
        if (!allowed) {
            // the delegate took the URL (the end of the flow): the page is not shown any further
            NSLog(@"[WebKit] navigation to %@ taken by the app", [u host]);
            [self release];
            return;
        }
        [_url release];
        _url = [u retain];
        _loading = YES;
        [_nav release];
        _nav = [[WKNavigation alloc] init];
        if ([d respondsToSelector:@selector(webView:didStartProvisionalNavigation:)])
            objc_msgSend(d, @selector(webView:didStartProvisionalNavigation:), self, _nav);
        [self release];
    } else if (kind == 2) {  // a navigation ended
        if (arg == -1) return;  // (stopped: the app's own URL, already reported)
        _loading = NO;
        WKNavigation* n = _nav ? [[_nav retain] autorelease] : [[[WKNavigation alloc] init] autorelease];
        [self retain];
        if (arg == 0) {
            if ([d respondsToSelector:@selector(webView:didCommitNavigation:)]) objc_msgSend(d, @selector(webView:didCommitNavigation:), self, n);
            if ([d respondsToSelector:@selector(webView:didFinishNavigation:)]) objc_msgSend(d, @selector(webView:didFinishNavigation:), self, n);
        } else {
            NSString* u = [NSString stringWithUTF8String:text];
            NSError* e = [NSError errorWithDomain:@"NSURLErrorDomain" code:-1004
                                         userInfo:[NSDictionary dictionaryWithObjectsAndKeys:@"The page could not be loaded.", @"NSLocalizedDescription",
                                                                                             u, @"NSErrorFailingURLStringKey", nil]];
            NSLog(@"[WebKit] navigation failed (WebView2 status %d)", arg - 1);
            if ([d respondsToSelector:@selector(webView:didFailProvisionalNavigation:withError:)])
                objc_msgSend(d, @selector(webView:didFailProvisionalNavigation:withError:), self, n, e);
        }
        [self release];
    } else if (kind == 3) {  // the user closed the window: the Cancel button of the iOS screen
        _host = 0;
        _loading = NO;
        NSLog(@"[WebKit] window closed by the user");
        if ([d respondsToSelector:@selector(cancel:)]) objc_msgSend(d, @selector(cancel:), nil);
        else if ([d respondsToSelector:@selector(cancel)]) objc_msgSend(d, @selector(cancel));
        else if ([_uiDelegate respondsToSelector:@selector(webViewDidClose:)]) objc_msgSend(_uiDelegate, @selector(webViewDidClose:), self);
    } else if (kind == 4) {  // a script's result
        NSNumber* k = [NSNumber numberWithInt:arg];
        void (^h)(id, NSError*) = [[[_scripts objectForKey:k] retain] autorelease];
        [_scripts removeObjectForKey:k];
        if (!h) return;
        if (text[0] == 1) { h(nil, [NSError errorWithDomain:WKErrorDomain code:4 userInfo:nil]); return; }
        NSData* json = [NSData dataWithBytes:text length:strlen(text)];
        id v = [NSClassFromString(@"NSJSONSerialization") JSONObjectWithData:json options:4 /* fragments */ error:NULL];
        h([v isKindOfClass:[NSNull class]] ? nil : v, nil);
    }
}

- (void)_poll {
    if (!_host) return;
    static char buf[65536];
    int kind = 0, arg = 0, r;
    while (_host && (r = emu_webview_poll(_host, &kind, &arg, buf, sizeof buf)) == 1) [self _event:kind arg:arg text:buf];
    if (!_host) return;
    // the screen holding the web view went away (dismissed): close the window
    if ([self window]) {
        _shown = YES;
        _lostWindowAt = 0;
    } else if (_shown) {
        double now = emu_time();
        if (!_lostWindowAt) _lostWindowAt = now;
        else if (now - _lostWindowAt > 0.3) { [self _closeHost]; return; }
    }
    [self _startPolling];
}
- (void)removeFromSuperview {
    [super removeFromSuperview];
}
@end

// debug: EMU_TEST_WKWEBVIEW=<url> shows the URL in a WKWebView after the launch and logs what its
// navigation delegate sees; a navigation to ms-xal-test:... is taken (cancelled) and ends the test
@interface _EmuWKTest : NSObject {
@public
    WKWebView* _view;
}
@end
@implementation _EmuWKTest
- (void)webView:(WKWebView*)w decidePolicyForNavigationAction:(WKNavigationAction*)a decisionHandler:(void (^)(NSInteger))h {
    NSString* u = [[[a request] URL] absoluteString];
    NSLog(@"[WebKit test] decide %@", u);
    if ([u hasPrefix:@"ms-xal-test:"]) {
        h(0);
        [w evaluateJavaScript:@"document.title" completionHandler:^(id r, NSError* e) { NSLog(@"[WebKit test] title %@ (error %@)", r, e); }];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            [w removeFromSuperview];
            NSLog(@"[WebKit test] removed");
        });
        return;
    }
    h(1);
}
- (void)webView:(WKWebView*)w didStartProvisionalNavigation:(WKNavigation*)n { (void)w; (void)n; NSLog(@"[WebKit test] started"); }
- (void)webView:(WKWebView*)w didFinishNavigation:(WKNavigation*)n { (void)n; NSLog(@"[WebKit test] finished %@", [[w URL] absoluteString]); }
- (void)webView:(WKWebView*)w didFailProvisionalNavigation:(WKNavigation*)n withError:(NSError*)e { (void)w; (void)n; NSLog(@"[WebKit test] failed %@", e); }
- (void)cancel:(id)s { (void)s; NSLog(@"[WebKit test] cancel"); }
@end
EMU_EXPORT void _uikit_test_wkwebview(UIView* parent) {
    const char* u = getenv("EMU_TEST_WKWEBVIEW");
    if (!u || !parent) return;
    _EmuWKTest* t = [[_EmuWKTest alloc] init];  // (kept)
    WKWebView* w = [[WKWebView alloc] initWithFrame:[parent bounds] configuration:nil];
    t->_view = w;
    [w setNavigationDelegate:t];
    [parent addSubview:w];
    [w loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:[NSString stringWithUTF8String:u]]]];
}
