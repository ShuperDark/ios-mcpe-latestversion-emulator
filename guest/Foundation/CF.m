// CoreFoundation (toll-free bridged to our Foundation classes), plus small
// C APIs from SystemConfiguration, Security and CFNetwork.
#import <emu/Foundation.h>

#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

extern id objc_retain(id);
extern void objc_release(id);
#include <emu/msg.h>
#include <emu/host.h>

typedef const void* CFTypeRef;
typedef const void* CFAllocatorRef;
typedef long CFIndex;
typedef unsigned long CFTypeID;
typedef unsigned int CFStringEncoding;
typedef unsigned long CFHashCode;
typedef struct { unsigned char b[16]; } CFUUIDBytes;
typedef struct { CFIndex location, length; } CFRange;

EMU_EXPORT const CFAllocatorRef kCFAllocatorDefault = NULL;
EMU_EXPORT const CFAllocatorRef kCFAllocatorSystemDefault = NULL;
EMU_EXPORT const CFAllocatorRef kCFAllocatorMalloc = NULL;
EMU_EXPORT const CFAllocatorRef kCFAllocatorNull = (CFAllocatorRef)1;
EMU_EXPORT CFTypeRef kCFBooleanTrue;
EMU_EXPORT CFTypeRef kCFBooleanFalse;
EMU_EXPORT CFTypeRef kCFNull;
EMU_EXPORT const void* kCFTypeArrayCallBacks[5];
EMU_EXPORT const void* kCFTypeDictionaryKeyCallBacks[6];
EMU_EXPORT const void* kCFTypeDictionaryValueCallBacks[5];
EMU_EXPORT const void* kCFCopyStringDictionaryKeyCallBacks[6];

__attribute__((constructor)) static void cf_init(void) {
    kCFBooleanTrue = (CFTypeRef)[NSNumber numberWithBool:YES];
    kCFBooleanFalse = (CFTypeRef)[NSNumber numberWithBool:NO];
    kCFNull = (CFTypeRef)[NSNull null];
}

EMU_EXPORT CFTypeRef CFRetain(CFTypeRef o) { return o ? (CFTypeRef)objc_retain((id)o) : NULL; }
EMU_EXPORT void CFRelease(CFTypeRef o) { if (o) objc_release((id)o); }
EMU_EXPORT CFTypeRef CFAutorelease(CFTypeRef o) { return (CFTypeRef)[(id)o autorelease]; }
EMU_EXPORT CFIndex CFGetRetainCount(CFTypeRef o) { return (CFIndex)[(id)o retainCount]; }
EMU_EXPORT CFTypeRef CFMakeCollectable(CFTypeRef o) { return o; }
EMU_EXPORT BOOL CFEqual(CFTypeRef a, CFTypeRef b) { return a == b || [(id)a isEqual:(id)b]; }
EMU_EXPORT CFHashCode CFHash(CFTypeRef o) { return [(id)o hash]; }
EMU_EXPORT CFTypeRef CFCopyDescription(CFTypeRef o) { return (CFTypeRef)[[(id)o description] retain]; }
EMU_EXPORT CFTypeID CFGetTypeID(CFTypeRef o) {
    id x = (id)o;
    if ([x isKindOfClass:[NSString class]]) return 7;
    if ([x isKindOfClass:[NSArray class]]) return 19;
    if ([x isKindOfClass:[NSDictionary class]]) return 18;
    if ([x isKindOfClass:[NSNumber class]]) return x == (id)kCFBooleanTrue || x == (id)kCFBooleanFalse ? 21 : 22;
    if ([x isKindOfClass:[NSData class]]) return 20;
    if ([x isKindOfClass:[NSDate class]]) return 42;
    return 1;
}
EMU_EXPORT CFTypeID CFStringGetTypeID(void) { return 7; }
EMU_EXPORT CFTypeID CFArrayGetTypeID(void) { return 19; }
EMU_EXPORT CFTypeID CFDictionaryGetTypeID(void) { return 18; }
EMU_EXPORT CFTypeID CFBooleanGetTypeID(void) { return 21; }
EMU_EXPORT CFTypeID CFNumberGetTypeID(void) { return 22; }
EMU_EXPORT CFTypeID CFDataGetTypeID(void) { return 20; }
EMU_EXPORT CFTypeID CFDateGetTypeID(void) { return 42; }
EMU_EXPORT BOOL CFBooleanGetValue(CFTypeRef b) { return [(id)b boolValue]; }

// ---- strings ----
#define kCFStringEncodingUTF8 0x08000100
static NSStringEncoding ns_enc(CFStringEncoding e) {
    switch (e) {
    case kCFStringEncodingUTF8: return NSUTF8StringEncoding;
    case 0x0600: return NSASCIIStringEncoding;
    case 0x0201: return NSISOLatin1StringEncoding;
    case 0x0100: return NSUnicodeStringEncoding;
    case 0x10000100: return NSUTF16BigEndianStringEncoding;
    case 0x14000100: return NSUTF16LittleEndianStringEncoding;
    case 0x0500: return NSWindowsCP1252StringEncoding;
    case 0: return NSMacOSRomanStringEncoding;
    default: return NSUTF8StringEncoding;
    }
}
EMU_EXPORT NSStringEncoding CFStringConvertEncodingToNSStringEncoding(CFStringEncoding e) { return ns_enc(e); }
EMU_EXPORT CFStringEncoding CFStringConvertNSStringEncodingToEncoding(NSStringEncoding e) {
    return e == NSASCIIStringEncoding ? 0x0600 : e == NSUnicodeStringEncoding ? 0x0100 : kCFStringEncodingUTF8;
}
EMU_EXPORT CFStringEncoding CFStringGetSystemEncoding(void) { return 0; }
EMU_EXPORT CFTypeRef CFStringCreateWithCString(CFAllocatorRef a, const char* s, CFStringEncoding e) {
    (void)a;
    return s ? (CFTypeRef)[[NSString alloc] initWithBytes:s length:strlen(s) encoding:ns_enc(e)] : NULL;
}
EMU_EXPORT CFTypeRef CFStringCreateWithCStringNoCopy(CFAllocatorRef a, const char* s, CFStringEncoding e, CFAllocatorRef d) {
    (void)d;
    return CFStringCreateWithCString(a, s, e);
}
EMU_EXPORT CFTypeRef CFStringCreateWithBytes(CFAllocatorRef a, const uint8_t* b, CFIndex n, CFStringEncoding e, BOOL bom) {
    (void)a; (void)bom;
    return (CFTypeRef)[[NSString alloc] initWithBytes:b length:(NSUInteger)n encoding:ns_enc(e)];
}
EMU_EXPORT CFTypeRef CFStringCreateWithCharacters(CFAllocatorRef a, const unichar* c, CFIndex n) {
    (void)a;
    return (CFTypeRef)[[NSString alloc] initWithCharacters:c length:(NSUInteger)n];
}
EMU_EXPORT CFTypeRef CFStringCreateCopy(CFAllocatorRef a, CFTypeRef s) { (void)a; return (CFTypeRef)[(id)s copy]; }
EMU_EXPORT CFTypeRef CFStringCreateMutable(CFAllocatorRef a, CFIndex n) { (void)a; (void)n; return (CFTypeRef)[[NSMutableString alloc] init]; }
EMU_EXPORT CFTypeRef CFStringCreateMutableCopy(CFAllocatorRef a, CFIndex n, CFTypeRef s) { (void)a; (void)n; return (CFTypeRef)[(id)s mutableCopy]; }
EMU_EXPORT void CFStringAppend(CFTypeRef m, CFTypeRef s) { [(NSMutableString*)m appendString:(NSString*)s]; }
EMU_EXPORT void CFStringAppendCString(CFTypeRef m, const char* s, CFStringEncoding e) { (void)e; [(NSMutableString*)m appendString:[NSString stringWithUTF8String:s]]; }
EMU_EXPORT CFIndex CFStringGetLength(CFTypeRef s) { return (CFIndex)[(NSString*)s length]; }
EMU_EXPORT unichar CFStringGetCharacterAtIndex(CFTypeRef s, CFIndex i) { return [(NSString*)s characterAtIndex:(NSUInteger)i]; }
EMU_EXPORT void CFStringGetCharacters(CFTypeRef s, CFRange r, unichar* buf) {
    [(NSString*)s getCharacters:buf range:NSMakeRange((NSUInteger)r.location, (NSUInteger)r.length)];
}
EMU_EXPORT const char* CFStringGetCStringPtr(CFTypeRef s, CFStringEncoding e) {
    (void)e;
    return s ? [(NSString*)s UTF8String] : NULL;
}
EMU_EXPORT const unichar* CFStringGetCharactersPtr(CFTypeRef s) { (void)s; return NULL; }
EMU_EXPORT BOOL CFStringGetCString(CFTypeRef s, char* buf, CFIndex n, CFStringEncoding e) {
    (void)e;
    const char* u = [(NSString*)s UTF8String];
    size_t l = strlen(u);
    if ((CFIndex)l + 1 > n) return NO;
    memcpy(buf, u, l + 1);
    return YES;
}
EMU_EXPORT CFIndex CFStringGetMaximumSizeForEncoding(CFIndex n, CFStringEncoding e) { (void)e; return n * 4; }
EMU_EXPORT int CFStringCompare(CFTypeRef a, CFTypeRef b, unsigned long opt) { return (int)[(NSString*)a compare:(NSString*)b options:opt]; }
EMU_EXPORT BOOL CFStringHasPrefix(CFTypeRef a, CFTypeRef b) { return [(NSString*)a hasPrefix:(NSString*)b]; }
EMU_EXPORT BOOL CFStringHasSuffix(CFTypeRef a, CFTypeRef b) { return [(NSString*)a hasSuffix:(NSString*)b]; }
EMU_EXPORT int CFStringGetIntValue(CFTypeRef s) { return [(NSString*)s intValue]; }
EMU_EXPORT double CFStringGetDoubleValue(CFTypeRef s) { return [(NSString*)s doubleValue]; }
EMU_EXPORT CFTypeRef CFStringCreateWithFormat(CFAllocatorRef a, CFTypeRef opts, CFTypeRef fmt, ...) {
    (void)a; (void)opts;
    va_list ap;
    va_start(ap, fmt);
    NSString* s = [[NSString alloc] initWithFormat:(NSString*)fmt arguments:ap];
    va_end(ap);
    return (CFTypeRef)s;
}
EMU_EXPORT CFTypeRef CFURLCreateStringByAddingPercentEscapes(CFAllocatorRef a, CFTypeRef s, CFTypeRef leave, CFTypeRef escape, CFStringEncoding e) {
    (void)a; (void)e;
    NSString* str = (NSString*)s;
    NSMutableString* out = [NSMutableString string];
    const unsigned char* p = (const unsigned char*)[str UTF8String];
    NSString* lv = (NSString*)leave;
    NSString* es = (NSString*)escape;
    for (; *p; p++) {
        char c = (char)*p;
        NSString* cs = [[[NSString alloc] initWithBytes:&c length:1 encoding:NSISOLatin1StringEncoding] autorelease];
        BOOL keep = (*p < 128) && (isalnum(*p) || strchr("-_.!~*'()", *p) || (lv && [lv rangeOfString:cs].location != NSNotFound) ||
                                   (strchr(";/?:@&=+$,#[]", *p) && !(es && [es rangeOfString:cs].location != NSNotFound)));
        if (keep) [out appendString:cs];
        else [out appendFormat:@"%%%02X", *p];
    }
    return (CFTypeRef)[out copy];
}
EMU_EXPORT CFTypeRef CFURLCreateStringByReplacingPercentEscapes(CFAllocatorRef a, CFTypeRef s, CFTypeRef leave) {
    (void)a; (void)leave;
    return (CFTypeRef)[[(NSString*)s stringByRemovingPercentEncoding] retain];
}
EMU_EXPORT CFTypeRef CFURLCreateStringByReplacingPercentEscapesUsingEncoding(CFAllocatorRef a, CFTypeRef s, CFTypeRef leave, CFStringEncoding e) {
    (void)e;
    return CFURLCreateStringByReplacingPercentEscapes(a, s, leave);
}

// ---- arrays / dictionaries / data / numbers ----
EMU_EXPORT CFTypeRef CFArrayCreate(CFAllocatorRef a, const void** v, CFIndex n, const void* cb) {
    (void)a; (void)cb;
    return (CFTypeRef)[[NSArray alloc] initWithObjects:(const id*)v count:(NSUInteger)n];
}
EMU_EXPORT CFTypeRef CFArrayCreateMutable(CFAllocatorRef a, CFIndex n, const void* cb) { (void)a; (void)n; (void)cb; return (CFTypeRef)[[NSMutableArray alloc] init]; }
EMU_EXPORT CFTypeRef CFArrayCreateCopy(CFAllocatorRef a, CFTypeRef arr) { (void)a; return (CFTypeRef)[(id)arr copy]; }
EMU_EXPORT CFIndex CFArrayGetCount(CFTypeRef a) { return (CFIndex)[(NSArray*)a count]; }
EMU_EXPORT const void* CFArrayGetValueAtIndex(CFTypeRef a, CFIndex i) { return [(NSArray*)a objectAtIndex:(NSUInteger)i]; }
EMU_EXPORT void CFArrayAppendValue(CFTypeRef a, const void* v) { [(NSMutableArray*)a addObject:(id)v]; }
EMU_EXPORT void CFArrayRemoveValueAtIndex(CFTypeRef a, CFIndex i) { [(NSMutableArray*)a removeObjectAtIndex:(NSUInteger)i]; }
EMU_EXPORT void CFArrayRemoveAllValues(CFTypeRef a) { [(NSMutableArray*)a removeAllObjects]; }
EMU_EXPORT CFTypeRef CFDictionaryCreate(CFAllocatorRef a, const void** k, const void** v, CFIndex n, const void* kc, const void* vc) {
    (void)a; (void)kc; (void)vc;
    return (CFTypeRef)[[NSDictionary alloc] initWithObjects:(const id*)v forKeys:(const id*)k count:(NSUInteger)n];
}
EMU_EXPORT CFTypeRef CFDictionaryCreateMutable(CFAllocatorRef a, CFIndex n, const void* kc, const void* vc) {
    (void)a; (void)n; (void)kc; (void)vc;
    return (CFTypeRef)[[NSMutableDictionary alloc] init];
}
EMU_EXPORT CFTypeRef CFDictionaryCreateMutableCopy(CFAllocatorRef a, CFIndex n, CFTypeRef d) { (void)a; (void)n; return (CFTypeRef)[(id)d mutableCopy]; }
EMU_EXPORT CFTypeRef CFDictionaryCreateCopy(CFAllocatorRef a, CFTypeRef d) { (void)a; return (CFTypeRef)[(id)d copy]; }
EMU_EXPORT const void* CFDictionaryGetValue(CFTypeRef d, const void* k) { return d && k ? [(NSDictionary*)d objectForKey:(id)k] : NULL; }
EMU_EXPORT BOOL CFDictionaryGetValueIfPresent(CFTypeRef d, const void* k, const void** v) {
    id r = [(NSDictionary*)d objectForKey:(id)k];
    if (v) *v = r;
    return r != nil;
}
EMU_EXPORT BOOL CFDictionaryContainsKey(CFTypeRef d, const void* k) { return [(NSDictionary*)d objectForKey:(id)k] != nil; }
EMU_EXPORT CFIndex CFDictionaryGetCount(CFTypeRef d) { return (CFIndex)[(NSDictionary*)d count]; }
EMU_EXPORT void CFDictionarySetValue(CFTypeRef d, const void* k, const void* v) { [(NSMutableDictionary*)d setObject:(id)v forKey:(id)k]; }
EMU_EXPORT void CFDictionaryAddValue(CFTypeRef d, const void* k, const void* v) {
    if (![(NSDictionary*)d objectForKey:(id)k]) [(NSMutableDictionary*)d setObject:(id)v forKey:(id)k];
}
EMU_EXPORT void CFDictionaryRemoveValue(CFTypeRef d, const void* k) { [(NSMutableDictionary*)d removeObjectForKey:(id)k]; }
EMU_EXPORT void CFDictionaryGetKeysAndValues(CFTypeRef d, const void** keys, const void** vals) {
    NSUInteger i = 0;
    for (id k in (NSDictionary*)d) {
        if (keys) keys[i] = k;
        if (vals) vals[i] = [(NSDictionary*)d objectForKey:k];
        i++;
    }
}
EMU_EXPORT CFTypeRef CFDataCreate(CFAllocatorRef a, const uint8_t* b, CFIndex n) { (void)a; return (CFTypeRef)[[NSData alloc] initWithBytes:b length:(NSUInteger)n]; }
EMU_EXPORT CFTypeRef CFDataCreateWithBytesNoCopy(CFAllocatorRef a, const uint8_t* b, CFIndex n, CFAllocatorRef d) {
    (void)a;
    return (CFTypeRef)[[NSData alloc] initWithBytesNoCopy:(void*)b length:(NSUInteger)n freeWhenDone:d != kCFAllocatorNull];
}
EMU_EXPORT CFTypeRef CFDataCreateMutable(CFAllocatorRef a, CFIndex n) { (void)a; (void)n; return (CFTypeRef)[[NSMutableData alloc] init]; }
EMU_EXPORT const uint8_t* CFDataGetBytePtr(CFTypeRef d) { return [(NSData*)d bytes]; }
EMU_EXPORT uint8_t* CFDataGetMutableBytePtr(CFTypeRef d) { return [(NSMutableData*)d mutableBytes]; }
EMU_EXPORT CFIndex CFDataGetLength(CFTypeRef d) { return (CFIndex)[(NSData*)d length]; }
EMU_EXPORT void CFDataAppendBytes(CFTypeRef d, const uint8_t* b, CFIndex n) { [(NSMutableData*)d appendBytes:b length:(NSUInteger)n]; }
EMU_EXPORT void CFDataSetLength(CFTypeRef d, CFIndex n) { [(NSMutableData*)d setLength:(NSUInteger)n]; }
EMU_EXPORT BOOL CFNumberIsFloatType(CFTypeRef n) {
    const char* t = [(NSNumber*)n objCType];
    return *t == 'f' || *t == 'd';
}
EMU_EXPORT CFTypeRef CFNumberCreate(CFAllocatorRef a, CFIndex type, const void* v) {
    (void)a;
    switch (type) {
    case 1: return (CFTypeRef)[[NSNumber numberWithChar:*(const char*)v] retain];
    case 2: return (CFTypeRef)[[NSNumber numberWithInt:*(const short*)v] retain];
    case 3: case 9: return (CFTypeRef)[[NSNumber numberWithInt:*(const int*)v] retain];
    case 4: case 10: case 11: case 15: return (CFTypeRef)[[NSNumber numberWithLongLong:*(const long long*)v] retain];
    case 5: case 12: return (CFTypeRef)[[NSNumber numberWithFloat:*(const float*)v] retain];
    default: return (CFTypeRef)[[NSNumber numberWithDouble:*(const double*)v] retain];
    }
}
EMU_EXPORT BOOL CFNumberGetValue(CFTypeRef n, CFIndex type, void* v) {
    NSNumber* x = (NSNumber*)n;
    switch (type) {
    case 1: *(char*)v = [x charValue]; break;
    case 2: *(short*)v = (short)[x intValue]; break;
    case 3: case 9: *(int*)v = [x intValue]; break;
    case 4: case 10: case 11: case 15: *(long long*)v = [x longLongValue]; break;
    case 5: case 12: *(float*)v = [x floatValue]; break;
    default: *(double*)v = [x doubleValue]; break;
    }
    return YES;
}
EMU_EXPORT CFIndex CFNumberGetType(CFTypeRef n) {
    const char* t = [(NSNumber*)n objCType];
    return *t == 'd' ? 13 : *t == 'f' ? 12 : 4;
}

// ---- UUID ----
@interface __CFUUID : NSObject {
@public
    CFUUIDBytes _b;
}
@end
@implementation __CFUUID
@end
EMU_EXPORT CFTypeRef CFUUIDCreate(CFAllocatorRef a) {
    (void)a;
    __CFUUID* u = [[__CFUUID alloc] init];
    arc4random_buf(u->_b.b, 16);
    u->_b.b[6] = (u->_b.b[6] & 0x0f) | 0x40;
    u->_b.b[8] = (u->_b.b[8] & 0x3f) | 0x80;
    return (CFTypeRef)u;
}
EMU_EXPORT CFTypeRef CFUUIDCreateFromUUIDBytes(CFAllocatorRef a, CFUUIDBytes b) {
    (void)a;
    __CFUUID* u = [[__CFUUID alloc] init];
    u->_b = b;
    return (CFTypeRef)u;
}
EMU_EXPORT CFUUIDBytes CFUUIDGetUUIDBytes(CFTypeRef u) { return ((__CFUUID*)u)->_b; }
EMU_EXPORT CFTypeRef CFUUIDCreateString(CFAllocatorRef a, CFTypeRef uu) {
    (void)a;
    const unsigned char* u = ((__CFUUID*)uu)->_b.b;
    return (CFTypeRef)[[NSString stringWithFormat:@"%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", u[0], u[1], u[2],
                                                  u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]] retain];
}

// ---- run loop / bundle / misc ----
EMU_EXPORT CFTypeRef CFRunLoopGetCurrent(void) { return (CFTypeRef)[NSRunLoop currentRunLoop]; }
EMU_EXPORT CFTypeRef CFRunLoopGetMain(void) { return (CFTypeRef)[NSRunLoop mainRunLoop]; }
EMU_EXPORT void CFRunLoopRun(void) { [[NSRunLoop currentRunLoop] run]; }
EMU_EXPORT int CFRunLoopRunInMode(CFTypeRef mode, double secs, BOOL ret) {
    (void)mode; (void)ret;
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:secs]];
    return 3;
}
EMU_EXPORT void CFRunLoopStop(CFTypeRef rl) { (void)rl; }
EMU_EXPORT void CFRunLoopWakeUp(CFTypeRef rl) { (void)rl; }
EMU_EXPORT double CFAbsoluteTimeGetCurrent(void) { return [NSDate timeIntervalSinceReferenceDate]; }
EMU_EXPORT CFTypeRef CFBundleGetMainBundle(void) { return (CFTypeRef)objc_msgSend((id)NSClassFromString(@"NSBundle"), @selector(mainBundle)); }
EMU_EXPORT CFTypeRef CFBundleGetValueForInfoDictionaryKey(CFTypeRef b, CFTypeRef k) { return (CFTypeRef)objc_msgSend((id)b, @selector(objectForInfoDictionaryKey:), (id)k); }
EMU_EXPORT CFTypeRef CFBundleGetInfoDictionary(CFTypeRef b) { return (CFTypeRef)objc_msgSend((id)b, @selector(infoDictionary)); }
EMU_EXPORT CFTypeRef CFBundleGetIdentifier(CFTypeRef b) { return (CFTypeRef)objc_msgSend((id)b, @selector(bundleIdentifier)); }
EMU_EXPORT CFTypeRef CFLocaleCopyCurrent(void) { return (CFTypeRef)[objc_msgSend((id)NSClassFromString(@"NSLocale"), @selector(currentLocale)) retain]; }
EMU_EXPORT CFTypeRef CFLocaleGetValue(CFTypeRef l, CFTypeRef k) { return (CFTypeRef)objc_msgSend((id)l, @selector(objectForKey:), (id)k); }
EMU_EXPORT CFTypeRef CFLocaleCopyPreferredLanguages(void) { return (CFTypeRef)[[NSArray arrayWithObject:@"en"] retain]; }
EMU_EXPORT CFTypeRef CFLocaleGetIdentifier(CFTypeRef l) { return (CFTypeRef)objc_msgSend((id)l, @selector(localeIdentifier)); }
EMU_EXPORT CFTypeRef CFPreferencesCopyAppValue(CFTypeRef k, CFTypeRef app) {
    (void)app;
    id d = objc_msgSend((id)NSClassFromString(@"NSUserDefaults"), @selector(standardUserDefaults));
    return (CFTypeRef)[objc_msgSend(d, @selector(objectForKey:), (id)k) retain];
}
EMU_EXPORT void CFPreferencesSetAppValue(CFTypeRef k, CFTypeRef v, CFTypeRef app) {
    (void)app;
    id d = objc_msgSend((id)NSClassFromString(@"NSUserDefaults"), @selector(standardUserDefaults));
    objc_msgSend(d, @selector(setObject:forKey:), (id)v, (id)k);
}
EMU_EXPORT BOOL CFPreferencesAppSynchronize(CFTypeRef app) { (void)app; return YES; }
EMU_EXPORT CFTypeRef CFPropertyListCreateWithData(CFAllocatorRef a, CFTypeRef d, unsigned long opt, CFIndex* fmt, void** err) {
    (void)a; (void)opt;
    if (fmt) *fmt = 100;
    if (err) *err = NULL;
    return (CFTypeRef)[_emu_plist_parse((NSData*)d) retain];
}
EMU_EXPORT CFTypeRef CFPropertyListCreateDeepCopy(CFAllocatorRef a, CFTypeRef p, unsigned long opt) { (void)a; (void)opt; return (CFTypeRef)[(id)p mutableCopy]; }
EMU_EXPORT double CFTimeZoneGetSecondsFromGMT(CFTypeRef tz, double at) { (void)at; return tz ? (double)((NSInteger(*)(id, SEL))objc_msgSend)((id)tz, @selector(secondsFromGMT)) : 0.0; }
EMU_EXPORT CFTypeRef CFTimeZoneCopySystem(void) { return (CFTypeRef)[objc_msgSend((id)NSClassFromString(@"NSTimeZone"), @selector(systemTimeZone)) retain]; }
EMU_EXPORT CFTypeRef CFNotificationCenterGetDarwinNotifyCenter(void) { return (CFTypeRef)1; }
EMU_EXPORT CFTypeRef CFNotificationCenterGetLocalCenter(void) { return (CFTypeRef)1; }
EMU_EXPORT void CFNotificationCenterAddObserver(CFTypeRef c, const void* o, void* cb, CFTypeRef n, const void* obj, CFIndex s) {
    (void)c; (void)o; (void)cb; (void)n; (void)obj; (void)s;
}
EMU_EXPORT void CFNotificationCenterRemoveObserver(CFTypeRef c, const void* o, CFTypeRef n, const void* obj) { (void)c; (void)o; (void)n; (void)obj; }
EMU_EXPORT void CFNotificationCenterPostNotification(CFTypeRef c, CFTypeRef n, const void* o, CFTypeRef u, BOOL d) { (void)c; (void)n; (void)o; (void)u; (void)d; }

// ===================================================================== SystemConfiguration
@interface __SCNetworkReachability : NSObject
@end
@implementation __SCNetworkReachability
@end
EMU_EXPORT CFTypeRef SCNetworkReachabilityCreateWithAddress(CFAllocatorRef a, const void* addr) { (void)a; (void)addr; return (CFTypeRef)[[__SCNetworkReachability alloc] init]; }
EMU_EXPORT CFTypeRef SCNetworkReachabilityCreateWithName(CFAllocatorRef a, const char* n) { (void)a; (void)n; return (CFTypeRef)[[__SCNetworkReachability alloc] init]; }
EMU_EXPORT BOOL SCNetworkReachabilityGetFlags(CFTypeRef r, uint32_t* flags) {
    (void)r;
    // reachable over a local (Wi-Fi like) network: kSCNetworkReachabilityFlagsReachable |
    // kSCNetworkReachabilityFlagsIsDirect. The game only allows LAN multiplayer on Wi-Fi.
    // EMU_OFFLINE=1 reports "no network" instead.
    *flags = getenv("EMU_OFFLINE") ? 0 : ((1u << 1) | (1u << 17));
    return YES;
}
EMU_EXPORT BOOL SCNetworkReachabilitySetCallback(CFTypeRef r, void* cb, void* ctx) { (void)r; (void)cb; (void)ctx; return YES; }
EMU_EXPORT BOOL SCNetworkReachabilityScheduleWithRunLoop(CFTypeRef r, CFTypeRef rl, CFTypeRef m) { (void)r; (void)rl; (void)m; return YES; }
EMU_EXPORT BOOL SCNetworkReachabilityUnscheduleFromRunLoop(CFTypeRef r, CFTypeRef rl, CFTypeRef m) { (void)r; (void)rl; (void)m; return YES; }
EMU_EXPORT BOOL SCNetworkReachabilitySetDispatchQueue(CFTypeRef r, void* q) { (void)r; (void)q; return YES; }
EMU_EXPORT CFTypeRef CFNetworkCopySystemProxySettings(void) { return (CFTypeRef)[[NSDictionary alloc] init]; }
EMU_EXPORT NSString* const kCFNetworkProxiesHTTPPort = @"HTTPPort";
EMU_EXPORT NSString* const kCFNetworkProxiesHTTPProxy = @"HTTPProxy";
EMU_EXPORT NSString* const kCFNetworkProxiesHTTPEnable = @"HTTPEnable";

// ===================================================================== Security (no keychain)
#define SECSTR(n) EMU_EXPORT NSString* const n = @#n
SECSTR(kSecAttrAccessGroup); SECSTR(kSecAttrAccessible); SECSTR(kSecAttrAccessibleAfterFirstUnlock);
SECSTR(kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly); SECSTR(kSecAttrAccessibleAlways); SECSTR(kSecAttrAccessibleAlwaysThisDeviceOnly);
SECSTR(kSecAttrAccessibleWhenUnlocked); SECSTR(kSecAttrAccessibleWhenUnlockedThisDeviceOnly);
SECSTR(kSecAttrAccount); SECSTR(kSecAttrDescription); SECSTR(kSecAttrGeneric); SECSTR(kSecAttrIsInvisible); SECSTR(kSecAttrLabel);
SECSTR(kSecAttrService); SECSTR(kSecClass); SECSTR(kSecClassGenericPassword); SECSTR(kSecClassKey); SECSTR(kSecClassInternetPassword);
SECSTR(kSecMatchLimit); SECSTR(kSecMatchLimitAll); SECSTR(kSecMatchLimitOne); SECSTR(kSecReturnAttributes); SECSTR(kSecReturnData);
SECSTR(kSecReturnRef); SECSTR(kSecValueData); SECSTR(kSecValueRef); SECSTR(kSecAttrKeyType); SECSTR(kSecAttrApplicationTag);
SECSTR(kSecAttrKeySizeInBits); SECSTR(kSecAttrKeyTypeRSA); SECSTR(kSecAttrSynchronizable); SECSTR(kSecAttrServer);
SECSTR(kSecReturnPersistentRef); SECSTR(kSecValuePersistentRef); SECSTR(kSecAttrKeyClass); SECSTR(kSecAttrKeyClassPublic);
SECSTR(kSecAttrKeyClassPrivate); SECSTR(kSecAttrKeyTypeEC); SECSTR(kSecAttrKeyTypeECSECPrimeRandom); SECSTR(kSecAttrIsPermanent);
SECSTR(kSecAttrCanVerify); SECSTR(kSecAttrCanEncrypt); SECSTR(kSecAttrCanSign); SECSTR(kSecAttrCanDecrypt);

// SecKeyRef: an RSA public key (DER) - verification by the emulator (Windows CNG)
uint64_t emu_rsa_block_size(const void* der, uint64_t len);
int emu_rsa_verify(const void* der, uint64_t len, uint32_t padding, const void* data, uint64_t dlen, const void* sig, uint64_t slen);
@interface _EmuSecKey : NSObject {
@public
    NSData* _der;
}
@end
@implementation _EmuSecKey
- (void)dealloc { [_der release]; [super dealloc]; }
@end
static BOOL key_is_ec(CFTypeRef k) {
    if (![(id)k isKindOfClass:[_EmuSecKey class]]) return NO;
    NSData* d = ((_EmuSecKey*)k)->_der;
    return ([d length] == 65 || [d length] == 97) && ((const uint8_t*)[d bytes])[0] == 4;
}
EMU_EXPORT size_t SecKeyGetBlockSize(CFTypeRef k) {
    if (![(id)k isKindOfClass:[_EmuSecKey class]]) return 0;
    if (key_is_ec(k)) return 64;
    NSData* d = ((_EmuSecKey*)k)->_der;
    return (size_t)emu_rsa_block_size([d bytes], [d length]);
}
EMU_EXPORT int SecKeyRawVerify(CFTypeRef k, uint32_t padding, const uint8_t* data, size_t dlen, const uint8_t* sig, size_t slen) {
    if (![(id)k isKindOfClass:[_EmuSecKey class]]) {
        if (getenv("EMU_TRACE_KEYCHAIN")) NSLog(@"[Security] SecKeyRawVerify: not a key (%p)", k);
        return -50;  // errSecParam
    }
    NSData* d = ((_EmuSecKey*)k)->_der;
    int r = emu_rsa_verify([d bytes], [d length], padding, data, dlen, sig, slen);
    if (getenv("EMU_TRACE_KEYCHAIN")) NSLog(@"[Security] SecKeyRawVerify(padding %x, %zu bytes, signature %zu bytes) = %d", padding, dlen, slen, r);
    return r;
}

// Keychain: kept in <home>/Library/Keychain.plist so that items (e.g. the game's device
// id) survive restarts like on a device, and two data folders are two devices.
static NSMutableArray* g_keychain;
static pthread_mutex_t g_kc_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
#define KC_LOCKED(body) do { pthread_mutex_lock(&g_kc_lock); body; pthread_mutex_unlock(&g_kc_lock); } while (0)
static NSString* kc_path(void) {
    return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Keychain.plist"];
}
static void kc_load(void) {
    if (g_keychain) return;
    NSArray* a = [NSArray arrayWithContentsOfFile:kc_path()];
    g_keychain = a ? [a mutableCopy] : [[NSMutableArray alloc] init];
}
static void kc_save(void) {
    ((BOOL (*)(id, SEL, id, BOOL, id, id*))objc_msgSend)([(id)NSClassFromString(@"NSFileManager") performSelector:@selector(defaultManager)], @selector(createDirectoryAtPath:withIntermediateDirectories:attributes:error:), [kc_path() stringByDeletingLastPathComponent], YES, nil, NULL);
    if (![g_keychain writeToFile:kc_path() atomically:YES]) NSLog(@"[Security] could not save the keychain");
}
// (the item's names only: the stored values - tokens, keys - stay out of the log)
static void kc_trace(const char* op, NSDictionary* d) {
    if (!getenv("EMU_TRACE_KEYCHAIN")) return;
    NSMutableString* s = [NSMutableString string];
    for (NSString* k in @[@"kSecClass", @"kSecAttrService", @"kSecAttrAccount", @"kSecAttrLabel", @"kSecAttrGeneric", @"kSecMatchLimit"]) {
        id v = [d objectForKey:k];
        if (v) [s appendFormat:@" %@=%@", [k substringFromIndex:4], v];
    }
    id data = [d objectForKey:@"kSecValueData"];
    if (data) [s appendFormat:@" (data %lu bytes)", (unsigned long)[data length]];
    NSLog(@"[Security] %s%@", op, s);
}
static BOOL kc_match(NSDictionary* item, NSDictionary* q) {
    id pref = [q objectForKey:@"kSecValuePersistentRef"];
    if (pref && ![pref isEqual:[item objectForKey:@"emuPersistentRef"]]) return NO;
    for (NSString* k in @[@"kSecClass", @"kSecAttrService", @"kSecAttrAccount", @"kSecAttrGeneric", @"kSecAttrApplicationTag", @"kSecAttrKeyType", @"kSecAttrLabel", @"kSecAttrApplicationLabel", @"kSecAttrKeyClass"]) {
        id qv = [q objectForKey:k];
        if (qv && ![qv isEqual:[item objectForKey:k]]) return NO;
    }
    return YES;
}
static int SecItemAdd_locked(CFTypeRef attrs, CFTypeRef* result);
static int SecItemCopyMatching_locked(CFTypeRef query, CFTypeRef* result);
static int SecItemUpdate_locked(CFTypeRef query, CFTypeRef attrs);
static int SecItemDelete_locked(CFTypeRef query);
EMU_EXPORT int SecItemAdd(CFTypeRef attrs, CFTypeRef* result) {
    pthread_mutex_lock(&g_kc_lock);
    int r = SecItemAdd_locked(attrs, result);
    pthread_mutex_unlock(&g_kc_lock);
    return r;
}
static int SecItemAdd_locked(CFTypeRef attrs, CFTypeRef* result) {
    kc_load();
    NSDictionary* a = (NSDictionary*)attrs;
    kc_trace("add", a);
    for (NSDictionary* i in g_keychain) if (kc_match(i, a)) return -25299;  // errSecDuplicateItem
    NSMutableDictionary* item = [[a mutableCopy] autorelease];
    // keys: a persistent reference to find them again
    static unsigned long s_ref;
    NSData* pref = [[NSString stringWithFormat:@"emu-item-%lu-%f", ++s_ref, emu_time()] dataUsingEncoding:NSUTF8StringEncoding];
    [item setObject:pref forKey:@"emuPersistentRef"];
    for (NSString* k in @[@"kSecReturnPersistentRef", @"kSecReturnRef", @"kSecReturnData", @"kSecReturnAttributes"]) [item removeObjectForKey:k];
    // a key object (SecKeyRef) is kept as its data (the keychain is a property list)
    id vref = [item objectForKey:@"kSecValueRef"];
    if (vref) {
        if ([vref isKindOfClass:[_EmuSecKey class]] && ((_EmuSecKey*)vref)->_der) {
            [item setObject:((_EmuSecKey*)vref)->_der forKey:@"kSecValueData"];
            if (![item objectForKey:@"kSecClass"]) [item setObject:@"kSecClassKey" forKey:@"kSecClass"];
            if (![item objectForKey:@"kSecAttrKeyClass"])
                [item setObject:[((_EmuSecKey*)vref)->_der length] == 97 ? @"kSecAttrKeyClassPrivate" : @"kSecAttrKeyClassPublic" forKey:@"kSecAttrKeyClass"];
        }
        [item removeObjectForKey:@"kSecValueRef"];
    }
    [g_keychain addObject:item];
    kc_save();
    if (result) {
        *result = NULL;
        if ([[a objectForKey:@"kSecReturnPersistentRef"] boolValue]) *result = (CFTypeRef)[pref retain];
        else if ([[a objectForKey:@"kSecReturnRef"] boolValue] && [[a objectForKey:@"kSecClass"] isEqual:@"kSecClassKey"]) {
            _EmuSecKey* key = [[_EmuSecKey alloc] init];
            key->_der = [[item objectForKey:@"kSecValueData"] retain];
            *result = (CFTypeRef)key;
        }
    }
    return 0;
}
EMU_EXPORT int SecItemCopyMatching(CFTypeRef query, CFTypeRef* result) {
    pthread_mutex_lock(&g_kc_lock);
    int r = SecItemCopyMatching_locked(query, result);
    pthread_mutex_unlock(&g_kc_lock);
    return r;
}
// One result as Security returns it: a single requested type is returned as itself (the key
// object, the data, the persistent reference); attributes or several types make a dictionary
// (the attributes plus kSecValueRef / kSecValueData / kSecValuePersistentRef). Callers own it.
static id kc_result(NSDictionary* item, BOOL ref, BOOL data, BOOL attrs, BOOL pref) {
    BOOL isKey = [[item objectForKey:@"kSecClass"] isEqual:@"kSecClassKey"];
    id key = nil;
    if (ref && isKey) {
        _EmuSecKey* k = [[[_EmuSecKey alloc] init] autorelease];
        k->_der = [[item objectForKey:@"kSecValueData"] retain];
        key = k;
    }
    if ((int)ref + (int)data + (int)attrs + (int)pref > 1 || attrs) {
        NSMutableDictionary* d = [NSMutableDictionary dictionary];
        for (NSString* k in item)
            if (![k isEqualToString:@"kSecValueData"] && ![k isEqualToString:@"emuPersistentRef"]) [d setObject:[item objectForKey:k] forKey:k];
        if (key) [d setObject:key forKey:@"kSecValueRef"];
        if (data && [item objectForKey:@"kSecValueData"]) [d setObject:[item objectForKey:@"kSecValueData"] forKey:@"kSecValueData"];
        if (pref && [item objectForKey:@"emuPersistentRef"]) [d setObject:[item objectForKey:@"emuPersistentRef"] forKey:@"kSecValuePersistentRef"];
        return [d retain];
    }
    if (ref) return [key retain];
    if (data) return [[item objectForKey:@"kSecValueData"] retain];
    if (pref) return [[item objectForKey:@"emuPersistentRef"] retain];
    return nil;
}
static int SecItemCopyMatching_locked(CFTypeRef query, CFTypeRef* result) {
    kc_load();
    NSDictionary* q = (NSDictionary*)query;
    kc_trace("copy", q);
    NSMutableArray* found = [NSMutableArray array];
    for (NSDictionary* i in g_keychain) if (kc_match(i, q)) [found addObject:i];
    if (getenv("EMU_TRACE_KEYCHAIN")) NSLog(@"[Security]   -> %lu found", (unsigned long)[found count]);
    if (![found count]) { if (result) *result = NULL; return -25300; }  // errSecItemNotFound
    if (result) {
        BOOL all = [[q objectForKey:@"kSecMatchLimit"] isEqual:@"kSecMatchLimitAll"];
        BOOL ref = [[q objectForKey:@"kSecReturnRef"] boolValue], data = [[q objectForKey:@"kSecReturnData"] boolValue];
        BOOL attrs = [[q objectForKey:@"kSecReturnAttributes"] boolValue], pref = [[q objectForKey:@"kSecReturnPersistentRef"] boolValue];
        if (all) {
            NSMutableArray* a = [[NSMutableArray alloc] init];
            for (NSDictionary* i in found) {
                id r = kc_result(i, ref, data, attrs, pref);
                if (r) { [a addObject:r]; [r release]; }
            }
            *result = (CFTypeRef)a;
        } else {
            *result = (CFTypeRef)kc_result([found objectAtIndex:0], ref, data, attrs, pref);
        }
    }
    return 0;
}
EMU_EXPORT int SecItemUpdate(CFTypeRef query, CFTypeRef attrs) {
    pthread_mutex_lock(&g_kc_lock);
    int r = SecItemUpdate_locked(query, attrs);
    pthread_mutex_unlock(&g_kc_lock);
    return r;
}
static int SecItemUpdate_locked(CFTypeRef query, CFTypeRef attrs) {
    kc_load();
    NSDictionary* q = (NSDictionary*)query;
    kc_trace("update", (NSDictionary*)attrs);
    for (NSUInteger i = 0; i < [g_keychain count]; i++) {
        if (!kc_match([g_keychain objectAtIndex:i], q)) continue;
        NSMutableDictionary* m = [[[g_keychain objectAtIndex:i] mutableCopy] autorelease];
        [m addEntriesFromDictionary:(NSDictionary*)attrs];
        [g_keychain replaceObjectAtIndex:i withObject:m];
        kc_save();
        return 0;
    }
    return -25300;
}
EMU_EXPORT int SecItemDelete(CFTypeRef query) {
    pthread_mutex_lock(&g_kc_lock);
    int r = SecItemDelete_locked(query);
    pthread_mutex_unlock(&g_kc_lock);
    return r;
}
static int SecItemDelete_locked(CFTypeRef query) {
    kc_load();
    NSDictionary* q = (NSDictionary*)query;
    kc_trace("delete", q);
    NSUInteger removed = 0;
    for (NSUInteger i = [g_keychain count]; i-- > 0;)
        if (kc_match([g_keychain objectAtIndex:i], q)) { [g_keychain removeObjectAtIndex:i]; removed++; }
    if (!removed) return -25300;  // errSecItemNotFound
    kc_save();
    return 0;
}
// ---- EC (P-256) keys: the Xbox sign-in (Xal) makes a device key and signs its requests ----
// _EmuSecKey._der holds Apple's external representation: 04|X|Y (public) or 04|X|Y|D (private).
// The ECDSA itself is the emulator's (iOS MCPE(v1.26.62) Emulator/hle_security.cpp, Windows CNG).
int emu_ec_generate(uint8_t* out);
int64_t emu_ec_sign(const void* key, uint64_t keylen, const void* digest, uint64_t dlen, void* out, uint64_t cap);
int emu_ec_verify(const void* key, uint64_t keylen, const void* digest, uint64_t dlen, const void* sig, uint64_t slen);
int emu_random_bytes(void* out, uint64_t n);
int emu_cert_verify_ssl(const void* blob, uint64_t len, const char* host);
extern unsigned char* CC_SHA256(const void* d, uint32_t len, unsigned char* md);
extern unsigned char* CC_SHA1(const void* d, uint32_t len, unsigned char* md);
extern unsigned char* CC_SHA384(const void* d, uint32_t len, unsigned char* md);
extern unsigned char* CC_SHA512(const void* d, uint32_t len, unsigned char* md);

SECSTR(kSecPrivateKeyAttrs); SECSTR(kSecPublicKeyAttrs); SECSTR(kSecAttrTokenID); SECSTR(kSecAttrTokenIDSecureEnclave);
SECSTR(kSecAttrAccessControl); SECSTR(kSecAttrEffectiveKeySize); SECSTR(kSecAttrKeyTypeECDSA); SECSTR(kSecUseDataProtectionKeychain);
SECSTR(kSecAttrCanDerive); SECSTR(kSecAttrCanWrap); SECSTR(kSecAttrCanUnwrap); SECSTR(kSecAttrIsExtractable); SECSTR(kSecAttrIsSensitive);
SECSTR(kSecUseAuthenticationUI); SECSTR(kSecUseOperationPrompt); SECSTR(kSecAttrCreationDate); SECSTR(kSecAttrModificationDate);
#define SECALG(n) EMU_EXPORT NSString* const kSecKeyAlgorithm##n = @"algid:" #n
SECALG(ECDSASignatureDigestX962); SECALG(ECDSASignatureDigestX962SHA1); SECALG(ECDSASignatureDigestX962SHA224);
SECALG(ECDSASignatureDigestX962SHA256); SECALG(ECDSASignatureDigestX962SHA384); SECALG(ECDSASignatureDigestX962SHA512);
SECALG(ECDSASignatureMessageX962SHA1); SECALG(ECDSASignatureMessageX962SHA224); SECALG(ECDSASignatureMessageX962SHA256);
SECALG(ECDSASignatureMessageX962SHA384); SECALG(ECDSASignatureMessageX962SHA512); SECALG(ECDSASignatureRFC4754);
SECALG(ECDSASignatureDigestRFC4754); SECALG(ECDSASignatureMessageRFC4754SHA256);
SECALG(RSASignatureDigestPKCS1v15SHA256); SECALG(RSASignatureMessagePKCS1v15SHA256);
SECALG(ECDHKeyExchangeStandard); SECALG(ECDHKeyExchangeCofactor);
EMU_EXPORT const void* const kSecRandomDefault = NULL;

static _EmuSecKey* make_key(NSData* d) {
    _EmuSecKey* k = [[_EmuSecKey alloc] init];
    k->_der = [d copy];
    return k;
}
static void set_error(CFTypeRef* err, int code) {
    if (err) *err = (CFTypeRef)[[NSError errorWithDomain:@"NSOSStatusErrorDomain" code:code userInfo:nil] retain];
}
static NSDictionary* key_item(NSDictionary* params, NSDictionary* attrs, NSData* value, BOOL priv) {
    NSMutableDictionary* item = [NSMutableDictionary dictionary];
    for (NSString* k in @[@"kSecAttrLabel", @"kSecAttrApplicationTag", @"kSecAttrApplicationLabel", @"kSecAttrAccessGroup", @"kSecAttrAccessible", @"kSecAttrSynchronizable"]) {
        id v = [attrs objectForKey:k] ? [attrs objectForKey:k] : [params objectForKey:k];
        if (v) [item setObject:v forKey:k];
    }
    [item setObject:@"kSecClassKey" forKey:@"kSecClass"];
    [item setObject:[params objectForKey:@"kSecAttrKeyType"] ? [params objectForKey:@"kSecAttrKeyType"] : @"kSecAttrKeyTypeEC" forKey:@"kSecAttrKeyType"];
    [item setObject:priv ? @"kSecAttrKeyClassPrivate" : @"kSecAttrKeyClassPublic" forKey:@"kSecAttrKeyClass"];
    [item setObject:[NSNumber numberWithInt:256] forKey:@"kSecAttrKeySizeInBits"];
    [item setObject:value forKey:@"kSecValueData"];
    return item;
}
static BOOL wants_permanent(NSDictionary* params, NSDictionary* attrs) {
    id p = [attrs objectForKey:@"kSecAttrIsPermanent"];
    if (!p) p = [params objectForKey:@"kSecAttrIsPermanent"];
    return [p boolValue];
}
static int generate_pair(NSDictionary* params, _EmuSecKey** pub, _EmuSecKey** priv) {
    NSString* type = [params objectForKey:@"kSecAttrKeyType"];
    int bits = [[params objectForKey:@"kSecAttrKeySizeInBits"] intValue];
    if ([type isEqual:@"kSecAttrKeyTypeRSA"] || (bits && bits != 256)) {
        NSLog(@"[Security] key generation: only P-256 EC keys are supported (%@, %d bits)", type, bits);
        return -4;  // errSecUnimplemented
    }
    uint8_t k[97];
    if (emu_ec_generate(k) != 0) return -67673;  // errSecInternalComponent-ish
    NSData* privData = [NSData dataWithBytes:k length:97];
    NSData* pubData = [NSData dataWithBytes:k length:65];
    NSDictionary* pa = [params objectForKey:@"kSecPrivateKeyAttrs"];
    NSDictionary* ua = [params objectForKey:@"kSecPublicKeyAttrs"];
    if (wants_permanent(params, pa)) SecItemAdd((CFTypeRef)key_item(params, pa, privData, YES), NULL);
    if (wants_permanent(params, ua)) SecItemAdd((CFTypeRef)key_item(params, ua, pubData, NO), NULL);
    if (getenv("EMU_TRACE_KEYCHAIN")) NSLog(@"[Security] generated a P-256 key pair (permanent: %d)", wants_permanent(params, pa));
    if (pub) *pub = make_key(pubData);
    if (priv) *priv = make_key(privData);
    return 0;
}
EMU_EXPORT int SecKeyGeneratePair(CFTypeRef params, CFTypeRef* pub, CFTypeRef* priv) {
    _EmuSecKey *u = nil, *p = nil;
    int r = generate_pair((NSDictionary*)params, &u, &p);
    if (pub) *pub = (CFTypeRef)u; else [u release];
    if (priv) *priv = (CFTypeRef)p; else [p release];
    return r;
}
EMU_EXPORT CFTypeRef SecKeyCreateRandomKey(CFTypeRef params, CFTypeRef* err) {
    _EmuSecKey *u = nil, *p = nil;
    int r = generate_pair((NSDictionary*)params, &u, &p);
    [u release];
    if (r) set_error(err, r);
    return (CFTypeRef)p;
}
EMU_EXPORT CFTypeRef SecKeyCopyPublicKey(CFTypeRef key) {
    if (!key_is_ec(key)) return key ? (CFTypeRef)[(id)key retain] : NULL;
    NSData* d = ((_EmuSecKey*)key)->_der;
    return (CFTypeRef)make_key([d subdataWithRange:NSMakeRange(0, 65)]);
}
EMU_EXPORT CFTypeRef SecKeyCopyExternalRepresentation(CFTypeRef key, CFTypeRef* err) {
    if (![(id)key isKindOfClass:[_EmuSecKey class]]) { set_error(err, -50); return NULL; }
    return (CFTypeRef)[((_EmuSecKey*)key)->_der copy];
}
EMU_EXPORT CFTypeRef SecKeyCreateWithData(CFTypeRef data, CFTypeRef attrs, CFTypeRef* err) {
    (void)attrs;
    if (!data) { set_error(err, -50); return NULL; }
    return (CFTypeRef)make_key((NSData*)data);
}
EMU_EXPORT CFTypeRef SecKeyCopyAttributes(CFTypeRef key) {
    if (!key_is_ec(key)) return (CFTypeRef)[[NSDictionary alloc] init];
    BOOL priv = [((_EmuSecKey*)key)->_der length] == 97;
    return (CFTypeRef)[[NSDictionary alloc] initWithObjectsAndKeys:@"kSecAttrKeyTypeEC", @"kSecAttrKeyType", [NSNumber numberWithInt:256],
                                                                  @"kSecAttrKeySizeInBits", priv ? @"kSecAttrKeyClassPrivate" : @"kSecAttrKeyClassPublic",
                                                                  @"kSecAttrKeyClass", nil];
}
// the digest an ECDSA algorithm signs: the data itself (Digest...) or its hash (Message...)
static NSData* alg_digest(NSString* alg, NSData* data) {
    if (![alg hasPrefix:@"algid:ECDSASignature"]) return nil;
    if ([alg rangeOfString:@"Message"].location == NSNotFound) return data;
    unsigned char md[64];
    if ([alg hasSuffix:@"SHA256"]) { CC_SHA256([data bytes], (uint32_t)[data length], md); return [NSData dataWithBytes:md length:32]; }
    if ([alg hasSuffix:@"SHA384"]) { CC_SHA384([data bytes], (uint32_t)[data length], md); return [NSData dataWithBytes:md length:48]; }
    if ([alg hasSuffix:@"SHA512"]) { CC_SHA512([data bytes], (uint32_t)[data length], md); return [NSData dataWithBytes:md length:64]; }
    if ([alg hasSuffix:@"SHA1"]) { CC_SHA1([data bytes], (uint32_t)[data length], md); return [NSData dataWithBytes:md length:20]; }
    return nil;
}
EMU_EXPORT BOOL SecKeyIsAlgorithmSupported(CFTypeRef key, NSInteger op, CFTypeRef alg) {
    (void)op;
    return key_is_ec(key) && [(NSString*)alg hasPrefix:@"algid:ECDSASignature"] && [(NSString*)alg rangeOfString:@"RFC4754"].location == NSNotFound;
}
EMU_EXPORT CFTypeRef SecKeyCreateSignature(CFTypeRef key, CFTypeRef alg, CFTypeRef data, CFTypeRef* err) {
    NSData* digest = alg_digest((NSString*)alg, (NSData*)data);
    if (!key_is_ec(key) || [((_EmuSecKey*)key)->_der length] != 97 || !digest) {
        NSLog(@"[Security] SecKeyCreateSignature: unsupported key / algorithm %@", (NSString*)alg);
        set_error(err, -50);
        return NULL;
    }
    NSData* k = ((_EmuSecKey*)key)->_der;
    uint8_t sig[80];
    int64_t n = emu_ec_sign([k bytes], [k length], [digest bytes], [digest length], sig, sizeof sig);
    if (n <= 0) { set_error(err, -67673); return NULL; }
    return (CFTypeRef)[[NSData alloc] initWithBytes:sig length:(NSUInteger)n];
}
EMU_EXPORT BOOL SecKeyVerifySignature(CFTypeRef key, CFTypeRef alg, CFTypeRef data, CFTypeRef sig, CFTypeRef* err) {
    NSData* digest = alg_digest((NSString*)alg, (NSData*)data);
    if (!key_is_ec(key) || !digest) { set_error(err, -50); return NO; }
    NSData* k = ((_EmuSecKey*)key)->_der;
    BOOL ok = emu_ec_verify([k bytes], [k length], [digest bytes], [digest length], [(NSData*)sig bytes], [(NSData*)sig length]) == 1;
    if (!ok) set_error(err, -67808);  // errSecVerifyFailed
    return ok;
}
EMU_EXPORT int SecKeyRawSign(CFTypeRef key, uint32_t padding, const uint8_t* digest, size_t dlen, uint8_t* sig, size_t* siglen) {
    (void)padding;
    if (!key_is_ec(key) || [((_EmuSecKey*)key)->_der length] != 97 || !sig || !siglen) return -50;
    NSData* k = ((_EmuSecKey*)key)->_der;
    int64_t n = emu_ec_sign([k bytes], [k length], digest, dlen, sig, *siglen);
    if (n <= 0) return -50;
    *siglen = (size_t)n;
    return 0;
}
EMU_EXPORT int SecRandomCopyBytes(const void* rnd, size_t count, void* bytes) {
    (void)rnd;
    return emu_random_bytes(bytes, count) == 0 ? 0 : -1;
}

// ---- certificates and server trust (the HTTP library checks the servers of its web sockets) ----
@interface _EmuSecCertificate : NSObject {
@public
    NSData* _der;
}
@end
@implementation _EmuSecCertificate
- (void)dealloc { [_der release]; [super dealloc]; }
@end
@interface _EmuSecPolicy : NSObject {
@public
    NSString* _host;
}
@end
@implementation _EmuSecPolicy
- (void)dealloc { [_host release]; [super dealloc]; }
@end
@interface _EmuSecTrust : NSObject {
@public
    NSArray* _certs;
    NSString* _host;
}
@end
@implementation _EmuSecTrust
- (void)dealloc { [_certs release]; [_host release]; [super dealloc]; }
@end
EMU_EXPORT CFTypeRef SecCertificateCreateWithData(CFAllocatorRef a, CFTypeRef d) {
    (void)a;
    if (!d || ![(NSData*)d length]) return NULL;
    _EmuSecCertificate* c = [[_EmuSecCertificate alloc] init];
    c->_der = [(NSData*)d copy];
    return (CFTypeRef)c;
}
EMU_EXPORT CFTypeRef SecCertificateCopyData(CFTypeRef c) {
    return [(id)c isKindOfClass:[_EmuSecCertificate class]] ? (CFTypeRef)[((_EmuSecCertificate*)c)->_der copy] : NULL;
}
EMU_EXPORT CFTypeRef SecCertificateCopySubjectSummary(CFTypeRef c) { (void)c; return (CFTypeRef)[@"certificate" copy]; }
EMU_EXPORT CFTypeRef SecPolicyCreateSSL(BOOL server, CFTypeRef host) {
    (void)server;
    _EmuSecPolicy* p = [[_EmuSecPolicy alloc] init];
    p->_host = [(NSString*)host copy];
    return (CFTypeRef)p;
}
EMU_EXPORT CFTypeRef SecPolicyCreateBasicX509(void) { return (CFTypeRef)[[_EmuSecPolicy alloc] init]; }
EMU_EXPORT int SecTrustCreateWithCertificates(CFTypeRef certs, CFTypeRef policies, CFTypeRef* trust) {
    if (!trust) return -50;
    _EmuSecTrust* t = [[_EmuSecTrust alloc] init];
    t->_certs = [([(id)certs isKindOfClass:[NSArray class]] ? (NSArray*)certs : (certs ? [NSArray arrayWithObject:(id)certs] : [NSArray array])) copy];
    id p = [(id)policies isKindOfClass:[NSArray class]] ? [(NSArray*)policies firstObject] : (id)policies;
    if ([p isKindOfClass:[_EmuSecPolicy class]]) t->_host = [((_EmuSecPolicy*)p)->_host copy];
    *trust = (CFTypeRef)t;
    return 0;
}
EMU_EXPORT int SecTrustSetPolicies(CFTypeRef trust, CFTypeRef policies) {
    id p = [(id)policies isKindOfClass:[NSArray class]] ? [(NSArray*)policies firstObject] : (id)policies;
    if ([(id)trust isKindOfClass:[_EmuSecTrust class]] && [p isKindOfClass:[_EmuSecPolicy class]]) {
        _EmuSecTrust* t = (_EmuSecTrust*)trust;
        [t->_host release];
        t->_host = [((_EmuSecPolicy*)p)->_host copy];
    }
    return 0;
}
// (custom anchors are not used for the check: Windows' trusted roots are)
EMU_EXPORT int SecTrustSetAnchorCertificates(CFTypeRef t, CFTypeRef c) { (void)t; (void)c; return 0; }
EMU_EXPORT int SecTrustSetAnchorCertificatesOnly(CFTypeRef t, BOOL b) { (void)t; (void)b; return 0; }
EMU_EXPORT int SecTrustSetNetworkFetchAllowed(CFTypeRef t, BOOL b) { (void)t; (void)b; return 0; }
static BOOL trust_check(CFTypeRef trust) {
    if (![(id)trust isKindOfClass:[_EmuSecTrust class]]) return NO;
    _EmuSecTrust* t = (_EmuSecTrust*)trust;
    NSMutableData* blob = [NSMutableData data];
    for (id c in t->_certs) {
        if (![c isKindOfClass:[_EmuSecCertificate class]]) continue;
        NSData* d = ((_EmuSecCertificate*)c)->_der;
        uint32_t n = (uint32_t)[d length];
        [blob appendBytes:&n length:4];
        [blob appendData:d];
    }
    BOOL ok = emu_cert_verify_ssl([blob bytes], [blob length], t->_host ? [t->_host UTF8String] : "") == 1;
    if (getenv("EMU_TRACE_KEYCHAIN")) NSLog(@"[Security] trust of %@ (%lu certificates): %d", t->_host, (unsigned long)[t->_certs count], ok);
    return ok;
}
EMU_EXPORT BOOL SecTrustEvaluateWithError(CFTypeRef trust, CFTypeRef* err) {
    BOOL ok = trust_check(trust);
    if (!ok) set_error(err, -67843);  // errSecNotTrusted
    else if (err) *err = NULL;
    return ok;
}
EMU_EXPORT int SecTrustEvaluate(CFTypeRef trust, uint32_t* result) {
    if (result) *result = trust_check(trust) ? 4 : 5;  // kSecTrustResultUnspecified / kSecTrustResultRecoverableTrustFailure
    return 0;
}
EMU_EXPORT int SecTrustGetTrustResult(CFTypeRef trust, uint32_t* result) { return SecTrustEvaluate(trust, result); }
EMU_EXPORT NSInteger SecTrustGetCertificateCount(CFTypeRef trust) {
    return [(id)trust isKindOfClass:[_EmuSecTrust class]] ? (NSInteger)[((_EmuSecTrust*)trust)->_certs count] : 0;
}
EMU_EXPORT CFTypeRef SecTrustGetCertificateAtIndex(CFTypeRef trust, NSInteger i) {
    if (![(id)trust isKindOfClass:[_EmuSecTrust class]]) return NULL;
    NSArray* a = ((_EmuSecTrust*)trust)->_certs;
    return i >= 0 && (NSUInteger)i < [a count] ? (CFTypeRef)[a objectAtIndex:(NSUInteger)i] : NULL;
}
EMU_EXPORT CFTypeRef SecTrustCopyCertificateChain(CFTypeRef trust) {
    return [(id)trust isKindOfClass:[_EmuSecTrust class]] ? (CFTypeRef)[((_EmuSecTrust*)trust)->_certs copy] : NULL;
}
EMU_EXPORT CFTypeRef SecTrustCopyPublicKey(CFTypeRef trust) { (void)trust; return NULL; }
EMU_EXPORT CFTypeRef SecTrustCopyKey(CFTypeRef trust) { (void)trust; return NULL; }

// ---- Network.framework: the path monitor (Xal waits for "network available") ----
@interface _EmuNWPath : NSObject
@end
@implementation _EmuNWPath
@end
@interface _EmuNWPathMonitor : NSObject {
@public
    void* _queue;
    void (^_update)(id);
    void (^_cancel)(void);
}
@end
@implementation _EmuNWPathMonitor
- (void)dealloc { [_update release]; [_cancel release]; [super dealloc]; }
@end
extern void dispatch_async(void* q, void (^b)(void));
extern void* dispatch_get_global_queue(long p, unsigned long f);
EMU_EXPORT CFTypeRef nw_path_monitor_create(void) { return (CFTypeRef)[[_EmuNWPathMonitor alloc] init]; }
EMU_EXPORT CFTypeRef nw_path_monitor_create_with_type(int type) { (void)type; return nw_path_monitor_create(); }
EMU_EXPORT void nw_path_monitor_set_queue(CFTypeRef m, void* q) { ((_EmuNWPathMonitor*)m)->_queue = q; }
EMU_EXPORT void nw_path_monitor_set_update_handler(CFTypeRef m, void (^h)(id)) {
    _EmuNWPathMonitor* x = (_EmuNWPathMonitor*)m;
    [x->_update release];
    x->_update = [h copy];
}
EMU_EXPORT void nw_path_monitor_set_cancel_handler(CFTypeRef m, void (^h)(void)) {
    _EmuNWPathMonitor* x = (_EmuNWPathMonitor*)m;
    [x->_cancel release];
    x->_cancel = [h copy];
}
EMU_EXPORT void nw_path_monitor_start(CFTypeRef m) {
    _EmuNWPathMonitor* x = [(_EmuNWPathMonitor*)m retain];
    dispatch_async(x->_queue ? x->_queue : dispatch_get_global_queue(0, 0), ^{
        _EmuNWPath* p = [[_EmuNWPath alloc] init];
        if (x->_update) x->_update(p);
        [p release];
        [x release];
    });
}
EMU_EXPORT void nw_path_monitor_cancel(CFTypeRef m) {
    _EmuNWPathMonitor* x = [(_EmuNWPathMonitor*)m retain];
    void (^c)(void) = [x->_cancel copy];
    [x->_update release];
    x->_update = nil;
    dispatch_async(x->_queue ? x->_queue : dispatch_get_global_queue(0, 0), ^{
        if (c) c();
        [c release];
        [x release];
    });
}
// nw_path_status_satisfied (1) / unsatisfied (2, EMU_OFFLINE=1)
EMU_EXPORT int nw_path_get_status(CFTypeRef p) { (void)p; return getenv("EMU_OFFLINE") ? 2 : 1; }
EMU_EXPORT BOOL nw_path_is_expensive(CFTypeRef p) { (void)p; return NO; }
EMU_EXPORT BOOL nw_path_is_constrained(CFTypeRef p) { (void)p; return NO; }
EMU_EXPORT BOOL nw_path_has_ipv4(CFTypeRef p) { (void)p; return YES; }
EMU_EXPORT BOOL nw_path_has_ipv6(CFTypeRef p) { (void)p; return NO; }
EMU_EXPORT BOOL nw_path_has_dns(CFTypeRef p) { (void)p; return YES; }
// nw_interface_type_wifi (1): the PC's network counts as Wi-Fi
EMU_EXPORT BOOL nw_path_uses_interface_type(CFTypeRef p, int type) { (void)p; return type == 1; }
EMU_EXPORT void* nw_retain(void* o) { return objc_retain((id)o); }
EMU_EXPORT void nw_release(void* o) { objc_release((id)o); }
