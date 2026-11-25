// Metal, MetalKit (MTKView) and CAMetalLayer for the game's renderer (bgfx's Metal backend).
// The objects are thin wrappers of the emulator's implementation on OpenGL
// (emu/metal_host.h, hle_metal.cpp); descriptors are plain data objects.
#import <emu/UIKit.h>
#include <emu/host.h>
#include <emu/metal_host.h>

#include <float.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <emu/msg.h>

typedef struct { NSUInteger x, y, z; } MTLOrigin;
typedef struct { NSUInteger width, height, depth; } MTLSize;
typedef struct { MTLOrigin origin; MTLSize size; } MTLRegion;
typedef struct { double originX, originY, width, height, znear, zfar; } MTLViewport;
typedef struct { NSUInteger x, y, width, height; } MTLScissorRect;
typedef struct { double red, green, blue, alpha; } MTLClearColor;
typedef struct { uint8_t red, green, blue, alpha; } MTLTextureSwizzleChannels;
typedef struct { NSUInteger location, length; } MTLRange_;

EMU_EXPORT NSString* const MTLCommandBufferErrorDomain = @"MTLCommandBufferErrorDomain";
EMU_EXPORT NSString* const MTLLibraryErrorDomain = @"MTLLibraryErrorDomain";

static void* emu_alloc_copy(const void* p, size_t n) {
    void* r = malloc(n);
    memcpy(r, p, n);
    return r;
}

// ===================================================================== base classes
@interface _MTLObject : NSObject {
@public
    NSString* _label;
    id _device;
}
@end
@implementation _MTLObject
- (void)dealloc { [_label release]; [super dealloc]; }
- (NSString*)label { return _label; }
- (void)setLabel:(NSString*)l { [_label release]; _label = [l copy]; }
- (id)device { return _device; }
@end

// ===================================================================== descriptors
#define PROP_OBJ(TYPE, NAME, SETTER)                                                 \
    -(TYPE)NAME { return _##NAME; }                                                  \
    -(void)SETTER:(TYPE)v { if (v != _##NAME) { [_##NAME release]; _##NAME = [v retain]; } }

@interface MTLTextureDescriptor : NSObject <NSCopying> {
@public
    NSUInteger _textureType, _pixelFormat, _width, _height, _depth, _mipmapLevelCount, _sampleCount, _arrayLength;
    NSUInteger _resourceOptions, _cpuCacheMode, _storageMode, _hazardTrackingMode, _usage, _compressionType;
    BOOL _allowGPUOptimizedContents;
    MTLTextureSwizzleChannels _swizzle;
}
@end
@implementation MTLTextureDescriptor
- (instancetype)init {
    _textureType = 2;
    _pixelFormat = 70;
    _width = _height = _depth = 1;
    _mipmapLevelCount = _sampleCount = _arrayLength = 1;
    _usage = 1;
    _allowGPUOptimizedContents = YES;
    _swizzle = (MTLTextureSwizzleChannels){2, 3, 4, 5};
    return self;
}
+ (instancetype)texture2DDescriptorWithPixelFormat:(NSUInteger)f width:(NSUInteger)w height:(NSUInteger)h mipmapped:(BOOL)m {
    MTLTextureDescriptor* d = [[[self alloc] init] autorelease];
    d->_pixelFormat = f;
    d->_width = w;
    d->_height = h;
    if (m) {
        NSUInteger s = w > h ? w : h, n = 1;
        while (s > 1) { s >>= 1; n++; }
        d->_mipmapLevelCount = n;
    }
    return d;
}
+ (instancetype)textureCubeDescriptorWithPixelFormat:(NSUInteger)f size:(NSUInteger)s mipmapped:(BOOL)m {
    MTLTextureDescriptor* d = [self texture2DDescriptorWithPixelFormat:f width:s height:s mipmapped:m];
    d->_textureType = 5;
    return d;
}
+ (instancetype)textureBufferDescriptorWithPixelFormat:(NSUInteger)f width:(NSUInteger)w resourceOptions:(NSUInteger)o usage:(NSUInteger)u {
    MTLTextureDescriptor* d = [self texture2DDescriptorWithPixelFormat:f width:w height:1 mipmapped:NO];
    d->_textureType = 9;
    d->_resourceOptions = o;
    d->_usage = u;
    return d;
}
- (id)copyWithZone:(NSZone*)z {
    MTLTextureDescriptor* d = [[MTLTextureDescriptor allocWithZone:z] init];
    memcpy((char*)d + sizeof(Class), (char*)self + sizeof(Class), class_getInstanceSize([MTLTextureDescriptor class]) - sizeof(Class));
    return d;
}
- (NSUInteger)textureType { return _textureType; }
- (void)setTextureType:(NSUInteger)v { _textureType = v; }
- (NSUInteger)pixelFormat { return _pixelFormat; }
- (void)setPixelFormat:(NSUInteger)v { _pixelFormat = v; }
- (NSUInteger)width { return _width; }
- (void)setWidth:(NSUInteger)v { _width = v; }
- (NSUInteger)height { return _height; }
- (void)setHeight:(NSUInteger)v { _height = v; }
- (NSUInteger)depth { return _depth; }
- (void)setDepth:(NSUInteger)v { _depth = v; }
- (NSUInteger)mipmapLevelCount { return _mipmapLevelCount; }
- (void)setMipmapLevelCount:(NSUInteger)v { _mipmapLevelCount = v; }
- (NSUInteger)sampleCount { return _sampleCount; }
- (void)setSampleCount:(NSUInteger)v { _sampleCount = v; }
- (NSUInteger)arrayLength { return _arrayLength; }
- (void)setArrayLength:(NSUInteger)v { _arrayLength = v; }
- (NSUInteger)resourceOptions { return _resourceOptions; }
- (void)setResourceOptions:(NSUInteger)v { _resourceOptions = v; _storageMode = (v >> 4) & 0xf; _cpuCacheMode = v & 0xf; }
- (NSUInteger)cpuCacheMode { return _cpuCacheMode; }
- (void)setCpuCacheMode:(NSUInteger)v { _cpuCacheMode = v; }
- (NSUInteger)storageMode { return _storageMode; }
- (void)setStorageMode:(NSUInteger)v { _storageMode = v; }
- (NSUInteger)hazardTrackingMode { return _hazardTrackingMode; }
- (void)setHazardTrackingMode:(NSUInteger)v { _hazardTrackingMode = v; }
- (NSUInteger)usage { return _usage; }
- (void)setUsage:(NSUInteger)v { _usage = v; }
- (BOOL)allowGPUOptimizedContents { return _allowGPUOptimizedContents; }
- (void)setAllowGPUOptimizedContents:(BOOL)v { _allowGPUOptimizedContents = v; }
- (MTLTextureSwizzleChannels)swizzle { return _swizzle; }
- (void)setSwizzle:(MTLTextureSwizzleChannels)v { _swizzle = v; }
- (NSUInteger)compressionType { return _compressionType; }
- (void)setCompressionType:(NSUInteger)v { _compressionType = v; }
@end

@interface MTLSamplerDescriptor : NSObject <NSCopying> {
@public
    NSUInteger _minFilter, _magFilter, _mipFilter, _maxAnisotropy, _s, _t, _r, _border, _compare;
    BOOL _normalized, _lodAverage, _supportArgumentBuffers;
    float _lodMin, _lodMax;
    NSString* _label;
}
@end
@implementation MTLSamplerDescriptor
- (instancetype)init {
    _maxAnisotropy = 1;
    _normalized = YES;
    _lodMax = FLT_MAX;
    return self;
}
- (void)dealloc { [_label release]; [super dealloc]; }
- (id)copyWithZone:(NSZone*)z {
    MTLSamplerDescriptor* d = [[MTLSamplerDescriptor allocWithZone:z] init];
    memcpy((char*)d + sizeof(Class), (char*)self + sizeof(Class), class_getInstanceSize([MTLSamplerDescriptor class]) - sizeof(Class));
    [d->_label retain];
    return d;
}
- (NSUInteger)minFilter { return _minFilter; }
- (void)setMinFilter:(NSUInteger)v { _minFilter = v; }
- (NSUInteger)magFilter { return _magFilter; }
- (void)setMagFilter:(NSUInteger)v { _magFilter = v; }
- (NSUInteger)mipFilter { return _mipFilter; }
- (void)setMipFilter:(NSUInteger)v { _mipFilter = v; }
- (NSUInteger)maxAnisotropy { return _maxAnisotropy; }
- (void)setMaxAnisotropy:(NSUInteger)v { _maxAnisotropy = v; }
- (NSUInteger)sAddressMode { return _s; }
- (void)setSAddressMode:(NSUInteger)v { _s = v; }
- (NSUInteger)tAddressMode { return _t; }
- (void)setTAddressMode:(NSUInteger)v { _t = v; }
- (NSUInteger)rAddressMode { return _r; }
- (void)setRAddressMode:(NSUInteger)v { _r = v; }
- (NSUInteger)borderColor { return _border; }
- (void)setBorderColor:(NSUInteger)v { _border = v; }
- (BOOL)normalizedCoordinates { return _normalized; }
- (void)setNormalizedCoordinates:(BOOL)v { _normalized = v; }
- (float)lodMinClamp { return _lodMin; }
- (void)setLodMinClamp:(float)v { _lodMin = v; }
- (float)lodMaxClamp { return _lodMax; }
- (void)setLodMaxClamp:(float)v { _lodMax = v; }
- (BOOL)lodAverage { return _lodAverage; }
- (void)setLodAverage:(BOOL)v { _lodAverage = v; }
- (NSUInteger)compareFunction { return _compare; }
- (void)setCompareFunction:(NSUInteger)v { _compare = v; }
- (BOOL)supportArgumentBuffers { return _supportArgumentBuffers; }
- (void)setSupportArgumentBuffers:(BOOL)v { _supportArgumentBuffers = v; }
- (NSString*)label { return _label; }
- (void)setLabel:(NSString*)l { [_label release]; _label = [l copy]; }
@end

@interface MTLStencilDescriptor : NSObject <NSCopying> {
@public
    NSUInteger _func, _fail, _depthFail, _pass;
    uint32_t _readMask, _writeMask;
}
@end
@implementation MTLStencilDescriptor
- (instancetype)init {
    _func = 7;
    _readMask = _writeMask = 0xffffffff;
    return self;
}
- (id)copyWithZone:(NSZone*)z {
    MTLStencilDescriptor* d = [[MTLStencilDescriptor allocWithZone:z] init];
    d->_func = _func; d->_fail = _fail; d->_depthFail = _depthFail; d->_pass = _pass;
    d->_readMask = _readMask; d->_writeMask = _writeMask;
    return d;
}
- (NSUInteger)stencilCompareFunction { return _func; }
- (void)setStencilCompareFunction:(NSUInteger)v { _func = v; }
- (NSUInteger)stencilFailureOperation { return _fail; }
- (void)setStencilFailureOperation:(NSUInteger)v { _fail = v; }
- (NSUInteger)depthFailureOperation { return _depthFail; }
- (void)setDepthFailureOperation:(NSUInteger)v { _depthFail = v; }
- (NSUInteger)depthStencilPassOperation { return _pass; }
- (void)setDepthStencilPassOperation:(NSUInteger)v { _pass = v; }
- (uint32_t)readMask { return _readMask; }
- (void)setReadMask:(uint32_t)v { _readMask = v; }
- (uint32_t)writeMask { return _writeMask; }
- (void)setWriteMask:(uint32_t)v { _writeMask = v; }
- (void)fill:(emu_mtl_stencil*)s {
    s->func = (uint32_t)_func; s->fail = (uint32_t)_fail; s->depth_fail = (uint32_t)_depthFail; s->pass = (uint32_t)_pass;
    s->read_mask = _readMask; s->write_mask = _writeMask;
}
@end

@interface MTLDepthStencilDescriptor : NSObject <NSCopying> {
@public
    NSUInteger _depthCompare;
    BOOL _depthWrite;
    MTLStencilDescriptor *_frontFaceStencil, *_backFaceStencil;
    NSString* _label;
}
@end
@implementation MTLDepthStencilDescriptor
- (instancetype)init { _depthCompare = 7; return self; }
- (void)dealloc { [_frontFaceStencil release]; [_backFaceStencil release]; [_label release]; [super dealloc]; }
- (id)copyWithZone:(NSZone*)z {
    MTLDepthStencilDescriptor* d = [[MTLDepthStencilDescriptor allocWithZone:z] init];
    d->_depthCompare = _depthCompare;
    d->_depthWrite = _depthWrite;
    d->_frontFaceStencil = [_frontFaceStencil copy];
    d->_backFaceStencil = [_backFaceStencil copy];
    return d;
}
- (NSUInteger)depthCompareFunction { return _depthCompare; }
- (void)setDepthCompareFunction:(NSUInteger)v { _depthCompare = v; }
- (BOOL)isDepthWriteEnabled { return _depthWrite; }
- (BOOL)depthWriteEnabled { return _depthWrite; }
- (void)setDepthWriteEnabled:(BOOL)v { _depthWrite = v; }
- (MTLStencilDescriptor*)frontFaceStencil { return _frontFaceStencil; }
- (void)setFrontFaceStencil:(MTLStencilDescriptor*)s { [_frontFaceStencil release]; _frontFaceStencil = [s copy]; }
- (MTLStencilDescriptor*)backFaceStencil { return _backFaceStencil; }
- (void)setBackFaceStencil:(MTLStencilDescriptor*)s { [_backFaceStencil release]; _backFaceStencil = [s copy]; }
- (NSString*)label { return _label; }
- (void)setLabel:(NSString*)l { [_label release]; _label = [l copy]; }
@end

// ---- indexed descriptor arrays (colorAttachments, attributes, layouts) ----
@interface _MTLDescArray : NSObject {
@public
    Class _cls;
    id _items[32];
}
- (instancetype)initWithClass:(Class)c;
@end
@implementation _MTLDescArray
- (instancetype)initWithClass:(Class)c { _cls = c; return self; }
- (void)dealloc {
    for (int i = 0; i < 32; i++) [_items[i] release];
    [super dealloc];
}
- (id)objectAtIndexedSubscript:(NSUInteger)i {
    if (getenv("EMU_MTL_TRACE")) NSLog(@"[Metal] array %p [%lu] cls %s", self, (unsigned long)i, class_getName(_cls));
    if (i >= 32) return nil;
    if (!_items[i]) _items[i] = [[_cls alloc] init];
    return _items[i];
}
- (void)setObject:(id)o atIndexedSubscript:(NSUInteger)i {
    if (i >= 32) return;
    [_items[i] release];
    _items[i] = [o copy];
}
- (id)existing:(NSUInteger)i { return i < 32 ? _items[i] : nil; }
- (id)copyArray {
    _MTLDescArray* a = [[[self class] alloc] initWithClass:_cls];
    for (int i = 0; i < 32; i++) a->_items[i] = [_items[i] copy];
    return a;
}
@end

@interface MTLRenderPipelineColorAttachmentDescriptor : NSObject <NSCopying> {
@public
    NSUInteger _pixelFormat, _srcRGB, _dstRGB, _rgbOp, _srcA, _dstA, _aOp, _writeMask;
    BOOL _blending;
}
@end
@implementation MTLRenderPipelineColorAttachmentDescriptor
- (instancetype)init { _srcRGB = _srcA = 1; _writeMask = 0xf; return self; }
- (id)copyWithZone:(NSZone*)z {
    MTLRenderPipelineColorAttachmentDescriptor* d = [[[self class] allocWithZone:z] init];
    memcpy((char*)d + sizeof(Class), (char*)self + sizeof(Class), class_getInstanceSize([self class]) - sizeof(Class));
    return d;
}
- (NSUInteger)pixelFormat { return _pixelFormat; }
- (void)setPixelFormat:(NSUInteger)v { _pixelFormat = v; }
- (BOOL)isBlendingEnabled { return _blending; }
- (BOOL)blendingEnabled { return _blending; }
- (void)setBlendingEnabled:(BOOL)v { _blending = v; }
- (NSUInteger)sourceRGBBlendFactor { return _srcRGB; }
- (void)setSourceRGBBlendFactor:(NSUInteger)v { _srcRGB = v; }
- (NSUInteger)destinationRGBBlendFactor { return _dstRGB; }
- (void)setDestinationRGBBlendFactor:(NSUInteger)v { _dstRGB = v; }
- (NSUInteger)rgbBlendOperation { return _rgbOp; }
- (void)setRgbBlendOperation:(NSUInteger)v { _rgbOp = v; }
- (NSUInteger)sourceAlphaBlendFactor { return _srcA; }
- (void)setSourceAlphaBlendFactor:(NSUInteger)v { _srcA = v; }
- (NSUInteger)destinationAlphaBlendFactor { return _dstA; }
- (void)setDestinationAlphaBlendFactor:(NSUInteger)v { _dstA = v; }
- (NSUInteger)alphaBlendOperation { return _aOp; }
- (void)setAlphaBlendOperation:(NSUInteger)v { _aOp = v; }
- (NSUInteger)writeMask { return _writeMask; }
- (void)setWriteMask:(NSUInteger)v { _writeMask = v; }
@end

@interface MTLVertexAttributeDescriptor : NSObject <NSCopying> {
@public
    NSUInteger _format, _offset, _bufferIndex;
}
@end
@implementation MTLVertexAttributeDescriptor
- (id)copyWithZone:(NSZone*)z {
    MTLVertexAttributeDescriptor* d = [[[self class] allocWithZone:z] init];
    d->_format = _format; d->_offset = _offset; d->_bufferIndex = _bufferIndex;
    return d;
}
- (NSUInteger)format { return _format; }
- (void)setFormat:(NSUInteger)v {
    _format = v;
    if (getenv("EMU_MTL_TRACE")) NSLog(@"[Metal] attr %p setFormat %lu", self, (unsigned long)v);
}
- (NSUInteger)offset { return _offset; }
- (void)setOffset:(NSUInteger)v { _offset = v; }
- (NSUInteger)bufferIndex { return _bufferIndex; }
- (void)setBufferIndex:(NSUInteger)v { _bufferIndex = v; }
@end

@interface MTLVertexBufferLayoutDescriptor : NSObject <NSCopying> {
@public
    NSUInteger _stride, _stepFunction, _stepRate;
}
@end
@implementation MTLVertexBufferLayoutDescriptor
- (instancetype)init { _stepFunction = 1; _stepRate = 1; return self; }
- (id)copyWithZone:(NSZone*)z {
    MTLVertexBufferLayoutDescriptor* d = [[[self class] allocWithZone:z] init];
    d->_stride = _stride; d->_stepFunction = _stepFunction; d->_stepRate = _stepRate;
    return d;
}
- (NSUInteger)stride { return _stride; }
- (void)setStride:(NSUInteger)v { _stride = v; }
- (NSUInteger)stepFunction { return _stepFunction; }
- (void)setStepFunction:(NSUInteger)v { _stepFunction = v; }
- (NSUInteger)stepRate { return _stepRate; }
- (void)setStepRate:(NSUInteger)v { _stepRate = v; }
@end

@interface MTLVertexDescriptor : NSObject <NSCopying> {
@public
    _MTLDescArray *_attributes, *_layouts;
}
@end
@implementation MTLVertexDescriptor
+ (instancetype)vertexDescriptor { return [[[self alloc] init] autorelease]; }
- (instancetype)init {
    _attributes = [[_MTLDescArray alloc] initWithClass:[MTLVertexAttributeDescriptor class]];
    _layouts = [[_MTLDescArray alloc] initWithClass:[MTLVertexBufferLayoutDescriptor class]];
    return self;
}
- (void)dealloc { [_attributes release]; [_layouts release]; [super dealloc]; }
- (id)copyWithZone:(NSZone*)z {
    MTLVertexDescriptor* d = [[[self class] allocWithZone:z] init];
    [d->_attributes release];
    [d->_layouts release];
    d->_attributes = [_attributes copyArray];
    d->_layouts = [_layouts copyArray];
    return d;
}
- (id)attributes {
    if (getenv("EMU_MTL_TRACE")) NSLog(@"[Metal] vd %p attributes -> %p", self, _attributes);
    return _attributes;
}
- (id)layouts { return _layouts; }
- (void)reset {
    [_attributes release];
    [_layouts release];
    _attributes = [[_MTLDescArray alloc] initWithClass:[MTLVertexAttributeDescriptor class]];
    _layouts = [[_MTLDescArray alloc] initWithClass:[MTLVertexBufferLayoutDescriptor class]];
}
@end

@interface _MTLFunction : _MTLObject {
@public
    uint64_t _h;
    NSString* _name;
}
@end

@interface MTLRenderPipelineDescriptor : NSObject <NSCopying> {
@public
    _MTLFunction *_vertexFunction, *_fragmentFunction;
    MTLVertexDescriptor* _vertexDescriptor;
    _MTLDescArray* _colorAttachments;
    NSUInteger _depthFormat, _stencilFormat, _sampleCount, _topology, _maxAmp;
    BOOL _alphaToCoverage, _alphaToOne, _rasterEnabled, _supportICB;
    NSString* _label;
}
@end
@implementation MTLRenderPipelineDescriptor
- (instancetype)init {
    _colorAttachments = [[_MTLDescArray alloc] initWithClass:[MTLRenderPipelineColorAttachmentDescriptor class]];
    _sampleCount = 1;
    _rasterEnabled = YES;
    _maxAmp = 1;
    return self;
}
- (void)dealloc {
    [_vertexFunction release]; [_fragmentFunction release]; [_vertexDescriptor release]; [_colorAttachments release]; [_label release];
    [super dealloc];
}
- (id)copyWithZone:(NSZone*)z {
    MTLRenderPipelineDescriptor* d = [[[self class] allocWithZone:z] init];
    [d->_colorAttachments release];
    d->_vertexFunction = [_vertexFunction retain];
    d->_fragmentFunction = [_fragmentFunction retain];
    d->_vertexDescriptor = [_vertexDescriptor copy];
    d->_colorAttachments = [_colorAttachments copyArray];
    d->_depthFormat = _depthFormat; d->_stencilFormat = _stencilFormat; d->_sampleCount = _sampleCount; d->_topology = _topology;
    d->_alphaToCoverage = _alphaToCoverage; d->_alphaToOne = _alphaToOne; d->_rasterEnabled = _rasterEnabled;
    d->_label = [_label copy];
    return d;
}
- (void)reset {
    [_vertexFunction release]; _vertexFunction = nil;
    [_fragmentFunction release]; _fragmentFunction = nil;
    [_vertexDescriptor release]; _vertexDescriptor = nil;
    [_colorAttachments release];
    _colorAttachments = [[_MTLDescArray alloc] initWithClass:[MTLRenderPipelineColorAttachmentDescriptor class]];
    _depthFormat = _stencilFormat = 0;
    _sampleCount = 1;
    _alphaToCoverage = _alphaToOne = NO;
    _rasterEnabled = YES;
}
PROP_OBJ(_MTLFunction*, vertexFunction, setVertexFunction)
PROP_OBJ(_MTLFunction*, fragmentFunction, setFragmentFunction)
- (MTLVertexDescriptor*)vertexDescriptor {
    if (!_vertexDescriptor) _vertexDescriptor = [[MTLVertexDescriptor alloc] init];
    return _vertexDescriptor;
}
- (void)setVertexDescriptor:(MTLVertexDescriptor*)v {
    [_vertexDescriptor release];
    _vertexDescriptor = [v copy];
    if (getenv("EMU_MTL_TRACE")) NSLog(@"[Metal] setVertexDescriptor %p -> copy %p (attr0 %p -> %p)", v, _vertexDescriptor, [v->_attributes existing:0], [_vertexDescriptor->_attributes existing:0]);
}
- (id)colorAttachments { return _colorAttachments; }
- (NSUInteger)depthAttachmentPixelFormat { return _depthFormat; }
- (void)setDepthAttachmentPixelFormat:(NSUInteger)v { _depthFormat = v; }
- (NSUInteger)stencilAttachmentPixelFormat { return _stencilFormat; }
- (void)setStencilAttachmentPixelFormat:(NSUInteger)v { _stencilFormat = v; }
- (NSUInteger)sampleCount { return _sampleCount; }
- (void)setSampleCount:(NSUInteger)v { _sampleCount = v; }
- (NSUInteger)rasterSampleCount { return _sampleCount; }
- (void)setRasterSampleCount:(NSUInteger)v { _sampleCount = v; }
- (BOOL)isAlphaToCoverageEnabled { return _alphaToCoverage; }
- (void)setAlphaToCoverageEnabled:(BOOL)v { _alphaToCoverage = v; }
- (BOOL)isAlphaToOneEnabled { return _alphaToOne; }
- (void)setAlphaToOneEnabled:(BOOL)v { _alphaToOne = v; }
- (BOOL)isRasterizationEnabled { return _rasterEnabled; }
- (void)setRasterizationEnabled:(BOOL)v { _rasterEnabled = v; }
- (NSUInteger)inputPrimitiveTopology { return _topology; }
- (void)setInputPrimitiveTopology:(NSUInteger)v { _topology = v; }
- (BOOL)supportIndirectCommandBuffers { return _supportICB; }
- (void)setSupportIndirectCommandBuffers:(BOOL)v { _supportICB = v; }
- (NSUInteger)maxVertexAmplificationCount { return _maxAmp; }
- (void)setMaxVertexAmplificationCount:(NSUInteger)v { _maxAmp = v; }
- (NSString*)label { return _label; }
- (void)setLabel:(NSString*)l { [_label release]; _label = [l copy]; }
@end

@interface MTLComputePipelineDescriptor : NSObject {
@public
    _MTLFunction* _computeFunction;
    NSString* _label;
    BOOL _tgSizeMultiple;
    NSUInteger _maxThreads;
}
@end
@implementation MTLComputePipelineDescriptor
- (void)dealloc { [_computeFunction release]; [_label release]; [super dealloc]; }
PROP_OBJ(_MTLFunction*, computeFunction, setComputeFunction)
- (BOOL)threadGroupSizeIsMultipleOfThreadExecutionWidth { return _tgSizeMultiple; }
- (void)setThreadGroupSizeIsMultipleOfThreadExecutionWidth:(BOOL)v { _tgSizeMultiple = v; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return _maxThreads; }
- (void)setMaxTotalThreadsPerThreadgroup:(NSUInteger)v { _maxThreads = v; }
- (NSString*)label { return _label; }
- (void)setLabel:(NSString*)l { [_label release]; _label = [l copy]; }
@end

// ---- render pass ----
@class _MTLTexture;
@interface MTLRenderPassAttachmentDescriptor : NSObject <NSCopying> {
@public
    _MTLTexture *_texture, *_resolveTexture;
    NSUInteger _level, _slice, _depthPlane, _resolveLevel, _resolveSlice, _resolveDepthPlane, _loadAction, _storeAction, _storeOptions;
}
@end
@implementation MTLRenderPassAttachmentDescriptor
- (void)dealloc { [(id)_texture release]; [(id)_resolveTexture release]; [super dealloc]; }
- (id)copyWithZone:(NSZone*)z {
    MTLRenderPassAttachmentDescriptor* d = [[[self class] allocWithZone:z] init];
    memcpy((char*)d + sizeof(Class), (char*)self + sizeof(Class), class_getInstanceSize([self class]) - sizeof(Class));
    [(id)d->_texture retain];
    [(id)d->_resolveTexture retain];
    return d;
}
- (id)texture { return (id)_texture; }
- (void)setTexture:(id)t { if (t != (id)_texture) { [(id)_texture release]; _texture = [t retain]; } }
- (id)resolveTexture { return (id)_resolveTexture; }
- (void)setResolveTexture:(id)t { if (t != (id)_resolveTexture) { [(id)_resolveTexture release]; _resolveTexture = [t retain]; } }
- (NSUInteger)level { return _level; }
- (void)setLevel:(NSUInteger)v { _level = v; }
- (NSUInteger)slice { return _slice; }
- (void)setSlice:(NSUInteger)v { _slice = v; }
- (NSUInteger)depthPlane { return _depthPlane; }
- (void)setDepthPlane:(NSUInteger)v { _depthPlane = v; }
- (NSUInteger)resolveLevel { return _resolveLevel; }
- (void)setResolveLevel:(NSUInteger)v { _resolveLevel = v; }
- (NSUInteger)resolveSlice { return _resolveSlice; }
- (void)setResolveSlice:(NSUInteger)v { _resolveSlice = v; }
- (NSUInteger)resolveDepthPlane { return _resolveDepthPlane; }
- (void)setResolveDepthPlane:(NSUInteger)v { _resolveDepthPlane = v; }
- (NSUInteger)loadAction { return _loadAction; }
- (void)setLoadAction:(NSUInteger)v { _loadAction = v; }
- (NSUInteger)storeAction { return _storeAction; }
- (void)setStoreAction:(NSUInteger)v { _storeAction = v; }
- (NSUInteger)storeActionOptions { return _storeOptions; }
- (void)setStoreActionOptions:(NSUInteger)v { _storeOptions = v; }
@end

@interface MTLRenderPassColorAttachmentDescriptor : MTLRenderPassAttachmentDescriptor {
@public
    MTLClearColor _clearColor;
}
@end
@implementation MTLRenderPassColorAttachmentDescriptor
- (instancetype)init { _clearColor = (MTLClearColor){0, 0, 0, 1}; return self; }
- (MTLClearColor)clearColor { return _clearColor; }
- (void)setClearColor:(MTLClearColor)c { _clearColor = c; }
@end

@interface MTLRenderPassDepthAttachmentDescriptor : MTLRenderPassAttachmentDescriptor {
@public
    double _clearDepth;
    NSUInteger _resolveFilter;
}
@end
@implementation MTLRenderPassDepthAttachmentDescriptor
- (instancetype)init { _clearDepth = 1.0; return self; }
- (double)clearDepth { return _clearDepth; }
- (void)setClearDepth:(double)v { _clearDepth = v; }
- (NSUInteger)depthResolveFilter { return _resolveFilter; }
- (void)setDepthResolveFilter:(NSUInteger)v { _resolveFilter = v; }
@end

@interface MTLRenderPassStencilAttachmentDescriptor : MTLRenderPassAttachmentDescriptor {
@public
    uint32_t _clearStencil;
}
@end
@implementation MTLRenderPassStencilAttachmentDescriptor
- (uint32_t)clearStencil { return _clearStencil; }
- (void)setClearStencil:(uint32_t)v { _clearStencil = v; }
@end

@interface MTLRenderPassDescriptor : NSObject <NSCopying> {
@public
    _MTLDescArray* _colorAttachments;
    MTLRenderPassDepthAttachmentDescriptor* _depth;
    MTLRenderPassStencilAttachmentDescriptor* _stencil;
    id _visibility;
    NSUInteger _layers, _width, _height, _samples, _tgMem, _tileW, _tileH, _imageblock;
}
@end
@implementation MTLRenderPassDescriptor
+ (instancetype)renderPassDescriptor { return [[[self alloc] init] autorelease]; }
- (instancetype)init {
    _colorAttachments = [[_MTLDescArray alloc] initWithClass:[MTLRenderPassColorAttachmentDescriptor class]];
    _depth = [[MTLRenderPassDepthAttachmentDescriptor alloc] init];
    _stencil = [[MTLRenderPassStencilAttachmentDescriptor alloc] init];
    return self;
}
- (void)dealloc { [_colorAttachments release]; [_depth release]; [_stencil release]; [_visibility release]; [super dealloc]; }
- (id)copyWithZone:(NSZone*)z {
    MTLRenderPassDescriptor* d = [[[self class] allocWithZone:z] init];
    [d->_colorAttachments release]; [d->_depth release]; [d->_stencil release];
    d->_colorAttachments = [_colorAttachments copyArray];
    d->_depth = [_depth copy];
    d->_stencil = [_stencil copy];
    d->_visibility = [_visibility retain];
    d->_layers = _layers; d->_width = _width; d->_height = _height; d->_samples = _samples;
    return d;
}
- (id)colorAttachments { return _colorAttachments; }
- (id)depthAttachment { return _depth; }
- (void)setDepthAttachment:(id)d { [_depth release]; _depth = [d copy]; }
- (id)stencilAttachment { return _stencil; }
- (void)setStencilAttachment:(id)s { [_stencil release]; _stencil = [s copy]; }
PROP_OBJ(id, visibility, setVisibility)
- (id)visibilityResultBuffer { return _visibility; }
- (void)setVisibilityResultBuffer:(id)b { [self setVisibility:b]; }
- (NSUInteger)renderTargetArrayLength { return _layers; }
- (void)setRenderTargetArrayLength:(NSUInteger)v { _layers = v; }
- (NSUInteger)renderTargetWidth { return _width; }
- (void)setRenderTargetWidth:(NSUInteger)v { _width = v; }
- (NSUInteger)renderTargetHeight { return _height; }
- (void)setRenderTargetHeight:(NSUInteger)v { _height = v; }
- (NSUInteger)defaultRasterSampleCount { return _samples; }
- (void)setDefaultRasterSampleCount:(NSUInteger)v { _samples = v; }
- (NSUInteger)threadgroupMemoryLength { return _tgMem; }
- (void)setThreadgroupMemoryLength:(NSUInteger)v { _tgMem = v; }
- (NSUInteger)tileWidth { return _tileW; }
- (void)setTileWidth:(NSUInteger)v { _tileW = v; }
- (NSUInteger)tileHeight { return _tileH; }
- (void)setTileHeight:(NSUInteger)v { _tileH = v; }
- (NSUInteger)imageblockSampleLength { return _imageblock; }
- (void)setImageblockSampleLength:(NSUInteger)v { _imageblock = v; }
- (id)sampleBufferAttachments { return nil; }
- (id)rasterizationRateMap { return nil; }
- (void)setRasterizationRateMap:(id)m { (void)m; }
@end

@interface MTLComputePassDescriptor : NSObject {
@public
    NSUInteger _dispatchType;
}
@end
@implementation MTLComputePassDescriptor
+ (instancetype)computePassDescriptor { return [[[self alloc] init] autorelease]; }
- (NSUInteger)dispatchType { return _dispatchType; }
- (void)setDispatchType:(NSUInteger)v { _dispatchType = v; }
- (id)sampleBufferAttachments { return nil; }
@end
@interface MTLBlitPassDescriptor : NSObject
@end
@implementation MTLBlitPassDescriptor
+ (instancetype)blitPassDescriptor { return [[[self alloc] init] autorelease]; }
- (id)sampleBufferAttachments { return nil; }
@end

@interface MTLCompileOptions : NSObject {
@public
    BOOL _fastMath, _preserveInvariance;
    NSUInteger _languageVersion, _libraryType, _optimizationLevel, _mathMode, _mathFloatingPointFunctions;
    NSDictionary* _macros;
    NSString* _installName;
}
@end
@implementation MTLCompileOptions
- (instancetype)init { _fastMath = YES; _languageVersion = 0x20004; return self; }
- (void)dealloc { [_macros release]; [_installName release]; [super dealloc]; }
- (BOOL)fastMathEnabled { return _fastMath; }
- (void)setFastMathEnabled:(BOOL)v { _fastMath = v; }
- (BOOL)preserveInvariance { return _preserveInvariance; }
- (void)setPreserveInvariance:(BOOL)v { _preserveInvariance = v; }
- (NSUInteger)languageVersion { return _languageVersion; }
- (void)setLanguageVersion:(NSUInteger)v { _languageVersion = v; }
- (NSUInteger)libraryType { return _libraryType; }
- (void)setLibraryType:(NSUInteger)v { _libraryType = v; }
- (NSUInteger)optimizationLevel { return _optimizationLevel; }
- (void)setOptimizationLevel:(NSUInteger)v { _optimizationLevel = v; }
- (NSUInteger)mathMode { return _mathMode; }
- (void)setMathMode:(NSUInteger)v { _mathMode = v; }
- (NSUInteger)mathFloatingPointFunctions { return _mathFloatingPointFunctions; }
- (void)setMathFloatingPointFunctions:(NSUInteger)v { _mathFloatingPointFunctions = v; }
PROP_OBJ(NSDictionary*, macros, setMacros)
- (NSDictionary*)preprocessorMacros { return _macros; }
- (void)setPreprocessorMacros:(NSDictionary*)m { [self setMacros:m]; }
PROP_OBJ(NSString*, installName, setInstallName)
- (id)libraries { return nil; }
- (void)setLibraries:(id)l { (void)l; }
@end

// ===================================================================== reflection
@interface MTLType : NSObject {
@public
    NSUInteger _dataType;
}
@end
@implementation MTLType
- (NSUInteger)dataType { return _dataType; }
@end

@interface MTLArrayType : MTLType {
@public
    NSUInteger _elementType, _arrayLength, _stride;
}
@end
@implementation MTLArrayType
- (NSUInteger)elementType { return _elementType; }
- (NSUInteger)arrayLength { return _arrayLength; }
- (NSUInteger)stride { return _stride; }
- (NSUInteger)argumentIndexStride { return 0; }
- (id)elementStructType { return nil; }
- (id)elementArrayType { return nil; }
- (id)elementPointerType { return nil; }
- (id)elementTextureReferenceType { return nil; }
@end

@interface MTLStructMember : NSObject {
@public
    NSString* _name;
    NSUInteger _offset, _dataType;
    MTLArrayType* _arrayType;
}
@end
@implementation MTLStructMember
- (void)dealloc { [_name release]; [_arrayType release]; [super dealloc]; }
- (NSString*)name { return _name; }
- (NSUInteger)offset { return _offset; }
- (NSUInteger)dataType { return _dataType; }
- (id)arrayType { return _arrayType; }
- (id)structType { return nil; }
- (id)pointerType { return nil; }
- (id)textureReferenceType { return nil; }
- (NSUInteger)argumentIndex { return 0; }
@end

@interface MTLStructType : MTLType {
@public
    NSArray* _members;
}
@end
@implementation MTLStructType
- (instancetype)init { _dataType = 1; return self; }
- (void)dealloc { [_members release]; [super dealloc]; }
- (NSArray*)members { return _members; }
- (id)memberByName:(NSString*)n {
    for (MTLStructMember* m in _members) if ([m->_name isEqualToString:n]) return m;
    return nil;
}
@end

@interface MTLArgument : NSObject {
@public
    NSString* _name;
    NSUInteger _type, _access, _index, _dataType, _dataSize, _alignment, _textureType, _textureDataType, _arrayLength;
    BOOL _active;
    MTLStructType* _structType;
}
@end
@implementation MTLArgument
- (void)dealloc { [_name release]; [_structType release]; [super dealloc]; }
- (NSString*)name { return _name; }
- (NSUInteger)type { return _type; }
- (NSUInteger)access { return _access; }
- (NSUInteger)index { return _index; }
- (BOOL)isActive { return _active; }
- (BOOL)active { return _active; }
- (NSUInteger)bufferAlignment { return _alignment; }
- (NSUInteger)bufferDataSize { return _dataSize; }
- (NSUInteger)bufferDataType { return _dataType; }
- (id)bufferStructType { return _structType; }
- (id)bufferPointerType { return nil; }
- (NSUInteger)threadgroupMemoryAlignment { return 16; }
- (NSUInteger)threadgroupMemoryDataSize { return 0; }
- (NSUInteger)textureType { return _textureType; }
- (NSUInteger)textureDataType { return _textureDataType; }
- (BOOL)isDepthTexture { return NO; }
- (NSUInteger)arrayLength { return _arrayLength ? _arrayLength : 1; }
@end

static NSArray* make_args(const emu_mtl_refl_arg* a, uint32_t n) {
    NSMutableArray* r = [NSMutableArray arrayWithCapacity:n];
    for (uint32_t i = 0; i < n; i++) {
        MTLArgument* g = [[MTLArgument alloc] init];
        g->_name = [[NSString alloc] initWithUTF8String:a[i].name ? a[i].name : ""];
        g->_type = a[i].type;
        g->_access = a[i].access;
        g->_index = a[i].index;
        g->_active = a[i].active != 0;
        g->_dataType = a[i].data_type;
        g->_dataSize = a[i].data_size;
        g->_alignment = a[i].alignment;
        g->_textureType = a[i].texture_type;
        g->_textureDataType = a[i].texture_data_type;
        if (a[i].type == 0 && a[i].data_type == 1) {
            MTLStructType* st = [[MTLStructType alloc] init];
            NSMutableArray* ms = [NSMutableArray arrayWithCapacity:a[i].nmembers];
            for (uint32_t k = 0; k < a[i].nmembers; k++) {
                const emu_mtl_refl_member* m = &a[i].members[k];
                MTLStructMember* sm = [[MTLStructMember alloc] init];
                sm->_name = [[NSString alloc] initWithUTF8String:m->name ? m->name : ""];
                sm->_offset = m->offset;
                if (m->array_len) {
                    sm->_dataType = 2;  // MTLDataTypeArray
                    MTLArrayType* at = [[MTLArrayType alloc] init];
                    at->_dataType = 2;
                    at->_elementType = m->data_type;
                    at->_arrayLength = m->array_len;
                    at->_stride = m->stride;
                    sm->_arrayType = at;
                } else {
                    sm->_dataType = m->data_type;
                }
                [ms addObject:sm];
                [sm release];
            }
            st->_members = [ms retain];
            g->_structType = st;
        }
        [r addObject:g];
        [g release];
    }
    return r;
}

@interface MTLRenderPipelineReflection : NSObject {
@public
    NSArray *_vertexArguments, *_fragmentArguments;
}
@end
@implementation MTLRenderPipelineReflection
- (void)dealloc { [_vertexArguments release]; [_fragmentArguments release]; [super dealloc]; }
- (NSArray*)vertexArguments { return _vertexArguments; }
- (NSArray*)fragmentArguments { return _fragmentArguments; }
- (NSArray*)tileArguments { return [NSArray array]; }
@end
@interface MTLComputePipelineReflection : NSObject {
@public
    NSArray* _arguments;
}
@end
@implementation MTLComputePipelineReflection
- (void)dealloc { [_arguments release]; [super dealloc]; }
- (NSArray*)arguments { return _arguments; }
@end

// ===================================================================== resources
@interface _MTLBuffer : _MTLObject {
@public
    uint64_t _h;
    void* _contents;
    NSUInteger _length, _options;
}
@end
@implementation _MTLBuffer
- (void)dealloc { emu_mtl_buffer_release(_h); [super dealloc]; }
- (void*)contents { return _contents; }
- (NSUInteger)length { return _length; }
- (NSUInteger)allocatedSize { return _length; }
- (void)didModifyRange:(MTLRange_)r { emu_mtl_buffer_modified(_h, r.location, r.length); }
- (NSUInteger)storageMode { return (_options >> 4) & 0xf; }
- (NSUInteger)cpuCacheMode { return _options & 0xf; }
- (NSUInteger)resourceOptions { return _options; }
- (NSUInteger)hazardTrackingMode { return 0; }
- (NSUInteger)setPurgeableState:(NSUInteger)s { (void)s; return 1; }
- (uint64_t)gpuAddress { return (uint64_t)_contents; }
- (id)newTextureWithDescriptor:(id)d offset:(NSUInteger)o bytesPerRow:(NSUInteger)b { (void)d; (void)o; (void)b; return nil; }
- (void)addDebugMarker:(NSString*)m range:(MTLRange_)r { (void)m; (void)r; }
- (void)removeAllDebugMarkers {}
- (BOOL)isAliasable { return NO; }
- (void)makeAliasable {}
- (id)heap { return nil; }
@end

@interface _MTLTexture : _MTLObject {
@public
    uint64_t _h;
    emu_mtl_tex_desc _d;
    _MTLTexture* _parent;
    BOOL _framebufferOnly;
    BOOL _borrowed;  // a drawable's texture (the host's ring): not released here
}
@end
@implementation _MTLTexture
- (void)dealloc {
    if (!_borrowed) emu_mtl_texture_release(_h);
    [_parent release];
    [super dealloc];
}
- (NSUInteger)width { return _d.width; }
- (NSUInteger)height { return _d.height; }
- (NSUInteger)depth { return _d.depth ? _d.depth : 1; }
- (NSUInteger)mipmapLevelCount { return _d.mips ? _d.mips : 1; }
- (NSUInteger)arrayLength { return _d.array_length ? _d.array_length : 1; }
- (NSUInteger)sampleCount { return _d.samples ? _d.samples : 1; }
- (NSUInteger)pixelFormat { return _d.format; }
- (NSUInteger)textureType { return _d.type; }
- (NSUInteger)usage { return _d.usage; }
- (NSUInteger)storageMode { return _d.storage; }
- (NSUInteger)cpuCacheMode { return 0; }
- (NSUInteger)hazardTrackingMode { return 0; }
- (NSUInteger)resourceOptions { return _d.storage << 4; }
- (BOOL)isFramebufferOnly { return _framebufferOnly; }
- (BOOL)framebufferOnly { return _framebufferOnly; }
- (BOOL)allowGPUOptimizedContents { return YES; }
- (BOOL)isShareable { return NO; }
- (BOOL)isSparse { return NO; }
- (NSUInteger)compressionType { return 0; }
- (MTLTextureSwizzleChannels)swizzle { return (MTLTextureSwizzleChannels){2, 3, 4, 5}; }
- (id)parentTexture { return _parent; }
- (NSUInteger)parentRelativeLevel { return 0; }
- (NSUInteger)parentRelativeSlice { return 0; }
- (id)buffer { return nil; }
- (id)rootResource { return nil; }
- (void*)iosurface { return NULL; }
- (NSUInteger)allocatedSize { return (NSUInteger)_d.width * _d.height * 4; }
- (NSUInteger)setPurgeableState:(NSUInteger)s { (void)s; return 1; }
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)l slice:(NSUInteger)s withBytes:(const void*)b bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi {
    uint64_t reg[6] = {r.origin.x, r.origin.y, r.origin.z, r.size.width, r.size.height, r.size.depth};
    emu_mtl_texture_replace(_h, reg, (uint32_t)l, (uint32_t)s, b, bpr, bpi);
}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)l withBytes:(const void*)b bytesPerRow:(NSUInteger)bpr {
    [self replaceRegion:r mipmapLevel:l slice:0 withBytes:b bytesPerRow:bpr bytesPerImage:0];
}
- (void)getBytes:(void*)p bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)l slice:(NSUInteger)s {
    uint64_t reg[6] = {r.origin.x, r.origin.y, r.origin.z, r.size.width, r.size.height, r.size.depth};
    emu_mtl_texture_get(_h, p, bpr, bpi, reg, (uint32_t)l, (uint32_t)s);
}
- (void)getBytes:(void*)p bytesPerRow:(NSUInteger)bpr fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)l {
    [self getBytes:p bytesPerRow:bpr bytesPerImage:bpr * r.size.height fromRegion:r mipmapLevel:l slice:0];
}
- (id)newTextureViewWithPixelFormat:(NSUInteger)f textureType:(NSUInteger)t levels:(MTLRange_)lv slices:(MTLRange_)sl {
    uint64_t h = emu_mtl_texture_view(_h, (uint32_t)f, (uint32_t)t, (uint32_t)lv.location, (uint32_t)lv.length, (uint32_t)sl.location, (uint32_t)sl.length);
    if (!h) return nil;
    _MTLTexture* v = [[_MTLTexture alloc] init];
    v->_h = h;
    v->_device = _device;
    emu_mtl_texture_info(h, &v->_d);
    v->_parent = [self retain];
    return v;
}
- (id)newTextureViewWithPixelFormat:(NSUInteger)f {
    MTLRange_ lv = {0, [self mipmapLevelCount]}, sl = {0, _d.type == 5 ? 6 : [self arrayLength]};
    if (_d.type == 5) sl.length = 1;
    return [self newTextureViewWithPixelFormat:f textureType:_d.type levels:lv slices:sl];
}
- (id)newTextureViewWithPixelFormat:(NSUInteger)f textureType:(NSUInteger)t levels:(MTLRange_)lv slices:(MTLRange_)sl swizzle:(MTLTextureSwizzleChannels)sw {
    (void)sw;
    return [self newTextureViewWithPixelFormat:f textureType:t levels:lv slices:sl];
}
- (void)makeAliasable {}
- (BOOL)isAliasable { return NO; }
- (id)heap { return nil; }
@end

@interface _MTLSamplerState : _MTLObject {
@public
    uint64_t _h;
}
@end
@implementation _MTLSamplerState
- (void)dealloc { emu_mtl_sampler_release(_h); [super dealloc]; }
- (uint64_t)gpuResourceID { return _h; }
@end

@interface _MTLDepthStencilState : _MTLObject {
@public
    uint64_t _h;
}
@end
@implementation _MTLDepthStencilState
- (void)dealloc { emu_mtl_ds_release(_h); [super dealloc]; }
@end

@interface _MTLRenderPipelineState : _MTLObject {
@public
    uint64_t _h;
}
@end
@implementation _MTLRenderPipelineState
- (void)dealloc { emu_mtl_pipeline_release(_h); [super dealloc]; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return 1024; }
- (BOOL)threadgroupSizeMatchesTileSize { return NO; }
- (NSUInteger)imageblockSampleLength { return 0; }
- (BOOL)supportIndirectCommandBuffers { return NO; }
- (uint64_t)gpuResourceID { return _h; }
@end

@interface _MTLComputePipelineState : _MTLObject {
@public
    uint64_t _h;
}
@end
@implementation _MTLComputePipelineState
- (void)dealloc { emu_mtl_pipeline_release(_h); [super dealloc]; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return 1024; }
- (NSUInteger)threadExecutionWidth { return 32; }
- (NSUInteger)staticThreadgroupMemoryLength { return 0; }
- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)s { (void)s; return 0; }
- (BOOL)supportIndirectCommandBuffers { return NO; }
- (uint64_t)gpuResourceID { return _h; }
@end

@interface MTLVertexAttribute : NSObject {
@public
    NSString* _name;
    NSUInteger _index, _type;
}
@end
@implementation MTLVertexAttribute
- (void)dealloc { [_name release]; [super dealloc]; }
- (NSString*)name { return _name; }
- (NSUInteger)attributeIndex { return _index; }
- (NSUInteger)attributeType { return _type; }
- (BOOL)isActive { return YES; }
- (BOOL)active { return YES; }
- (BOOL)isPatchData { return NO; }
- (BOOL)isPatchControlPointData { return NO; }
@end
@interface MTLAttribute : MTLVertexAttribute
@end
@implementation MTLAttribute
@end

@implementation _MTLFunction
- (void)dealloc { emu_mtl_function_release(_h); [_name release]; [super dealloc]; }
- (NSString*)name { return _name; }
- (NSUInteger)functionType { return emu_mtl_function_type(_h); }
- (NSArray*)vertexAttributes {
    NSMutableArray* r = [NSMutableArray array];
    char name[128];
    uint32_t index = 0, type = 0;
    for (uint32_t i = 0; emu_mtl_function_attribute(_h, i, name, sizeof name, &index, &type); i++) {
        MTLVertexAttribute* a = [[MTLVertexAttribute alloc] init];
        a->_name = [[NSString alloc] initWithUTF8String:name];
        a->_index = index;
        a->_type = type;
        [r addObject:a];
        [a release];
    }
    return r;
}
- (NSArray*)stageInputAttributes { return [self vertexAttributes]; }
- (NSDictionary*)functionConstantsDictionary { return [NSDictionary dictionary]; }
- (NSUInteger)patchType { return 0; }
- (NSInteger)patchControlPointCount { return 0; }
- (NSUInteger)options { return 0; }
- (id)newArgumentEncoderWithBufferIndex:(NSUInteger)i { (void)i; return nil; }
@end

@interface _MTLLibrary : _MTLObject {
@public
    uint64_t _h;
}
@end
@implementation _MTLLibrary
- (void)dealloc { emu_mtl_library_release(_h); [super dealloc]; }
- (id)newFunctionWithName:(NSString*)n {
    uint64_t f = emu_mtl_function_new(_h, [n UTF8String]);
    if (!f) return nil;
    _MTLFunction* r = [[_MTLFunction alloc] init];
    r->_h = f;
    r->_name = [n copy];
    r->_device = _device;
    return r;
}
- (id)newFunctionWithName:(NSString*)n constantValues:(id)c error:(NSError**)e {
    (void)c;
    if (e) *e = nil;
    return [self newFunctionWithName:n];
}
- (NSArray*)functionNames { return [NSArray arrayWithObject:@"xlatMtlMain"]; }
- (NSUInteger)type { return 0; }
- (NSString*)installName { return nil; }
@end

// ===================================================================== encoders
@interface _MTLRenderEncoder : _MTLObject {
@public
    uint64_t _e;
}
@end
@implementation _MTLRenderEncoder
static uint64_t H(id o) { return o ? ((_MTLBuffer*)o)->_h : 0; }  // (every resource class has _h first after _MTLObject)
- (void)setRenderPipelineState:(id)p { emu_mtl_r_pipeline(_e, H(p)); }
- (void)setVertexBuffer:(id)b offset:(NSUInteger)o atIndex:(NSUInteger)i { emu_mtl_r_buffer(_e, 0, (uint32_t)i, H(b), o); }
- (void)setFragmentBuffer:(id)b offset:(NSUInteger)o atIndex:(NSUInteger)i { emu_mtl_r_buffer(_e, 1, (uint32_t)i, H(b), o); }
- (void)setVertexBuffers:(const id*)b offsets:(const NSUInteger*)o withRange:(MTLRange_)r {
    for (NSUInteger k = 0; k < r.length; k++) emu_mtl_r_buffer(_e, 0, (uint32_t)(r.location + k), H(b[k]), o ? o[k] : 0);
}
- (void)setFragmentBuffers:(const id*)b offsets:(const NSUInteger*)o withRange:(MTLRange_)r {
    for (NSUInteger k = 0; k < r.length; k++) emu_mtl_r_buffer(_e, 1, (uint32_t)(r.location + k), H(b[k]), o ? o[k] : 0);
}
- (void)setVertexBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i { [self _offset:o stage:0 index:i]; }
- (void)setFragmentBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i { [self _offset:o stage:1 index:i]; }
- (void)_offset:(NSUInteger)o stage:(uint32_t)s index:(NSUInteger)i {
    // the host keeps the buffer: rebind it with the new offset
    emu_mtl_r_buffer(_e, s | 0x80000000u, (uint32_t)i, 0, o);
}
- (void)setVertexBytes:(const void*)b length:(NSUInteger)n atIndex:(NSUInteger)i { emu_mtl_r_bytes(_e, 0, (uint32_t)i, b, n); }
- (void)setFragmentBytes:(const void*)b length:(NSUInteger)n atIndex:(NSUInteger)i { emu_mtl_r_bytes(_e, 1, (uint32_t)i, b, n); }
- (void)setVertexTexture:(id)t atIndex:(NSUInteger)i { emu_mtl_r_texture(_e, 0, (uint32_t)i, H(t)); }
- (void)setFragmentTexture:(id)t atIndex:(NSUInteger)i { emu_mtl_r_texture(_e, 1, (uint32_t)i, H(t)); }
- (void)setVertexTextures:(const id*)t withRange:(MTLRange_)r {
    for (NSUInteger k = 0; k < r.length; k++) emu_mtl_r_texture(_e, 0, (uint32_t)(r.location + k), H(t[k]));
}
- (void)setFragmentTextures:(const id*)t withRange:(MTLRange_)r {
    for (NSUInteger k = 0; k < r.length; k++) emu_mtl_r_texture(_e, 1, (uint32_t)(r.location + k), H(t[k]));
}
- (void)setVertexSamplerState:(id)s atIndex:(NSUInteger)i { emu_mtl_r_sampler(_e, 0, (uint32_t)i, H(s)); }
- (void)setFragmentSamplerState:(id)s atIndex:(NSUInteger)i { emu_mtl_r_sampler(_e, 1, (uint32_t)i, H(s)); }
- (void)setVertexSamplerState:(id)s lodMinClamp:(float)a lodMaxClamp:(float)b atIndex:(NSUInteger)i { (void)a; (void)b; [self setVertexSamplerState:s atIndex:i]; }
- (void)setFragmentSamplerState:(id)s lodMinClamp:(float)a lodMaxClamp:(float)b atIndex:(NSUInteger)i { (void)a; (void)b; [self setFragmentSamplerState:s atIndex:i]; }
- (void)setVertexSamplerStates:(const id*)s withRange:(MTLRange_)r {
    for (NSUInteger k = 0; k < r.length; k++) emu_mtl_r_sampler(_e, 0, (uint32_t)(r.location + k), H(s[k]));
}
- (void)setFragmentSamplerStates:(const id*)s withRange:(MTLRange_)r {
    for (NSUInteger k = 0; k < r.length; k++) emu_mtl_r_sampler(_e, 1, (uint32_t)(r.location + k), H(s[k]));
}
- (void)setViewport:(MTLViewport)v { emu_mtl_r_viewport(_e, &v.originX); }
- (void)setViewports:(const MTLViewport*)v count:(NSUInteger)n { if (n) emu_mtl_r_viewport(_e, &v[0].originX); }
- (void)setScissorRect:(MTLScissorRect)r { emu_mtl_r_scissor(_e, r.x, r.y, r.width, r.height); }
- (void)setScissorRects:(const MTLScissorRect*)r count:(NSUInteger)n { if (n) [self setScissorRect:r[0]]; }
- (void)setCullMode:(NSUInteger)m { emu_mtl_r_state(_e, 0, (uint32_t)m); }
- (void)setFrontFacingWinding:(NSUInteger)w { emu_mtl_r_state(_e, 1, (uint32_t)w); }
- (void)setTriangleFillMode:(NSUInteger)f { emu_mtl_r_state(_e, 2, (uint32_t)f); }
- (void)setDepthClipMode:(NSUInteger)m { emu_mtl_r_state(_e, 4, (uint32_t)m); }
- (void)setDepthBias:(float)b slopeScale:(float)s clamp:(float)c { emu_mtl_r_depth_bias(_e, b, s, c); }
- (void)setDepthStencilState:(id)s { emu_mtl_r_ds(_e, H(s)); }
- (void)setStencilReferenceValue:(uint32_t)v { emu_mtl_r_stencil_ref(_e, v, v); }
- (void)setStencilFrontReferenceValue:(uint32_t)f backReferenceValue:(uint32_t)b { emu_mtl_r_stencil_ref(_e, f, b); }
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a { emu_mtl_r_blend_color(_e, r, g, b, a); }
- (void)setVisibilityResultMode:(NSUInteger)m offset:(NSUInteger)o { (void)m; (void)o; }
- (void)setColorStoreAction:(NSUInteger)a atIndex:(NSUInteger)i { (void)a; (void)i; }
- (void)setDepthStoreAction:(NSUInteger)a { (void)a; }
- (void)setStencilStoreAction:(NSUInteger)a { (void)a; }
- (void)drawPrimitives:(NSUInteger)p vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n {
    emu_mtl_r_draw(_e, (uint32_t)p, s, n, 1, 0);
}
- (void)drawPrimitives:(NSUInteger)p vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n instanceCount:(NSUInteger)ic {
    emu_mtl_r_draw(_e, (uint32_t)p, s, n, ic, 0);
}
- (void)drawPrimitives:(NSUInteger)p vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n instanceCount:(NSUInteger)ic baseInstance:(NSUInteger)bi {
    emu_mtl_r_draw(_e, (uint32_t)p, s, n, ic, bi);
}
- (void)drawIndexedPrimitives:(NSUInteger)p indexCount:(NSUInteger)n indexType:(NSUInteger)t indexBuffer:(id)b indexBufferOffset:(NSUInteger)o {
    emu_mtl_r_draw_indexed(_e, (uint32_t)p, n, (uint32_t)t, H(b), o, 1, 0, 0);
}
- (void)drawIndexedPrimitives:(NSUInteger)p indexCount:(NSUInteger)n indexType:(NSUInteger)t indexBuffer:(id)b indexBufferOffset:(NSUInteger)o instanceCount:(NSUInteger)ic {
    emu_mtl_r_draw_indexed(_e, (uint32_t)p, n, (uint32_t)t, H(b), o, ic, 0, 0);
}
- (void)drawIndexedPrimitives:(NSUInteger)p indexCount:(NSUInteger)n indexType:(NSUInteger)t indexBuffer:(id)b indexBufferOffset:(NSUInteger)o
                instanceCount:(NSUInteger)ic baseVertex:(NSInteger)bv baseInstance:(NSUInteger)bi {
    emu_mtl_r_draw_indexed(_e, (uint32_t)p, n, (uint32_t)t, H(b), o, ic, bv, bi);
}
- (void)drawPrimitives:(NSUInteger)p indirectBuffer:(id)b indirectBufferOffset:(NSUInteger)o {
    // {vertexCount, instanceCount, vertexStart, baseInstance} (the GPU has finished: commands run at once)
    const uint32_t* a = (const uint32_t*)((char*)[(_MTLBuffer*)b contents] + o);
    emu_mtl_r_draw(_e, (uint32_t)p, a[2], a[0], a[1], a[3]);
}
- (void)drawIndexedPrimitives:(NSUInteger)p indexType:(NSUInteger)t indexBuffer:(id)ib indexBufferOffset:(NSUInteger)io indirectBuffer:(id)b indirectBufferOffset:(NSUInteger)o {
    // {indexCount, instanceCount, indexStart, baseVertex, baseInstance}
    const uint32_t* a = (const uint32_t*)((char*)[(_MTLBuffer*)b contents] + o);
    emu_mtl_r_draw_indexed(_e, (uint32_t)p, a[0], (uint32_t)t, H(ib), io + a[2] * (t ? 4 : 2), a[1], (int32_t)a[3], a[4]);
}
- (void)endEncoding { emu_mtl_r_end(_e); }
- (void)pushDebugGroup:(NSString*)s { (void)s; }
- (void)popDebugGroup {}
- (void)insertDebugSignpost:(NSString*)s { (void)s; }
- (void)useResource:(id)r usage:(NSUInteger)u { (void)r; (void)u; }
- (void)useResource:(id)r usage:(NSUInteger)u stages:(NSUInteger)s { (void)r; (void)u; (void)s; }
- (void)useResources:(const id*)r count:(NSUInteger)n usage:(NSUInteger)u { (void)r; (void)n; (void)u; }
- (void)useHeap:(id)h { (void)h; }
- (void)useHeap:(id)h stages:(NSUInteger)s { (void)h; (void)s; }
- (void)textureBarrier {}
- (void)memoryBarrierWithScope:(NSUInteger)s afterStages:(NSUInteger)a beforeStages:(NSUInteger)b { (void)s; (void)a; (void)b; }
- (void)updateFence:(id)f afterStages:(NSUInteger)s { (void)f; (void)s; }
- (void)waitForFence:(id)f beforeStages:(NSUInteger)s { (void)f; (void)s; }
- (NSUInteger)tileWidth { return 32; }
- (NSUInteger)tileHeight { return 32; }
@end

@interface _MTLBlitEncoder : _MTLObject
@end
@implementation _MTLBlitEncoder
- (void)copyFromBuffer:(id)s sourceOffset:(NSUInteger)so toBuffer:(id)d destinationOffset:(NSUInteger)dof size:(NSUInteger)n {
    emu_mtl_b_copy_buffer(H(s), so, H(d), dof, n);
}
- (void)copyFromBuffer:(id)s sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)bpr sourceBytesPerImage:(NSUInteger)bpi sourceSize:(MTLSize)sz
             toTexture:(id)d destinationSlice:(NSUInteger)sl destinationLevel:(NSUInteger)lv destinationOrigin:(MTLOrigin)o {
    uint64_t size[3] = {sz.width, sz.height, sz.depth}, org[3] = {o.x, o.y, o.z};
    emu_mtl_b_buffer_to_texture(H(s), so, bpr, bpi, size, H(d), (uint32_t)sl, (uint32_t)lv, org);
}
- (void)copyFromBuffer:(id)s sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)bpr sourceBytesPerImage:(NSUInteger)bpi sourceSize:(MTLSize)sz
             toTexture:(id)d destinationSlice:(NSUInteger)sl destinationLevel:(NSUInteger)lv destinationOrigin:(MTLOrigin)o options:(NSUInteger)op {
    (void)op;
    [self copyFromBuffer:s sourceOffset:so sourceBytesPerRow:bpr sourceBytesPerImage:bpi sourceSize:sz toTexture:d destinationSlice:sl destinationLevel:lv destinationOrigin:o];
}
- (void)copyFromTexture:(id)s sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)sz
              toTexture:(id)d destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dor {
    uint64_t a[3] = {so.x, so.y, so.z}, size[3] = {sz.width, sz.height, sz.depth}, b[3] = {dor.x, dor.y, dor.z};
    emu_mtl_b_texture_to_texture(H(s), (uint32_t)ss, (uint32_t)sl, a, size, H(d), (uint32_t)ds, (uint32_t)dl, b);
}
- (void)copyFromTexture:(id)s toTexture:(id)d {
    _MTLTexture* t = s;
    uint64_t o[3] = {0, 0, 0};
    for (uint32_t l = 0; l < [t mipmapLevelCount]; l++) {
        uint64_t size[3] = {[t width] >> l ?: 1, [t height] >> l ?: 1, [t depth]};
        for (uint32_t k = 0; k < [t arrayLength] * ([t textureType] == 5 ? 6 : 1); k++)
            emu_mtl_b_texture_to_texture(H(s), k, l, o, size, H(d), k, l, o);
    }
}
- (void)copyFromTexture:(id)s sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)sz
               toBuffer:(id)d destinationOffset:(NSUInteger)off destinationBytesPerRow:(NSUInteger)bpr destinationBytesPerImage:(NSUInteger)bpi {
    uint64_t a[3] = {so.x, so.y, so.z}, size[3] = {sz.width, sz.height, sz.depth};
    emu_mtl_b_texture_to_buffer(H(s), (uint32_t)ss, (uint32_t)sl, a, size, H(d), off, bpr, bpi);
}
- (void)copyFromTexture:(id)s sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)sz
               toBuffer:(id)d destinationOffset:(NSUInteger)off destinationBytesPerRow:(NSUInteger)bpr destinationBytesPerImage:(NSUInteger)bpi options:(NSUInteger)op {
    (void)op;
    [self copyFromTexture:s sourceSlice:ss sourceLevel:sl sourceOrigin:so sourceSize:sz toBuffer:d destinationOffset:off destinationBytesPerRow:bpr destinationBytesPerImage:bpi];
}
- (void)generateMipmapsForTexture:(id)t { emu_mtl_b_mipmaps(H(t)); }
- (void)fillBuffer:(id)b range:(MTLRange_)r value:(uint8_t)v { emu_mtl_b_fill(H(b), r.location, r.length, v); }
- (void)synchronizeResource:(id)r { (void)r; }
- (void)synchronizeTexture:(id)t slice:(NSUInteger)s level:(NSUInteger)l { (void)t; (void)s; (void)l; }
- (void)optimizeContentsForGPUAccess:(id)t { (void)t; }
- (void)optimizeContentsForCPUAccess:(id)t { (void)t; }
- (void)updateFence:(id)f { (void)f; }
- (void)waitForFence:(id)f { (void)f; }
- (void)endEncoding {}
- (void)pushDebugGroup:(NSString*)s { (void)s; }
- (void)popDebugGroup {}
- (void)insertDebugSignpost:(NSString*)s { (void)s; }
@end

@interface _MTLComputeEncoder : _MTLObject
@end
@implementation _MTLComputeEncoder
- (void)setComputePipelineState:(id)p { emu_mtl_c_pipeline(H(p)); }
- (void)setBuffer:(id)b offset:(NSUInteger)o atIndex:(NSUInteger)i { emu_mtl_c_buffer((uint32_t)i, H(b), o); }
- (void)setBuffers:(const id*)b offsets:(const NSUInteger*)o withRange:(MTLRange_)r {
    for (NSUInteger k = 0; k < r.length; k++) emu_mtl_c_buffer((uint32_t)(r.location + k), H(b[k]), o ? o[k] : 0);
}
- (void)setBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i { emu_mtl_c_buffer((uint32_t)i | 0x80000000u, 0, o); }
- (void)setBytes:(const void*)b length:(NSUInteger)n atIndex:(NSUInteger)i { emu_mtl_c_bytes((uint32_t)i, b, n); }
- (void)setTexture:(id)t atIndex:(NSUInteger)i { emu_mtl_c_texture((uint32_t)i, H(t)); }
- (void)setTextures:(const id*)t withRange:(MTLRange_)r {
    for (NSUInteger k = 0; k < r.length; k++) emu_mtl_c_texture((uint32_t)(r.location + k), H(t[k]));
}
- (void)setSamplerState:(id)s atIndex:(NSUInteger)i { emu_mtl_c_sampler((uint32_t)i, H(s)); }
- (void)setSamplerStates:(const id*)s withRange:(MTLRange_)r {
    for (NSUInteger k = 0; k < r.length; k++) emu_mtl_c_sampler((uint32_t)(r.location + k), H(s[k]));
}
- (void)setThreadgroupMemoryLength:(NSUInteger)n atIndex:(NSUInteger)i { (void)n; (void)i; }
- (void)dispatchThreadgroups:(MTLSize)g threadsPerThreadgroup:(MTLSize)t {
    emu_mtl_c_dispatch(g.width, g.height, g.depth, t.width, t.height, t.depth);
}
- (void)dispatchThreads:(MTLSize)n threadsPerThreadgroup:(MTLSize)t {
    emu_mtl_c_dispatch((n.width + t.width - 1) / t.width, (n.height + t.height - 1) / t.height, (n.depth + t.depth - 1) / t.depth,
                       t.width, t.height, t.depth);
}
- (void)dispatchThreadgroupsWithIndirectBuffer:(id)b indirectBufferOffset:(NSUInteger)o threadsPerThreadgroup:(MTLSize)t {
    const uint32_t* a = (const uint32_t*)((char*)[(_MTLBuffer*)b contents] + o);
    emu_mtl_c_dispatch(a[0], a[1], a[2], t.width, t.height, t.depth);
}
- (void)memoryBarrierWithScope:(NSUInteger)s { (void)s; }
- (void)memoryBarrierWithResources:(const id*)r count:(NSUInteger)n { (void)r; (void)n; }
- (void)useResource:(id)r usage:(NSUInteger)u { (void)r; (void)u; }
- (void)useResources:(const id*)r count:(NSUInteger)n usage:(NSUInteger)u { (void)r; (void)n; (void)u; }
- (void)useHeap:(id)h { (void)h; }
- (void)updateFence:(id)f { (void)f; }
- (void)waitForFence:(id)f { (void)f; }
- (NSUInteger)dispatchType { return 0; }
- (void)endEncoding {}
- (void)pushDebugGroup:(NSString*)s { (void)s; }
- (void)popDebugGroup {}
- (void)insertDebugSignpost:(NSString*)s { (void)s; }
@end

// ===================================================================== drawable / command buffer / queue
@interface _CAMetalDrawable : NSObject {
@public
    _MTLTexture* _texture;
    id _layer;
    NSUInteger _id;
    NSMutableArray* _presentedHandlers;
}
@end
@implementation _CAMetalDrawable
- (void)dealloc { [_texture release]; [_layer release]; [_presentedHandlers release]; [super dealloc]; }
- (id)texture { return _texture; }
- (id)layer { return _layer; }
- (void)present {
    emu_mtl_present(_texture->_h);
    for (void (^h)(id) in _presentedHandlers) h(self);
}
- (void)presentAtTime:(double)t { (void)t; [self present]; }
- (void)presentAfterMinimumDuration:(double)d { (void)d; [self present]; }
- (double)presentedTime { return emu_time(); }
- (NSUInteger)drawableID { return _id; }
- (void)addPresentedHandler:(void (^)(id))h {
    if (!_presentedHandlers) _presentedHandlers = [[NSMutableArray alloc] init];
    id c = [h copy];
    [_presentedHandlers addObject:c];
    [c release];
}
@end

@interface _MTLCommandBuffer : _MTLObject {
@public
    id _queue;
    NSMutableArray *_scheduled, *_completed, *_drawables;
    NSUInteger _status;
    double _t0, _t1;
    id _current;  // the open encoder (encoders are ended implicitly)
    uint64_t _ticket;
}
- (void)_complete;
@end

// Committed command buffers waiting for the GPU. Like Metal, the completion handlers run on
// another thread ("Metal completion") as soon as the GPU has finished the buffer; commit
// keeps at most 3 buffers in flight.
static NSMutableArray* g_inflight;
static pthread_mutex_t g_inflight_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_completion_once = PTHREAD_ONCE_INIT;
static void* completion_thread(void* arg) {
    (void)arg;
    pthread_setname_np("Metal completion");
    for (;;) {
        uint64_t done = emu_mtl_next_completed();
        for (;;) {
            void* pool = objc_autoreleasePoolPush();
            pthread_mutex_lock(&g_inflight_lock);
            _MTLCommandBuffer* b = nil;
            if ([g_inflight count]) {
                _MTLCommandBuffer* f = [g_inflight objectAtIndex:0];
                if (f->_ticket <= done) {
                    b = [f retain];
                    [g_inflight removeObjectAtIndex:0];
                }
            }
            pthread_mutex_unlock(&g_inflight_lock);
            if (b) {
                [b _complete];
                [b release];
            }
            objc_autoreleasePoolPop(pool);
            if (!b) break;
        }
    }
    return NULL;
}
static void start_completion_thread(void) {
    g_inflight = [[NSMutableArray alloc] init];
    pthread_t t;
    pthread_create(&t, NULL, completion_thread, NULL);
    pthread_detach(t);
}

@implementation _MTLCommandBuffer
- (instancetype)init {
    _scheduled = [[NSMutableArray alloc] init];
    _completed = [[NSMutableArray alloc] init];
    _drawables = [[NSMutableArray alloc] init];
    return self;
}
- (void)dealloc { [_scheduled release]; [_completed release]; [_drawables release]; [_current release]; [super dealloc]; }
- (id)commandQueue { return _queue; }
- (BOOL)retainedReferences { return YES; }
- (NSUInteger)status { return _status; }
- (NSError*)error { return nil; }
- (NSUInteger)errorOptions { return 0; }
- (id)logs { return nil; }
- (double)GPUStartTime { return _t0; }
- (double)GPUEndTime { return _t1; }
- (double)kernelStartTime { return _t0; }
- (double)kernelEndTime { return _t1; }
- (id)_encoder:(id)e { [_current release]; _current = [e retain]; return [e autorelease]; }
- (id)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor*)d {
    if (!d) return nil;
    emu_mtl_pass_desc p;
    memset(&p, 0, sizeof p);
    for (int i = 0; i < 8; i++) {
        MTLRenderPassColorAttachmentDescriptor* a = [d->_colorAttachments existing:i];
        if (!a || !a->_texture) continue;
        p.color[i].tex = a->_texture->_h;
        p.color[i].resolve = a->_resolveTexture ? a->_resolveTexture->_h : 0;
        p.color[i].level = (uint32_t)a->_level;
        p.color[i].slice = (uint32_t)a->_slice;
        p.color[i].depth_plane = (uint32_t)a->_depthPlane;
        p.color[i].resolve_level = (uint32_t)a->_resolveLevel;
        p.color[i].resolve_slice = (uint32_t)a->_resolveSlice;
        p.color[i].load = (uint32_t)a->_loadAction;
        p.color[i].store = (uint32_t)a->_storeAction;
        p.color[i].clear[0] = a->_clearColor.red;
        p.color[i].clear[1] = a->_clearColor.green;
        p.color[i].clear[2] = a->_clearColor.blue;
        p.color[i].clear[3] = a->_clearColor.alpha;
    }
    if (d->_depth->_texture) {
        p.depth.tex = d->_depth->_texture->_h;
        p.depth.level = (uint32_t)d->_depth->_level;
        p.depth.slice = (uint32_t)d->_depth->_slice;
        p.depth.load = (uint32_t)d->_depth->_loadAction;
        p.depth.store = (uint32_t)d->_depth->_storeAction;
        p.depth.clear[0] = d->_depth->_clearDepth;
    }
    if (d->_stencil->_texture) {
        p.stencil.tex = d->_stencil->_texture->_h;
        p.stencil.level = (uint32_t)d->_stencil->_level;
        p.stencil.slice = (uint32_t)d->_stencil->_slice;
    }
    p.stencil.load = (uint32_t)d->_stencil->_loadAction;
    p.stencil.clear[1] = d->_stencil->_clearStencil;
    p.width = (uint32_t)d->_width;
    p.height = (uint32_t)d->_height;
    p.layers = (uint32_t)d->_layers;
    p.samples = (uint32_t)d->_samples;
    _MTLRenderEncoder* e = [[_MTLRenderEncoder alloc] init];
    e->_e = emu_mtl_render_begin(&p);
    e->_device = _device;
    return [self _encoder:e];
}
- (id)parallelRenderCommandEncoderWithDescriptor:(id)d { (void)d; return nil; }
- (id)blitCommandEncoder { _MTLBlitEncoder* e = [[_MTLBlitEncoder alloc] init]; e->_device = _device; return [self _encoder:e]; }
- (id)blitCommandEncoderWithDescriptor:(id)d { (void)d; return [self blitCommandEncoder]; }
- (id)computeCommandEncoder { _MTLComputeEncoder* e = [[_MTLComputeEncoder alloc] init]; e->_device = _device; return [self _encoder:e]; }
- (id)computeCommandEncoderWithDescriptor:(id)d { (void)d; return [self computeCommandEncoder]; }
- (id)computeCommandEncoderWithDispatchType:(NSUInteger)t { (void)t; return [self computeCommandEncoder]; }
- (id)resourceStateCommandEncoder { return nil; }
- (id)accelerationStructureCommandEncoder { return nil; }
- (void)presentDrawable:(id)d { if (d) [_drawables addObject:d]; }
- (void)presentDrawable:(id)d atTime:(double)t { (void)t; [self presentDrawable:d]; }
- (void)presentDrawable:(id)d afterMinimumDuration:(double)t { (void)t; [self presentDrawable:d]; }
- (void)addScheduledHandler:(void (^)(id))h { id c = [h copy]; [_scheduled addObject:c]; [c release]; }
- (void)addCompletedHandler:(void (^)(id))h { id c = [h copy]; [_completed addObject:c]; [c release]; }
- (void)enqueue { if (_status < 1) _status = 1; }
- (void)commit {
    if (_status >= 2) return;
    pthread_once(&g_completion_once, start_completion_thread);
    _t0 = emu_time();
    _status = 2;  // committed
    pthread_mutex_lock(&g_inflight_lock);
    _ticket = emu_mtl_commit();
    [g_inflight addObject:self];
    pthread_mutex_unlock(&g_inflight_lock);
    for (_CAMetalDrawable* d in _drawables) [d present];
    [_drawables removeAllObjects];
    _status = 3;  // scheduled
    for (void (^h)(id) in _scheduled) h(self);
    [_scheduled removeAllObjects];
    if (_ticket > 3) emu_mtl_wait_completed(_ticket - 3);
}
- (void)_complete {
    _t1 = emu_time();
    _status = 4;  // completed
    for (void (^h)(id) in _completed) h(self);
    [_completed removeAllObjects];
}
- (void)waitUntilCompleted {
    if (_status == 2 || _status == 3) emu_mtl_wait_completed(_ticket);
    // (the handlers run on the completion thread; wait for them like Metal does)
    for (int i = 0; i < 3000 && _status != 4; i++) usleep(1000);
}
- (void)waitUntilScheduled {}
- (void)pushDebugGroup:(NSString*)s { (void)s; }
- (void)popDebugGroup {}
- (void)encodeSignalEvent:(id)e value:(uint64_t)v { (void)e; (void)v; }
- (void)encodeWaitForEvent:(id)e value:(uint64_t)v { (void)e; (void)v; }
- (void)useResidencySet:(id)s { (void)s; }
@end

@interface _MTLCommandQueue : _MTLObject
@end
@implementation _MTLCommandQueue
- (id)commandBuffer {
    _MTLCommandBuffer* b = [[_MTLCommandBuffer alloc] init];
    b->_queue = self;
    b->_device = _device;
    return [b autorelease];
}
- (id)commandBufferWithUnretainedReferences { return [self commandBuffer]; }
- (id)commandBufferWithDescriptor:(id)d { (void)d; return [self commandBuffer]; }
- (void)insertDebugCaptureBoundary {}
- (void)addResidencySet:(id)s { (void)s; }
- (void)removeResidencySet:(id)s { (void)s; }
@end

@interface _MTLDummy : _MTLObject
@end
@implementation _MTLDummy
- (uint64_t)signaledValue { return ~0ull; }
- (void)setSignaledValue:(uint64_t)v { (void)v; }
- (NSUInteger)size { return 0; }
- (NSUInteger)usedSize { return 0; }
- (NSUInteger)currentAllocatedSize { return 0; }
- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)a { (void)a; return 0; }
- (id)newBufferWithLength:(NSUInteger)l options:(NSUInteger)o { return [_device newBufferWithLength:l options:o]; }
- (id)newTextureWithDescriptor:(id)d { return [_device newTextureWithDescriptor:d]; }
- (NSUInteger)setPurgeableState:(NSUInteger)s { (void)s; return 1; }
- (void)notifyListener:(id)l atValue:(uint64_t)v block:(id)b { (void)l; (void)v; if (b) ((void (^)(id, uint64_t))b)(self, v); }
@end

// ===================================================================== device
@interface _MTLDevice : NSObject
@end
@implementation _MTLDevice
- (id)retain { return self; }
- (oneway void)release {}
- (id)autorelease { return self; }
// The game picks its rendering tier by this name (data/tiers.bin): A11 = tier 1, the classic
// renderer. (Vibrant Visuals, tier 2+, needs compute and image features the emulator lacks.)
- (NSString*)name { return @"Apple A11 GPU"; }
- (uint64_t)registryID { return 0x100000001ull; }
- (NSString*)description { return @"<MTLDevice: Apple A11 GPU (emulated on OpenGL)>"; }
- (MTLSize)maxThreadsPerThreadgroup { return (MTLSize){1024, 1024, 1024}; }
- (NSUInteger)maxThreadgroupMemoryLength { return 32768; }
- (NSUInteger)maxBufferLength { return 1ull << 30; }
- (uint64_t)recommendedMaxWorkingSetSize { return 3ull << 30; }
- (NSUInteger)currentAllocatedSize { return 0; }
- (BOOL)hasUnifiedMemory { return YES; }
- (BOOL)isLowPower { return NO; }
- (BOOL)isHeadless { return NO; }
- (BOOL)isRemovable { return NO; }
- (NSUInteger)location { return 0; }
- (NSUInteger)readWriteTextureSupport { return 2; }
- (NSUInteger)argumentBuffersSupport { return 1; }
- (BOOL)areRasterOrderGroupsSupported { return NO; }
- (BOOL)supports32BitFloatFiltering { return YES; }
- (BOOL)supports32BitMSAA { return YES; }
- (BOOL)supportsBCTextureCompression { return NO; }
- (BOOL)isDepth24Stencil8PixelFormatSupported { return NO; }
- (BOOL)depth24Stencil8PixelFormatSupported { return NO; }
- (BOOL)supportsQueryTextureLOD { return YES; }
- (BOOL)supportsPullModelInterpolation { return NO; }
- (BOOL)supportsShaderBarycentricCoordinates { return NO; }
- (BOOL)areProgrammableSamplePositionsSupported { return NO; }
- (BOOL)supportsDynamicLibraries { return NO; }
- (BOOL)supportsRaytracing { return NO; }
- (BOOL)supportsFunctionPointers { return NO; }
- (BOOL)supportsPrimitiveMotionBlur { return NO; }
- (NSUInteger)maxArgumentBufferSamplerCount { return 16; }
- (NSUInteger)sparseTileSizeInBytes { return 0; }
- (BOOL)supportsFamily:(NSInteger)f {
    // Apple1..Apple4 (A11), Common1..3
    if (f >= 1001 && f <= 1004) return YES;
    if (f >= 3001 && f <= 3003) return YES;
    return NO;
}
- (BOOL)supportsFeatureSet:(NSUInteger)fs { return fs <= 16; }
- (BOOL)supportsTextureSampleCount:(NSUInteger)n { return n == 1 || n == 2 || n == 4 || n == 8; }
- (BOOL)supportsVertexAmplificationCount:(NSUInteger)n { return n <= 1; }
- (BOOL)supportsCounterSampling:(NSUInteger)p { (void)p; return NO; }
- (NSUInteger)minimumLinearTextureAlignmentForPixelFormat:(NSUInteger)f { (void)f; return 16; }
- (NSUInteger)minimumTextureBufferAlignmentForPixelFormat:(NSUInteger)f { (void)f; return 16; }
- (id)counterSets { return [NSArray array]; }
- (id)newCommandQueue {
    _MTLCommandQueue* q = [[_MTLCommandQueue alloc] init];
    q->_device = self;
    return q;
}
- (id)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)n { (void)n; return [self newCommandQueue]; }
- (id)_buffer:(NSUInteger)len bytes:(const void*)b options:(NSUInteger)o {
    _MTLBuffer* r = [[_MTLBuffer alloc] init];
    r->_h = emu_mtl_buffer_new(len, b, (uint32_t)o, &r->_contents);
    r->_length = len;
    r->_options = o;
    r->_device = self;
    return r;
}
- (id)newBufferWithLength:(NSUInteger)l options:(NSUInteger)o { return [self _buffer:l bytes:NULL options:o]; }
- (id)newBufferWithBytes:(const void*)b length:(NSUInteger)l options:(NSUInteger)o { return [self _buffer:l bytes:b options:o]; }
- (id)newBufferWithBytesNoCopy:(void*)b length:(NSUInteger)l options:(NSUInteger)o deallocator:(void (^)(void*, NSUInteger))d {
    id r = [self _buffer:l bytes:b options:o];
    if (d) d(b, l);
    return r;
}
- (id)newTextureWithDescriptor:(MTLTextureDescriptor*)d {
    if (!d) return nil;
    emu_mtl_tex_desc x;
    memset(&x, 0, sizeof x);
    x.type = (uint32_t)d->_textureType;
    x.format = (uint32_t)d->_pixelFormat;
    x.width = (uint32_t)d->_width;
    x.height = (uint32_t)d->_height;
    x.depth = (uint32_t)d->_depth;
    x.mips = (uint32_t)d->_mipmapLevelCount;
    x.array_length = (uint32_t)d->_arrayLength;
    x.samples = (uint32_t)d->_sampleCount;
    x.usage = (uint32_t)d->_usage;
    x.storage = (uint32_t)d->_storageMode;
    uint64_t h = emu_mtl_texture_new(&x);
    if (!h) return nil;
    _MTLTexture* t = [[_MTLTexture alloc] init];
    t->_h = h;
    t->_d = x;
    t->_device = self;
    return t;
}
- (id)newTextureWithDescriptor:(id)d iosurface:(void*)s plane:(NSUInteger)p { (void)s; (void)p; return [self newTextureWithDescriptor:d]; }
- (id)newSamplerStateWithDescriptor:(MTLSamplerDescriptor*)d {
    if (!d) return nil;
    emu_mtl_sampler_desc x;
    memset(&x, 0, sizeof x);
    x.min_filter = (uint32_t)d->_minFilter;
    x.mag_filter = (uint32_t)d->_magFilter;
    x.mip_filter = (uint32_t)d->_mipFilter;
    x.s = (uint32_t)d->_s;
    x.t = (uint32_t)d->_t;
    x.r = (uint32_t)d->_r;
    x.max_aniso = (uint32_t)d->_maxAnisotropy;
    x.compare = (uint32_t)d->_compare;
    x.border = (uint32_t)d->_border;
    x.normalized = d->_normalized;
    x.lod_min = d->_lodMin;
    x.lod_max = d->_lodMax;
    _MTLSamplerState* s = [[_MTLSamplerState alloc] init];
    s->_h = emu_mtl_sampler_new(&x);
    s->_device = self;
    return s;
}
- (id)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor*)d {
    if (!d) return nil;
    emu_mtl_ds_desc x;
    memset(&x, 0, sizeof x);
    x.depth_func = (uint32_t)d->_depthCompare;
    x.depth_write = d->_depthWrite;
    if (d->_frontFaceStencil) { x.has_front = 1; [d->_frontFaceStencil fill:&x.front]; }
    if (d->_backFaceStencil) { x.has_back = 1; [d->_backFaceStencil fill:&x.back]; }
    _MTLDepthStencilState* s = [[_MTLDepthStencilState alloc] init];
    s->_h = emu_mtl_ds_new(&x);
    s->_device = self;
    return s;
}
- (id)newLibraryWithSource:(NSString*)src options:(id)o error:(NSError**)e {
    (void)o;
    char err[1024] = {0};
    uint64_t h = emu_mtl_library_new([src UTF8String], err, sizeof err);
    if (!h) {
        if (e) *e = [NSError errorWithDomain:MTLLibraryErrorDomain code:3
                                    userInfo:[NSDictionary dictionaryWithObject:[NSString stringWithUTF8String:err] forKey:@"NSLocalizedDescription"]];
        return nil;
    }
    if (e) *e = nil;
    _MTLLibrary* l = [[_MTLLibrary alloc] init];
    l->_h = h;
    l->_device = self;
    return l;
}
- (void)newLibraryWithSource:(NSString*)src options:(id)o completionHandler:(void (^)(id, NSError*))h {
    NSError* e = nil;
    id l = [self newLibraryWithSource:src options:o error:&e];
    if (h) h(l, e);
    [l release];
}
- (id)newDefaultLibrary { return nil; }
- (id)newDefaultLibraryWithBundle:(id)b error:(NSError**)e { (void)b; if (e) *e = nil; return nil; }
- (id)newLibraryWithData:(id)d error:(NSError**)e { (void)d; if (e) *e = nil; return nil; }
- (id)newLibraryWithFile:(NSString*)f error:(NSError**)e { (void)f; if (e) *e = nil; return nil; }
- (id)newLibraryWithURL:(id)u error:(NSError**)e { (void)u; if (e) *e = nil; return nil; }
- (id)_pipeline:(MTLRenderPipelineDescriptor*)d reflection:(id*)refl error:(NSError**)e {
    if (!d) return nil;
    emu_mtl_rp_desc x;
    memset(&x, 0, sizeof x);
    x.vfn = d->_vertexFunction ? d->_vertexFunction->_h : 0;
    x.ffn = d->_fragmentFunction ? d->_fragmentFunction->_h : 0;
    for (int i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor* a = [d->_colorAttachments existing:i];
        if (!a) { x.color[i].write_mask = 0xf; continue; }
        x.color[i].format = (uint32_t)a->_pixelFormat;
        x.color[i].blend = a->_blending;
        x.color[i].rgb_op = (uint32_t)a->_rgbOp;
        x.color[i].alpha_op = (uint32_t)a->_aOp;
        x.color[i].src_rgb = (uint32_t)a->_srcRGB;
        x.color[i].dst_rgb = (uint32_t)a->_dstRGB;
        x.color[i].src_a = (uint32_t)a->_srcA;
        x.color[i].dst_a = (uint32_t)a->_dstA;
        x.color[i].write_mask = (uint32_t)a->_writeMask;
    }
    x.depth_format = (uint32_t)d->_depthFormat;
    x.stencil_format = (uint32_t)d->_stencilFormat;
    x.samples = (uint32_t)d->_sampleCount;
    x.alpha_to_coverage = d->_alphaToCoverage;
    x.raster_enabled = d->_rasterEnabled;
    MTLVertexDescriptor* vd = d->_vertexDescriptor;
    if (getenv("EMU_MTL_TRACE")) NSLog(@"[Metal] pipeline vd %p attrs %p item0 %p item2 %p", vd, vd ? vd->_attributes : nil, vd ? [vd->_attributes existing:0] : nil, vd ? [vd->_attributes existing:2] : nil);
    if (vd) {
        x.has_vdesc = 1;
        for (int i = 0; i < 31; i++) {
            MTLVertexAttributeDescriptor* a = [vd->_attributes existing:i];
            if (a) { x.attr[i].format = (uint32_t)a->_format; x.attr[i].offset = (uint32_t)a->_offset; x.attr[i].buffer = (uint32_t)a->_bufferIndex; }
            MTLVertexBufferLayoutDescriptor* l = [vd->_layouts existing:i];
            if (l) { x.layout[i].stride = (uint32_t)l->_stride; x.layout[i].step_fn = (uint32_t)l->_stepFunction; x.layout[i].step_rate = (uint32_t)l->_stepRate; }
            else { x.layout[i].step_fn = 1; x.layout[i].step_rate = 1; }
        }
    }
    emu_mtl_refl r;
    memset(&r, 0, sizeof r);
    char err[2048] = {0};
    uint64_t h = emu_mtl_render_pipeline_new(&x, &r, err, sizeof err);
    if (!h) {
        if (e) *e = [NSError errorWithDomain:MTLLibraryErrorDomain code:1
                                    userInfo:[NSDictionary dictionaryWithObject:[NSString stringWithUTF8String:err] forKey:@"NSLocalizedDescription"]];
        if (refl) *refl = nil;
        return nil;
    }
    if (e) *e = nil;
    if (refl) {
        MTLRenderPipelineReflection* rf = [[[MTLRenderPipelineReflection alloc] init] autorelease];
        rf->_vertexArguments = [make_args(r.vertex, r.nvertex) retain];
        rf->_fragmentArguments = [make_args(r.fragment, r.nfragment) retain];
        *refl = rf;
    }
    _MTLRenderPipelineState* p = [[_MTLRenderPipelineState alloc] init];
    p->_h = h;
    p->_device = self;
    p->_label = [d->_label copy];
    return p;
}
- (id)newRenderPipelineStateWithDescriptor:(id)d error:(NSError**)e { return [self _pipeline:d reflection:NULL error:e]; }
- (id)newRenderPipelineStateWithDescriptor:(id)d options:(NSUInteger)o reflection:(id*)r error:(NSError**)e {
    return [self _pipeline:d reflection:(o ? r : NULL) error:e];
}
- (void)newRenderPipelineStateWithDescriptor:(id)d completionHandler:(void (^)(id, NSError*))h {
    NSError* e = nil;
    id p = [self _pipeline:d reflection:NULL error:&e];
    if (h) h(p, e);
    [p release];
}
- (id)_compute:(_MTLFunction*)f reflection:(id*)refl error:(NSError**)e {
    emu_mtl_refl r;
    memset(&r, 0, sizeof r);
    char err[1024] = {0};
    uint64_t h = emu_mtl_compute_pipeline_new(f ? f->_h : 0, &r, err, sizeof err);
    if (!h) {
        if (e) *e = [NSError errorWithDomain:MTLLibraryErrorDomain code:1
                                    userInfo:[NSDictionary dictionaryWithObject:[NSString stringWithUTF8String:err] forKey:@"NSLocalizedDescription"]];
        if (refl) *refl = nil;
        return nil;
    }
    if (e) *e = nil;
    if (refl) {
        MTLComputePipelineReflection* rf = [[[MTLComputePipelineReflection alloc] init] autorelease];
        rf->_arguments = [make_args(r.vertex, r.nvertex) retain];
        *refl = rf;
    }
    _MTLComputePipelineState* p = [[_MTLComputePipelineState alloc] init];
    p->_h = h;
    p->_device = self;
    return p;
}
- (id)newComputePipelineStateWithFunction:(id)f error:(NSError**)e { return [self _compute:f reflection:NULL error:e]; }
- (id)newComputePipelineStateWithFunction:(id)f options:(NSUInteger)o reflection:(id*)r error:(NSError**)e {
    return [self _compute:f reflection:(o ? r : NULL) error:e];
}
- (id)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor*)d options:(NSUInteger)o reflection:(id*)r error:(NSError**)e {
    return [self _compute:d->_computeFunction reflection:(o ? r : NULL) error:e];
}
- (id)newFence { _MTLDummy* d = [[_MTLDummy alloc] init]; d->_device = self; return d; }
- (id)newEvent { return [self newFence]; }
- (id)newSharedEvent { return [self newFence]; }
- (id)newHeapWithDescriptor:(id)d { (void)d; return [self newFence]; }
- (id)newArgumentEncoderWithArguments:(id)a { (void)a; return nil; }
- (id)newIndirectCommandBufferWithDescriptor:(id)d maxCommandCount:(NSUInteger)n options:(NSUInteger)o { (void)d; (void)n; (void)o; return nil; }
- (id)newCounterSampleBufferWithDescriptor:(id)d error:(NSError**)e { (void)d; if (e) *e = nil; return nil; }
- (id)newResidencySetWithDescriptor:(id)d error:(NSError**)e { (void)d; if (e) *e = nil; return [self newFence]; }
- (void)sampleTimestamps:(uint64_t*)c gpuTimestamp:(uint64_t*)g {
    uint64_t t = (uint64_t)(emu_time() * 1e9);
    if (c) *c = t;
    if (g) *g = t;
}
- (MTLSize)sparseTileSizeWithTextureType:(NSUInteger)t pixelFormat:(NSUInteger)f sampleCount:(NSUInteger)s {
    (void)t; (void)f; (void)s;
    return (MTLSize){0, 0, 0};
}
- (NSUInteger)heapBufferSizeAndAlignWithLength:(NSUInteger)l options:(NSUInteger)o { (void)o; return l; }
@end

static _MTLDevice* g_device;
static pthread_once_t g_device_once = PTHREAD_ONCE_INIT;
static void make_device(void) {
    if (!emu_mtl_available()) {
        NSLog(@"[Metal] not available (the emulator needs OpenGL 4.6)");
        return;
    }
    g_device = [[_MTLDevice alloc] init];
}
EMU_EXPORT id MTLCreateSystemDefaultDevice(void) {
    pthread_once(&g_device_once, make_device);
    return g_device;
}
EMU_EXPORT NSArray* MTLCopyAllDevices(void) {
    id d = MTLCreateSystemDefaultDevice();
    return d ? [[NSArray alloc] initWithObjects:d, nil] : [[NSArray alloc] init];
}

// ===================================================================== CAMetalLayer / MTKView
@interface CAMetalLayer : CALayer {
@public
    id _mdevice;
    NSUInteger _mformat, _maxDrawables, _drawableCount;
    BOOL _framebufferOnly, _presentsWithTransaction, _displaySync, _allowsTimeout, _edr, _sizeSet, _appSized;
    CGSize _drawableSize;
    void* _colorspace;
}
@end
@implementation CAMetalLayer
- (instancetype)init {
    if ((self = [super init])) {
        _mformat = 80;  // BGRA8Unorm
        _maxDrawables = 3;
        _framebufferOnly = YES;
        _displaySync = YES;
        _allowsTimeout = YES;
    }
    return self;
}
- (id)device { return _mdevice; }
- (void)setDevice:(id)d { _mdevice = d; }
- (id)preferredDevice { return MTLCreateSystemDefaultDevice(); }
- (NSUInteger)pixelFormat { return _mformat; }
- (void)setPixelFormat:(NSUInteger)f { _mformat = f; }
- (BOOL)framebufferOnly { return _framebufferOnly; }
- (void)setFramebufferOnly:(BOOL)b { _framebufferOnly = b; }
- (CGSize)drawableSize {
    if (_sizeSet) return _drawableSize;
    CGRect b = [self bounds];
    CGFloat s = [self contentsScale];
    return CGSizeMake(b.size.width * s, b.size.height * s);
}
- (void)setDrawableSize:(CGSize)s { _drawableSize = s; _sizeSet = YES; _appSized = YES; }
// (MTKView's automatic size: not the app's)
- (void)_emuSetAutoDrawableSize:(CGSize)s { _drawableSize = s; _sizeSet = YES; }
- (BOOL)_emuAppSized { return _appSized; }
- (BOOL)presentsWithTransaction { return _presentsWithTransaction; }
- (void)setPresentsWithTransaction:(BOOL)b { _presentsWithTransaction = b; }
- (BOOL)displaySyncEnabled { return _displaySync; }
- (void)setDisplaySyncEnabled:(BOOL)b { _displaySync = b; }
- (NSUInteger)maximumDrawableCount { return _maxDrawables; }
- (void)setMaximumDrawableCount:(NSUInteger)n { _maxDrawables = n; }
- (BOOL)allowsNextDrawableTimeout { return _allowsTimeout; }
- (void)setAllowsNextDrawableTimeout:(BOOL)b { _allowsTimeout = b; }
- (BOOL)wantsExtendedDynamicRangeContent { return _edr; }
- (void)setWantsExtendedDynamicRangeContent:(BOOL)b { _edr = b; }
- (void*)colorspace { return _colorspace; }
- (void)setColorspace:(void*)c { _colorspace = c; }
- (void)setMagnificationFilter:(NSString*)f { (void)f; }
- (void)setMinificationFilter:(NSString*)f { (void)f; }
- (id)developerHUDProperties { return nil; }
- (void)setDeveloperHUDProperties:(id)p { (void)p; }
- (id)nextDrawable {
    CGSize s = [self drawableSize];
    uint32_t w = (uint32_t)(s.width + 0.5), h = (uint32_t)(s.height + 0.5);
    if (!w || !h) return nil;
    uint64_t t = emu_mtl_next_drawable(w, h, (uint32_t)_mformat);
    if (!t) return nil;
    _CAMetalDrawable* d = [[_CAMetalDrawable alloc] init];
    _MTLTexture* tex = [[_MTLTexture alloc] init];
    tex->_borrowed = YES;
    emu_mtl_texture_info(t, &tex->_d);
    tex->_h = t;
    tex->_framebufferOnly = _framebufferOnly;
    tex->_device = _mdevice;
    d->_texture = tex;
    d->_layer = [self retain];
    d->_id = ++_drawableCount;
    return [d autorelease];
}
@end


@interface MTKView : UIView {
@public
    id _mtkDelegate;
    NSUInteger _depthFormat, _samples, _fps;
    BOOL _paused, _setNeedsDisplay, _autoResize, _appSized;
    MTLClearColor _clear;
    double _clearDepth;
    uint32_t _clearStencil;
    _CAMetalDrawable* _current;
}
@end
@implementation MTKView
+ (Class)layerClass { return [CAMetalLayer class]; }
- (instancetype)initWithFrame:(CGRect)f device:(id)d {
    if ((self = [self initWithFrame:f])) [(CAMetalLayer*)[self layer] setDevice:d];
    return self;
}
- (instancetype)initWithFrame:(CGRect)f {
    if ((self = [super initWithFrame:f])) {
        _fps = 60;
        _autoResize = YES;
        _clear = (MTLClearColor){0, 0, 0, 1};
        _clearDepth = 1;
        CGFloat s = [[UIScreen mainScreen] scale];
        [self setContentScaleFactor:s];
        [[self layer] setContentsScale:s];
    }
    return self;
}
- (void)dealloc { [_current release]; [super dealloc]; }
- (CAMetalLayer*)_ml { return (CAMetalLayer*)[self layer]; }
- (id)device { return [[self _ml] device]; }
- (void)setDevice:(id)d { [[self _ml] setDevice:d]; }
- (id)preferredDevice { return MTLCreateSystemDefaultDevice(); }
- (id)delegate { return _mtkDelegate; }
- (void)setDelegate:(id)d { _mtkDelegate = d; }
- (NSUInteger)colorPixelFormat { return [[self _ml] pixelFormat]; }
- (void)setColorPixelFormat:(NSUInteger)f { [[self _ml] setPixelFormat:f]; }
- (NSUInteger)depthStencilPixelFormat { return _depthFormat; }
- (void)setDepthStencilPixelFormat:(NSUInteger)f { _depthFormat = f; }
- (NSUInteger)sampleCount { return _samples ? _samples : 1; }
- (void)setSampleCount:(NSUInteger)n { _samples = n; }
- (BOOL)framebufferOnly { return [[self _ml] framebufferOnly]; }
- (void)setFramebufferOnly:(BOOL)b { [[self _ml] setFramebufferOnly:b]; }
- (BOOL)presentsWithTransaction { return [[self _ml] presentsWithTransaction]; }
- (void)setPresentsWithTransaction:(BOOL)b { [[self _ml] setPresentsWithTransaction:b]; }
- (CGSize)drawableSize { return [[self _ml] drawableSize]; }
// (an explicit size from the app: from then on the drawable keeps the app's size, see layoutSubviews)
- (void)setDrawableSize:(CGSize)s { _appSized = YES; [[self _ml] setDrawableSize:s]; }
- (BOOL)_emuAppSized { return _appSized; }
- (BOOL)autoResizeDrawable { return _autoResize; }
- (void)setAutoResizeDrawable:(BOOL)b { _autoResize = b; }
- (BOOL)isPaused { return _paused; }
- (void)setPaused:(BOOL)b { _paused = b; }
- (BOOL)enableSetNeedsDisplay { return _setNeedsDisplay; }
- (void)setEnableSetNeedsDisplay:(BOOL)b { _setNeedsDisplay = b; }
- (NSInteger)preferredFramesPerSecond { return (NSInteger)_fps; }
- (void)setPreferredFramesPerSecond:(NSInteger)f { _fps = (NSUInteger)f; }
- (MTLClearColor)clearColor { return _clear; }
- (void)setClearColor:(MTLClearColor)c { _clear = c; }
- (double)clearDepth { return _clearDepth; }
- (void)setClearDepth:(double)d { _clearDepth = d; }
- (uint32_t)clearStencil { return _clearStencil; }
- (void)setClearStencil:(uint32_t)s { _clearStencil = s; }
- (id)currentDrawable {
    if (!_current) _current = [[[self _ml] nextDrawable] retain];
    return _current;
}
- (id)currentRenderPassDescriptor {
    _CAMetalDrawable* d = [self currentDrawable];
    if (!d) return nil;
    MTLRenderPassDescriptor* p = [MTLRenderPassDescriptor renderPassDescriptor];
    MTLRenderPassColorAttachmentDescriptor* c = p.colorAttachments[0];
    [c setTexture:d->_texture];
    [c setLoadAction:2];
    [c setStoreAction:1];
    [c setClearColor:_clear];
    return p;
}
- (id)depthStencilTexture { return nil; }
- (id)multisampleColorTexture { return nil; }
- (void)releaseDrawables { [_current release]; _current = nil; }
- (void)draw {
    if ([_mtkDelegate respondsToSelector:@selector(drawInMTKView:)]) objc_msgSend(_mtkDelegate, @selector(drawInMTKView:), self);
    [_current release];
    _current = nil;
}
- (CGSize)preferredDrawableSize {
    CGRect b = [self bounds];
    CGFloat s = [self contentScaleFactor];
    return CGSizeMake(b.size.width * s, b.size.height * s);
}
// The drawable follows the view's size until the app sets a size of its own: Minecraft sizes its
// drawable in setRenderingSize, and a second, different size from the view (the emulator resizes
// the views with the window) made it reload its whole renderer and lose the UI's textures.
- (void)layoutSubviews {
    [super layoutSubviews];
    if (_autoResize && !_appSized && ![[self _ml] _emuAppSized]) {
        CGSize s = [self preferredDrawableSize];
        CGSize cur = [self drawableSize];
        if (s.width > 0 && s.height > 0 && (s.width != cur.width || s.height != cur.height)) {
            [[self _ml] _emuSetAutoDrawableSize:s];
            if ([_mtkDelegate respondsToSelector:@selector(mtkView:drawableSizeWillChange:)]) {
                void (*f)(id, SEL, id, CGSize) = (void (*)(id, SEL, id, CGSize))objc_msgSend;
                f(_mtkDelegate, @selector(mtkView:drawableSizeWillChange:), self, s);
            }
        }
    }
}
- (void)setColorspace:(void*)c { [[self _ml] setColorspace:c]; }
- (void*)colorspace { return [[self _ml] colorspace]; }
@end
