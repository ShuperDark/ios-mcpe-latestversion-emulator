#include "profiler.h"
#include "common.h"
#include "hle.h"
#include "cpu.h"
#include "macho.h"
#include "vmem.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Dynarmic::Backend::X64 {
extern void (*g_block_registered_hook)(const void* start, size_t size, std::uint64_t location);
extern void (*g_perf_map_hook)(const void* start, const void* end, std::string_view name);
}

namespace profiler {
namespace {

// EMU_PROFILE_GUEST: compiled blocks (host code start -> size, guest pc) to find the guest
// code a JIT sample is in
std::mutex g_blocks_lock;
std::map<u64, std::pair<u64, u64>> g_blocks;
void on_block(const void* start, size_t size, std::uint64_t location) {
    std::lock_guard lk(g_blocks_lock);
    g_blocks[(u64)start] = {size, location & 0x00FFFFFFFFFFFFFFull};
}
// dynarmic's own code: start -> (end, name); reported as pseudo guest pcs 1, 2, ...
std::map<u64, std::pair<u64, u32>> g_special;
std::vector<std::string> g_special_names{""};
void on_perf_map(const void* start, const void* end, std::string_view name) {
    std::lock_guard lk(g_blocks_lock);
    u32 id = 0;
    for (u32 i = 1; i < g_special_names.size(); i++)
        if (g_special_names[i] == name) id = i;
    if (!id) { id = (u32)g_special_names.size(); g_special_names.emplace_back(name); }
    g_special[(u64)start] = {(u64)end, id};
}
std::string special_name(u64 pc) {
    std::lock_guard lk(g_blocks_lock);
    return pc < g_special_names.size() ? g_special_names[pc] : std::string();
}
u64 guest_pc_of(u64 rip) {
    std::lock_guard lk(g_blocks_lock);
    auto it = g_blocks.upper_bound(rip);
    if (it != g_blocks.begin()) {
        --it;
        if (rip < it->first + it->second.first) return it->second.second;
    }
    auto sp = g_special.upper_bound(rip);
    if (sp != g_special.begin()) {
        --sp;
        if (rip < sp->second.first) return sp->second.second;
    }
    return 0;
}

struct Entry {
    DWORD tid;
    HANDLE h;
    std::string name;
    std::unordered_map<u64, u32> rips;
    std::unordered_map<u64, u32> guest;  // EMU_PROFILE_GUEST: guest return addresses (inclusive)
    std::unordered_map<u64, u32> gwait;  // ... of samples outside guest code (HLE calls, waits)
    Cpu* cpu = nullptr;
    u64 samples = 0;
};

std::mutex g_lock;
std::vector<Entry*> g_entries;
std::vector<Entry*> g_retired;  // finished threads, reported once more
bool g_enabled = false;
bool g_guest = false;
DWORD g_period_ms = 1;  // EMU_PROFILE_PERIOD: ms between samples (lower overhead)

// Guest call stack of a sample in JIT code: dynarmic keeps its A64JitState in r15 (reg[0..30]
// first); x29/x30 there are the values at the start of the current block, good enough to
// walk the frame records {fp, lr}.
int guest_stack_fp(u64 fp, u64 lr, u64* out, int max) {
    int n = 0;
    __try {
        if (lr) out[n++] = lr & 0x0000ffffffffffffull;
        for (int i = 0; i < 48 && n < max; i++) {
            if (!fp || (fp & 7) || fp < 0x10000 || !vmem::is_mapped(fp, 16)) break;
            const u64* fr = reinterpret_cast<const u64*>(fp);
            u64 ret = fr[1], nfp = fr[0];
            if (!ret) break;
            out[n++] = ret & 0x0000ffffffffffffull;
            if (nfp <= fp) break;
            fp = nfp;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}
int guest_stack(u64 r15, u64* out, int max) {
    __try {
        const u64* reg = reinterpret_cast<const u64*>(r15);
        return guest_stack_fp(reg[29], reg[30], out, max);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
void add_stack(std::unordered_map<u64, u32>& m, const u64* st, int n) {
    for (int i = 0; i < n; i++) {
        bool dup = false;  // recursion: count a function once per sample
        for (int j = 0; j < i && !dup; j++) dup = st[j] == st[i];
        if (!dup) m[st[i]]++;
    }
}
bool in_module(u64 rip) {
    HMODULE mod = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(rip), &mod) != 0;
}
double g_interval = 10;

struct Sym {
    std::string name;
    int kind;  // 0 guest code (JIT), 1 emulator, 2 OS / other module
};

std::unordered_map<u64, Sym> g_sym_cache;

Sym classify(u64 rip) {
    auto it = g_sym_cache.find(rip);
    if (it != g_sym_cache.end()) return it->second;
    Sym s;
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(rip), &mod)) {
        s = {"[guest code (JIT)]", 0};
    } else {
        char buf[sizeof(SYMBOL_INFO) + 512];
        auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 511;
        DWORD64 disp = 0;
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(mod, path, MAX_PATH);
        std::wstring p = path;
        std::string modname = narrow(p.substr(p.find_last_of(L"\\/") + 1));
        bool exe = mod == GetModuleHandleW(nullptr);
        std::string fn = SymFromAddr(GetCurrentProcess(), rip, &disp, si) ? std::string(si->Name) : "?";
        s = {exe ? fn : modname + "!" + fn, exe ? 1 : 2};
    }
    g_sym_cache.emplace(rip, s);
    return s;
}

void report() {
    std::vector<Entry*> all;
    {
        std::lock_guard lk(g_lock);
        all = g_entries;
        all.insert(all.end(), g_retired.begin(), g_retired.end());
    }
    // group threads by name
    struct Group {
        u64 samples = 0, threads = 0;
        u64 kind[3] = {};
        u64 jitc = 0;  // the JIT compiler (dynarmic / xbyak)
        std::unordered_map<u64, u64> self;  // guest block -> samples
        std::map<std::string, u64> syms;
    };
    std::map<std::string, Group> groups;
    {
        std::lock_guard lk(g_lock);
        for (Entry* e : all) {
            Group& g = groups[e->name];
            g.threads++;
            g.samples += e->samples;
            for (auto& [rip, n] : e->rips) {
                Sym s = classify(rip);
                g.kind[s.kind] += n;
                if (g_guest && s.kind == 0) g.self[guest_pc_of(rip)] += n;
                if (s.kind == 1 && (s.name.find("Dynarmic") != std::string::npos || s.name.find("Xbyak") != std::string::npos ||
                                    s.name.find("robin_hash") != std::string::npos || s.name.find("_Finding") != std::string::npos ||
                                    s.name.find("mcl::") != std::string::npos || s.name.find("boost::") != std::string::npos))
                    g.jitc += n;
                g.syms[s.name] += n;
            }
            e->rips.clear();
            e->samples = 0;
        }
    }
    LOGI("==== profile (last %.0f s, 1 sample per %lu ms per thread) ====", g_interval, g_period_ms);
    for (auto& [name, g] : groups) {
        if (!g.samples) continue;
        // waiting threads sit in ntdll / kernelbase wait functions
        u64 wait = 0;
        for (auto& [sn, n] : g.syms)
            if (sn.find("NtWait") != std::string::npos || sn.find("NtDelay") != std::string::npos ||
                sn.find("NtRemoveIoCompletion") != std::string::npos || sn.find("ZwWait") != std::string::npos ||
                sn.find("NtUserMsgWait") != std::string::npos || sn.find("SleepConditionVariable") != std::string::npos)
                wait += n;
        double busy = 100.0 * (g.samples - wait) / g.samples;
        LOGI("[%s] x%llu: busy %.0f%% | guest code %.1f%%, emulator %.1f%% (jit compiler %.1f%%), os %.1f%% (of all samples)",
             name.c_str(), (unsigned long long)g.threads, busy, 100.0 * g.kind[0] / g.samples, 100.0 * g.kind[1] / g.samples,
             100.0 * g.jitc / g.samples, 100.0 * g.kind[2] / g.samples);
        if (busy < 5) { for (Entry* e : all) if (e->name == name) { e->guest.clear(); e->gwait.clear(); } continue; }
        std::vector<std::pair<u64, std::string>> top;
        for (auto& [sn, n] : g.syms) top.emplace_back(n, sn);
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 14; i++)
            LOGI("    %5.1f%%  %s", 100.0 * top[i].first / g.samples, top[i].second.c_str());
        if (g_guest) {
            std::vector<std::pair<u64, u64>> st(g.self.begin(), g.self.end());
            std::sort(st.begin(), st.end(), [](auto& a, auto& b) { return a.second > b.second; });
            for (size_t i = 0; i < st.size() && i < 40; i++)
                LOGI("      self  %5.1f%%  block %s", 100.0 * st[i].second / g.samples,
                     st[i].first && st[i].first < 4096 ? special_name(st[i].first).c_str() : macho::symbolicate(st[i].first).c_str());
            std::unordered_map<u64, u64> gs;
            {
                std::lock_guard lk(g_lock);
                for (Entry* e : all)
                    if (e->name == name) { for (auto& [a, n] : e->guest) gs[a] += n; e->guest.clear(); }
            }
            std::unordered_map<u64, u64> gw;
            {
                std::lock_guard lk(g_lock);
                for (Entry* e : all)
                    if (e->name == name) { for (auto& [a, n] : e->gwait) gw[a] += n; e->gwait.clear(); }
            }
            std::vector<std::pair<u64, u64>> wt(gw.begin(), gw.end());
            std::sort(wt.begin(), wt.end(), [](auto& a, auto& b) { return a.second > b.second; });
            for (size_t i = 0; i < wt.size() && i < 30; i++)
                LOGI("      outside %5.1f%%  ret %s", 100.0 * wt[i].second / g.samples, macho::symbolicate(wt[i].first).c_str());
            std::vector<std::pair<u64, u64>> gt(gs.begin(), gs.end());
            std::sort(gt.begin(), gt.end(), [](auto& a, auto& b) { return a.second > b.second; });
            for (size_t i = 0; i < gt.size() && i < 60; i++)
                LOGI("      guest %5.1f%%  ret %s", 100.0 * gt[i].second / g.samples, macho::symbolicate(gt[i].first).c_str());
        }
    }
    {
        std::lock_guard lk(g_lock);
        for (Entry* e : all)
            if (std::find(g_entries.begin(), g_entries.end(), e) == g_entries.end()) {
                g_retired.erase(std::find(g_retired.begin(), g_retired.end(), e));
                delete e;
            }
    }
    hle::prof_report(g_interval);
}

void sampler() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (;;) {
        Sleep(g_period_ms);
        {
            std::lock_guard lk(g_lock);
            for (Entry* e : g_entries) {
                if (SuspendThread(e->h) == (DWORD)-1) continue;
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                // (nothing that allocates while the thread is suspended: it may hold the heap lock)
                BOOL ok = GetThreadContext(e->h, &ctx);
                ResumeThread(e->h);
                if (ok) {
                    e->rips[ctx.Rip]++;
                    e->samples++;
                    if (g_guest) {
                        u64 st[50];
                        if (!in_module(ctx.Rip)) add_stack(e->guest, st, guest_stack(ctx.R15, st, 50));
                        else if (e->cpu) add_stack(e->gwait, st, guest_stack_fp(e->cpu->x(29), e->cpu->x(30), st, 50));
                    }
                }
            }
        }
        QueryPerformanceCounter(&t);
        if (double(t.QuadPart - t0.QuadPart) / f.QuadPart >= g_interval) {
            report();
            QueryPerformanceCounter(&t0);
        }
    }
}

}  // namespace

void init() {
    const char* v = getenv("EMU_PROFILE");
    if (!v) return;
    g_interval = (std::max)(1.0, atof(v));
    g_enabled = true;
    g_guest = getenv("EMU_PROFILE_GUEST") != nullptr;
    if (const char* p = getenv("EMU_PROFILE_PERIOD")) g_period_ms = (std::max)(1, atoi(p));
    if (g_guest) {
        Dynarmic::Backend::X64::g_block_registered_hook = on_block;
        Dynarmic::Backend::X64::g_perf_map_hook = on_perf_map;
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    std::thread(sampler).detach();
    LOGI("profiler: reporting every %.0f s", g_interval);
}

void register_thread(const char* name) {
    if (!g_enabled) return;
    auto* e = new Entry;
    e->tid = GetCurrentThreadId();
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &e->h,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    e->name = name && *name ? name : "thread";
    e->cpu = g_cpu;
    std::lock_guard lk(g_lock);
    g_entries.push_back(e);
}

void set_thread_name(const char* name) {
    if (!g_enabled || !name) return;
    DWORD tid = GetCurrentThreadId();
    std::lock_guard lk(g_lock);
    for (Entry* e : g_entries)
        if (e->tid == tid) e->name = name;
}

void unregister_thread() {
    if (!g_enabled) return;
    DWORD tid = GetCurrentThreadId();
    std::lock_guard lk(g_lock);
    for (size_t i = 0; i < g_entries.size(); i++)
        if (g_entries[i]->tid == tid) {
            CloseHandle(g_entries[i]->h);
            g_retired.push_back(g_entries[i]);
            g_entries.erase(g_entries.begin() + i);
            return;
        }
}

}  // namespace profiler
