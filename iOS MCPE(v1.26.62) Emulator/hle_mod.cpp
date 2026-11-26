// Services for mods (guest dylibs loaded from <data>/mods, see guest/include/emu/mod.h).
#include "hle.h"
#include "macho.h"
#include "vfs.h"
#include "vmem.h"

#include <cstring>
#include <vector>

namespace {
// Instructions whose meaning depends on their address cannot be moved into a trampoline.
bool pc_relative(u32 i) {
    return (i & 0x1f000000) == 0x10000000 ||  // ADR / ADRP
           (i & 0x7c000000) == 0x14000000 ||  // B / BL
           (i & 0xff000010) == 0x54000000 ||  // B.cond
           (i & 0x7e000000) == 0x34000000 ||  // CBZ / CBNZ
           (i & 0x7e000000) == 0x36000000 ||  // TBZ / TBNZ
           (i & 0x3b000000) == 0x18000000;    // LDR (literal)
}
constexpr u32 kLdrX16Lit8 = 0x58000050;  // ldr x16, #8
constexpr u32 kBrX16 = 0xd61f0200;       // br x16
}  // namespace

HLE_FUNC(emu_log, void, (const char* msg)) { LOGI("[mod] %s", msg ? msg : "(null)"); }

HLE_FUNC(emu_game_base, u64, ()) {
    for (auto& i : macho::images())
        if (i->install_name == "minecraftpe") return i->base;
    return 0x100000000ull;
}

// Redirects the function at `target` to `replacement`. If `original` is not null it
// receives a trampoline that runs the replaced function. The first 16 bytes of the
// target are overwritten, so they must not contain PC-relative instructions (a function
// that is already hooked - by the emulator or another mod - can be hooked again: the
// trampoline then runs the previous hook).
// Returns 0 on success. Call it from the mod's constructor (before the game runs the
// target for the first time) - code that was already translated by the JIT keeps
// running the old version on threads other than the calling one.
HLE_FUNC(emu_hook, int, (u64 target, u64 replacement, u64* original)) { return hle::hook_guest(target, replacement, original); }
int hle::hook_guest(u64 target, u64 replacement, u64* original) {
    if (!target || !replacement || !vmem::is_mapped(target, 16)) {
        LOGW("[mod] emu_hook: bad address %llx", (unsigned long long)target);
        return -1;
    }
    u32* t = gptr<u32>(target);
    // already redirected (by the emulator or another mod): chain to the current replacement
    const bool chained = t[0] == kLdrX16Lit8 && t[1] == kBrX16;
    for (int k = 0; k < 4 && !chained; k++)
        if (pc_relative(t[k])) {
            LOGW("[mod] emu_hook: %llx starts with a PC-relative instruction (%08x), not hooked", (unsigned long long)target, t[k]);
            return -2;
        }
    if (original) {
        u32* tr = static_cast<u32*>(vmem::gmemalign(16, 32));
        if (chained) {
            memcpy(tr, t, 16);  // jumps to the previous replacement
        } else {
            memcpy(tr, t, 16);
            tr[4] = kLdrX16Lit8;
            tr[5] = kBrX16;
            *reinterpret_cast<u64*>(tr + 6) = target + 16;
        }
        *original = gaddr_of(tr);
    }
    t[0] = kLdrX16Lit8;
    t[1] = kBrX16;
    *reinterpret_cast<u64*>(t + 2) = replacement;
    if (g_cpu) g_cpu->invalidate_code(target, 16);
    LOGI("[mod] hooked %llx -> %llx", (unsigned long long)target, (unsigned long long)replacement);
    return 0;
}

// ---- overlay: a mod's own UI over the game (overlay.cpp; emu_overlay_* in emu_mod.h)
#include "overlay.h"
HLE_FUNC(emu_overlay_render, void,
         (const overlay::Vertex* v, int nv, const u32* idx, int ni, const overlay::Cmd* cmds, int nc, int fb_w, int fb_h)) {
    if (nv < 0 || ni < 0 || nc < 0 || (nv && !v) || (ni && !idx) || (nc && !cmds)) return;
    overlay::submit(v, nv, idx, ni, cmds, nc, fb_w, fb_h);
}
HLE_FUNC(emu_overlay_texture, void, (int id, const void* rgba, int w, int h)) { overlay::set_texture(id, rgba, w, h); }
HLE_FUNC(emu_overlay_capture, void, (const float* rects, int n, u32 flags)) { overlay::set_capture(rects, rects ? n : 0, flags); }
HLE_FUNC(emu_overlay_events, int, (overlay::InputEvent* out, int max)) { return out && max > 0 ? overlay::take_events(out, max) : 0; }
HLE_FUNC(emu_overlay_frame_size, void, (int* w, int* h)) {
    int a = 0, b = 0;
    overlay::frame_size(&a, &b);
    if (w) *w = a;
    if (h) *h = b;
}

// A font of the system with Japanese glyphs (TrueType / collection bytes): the size, and the
// bytes copied into out when it is big enough.
#include <windows.h>
HLE_FUNC(emu_system_font, u64, (void* out, u64 cap)) {
    static std::vector<u8> font;
    static bool tried = false;
    if (!tried) {
        tried = true;
        wchar_t win[MAX_PATH] = {};
        if (!GetWindowsDirectoryW(win, MAX_PATH)) wcscpy(win, L"C:\\Windows");
        for (const wchar_t* name : {L"meiryo.ttc", L"YuGothM.ttc", L"msgothic.ttc", L"BIZ-UDGothicR.ttc"}) {
            std::wstring p = std::wstring(win) + L"\\Fonts\\" + name;
            HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                LOGW("[mod] system font %ls: error %lu", p.c_str(), GetLastError());
                continue;
            }
            LARGE_INTEGER sz{};
            GetFileSizeEx(h, &sz);
            font.resize((size_t)sz.QuadPart);
            DWORD got = 0;
            bool ok = sz.QuadPart > 0 && ReadFile(h, font.data(), (DWORD)font.size(), &got, nullptr) && got == font.size();
            CloseHandle(h);
            if (ok) {
                LOGI("[mod] system font: %ls (%zu bytes)", name, font.size());
                break;
            }
            font.clear();
        }
    }
    if (out && cap >= font.size()) memcpy(out, font.data(), font.size());
    return font.size();
}

// 1 when [addr, addr + len) is committed, readable memory (a mod can check a pointer before reading it).
HLE_FUNC(emu_readable, int, (u64 addr, u64 len)) {
    if (!addr || addr + len < addr) return 0;
    u64 a = addr, end = addr + (len ? len : 1);
    while (a < end) {
        MEMORY_BASIC_INFORMATION mi{};
        if (!VirtualQuery((const void*)a, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
        DWORD p = mi.Protect & 0xff;
        if ((mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                                                    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return 0;
        a = (u64)mi.BaseAddress + mi.RegionSize;
    }
    return 1;
}

// Adds a file to the app's bundle (read-only for the game, like on iOS) for this run: rel is a path
// inside minecraftpe.app ("data/resource_packs/persona/pieces/x/x.json"), new or replacing the app's
// own. The app on the disk is not changed (vfs.cpp: the overlay is emptied at every start, so a mod
// that is removed leaves nothing behind). Returns 0, or -1 (a bad path: absolute, "..", "\" or ":").
HLE_FUNC(emu_bundle_write, int, (const char* rel, const void* data, u64 len)) {
    if (!rel || !*rel || rel[0] == '/' || strstr(rel, "..") || strchr(rel, '\\') || strchr(rel, ':') || (len && !data)) {
        LOGW("[mod] emu_bundle_write: refusing the path %s", rel ? rel : "(null)");
        return -1;
    }
    if (!vfs::overlay_write(rel, data, (size_t)len)) {
        LOGW("[mod] emu_bundle_write: cannot write %s", rel);
        return -1;
    }
    LOGD("[mod] added %s to the app", rel);
    return 0;
}
