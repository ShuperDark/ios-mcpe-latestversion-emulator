// API for iOS MCPE (v1.26.62) Emulator mods.
//
// A mod is an arm64 dylib in <data>/mods (%LOCALAPPDATA%\iOS-MCPE-1.26.62\mods). The
// emulator loads it together with the game; functions marked
// __attribute__((constructor)) run before the game's own initialization.
// Mods can call everything the game can: libc, libc++, Foundation / UIKit (Objective-C
// runtime: objc_getClass, class_replaceMethod, ...), the game's own code by address
// (the game is loaded at emu_game_base(), 0x100000000), and the functions below.
#pragma once
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// Writes a line to emulator.log (and the console), prefixed with "[mod]".
void emu_log(const char* message);

// Load address of the game binary (addresses from a disassembler with base 0x100000000
// can be used as they are while this returns 0x100000000).
uint64_t emu_game_base(void);

// Redirects the function at `target` to `replacement`; `*original` (optional) receives a
// trampoline that calls the replaced function. Call it from a constructor. Returns 0
// on success (fails if the first 4 instructions of the target are PC-relative; a function
// that the emulator or another mod already hooked can be hooked again - the chain is kept).
int emu_hook(uint64_t target, uint64_t replacement, uint64_t* original);

// Window title.
void emu_set_title(const char* title);

// Picture picker: a Windows file dialog, or the photo / file picker of the phone connected
// to the remote screen. emu_pick_media_begin() asks for a picture (PNG / JPEG / BMP / GIF)
// and returns at once; then call emu_pick_image_poll() (from the game's thread, e.g. once a
// frame) until it is not 0: 1 = done, *len bytes ready (copy them with emu_pick_image_take),
// -1 = cancelled. The file's bytes are returned as they are (decode them yourself).
// emu_pick_image_cancel() closes a picker that is still open (its answer is dropped).
void emu_pick_media_begin(void);
int emu_pick_image_poll(size_t* len);
void emu_pick_image_take(void* out);
void emu_pick_image_cancel(void);
// MMD model picker (answered the same way, through emu_pick_image_poll / take): a ZIP archive
// of the model's folder. On the PC the player picks a .pmx / .pmd (the emulator packs every
// file of its folder, uncompressed, UTF-8 names, the picked model first) or a .zip as it is;
// a phone sends the file it picked (a .zip, or a lone .pmx / .pmd: check the first bytes).
void emu_pick_model_begin(void);

// Sound file picker (answered through emu_pick_image_poll / take): an .mp3, .ogg or .wav file.
void emu_pick_sound_begin(void);

// Several sound files at once (answered through emu_pick_image_poll / take): always a ZIP archive of the picked
// files (stored, UTF-8 names, even for one file); emu_pick_name gives the first one's name.
void emu_pick_sounds_begin(void);

// MMD motion picker (answered through emu_pick_image_poll / take): a .vmd file.
void emu_pick_motion_begin(void);
// The name of the file the last picker returned (UTF-8, without its folder) into out (n bytes
// with the terminating 0; cut at a whole character); returns the name's length (0 = unknown).
// Call it after emu_pick_image_poll returned 1.
int emu_pick_name(char* out, size_t n);

// A button on the screen of the phone connected to the remote screen, at the left edge of the
// game, drawn with the game's button texture: mode 0 hidden, 1 shown, 2 transparent (still
// tappable). emu_overlay_taps returns how many times it was tapped since the last call.
void emu_overlay_button(int mode, const char* label);
int emu_overlay_taps(void);

// 1 while the key (a Windows virtual-key code, e.g. 'P' = 0x50) is held down, the emulator's
// window is in front and no text field takes the keys.
int emu_key_down(int vk);

// Sounds: emu_sound_load decodes an MP3, Ogg Vorbis or WAV (PCM / float) file (its bytes) and returns a sound
// id (0 = cannot decode); emu_sound_play mixes it into the game's audio output (volume 1 =
// as recorded) and returns a voice id; emu_sound_stop stops a voice (0 = every voice of
// every mod); emu_sound_playing tells whether a voice still plays; emu_sound_free forgets a
// sound (voices that play it finish first).
int emu_sound_load(const void* data, size_t len);
void emu_sound_free(int sound);
int emu_sound_play(int sound, float volume);
void emu_sound_stop(int voice);
int emu_sound_playing(int voice);
// emu_sound_play from `start` seconds into the sound at `rate` times its speed (0.25..4; the pitch changes with
// it); 0 when start is past its end. emu_sound_set_volume changes a playing voice's volume; emu_sound_length:
// the sound's length in seconds.
int emu_sound_play_ex(int sound, float volume, float start, float rate);
void emu_sound_set_volume(int voice, float volume);
float emu_sound_length(int sound);

// ---- Overlay: the mod's own UI drawn over the game (PC window, phone and screenshots).
// Coordinates are pixels of the game's frame (emu_overlay_frame_size), origin top-left.
// The vertex / command layout matches Dear ImGui's ImDrawVert / ImDrawCmd, so ImGui's draw
// data can be sent as it is (merge all lists into one vertex and one index buffer).
typedef struct EmuOverlayVertex {
    float x, y, u, v;
    uint32_t col;  // RGBA8, R in the low byte (IM_COL32)
} EmuOverlayVertex;
typedef struct EmuOverlayCmd {
    uint32_t elem_count, idx_offset, vtx_offset, texture;  // texture: id given to emu_overlay_texture (0 = white)
    float clip[4];                                         // x0, y0, x1, y1
} EmuOverlayCmd;
typedef struct EmuOverlayEvent {
    uint32_t type;  // 1 pointer down, 2 move, 3 up, 4 wheel, 5 key down, 6 key up, 7 char, 8 hover
    int32_t id;     // pointer: 0 left / 1 right / 2 middle (PC mouse), 100 + n (phone touch); wheel: +1 / -1; key: virtual-key code
    float x, y;
    uint32_t chr;   // char: unicode code point
    uint32_t mods;
} EmuOverlayEvent;
enum { EMU_OVERLAY_WANT_CURSOR = 1, EMU_OVERLAY_WANT_KEYBOARD = 2 };
// The frame to draw (call it once a frame; the last one stays on screen for up to 1 second).
// fb_w x fb_h: the frame size it was made for.
void emu_overlay_render(const EmuOverlayVertex* vtx, int nvtx, const uint32_t* idx, int nidx, const EmuOverlayCmd* cmds, int ncmds,
                        int fb_w, int fb_h);
// A texture (RGBA8, w x h) for the commands (id > 0); rgba = NULL deletes it.
void emu_overlay_texture(int id, const void* rgba, int w, int h);
// The areas (x0, y0, x1, y1 each) where pointer input goes to the mod instead of the game, and
// flags: EMU_OVERLAY_WANT_CURSOR frees the mouse (no mouse look) while the UI is open,
// EMU_OVERLAY_WANT_KEYBOARD takes the keys and characters (a text field of the UI has the focus).
void emu_overlay_capture(const float* rects, int nrects, uint32_t flags);
// Input taken by the overlay (and the mouse position while it is over the game): returns the count.
int emu_overlay_events(EmuOverlayEvent* out, int max);
void emu_overlay_frame_size(int* w, int* h);
// A system font with Japanese glyphs (TrueType collection bytes): returns its size and copies it
// into out when cap is big enough (call with out = NULL first).
uint64_t emu_system_font(void* out, uint64_t cap);

// Adds a file to the game's app (minecraftpe.app, which the game itself cannot write, like on iOS) or
// replaces one of its files, for this run only: e.g. pieces of its built-in resource packs. rel is a
// path inside the app ("data/resource_packs/..."). The app on the disk is not changed (the files are
// kept in <data>/bundle_overlay, emptied at every start: a removed mod leaves nothing behind). Call it
// from a constructor, before the game reads the files. Returns 0, or -1 (an absolute path, "..").
int emu_bundle_write(const char* rel, const void* data, uint64_t len);

// HTTP(S) of the game (NSURLSession; these two come from the emulator's Foundation):
// emu_http_add_body_filter: the bodies of the responses whose URL contains url_part go to
// fn(url, body, len, &out_len) before the game gets them; fn returns a malloc'd replacement (out_len
// bytes) or NULL to keep the body. emu_http_add_local_response: requests of URLs that start with url get
// these bytes (status 200, content_type) without the network. Both return 0, or -1 (too many).
int emu_http_add_body_filter(const char* url_part, void* (*fn)(const char* url, const void* body, size_t len, size_t* out_len));
int emu_http_add_local_response(const char* url, const void* data, size_t len, const char* content_type);

// 1 when the memory [addr, addr + len) can be read (check a pointer of unknown origin before following it).
int emu_readable(const void* addr, uint64_t len);

#ifdef __cplusplus
}
#endif

// printf-style logging
#define EMU_LOGF(...)                                   \
    do {                                                \
        char emu_log_buf_[512];                         \
        snprintf(emu_log_buf_, sizeof emu_log_buf_, __VA_ARGS__); \
        emu_log(emu_log_buf_);                          \
    } while (0)
