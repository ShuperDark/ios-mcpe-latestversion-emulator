// Java Edition shader packs (see shaderpack.h).
//
// How the game's frame is turned into the pack's frame:
//   - The game draws the world into its framebuffer (the one with the window's renderbuffer).
//     The first world draw (sky, terrain, entities: a perspective projection) starts the
//     "world" part: from then on that framebuffer is replaced by the pack's G-buffers
//     (colortex0-7 + a depth texture), and each draw is made with the pack's program for its
//     kind (gbuffers_terrain, gbuffers_water, gbuffers_entities, ... with OptiFine's fallbacks).
//   - The pack's programs are Java Edition GLSL (compatibility profile): the vertex data comes
//     through gl_Vertex, gl_Color, gl_MultiTexCoord0/1, gl_Normal and the matrices through
//     gl_ModelViewMatrix / gl_ProjectionMatrix. The attributes are renamed to generic ones bound
//     to the game's attribute locations; the matrices are the fixed-function ones (glLoadMatrix),
//     made from the game's uniforms (WORLDVIEW, PROJ, CHUNK_ORIGIN_AND_SCALE, WORLDVIEWPROJ).
//   - The terrain has no normals: they are made from the vertex buffers (each quad's face), with
//     the tangent, the texture cell's middle (mc_midTexCoord) and the block (mc_Entity, from the
//     texture cell: the block textures are found in the game's texture atlas).
//   - The first orthographic draw (the GUI) or the presentation ends the world part: the shadow
//     map is drawn (the terrain draws of the frame again, from the sun), then the composite
//     passes and the final pass, which draws into the game's framebuffer. The GUI follows.
//
// Debugging a pack: <data>/shader_debug.txt (read every second) with "show=N" shows colortexN (8: depthtex0,
// 9: shadowtex0, 10: shadowcolor0) instead of the picture, "passes=N" runs only the first N composite passes,
// "trace=N" logs the game's draws of the next N frames (once per run).
#include "shaderpack.h"

#include "common.h"
#include "vopts.h"
#include "zipfile.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <regex>
#include <set>
#include <unordered_map>

namespace fs = std::filesystem;

namespace shaderpack {
namespace {

// ============================================================================ GL
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;

#define GLX(RET, NAME, PARAMS) using PFN_##NAME = RET(APIENTRY*) PARAMS; PFN_##NAME NAME##_;
GLX(void, glActiveTexture, (GLenum))
GLX(GLuint, glCreateShader, (GLenum))
GLX(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))
GLX(void, glCompileShader, (GLuint))
GLX(void, glGetShaderiv, (GLuint, GLenum, GLint*))
GLX(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))
GLX(GLuint, glCreateProgram, ())
GLX(void, glAttachShader, (GLuint, GLuint))
GLX(void, glBindAttribLocation, (GLuint, GLuint, const GLchar*))
GLX(void, glLinkProgram, (GLuint))
GLX(void, glGetProgramiv, (GLuint, GLenum, GLint*))
GLX(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))
GLX(void, glDeleteShader, (GLuint))
GLX(void, glDeleteProgram, (GLuint))
GLX(void, glUseProgram, (GLuint))
GLX(GLint, glGetUniformLocation, (GLuint, const GLchar*))
GLX(void, glGetActiveUniform, (GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, GLchar*))
GLX(void, glUniform1i, (GLint, GLint))
GLX(void, glUniform2i, (GLint, GLint, GLint))
GLX(void, glUniform3i, (GLint, GLint, GLint, GLint))
GLX(void, glUniform1f, (GLint, float))
GLX(void, glUniform2f, (GLint, float, float))
GLX(void, glUniform3f, (GLint, float, float, float))
GLX(void, glUniform4f, (GLint, float, float, float, float))
GLX(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const float*))
GLX(void, glGetUniformfv, (GLuint, GLint, float*))
GLX(void, glGetUniformiv, (GLuint, GLint, GLint*))
GLX(GLint, glGetAttribLocation, (GLuint, const GLchar*))
GLX(void, glGenFramebuffers, (GLsizei, GLuint*))
GLX(void, glBindFramebuffer, (GLenum, GLuint))
GLX(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))
GLX(GLenum, glCheckFramebufferStatus, (GLenum))
GLX(void, glDrawBuffers, (GLsizei, const GLenum*))
GLX(void, glGenBuffers, (GLsizei, GLuint*))
GLX(void, glBindBuffer, (GLenum, GLuint))
GLX(void, glBufferData, (GLenum, GLsizeiptr, const void*, GLenum))
GLX(void, glDeleteBuffers, (GLsizei, const GLuint*))
GLX(void, glGetBufferSubData, (GLenum, GLintptr, GLsizeiptr, void*))
GLX(void, glGetBufferParameteriv, (GLenum, GLenum, GLint*))
GLX(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))
GLX(void, glEnableVertexAttribArray, (GLuint))
GLX(void, glDisableVertexAttribArray, (GLuint))
GLX(void, glVertexAttrib4f, (GLuint, float, float, float, float))
GLX(void, glGetVertexAttribiv, (GLuint, GLenum, GLint*))
GLX(void, glGetVertexAttribPointerv, (GLuint, GLenum, void**))
GLX(void, glGenVertexArrays, (GLsizei, GLuint*))
GLX(void, glBindVertexArray, (GLuint))
GLX(void, glGenSamplers, (GLsizei, GLuint*))
GLX(void, glSamplerParameteri, (GLuint, GLenum, GLint))
GLX(void, glBindSampler, (GLuint, GLuint))
GLX(void, glCopyImageSubData, (GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei))
GLX(void, glGenerateMipmap, (GLenum))
GLX(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))
GLX(void, glGetNamedBufferSubData, (GLuint, GLintptr, GLsizeiptr, void*))
GLX(void, glGetNamedBufferParameteriv, (GLuint, GLenum, GLint*))
GLX(void, glDeleteFramebuffers, (GLsizei, const GLuint*))
GLX(void, glDeleteVertexArrays, (GLsizei, const GLuint*))
GLX(void, glDeleteSamplers, (GLsizei, const GLuint*))
GLX(void, glGetFramebufferAttachmentParameteriv, (GLenum, GLenum, GLenum, GLint*))

constexpr GLenum kFramebuffer = 0x8D40, kDrawFramebuffer = 0x8CA9, kReadFramebuffer = 0x8CA8;
constexpr GLenum kColorAttachment0 = 0x8CE0, kDepthAttachment = 0x8D00, kDepthStencilAttachment = 0x821A;
constexpr GLenum kArrayBuffer = 0x8892, kElementArrayBuffer = 0x8893;
constexpr GLenum kTexture0 = 0x84C0;
constexpr GLenum kVertexShader = 0x8B31, kFragmentShader = 0x8B30;
constexpr GLenum kCompileStatus = 0x8B81, kLinkStatus = 0x8B82, kInfoLogLength = 0x8B84;
constexpr GLenum kActiveUniforms = 0x8B86;
constexpr GLenum kSampler2DShadow = 0x8B62;
constexpr GLenum kFloat = 0x1406, kInt = 0x1404, kIntVec2 = 0x8B53, kIntVec3 = 0x8B54, kBool = 0x8B56;
constexpr GLenum kHalfFloat = 0x140B;
constexpr GLenum kClampToEdge = 0x812F;
constexpr GLenum kDepth24Stencil8 = 0x88F0, kDepthStencil = 0x84F9, kUnsignedInt248 = 0x84FA;
constexpr GLenum kDepthComponent24 = 0x81A6;
constexpr GLenum kTextureCompareMode = 0x884C, kTextureCompareFunc = 0x884D, kCompareRefToTexture = 0x884E;
constexpr GLenum kTextureMaxLevel = 0x813D;
constexpr GLenum kAttribEnabled = 0x8622, kAttribSize = 0x8623, kAttribStride = 0x8624, kAttribType = 0x8625,
                 kAttribNormalized = 0x886A, kAttribBuffer = 0x889F, kAttribPointer = 0x8645;
constexpr GLenum kCurrentProgram = 0x8B8D, kActiveTextureBinding = 0x84E0;
constexpr GLenum kArrayBufferBinding = 0x8894, kElementArrayBufferBinding = 0x8895, kVertexArrayBinding = 0x85B5;
constexpr GLenum kDrawFramebufferBinding = 0x8CA6, kReadFramebufferBinding = 0x8CAA;
constexpr GLenum kBufferSize = 0x8764;
constexpr GLenum kStaticDraw = 0x88E4;
constexpr GLenum kLinearMipmapLinear = 0x2703;

// texture units used by the pack's textures (the game uses the first few)
constexpr int kUnitNormals = 8, kUnitSpecular = 9, kUnitNoiseG = 10, kUnitShadow0G = 11, kUnitShadow1G = 12,
              kUnitShadowColor0G = 13, kUnitShadowColor1G = 14, kUnitDepthG = 15, kUnitColortexG = 18;  // +0..7
constexpr int kUnitColortex = 16, kUnitDepth0 = 24, kUnitShadow0 = 27, kUnitShadow1 = 28, kUnitShadowColor0 = 29,
              kUnitShadowColor1 = 30, kUnitNoise = 31;
// attribute locations of the pack's extra inputs (the game uses 0..4)
constexpr GLuint kLocLmConst = 9, kLocTexConst = 10, kLocColorConst = 11, kLocNormal = 12, kLocTangent = 13,
                 kLocEntity = 14, kLocMidTex = 15;

// ============================================================================ math
struct Mat4 {
    float m[16];  // column-major
    static Mat4 identity() {
        Mat4 r{};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
        return r;
    }
    float& at(int row, int col) { return m[col * 4 + row]; }
    float at(int row, int col) const { return m[col * 4 + row]; }
};
Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.at(rr, k) * b.at(k, c);
            r.at(rr, c) = s;
        }
    return r;
}
std::array<float, 4> mul(const Mat4& a, float x, float y, float z, float w) {
    std::array<float, 4> r{};
    for (int i = 0; i < 4; i++) r[i] = a.at(i, 0) * x + a.at(i, 1) * y + a.at(i, 2) * z + a.at(i, 3) * w;
    return r;
}
Mat4 inverse(const Mat4& in) {
    const float* m = in.m;
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    Mat4 r = Mat4::identity();
    if (fabsf(det) < 1e-30f) return r;
    for (int i = 0; i < 16; i++) r.m[i] = inv[i] / det;
    return r;
}
Mat4 translate(float x, float y, float z) {
    Mat4 r = Mat4::identity();
    r.m[12] = x, r.m[13] = y, r.m[14] = z;
    return r;
}
Mat4 scale(float x, float y, float z) {
    Mat4 r = Mat4::identity();
    r.m[0] = x, r.m[5] = y, r.m[10] = z;
    return r;
}
Mat4 rotate(float deg, float x, float y, float z) {  // glRotatef
    float a = deg * 3.14159265f / 180, c = cosf(a), s = sinf(a), l = sqrtf(x * x + y * y + z * z);
    x /= l, y /= l, z /= l;
    Mat4 r = Mat4::identity();
    r.at(0, 0) = x * x * (1 - c) + c, r.at(0, 1) = x * y * (1 - c) - z * s, r.at(0, 2) = x * z * (1 - c) + y * s;
    r.at(1, 0) = y * x * (1 - c) + z * s, r.at(1, 1) = y * y * (1 - c) + c, r.at(1, 2) = y * z * (1 - c) - x * s;
    r.at(2, 0) = x * z * (1 - c) - y * s, r.at(2, 1) = y * z * (1 - c) + x * s, r.at(2, 2) = z * z * (1 - c) + c;
    return r;
}
Mat4 ortho(float l, float r, float b, float t, float n, float f) {
    Mat4 m = Mat4::identity();
    m.at(0, 0) = 2 / (r - l), m.at(1, 1) = 2 / (t - b), m.at(2, 2) = -2 / (f - n);
    m.at(0, 3) = -(r + l) / (r - l), m.at(1, 3) = -(t + b) / (t - b), m.at(2, 3) = -(f + n) / (f - n);
    return m;
}
// orthographic (the GUI): the last row is (0, 0, 0, 1); with a perspective projection it is not
// (for a combined matrix any of its first three entries may be 0, not all of them)
double seconds_now() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}
bool is_perspective(const float* m) { return fabsf(m[3]) + fabsf(m[7]) + fabsf(m[11]) > 1e-6f; }

// ============================================================================ images
bool decode_image(const std::vector<uint8_t>& bytes, int& w, int& h, std::vector<uint8_t>& rgba) {
    static bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || true;
    (void)com;
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    bool ok = false;
    IWICStream* s = nullptr;
    IWICBitmapDecoder* d = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* cv = nullptr;
    if (SUCCEEDED(f->CreateStream(&s)) && SUCCEEDED(s->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), (DWORD)bytes.size())) &&
        SUCCEEDED(f->CreateDecoderFromStream(s, nullptr, WICDecodeMetadataCacheOnDemand, &d)) && SUCCEEDED(d->GetFrame(0, &fr)) &&
        SUCCEEDED(f->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        UINT uw = 0, uh = 0;
        cv->GetSize(&uw, &uh);
        w = (int)uw, h = (int)uh;
        rgba.resize(size_t(w) * h * 4);
        ok = SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)rgba.size(), rgba.data()));
    }
    if (cv) cv->Release();
    if (fr) fr->Release();
    if (d) d->Release();
    if (s) s->Release();
    f->Release();
    return ok;
}
// uncompressed / RLE true-color TGA (the game's .tga block textures)
bool decode_tga(const std::vector<uint8_t>& b, int& w, int& h, std::vector<uint8_t>& rgba) {
    if (b.size() < 18) return false;
    int idlen = b[0], type = b[2], bpp = b[16];
    w = b[12] | b[13] << 8, h = b[14] | b[15] << 8;
    bool top = (b[17] & 0x20) != 0;
    if ((type != 2 && type != 10) || (bpp != 24 && bpp != 32) || w <= 0 || h <= 0) return false;
    size_t p = 18 + idlen, n = size_t(w) * h, px = bpp / 8;
    std::vector<uint8_t> raw(n * 4);
    size_t i = 0;
    auto put = [&](const uint8_t* s) {
        raw[i * 4] = s[2], raw[i * 4 + 1] = s[1], raw[i * 4 + 2] = s[0], raw[i * 4 + 3] = px == 4 ? s[3] : 255;
        i++;
    };
    while (i < n) {
        if (type == 2) {
            if (p + px > b.size()) return false;
            put(&b[p]);
            p += px;
            continue;
        }
        if (p >= b.size()) return false;
        int c = b[p++], k = (c & 0x7f) + 1;
        if (c & 0x80) {
            if (p + px > b.size()) return false;
            for (int j = 0; j < k && i < n; j++) put(&b[p]);
            p += px;
        } else {
            for (int j = 0; j < k && i < n; j++) {
                if (p + px > b.size()) return false;
                put(&b[p]);
                p += px;
            }
        }
    }
    rgba.resize(n * 4);
    for (int y = 0; y < h; y++) memcpy(&rgba[size_t(y) * w * 4], &raw[size_t(top ? y : h - 1 - y) * w * 4], size_t(w) * 4);
    return true;
}
bool read_file(const fs::path& p, std::vector<uint8_t>& out) {
    FILE* f = _wfopen(p.wstring().c_str(), L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? size_t(n) : 0);
    bool ok = fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

// ============================================================================ the list of packs
std::mutex g_list_lock;
fs::path g_root, g_dir;
std::vector<fs::path> g_packs;
std::atomic<int> g_selected{-1};
std::atomic<int> g_loaded_choice{-2};  // the pack loaded on the render thread (-2: none yet)

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

void save_choice() {
    std::lock_guard lk(g_list_lock);
    FILE* f = _wfopen((g_root / L"shaders.txt").wstring().c_str(), L"wb");
    if (!f) return;
    int i = g_selected.load();
    std::string n = i >= 0 && i < (int)g_packs.size() ? utf8(g_packs[i].filename().wstring()) : "";
    fprintf(f, "# The shader pack in use (a file in the shaders folder; empty: none)\nshader=%s\n", n.c_str());
    fclose(f);
}

// ============================================================================ the loaded pack
enum Stage {
    kBasic, kTextured, kTexturedLit, kSkyBasic, kSkyTextured, kClouds, kTerrain, kDamagedBlock, kBlock, kWater,
    kEntities, kHand, kWeather, kArmorGlint, kSpiderEyes, kHandWater, kStageCount
};
const char* const kStageNames[kStageCount] = {
    "gbuffers_basic", "gbuffers_textured", "gbuffers_textured_lit", "gbuffers_skybasic", "gbuffers_skytextured",
    "gbuffers_clouds", "gbuffers_terrain", "gbuffers_damagedblock", "gbuffers_block", "gbuffers_water",
    "gbuffers_entities", "gbuffers_hand", "gbuffers_weather", "gbuffers_armor_glint", "gbuffers_spidereyes",
    "gbuffers_hand_water"};
// OptiFine's fallbacks
const int kFallback[kStageCount] = {-1, kBasic, kTextured, kBasic, kTextured, kTextured, kTexturedLit, kTerrain, kTerrain,
                                    kTerrain, kTexturedLit, kTexturedLit, kTexturedLit, kTextured, kTextured, kHand};

struct Source {
    std::string vs, fs;
    std::vector<int> drawbuffers;  // colortex indices written (gl_FragData[i] -> drawbuffers[i])
    bool present = false;
};

struct BufferSetup {
    GLenum format = 0x8058;  // RGBA8
    bool clear = true;
    float clear_color[4] = {0, 0, 0, 0};
    bool mipmap = false;
};

struct Pack {
    std::string name;
    Source gbuffers[kStageCount];
    Source shadow;
    std::vector<std::pair<std::string, Source>> composites;  // deferred*, then composite*, in order
    int first_composite = 0;                                  // index of the first composite* (after deferred*)
    Source final_;
    BufferSetup buffers[8];
    int shadow_res = 1024;
    float shadow_distance = 160;
    float sun_path_rotation = 0;
    float shadow_interval = 2;
    bool shadow_hw_filter = false, shadow_hw_filter1 = false;
    int noise_res = 256;
    float wetness_halflife = 600, dryness_halflife = 200, eye_brightness_halflife = 10, center_depth_halflife = 1;
    bool old_lighting = true;
    bool clouds_off = false;  // shaders.properties "clouds = off": the game's clouds are not drawn
    bool sun_off = false, moon_off = false;  // "sun = false", "moon = false"
    // OptiFine's dynamic lights for the held item (shaders.properties "dynamicHandLight"): the light of a light-emitting
    // held block raises the block light of what is near the camera (gl_MultiTexCoord1). Not set: on, unless the pack
    // lights it itself (it reads heldBlockLightValue)
    bool dynamic_hand_light = false;
    struct CustomTex {
        std::string stage, sampler;
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        GLuint tex = 0;
    };
    std::vector<CustomTex> custom;
};
std::unique_ptr<Pack> g_pack;

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
bool ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

// ---- the pack's options (OptiFine's "Shader Options"): its values are applied to the sources' lines ----
// "#define NAME" / "//#define NAME" (an option: on / off), "#define NAME value // [values]", "const int|float NAME = value;
// // [values]"; the values chosen (those that are not the default) are in <shaders>/<pack>.txt as "NAME=value"
std::map<std::string, std::string> g_apply_opts;  // used while a pack is loaded

struct OptLine {
    enum Kind { kNone, kBool, kValue, kConst } kind = kNone;
    bool commented = false;
    std::string name, value, comment;
    size_t value_pos = 0, value_len = 0;  // in the line
    size_t define_pos = 0;                // where "#define" starts (bool: the text from there is kept)
};
OptLine parse_opt_line(const std::string& line) {
    OptLine o;
    size_t i = 0, n = line.size();
    auto ws = [&] { while (i < n && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) i++; };
    auto ident = [&] {
        size_t s = i;
        while (i < n && ident_char(line[i])) i++;
        return line.substr(s, i - s);
    };
    ws();
    if (line.compare(i, 2, "//") == 0) {
        o.commented = true;
        i += 2;
        ws();
    }
    if (line.compare(i, 7, "#define") == 0 && i + 7 < n && (line[i + 7] == ' ' || line[i + 7] == '\t')) {
        o.define_pos = i;
        i += 7;
        ws();
        o.name = ident();
        if (o.name.empty()) return {};
        ws();
        if (i >= n || line.compare(i, 2, "//") == 0) {
            o.kind = OptLine::kBool;
            o.comment = i < n ? line.substr(i + 2) : "";
            return o;
        }
        if (o.commented) return {};
        size_t s = i;
        while (i < n && line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line.compare(i, 2, "//") != 0) i++;
        o.value = line.substr(s, i - s);
        o.value_pos = s, o.value_len = i - s;
        ws();
        if (line.compare(i, 2, "//") == 0) o.comment = line.substr(i + 2);
        else if (i < n) return {};  // (a macro with arguments / an expression)
        o.kind = OptLine::kValue;
        return o;
    }
    if (o.commented) return {};
    if (line.compare(i, 6, "const ") == 0) {
        i += 6;
        ws();
        std::string type = ident();
        if (type != "int" && type != "float" && type != "bool") return {};
        ws();
        o.name = ident();
        ws();
        if (o.name.empty() || i >= n || line[i] != '=') return {};
        i++;
        ws();
        size_t s = i;
        while (i < n && line[i] != ';') i++;
        if (i >= n) return {};
        std::string v = trim(line.substr(s, i - s));
        o.value = v, o.value_pos = s, o.value_len = i - s;
        size_t c = line.find("//", i);
        if (c != std::string::npos) o.comment = line.substr(c + 2);
        o.kind = OptLine::kConst;
        return o;
    }
    return {};
}

std::string apply_opt_line(const std::string& line) {
    if (g_apply_opts.empty() || line.find_first_of("#c") == std::string::npos) return line;
    OptLine o = parse_opt_line(line);
    if (o.kind == OptLine::kNone) return line;
    auto it = g_apply_opts.find(o.name);
    if (it == g_apply_opts.end()) return line;
    const std::string& want = it->second;
    if (o.kind == OptLine::kBool) {
        bool on = want == "true";
        if (on && o.commented) return line.substr(0, line.find("//")) + line.substr(o.define_pos);
        if (!on && !o.commented) return "//" + line;
        return line;
    }
    if (want == "true" || want == "false") {
        if (o.kind != OptLine::kConst) return line;
    }
    return line.substr(0, o.value_pos) + want + line.substr(o.value_pos + o.value_len);
}

// #include "file": relative to the file, "/file": to the shaders folder
std::string expand_includes(const ZipFile& z, const std::string& root, const std::string& path, int depth, std::set<std::string>& seen) {
    std::string src;
    if (depth > 16 || !z.read_text(path, src)) return "";
    std::string out, dir = path.substr(0, path.rfind('/') + 1);
    size_t pos = 0;
    while (pos < src.size()) {
        size_t e = src.find('\n', pos);
        if (e == std::string::npos) e = src.size();
        std::string line = src.substr(pos, e - pos);
        pos = e + 1;
        std::string t = trim(line);
        if (t.rfind("#include", 0) == 0) {
            size_t q1 = t.find('"'), q2 = t.rfind('"');
            if (q1 != std::string::npos && q2 > q1) {
                std::string inc = t.substr(q1 + 1, q2 - q1 - 1);
                std::string full = inc[0] == '/' ? root + inc.substr(1) : dir + inc;
                // normalize "a/../b"
                std::vector<std::string> parts;
                size_t s = 0;
                while (s <= full.size()) {
                    size_t k = full.find('/', s);
                    if (k == std::string::npos) k = full.size();
                    std::string p = full.substr(s, k - s);
                    if (p == "..") {
                        if (!parts.empty()) parts.pop_back();
                    } else if (!p.empty() && p != ".") {
                        parts.push_back(p);
                    }
                    s = k + 1;
                }
                full.clear();
                for (auto& p : parts) full += (full.empty() ? "" : "/") + p;
                if (!seen.count(full)) {  // (include guards are usually missing in packs: once is enough)
                    seen.insert(full);
                    out += expand_includes(z, root, full, depth + 1, seen) + "\n";
                }
                continue;
            }
        }
        out += apply_opt_line(line) + "\n";
    }
    return out;
}

std::vector<int> parse_drawbuffers(const std::string& fs) {
    std::vector<int> r;
    size_t p = fs.find("DRAWBUFFERS:");
    if (p != std::string::npos) {
        for (p += 12; p < fs.size() && isdigit((unsigned char)fs[p]); p++) r.push_back(fs[p] - '0');
        return r;
    }
    p = fs.find("RENDERTARGETS:");
    if (p != std::string::npos) {
        p += 14;
        while (p < fs.size()) {
            while (p < fs.size() && (fs[p] == ' ' || fs[p] == ',')) p++;
            if (p >= fs.size() || !isdigit((unsigned char)fs[p])) break;
            r.push_back(atoi(fs.c_str() + p));
            while (p < fs.size() && isdigit((unsigned char)fs[p])) p++;
        }
        return r;
    }
    // not given: the outputs the program writes
    int maxi = -1;
    for (size_t k = fs.find("gl_FragData["); k != std::string::npos; k = fs.find("gl_FragData[", k + 1))
        maxi = std::max(maxi, atoi(fs.c_str() + k + 12));
    for (int i = 0; i <= std::max(0, maxi); i++) r.push_back(i);
    return r;
}

int buffer_index(const std::string& n) {
    static const char* const names[] = {"gcolor", "gdepth", "gnormal", "composite", "gaux1", "gaux2", "gaux3", "gaux4"};
    for (int i = 0; i < 8; i++)
        if (n == names[i]) return i;
    if (n.rfind("colortex", 0) == 0 && n.size() == 9 && n[8] >= '0' && n[8] <= '7') return n[8] - '0';
    return -1;
}

GLenum parse_format(const std::string& f) {
    static const std::pair<const char*, GLenum> fm[] = {
        {"RGBA8", 0x8058}, {"RGBA", 0x8058}, {"RGBA16", 0x805B}, {"RGBA16F", 0x881A}, {"RGBA32F", 0x8814}, {"RGB8", 0x8051},
        {"RGB", 0x8051}, {"RGB16", 0x8054}, {"RGB16F", 0x881B}, {"RGB32F", 0x8815}, {"R11F_G11F_B10F", 0x8C3A},
        {"RGB10_A2", 0x8059}, {"R8", 0x8229}, {"R16", 0x822A}, {"R16F", 0x822D}, {"R32F", 0x822E}, {"RG8", 0x822B},
        {"RG16", 0x822C}, {"RG16F", 0x822F}, {"RG32F", 0x8230}, {"RGB5_A1", 0x8057}, {"RGBA4", 0x8056}, {"RGBA2", 0x8055},
        {"R3_G3_B2", 0x2A10}, {"RGB9_E5", 0x8C3D}, {"RGBA8_SNORM", 0x8F97}, {"RGBA16_SNORM", 0x8F9B}};
    for (auto& [n, v] : fm)
        if (f == n) return v;
    return 0;
}

// "const <type> name = value;" lines (OptiFine reads them even inside comments)
void parse_consts(Pack& p, const std::string& src) {
    size_t pos = 0;
    while ((pos = src.find("const ", pos)) != std::string::npos) {
        size_t s = pos + 6;
        pos = s;
        size_t semi = src.find(';', s), nl = src.find('\n', s);
        if (semi == std::string::npos || (nl != std::string::npos && nl < semi)) continue;
        std::string decl = trim(src.substr(s, semi - s));
        size_t eq = decl.find('=');
        if (eq == std::string::npos) continue;
        std::string lhs = trim(decl.substr(0, eq)), val = trim(decl.substr(eq + 1));
        size_t sp = lhs.find_last_of(" \t");
        if (sp == std::string::npos) continue;
        std::string type = trim(lhs.substr(0, sp)), name = trim(lhs.substr(sp + 1));
        auto num = [&](float& out) {
            char* end = nullptr;
            float v = strtof(val.c_str(), &end);
            if (end != val.c_str()) out = v;
        };
        auto flag = [&](bool& out) {
            if (val == "true") out = true;
            else if (val == "false") out = false;
        };
        if (name == "shadowMapResolution") {
            float v = 0;
            num(v);
            if (v >= 64 && v <= 8192) p.shadow_res = (int)v;
        } else if (name == "shadowDistance") num(p.shadow_distance);
        else if (name == "sunPathRotation") num(p.sun_path_rotation);
        else if (name == "shadowIntervalSize") num(p.shadow_interval);
        else if (name == "shadowHardwareFiltering" || name == "shadowHardwareFiltering0") flag(p.shadow_hw_filter);
        else if (name == "shadowHardwareFiltering1") flag(p.shadow_hw_filter1);
        else if (name == "noiseTextureResolution") {
            float v = 0;
            num(v);
            if (v >= 4 && v <= 4096) p.noise_res = (int)v;
        } else if (name == "wetnessHalflife") num(p.wetness_halflife);
        else if (name == "drynessHalflife") num(p.dryness_halflife);
        else if (name == "eyeBrightnessHalflife") num(p.eye_brightness_halflife);
        else if (name == "centerDepthHalflife") num(p.center_depth_halflife);
        else {
            auto ends = [&](const char* suf, std::string& base) {
                size_t l = strlen(suf);
                if (name.size() <= l || name.compare(name.size() - l, l, suf) != 0) return false;
                base = name.substr(0, name.size() - l);
                return true;
            };
            std::string base;
            int bi;
            if (ends("Format", base) && (bi = buffer_index(base)) >= 0) {
                if (GLenum f = parse_format(val)) p.buffers[bi].format = f;
            } else if (ends("ClearColor", base) && (bi = buffer_index(base)) >= 0) {
                float c[4] = {0, 0, 0, 0};
                if (sscanf(val.c_str(), "vec4(%f,%f,%f,%f)", &c[0], &c[1], &c[2], &c[3]) == 4 ||
                    sscanf(val.c_str(), "vec4( %f , %f , %f , %f )", &c[0], &c[1], &c[2], &c[3]) == 4)
                    memcpy(p.buffers[bi].clear_color, c, sizeof c);
            } else if (ends("Clear", base) && (bi = buffer_index(base)) >= 0) {
                flag(p.buffers[bi].clear);
            } else if (ends("MipmapEnabled", base) && (bi = buffer_index(base)) >= 0) {
                flag(p.buffers[bi].mipmap);
            }
        }
        (void)type;
    }
}

std::map<std::string, std::string> parse_properties(const std::string& text) {
    std::map<std::string, std::string> r;
    std::string cur;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        bool cont = !line.empty() && line.back() == '\\';
        if (cont) line.pop_back();
        cur += line;
        if (cont) continue;
        std::string t = trim(cur);
        cur.clear();
        if (t.empty() || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        r[trim(t.substr(0, eq))] = trim(t.substr(eq + 1));
    }
    return r;
}

// the folder with the programs: "shaders/" (possibly inside a folder)
bool pack_root(const ZipFile& z, std::string& root) {
    bool found = false;
    for (auto& n : z.names()) {
        size_t k = n.find("shaders/");
        if (k == std::string::npos || (k > 0 && n[k - 1] != '/')) continue;
        std::string r = n.substr(0, k + 8);
        std::string rest = n.substr(k + 8);
        if (rest.find('/') != std::string::npos) continue;
        if (rest.rfind("composite", 0) == 0 || rest.rfind("gbuffers_", 0) == 0 || rest.rfind("final.", 0) == 0) {
            if (!found || r.size() < root.size()) root = r;
            found = true;
        }
    }
    return found;
}

// the pack's option values (<shaders>/<pack>.txt: "NAME=value")
fs::path options_file(const fs::path& pack) { return pack.parent_path() / (pack.filename().wstring() + L".txt"); }
std::map<std::string, std::string> read_option_values(const fs::path& pack) {
    std::map<std::string, std::string> r;
    std::vector<uint8_t> b;
    if (!read_file(options_file(pack), b)) return r;
    std::string t(b.begin(), b.end());
    size_t pos = 0;
    while (pos < t.size()) {
        size_t e = t.find('\n', pos);
        if (e == std::string::npos) e = t.size();
        std::string line = trim(t.substr(pos, e - pos));
        pos = e + 1;
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (!k.empty() && !v.empty()) r[k] = v;
    }
    return r;
}

// dim: "" (the overworld) or "world-1/" (the nether: its programs are used where the pack has them)
std::unique_ptr<Pack> load_pack(const fs::path& path, const std::string& dim) {
    ZipFile z;
    if (!z.open(path.wstring())) {
        LOGW("shaders: %s is not a zip file", utf8(path.filename().wstring()).c_str());
        return nullptr;
    }
    std::string root;
    if (!pack_root(z, root)) {
        LOGW("shaders: %s has no shaders folder", utf8(path.filename().wstring()).c_str());
        return nullptr;
    }
    g_apply_opts = read_option_values(path);
    struct Clear { ~Clear() { g_apply_opts.clear(); } } clear_opts;
    if (!g_apply_opts.empty()) LOGI("shaders: %zu options set (%s)", g_apply_opts.size(), utf8(options_file(path).filename().wstring()).c_str());
    auto p = std::make_unique<Pack>();
    p->name = utf8(path.filename().wstring());
    std::string props_text;
    z.read_text(root + "shaders.properties", props_text);
    auto props = parse_properties(props_text);
    auto enabled = [&](const std::string& prog) {
        auto it = props.find("program." + prog + ".enabled");
        return it == props.end() || (it->second != "false" && it->second.rfind("!", 0) != 0 && it->second != "0");
    };
    if (props.count("oldLighting")) p->old_lighting = props["oldLighting"] == "true";
    if (props.count("clouds")) p->clouds_off = props["clouds"] == "off";
    if (props.count("sun")) p->sun_off = props["sun"] == "false";
    if (props.count("moon")) p->moon_off = props["moon"] == "false";
    auto load = [&](const std::string& name, Source& s) {
        if (!enabled(name)) return;
        std::string vsp = root + name + ".vsh", fsp = root + name + ".fsh";
        if (!dim.empty() && z.has(root + dim + name + ".vsh") && z.has(root + dim + name + ".fsh"))
            vsp = root + dim + name + ".vsh", fsp = root + dim + name + ".fsh";
        if (!z.has(vsp) || !z.has(fsp)) return;
        std::set<std::string> seen;
        s.vs = expand_includes(z, root, vsp, 0, seen);
        seen.clear();
        s.fs = expand_includes(z, root, fsp, 0, seen);
        s.present = !s.vs.empty() && !s.fs.empty();
        if (!s.present) return;
        s.drawbuffers = parse_drawbuffers(s.fs);
        parse_consts(*p, s.vs);
        parse_consts(*p, s.fs);
    };
    for (int i = 0; i < kStageCount; i++) load(kStageNames[i], p->gbuffers[i]);
    load("shadow", p->shadow);
    for (int i = 0; i < 16; i++) {
        Source s;
        std::string n = i ? "deferred" + std::to_string(i) : "deferred";
        load(n, s);
        if (s.present) p->composites.push_back({n, s});
    }
    {
        bool own = false;
        for (auto& g : p->gbuffers) own |= g.vs.find("heldBlockLightValue") != std::string::npos || g.fs.find("heldBlockLightValue") != std::string::npos;
        p->dynamic_hand_light = props.count("dynamicHandLight") ? props["dynamicHandLight"] == "true" : !own;
    }
    p->first_composite = (int)p->composites.size();
    for (int i = 0; i < 16; i++) {
        Source s;
        std::string n = i ? "composite" + std::to_string(i) : "composite";
        load(n, s);
        if (s.present) p->composites.push_back({n, s});
    }
    load("final", p->final_);
    p->buffers[1].clear_color[0] = p->buffers[1].clear_color[1] = p->buffers[1].clear_color[2] = p->buffers[1].clear_color[3] = 1;
    // custom textures: texture.<stage>.<sampler>=<file in the shaders folder>
    for (auto& [k, v] : props) {
        if (k.rfind("texture.", 0) != 0) continue;
        size_t d = k.find('.', 8);
        if (d == std::string::npos) continue;
        Pack::CustomTex t;
        t.stage = k.substr(8, d - 8);
        t.sampler = k.substr(d + 1);
        std::string file = v;
        if (!file.empty() && file[0] == '/') file = file.substr(1);
        std::vector<uint8_t> bytes;
        if (!z.read(root + file, bytes) || !decode_image(bytes, t.w, t.h, t.rgba)) {
            LOGW("shaders: texture %s (%s) not loaded", k.c_str(), v.c_str());
            continue;
        }
        p->custom.push_back(std::move(t));
    }
    int ng = 0;
    for (auto& s : p->gbuffers) ng += s.present;
    LOGI("shaders: loaded %s%s (%d gbuffers programs, shadow %s, %zu composite passes, final %s; shadow map %d, distance %.0f, sun path %.0f)",
         p->name.c_str(), dim.empty() ? "" : " (nether)", ng, p->shadow.present ? "yes" : "no", p->composites.size(), p->final_.present ? "yes" : "no", p->shadow_res,
         p->shadow_distance, p->sun_path_rotation);
    return p;
}

// ============================================================================ the video settings (vopts, Settings > Video)
struct VideoSettings {
    unsigned gen = 0;
    float render_q = 1, shadow_q = 1, hand_depth = 1;  // Render Quality, Shadow Quality, Hand Depth
    int aa = 0;                                          // Antialiasing: 0 OFF, 1 FXAA 2x, 2 FXAA 4x
    int old_lighting = 0;                                // 0 the pack's, 1 ON, 2 OFF
    bool hand_light = true;                              // Dynamic Lights (the held light source)
    bool clouds = true, rain = true, sky = true, stars = true, sun_moon = true, weather = true, fog = true;
    int fog_start = 3;      // 0.2 0.4 0.6 0.8 (3: the default, the game's own)
    float cloud_height = 0; // blocks up
    int time = 0;           // 0 the world's, 1 day, 2 night
    int mip_levels = 3, mip_type = 1, af = 0;
    bool anim_water = true, anim_lava = true, anim_fire = true, anim_portal = true, anim_terrain = true;
} g_vs;
std::atomic<bool> g_reload_pack_vs{false};

void refresh_video_settings() {
    unsigned gen = vopts::generation();
    if (gen == g_vs.gen) return;
    static const float q[] = {0.5f, 0.7f, 1.0f, 1.5f, 2.0f}, hd[] = {0.5f, 1.0f, 2.0f};
    VideoSettings o = g_vs;
    g_vs.gen = gen;
    g_vs.aa = vopts::get("ofAaLevel");
    g_vs.render_q = q[std::clamp(vopts::get("ofRenderQuality"), 0, 4)];
    g_vs.shadow_q = q[std::clamp(vopts::get("ofShadowQuality"), 0, 4)];
    g_vs.hand_depth = hd[std::clamp(vopts::get("ofHandDepth"), 0, 2)];
    g_vs.old_lighting = vopts::get("ofOldLighting");
    g_vs.hand_light = vopts::get("ofDynamicLights") != 0;
    g_vs.clouds = vopts::get("ofClouds") != 3;
    g_vs.cloud_height = vopts::get("ofCloudHeight") * 12.8f;
    g_vs.rain = vopts::get("ofRain") != 3;
    g_vs.sky = vopts::get("ofSky") != 0;
    g_vs.stars = vopts::get("ofStars") != 0;
    g_vs.sun_moon = vopts::get("ofSunMoon") != 0;
    g_vs.fog = vopts::get("ofFogFancy") != 2;
    g_vs.fog_start = vopts::get("ofFogStart");
    g_vs.weather = vopts::get("ofWeather") != 0;
    g_vs.time = vopts::get("ofTime");
    g_vs.mip_levels = vopts::get("ofMipmapLevels");
    g_vs.mip_type = vopts::get("ofMipmapType");
    g_vs.af = vopts::get("ofAfLevel");
    g_vs.anim_water = vopts::get("ofAnimatedWater") != 0;
    g_vs.anim_lava = vopts::get("ofAnimatedLava") != 0;
    g_vs.anim_fire = vopts::get("ofAnimatedFire") != 0;
    g_vs.anim_portal = vopts::get("ofAnimatedPortal") != 0;
    g_vs.anim_terrain = vopts::get("ofAnimatedTerrain") != 0;
    // (the programs are made with MC_RENDER_QUALITY, MC_SHADOW_QUALITY, MC_HAND_DEPTH: made again)
    if (o.gen && (o.render_q != g_vs.render_q || o.shadow_q != g_vs.shadow_q || o.hand_depth != g_vs.hand_depth)) g_reload_pack_vs = true;
}

// ============================================================================ the "Shader Options" screens
// The options of the pack chosen (read from its sources), the screens of shaders.properties (screen=..., screen.NAME=...;
// without them: all the options), its profiles (profile.NAME=...) and its names for them (shaders/lang/<language>.lang).
// The settings screen (guest/EmuShaders) shows one screen at a time; the values chosen are saved and the pack is loaded
// again with them when the screens are closed.
struct ShOption {
    std::string name;
    bool is_bool = false;
    std::vector<std::string> values;  // the choices (bool: false, true)
    std::string def, cur;
};
struct ShMeta {
    fs::path pack;
    std::vector<ShOption> opts;
    std::map<std::string, int> index;
    std::map<std::string, std::string> props, lang, lang_en;
    std::vector<std::pair<std::string, std::string>> profiles;  // name, its definition
    bool changed = false;
};
std::mutex g_meta_lock;
std::unique_ptr<ShMeta> g_meta;
struct ShEntry {
    int kind = 0;  // 0 empty, 1 option, 2 screen, 3 profile
    std::string name;
};
std::vector<ShEntry> g_screen;
std::string g_screen_name;
std::atomic<bool> g_reload_pack{false};  // the options changed: the pack is loaded again (render thread)

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> r;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isspace((unsigned char)s[i])) i++;
        size_t b = i;
        while (i < s.size() && !isspace((unsigned char)s[i])) i++;
        if (i > b) r.push_back(s.substr(b, i - b));
    }
    return r;
}

std::map<std::string, std::string> parse_lang(const std::string& text) {
    std::map<std::string, std::string> r;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = trim(text.substr(pos, e - pos));
        pos = e + 1;
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq != std::string::npos) r[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return r;
}

std::unique_ptr<ShMeta> read_meta(const fs::path& path) {
    ZipFile z;
    std::string root;
    if (!z.open(path.wstring()) || !pack_root(z, root)) return nullptr;
    auto m = std::make_unique<ShMeta>();
    m->pack = path;
    std::string props_text;
    z.read_text(root + "shaders.properties", props_text);
    m->props = parse_properties(props_text);
    // profiles, in their order
    {
        size_t pos = 0;
        while (pos < props_text.size()) {
            size_t e = props_text.find('\n', pos);
            if (e == std::string::npos) e = props_text.size();
            std::string t = trim(props_text.substr(pos, e - pos));
            pos = e + 1;
            if (t.rfind("profile.", 0) != 0) continue;
            size_t eq = t.find('=');
            if (eq == std::string::npos) continue;
            std::string n = trim(t.substr(8, eq - 8));
            auto it = m->props.find("profile." + n);
            if (!n.empty() && it != m->props.end()) m->profiles.push_back({n, it->second});
        }
    }
    std::string lt;
    std::string lang = vopts::language();
    if (z.read_text(root + "lang/" + lang + ".lang", lt)) m->lang = parse_lang(lt);
    if (z.read_text(root + "lang/en_US.lang", lt)) m->lang_en = parse_lang(lt);
    // the sources: the names used by #ifdef / #ifndef / defined (the on / off options), then the option lines
    std::vector<std::string> files;
    for (auto& n : z.names()) {
        if (n.compare(0, root.size(), root) != 0) continue;
        size_t d = n.rfind('.');
        std::string ext = d == std::string::npos ? "" : n.substr(d + 1);
        if (ext == "vsh" || ext == "fsh" || ext == "gsh" || ext == "csh" || ext == "glsl" || ext == "inc") files.push_back(n);
    }
    std::sort(files.begin(), files.end());
    std::set<std::string> tested;
    std::vector<std::string> texts;
    for (auto& f : files) {
        std::string t;
        if (!z.read_text(f, t)) continue;
        for (const char* kw : {"#ifdef", "#ifndef", "defined"}) {
            size_t l = strlen(kw);
            for (size_t k = t.find(kw); k != std::string::npos; k = t.find(kw, k + l)) {
                size_t i = k + l;
                while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '(')) i++;
                size_t b = i;
                while (i < t.size() && ident_char(t[i])) i++;
                if (i > b) tested.insert(t.substr(b, i - b));
            }
        }
        texts.push_back(std::move(t));
    }
    auto values = read_option_values(path);
    for (auto& t : texts) {
        size_t pos = 0;
        while (pos < t.size()) {
            size_t e = t.find('\n', pos);
            if (e == std::string::npos) e = t.size();
            std::string line = t.substr(pos, e - pos);
            pos = e + 1;
            if (line.find("define") == std::string::npos && line.find("const") == std::string::npos) continue;
            OptLine o = parse_opt_line(line);
            if (o.kind == OptLine::kNone || m->index.count(o.name)) continue;
            ShOption s;
            s.name = o.name;
            if (o.kind == OptLine::kBool) {
                if (!tested.count(o.name)) continue;
                s.is_bool = true;
                s.values = {"false", "true"};
                s.def = o.commented ? "false" : "true";
            } else {
                if (o.kind == OptLine::kConst && (o.value == "true" || o.value == "false")) {
                    s.is_bool = true;
                    s.values = {"false", "true"};
                    s.def = o.value;
                } else {
                    size_t a = o.comment.rfind('['), b = o.comment.rfind(']');
                    if (a == std::string::npos || b == std::string::npos || b < a) continue;
                    s.values = split_ws(o.comment.substr(a + 1, b - a - 1));
                    if (s.values.empty()) continue;
                    s.def = o.value;
                    if (std::find(s.values.begin(), s.values.end(), s.def) == s.values.end()) s.values.insert(s.values.begin(), s.def);
                }
            }
            auto v = values.find(s.name);
            s.cur = v != values.end() && std::find(s.values.begin(), s.values.end(), v->second) != s.values.end() ? v->second : s.def;
            m->index[s.name] = (int)m->opts.size();
            m->opts.push_back(std::move(s));
        }
    }
    LOGI("shaders: %s: %zu options, %zu profiles", utf8(path.filename().wstring()).c_str(), m->opts.size(), m->profiles.size());
    return m;
}

std::string meta_text(const ShMeta& m, const std::string& key) {
    auto it = m.lang.find(key);
    if (it != m.lang.end()) return it->second;
    it = m.lang_en.find(key);
    return it != m.lang_en.end() ? it->second : "";
}

bool lang_ja() { return vopts::language() == "ja_JP"; }

// a profile's assignments (OPT, !OPT, OPT:value, OPT=value, profile.OTHER)
void profile_values(const ShMeta& m, const std::string& def, std::map<std::string, std::string>& out, int depth = 0) {
    if (depth > 8) return;
    for (auto& tok : split_ws(def)) {
        if (tok.rfind("profile.", 0) == 0) {
            for (auto& [n, d] : m.profiles)
                if (n == tok.substr(8)) profile_values(m, d, out, depth + 1);
            continue;
        }
        if (tok.find("program.") != std::string::npos) continue;
        size_t c = tok.find_first_of(":=");
        if (c != std::string::npos) out[tok.substr(0, c)] = tok.substr(c + 1);
        else if (tok[0] == '!') out[tok.substr(1)] = "false";
        else out[tok] = "true";
    }
}

int current_profile(const ShMeta& m) {
    for (int i = 0; i < (int)m.profiles.size(); i++) {
        std::map<std::string, std::string> v;
        profile_values(m, m.profiles[i].second, v);
        bool all = true;
        for (auto& [k, val] : v) {
            auto it = m.index.find(k);
            if (it != m.index.end() && m.opts[it->second].cur != val) all = false;
        }
        if (all) return i;
    }
    return -1;
}

std::string option_label(const ShMeta& m, const ShOption& o) {
    std::string n = meta_text(m, "option." + o.name);
    if (n.empty()) n = o.name;
    std::string v;
    if (o.is_bool) {
        v = o.cur == "true" ? (lang_ja() ? "オン" : "ON") : (lang_ja() ? "オフ" : "OFF");
    } else {
        v = meta_text(m, "value." + o.name + "." + o.cur);
        if (v.empty()) v = meta_text(m, "prefix." + o.name) + o.cur + meta_text(m, "suffix." + o.name);
    }
    return n + ": " + v;
}

void save_meta(ShMeta& m) {
    bool any = false;
    for (auto& o : m.opts) any |= o.cur != o.def;
    if (!any) {  // (all the defaults: no file)
        std::error_code ec;
        fs::remove(options_file(m.pack), ec);
        return;
    }
    FILE* f = _wfopen(options_file(m.pack).wstring().c_str(), L"wb");
    if (!f) return;
    for (auto& o : m.opts)
        if (o.cur != o.def) fprintf(f, "%s=%s\n", o.name.c_str(), o.cur.c_str());
    fclose(f);
}

// ============================================================================ GLSL
std::string g_gl_vendor_macro;

// Java GLSL -> what runs here: the fixed-function attributes are generic ones
// wrap: the pack's main() is called from one that first discards as the game's shader would (emu_discard: 0 none,
// 1 alpha < 0.5, 2 alpha <= 0, 3 nothing drawn: rgba == 0; on emu_albedo at the game's texture coordinate)
std::string translate(const std::string& src, bool vertex, bool wrap = false) {
    std::string version = "#version 120";
    std::vector<std::string> extensions;
    std::string body;
    size_t pos = 0;
    bool has_version = false;
    while (pos < src.size()) {
        size_t e = src.find('\n', pos);
        if (e == std::string::npos) e = src.size();
        std::string line = src.substr(pos, e - pos);
        pos = e + 1;
        std::string t = trim(line);
        if (t.rfind("#version", 0) == 0 && !has_version) {
            version = t;
            has_version = true;
            body += "\n";
            continue;
        }
        if (t.rfind("#extension", 0) == 0) {
            extensions.push_back(t);
            body += "\n";
            continue;
        }
        body += line + "\n";
    }
    int ver = atoi(version.c_str() + 8);
    bool compat = version.find("compatibility") != std::string::npos;
    if (ver >= 150 && !compat && version.find("core") == std::string::npos) version += " compatibility";
    if (ver >= 150) compat = true;
    // renames (whole identifiers only)
    struct Ren { const char* from; const char* to; };
    static const Ren vren[] = {
        {"gl_Vertex", "vec4(emu_Vertex.xyz, 1.0)"},
        {"gl_Color", "emu_color()"},
        {"gl_MultiTexCoord0", "emu_MultiTexCoord0"},
        {"gl_MultiTexCoord1", "emu_lmcoord()"},
        {"gl_MultiTexCoord2", "vec4(0.0, 0.0, 0.0, 1.0)"},
        {"gl_MultiTexCoord3", "vec4(0.0, 0.0, 0.0, 1.0)"},
        {"gl_Normal", "emu_Normal"},
        {"ftransform", "emu_ftransform"},
    };
    std::string out;
    out.reserve(body.size() + 1024);
    bool uses_lm = false;
    if (vertex) {
        for (size_t i = 0; i < body.size();) {
            if (ident_char(body[i]) && (i == 0 || !ident_char(body[i - 1]))) {
                size_t j = i;
                while (j < body.size() && ident_char(body[j])) j++;
                std::string id = body.substr(i, j - i);
                if (id == "gl_MultiTexCoord1") uses_lm = true;
                const char* rep = nullptr;
                for (auto& r : vren)
                    if (id == r.from) rep = r.to;
                out += rep ? rep : id;
                i = j;
                continue;
            }
            out += body[i++];
        }
    } else {
        out = body;
    }
    std::string head = version + "\n";
    for (auto& x : extensions) head += x + "\n";
    char quality[160];
    snprintf(quality, sizeof quality, "#define MC_RENDER_QUALITY %.1f\n#define MC_SHADOW_QUALITY %.1f\n#define MC_HAND_DEPTH %g\n", g_vs.render_q,
             g_vs.shadow_q, 0.125 * g_vs.hand_depth);
    head += "#define MC_VERSION 11202\n#define MC_GL_VERSION 430\n#define MC_GLSL_VERSION " + std::to_string(std::max(ver, 120)) +
            "\n#define MC_OS_WINDOWS\n" + g_gl_vendor_macro + quality;
    if (vertex) {
        const char* a = (ver < 140 || compat) ? "attribute" : "in";
        head += std::string(a) + " vec4 emu_Vertex;\n" + a + " vec4 emu_Color;\n" + a + " vec4 emu_MultiTexCoord0;\n" + a +
                " vec4 emu_MultiTexCoord1;\n" + a + " vec3 emu_Normal;\n" +
                "uniform vec4 emu_LmScale;\nuniform vec4 emu_LmBias;\n"
                "vec4 emu_ftransform() { return gl_ModelViewProjectionMatrix * vec4(emu_Vertex.xyz, 1.0); }\n"
                // the terrain's colour without the game's shading of its sides (OptiFine's "Old Lighting" OFF: the pack
                // shades them; the game's: top 1, bottom 0.5, north / south 0.8, east / west 0.6)
                "uniform float emu_Unshade;\n"
                "vec4 emu_color() {\n"
                "    vec4 c = emu_Color;\n"
                "    if (emu_Unshade > 0.5) {\n"
                "        vec3 n = abs(emu_Normal);\n"
                "        float s = n.y > 0.5 ? (emu_Normal.y > 0.0 ? 1.0 : 0.5) : (n.z > 0.5 ? 0.8 : 0.6);\n"
                "        c.rgb = min(c.rgb / s, vec3(1.0));\n"
                "    }\n"
                "    return c;\n"
                "}\n";
        // the lightmap coordinate (0..240); emu_HandLight: the held block's light (Pack::dynamic_hand_light; 0: none), as
        // OptiFine's dynamic lights: the light minus the distance from the camera
        if (uses_lm)
            head += "uniform float emu_HandLight;\n"
                    "vec4 emu_lmcoord() {\n"
                    "    vec4 lm = emu_MultiTexCoord1 * emu_LmScale + emu_LmBias;\n"
                    "    if (emu_HandLight > 0.0) {\n"
                    "        float d = length((gl_ModelViewMatrix * vec4(emu_Vertex.xyz, 1.0)).xyz);\n"
                    "        lm.x = max(lm.x, clamp(emu_HandLight - d, 0.0, 15.0) * 16.0);\n"
                    "    }\n"
                    "    return lm;\n"
                    "}\n";
    }
    if (wrap) {
        static const std::regex main_re(R"(\bvoid\s+main\s*\(\s*(void)?\s*\))");
        out = std::regex_replace(out, main_re, "void emu_pack_main()");
        bool old = ver < 130 || compat;
        if (vertex) {
            out += std::string("\n") + (old ? "varying" : "out") +
                   " vec2 emu_uv;\nvoid main() { emu_uv = emu_MultiTexCoord0.xy; emu_pack_main(); }\n";
        } else {
            out += std::string("\n") + (old ? "varying" : "in") +
                   " vec2 emu_uv;\nuniform sampler2D emu_albedo;\nuniform int emu_discard;\n"
                   "void main() {\n"
                   "    if (emu_discard != 0) {\n"
                   "        vec4 emu_c = texture2D(emu_albedo, emu_uv);\n"
                   "        if (emu_discard == 1 && emu_c.a < 0.5) discard;\n"
                   "        if (emu_discard == 2 && emu_c.a <= 0.0) discard;\n"
                   "        if (emu_discard == 3 && emu_c.a + emu_c.r + emu_c.g + emu_c.b == 0.0) discard;\n"
                   "    }\n"
                   "    emu_pack_main();\n"
                   "}\n";
        }
    }
    head += "#line 1\n";
    return head + out;
}

GLuint compile(GLenum type, const std::string& src, const std::string& what) {
    GLuint s = glCreateShader_(type);
    const GLchar* p = src.c_str();
    glShaderSource_(s, 1, &p, nullptr);
    glCompileShader_(s);
    GLint ok = 0;
    glGetShaderiv_(s, kCompileStatus, &ok);
    if (!ok) {
        GLint n = 0;
        glGetShaderiv_(s, kInfoLogLength, &n);
        std::string log(std::max(n, 1), 0);
        glGetShaderInfoLog_(s, n, nullptr, log.data());
        LOGW("shaders: %s does not compile:\n%s", what.c_str(), log.substr(0, 3000).c_str());
        glDeleteShader_(s);
        return 0;
    }
    return s;
}

// ============================================================================ programs
struct UniformInfo { GLint loc; GLenum type; };

struct Program {
    GLuint id = 0;
    std::unordered_map<std::string, UniformInfo> uniforms;
    std::vector<int> drawbuffers;
    uint64_t frame_set = ~0ull;  // the frame whose uniforms are set
    bool ok = false;
    GLint loc(const char* n) const {
        auto it = uniforms.find(n);
        return it == uniforms.end() ? -1 : it->second.loc;
    }
};

struct AttribLocs { GLint pos = -1, color = -1, tex0 = -1, tex1 = -1, normal = -1; };

bool link_program(Program& pr, const Source& s, const AttribLocs& a, const std::string& what, bool wrap = false) {
    GLuint vs = compile(kVertexShader, translate(s.vs, true, wrap), what + ".vsh");
    GLuint fsh = vs ? compile(kFragmentShader, translate(s.fs, false, wrap), what + ".fsh") : 0;
    if (!vs || !fsh) {
        if (vs) glDeleteShader_(vs);
        return false;
    }
    GLuint p = glCreateProgram_();
    glAttachShader_(p, vs);
    glAttachShader_(p, fsh);
    glBindAttribLocation_(p, a.pos >= 0 ? a.pos : 0, "emu_Vertex");
    glBindAttribLocation_(p, a.color >= 0 ? a.color : kLocColorConst, "emu_Color");
    glBindAttribLocation_(p, a.tex0 >= 0 ? a.tex0 : kLocTexConst, "emu_MultiTexCoord0");
    glBindAttribLocation_(p, a.tex1 >= 0 ? a.tex1 : kLocLmConst, "emu_MultiTexCoord1");
    glBindAttribLocation_(p, a.normal >= 0 ? a.normal : kLocNormal, "emu_Normal");
    glBindAttribLocation_(p, kLocTangent, "at_tangent");
    glBindAttribLocation_(p, kLocEntity, "mc_Entity");
    glBindAttribLocation_(p, kLocMidTex, "mc_midTexCoord");
    glLinkProgram_(p);
    glDeleteShader_(vs);
    glDeleteShader_(fsh);
    GLint ok = 0;
    glGetProgramiv_(p, kLinkStatus, &ok);
    if (!ok) {
        GLint n = 0;
        glGetProgramiv_(p, kInfoLogLength, &n);
        std::string log(std::max(n, 1), 0);
        glGetProgramInfoLog_(p, n, nullptr, log.data());
        LOGW("shaders: %s does not link:\n%s", what.c_str(), log.substr(0, 3000).c_str());
        glDeleteProgram_(p);
        return false;
    }
    pr.id = p;
    pr.ok = true;
    pr.drawbuffers = s.drawbuffers;
    GLint count = 0;
    glGetProgramiv_(p, kActiveUniforms, &count);
    for (GLint i = 0; i < count; i++) {
        char name[256];
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform_(p, i, sizeof name, nullptr, &size, &type, name);
        std::string n = name;
        if (n.size() > 3 && n.compare(n.size() - 3, 3, "[0]") == 0) n.resize(n.size() - 3);
        pr.uniforms[n] = {glGetUniformLocation_(p, name), type};
    }
    return true;
}

// ============================================================================ game programs
enum Kind { kOther, kKTerrain, kKCutout, kKWater, kKTerrainEntity, kKEntity, kKGlint, kKSky, kKClouds, kKWeather, kKOverlay,
            kKTextured, kKBasic };
struct GameProgram {
    Kind kind = kOther;
    bool hand_item = false;
    bool seasons = false, seasons_far = false;          // leaves: the vertex colour is a place in the foliage colour map
    bool multitexture = false, color_mask = false, color_second = false;  // entity textures combined in the game's shader
    bool no_texture = false;  // the colour only (held items: voxels)
    bool alpha_test = false, emissive = false;  // the game's shader discards (ALPHA_TEST; USE_EMISSIVE: rgba 0)
    GLint u_tex2 = -1, u_change_color = -1, u_alpha = -1;
    AttribLocs attr;
    GLint u_worldview = -1, u_proj = -1, u_wvp = -1, u_world = -1, u_origin = -1, u_fog_color = -1, u_fog_control = -1,
          u_render_distance = -1, u_tile_light = -1, u_current_color = -1, u_view_pos = -1, u_overlay = -1, u_tex0 = -1, u_tex1 = -1;
    // the video settings: the sun and the moon (texture_ccolor), the stars, the held light in the game's terrain shader
    // (EMU_HAND_LIGHT, see patch_game_shader); the game's values of the uniforms changed (fog, the clouds' height) and
    // the values written instead (a new value from the game: different from it)
    bool sun_moon = false, stars = false;
    GLint u_hand = -1;
    GLint u_uv_offset = -1, u_uv_rotation = -1, u_glint_scale = -1, u_glint_color = -1;  // GLINT: its two layers
    float fog_game[2] = {}, fog_set[2] = {}, wvp_game[16] = {}, wvp_set[16] = {};
    bool fog_changed = false, wvp_changed = false;
};
std::unordered_map<GLuint, GameProgram> g_game_programs;
GLuint g_cur_game_program = 0;

bool has_define(const std::string& s, const char* d) {
    std::string k = std::string("#define ") + d;
    size_t p = s.find(k);
    return p != std::string::npos && (p + k.size() >= s.size() || !ident_char(s[p + k.size()]));
}

int discard_mode(const GameProgram& gp) {
    if (!gp.alpha_test) return 0;
    if (gp.kind == kKEntity || gp.kind == kKGlint) return gp.emissive ? 3 : gp.color_mask ? 2 : 1;  // NEEDS_DISCARD
    return 1;
}

// ============================================================================ render targets
struct Targets {
    int w = 0, h = 0;
    GLuint fbo = 0, composite_fbo = 0;
    GLuint tex[8][2] = {};
    int cur[8] = {};
    GLenum fmt[8] = {};
    GLuint depth[3] = {};  // depthtex0 (drawn), depthtex1 (without the translucent), depthtex2 (without the hand)
    GLuint composite_ds = 0;  // the composite passes' depth / stencil: a copy of depthtex0 (its stencil: kCamStencil)
    GLuint shadow_fbo = 0, shadow_depth = 0, shadow_color[2] = {};
    int shadow_res = 0;
    GLuint noise = 0, white = 0, flat_normal = 0, black = 0;
    GLuint sampler_plain = 0, sampler_shadow = 0, sampler_linear = 0;
    GLuint quad_vbo = 0, vao = 0;
    GLuint attached[8] = {};
} g_t;

GLuint make_tex(GLenum internal, int w, int h, GLenum format, GLenum type, const void* px, GLenum filter) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kClampToEdge);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kClampToEdge);
    return t;
}

void free_targets() {
    for (auto& t : g_t.tex)
        for (GLuint& x : t)
            if (x) glDeleteTextures(1, &x), x = 0;
    for (GLuint& d : g_t.depth)
        if (d) glDeleteTextures(1, &d), d = 0;
    if (g_t.composite_ds) glDeleteTextures(1, &g_t.composite_ds), g_t.composite_ds = 0;
    memset(g_t.attached, 0, sizeof g_t.attached);
    g_t.w = g_t.h = 0;
}

void free_shadow() {
    if (g_t.shadow_depth) glDeleteTextures(1, &g_t.shadow_depth), g_t.shadow_depth = 0;
    for (GLuint& c : g_t.shadow_color)
        if (c) glDeleteTextures(1, &c), c = 0;
    g_t.shadow_res = 0;
}

// the shadow map's size (the pack's x Shadow Quality)
int shadow_size() { return std::clamp((int)lroundf(g_pack->shadow_res * g_vs.shadow_q), 64, 8192); }

void ensure_targets(int w, int h) {
    const Pack& p = *g_pack;
    bool same = g_t.w == w && g_t.h == h;
    for (int i = 0; i < 8 && same; i++) same = g_t.fmt[i] == p.buffers[i].format;
    if (!same) {
        free_targets();
        g_t.w = w, g_t.h = h;
        for (int i = 0; i < 8; i++) {
            g_t.fmt[i] = p.buffers[i].format;
            for (int k = 0; k < 2; k++) g_t.tex[i][k] = make_tex(g_t.fmt[i], w, h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, GL_LINEAR);
            g_t.cur[i] = 0;
        }
        for (GLuint& d : g_t.depth) d = make_tex(kDepth24Stencil8, w, h, kDepthStencil, kUnsignedInt248, nullptr, GL_NEAREST);
        if (!g_t.fbo) glGenFramebuffers_(1, &g_t.fbo);
        if (!g_t.composite_fbo) glGenFramebuffers_(1, &g_t.composite_fbo);
        glBindFramebuffer_(kFramebuffer, g_t.fbo);
        glFramebufferTexture2D_(kFramebuffer, kDepthStencilAttachment, GL_TEXTURE_2D, g_t.depth[0], 0);
        g_t.composite_ds = make_tex(kDepth24Stencil8, w, h, kDepthStencil, kUnsignedInt248, nullptr, GL_NEAREST);
        glBindFramebuffer_(kFramebuffer, g_t.composite_fbo);
        glFramebufferTexture2D_(kFramebuffer, kDepthStencilAttachment, GL_TEXTURE_2D, g_t.composite_ds, 0);
        LOGI("shaders: G-buffers %dx%d", w, h);
    }
    int want_shadow = shadow_size();
    if (g_t.shadow_res != want_shadow && p.shadow.present) {
        free_shadow();
        g_t.shadow_res = want_shadow;
        int r = want_shadow;
        g_t.shadow_depth = make_tex(kDepthComponent24, r, r, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr, GL_LINEAR);
        for (GLuint& c : g_t.shadow_color) c = make_tex(0x8058, r, r, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, GL_LINEAR);
        if (!g_t.shadow_fbo) glGenFramebuffers_(1, &g_t.shadow_fbo);
        glBindFramebuffer_(kFramebuffer, g_t.shadow_fbo);
        glFramebufferTexture2D_(kFramebuffer, kDepthAttachment, GL_TEXTURE_2D, g_t.shadow_depth, 0);
        glFramebufferTexture2D_(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D, g_t.shadow_color[0], 0);
        glFramebufferTexture2D_(kFramebuffer, kColorAttachment0 + 1, GL_TEXTURE_2D, g_t.shadow_color[1], 0);
    }
    if (!g_t.noise) {
        int n = p.noise_res;
        std::vector<uint8_t> px(size_t(n) * n * 4);
        std::mt19937 rng(12345);
        for (auto& b : px) b = (uint8_t)(rng() & 255);
        g_t.noise = make_tex(0x8058, n, n, GL_RGBA, GL_UNSIGNED_BYTE, px.data(), GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        uint8_t w4[4] = {255, 255, 255, 255}, nrm[4] = {128, 128, 255, 255}, blk[4] = {0, 0, 0, 0};
        g_t.white = make_tex(0x8058, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, w4, GL_NEAREST);
        g_t.flat_normal = make_tex(0x8058, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, nrm, GL_NEAREST);
        g_t.black = make_tex(0x8058, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, blk, GL_NEAREST);
        glGenSamplers_(1, &g_t.sampler_plain);
        glSamplerParameteri_(g_t.sampler_plain, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glSamplerParameteri_(g_t.sampler_plain, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glSamplerParameteri_(g_t.sampler_plain, GL_TEXTURE_WRAP_S, kClampToEdge);
        glSamplerParameteri_(g_t.sampler_plain, GL_TEXTURE_WRAP_T, kClampToEdge);
        glGenSamplers_(1, &g_t.sampler_linear);
        glSamplerParameteri_(g_t.sampler_linear, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri_(g_t.sampler_linear, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glSamplerParameteri_(g_t.sampler_linear, GL_TEXTURE_WRAP_S, kClampToEdge);
        glSamplerParameteri_(g_t.sampler_linear, GL_TEXTURE_WRAP_T, kClampToEdge);
        glGenSamplers_(1, &g_t.sampler_shadow);
        glSamplerParameteri_(g_t.sampler_shadow, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri_(g_t.sampler_shadow, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glSamplerParameteri_(g_t.sampler_shadow, GL_TEXTURE_WRAP_S, kClampToEdge);
        glSamplerParameteri_(g_t.sampler_shadow, GL_TEXTURE_WRAP_T, kClampToEdge);
        glSamplerParameteri_(g_t.sampler_shadow, kTextureCompareMode, kCompareRefToTexture);
        glSamplerParameteri_(g_t.sampler_shadow, kTextureCompareFunc, GL_LEQUAL);
        const float quad[] = {0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1};
        glGenBuffers_(1, &g_t.quad_vbo);
        glBindBuffer_(kArrayBuffer, g_t.quad_vbo);
        glBufferData_(kArrayBuffer, sizeof quad, quad, kStaticDraw);
        glGenVertexArrays_(1, &g_t.vao);
    }
    for (auto& c : g_pack->custom) {
        if (c.tex || c.rgba.empty()) continue;
        c.tex = make_tex(0x8058, c.w, c.h, GL_RGBA, GL_UNSIGNED_BYTE, c.rgba.data(), GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        c.rgba.clear();
        c.rgba.shrink_to_fit();
    }
}

// ============================================================================ frame state
enum Phase { kNone, kWorld, kDone };
struct FrameState {
    Phase phase = kNone;
    uint64_t frame = 0;
    bool terrain_seen = false, translucent_seen = false, hand = false;
    // the game draws the clouds before the translucent terrain (Java: after it, not in depthtex1): depthtex1 is taken
    // before the clouds (clouds_early), and again when the translucent terrain starts only if the terrain or entities
    // were drawn after the clouds (then the clouds are in it, as before)
    bool clouds_early = false, opaque_after_clouds = false;
    bool view_set = false;
    Mat4 view = Mat4::identity(), proj = Mat4::identity(), prev_view = Mat4::identity(), prev_proj = Mat4::identity();
    Mat4 shadow_view = Mat4::identity(), shadow_proj = Mat4::identity();
    float cam[3] = {}, prev_cam[3] = {};
    bool cam_known = false;
    float fog_color[4] = {0.6f, 0.7f, 1, 1}, sky_color[3] = {0.5f, 0.7f, 1}, clear_color[4] = {0, 0, 0, 1};
    float near_ = 0.05f, far_ = 256;  // the projection's planes (far: very far, for the game's sky)
    float render_distance = 128;      // blocks (the game's RENDER_DISTANCE)
    double start_time = 0, last_time = 0, frame_time = 0.016;
    float wetness = 0, center_depth = 0.5f;
    bool cam_marked = false;  // the camera's entity was drawn (its pixels marked in the stencil, see mark_camera_entity)
} g_f;

// The camera's entity (the player, seen in third person) moves with the camera: the packs' motion blur and temporal
// filters reproject every pixel with the camera's motion (gbufferPrevious*), which smears and speckles it while the
// view turns. Its pixels get this stencil bit (the later draws in front of it clear it), and the passes that use the
// previous matrices are drawn twice: elsewhere as they are, on it with the previous matrices = the current ones.
constexpr GLint kCamStencil = 0x80;
std::atomic<bool> g_draw_cam_entity{false};  // the entity drawn now is the camera's (set_draw_entity)

// the stencil written by the draws (after the camera's entity was drawn): the bit set (cam) or cleared
void mark_camera_entity(bool cam) {
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, cam ? kCamStencil : 0, kCamStencil);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glStencilMask(kCamStencil);
}

GLint g_game_vp[4] = {};     // the game's viewport (while the world is drawn into the G-buffers: scaled to them)
GLuint g_game_fb = 0;        // the framebuffer the game has bound (draw)
GLuint g_main_fb = 0;        // the game's framebuffer with the window's renderbuffer
GLuint g_drawable_rb = 0;
int g_drawable_w = 0, g_drawable_h = 0;
bool g_ready = false;

// the game's viewport (in the drawable's pixels) in the G-buffers' (Render Quality)
void scaled_viewport(const GLint* vp) {
    float sx = g_drawable_w > 0 ? (float)g_t.w / g_drawable_w : 1, sy = g_drawable_h > 0 ? (float)g_t.h / g_drawable_h : 1;
    glViewport((GLint)lroundf(vp[0] * sx), (GLint)lroundf(vp[1] * sy), (GLsizei)lroundf(vp[2] * sx), (GLsizei)lroundf(vp[3] * sy));
}
int g_debug_passes = -1;
bool g_debug_norain = false;  // debug: composite passes run (shader_debug.txt "passes=N"; -1: all)
int g_trace_frames = 0;  // debug: draws of the next frames logged (shader_debug.txt "trace=N")
int discard_mode(const struct GameProgram& gp);
// the weather (seen in the frame's draws)
bool g_weather_drawn = false;
float g_rain_alpha = 0, g_rain_alpha_shown = 0;
float g_rain = 0;
float g_fog_control[2] = {0, 1};
bool g_in_water = false;
float g_cam_game[3] = {};
// the latest colours made by LightTexture::getColorForUV (entities: TILE_LIGHT_COLOR) and their block / sky light
struct LightColor {
    int block = -1, sky = -1;
    float color[4] = {};
};
constexpr unsigned kLightColors = 64;
LightColor g_light_colors[kLightColors];
unsigned g_light_next = 0;
std::mutex g_light_mutex;
// the block and sky light (0..15) the game made the colour from (the newest match; false: not found)
bool light_of_color(const float c[4], int* block, int* sky) {
    std::lock_guard<std::mutex> l(g_light_mutex);
    unsigned n = std::min(g_light_next, kLightColors);
    for (unsigned i = 1; i <= n; i++) {
        const LightColor& e = g_light_colors[(g_light_next - i) % kLightColors];
        if (fabsf(e.color[0] - c[0]) < 1e-4f && fabsf(e.color[1] - c[1]) < 1e-4f && fabsf(e.color[2] - c[2]) < 1e-4f) {
            *block = e.block, *sky = e.sky;
            return true;
        }
    }
    return false;
}
bool g_cam_game_set = false;
uint64_t g_last_terrain_frame = 0;


// time of day (from the game's Dimension::getTimeOfDay)
std::atomic<int> g_world_time{6000};
std::atomic<float> g_celestial{0.0f};
std::atomic<bool> g_nether{false};
uint64_t g_tod_frame = 0;  // the frame Dimension::getTimeOfDay was last called in
std::atomic<int> g_dimension{-1};  // the camera entity's dimension (-1: not known)

// terrain draws of the frame (the shadow pass draws them again)
struct AttribState { GLint enabled = 0, size = 0, type = 0, normalized = 0, stride = 0, buffer = 0; uintptr_t offset = 0; };
struct TerrainDraw {
    GLuint game_program;
    AttribState pos, color, tex0, tex1;
    GLuint ebo;
    GLenum mode, type;
    GLsizei count;
    uintptr_t indices;
    float origin[4];
    GLuint atlas;
    GLuint extra_vbo;
    bool cutout;
};
std::vector<TerrainDraw> g_terrain_draws;

// ============================================================================ per-vertex extras (terrain)
// normal (3 bytes) + pad, tangent (4 bytes, w = handedness), mid texture coordinate (2 floats),
// mc_Entity: the block id and (as OptiFine) 1 for liquids
struct Extra { int8_t n[4]; int8_t t[4]; float mid[2]; float id[2]; uint8_t color[4]; };
struct ExtraKey {
    GLuint vbo;
    uintptr_t offset;
    GLint stride;
    int default_id;  // the block of the vertices whose texture is not known (water layer: water)
    GLuint colormap;  // leaves (SEASONS): the foliage colour map the vertex colours point into (0: none)
    bool operator==(const ExtraKey& o) const {
        return vbo == o.vbo && offset == o.offset && stride == o.stride && default_id == o.default_id && colormap == o.colormap;
    }
};
struct ExtraKeyHash {
    size_t operator()(const ExtraKey& k) const { return k.vbo * 1000003u ^ (size_t)k.offset * 31 ^ (size_t)k.stride ^ (size_t)k.default_id << 20; }
};
struct ExtraBuf {
    GLuint vbo = 0;
    uint64_t gen = 0;
    int vertices = 0;
    uint64_t version = 0;        // made again: a new version
    std::vector<uint8_t> water;  // per vertex: 1 = water (empty: none)
    std::vector<uint32_t> unknown_cell;  // per vertex: the atlas cell of a texture not known (~0: known)
};
ExtraBuf* g_last_extras = nullptr;  // the extras of the last get_extras
uint64_t g_extras_version = 0;
std::unordered_map<ExtraKey, ExtraBuf, ExtraKeyHash> g_extras;
std::unordered_map<GLuint, uint64_t> g_buffer_gen;
uint64_t g_gen_counter = 1;

// the texture atlas: block textures found in it (cells in pixels) -> Java block ids
struct AtlasCell { int x, y, w, h; float id; };
struct Atlas {
    GLuint tex = 0;
    int w = 0, h = 0;
    std::vector<AtlasCell> cells;
    bool scanned = false;
    // the water's cells (16 pixels; animated, so not found by their picture): learnt from the water layer's draws
    std::set<uint32_t> water;
} g_atlas;
bool g_water_learnt = false;  // a new water cell: the extras are made again
std::unordered_map<GLuint, Atlas> g_atlas_cache;  // textures used by the terrain (scanned once)

float decode_component(const uint8_t* p, GLenum type, bool normalized) {
    switch (type) {
    case kFloat: { float v; memcpy(&v, p, 4); return v; }
    case 0x1402: { int16_t v; memcpy(&v, p, 2); return normalized ? std::max(-1.0f, v / 32767.0f) : (float)v; }   // SHORT
    case 0x1403: { uint16_t v; memcpy(&v, p, 2); return normalized ? v / 65535.0f : (float)v; }                   // UNSIGNED_SHORT
    case 0x1400: { int8_t v = (int8_t)*p; return normalized ? std::max(-1.0f, v / 127.0f) : (float)v; }          // BYTE
    case 0x1401: return normalized ? *p / 255.0f : (float)*p;                                                     // UNSIGNED_BYTE
    case kHalfFloat: {
        uint16_t h;
        memcpy(&h, p, 2);
        uint32_t s = (h & 0x8000u) << 16, e = (h >> 10) & 31, m = h & 1023, f;
        if (e == 0) f = s | (m ? 0 : 0);
        else if (e == 31) f = s | 0x7f800000u | (m << 13);
        else f = s | ((e + 112) << 23) | (m << 13);
        float v;
        memcpy(&v, &f, 4);
        return v;
    }
    default: return 0;
    }
}
int type_size(GLenum t) {
    switch (t) {
    case kFloat: case 0x1404: case 0x1405: return 4;
    case 0x1402: case 0x1403: case kHalfFloat: return 2;
    default: return 1;
    }
}

uint32_t cell_key(float u, float v) {
    return (uint32_t)std::clamp((int)(u * g_atlas.w / 16), 0, 65535) << 16 | (uint32_t)std::clamp((int)(v * g_atlas.h / 16), 0, 65535);
}
float block_id_at(float u, float v) {
    if (g_atlas.w <= 0) return 0;
    float px = u * g_atlas.w, py = v * g_atlas.h;
    for (auto& c : g_atlas.cells)
        if (px >= c.x && px < c.x + c.w && py >= c.y && py < c.y + c.h) return c.id;
    if (!g_atlas.water.empty() && g_atlas.water.count(cell_key(u, v))) return 9;
    return 0;
}

// the top half of a double plant: tall grass (31) whose vertices all count as "above the middle" (mc_midTexCoord):
// packs sway the top of grass, and this half follows the bottom half's top (the same sway, no gap)
constexpr float kDoubleTop = 1175;
struct Colormap { int w = 0, h = 0; std::vector<uint8_t> px; };
std::unordered_map<GLuint, Colormap> g_colormaps;
const Colormap* colormap(GLuint tex);

GLuint get_extras(const AttribState& pos, const AttribState& tex, int default_id, const AttribState* col = nullptr,
                  GLuint cmap = 0, bool far_ = false) {
    if (!pos.buffer || pos.stride <= 0) return 0;
    if (tex.buffer && tex.buffer != pos.buffer) return 0;
    if (col && (col->buffer != pos.buffer || col->type != 0x1401)) col = nullptr, cmap = 0;  // (colours: 4 unsigned bytes)
    if (!col) cmap = 0;
    ExtraKey key{(GLuint)pos.buffer, pos.offset, pos.stride, default_id, cmap};
    auto gi = g_buffer_gen.find(pos.buffer);
    uint64_t gen = gi == g_buffer_gen.end() ? 0 : gi->second;
    auto& e = g_extras[key];
    if (e.vbo && e.gen == gen && e.gen != 0) return g_last_extras = &e, e.vbo;
    if (e.vbo && gi == g_buffer_gen.end() && e.gen == 0) return g_last_extras = &e, e.vbo;
    GLint size = 0;
    if (glGetNamedBufferParameteriv_) glGetNamedBufferParameteriv_(pos.buffer, kBufferSize, &size);
    if (size <= 0 || (uintptr_t)size <= pos.offset) return 0;
    std::vector<uint8_t> data(size - pos.offset);
    glGetNamedBufferSubData_(pos.buffer, pos.offset, (GLsizeiptr)data.size(), data.data());
    int n = (int)(data.size() / pos.stride);
    std::vector<Extra> ex(n);
    int psz = type_size(pos.type), tsz = tex.buffer ? type_size(tex.type) : 0;
    intptr_t toff = tex.buffer ? (intptr_t)tex.offset - (intptr_t)pos.offset : 0;
    auto P = [&](int i, float* o) {
        const uint8_t* b = &data[size_t(i) * pos.stride];
        for (int c = 0; c < 3; c++) o[c] = decode_component(b + c * psz, pos.type, pos.normalized != 0);
    };
    auto T = [&](int i, float* o) {
        o[0] = o[1] = 0;
        if (!tex.buffer) return;
        intptr_t at = intptr_t(i) * pos.stride + toff;
        if (at < 0 || size_t(at) + 2 * tsz > data.size()) return;
        for (int c = 0; c < 2; c++) o[c] = decode_component(&data[at + c * tsz], tex.type, tex.normalized != 0);
    };
    const Colormap* cm = cmap ? colormap(cmap) : nullptr;
    std::vector<uint32_t> cells(n, ~0u);
    intptr_t coff = col ? (intptr_t)col->offset - (intptr_t)pos.offset : 0;
    auto leaf_color = [&](int i, uint8_t* out) {
        out[0] = out[1] = out[2] = out[3] = 255;
        if (!cm || cm->px.empty()) return;
        intptr_t at = intptr_t(i) * pos.stride + coff;
        if (at < 0 || size_t(at) + 4 > data.size()) return;
        const uint8_t* c = &data[at];
        // the game's SEASONS shader: rgb *= mix(1, colormap(c.rg) * 2, c.b) * c.a
        int x = std::clamp((int)(c[0] / 255.0f * cm->w), 0, cm->w - 1), y = std::clamp((int)(c[1] / 255.0f * cm->h), 0, cm->h - 1);
        const uint8_t* m = &cm->px[(size_t(y) * cm->w + x) * 4];
        float b = far_ ? 1.0f : c[2] / 255.0f, a = c[3] / 255.0f;
        for (int k = 0; k < 3; k++) {
            float f = (1 - b) + b * (m[k] / 255.0f * 2);
            out[k] = (uint8_t)std::clamp(lroundf(f * a * 255), 0L, 255L);
        }
    };
    for (int q = 0; q + 3 < n; q += 4) {
        float p[4][3], uv[4][2];
        for (int k = 0; k < 4; k++) P(q + k, p[k]), T(q + k, uv[k]);
        float e1[3], e2[3], nn[3];
        for (int c = 0; c < 3; c++) e1[c] = p[1][c] - p[0][c], e2[c] = p[2][c] - p[0][c];
        nn[0] = e1[1] * e2[2] - e1[2] * e2[1];
        nn[1] = e1[2] * e2[0] - e1[0] * e2[2];
        nn[2] = e1[0] * e2[1] - e1[1] * e2[0];
        float l = sqrtf(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
        if (l < 1e-12f) {  // a degenerate first triangle: the other one
            for (int c = 0; c < 3; c++) e1[c] = p[2][c] - p[0][c], e2[c] = p[3][c] - p[0][c];
            nn[0] = e1[1] * e2[2] - e1[2] * e2[1];
            nn[1] = e1[2] * e2[0] - e1[0] * e2[2];
            nn[2] = e1[0] * e2[1] - e1[1] * e2[0];
            l = sqrtf(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
        }
        if (l < 1e-12f) nn[0] = 0, nn[1] = 1, nn[2] = 0, l = 1;
        for (float& c : nn) c /= l;
        // tangent: the direction of increasing u
        float du1 = uv[1][0] - uv[0][0], dv1 = uv[1][1] - uv[0][1], du2 = uv[2][0] - uv[0][0], dv2 = uv[2][1] - uv[0][1];
        float det = du1 * dv2 - du2 * dv1, t[3] = {1, 0, 0}, w = 1;
        if (fabsf(det) > 1e-12f) {
            float r = 1 / det;
            float b[3];
            for (int c = 0; c < 3; c++) {
                t[c] = (e1[c] * dv2 - e2[c] * dv1) * r;
                b[c] = (e2[c] * du1 - e1[c] * du2) * r;
            }
            float d = t[0] * nn[0] + t[1] * nn[1] + t[2] * nn[2];
            for (int c = 0; c < 3; c++) t[c] -= nn[c] * d;
            float cx = nn[1] * t[2] - nn[2] * t[1], cy = nn[2] * t[0] - nn[0] * t[2], cz = nn[0] * t[1] - nn[1] * t[0];
            w = (cx * b[0] + cy * b[1] + cz * b[2]) < 0 ? -1.0f : 1.0f;
        }
        float tl = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
        if (tl < 1e-12f) t[0] = 1, t[1] = t[2] = 0, tl = 1;
        float mid[2] = {(uv[0][0] + uv[1][0] + uv[2][0] + uv[3][0]) / 4, (uv[0][1] + uv[1][1] + uv[2][1] + uv[3][1]) / 4};
        float id = block_id_at(mid[0], mid[1]);
        if (id == kDoubleTop) {
            id = 31;
            float top = std::max(std::max(uv[0][1], uv[1][1]), std::max(uv[2][1], uv[3][1]));
            mid[1] = top + 1.0f;  // (every vertex: texcoord.t < mid.t)
        }
        uint32_t unknown = id == 0 && g_atlas.w > 0 ? cell_key(mid[0], mid[1]) : ~0u;
        for (int k = 0; k < 4; k++) cells[q + k] = unknown;
        if (id == 0) id = (float)default_id;
        for (int k = 0; k < 4; k++) {
            Extra& x = ex[q + k];
            for (int c = 0; c < 3; c++) x.n[c] = (int8_t)lroundf(nn[c] * 127), x.t[c] = (int8_t)lroundf(t[c] / tl * 127);
            x.n[3] = 0;
            x.t[3] = (int8_t)(w * 127);
            x.mid[0] = mid[0], x.mid[1] = mid[1];
            x.id[0] = id;
            x.id[1] = (id >= 8 && id <= 11) ? 1.0f : 0.0f;
            leaf_color(q + k, x.color);
        }
    }
    if (!e.vbo) glGenBuffers_(1, &e.vbo);
    GLint prev = 0;
    glGetIntegerv(kArrayBufferBinding, &prev);
    glBindBuffer_(kArrayBuffer, e.vbo);
    glBufferData_(kArrayBuffer, GLsizeiptr(ex.size() * sizeof(Extra)), ex.data(), kStaticDraw);
    glBindBuffer_(kArrayBuffer, prev);
    e.gen = gen ? gen : 0;
    e.vertices = n;
    e.version = ++g_extras_version;
    e.unknown_cell = std::move(cells);
    e.water.clear();
    for (int i = 0; i < n; i++)
        if (ex[i].id[1] == 1.0f && ex[i].id[0] == 9.0f) {
            if (e.water.empty()) e.water.assign(n, 0);
            e.water[i] = 1;
        }
    g_last_extras = &e;
    return e.vbo;
}

void scan_atlas(GLuint tex);

// ============================================================================ helpers
AttribState read_attrib(GLint loc) {
    AttribState a;
    if (loc < 0) return a;
    glGetVertexAttribiv_(loc, kAttribEnabled, &a.enabled);
    if (!a.enabled) return a;
    glGetVertexAttribiv_(loc, kAttribSize, &a.size);
    glGetVertexAttribiv_(loc, kAttribType, &a.type);
    glGetVertexAttribiv_(loc, kAttribNormalized, &a.normalized);
    glGetVertexAttribiv_(loc, kAttribStride, &a.stride);
    glGetVertexAttribiv_(loc, kAttribBuffer, &a.buffer);
    void* p = nullptr;
    glGetVertexAttribPointerv_(loc, kAttribPointer, &p);
    a.offset = (uintptr_t)p;
    if (a.stride == 0) a.stride = a.size * type_size(a.type);
    return a;
}

void get_uniform(GLuint prog, GLint loc, float* out, int n) {
    for (int i = 0; i < n; i++) out[i] = 0;
    if (loc >= 0) glGetUniformfv_(prog, loc, out);
}

void bind_tex(int unit, GLuint tex, GLuint sampler = 0) {
    glActiveTexture_(kTexture0 + unit);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler_(unit, sampler);
}

// sets a uniform of whatever type the program declares
void set_f(const Program& p, const char* name, float a, float b = 0, float c = 0, float d = 0) {
    auto it = p.uniforms.find(name);
    if (it == p.uniforms.end()) return;
    GLint l = it->second.loc;
    switch (it->second.type) {
    case kFloat: glUniform1f_(l, a); break;
    case 0x8B50: glUniform2f_(l, a, b); break;
    case 0x8B51: glUniform3f_(l, a, b, c); break;
    case 0x8B52: glUniform4f_(l, a, b, c, d); break;
    case kInt: case kBool: glUniform1i_(l, (GLint)lroundf(a)); break;
    case kIntVec2: glUniform2i_(l, (GLint)lroundf(a), (GLint)lroundf(b)); break;
    case kIntVec3: glUniform3i_(l, (GLint)lroundf(a), (GLint)lroundf(b), (GLint)lroundf(c)); break;
    default: break;
    }
}
void set_m(const Program& p, const char* name, const Mat4& m) {
    GLint l = p.loc(name);
    if (l >= 0) glUniformMatrix4fv_(l, 1, GL_FALSE, m.m);
}

// ============================================================================ celestial
struct Celestial {
    float sun[3], moon[3], up[3], shadow_light[3];
    float sun_angle, shadow_angle;
};
Celestial celestial() {
    Celestial c{};
    float angle = g_celestial.load();
    float rot = g_pack ? g_pack->sun_path_rotation : 0;
    Mat4 m = g_f.view * rotate(-90, 0, 1, 0) * rotate(rot, 0, 0, 1) * rotate(angle * 360, 1, 0, 0);
    auto s = mul(m, 0, 100, 0, 0);
    auto u = mul(g_f.view, 0, 100, 0, 0);
    for (int i = 0; i < 3; i++) c.sun[i] = s[i], c.moon[i] = -s[i], c.up[i] = u[i];
    c.sun_angle = angle < 0.75f ? angle + 0.25f : angle - 0.75f;
    bool day = c.sun_angle <= 0.5f;
    c.shadow_angle = day ? c.sun_angle : c.sun_angle - 0.5f;
    for (int i = 0; i < 3; i++) c.shadow_light[i] = day ? c.sun[i] : c.moon[i];
    return c;
}

void make_shadow_matrices() {
    const Pack& p = *g_pack;
    float angle = g_celestial.load();
    float sun_angle = angle < 0.75f ? angle + 0.25f : angle - 0.75f;
    float a = angle * -360.0f;
    float interval = 0;  // (OptiFine: shadowAngleInterval, 0 by default)
    Mat4 m = translate(0, 0, -100) * rotate(90, 1, 0, 0);
    if (sun_angle <= 0.5f) m = m * rotate(a - interval, 0, 0, 1) * rotate(p.sun_path_rotation, 1, 0, 0);
    else m = m * rotate(a + 180 - interval, 0, 0, 1) * rotate(p.sun_path_rotation, 1, 0, 0);
    float ti = p.shadow_interval > 0 ? p.shadow_interval : 2, t2 = ti / 2;
    m = m * translate(fmodf(g_f.cam[0], ti) - t2, fmodf(g_f.cam[1], ti) - t2, fmodf(g_f.cam[2], ti) - t2);
    g_f.shadow_view = m;
    float hp = p.shadow_distance;
    g_f.shadow_proj = ortho(-hp, hp, -hp, hp, 0.05f, 256.0f);
}

// the frame's uniforms (once per program and frame)
std::atomic<int> g_held_block{0};  // the block the player holds (ItemInHandRenderer::tick)
std::atomic<int> g_held_light{0};  // its light (0..15)
std::atomic<int> g_eye{0};         // the camera in water (1) / lava (2)

void set_frame_uniforms(Program& pr, bool gbuffers) {
    if (pr.frame_set == g_f.frame) return;
    pr.frame_set = g_f.frame;
    const Pack& pk = *g_pack;
    Celestial c = celestial();
    set_m(pr, "gbufferModelView", g_f.view);
    set_m(pr, "gbufferModelViewInverse", inverse(g_f.view));
    set_m(pr, "gbufferProjection", g_f.proj);
    set_m(pr, "gbufferProjectionInverse", inverse(g_f.proj));
    set_m(pr, "gbufferPreviousModelView", g_f.prev_view);
    set_m(pr, "gbufferPreviousProjection", g_f.prev_proj);
    set_m(pr, "shadowModelView", g_f.shadow_view);
    set_m(pr, "shadowModelViewInverse", inverse(g_f.shadow_view));
    set_m(pr, "shadowProjection", g_f.shadow_proj);
    set_m(pr, "shadowProjectionInverse", inverse(g_f.shadow_proj));
    set_f(pr, "cameraPosition", g_f.cam[0], g_f.cam[1], g_f.cam[2]);
    set_f(pr, "previousCameraPosition", g_f.prev_cam[0], g_f.prev_cam[1], g_f.prev_cam[2]);
    set_f(pr, "eyeAltitude", g_f.cam[1]);
    set_f(pr, "sunPosition", c.sun[0], c.sun[1], c.sun[2]);
    set_f(pr, "moonPosition", c.moon[0], c.moon[1], c.moon[2]);
    set_f(pr, "upPosition", c.up[0], c.up[1], c.up[2]);
    set_f(pr, "shadowLightPosition", c.shadow_light[0], c.shadow_light[1], c.shadow_light[2]);
    set_f(pr, "sunAngle", c.sun_angle);
    set_f(pr, "shadowAngle", c.shadow_angle);
    int wt = g_world_time.load();
    set_f(pr, "worldTime", (float)(wt % 24000));
    set_f(pr, "worldDay", (float)(wt / 24000));
    set_f(pr, "moonPhase", (float)((wt / 24000) % 8));
    set_f(pr, "frameTimeCounter", (float)fmod(g_f.last_time - g_f.start_time, 3600.0));
    set_f(pr, "frameTime", (float)g_f.frame_time);
    set_f(pr, "frameCounter", (float)(g_f.frame % 720720));
    set_f(pr, "rainStrength", g_vs.weather ? g_rain : 0.0f);
    set_f(pr, "wetness", g_vs.weather ? g_f.wetness : 0.0f);
    set_f(pr, "viewWidth", (float)g_t.w);
    set_f(pr, "viewHeight", (float)g_t.h);
    set_f(pr, "aspectRatio", g_t.h ? (float)g_t.w / g_t.h : 1);
    set_f(pr, "near", g_f.near_);
    // OptiFine: the render distance in blocks (not the projection's far plane: the game's is ~2500, and packs that test
    // "depth > 1.0 - near / far / far" for the sky, as KUDA, never found it in floats)
    set_f(pr, "far", g_f.render_distance > 0 ? g_f.render_distance : g_f.far_);
    set_f(pr, "fogColor", g_f.fog_color[0], g_f.fog_color[1], g_f.fog_color[2]);
    set_f(pr, "skyColor", g_f.sky_color[0], g_f.sky_color[1], g_f.sky_color[2]);
    set_f(pr, "fogMode", 9729);  // GL_LINEAR
    set_f(pr, "fogDensity", 0);
    set_f(pr, "eyeBrightness", 0, 240);
    set_f(pr, "eyeBrightnessSmooth", 0, 240);
    set_f(pr, "isEyeInWater", (float)g_eye.load());
    set_f(pr, "nightVision", 0);
    set_f(pr, "blindness", 0);
    set_f(pr, "screenBrightness", 0.5f);
    int held = g_held_block.load();
    set_f(pr, "heldItemId", (float)held);
    set_f(pr, "heldBlockLightValue", (float)g_held_light.load());
    set_f(pr, "emu_HandLight", pk.dynamic_hand_light && g_vs.hand_light ? (float)g_held_light.load() : 0.0f);
    set_f(pr, "heldItemId2", 0);
    set_f(pr, "heldBlockLightValue2", 0);
    set_f(pr, "centerDepthSmooth", g_f.center_depth);
    set_f(pr, "atlasSize", (float)g_atlas.w, (float)g_atlas.h);
    set_f(pr, "terrainTextureSize", (float)g_atlas.w, (float)g_atlas.h);
    set_f(pr, "terrainIconSize", 16);
    set_f(pr, "entityId", -1);
    set_f(pr, "blockEntityId", -1);
    set_f(pr, "entityColor", 0, 0, 0, 0);
    set_f(pr, "shadowMapResolution", (float)shadow_size());
    // samplers
    auto sampler = [&](const char* n, int unit) {
        GLint l = pr.loc(n);
        if (l >= 0) glUniform1i_(l, unit);
    };
    static const char* const bufnames[8][2] = {{"gcolor", "colortex0"}, {"gdepth", "colortex1"}, {"gnormal", "colortex2"},
                                               {"composite", "colortex3"}, {"gaux1", "colortex4"}, {"gaux2", "colortex5"},
                                               {"gaux3", "colortex6"}, {"gaux4", "colortex7"}};
    if (gbuffers) {
        sampler("normals", kUnitNormals);
        sampler("specular", kUnitSpecular);
        sampler("noisetex", kUnitNoiseG);
        sampler("shadow", kUnitShadow0G);
        sampler("watershadow", kUnitShadow0G);
        sampler("shadowtex0", kUnitShadow0G);
        sampler("shadowtex1", kUnitShadow1G);
        sampler("shadowcolor", kUnitShadowColor0G);
        sampler("shadowcolor0", kUnitShadowColor0G);
        sampler("shadowcolor1", kUnitShadowColor1G);
        sampler("depthtex0", kUnitDepthG);
        sampler("depthtex1", kUnitDepthG);
        sampler("depthtex2", kUnitDepthG);
        sampler("gdepthtex", kUnitDepthG);
        for (int i = 0; i < 8; i++) sampler(bufnames[i][0], kUnitColortexG + i), sampler(bufnames[i][1], kUnitColortexG + i);
    } else {
        for (int i = 0; i < 8; i++) sampler(bufnames[i][0], kUnitColortex + i), sampler(bufnames[i][1], kUnitColortex + i);
        sampler("depthtex0", kUnitDepth0);
        sampler("gdepthtex", kUnitDepth0);
        sampler("depthtex1", kUnitDepth0 + 1);
        sampler("depthtex2", kUnitDepth0 + 2);
        sampler("shadow", kUnitShadow0);
        sampler("watershadow", kUnitShadow0);
        sampler("shadowtex0", kUnitShadow0);
        sampler("shadowtex1", kUnitShadow1);
        sampler("shadowcolor", kUnitShadowColor0);
        sampler("shadowcolor0", kUnitShadowColor0);
        sampler("shadowcolor1", kUnitShadowColor1);
        sampler("noisetex", kUnitNoise);
    }
}

// the shadow textures as the program declares them (sampler2DShadow: compare)
GLuint shadow_sampler(const Program& pr, const char* const* names, int n) {
    for (int i = 0; i < n; i++) {
        auto it = pr.uniforms.find(names[i]);
        if (it != pr.uniforms.end()) return it->second.type == kSampler2DShadow ? g_t.sampler_shadow : g_t.sampler_linear;
    }
    return g_t.sampler_linear;
}

// ============================================================================ the pack's gbuffers programs (per game program)
struct ProgKey {
    int stage;
    GLuint game;
    bool operator==(const ProgKey& o) const { return stage == o.stage && game == o.game; }
};
struct ProgKeyHash { size_t operator()(const ProgKey& k) const { return k.game * 64 + k.stage; } };
std::unordered_map<ProgKey, std::unique_ptr<Program>, ProgKeyHash> g_gbuffer_progs, g_shadow_progs;
std::vector<std::unique_ptr<Program>> g_composite_progs;
std::unique_ptr<Program> g_final_prog;
bool g_composites_linked = false;

int resolve_stage(int st) {
    while (st >= 0 && !g_pack->gbuffers[st].present) st = kFallback[st];
    return st;
}

Program* gbuffers_program(int stage, GLuint game, const GameProgram& gp) {
    int st = resolve_stage(stage);
    if (st < 0) return nullptr;
    auto& slot = g_gbuffer_progs[{st, game}];
    if (!slot) {
        slot = std::make_unique<Program>();
        link_program(*slot, g_pack->gbuffers[st], gp.attr, kStageNames[st], true);
    }
    return slot->ok ? slot.get() : nullptr;
}

Program* shadow_program(GLuint game, const GameProgram& gp) {
    if (!g_pack->shadow.present) return nullptr;
    auto& slot = g_shadow_progs[{0, game}];
    if (!slot) {
        slot = std::make_unique<Program>();
        link_program(*slot, g_pack->shadow, gp.attr, "shadow", true);
    }
    return slot->ok ? slot.get() : nullptr;
}

void link_composites() {
    if (g_composites_linked) return;
    g_composites_linked = true;
    AttribLocs a;
    a.pos = 0;
    a.tex0 = 1;
    for (auto& [n, s] : g_pack->composites) {
        auto p = std::make_unique<Program>();
        link_program(*p, s, a, n);
        g_composite_progs.push_back(std::move(p));
    }
    if (g_pack->final_.present) {
        g_final_prog = std::make_unique<Program>();
        link_program(*g_final_prog, g_pack->final_, a, "final");
    }
}

void unload_pack() {
    for (auto& [k, p] : g_gbuffer_progs)
        if (p->id) glDeleteProgram_(p->id);
    for (auto& [k, p] : g_shadow_progs)
        if (p->id) glDeleteProgram_(p->id);
    for (auto& p : g_composite_progs)
        if (p->id) glDeleteProgram_(p->id);
    if (g_final_prog && g_final_prog->id) glDeleteProgram_(g_final_prog->id);
    g_gbuffer_progs.clear();
    g_shadow_progs.clear();
    g_composite_progs.clear();
    g_final_prog.reset();
    g_composites_linked = false;
    if (g_pack)
        for (auto& c : g_pack->custom)
            if (c.tex) glDeleteTextures(1, &c.tex);
    free_targets();
    free_shadow();
    if (g_t.noise) glDeleteTextures(1, &g_t.noise), g_t.noise = 0;
    g_pack.reset();
}

// the choice made in the settings: loaded here (render thread)
bool g_loaded_nether = false;
void apply_choice() {
    int want = g_selected.load();
    bool nether = g_nether.load();
    bool reload = g_reload_pack.exchange(false);
    reload |= g_reload_pack_vs.exchange(false);
    if (!reload && want == g_loaded_choice.load() && (nether == g_loaded_nether || !g_pack)) return;
    g_loaded_choice = want;
    g_loaded_nether = nether;
    unload_pack();
    fs::path p;
    {
        std::lock_guard lk(g_list_lock);
        if (want >= 0 && want < (int)g_packs.size()) p = g_packs[want];
    }
    if (!p.empty()) g_pack = load_pack(p, nether ? "world-1/" : "");
    if (!g_pack) LOGI("shaders: none in use");
}

GLuint custom_texture(const char* stage, const char* sampler_name, int buffer) {
    if (!g_pack) return 0;
    static const char* const bufnames[8][2] = {{"gcolor", "colortex0"}, {"gdepth", "colortex1"}, {"gnormal", "colortex2"},
                                               {"composite", "colortex3"}, {"gaux1", "colortex4"}, {"gaux2", "colortex5"},
                                               {"gaux3", "colortex6"}, {"gaux4", "colortex7"}};
    for (auto& c : g_pack->custom) {
        if (c.stage != stage || !c.tex) continue;
        if (sampler_name && c.sampler == sampler_name) return c.tex;
        if (buffer >= 0 && (c.sampler == bufnames[buffer][0] || c.sampler == bufnames[buffer][1])) return c.tex;
    }
    return 0;
}

// ============================================================================ the world part
void attach_gbuffers() {
    glBindFramebuffer_(kFramebuffer, g_t.fbo);
    for (int i = 0; i < 8; i++) {
        GLuint t = g_t.tex[i][g_t.cur[i]];
        if (g_t.attached[i] != t) {
            glFramebufferTexture2D_(kFramebuffer, kColorAttachment0 + i, GL_TEXTURE_2D, t, 0);
            g_t.attached[i] = t;
        }
    }
}

void clear_water_draws();
void draw_deferred_water();

void begin_world() {
    // the nether: the camera entity's dimension; not known: the overworld's Dimension::getTimeOfDay (the sky, the sun)
    // is not called there (it is after going through a portal: the overworld stays loaded)
    int dim = g_dimension.load();
    bool nether = dim >= 0 ? dim == 1 : g_f.frame > g_tod_frame + 30;
    if (nether != g_nether.load()) {
        g_nether = nether;
        LOGI("shaders: %s", nether ? "the nether (no sky)" : "the overworld");
    }
    refresh_video_settings();
    apply_choice();
    if (!g_pack) return;
    // (Render Quality: the G-buffers' size; the game's viewport is scaled while the world is drawn, see on_viewport)
    int w = std::max(1, (int)lroundf(g_drawable_w * g_vs.render_q)), h = std::max(1, (int)lroundf(g_drawable_h * g_vs.render_q));
    if (g_drawable_w <= 0 || g_drawable_h <= 0) return;
    ensure_targets(w, h);
    g_f.phase = kWorld;
    glGetIntegerv(GL_VIEWPORT, g_game_vp);
    if (g_t.w != g_drawable_w || g_t.h != g_drawable_h) scaled_viewport(g_game_vp);
    g_f.terrain_seen = g_f.translucent_seen = g_f.hand = g_f.cam_marked = false;
    g_f.clouds_early = g_f.opaque_after_clouds = false;
    g_terrain_draws.clear();
    clear_water_draws();
    double now = seconds_now();
    if (g_f.start_time == 0) g_f.start_time = g_f.last_time = now - 0.016;
    g_f.frame_time = std::clamp(now - g_f.last_time, 0.001, 0.25);  // (packs divide by it: never 0)
    g_f.last_time = now;
    glGetFloatv(GL_COLOR_CLEAR_VALUE, g_f.clear_color);
    // clear the G-buffers
    attach_gbuffers();
    glPushAttrib(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_SCISSOR_BIT | GL_STENCIL_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glDepthMask(1);
    for (int i = 0; i < 8; i++) {
        const BufferSetup& b = g_pack->buffers[i];
        if (!b.clear) continue;
        GLenum db = kColorAttachment0 + i;
        glDrawBuffers_(1, &db);
        if (i == 0) glClearColor(g_f.fog_color[0], g_f.fog_color[1], g_f.fog_color[2], 1);
        else glClearColor(b.clear_color[0], b.clear_color[1], b.clear_color[2], b.clear_color[3]);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glClearDepth(1);
    glClearStencil(0);
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    // the shadow map of this frame (the entities are drawn into it with their own draws, the terrain at the end)
    if (g_cam_game_set) memcpy(g_f.cam, g_cam_game, 12);
    make_shadow_matrices();
    if (g_pack->shadow.present && g_t.shadow_fbo) {
        glBindFramebuffer_(kFramebuffer, g_t.shadow_fbo);
        GLenum dbs[2] = {kColorAttachment0, kColorAttachment0 + 1};
        glDrawBuffers_(2, dbs);
        glClearColor(1, 1, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBindFramebuffer_(kFramebuffer, g_t.fbo);
    }
    glPopAttrib();
    GLenum db = kColorAttachment0;
    glDrawBuffers_(1, &db);
}

void copy_depth(int to) {
    glCopyImageSubData_(g_t.depth[0], GL_TEXTURE_2D, 0, 0, 0, 0, g_t.depth[to], GL_TEXTURE_2D, 0, 0, 0, 0, g_t.w, g_t.h, 1);
}
// depthtex1 (the depth without the translucent terrain): when it starts (see FrameState::clouds_early)
void copy_depth1() {
    if (!g_f.clouds_early || g_f.opaque_after_clouds) copy_depth(1);
}

void set_draw_buffers(const std::vector<int>& bufs) {
    GLenum db[8];
    int n = 0;
    for (int b : bufs)
        if (b >= 0 && b < 8 && n < 8) db[n++] = kColorAttachment0 + b;
    if (!n) db[n++] = kColorAttachment0;
    glDrawBuffers_(n, db);
}

void fixed_matrices(const Mat4& mv, const Mat4& p, bool lightmap_matrix) {
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(p.m);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(mv.m);
    glMatrixMode(GL_TEXTURE);
    glActiveTexture_(kTexture0);
    glLoadIdentity();
    glActiveTexture_(kTexture0 + 1);
    if (lightmap_matrix) {  // OptiFine: lightmap coordinates 0..240 -> (x + 8) / 256
        Mat4 t = translate(8.0f / 256, 8.0f / 256, 0) * scale(1.0f / 256, 1.0f / 256, 1);
        glLoadMatrixf(t.m);
    } else {
        glLoadIdentity();
    }
    glMatrixMode(GL_MODELVIEW);
}

void draw_composites();

void end_world() {
    if (g_f.phase != kWorld) return;
    g_f.phase = kDone;
    static double last_log = 0;
    if (g_f.last_time - last_log > 5) {
        last_log = g_f.last_time;
        LOGI("shaders: frame %llu: %zu terrain draws, time %d (sun %.3f), camera %.1f %.1f %.1f (%s), near %.3f far %.1f (render distance %.1f), "
             "fog %.2f %.2f %.2f (%.2f..%.2f), sky %.2f %.2f %.2f, rain %.2f (%.2f), held light %d",
             (unsigned long long)g_f.frame, g_terrain_draws.size(), g_world_time.load(), g_celestial.load(), g_f.cam[0], g_f.cam[1],
             g_f.cam[2], g_f.cam_known ? "known" : "unknown", g_f.near_, g_f.far_, g_f.render_distance, g_f.fog_color[0], g_f.fog_color[1], g_f.fog_color[2],
             g_fog_control[0], g_fog_control[1], g_f.sky_color[0], g_f.sky_color[1], g_f.sky_color[2], g_rain, g_rain_alpha_shown,
             g_held_light.load());
    }
    if (!g_f.translucent_seen) {
        copy_depth1();
        draw_deferred_water();
    }
    if (!g_f.hand) copy_depth(2);
    draw_composites();
}

// ============================================================================ the shadow pass
void draw_shadow_pass() {
    if (!g_pack->shadow.present || !g_t.shadow_fbo || g_terrain_draws.empty()) return;
    glBindFramebuffer_(kFramebuffer, g_t.shadow_fbo);
    int r = g_t.shadow_res;
    glViewport(0, 0, r, r);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(1);
    glColorMask(1, 1, 1, 1);
    std::vector<int> sdb = g_pack->shadow.drawbuffers;
    set_draw_buffers(sdb);
    glBindVertexArray_(g_t.vao);
    for (GLuint l = 0; l < 16; l++) glDisableVertexAttribArray_(l);
    auto setp = [](GLint loc, const AttribState& a) {
        if (loc < 0) return;
        if (!a.enabled || !a.buffer) {
            glDisableVertexAttribArray_(loc);
            return;
        }
        glBindBuffer_(kArrayBuffer, a.buffer);
        glVertexAttribPointer_(loc, a.size, a.type, (GLboolean)a.normalized, a.stride, (const void*)a.offset);
        glEnableVertexAttribArray_(loc);
    };
    for (auto& d : g_terrain_draws) {
        auto gi = g_game_programs.find(d.game_program);
        if (gi == g_game_programs.end()) continue;
        Program* sp = shadow_program(d.game_program, gi->second);
        if (!sp) break;
        const AttribLocs& a = gi->second.attr;
        glUseProgram_(sp->id);
        set_frame_uniforms(*sp, true);
        glUniform4f_(sp->loc("emu_LmScale"), 256, 256, 1, 1);
        glUniform4f_(sp->loc("emu_LmBias"), -8, -8, 0, 0);
        GLint tl = sp->loc("texture");
        if (tl < 0) tl = sp->loc("tex");
        if (tl < 0) tl = sp->loc("gtexture");
        if (tl >= 0) glUniform1i_(tl, 0);
        bind_tex(0, d.atlas);
        Mat4 mv = g_f.shadow_view * translate(d.origin[0], d.origin[1], d.origin[2]) * scale(d.origin[3], d.origin[3], d.origin[3]);
        fixed_matrices(mv, g_f.shadow_proj, true);
        setp(a.pos, d.pos);
        setp(a.color, d.color);
        setp(a.tex0, d.tex0);
        setp(a.tex1, d.tex1);
        if (d.extra_vbo) {
            glBindBuffer_(kArrayBuffer, d.extra_vbo);
            glVertexAttribPointer_(kLocNormal, 3, 0x1400, GL_TRUE, sizeof(Extra), (const void*)offsetof(Extra, n));
            glVertexAttribPointer_(kLocTangent, 4, 0x1400, GL_TRUE, sizeof(Extra), (const void*)offsetof(Extra, t));
            glVertexAttribPointer_(kLocMidTex, 2, kFloat, GL_FALSE, sizeof(Extra), (const void*)offsetof(Extra, mid));
            glVertexAttribPointer_(kLocEntity, 2, kFloat, GL_FALSE, sizeof(Extra), (const void*)offsetof(Extra, id));
            for (GLuint l : {kLocNormal, kLocTangent, kLocMidTex, kLocEntity}) glEnableVertexAttribArray_(l);
        } else {
            for (GLuint l : {kLocNormal, kLocTangent, kLocMidTex, kLocEntity}) glDisableVertexAttribArray_(l);
            glVertexAttrib4f_(kLocNormal, 0, 1, 0, 0);
            glVertexAttrib4f_(kLocTangent, 1, 0, 0, 1);
            glVertexAttrib4f_(kLocEntity, 0, 0, 0, 0);
        }
        if (a.color < 0) glVertexAttrib4f_(kLocColorConst, 1, 1, 1, 1);
        glDisable(GL_ALPHA_TEST);
        if (GLint l = sp->loc("emu_discard"); l >= 0) glUniform1i_(l, d.cutout ? 1 : 0);
        if (GLint l = sp->loc("emu_albedo"); l >= 0) glUniform1i_(l, 0);
        glBindBuffer_(kElementArrayBuffer, d.ebo);
        glDrawElements(d.mode, d.count, d.type, (const void*)d.indices);
    }
    glDisable(GL_ALPHA_TEST);
    for (GLuint l = 0; l < 16; l++) glDisableVertexAttribArray_(l);
}

// ============================================================================ composite / final
void draw_quad(const Program&) {
    glBindVertexArray_(g_t.vao);
    for (GLuint l = 0; l < 16; l++) glDisableVertexAttribArray_(l);
    glBindBuffer_(kArrayBuffer, g_t.quad_vbo);
    glVertexAttribPointer_(0, 2, kFloat, GL_FALSE, 8, nullptr);
    glVertexAttribPointer_(1, 2, kFloat, GL_FALSE, 8, nullptr);
    glEnableVertexAttribArray_(0);
    glEnableVertexAttribArray_(1);
    glVertexAttrib4f_(kLocColorConst, 1, 1, 1, 1);
    glVertexAttrib4f_(kLocLmConst, 240, 240, 0, 1);
    glVertexAttrib4f_(kLocNormal, 0, 0, 1, 0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray_(0);
    glDisableVertexAttribArray_(1);
}

void bind_composite_textures(const Program& pr, const char* stage) {
    for (int i = 0; i < 8; i++) {
        GLuint t = custom_texture(stage, nullptr, i);
        bind_tex(kUnitColortex + i, t ? t : g_t.tex[i][g_t.cur[i]], 0);
    }
    for (int i = 0; i < 3; i++) bind_tex(kUnitDepth0 + i, g_t.depth[i], g_t.sampler_plain);
    static const char* const s0[] = {"shadow", "shadowtex0", "watershadow"};
    static const char* const s1[] = {"shadowtex1"};
    bind_tex(kUnitShadow0, g_t.shadow_depth ? g_t.shadow_depth : g_t.white, shadow_sampler(pr, s0, 3));
    bind_tex(kUnitShadow1, g_t.shadow_depth ? g_t.shadow_depth : g_t.white, shadow_sampler(pr, s1, 1));
    bind_tex(kUnitShadowColor0, g_t.shadow_color[0] ? g_t.shadow_color[0] : g_t.white, 0);
    bind_tex(kUnitShadowColor1, g_t.shadow_color[1] ? g_t.shadow_color[1] : g_t.white, 0);
    GLuint noise = custom_texture(stage, "noisetex", -1);
    bind_tex(kUnitNoise, noise ? noise : g_t.noise, 0);
}

void draw_composites() {
    // the game's state
    GLint prog = 0, vao = 0, abuf = 0, active = 0;
    glGetIntegerv(kCurrentProgram, &prog);
    glGetIntegerv(kVertexArrayBinding, &vao);
    glGetIntegerv(kArrayBufferBinding, &abuf);
    glGetIntegerv(kActiveTextureBinding, &active);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(0xFFFFFFFF);
    // the game may leave the stencil test on (the entities' shadows, when nothing is drawn after them: no GUI); the
    // passes would be drawn only where the stencil lets them (else the picture is lost: the clear colour stays)
    glDisable(GL_STENCIL_TEST);

    // the camera's depth in the middle of the screen (depth of field)
    {
        glBindFramebuffer_(kReadFramebuffer, g_t.fbo);
        float d = 1;
        glReadPixels(g_t.w / 2, g_t.h / 2, 1, 1, GL_DEPTH_COMPONENT, kFloat, &d);
        float k = g_pack->center_depth_halflife > 0 ? (float)(1 - exp(-g_f.frame_time * 0.693 / g_pack->center_depth_halflife)) : 1;
        g_f.center_depth += (d - g_f.center_depth) * k;
    }
    draw_shadow_pass();
    link_composites();

    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_ALPHA_TEST);
    glDepthMask(0);
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, g_t.w, g_t.h);
    fixed_matrices(Mat4::identity(), ortho(0, 1, 0, 1, -1, 1), false);
    // the camera's entity's pixels (the stencil) for the passes (see kCamStencil)
    bool cam_split = g_f.cam_marked && g_t.composite_ds;
    if (cam_split)
        glCopyImageSubData_(g_t.depth[0], GL_TEXTURE_2D, 0, 0, 0, 0, g_t.composite_ds, GL_TEXTURE_2D, 0, 0, 0, 0, g_t.w, g_t.h, 1);
    int idx = 0;
    for (auto& cp : g_composite_progs) {
        if (g_debug_passes >= 0 && idx >= g_debug_passes) break;
        const char* stage = idx < g_pack->first_composite ? "deferred" : "composite";
        idx++;
        if (!cp->ok) continue;
        // writes into the other texture of each buffer it draws to, which then becomes the current one
        glBindFramebuffer_(kFramebuffer, g_t.composite_fbo);
        GLenum db[8];
        int n = 0;
        for (int b : cp->drawbuffers) {
            if (b < 0 || b >= 8 || n >= 8) continue;
            glFramebufferTexture2D_(kFramebuffer, kColorAttachment0 + n, GL_TEXTURE_2D, g_t.tex[b][1 - g_t.cur[b]], 0);
            db[n] = kColorAttachment0 + n;
            n++;
        }
        for (int k = n; k < 8; k++) glFramebufferTexture2D_(kFramebuffer, kColorAttachment0 + k, GL_TEXTURE_2D, 0, 0);
        if (!n) continue;
        glDrawBuffers_(n, db);
        glUseProgram_(cp->id);
        set_frame_uniforms(*cp, false);
        bind_composite_textures(*cp, stage);
        if (cam_split && (cp->loc("gbufferPreviousModelView") >= 0 || cp->loc("previousCameraPosition") >= 0)) {
            // twice: elsewhere as it is, then on the camera's entity without the camera's motion
            glEnable(GL_STENCIL_TEST);
            glStencilMask(0);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            glStencilFunc(GL_NOTEQUAL, kCamStencil, kCamStencil);
            draw_quad(*cp);
            set_m(*cp, "gbufferPreviousModelView", g_f.view);
            set_m(*cp, "gbufferPreviousProjection", g_f.proj);
            set_f(*cp, "previousCameraPosition", g_f.cam[0], g_f.cam[1], g_f.cam[2]);
            glStencilFunc(GL_EQUAL, kCamStencil, kCamStencil);
            draw_quad(*cp);
            set_m(*cp, "gbufferPreviousModelView", g_f.prev_view);
            set_m(*cp, "gbufferPreviousProjection", g_f.prev_proj);
            set_f(*cp, "previousCameraPosition", g_f.prev_cam[0], g_f.prev_cam[1], g_f.prev_cam[2]);
            glDisable(GL_STENCIL_TEST);
        } else {
            draw_quad(*cp);
        }
        for (int b : cp->drawbuffers)
            if (b >= 0 && b < 8) g_t.cur[b] = 1 - g_t.cur[b];
        for (int b : cp->drawbuffers) {
            if (b < 0 || b >= 8 || !g_pack->buffers[b].mipmap) continue;
            glActiveTexture_(kTexture0 + kUnitColortex + b);
            glBindTexture(GL_TEXTURE_2D, g_t.tex[b][g_t.cur[b]]);
            glGenerateMipmap_(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, kLinearMipmapLinear);
        }
    }
    // final: into the game's framebuffer (Antialiasing: into a texture, then FXAA into the game's framebuffer)
    static GLuint fxaa_fbo = 0, fxaa_tex = 0, fxaa_prog = 0;
    static int fxaa_w = 0, fxaa_h = 0;
    bool fxaa = g_vs.aa > 0;
    if (fxaa && (fxaa_w != g_drawable_w || fxaa_h != g_drawable_h || !fxaa_tex)) {
        if (fxaa_tex) glDeleteTextures(1, &fxaa_tex);
        fxaa_tex = make_tex(0x8058, g_drawable_w, g_drawable_h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, GL_LINEAR);
        fxaa_w = g_drawable_w, fxaa_h = g_drawable_h;
        if (!fxaa_fbo) glGenFramebuffers_(1, &fxaa_fbo);
        glBindFramebuffer_(kFramebuffer, fxaa_fbo);
        glFramebufferTexture2D_(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D, fxaa_tex, 0);
    }
    if (fxaa && !fxaa_prog) {
        // FXAA (Timothy Lottes' FXAA 3, the "PC console" variant); FXAA_SPAN: 8 (2x) / 16 (4x)
        Source src;
        src.vs = R"(#version 120
varying vec2 uv;
void main() { uv = gl_MultiTexCoord0.st; gl_Position = ftransform(); }
)";
        src.fs = R"(#version 120
uniform sampler2D tex;
uniform vec2 texel;
uniform float span;
varying vec2 uv;
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
void main() {
    vec3 nw = texture2D(tex, uv + vec2(-1.0, -1.0) * texel).rgb;
    vec3 ne = texture2D(tex, uv + vec2(1.0, -1.0) * texel).rgb;
    vec3 sw = texture2D(tex, uv + vec2(-1.0, 1.0) * texel).rgb;
    vec3 se = texture2D(tex, uv + vec2(1.0, 1.0) * texel).rgb;
    vec4 m = texture2D(tex, uv);
    float lnw = luma(nw), lne = luma(ne), lsw = luma(sw), lse = luma(se), lm = luma(m.rgb);
    float lmin = min(lm, min(min(lnw, lne), min(lsw, lse)));
    float lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));
    vec2 dir = vec2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));
    float reduce = max((lnw + lne + lsw + lse) * (0.25 / 8.0), 1.0 / 128.0);
    float rcp = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcp, vec2(-span), vec2(span)) * texel;
    vec3 a = 0.5 * (texture2D(tex, uv + dir * (1.0 / 3.0 - 0.5)).rgb + texture2D(tex, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 b = a * 0.5 + 0.25 * (texture2D(tex, uv - dir * 0.5).rgb + texture2D(tex, uv + dir * 0.5).rgb);
    float lb = luma(b);
    gl_FragColor = vec4(lb < lmin || lb > lmax ? a : b, 1.0);
}
)";
        Program pr;
        AttribLocs a;
        a.pos = 0;
        a.tex0 = 1;
        if (link_program(pr, src, a, "fxaa")) fxaa_prog = pr.id;
    }
    fxaa = fxaa && fxaa_prog && fxaa_fbo;
    glBindFramebuffer_(kFramebuffer, fxaa ? fxaa_fbo : g_main_fb);
    GLenum c0 = kColorAttachment0;
    glDrawBuffers_(1, &c0);
    glViewport(0, 0, g_drawable_w, g_drawable_h);
    if (g_final_prog && g_final_prog->ok) {
        glUseProgram_(g_final_prog->id);
        set_frame_uniforms(*g_final_prog, false);
        bind_composite_textures(*g_final_prog, "final");
        draw_quad(*g_final_prog);
    } else {
        glBindFramebuffer_(kReadFramebuffer, g_t.composite_fbo);
        glFramebufferTexture2D_(kReadFramebuffer, kColorAttachment0, GL_TEXTURE_2D, g_t.tex[0][g_t.cur[0]], 0);
        glReadBuffer(kColorAttachment0);
        glBlitFramebuffer_(0, 0, g_t.w, g_t.h, 0, 0, g_drawable_w, g_drawable_h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    if (fxaa) {
        glBindFramebuffer_(kFramebuffer, g_main_fb);
        glDrawBuffers_(1, &c0);
        glUseProgram_(fxaa_prog);
        glUniform1i_(glGetUniformLocation_(fxaa_prog, "tex"), kUnitColortex);
        glUniform2f_(glGetUniformLocation_(fxaa_prog, "texel"), 1.0f / g_drawable_w, 1.0f / g_drawable_h);
        glUniform1f_(glGetUniformLocation_(fxaa_prog, "span"), g_vs.aa >= 2 ? 16.0f : 8.0f);
        bind_tex(kUnitColortex, fxaa_tex, g_t.sampler_linear);
        Program dummy;
        draw_quad(dummy);
    }
    // debug: <data>/shader_debug.txt "show=N" shows colortexN (8: depthtex0, 9: shadowtex0, 10: shadowcolor0, 11: depthtex1) instead
    {
        static double next_read = 0;
        static int show = -1;
        if (g_f.last_time >= next_read) {
            next_read = g_f.last_time + 1;
            show = -1;
            g_debug_passes = -1;
            g_debug_norain = false;
            std::vector<uint8_t> b;
            if (read_file(g_root / L"shader_debug.txt", b)) {
                std::string t(b.begin(), b.end());
                size_t k = t.find("show=");
                if (k != std::string::npos) show = atoi(t.c_str() + k + 5);
                g_debug_norain = t.find("norain") != std::string::npos;
                k = t.find("passes=");
                if (k != std::string::npos) g_debug_passes = atoi(t.c_str() + k + 7);
            }
        }
        if (show >= 0 && show <= 11) {
            GLuint t = show < 8 ? g_t.tex[show][g_t.cur[show]] : show == 8 ? g_t.depth[0] : show == 9 ? g_t.shadow_depth
                     : show == 10 ? g_t.shadow_color[0] : g_t.depth[1];
            if (t) {
                static GLuint dbg_prog = 0;
                if (!dbg_prog) {
                    Source src;
                    src.vs = R"(#version 120
varying vec2 uv;
void main() { uv = gl_MultiTexCoord0.st; gl_Position = ftransform(); }
)";
                    src.fs = R"(#version 120
uniform sampler2D t;
uniform int depth;
varying vec2 uv;
void main() {
    vec4 c = texture2D(t, uv);
    if (depth == 1) c = vec4(vec3(c.r >= 1.0 ? 1.0 : pow(c.r, 64.0) * 0.8), 1.0);  // (nothing drawn: white)
    gl_FragColor = vec4(c.rgb, 1.0);
}
)";
                    Program pr;
                    AttribLocs a;
                    a.pos = 0;
                    a.tex0 = 1;
                    if (link_program(pr, src, a, "debug")) dbg_prog = pr.id;
                }
                if (dbg_prog) {
                    glUseProgram_(dbg_prog);
                    glUniform1i_(glGetUniformLocation_(dbg_prog, "t"), kUnitColortex);
                    bool dt = show == 8 || show == 9 || show == 11;
                    glUniform1i_(glGetUniformLocation_(dbg_prog, "depth"), dt ? 1 : 0);
                    bind_tex(kUnitColortex, t, dt ? g_t.sampler_plain : 0);
                    Program dummy;
                    draw_quad(dummy);
                }
            }
        }
    }
    // the game's depth buffer: cleared (the GUI draws on top)
    glDepthMask(1);
    glClear(GL_DEPTH_BUFFER_BIT);
    // restore
    for (int u = 8; u < 32; u++) glBindSampler_(u, 0);
    glBindSampler_(0, 0);
    glPopClientAttrib();
    glPopAttrib();
    glViewport(g_game_vp[0], g_game_vp[1], g_game_vp[2], g_game_vp[3]);  // (the game's, not scaled: the GUI follows)
    glBindFramebuffer_(kFramebuffer, g_main_fb);
    // (glPopAttrib restores the draw buffers saved with the G-buffers bound into the game's framebuffer)
    glDrawBuffers_(1, &c0);
    glReadBuffer(kColorAttachment0);
    glBindVertexArray_(vao);
    glBindBuffer_(kArrayBuffer, abuf);
    glActiveTexture_(active);
    glUseProgram_(prog);
}

// ============================================================================ the atlas
struct BlockTex { const char* file; float id; };
const BlockTex kBlockTextures[] = {
    {"water_still", 9}, {"water_flow", 8}, {"lava_still", 11}, {"lava_flow", 10},
    {"leaves_oak", 18}, {"leaves_spruce", 18}, {"leaves_birch", 18}, {"leaves_jungle", 18}, {"leaves_acacia", 161}, {"leaves_big_oak", 161},
    {"leaves_oak_opaque", 18}, {"leaves_spruce_opaque", 18}, {"leaves_birch_opaque", 18}, {"leaves_jungle_opaque", 18},
    {"leaves_acacia_opaque", 161}, {"leaves_big_oak_opaque", 161},
    {"tallgrass", 31}, {"fern", 31}, {"deadbush", 32}, {"web", 30}, {"reeds", 83}, {"vine", 106},
    {"sapling_oak", 6}, {"sapling_spruce", 6}, {"sapling_birch", 6}, {"sapling_jungle", 6}, {"sapling_acacia", 6}, {"sapling_roofed_oak", 6},
    {"flower_dandelion", 37}, {"flower_rose", 38}, {"flower_allium", 38}, {"flower_blue_orchid", 38}, {"flower_houstonia", 38},
    {"flower_oxeye_daisy", 38}, {"flower_tulip_orange", 38}, {"flower_tulip_pink", 38}, {"flower_tulip_red", 38}, {"flower_tulip_white", 38},
    {"double_plant_grass_bottom", 31}, {"double_plant_grass_top", kDoubleTop}, {"double_plant_fern_bottom", 31}, {"double_plant_fern_top", kDoubleTop},
    {"double_plant_rose_bottom", 31}, {"double_plant_rose_top", kDoubleTop}, {"double_plant_paeonia_bottom", 31}, {"double_plant_paeonia_top", kDoubleTop},
    {"double_plant_syringa_bottom", 31}, {"double_plant_syringa_top", kDoubleTop}, {"double_plant_sunflower_bottom", 31}, {"double_plant_sunflower_top", kDoubleTop},
    {"wheat_stage_0", 59}, {"wheat_stage_1", 59}, {"wheat_stage_2", 59}, {"wheat_stage_3", 59}, {"wheat_stage_4", 59}, {"wheat_stage_5", 59},
    {"wheat_stage_6", 59}, {"wheat_stage_7", 59}, {"carrots_stage_0", 141}, {"carrots_stage_1", 141}, {"carrots_stage_2", 141}, {"carrots_stage_3", 141},
    {"potatoes_stage_0", 142}, {"potatoes_stage_1", 142}, {"potatoes_stage_2", 142}, {"potatoes_stage_3", 142},
    {"beetroots_stage_0", 207}, {"beetroots_stage_1", 207}, {"beetroots_stage_2", 207}, {"beetroots_stage_3", 207},
    {"torch_on", 50}, {"redstone_torch_on", 76}, {"glowstone", 89}, {"fire_0", 51}, {"fire_1", 51}, {"pumpkin_face_on", 91},
    {"redstone_lamp_on", 124}, {"beacon", 138}, {"sea_lantern", 169}, {"furnace_front_on", 62}, {"glowing_obsidian", 49},
    {"glass", 20}, {"ice", 79}, {"portal", 90}, {"slime", 165}, {"waterlily", 111},
};

fs::path g_textures_dir;  // the game's textures/blocks

void scan_atlas(GLuint tex) {
    g_atlas.scanned = true;
    if (!tex || g_textures_dir.empty()) return;
    GLint prev = 0, active = 0;
    glGetIntegerv(kActiveTextureBinding, &active);
    glActiveTexture_(kTexture0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, tex);
    GLint w = 0, h = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    std::vector<uint8_t> px;
    if (w > 0 && h > 0 && w <= 8192 && h <= 8192) {
        px.resize(size_t(w) * h * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    }
    glBindTexture(GL_TEXTURE_2D, prev);
    glActiveTexture_(active);
    if (px.empty()) return;
    g_atlas.w = w, g_atlas.h = h;
    g_atlas.cells.clear();
    // the textures' first 16x16 frames, keyed by their first row
    struct Want { float id; std::vector<uint8_t> px; int size; std::string name; int tex; };
    std::vector<Want> wants;
    int ntex = 0;
    for (auto& b : kBlockTextures) {
        // the .png and the .tga (the game may use either); animated textures are strips of frames: each frame
        for (const char* ext : {".png", ".tga"}) {
            std::vector<uint8_t> bytes, rgba;
            int tw = 0, th = 0;
            if (!read_file(g_textures_dir / (std::string(b.file) + ext), bytes)) continue;
            bool ok = ext[1] == 'p' ? decode_image(bytes, tw, th, rgba) : decode_tga(bytes, tw, th, rgba);
            if (!ok || tw <= 0 || th < tw) continue;
            for (int f = 0; f + tw <= th && f / tw < 64; f += tw) {
                Want wt{b.id, {}, tw, b.file, ntex};
                wt.px.assign(rgba.begin() + size_t(f) * tw * 4, rgba.begin() + size_t(f + tw) * tw * 4);
                for (size_t i = 0; i < wt.px.size(); i += 4)
                    if (wt.px[i + 3] == 0) wt.px[i] = wt.px[i + 1] = wt.px[i + 2] = 0;
                wants.push_back(std::move(wt));
            }
        }
        ntex++;
    }
    // (the atlas' transparent pixels are compared without their colour too)
    for (size_t i = 0; i < px.size(); i += 4)
        if (px[i + 3] == 0) px[i] = px[i + 1] = px[i + 2] = 0;
    auto row_key = [](const uint8_t* p) {
        uint64_t a, b;
        memcpy(&a, p, 8);
        memcpy(&b, p + 8, 8);
        return a * 0x9E3779B97F4A7C15ull ^ b;
    };
    std::unordered_multimap<uint64_t, int> first_rows;
    for (int i = 0; i < (int)wants.size(); i++) first_rows.emplace(row_key(wants[i].px.data()), i);
    std::vector<bool> found(ntex);
    for (int y = 0; y < h; y++)
        for (int x = 0; x + 4 <= w; x++) {
            auto range = first_rows.equal_range(row_key(&px[(size_t(y) * w + x) * 4]));
            for (auto it = range.first; it != range.second; ++it) {
                const Want& wt = wants[it->second];
                int s = wt.size;
                if (x + s > w || y + s > h) continue;
                bool same = true;
                for (int r = 0; r < s && same; r++)
                    same = memcmp(&px[(size_t(y + r) * w + x) * 4], &wt.px[size_t(r) * s * 4], size_t(s) * 4) == 0;
                if (same) {
                    bool dup = false;
                    for (auto& c : g_atlas.cells) dup |= c.x == x && c.y == y;
                    if (!dup) g_atlas.cells.push_back({x, y, s, s, wt.id});
                    found[wt.tex] = true;
                }
            }
        }
    // animated textures (portal, lava, fire...): the game blends two frames into the atlas (not found above); the
    // atlas' tiles are 32 pixels with the 16-pixel texture in the middle: the tile nearest to one of the frames, when
    // it is clearly the nearest
    for (int t = 0; t < ntex; t++) {
        if (found[t]) continue;
        std::vector<const Want*> frames;
        for (auto& wt : wants)
            if (wt.tex == t && wt.size == 16) frames.push_back(&wt);
        if (frames.size() < 2) continue;
        long best = LONG_MAX, second = LONG_MAX;
        int bx = 0, by = 0;
        for (int y = 8; y + 16 <= h; y += 16)
            for (int x = 8; x + 16 <= w; x += 16) {
                bool taken = false;
                for (auto& c : g_atlas.cells) taken |= x + 16 > c.x && x < c.x + c.w && y + 16 > c.y && y < c.y + c.h;
                if (taken) continue;
                long d = LONG_MAX;
                for (const Want* f : frames) {
                    long sum = 0;
                    for (int r = 0; r < 16 && sum < d && sum < second; r++) {
                        const uint8_t* a = &px[(size_t(y + r) * w + x) * 4];
                        const uint8_t* b = &f->px[size_t(r) * 64];
                        for (int i = 0; i < 64; i++) sum += std::abs(a[i] - b[i]);
                    }
                    d = std::min(d, sum);
                }
                if (d < best) second = best, best = d, bx = x, by = y;
                else if (d < second) second = d;
            }
        // (a mean difference below 12 of 255 per channel and at most half the next tile's)
        if (best < 12 * 1024 && best * 2 < second) {
            g_atlas.cells.push_back({bx, by, 16, 16, frames[0]->id});
            found[t] = true;
            LOGI("shaders: atlas: %s (animated) at %d,%d (difference %.1f, next %.1f)", frames[0]->name.c_str(), bx, by,
                 best / 1024.0, second == LONG_MAX ? -1.0 : second / 1024.0);
        }
    }
    int nf = 0;
    for (bool f : found) nf += f;
    {
        std::string miss;
        int wi = 0;
        for (auto& b : kBlockTextures) {
            (void)b;
        }
        std::set<std::string> shown;
        for (auto& wt : wants)
            if (!found[wt.tex] && shown.insert(wt.name).second) miss += " " + wt.name;
        if (!miss.empty()) LOGI("shaders: not found in the atlas:%s", miss.c_str());
        (void)wi;
    }
    LOGI("shaders: texture atlas %dx%d: %d of %d block textures found", w, h, nf, ntex);
    // the extras made before are made again with the blocks
    if (nf)
        for (auto& [k, e] : g_extras) e.gen = ~0ull;
}

}  // namespace

namespace {
// ---------------------------------------------------------------- textures read back from the game
// a texture's pixels (RGBA8), read with glGetTexImage on texture unit 7 (restored)
bool read_texture(GLuint tex, int& w, int& h, std::vector<uint8_t>& px) {
    GLint active = 0, prev = 0;
    glGetIntegerv(kActiveTextureBinding, &active);
    glActiveTexture_(kTexture0 + 7);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    bool ok = w > 0 && h > 0 && w <= 8192 && h <= 8192;
    if (ok) {
        px.resize(size_t(w) * h * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    }
    glBindTexture(GL_TEXTURE_2D, prev);
    glActiveTexture_(active);
    return ok;
}

const Colormap* colormap(GLuint tex) {
    auto it = g_colormaps.find(tex);
    if (it != g_colormaps.end()) return &it->second;
    Colormap& c = g_colormaps[tex];
    if (!read_texture(tex, c.w, c.h, c.px)) c.px.clear();
    return &c;
}

GLuint bound_texture(GLint unit) {
    GLint active = 0, t = 0;
    glGetIntegerv(kActiveTextureBinding, &active);
    glActiveTexture_(kTexture0 + unit);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &t);
    glActiveTexture_(active);
    return (GLuint)t;
}

// entities whose texture the game's shader makes of several (USE_MULTITEXTURE: the sheep, the horses...; USE_COLOR_MASK:
// dyed wool, leather armour): made into one texture (cached) for the pack, as entity.fragment does
struct Resolved { GLuint tex = 0; };
std::map<std::array<uint32_t, 6>, Resolved> g_resolved;
GLuint g_resolve_prog = 0, g_resolve_fbo = 0;

GLuint resolved_texture(GLuint t0, GLuint t1, GLuint t2, int flags, const float* change) {
    uint32_t cc = 0;
    for (int k = 0; k < 4; k++) cc |= (uint32_t)std::clamp((int)lroundf(change[k] * 255), 0, 255) << (k * 8);
    std::array<uint32_t, 6> key{t0, t1, t2, (uint32_t)flags, cc, 0};
    auto it = g_resolved.find(key);
    if (it != g_resolved.end()) return it->second.tex;
    if (g_resolved.size() > 256) {
        for (auto& [k, v] : g_resolved) glDeleteTextures(1, &v.tex);
        g_resolved.clear();
    }
    if (!g_resolve_prog) {
        Source src;
        src.vs = R"(#version 120
varying vec2 uv;
void main() { uv = gl_MultiTexCoord0.st; gl_Position = ftransform(); }
)";
        src.fs = R"(#version 120
uniform sampler2D t0, t1, t2;
uniform vec3 flags;  // multitexture, colour mask, second texture coloured (0 / 1)
uniform vec4 change;
varying vec2 uv;
void main() {
    vec4 c = texture2D(t0, uv);
    // USE_COLOR_MASK: the alpha is where the colour (dye) goes; only alpha 0 is a hole (NEEDS_DISCARD: a <= 0)
    if (flags.y > 0.5) { c.rgb = mix(c.rgb, c.rgb * change.rgb, c.a); c.a = c.a > 0.0 ? change.a : 0.0; }
    if (flags.x > 0.5) {
        vec4 a = texture2D(t1, uv), b = texture2D(t2, uv);
        c.rgb = mix(c.rgb, a.rgb, a.a);
        if (flags.z > 0.5) { if (b.a > 0.0) c.rgb = b.rgb + (b.rgb * change.rgb - b.rgb) * b.a; }
        else c.rgb = mix(c.rgb, b.rgb, b.a);
    }
    gl_FragColor = c;
}
)";
        Program pr;
        AttribLocs a;
        a.pos = 0;
        a.tex0 = 1;
        static bool failed = false;
        if (failed) return 0;
        if (!link_program(pr, src, a, "entity texture")) return failed = true, 0;
        g_resolve_prog = pr.id;
        glGenFramebuffers_(1, &g_resolve_fbo);
    }
    int w = 0, h = 0;
    {
        GLint active = 0, prev = 0;
        glGetIntegerv(kActiveTextureBinding, &active);
        glActiveTexture_(kTexture0 + 7);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
        glBindTexture(GL_TEXTURE_2D, t0);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
        glBindTexture(GL_TEXTURE_2D, prev);
        glActiveTexture_(active);
    }
    LOGD("shaders: entity texture %u+%u+%u flags %d change %.2f %.2f %.2f %.2f (%dx%d)", t0, t1, t2, flags, change[0], change[1],
         change[2], change[3], w, h);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;
    Resolved r;
    r.tex = make_tex(0x8058, w, h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, GL_NEAREST);
    // draw it (the state is saved and restored around)
    GLint prog = 0, fb = 0, vao = 0, abuf = 0, active = 0, vp[4];
    glGetIntegerv(kCurrentProgram, &prog);
    glGetIntegerv(kDrawFramebufferBinding, &fb);
    glGetIntegerv(kVertexArrayBinding, &vao);
    glGetIntegerv(kArrayBufferBinding, &abuf);
    glGetIntegerv(kActiveTextureBinding, &active);
    glGetIntegerv(GL_VIEWPORT, vp);
    glPushAttrib(GL_COLOR_BUFFER_BIT | GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT | GL_SCISSOR_BIT | GL_STENCIL_BUFFER_BIT | GL_TRANSFORM_BIT);
    glBindFramebuffer_(kFramebuffer, g_resolve_fbo);
    glFramebufferTexture2D_(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D, r.tex, 0);
    GLenum db = kColorAttachment0;
    glDrawBuffers_(1, &db);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_ALPHA_TEST);
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, w, h);
    glUseProgram_(g_resolve_prog);
    glUniform1i_(glGetUniformLocation_(g_resolve_prog, "t0"), kUnitColortex);
    glUniform1i_(glGetUniformLocation_(g_resolve_prog, "t1"), kUnitColortex + 1);
    glUniform1i_(glGetUniformLocation_(g_resolve_prog, "t2"), kUnitColortex + 2);
    glUniform3f_(glGetUniformLocation_(g_resolve_prog, "flags"), flags & 1 ? 1.0f : 0.0f, flags & 2 ? 1.0f : 0.0f, flags & 4 ? 1.0f : 0.0f);
    glUniform4f_(glGetUniformLocation_(g_resolve_prog, "change"), change[0], change[1], change[2], change[3]);
    bind_tex(kUnitColortex, t0, g_t.sampler_plain);
    bind_tex(kUnitColortex + 1, t1 ? t1 : g_t.black, g_t.sampler_plain);
    bind_tex(kUnitColortex + 2, t2 ? t2 : g_t.black, g_t.sampler_plain);
    fixed_matrices(Mat4::identity(), ortho(0, 1, 0, 1, -1, 1), false);
    Program dummy;
    draw_quad(dummy);
    for (int u = 0; u < 3; u++) glBindSampler_(kUnitColortex + u, 0);
    glPopAttrib();
    glBindFramebuffer_(kFramebuffer, fb);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glBindVertexArray_(vao);
    glBindBuffer_(kArrayBuffer, abuf);
    glActiveTexture_(active);
    glUseProgram_(prog);
    g_resolved[key] = r;
    return r.tex;
}

// Water in the opaque terrain (the game draws far water as part of the opaque layer): its triangles are drawn
// later with the pack's water program; the rest now. The index lists are made once per draw (and version).
struct Split {
    uint64_t version = 0;
    GLuint land = 0, water = 0;  // element buffers (32-bit indices)
    GLsizei land_count = 0, water_count = 0;
};
struct SplitKey {
    GLuint extras, ebo;
    uintptr_t offset;
    GLsizei count;
    bool operator<(const SplitKey& o) const {
        return std::tie(extras, ebo, offset, count) < std::tie(o.extras, o.ebo, o.offset, o.count);
    }
};
std::map<SplitKey, Split> g_splits;

const Split* split_for(const ExtraBuf& e, GLuint ebo, GLenum type, uintptr_t offset, GLsizei count) {
    if (e.water.empty() || !ebo || count <= 0 || (type != 0x1403 && type != 0x1405)) return nullptr;
    Split& sp = g_splits[{e.vbo, ebo, offset, count}];
    if (sp.version == e.version) return sp.water_count ? &sp : nullptr;
    sp.version = e.version;
    int isz = type == 0x1405 ? 4 : 2;
    std::vector<uint8_t> raw(size_t(count) * isz);
    glGetNamedBufferSubData_(ebo, offset, (GLsizeiptr)raw.size(), raw.data());
    std::vector<uint32_t> land, water;
    for (GLsizei i = 0; i + 2 < count; i += 3) {
        uint32_t idx[3];
        for (int k = 0; k < 3; k++) {
            if (isz == 4) memcpy(&idx[k], &raw[size_t(i + k) * 4], 4);
            else { uint16_t v; memcpy(&v, &raw[size_t(i + k) * 2], 2); idx[k] = v; }
        }
        bool w = idx[0] < e.water.size() && e.water[idx[0]];
        auto& dst = w ? water : land;
        dst.insert(dst.end(), idx, idx + 3);
    }
    GLint prev = 0;
    glGetIntegerv(kArrayBufferBinding, &prev);
    for (auto [list, buf, cnt] : {std::tuple{&land, &sp.land, &sp.land_count}, std::tuple{&water, &sp.water, &sp.water_count}}) {
        *cnt = (GLsizei)list->size();
        if (!*buf) glGenBuffers_(1, buf);
        // (uploaded through GL_ARRAY_BUFFER: binding an element buffer would change the game's vertex array)
        glBindBuffer_(kArrayBuffer, *buf);
        glBufferData_(kArrayBuffer, GLsizeiptr(list->size() * 4 + 4), list->empty() ? nullptr : list->data(), kStaticDraw);
    }
    glBindBuffer_(kArrayBuffer, prev);
    if (g_splits.size() > 8192) {
        for (auto& [k, v] : g_splits) glDeleteBuffers_(1, &v.land), glDeleteBuffers_(1, &v.water);
        g_splits.clear();
        return nullptr;
    }
    return sp.water_count ? &sp : nullptr;
}

// the water layer's triangles whose texture is not known: water (its cells are learnt; once per draw and version)
std::set<std::tuple<GLuint, uint64_t, GLuint, uintptr_t, GLsizei>> g_learnt_draws;
void learn_water(const ExtraBuf& e, GLuint ebo, GLenum type, uintptr_t offset, GLsizei count) {
    if (!ebo || count <= 0 || (type != 0x1403 && type != 0x1405) || e.unknown_cell.empty()) return;
    if (!g_learnt_draws.insert({e.vbo, e.version, ebo, offset, count}).second) return;
    if (g_learnt_draws.size() > 65536) g_learnt_draws.clear();
    int isz = type == 0x1405 ? 4 : 2;
    std::vector<uint8_t> raw(size_t(count) * isz);
    glGetNamedBufferSubData_(ebo, offset, (GLsizeiptr)raw.size(), raw.data());
    for (GLsizei i = 0; i < count; i += 3) {
        uint32_t v;
        if (isz == 4) memcpy(&v, &raw[size_t(i) * 4], 4);
        else { uint16_t x; memcpy(&x, &raw[size_t(i) * 2], 2); v = x; }
        if (v < e.unknown_cell.size() && e.unknown_cell[v] != ~0u && g_atlas.water.insert(e.unknown_cell[v]).second)
            g_water_learnt = true;
    }
}

struct WaterDraw {
    GLuint game_program;
    AttribState pos, color, tex0, tex1;
    GLuint ebo, extra_vbo;
    GLsizei count;
    float origin[4];
    GLuint atlas, lightmap;
    GLint tex0_unit, tex1_unit;
};
std::vector<WaterDraw> g_water_draws;
void clear_water_draws() { g_water_draws.clear(); }

// the pack's textures for a gbuffers program (units 8..25)
void bind_gbuffer_textures(const Program& pp) {
    static const char* const s0[] = {"shadow", "shadowtex0", "watershadow"};
    static const char* const s1[] = {"shadowtex1"};
    bind_tex(kUnitNormals, g_t.flat_normal);
    bind_tex(kUnitSpecular, g_t.black);
    bind_tex(kUnitNoiseG, g_t.noise);
    bind_tex(kUnitShadow0G, g_t.shadow_depth ? g_t.shadow_depth : g_t.white, shadow_sampler(pp, s0, 3));
    bind_tex(kUnitShadow1G, g_t.shadow_depth ? g_t.shadow_depth : g_t.white, shadow_sampler(pp, s1, 1));
    bind_tex(kUnitShadowColor0G, g_t.shadow_color[0] ? g_t.shadow_color[0] : g_t.white);
    bind_tex(kUnitShadowColor1G, g_t.shadow_color[1] ? g_t.shadow_color[1] : g_t.white);
    bind_tex(kUnitDepthG, g_t.depth[1], g_t.sampler_plain);
    for (int i = 0; i < 8; i++) {
        GLuint ct = custom_texture("gbuffers", nullptr, i);
        bind_tex(kUnitColortexG + i, ct ? ct : g_t.tex[i][1 - g_t.cur[i]]);
    }
}

// the water taken out of the opaque terrain, drawn as water (when the translucent terrain starts)
void draw_deferred_water() {
    if (g_water_draws.empty()) return;
    GLint prog = 0, vao = 0, abuf = 0, active = 0;
    glGetIntegerv(kCurrentProgram, &prog);
    glGetIntegerv(kVertexArrayBinding, &vao);
    glGetIntegerv(kArrayBufferBinding, &abuf);
    glGetIntegerv(kActiveTextureBinding, &active);
    GLuint saved_tex[2] = {bound_texture(g_water_draws[0].tex0_unit), bound_texture(g_water_draws[0].tex1_unit)};
    GLint u0 = g_water_draws[0].tex0_unit, u1 = g_water_draws[0].tex1_unit;
    glPushAttrib(GL_COLOR_BUFFER_BIT | GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT | GL_POLYGON_BIT | GL_TRANSFORM_BIT | GL_FOG_BIT |
                 GL_STENCIL_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(1);
    glDisable(GL_CULL_FACE);
    // (the stencil test may be left on by the entities' shadows, see draw_composites); water in front of the camera's
    // entity unmarks it
    if (g_f.cam_marked) mark_camera_entity(false);
    else glDisable(GL_STENCIL_TEST);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.1f);
    glBindVertexArray_(g_t.vao);
    for (GLuint l = 0; l < 16; l++) glDisableVertexAttribArray_(l);
    auto setp = [](GLint loc, const AttribState& a) {
        if (loc < 0 || !a.enabled || !a.buffer) return;
        glBindBuffer_(kArrayBuffer, a.buffer);
        glVertexAttribPointer_(loc, a.size, a.type, (GLboolean)a.normalized, a.stride, (const void*)a.offset);
        glEnableVertexAttribArray_(loc);
    };
    for (auto& d : g_water_draws) {
        auto gi = g_game_programs.find(d.game_program);
        if (gi == g_game_programs.end()) continue;
        const GameProgram& gp = gi->second;
        Program* pp = gbuffers_program(kWater, d.game_program, gp);
        if (!pp) break;
        glUseProgram_(pp->id);
        set_frame_uniforms(*pp, true);
        for (const char* n : {"texture", "tex", "gtexture"})
            if (GLint l = pp->loc(n); l >= 0) glUniform1i_(l, d.tex0_unit);
        if (GLint l = pp->loc("lightmap"); l >= 0) glUniform1i_(l, d.tex1_unit);
        glUniform4f_(pp->loc("emu_LmScale"), 256, 256, 1, 1);
        glUniform4f_(pp->loc("emu_LmBias"), -8, -8, 0, 0);
        bind_gbuffer_textures(*pp);
        bind_tex(d.tex0_unit, d.atlas);
        bind_tex(d.tex1_unit, d.lightmap);
        Mat4 mv = g_f.view * translate(d.origin[0], d.origin[1], d.origin[2]) * scale(d.origin[3], d.origin[3], d.origin[3]);
        fixed_matrices(mv, g_f.proj, true);
        for (GLuint l = 0; l < 16; l++) glDisableVertexAttribArray_(l);
        setp(gp.attr.pos, d.pos);
        setp(gp.attr.color, d.color);
        setp(gp.attr.tex0, d.tex0);
        setp(gp.attr.tex1, d.tex1);
        glBindBuffer_(kArrayBuffer, d.extra_vbo);
        glVertexAttribPointer_(kLocNormal, 3, 0x1400, GL_TRUE, sizeof(Extra), (const void*)offsetof(Extra, n));
        glVertexAttribPointer_(kLocTangent, 4, 0x1400, GL_TRUE, sizeof(Extra), (const void*)offsetof(Extra, t));
        glVertexAttribPointer_(kLocMidTex, 2, kFloat, GL_FALSE, sizeof(Extra), (const void*)offsetof(Extra, mid));
        glVertexAttribPointer_(kLocEntity, 2, kFloat, GL_FALSE, sizeof(Extra), (const void*)offsetof(Extra, id));
        for (GLuint l : {kLocNormal, kLocTangent, kLocMidTex, kLocEntity}) glEnableVertexAttribArray_(l);
        set_draw_buffers(pp->drawbuffers);
        glBindBuffer_(kElementArrayBuffer, d.ebo);
        glDrawElements(GL_TRIANGLES, d.count, 0x1405, nullptr);
    }
    for (GLuint l = 0; l < 16; l++) glDisableVertexAttribArray_(l);
    glBindSampler_(u0, 0);
    glBindSampler_(u1, 0);
    bind_tex(u1, saved_tex[1]);
    bind_tex(u0, saved_tex[0]);
    glPopAttrib();
    glBindVertexArray_(vao);
    glBindBuffer_(kArrayBuffer, abuf);
    glActiveTexture_(active);
    glUseProgram_(prog);
    g_water_draws.clear();
}

}  // namespace

// ============================================================================ API
void init(const std::wstring& root) {
    std::lock_guard lk(g_list_lock);
    g_root = root;
    g_dir = g_root / L"shaders";
    std::error_code ec;
    fs::create_directories(g_dir, ec);
    g_textures_dir = g_root / L"app" / L"Payload" / L"minecraftpe.app" / L"data" / L"resourcepacks" / L"vanilla" / L"client" / L"textures" / L"blocks";
    g_packs.clear();
    for (auto& e : fs::directory_iterator(g_dir, ec)) {
        if (!e.is_regular_file()) continue;
        std::wstring ext = e.path().extension().wstring();
        for (auto& c : ext) c = towlower(c);
        if (ext == L".zip") g_packs.push_back(e.path());
    }
    std::sort(g_packs.begin(), g_packs.end(), [](const fs::path& a, const fs::path& b) {
        return _wcsicmp(a.filename().c_str(), b.filename().c_str()) < 0;
    });
    // the saved choice
    std::vector<uint8_t> bytes;
    std::string want;
    if (read_file(g_root / L"shaders.txt", bytes)) {
        std::string t(bytes.begin(), bytes.end());
        size_t p = t.find("shader=");
        if (p != std::string::npos) {
            size_t e = t.find_first_of("\r\n", p);
            want = trim(t.substr(p + 7, e == std::string::npos ? std::string::npos : e - p - 7));
        }
    }
    int sel = -1;
    for (int i = 0; i < (int)g_packs.size(); i++)
        if (!want.empty() && utf8(g_packs[i].filename().wstring()) == want) sel = i;
    g_selected = sel;
    LOGI("shaders: %zu shader packs in %s; in use: %s", g_packs.size(), utf8(g_dir.wstring()).c_str(),
         sel >= 0 ? want.c_str() : "none");
}

int count() {
    std::lock_guard lk(g_list_lock);
    return (int)g_packs.size();
}
std::string name(int i) {
    std::lock_guard lk(g_list_lock);
    return i >= 0 && i < (int)g_packs.size() ? utf8(g_packs[i].filename().wstring()) : "";
}
int selected() { return g_selected.load(); }

// ---- "Shader Options" (see ShMeta) ----
bool options_begin() {
    fs::path p;
    {
        std::lock_guard lk(g_list_lock);
        int i = g_selected.load();
        if (i >= 0 && i < (int)g_packs.size()) p = g_packs[i];
    }
    std::lock_guard lk(g_meta_lock);
    g_meta = p.empty() ? nullptr : read_meta(p);
    g_screen.clear();
    return g_meta != nullptr;
}

int options_open(const std::string& screen) {
    std::lock_guard lk(g_meta_lock);
    g_screen.clear();
    g_screen_name = screen;
    if (!g_meta) return 0;
    ShMeta& m = *g_meta;
    auto it = m.props.find(screen.empty() ? "screen" : "screen." + screen);
    std::set<std::string> shown;
    auto add_option = [&](const std::string& n) {
        if (!m.index.count(n)) return;
        g_screen.push_back({1, n});
        shown.insert(n);
    };
    if (it == m.props.end()) {
        if (screen.empty())
            for (auto& o : m.opts) add_option(o.name);
        return (int)g_screen.size();
    }
    // the options in any screen (for "*": the others)
    std::set<std::string> listed;
    for (auto& [k, v] : m.props)
        if (k == "screen" || (k.rfind("screen.", 0) == 0 && k.find('.', 7) == std::string::npos))
            for (auto& t : split_ws(v)) listed.insert(t);
    for (auto& t : split_ws(it->second)) {
        if (t == "<empty>") g_screen.push_back({0, ""});
        else if (t == "<profile>") g_screen.push_back({m.profiles.empty() ? 0 : 3, ""});
        else if (t.size() > 2 && t.front() == '[' && t.back() == ']') g_screen.push_back({2, t.substr(1, t.size() - 2)});
        else if (t == "*") {
            for (auto& o : m.opts)
                if (!listed.count(o.name) && !shown.count(o.name)) add_option(o.name);
        } else add_option(t);
    }
    return (int)g_screen.size();
}

std::string options_title() {
    std::lock_guard lk(g_meta_lock);
    std::string t;
    if (g_meta && !g_screen_name.empty()) t = meta_text(*g_meta, "screen." + g_screen_name);
    if (t.empty()) t = g_screen_name.empty() ? (lang_ja() ? "シェーダーのオプション設定" : "Shader Options") : g_screen_name;
    return t;
}

int options_entry(int i, std::string& label) {
    std::lock_guard lk(g_meta_lock);
    label.clear();
    if (!g_meta || i < 0 || i >= (int)g_screen.size()) return -1;
    ShMeta& m = *g_meta;
    const ShEntry& e = g_screen[i];
    if (e.kind == 1) label = option_label(m, m.opts[m.index[e.name]]);
    else if (e.kind == 2) {
        label = meta_text(m, "screen." + e.name);
        if (label.empty()) label = e.name;
        label += "...";
    } else if (e.kind == 3) {
        int p = current_profile(m);
        std::string n = p < 0 ? (lang_ja() ? "カスタム" : "Custom") : meta_text(m, "profile." + m.profiles[p].first);
        if (n.empty()) n = m.profiles[p].first;
        label = (lang_ja() ? "プロファイル: " : "Profile: ") + n;
    }
    return e.kind;
}

int options_click(int i, std::string& screen) {
    std::lock_guard lk(g_meta_lock);
    if (!g_meta || i < 0 || i >= (int)g_screen.size()) return -1;
    ShMeta& m = *g_meta;
    const ShEntry& e = g_screen[i];
    if (e.kind == 1) {
        ShOption& o = m.opts[m.index[e.name]];
        auto vi = std::find(o.values.begin(), o.values.end(), o.cur);
        size_t k = vi == o.values.end() ? 0 : (vi - o.values.begin() + 1) % o.values.size();
        o.cur = o.values[k];
        m.changed = true;
    } else if (e.kind == 3 && !m.profiles.empty()) {
        int p = (current_profile(m) + 1) % (int)m.profiles.size();
        std::map<std::string, std::string> v;
        profile_values(m, m.profiles[p].second, v);
        for (auto& [k, val] : v) {
            auto it = m.index.find(k);
            if (it == m.index.end()) continue;
            ShOption& o = m.opts[it->second];
            if (std::find(o.values.begin(), o.values.end(), val) != o.values.end()) o.cur = val;
        }
        m.changed = true;
    } else if (e.kind == 2) {
        screen = e.name;
    }
    return e.kind;
}

void options_reset() {
    std::lock_guard lk(g_meta_lock);
    if (!g_meta) return;
    for (auto& o : g_meta->opts) o.cur = o.def;
    g_meta->changed = true;
}

void options_close() {
    std::lock_guard lk(g_meta_lock);
    if (!g_meta || !g_meta->changed) return;
    g_meta->changed = false;
    save_meta(*g_meta);
    g_reload_pack = true;
    LOGI("shaders: options saved (%s): the pack is loaded again", utf8(options_file(g_meta->pack).filename().wstring()).c_str());
}
void select(int i) {
    if (i < -1 || i >= count()) return;
    if (g_selected.exchange(i) == i) return;
    LOGI("shaders: chosen %s", i >= 0 ? name(i).c_str() : "none");
    save_choice();
}

void gl_ready(void* (*getproc)(const char*)) {
#define LD(NAME) NAME##_ = reinterpret_cast<PFN_##NAME>(getproc(#NAME));
    LD(glActiveTexture) LD(glCreateShader) LD(glShaderSource) LD(glCompileShader) LD(glGetShaderiv) LD(glGetShaderInfoLog)
    LD(glCreateProgram) LD(glAttachShader) LD(glBindAttribLocation) LD(glLinkProgram) LD(glGetProgramiv) LD(glGetProgramInfoLog)
    LD(glDeleteShader) LD(glDeleteProgram) LD(glUseProgram) LD(glGetUniformLocation) LD(glGetActiveUniform) LD(glUniform1i)
    LD(glUniform2i) LD(glUniform3i) LD(glUniform1f) LD(glUniform2f) LD(glUniform3f) LD(glUniform4f) LD(glUniformMatrix4fv)
    LD(glGetUniformfv) LD(glGetUniformiv) LD(glGetAttribLocation) LD(glGenFramebuffers) LD(glBindFramebuffer) LD(glFramebufferTexture2D)
    LD(glCheckFramebufferStatus) LD(glDrawBuffers) LD(glGenBuffers) LD(glBindBuffer) LD(glBufferData) LD(glDeleteBuffers)
    LD(glGetBufferSubData) LD(glGetBufferParameteriv) LD(glVertexAttribPointer) LD(glEnableVertexAttribArray)
    LD(glDisableVertexAttribArray) LD(glVertexAttrib4f) LD(glGetVertexAttribiv) LD(glGetVertexAttribPointerv)
    LD(glGenVertexArrays) LD(glBindVertexArray) LD(glGenSamplers) LD(glSamplerParameteri) LD(glBindSampler)
    LD(glCopyImageSubData) LD(glGenerateMipmap) LD(glBlitFramebuffer) LD(glGetNamedBufferSubData) LD(glGetNamedBufferParameteriv)
    LD(glDeleteFramebuffers) LD(glDeleteVertexArrays) LD(glDeleteSamplers) LD(glGetFramebufferAttachmentParameteriv)
#undef LD
    g_ready = glCopyImageSubData_ && glBindSampler_ && glGetNamedBufferSubData_ && glDrawBuffers_ && glGenVertexArrays_;
    const char* vendor = (const char*)glGetString(GL_VENDOR);
    std::string v = vendor ? vendor : "";
    if (v.find("NVIDIA") != std::string::npos) g_gl_vendor_macro = "#define MC_GL_VENDOR_NVIDIA\n";
    else if (v.find("ATI") != std::string::npos || v.find("AMD") != std::string::npos) g_gl_vendor_macro = "#define MC_GL_VENDOR_ATI\n";
    else if (v.find("Intel") != std::string::npos) g_gl_vendor_macro = "#define MC_GL_VENDOR_INTEL\n";
    if (!g_ready) LOGW("shaders: the OpenGL driver lacks functions shader packs need (OpenGL 4.5): shader packs are off");
}

void on_link(unsigned prog, const std::string& vs, const std::string& fsrc) {
    GameProgram gp;
    auto loc = [&](const char* n) { return glGetAttribLocation_ ? glGetAttribLocation_(prog, n) : -1; };
    auto uni = [&](const char* n) { return glGetUniformLocation_ ? glGetUniformLocation_(prog, n) : -1; };
    gp.attr.pos = loc("POSITION");
    gp.attr.color = loc("COLOR");
    gp.attr.tex0 = loc("TEXCOORD_0");
    gp.attr.tex1 = loc("TEXCOORD_1");
    gp.attr.normal = loc("NORMAL");
    gp.u_worldview = uni("WORLDVIEW");
    gp.u_proj = uni("PROJ");
    gp.u_wvp = uni("WORLDVIEWPROJ");
    gp.u_world = uni("WORLD");
    gp.u_origin = uni("CHUNK_ORIGIN_AND_SCALE");
    gp.u_fog_color = uni("FOG_COLOR");
    gp.u_fog_control = uni("FOG_CONTROL");
    gp.u_render_distance = uni("RENDER_DISTANCE");
    gp.u_tile_light = uni("TILE_LIGHT_COLOR");
    gp.u_current_color = uni("CURRENT_COLOR");
    gp.u_view_pos = uni("VIEW_POS");
    gp.u_overlay = uni("OVERLAY_COLOR");
    gp.u_tex0 = uni("TEXTURE_0");
    gp.u_tex1 = uni("TEXTURE_1");
    gp.u_tex2 = uni("TEXTURE_2");
    gp.u_change_color = uni("CHANGE_COLOR");
    gp.u_alpha = uni("ALPHA");
    std::string both = vs + fsrc;
    gp.seasons = has_define(both, "SEASONS");
    gp.u_hand = uni("EMU_HAND_LIGHT");
    gp.u_uv_offset = uni("UV_OFFSET");
    gp.u_uv_rotation = uni("UV_ROTATION");
    gp.u_glint_scale = uni("GLINT_UV_SCALE");
    gp.u_glint_color = uni("GLINT_COLOR");
    gp.stars = fsrc.find("color.rgb * CURRENT_COLOR.rgb * color.a") != std::string::npos;
    gp.sun_moon = fsrc.find("CURRENT_COLOR") != std::string::npos && fsrc.find("TEXTURE_0") != std::string::npos &&
                  vs.find("TILE_LIGHT_COLOR") == std::string::npos && vs.find("CHUNK_ORIGIN_AND_SCALE") == std::string::npos;
    gp.alpha_test = has_define(both, "ALPHA_TEST");
    gp.emissive = has_define(both, "USE_EMISSIVE");
    gp.seasons_far = has_define(both, "SEASONS_FAR");
    if (vs.find("CHUNK_ORIGIN_AND_SCALE") != std::string::npos) {
        if (has_define(both, "AS_ENTITY_RENDERER")) gp.kind = kKTerrainEntity;
        else if (has_define(both, "BLEND")) gp.kind = kKWater;
        else if (has_define(both, "ALPHA_TEST")) gp.kind = kKCutout;
        else gp.kind = kKTerrain;
    } else if (vs.find("TILE_LIGHT_COLOR") != std::string::npos && gp.attr.normal >= 0) {
        gp.kind = has_define(both, "GLINT") ? kKGlint : kKEntity;
        gp.hand_item = has_define(both, "ITEM_IN_HAND");
        gp.multitexture = has_define(both, "USE_MULTITEXTURE");
        gp.no_texture = has_define(both, "NO_TEXTURE");
        gp.color_mask = has_define(both, "USE_COLOR_MASK");
        gp.color_second = has_define(both, "COLOR_SECOND_TEXTURE");
    } else if (vs.find("mix( CURRENT_COLOR, FOG_COLOR") != std::string::npos) {
        gp.kind = kKSky;
    } else if (vs.find("inverseLightDirection") != std::string::npos) {
        gp.kind = kKClouds;
    } else if (vs.find("PARTICLE_BOX") != std::string::npos || vs.find("TEXCOORD_0.y * 10.0") != std::string::npos) {
        gp.kind = kKWeather;
    } else if (gp.u_worldview >= 0 && gp.u_proj >= 0) {
        gp.kind = kKOverlay;
    } else if (gp.u_wvp >= 0) {
        gp.kind = gp.attr.tex0 >= 0 ? kKTextured : kKBasic;
    }
    g_game_programs[prog] = gp;
    static const char* const kn[] = {"other", "terrain", "cutout", "water", "terrain-entity", "entity", "glint", "sky", "clouds",
                                     "weather", "overlay", "textured", "basic"};
    std::string defs;
    for (size_t k = both.find("#define "); k != std::string::npos; k = both.find("#define ", k + 8)) {
        size_t e = both.find_first_of(" \t\r\n", k + 8);
        std::string d = both.substr(k + 8, e - k - 8);
        if (!d.empty() && isupper((unsigned char)d[0]) && defs.find(" " + d + " ") == std::string::npos) defs += " " + d + " ";
    }
    LOGI("shaders: game program %u: %s (attributes %d %d %d %d %d)%s", prog, kn[gp.kind], gp.attr.pos, gp.attr.color, gp.attr.tex0,
         gp.attr.tex1, gp.attr.normal, defs.c_str());
}

void on_delete_program(unsigned prog) {
    g_game_programs.erase(prog);
    for (auto it = g_gbuffer_progs.begin(); it != g_gbuffer_progs.end();) {
        if (it->first.game == prog) {
            if (it->second->id) glDeleteProgram_(it->second->id);
            it = g_gbuffer_progs.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = g_shadow_progs.begin(); it != g_shadow_progs.end();) {
        if (it->first.game == prog) {
            if (it->second->id) glDeleteProgram_(it->second->id);
            it = g_shadow_progs.erase(it);
        } else {
            ++it;
        }
    }
}

void on_use_program(unsigned prog) { g_cur_game_program = prog; }

unsigned on_bind_framebuffer(unsigned target, unsigned fb) {
    if (g_trace_frames > 0) LOGI("trace: bind framebuffer %x %u", target, fb);
    if (target == kFramebuffer || target == kDrawFramebuffer) g_game_fb = fb;
    if (g_f.phase == kWorld && fb == g_main_fb && fb != 0 && (target == kFramebuffer || target == kDrawFramebuffer)) return g_t.fbo;
    return fb;
}

void on_framebuffer_renderbuffer(unsigned attachment, unsigned rb) {
    if (attachment == kColorAttachment0 && rb && rb == g_drawable_rb) {
        g_main_fb = g_game_fb;
        LOGI("shaders: the game's framebuffer is %u", g_main_fb);
    }
}

bool on_viewport(int x, int y, int w, int h) {
    if (g_f.phase != kWorld || g_game_fb != g_main_fb) return false;
    g_game_vp[0] = x, g_game_vp[1] = y, g_game_vp[2] = w, g_game_vp[3] = h;
    if (g_t.w == g_drawable_w && g_t.h == g_drawable_h) return false;
    scaled_viewport(g_game_vp);
    return true;
}

void set_drawable(unsigned rb, int w, int h) {
    g_drawable_rb = rb;
    g_drawable_w = w;
    g_drawable_h = h;
}

bool on_clear(unsigned mask) {
    if (g_trace_frames > 0) LOGI("trace: clear %x fb %u phase %d", mask, g_game_fb, (int)g_f.phase);
    if (g_f.phase != kWorld || g_game_fb != g_main_fb) return false;
    if ((mask & GL_DEPTH_BUFFER_BIT) && !(mask & GL_COLOR_BUFFER_BIT) && g_f.terrain_seen) {
        // the depth cleared for the hand: kept (the hand is drawn nearer, OptiFine-like)
        if (!g_f.hand) {
            copy_depth(2);
            if (!g_f.translucent_seen) copy_depth1(), g_f.translucent_seen = true;
        }
        g_f.hand = true;
        if (mask & GL_STENCIL_BUFFER_BIT) {  // (the camera's entity's bit kept)
            GLint sm = 0;
            glGetIntegerv(GL_STENCIL_WRITEMASK, &sm);
            glStencilMask(sm & ~kCamStencil);
            glClear(GL_STENCIL_BUFFER_BIT);
            glStencilMask(sm);
        }
        return true;
    }
    // other clears: of colortex0 and the depth
    GLenum db = kColorAttachment0;
    glDrawBuffers_(1, &db);
    return false;
}

// ---- the video settings applied to the game's draws (with or without a shader pack) ----
bool g_after_sky = false;  // the last draw was the sky's plane, or the sun, the moon or the stars drawn after it
struct TexOrig {
    GLint min_filter = 0, max_level = 1000;
    bool mips = false;  // the game made its levels (they are kept: not made again here)
};
std::unordered_map<GLuint, TexOrig> g_tex_orig;  // the textures' own filtering
GLuint g_tex_applied = 0;
unsigned g_tex_applied_gen = 0;
uint64_t g_tex_mip_frame = ~0ull;

// Mipmap Levels / Type, Anisotropic Filtering: on the terrain's atlas (the defaults: as the game has it)
void apply_texture_settings(GLuint tex) {
    bool def = g_vs.mip_levels == 3 && g_vs.mip_type == 1 && g_vs.af == 0;  // (the game's: 3 levels, GL_NEAREST_MIPMAP_LINEAR)
    auto it = g_tex_orig.find(tex);
    if (tex == g_tex_applied && g_tex_applied_gen == g_vs.gen) {
        // levels made here: again each frame (the animated textures change the atlas)
        if (!def && g_vs.mip_levels > 0 && it != g_tex_orig.end() && !it->second.mips && g_tex_mip_frame != g_f.frame) {
            g_tex_mip_frame = g_f.frame;
            glGenerateMipmap_(GL_TEXTURE_2D);
        }
        return;
    }
    g_tex_applied = tex, g_tex_applied_gen = g_vs.gen;
    if (it == g_tex_orig.end()) {
        TexOrig o;
        GLint w1 = 0;
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &o.min_filter);
        glGetTexParameteriv(GL_TEXTURE_2D, kTextureMaxLevel, &o.max_level);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_WIDTH, &w1);
        o.mips = w1 > 0;
        it = g_tex_orig.emplace(tex, o).first;
        LOGI("video settings: the terrain's texture %u: min filter %x, max level %d, mipmaps %s", tex, o.min_filter, o.max_level,
             o.mips ? "yes" : "no");
    }
    const TexOrig& o = it->second;
    if (def) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, o.min_filter);
        glTexParameteri(GL_TEXTURE_2D, kTextureMaxLevel, o.max_level);
        glTexParameterf(GL_TEXTURE_2D, 0x84FE, 1.0f);  // GL_TEXTURE_MAX_ANISOTROPY
        return;
    }
    static const GLint types[] = {GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_NEAREST, kLinearMipmapLinear};
    if (g_vs.mip_levels == 0) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, g_vs.mip_type >= 2 ? GL_LINEAR : GL_NEAREST);
    } else {
        int levels = g_vs.mip_levels >= 4 ? 1000 : g_vs.mip_levels;
        if (o.mips) {
            levels = std::min(levels, o.max_level);  // (the game's levels only)
        } else {
            glGenerateMipmap_(GL_TEXTURE_2D);
            g_tex_mip_frame = g_f.frame;
        }
        glTexParameteri(GL_TEXTURE_2D, kTextureMaxLevel, levels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, types[std::clamp(g_vs.mip_type, 0, 3)]);
    }
    static const float af[] = {1, 2, 4, 8, 16};
    glTexParameterf(GL_TEXTURE_2D, 0x84FE, af[std::clamp(g_vs.af, 0, 4)]);
}

// ---- Animations: the animated textures' frames are drawn into the terrain's atlas each frame, one draw per entry of
// textures/flipbook_textures.json, in its order; an animation turned off: its draws are left out (its picture stays)
std::vector<std::string> g_flipbooks;  // the atlas tiles, in order
bool g_flipbooks_read = false;
int g_flip_draw = 0, g_flip_draws_last = -1;  // the atlas draws in this frame / the last one
std::unordered_map<GLuint, GLuint> g_fb_texture;  // framebuffer -> its colour texture

bool flipbook_enabled(const std::string& tile) {
    if (tile.rfind("fire", 0) == 0) return g_vs.anim_fire;
    if (tile.find("water") != std::string::npos) return g_vs.anim_water;
    if (tile.find("lava") != std::string::npos) return g_vs.anim_lava;
    if (tile == "portal") return g_vs.anim_portal;
    return g_vs.anim_terrain;
}

// true: an animated texture's frame that is not drawn
bool skip_flipbook_draw() {
    if (g_game_fb == 0 || g_game_fb == g_main_fb || !g_tex_applied) return false;
    auto it = g_fb_texture.find(g_game_fb);
    if (it == g_fb_texture.end()) {
        GLint type = 0, name = 0;
        glGetFramebufferAttachmentParameteriv_(kFramebuffer, kColorAttachment0, 0x8CD0, &type);  // ..._OBJECT_TYPE
        if (type == GL_TEXTURE) glGetFramebufferAttachmentParameteriv_(kFramebuffer, kColorAttachment0, 0x8CD1, &name);  // ..._NAME
        it = g_fb_texture.emplace(g_game_fb, (GLuint)name).first;
    }
    if (it->second != g_tex_applied) return false;  // (not the terrain's atlas)
    int k = g_flip_draw++;
    if (!g_flipbooks_read) {
        g_flipbooks_read = true;
        std::vector<uint8_t> b;
        if (read_file(g_root / L"app" / L"Payload" / L"minecraftpe.app" / L"data" / L"resourcepacks" / L"vanilla" / L"client" / L"textures" /
                          L"flipbook_textures.json",
                      b)) {
            std::string t(b.begin(), b.end());
            for (size_t p = t.find("\"atlas_tile\""); p != std::string::npos; p = t.find("\"atlas_tile\"", p + 1)) {
                size_t q1 = t.find('"', t.find(':', p) + 1), q2 = q1 == std::string::npos ? q1 : t.find('"', q1 + 1);
                if (q2 != std::string::npos) g_flipbooks.push_back(t.substr(q1 + 1, q2 - q1 - 1));
            }
        }
        LOGI("video settings: %zu animated textures", g_flipbooks.size());
    }
    // (only when the draws are the list's: else which is which is not known)
    if (g_flip_draws_last != (int)g_flipbooks.size() || k >= (int)g_flipbooks.size()) return false;
    return !flipbook_enabled(g_flipbooks[k]);
}

// true: not drawn
bool filter_draw(GameProgram& gp, GLuint prog) {
    if (gp.kind == kKTextured && g_game_fb != g_main_fb && skip_flipbook_draw()) return true;
    // Dynamic Lights without a pack: the game's terrain shader (EMU_HAND_LIGHT, see glShaderSource in hle_gl.cpp)
    if (gp.u_hand >= 0) glUniform1f_(gp.u_hand, g_vs.hand_light ? (float)g_held_light.load() : 0.0f);
    switch (gp.kind) {
    case kKTerrain:
    case kKCutout:
    case kKWater: {
        GLint unit = 0, active = 0, tb = 0;
        if (gp.u_tex0 >= 0) glGetUniformiv_(prog, gp.u_tex0, &unit);
        glGetIntegerv(kActiveTextureBinding, &active);
        glActiveTexture_(kTexture0 + unit);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tb);
        if (tb) apply_texture_settings((GLuint)tb);
        glActiveTexture_(active);
        break;
    }
    default: break;
    }
    // the sky: its plane, then the sun and the moon (texture_ccolor), then the stars
    bool after_sky = g_after_sky;
    g_after_sky = gp.kind == kKSky || (after_sky && (gp.sun_moon || gp.stars));
    switch (gp.kind) {
    case kKSky:
        if (!g_vs.sky) return true;
        break;
    case kKClouds:
        if (!g_vs.clouds) return true;
        break;
    case kKWeather:
        if (!g_vs.rain || !g_vs.weather) return true;
        break;
    default: break;
    }
    if (after_sky && gp.stars && !g_vs.stars) return true;
    if (after_sky && gp.sun_moon) {
        if (!g_vs.sun_moon) return true;
        if (g_pack && (g_pack->sun_off || g_pack->moon_off)) {  // shaders.properties "sun", "moon"
            // which: the moon's texture has its phases (twice as wide as high), the sun's is square
            GLint unit = 0, active = 0, w = 0, h = 0;
            if (gp.u_tex0 >= 0) glGetUniformiv_(prog, gp.u_tex0, &unit);
            glGetIntegerv(kActiveTextureBinding, &active);
            glActiveTexture_(kTexture0 + unit);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
            glActiveTexture_(active);
            bool moon = w != h;
            if (moon ? g_pack->moon_off : g_pack->sun_off) return true;
        }
    }
    // Fog (OFF), Fog Start: FOG_CONTROL (the fog's start and end, of the render distance); not under water / lava
    if (gp.u_fog_control >= 0) {
        bool want = (!g_vs.fog || g_vs.fog_start != 3) && g_eye.load() == 0;
        if (want || gp.fog_changed) {
            float cur[4];
            get_uniform(prog, gp.u_fog_control, cur, 2);
            if (!gp.fog_changed || cur[0] != gp.fog_set[0] || cur[1] != gp.fog_set[1]) gp.fog_game[0] = cur[0], gp.fog_game[1] = cur[1];
            if (want) {
                static const float starts[] = {0.2f, 0.4f, 0.6f, 0.8f};
                gp.fog_set[0] = g_vs.fog ? starts[std::clamp(g_vs.fog_start, 0, 3)] * gp.fog_game[1] : 1000.0f;
                gp.fog_set[1] = g_vs.fog ? gp.fog_game[1] : 1001.0f;
                glUniform2f_(gp.u_fog_control, gp.fog_set[0], gp.fog_set[1]);
                gp.fog_changed = true;
            } else {
                glUniform2f_(gp.u_fog_control, gp.fog_game[0], gp.fog_game[1]);
                gp.fog_changed = false;
            }
        }
    }
    // Cloud Height: the clouds moved up
    if (gp.kind == kKClouds && gp.u_wvp >= 0 && (g_vs.cloud_height > 0 || gp.wvp_changed)) {
        float cur[16];
        get_uniform(prog, gp.u_wvp, cur, 16);
        if (!gp.wvp_changed || memcmp(cur, gp.wvp_set, sizeof cur) != 0) memcpy(gp.wvp_game, cur, sizeof cur);
        if (g_vs.cloud_height > 0) {
            Mat4 m;
            memcpy(m.m, gp.wvp_game, sizeof m.m);
            m = m * translate(0, g_vs.cloud_height, 0);
            memcpy(gp.wvp_set, m.m, sizeof m.m);
            glUniformMatrix4fv_(gp.u_wvp, 1, GL_FALSE, gp.wvp_set);
            gp.wvp_changed = true;
        } else {
            glUniformMatrix4fv_(gp.u_wvp, 1, GL_FALSE, gp.wvp_game);
            gp.wvp_changed = false;
        }
    }
    return false;
}

bool on_draw(unsigned mode, int first, int count, unsigned type, const void* indices, bool elements) {
    if (!g_ready) return false;
    refresh_video_settings();
    {
        auto gi = g_game_programs.find(g_cur_game_program);
        if (gi != g_game_programs.end() && filter_draw(gi->second, g_cur_game_program)) return true;
    }
    if (g_trace_frames > 0) {
        auto gi = g_game_programs.find(g_cur_game_program);
        LOGI("trace: draw fb %u (main %u) prog %u kind %d phase %d count %d blend %d a2c %d cull %d depthmask %d depthtest %d func %x", g_game_fb, g_main_fb,
             g_cur_game_program, gi == g_game_programs.end() ? -1 : (int)gi->second.kind, (int)g_f.phase, count, glIsEnabled(GL_BLEND),
             glIsEnabled(0x809E), glIsEnabled(GL_CULL_FACE), [] { GLboolean m = 0; glGetBooleanv(GL_DEPTH_WRITEMASK, &m); return (int)m; }(),
             glIsEnabled(GL_DEPTH_TEST), [] { GLint f = 0; glGetIntegerv(GL_DEPTH_FUNC, &f); return f; }());
    }
    if (g_main_fb == 0 || g_game_fb != g_main_fb || g_f.phase == kDone) return false;
    auto gi = g_game_programs.find(g_cur_game_program);
    if (gi == g_game_programs.end()) {
        if (g_f.phase == kWorld) end_world();
        return false;
    }
    GameProgram& gp = gi->second;
    GLuint prog = g_cur_game_program;
    // the matrices of this draw
    float wv[16], pj[16], wvp[16], org[4] = {0, 0, 0, 1};
    bool chunk = gp.u_origin >= 0 && gp.u_worldview >= 0 && gp.u_proj >= 0;
    bool split = gp.u_worldview >= 0 && gp.u_proj >= 0;
    if (split) {
        get_uniform(prog, gp.u_worldview, wv, 16);
        get_uniform(prog, gp.u_proj, pj, 16);
        if (chunk) get_uniform(prog, gp.u_origin, org, 4);
    } else if (gp.u_wvp >= 0) {
        get_uniform(prog, gp.u_wvp, wvp, 16);
    } else {
        if (g_f.phase == kWorld) end_world();
        return false;
    }
    bool persp = is_perspective(split ? pj : wvp);
    if (g_f.phase == kNone) {
        if (!persp || gp.kind == kOther) return false;
        // the world: the sky, the terrain (or anything while the terrain was drawn just before; not the menu's panorama)
        bool world_kind = gp.kind == kKSky || gp.kind == kKTerrain || gp.kind == kKCutout || gp.kind == kKWater;
        if (!world_kind && (g_last_terrain_frame == 0 || g_f.frame > g_last_terrain_frame + 2)) return false;
        if (g_selected.load() < 0 && g_loaded_choice.load() < 0) return false;
        begin_world();
        if (g_f.phase != kWorld) return false;
    }
    if (!persp || gp.kind == kOther) {
        end_world();
        return false;
    }
    // the frame's view (the terrain's)
    if (chunk) g_last_terrain_frame = g_f.frame;
    if (chunk && !g_f.terrain_seen) {
        g_f.terrain_seen = true;
        memcpy(g_f.view.m, wv, sizeof wv);
        memcpy(g_f.proj.m, pj, sizeof pj);
        float a = pj[10], b = pj[14];
        if (fabsf(a - 1) > 1e-6f && fabsf(a + 1) > 1e-6f) g_f.near_ = b / (a - 1), g_f.far_ = b / (a + 1);
        if (g_cam_game_set) memcpy(g_f.cam, g_cam_game, 12), g_f.cam_known = true;
        if (gp.u_fog_color >= 0) get_uniform(prog, gp.u_fog_color, g_f.fog_color, 4);
        float rd[4];
        if (gp.u_render_distance >= 0) get_uniform(prog, gp.u_render_distance, rd, 1), g_f.render_distance = rd[0];
    }
    if (gp.kind == kKSky && gp.u_current_color >= 0) {
        float cc[4];
        get_uniform(prog, gp.u_current_color, cc, 4);
        memcpy(g_f.sky_color, cc, 12);
        if (gp.u_fog_color >= 0) get_uniform(prog, gp.u_fog_color, g_f.fog_color, 4);
    }
    // the stage
    int stage;
    bool cutout = false;
    switch (gp.kind) {
    case kKTerrain: stage = kTerrain; break;
    case kKCutout: stage = kTerrain; cutout = true; break;
    case kKWater: stage = kWater; break;
    case kKTerrainEntity: stage = g_f.hand ? kHand : kBlock; cutout = true; break;
    case kKEntity: stage = g_f.hand ? kHand : kEntities; cutout = true; break;
    case kKGlint: stage = g_f.hand ? kHand : kEntities; cutout = true; break;  // (the glint: a pass after it)
    case kKSky: stage = kSkyBasic; break;
    case kKClouds: stage = kClouds; break;
    case kKWeather: stage = kWeather; break;
    case kKOverlay: stage = kDamagedBlock; break;
    case kKTextured: stage = g_f.terrain_seen ? kTextured : kSkyTextured; cutout = true; break;
    case kKBasic: stage = g_f.terrain_seen ? kBasic : kSkyBasic; break;
    default: stage = kBasic; break;
    }
    if (g_f.hand && stage == kWater) stage = kHandWater;
    if (gp.kind == kKClouds && g_pack->clouds_off) return true;
    if (!g_f.translucent_seen && !g_f.hand) {
        if (gp.kind == kKClouds && !g_f.clouds_early) copy_depth(1), g_f.clouds_early = true;
        else if (g_f.clouds_early && (stage == kTerrain || stage == kBlock || stage == kEntities)) g_f.opaque_after_clouds = true;
    }
    if (gp.kind == kKWeather && gp.u_alpha >= 0) {  // rain_snow: ALPHA is the rain's (snow's) strength
        float a[4];
        get_uniform(prog, gp.u_alpha, a, 4);
        g_rain_alpha = std::max(g_rain_alpha, std::max(std::max(a[0], a[1]), std::max(a[2], a[3])));
        g_weather_drawn = true;
    }
    if (gp.u_fog_control >= 0 && (gp.kind == kKEntity || gp.kind == kKTerrainEntity || gp.kind == kKTextured)) {
        float fc[4];
        get_uniform(prog, gp.u_fog_control, fc, 2);
        g_fog_control[0] = fc[0], g_fog_control[1] = fc[1];
    }
    // drawn as the game draws them, into colortex0 only: the block selection (its overlay and outline), the entities'
    // shadows (stencil volumes and the darkening over them), lines; the pack's programs would overwrite its other buffers
    if (gp.kind == kKBasic && g_f.terrain_seen && g_pack->shadow.present && mode >= 4 /* triangles */) return true;
    if (gp.kind == kKOverlay || (gp.kind == kKBasic && g_f.terrain_seen)) {
        GLenum db = kColorAttachment0;
        glDrawBuffers_(1, &db);
        if (g_f.hand) glDepthRange(0, 0.1 * g_vs.hand_depth);  // (Hand Depth)
        if (elements) glDrawElements(mode, count, type, indices);
        else glDrawArrays(mode, first, count);
        if (g_f.hand) glDepthRange(0, 1);
        return true;
    }
    if ((stage == kWater) && !g_f.translucent_seen) {
        g_f.translucent_seen = true;
        copy_depth1();
        draw_deferred_water();
    }
    Program* pp = gbuffers_program(stage, prog, gp);
    if (!pp) {  // no program: the game's, into colortex0
        GLenum db = kColorAttachment0;
        glDrawBuffers_(1, &db);
        if (g_f.hand) glDepthRange(0, 0.1 * g_vs.hand_depth);  // (Hand Depth)
        if (elements) glDrawElements(mode, count, type, indices);
        else glDrawArrays(mode, first, count);
        if (g_f.hand) glDepthRange(0, 1);
        return true;
    }
    // matrices
    Mat4 mv, p;
    if (split) {
        memcpy(p.m, pj, sizeof pj);
        memcpy(mv.m, wv, sizeof wv);
        if (chunk) mv = mv * translate(org[0], org[1], org[2]) * scale(org[3], org[3], org[3]);
    } else {
        p = g_f.proj;
        Mat4 w;
        memcpy(w.m, wvp, sizeof wvp);
        mv = inverse(p) * w;
    }
    GLint active = 0, abuf = 0;
    glGetIntegerv(kActiveTextureBinding, &active);
    glGetIntegerv(kArrayBufferBinding, &abuf);
    glUseProgram_(pp->id);
    set_frame_uniforms(*pp, true);
    // per draw
    GLint tex0_unit = 0, tex1_unit = 1;
    if (gp.u_tex0 >= 0) glGetUniformiv_(prog, gp.u_tex0, &tex0_unit);
    if (gp.u_tex1 >= 0) glGetUniformiv_(prog, gp.u_tex1, &tex1_unit);
    GLint albedo_unit = tex0_unit;
    if (gp.no_texture) {
        bind_tex(kUnitDepthG + 1, g_t.white);
        glActiveTexture_(active);
        albedo_unit = kUnitDepthG + 1;
    }
    if ((gp.kind == kKEntity || gp.kind == kKGlint) && (gp.multitexture || gp.color_mask)) {
        GLint u1 = 1, u2 = 2;
        if (gp.u_tex1 >= 0) glGetUniformiv_(prog, gp.u_tex1, &u1);
        if (gp.u_tex2 >= 0) glGetUniformiv_(prog, gp.u_tex2, &u2);
        float change[4] = {1, 1, 1, 1};
        if (gp.u_change_color >= 0) get_uniform(prog, gp.u_change_color, change, 4);
        int flags = (gp.multitexture ? 1 : 0) | (gp.color_mask ? 2 : 0) | (gp.color_second ? 4 : 0);
        GLuint t = resolved_texture(bound_texture(tex0_unit), gp.multitexture ? bound_texture(u1) : 0,
                                    gp.multitexture ? bound_texture(u2) : 0, flags, change);
        if (t) {
            bind_tex(kUnitDepthG + 1, t);
            glActiveTexture_(active);
            albedo_unit = kUnitDepthG + 1;
        }
    }
    for (const char* n : {"texture", "tex", "gtexture"})
        if (GLint l = pp->loc(n); l >= 0) glUniform1i_(l, albedo_unit);
    if (GLint l = pp->loc("lightmap"); l >= 0) glUniform1i_(l, gp.attr.tex1 >= 0 ? tex1_unit : kUnitSpecular + 0);
    bool lm = gp.attr.tex1 >= 0;
    glUniform4f_(pp->loc("emu_LmScale"), lm ? 256.0f : 1.0f, lm ? 256.0f : 1.0f, 1, 1);
    glUniform4f_(pp->loc("emu_LmBias"), lm ? -8.0f : 0.0f, lm ? -8.0f : 0.0f, 0, 0);
    {
        bool old = g_vs.old_lighting == 0 ? g_pack->old_lighting : g_vs.old_lighting == 1;
        bool terrain_kind = gp.kind == kKTerrain || gp.kind == kKCutout || gp.kind == kKWater;
        glUniform1f_(pp->loc("emu_Unshade"), terrain_kind && !old ? 1.0f : 0.0f);
    }
    // no lightmap coordinates (entities, the sky, particles): the entities' block and sky light, from the colour the
    // game lights them with (TILE_LIGHT_COLOR: LightTexture::getColorForUV, see on_light_color); else no block light
    // (torches) and the sky's light as bright as that colour
    if (!lm) {
        float tile[4] = {1, 1, 1, 1};
        if (gp.u_tile_light >= 0) get_uniform(prog, gp.u_tile_light, tile, 4);
        int bl = 0, sl = 0;
        if (gp.u_tile_light >= 0 && light_of_color(tile, &bl, &sl))
            glVertexAttrib4f_(kLocLmConst, 16.0f * bl, 16.0f * sl, 0, 1);
        else
            glVertexAttrib4f_(kLocLmConst, 0, 240 * std::clamp(std::max({tile[0], tile[1], tile[2]}), 0.0f, 1.0f), 0, 1);
    }
    if (gp.attr.color < 0) glVertexAttrib4f_(kLocColorConst, 1, 1, 1, 1);
    // the game's sky: its vertex colour is the blend factor between the sky's and the fog's colour (Java: the colour)
    bool const_color = gp.kind == kKSky;
    if (const_color) {
        glDisableVertexAttribArray_(gp.attr.color);
        glVertexAttrib4f_(gp.attr.color, g_f.sky_color[0], g_f.sky_color[1], g_f.sky_color[2], 1);
    }
    if (gp.attr.tex0 < 0) glVertexAttrib4f_(kLocTexConst, 0, 0, 0, 1);
    if (gp.u_overlay >= 0) {
        float ov[4];
        get_uniform(prog, gp.u_overlay, ov, 4);
        set_f(*pp, "entityColor", ov[0], ov[1], ov[2], ov[3]);
    }
    // textures of the pack
    bind_gbuffer_textures(*pp);
    fixed_matrices(mv, p, true);
    glActiveTexture_(active);
    // fog (gl_Fog)
    {
        float fc[4] = {0, 0, 0, 0}, fctl[4] = {0, 1, 0, 0};
        if (gp.u_fog_color >= 0) get_uniform(prog, gp.u_fog_color, fc, 4);
        if (gp.u_fog_control >= 0) get_uniform(prog, gp.u_fog_control, fctl, 2);
        glFogfv(GL_FOG_COLOR, fc);
        glFogi(GL_FOG_MODE, GL_LINEAR);
        glFogf(GL_FOG_START, fctl[0] * g_f.render_distance);
        glFogf(GL_FOG_END, fctl[1] * g_f.render_distance);
    }
    // per-vertex extras
    AttribState pos = read_attrib(gp.attr.pos), t0 = read_attrib(gp.attr.tex0);
    GLuint extra = 0;
    AttribState seasons_col;  // the game's colour attribute replaced by the leaves' colour (restored after the draw)
    const Split* wsplit = nullptr;
    GLint game_ebo = 0;
    bool terrain = gp.kind == kKTerrain || gp.kind == kKCutout || gp.kind == kKWater;
    if (terrain) {
        GLint tb = 0;
        glActiveTexture_(kTexture0 + tex0_unit);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tb);
        glActiveTexture_(active);
        if ((GLuint)tb != g_atlas.tex) {
            if (g_atlas.tex) g_atlas_cache[g_atlas.tex] = g_atlas;
            auto ci = g_atlas_cache.find(tb);
            if (ci != g_atlas_cache.end()) {
                g_atlas = ci->second;
            } else {
                g_atlas = Atlas{};
                g_atlas.tex = tb;
                scan_atlas(tb);
            }
        }
        AttribState col;
        GLuint cmap = 0;
        if (gp.seasons) {
            col = read_attrib(gp.attr.color);
            GLint u2 = 2;
            if (gp.u_tex2 >= 0) glGetUniformiv_(prog, gp.u_tex2, &u2);
            cmap = bound_texture(u2);
        }
        g_last_extras = nullptr;
        extra = get_extras(pos, t0, gp.kind == kKWater ? 9 : 0, gp.seasons ? &col : nullptr, cmap, gp.seasons_far);
        if (extra && g_last_extras && gp.kind == kKWater && elements) {
            GLint ebo = 0;
            glGetIntegerv(kElementArrayBufferBinding, &ebo);
            learn_water(*g_last_extras, ebo, type, (uintptr_t)indices, count);
        }
        if (extra && g_last_extras && (gp.kind == kKTerrain || gp.kind == kKCutout) && elements && !g_f.hand) {
            GLint ebo = 0;
            glGetIntegerv(kElementArrayBufferBinding, &ebo);
            wsplit = split_for(*g_last_extras, ebo, type, (uintptr_t)indices, count);
            if (wsplit) game_ebo = ebo;
        }
        if (g_water_learnt) {  // the water's cells are known now: every buffer's extras again
            g_water_learnt = false;
            for (auto& [k, e2] : g_extras) e2.gen = ~0ull;
        }
        seasons_col = gp.seasons && cmap && extra ? col : AttribState{};
    }

    if (extra) {
        glBindBuffer_(kArrayBuffer, extra);
        glVertexAttribPointer_(kLocNormal, 3, 0x1400, GL_TRUE, sizeof(Extra), (const void*)offsetof(Extra, n));
        glVertexAttribPointer_(kLocTangent, 4, 0x1400, GL_TRUE, sizeof(Extra), (const void*)offsetof(Extra, t));
        glVertexAttribPointer_(kLocMidTex, 2, kFloat, GL_FALSE, sizeof(Extra), (const void*)offsetof(Extra, mid));
        glVertexAttribPointer_(kLocEntity, 2, kFloat, GL_FALSE, sizeof(Extra), (const void*)offsetof(Extra, id));
        for (GLuint l : {kLocNormal, kLocTangent, kLocMidTex, kLocEntity}) glEnableVertexAttribArray_(l);
        if (seasons_col.buffer)
            glVertexAttribPointer_(gp.attr.color, 4, 0x1401, GL_TRUE, sizeof(Extra), (const void*)offsetof(Extra, color));
        glBindBuffer_(kArrayBuffer, abuf);
    } else {
        if (gp.attr.normal < 0) glVertexAttrib4f_(kLocNormal, 0, 1, 0, 0);
        glVertexAttrib4f_(kLocTangent, 1, 0, 0, 1);
        glVertexAttrib4f_(kLocMidTex, 0, 0, 0, 1);
        glVertexAttrib4f_(kLocEntity, stage == kWater ? 9.0f : 0.0f, stage == kWater ? 1.0f : 0.0f, 0, 0);
    }
    set_draw_buffers(pp->drawbuffers);
    // holes: where the game's shader discards (its textures keep some alpha in places it draws, e.g. the sheep's face);
    // the translucent things without it as OptiFine (alpha test 0.1)
    int dmode = discard_mode(gp);
    if (GLint l = pp->loc("emu_discard"); l >= 0) glUniform1i_(l, dmode);
    if (GLint l = pp->loc("emu_albedo"); l >= 0) glUniform1i_(l, albedo_unit);
    if (!gp.alpha_test && (stage == kWater || stage == kHandWater || gp.kind == kKTextured || gp.kind == kKWeather)) {
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 0.1f);
    }
    if (g_f.hand) glDepthRange(0, 0.1 * g_vs.hand_depth);  // (Hand Depth)
    // the camera's entity marked in the stencil (then the draws in front of it unmark it)
    bool cam_entity = (gp.kind == kKEntity || gp.kind == kKTerrainEntity || gp.kind == kKGlint) && !g_f.hand && g_draw_cam_entity.load();
    bool stencil_mark = cam_entity || g_f.cam_marked;
    if (stencil_mark) {
        glPushAttrib(GL_STENCIL_BUFFER_BIT);
        mark_camera_entity(cam_entity);
        if (cam_entity) g_f.cam_marked = true;
    }
    // translucent terrain (water) writes the depth, as in Java (packs compare depthtex0 and depthtex1)
    GLboolean depth_mask = 1;
    bool water = stage == kWater || stage == kHandWater;
    if (water) {
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
        glDepthMask(1);
    }
    if (wsplit) {
        // the land now, the water later
        glBindBuffer_(kElementArrayBuffer, wsplit->land);
        if (wsplit->land_count) glDrawElements(mode, wsplit->land_count, 0x1405, nullptr);
        glBindBuffer_(kElementArrayBuffer, game_ebo);
        WaterDraw wd{};
        wd.game_program = prog;
        wd.pos = pos;
        wd.color = read_attrib(gp.attr.color);
        wd.tex0 = t0;
        wd.tex1 = read_attrib(gp.attr.tex1);
        wd.ebo = wsplit->water;
        wd.extra_vbo = extra;
        wd.count = wsplit->water_count;
        memcpy(wd.origin, org, sizeof org);
        wd.tex0_unit = tex0_unit;
        wd.tex1_unit = tex1_unit;
        wd.atlas = bound_texture(tex0_unit);
        wd.lightmap = bound_texture(tex1_unit);
        if (wd.tex0.buffer && wd.pos.buffer) g_water_draws.push_back(wd);
    } else if (elements) {
        glDrawElements(mode, count, type, indices);
    } else {
        glDrawArrays(mode, first, count);
    }
    if (water) glDepthMask(depth_mask);
    if (stencil_mark) glPopAttrib();
    glDisable(GL_ALPHA_TEST);
    // the enchantment's glint: the game draws it in the same pass (the item's texture + the glint texture, TEXTURE_1,
    // its two layers moving: UV_OFFSET, UV_ROTATION, GLINT_UV_SCALE); Java draws it after the item, with
    // gbuffers_armor_glint: the glint texture, the layers by the texture matrix, blended GL_SRC_COLOR, GL_ONE
    if (gp.kind == kKGlint) {
        Program* gl = gbuffers_program(kArmorGlint, prog, gp);
        if (gl) {
            float off[4] = {0, 0, 0, 0}, rot[4] = {0, 0, 0, 0}, sc[4] = {1, 1, 0, 0}, col[4] = {1, 1, 1, 1}, tile[4] = {1, 1, 1, 1};
            if (gp.u_uv_offset >= 0) get_uniform(prog, gp.u_uv_offset, off, 2);
            if (gp.u_uv_rotation >= 0) get_uniform(prog, gp.u_uv_rotation, rot, 2);
            if (gp.u_glint_scale >= 0) get_uniform(prog, gp.u_glint_scale, sc, 2);
            if (gp.u_glint_color >= 0) get_uniform(prog, gp.u_glint_color, col, 4);
            if (gp.u_tile_light >= 0) get_uniform(prog, gp.u_tile_light, tile, 4);
            glUseProgram_(gl->id);
            set_frame_uniforms(*gl, true);
            for (const char* n : {"texture", "tex", "gtexture"})
                if (GLint l = gl->loc(n); l >= 0) glUniform1i_(l, tex1_unit);
            if (GLint l = gl->loc("lightmap"); l >= 0) glUniform1i_(l, kUnitSpecular);
            glUniform4f_(gl->loc("emu_LmScale"), lm ? 256.0f : 1.0f, lm ? 256.0f : 1.0f, 1, 1);
            glUniform4f_(gl->loc("emu_LmBias"), lm ? -8.0f : 0.0f, lm ? -8.0f : 0.0f, 0, 0);
            if (GLint l = gl->loc("emu_discard"); l >= 0) glUniform1i_(l, 0);
            if (GLint l = gl->loc("emu_albedo"); l >= 0) glUniform1i_(l, tex1_unit);
            bind_gbuffer_textures(*gl);
            set_draw_buffers(gl->drawbuffers);
            // the colour: the glint's, lit as the item (the vertex colour, constant here)
            GLint color_loc = gp.attr.color >= 0 ? gp.attr.color : (GLint)kLocColorConst;
            bool color_array = false;
            if (gp.attr.color >= 0) {
                GLint en = 0;
                glGetVertexAttribiv_(gp.attr.color, kAttribEnabled, &en);
                color_array = en != 0;
                glDisableVertexAttribArray_(gp.attr.color);
            }
            glVertexAttrib4f_(color_loc, col[0] * tile[0], col[1] * tile[1], col[2] * tile[2], 1);
            glPushAttrib(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_ENABLE_BIT);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_COLOR, GL_ONE);
            glDepthMask(0);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            glMatrixMode(GL_TEXTURE);
            glActiveTexture_(kTexture0);
            for (int layer = 0; layer < 2; layer++) {
                // uv' = scale * (R(rotation) (uv - 0.5) + (offset, 0) + 0.5)   (R: the game's mat2(c, -s, s, c))
                float c = cosf(rot[layer]), sn = sinf(rot[layer]);
                float m[16] = {sc[0] * c, -sc[1] * sn, 0, 0, sc[0] * sn, sc[1] * c, 0, 0, 0, 0, 1, 0,
                               sc[0] * (0.5f - 0.5f * c - 0.5f * sn + off[layer]), sc[1] * (0.5f + 0.5f * sn - 0.5f * c), 0, 1};
                glLoadMatrixf(m);
                if (elements) glDrawElements(mode, count, type, indices);
                else glDrawArrays(mode, first, count);
            }
            glLoadIdentity();
            glMatrixMode(GL_MODELVIEW);
            glActiveTexture_(active);
            glPopAttrib();
            if (color_array) glEnableVertexAttribArray_(gp.attr.color);
            glUseProgram_(pp->id);
            set_draw_buffers(pp->drawbuffers);
        }
    }
    if (g_f.hand) glDepthRange(0, 1);
    // entities cast the pack's shadows: drawn into the shadow map now (their buffers may change later in the frame)
    if ((gp.kind == kKEntity || gp.kind == kKTerrainEntity || gp.kind == kKGlint) && !g_f.hand && g_f.terrain_seen && g_pack->shadow.present &&
        g_t.shadow_fbo) {
        if (Program* sp = shadow_program(prog, gp)) {
            glPushAttrib(GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT | GL_VIEWPORT_BIT | GL_POLYGON_BIT);
            glBindFramebuffer_(kFramebuffer, g_t.shadow_fbo);
            glViewport(0, 0, g_t.shadow_res, g_t.shadow_res);
            set_draw_buffers(g_pack->shadow.drawbuffers);
            glDisable(GL_BLEND);
            glDisable(GL_CULL_FACE);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_STENCIL_TEST);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            glDepthMask(1);
            glColorMask(1, 1, 1, 1);
            glUseProgram_(sp->id);
            set_frame_uniforms(*sp, true);
            for (const char* n : {"texture", "tex", "gtexture"})
                if (GLint l = sp->loc(n); l >= 0) glUniform1i_(l, albedo_unit);
            glUniform4f_(sp->loc("emu_LmScale"), lm ? 256.0f : 1.0f, lm ? 256.0f : 1.0f, 1, 1);
            glUniform4f_(sp->loc("emu_LmBias"), lm ? -8.0f : 0.0f, lm ? -8.0f : 0.0f, 0, 0);
            if (GLint l = sp->loc("emu_discard"); l >= 0) glUniform1i_(l, dmode);
            if (GLint l = sp->loc("emu_albedo"); l >= 0) glUniform1i_(l, albedo_unit);
            glVertexAttrib4f_(kLocEntity, 0, 0, 0, 0);
            Mat4 smv = g_f.shadow_view * inverse(g_f.view) * mv;
            fixed_matrices(smv, g_f.shadow_proj, true);
            glActiveTexture_(active);
            if (elements) glDrawElements(mode, count, type, indices);
            else glDrawArrays(mode, first, count);
            glBindFramebuffer_(kFramebuffer, g_t.fbo);
            glPopAttrib();
        }
    }
    if (extra)
        for (GLuint l : {kLocNormal, kLocTangent, kLocMidTex, kLocEntity}) glDisableVertexAttribArray_(l);
    if (const_color) glEnableVertexAttribArray_(gp.attr.color);
    if (seasons_col.buffer) {
        glBindBuffer_(kArrayBuffer, seasons_col.buffer);
        glVertexAttribPointer_(gp.attr.color, seasons_col.size, seasons_col.type, (GLboolean)seasons_col.normalized, seasons_col.stride,
                               (const void*)seasons_col.offset);
        glBindBuffer_(kArrayBuffer, abuf);
    }
    glUseProgram_(prog);
    // remember the terrain draw for the shadow pass
    if (elements && (gp.kind == kKTerrain || gp.kind == kKCutout) && !g_f.hand && pos.buffer) {
        GLint ebo = 0;
        glGetIntegerv(kElementArrayBufferBinding, &ebo);
        if (ebo) {
            TerrainDraw d{};
            d.game_program = prog;
            d.pos = pos;
            d.color = read_attrib(gp.attr.color);
            d.tex0 = t0;
            d.tex1 = read_attrib(gp.attr.tex1);
            d.ebo = ebo;
            d.mode = mode;
            d.type = type;
            d.count = count;
            d.indices = (uintptr_t)indices;
            memcpy(d.origin, org, sizeof org);
            d.atlas = g_atlas.tex;
            d.extra_vbo = extra;
            d.cutout = cutout;
            g_terrain_draws.push_back(d);
        }
    }
    return true;
}

void purge_recorded(const std::set<GLuint>& gone);

void on_buffer_changed(unsigned target) {
    if (target != kArrayBuffer) return;
    GLint b = 0;
    glGetIntegerv(kArrayBufferBinding, &b);
    if (b) {
        g_buffer_gen[b] = ++g_gen_counter;
        if (g_f.phase == kWorld) purge_recorded({(GLuint)b});  // (new contents, maybe smaller: not drawn again)
    }
}

void on_delete_buffers(int n, const unsigned* ids) {
    std::set<GLuint> gone;
    for (int i = 0; i < n; i++) {
        if (!ids[i]) continue;
        gone.insert(ids[i]);
        g_buffer_gen.erase(ids[i]);
        for (auto it = g_extras.begin(); it != g_extras.end();) {
            if (it->first.vbo == ids[i]) {
                if (it->second.vbo) {
                    gone.insert(it->second.vbo);
                    for (auto sp = g_splits.begin(); sp != g_splits.end();) {
                        if (sp->first.extras == it->second.vbo) {
                            glDeleteBuffers_(1, &sp->second.land);
                            glDeleteBuffers_(1, &sp->second.water);
                            gone.insert(sp->second.land);
                            gone.insert(sp->second.water);
                            sp = g_splits.erase(sp);
                        } else {
                            ++sp;
                        }
                    }
                    glDeleteBuffers_(1, &it->second.vbo);
                }
                if (g_last_extras == &it->second) g_last_extras = nullptr;
                it = g_extras.erase(it);
            } else {
                ++it;
            }
        }
    }
    purge_recorded(gone);
}

void purge_recorded(const std::set<GLuint>& gone) {
    if (gone.empty()) return;
    // the frame's recorded draws are drawn again later (shadows, water): not with a deleted buffer (the driver would
    // read a new, empty buffer of that name)
    auto uses = [&](const AttribState& a) { return a.enabled && gone.count((GLuint)a.buffer); };
    g_terrain_draws.erase(std::remove_if(g_terrain_draws.begin(), g_terrain_draws.end(),
                                         [&](const TerrainDraw& d) {
                                             return uses(d.pos) || uses(d.color) || uses(d.tex0) || uses(d.tex1) ||
                                                    gone.count(d.ebo) || gone.count(d.extra_vbo);
                                         }),
                          g_terrain_draws.end());
    g_water_draws.erase(std::remove_if(g_water_draws.begin(), g_water_draws.end(),
                                       [&](const WaterDraw& d) {
                                           return uses(d.pos) || uses(d.color) || uses(d.tex0) || uses(d.tex1) || gone.count(d.ebo) ||
                                                  gone.count(d.extra_vbo);
                                       }),
                        g_water_draws.end());
}

void update_weather() {
    float dt = (float)g_f.frame_time;
    if (g_debug_norain) g_weather_drawn = false, g_rain = 0, g_f.wetness = 0;
    float target = g_weather_drawn ? std::clamp(g_rain_alpha * 1.5f, 0.0f, 1.0f) : 0.0f;
    g_rain_alpha_shown = g_rain_alpha;
    g_rain_alpha = 0;
    g_rain += std::clamp(target - g_rain, -dt / 3.0f, dt / 3.0f);  // ~3 s
    float hl = g_pack ? (g_rain > g_f.wetness ? g_pack->wetness_halflife : g_pack->dryness_halflife) : 10;
    float k = hl > 0 ? 1.0f - expf(-dt * 0.693f / hl) : 1.0f;
    g_f.wetness += (g_rain - g_f.wetness) * k;
    g_weather_drawn = false;
}

void before_present() {
    if (g_trace_frames > 0) LOGI("trace: present (frame %llu)", (unsigned long long)g_f.frame), g_trace_frames--;
    g_after_sky = false;
    g_flip_draws_last = g_flip_draw;
    g_flip_draw = 0;
    {
        static double next = 0;
        double now = seconds_now();
        if (now >= next) {
            next = now + 1;
            std::vector<uint8_t> b;
            if (read_file(g_root / L"shader_debug.txt", b)) {
                std::string t(b.begin(), b.end());
                size_t k = t.find("trace=");
                static bool done = false;
                if (k != std::string::npos && !done) g_trace_frames = atoi(t.c_str() + k + 6), done = true;
            }
        }
    }
    if (g_f.phase == kWorld) {
        GLint fb = 0;
        glGetIntegerv(kDrawFramebufferBinding, &fb);
        end_world();
        glBindFramebuffer_(kFramebuffer, fb == (GLint)g_t.fbo ? g_main_fb : fb);
    }
    if (g_f.phase != kNone) {
        update_weather();
        g_f.prev_view = g_f.view;
        g_f.prev_proj = g_f.proj;
        memcpy(g_f.prev_cam, g_f.cam, sizeof g_f.cam);
    }
    g_f.phase = kNone;
    g_f.frame++;
    if (g_selected.load() < 0 && g_pack) apply_choice();  // "Default": the pack's resources are freed (a pack is loaded by the next world frame)
}

void set_dimension(int dim) { g_dimension = dim; }
void set_held_block(int id, int light) { g_held_block = id, g_held_light = light; }

void on_light_color(int block, int sky, const float color[4]) {
    std::lock_guard<std::mutex> l(g_light_mutex);
    LightColor& e = g_light_colors[g_light_next++ % kLightColors];
    e.block = block, e.sky = sky;
    memcpy(e.color, color, sizeof e.color);
}

void set_draw_entity(bool camera_entity) { g_draw_cam_entity = camera_entity; }

void set_camera(float x, float y, float z, int eye) {
    g_eye = eye;
    g_cam_game[0] = x, g_cam_game[1] = y, g_cam_game[2] = z;
    g_cam_game_set = true;
}

float time_of_day(int time, float partial, bool nether) {
    g_tod_frame = g_f.frame;
    // Time (Other Settings): day only / night only (the sky, the sun and the moon, the light: noon / midnight)
    refresh_video_settings();
    if (!nether && g_vs.time) time = (time / 24000) * 24000 + (g_vs.time == 1 ? 6000 : 18000), partial = 0;
    float v =((float)(time % 24000) + partial) / 24000.0f;
    float t = v - 0.25f;
    if (t < 0) t = v + 0.75f;
    if (t > 1) t -= 1;
    float r = t + ((1 - t) - (cosf(3.14159265f * t) + 1) * 0.5f) * (1.0f / 3);
    if (!nether) {
        g_world_time = time;
        g_celestial = r;
    }
    return r;
}

}  // namespace shaderpack
