// Metal on the emulator: the interface between the guest Metal framework (guest/Metal,
// Objective-C classes) and the host implementation on OpenGL 4.6 (hle_metal.cpp).
// Plain C, included by both sides. Handles are host pointers (0 = none).
#pragma once
#include <stdint.h>

#pragma pack(push, 8)
typedef struct emu_mtl_tex_desc {
    uint32_t type;     // MTLTextureType
    uint32_t format;   // MTLPixelFormat
    uint32_t width, height, depth;
    uint32_t mips, array_length, samples;
    uint32_t usage, storage;
} emu_mtl_tex_desc;

typedef struct emu_mtl_sampler_desc {
    uint32_t min_filter, mag_filter, mip_filter;
    uint32_t s, t, r;  // MTLSamplerAddressMode
    uint32_t max_aniso, compare, border, normalized;
    float lod_min, lod_max;
} emu_mtl_sampler_desc;

typedef struct emu_mtl_stencil {
    uint32_t fail, depth_fail, pass, func, read_mask, write_mask;
} emu_mtl_stencil;

typedef struct emu_mtl_ds_desc {
    uint32_t depth_func, depth_write, has_front, has_back;
    emu_mtl_stencil front, back;
} emu_mtl_ds_desc;

typedef struct emu_mtl_color_att {
    uint32_t format, blend, rgb_op, alpha_op, src_rgb, dst_rgb, src_a, dst_a, write_mask;
} emu_mtl_color_att;

typedef struct emu_mtl_vattr { uint32_t format, offset, buffer; } emu_mtl_vattr;
typedef struct emu_mtl_vlayout { uint32_t stride, step_fn, step_rate; } emu_mtl_vlayout;

typedef struct emu_mtl_rp_desc {
    uint64_t vfn, ffn;  // function handles
    emu_mtl_color_att color[8];
    uint32_t depth_format, stencil_format, samples, alpha_to_coverage, has_vdesc, raster_enabled;
    emu_mtl_vattr attr[31];
    emu_mtl_vlayout layout[31];
} emu_mtl_rp_desc;

typedef struct emu_mtl_pass_att {
    uint64_t tex, resolve;
    uint32_t level, slice, depth_plane, load, store, resolve_level, resolve_slice, _pad;
    double clear[4];  // color; depth in [0], stencil in [1]
} emu_mtl_pass_att;

typedef struct emu_mtl_pass_desc {
    emu_mtl_pass_att color[8];
    emu_mtl_pass_att depth, stencil;
    uint32_t width, height, layers, samples;
} emu_mtl_pass_desc;

// reflection (MTLRenderPipelineReflection / MTLComputePipelineReflection)
typedef struct emu_mtl_refl_member {
    const char* name;
    uint32_t offset, data_type, array_len, stride, size, _pad;
} emu_mtl_refl_member;

typedef struct emu_mtl_refl_arg {
    const char* name;
    uint32_t type;        // MTLArgumentType: 0 buffer, 2 texture, 3 sampler
    uint32_t index, active;
    uint32_t data_type;   // buffers: MTLDataTypeStruct (or the element type)
    uint32_t data_size, alignment;
    uint32_t texture_type, texture_data_type, access;
    uint32_t nmembers;
    const emu_mtl_refl_member* members;
} emu_mtl_refl_arg;

typedef struct emu_mtl_refl {
    uint32_t nvertex, nfragment;
    const emu_mtl_refl_arg* vertex;    // compute: the kernel's arguments
    const emu_mtl_refl_arg* fragment;
    uint32_t threads_per_group_max, thread_width;
} emu_mtl_refl;
#pragma pack(pop)

#ifdef __cplusplus
extern "C" {
#endif
// device
int emu_mtl_available(void);
// buffers: *contents = CPU address of the storage (guest-visible)
uint64_t emu_mtl_buffer_new(uint64_t length, const void* bytes, uint32_t options, void** contents);
void emu_mtl_buffer_release(uint64_t b);
void emu_mtl_buffer_modified(uint64_t b, uint64_t offset, uint64_t length);
// textures (region: x, y, z, w, h, d)
uint64_t emu_mtl_texture_new(const emu_mtl_tex_desc* d);
uint64_t emu_mtl_texture_view(uint64_t t, uint32_t format, uint32_t type, uint32_t level0, uint32_t nlevels, uint32_t slice0, uint32_t nslices);
void emu_mtl_texture_release(uint64_t t);
void emu_mtl_texture_replace(uint64_t t, const uint64_t* region, uint32_t level, uint32_t slice, const void* bytes, uint64_t bpr, uint64_t bpi);
void emu_mtl_texture_get(uint64_t t, void* out, uint64_t bpr, uint64_t bpi, const uint64_t* region, uint32_t level, uint32_t slice);
void emu_mtl_texture_info(uint64_t t, emu_mtl_tex_desc* out);
uint64_t emu_mtl_sampler_new(const emu_mtl_sampler_desc* d);
void emu_mtl_sampler_release(uint64_t s);
uint64_t emu_mtl_ds_new(const emu_mtl_ds_desc* d);
void emu_mtl_ds_release(uint64_t s);
// shaders: library = parsed MSL; function = an entry point (0 when absent)
uint64_t emu_mtl_library_new(const char* source, char* err, uint32_t errlen);
void emu_mtl_library_release(uint64_t l);
uint64_t emu_mtl_function_new(uint64_t lib, const char* name);
uint32_t emu_mtl_function_type(uint64_t f);  // MTLFunctionType: 1 vertex, 2 fragment, 3 kernel
// vertex function inputs ([[attribute(n)]]): name i -> *index, *data_type (MTLDataType); returns 0 past the end
int emu_mtl_function_attribute(uint64_t f, uint32_t i, char* name, uint32_t namelen, uint32_t* index, uint32_t* data_type);
void emu_mtl_function_release(uint64_t f);
uint64_t emu_mtl_render_pipeline_new(const emu_mtl_rp_desc* d, emu_mtl_refl* refl, char* err, uint32_t errlen);
uint64_t emu_mtl_compute_pipeline_new(uint64_t fn, emu_mtl_refl* refl, char* err, uint32_t errlen);
void emu_mtl_pipeline_release(uint64_t p);
// the drawable of a CAMetalLayer: a texture of the given size (BGRA8 etc.)
uint64_t emu_mtl_next_drawable(uint32_t w, uint32_t h, uint32_t format);
// render pass (stage: 0 vertex, 1 fragment)
uint64_t emu_mtl_render_begin(const emu_mtl_pass_desc* d);
void emu_mtl_r_pipeline(uint64_t e, uint64_t p);
void emu_mtl_r_buffer(uint64_t e, uint32_t stage, uint32_t index, uint64_t buf, uint64_t offset);
void emu_mtl_r_bytes(uint64_t e, uint32_t stage, uint32_t index, const void* bytes, uint64_t len);
void emu_mtl_r_texture(uint64_t e, uint32_t stage, uint32_t index, uint64_t tex);
void emu_mtl_r_sampler(uint64_t e, uint32_t stage, uint32_t index, uint64_t s);
void emu_mtl_r_viewport(uint64_t e, const double* v);  // x, y, w, h, znear, zfar
void emu_mtl_r_scissor(uint64_t e, uint64_t x, uint64_t y, uint64_t w, uint64_t h);
void emu_mtl_r_state(uint64_t e, uint32_t what, uint32_t value);  // 0 cull, 1 winding, 2 fill, 3 stencil ref, 4 depth clip
void emu_mtl_r_stencil_ref(uint64_t e, uint32_t front, uint32_t back);
void emu_mtl_r_depth_bias(uint64_t e, float bias, float slope, float clamp);
void emu_mtl_r_ds(uint64_t e, uint64_t ds);
void emu_mtl_r_blend_color(uint64_t e, float r, float g, float b, float a);
void emu_mtl_r_draw(uint64_t e, uint32_t prim, uint64_t start, uint64_t count, uint64_t instances, uint64_t base_instance);
void emu_mtl_r_draw_indexed(uint64_t e, uint32_t prim, uint64_t count, uint32_t itype, uint64_t ibuf, uint64_t ioffset,
                            uint64_t instances, int64_t base_vertex, uint64_t base_instance);
void emu_mtl_r_end(uint64_t e);
// blit
void emu_mtl_b_copy_buffer(uint64_t src, uint64_t soff, uint64_t dst, uint64_t doff, uint64_t size);
void emu_mtl_b_buffer_to_texture(uint64_t src, uint64_t off, uint64_t bpr, uint64_t bpi, const uint64_t* size3, uint64_t dst,
                                 uint32_t slice, uint32_t level, const uint64_t* origin3);
void emu_mtl_b_texture_to_buffer(uint64_t src, uint32_t slice, uint32_t level, const uint64_t* origin3, const uint64_t* size3,
                                 uint64_t dst, uint64_t off, uint64_t bpr, uint64_t bpi);
void emu_mtl_b_texture_to_texture(uint64_t src, uint32_t sslice, uint32_t slevel, const uint64_t* sorigin3, const uint64_t* size3,
                                  uint64_t dst, uint32_t dslice, uint32_t dlevel, const uint64_t* dorigin3);
void emu_mtl_b_mipmaps(uint64_t tex);
void emu_mtl_b_fill(uint64_t buf, uint64_t off, uint64_t len, uint32_t value);
// compute
void emu_mtl_c_pipeline(uint64_t p);
void emu_mtl_c_buffer(uint32_t index, uint64_t buf, uint64_t offset);
void emu_mtl_c_bytes(uint32_t index, const void* bytes, uint64_t len);
void emu_mtl_c_texture(uint32_t index, uint64_t tex);
void emu_mtl_c_sampler(uint32_t index, uint64_t s);
void emu_mtl_c_dispatch(uint64_t gx, uint64_t gy, uint64_t gz, uint64_t tx, uint64_t ty, uint64_t tz);
// command buffers: the commands run when they are encoded. commit returns a ticket (a GPU fence).
// next_completed (the guest's completion thread) blocks until the oldest pending ticket has
// completed and returns it; wait_completed blocks until ticket `t` has completed.
uint64_t emu_mtl_commit(void);
uint64_t emu_mtl_next_completed(void);
void emu_mtl_wait_completed(uint64_t t);
void emu_mtl_present(uint64_t drawable_tex);
void emu_mtl_label(uint64_t obj, const char* label);
#ifdef __cplusplus
}
#endif
