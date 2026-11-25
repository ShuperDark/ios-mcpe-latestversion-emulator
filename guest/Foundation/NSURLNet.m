// Networking: NSURLRequest / NSURLResponse, NSURLSession (data, upload and download tasks,
// completion handlers or a delegate), NSURLConnection, NSStream / NSInputStream.
//
// Every request runs on a thread of its own that calls the emulator's blocking HTTP
// functions (iOS MCPE(v1.26.62) Emulator/hle_http.cpp, WinHTTP: TLS, proxies, redirects
// and gzip are Windows' own). The results go to the session's delegate queue (or the
// completion handler) like on iOS. EMU_OFFLINE=1 makes every request fail as "offline".
#import <emu/Foundation.h>

#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <emu/host.h>
#include <emu/msg.h>
extern void* objc_autoreleasePoolPush(void);
extern void objc_autoreleasePoolPop(void*);

extern NSString* const NSURLErrorDomain;
extern NSString* const NSLocalizedDescriptionKey;

uint64_t emu_http_open(const char* method, const char* url, const char* headers, double timeout, int* error);
int emu_http_send(uint64_t h, const void* body, uint64_t len);
int64_t emu_http_response(uint64_t h, int* status, char* out, uint64_t cap);
int64_t emu_http_url(uint64_t h, char* out, uint64_t cap);
int64_t emu_http_read(uint64_t h, void* buf, uint64_t n);
void emu_http_cancel(uint64_t h);
void emu_http_close(uint64_t h);

static int trace_http(void) { static int t = -1; if (t < 0) t = getenv("EMU_TRACE_HTTP") != NULL; return t; }

static NSString* error_text(NSInteger code) {
    switch (code) {
    case -999: return @"cancelled";
    case -1000: return @"unsupported URL";
    case -1001: return @"The request timed out.";
    case -1002: return @"unsupported URL";
    case -1003: return @"A server with the specified hostname could not be found.";
    case -1004: return @"Could not connect to the server.";
    case -1005: return @"The network connection was lost.";
    case -1007: return @"too many HTTP redirects";
    case -1009: return @"The Internet connection appears to be offline.";
    case -1011: return @"bad server response";
    case -1200: return @"An SSL error has occurred and a secure connection to the server cannot be made.";
    case -1202: return @"The certificate for this server is invalid.";
    default: return @"unknown error";
    }
}
static NSError* url_error(NSInteger code, NSURL* u) {
    NSMutableDictionary* info = [NSMutableDictionary dictionaryWithObject:error_text(code) forKey:NSLocalizedDescriptionKey];
    if (u) {
        [info setObject:u forKey:@"NSErrorFailingURLKey"];
        [info setObject:[u absoluteString] forKey:@"NSErrorFailingURLStringKey"];
    }
    return [NSError errorWithDomain:NSURLErrorDomain code:code userInfo:info];
}

// ===================================================================== NSStream
@interface NSStream : NSObject {
@public
    id _sdelegate;
    NSUInteger _status;  // NSStreamStatus: 0 not open, 2 open, 5 at end, 6 closed, 7 error
}
@end
@implementation NSStream
- (void)open { _status = 2; }
- (void)close { _status = 6; }
- (id)delegate { return _sdelegate; }
- (void)setDelegate:(id)d { _sdelegate = d; }
- (NSUInteger)streamStatus { return _status; }
- (NSError*)streamError { return nil; }
- (id)propertyForKey:(NSString*)k { (void)k; return nil; }
- (BOOL)setProperty:(id)p forKey:(NSString*)k { (void)p; (void)k; return NO; }
- (void)scheduleInRunLoop:(id)r forMode:(NSString*)m { (void)r; (void)m; }
- (void)removeFromRunLoop:(id)r forMode:(NSString*)m { (void)r; (void)m; }
@end
@interface NSInputStream : NSStream {
@public
    NSData* _data;
    NSUInteger _pos;
}
@end
@implementation NSInputStream
+ (instancetype)inputStreamWithData:(NSData*)d { return [[[self alloc] initWithData:d] autorelease]; }
+ (instancetype)inputStreamWithFileAtPath:(NSString*)p {
    NSData* d = [NSData dataWithContentsOfFile:p];
    return d ? [self inputStreamWithData:d] : nil;
}
+ (instancetype)inputStreamWithURL:(NSURL*)u { return [self inputStreamWithFileAtPath:[u path]]; }
- (instancetype)init { return [super init]; }
- (instancetype)initWithData:(NSData*)d { _data = [d copy]; return self; }
- (instancetype)initWithFileAtPath:(NSString*)p { return [self initWithData:[NSData dataWithContentsOfFile:p]]; }
- (instancetype)initWithURL:(NSURL*)u { return [self initWithFileAtPath:[u path]]; }
- (void)dealloc { [_data release]; [super dealloc]; }
- (NSInteger)read:(uint8_t*)buf maxLength:(NSUInteger)n {
    NSUInteger left = [_data length] - _pos;
    if (n > left) n = left;
    memcpy(buf, (const uint8_t*)[_data bytes] + _pos, n);
    _pos += n;
    if (_pos >= [_data length]) _status = 5;
    return (NSInteger)n;
}
- (BOOL)getBuffer:(uint8_t**)b length:(NSUInteger*)l { (void)b; (void)l; return NO; }
- (BOOL)hasBytesAvailable { return _pos < [_data length]; }
@end
@interface NSOutputStream : NSStream {
@public
    NSMutableData* _out;
}
@end
@implementation NSOutputStream
+ (instancetype)outputStreamToMemory { NSOutputStream* s = [[[self alloc] init] autorelease]; s->_out = [[NSMutableData alloc] init]; return s; }
- (void)dealloc { [_out release]; [super dealloc]; }
- (NSInteger)write:(const uint8_t*)b maxLength:(NSUInteger)n { [_out appendBytes:b length:n]; return (NSInteger)n; }
- (BOOL)hasSpaceAvailable { return YES; }
- (id)propertyForKey:(NSString*)k { return [k isEqualToString:@"NSStreamDataWrittenToMemoryStreamKey"] ? _out : nil; }
@end
EMU_EXPORT NSString* const NSStreamDataWrittenToMemoryStreamKey = @"NSStreamDataWrittenToMemoryStreamKey";

// ===================================================================== NSURLRequest
@interface NSURLRequest : NSObject <NSCopying, NSMutableCopying> {
@public
    NSURL* _url;
    NSURL* _mainDoc;
    NSString* _method;
    NSMutableDictionary* _headers;
    NSData* _body;
    NSInputStream* _bodyStream;
    NSTimeInterval _timeout;
    NSUInteger _policy;
    BOOL _cookies;
}
@end
@interface NSMutableURLRequest : NSURLRequest
@end
@implementation NSURLRequest
+ (instancetype)requestWithURL:(NSURL*)u { return [[[self alloc] initWithURL:u] autorelease]; }
+ (instancetype)requestWithURL:(NSURL*)u cachePolicy:(NSUInteger)p timeoutInterval:(NSTimeInterval)t {
    return [[[self alloc] initWithURL:u cachePolicy:p timeoutInterval:t] autorelease];
}
+ (BOOL)supportsSecureCoding { return YES; }
- (instancetype)init { return [self initWithURL:nil]; }
- (instancetype)initWithURL:(NSURL*)u { return [self initWithURL:u cachePolicy:0 timeoutInterval:60]; }
- (instancetype)initWithURL:(NSURL*)u cachePolicy:(NSUInteger)p timeoutInterval:(NSTimeInterval)t {
    _url = [u retain];
    _method = [@"GET" retain];
    _headers = [[NSMutableDictionary alloc] init];
    _timeout = t;
    _policy = p;
    _cookies = YES;
    return self;
}
- (void)dealloc {
    [_url release]; [_mainDoc release]; [_method release]; [_headers release]; [_body release]; [_bodyStream release];
    [super dealloc];
}
- (id)copyWithZone:(NSZone*)z { return [self mutableCopyWithZone:z]; }
- (id)mutableCopyWithZone:(NSZone*)z {
    (void)z;
    NSMutableURLRequest* r = [[NSMutableURLRequest alloc] initWithURL:_url cachePolicy:_policy timeoutInterval:_timeout];
    [r setHTTPMethod:_method];
    [r setAllHTTPHeaderFields:_headers];
    [r setHTTPBody:_body];
    r->_bodyStream = [_bodyStream retain];
    r->_mainDoc = [_mainDoc retain];
    r->_cookies = _cookies;
    return r;
}
- (NSURL*)URL { return _url; }
- (NSURL*)mainDocumentURL { return _mainDoc; }
- (NSString*)HTTPMethod { return _method; }
- (NSDictionary*)allHTTPHeaderFields { return [[_headers copy] autorelease]; }
- (NSString*)valueForHTTPHeaderField:(NSString*)f {
    for (NSString* k in _headers)
        if ([k caseInsensitiveCompare:f] == 0) return [_headers objectForKey:k];
    return nil;
}
- (NSData*)HTTPBody { return _body; }
- (NSInputStream*)HTTPBodyStream { return _bodyStream; }
- (NSTimeInterval)timeoutInterval { return _timeout; }
- (NSUInteger)cachePolicy { return _policy; }
- (BOOL)HTTPShouldHandleCookies { return _cookies; }
- (BOOL)HTTPShouldUsePipelining { return NO; }
- (BOOL)allowsCellularAccess { return YES; }
- (BOOL)allowsExpensiveNetworkAccess { return YES; }
- (BOOL)allowsConstrainedNetworkAccess { return YES; }
- (NSUInteger)networkServiceType { return 0; }
- (NSString*)description { return [NSString stringWithFormat:@"<NSURLRequest %@ %@>", _method, [_url absoluteString]]; }
@end
@implementation NSMutableURLRequest
- (void)setURL:(NSURL*)u { [_url release]; _url = [u retain]; }
- (void)setMainDocumentURL:(NSURL*)u { [_mainDoc release]; _mainDoc = [u retain]; }
- (void)setHTTPMethod:(NSString*)m { [_method release]; _method = [(m ? m : @"GET") copy]; }
- (void)setHTTPBody:(NSData*)b { [_body release]; _body = [b copy]; [_bodyStream release]; _bodyStream = nil; }
- (void)setHTTPBodyStream:(NSInputStream*)s { [_bodyStream release]; _bodyStream = [s retain]; [_body release]; _body = nil; }
- (void)_removeHeader:(NSString*)f {
    for (NSString* k in [_headers allKeys])
        if ([k caseInsensitiveCompare:f] == 0) [_headers removeObjectForKey:k];
}
- (void)setValue:(NSString*)v forHTTPHeaderField:(NSString*)f {
    if (!f) return;
    [self _removeHeader:f];
    if (v) [_headers setObject:[[v copy] autorelease] forKey:f];
}
- (void)addValue:(NSString*)v forHTTPHeaderField:(NSString*)f {
    if (!f || !v) return;
    NSString* cur = [self valueForHTTPHeaderField:f];
    [self setValue:cur ? [NSString stringWithFormat:@"%@,%@", cur, v] : v forHTTPHeaderField:f];
}
- (void)setAllHTTPHeaderFields:(NSDictionary*)d {
    [_headers removeAllObjects];
    for (NSString* k in d) [self setValue:[d objectForKey:k] forHTTPHeaderField:k];
}
- (void)setTimeoutInterval:(NSTimeInterval)t { _timeout = t; }
- (void)setCachePolicy:(NSUInteger)p { _policy = p; }
- (void)setHTTPShouldHandleCookies:(BOOL)b { _cookies = b; }
- (void)setHTTPShouldUsePipelining:(BOOL)b { (void)b; }
- (void)setNetworkServiceType:(NSUInteger)t { (void)t; }
- (void)setAllowsCellularAccess:(BOOL)b { (void)b; }
- (void)setAllowsExpensiveNetworkAccess:(BOOL)b { (void)b; }
- (void)setAllowsConstrainedNetworkAccess:(BOOL)b { (void)b; }
- (void)setAssumesHTTP3Capable:(BOOL)b { (void)b; }
@end

// ===================================================================== NSURLResponse
@interface NSURLResponse : NSObject <NSCopying> {
@public
    NSURL* _u;
    NSString* _mime;
    NSString* _encoding;
    long long _length;
}
@end
@implementation NSURLResponse
- (instancetype)initWithURL:(NSURL*)u MIMEType:(NSString*)m expectedContentLength:(NSInteger)l textEncodingName:(NSString*)e {
    _u = [u retain];
    _mime = [m copy];
    _encoding = [e copy];
    _length = l;
    return self;
}
- (void)dealloc { [_u release]; [_mime release]; [_encoding release]; [super dealloc]; }
- (id)copyWithZone:(NSZone*)z { (void)z; return [self retain]; }
- (NSURL*)URL { return _u; }
- (long long)expectedContentLength { return _length; }
- (NSString*)MIMEType { return _mime; }
- (NSString*)textEncodingName { return _encoding; }
- (NSString*)suggestedFilename { NSString* f = [[_u path] lastPathComponent]; return [f length] ? f : @"Unknown"; }
@end
@interface NSHTTPURLResponse : NSURLResponse {
@public
    NSInteger _status;
    NSDictionary* _hdr;
}
@end
@implementation NSHTTPURLResponse
- (instancetype)initWithURL:(NSURL*)u statusCode:(NSInteger)s HTTPVersion:(NSString*)v headerFields:(NSDictionary*)h {
    (void)v;
    _u = [u retain];
    _status = s;
    _hdr = [(h ? h : [NSDictionary dictionary]) copy];
    _length = -1;
    NSString* cl = [self valueForHTTPHeaderField:@"Content-Length"];
    if (cl) _length = [cl longLongValue];
    NSString* ct = [self valueForHTTPHeaderField:@"Content-Type"];
    if (ct) {
        NSArray* parts = [ct componentsSeparatedByString:@";"];
        _mime = [[[[parts objectAtIndex:0] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]] lowercaseString] copy];
        for (NSString* p in parts) {
            NSString* t = [p stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
            if ([[t lowercaseString] hasPrefix:@"charset="]) _encoding = [[t substringFromIndex:8] copy];
        }
    }
    return self;
}
- (void)dealloc { [_hdr release]; [super dealloc]; }
- (NSInteger)statusCode { return _status; }
- (NSDictionary*)allHeaderFields { return _hdr; }
- (NSString*)valueForHTTPHeaderField:(NSString*)f {
    for (NSString* k in _hdr)
        if ([k caseInsensitiveCompare:f] == 0) return [_hdr objectForKey:k];
    return nil;
}
+ (NSString*)localizedStringForStatusCode:(NSInteger)c { return [NSString stringWithFormat:@"HTTP %ld", (long)c]; }
@end

// ===================================================================== the transfer (on a thread of its own)
// One request: send, the response, the body in chunks. The callbacks run on the transfer's
// thread; the users post them on to their delegate queue.
@interface _EmuTransfer : NSObject {
@public
    NSURLRequest* _req;
    NSData* _upload;            // uploadTask fromData (instead of the request's body)
    NSDictionary* _extraHeaders; // the session configuration's HTTPAdditionalHeaders
    NSTimeInterval _timeout;
    uint64_t _h;
    pthread_mutex_t _lock;
    BOOL _cancelled;
    void (^_onResponse)(NSHTTPURLResponse*);
    void (^_onData)(NSData*);
    void (^_onDone)(NSError*);
}
- (void)start;
- (void)cancel;
@end
static void* transfer_main(void* arg);
@implementation _EmuTransfer
- (instancetype)init { pthread_mutex_init(&_lock, NULL); return self; }
- (void)dealloc {
    [_req release]; [_upload release]; [_extraHeaders release];
    [_onResponse release]; [_onData release]; [_onDone release];
    [super dealloc];
}
- (void)start {
    [self retain];  // (released by the thread)
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, transfer_main, self) != 0) {
        _onDone(url_error(-1, [_req URL]));
        [self release];
    }
    pthread_attr_destroy(&a);
}
- (void)cancel {
    pthread_mutex_lock(&_lock);
    _cancelled = YES;
    if (_h) emu_http_cancel(_h);
    pthread_mutex_unlock(&_lock);
}
- (NSData*)_body {
    if (_upload) return _upload;
    if ([_req HTTPBody]) return [_req HTTPBody];
    NSInputStream* s = [_req HTTPBodyStream];
    if (!s) return nil;
    NSMutableData* d = [NSMutableData data];
    [s open];
    uint8_t buf[16384];
    for (;;) {
        NSInteger n = [s read:buf maxLength:sizeof buf];
        if (n <= 0) break;
        [d appendBytes:buf length:(NSUInteger)n];
    }
    [s close];
    return d;
}
- (void)_run {
    NSURL* url = [_req URL];
    if (getenv("EMU_OFFLINE")) { _onDone(url_error(-1009, url)); return; }
    // headers: the configuration's, then the request's; the ones WinHTTP manages itself are left out
    NSMutableDictionary* hdr = [NSMutableDictionary dictionary];
    for (NSString* k in _extraHeaders) [hdr setObject:[_extraHeaders objectForKey:k] forKey:k];
    NSDictionary* rh = [_req allHTTPHeaderFields];
    for (NSString* k in rh) {
        for (NSString* e in [hdr allKeys])
            if ([e caseInsensitiveCompare:k] == 0) [hdr removeObjectForKey:e];
        [hdr setObject:[rh objectForKey:k] forKey:k];
    }
    NSMutableString* lines = [NSMutableString string];
    for (NSString* k in hdr) {
        NSString* lk = [k lowercaseString];
        if ([lk isEqualToString:@"content-length"] || [lk isEqualToString:@"host"] || [lk isEqualToString:@"accept-encoding"] ||
            [lk isEqualToString:@"connection"])
            continue;
        id v = [hdr objectForKey:k];
        [lines appendFormat:@"%@: %@\r\n", k, [v isKindOfClass:[NSString class]] ? v : [v description]];
    }
    NSData* body = [self _body];
    // (EMU_TRACE_HTTP=3: the request bodies too)
    if (trace_http() && getenv("EMU_TRACE_HTTP")[0] == '3' && [body length])
        NSLog(@"[http] request body %@: %.*s", [url host], (int)([body length] > 800 ? 800 : [body length]), (const char*)[body bytes]);
    NSTimeInterval timeout = [_req timeoutInterval] > 0 ? [_req timeoutInterval] : _timeout;
    int err = 0;
    uint64_t h = emu_http_open([[_req HTTPMethod] UTF8String], [[url absoluteString] UTF8String], [lines UTF8String], timeout, &err);
    if (!h) { _onDone(url_error(err ? err : -1, url)); return; }
    pthread_mutex_lock(&_lock);
    _h = h;
    BOOL cancelled = _cancelled;
    pthread_mutex_unlock(&_lock);
    if (cancelled) err = -999;
    else err = emu_http_send(h, [body bytes], [body length]);
    if (err) {
        if (trace_http()) NSLog(@"[http] %@ %@ failed (%d); headers: %@ body %lu bytes", [_req HTTPMethod], [url absoluteString], err, hdr, (unsigned long)[body length]);
        pthread_mutex_lock(&_lock); _h = 0; pthread_mutex_unlock(&_lock);
        emu_http_close(h);
        _onDone(url_error(err, url));
        return;
    }
    // the response
    int status = 0;
    char small[4096];
    int64_t need = emu_http_response(h, &status, small, sizeof small);
    char* raw = small;
    if (need >= (int64_t)sizeof small) {
        raw = malloc((size_t)need + 1);
        emu_http_response(h, &status, raw, (uint64_t)need + 1);
    }
    NSMutableDictionary* fields = [NSMutableDictionary dictionary];
    if (need > 0) {
        NSString* all = [[[NSString alloc] initWithBytes:raw length:(NSUInteger)need encoding:NSUTF8StringEncoding] autorelease];
        for (NSString* line in [all componentsSeparatedByString:@"\r\n"]) {
            NSRange c = [line rangeOfString:@":"];
            if (c.location == NSNotFound || c.location == 0) continue;
            NSString* k = [line substringToIndex:c.location];
            NSString* v = [[line substringFromIndex:c.location + 1] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
            NSString* existing = nil;
            for (NSString* e in fields)
                if ([e caseInsensitiveCompare:k] == 0) { existing = e; break; }
            if (existing) [fields setObject:[NSString stringWithFormat:@"%@, %@", [fields objectForKey:existing], v] forKey:existing];
            else [fields setObject:v forKey:k];
        }
    }
    if (raw != small) free(raw);
    // WinHTTP has decoded a compressed body already: its length is not the one in the header
    for (NSString* k in [fields allKeys])
        if ([k caseInsensitiveCompare:@"Content-Encoding"] == 0) {
            for (NSString* l in [fields allKeys])
                if ([l caseInsensitiveCompare:@"Content-Length"] == 0) [fields removeObjectForKey:l];
            [fields removeObjectForKey:k];
            break;
        }
    NSURL* final = url;
    char ubuf[2048];
    int64_t ul = emu_http_url(h, ubuf, sizeof ubuf);
    if (ul > 0 && ul < (int64_t)sizeof ubuf) {
        NSURL* f = [NSURL URLWithString:[NSString stringWithUTF8String:ubuf]];
        if (f) final = f;
    }
    NSHTTPURLResponse* resp = [[[NSHTTPURLResponse alloc] initWithURL:final statusCode:status HTTPVersion:@"HTTP/1.1" headerFields:fields] autorelease];
    if (trace_http()) NSLog(@"[http] %@ %@ -> %d", [_req HTTPMethod], [url absoluteString], status);
    if (trace_http() && status >= 400) NSLog(@"[http] request headers: %@ body %lu bytes; response headers: %@", hdr, (unsigned long)[body length], fields);
    _onResponse(resp);
    // the body
    NSError* e = nil;
    uint8_t* buf = malloc(65536);
    for (;;) {
        int64_t n = emu_http_read(h, buf, 65536);
        if (n == 0) break;
        if (n < 0) { e = url_error((NSInteger)n, url); break; }
        if (trace_http() && (status >= 400 || getenv("EMU_TRACE_HTTP")[0] == '2'))
            NSLog(@"[http] %d body: %.*s", status, (int)(n > 600 ? 600 : n), (const char*)buf);
        _onData([NSData dataWithBytes:buf length:(NSUInteger)n]);
    }
    free(buf);
    pthread_mutex_lock(&_lock); _h = 0; pthread_mutex_unlock(&_lock);
    emu_http_close(h);
    _onDone(e);
}
@end
static void* transfer_main(void* arg) {
    _EmuTransfer* t = (_EmuTransfer*)arg;
    void* pool = objc_autoreleasePoolPush();
    pthread_setname_np("com.apple.NSURLSession-work");
    [t _run];
    objc_autoreleasePoolPop(pool);
    [t release];
    return NULL;
}

// ===================================================================== NSURLSession
@interface NSURLSessionConfiguration : NSObject <NSCopying> {
@public
    NSString* _ident;
    NSDictionary* _headers;
    NSTimeInterval _timeoutRequest, _timeoutResource;
    NSInteger _maxConn;
    NSUInteger _cachePolicy;
    id _cache, _cookies;
    BOOL _setCookies, _cellular, _waits;
    NSDictionary* _proxy;
}
@end
@implementation NSURLSessionConfiguration
+ (instancetype)defaultSessionConfiguration {
    NSURLSessionConfiguration* c = [[[self alloc] init] autorelease];
    c->_timeoutRequest = 60;
    c->_timeoutResource = 7 * 24 * 3600;
    c->_maxConn = 6;
    c->_setCookies = YES;
    c->_cellular = YES;
    return c;
}
+ (instancetype)ephemeralSessionConfiguration { return [self defaultSessionConfiguration]; }
+ (instancetype)backgroundSessionConfigurationWithIdentifier:(NSString*)i {
    NSURLSessionConfiguration* c = [self defaultSessionConfiguration];
    c->_ident = [i copy];
    return c;
}
+ (instancetype)backgroundSessionConfiguration:(NSString*)i { return [self backgroundSessionConfigurationWithIdentifier:i]; }
- (void)dealloc { [_ident release]; [_headers release]; [_cache release]; [_cookies release]; [_proxy release]; [super dealloc]; }
- (id)copyWithZone:(NSZone*)z {
    (void)z;
    NSURLSessionConfiguration* c = [[NSURLSessionConfiguration alloc] init];
    c->_ident = [_ident copy];
    c->_headers = [_headers copy];
    c->_timeoutRequest = _timeoutRequest;
    c->_timeoutResource = _timeoutResource;
    c->_maxConn = _maxConn;
    c->_cachePolicy = _cachePolicy;
    c->_cache = [_cache retain];
    c->_cookies = [_cookies retain];
    c->_setCookies = _setCookies;
    c->_cellular = _cellular;
    c->_waits = _waits;
    c->_proxy = [_proxy copy];
    return c;
}
- (NSString*)identifier { return _ident; }
- (NSDictionary*)HTTPAdditionalHeaders { return _headers; }
- (void)setHTTPAdditionalHeaders:(NSDictionary*)d { [_headers release]; _headers = [d copy]; }
- (NSTimeInterval)timeoutIntervalForRequest { return _timeoutRequest; }
- (void)setTimeoutIntervalForRequest:(NSTimeInterval)t { _timeoutRequest = t; }
- (NSTimeInterval)timeoutIntervalForResource { return _timeoutResource; }
- (void)setTimeoutIntervalForResource:(NSTimeInterval)t { _timeoutResource = t; }
- (NSInteger)HTTPMaximumConnectionsPerHost { return _maxConn; }
- (void)setHTTPMaximumConnectionsPerHost:(NSInteger)n { _maxConn = n; }
- (NSUInteger)requestCachePolicy { return _cachePolicy; }
- (void)setRequestCachePolicy:(NSUInteger)p { _cachePolicy = p; }
- (id)URLCache { return _cache; }
- (void)setURLCache:(id)c { [_cache release]; _cache = [c retain]; }
- (id)HTTPCookieStorage { return _cookies; }
- (void)setHTTPCookieStorage:(id)c { [_cookies release]; _cookies = [c retain]; }
- (BOOL)HTTPShouldSetCookies { return _setCookies; }
- (void)setHTTPShouldSetCookies:(BOOL)b { _setCookies = b; }
- (NSUInteger)HTTPCookieAcceptPolicy { return 2; }
- (void)setHTTPCookieAcceptPolicy:(NSUInteger)p { (void)p; }
- (BOOL)allowsCellularAccess { return _cellular; }
- (void)setAllowsCellularAccess:(BOOL)b { _cellular = b; }
- (void)setAllowsExpensiveNetworkAccess:(BOOL)b { (void)b; }
- (void)setAllowsConstrainedNetworkAccess:(BOOL)b { (void)b; }
- (BOOL)waitsForConnectivity { return _waits; }
- (void)setWaitsForConnectivity:(BOOL)b { _waits = b; }
- (NSDictionary*)connectionProxyDictionary { return _proxy; }
- (void)setConnectionProxyDictionary:(NSDictionary*)d { [_proxy release]; _proxy = [d copy]; }
- (void)setTLSMinimumSupportedProtocol:(int)p { (void)p; }
- (void)setTLSMinimumSupportedProtocolVersion:(int)p { (void)p; }
- (void)setTLSMaximumSupportedProtocolVersion:(int)p { (void)p; }
- (void)setHTTPShouldUsePipelining:(BOOL)b { (void)b; }
- (void)setDiscretionary:(BOOL)b { (void)b; }
- (void)setSessionSendsLaunchEvents:(BOOL)b { (void)b; }
- (void)setSharedContainerIdentifier:(NSString*)s { (void)s; }
- (void)setMultipathServiceType:(NSInteger)t { (void)t; }
- (void)setNetworkServiceType:(NSUInteger)t { (void)t; }
- (void)setProtocolClasses:(NSArray*)a { (void)a; }
- (NSArray*)protocolClasses { return [NSArray array]; }
@end

@class NSURLSession;
@interface NSURLSessionTask : NSObject <NSCopying> {
@public
    NSURLRequest* _req;
    NSURLRequest* _orig;
    void (^_handler)(id, NSURLResponse*, NSError*);  // (NSData* or, for downloads, NSURL*)
    NSURLSession* _session;
    NSUInteger _ident;
    NSInteger _state;  // 0 running, 1 suspended, 2 canceling, 3 completed
    NSURLResponse* _response;
    NSError* _error;
    NSMutableData* _data;  // collected for a completion handler / a download
    NSData* _upload;
    _EmuTransfer* _transfer;
    int64_t _received, _expected, _sent;
    float _priority;
    NSString* _desc;
    BOOL _download, _started;
}
- (void)_finish:(NSError*)e;
@end
@interface NSURLSession : NSObject {
@public
    id _delegate;
    id _queue;  // NSOperationQueue
    NSURLSessionConfiguration* _config;
    NSUInteger _next;
    NSMutableArray* _tasks;
    pthread_mutex_t _lock;
    BOOL _invalidating, _invalid;
    NSString* _desc;
}
- (void)_post:(void (^)(void))b;
- (void)_taskDone:(NSURLSessionTask*)t;
@end

@implementation NSURLSessionTask
- (void)dealloc {
    [_req release]; [_orig release]; [_handler release]; [_response release]; [_error release]; [_data release];
    [_upload release]; [_transfer release]; [_desc release];
    [super dealloc];
}
- (id)copyWithZone:(NSZone*)z { (void)z; return [self retain]; }
- (void)resume {
    if (_started || _state != 1) { if (_state == 1) _state = 0; return; }
    _started = YES;
    _state = 0;
    NSURLSession* s = _session;
    _EmuTransfer* t = [[_EmuTransfer alloc] init];
    t->_req = [_req retain];
    t->_upload = [_upload retain];
    t->_extraHeaders = [s->_config->_headers retain];
    t->_timeout = s->_config->_timeoutRequest;
    _transfer = t;
    BOOL collect = _handler != nil || _download;
    if (collect) _data = [[NSMutableData alloc] init];
    [self retain];
    t->_onResponse = [^(NSHTTPURLResponse* r) {
        [r retain];
        [s _post:^{
            self->_response = r;  // (the reference taken above)
            self->_expected = [r expectedContentLength];
            id d = s->_delegate;
            if (!self->_handler && !self->_download && [d respondsToSelector:@selector(URLSession:dataTask:didReceiveResponse:completionHandler:)])
                ((void (*)(id, SEL, id, id, id, id))objc_msgSend)(d, @selector(URLSession:dataTask:didReceiveResponse:completionHandler:), s, self, r,
                                                               [[^(NSInteger disposition) { (void)disposition; } copy] autorelease]);
        }];
    } copy];
    t->_onData = [^(NSData* d) {
        [d retain];
        [s _post:^{
            self->_received += [d length];
            if (self->_data) [self->_data appendData:d];
            else {
                id dl = s->_delegate;
                if ([dl respondsToSelector:@selector(URLSession:dataTask:didReceiveData:)])
                    ((void (*)(id, SEL, id, id, id))objc_msgSend)(dl, @selector(URLSession:dataTask:didReceiveData:), s, self, d);
            }
            [d release];
        }];
    } copy];
    t->_onDone = [^(NSError* e) {
        [e retain];
        [s _post:^{
            [self _finish:e];
            [e release];
            [self release];
        }];
    } copy];
    // the upload's progress (all at once)
    int64_t up = _upload ? (int64_t)[_upload length] : (int64_t)[[_req HTTPBody] length];
    if (up > 0) {
        [s _post:^{
            self->_sent = up;
            id d = s->_delegate;
            if ([d respondsToSelector:@selector(URLSession:task:didSendBodyData:totalBytesSent:totalBytesExpectedToSend:)])
                ((void (*)(id, SEL, id, id, int64_t, int64_t, int64_t))objc_msgSend)(
                    d, @selector(URLSession:task:didSendBodyData:totalBytesSent:totalBytesExpectedToSend:), s, self, up, up, up);
        }];
    }
    [t start];
}
// (on the delegate queue)
- (void)_finish:(NSError*)e {
    if (_state == 3) return;
    _state = 3;
    _error = [e retain];
    NSURLSession* s = _session;
    id d = s->_delegate;
    if (_download) {
        NSURL* file = nil;
        if (!e) {
            NSString* p = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"CFNetworkDownload_%lu_%f.tmp",
                                                                                                         (unsigned long)_ident, emu_time()]];
            if ([_data writeToFile:p atomically:NO]) file = [NSURL fileURLWithPath:p];
        }
        if (_handler) _handler(file, _response, e);
        else if (file && [d respondsToSelector:@selector(URLSession:downloadTask:didFinishDownloadingToURL:)])
            ((void (*)(id, SEL, id, id, id))objc_msgSend)(d, @selector(URLSession:downloadTask:didFinishDownloadingToURL:), s, self, file);
        if (file) ((BOOL (*)(id, SEL, id, id*))objc_msgSend)([(id)NSClassFromString(@"NSFileManager") performSelector:@selector(defaultManager)], @selector(removeItemAtPath:error:), [file path], NULL);
    } else if (_handler) {
        _handler(e ? nil : _data, _response, e);
    }
    if (!_handler && [d respondsToSelector:@selector(URLSession:task:didCompleteWithError:)])
        ((void (*)(id, SEL, id, id, id))objc_msgSend)(d, @selector(URLSession:task:didCompleteWithError:), s, self, e);
    [_handler release];
    _handler = nil;
    [s _taskDone:self];
}
- (void)cancel {
    if (_state == 3 || _state == 2) return;
    if (!_started) {
        _state = 2;
        NSURLSession* s = _session;
        [self retain];
        [s _post:^{ [self _finish:url_error(-999, [self->_req URL])]; [self release]; }];
        return;
    }
    _state = 2;
    [_transfer cancel];
}
- (void)suspend { if (_state == 0 && !_started) _state = 1; }
- (NSURLRequest*)originalRequest { return _orig; }
- (NSURLRequest*)currentRequest { return _req; }
- (NSURLResponse*)response { return _response; }
- (NSUInteger)taskIdentifier { return _ident; }
- (NSInteger)state { return _state; }
- (NSError*)error { return _error; }
- (float)priority { return _priority; }
- (void)setPriority:(float)p { _priority = p; }
- (int64_t)countOfBytesReceived { return _received; }
- (int64_t)countOfBytesExpectedToReceive { return _expected; }
- (int64_t)countOfBytesSent { return _sent; }
- (int64_t)countOfBytesExpectedToSend { return _upload ? (int64_t)[_upload length] : (int64_t)[[_req HTTPBody] length]; }
- (NSString*)taskDescription { return _desc; }
- (void)setTaskDescription:(NSString*)d { [_desc release]; _desc = [d copy]; }
- (id)progress { return nil; }
- (void)setEarliestBeginDate:(id)d { (void)d; }
- (void)setCountOfBytesClientExpectsToSend:(int64_t)n { (void)n; }
- (void)setCountOfBytesClientExpectsToReceive:(int64_t)n { (void)n; }
- (BOOL)prefersIncrementalDelivery { return YES; }
- (void)setPrefersIncrementalDelivery:(BOOL)b { (void)b; }
@end
@interface NSURLSessionDataTask : NSURLSessionTask
@end
@implementation NSURLSessionDataTask
@end
@interface NSURLSessionUploadTask : NSURLSessionDataTask
@end
@implementation NSURLSessionUploadTask
@end
@interface NSURLSessionDownloadTask : NSURLSessionTask
@end
@implementation NSURLSessionDownloadTask
- (void)cancelByProducingResumeData:(void (^)(NSData*))h { [self cancel]; if (h) h(nil); }
@end
@interface NSURLSessionStreamTask : NSURLSessionTask
@end
@implementation NSURLSessionStreamTask
@end

@implementation NSURLSession
+ (NSURLSession*)sharedSession {
    static NSURLSession* s;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ s = [[NSURLSession alloc] _initWithConfiguration:nil delegate:nil queue:nil]; });
    return s;
}
+ (NSURLSession*)sessionWithConfiguration:(id)c { return [[[self alloc] _initWithConfiguration:c delegate:nil queue:nil] autorelease]; }
+ (NSURLSession*)sessionWithConfiguration:(id)c delegate:(id)d delegateQueue:(id)q {
    return [[[self alloc] _initWithConfiguration:c delegate:d queue:q] autorelease];
}
- (instancetype)_initWithConfiguration:(NSURLSessionConfiguration*)c delegate:(id)d queue:(id)q {
    _config = c ? [c copy] : [[NSURLSessionConfiguration defaultSessionConfiguration] retain];
    _delegate = [d retain];  // (until the session is invalidated, like on iOS)
    if (q) _queue = [q retain];
    else {
        _queue = [[NSClassFromString(@"NSOperationQueue") alloc] init];
        objc_msgSend(_queue, @selector(setMaxConcurrentOperationCount:), (NSInteger)1);
    }
    _tasks = [[NSMutableArray alloc] init];
    pthread_mutex_init(&_lock, NULL);
    return self;
}
- (void)dealloc { [_delegate release]; [_queue release]; [_config release]; [_tasks release]; [_desc release]; [super dealloc]; }
- (id)delegate { return _delegate; }
- (id)delegateQueue { return _queue; }
- (NSURLSessionConfiguration*)configuration { return [[_config copy] autorelease]; }
- (NSString*)sessionDescription { return _desc; }
- (void)setSessionDescription:(NSString*)d { [_desc release]; _desc = [d copy]; }
- (void)_post:(void (^)(void))b {
    void (^c)(void) = [b copy];
    objc_msgSend(_queue, @selector(addOperationWithBlock:), c);
    [c release];
}
- (id)_task:(Class)c request:(NSURLRequest*)r handler:(id)h {
    NSURLSessionTask* t = [[[c alloc] init] autorelease];
    t->_req = [r copy];
    t->_orig = [r copy];
    t->_handler = [h copy];
    t->_session = self;
    t->_state = 1;  // (suspended until resume)
    t->_priority = 0.5f;
    t->_expected = -1;
    pthread_mutex_lock(&_lock);
    t->_ident = ++_next;
    [_tasks addObject:t];
    pthread_mutex_unlock(&_lock);
    [self retain];  // (the tasks keep their session)
    return t;
}
- (void)_taskDone:(NSURLSessionTask*)t {
    pthread_mutex_lock(&_lock);
    [_tasks removeObjectIdenticalTo:t];
    BOOL last = [_tasks count] == 0 && _invalidating && !_invalid;
    if (last) _invalid = YES;
    pthread_mutex_unlock(&_lock);
    if (last) [self _becomeInvalid];
    [self release];
}
- (void)_becomeInvalid {
    id d = _delegate;
    if ([d respondsToSelector:@selector(URLSession:didBecomeInvalidWithError:)])
        ((void (*)(id, SEL, id, id))objc_msgSend)(d, @selector(URLSession:didBecomeInvalidWithError:), self, nil);
    _delegate = nil;
    [d release];
}
- (NSURLSessionDataTask*)dataTaskWithRequest:(NSURLRequest*)r completionHandler:(void (^)(NSData*, NSURLResponse*, NSError*))h {
    return [self _task:[NSURLSessionDataTask class] request:r handler:h];
}
- (NSURLSessionDataTask*)dataTaskWithRequest:(NSURLRequest*)r { return [self _task:[NSURLSessionDataTask class] request:r handler:nil]; }
- (NSURLSessionDataTask*)dataTaskWithURL:(NSURL*)u completionHandler:(void (^)(NSData*, NSURLResponse*, NSError*))h {
    return [self dataTaskWithRequest:[NSURLRequest requestWithURL:u] completionHandler:h];
}
- (NSURLSessionDataTask*)dataTaskWithURL:(NSURL*)u { return [self dataTaskWithRequest:[NSURLRequest requestWithURL:u]]; }
- (NSURLSessionUploadTask*)uploadTaskWithRequest:(NSURLRequest*)r fromData:(NSData*)d completionHandler:(void (^)(NSData*, NSURLResponse*, NSError*))h {
    NSURLSessionUploadTask* t = [self _task:[NSURLSessionUploadTask class] request:r handler:h];
    t->_upload = [d copy];
    return t;
}
- (NSURLSessionUploadTask*)uploadTaskWithRequest:(NSURLRequest*)r fromData:(NSData*)d {
    return [self uploadTaskWithRequest:r fromData:d completionHandler:nil];
}
- (NSURLSessionUploadTask*)uploadTaskWithRequest:(NSURLRequest*)r fromFile:(NSURL*)f completionHandler:(void (^)(NSData*, NSURLResponse*, NSError*))h {
    return [self uploadTaskWithRequest:r fromData:[NSData dataWithContentsOfFile:[f path]] completionHandler:h];
}
- (NSURLSessionUploadTask*)uploadTaskWithRequest:(NSURLRequest*)r fromFile:(NSURL*)f {
    return [self uploadTaskWithRequest:r fromFile:f completionHandler:nil];
}
- (NSURLSessionUploadTask*)uploadTaskWithStreamedRequest:(NSURLRequest*)r { return [self _task:[NSURLSessionUploadTask class] request:r handler:nil]; }
- (NSURLSessionDownloadTask*)downloadTaskWithRequest:(NSURLRequest*)r completionHandler:(void (^)(NSURL*, NSURLResponse*, NSError*))h {
    NSURLSessionDownloadTask* t = [self _task:[NSURLSessionDownloadTask class] request:r handler:h];
    t->_download = YES;
    return t;
}
- (NSURLSessionDownloadTask*)downloadTaskWithRequest:(NSURLRequest*)r { return [self downloadTaskWithRequest:r completionHandler:nil]; }
- (NSURLSessionDownloadTask*)downloadTaskWithURL:(NSURL*)u completionHandler:(void (^)(NSURL*, NSURLResponse*, NSError*))h {
    return [self downloadTaskWithRequest:[NSURLRequest requestWithURL:u] completionHandler:h];
}
- (NSURLSessionDownloadTask*)downloadTaskWithURL:(NSURL*)u { return [self downloadTaskWithURL:u completionHandler:nil]; }
- (void)finishTasksAndInvalidate {
    if (self == [NSURLSession sharedSession]) return;
    pthread_mutex_lock(&_lock);
    _invalidating = YES;
    BOOL now = [_tasks count] == 0 && !_invalid;
    if (now) _invalid = YES;
    pthread_mutex_unlock(&_lock);
    if (now) {
        [self retain];
        [self _post:^{ [self _becomeInvalid]; [self release]; }];
    }
}
- (void)invalidateAndCancel {
    if (self == [NSURLSession sharedSession]) return;
    pthread_mutex_lock(&_lock);
    NSArray* tasks = [[_tasks copy] autorelease];
    pthread_mutex_unlock(&_lock);
    for (NSURLSessionTask* t in tasks) [t cancel];
    [self finishTasksAndInvalidate];
}
- (void)resetWithCompletionHandler:(void (^)(void))h { if (h) [self _post:h]; }
- (void)flushWithCompletionHandler:(void (^)(void))h { if (h) [self _post:h]; }
- (void)getTasksWithCompletionHandler:(void (^)(NSArray*, NSArray*, NSArray*))h {
    pthread_mutex_lock(&_lock);
    NSMutableArray *data = [NSMutableArray array], *up = [NSMutableArray array], *down = [NSMutableArray array];
    for (NSURLSessionTask* t in _tasks)
        [([t isKindOfClass:[NSURLSessionUploadTask class]] ? up : [t isKindOfClass:[NSURLSessionDownloadTask class]] ? down : data) addObject:t];
    pthread_mutex_unlock(&_lock);
    void (^c)(NSArray*, NSArray*, NSArray*) = [h copy];
    [self _post:^{ c(data, up, down); [c release]; }];
}
- (void)getAllTasksWithCompletionHandler:(void (^)(NSArray*))h {
    pthread_mutex_lock(&_lock);
    NSArray* all = [[_tasks copy] autorelease];
    pthread_mutex_unlock(&_lock);
    void (^c)(NSArray*) = [h copy];
    [self _post:^{ c(all); [c release]; }];
}
@end

// ===================================================================== NSURLConnection (the old API)
@interface NSURLConnection : NSObject {
    NSURLRequest* _req;
    id _delegate;
    id _queue;  // NSOperationQueue, or nil: the main thread
    _EmuTransfer* _t;
    BOOL _started;
}
@end
@implementation NSURLConnection
+ (BOOL)canHandleRequest:(NSURLRequest*)r { NSString* s = [[[r URL] scheme] lowercaseString]; return [s isEqualToString:@"http"] || [s isEqualToString:@"https"]; }
+ (NSURLConnection*)connectionWithRequest:(NSURLRequest*)r delegate:(id)d { return [[[self alloc] initWithRequest:r delegate:d] autorelease]; }
+ (NSData*)sendSynchronousRequest:(NSURLRequest*)r returningResponse:(NSURLResponse**)resp error:(NSError**)e {
    __block NSHTTPURLResponse* response = nil;
    __block NSError* error = nil;
    NSMutableData* data = [NSMutableData data];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    _EmuTransfer* t = [[[_EmuTransfer alloc] init] autorelease];
    t->_req = [r retain];
    t->_timeout = 60;
    t->_onResponse = [^(NSHTTPURLResponse* x) { response = [x retain]; } copy];
    t->_onData = [^(NSData* d) { [data appendData:d]; } copy];
    t->_onDone = [^(NSError* x) { error = [x retain]; dispatch_semaphore_signal(done); } copy];
    [t start];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    dispatch_release(done);
    if (resp) *resp = [response autorelease];
    else [response release];
    if (e) *e = [error autorelease];
    else [error release];
    return error ? nil : data;
}
+ (void)sendAsynchronousRequest:(NSURLRequest*)r queue:(id)q completionHandler:(void (^)(NSURLResponse*, NSData*, NSError*))h {
    void (^c)(NSURLResponse*, NSData*, NSError*) = [h copy];
    NSURLSession* s = [NSURLSession sessionWithConfiguration:nil delegate:nil delegateQueue:q];
    NSURLSessionDataTask* t = [s dataTaskWithRequest:r completionHandler:^(NSData* d, NSURLResponse* resp, NSError* e) { c(resp, d, e); [c release]; }];
    [t resume];
    [s finishTasksAndInvalidate];
}
- (instancetype)initWithRequest:(NSURLRequest*)r delegate:(id)d startImmediately:(BOOL)s {
    _req = [r copy];
    _delegate = [d retain];  // (NSURLConnection keeps its delegate until it finishes)
    if (s) [self start];
    return self;
}
- (instancetype)initWithRequest:(NSURLRequest*)r delegate:(id)d { return [self initWithRequest:r delegate:d startImmediately:YES]; }
- (void)dealloc { [_req release]; [_delegate release]; [_queue release]; [_t release]; [super dealloc]; }
- (void)_post:(void (^)(void))b {
    if (_queue) objc_msgSend(_queue, @selector(addOperationWithBlock:), b);
    else dispatch_async(dispatch_get_main_queue(), b);
}
- (void)start {
    if (_started) return;
    _started = YES;
    [self retain];
    _t = [[_EmuTransfer alloc] init];
    _t->_req = [_req retain];
    _t->_timeout = 60;
    _t->_onResponse = [^(NSHTTPURLResponse* r) {
        [r retain];
        [self _post:^{
            if ([self->_delegate respondsToSelector:@selector(connection:didReceiveResponse:)])
                objc_msgSend(self->_delegate, @selector(connection:didReceiveResponse:), self, r);
            [r release];
        }];
    } copy];
    _t->_onData = [^(NSData* d) {
        [d retain];
        [self _post:^{
            if ([self->_delegate respondsToSelector:@selector(connection:didReceiveData:)])
                objc_msgSend(self->_delegate, @selector(connection:didReceiveData:), self, d);
            [d release];
        }];
    } copy];
    _t->_onDone = [^(NSError* e) {
        [e retain];
        [self _post:^{
            if (e) {
                if ([self->_delegate respondsToSelector:@selector(connection:didFailWithError:)])
                    objc_msgSend(self->_delegate, @selector(connection:didFailWithError:), self, e);
            } else if ([self->_delegate respondsToSelector:@selector(connectionDidFinishLoading:)]) {
                objc_msgSend(self->_delegate, @selector(connectionDidFinishLoading:), self);
            }
            [e release];
            [self->_delegate release];
            self->_delegate = nil;
            [self release];
        }];
    } copy];
    [_t start];
}
- (void)cancel { [_t cancel]; }
- (void)scheduleInRunLoop:(id)r forMode:(NSString*)m { (void)r; (void)m; }
- (void)unscheduleFromRunLoop:(id)r forMode:(NSString*)m { (void)r; (void)m; }
- (void)setDelegateQueue:(id)q { [_queue release]; _queue = [q retain]; }
- (NSURLRequest*)originalRequest { return _req; }
- (NSURLRequest*)currentRequest { return _req; }
@end
