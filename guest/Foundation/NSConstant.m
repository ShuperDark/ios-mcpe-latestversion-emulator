// Compile-time collection / number literals of current clang (iOS 14+ SDKs):
//   @[ ... ]  -> NSConstantArray      {isa; NSUInteger count; const id* objects;}       (__objc_arrayobj)
//   @{ ... }  -> NSConstantDictionary {isa; NSUInteger options; NSUInteger count; const id* keys; const id* objects;} (__objc_dictobj)
//   @123      -> NSConstantIntegerNumber {isa; const char* encoding; long long value;} (__objc_intobj)
//   @1.5      -> NSConstantDoubleNumber / NSConstantFloatNumber {isa; double / float value;}
//   @[] / @{} -> &__NSArray0__struct / &__NSDictionary0__struct (or the __NSArray0__ / __NSDictionary0__ pointers)
// The objects live in the image's data (never freed; retain/release ignore them).
//
// NSArray keeps _count and _items as its first ivars, so NSConstantArray is just an
// NSArray. Integer numbers are rewritten by the emulator after binding into NSNumber's
// layout (emulator: fix_constant_literals). Dictionaries and floating point numbers
// forward to a real object created on first use.
#import <emu/Foundation.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

@interface NSConstantArray : NSArray
@end
@implementation NSConstantArray
- (id)retain { return self; }
- (oneway void)release {}
- (id)autorelease { return self; }
- (NSUInteger)retainCount { return (NSUInteger)-1 >> 1; }
- (void)dealloc {}
- (id)copyWithZone:(NSZone*)z { (void)z; return self; }
- (id)copy { return self; }
@end

// ---- side table: literal -> real object ----
typedef struct backing { const void* key; id obj; struct backing* next; } backing;
static backing* g_backing;
static pthread_mutex_t g_backing_lock = PTHREAD_MUTEX_INITIALIZER;
static id backing_get(const void* k, id (*make)(const void*)) {
    pthread_mutex_lock(&g_backing_lock);
    for (backing* b = g_backing; b; b = b->next)
        if (b->key == k) { id o = b->obj; pthread_mutex_unlock(&g_backing_lock); return o; }
    pthread_mutex_unlock(&g_backing_lock);
    id o = make(k);
    pthread_mutex_lock(&g_backing_lock);
    backing* b = malloc(sizeof *b);
    b->key = k; b->obj = o; b->next = g_backing; g_backing = b;
    pthread_mutex_unlock(&g_backing_lock);
    return o;
}

typedef struct { Class isa; unsigned long options, count; const id* keys; const id* objects; } const_dict;
static id make_dict(const void* p) {
    const const_dict* d = p;
    return [[NSDictionary alloc] initWithObjects:d->objects forKeys:d->keys count:d->count];
}

@interface NSConstantDictionary : NSDictionary
@end
@implementation NSConstantDictionary
- (NSDictionary*)_real { return backing_get(self, make_dict); }
- (id)retain { return self; }
- (oneway void)release {}
- (id)autorelease { return self; }
- (NSUInteger)retainCount { return (NSUInteger)-1 >> 1; }
- (void)dealloc {}
- (id)copyWithZone:(NSZone*)z { (void)z; return self; }
- (id)copy { return self; }
- (id)mutableCopyWithZone:(NSZone*)z { return [[self _real] mutableCopyWithZone:z]; }
- (id)mutableCopy { return [[self _real] mutableCopy]; }
- (NSUInteger)count { return ((const_dict*)self)->count; }
- (id)objectForKey:(id)k { return [[self _real] objectForKey:k]; }
- (id)objectForKeyedSubscript:(id)k { return [[self _real] objectForKey:k]; }
- (id)valueForKey:(NSString*)k { return [[self _real] objectForKey:k]; }
- (NSArray*)allKeys { return [[self _real] allKeys]; }
- (NSArray*)allValues { return [[self _real] allValues]; }
- (NSEnumerator*)keyEnumerator { return [[self _real] keyEnumerator]; }
- (NSEnumerator*)objectEnumerator { return [[self _real] objectEnumerator]; }
- (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState*)st objects:(id*)buf count:(NSUInteger)n {
    return [[self _real] countByEnumeratingWithState:st objects:buf count:n];
}
- (void)enumerateKeysAndObjectsUsingBlock:(void (^)(id, id, BOOL*))b { [[self _real] enumerateKeysAndObjectsUsingBlock:b]; }
- (BOOL)isEqual:(id)o { return o == self || [[self _real] isEqual:o]; }
- (BOOL)isEqualToDictionary:(NSDictionary*)o { return [[self _real] isEqual:o]; }
- (NSString*)description { return [[self _real] description]; }
- (id)forwardingTargetForSelector:(SEL)s { (void)s; return [self _real]; }
@end

// The emulator rewrites the integer literals into NSNumber's own layout before any code runs.
@interface NSConstantIntegerNumber : NSNumber
@end
@implementation NSConstantIntegerNumber
- (id)retain { return self; }
- (oneway void)release {}
- (id)autorelease { return self; }
- (NSUInteger)retainCount { return (NSUInteger)-1 >> 1; }
- (void)dealloc {}
- (id)copyWithZone:(NSZone*)z { (void)z; return self; }
@end

// Floating point literals have no room for NSNumber's type byte: real NSNumber on first use.
typedef struct { Class isa; double v; } const_double;
typedef struct { Class isa; float v; } const_float;
static id make_double(const void* p) { return [[NSNumber alloc] initWithDouble:((const const_double*)p)->v]; }
static id make_float(const void* p) { return [[NSNumber alloc] initWithFloat:((const const_float*)p)->v]; }

#define CONST_FP_NUMBER(CLS, MAKE)                                                              \
    @interface CLS : NSNumber                                                                    \
    @end                                                                                         \
    @implementation CLS                                                                          \
    - (NSNumber*)_real { return backing_get(self, MAKE); }                                       \
    - (id)retain { return self; }                                                                \
    - (oneway void)release {}                                                                    \
    - (id)autorelease { return self; }                                                           \
    - (void)dealloc {}                                                                           \
    - (id)copyWithZone:(NSZone*)z { (void)z; return self; }                                      \
    - (double)doubleValue { return [[self _real] doubleValue]; }                                 \
    - (float)floatValue { return [[self _real] floatValue]; }                                    \
    - (int)intValue { return [[self _real] intValue]; }                                          \
    - (long long)longLongValue { return [[self _real] longLongValue]; }                          \
    - (NSInteger)integerValue { return [[self _real] integerValue]; }                            \
    - (NSUInteger)unsignedIntegerValue { return [[self _real] unsignedIntegerValue]; }           \
    - (unsigned)unsignedIntValue { return [[self _real] unsignedIntValue]; }                     \
    - (BOOL)boolValue { return [[self _real] boolValue]; }                                       \
    - (const char*)objCType { return [[self _real] objCType]; }                                  \
    - (NSString*)stringValue { return [[self _real] stringValue]; }                              \
    - (NSString*)description { return [[self _real] description]; }                             \
    - (BOOL)isEqual:(id)o { return o == self || [[self _real] isEqual:o]; }                      \
    - (BOOL)isEqualToNumber:(NSNumber*)o { return [[self _real] isEqual:o]; }                    \
    - (NSComparisonResult)compare:(NSNumber*)o { return [[self _real] compare:o]; }             \
    - (NSUInteger)hash { return [[self _real] hash]; }                                           \
    - (id)forwardingTargetForSelector:(SEL)s { (void)s; return [self _real]; }                   \
    @end
CONST_FP_NUMBER(NSConstantDoubleNumber, make_double)
CONST_FP_NUMBER(NSConstantFloatNumber, make_float)

// ---- empty literals ----
EMU_EXPORT struct { Class isa; unsigned long count; const id* items; unsigned long pad[4]; } __NSArray0__struct;
EMU_EXPORT struct { Class isa; unsigned long options, count; const id* keys; const id* objects; unsigned long pad[2]; } __NSDictionary0__struct;
EMU_EXPORT id __NSArray0__ = (id)&__NSArray0__struct;
EMU_EXPORT id __NSDictionary0__ = (id)&__NSDictionary0__struct;

__attribute__((constructor)) static void init_empty_literals(void) {
    __NSArray0__struct.isa = [NSConstantArray class];
    __NSDictionary0__struct.isa = [NSConstantDictionary class];
}
