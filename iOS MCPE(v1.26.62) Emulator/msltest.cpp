// Developer self test: iOSMCPE-Emulator.exe --msltest <dir with .metal files> [out dir]
// Translates every MSL file and compiles the result with the GL driver; prints a summary.
#include "common.h"
#include "msl2glsl.h"
#include "window.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

int msl_selftest(const std::wstring& dir, const std::wstring& outdir) {
    if (!window::create(320, 240, "msltest")) return 1;
    using PFNCREATE = GLuint(APIENTRY*)(GLenum);
    using PFNSOURCE = void(APIENTRY*)(GLuint, GLsizei, const char* const*, const GLint*);
    using PFNCOMPILE = void(APIENTRY*)(GLuint);
    using PFNGETIV = void(APIENTRY*)(GLuint, GLenum, GLint*);
    using PFNLOG = void(APIENTRY*)(GLuint, GLsizei, GLsizei*, char*);
    using PFNDEL = void(APIENTRY*)(GLuint);
    auto cs = (PFNCREATE)window::gl_proc("glCreateShader");
    auto ss = (PFNSOURCE)window::gl_proc("glShaderSource");
    auto cp = (PFNCOMPILE)window::gl_proc("glCompileShader");
    auto gi = (PFNGETIV)window::gl_proc("glGetShaderiv");
    auto gl = (PFNLOG)window::gl_proc("glGetShaderInfoLog");
    auto ds = (PFNDEL)window::gl_proc("glDeleteShader");
    int total = 0, terr = 0, cerr = 0;
    std::map<std::string, int> reasons;
    std::error_code ec;
    if (!outdir.empty()) fs::create_directories(outdir, ec);
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() != L".metal") continue;
        std::ifstream f(e.path(), std::ios::binary);
        std::stringstream ss_;
        ss_ << f.rdbuf();
        auto lib = msl::translate(ss_.str());
        std::string name = narrow(e.path().filename().wstring());
        if (!lib->error.empty()) {
            terr++;
            reasons[lib->error.substr(0, 60)]++;
            LOGW("%s: %s", name.c_str(), lib->error.c_str());
            continue;
        }
        for (auto& fn : lib->functions) {
            total++;
            if (!fn->error.empty()) {
                terr++;
                std::string r = fn->error.substr(0, fn->error.find(" near"));
                reasons[r]++;
                LOGW("%s: %s", name.c_str(), fn->error.c_str());
                continue;
            }
            std::string glsl = msl::emit_stage(*fn, fn->stage == msl::Stage::Fragment ? 31 : 0, fn->stage == msl::Stage::Vertex ? 32 : 0, nullptr, 8, 8, 1);
            GLenum type = fn->stage == msl::Stage::Vertex ? 0x8B31 : fn->stage == msl::Stage::Fragment ? 0x8B30 : 0x91B9;
            GLuint sh = cs(type);
            const char* p = glsl.c_str();
            ss(sh, 1, &p, nullptr);
            cp(sh);
            GLint ok = 0;
            gi(sh, 0x8B81, &ok);
            if (!ok) {
                cerr++;
                char log[4096] = {0};
                gl(sh, sizeof log, nullptr, log);
                std::string l = log;
                std::string first = l.substr(0, l.find('\n'));
                // "0(123) : error C1234: msg" -> the message
                size_t c = first.find("error");
                reasons["GL " + (c == std::string::npos ? first : first.substr(c)).substr(0, 90)]++;
                LOGW("%s: compile failed: %s", name.c_str(), log);
                if (!outdir.empty()) {
                    std::ofstream o(fs::path(outdir) / (e.path().stem().wstring() + L".glsl"), std::ios::binary);
                    o << glsl;
                }
            }
            ds(sh);
            if (!outdir.empty() && getenv("EMU_MSLTEST_ALL")) {
                std::ofstream o(fs::path(outdir) / (e.path().stem().wstring() + L".glsl"), std::ios::binary);
                o << glsl;
            }
        }
    }
    LOGI("msltest: %d functions, %d translation errors, %d compile errors", total, terr, cerr);
    std::vector<std::pair<int, std::string>> v;
    for (auto& [r, n] : reasons) v.push_back({n, r});
    std::sort(v.rbegin(), v.rend());
    for (auto& [n, r] : v) LOGI("  %5d  %s", n, r.c_str());
    return 0;
}
