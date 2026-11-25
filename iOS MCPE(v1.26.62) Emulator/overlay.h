// Mod overlay: a mod's own UI drawn over the game's frame (emu_overlay_* in emu_mod.h).
// The mod sends triangle lists (Dear ImGui's draw data layout) and the screen areas its UI
// covers; pointer input on those areas (PC mouse or phone touches) goes to the mod instead
// of the game.
#pragma once
#include "common.h"
#include "window.h"

namespace overlay {

#pragma pack(push, 4)
struct Vertex {  // = ImDrawVert
    float x, y, u, v;
    u32 col;  // RGBA8 (R in the low byte)
};
struct Cmd {
    u32 elem_count, idx_offset, vtx_offset, texture;  // texture: id from emu_overlay_texture (0 = none)
    float clip[4];                                     // x0, y0, x1, y1 (pixels of the game's frame)
};
struct InputEvent {  // emu_overlay_events
    u32 type;        // 1 pointer down, 2 move, 3 up, 4 wheel, 5 key down, 6 key up, 7 char, 8 hover (no button)
    s32 id;          // pointer: 0 left / 1 right / 2 middle (PC) or 100 + touch id (phone); key: virtual-key code; wheel: +1 / -1
    float x, y;      // pixels of the game's frame
    u32 chr;         // char: unicode code point
    u32 mods;
};
#pragma pack(pop)

enum Flags : u32 {
    WantCursor = 1,    // free the mouse (no mouse look) while the mod's UI is open
    WantKeyboard = 2,  // keys and characters go to the mod (one of its text fields has the focus)
};

void submit(const Vertex* v, int nv, const u32* idx, int ni, const Cmd* cmds, int nc, int fb_w, int fb_h);
void set_texture(int id, const void* rgba, int w, int h);
void set_capture(const float* rects, int n, u32 flags);
int take_events(InputEvent* out, int max);
void frame_size(int* w, int* h);

// host side
bool filter(const window::Event& e);  // true: the event is the overlay's (the game must not see it)
bool wants_cursor();
bool wants_keyboard();
void draw(u32 fbo, int w, int h, void* (*proc)(const char*));  // render thread, before the frame is shown / sent

}  // namespace overlay
