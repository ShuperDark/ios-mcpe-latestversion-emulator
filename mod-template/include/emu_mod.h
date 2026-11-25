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
