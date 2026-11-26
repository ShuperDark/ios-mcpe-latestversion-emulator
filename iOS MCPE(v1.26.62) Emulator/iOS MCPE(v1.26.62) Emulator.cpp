// iOS MCPE (v1.26.62) Emulator - entry point.
//
// Boots the arm64 iOS build of Minecraft Pocket Edition 1.26.62 on Windows:
//   * dynarmic JIT runs the arm64 code
//   * guest and host share one address space (guest VA == host VA)
//   * libSystem (libc, pthreads, mach, ...) and OpenGL ES are emulated on the
//     host (HLE); the Objective-C runtime, Foundation, UIKit and libc++ are
//     arm64 replacement libraries built from guest/ with zig.
#include "auth.h"
#include "common.h"
#include "controls.h"
#include "profiler.h"
#include "remote.h"
#include "shaderpack.h"
#include "vopts.h"
#include "cpu.h"
#include "hle.h"
#include "macho.h"
#include "vfs.h"
#include "vmem.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <timeapi.h>
#include <shlobj.h>

#include <csignal>
#include <exception>
#include <filesystem>
#include <thread>
#include <unordered_set>
#include <regex>

namespace fs = std::filesystem;

void log_open_file(const std::string& path);
void gl_dump_trace();
namespace tlv { void register_image(MachImage& img); }
namespace patches { void apply(MachImage& img); void install_input_hooks(); void apply_game_fixes(); }
namespace objc_stubs {
gaddr synthesize_class(const std::string& name, bool meta);
}

// NVIDIA / AMD: prefer the discrete GPU
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace {

struct Options {
    std::string ipa, app_dir, data_dir, guest_dir;
    std::wstring msltest, msltest_out;
    bool verbose = false;
};

fs::path exe_dir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

fs::path local_appdata() {
    PWSTR p = nullptr;
    fs::path r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}

// options.txt "graphics_mode": 0 simple, 1 fancy, 2 Vibrant Visuals (deferred), 3 ray tracing.
// The emulated device (A11) gets the classic renderer, but a value saved earlier makes the game
// start Vibrant Visuals anyway: much slower (G-buffer, 2048x2048 shadow cascades, compute) and
// on the emulator its final composite sometimes never reaches the screen, so world loading
// seems to hang on "Loading...". Such values go back to fancy. EMU_ALLOW_VIBRANT=1 keeps them.
void reset_unsupported_graphics_mode(const fs::path& options) {
    if (getenv("EMU_ALLOW_VIBRANT")) return;
    std::error_code ec;
    if (!fs::exists(options, ec)) return;
    std::string text;
    {
        FILE* f = _wfopen(options.c_str(), L"rb");
        if (!f) return;
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        fclose(f);
    }
    bool changed = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        static const char kKey[] = "graphics_mode:";
        if (text.compare(pos, sizeof kKey - 1, kKey) == 0) {
            size_t v = pos + sizeof kKey - 1;
            if (v < eol && (text[v] == '2' || text[v] == '3') && (v + 1 == eol || text[v + 1] == '\r')) {
                LOGI("options.txt: graphics_mode %c (Vibrant Visuals / ray tracing) is not supported, back to 1 (fancy)", text[v]);
                text[v] = '1';
                changed = true;
            }
        }
        pos = eol + 1;
    }
    if (!changed) return;
    if (FILE* f = _wfopen(options.c_str(), L"wb")) {
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
    }
}

// Search the exe directory and its parents for "<dir>/<sub>"
fs::path search_up(const fs::path& start, const fs::path& sub) {
    for (fs::path d = start; !d.empty(); d = d.parent_path()) {
        if (fs::exists(d / sub)) return d / sub;
        if (d == d.parent_path()) break;
    }
    return {};
}

fs::path find_ipa() {
    for (fs::path d = exe_dir(); !d.empty(); d = d.parent_path()) {
        std::error_code ec;
        for (const char* sub : {"ipa", "."}) {
            fs::path dir = d / sub;
            if (!fs::is_directory(dir, ec)) continue;
            for (auto& e : fs::directory_iterator(dir, ec)) {
                if (e.path().extension() == ".ipa") return e.path();
            }
        }
        if (d == d.parent_path()) break;
    }
    return {};
}

bool run_process(const std::wstring& cmdline) {
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::wstring cl = cmdline;
    if (!CreateProcessW(nullptr, cl.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}

// Extracts Payload/*.app from the IPA (a zip) using Windows' bundled bsdtar.
fs::path extract_ipa(const fs::path& ipa, const fs::path& dest_root) {
    fs::path marker = dest_root / ".extracted";
    std::error_code ec;
    auto find_app = [&]() -> fs::path {
        fs::path payload = dest_root / "Payload";
        if (!fs::is_directory(payload, ec)) return {};
        for (auto& e : fs::directory_iterator(payload, ec))
            if (e.path().extension() == ".app") return e.path();
        return {};
    };
    if (fs::exists(marker, ec)) {
        fs::path a = find_app();
        if (!a.empty()) return a;
    }
    LOGI("extracting %s ...", narrow(ipa.wstring()).c_str());
    fs::create_directories(dest_root, ec);
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + ipa.wstring() + L"\" -C \"" + dest_root.wstring() + L"\"";
    if (!run_process(cmd)) fatal("failed to extract %s (tar.exe)", narrow(ipa.wstring()).c_str());
    FILE* f = _wfopen(marker.c_str(), L"w");
    if (f) fclose(f);
    fs::path a = find_app();
    if (a.empty()) fatal("no Payload/*.app inside %s", narrow(ipa.wstring()).c_str());
    return a;
}

Options parse_args(int argc, wchar_t** argv) {
    Options o;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? narrow(argv[++i]) : std::string(); };
        if (a == L"--ipa") o.ipa = next();
        else if (a == L"--app") o.app_dir = next();
        else if (a == L"--data") o.data_dir = next();
        else if (a == L"--guest-libs") o.guest_dir = next();
        else if (a == L"-v" || a == L"--verbose") o.verbose = true;
        else if (a == L"--msltest") { o.msltest = i + 1 < argc ? argv[++i] : L""; if (i + 1 < argc && argv[i + 1][0] != L'-') o.msltest_out = argv[++i]; }
        else if (a == L"-vv") { o.verbose = true; g_log_level = LogLevel::Trace; }
        else if (a.size() > 4 && a.substr(a.size() - 4) == L".ipa") o.ipa = narrow(a);
    }
    return o;
}

LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    static std::atomic<bool> once{false};
    if (once.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;
    auto* er = ep->ExceptionRecord;
    LOGE("host exception 0x%08lx at %p (addr %p)", er->ExceptionCode, er->ExceptionAddress,
         er->NumberParameters > 1 ? (void*)er->ExceptionInformation[1] : nullptr);
    if (g_cpu) g_cpu->dump_state("host exception while running guest code");
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI first_chance(EXCEPTION_POINTERS* ep) {
    static std::atomic<int> n{0};
    auto* er = ep->ExceptionRecord;
    DWORD c = er->ExceptionCode;
    if (c == 0x406D1388 || c == DBG_PRINTEXCEPTION_C || c == 0x4001000A) return EXCEPTION_CONTINUE_SEARCH;  // thread names, debug output
    if (n++ < 40) {
        HMODULE mod = nullptr;
        wchar_t mname[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)er->ExceptionAddress, &mod))
            GetModuleFileNameW(mod, mname, MAX_PATH);
        LOGW("  in module %s (+0x%llx)", narrow(mname).c_str(), (unsigned long long)((char*)er->ExceptionAddress - (char*)mod));
        gl_dump_trace();
        LOGW("first-chance exception 0x%08lx at %p (param %p) thread %u", c, er->ExceptionAddress,
             er->NumberParameters > 1 ? (void*)er->ExceptionInformation[1] : nullptr, threads::current() ? threads::current()->id : 0);
        if (g_cpu && (c == 0xE06D7363 || (c == 0xC0000005 && !wcsstr(mname, L"nvogl")))) g_cpu->dump_state("host exception");
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

std::shared_ptr<MachImage> g_main;

gaddr resolve(const MachImage& img, const MachBind& b, bool& ok) {
    const std::string& s = b.symbol;
    // 1. exported by a guest image (our runtime libraries first, then the app)
    for (auto& i : macho::images()) {
        if (i.get() == &img && b.weak_def) continue;
        auto it = i->exports.find(s);
        if (it != i->exports.end()) return it->second;
    }
    // 2. host implementation
    if (gaddr a = hle::lookup(s)) return a;
    // 3. Objective-C classes we do not implement: synthesize empty subclasses of NSObject
    if (s.rfind("_OBJC_CLASS_$_", 0) == 0) return objc_stubs::synthesize_class(s.substr(14), false);
    if (s.rfind("_OBJC_METACLASS_$_", 0) == 0) return objc_stubs::synthesize_class(s.substr(18), true);
    // 4. placeholders that report when used
    const MachSection* la = img.find_section("__DATA", "__la_symbol_ptr");
    bool lazy_slot = b.lazy || (la && b.where >= la->addr && b.where < la->addr + la->size);
    bool looks_data = !lazy_slot && (s.rfind("_k", 0) == 0 || s.find("Key") != std::string::npos ||
                                     s.find("Notification") != std::string::npos || s.find("Domain") != std::string::npos ||
                                     s.rfind("__ZTV", 0) == 0 || s.rfind("__ZTI", 0) == 0 || s.rfind("_OBJC_", 0) == 0);
    // Chained-fixup images have no lazy pointers, so constants are recognized by name:
    // NSString constants (NSFooKey, UIFooNotification, CHHapticEventType..., ...) get a
    // constant string with the symbol's name, other data zeroed memory.
    static const std::regex str_const(
        "^_(NS|UI|CH|WK|AV|GC|MTL|CA|SK|PH|UN|CT|CF|kCF|kSec|kCG|kCT|kUT)[A-Za-z0-9_]*"
        "(Key|Notification|Name|Identifier|Gregorian|Size|Domain|Mode|Type[A-Za-z]*|Cookies|Option[A-Za-z]*|"
        "Attribute[A-Za-z]*|Continuous|Transient|String|Value[A-Za-z]*|Intensity|Sharpness)$");
    if (!lazy_slot && std::regex_match(s, str_const)) {
        gaddr cls = macho::find_export("___CFConstantStringClassReference");
        if (cls) {
            std::string text = s.substr(1);
            char* str = static_cast<char*>(vmem::gmalloc(text.size() + 1));
            memcpy(str, text.c_str(), text.size() + 1);
            u64* cfs = static_cast<u64*>(vmem::gcalloc(4, 8));
            cfs[0] = cls;
            cfs[1] = 0x7c8;
            cfs[2] = gaddr_of(str);
            cfs[3] = text.size();
            u64* var = static_cast<u64*>(hle::make_unimplemented_data_ptr(s));
            *var = gaddr_of(cfs);
            LOGD("string constant %s synthesized", s.c_str());
            ok = true;
            return gaddr_of(var);
        }
    }
    if (!lazy_slot && (s.rfind("_UIAccessibilityTrait", 0) == 0 || s.find("_struct") != std::string::npos)) looks_data = true;
    // weak function imports stay NULL so that availability checks work; weak data gets zeroed memory
    if (b.weak_import && !looks_data) { LOGD("weak import %s left NULL", s.c_str()); return 0; }
    static std::unordered_set<std::string> reported;
    if (reported.insert(s).second)
        LOGD("unresolved %s %s (from %s)", looks_data ? "data" : "function", s.c_str(), img.install_name.c_str());
    ok = true;
    return looks_data ? hle::make_unimplemented_data(s) : hle::make_unimplemented_stub(s);
}

// @123 literals (NSConstantIntegerNumber {isa; const char* encoding; long long value;}) are
// rewritten into the guest NSNumber layout {isa; char type; (pad) long long value;}
// (guest/Foundation/NSConstant.m).
void fix_constant_literals(MachImage& img) {
    size_t n = 0;
    for (auto& s : img.sections) {
        if (s.sectname != "__objc_intobj") continue;
        for (gaddr a = s.addr; a + 24 <= s.addr + s.size; a += 24) {
            u64* o = gptr<u64>(a);
            const char* enc = gptr<const char>(o[1]);
            char t = enc && vmem::is_mapped(o[1]) ? enc[0] : 'q';
            o[1] = (u8)t;
            n++;
        }
    }
    if (n) LOGD("%s: %zu integer literals", img.install_name.c_str(), n);
}

void run_initializers(MachImage& img, Cpu& cpu, gaddr argv, gaddr envp, gaddr apple) {
    for (auto& s : img.sections) {
        u32 type = s.flags & 0xff;
        if (type != 0x9 && type != 0x16) continue;  // S_MOD_INIT_FUNC_POINTERS / S_INIT_FUNC_OFFSETS
        u64 n = s.size / (type == 0x16 ? 4 : 8);
        LOGI("running %llu initializers of %s", (unsigned long long)n, img.install_name.c_str());
        for (u64 i = 0; i < n; i++) {
            gaddr fn = type == 0x16 ? img.base + gptr<u32>(s.addr)[i] : gptr<u64>(s.addr)[i];
            if (!fn) continue;
            LOGT("  init %s", macho::symbolicate(fn).c_str());
            cpu.call(fn, {1, argv, envp, apple});
        }
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (auth::forward_callback(argc, argv)) return 0;  // started by the browser after a sign-in
    SetConsoleOutputCP(CP_UTF8);
    SetUnhandledExceptionFilter(crash_filter);
    if (getenv("EMU_TRACE_EXCEPTIONS")) AddVectoredExceptionHandler(1, first_chance);
    _set_invalid_parameter_handler([](const wchar_t* expr, const wchar_t* fn, const wchar_t* file, unsigned line, uintptr_t) {
        LOGE("CRT invalid parameter: %s in %s (%s:%u)", expr ? narrow(expr).c_str() : "?", fn ? narrow(fn).c_str() : "?",
             file ? narrow(file).c_str() : "?", line);
        if (g_cpu) g_cpu->dump_state("CRT invalid parameter");
    });
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    signal(SIGABRT, [](int) {
        LOGE("host abort()");
        if (g_cpu) g_cpu->dump_state("host abort");
        fflush(stderr);
        ExitProcess(3);
    });
    std::set_terminate([] {
        LOGE("host std::terminate called");
        if (g_cpu) g_cpu->dump_state("std::terminate");
        fflush(stderr);
        ExitProcess(3);
    });
    Options opt = parse_args(argc, argv);
    if (opt.verbose && g_log_level > LogLevel::Debug) g_log_level = LogLevel::Debug;

    fs::path root = opt.data_dir.empty() ? local_appdata() / L"iOS-MCPE-1.26.62" : fs::path(widen(opt.data_dir));
    std::error_code ec;
    fs::create_directories(root, ec);
    fs::create_directories(root / L"mods", ec);  // arm64 dylib mods (see mod-template/)
    log_open_file(narrow((root / L"emulator.log").wstring()));
    LOGI("iOS MCPE 1.26.62 emulator starting; data dir %s", narrow(root.wstring()).c_str());
    controls::init((root / L"controls.ini").wstring());
    reset_unsupported_graphics_mode(root / L"home" / L"Documents" / L"games" / L"com.mojang" / L"minecraftpe" / L"options.txt");
    // (0.16.2's shader packs and OptiFine-style video settings drew through its OpenGL ES
    // renderer; 1.26 renders with Metal, so they are not used: vopts keeps its defaults)
    if (!opt.msltest.empty()) {
        int msl_selftest(const std::wstring& dir, const std::wstring& outdir);
        ExitProcess((UINT)msl_selftest(opt.msltest, opt.msltest_out));
    }
    // 1 ms timer resolution: frame pacing and the guest's sleeps / timed waits would
    // otherwise be rounded up to the default 15.6 ms scheduler tick.
    timeBeginPeriod(1);
    profiler::init();
    remote::start();  // phone screen ([remote] in controls.ini)

    // ---- locate the game ----
    fs::path app;
    if (!opt.app_dir.empty()) app = widen(opt.app_dir);
    else {
        fs::path ipa = opt.ipa.empty() ? find_ipa() : fs::path(widen(opt.ipa));
        if (ipa.empty()) fatal("Minecraft 1.26.62 .ipa not found. Put it into an 'ipa' folder next to the emulator or pass --ipa <file>.");
        app = extract_ipa(ipa, root / L"app");
    }
    if (!fs::exists(app / L"minecraftpe")) fatal("%s/minecraftpe not found", narrow(app.wstring()).c_str());

    fs::path guest_dir = opt.guest_dir.empty() ? exe_dir() / L"guest" : fs::path(widen(opt.guest_dir));
    if (!fs::exists(guest_dir / L"libc++.dylib")) {
        fs::path g = search_up(exe_dir(), L"out/guest");
        if (!g.empty()) guest_dir = g;
    }
    if (!fs::exists(guest_dir / L"libc++.dylib"))
        fatal("guest runtime libraries not found (expected %s). Run: py guest/build.py", narrow(guest_dir.wstring()).c_str());

    vfs::init(narrow(app.wstring()), narrow((root / L"home").wstring()));
    hle::init();
    GuestThread* mt = threads::create_main_thread(8ull << 20);
    Cpu& cpu = *mt->cpu;

    // ---- load images ----
    // The app first, so that it gets its preferred address (0x100000000) and dyld image index 0.
    g_main = macho::load(narrow((app / L"minecraftpe").wstring()), "minecraftpe", 0);
    macho::register_image(g_main);
    struct LibDef { const wchar_t* file; const char* install; };
    const LibDef libs[] = {
        {L"libc++.dylib", "/usr/lib/libc++.1.dylib"},
        {L"libobjc.dylib", "/usr/lib/libobjc.A.dylib"},
        {L"libz.dylib", "/usr/lib/libz.1.dylib"},
        {L"libdispatch.dylib", "/usr/lib/system/libdispatch.dylib"},
        {L"Foundation.dylib", "/System/Library/Frameworks/Foundation.framework/Foundation"},
        {L"UIKit.dylib", "/System/Library/Frameworks/UIKit.framework/UIKit"},
        {L"Metal.dylib", "/System/Library/Frameworks/Metal.framework/Metal"},
    };
    std::vector<std::shared_ptr<MachImage>> dylibs;
    gaddr next_base = 0x300000000ull;
    // The app's own frameworks (Xbox Live / HTTP / PlayFab). MinecraftSwift (the StoreKit 2
    // bridge, needs the Swift runtime) is not loaded: its StoreKitBridge class is synthesized.
    std::vector<std::shared_ptr<MachImage>> frameworks;
    for (const wchar_t* fw : {L"HttpClient", L"Xal", L"xsapi-c", L"PlayFabMultiplayerFramework_iOS"}) {
        fs::path p = app / L"Frameworks" / (std::wstring(fw) + L".framework") / fw;
        if (!fs::exists(p)) { LOGW("framework %s missing", narrow(p.wstring()).c_str()); continue; }
        std::string inst = "@rpath/" + narrow(fw) + ".framework/" + narrow(fw);
        auto img = macho::load(narrow(p.wstring()), inst, next_base);
        next_base = align_up(img->vm_end, 0x10000000ull);
        macho::register_image(img);
        frameworks.push_back(img);
    }
    for (auto& l : libs) {
        fs::path p = guest_dir / l.file;
        if (!fs::exists(p)) { LOGW("guest library %s missing", narrow(p.wstring()).c_str()); continue; }
        auto img = macho::load(narrow(p.wstring()), l.install, next_base);
        next_base = align_up(img->vm_end, 0x10000000ull);
        macho::register_image(img);
        dylibs.push_back(img);
    }
    // Mods: arm64 dylibs in <data>/mods (see mod-template/). They are bound like the
    // runtime libraries and their constructors run before the game's initializers.
    {
        std::vector<fs::path> mods;
        for (auto& e : fs::directory_iterator(root / L"mods", ec))
            if (e.is_regular_file() && e.path().extension() == L".dylib") mods.push_back(e.path());
        std::sort(mods.begin(), mods.end());
        for (auto& p : mods) {
            LOGI("loading mod %s", narrow(p.filename().wstring()).c_str());
            auto img = macho::load(narrow(p.wstring()), "@mods/" + narrow(p.filename().wstring()), next_base);
            next_base = align_up(img->vm_end, 0x10000000ull);
            macho::register_image(img);
            dylibs.push_back(img);
        }
    }
    for (auto& i : dylibs) macho::bind_all(*i, resolve);
    for (auto& i : frameworks) macho::bind_all(*i, resolve);
    macho::bind_all(*g_main, resolve);
    for (auto& i : macho::images()) tlv::register_image(*i);
    for (auto& i : macho::images()) fix_constant_literals(*i);
    void install_debug_hooks();
    install_debug_hooks();  // EMU_LOGHOOK
    patches::apply_game_fixes();  // (patches.cpp)

    // ---- process arguments ----
    std::string exe_path = std::string(vfs::kBundlePath) + "/minecraftpe";
    auto gstr = [](const std::string& s) {
        char* p = static_cast<char*>(vmem::gmalloc(s.size() + 1));
        memcpy(p, s.c_str(), s.size() + 1);
        return gaddr_of(p);
    };
    u64* gargv = static_cast<u64*>(vmem::gcalloc(4, 8));
    gargv[0] = gstr(exe_path);
    u64* genvp = static_cast<u64*>(vmem::gcalloc(4, 8));
    genvp[0] = gstr(std::string("HOME=") + vfs::kHomePath);
    u64* gapple = static_cast<u64*>(vmem::gcalloc(4, 8));
    gapple[0] = gstr("executable_path=" + exe_path);

    // ---- initializers: runtime libraries in dependency order, then the app ----
    for (auto& i : dylibs) run_initializers(*i, cpu, gaddr_of(gargv), gaddr_of(genvp), gaddr_of(gapple));
    // frameworks in dependency order (HttpClient <- Xal <- xsapi-c)
    for (auto& i : frameworks) run_initializers(*i, cpu, gaddr_of(gargv), gaddr_of(genvp), gaddr_of(gapple));
    run_initializers(*g_main, cpu, gaddr_of(gargv), gaddr_of(genvp), gaddr_of(gapple));

    if (const char* wd = getenv("EMU_WATCHDOG")) {
        double secs = atof(wd);
        std::thread([secs] {
            for (;;) {
                Sleep((DWORD)(secs * 1000));
                LOGW("==== watchdog: dumping %zu guest threads ====", threads::all().size());
                for (GuestThread* t : threads::all()) {
                    if (t->finished || !t->cpu) continue;
                    t->cpu->dump_state("watchdog");
                }
            }
        }).detach();
    }
    LOGI("calling main at %s", macho::symbolicate(g_main->entry).c_str());
    u64 r = cpu.call(g_main->entry, {1, gaddr_of(gargv), gaddr_of(genvp), gaddr_of(gapple)});
    LOGI("main returned %d", (int)r);
    fflush(stdout);
    ExitProcess((UINT)r);
}
