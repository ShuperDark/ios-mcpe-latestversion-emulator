// Assorted Foundation classes: NSCharacterSet, NSScanner, NSURL & networking
// (offline), formatters, archivers (stubs), NSIndexPath, NSCoder...
#import <emu/Foundation.h>

#include <ctype.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <emu/msg.h>
extern NSString* const NSURLErrorDomain;
extern NSString* const NSLocalizedDescriptionKey;

// ===================================================================== NSCharacterSet
typedef BOOL (*charpred)(unichar c);
@interface NSCharacterSet : NSObject <NSCopying, NSMutableCopying> {
@public
    uint8_t* _bits;  // 8 KiB bitmap for the BMP
    BOOL _inverted;
}
@end
@interface NSMutableCharacterSet : NSCharacterSet
@end
static NSCharacterSet* make_set(BOOL (*pred)(unsigned c)) {
    NSCharacterSet* s = [[NSCharacterSet alloc] init];
    s->_bits = calloc(8192, 1);
    for (unsigned c = 0; c < 65536; c++)
        if (pred(c)) s->_bits[c >> 3] |= (uint8_t)(1 << (c & 7));
    return [s autorelease];
}
static BOOL p_ws(unsigned c) { return c == ' ' || c == '\t' || c == 0xa0 || c == 0x3000; }
static BOOL p_wsnl(unsigned c) { return p_ws(c) || c == '\n' || c == '\r' || c == 0x85 || c == 0x2028 || c == 0x2029 || c == 0x0b || c == 0x0c; }
static BOOL p_nl(unsigned c) { return c == '\n' || c == '\r' || c == 0x85 || c == 0x2028 || c == 0x2029; }
static BOOL p_letter(unsigned c) { return c < 128 ? isalpha((int)c) != 0 : c >= 0xc0; }
static BOOL p_digit(unsigned c) { return c >= '0' && c <= '9'; }
static BOOL p_alnum(unsigned c) { return p_letter(c) || p_digit(c); }
static BOOL p_upper(unsigned c) { return c < 128 && isupper((int)c); }
static BOOL p_lower(unsigned c) { return c < 128 && islower((int)c); }
static BOOL p_punct(unsigned c) { return c < 128 && ispunct((int)c); }
static BOOL p_symbol(unsigned c) { return c < 128 && strchr("$+<=>^`|~", (int)c) && c; }
static BOOL p_control(unsigned c) { return c < 32 || (c >= 127 && c < 160); }
static BOOL p_unreserved(unsigned c) { return c < 128 && (isalnum((int)c) || strchr("-._~", (int)c)) && c; }
static BOOL p_query(unsigned c) { return p_unreserved(c) || (c < 128 && strchr("!$&'()*+,;=:@/?", (int)c) && c); }
static BOOL p_path(unsigned c) { return p_unreserved(c) || (c < 128 && strchr("!$&'()*+,;=:@/", (int)c) && c); }
static BOOL p_fragment(unsigned c) { return p_query(c); }
static BOOL p_host(unsigned c) { return p_unreserved(c) || (c < 128 && strchr("!$&'()*+,;=:[]", (int)c) && c); }
static BOOL p_user(unsigned c) { return p_unreserved(c) || (c < 128 && strchr("!$&'()*+,;=", (int)c) && c); }
static BOOL p_none(unsigned c) { (void)c; return NO; }

@implementation NSCharacterSet
#define CACHED(name, pred) + (instancetype)name { static NSCharacterSet* s; if (!s) s = [make_set(pred) retain]; return s; }
CACHED(whitespaceCharacterSet, p_ws)
CACHED(whitespaceAndNewlineCharacterSet, p_wsnl)
CACHED(newlineCharacterSet, p_nl)
CACHED(letterCharacterSet, p_letter)
CACHED(decimalDigitCharacterSet, p_digit)
CACHED(alphanumericCharacterSet, p_alnum)
CACHED(uppercaseLetterCharacterSet, p_upper)
CACHED(lowercaseLetterCharacterSet, p_lower)
CACHED(punctuationCharacterSet, p_punct)
CACHED(symbolCharacterSet, p_symbol)
CACHED(controlCharacterSet, p_control)
CACHED(URLQueryAllowedCharacterSet, p_query)
CACHED(URLPathAllowedCharacterSet, p_path)
CACHED(URLFragmentAllowedCharacterSet, p_fragment)
CACHED(URLHostAllowedCharacterSet, p_host)
CACHED(URLUserAllowedCharacterSet, p_user)
CACHED(URLPasswordAllowedCharacterSet, p_user)
CACHED(illegalCharacterSet, p_none)
+ (instancetype)characterSetWithCharactersInString:(NSString*)str {
    NSCharacterSet* s = [make_set(p_none) retain];
    NSUInteger n = [str length];
    for (NSUInteger i = 0; i < n; i++) {
        unichar c = [str characterAtIndex:i];
        s->_bits[c >> 3] |= (uint8_t)(1 << (c & 7));
    }
    return [s autorelease];
}
+ (instancetype)characterSetWithRange:(NSRange)r {
    NSCharacterSet* s = [make_set(p_none) retain];
    for (NSUInteger c = r.location; c < NSMaxRange(r) && c < 65536; c++) s->_bits[c >> 3] |= (uint8_t)(1 << (c & 7));
    return [s autorelease];
}
- (void)dealloc { free(_bits); [super dealloc]; }
- (BOOL)characterIsMember:(unichar)c {
    BOOL m = _bits ? (_bits[c >> 3] >> (c & 7)) & 1 : NO;
    return _inverted ? !m : m;
}
- (BOOL)longCharacterIsMember:(uint32_t)c { return c < 65536 ? [self characterIsMember:(unichar)c] : (_inverted ? YES : NO); }
- (NSCharacterSet*)invertedSet {
    NSCharacterSet* s = [[NSCharacterSet alloc] init];
    s->_bits = malloc(8192);
    memcpy(s->_bits, _bits, 8192);
    s->_inverted = !_inverted;
    return [s autorelease];
}
- (id)copyWithZone:(NSZone*)z { (void)z; return [self retain]; }
- (id)mutableCopyWithZone:(NSZone*)z {
    (void)z;
    NSMutableCharacterSet* s = [[NSMutableCharacterSet alloc] init];
    s->_bits = malloc(8192);
    for (int i = 0; i < 8192; i++) s->_bits[i] = _inverted ? (uint8_t)~_bits[i] : _bits[i];
    return s;
}
- (BOOL)isSupersetOfSet:(NSCharacterSet*)o {
    for (unsigned c = 0; c < 65536; c++) if ([o characterIsMember:(unichar)c] && ![self characterIsMember:(unichar)c]) return NO;
    return YES;
}
@end
@implementation NSMutableCharacterSet
+ (instancetype)characterSetWithCharactersInString:(NSString*)s { return [[[super characterSetWithCharactersInString:s] mutableCopy] autorelease]; }
- (void)addCharactersInString:(NSString*)str {
    if (!_bits) _bits = calloc(8192, 1);
    for (NSUInteger i = 0; i < [str length]; i++) { unichar c = [str characterAtIndex:i]; _bits[c >> 3] |= (uint8_t)(1 << (c & 7)); }
}
- (void)removeCharactersInString:(NSString*)str {
    if (!_bits) return;
    for (NSUInteger i = 0; i < [str length]; i++) { unichar c = [str characterAtIndex:i]; _bits[c >> 3] &= (uint8_t)~(1 << (c & 7)); }
}
- (void)addCharactersInRange:(NSRange)r {
    if (!_bits) _bits = calloc(8192, 1);
    for (NSUInteger c = r.location; c < NSMaxRange(r) && c < 65536; c++) _bits[c >> 3] |= (uint8_t)(1 << (c & 7));
}
- (void)formUnionWithCharacterSet:(NSCharacterSet*)o {
    if (!_bits) _bits = calloc(8192, 1);
    for (unsigned c = 0; c < 65536; c++) if ([o characterIsMember:(unichar)c]) _bits[c >> 3] |= (uint8_t)(1 << (c & 7));
}
- (void)invert { _inverted = !_inverted; }
@end

// ===================================================================== NSScanner
@interface NSScanner : NSObject {
    NSString* _s;
    NSUInteger _loc;
    NSCharacterSet* _skip;
    BOOL _ci;
}
@end
@implementation NSScanner
+ (instancetype)scannerWithString:(NSString*)s { return [[[self alloc] initWithString:s] autorelease]; }
+ (instancetype)localizedScannerWithString:(NSString*)s { return [self scannerWithString:s]; }
- (instancetype)initWithString:(NSString*)s {
    _s = [s copy];
    _skip = [[NSCharacterSet whitespaceAndNewlineCharacterSet] retain];
    return self;
}
- (void)dealloc { [_s release]; [_skip release]; [super dealloc]; }
- (NSString*)string { return _s; }
- (NSUInteger)scanLocation { return _loc; }
- (void)setScanLocation:(NSUInteger)l { _loc = l; }
- (void)setCharactersToBeSkipped:(NSCharacterSet*)s { [_skip release]; _skip = [s retain]; }
- (NSCharacterSet*)charactersToBeSkipped { return _skip; }
- (void)setCaseSensitive:(BOOL)c { _ci = !c; }
- (BOOL)isAtEnd {
    NSUInteger l = _loc, n = [_s length];
    while (l < n && _skip && [_skip characterIsMember:[_s characterAtIndex:l]]) l++;
    return l >= n;
}
- (void)_skip {
    NSUInteger n = [_s length];
    while (_loc < n && _skip && [_skip characterIsMember:[_s characterAtIndex:_loc]]) _loc++;
}
- (BOOL)scanString:(NSString*)str intoString:(NSString**)out {
    [self _skip];
    NSUInteger l = [str length];
    if (_loc + l > [_s length]) return NO;
    NSRange r = [_s rangeOfString:str options:NSAnchoredSearch | (_ci ? NSCaseInsensitiveSearch : 0) range:NSMakeRange(_loc, l)];
    if (r.location == NSNotFound) return NO;
    if (out) *out = [_s substringWithRange:r];
    _loc += l;
    return YES;
}
- (BOOL)scanUpToString:(NSString*)str intoString:(NSString**)out {
    [self _skip];
    NSUInteger n = [_s length];
    if (_loc >= n) return NO;
    NSRange r = [_s rangeOfString:str options:0 range:NSMakeRange(_loc, n - _loc)];
    NSUInteger end = r.location == NSNotFound ? n : r.location;
    if (end == _loc) return NO;
    if (out) *out = [_s substringWithRange:NSMakeRange(_loc, end - _loc)];
    _loc = end;
    return YES;
}
- (BOOL)scanCharactersFromSet:(NSCharacterSet*)set intoString:(NSString**)out {
    [self _skip];
    NSUInteger st = _loc, n = [_s length];
    while (_loc < n && [set characterIsMember:[_s characterAtIndex:_loc]]) _loc++;
    if (_loc == st) return NO;
    if (out) *out = [_s substringWithRange:NSMakeRange(st, _loc - st)];
    return YES;
}
- (BOOL)scanUpToCharactersFromSet:(NSCharacterSet*)set intoString:(NSString**)out {
    [self _skip];
    NSUInteger st = _loc, n = [_s length];
    while (_loc < n && ![set characterIsMember:[_s characterAtIndex:_loc]]) _loc++;
    if (_loc == st) return NO;
    if (out) *out = [_s substringWithRange:NSMakeRange(st, _loc - st)];
    return YES;
}
- (BOOL)_scanNumber:(char*)buf max:(int)max hex:(BOOL)hex float:(BOOL)fl {
    [self _skip];
    NSUInteger n = [_s length], l = _loc;
    int k = 0;
    if (l < n && ([_s characterAtIndex:l] == '-' || [_s characterAtIndex:l] == '+')) buf[k++] = (char)[_s characterAtIndex:l++];
    if (hex && l + 1 < n && [_s characterAtIndex:l] == '0' && ([_s characterAtIndex:l + 1] == 'x' || [_s characterAtIndex:l + 1] == 'X')) l += 2;
    int digits = 0;
    while (l < n && k < max - 1) {
        unichar c = [_s characterAtIndex:l];
        BOOL ok = hex ? (c < 128 && isxdigit(c)) : (c >= '0' && c <= '9') || (fl && (c == '.' || c == 'e' || c == 'E' || ((c == '-' || c == '+') && k && (buf[k - 1] == 'e' || buf[k - 1] == 'E'))));
        if (!ok) break;
        if (c < 128 && isxdigit(c)) digits++;
        buf[k++] = (char)c;
        l++;
    }
    buf[k] = 0;
    if (!digits) return NO;
    _loc = l;
    return YES;
}
- (BOOL)scanInt:(int*)v { char b[64]; if (![self _scanNumber:b max:64 hex:NO float:NO]) return NO; if (v) *v = (int)strtol(b, NULL, 10); return YES; }
- (BOOL)scanInteger:(NSInteger*)v { char b[64]; if (![self _scanNumber:b max:64 hex:NO float:NO]) return NO; if (v) *v = (NSInteger)strtoll(b, NULL, 10); return YES; }
- (BOOL)scanLongLong:(long long*)v { char b[64]; if (![self _scanNumber:b max:64 hex:NO float:NO]) return NO; if (v) *v = strtoll(b, NULL, 10); return YES; }
- (BOOL)scanUnsignedLongLong:(unsigned long long*)v { char b[64]; if (![self _scanNumber:b max:64 hex:NO float:NO]) return NO; if (v) *v = strtoull(b, NULL, 10); return YES; }
- (BOOL)scanHexInt:(unsigned*)v { char b[64]; if (![self _scanNumber:b max:64 hex:YES float:NO]) return NO; if (v) *v = (unsigned)strtoul(b, NULL, 16); return YES; }
- (BOOL)scanHexLongLong:(unsigned long long*)v { char b[64]; if (![self _scanNumber:b max:64 hex:YES float:NO]) return NO; if (v) *v = strtoull(b, NULL, 16); return YES; }
- (BOOL)scanDouble:(double*)v { char b[64]; if (![self _scanNumber:b max:64 hex:NO float:YES]) return NO; if (v) *v = strtod(b, NULL); return YES; }
- (BOOL)scanFloat:(float*)v { char b[64]; if (![self _scanNumber:b max:64 hex:NO float:YES]) return NO; if (v) *v = strtof(b, NULL); return YES; }
@end

// ===================================================================== NSURL
@interface NSURL : NSObject <NSCopying> {
    NSString* _str;
    BOOL _file;
}
@end
@implementation NSURL
+ (instancetype)URLWithString:(NSString*)s { return s ? [[[self alloc] initWithString:s] autorelease] : nil; }
+ (instancetype)URLWithString:(NSString*)s relativeToURL:(NSURL*)base {
    if (!base || [s rangeOfString:@"://"].location != NSNotFound) return [self URLWithString:s];
    NSString* b = [base absoluteString];
    if ([s hasPrefix:@"/"]) {
        NSRange r = [b rangeOfString:@"://"];
        NSRange slash = [b rangeOfString:@"/" options:0 range:NSMakeRange(r.location + 3, [b length] - r.location - 3)];
        NSString* root = slash.location == NSNotFound ? b : [b substringToIndex:slash.location];
        return [self URLWithString:[root stringByAppendingString:s]];
    }
    return [self URLWithString:[[b stringByDeletingLastPathComponent] stringByAppendingPathComponent:s]];
}
+ (instancetype)fileURLWithPath:(NSString*)p { return p ? [[[self alloc] initFileURLWithPath:p] autorelease] : nil; }
+ (instancetype)fileURLWithPath:(NSString*)p isDirectory:(BOOL)d { (void)d; return [self fileURLWithPath:p]; }
+ (instancetype)fileURLWithPathComponents:(NSArray*)c { return [self fileURLWithPath:[NSString pathWithComponents:c]]; }
- (instancetype)initWithString:(NSString*)s {
    if (!s) { [self release]; return nil; }
    _str = [s copy];
    _file = [s hasPrefix:@"file:"];
    return self;
}
- (instancetype)initWithString:(NSString*)s relativeToURL:(NSURL*)u { [self release]; return [[NSURL URLWithString:s relativeToURL:u] retain]; }
- (instancetype)initFileURLWithPath:(NSString*)p {
    _str = [[@"file://" stringByAppendingString:[p stringByAddingPercentEncodingWithAllowedCharacters:[NSCharacterSet URLPathAllowedCharacterSet]]] retain];
    _file = YES;
    return self;
}
- (instancetype)initFileURLWithPath:(NSString*)p isDirectory:(BOOL)d { (void)d; return [self initFileURLWithPath:p]; }
- (void)dealloc { [_str release]; [super dealloc]; }
- (id)copyWithZone:(NSZone*)z { (void)z; return [self retain]; }
- (NSString*)absoluteString { return _str; }
- (NSURL*)absoluteURL { return self; }
- (NSString*)relativeString { return _str; }
- (NSString*)description { return _str; }
- (BOOL)isFileURL { return _file; }
- (NSUInteger)hash { return [_str hash]; }
- (BOOL)isEqual:(id)o { return [o isKindOfClass:[NSURL class]] && [_str isEqualToString:[o absoluteString]]; }
- (NSString*)scheme {
    NSRange r = [_str rangeOfString:@":"];
    return r.location == NSNotFound ? nil : [_str substringToIndex:r.location];
}
- (NSString*)resourceSpecifier {
    NSRange r = [_str rangeOfString:@":"];
    return r.location == NSNotFound ? _str : [_str substringFromIndex:r.location + 1];
}
- (NSString*)_afterScheme {
    NSRange r = [_str rangeOfString:@"://"];
    return r.location == NSNotFound ? [self resourceSpecifier] : [_str substringFromIndex:r.location + 3];
}
- (NSString*)host {
    if (_file) return nil;
    NSString* a = [self _afterScheme];
    NSUInteger end = [a length];
    for (NSString* sep in [NSArray arrayWithObjects:@"/", @"?", @"#", nil]) {
        NSRange r = [a rangeOfString:sep];
        if (r.location != NSNotFound && r.location < end) end = r.location;
    }
    NSString* hp = [a substringToIndex:end];
    NSRange at = [hp rangeOfString:@"@"];
    if (at.location != NSNotFound) hp = [hp substringFromIndex:at.location + 1];
    NSRange colon = [hp rangeOfString:@":" options:NSBackwardsSearch];
    return colon.location != NSNotFound ? [hp substringToIndex:colon.location] : hp;
}
- (NSNumber*)port {
    NSString* a = [self _afterScheme];
    NSRange sl = [a rangeOfString:@"/"];
    NSString* hp = sl.location == NSNotFound ? a : [a substringToIndex:sl.location];
    NSRange colon = [hp rangeOfString:@":" options:NSBackwardsSearch];
    return colon.location != NSNotFound ? [NSNumber numberWithInt:[[hp substringFromIndex:colon.location + 1] intValue]] : nil;
}
- (NSString*)_pathPart {
    NSString* a = _file ? [_str substringFromIndex:7] : [self _afterScheme];
    if (!_file) {
        NSRange sl = [a rangeOfString:@"/"];
        a = sl.location == NSNotFound ? @"" : [a substringFromIndex:sl.location];
    }
    NSRange q = [a rangeOfString:@"?"];
    if (q.location != NSNotFound) a = [a substringToIndex:q.location];
    NSRange h = [a rangeOfString:@"#"];
    if (h.location != NSNotFound) a = [a substringToIndex:h.location];
    return a;
}
- (NSString*)path {
    NSString* p = [[self _pathPart] stringByRemovingPercentEncoding];
    if (_file && [p length] > 1 && [p hasSuffix:@"/"]) p = [p substringToIndex:[p length] - 1];
    return p;
}
- (NSString*)relativePath { return [self path]; }
- (NSString*)query {
    NSRange q = [_str rangeOfString:@"?"];
    if (q.location == NSNotFound) return nil;
    NSString* r = [_str substringFromIndex:q.location + 1];
    NSRange h = [r rangeOfString:@"#"];
    return h.location == NSNotFound ? r : [r substringToIndex:h.location];
}
- (NSString*)fragment {
    NSRange h = [_str rangeOfString:@"#"];
    return h.location == NSNotFound ? nil : [_str substringFromIndex:h.location + 1];
}
- (NSString*)user { return nil; }
- (NSString*)password { return nil; }
- (NSString*)lastPathComponent { return [[self path] lastPathComponent]; }
- (NSString*)pathExtension { return [[self path] pathExtension]; }
- (NSArray*)pathComponents { return [[self path] pathComponents]; }
- (NSURL*)URLByAppendingPathComponent:(NSString*)c {
    if (_file) return [NSURL fileURLWithPath:[[self path] stringByAppendingPathComponent:c]];
    NSString* s = [_str hasSuffix:@"/"] ? [_str stringByAppendingString:c] : [NSString stringWithFormat:@"%@/%@", _str, c];
    return [NSURL URLWithString:s];
}
- (NSURL*)URLByAppendingPathComponent:(NSString*)c isDirectory:(BOOL)d { (void)d; return [self URLByAppendingPathComponent:c]; }
- (NSURL*)URLByDeletingLastPathComponent {
    if (_file) return [NSURL fileURLWithPath:[[self path] stringByDeletingLastPathComponent]];
    return [NSURL URLWithString:[_str stringByDeletingLastPathComponent]];
}
- (NSURL*)URLByAppendingPathExtension:(NSString*)e {
    if (_file) return [NSURL fileURLWithPath:[[self path] stringByAppendingPathExtension:e]];
    return [NSURL URLWithString:[_str stringByAppendingPathExtension:e]];
}
- (NSURL*)URLByDeletingPathExtension {
    if (_file) return [NSURL fileURLWithPath:[[self path] stringByDeletingPathExtension]];
    return [NSURL URLWithString:[_str stringByDeletingPathExtension]];
}
- (NSURL*)URLByStandardizingPath { return self; }
- (NSURL*)URLByResolvingSymlinksInPath { return self; }
- (NSURL*)baseURL { return nil; }
- (const char*)fileSystemRepresentation { return [[self path] fileSystemRepresentation]; }
- (BOOL)checkResourceIsReachableAndReturnError:(NSError**)e {
    BOOL ok = [[NSClassFromString(@"NSFileManager") performSelector:@selector(defaultManager)] performSelector:@selector(fileExistsAtPath:) withObject:[self path]] != nil;
    if (!ok && e) *e = [NSError errorWithDomain:@"NSCocoaErrorDomain" code:260 userInfo:nil];
    return ok;
}
- (BOOL)getResourceValue:(id*)v forKey:(NSString*)k error:(NSError**)e {
    (void)e;
    if ([k isEqualToString:@"NSURLNameKey"]) *v = [self lastPathComponent];
    else if ([k isEqualToString:@"NSURLIsExcludedFromBackupKey"]) *v = [NSNumber numberWithBool:YES];
    else if ([k isEqualToString:@"NSURLIsDirectoryKey"]) {
        BOOL d = NO;
        id fm = [NSClassFromString(@"NSFileManager") performSelector:@selector(defaultManager)];
        ((BOOL(*)(id, SEL, id, BOOL*))objc_msgSend)(fm, @selector(fileExistsAtPath:isDirectory:), [self path], &d);
        *v = [NSNumber numberWithBool:d];
    } else if ([k isEqualToString:@"NSURLVolumeAvailableCapacityForImportantUsageKey"] ||
               [k isEqualToString:@"NSURLVolumeAvailableCapacityForOpportunisticUsageKey"] ||
               [k isEqualToString:@"NSURLVolumeAvailableCapacityKey"]) {
        // free space of the drive with the data folder (the emulator's statfs)
        id fm = [NSClassFromString(@"NSFileManager") performSelector:@selector(defaultManager)];
        NSDictionary* a = ((id(*)(id, SEL, id, id*))objc_msgSend)(fm, @selector(attributesOfFileSystemForPath:error:), [self path], NULL);
        *v = [a objectForKey:@"NSFileSystemFreeSize"];
    } else if ([k isEqualToString:@"NSURLVolumeTotalCapacityKey"]) {
        id fm = [NSClassFromString(@"NSFileManager") performSelector:@selector(defaultManager)];
        NSDictionary* a = ((id(*)(id, SEL, id, id*))objc_msgSend)(fm, @selector(attributesOfFileSystemForPath:error:), [self path], NULL);
        *v = [a objectForKey:@"NSFileSystemSize"];
    } else *v = nil;
    return YES;
}
- (NSDictionary*)resourceValuesForKeys:(NSArray*)keys error:(NSError**)e {
    NSMutableDictionary* d = [NSMutableDictionary dictionary];
    for (NSString* k in keys) { id v = nil; [self getResourceValue:&v forKey:k error:e]; if (v) [d setObject:v forKey:k]; }
    return d;
}
- (BOOL)setResourceValue:(id)v forKey:(NSString*)k error:(NSError**)e { (void)v; (void)k; (void)e; return YES; }
- (BOOL)setResourceValues:(NSDictionary*)d error:(NSError**)e { (void)d; (void)e; return YES; }
- (BOOL)startAccessingSecurityScopedResource { return YES; }
- (void)stopAccessingSecurityScopedResource {}
@end

@interface NSURLQueryItem : NSObject {
    NSString* _name;
    NSString* _value;
}
@end
@implementation NSURLQueryItem
+ (instancetype)queryItemWithName:(NSString*)n value:(NSString*)v {
    NSURLQueryItem* q = [[[self alloc] init] autorelease];
    q->_name = [n copy];
    q->_value = [v copy];
    return q;
}
- (void)dealloc { [_name release]; [_value release]; [super dealloc]; }
- (NSString*)name { return _name; }
- (NSString*)value { return _value; }
@end

@interface NSURLComponents : NSObject {
    NSString* _scheme;
    NSString* _host;
    NSString* _path;
    NSString* _query;
    NSString* _fragment;
    NSNumber* _port;
}
@end
@implementation NSURLComponents
+ (instancetype)componentsWithString:(NSString*)s { return [[[self alloc] initWithString:s] autorelease]; }
+ (instancetype)componentsWithURL:(NSURL*)u resolvingAgainstBaseURL:(BOOL)b { (void)b; return [self componentsWithString:[u absoluteString]]; }
- (instancetype)initWithString:(NSString*)s {
    NSURL* u = [NSURL URLWithString:s];
    if (!u) { [self release]; return nil; }
    _scheme = [[u scheme] copy];
    _host = [[u host] copy];
    _path = [[u path] copy];
    _query = [[u query] copy];
    _fragment = [[u fragment] copy];
    _port = [[u port] retain];
    return self;
}
- (instancetype)init { _path = [@"" retain]; return self; }
- (void)dealloc { [_scheme release]; [_host release]; [_path release]; [_query release]; [_fragment release]; [_port release]; [super dealloc]; }
#define PROP(get, set, iv) - (NSString*)get { return iv; } - (void)set:(NSString*)v { [iv release]; iv = [v copy]; }
PROP(scheme, setScheme, _scheme)
PROP(host, setHost, _host)
PROP(path, setPath, _path)
PROP(query, setQuery, _query)
PROP(fragment, setFragment, _fragment)
- (NSString*)percentEncodedHost { return _host; }
- (NSString*)percentEncodedPath { return [_path stringByAddingPercentEncodingWithAllowedCharacters:[NSCharacterSet URLPathAllowedCharacterSet]]; }
- (NSString*)percentEncodedQuery { return _query; }
- (NSString*)percentEncodedFragment { return _fragment; }
- (void)setPercentEncodedQuery:(NSString*)q { [self setQuery:q]; }
- (void)setPercentEncodedPath:(NSString*)p { [self setPath:[p stringByRemovingPercentEncoding]]; }
- (NSNumber*)port { return _port; }
- (void)setPort:(NSNumber*)p { [_port release]; _port = [p retain]; }
- (NSArray*)queryItems {
    if (!_query) return nil;
    NSMutableArray* r = [NSMutableArray array];
    for (NSString* kv in [_query componentsSeparatedByString:@"&"]) {
        if (![kv length]) continue;
        NSRange eq = [kv rangeOfString:@"="];
        NSString* k = eq.location == NSNotFound ? kv : [kv substringToIndex:eq.location];
        NSString* v = eq.location == NSNotFound ? nil : [kv substringFromIndex:eq.location + 1];
        [r addObject:[NSURLQueryItem queryItemWithName:[k stringByRemovingPercentEncoding] value:[v stringByRemovingPercentEncoding]]];
    }
    return r;
}
- (void)setQueryItems:(NSArray*)items {
    NSMutableArray* parts = [NSMutableArray array];
    NSCharacterSet* cs = [NSCharacterSet URLQueryAllowedCharacterSet];
    for (NSURLQueryItem* q in items) {
        NSString* k = [[q name] stringByAddingPercentEncodingWithAllowedCharacters:cs];
        [parts addObject:[q value] ? [NSString stringWithFormat:@"%@=%@", k, [[q value] stringByAddingPercentEncodingWithAllowedCharacters:cs]] : k];
    }
    [self setQuery:[parts componentsJoinedByString:@"&"]];
}
- (NSString*)string {
    NSMutableString* s = [NSMutableString string];
    if (_scheme) [s appendFormat:@"%@://", _scheme];
    if (_host) [s appendString:_host];
    if (_port) [s appendFormat:@":%@", _port];
    if (_path) [s appendString:[self percentEncodedPath]];
    if (_query) [s appendFormat:@"?%@", _query];
    if (_fragment) [s appendFormat:@"#%@", _fragment];
    return s;
}
- (NSURL*)URL { return [NSURL URLWithString:[self string]]; }
@end

// (networking: NSURLRequest, NSURLSession, NSURLConnection, NSStream are in NSURLNet.m)
@interface NSHTTPCookieStorage : NSObject
@end
@implementation NSHTTPCookieStorage
+ (NSHTTPCookieStorage*)sharedHTTPCookieStorage {
    static NSHTTPCookieStorage* s;
    if (!s) s = [[NSHTTPCookieStorage alloc] init];
    return s;
}
- (NSArray*)cookies { return [NSArray array]; }
- (NSArray*)cookiesForURL:(NSURL*)u { (void)u; return [NSArray array]; }
- (void)deleteCookie:(id)c { (void)c; }
- (void)setCookie:(id)c { (void)c; }
- (void)setCookieAcceptPolicy:(NSUInteger)p { (void)p; }
@end
@interface NSURLCache : NSObject
@end
@implementation NSURLCache
+ (NSURLCache*)sharedURLCache { static NSURLCache* c; if (!c) c = [[NSURLCache alloc] init]; return c; }
+ (void)setSharedURLCache:(NSURLCache*)c { (void)c; }
- (instancetype)initWithMemoryCapacity:(NSUInteger)m diskCapacity:(NSUInteger)d diskPath:(NSString*)p { (void)m; (void)d; (void)p; return self; }
- (void)removeAllCachedResponses {}
@end

// ===================================================================== formatters
@interface NSFormatter : NSObject
@end
@implementation NSFormatter
@end
@interface NSDateFormatter : NSFormatter {
    NSString* _fmt;
    id _tz;
    BOOL _utc;
}
@end
@implementation NSDateFormatter
- (void)dealloc { [_fmt release]; [_tz release]; [super dealloc]; }
- (void)setDateFormat:(NSString*)f { [_fmt release]; _fmt = [f copy]; }
- (NSString*)dateFormat { return _fmt; }
- (void)setTimeZone:(id)tz { [_tz release]; _tz = [tz retain]; _utc = tz && [(id)tz secondsFromGMT] == 0; }
- (id)timeZone { return _tz; }
- (void)setLocale:(id)l { (void)l; }
- (void)setDateStyle:(NSUInteger)s { (void)s; }
- (void)setTimeStyle:(NSUInteger)s { (void)s; }
- (void)setFormatterBehavior:(NSUInteger)b { (void)b; }
- (void)setCalendar:(id)c { (void)c; }
- (void)setLenient:(BOOL)l { (void)l; }
- (NSString*)stringFromDate:(NSDate*)d {
    time_t t = (time_t)[d timeIntervalSince1970];
    struct tm tm;
    if (_utc) gmtime_r(&t, &tm);
    else localtime_r(&t, &tm);
    // translate common Unicode date patterns to strftime
    NSString* f = _fmt ? _fmt : @"yyyy-MM-dd HH:mm:ss";
    const char* s = [f UTF8String];
    NSMutableString* out = [NSMutableString string];
    char buf[64];
    while (*s) {
        if (*s == '\'') {
            s++;
            while (*s && *s != '\'') { [out appendFormat:@"%c", *s]; s++; }
            if (*s) s++;
            continue;
        }
        char c = *s;
        int n = 0;
        while (s[n] == c) n++;
        buf[0] = 0;
        switch (c) {
        case 'y': snprintf(buf, sizeof buf, n == 2 ? "%02d" : "%04d", n == 2 ? (tm.tm_year + 1900) % 100 : tm.tm_year + 1900); break;
        case 'M': if (n >= 3) strftime(buf, sizeof buf, n == 3 ? "%b" : "%B", &tm); else snprintf(buf, sizeof buf, n == 2 ? "%02d" : "%d", tm.tm_mon + 1); break;
        case 'd': snprintf(buf, sizeof buf, n == 2 ? "%02d" : "%d", tm.tm_mday); break;
        case 'H': snprintf(buf, sizeof buf, n == 2 ? "%02d" : "%d", tm.tm_hour); break;
        case 'h': snprintf(buf, sizeof buf, n == 2 ? "%02d" : "%d", tm.tm_hour % 12 ? tm.tm_hour % 12 : 12); break;
        case 'm': snprintf(buf, sizeof buf, n == 2 ? "%02d" : "%d", tm.tm_min); break;
        case 's': snprintf(buf, sizeof buf, n == 2 ? "%02d" : "%d", tm.tm_sec); break;
        case 'S': snprintf(buf, sizeof buf, "%0*d", n, 0); break;
        case 'a': strftime(buf, sizeof buf, "%p", &tm); break;
        case 'E': strftime(buf, sizeof buf, n >= 4 ? "%A" : "%a", &tm); break;
        case 'Z': case 'z': case 'x': case 'X': snprintf(buf, sizeof buf, _utc ? "+0000" : "%+03ld%02ld", (long)(tm.tm_gmtoff / 3600), (long)labs((tm.tm_gmtoff / 60) % 60)); break;
        default:
            for (int k = 0; k < n; k++) buf[k] = c;
            buf[n] = 0;
        }
        [out appendString:[NSString stringWithUTF8String:buf]];
        s += n;
    }
    return out;
}
- (NSDate*)dateFromString:(NSString*)str {
    // Supports the RFC 3339 / ISO 8601 subset used by web APIs.
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    int y, mo, d, h = 0, mi = 0, se = 0;
    if (sscanf([str UTF8String], "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) >= 3) {
        tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d; tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = se;
        return [NSDate dateWithTimeIntervalSince1970:(double)timegm(&tm)];
    }
    return nil;
}
- (BOOL)getObjectValue:(id*)o forString:(NSString*)s range:(NSRange*)r error:(NSError**)e { (void)r; (void)e; *o = [self dateFromString:s]; return *o != nil; }
- (instancetype)initForRfc1123Formatting { [self setDateFormat:@"EEE, dd MMM yyyy HH:mm:ss 'GMT'"]; _utc = YES; return self; }
- (instancetype)initForUtcFormatting { [self setDateFormat:@"yyyy-MM-dd'T'HH:mm:ss'Z'"]; _utc = YES; return self; }
@end
@interface NSNumberFormatter : NSFormatter {
    NSUInteger _style;
}
@end
@implementation NSNumberFormatter
- (void)setNumberStyle:(NSUInteger)s { _style = s; }
- (NSUInteger)numberStyle { return _style; }
- (void)setLocale:(id)l { (void)l; }
- (void)setFormatterBehavior:(NSUInteger)b { (void)b; }
- (void)setMaximumFractionDigits:(NSUInteger)n { (void)n; }
- (void)setMinimumFractionDigits:(NSUInteger)n { (void)n; }
- (void)setGroupingSeparator:(NSString*)s { (void)s; }
- (void)setUsesGroupingSeparator:(BOOL)b { (void)b; }
- (void)setCurrencyCode:(NSString*)c { (void)c; }
- (NSString*)stringFromNumber:(NSNumber*)n {
    if (_style == 2) return [NSString stringWithFormat:@"$%.2f", [n doubleValue]];
    return [n stringValue];
}
- (NSNumber*)numberFromString:(NSString*)s {
    const char* p = [s UTF8String];
    char* e = NULL;
    double d = strtod(p, &e);
    if (e == p) return nil;
    if (!strchr(p, '.') && !strchr(p, 'e')) return [NSNumber numberWithLongLong:strtoll(p, NULL, 10)];
    return [NSNumber numberWithDouble:d];
}
- (BOOL)getObjectValue:(id*)o forString:(NSString*)s range:(NSRange*)r error:(NSError**)e { (void)r; (void)e; *o = [self numberFromString:s]; return *o != nil; }
@end

// ===================================================================== archivers (not supported)
@interface NSCoder : NSObject
@end
@implementation NSCoder
- (BOOL)allowsKeyedCoding { return YES; }
- (BOOL)containsValueForKey:(NSString*)k { (void)k; return NO; }
- (id)decodeObjectForKey:(NSString*)k { (void)k; return nil; }
- (id)decodeObjectOfClass:(Class)c forKey:(NSString*)k { (void)c; (void)k; return nil; }
- (id)decodeObjectOfClasses:(NSSet*)c forKey:(NSString*)k { (void)c; (void)k; return nil; }
- (BOOL)decodeBoolForKey:(NSString*)k { (void)k; return NO; }
- (int)decodeIntForKey:(NSString*)k { (void)k; return 0; }
- (int32_t)decodeInt32ForKey:(NSString*)k { (void)k; return 0; }
- (int64_t)decodeInt64ForKey:(NSString*)k { (void)k; return 0; }
- (NSInteger)decodeIntegerForKey:(NSString*)k { (void)k; return 0; }
- (double)decodeDoubleForKey:(NSString*)k { (void)k; return 0; }
- (float)decodeFloatForKey:(NSString*)k { (void)k; return 0; }
- (CGRect)decodeCGRectForKey:(NSString*)k { (void)k; return CGRectMake(0, 0, 0, 0); }
- (void)encodeObject:(id)o forKey:(NSString*)k { (void)o; (void)k; }
- (void)encodeBool:(BOOL)b forKey:(NSString*)k { (void)b; (void)k; }
- (void)encodeInt:(int)i forKey:(NSString*)k { (void)i; (void)k; }
- (void)encodeInteger:(NSInteger)i forKey:(NSString*)k { (void)i; (void)k; }
- (void)encodeInt64:(int64_t)i forKey:(NSString*)k { (void)i; (void)k; }
- (void)encodeDouble:(double)d forKey:(NSString*)k { (void)d; (void)k; }
- (void)encodeFloat:(float)f forKey:(NSString*)k { (void)f; (void)k; }
- (BOOL)requiresSecureCoding { return NO; }
@end
// NSKeyedArchiver / NSKeyedUnarchiver for property-list objects (strings, numbers, data, dates,
// arrays, dictionaries) in Apple's keyed archive format ($archiver / $objects / $top, written as
// an XML property list; UIDs as {CF$UID: n}). The game keeps its keychain items (the PlayFab
// account of the signed-out player...) as archives of strings: empty archives lost them.
static NSDictionary* ka_uid(NSUInteger i) { return [NSDictionary dictionaryWithObject:[NSNumber numberWithUnsignedInteger:i] forKey:@"CF$UID"]; }
static NSUInteger ka_class(NSMutableArray* objs, NSMutableDictionary* classes, NSString* name, NSArray* chain) {
    NSNumber* have = [classes objectForKey:name];
    if (have) return [have unsignedIntegerValue];
    NSUInteger i = [objs count];
    [objs addObject:[NSDictionary dictionaryWithObjectsAndKeys:name, @"$classname", chain, @"$classes", nil]];
    [classes setObject:[NSNumber numberWithUnsignedInteger:i] forKey:name];
    return i;
}
static NSUInteger ka_encode(id o, NSMutableArray* objs, NSMutableDictionary* classes) {
    if (!o || o == (id)[NSNull null]) return 0;  // $null
    if ([o isKindOfClass:[NSString class]] || [o isKindOfClass:[NSNumber class]]) {
        [objs addObject:[[o copy] autorelease]];
        return [objs count] - 1;
    }
    NSUInteger me = [objs count];
    [objs addObject:[NSNull null]];  // (placeholder: the object's slot comes before its members)
    NSMutableDictionary* d = [NSMutableDictionary dictionary];
    if ([o isKindOfClass:[NSData class]]) {
        [d setObject:[[o copy] autorelease] forKey:@"NS.data"];
        [d setObject:ka_uid(ka_class(objs, classes, @"NSData", @[@"NSData", @"NSObject"])) forKey:@"$class"];
    } else if ([o isKindOfClass:[NSDate class]]) {
        [d setObject:[NSNumber numberWithDouble:[o timeIntervalSinceReferenceDate]] forKey:@"NS.time"];
        [d setObject:ka_uid(ka_class(objs, classes, @"NSDate", @[@"NSDate", @"NSObject"])) forKey:@"$class"];
    } else if ([o isKindOfClass:[NSArray class]]) {
        NSMutableArray* items = [NSMutableArray array];
        for (id x in o) [items addObject:ka_uid(ka_encode(x, objs, classes))];
        [d setObject:items forKey:@"NS.objects"];
        [d setObject:ka_uid(ka_class(objs, classes, @"NSArray", @[@"NSArray", @"NSObject"])) forKey:@"$class"];
    } else if ([o isKindOfClass:[NSDictionary class]]) {
        NSMutableArray *keys = [NSMutableArray array], *vals = [NSMutableArray array];
        for (id k in o) {
            [keys addObject:ka_uid(ka_encode(k, objs, classes))];
            [vals addObject:ka_uid(ka_encode([o objectForKey:k], objs, classes))];
        }
        [d setObject:keys forKey:@"NS.keys"];
        [d setObject:vals forKey:@"NS.objects"];
        [d setObject:ka_uid(ka_class(objs, classes, @"NSDictionary", @[@"NSDictionary", @"NSObject"])) forKey:@"$class"];
    } else {
        static NSMutableSet* reported;  // (once per class)
        if (!reported) reported = [[NSMutableSet alloc] init];
        NSString* cn = NSStringFromClass([o class]);
        if (cn && ![reported containsObject:cn]) {
            [reported addObject:cn];
            NSLog(@"[Foundation] NSKeyedArchiver: %@ objects are not supported (archived as nil)", cn);
        }
        [objs replaceObjectAtIndex:me withObject:@"$null"];
        return 0;
    }
    [objs replaceObjectAtIndex:me withObject:d];
    return me;
}
static NSData* ka_archive(id root) {
    NSMutableArray* objs = [NSMutableArray arrayWithObject:@"$null"];
    NSMutableDictionary* classes = [NSMutableDictionary dictionary];
    NSUInteger r = ka_encode(root, objs, classes);
    NSDictionary* top = [NSDictionary dictionaryWithObject:ka_uid(r) forKey:@"root"];
    NSDictionary* a = [NSDictionary dictionaryWithObjectsAndKeys:@"NSKeyedArchiver", @"$archiver", [NSNumber numberWithInt:100000], @"$version",
                                                                 top, @"$top", objs, @"$objects", nil];
    return _emu_plist_xml(a);
}
static NSInteger ka_uid_of(id v) {
    if ([v isKindOfClass:[NSDictionary class]] && [v objectForKey:@"CF$UID"]) return [[v objectForKey:@"CF$UID"] integerValue];
    return -1;
}
static id ka_decode(NSArray* objs, NSInteger i, int depth) {
    if (i <= 0 || (NSUInteger)i >= [objs count] || depth > 64) return nil;
    id o = [objs objectAtIndex:(NSUInteger)i];
    if (![o isKindOfClass:[NSDictionary class]]) return [o isEqual:@"$null"] ? nil : o;
    NSInteger ci = ka_uid_of([o objectForKey:@"$class"]);
    NSDictionary* cls = ci > 0 && (NSUInteger)ci < [objs count] ? [objs objectAtIndex:(NSUInteger)ci] : nil;
    NSString* name = [cls isKindOfClass:[NSDictionary class]] ? [cls objectForKey:@"$classname"] : nil;
    if ([name hasSuffix:@"Data"]) return [o objectForKey:@"NS.data"];
    if ([name hasSuffix:@"Date"]) return [NSDate dateWithTimeIntervalSinceReferenceDate:[[o objectForKey:@"NS.time"] doubleValue]];
    if ([name hasSuffix:@"String"]) return [o objectForKey:@"NS.string"];
    if ([name hasSuffix:@"Array"] || [name hasSuffix:@"Set"]) {
        NSMutableArray* a = [NSMutableArray array];
        for (id u in [o objectForKey:@"NS.objects"]) {
            id x = ka_decode(objs, ka_uid_of(u), depth + 1);
            if (x) [a addObject:x];
        }
        return a;
    }
    if ([name hasSuffix:@"Dictionary"]) {
        NSArray *ks = [o objectForKey:@"NS.keys"], *vs = [o objectForKey:@"NS.objects"];
        NSMutableDictionary* d = [NSMutableDictionary dictionary];
        for (NSUInteger k = 0; k < [ks count] && k < [vs count]; k++) {
            id key = ka_decode(objs, ka_uid_of([ks objectAtIndex:k]), depth + 1);
            id val = ka_decode(objs, ka_uid_of([vs objectAtIndex:k]), depth + 1);
            if (key && val) [d setObject:val forKey:key];
        }
        return d;
    }
    NSLog(@"[Foundation] NSKeyedUnarchiver: %@ objects are not supported", name);
    return nil;
}
static id ka_unarchive(NSData* data) {
    if (![data length]) return nil;
    id p = _emu_plist_parse(data);
    if (![p isKindOfClass:[NSDictionary class]]) return nil;
    NSArray* objs = [p objectForKey:@"$objects"];
    NSDictionary* top = [p objectForKey:@"$top"];
    if (![objs isKindOfClass:[NSArray class]] || ![top isKindOfClass:[NSDictionary class]]) return nil;
    id root = [top objectForKey:@"root"];
    if (!root) root = [[top allValues] firstObject];
    return ka_decode(objs, ka_uid_of(root), 0);
}

@interface NSKeyedArchiver : NSCoder
@end
@implementation NSKeyedArchiver
+ (NSData*)archivedDataWithRootObject:(id)o { return ka_archive(o); }
+ (NSData*)archivedDataWithRootObject:(id)o requiringSecureCoding:(BOOL)s error:(NSError**)e {
    (void)s;
    if (e) *e = nil;
    return ka_archive(o);
}
+ (BOOL)archiveRootObject:(id)o toFile:(NSString*)p { return [ka_archive(o) writeToFile:p atomically:YES]; }
+ (void)setClassName:(NSString*)n forClass:(Class)c { (void)n; (void)c; }
- (instancetype)initForWritingWithMutableData:(NSMutableData*)d { (void)d; return self; }
- (instancetype)initRequiringSecureCoding:(BOOL)b { (void)b; return self; }
- (void)finishEncoding {}
- (void)setRequiresSecureCoding:(BOOL)b { (void)b; }
@end
@interface NSKeyedUnarchiver : NSCoder
@end
@implementation NSKeyedUnarchiver
+ (id)unarchiveObjectWithData:(NSData*)d { return ka_unarchive(d); }
+ (id)unarchiveObjectWithFile:(NSString*)p { return ka_unarchive([NSData dataWithContentsOfFile:p]); }
+ (id)unarchiveTopLevelObjectWithData:(NSData*)d error:(NSError**)e { if (e) *e = nil; return ka_unarchive(d); }
+ (id)unarchivedObjectOfClass:(Class)c fromData:(NSData*)d error:(NSError**)e { (void)c; if (e) *e = nil; return ka_unarchive(d); }
+ (id)unarchivedObjectOfClasses:(NSSet*)c fromData:(NSData*)d error:(NSError**)e { (void)c; if (e) *e = nil; return ka_unarchive(d); }
+ (void)setClass:(Class)c forClassName:(NSString*)n { (void)c; (void)n; }
- (instancetype)initForReadingWithData:(NSData*)d { (void)d; return self; }
- (instancetype)initForReadingFromData:(NSData*)d error:(NSError**)e { (void)d; if (e) *e = nil; return self; }
- (void)finishDecoding {}
- (void)setRequiresSecureCoding:(BOOL)b { (void)b; }
@end

// ===================================================================== NSIndexPath / NSIndexSet
@interface NSIndexPath : NSObject <NSCopying> {
    NSUInteger* _idx;
    NSUInteger _n;
}
@end
@implementation NSIndexPath
+ (instancetype)indexPathWithIndexes:(const NSUInteger*)i length:(NSUInteger)n {
    NSIndexPath* p = [[[self alloc] init] autorelease];
    p->_idx = malloc((n + 1) * sizeof(NSUInteger));
    memcpy(p->_idx, i, n * sizeof(NSUInteger));
    p->_n = n;
    return p;
}
+ (instancetype)indexPathWithIndex:(NSUInteger)i { return [self indexPathWithIndexes:&i length:1]; }
+ (instancetype)indexPathForRow:(NSInteger)r inSection:(NSInteger)s { NSUInteger i[2] = {(NSUInteger)s, (NSUInteger)r}; return [self indexPathWithIndexes:i length:2]; }
+ (instancetype)indexPathForItem:(NSInteger)r inSection:(NSInteger)s { return [self indexPathForRow:r inSection:s]; }
- (void)dealloc { free(_idx); [super dealloc]; }
- (id)copyWithZone:(NSZone*)z { (void)z; return [self retain]; }
- (NSUInteger)length { return _n; }
- (NSUInteger)indexAtPosition:(NSUInteger)p { return p < _n ? _idx[p] : (NSUInteger)NSNotFound; }
- (NSInteger)section { return _n > 0 ? (NSInteger)_idx[0] : 0; }
- (NSInteger)row { return _n > 1 ? (NSInteger)_idx[1] : 0; }
- (NSInteger)item { return [self row]; }
- (BOOL)isEqual:(id)o {
    if (![o isKindOfClass:[NSIndexPath class]]) return NO;
    NSIndexPath* x = o;
    return x->_n == _n && !memcmp(x->_idx, _idx, _n * sizeof(NSUInteger));
}
- (NSUInteger)hash { return _n ? _idx[_n - 1] : 0; }
@end
@interface NSIndexSet : NSObject <NSCopying> {
    NSMutableArray* _a;
}
@end
@implementation NSIndexSet
+ (instancetype)indexSet { return [[[self alloc] init] autorelease]; }
+ (instancetype)indexSetWithIndex:(NSUInteger)i { NSIndexSet* s = [self indexSet]; [s->_a addObject:[NSNumber numberWithUnsignedInteger:i]]; return s; }
+ (instancetype)indexSetWithIndexesInRange:(NSRange)r {
    NSIndexSet* s = [self indexSet];
    for (NSUInteger i = r.location; i < NSMaxRange(r); i++) [s->_a addObject:[NSNumber numberWithUnsignedInteger:i]];
    return s;
}
- (instancetype)init { _a = [[NSMutableArray alloc] init]; return self; }
- (void)dealloc { [_a release]; [super dealloc]; }
- (id)copyWithZone:(NSZone*)z { (void)z; return [self retain]; }
- (NSUInteger)count { return [_a count]; }
- (NSUInteger)firstIndex { return [_a count] ? [[_a objectAtIndex:0] unsignedIntegerValue] : (NSUInteger)NSNotFound; }
- (BOOL)containsIndex:(NSUInteger)i { return [_a containsObject:[NSNumber numberWithUnsignedInteger:i]]; }
- (void)addIndex:(NSUInteger)i { if (![self containsIndex:i]) [_a addObject:[NSNumber numberWithUnsignedInteger:i]]; }
- (void)enumerateIndexesUsingBlock:(void (^)(NSUInteger, BOOL*))b {
    BOOL stop = NO;
    for (NSNumber* n in _a) { b([n unsignedIntegerValue], &stop); if (stop) break; }
}
@end
@interface NSMutableIndexSet : NSIndexSet
@end
@implementation NSMutableIndexSet
@end

// ===================================================================== small stubs
@interface NSPredicate : NSObject
@end
@implementation NSPredicate
+ (NSPredicate*)predicateWithFormat:(NSString*)f, ... { (void)f; return [[[self alloc] init] autorelease]; }
+ (NSPredicate*)predicateWithBlock:(BOOL (^)(id, NSDictionary*))b { (void)b; return [[[self alloc] init] autorelease]; }
- (BOOL)evaluateWithObject:(id)o { (void)o; return NO; }
@end
@interface NSRegularExpression : NSObject
@end
@implementation NSRegularExpression
+ (instancetype)regularExpressionWithPattern:(NSString*)p options:(NSUInteger)o error:(NSError**)e { (void)p; (void)o; if (e) *e = nil; return [[[self alloc] init] autorelease]; }
- (NSString*)stringByReplacingMatchesInString:(NSString*)s options:(NSUInteger)o range:(NSRange)r withTemplate:(NSString*)t { (void)o; (void)r; (void)t; return s; }
- (NSUInteger)numberOfMatchesInString:(NSString*)s options:(NSUInteger)o range:(NSRange)r { (void)s; (void)o; (void)r; return 0; }
- (NSArray*)matchesInString:(NSString*)s options:(NSUInteger)o range:(NSRange)r { (void)s; (void)o; (void)r; return [NSArray array]; }
- (id)firstMatchInString:(NSString*)s options:(NSUInteger)o range:(NSRange)r { (void)s; (void)o; (void)r; return nil; }
@end
@interface NSXMLParser : NSObject {
    id _delegate;
}
@end
@implementation NSXMLParser
- (instancetype)initWithData:(NSData*)d { (void)d; return self; }
- (void)setDelegate:(id)d { _delegate = d; }
- (id)delegate { return _delegate; }
- (void)setShouldProcessNamespaces:(BOOL)b { (void)b; }
- (void)setShouldReportNamespacePrefixes:(BOOL)b { (void)b; }
- (void)setShouldResolveExternalEntities:(BOOL)b { (void)b; }
- (BOOL)parse { return NO; }
- (void)abortParsing {}
- (NSError*)parserError { return [NSError errorWithDomain:@"NSXMLParserErrorDomain" code:1 userInfo:nil]; }
- (NSInteger)lineNumber { return 0; }
- (NSInteger)columnNumber { return 0; }
@end
@interface NSMethodSignature : NSObject
@end
@implementation NSMethodSignature
+ (NSMethodSignature*)signatureWithObjCTypes:(const char*)t { (void)t; return [[[self alloc] init] autorelease]; }
- (NSUInteger)numberOfArguments { return 2; }
- (const char*)methodReturnType { return "v"; }
- (NSUInteger)methodReturnLength { return 0; }
@end
@interface NSInvocation : NSObject
@end
@implementation NSInvocation
+ (NSInvocation*)invocationWithMethodSignature:(id)s { (void)s; return [[[self alloc] init] autorelease]; }
- (void)setTarget:(id)t { (void)t; }
- (void)setSelector:(SEL)s { (void)s; }
- (void)setArgument:(void*)a atIndex:(NSInteger)i { (void)a; (void)i; }
- (void)getReturnValue:(void*)r { (void)r; }
- (void)invoke { NSLog(@"NSInvocation -invoke is not supported"); }
- (void)retainArguments {}
@end
@interface NSCache : NSObject {
    NSMutableDictionary* _d;
}
@end
@implementation NSCache
- (instancetype)init { _d = [[NSMutableDictionary alloc] init]; return self; }
- (void)dealloc { [_d release]; [super dealloc]; }
- (id)objectForKey:(id)k { return [_d objectForKey:k]; }
- (void)setObject:(id)o forKey:(id)k { [_d setObject:o forKey:k]; }
- (void)setObject:(id)o forKey:(id)k cost:(NSUInteger)c { (void)c; [_d setObject:o forKey:k]; }
- (void)removeObjectForKey:(id)k { [_d removeObjectForKey:k]; }
- (void)removeAllObjects { [_d removeAllObjects]; }
- (void)setCountLimit:(NSUInteger)n { (void)n; }
- (void)setTotalCostLimit:(NSUInteger)n { (void)n; }
- (void)setName:(NSString*)n { (void)n; }
@end
@interface NSLock : NSObject {
    pthread_mutex_t _m;
}
@end
@implementation NSLock
- (instancetype)init { pthread_mutex_init(&_m, NULL); return self; }
- (void)lock { pthread_mutex_lock(&_m); }
- (void)unlock { pthread_mutex_unlock(&_m); }
- (BOOL)tryLock { return pthread_mutex_trylock(&_m) == 0; }
- (void)setName:(NSString*)n { (void)n; }
@end
@interface NSRecursiveLock : NSObject {
    pthread_mutex_t _m;
}
@end
@implementation NSRecursiveLock
- (instancetype)init {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&_m, &a);
    return self;
}
- (void)lock { pthread_mutex_lock(&_m); }
- (void)unlock { pthread_mutex_unlock(&_m); }
- (BOOL)tryLock { return pthread_mutex_trylock(&_m) == 0; }
@end
@interface NSCondition : NSObject {
    pthread_mutex_t _m;
    pthread_cond_t _c;
}
@end
@implementation NSCondition
- (instancetype)init { pthread_mutex_init(&_m, NULL); pthread_cond_init(&_c, NULL); return self; }
- (void)lock { pthread_mutex_lock(&_m); }
- (void)unlock { pthread_mutex_unlock(&_m); }
- (void)wait { pthread_cond_wait(&_c, &_m); }
- (BOOL)waitUntilDate:(NSDate*)d {
    struct timespec ts;
    double t = [d timeIntervalSince1970];
    ts.tv_sec = (time_t)t;
    ts.tv_nsec = (long)((t - (double)ts.tv_sec) * 1e9);
    return pthread_cond_timedwait(&_c, &_m, &ts) == 0;
}
- (void)signal { pthread_cond_signal(&_c); }
- (void)broadcast { pthread_cond_broadcast(&_c); }
@end
