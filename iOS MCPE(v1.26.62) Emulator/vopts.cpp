// OptiFine-like video settings (see vopts.h).
#include "vopts.h"

#include "common.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>

namespace fs = std::filesystem;

namespace vopts {
namespace {

struct Def {
    const char* key;
    int def;    // default choice
    int count;  // number of choices
};
// (the labels and the choices' names are in guest/EmuShaders/shaders_ui.cpp, in the same order)
const Def kDefs[] = {
    // Shaders
    {"ofAaLevel", 0, 3},        // OFF, FXAA 2x, FXAA 4x
    {"ofNormalMap", 1, 2},      // OFF, ON
    {"ofSpecularMap", 1, 2},
    {"ofRenderQuality", 2, 5},  // 0.5x 0.7x 1x 1.5x 2x
    {"ofShadowQuality", 2, 5},
    {"ofHandDepth", 1, 3},      // 0.5x 1x 2x
    {"ofOldHandLight", 0, 3},   // Default, ON, OFF
    {"ofOldLighting", 0, 3},
    // Video
    {"ofSmoothLighting", 2, 3},  // OFF, Minimum, Maximum
    {"ofAoLevel", 10, 11},       // OFF, 10% .. 100%
    {"ofDynamicLights", 2, 3},   // OFF, Fast, Fancy
    {"ofMaxFps", 0, 10},         // VSync, 30 60 75 90 120 144 165 240, Unlimited
    {"ofUseVbo", 1, 2},
    {"ofAttackIndicator", 0, 3},  // Crosshair, Hotbar, OFF
    {"ofDynamicFov", 1, 2},
    // Details
    {"ofClouds", 0, 4},  // Default, Fast, Fancy, OFF
    {"ofCloudHeight", 0, 11},
    {"ofTrees", 0, 4},  // Default, Fast, Fancy, Smart
    {"ofRain", 0, 4},   // Default, Fast, Fancy, OFF
    {"ofSky", 1, 2},
    {"ofStars", 1, 2},
    {"ofSunMoon", 1, 2},
    {"ofShowCapes", 1, 2},
    {"ofFogFancy", 1, 3},  // Fast, Fancy, OFF
    {"ofFogStart", 3, 4},  // 0.2 0.4 0.6 0.8
    {"ofTranslucentBlocks", 0, 3},
    {"ofHeldItemTooltips", 1, 2},
    {"ofDroppedItems", 0, 3},
    {"ofEntityShadows", 1, 2},
    {"ofVignette", 0, 3},
    {"ofAlternateBlocks", 1, 2},
    {"ofSwampColors", 1, 2},
    {"ofBiomeBlend", 0, 3},  // 5x5, 15x15, OFF
    // Animations
    {"ofAnimatedWater", 1, 2},
    {"ofAnimatedLava", 1, 2},
    {"ofAnimatedFire", 1, 2},
    {"ofAnimatedPortal", 1, 2},
    {"ofAnimatedRedstone", 1, 2},
    {"ofAnimatedExplosion", 1, 2},
    {"ofAnimatedFlame", 1, 2},
    {"ofAnimatedSmoke", 1, 2},
    {"ofVoidParticles", 1, 2},
    {"ofWaterParticles", 1, 2},
    {"ofRainSplash", 1, 2},
    {"ofPortalParticles", 1, 2},
    {"ofPotionParticles", 1, 2},
    {"ofDrippingWaterLava", 1, 2},
    {"ofAnimatedTerrain", 1, 2},
    {"ofAnimatedTextures", 1, 2},
    {"ofFireworkParticles", 1, 2},
    {"ofParticles", 0, 3},  // All, Decreased, Minimal
    // Quality
    {"ofMipmapLevels", 3, 5},  // OFF, 1 2 3, Max (the default: the game's own, 3 levels, linear)
    {"ofMipmapType", 1, 4},    // Nearest, Linear, Bilinear, Trilinear
    {"ofAfLevel", 0, 5},       // OFF, 2 4 8 16
    {"ofAaQuality", 0, 6},     // OFF, 2 4 6 8 16
    {"ofEmissiveTextures", 1, 2},
    {"ofRandomEntities", 1, 2},
    {"ofBetterGrass", 0, 3},  // OFF, Fast, Fancy
    {"ofBetterSnow", 0, 2},
    {"ofCustomFonts", 1, 2},
    {"ofCustomColors", 1, 2},
    {"ofConnectedTextures", 1, 3},  // OFF, Fast, Fancy
    {"ofNaturalTextures", 0, 2},
    {"ofCustomSky", 1, 2},
    {"ofCustomItems", 1, 2},
    {"ofCustomEntityModels", 1, 2},
    {"ofCustomGuis", 1, 2},
    // Performance
    {"ofSmoothFps", 0, 2},
    {"ofSmoothWorld", 1, 2},
    {"ofFastRender", 0, 2},
    {"ofFastMath", 0, 2},
    {"ofChunkUpdates", 0, 5},  // 1 .. 5
    {"ofChunkUpdatesDynamic", 0, 2},
    {"ofRenderRegions", 0, 2},
    {"ofLazyChunkLoading", 1, 2},
    {"ofSmartAnimations", 0, 2},
    // Other
    {"ofLagometer", 0, 2},
    {"ofProfiler", 0, 2},
    {"ofShowFps", 0, 2},
    {"ofAdvancedTooltips", 0, 2},
    {"ofWeather", 1, 2},
    {"ofTime", 0, 3},  // Default, Day Only, Night Only
    {"ofFullscreen", 0, 2},
    {"ofFullscreenMode", 0, 1},  // Current
    {"ofAutoSaveTicks", 0, 6},  // 45s 90s 3min 6min 12min 24min
    {"ofScreenshotSize", 0, 4},  // 1x .. 4x
    {"ofShowGlErrors", 1, 2},
};

std::mutex g_lock;
std::map<std::string, int> g_values;  // the values set (others: their default)
fs::path g_file, g_options;
std::atomic<unsigned> g_gen{1};

const Def* find(const char* key) {
    if (!key) return nullptr;
    for (auto& d : kDefs)
        if (!strcmp(d.key, key)) return &d;
    return nullptr;
}

void save_locked() {
    FILE* f = _wfopen(g_file.wstring().c_str(), L"wb");
    if (!f) return;
    fputs("# Video settings (Settings > Video): option:choice (0 = the first choice)\n", f);
    for (auto& d : kDefs) {
        auto it = g_values.find(d.key);
        fprintf(f, "%s:%d\n", d.key, it == g_values.end() ? d.def : it->second);
    }
    fclose(f);
}

}  // namespace

void init(const std::wstring& root) {
    std::lock_guard lk(g_lock);
    g_file = fs::path(root) / L"optionsof.txt";
    g_options = fs::path(root) / L"home" / L"Documents" / L"games" / L"com.mojang" / L"minecraftpe" / L"options.txt";
    g_values.clear();
    FILE* f = _wfopen(g_file.wstring().c_str(), L"rb");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char* c = strchr(line, ':');
        if (!c) continue;
        *c = 0;
        const Def* d = find(line);
        if (!d) continue;
        int v = atoi(c + 1);
        if (v >= 0 && v < d->count) g_values[d->key] = v;
    }
    fclose(f);
    LOGI("video settings: %zu read from optionsof.txt", g_values.size());
}

int get(const char* key) {
    const Def* d = find(key);
    if (!d) return -1;
    std::lock_guard lk(g_lock);
    auto it = g_values.find(d->key);
    return it == g_values.end() ? d->def : it->second;
}

int count(const char* key) {
    const Def* d = find(key);
    return d ? d->count : 0;
}

void set(const char* key, int value) {
    const Def* d = find(key);
    if (!d || value < 0 || value >= d->count) return;
    std::lock_guard lk(g_lock);
    auto it = g_values.find(d->key);
    if (it != g_values.end() && it->second == value) return;
    g_values[d->key] = value;
    g_gen++;
    LOGI("video settings: %s = %d", d->key, value);
    save_locked();
}

void reset_all() {
    std::lock_guard lk(g_lock);
    g_values.clear();
    g_gen++;
    LOGI("video settings: reset");
    save_locked();
}

unsigned generation() { return g_gen.load(); }

std::mutex g_lang_lock;
std::string g_lang_set;  // from the game (set_language)

void set_language(const std::string& code) {
    std::lock_guard lk(g_lang_lock);
    g_lang_set = code;
}

std::string language() {
    {
        std::lock_guard lk(g_lang_lock);
        if (!g_lang_set.empty()) return g_lang_set;
    }
    static std::mutex m;
    static std::string lang = "en_US";
    static ULONGLONG next = 0;
    std::lock_guard lk(m);
    ULONGLONG now = GetTickCount64();
    if (now < next) return lang;
    next = now + 1000;
    FILE* f = _wfopen(g_options.wstring().c_str(), L"rb");
    if (!f) return lang;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "game_language:", 14)) continue;
        std::string v = line + 14;
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
        if (!v.empty()) lang = v;
        break;
    }
    fclose(f);
    return lang;
}

}  // namespace vopts
