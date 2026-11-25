// libSystem functions that the 1.26 game (or its frameworks) use and 0.16.2 did not.
#include "hle_common.h"
#include "macho.h"
#include "vmem.h"
#include "vfs.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <cmath>
#include <limits>

using namespace hlec;

// ---- stack probe: x9 = size; must not change any register ----
HLE_RAW(__chkstk_darwin) {}

// ---- setjmp / longjmp ----
// Our own jmp_buf layout (fits in Darwin's 192-byte jmp_buf):
//   [0..11] x19..x30, [12] sp, [13..20] d8..d15
static void save_jmp(Cpu& c, u64* jb) {
    for (int i = 0; i < 12; i++) jb[i] = c.x(19 + i);
    jb[12] = c.sp();
    for (int i = 0; i < 8; i++) {
        double d = c.d(8 + i);
        memcpy(&jb[13 + i], &d, 8);
    }
}
static void restore_jmp(Cpu& c, const u64* jb, int val) {
    for (int i = 0; i < 12; i++) c.set_x(19 + i, jb[i]);
    c.set_sp(jb[12]);
    for (int i = 0; i < 8; i++) {
        double d;
        memcpy(&d, &jb[13 + i], 8);
        c.set_d(8 + i, d);
    }
    c.set_x(0, val ? (u64)(u32)val : 1);
}
HLE_RAW(setjmp) { save_jmp(c, gptr<u64>(c.x(0))); c.set_x(0, 0); }
HLE_RAW_SYM("__setjmp", u_setjmp) { save_jmp(c, gptr<u64>(c.x(0))); c.set_x(0, 0); }
HLE_RAW(sigsetjmp) { save_jmp(c, gptr<u64>(c.x(0))); c.set_x(0, 0); }
HLE_RAW(longjmp) { restore_jmp(c, gptr<u64>(c.x(0)), (int)c.x(1)); }
HLE_RAW_SYM("__longjmp", u_longjmp) { restore_jmp(c, gptr<u64>(c.x(0)), (int)c.x(1)); }
HLE_RAW(siglongjmp) { restore_jmp(c, gptr<u64>(c.x(0)), (int)c.x(1)); }

// ---- CommonCrypto ----
HLE_FUNC(CCRandomGenerateBytes, int, (void* p, size_t n)) {
    BCryptGenRandom(nullptr, (PUCHAR)p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return 0;  // kCCSuccess
}

// ---- misc libc ----
HLE_FUNC(basename, char*, (char* path)) {
    static thread_local char buf[1024];
    if (!path || !*path) { strcpy(buf, "."); return buf; }
    std::string s = path;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    auto sl = s.rfind('/');
    if (sl != std::string::npos && s.size() > 1) s = s.substr(sl + 1);
    strncpy(buf, s.c_str(), sizeof buf - 1);
    return buf;
}

static bool fnmatch_impl(const char* p, const char* s, int flags) {
    const bool pathname = flags & 2, noescape = flags & 1;
    while (*p) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return !pathname || !strchr(s, '/');
            for (; *s; s++) {
                if (fnmatch_impl(p, s, flags)) return true;
                if (pathname && *s == '/') return false;
            }
            return fnmatch_impl(p, s, flags);
        }
        if (!*s) return false;
        if (*p == '?') {
            if (pathname && *s == '/') return false;
            p++, s++;
            continue;
        }
        if (*p == '[') {
            const char* q = p + 1;
            bool neg = *q == '!' || *q == '^';
            if (neg) q++;
            bool hit = false;
            while (*q && *q != ']') {
                char lo = *q, hi = lo;
                if (q[1] == '-' && q[2] && q[2] != ']') { hi = q[2]; q += 3; }
                else q++;
                if (*s >= lo && *s <= hi) hit = true;
            }
            if (*q != ']') return false;
            if (hit == neg) return false;
            p = q + 1, s++;
            continue;
        }
        if (*p == '\\' && !noescape && p[1]) p++;
        if (*p != *s) return false;
        p++, s++;
    }
    return !*s;
}
HLE_FUNC(fnmatch, int, (const char* pat, const char* str, int flags)) {
    return pat && str && fnmatch_impl(pat, str, flags) ? 0 : 1;  // FNM_NOMATCH = 1
}

HLE_FUNC(getpwuid_r, int, (u32, void*, char*, size_t, u64* result)) {
    if (result) *result = 0;
    return 0;
}
HLE_FUNC(ilogbf, int, (float f)) { return std::ilogb(f); }
HLE_RAW(lldiv) {
    s64 a = (s64)c.x(0), b = (s64)c.x(1);
    c.set_x(0, (u64)(b ? a / b : 0));
    c.set_x(1, (u64)(b ? a % b : 0));
}
HLE_FUNC(nan, double, (const char*)) { return std::numeric_limits<double>::quiet_NaN(); }
HLE_FUNC(nanf, float, (const char*)) { return std::numeric_limits<float>::quiet_NaN(); }

// Darwin wchar_t is UTF-32
static size_t utf8_decode(const u8* s, u32* cp) {
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    if ((s[0] & 0xe0) == 0xc0 && (s[1] & 0xc0) == 0x80) { *cp = ((s[0] & 0x1f) << 6) | (s[1] & 0x3f); return 2; }
    if ((s[0] & 0xf0) == 0xe0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) {
        *cp = ((s[0] & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f); return 3;
    }
    if ((s[0] & 0xf8) == 0xf0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 && (s[3] & 0xc0) == 0x80) {
        *cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3f) << 12) | ((s[2] & 0x3f) << 6) | (s[3] & 0x3f); return 4;
    }
    return 0;
}
static size_t utf8_encode(u32 cp, char* o) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xc0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3f)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xe0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3f)); o[2] = (char)(0x80 | (cp & 0x3f)); return 3; }
    o[0] = (char)(0xf0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); o[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}
HLE_FUNC(mbsrtowcs, s64, (u32* dst, const char** src, size_t len, void*)) {
    const u8* s = (const u8*)*src;
    size_t n = 0;
    while (!dst || n < len) {
        if (!*s) {
            if (dst) { dst[n] = 0; *src = nullptr; }
            return (s64)n;
        }
        u32 cp;
        size_t k = utf8_decode(s, &cp);
        if (!k) { set_errno(D_EILSEQ); if (dst) *src = (const char*)s; return -1; }
        if (dst) dst[n] = cp;
        s += k;
        n++;
    }
    *src = (const char*)s;
    return (s64)n;
}
HLE_FUNC(wcsrtombs, s64, (char* dst, const u32** src, size_t len, void*)) {
    const u32* s = *src;
    size_t n = 0;
    char tmp[4];
    for (;; s++) {
        if (!*s) {
            if (dst) { if (n < len) dst[n] = 0; *src = nullptr; }
            return (s64)n;
        }
        size_t k = utf8_encode(*s, tmp);
        if (dst) {
            if (n + k > len) break;
            memcpy(dst + n, tmp, k);
        }
        n += k;
    }
    *src = s;
    return (s64)n;
}

HLE_FUNC(os_proc_available_memory, u64, ()) { return 3ull << 30; }
HLE_FUNC(pause, int, ()) { Sleep(INFINITE); return -1; }
HLE_FUNC(pid_for_task, int, (u32, int* pid)) { if (pid) *pid = 4242; return 0; }
HLE_FUNC(pthread_atfork, int, (u64, u64, u64)) { return 0; }
HLE_FUNC(sys_icache_invalidate, void, (u64 addr, u64 len)) { if (g_cpu) g_cpu->invalidate_code(addr, len); }
HLE_FUNC(sys_dcache_flush, void, (u64, u64)) {}
HLE_FUNC(utime, int, (const char*, void*)) { return 0; }
HLE_FUNC(tmpfile, void*, ()) { set_errno(D_ENOSYS); return nullptr; }
HLE_FUNC(vm_read, int, (u32, u64, u64, u64*, u32*)) { return 1; }  // KERN_INVALID_ADDRESS
HLE_FUNC(vm_region_recurse_64, int, (u32, u64*, u64*, u32*, void*, u32*)) { return 1; }
HLE_FUNC(connectx, int, (int, void*, u32, u32, void*, u32, size_t*, void*)) { set_errno(D_ENOTSUP); return -1; }
HLE_FUNC(copyfile_state_alloc, void*, ()) { return vmem::gcalloc(1, 64); }
HLE_FUNC(copyfile_state_free, int, (void* p)) { vmem::gfree(p); return 0; }
HLE_FUNC(fcopyfile, int, (int, int, void*, u32)) { set_errno(D_ENOTSUP); return -1; }
HLE_FUNC(__darwin_check_fd_set_overflow, int, (int fd, const void*, int)) { return fd >= 0 && fd < 1024 ? 1 : 0; }

// NXGetLocalArchInfo: {const char* name; cpu_type_t cputype; cpu_subtype_t cpusubtype; NXByteOrder byteorder; const char* description;}
HLE_FUNC(NXGetLocalArchInfo, void*, ()) {
    struct ArchInfo { u64 name; s32 cputype, cpusubtype, byteorder, _pad; u64 desc; };
    static ArchInfo* ai = [] {
        auto* a = static_cast<ArchInfo*>(vmem::gcalloc(1, sizeof(ArchInfo)));
        a->name = gaddr_of(intern("arm64"));
        a->cputype = 0x0100000c;
        a->cpusubtype = 0;
        a->byteorder = 1;  // NX_LittleEndian
        a->desc = gaddr_of(intern("arm64"));
        return a;
    }();
    return ai;
}

// ---- Security (keys / trust): not available ----
HLE_FUNC(SecKeyGeneratePair, int, (void*, u64* pub, u64* priv)) { if (pub) *pub = 0; if (priv) *priv = 0; return -4; }  // errSecUnimplemented
HLE_FUNC(SecKeyCreateSignature, void*, (void*, void*, void*, u64* err)) { if (err) *err = 0; return nullptr; }
HLE_FUNC(SecKeyCopyExternalRepresentation, void*, (void*, u64* err)) { if (err) *err = 0; return nullptr; }
HLE_FUNC(SecKeyRawSign, int, (void*, u32, const void*, size_t, void*, size_t*)) { return -4; }
HLE_FUNC(SecTrustEvaluateWithError, bool, (void*, u64* err)) { if (err) *err = 0; return false; }

// ---- CFNetwork proxies: none ----
HLE_FUNC(CFNetworkCopyProxiesForURL, void*, (void*, void*)) { return nullptr; }
HLE_FUNC(CFNetworkCopyProxiesForAutoConfigurationScript, void*, (void*, void*, u64* err)) { if (err) *err = 0; return nullptr; }

// ---- debugging: EMU_LOGHOOK="<hex addr>:<reg>[,...]" logs the C string in x<reg> whenever the
// guest function at <addr> is called (then runs it). Up to 8 hooks.
namespace {
struct LogHook { u64 orig = 0; int reg = 0; u64 addr = 0; };
LogHook g_loghooks[8];
template <int N>
void loghook_handler(Cpu& c) {
    LogHook& h = g_loghooks[N];
    u64 a[8];
    for (int i = 0; i < 8; i++) a[i] = c.x(i);
    const char* s = gptr<const char>(a[h.reg]);
    LOGI("[loghook %llx] %s (x0=%llx x1=%llx x2=%llx) from %s", (unsigned long long)h.addr,
         s && vmem::is_mapped(a[h.reg], 1) ? s : "(not a string)", (unsigned long long)a[0], (unsigned long long)a[1],
         (unsigned long long)a[2], macho::symbolicate(c.lr()).c_str());
    u64 r = c.call(h.orig, {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]});
    c.set_x(0, r);
}
using HookFn = void (*)(Cpu&);
const HookFn kLogHookFns[8] = {loghook_handler<0>, loghook_handler<1>, loghook_handler<2>, loghook_handler<3>,
                               loghook_handler<4>, loghook_handler<5>, loghook_handler<6>, loghook_handler<7>};
}  // namespace

void install_debug_hooks() {
    const char* e = getenv("EMU_LOGHOOK");
    if (!e) return;
    int n = 0;
    for (const char* p = e; *p && n < 8;) {
        char* end = nullptr;
        u64 addr = strtoull(p, &end, 16);
        int reg = 0;
        if (end && *end == ':') reg = (int)strtol(end + 1, &end, 10);
        g_loghooks[n].addr = addr;
        g_loghooks[n].reg = reg & 7;
        gaddr stub = hle::add_function("<loghook>", kLogHookFns[n], true);
        if (hle::hook_guest(addr, stub, &g_loghooks[n].orig) == 0) n++;
        p = end && *end ? end + 1 : end ? end : p + strlen(p);
        if (!end) break;
    }
}

// ---- free space for NSFileManager / NSURL (the drive with the data folder) ----
HLE_FUNC(emu_disk_space, void, (const char*, u64* total, u64* free_bytes)) {
    ULARGE_INTEGER avail{}, tot{}, fr{};
    std::wstring root = widen(vfs::host_data_dir());
    if (GetDiskFreeSpaceExW(root.c_str(), &avail, &tot, &fr)) {
        if (total) *total = tot.QuadPart;
        if (free_bytes) *free_bytes = avail.QuadPart;
    }
}
