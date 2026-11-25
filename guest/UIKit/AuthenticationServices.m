// AuthenticationServices: ASWebAuthenticationSession (and the older SFAuthenticationSession),
// which the Xbox sign-in (Xal) uses to show the Microsoft account page. The page opens in
// the PC's browser; the emulator gets the redirect to the callback scheme back
// (iOS MCPE(v1.26.62) Emulator/auth.cpp) and the completion handler runs on the main thread.
#import <emu/UIKit.h>
#include <emu/host.h>
#include <emu/msg.h>

#include <dispatch/dispatch.h>
#include <stdlib.h>

int emu_auth_begin(const char* url, const char* scheme);
int emu_auth_poll(char* out, uint64_t cap);
void emu_auth_cancel(void);

EMU_EXPORT NSString* const ASWebAuthenticationSessionErrorDomain = @"com.apple.AuthenticationServices.WebAuthenticationSession";
EMU_EXPORT NSString* const SFAuthenticationErrorDomain = @"com.apple.SafariServices.Authentication";

static id g_active;  // the session in progress (one at a time)

@interface ASWebAuthenticationSession : NSObject {
@public
    NSURL* _url;
    NSString* _scheme;
    void (^_done)(NSURL*, NSError*);
    id _provider;
    BOOL _ephemeral, _running;
    NSDictionary* _headers;
}
@end
@implementation ASWebAuthenticationSession
- (instancetype)initWithURL:(NSURL*)u callbackURLScheme:(NSString*)s completionHandler:(void (^)(NSURL*, NSError*))h {
    _url = [u retain];
    _scheme = [s copy];
    _done = [h copy];
    return self;
}
- (instancetype)initWithURL:(NSURL*)u callback:(id)cb completionHandler:(void (^)(NSURL*, NSError*))h {
    // (iOS 17.4: ASWebAuthenticationSessionCallback with a custom scheme)
    NSString* s = [cb respondsToSelector:@selector(customScheme)] ? [cb performSelector:@selector(customScheme)] : nil;
    return [self initWithURL:u callbackURLScheme:s completionHandler:h];
}
- (void)dealloc { [_url release]; [_scheme release]; [_done release]; [_headers release]; [super dealloc]; }
- (id)presentationContextProvider { return _provider; }
- (void)setPresentationContextProvider:(id)p { _provider = p; }
- (BOOL)prefersEphemeralWebBrowserSession { return _ephemeral; }
- (void)setPrefersEphemeralWebBrowserSession:(BOOL)b { _ephemeral = b; }
- (NSDictionary*)additionalHeaderFields { return _headers; }
- (void)setAdditionalHeaderFields:(NSDictionary*)h { [_headers release]; _headers = [h copy]; }
- (BOOL)canStart { return !_running; }
- (NSString*)_errorDomain { return ASWebAuthenticationSessionErrorDomain; }
- (void)_finish:(NSURL*)u error:(NSError*)e {
    if (!_running) return;
    _running = NO;
    if (g_active == self) g_active = nil;
    void (^d)(NSURL*, NSError*) = _done;
    _done = nil;
    if (d) d(u, e);
    [d release];
    [self release];  // (taken by start)
}
- (void)_poll {
    if (!_running) return;
    char buf[8192];
    int r = emu_auth_poll(buf, sizeof buf);
    if (r == 1) {
        NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:buf]];
        NSLog(@"[Auth] sign-in finished");
        [self _finish:u error:nil];
        return;
    }
    if (r == -1) {
        // ASWebAuthenticationSessionErrorCodeCanceledLogin
        [self _finish:nil error:[NSError errorWithDomain:[self _errorDomain] code:1 userInfo:nil]];
        return;
    }
    [self retain];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        [self _poll];
        [self release];
    });
}
- (BOOL)start {
    if (_running || !_url) return NO;
    if (g_active) [g_active cancel];
    _running = YES;
    g_active = self;
    [self retain];
    NSLog(@"[Auth] opening the sign-in page in the browser (callback scheme %@)", _scheme);
    emu_auth_begin([[_url absoluteString] UTF8String], _scheme ? [_scheme UTF8String] : "");
    [self _poll];
    return YES;
}
- (void)cancel {
    if (!_running) return;
    emu_auth_cancel();
    // (cancel by the app: no completion call, like on iOS)
    _running = NO;
    if (g_active == self) g_active = nil;
    [_done release];
    _done = nil;
    [self release];
}
@end

@interface SFAuthenticationSession : ASWebAuthenticationSession
@end
@implementation SFAuthenticationSession
- (NSString*)_errorDomain { return SFAuthenticationErrorDomain; }
@end
