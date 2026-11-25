// Java Edition shader packs (OptiFine / Shaders Mod format, .zip files in <data>/shaders).
//
// The game's draws are redirected at the OpenGL level: while the world is drawn, its draws go
// into the pack's G-buffers (colortex0-7 and the depth textures) with the pack's gbuffers_*
// programs instead of the game's shaders; the terrain is drawn again from the sun for the
// shadow map (shadow program); before the GUI is drawn the composite passes and the final pass
// make the picture in the game's framebuffer.
#pragma once
#include <cstdint>
#include <string>

namespace shaderpack {

// Creates <root>/shaders, lists the packs in it and reads the saved choice (<root>/shaders.txt).
void init(const std::wstring& root);
int count();                // packs found (the choice "Default" = none is not counted)
std::string name(int i);    // the pack's file name (UTF-8)
int selected();             // the pack in use (-1: none)
void select(int i);         // used from the next frame; saved

// "Shader Options" of the pack chosen (the settings screen): its screens one at a time
bool options_begin();                          // reads the pack's options (false: no pack chosen)
int options_open(const std::string& screen);   // "" the main screen; returns its number of entries
std::string options_title();
int options_entry(int i, std::string& label);  // 0 empty, 1 option, 2 screen ("Name..."), 3 profile; -1 none
int options_click(int i, std::string& screen); // the next value / profile; a screen: its name in screen
void options_reset();                          // all to their defaults
void options_close();                          // saved; the pack is loaded again with them

// ---- OpenGL hooks (render thread; hle_gl.cpp) ----
void gl_ready(void* (*getproc)(const char*));
void on_link(unsigned prog, const std::string& vs, const std::string& fs);
void on_delete_program(unsigned prog);
void on_use_program(unsigned prog);
unsigned on_bind_framebuffer(unsigned target, unsigned fb);  // the framebuffer to bind instead
void on_framebuffer_renderbuffer(unsigned attachment, unsigned rb);
void set_drawable(unsigned rb, int w, int h);
bool on_viewport(int x, int y, int w, int h);  // true: set (scaled to the G-buffers: Render Quality)
bool on_clear(unsigned mask);  // true: done (not passed on)
// true: drawn (the game's draw is not passed on)
bool on_draw(unsigned mode, int first, int count, unsigned type, const void* indices, bool elements);
void on_buffer_changed(unsigned target);
void on_delete_buffers(int n, const unsigned* ids);
void before_present();

// the game's Dimension::getTimeOfDay(int time, float partial) (replaced: the value is the same)
float time_of_day(int time, float partial, bool nether);
// the camera's position in the world (LevelRenderer::computeCameraPos)
// eye: 0 in the air, 1 in water, 2 in lava
void set_camera(float x, float y, float z, int eye);
// the camera entity's dimension (0 the overworld, 1 the nether, 2 the end; -1: not known)
void set_dimension(int dim);
// the block the player holds (its id; 0: none / not a block) and its light (the game's Block::mLightEmission, 0..15),
// from ItemInHandRenderer::tick
void set_held_block(int id, int light);
// LightTexture::getColorForUV(uv) -> colour: the game's light of an entity (block and sky light, 0..15) and the
// colour it is drawn with (TILE_LIGHT_COLOR); the pack gets the two lights instead of the colour
void on_light_color(int block, int sky, const float color[4]);
// the entity whose draws follow is the camera's (the player seen in third person: not reprojected with the camera's
// motion by the packs' motion blur / temporal filters) or not
void set_draw_entity(bool camera_entity);

}  // namespace shaderpack
