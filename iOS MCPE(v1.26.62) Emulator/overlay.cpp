// Mod overlay (see overlay.h): draws the mods' triangle lists into the game's frame with
// OpenGL right before the frame is shown (so the phone and screenshots get it too) and
// routes pointer / key input on the mods' UI to them.
#include "overlay.h"
#include <cstring>

#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>

#include <deque>
#include <map>
#include <mutex>
#include <vector>

namespace overlay {
namespace {

std::mutex g_lock;
// draw data (latest frame of the mod)
std::vector<Vertex> g_vtx;
std::vector<u32> g_idx;
std::vector<Cmd> g_cmds;
int g_fb_w = 0, g_fb_h = 0;      // the frame size the mod drew for
int g_cur_w = 0, g_cur_h = 0;    // the game's frame size (last present)
double g_submit_time = 0;
struct Tex {
    std::vector<u8> px;
    int w = 0, h = 0;
    bool dirty = false;
    GLuint gl = 0;
};
std::map<int, Tex> g_tex;
// input routing
std::vector<float> g_rects;  // x0, y0, x1, y1 ...
u32 g_flags = 0;
std::deque<InputEvent> g_events;
bool g_owned[128];  // pointers that went down on the overlay (their moves / ups are the overlay's)

double now_s() {
    static LARGE_INTEGER f{}, t0{};
    if (!f.QuadPart) {
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
    }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart - t0.QuadPart) / double(f.QuadPart);
}

bool inside(float x, float y) {
    for (size_t i = 0; i + 3 < g_rects.size(); i += 4)
        if (x >= g_rects[i] && y >= g_rects[i + 1] && x < g_rects[i + 2] && y < g_rects[i + 3]) return true;
    return false;
}

void queue(u32 type, s32 id, float x, float y, u32 chr = 0, u32 mods = 0) {
    if (g_events.size() >= 512) g_events.pop_front();
    g_events.push_back({type, id, x, y, chr, mods});
}

}  // namespace

void submit(const Vertex* v, int nv, const u32* idx, int ni, const Cmd* cmds, int nc, int fb_w, int fb_h) {
    std::lock_guard lk(g_lock);
    g_vtx.assign(v, v + std::max(nv, 0));
    g_idx.assign(idx, idx + std::max(ni, 0));
    g_cmds.assign(cmds, cmds + std::max(nc, 0));
    g_fb_w = fb_w;
    g_fb_h = fb_h;
    g_submit_time = now_s();
}

void set_texture(int id, const void* rgba, int w, int h) {
    std::lock_guard lk(g_lock);
    Tex& t = g_tex[id];
    if (!rgba || w <= 0 || h <= 0) {
        t.px.clear();
        t.w = t.h = 0;
        t.dirty = true;
        return;
    }
    t.px.assign((const u8*)rgba, (const u8*)rgba + size_t(w) * h * 4);
    t.w = w;
    t.h = h;
    t.dirty = true;
}

void set_capture(const float* rects, int n, u32 flags) {
    std::lock_guard lk(g_lock);
    g_rects.assign(rects, rects + std::max(n, 0) * 4);
    g_flags = flags;
}

int take_events(InputEvent* out, int max) {
    std::lock_guard lk(g_lock);
    int n = 0;
    while (n < max && !g_events.empty()) {
        out[n++] = g_events.front();
        g_events.pop_front();
    }
    return n;
}

void frame_size(int* w, int* h) {
    std::lock_guard lk(g_lock);
    *w = g_cur_w;
    *h = g_cur_h;
}

bool wants_cursor() {
    std::lock_guard lk(g_lock);
    return (g_flags & WantCursor) && now_s() - g_submit_time < 1.0;
}

bool wants_keyboard() {
    std::lock_guard lk(g_lock);
    return (g_flags & WantKeyboard) && now_s() - g_submit_time < 1.0;
}

bool filter(const window::Event& e) {
    std::lock_guard lk(g_lock);
    bool active = (!g_rects.empty() || g_flags) && now_s() - g_submit_time < 1.0;
    using namespace window;
    switch (e.type) {
    case EvPointerDown: {  // phone touches (and the PC mouse with EMU_TOUCH_MENUS)
        int k = 100 + (e.id & 15);
        if (!active || !inside(e.x, e.y)) return false;
        g_owned[k] = true;
        queue(1, k, e.x, e.y);
        return true;
    }
    case EvPointerMove: {
        int k = 100 + (e.id & 15);
        if (e.id < 0) {
            if (active) queue(8, 0, e.x, e.y);
            return false;
        }
        if (!g_owned[k]) return false;
        queue(2, k, e.x, e.y);
        return true;
    }
    case EvPointerUp: {
        int k = 100 + (e.id & 15);
        if (!g_owned[k]) return false;
        g_owned[k] = false;
        queue(3, k, e.x, e.y);
        return true;
    }
    case EvGameMousePos: {  // the PC mouse (game's mouse mode)
        bool held = g_owned[0] || g_owned[1] || g_owned[2];
        if (active || held) queue(held ? 2 : 8, held ? (g_owned[0] ? 0 : g_owned[1] ? 1 : 2) : 0, e.x, e.y);
        return held;
    }
    case EvGameMouseButton: {
        if (e.id >= 1 && e.id <= 3) {
            int k = e.id - 1;
            if (e.chr) {
                if (!active || !inside(e.x, e.y)) return false;
                g_owned[k] = true;
                queue(1, k, e.x, e.y);
                return true;
            }
            if (!g_owned[k]) return false;
            g_owned[k] = false;
            queue(3, k, e.x, e.y);
            return true;
        }
        if (e.id == 4) {  // wheel
            if (!active || !inside(e.x, e.y)) return false;
            queue(4, (s32)e.chr, e.x, e.y);
            return true;
        }
        return false;
    }
    case EvWheel:
        if (!active || !inside(e.x, e.y)) return false;
        queue(4, e.id > 0 ? 1 : -1, e.x, e.y);
        return true;
    case EvKeyDown:
    case EvKeyUp:
    case EvChar:
        if (!active || !(g_flags & WantKeyboard)) return false;
        queue(e.type == EvKeyDown ? 5 : e.type == EvKeyUp ? 6 : 7, e.id, 0, 0, e.chr, e.mods);
        return true;
    default: return false;
    }
}

// ---------------------------------------------------------------- rendering
namespace {
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef intptr_t GLintptr;
#define GLF(ret, name, args) ret(APIENTRY* name) args = nullptr;
GLF(GLuint, glCreateShader_, (GLenum))
GLF(void, glShaderSource_, (GLuint, GLsizei, const GLchar* const*, const GLint*))
GLF(void, glCompileShader_, (GLuint))
GLF(void, glGetShaderiv_, (GLuint, GLenum, GLint*))
GLF(void, glGetShaderInfoLog_, (GLuint, GLsizei, GLsizei*, GLchar*))
GLF(GLuint, glCreateProgram_, ())
GLF(void, glAttachShader_, (GLuint, GLuint))
GLF(void, glLinkProgram_, (GLuint))
GLF(void, glUseProgram_, (GLuint))
GLF(GLint, glGetUniformLocation_, (GLuint, const GLchar*))
GLF(void, glUniformMatrix4fv_, (GLint, GLsizei, GLboolean, const GLfloat*))
GLF(void, glUniform1i_, (GLint, GLint))
GLF(void, glGenVertexArrays_, (GLsizei, GLuint*))
GLF(void, glBindVertexArray_, (GLuint))
GLF(void, glGenBuffers_, (GLsizei, GLuint*))
GLF(void, glBindBuffer_, (GLenum, GLuint))
GLF(void, glBufferData_, (GLenum, GLsizeiptr, const void*, GLenum))
GLF(void, glVertexAttribPointer_, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))
GLF(void, glEnableVertexAttribArray_, (GLuint))
GLF(void, glBlendEquation_, (GLenum))
GLF(void, glBlendFuncSeparate_, (GLenum, GLenum, GLenum, GLenum))
GLF(void, glActiveTexture_, (GLenum))
GLF(void, glBindSampler_, (GLuint, GLuint))
GLF(void, glDrawElementsBaseVertex_, (GLenum, GLsizei, GLenum, const void*, GLint))
GLF(void, glBindFramebuffer_, (GLenum, GLuint))
GLF(void, glClipControl_, (GLenum, GLenum))
GLF(void, glColorMaski_, (GLuint, GLboolean, GLboolean, GLboolean, GLboolean))
#undef GLF

GLuint g_prog = 0, g_vao = 0, g_vbo = 0, g_ibo = 0, g_white = 0;
GLint g_u_proj = -1, g_u_tex = -1;
bool g_gl_failed = false;

const char* kVS = R"(#version 410 core
layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 col;
uniform mat4 proj;
out vec2 f_uv;
out vec4 f_col;
void main() { f_uv = uv; f_col = col; gl_Position = proj * vec4(pos, 0.0, 1.0); }
)";
const char* kFS = R"(#version 410 core
in vec2 f_uv;
in vec4 f_col;
uniform sampler2D tex;
layout(location = 0) out vec4 o;
void main() { o = f_col * texture(tex, f_uv); }
)";

bool gl_init(void* (*proc)(const char*)) {
    if (g_prog) return true;
    if (g_gl_failed) return false;
#define LOAD(n)                                         \
    *(void**)&n##_ = proc(#n);                          \
    if (!n##_ && strcmp(#n, "glClipControl") && strcmp(#n, "glColorMaski")) { \
        LOGW("overlay: %s missing", #n);                 \
        g_gl_failed = true;                             \
        return false;                                   \
    }
    LOAD(glCreateShader) LOAD(glShaderSource) LOAD(glCompileShader) LOAD(glGetShaderiv) LOAD(glGetShaderInfoLog)
    LOAD(glCreateProgram) LOAD(glAttachShader) LOAD(glLinkProgram) LOAD(glUseProgram) LOAD(glGetUniformLocation)
    LOAD(glUniformMatrix4fv) LOAD(glUniform1i) LOAD(glGenVertexArrays) LOAD(glBindVertexArray) LOAD(glGenBuffers)
    LOAD(glBindBuffer) LOAD(glBufferData) LOAD(glVertexAttribPointer) LOAD(glEnableVertexAttribArray)
    LOAD(glBlendEquation) LOAD(glBlendFuncSeparate) LOAD(glActiveTexture) LOAD(glBindSampler)
    LOAD(glDrawElementsBaseVertex) LOAD(glBindFramebuffer) LOAD(glClipControl) LOAD(glColorMaski)
#undef LOAD
    auto sh = [](GLenum type, const char* src) {
        GLuint s = glCreateShader_(type);
        glShaderSource_(s, 1, &src, nullptr);
        glCompileShader_(s);
        GLint ok = 0;
        glGetShaderiv_(s, 0x8B81 /*COMPILE_STATUS*/, &ok);
        if (!ok) {
            char log[1024] = {};
            glGetShaderInfoLog_(s, sizeof log, nullptr, log);
            LOGW("overlay: shader: %s", log);
        }
        return s;
    };
    GLuint p = glCreateProgram_();
    glAttachShader_(p, sh(0x8B31 /*VERTEX*/, kVS));
    glAttachShader_(p, sh(0x8B30 /*FRAGMENT*/, kFS));
    glLinkProgram_(p);
    g_u_proj = glGetUniformLocation_(p, "proj");
    g_u_tex = glGetUniformLocation_(p, "tex");
    glGenVertexArrays_(1, &g_vao);
    glGenBuffers_(1, &g_vbo);
    glGenBuffers_(1, &g_ibo);
    GLint prev_vao = 0, prev_ab = 0;
    glGetIntegerv(0x85B5 /*VERTEX_ARRAY_BINDING*/, &prev_vao);
    glGetIntegerv(0x8894 /*ARRAY_BUFFER_BINDING*/, &prev_ab);
    glBindVertexArray_(g_vao);
    glBindBuffer_(0x8892 /*ARRAY_BUFFER*/, g_vbo);
    glBindBuffer_(0x8893 /*ELEMENT_ARRAY_BUFFER*/, g_ibo);  // (recorded in the VAO)
    glEnableVertexAttribArray_(0);
    glEnableVertexAttribArray_(1);
    glEnableVertexAttribArray_(2);
    glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
    glVertexAttribPointer_(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)8);
    glVertexAttribPointer_(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)16);
    glBindVertexArray_(prev_vao);
    glBindBuffer_(0x8892, prev_ab);
    // 1x1 white texture for untextured triangles
    GLint prev_tex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGenTextures(1, &g_white);
    glBindTexture(GL_TEXTURE_2D, g_white);
    u32 white = 0xffffffff;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, prev_tex);
    g_prog = p;
    LOGI("overlay: renderer ready");
    return true;
}
}  // namespace

void draw(u32 fbo, int w, int h, void* (*proc)(const char*)) {
    std::vector<Vertex> vtx;
    std::vector<u32> idx;
    std::vector<Cmd> cmds;
    std::vector<std::pair<int, Tex*>> uploads;
    {
        std::lock_guard lk(g_lock);
        g_cur_w = w;
        g_cur_h = h;
        if (g_cmds.empty() || now_s() - g_submit_time > 1.0) return;  // nothing (or the mod stopped drawing)
        vtx = g_vtx;
        idx = g_idx;
        cmds = g_cmds;
    }
    if (!gl_init(proc)) return;
    // textures: upload new pixels (the map is only changed under the lock; GL names live here)
    std::map<int, GLuint> texnames;
    {
        std::lock_guard lk(g_lock);
        GLint prev_tex = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
        for (auto& [id, t] : g_tex) {
            if (t.dirty) {
                t.dirty = false;
                if (!t.gl) glGenTextures(1, &t.gl);
                glBindTexture(GL_TEXTURE_2D, t.gl);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glPixelStorei(0x0CF2 /*UNPACK_ROW_LENGTH*/, 0);
                if (t.w > 0) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.w, t.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, t.px.data());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F /*CLAMP_TO_EDGE*/);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
                t.px.clear();
                t.px.shrink_to_fit();
            }
            texnames[id] = t.w > 0 ? t.gl : 0;
        }
        glBindTexture(GL_TEXTURE_2D, prev_tex);
    }
    // save the state the game's renderer (Metal on GL) may rely on
    GLint prev_prog, prev_vao, prev_ab, prev_draw_fb, prev_tex, prev_active, prev_sampler, prev_vp[4], prev_sc[4];
    GLint prev_bsrc_rgb, prev_bdst_rgb, prev_bsrc_a, prev_bdst_a, prev_beq_rgb, prev_beq_a, prev_clip_origin = 0x8CA1, prev_clip_depth = 0x935E;
    GLboolean prev_blend = glIsEnabled(GL_BLEND), prev_cull = glIsEnabled(GL_CULL_FACE), prev_depth = glIsEnabled(GL_DEPTH_TEST),
              prev_scissor = glIsEnabled(GL_SCISSOR_TEST), prev_stencil = glIsEnabled(GL_STENCIL_TEST),
              prev_srgb = glIsEnabled(0x8DB9 /*FRAMEBUFFER_SRGB*/), prev_cmask[4];
    glGetIntegerv(0x8B8D /*CURRENT_PROGRAM*/, &prev_prog);
    glGetIntegerv(0x85B5, &prev_vao);
    glGetIntegerv(0x8894, &prev_ab);
    glGetIntegerv(0x8CA6 /*DRAW_FRAMEBUFFER_BINDING*/, &prev_draw_fb);
    glGetIntegerv(0x84E0 /*ACTIVE_TEXTURE*/, &prev_active);
    glActiveTexture_(0x84C0 /*TEXTURE0*/);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(0x8919 /*SAMPLER_BINDING*/, &prev_sampler);
    glGetIntegerv(GL_VIEWPORT, prev_vp);
    glGetIntegerv(GL_SCISSOR_BOX, prev_sc);
    glGetIntegerv(0x80C9 /*BLEND_SRC_RGB*/, &prev_bsrc_rgb);
    glGetIntegerv(0x80C8 /*BLEND_DST_RGB*/, &prev_bdst_rgb);
    glGetIntegerv(0x80CB /*BLEND_SRC_ALPHA*/, &prev_bsrc_a);
    glGetIntegerv(0x80CA /*BLEND_DST_ALPHA*/, &prev_bdst_a);
    glGetIntegerv(0x8009 /*BLEND_EQUATION_RGB*/, &prev_beq_rgb);
    glGetIntegerv(0x883D /*BLEND_EQUATION_ALPHA*/, &prev_beq_a);
    glGetBooleanv(GL_COLOR_WRITEMASK, prev_cmask);
    if (glClipControl_) {
        glGetIntegerv(0x935C /*CLIP_ORIGIN*/, &prev_clip_origin);
        glGetIntegerv(0x935D /*CLIP_DEPTH_MODE*/, &prev_clip_depth);
        glClipControl_(0x8CA1 /*LOWER_LEFT*/, 0x935E /*NEGATIVE_ONE_TO_ONE*/);
    }

    glBindFramebuffer_(0x8CA9 /*DRAW_FRAMEBUFFER*/, fbo);
    glViewport(0, 0, w, h);
    glEnable(GL_BLEND);
    glBlendEquation_(0x8006 /*FUNC_ADD*/);
    glBlendFuncSeparate_(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(0x8DB9);
    glEnable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram_(g_prog);
    glUniform1i_(g_u_tex, 0);
    glBindSampler_(0, 0);
    // the mod drew for fb_w x fb_h; scale to the actual frame (it catches up on the next frame)
    float sx = g_fb_w > 0 ? (float)w / g_fb_w : 1.0f, sy = g_fb_h > 0 ? (float)h / g_fb_h : 1.0f;
    float L = 0, R = (float)w / sx, T = 0, B = (float)h / sy;
    const float proj[16] = {2.0f / (R - L), 0, 0, 0, 0, 2.0f / (T - B), 0, 0, 0, 0, -1, 0, (R + L) / (L - R), (T + B) / (B - T), 0, 1};
    glUniformMatrix4fv_(g_u_proj, 1, GL_FALSE, proj);
    glBindVertexArray_(g_vao);
    glBindBuffer_(0x8892, g_vbo);
    glBufferData_(0x8892, (GLsizeiptr)(vtx.size() * sizeof(Vertex)), vtx.data(), 0x88E0 /*STREAM_DRAW*/);
    glBufferData_(0x8893, (GLsizeiptr)(idx.size() * sizeof(u32)), idx.data(), 0x88E0);
    for (const Cmd& c : cmds) {
        if (!c.elem_count || size_t(c.idx_offset) + c.elem_count > idx.size()) continue;
        bool ok = true;  // (every index must point into the vertex buffer: the driver would read past it)
        for (u32 k = 0; k < c.elem_count && ok; k++) ok = size_t(idx[c.idx_offset + k]) + c.vtx_offset < vtx.size();
        if (!ok) continue;
        int x0 = (int)(c.clip[0] * sx), y0 = (int)(c.clip[1] * sy), x1 = (int)(c.clip[2] * sx + 0.999f), y1 = (int)(c.clip[3] * sy + 0.999f);
        if (x1 <= x0 || y1 <= y0) continue;
        glScissor(x0, h - y1, x1 - x0, y1 - y0);
        auto it = texnames.find((int)c.texture);
        glBindTexture(GL_TEXTURE_2D, it != texnames.end() && it->second ? it->second : g_white);
        glDrawElementsBaseVertex_(GL_TRIANGLES, (GLsizei)c.elem_count, GL_UNSIGNED_INT, (void*)(uintptr_t)(c.idx_offset * sizeof(u32)),
                                  (GLint)c.vtx_offset);
    }

    // restore
    glBindVertexArray_(prev_vao);
    glBindBuffer_(0x8892, prev_ab);
    glUseProgram_(prev_prog);
    glBindTexture(GL_TEXTURE_2D, prev_tex);
    glBindSampler_(0, prev_sampler);
    glActiveTexture_(prev_active);
    glBindFramebuffer_(0x8CA9, prev_draw_fb);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    glScissor(prev_sc[0], prev_sc[1], prev_sc[2], prev_sc[3]);
    glBlendFuncSeparate_(prev_bsrc_rgb, prev_bdst_rgb, prev_bsrc_a, prev_bdst_a);
    glBlendEquation_(prev_beq_rgb);
    (void)prev_beq_a;
    glColorMask(prev_cmask[0], prev_cmask[1], prev_cmask[2], prev_cmask[3]);
    if (!prev_blend) glDisable(GL_BLEND);
    if (prev_cull) glEnable(GL_CULL_FACE);
    if (prev_depth) glEnable(GL_DEPTH_TEST);
    if (!prev_scissor) glDisable(GL_SCISSOR_TEST);
    if (prev_stencil) glEnable(GL_STENCIL_TEST);
    if (prev_srgb) glEnable(0x8DB9);
    if (glClipControl_) glClipControl_(prev_clip_origin, prev_clip_depth);
}

}  // namespace overlay
