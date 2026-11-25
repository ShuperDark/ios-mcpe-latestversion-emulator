// Metal on OpenGL 4.6 (host side of guest/Metal).
//
// * Coordinates: glClipControl(UPPER_LEFT, ZERO_TO_ONE) gives GL Metal's conventions (y down
//   in window space, depth 0..1), so textures, viewports and scissors keep Metal's layout:
//   row 0 of every texture is the top. Only the final present flips.
// * Buffers are persistently mapped coherent GL buffers: MTLBuffer.contents is the mapping.
// * Commands run when they are encoded (each thread that uses Metal gets its own GL context
//   sharing the window's objects); commit waits until the GPU has finished them, so the
//   guest's completion handlers are correct.
// * Shaders: MSL -> GLSL (msl2glsl.cpp). Stage bindings: vertex buffers n -> SSBO n,
//   fragment buffers n -> SSBO 31+n; fragment textures n -> unit n, vertex textures -> 32+n.
#include "hle.h"
#include "msl2glsl.h"
#include "vmem.h"
#include "window.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <map>
#include <mutex>
#include <unordered_map>

#include "../guest/include/emu/metal_host.h"

namespace {

// ------------------------------------------------------------------ GL entry points
typedef ptrdiff_t GLsizeiptr_;
typedef ptrdiff_t GLintptr_;
typedef struct __GLsync* GLsync_;
#define GLFN(ret, name, args) using PFN_##name = ret(APIENTRY*) args; PFN_##name name##_ = nullptr;
#define GL_FUNCS(X) \
    X(void, glCreateBuffers, (GLsizei, GLuint*)) \
    X(void, glNamedBufferStorage, (GLuint, GLsizeiptr_, const void*, GLbitfield)) \
    X(void*, glMapNamedBufferRange, (GLuint, GLintptr_, GLsizeiptr_, GLbitfield)) \
    X(void, glDeleteBuffers, (GLsizei, const GLuint*)) \
    X(void, glCopyNamedBufferSubData, (GLuint, GLuint, GLintptr_, GLintptr_, GLsizeiptr_)) \
    X(void, glNamedBufferSubData, (GLuint, GLintptr_, GLsizeiptr_, const void*)) \
    X(void, glCreateTextures, (GLenum, GLsizei, GLuint*)) \
    X(void, glTextureStorage1D, (GLuint, GLsizei, GLenum, GLsizei)) \
    X(void, glTextureStorage2D, (GLuint, GLsizei, GLenum, GLsizei, GLsizei)) \
    X(void, glTextureStorage3D, (GLuint, GLsizei, GLenum, GLsizei, GLsizei, GLsizei)) \
    X(void, glTextureStorage2DMultisample, (GLuint, GLsizei, GLenum, GLsizei, GLsizei, GLboolean)) \
    X(void, glTextureSubImage1D, (GLuint, GLint, GLint, GLsizei, GLenum, GLenum, const void*)) \
    X(void, glTextureSubImage2D, (GLuint, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*)) \
    X(void, glTextureSubImage3D, (GLuint, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void*)) \
    X(void, glCompressedTextureSubImage2D, (GLuint, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void*)) \
    X(void, glCompressedTextureSubImage3D, (GLuint, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLsizei, const void*)) \
    X(void, glGetTextureSubImage, (GLuint, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, GLsizei, void*)) \
    X(void, glTextureParameteri, (GLuint, GLenum, GLint)) \
    X(void, glGenerateTextureMipmap, (GLuint)) \
    X(void, glTextureView, (GLuint, GLenum, GLuint, GLenum, GLuint, GLuint, GLuint, GLuint)) \
    X(void, glBindTextureUnit, (GLuint, GLuint)) \
    X(void, glCopyImageSubData, (GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei)) \
    X(void, glCreateSamplers, (GLsizei, GLuint*)) \
    X(void, glSamplerParameteri, (GLuint, GLenum, GLint)) \
    X(void, glSamplerParameterf, (GLuint, GLenum, GLfloat)) \
    X(void, glSamplerParameterfv, (GLuint, GLenum, const GLfloat*)) \
    X(void, glBindSampler, (GLuint, GLuint)) \
    X(void, glDeleteSamplers, (GLsizei, const GLuint*)) \
    X(GLuint, glCreateShader, (GLenum)) \
    X(void, glShaderSource, (GLuint, GLsizei, const char* const*, const GLint*)) \
    X(void, glCompileShader, (GLuint)) \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint*)) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, char*)) \
    X(void, glDeleteShader, (GLuint)) \
    X(GLuint, glCreateProgram, ()) \
    X(void, glAttachShader, (GLuint, GLuint)) \
    X(void, glLinkProgram, (GLuint)) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint*)) \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, char*)) \
    X(void, glDeleteProgram, (GLuint)) \
    X(void, glUseProgram, (GLuint)) \
    X(void, glCreateVertexArrays, (GLsizei, GLuint*)) \
    X(void, glBindVertexArray, (GLuint)) \
    X(void, glVertexArrayAttribFormat, (GLuint, GLuint, GLint, GLenum, GLboolean, GLuint)) \
    X(void, glVertexArrayAttribIFormat, (GLuint, GLuint, GLint, GLenum, GLuint)) \
    X(void, glVertexArrayAttribBinding, (GLuint, GLuint, GLuint)) \
    X(void, glVertexArrayBindingDivisor, (GLuint, GLuint, GLuint)) \
    X(void, glEnableVertexArrayAttrib, (GLuint, GLuint)) \
    X(void, glDisableVertexArrayAttrib, (GLuint, GLuint)) \
    X(void, glVertexArrayVertexBuffer, (GLuint, GLuint, GLuint, GLintptr_, GLsizei)) \
    X(void, glVertexArrayElementBuffer, (GLuint, GLuint)) \
    X(void, glVertexAttrib4f, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glBindBufferRange, (GLenum, GLuint, GLuint, GLintptr_, GLsizeiptr_)) \
    X(void, glBindImageTexture, (GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum)) \
    X(void, glCreateFramebuffers, (GLsizei, GLuint*)) \
    X(void, glDeleteFramebuffers, (GLsizei, const GLuint*)) \
    X(void, glNamedFramebufferTexture, (GLuint, GLenum, GLuint, GLint)) \
    X(void, glNamedFramebufferTextureLayer, (GLuint, GLenum, GLuint, GLint, GLint)) \
    X(void, glNamedFramebufferDrawBuffers, (GLuint, GLsizei, const GLenum*)) \
    X(void, glNamedFramebufferParameteri, (GLuint, GLenum, GLint)) \
    X(GLenum, glCheckNamedFramebufferStatus, (GLuint, GLenum)) \
    X(void, glBindFramebuffer, (GLenum, GLuint)) \
    X(void, glClearNamedFramebufferfv, (GLuint, GLenum, GLint, const GLfloat*)) \
    X(void, glClearNamedFramebufferiv, (GLuint, GLenum, GLint, const GLint*)) \
    X(void, glClearNamedFramebufferuiv, (GLuint, GLenum, GLint, const GLuint*)) \
    X(void, glClearNamedFramebufferfi, (GLuint, GLenum, GLint, GLfloat, GLint)) \
    X(void, glBlitNamedFramebuffer, (GLuint, GLuint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void, glCreateRenderbuffers, (GLsizei, GLuint*)) \
    X(void, glNamedRenderbufferStorage, (GLuint, GLenum, GLsizei, GLsizei)) \
    X(void, glNamedFramebufferRenderbuffer, (GLuint, GLenum, GLenum, GLuint)) \
    X(void, glEnablei, (GLenum, GLuint)) \
    X(void, glDisablei, (GLenum, GLuint)) \
    X(void, glBlendFuncSeparatei, (GLuint, GLenum, GLenum, GLenum, GLenum)) \
    X(void, glBlendEquationSeparatei, (GLuint, GLenum, GLenum)) \
    X(void, glColorMaski, (GLuint, GLboolean, GLboolean, GLboolean, GLboolean)) \
    X(void, glBlendColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glStencilFuncSeparate, (GLenum, GLenum, GLint, GLuint)) \
    X(void, glStencilOpSeparate, (GLenum, GLenum, GLenum, GLenum)) \
    X(void, glStencilMaskSeparate, (GLenum, GLuint)) \
    X(void, glDepthRangef, (GLfloat, GLfloat)) \
    X(void, glPolygonOffsetClamp, (GLfloat, GLfloat, GLfloat)) \
    X(void, glClipControl, (GLenum, GLenum)) \
    X(void, glDrawArraysInstancedBaseInstance, (GLenum, GLint, GLsizei, GLsizei, GLuint)) \
    X(void, glDrawElementsInstancedBaseVertexBaseInstance, (GLenum, GLsizei, GLenum, const void*, GLsizei, GLint, GLuint)) \
    X(void, glDispatchCompute, (GLuint, GLuint, GLuint)) \
    X(void, glMemoryBarrier, (GLbitfield)) \
    X(GLsync_, glFenceSync, (GLenum, GLbitfield)) \
    X(GLenum, glClientWaitSync, (GLsync_, GLbitfield, unsigned long long)) \
    X(void, glDeleteSync, (GLsync_)) \
    X(void, glObjectLabel, (GLenum, GLuint, GLsizei, const char*)) \
    X(void, glDebugMessageCallback, (void*, const void*))
GL_FUNCS(GLFN)
#undef GLFN

std::once_flag g_load_once;
bool g_gl_ok = false;
GLint g_ssbo_align = 16, g_max_bindings = 16;

void load_gl() {
    std::call_once(g_load_once, [] {
        bool ok = true;
#define GLLOAD(ret, name, args)                                       \
    name##_ = (PFN_##name)window::gl_proc(#name);                     \
    if (!name##_) { LOGW("Metal: GL function %s missing", #name); ok = false; }
        GL_FUNCS(GLLOAD)
#undef GLLOAD
        g_gl_ok = ok || (glCreateBuffers_ && glClipControl_ && glNamedBufferStorage_);
        glGetIntegerv(0x90DF, &g_ssbo_align);   // GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT
        glGetIntegerv(0x82DA, &g_max_bindings); // GL_MAX_VERTEX_ATTRIB_BINDINGS
        if (g_ssbo_align <= 0) g_ssbo_align = 256;
        LOGI("Metal on OpenGL: %s (storage buffer alignment %d, %d vertex bindings)", g_gl_ok ? "available" : "NOT available",
             g_ssbo_align, g_max_bindings);
    });
}

// ------------------------------------------------------------------ objects
std::atomic<u64> g_serial{1};

struct Buffer {
    GLuint id = 0;
    u8* ptr = nullptr;
    u64 len = 0;
};

struct PixelFormat {
    GLenum internal = 0, fmt = 0, type = 0;
    u32 bpp = 0;                      // bytes per pixel (uncompressed)
    u32 bw = 1, bh = 1, bbytes = 0;   // block size (compressed)
    bool depth = false, stencil = false;
    char kind = 'f';                  // f / i / u (clear and sampling)
};

PixelFormat pixel_format(u32 f) {
    // GL_UNSIGNED_BYTE 0x1401, BYTE 0x1400, USHORT 0x1403, SHORT 0x1402, UINT 0x1405, INT 0x1404, FLOAT 0x1406, HALF 0x140B
    // RED 0x1903, RG 0x8227, RGB 0x1907, RGBA 0x1908, BGRA 0x80E1, *_INTEGER: RED 0x8D94, RG 0x8228, RGBA 0x8D99
    switch (f) {
    case 1: return {0x8229, 0x1903, 0x1401, 1};                 // A8Unorm -> R8 (sampled .a: swizzle below)
    case 10: return {0x8229, 0x1903, 0x1401, 1};                // R8Unorm
    case 11: return {0x8229, 0x1903, 0x1401, 1};                // R8Unorm_sRGB (approx.)
    case 12: return {0x8F94, 0x1903, 0x1400, 1};                // R8Snorm
    case 13: return {0x8232, 0x8D94, 0x1401, 1, 1, 1, 0, false, false, 'u'};  // R8Uint
    case 14: return {0x8231, 0x8D94, 0x1400, 1, 1, 1, 0, false, false, 'i'};  // R8Sint
    case 20: return {0x822A, 0x1903, 0x1403, 2};                // R16Unorm
    case 22: return {0x8F98, 0x1903, 0x1402, 2};                // R16Snorm
    case 23: return {0x8234, 0x8D94, 0x1403, 2, 1, 1, 0, false, false, 'u'};
    case 24: return {0x8233, 0x8D94, 0x1402, 2, 1, 1, 0, false, false, 'i'};
    case 25: return {0x822D, 0x1903, 0x140B, 2};                // R16Float
    case 30: case 31: return {0x822B, 0x8227, 0x1401, 2};       // RG8Unorm
    case 32: return {0x8F95, 0x8227, 0x1400, 2};
    case 33: return {0x8238, 0x8228, 0x1401, 2, 1, 1, 0, false, false, 'u'};
    case 34: return {0x8237, 0x8228, 0x1400, 2, 1, 1, 0, false, false, 'i'};
    case 40: return {0x8D62, 0x1907, 0x8363, 2};                // B5G6R5Unorm -> RGB565
    case 41: case 43: return {0x8057, 0x80E1, 0x8366, 2};       // BGR5A1 / A1BGR5 (approx.)
    case 42: return {0x8056, 0x1908, 0x8033, 2};                // ABGR4Unorm (approx.)
    case 53: return {0x8236, 0x8D94, 0x1405, 4, 1, 1, 0, false, false, 'u'};  // R32Uint
    case 54: return {0x8235, 0x8D94, 0x1404, 4, 1, 1, 0, false, false, 'i'};
    case 55: return {0x822E, 0x1903, 0x1406, 4};                // R32Float
    case 60: return {0x822C, 0x8227, 0x1403, 4};                // RG16Unorm
    case 62: return {0x8F99, 0x8227, 0x1402, 4};
    case 63: return {0x823A, 0x8228, 0x1403, 4, 1, 1, 0, false, false, 'u'};
    case 64: return {0x8239, 0x8228, 0x1402, 4, 1, 1, 0, false, false, 'i'};
    case 65: return {0x822F, 0x8227, 0x140B, 4};                // RG16Float
    case 70: return {0x8058, 0x1908, 0x1401, 4};                // RGBA8Unorm
    case 71: return {0x8C43, 0x1908, 0x1401, 4};                // RGBA8Unorm_sRGB
    case 72: return {0x8F97, 0x1908, 0x1400, 4};
    case 73: return {0x8D7C, 0x8D99, 0x1401, 4, 1, 1, 0, false, false, 'u'};
    case 74: return {0x8D8E, 0x8D99, 0x1400, 4, 1, 1, 0, false, false, 'i'};
    case 80: return {0x8058, 0x80E1, 0x1401, 4};                // BGRA8Unorm
    case 81: return {0x8C43, 0x80E1, 0x1401, 4};                // BGRA8Unorm_sRGB
    case 90: case 94: return {0x8059, 0x1908, 0x8368, 4};       // RGB10A2Unorm / BGR10A2
    case 91: return {0x906F, 0x8D99, 0x8368, 4, 1, 1, 0, false, false, 'u'};
    case 92: return {0x8C3A, 0x1907, 0x8C3B, 4};                // RG11B10Float
    case 93: return {0x8C3D, 0x1907, 0x8C3E, 4};                // RGB9E5Float
    case 103: return {0x823C, 0x8228, 0x1405, 8, 1, 1, 0, false, false, 'u'};
    case 104: return {0x823B, 0x8228, 0x1404, 8, 1, 1, 0, false, false, 'i'};
    case 105: return {0x8230, 0x8227, 0x1406, 8};               // RG32Float
    case 110: return {0x805B, 0x1908, 0x1403, 8};               // RGBA16Unorm
    case 112: return {0x8F9B, 0x1908, 0x1402, 8};
    case 113: return {0x8D76, 0x8D99, 0x1403, 8, 1, 1, 0, false, false, 'u'};
    case 114: return {0x8D88, 0x8D99, 0x1402, 8, 1, 1, 0, false, false, 'i'};
    case 115: return {0x881A, 0x1908, 0x140B, 8};               // RGBA16Float
    case 123: return {0x8D70, 0x8D99, 0x1405, 16, 1, 1, 0, false, false, 'u'};
    case 124: return {0x8D82, 0x8D99, 0x1404, 16, 1, 1, 0, false, false, 'i'};
    case 125: return {0x8814, 0x1908, 0x1406, 16};              // RGBA32Float
    // BC
    case 130: return {0x83F1, 0, 0, 0, 4, 4, 8};
    case 131: return {0x8C4D, 0, 0, 0, 4, 4, 8};
    case 132: return {0x83F2, 0, 0, 0, 4, 4, 16};
    case 133: return {0x8C4E, 0, 0, 0, 4, 4, 16};
    case 134: return {0x83F3, 0, 0, 0, 4, 4, 16};
    case 135: return {0x8C4F, 0, 0, 0, 4, 4, 16};
    case 140: return {0x8DBB, 0, 0, 0, 4, 4, 8};
    case 141: return {0x8DBC, 0, 0, 0, 4, 4, 8};
    case 142: return {0x8DBD, 0, 0, 0, 4, 4, 16};
    case 143: return {0x8DBE, 0, 0, 0, 4, 4, 16};
    case 150: return {0x8E8F, 0, 0, 0, 4, 4, 16};
    case 151: return {0x8E8E, 0, 0, 0, 4, 4, 16};
    case 152: return {0x8E8C, 0, 0, 0, 4, 4, 16};
    case 153: return {0x8E8D, 0, 0, 0, 4, 4, 16};
    // ETC2 / EAC
    case 170: return {0x9270, 0, 0, 0, 4, 4, 8};
    case 172: return {0x9271, 0, 0, 0, 4, 4, 8};
    case 174: return {0x9272, 0, 0, 0, 4, 4, 16};
    case 176: return {0x9273, 0, 0, 0, 4, 4, 16};
    case 178: return {0x9278, 0, 0, 0, 4, 4, 16};
    case 179: return {0x9279, 0, 0, 0, 4, 4, 16};
    case 180: return {0x9274, 0, 0, 0, 4, 4, 8};
    case 181: return {0x9275, 0, 0, 0, 4, 4, 8};
    case 182: return {0x9276, 0, 0, 0, 4, 4, 8};
    case 183: return {0x9277, 0, 0, 0, 4, 4, 8};
    // depth / stencil
    case 250: return {0x81A5, 0x1902, 0x1403, 2, 1, 1, 0, true, false};            // Depth16Unorm
    case 252: return {0x8CAC, 0x1902, 0x1406, 4, 1, 1, 0, true, false};            // Depth32Float
    case 253: return {0x8D48, 0x1901, 0x1401, 1, 1, 1, 0, false, true, 'u'};       // Stencil8
    case 255: return {0x88F0, 0x84F9, 0x84FA, 4, 1, 1, 0, true, true};             // Depth24Unorm_Stencil8
    case 260: return {0x8CAD, 0x84F9, 0x8DAD, 8, 1, 1, 0, true, true};             // Depth32Float_Stencil8
    case 261: case 262: return {0x8D48, 0x1901, 0x1401, 1, 1, 1, 0, false, true, 'u'};
    case 552: case 553: return {0x881A, 0x1908, 0x140B, 8};    // BGRA10_XR -> RGBA16F
    default: break;
    }
    // ASTC LDR 4x4 (204) ... 12x12 (217), sRGB 186..199
    static const u8 astc[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    if (f >= 204 && f < 218) return {GLenum(0x93B0 + (f - 204)), 0, 0, 0, astc[f - 204][0], astc[f - 204][1], 16};
    if (f >= 186 && f < 200) return {GLenum(0x93D0 + (f - 186)), 0, 0, 0, astc[f - 186][0], astc[f - 186][1], 16};
    return {0x8058, 0x1908, 0x1401, 4};
}

struct Texture {
    GLuint id = 0;
    GLenum target = 0x0DE1;
    emu_mtl_tex_desc d{};
    PixelFormat pf;
    u64 serial = 0;
    bool view = false;
};

GLenum target_of(u32 type) {
    switch (type) {
    case 0: return 0x0DE0;   // 1D
    case 1: return 0x8C18;   // 1D array
    case 3: return 0x8C1A;   // 2D array
    case 4: return 0x9100;   // 2D multisample
    case 5: return 0x8513;   // cube
    case 6: return 0x9009;   // cube array
    case 7: return 0x806F;   // 3D
    case 8: return 0x9102;   // 2D multisample array
    default: return 0x0DE1;  // 2D
    }
}

struct Sampler { GLuint id = 0; };
struct DepthStencil { emu_mtl_ds_desc d{}; };

struct Library {
    std::shared_ptr<msl::Library> lib;
};
struct Function {
    std::shared_ptr<msl::Library> lib;
    std::shared_ptr<msl::Function> fn;
};

struct VertexAttr {
    GLint size = 4;
    GLenum type = 0x1406;
    bool normalized = false, integer = false;
};

struct Program {
    GLuint prog = 0;
    std::shared_ptr<msl::Function> vs, fs;
};

struct Pipeline {
    bool compute = false;
    std::shared_ptr<Program> prog;
    emu_mtl_rp_desc d{};
    // vertex inputs: location -> attribute format (from the vertex descriptor)
    struct In { u32 loc; VertexAttr fmt; u32 offset, buffer; bool shader_int; };
    std::vector<In> inputs;
    std::vector<u32> vbuffers;  // buffer indices of the vertex descriptor
    std::shared_ptr<msl::Function> kernel;  // compute
    std::map<u64, GLuint> kernel_progs;     // by threadgroup size
};

std::mutex g_prog_lock;
std::map<std::pair<msl::Function*, msl::Function*>, std::weak_ptr<Program>> g_programs;

// ------------------------------------------------------------------ per-thread GL context state
struct Binding {
    Buffer* buf = nullptr;
    u64 off = 0;
};
struct StageState {
    Binding buffers[31];
    Texture* tex[31] = {};
    Sampler* smp[31] = {};
};

struct Encoder {
    GLuint fbo = 0;
    u32 w = 0, h = 0;
    Pipeline* pipe = nullptr;
    StageState st[2];
    DepthStencil* ds = nullptr;
    u32 stencil_front = 0, stencil_back = 0;
    u32 cull = 0, winding = 0, fill = 0;
    bool dirty_vertex = true;
    emu_mtl_pass_desc pass{};
    bool active = false;
};

struct Ctx {
    void* rc = nullptr;
    GLuint vao = 0;
    // framebuffer cache: attachment key -> fbo
    std::map<std::string, GLuint> fbos;
    // scratch buffer for setVertexBytes / misaligned constant buffers
    GLuint scratch = 0;
    u8* scratch_ptr = nullptr;
    u64 scratch_size = 0, scratch_pos = 0;
    Encoder enc;
    // compute state
    Pipeline* cpipe = nullptr;
    StageState cst;
    Pipeline* vao_pipe = nullptr;  // vertex layout currently in the VAO
    GLuint present_rb = 0, present_fbo = 0, present_src_fbo = 0;
    int present_w = 0, present_h = 0;
};
thread_local Ctx* t_ctx = nullptr;

// EMU_MTL_TRACE=<frames>: logs the render passes and draws of the first frames
// EMU_MTL_TRACE_AT=<seconds>: starts the trace at that time (after loading a world, ...)
int g_trace_frames = [] { const char* e = getenv("EMU_MTL_TRACE"); return e ? atoi(e) : 0; }();
double g_trace_at = [] { const char* e = getenv("EMU_MTL_TRACE_AT"); return e ? atof(e) : 0.0; }();
std::atomic<int> g_frame_no{0};
std::atomic<int> g_trace_start{-1};
bool tracing() {
    if (g_trace_frames <= 0) return false;
    if (g_trace_at > 0) {
        if (g_trace_start < 0) {
            if (window::now() < g_trace_at) return false;
            g_trace_start = g_frame_no.load();
        }
        return g_frame_no.load() - g_trace_start < g_trace_frames;
    }
    return g_frame_no.load() < g_trace_frames;
}
#define MTRACE(...) do { if (tracing()) LOGI("[mtl] " __VA_ARGS__); } while (0)

Ctx* ctx() {
    if (t_ctx) return t_ctx;
    load_gl();
    auto* c = new Ctx;
    c->rc = window::gl_make_shared_current();
    if (!c->rc) LOGE("Metal: no OpenGL context on this thread");
    glCreateVertexArrays_(1, &c->vao);
    glBindVertexArray_(c->vao);
    glClipControl_(0x8CA2, 0x935F);   // GL_UPPER_LEFT, GL_ZERO_TO_ONE
    glEnable(0x8DB9);                 // GL_FRAMEBUFFER_SRGB
    glEnable(0x8D69);                 // GL_PRIMITIVE_RESTART_FIXED_INDEX
    glEnable(0x8642);                 // GL_PROGRAM_POINT_SIZE
    glEnable(0x884F);                 // GL_TEXTURE_CUBE_MAP_SEAMLESS
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    c->scratch_size = 8 << 20;
    glCreateBuffers_(1, &c->scratch);
    glNamedBufferStorage_(c->scratch, (GLsizeiptr_)c->scratch_size, nullptr, 0x2 | 0x40 | 0x80 | 0x100);
    c->scratch_ptr = (u8*)glMapNamedBufferRange_(c->scratch, 0, (GLsizeiptr_)c->scratch_size, 0x2 | 0x40 | 0x80);
    t_ctx = c;
    LOGI("Metal: GL context for thread %u", threads::current() ? threads::current()->id : 0);
    return c;
}

// copies bytes into the scratch buffer; returns the offset
u64 scratch_put(Ctx* c, const void* p, u64 len) {
    u64 a = (u64)g_ssbo_align;
    u64 off = (c->scratch_pos + a - 1) / a * a;
    if (off + len > c->scratch_size) {
        // the GPU may still read the old contents: wait for it before wrapping
        glFinish();
        off = 0;
    }
    memcpy(c->scratch_ptr + off, p, len);
    c->scratch_pos = off + len;
    return off;
}

// ------------------------------------------------------------------ GL enum mapping
GLenum blend_factor(u32 f) {
    static const GLenum t[] = {0, 1, 0x0300, 0x0301, 0x0302, 0x0303, 0x0306, 0x0307, 0x0304, 0x0305, 0x0308, 0x8001, 0x8002,
                               0x8003, 0x8004, 0x88F9, 0x88FA, 0x8589, 0x88FB};
    return f < sizeof t / sizeof t[0] ? t[f] : 1;
}
GLenum blend_op(u32 o) {
    static const GLenum t[] = {0x8006, 0x800A, 0x800B, 0x8007, 0x8008};
    return o < 5 ? t[o] : 0x8006;
}
GLenum compare(u32 f) { return 0x0200 + (f & 7); }  // Never..Always = GL_NEVER..GL_ALWAYS (same order)
GLenum stencil_op(u32 o) {
    static const GLenum t[] = {0x1E00, 0, 0x1E01, 0x1E02, 0x1E03, 0x150A, 0x8507, 0x8508};
    return o < 8 ? t[o] : 0x1E00;
}
GLenum prim(u32 p) {
    static const GLenum t[] = {0x0000, 0x0001, 0x0003, 0x0004, 0x0005};
    return p < 5 ? t[p] : 0x0004;
}

VertexAttr vertex_format(u32 f) {
    // MTLVertexFormat
    const GLenum UB = 0x1401, B = 0x1400, US = 0x1403, S = 0x1402, H = 0x140B, F = 0x1406, I = 0x1404, UI = 0x1405;
    switch (f) {
    case 1: return {2, UB}; case 2: return {3, UB}; case 3: return {4, UB};
    case 4: return {2, B}; case 5: return {3, B}; case 6: return {4, B};
    case 7: return {2, UB, true}; case 8: return {3, UB, true}; case 9: return {4, UB, true};
    case 10: return {2, B, true}; case 11: return {3, B, true}; case 12: return {4, B, true};
    case 13: return {2, US}; case 14: return {3, US}; case 15: return {4, US};
    case 16: return {2, S}; case 17: return {3, S}; case 18: return {4, S};
    case 19: return {2, US, true}; case 20: return {3, US, true}; case 21: return {4, US, true};
    case 22: return {2, S, true}; case 23: return {3, S, true}; case 24: return {4, S, true};
    case 25: return {2, H}; case 26: return {3, H}; case 27: return {4, H};
    case 28: return {1, F}; case 29: return {2, F}; case 30: return {3, F}; case 31: return {4, F};
    case 32: return {1, I}; case 33: return {2, I}; case 34: return {3, I}; case 35: return {4, I};
    case 36: return {1, UI}; case 37: return {2, UI}; case 38: return {3, UI}; case 39: return {4, UI};
    case 40: return {4, 0x8D9F, true}; case 41: return {4, 0x8368, true};
    case 42: return {0x80E1, UB, true};  // UChar4Normalized_BGRA
    case 45: return {1, UB}; case 46: return {1, B}; case 47: return {1, UB, true}; case 48: return {1, B, true};
    case 49: return {1, US}; case 50: return {1, S}; case 51: return {1, US, true}; case 52: return {1, S, true};
    case 53: return {1, H};
    default: return {4, F};
    }
}

// ------------------------------------------------------------------ shaders
GLuint compile(GLenum type, const std::string& src, std::string& err) {
    GLuint s = glCreateShader_(type);
    const char* p = src.c_str();
    glShaderSource_(s, 1, &p, nullptr);
    glCompileShader_(s);
    GLint ok = 0;
    glGetShaderiv_(s, 0x8B81, &ok);
    if (!ok) {
        char log[8192] = {0};
        glGetShaderInfoLog_(s, sizeof log, nullptr, log);
        err = log;
        glDeleteShader_(s);
        return 0;
    }
    return s;
}
GLuint link(std::initializer_list<GLuint> shaders, std::string& err) {
    GLuint p = glCreateProgram_();
    for (GLuint s : shaders) glAttachShader_(p, s);
    glLinkProgram_(p);
    for (GLuint s : shaders) glDeleteShader_(s);
    GLint ok = 0;
    glGetProgramiv_(p, 0x8B82, &ok);
    if (!ok) {
        char log[8192] = {0};
        glGetProgramInfoLog_(p, sizeof log, nullptr, log);
        err = log;
        glDeleteProgram_(p);
        return 0;
    }
    return p;
}

void put_err(char* err, u32 n, const std::string& s) {
    if (!err || !n) return;
    size_t k = std::min<size_t>(s.size(), n - 1);
    memcpy(err, s.data(), k);
    err[k] = 0;
}

const char* gstr(const std::string& s) {
    char* p = (char*)vmem::gmalloc(s.size() + 1);
    memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

// reflection arguments of one function
std::pair<u32, const emu_mtl_refl_arg*> reflect(const msl::Function& f) {
    size_t n = f.buffers.size() + f.textures.size() + f.samplers.size();
    auto* a = (emu_mtl_refl_arg*)vmem::gcalloc(n ? n : 1, sizeof(emu_mtl_refl_arg));
    size_t k = 0;
    for (auto& b : f.buffers) {
        emu_mtl_refl_arg& r = a[k++];
        r.name = gstr(b.name);
        r.type = 0;
        r.index = b.index;
        r.active = 1;
        r.data_type = b.is_struct ? msl::DT_Struct : msl::DT_Float4;
        r.data_size = b.size;
        r.alignment = 16;
        r.access = b.writable ? 2 : 0;
        r.nmembers = (u32)b.members.size();
        auto* m = (emu_mtl_refl_member*)vmem::gcalloc(b.members.size() ? b.members.size() : 1, sizeof(emu_mtl_refl_member));
        for (size_t i = 0; i < b.members.size(); i++) {
            m[i].name = gstr(b.members[i].name);
            m[i].offset = b.members[i].offset;
            m[i].data_type = b.members[i].data_type;
            m[i].array_len = b.members[i].array_len;
            m[i].stride = b.members[i].stride;
            m[i].size = b.members[i].size;
        }
        r.members = m;
    }
    for (auto& t : f.textures) {
        emu_mtl_refl_arg& r = a[k++];
        r.name = gstr(t.name);
        r.type = 2;
        r.index = t.index;
        r.active = 1;
        r.texture_type = t.mtl_type;
        r.texture_data_type = t.component == 'u' ? msl::DT_UInt : t.component == 'i' ? msl::DT_Int : msl::DT_Float;
        r.access = t.image ? 2 : 0;
    }
    for (auto& s : f.samplers) {
        emu_mtl_refl_arg& r = a[k++];
        r.name = gstr(s.name);
        r.type = 3;
        r.index = s.index;
        r.active = 1;
    }
    return {(u32)n, a};
}

// ------------------------------------------------------------------ framebuffers
GLenum attach_point(const Texture* t, int color_index) {
    if (color_index >= 0) return 0x8CE0 + color_index;  // GL_COLOR_ATTACHMENTi
    if (t->pf.depth && t->pf.stencil) return 0x821A;     // GL_DEPTH_STENCIL_ATTACHMENT
    if (t->pf.stencil) return 0x8D20;                    // GL_STENCIL_ATTACHMENT
    return 0x8D00;                                       // GL_DEPTH_ATTACHMENT
}
void attach(GLuint fbo, GLenum point, const Texture* t, u32 level, u32 slice, u32 plane) {
    if (!t) {
        glNamedFramebufferTexture_(fbo, point, 0, 0);
        return;
    }
    u32 type = t->d.type;
    if (type == 2 || type == 4 || type == 0) glNamedFramebufferTexture_(fbo, point, t->id, (GLint)level);
    else glNamedFramebufferTextureLayer_(fbo, point, t->id, (GLint)level, (GLint)(type == 7 ? plane : slice));
}

GLuint pass_fbo(Ctx* c, const emu_mtl_pass_desc& d, u32& w, u32& h) {
    std::string key;
    char buf[96];
    w = h = 0;
    auto add = [&](const emu_mtl_pass_att& a, int i) {
        auto* t = (Texture*)a.tex;
        if (!t) return;
        snprintf(buf, sizeof buf, "%d:%llu/%u/%u/%u;", i, (unsigned long long)t->serial, a.level, a.slice, a.depth_plane);
        key += buf;
        u32 tw = std::max(1u, t->d.width >> a.level), th = std::max(1u, t->d.height >> a.level);
        if (!w || tw < w) w = tw;
        if (!h || th < h) h = th;
    };
    for (int i = 0; i < 8; i++) add(d.color[i], i);
    add(d.depth, 8);
    add(d.stencil, 9);
    if (key.empty()) {
        w = d.width ? d.width : 1;
        h = d.height ? d.height : 1;
        snprintf(buf, sizeof buf, "empty %ux%u", w, h);
        key = buf;
    }
    auto it = c->fbos.find(key);
    if (it != c->fbos.end()) return it->second;
    GLuint fbo = 0;
    glCreateFramebuffers_(1, &fbo);
    GLenum bufs[8];
    int nb = 0;
    for (int i = 0; i < 8; i++) {
        auto* t = (Texture*)d.color[i].tex;
        bufs[i] = t ? 0x8CE0 + i : 0;
        if (t) {
            attach(fbo, 0x8CE0 + i, t, d.color[i].level, d.color[i].slice, d.color[i].depth_plane);
            nb = i + 1;
        }
    }
    glNamedFramebufferDrawBuffers_(fbo, nb, bufs);
    auto* dt = (Texture*)d.depth.tex;
    auto* stt = (Texture*)d.stencil.tex;
    if (dt) attach(fbo, attach_point(dt, -1), dt, d.depth.level, d.depth.slice, 0);
    if (stt && stt != dt) attach(fbo, 0x8D20, stt, d.stencil.level, d.stencil.slice, 0);
    if (!nb && !dt && !stt) {
        glNamedFramebufferParameteri_(fbo, 0x9310, (GLint)w);  // GL_FRAMEBUFFER_DEFAULT_WIDTH
        glNamedFramebufferParameteri_(fbo, 0x9311, (GLint)h);
    }
    GLenum st = glCheckNamedFramebufferStatus_(fbo, 0x8D40);
    if (st != 0x8CD5) LOGW("Metal: render pass framebuffer incomplete (0x%x) %s", st, key.c_str());
    if (c->fbos.size() > 512) {
        for (auto& [k, f] : c->fbos) glDeleteFramebuffers_(1, &f);
        c->fbos.clear();
    }
    c->fbos[key] = fbo;
    return fbo;
}

// ------------------------------------------------------------------ draw state
void apply_pipeline_state(Ctx* c, Encoder& e) {
    Pipeline* p = e.pipe;
    if (!p || !p->prog) return;
    glUseProgram_(p->prog->prog);
    for (u32 i = 0; i < 8; i++) {
        const emu_mtl_color_att& a = p->d.color[i];
        if (a.blend) {
            glEnablei_(GL_BLEND, i);
            glBlendFuncSeparatei_(i, blend_factor(a.src_rgb), blend_factor(a.dst_rgb), blend_factor(a.src_a), blend_factor(a.dst_a));
            glBlendEquationSeparatei_(i, blend_op(a.rgb_op), blend_op(a.alpha_op));
        } else {
            glDisablei_(GL_BLEND, i);
        }
        u32 m = a.write_mask;
        glColorMaski_(i, (m & 8) != 0, (m & 4) != 0, (m & 2) != 0, (m & 1) != 0);
    }
    if (p->d.alpha_to_coverage) glEnable(0x809E);
    else glDisable(0x809E);
    if (!p->d.raster_enabled) glEnable(0x8C89);  // GL_RASTERIZER_DISCARD
    else glDisable(0x8C89);
    // vertex layout
    if (c->vao_pipe != p) {
        for (u32 l = 0; l < 16; l++) glDisableVertexArrayAttrib_(c->vao, l);
        for (auto& in : p->inputs) {
            glEnableVertexArrayAttrib_(c->vao, in.loc);
            GLuint binding = in.buffer % (u32)g_max_bindings;
            if (in.shader_int && !in.fmt.normalized && in.fmt.type != 0x1406 && in.fmt.type != 0x140B)
                glVertexArrayAttribIFormat_(c->vao, in.loc, in.fmt.size, in.fmt.type, in.offset);
            else
                glVertexArrayAttribFormat_(c->vao, in.loc, in.fmt.size, in.fmt.type, in.fmt.normalized, in.offset);
            glVertexArrayAttribBinding_(c->vao, in.loc, binding);
        }
        for (u32 b : p->vbuffers) {
            const emu_mtl_vlayout& l = p->d.layout[b];
            GLuint binding = b % (u32)g_max_bindings;
            glVertexArrayBindingDivisor_(c->vao, binding, l.step_fn == 2 ? std::max(1u, l.step_rate) : 0);
        }
        c->vao_pipe = p;
        e.dirty_vertex = true;
    }
}

void apply_ds(Encoder& e) {
    DepthStencil* ds = e.ds;
    emu_mtl_ds_desc d{};
    if (ds) d = ds->d;
    else d.depth_func = 7;
    bool has_depth = e.pass.depth.tex != 0;
    if (has_depth && (d.depth_func != 7 || d.depth_write)) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(compare(d.depth_func));
        glDepthMask(d.depth_write ? GL_TRUE : GL_FALSE);
    } else {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
    }
    bool has_stencil = e.pass.stencil.tex != 0 || (e.pass.depth.tex && ((Texture*)e.pass.depth.tex)->pf.stencil);
    if (has_stencil && (d.has_front || d.has_back)) {
        glEnable(GL_STENCIL_TEST);
        const emu_mtl_stencil& f = d.front;
        const emu_mtl_stencil& b = d.has_back ? d.back : d.front;
        glStencilFuncSeparate_(GL_FRONT, compare(f.func), (GLint)e.stencil_front, f.read_mask);
        glStencilFuncSeparate_(GL_BACK, compare(b.func), (GLint)e.stencil_back, b.read_mask);
        glStencilOpSeparate_(GL_FRONT, stencil_op(f.fail), stencil_op(f.depth_fail), stencil_op(f.pass));
        glStencilOpSeparate_(GL_BACK, stencil_op(b.fail), stencil_op(b.depth_fail), stencil_op(b.pass));
        glStencilMaskSeparate_(GL_FRONT, f.write_mask);
        glStencilMaskSeparate_(GL_BACK, b.write_mask);
    } else {
        glDisable(GL_STENCIL_TEST);
    }
}

void bind_stage(Ctx* c, const msl::Function& f, StageState& st, u32 bbase, u32 tbase) {
    for (auto& b : f.buffers) {
        Binding& bd = st.buffers[b.index < 31 ? b.index : 0];
        GLuint point = bbase + b.index;
        if (!bd.buf) {
            glBindBufferRange_(0x90D2, point, 0, 0, 0);
            continue;
        }
        u64 size = bd.buf->len > bd.off ? bd.buf->len - bd.off : 0;
        if (size == 0) continue;
        if (bd.off % (u64)g_ssbo_align) {
            // GL needs aligned offsets: a copy in the scratch buffer
            u64 n = std::min<u64>(size, b.size ? std::max<u64>(b.size, 16) : size);
            u64 so = scratch_put(c, bd.buf->ptr + bd.off, n);
            glBindBufferRange_(0x90D2, point, c->scratch, (GLintptr_)so, (GLsizeiptr_)n);
        } else {
            glBindBufferRange_(0x90D2, point, bd.buf->id, (GLintptr_)bd.off, (GLsizeiptr_)size);
        }
    }
    for (auto& t : f.textures) {
        Texture* tx = st.tex[t.index < 31 ? t.index : 0];
        if (t.image) {
            if (tx) glBindImageTexture_(t.image_unit, tx->id, 0, GL_TRUE, 0, 0x88BA, tx->pf.internal);  // GL_READ_WRITE
            continue;
        }
        GLuint unit = tbase + t.index;
        glBindTextureUnit_(unit, tx ? tx->id : 0);
        int si = t.sampler >= 0 && t.sampler < 31 ? t.sampler : (int)t.index;
        Sampler* s = st.smp[si];
        glBindSampler_(unit, s ? s->id : 0);
    }
}

bool prepare_draw(Ctx* c, Encoder& e) {
    if (!e.pipe || !e.pipe->prog) return false;
    Pipeline* p = e.pipe;
    apply_pipeline_state(c, e);
    // vertex buffers
    for (u32 b : p->vbuffers) {
        Binding& bd = e.st[0].buffers[b];
        GLuint binding = b % (u32)g_max_bindings;
        const emu_mtl_vlayout& l = p->d.layout[b];
        GLsizei stride = l.step_fn == 0 ? 0 : (GLsizei)l.stride;
        glVertexArrayVertexBuffer_(c->vao, binding, bd.buf ? bd.buf->id : 0, (GLintptr_)bd.off, stride);
    }
    bind_stage(c, *p->prog->vs, e.st[0], 0, 32);
    if (p->prog->fs) bind_stage(c, *p->prog->fs, e.st[1], 31, 0);
    apply_ds(e);
    return true;
}

}  // namespace

// ==================================================================== device
HLE_FUNC(emu_mtl_available, int, ()) {
    if (getenv("EMU_NO_METAL")) return 0;
    if (!window::exists()) return 0;
    ctx();
    return g_gl_ok ? 1 : 0;
}

// ==================================================================== buffers
HLE_FUNC(emu_mtl_buffer_new, u64, (u64 len, const void* bytes, u32 options, void** contents)) {
    (void)options;
    Ctx* c = ctx();
    (void)c;
    auto* b = new Buffer;
    b->len = len;
    u64 alloc = std::max<u64>(len, 16);
    glCreateBuffers_(1, &b->id);
    glNamedBufferStorage_(b->id, (GLsizeiptr_)alloc, nullptr, 0x1 | 0x2 | 0x40 | 0x80 | 0x100);
    b->ptr = (u8*)glMapNamedBufferRange_(b->id, 0, (GLsizeiptr_)alloc, 0x1 | 0x2 | 0x40 | 0x80);
    if (!b->ptr) LOGE("Metal: could not map a buffer of %llu bytes", (unsigned long long)len);
    else if (bytes) memcpy(b->ptr, bytes, len);
    if (contents) *contents = b->ptr;
    return (u64)b;
}
HLE_FUNC(emu_mtl_buffer_release, void, (u64 h)) {
    auto* b = (Buffer*)h;
    if (!b) return;
    ctx();
    glDeleteBuffers_(1, &b->id);  // (a mapped buffer is unmapped by its deletion)
    delete b;
}
HLE_FUNC(emu_mtl_buffer_modified, void, (u64, u64, u64)) {}  // coherent mapping

// ==================================================================== textures
static Texture* make_texture(const emu_mtl_tex_desc& d) {
    auto* t = new Texture;
    t->d = d;
    if (!t->d.width) t->d.width = 1;
    if (!t->d.height) t->d.height = 1;
    if (!t->d.depth) t->d.depth = 1;
    if (!t->d.mips) t->d.mips = 1;
    if (!t->d.array_length) t->d.array_length = 1;
    if (!t->d.samples) t->d.samples = 1;
    t->pf = pixel_format(d.format);
    t->target = target_of(d.type);
    t->serial = g_serial++;
    glCreateTextures_(t->target, 1, &t->id);
    const emu_mtl_tex_desc& x = t->d;
    GLenum fmt = t->pf.internal;
    switch (x.type) {
    case 0: glTextureStorage1D_(t->id, x.mips, fmt, x.width); break;
    case 1: glTextureStorage2D_(t->id, x.mips, fmt, x.width, x.array_length); break;
    case 3: glTextureStorage3D_(t->id, x.mips, fmt, x.width, x.height, x.array_length); break;
    case 4: glTextureStorage2DMultisample_(t->id, x.samples, fmt, x.width, x.height, GL_TRUE); break;
    case 6: glTextureStorage3D_(t->id, x.mips, fmt, x.width, x.height, 6 * x.array_length); break;
    case 7: glTextureStorage3D_(t->id, x.mips, fmt, x.width, x.height, x.depth); break;
    default: glTextureStorage2D_(t->id, x.mips, fmt, x.width, x.height); break;  // 2D, cube
    }
    if (x.type != 4) {
        glTextureParameteri_(t->id, 0x813D, (GLint)x.mips - 1);  // GL_TEXTURE_MAX_LEVEL
        if (d.format == 1) {  // A8: the value is alpha
            glTextureParameteri_(t->id, 0x8E42, GL_ZERO);
            glTextureParameteri_(t->id, 0x8E43, GL_ZERO);
            glTextureParameteri_(t->id, 0x8E44, GL_ZERO);
            glTextureParameteri_(t->id, 0x8E45, GL_RED);
        }
    }
    return t;
}
HLE_FUNC(emu_mtl_texture_new, u64, (const emu_mtl_tex_desc* d)) {
    ctx();
    return (u64)make_texture(*d);
}
HLE_FUNC(emu_mtl_texture_view, u64, (u64 h, u32 format, u32 type, u32 level0, u32 nlevels, u32 slice0, u32 nslices)) {
    auto* src = (Texture*)h;
    if (!src) return 0;
    ctx();
    auto* t = new Texture;
    t->d = src->d;
    t->d.format = format;
    t->d.type = type;
    t->d.mips = nlevels;
    t->d.array_length = nslices;
    t->d.width = std::max(1u, src->d.width >> level0);
    t->d.height = std::max(1u, src->d.height >> level0);
    t->pf = pixel_format(format);
    t->target = target_of(type);
    t->serial = g_serial++;
    t->view = true;
    glGenTextures(1, &t->id);
    glTextureView_(t->id, t->target, src->id, t->pf.internal, level0, nlevels, slice0, type == 5 ? 6 : type == 6 ? nslices * 6 : nslices);
    return (u64)t;
}
HLE_FUNC(emu_mtl_texture_release, void, (u64 h)) {
    auto* t = (Texture*)h;
    if (!t) return;
    ctx();
    glDeleteTextures(1, &t->id);
    delete t;
}
HLE_FUNC(emu_mtl_texture_info, void, (u64 h, emu_mtl_tex_desc* out)) {
    auto* t = (Texture*)h;
    if (t && out) *out = t->d;
}

static void upload(Texture* t, u32 level, u32 x, u32 y, u32 z, u32 w, u32 h, u32 d, const void* bytes, u64 bpr, u64 bpi) {
    if (!bytes || !w || !h) return;
    const PixelFormat& pf = t->pf;
    if (pf.bbytes) {  // compressed
        u32 bx = (w + pf.bw - 1) / pf.bw, by = (h + pf.bh - 1) / pf.bh;
        GLsizei size = (GLsizei)(bpr ? bpr * by : (u64)bx * by * pf.bbytes);
        if (t->target == 0x0DE1) glCompressedTextureSubImage2D_(t->id, level, x, y, w, h, pf.internal, size, bytes);
        else glCompressedTextureSubImage3D_(t->id, level, x, y, z, w, h, d, pf.internal, size * d, bytes);
        return;
    }
    if (pf.bpp && bpr) glPixelStorei(0x0CF2, (GLint)(bpr / pf.bpp));  // GL_UNPACK_ROW_LENGTH
    if (pf.bpp && bpr && bpi) glPixelStorei(0x806E, (GLint)(bpi / bpr));  // GL_UNPACK_IMAGE_HEIGHT
    switch (t->d.type) {
    case 0: glTextureSubImage1D_(t->id, level, x, w, pf.fmt, pf.type, bytes); break;
    case 2: case 4: glTextureSubImage2D_(t->id, level, x, y, w, h, pf.fmt, pf.type, bytes); break;
    default: glTextureSubImage3D_(t->id, level, x, y, z, w, h, d, pf.fmt, pf.type, bytes); break;
    }
    glPixelStorei(0x0CF2, 0);
    glPixelStorei(0x806E, 0);
}
HLE_FUNC(emu_mtl_texture_replace, void, (u64 h, const u64* r, u32 level, u32 slice, const void* bytes, u64 bpr, u64 bpi)) {
    auto* t = (Texture*)h;
    if (!t || !r) return;
    ctx();
    u32 z = (u32)r[2], d = (u32)r[5];
    if (t->d.type == 1 || t->d.type == 3 || t->d.type == 5 || t->d.type == 6) { z = slice; d = 1; }
    if (t->d.type == 1) { upload(t, level, (u32)r[0], slice, 0, (u32)r[3], 1, 1, bytes, bpr, bpi); return; }
    upload(t, level, (u32)r[0], (u32)r[1], z, (u32)r[3], (u32)r[4], d ? d : 1, bytes, bpr, bpi);
    glFlush();
}
HLE_FUNC(emu_mtl_texture_get, void, (u64 h, void* out, u64 bpr, u64 bpi, const u64* r, u32 level, u32 slice)) {
    auto* t = (Texture*)h;
    if (!t || !r || !out) return;
    ctx();
    glFinish();
    const PixelFormat& pf = t->pf;
    u32 z = (u32)r[2], d = (u32)r[5];
    if (t->d.type == 3 || t->d.type == 5 || t->d.type == 6) { z = slice; d = 1; }
    if (pf.bpp && bpr) glPixelStorei(0x0D02, (GLint)(bpr / pf.bpp));  // GL_PACK_ROW_LENGTH
    u64 size = bpi ? bpi * (d ? d : 1) : bpr * r[4];
    glGetTextureSubImage_(t->id, level, (GLint)r[0], (GLint)r[1], (GLint)z, (GLsizei)r[3], (GLsizei)r[4], (GLsizei)(d ? d : 1), pf.fmt,
                          pf.type, (GLsizei)size, out);
    glPixelStorei(0x0D02, 0);
}

// ==================================================================== samplers / depth-stencil
HLE_FUNC(emu_mtl_sampler_new, u64, (const emu_mtl_sampler_desc* d)) {
    ctx();
    auto* s = new Sampler;
    glCreateSamplers_(1, &s->id);
    GLenum mag = d->mag_filter ? GL_LINEAR : GL_NEAREST;
    GLenum min = d->min_filter ? GL_LINEAR : GL_NEAREST;
    if (d->mip_filter == 1) min = d->min_filter ? 0x2701 : 0x2700;       // *_MIPMAP_NEAREST
    else if (d->mip_filter == 2) min = d->min_filter ? 0x2703 : 0x2702;  // *_MIPMAP_LINEAR
    glSamplerParameteri_(s->id, GL_TEXTURE_MAG_FILTER, (GLint)mag);
    glSamplerParameteri_(s->id, GL_TEXTURE_MIN_FILTER, (GLint)min);
    auto wrap = [](u32 m) -> GLint {
        switch (m) {
        case 1: return 0x8743;  // GL_MIRROR_CLAMP_TO_EDGE
        case 2: return GL_REPEAT;
        case 3: return 0x8370;  // GL_MIRRORED_REPEAT
        case 4: case 5: return 0x812D;  // GL_CLAMP_TO_BORDER
        default: return 0x812F;  // GL_CLAMP_TO_EDGE
        }
    };
    glSamplerParameteri_(s->id, GL_TEXTURE_WRAP_S, wrap(d->s));
    glSamplerParameteri_(s->id, GL_TEXTURE_WRAP_T, wrap(d->t));
    glSamplerParameteri_(s->id, 0x8072, wrap(d->r));  // GL_TEXTURE_WRAP_R
    float border[4] = {0, 0, 0, 0};
    if (d->border == 1) border[3] = 1;
    if (d->border == 2) border[0] = border[1] = border[2] = border[3] = 1;
    glSamplerParameterfv_(s->id, GL_TEXTURE_BORDER_COLOR, border);
    glSamplerParameterf_(s->id, 0x813A, d->lod_min);  // GL_TEXTURE_MIN_LOD
    glSamplerParameterf_(s->id, 0x813B, d->lod_max);  // GL_TEXTURE_MAX_LOD
    if (d->max_aniso > 1) glSamplerParameterf_(s->id, 0x84FE, (float)d->max_aniso);  // GL_TEXTURE_MAX_ANISOTROPY
    if (d->compare) {
        glSamplerParameteri_(s->id, 0x884C, 0x884E);  // GL_TEXTURE_COMPARE_MODE = COMPARE_REF_TO_TEXTURE
        glSamplerParameteri_(s->id, 0x884D, (GLint)compare(d->compare));
    }
    return (u64)s;
}
HLE_FUNC(emu_mtl_sampler_release, void, (u64 h)) {
    auto* s = (Sampler*)h;
    if (!s) return;
    ctx();
    glDeleteSamplers_(1, &s->id);
    delete s;
}
HLE_FUNC(emu_mtl_ds_new, u64, (const emu_mtl_ds_desc* d)) {
    auto* s = new DepthStencil;
    s->d = *d;
    return (u64)s;
}
HLE_FUNC(emu_mtl_ds_release, void, (u64 h)) { delete (DepthStencil*)h; }

static void log_source(const std::string& src);
// ==================================================================== shaders
HLE_FUNC(emu_mtl_library_new, u64, (const char* src, char* err, u32 errlen)) {
    if (!src) return 0;
    auto lib = msl::translate(src);
    if (const char* dump = getenv("EMU_MTL_DUMP")) {
        static std::atomic<int> n{0};
        char path[600];
        snprintf(path, sizeof path, "%s/lib%04d.metal", dump, n++);
        if (FILE* f = fopen(path, "wb")) { fputs(src, f); fclose(f); }
    }
    if (!lib->error.empty()) {
        LOGW("Metal: library: %s", lib->error.c_str());
        put_err(err, errlen, lib->error);
        return 0;
    }
    for (auto& f : lib->functions)
        if (!f->error.empty()) {
            static std::atomic<int> n{0};
            if (n++ < 4) {
                LOGW("Metal: could not translate (%s):", f->error.c_str());
                log_source(src);
            }
        }
    auto* l = new Library;
    l->lib = lib;
    return (u64)l;
}
HLE_FUNC(emu_mtl_library_release, void, (u64 h)) { delete (Library*)h; }
HLE_FUNC(emu_mtl_function_new, u64, (u64 lh, const char* name)) {
    auto* l = (Library*)lh;
    if (!l || !name) return 0;
    for (auto& f : l->lib->functions) {
        if (f->name != name) continue;
        if (!f->error.empty()) {
            static std::atomic<int> n{0};
            if (n++ < 20) LOGW("Metal: function %s could not be translated: %s", name, f->error.c_str());
        }
        auto* r = new Function;
        r->lib = l->lib;
        r->fn = f;
        return (u64)r;
    }
    return 0;
}
HLE_FUNC(emu_mtl_function_type, u32, (u64 h)) {
    auto* f = (Function*)h;
    if (!f) return 0;
    return f->fn->stage == msl::Stage::Vertex ? 1 : f->fn->stage == msl::Stage::Fragment ? 2 : 3;
}
HLE_FUNC(emu_mtl_function_release, void, (u64 h)) { delete (Function*)h; }
HLE_FUNC(emu_mtl_function_attribute, int, (u64 h, u32 i, char* name, u32 namelen, u32* index, u32* data_type)) {
    auto* f = (Function*)h;
    if (!f || f->fn->stage != msl::Stage::Vertex || i >= f->fn->inputs.size()) return 0;
    const msl::Varying& v = f->fn->inputs[i];
    put_err(name, namelen, v.name);
    if (index) *index = v.location;
    if (data_type) {
        // GLSL type -> MTLDataType
        static const std::pair<const char*, u32> t[] = {{"float", 3}, {"vec2", 4}, {"vec3", 5}, {"vec4", 6}, {"int", 29}, {"ivec2", 30},
                                                        {"ivec3", 31}, {"ivec4", 32}, {"uint", 33}, {"uvec2", 34}, {"uvec3", 35}, {"uvec4", 36}};
        *data_type = 6;
        for (auto& [n, d] : t) if (v.type == n) *data_type = d;
    }
    return 1;
}

static void log_source(const std::string& src) {
    size_t line = 1, pos = 0;
    std::string out;
    while (pos < src.size()) {
        size_t e = src.find('\n', pos);
        if (e == std::string::npos) e = src.size();
        char b[16];
        snprintf(b, sizeof b, "%4zu ", line++);
        out += b + src.substr(pos, e - pos) + "\n";
        pos = e + 1;
    }
    LOGW("%s", out.c_str());
}

HLE_FUNC(emu_mtl_render_pipeline_new, u64, (const emu_mtl_rp_desc* d, emu_mtl_refl* refl, char* err, u32 errlen)) {
    auto* vf = (Function*)d->vfn;
    auto* ff = (Function*)d->ffn;
    if (!vf) { put_err(err, errlen, "no vertex function"); return 0; }
    if (!vf->fn->error.empty() || (ff && !ff->fn->error.empty())) {
        put_err(err, errlen, "shader translation failed: " + (vf->fn->error.empty() ? ff->fn->error : vf->fn->error));
        return 0;
    }
    ctx();
    std::shared_ptr<Program> prog;
    {
        std::lock_guard lk(g_prog_lock);
        auto key = std::make_pair(vf->fn.get(), ff ? ff->fn.get() : nullptr);
        prog = g_programs[key].lock();
        if (!prog) {
            std::string e1, e2, e3;
            std::string vsrc = msl::emit_stage(*vf->fn, 0, 32, ff ? ff->fn.get() : nullptr);
            std::string fsrc = ff ? msl::emit_stage(*ff->fn, 31, 0, vf->fn.get()) : "";
            GLuint vs = compile(0x8B31, vsrc, e1);
            GLuint fs = ff ? compile(0x8B30, fsrc, e2) : 0;
            GLuint p = 0;
            if (vs && (fs || !ff)) p = ff ? link({vs, fs}, e3) : link({vs}, e3);
            else {
                if (vs) glDeleteShader_(vs);
                if (fs) glDeleteShader_(fs);
            }
            if (!p) {
                static std::atomic<int> nlog{0};
                if (nlog++ < 5) {
                    LOGW("Metal: pipeline failed: %s%s%s", e1.c_str(), e2.c_str(), e3.c_str());
                    if (!e1.empty()) log_source(vsrc);
                    if (!e2.empty() || !e3.empty()) log_source(fsrc);
                }
                put_err(err, errlen, e1 + e2 + e3);
                return 0;
            }
            // EMU_MTL_DUMP=<dir>: the GLSL of every program
            if (const char* dump = getenv("EMU_MTL_DUMP")) {
                char path[600];
                snprintf(path, sizeof path, "%s/prog%u.vert", dump, p);
                if (FILE* f = fopen(path, "wb")) { fwrite(vsrc.data(), 1, vsrc.size(), f); fclose(f); }
                snprintf(path, sizeof path, "%s/prog%u.frag", dump, p);
                if (FILE* f = fopen(path, "wb")) { fwrite(fsrc.data(), 1, fsrc.size(), f); fclose(f); }
            }
            prog = std::make_shared<Program>();
            prog->prog = p;
            prog->vs = vf->fn;
            prog->fs = ff ? ff->fn : nullptr;
            g_programs[key] = prog;
        }
    }
    auto* pl = new Pipeline;
    pl->prog = prog;
    pl->d = *d;
    // vertex inputs: the shader's attributes with the descriptor's formats
    std::vector<bool> used(31, false);
    for (auto& in : vf->fn->inputs) {
        if (in.location >= 31) continue;
        const emu_mtl_vattr& a = d->attr[in.location];
        if (!d->has_vdesc || a.format == 0) {
            // no data: GL uses the current attribute value (0,0,0,1)
            continue;
        }
        Pipeline::In x;
        x.loc = in.location;
        x.fmt = vertex_format(a.format);
        x.offset = a.offset;
        x.buffer = a.buffer;
        x.shader_int = in.type.find("ivec") == 0 || in.type.find("uvec") == 0 || in.type == "int" || in.type == "uint";
        if (x.fmt.size == 0x80E1) x.shader_int = false;
        pl->inputs.push_back(x);
        if (a.buffer < 31 && !used[a.buffer]) {
            used[a.buffer] = true;
            pl->vbuffers.push_back(a.buffer);
        }
    }
    if (tracing() || getenv("EMU_MTL_DUMP")) {
        std::string a;
        char b[128];
        for (auto& in : pl->inputs) {
            snprintf(b, sizeof b, " loc%u fmt(size %d type %x norm %d int %d) off %u buf %u stride %u step %u;", in.loc, in.fmt.size, in.fmt.type,
                     in.fmt.normalized, in.shader_int, in.offset, in.buffer, d->layout[in.buffer].stride, d->layout[in.buffer].step_fn);
            a += b;
        }
        std::string raw;
        for (int i = 0; i < 8; i++) {
            char b2[64];
            snprintf(b2, sizeof b2, " a%d(f%u o%u b%u)", i, d->attr[i].format, d->attr[i].offset, d->attr[i].buffer);
            raw += b2;
        }
        std::string vin;
        for (auto& in : vf->fn->inputs) vin += " " + std::to_string(in.location) + ":" + in.type;
        LOGI("[mtl] vdesc %u%s; shader inputs%s", d->has_vdesc, raw.c_str(), vin.c_str());
        LOGI("[mtl] pipeline prog %u: color0 fmt %u blend %u (%u %u %u %u) mask %x depth fmt %u; inputs:%s", prog->prog, d->color[0].format,
             d->color[0].blend, d->color[0].src_rgb, d->color[0].dst_rgb, d->color[0].src_a, d->color[0].dst_a, d->color[0].write_mask, d->depth_format, a.c_str());
    }
    if (refl) {
        auto v = reflect(*vf->fn);
        refl->nvertex = v.first;
        refl->vertex = v.second;
        if (ff) {
            auto f = reflect(*ff->fn);
            refl->nfragment = f.first;
            refl->fragment = f.second;
        }
    }
    return (u64)pl;
}

HLE_FUNC(emu_mtl_compute_pipeline_new, u64, (u64 fh, emu_mtl_refl* refl, char* err, u32 errlen)) {
    auto* f = (Function*)fh;
    if (!f) { put_err(err, errlen, "no function"); return 0; }
    if (!f->fn->error.empty()) { put_err(err, errlen, f->fn->error); return 0; }
    auto* pl = new Pipeline;
    pl->compute = true;
    pl->kernel = f->fn;
    if (refl) {
        auto v = reflect(*f->fn);
        refl->nvertex = v.first;
        refl->vertex = v.second;
        refl->threads_per_group_max = 1024;
        refl->thread_width = 32;
    }
    return (u64)pl;
}
HLE_FUNC(emu_mtl_pipeline_release, void, (u64 h)) {
    auto* p = (Pipeline*)h;
    if (!p) return;
    if (t_ctx && t_ctx->vao_pipe == p) t_ctx->vao_pipe = nullptr;
    if (t_ctx && t_ctx->enc.pipe == p) t_ctx->enc.pipe = nullptr;
    if (t_ctx && t_ctx->cpipe == p) t_ctx->cpipe = nullptr;
    if (!p->kernel_progs.empty()) {
        ctx();
        for (auto& [k, g] : p->kernel_progs) glDeleteProgram_(g);
    }
    delete p;
}

// ==================================================================== drawables
namespace {
std::mutex g_drawable_lock;
struct DrawableTex { Texture* t = nullptr; };
DrawableTex g_drawables[3];
int g_drawable_next = 0;
}  // namespace

HLE_FUNC(emu_mtl_next_drawable, u64, (u32 w, u32 h, u32 format)) {
    ctx();
    std::lock_guard lk(g_drawable_lock);
    DrawableTex& d = g_drawables[g_drawable_next];
    g_drawable_next = (g_drawable_next + 1) % 3;
    if (!d.t || d.t->d.width != w || d.t->d.height != h || d.t->d.format != format) {
        if (d.t) {
            glDeleteTextures(1, &d.t->id);
            delete d.t;
        }
        emu_mtl_tex_desc td{};
        td.type = 2;
        td.format = format;
        td.width = w ? w : 1;
        td.height = h ? h : 1;
        td.mips = 1;
        td.usage = 4;  // render target
        d.t = make_texture(td);
    }
    return (u64)d.t;
}

HLE_FUNC(emu_mtl_present, void, (u64 h)) {
    auto* t = (Texture*)h;
    if (!t) return;
    Ctx* c = ctx();
    int w = (int)t->d.width, hh = (int)t->d.height;
    if (!c->present_rb || c->present_w != w || c->present_h != hh) {
        if (!c->present_rb) {
            glCreateRenderbuffers_(1, &c->present_rb);
            glCreateFramebuffers_(1, &c->present_fbo);
            glCreateFramebuffers_(1, &c->present_src_fbo);
        }
        glNamedRenderbufferStorage_(c->present_rb, 0x8058, w, hh);
        glNamedFramebufferRenderbuffer_(c->present_fbo, 0x8CE0, 0x8D41, c->present_rb);
        c->present_w = w;
        c->present_h = hh;
    }
    glNamedFramebufferTexture_(c->present_src_fbo, 0x8CE0, t->id, 0);
    MTRACE("present tex%u %dx%d (frame %d)", t->id, w, hh, g_frame_no.load());
    g_frame_no++;
    glDisable(GL_SCISSOR_TEST);
    glDisable(0x8DB9);  // the drawable's bytes as they are
    // Metal's row 0 is the top; the window's (GL) is the bottom: flip
    glBlitNamedFramebuffer_(c->present_src_fbo, c->present_fbo, 0, 0, w, hh, 0, hh, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    window::present(c->present_rb, w, hh);
    glEnable(0x8DB9);
    glBindFramebuffer_(0x8D40, 0);
    if (c->enc.active) glBindFramebuffer_(0x8D40, c->enc.fbo);
}

// ==================================================================== render pass
HLE_FUNC(emu_mtl_render_begin, u64, (const emu_mtl_pass_desc* d)) {
    Ctx* c = ctx();
    Encoder& e = c->enc;
    e = Encoder{};
    e.pass = *d;
    e.active = true;
    e.fbo = pass_fbo(c, *d, e.w, e.h);
    if (tracing()) {
        std::string a;
        char b[160];
        for (int i = 0; i < 8; i++)
            if (auto* t = (Texture*)d->color[i].tex) {
                snprintf(b, sizeof b, " c%d=tex%u(%ux%u fmt%u) load%u [%.2f %.2f %.2f %.2f]", i, t->id, t->d.width, t->d.height, t->d.format,
                         d->color[i].load, d->color[i].clear[0], d->color[i].clear[1], d->color[i].clear[2], d->color[i].clear[3]);
                a += b;
            }
        if (auto* t = (Texture*)d->depth.tex) { snprintf(b, sizeof b, " depth=tex%u fmt%u load%u", t->id, t->d.format, d->depth.load); a += b; }
        MTRACE("pass fbo %u %ux%u%s", e.fbo, e.w, e.h, a.c_str());
    }
    glBindFramebuffer_(0x8D40, e.fbo);
    // load actions
    glDisable(GL_SCISSOR_TEST);
    for (int i = 0; i < 8; i++) {
        const emu_mtl_pass_att& a = d->color[i];
        auto* t = (Texture*)a.tex;
        if (!t || a.load != 2) continue;
        glColorMaski_(i, 1, 1, 1, 1);
        if (t->pf.kind == 'u') {
            GLuint v[4] = {(GLuint)a.clear[0], (GLuint)a.clear[1], (GLuint)a.clear[2], (GLuint)a.clear[3]};
            glClearNamedFramebufferuiv_(e.fbo, GL_COLOR, i, v);
        } else if (t->pf.kind == 'i') {
            GLint v[4] = {(GLint)a.clear[0], (GLint)a.clear[1], (GLint)a.clear[2], (GLint)a.clear[3]};
            glClearNamedFramebufferiv_(e.fbo, GL_COLOR, i, v);
        } else {
            GLfloat v[4] = {(float)a.clear[0], (float)a.clear[1], (float)a.clear[2], (float)a.clear[3]};
            glClearNamedFramebufferfv_(e.fbo, GL_COLOR, i, v);
        }
    }
    auto* dt = (Texture*)d->depth.tex;
    auto* stt = (Texture*)d->stencil.tex;
    bool cd = dt && d->depth.load == 2, cs = (stt || (dt && dt->pf.stencil)) && d->stencil.load == 2;
    if (cd || cs) {
        glDepthMask(GL_TRUE);
        glStencilMaskSeparate_(GL_FRONT_AND_BACK, 0xff);
        if (cd && cs && dt && dt->pf.stencil && (!stt || stt == dt))
            glClearNamedFramebufferfi_(e.fbo, 0x84F9, 0, (float)d->depth.clear[0], (GLint)d->stencil.clear[1]);
        else {
            if (cd) {
                GLfloat v = (float)d->depth.clear[0];
                glClearNamedFramebufferfv_(e.fbo, GL_DEPTH, 0, &v);
            }
            if (cs) {
                GLint v = (GLint)d->stencil.clear[1];
                glClearNamedFramebufferiv_(e.fbo, GL_STENCIL, 0, &v);
            }
        }
    }
    // Metal's defaults: the whole target, no culling, clockwise front faces
    glViewport(0, 0, (GLsizei)e.w, (GLsizei)e.h);
    glDepthRangef_(0, 1);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, (GLsizei)e.w, (GLsizei)e.h);
    glDisable(GL_CULL_FACE);
    glFrontFace(GL_CW);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glBlendColor_(0, 0, 0, 0);
    c->vao_pipe = nullptr;
    return (u64)&e;
}

#define ENC(h) Encoder& e = *(Encoder*)(h); (void)e; Ctx* c = ctx(); (void)c
HLE_FUNC(emu_mtl_r_pipeline, void, (u64 h, u64 p)) {
    ENC(h);
    e.pipe = (Pipeline*)p;
}
HLE_FUNC(emu_mtl_r_buffer, void, (u64 h, u32 stage, u32 index, u64 buf, u64 off)) {
    ENC(h);
    if (stage & 0x80000000u) {  // set*BufferOffset: the bound buffer at a new offset
        stage &= 1;
        if (index < 31) e.st[stage].buffers[index].off = off;
        return;
    }
    if (stage > 1 || index >= 31) return;
    e.st[stage].buffers[index] = {(Buffer*)buf, off};
}
HLE_FUNC(emu_mtl_r_bytes, void, (u64 h, u32 stage, u32 index, const void* bytes, u64 len)) {
    ENC(h);
    if (stage > 1 || index >= 31 || !bytes) return;
    // a small buffer per call (rare in bgfx): a fresh 256-byte aligned buffer from a pool
    static thread_local std::vector<Buffer*> pool;
    static thread_local size_t next = 0;
    if (pool.size() < 256) {
        auto* b = new Buffer;
        b->len = 64 << 10;
        glCreateBuffers_(1, &b->id);
        glNamedBufferStorage_(b->id, (GLsizeiptr_)b->len, nullptr, 0x2 | 0x40 | 0x80 | 0x100);
        b->ptr = (u8*)glMapNamedBufferRange_(b->id, 0, (GLsizeiptr_)b->len, 0x2 | 0x40 | 0x80);
        pool.push_back(b);
    }
    Buffer* b = pool[next++ % pool.size()];
    memcpy(b->ptr, bytes, std::min<u64>(len, b->len));
    e.st[stage].buffers[index] = {b, 0};
}
HLE_FUNC(emu_mtl_r_texture, void, (u64 h, u32 stage, u32 index, u64 tex)) {
    ENC(h);
    if (stage > 1 || index >= 31) return;
    e.st[stage].tex[index] = (Texture*)tex;
}
HLE_FUNC(emu_mtl_r_sampler, void, (u64 h, u32 stage, u32 index, u64 s)) {
    ENC(h);
    if (stage > 1 || index >= 31) return;
    e.st[stage].smp[index] = (Sampler*)s;
}
HLE_FUNC(emu_mtl_r_viewport, void, (u64 h, const double* v)) {
    ENC(h);
    glViewport((GLint)v[0], (GLint)v[1], (GLsizei)v[2], (GLsizei)v[3]);
    glDepthRangef_((float)v[4], (float)v[5]);
}
HLE_FUNC(emu_mtl_r_scissor, void, (u64 h, u64 x, u64 y, u64 w, u64 hh)) {
    ENC(h);
    glScissor((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)hh);
}
HLE_FUNC(emu_mtl_r_state, void, (u64 h, u32 what, u32 v)) {
    ENC(h);
    static const bool flip = getenv("EMU_MTL_FLIP_WINDING") != nullptr;
    switch (what) {
    case 0:  // cull mode
        if (v == 0) glDisable(GL_CULL_FACE);
        else {
            glEnable(GL_CULL_FACE);
            glCullFace(v == 1 ? GL_FRONT : GL_BACK);
        }
        break;
    case 1: glFrontFace((v == 0) != flip ? GL_CW : GL_CCW); break;
    case 2: glPolygonMode(GL_FRONT_AND_BACK, v ? GL_LINE : GL_FILL); break;
    case 4: if (v) glEnable(0x864F); else glDisable(0x864F); break;  // GL_DEPTH_CLAMP (MTLDepthClipModeClamp)
    default: break;
    }
}
HLE_FUNC(emu_mtl_r_stencil_ref, void, (u64 h, u32 f, u32 b)) {
    ENC(h);
    e.stencil_front = f;
    e.stencil_back = b;
}
HLE_FUNC(emu_mtl_r_depth_bias, void, (u64 h, float bias, float slope, float clamp)) {
    ENC(h);
    if (bias == 0 && slope == 0) glDisable(GL_POLYGON_OFFSET_FILL);
    else {
        glEnable(GL_POLYGON_OFFSET_FILL);
        if (glPolygonOffsetClamp_) glPolygonOffsetClamp_(slope, bias, clamp);
        else glPolygonOffset(slope, bias);
    }
}
HLE_FUNC(emu_mtl_r_ds, void, (u64 h, u64 ds)) {
    ENC(h);
    e.ds = (DepthStencil*)ds;
}
HLE_FUNC(emu_mtl_r_blend_color, void, (u64 h, float r, float g, float b, float a)) {
    ENC(h);
    glBlendColor_(r, g, b, a);
}
HLE_FUNC(emu_mtl_r_draw, void, (u64 h, u32 p, u64 start, u64 count, u64 instances, u64 base_instance)) {
    ENC(h);
    if (!count || !instances || !prepare_draw(c, e)) return;
    MTRACE("draw prim %u start %llu count %llu inst %llu prog %u", p, (unsigned long long)start, (unsigned long long)count, (unsigned long long)instances, e.pipe->prog->prog);
    glVertexArrayElementBuffer_(c->vao, 0);
    glDrawArraysInstancedBaseInstance_(prim(p), (GLint)start, (GLsizei)count, (GLsizei)instances, (GLuint)base_instance);
}
HLE_FUNC(emu_mtl_r_draw_indexed, void, (u64 h, u32 p, u64 count, u32 itype, u64 ibuf, u64 ioff, u64 instances, s64 base_vertex, u64 base_instance)) {
    ENC(h);
    auto* ib = (Buffer*)ibuf;
    if (!count || !instances || !ib || !prepare_draw(c, e)) {
        MTRACE("draw indexed SKIPPED (pipe %p)", (void*)e.pipe);
        return;
    }
    MTRACE("draw indexed prim %u count %llu type %u off %llu inst %llu bv %lld prog %u", p, (unsigned long long)count, itype, (unsigned long long)ioff,
           (unsigned long long)instances, (long long)base_vertex, e.pipe->prog->prog);
    glVertexArrayElementBuffer_(c->vao, ib->id);
    glDrawElementsInstancedBaseVertexBaseInstance_(prim(p), (GLsizei)count, itype ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, (const void*)(uintptr_t)ioff,
                                                   (GLsizei)instances, (GLint)base_vertex, (GLuint)base_instance);
}
HLE_FUNC(emu_mtl_r_end, void, (u64 h)) {
    ENC(h);
    // multisample resolve
    for (int i = 0; i < 8; i++) {
        const emu_mtl_pass_att& a = e.pass.color[i];
        auto* t = (Texture*)a.tex;
        auto* r = (Texture*)a.resolve;
        if (!t || !r || (a.store != 2 && a.store != 3)) continue;
        GLuint rf = 0, src = 0;
        glCreateFramebuffers_(1, &rf);
        attach(rf, 0x8CE0, r, a.resolve_level, a.resolve_slice, 0);
        glCreateFramebuffers_(1, &src);
        attach(src, 0x8CE0, t, a.level, a.slice, 0);
        glDisable(GL_SCISSOR_TEST);
        glBlitNamedFramebuffer_(src, rf, 0, 0, e.w, e.h, 0, 0, e.w, e.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glDeleteFramebuffers_(1, &src);
        glDeleteFramebuffers_(1, &rf);
        glEnable(GL_SCISSOR_TEST);
    }
    e.active = false;
    glUseProgram_(0);
}

// ==================================================================== blit
static Buffer* B(u64 h) { return (Buffer*)h; }
static Texture* T(u64 h) { return (Texture*)h; }
HLE_FUNC(emu_mtl_b_copy_buffer, void, (u64 s, u64 so, u64 d, u64 dof, u64 n)) {
    ctx();
    if (!B(s) || !B(d) || !n) return;
    glCopyNamedBufferSubData_(B(s)->id, B(d)->id, (GLintptr_)so, (GLintptr_)dof, (GLsizeiptr_)n);
}
HLE_FUNC(emu_mtl_b_buffer_to_texture, void, (u64 s, u64 off, u64 bpr, u64 bpi, const u64* size, u64 d, u32 slice, u32 level, const u64* o)) {
    ctx();
    if (!B(s) || !T(d)) return;
    glFinish();  // the buffer's contents may come from the GPU
    Texture* t = T(d);
    u32 z = (u32)o[2], dd = (u32)size[2];
    if (t->d.type == 3 || t->d.type == 5 || t->d.type == 6 || t->d.type == 1) { z = slice; dd = 1; }
    upload(t, level, (u32)o[0], (u32)o[1], z, (u32)size[0], (u32)size[1], dd ? dd : 1, B(s)->ptr + off, bpr, bpi);
}
HLE_FUNC(emu_mtl_b_texture_to_buffer, void, (u64 s, u32 slice, u32 level, const u64* o, const u64* size, u64 d, u64 off, u64 bpr, u64 bpi)) {
    if (!T(s) || !B(d)) return;
    u64 r[6] = {o[0], o[1], o[2], size[0], size[1], size[2]};
    hle_impl_emu_mtl_texture_get(s, B(d)->ptr + off, bpr, bpi, r, level, slice);
}
HLE_FUNC(emu_mtl_b_texture_to_texture, void, (u64 s, u32 ss, u32 sl, const u64* so, const u64* size, u64 d, u32 ds, u32 dl, const u64* dor)) {
    ctx();
    if (!T(s) || !T(d)) return;
    MTRACE("blit tex%u -> tex%u %llux%llu", T(s)->id, T(d)->id, (unsigned long long)size[0], (unsigned long long)size[1]);
    Texture *a = T(s), *b = T(d);
    auto z = [](Texture* t, u32 slice, u64 oz) -> GLint { return t->d.type == 7 ? (GLint)oz : (t->d.type == 2 || t->d.type == 4) ? 0 : (GLint)slice; };
    GLsizei depth = a->d.type == 7 ? (GLsizei)size[2] : 1;
    glCopyImageSubData_(a->id, a->target, sl, (GLint)so[0], (GLint)so[1], z(a, ss, so[2]), b->id, b->target, dl, (GLint)dor[0],
                        (GLint)dor[1], z(b, ds, dor[2]), (GLsizei)size[0], (GLsizei)size[1], depth);
}
HLE_FUNC(emu_mtl_b_mipmaps, void, (u64 t)) {
    ctx();
    if (T(t)) glGenerateTextureMipmap_(T(t)->id);
}
HLE_FUNC(emu_mtl_b_fill, void, (u64 b, u64 off, u64 len, u32 v)) {
    ctx();
    if (!B(b)) return;
    glFinish();
    memset(B(b)->ptr + off, (int)v, len);
}

// ==================================================================== compute
HLE_FUNC(emu_mtl_c_pipeline, void, (u64 p)) { ctx()->cpipe = (Pipeline*)p; }
HLE_FUNC(emu_mtl_c_buffer, void, (u32 i, u64 b, u64 off)) {
    if (i & 0x80000000u) {
        i &= 0x7fffffffu;
        if (i < 31) ctx()->cst.buffers[i].off = off;
        return;
    }
    if (i < 31) ctx()->cst.buffers[i] = {(Buffer*)b, off};
}
HLE_FUNC(emu_mtl_c_bytes, void, (u32 i, const void* bytes, u64 len)) {
    Ctx* c = ctx();
    if (i >= 31 || !bytes) return;
    static thread_local Buffer* b = nullptr;
    if (!b) {
        b = new Buffer;
        b->len = 64 << 10;
        glCreateBuffers_(1, &b->id);
        glNamedBufferStorage_(b->id, (GLsizeiptr_)b->len, nullptr, 0x2 | 0x40 | 0x80 | 0x100);
        b->ptr = (u8*)glMapNamedBufferRange_(b->id, 0, (GLsizeiptr_)b->len, 0x2 | 0x40 | 0x80);
    }
    glFinish();
    memcpy(b->ptr, bytes, std::min<u64>(len, b->len));
    c->cst.buffers[i] = {b, 0};
}
HLE_FUNC(emu_mtl_c_texture, void, (u32 i, u64 t)) {
    if (i < 31) ctx()->cst.tex[i] = (Texture*)t;
}
HLE_FUNC(emu_mtl_c_sampler, void, (u32 i, u64 s)) {
    if (i < 31) ctx()->cst.smp[i] = (Sampler*)s;
}
HLE_FUNC(emu_mtl_c_dispatch, void, (u64 gx, u64 gy, u64 gz, u64 tx, u64 ty, u64 tz)) {
    Ctx* c = ctx();
    Pipeline* p = c->cpipe;
    MTRACE("dispatch %llu %llu %llu", (unsigned long long)gx, (unsigned long long)gy, (unsigned long long)gz);
    if (!p || !p->kernel || !gx || !gy || !gz) return;
    u64 key = (tx << 40) | (ty << 20) | tz;
    GLuint prog = 0;
    auto it = p->kernel_progs.find(key);
    if (it != p->kernel_progs.end()) prog = it->second;
    else {
        std::string e1, e2;
        std::string src = msl::emit_stage(*p->kernel, 0, 0, nullptr, (unsigned)tx, (unsigned)ty, (unsigned)tz);
        GLuint s = compile(0x91B9, src, e1);
        if (s) prog = link({s}, e2);
        if (!prog) {
            static std::atomic<int> n{0};
            if (n++ < 3) {
                LOGW("Metal: compute shader failed: %s%s", e1.c_str(), e2.c_str());
                log_source(src);
            }
        }
        p->kernel_progs[key] = prog;
    }
    if (!prog) return;
    glUseProgram_(prog);
    bind_stage(c, *p->kernel, c->cst, 0, 0);
    glDispatchCompute_((GLuint)gx, (GLuint)gy, (GLuint)gz);
    glMemoryBarrier_(0xFFFFFFFF);
    glUseProgram_(0);
}

// ==================================================================== command buffers
// Command buffers complete in order. Each commit gets a fence; the guest's completion thread
// (guest/Metal/Metal.m) waits for them in emu_mtl_next_completed and calls the handlers.
namespace {
std::mutex g_fence_lock;
std::condition_variable g_fence_new, g_fence_done;
std::deque<std::pair<u64, GLsync_>> g_fences;
u64 g_next_ticket = 1, g_done_ticket = 0;
}  // namespace
HLE_FUNC(emu_mtl_commit, u64, ()) {
    Ctx* c = ctx();
    GLsync_ s = glFenceSync_(0x9117, 0);  // GL_SYNC_GPU_COMMANDS_COMPLETE
    glFlush();                             // (the fence must reach the GPU for other contexts to wait on it)
    u64 t;
    {
        std::lock_guard lk(g_fence_lock);
        t = g_next_ticket++;
        g_fences.push_back({t, s});
        // the scratch ring: reused from the start once the GPU has caught up
        if (g_done_ticket + 1 >= t) c->scratch_pos = 0;
    }
    g_fence_new.notify_all();
    return t;
}
HLE_FUNC(emu_mtl_next_completed, u64, ()) {
    ctx();  // a GL context for this thread (to wait on the fences)
    std::unique_lock lk(g_fence_lock);
    for (;;) {
        g_fence_new.wait(lk, [] { return !g_fences.empty(); });
        auto [t, s] = g_fences.front();
        lk.unlock();
        GLenum r = glClientWaitSync_(s, 0, 100000000ull);  // 100 ms steps
        lk.lock();
        if (r == 0x911B) continue;  // GL_TIMEOUT_EXPIRED
        if (!g_fences.empty() && g_fences.front().first == t) {
            g_fences.pop_front();
            glDeleteSync_(s);
            g_done_ticket = t;
        }
        g_fence_done.notify_all();
        return t;
    }
}
HLE_FUNC(emu_mtl_wait_completed, void, (u64 t)) {
    std::unique_lock lk(g_fence_lock);
    g_fence_done.wait_for(lk, std::chrono::seconds(3), [t] { return g_done_ticket >= t; });
}
HLE_FUNC(emu_mtl_label, void, (u64, const char*)) {}
