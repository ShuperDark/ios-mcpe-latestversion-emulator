#include "vfs.h"

#include <filesystem>
#include <mutex>
#include <set>

namespace vfs {
namespace {
std::string g_bundle, g_data;
std::mutex g_lock;
std::string g_cwd = "/";

struct Mount {
    std::string guest;  // no trailing slash
    std::wstring host;
};
std::vector<Mount> g_mounts;

// Files that mods add to the app (emu_bundle_write): kept in <data>/bundle_overlay for this run only
// (emptied at every start, so a removed mod leaves nothing behind) and seen by the game in place of
// / next to the app's own files. Folders that exist only there are mapped too.
std::wstring g_overlay;
std::mutex g_overlay_lock;
std::set<std::string> g_overlay_paths;  // relative to the bundle, "/a/b.json" (lower case)

std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string normalize(const std::string& p) {
    // resolve "." and ".." and duplicate slashes
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string s = p.substr(i, j - i);
        if (s.empty() || s == ".") {
        } else if (s == "..") {
            if (!parts.empty()) parts.pop_back();
        } else
            parts.push_back(s);
        i = j + 1;
    }
    std::string r;
    for (auto& s : parts) r += "/" + s;
    return r.empty() ? "/" : r;
}
}  // namespace

void init(const std::string& host_bundle_dir, const std::string& host_data_dir) {
    g_bundle = host_bundle_dir;
    g_data = host_data_dir;
    std::error_code ec;
    for (const char* d : {"Documents", "Library", "Library/Caches", "Library/Preferences", "tmp", "root"})
        std::filesystem::create_directories(std::filesystem::path(widen(g_data)) / d, ec);
    g_overlay = widen(g_data + "/bundle_overlay");
    std::filesystem::remove_all(g_overlay, ec);
    {
        std::lock_guard lk(g_overlay_lock);
        g_overlay_paths.clear();
    }
    g_mounts.clear();
    g_mounts.push_back({kBundlePath, widen(g_bundle)});
    g_mounts.push_back({kHomePath, widen(g_data)});
    g_mounts.push_back({"/tmp", widen(g_data + "/tmp")});
    g_mounts.push_back({"/private/var/mobile/Containers/Data/Application/5E6F7A8B-0000-4000-8000-000000016200", widen(g_data)});
    g_cwd = "/";
}

const std::string& host_bundle_dir() { return g_bundle; }

bool overlay_write(const std::string& rel, const void* data, size_t len) {
    namespace fs = std::filesystem;
    std::string r = normalize("/" + rel);
    fs::path p = fs::path(g_overlay) / fs::path(widen(r.substr(1)));
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    FILE* f = _wfopen(p.c_str(), L"wb");
    if (!f) return false;
    bool ok = !len || fwrite(data, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    if (!ok) return false;
    std::lock_guard lk(g_overlay_lock);
    g_overlay_paths.insert(lower(r));
    // its folders, where the app has none
    for (size_t i = r.find('/', 1); i != std::string::npos; i = r.find('/', i + 1)) {
        std::string d = r.substr(0, i);
        if (!fs::is_directory(fs::path(widen(g_bundle + d)), ec)) g_overlay_paths.insert(lower(d));
    }
    return true;
}
const std::string& host_data_dir() { return g_data; }

std::string cwd() {
    std::lock_guard lk(g_lock);
    return g_cwd;
}
bool set_cwd(const char* p) {
    std::lock_guard lk(g_lock);
    std::string full = (p && p[0] == '/') ? std::string(p) : g_cwd + "/" + (p ? p : "");
    g_cwd = normalize(full);
    return true;
}

std::wstring to_host(const char* gp) {
    if (!gp) return L"";
    std::string full;
    if (gp[0] == '/') full = gp;
    else {
        std::lock_guard lk(g_lock);
        full = g_cwd + "/" + gp;
    }
    full = normalize(full);
    for (auto& m : g_mounts) {
        if (full == m.guest) return m.host;
        if (full.size() > m.guest.size() && full.compare(0, m.guest.size(), m.guest) == 0 && full[m.guest.size()] == '/') {
            std::wstring rest = widen(full.substr(m.guest.size()));
            for (auto& c : rest) if (c == L'/') c = L'\\';
            if (&m == &g_mounts[0]) {  // the bundle: a file a mod added?
                std::lock_guard lk(g_overlay_lock);
                if (!g_overlay_paths.empty() && g_overlay_paths.count(lower(full.substr(m.guest.size())))) return g_overlay + rest;
            }
            return m.host + rest;
        }
    }
    // everything else lives in a private sandbox root
    std::wstring rest = widen(full);
    for (auto& c : rest) if (c == L'/') c = L'\\';
    return widen(g_data + "/root") + rest;
}

std::string to_guest(const std::wstring& hp) {
    std::wstring h = hp;
    for (auto& c : h) if (c == L'\\') c = L'/';
    for (auto& m : g_mounts) {
        std::wstring mh = m.host;
        for (auto& c : mh) if (c == L'\\') c = L'/';
        if (h.size() >= mh.size() && _wcsnicmp(h.c_str(), mh.c_str(), mh.size()) == 0 &&
            (h.size() == mh.size() || h[mh.size()] == L'/'))
            return m.guest + narrow(h.substr(mh.size()));
    }
    std::wstring root = widen(g_data + "/root");
    for (auto& c : root) if (c == L'\\') c = L'/';
    if (h.size() >= root.size() && _wcsnicmp(h.c_str(), root.c_str(), root.size()) == 0) {
        std::string r = narrow(h.substr(root.size()));
        return r.empty() ? "/" : r;
    }
    return narrow(h);
}

bool is_readonly(const char* gp) {
    if (!gp) return false;
    std::string full;
    if (gp[0] == '/') full = gp;
    else {
        std::lock_guard lk(g_lock);
        full = g_cwd + "/" + gp;
    }
    full = normalize(full);
    std::string b = kBundlePath;
    return full == b || (full.size() > b.size() && full.compare(0, b.size(), b) == 0 && full[b.size()] == '/');
}

bool is_dev_random(const char* p) { return p && (!strcmp(p, "/dev/urandom") || !strcmp(p, "/dev/random")); }
bool is_dev_null(const char* p) { return p && !strcmp(p, "/dev/null"); }

}  // namespace vfs
