// The Microsoft account sign-in for the guest's ASWebAuthenticationSession (guest/UIKit/AuthenticationServices.m).
//
// On iOS the Xbox sign-in (Xal) shows login.live.com in a system web sheet and gets the
// result as a redirect to its own URL scheme (ms-xal-<title id>://auth?code=...). Here the
// page opens in the PC's default browser, and the scheme is registered for the current
// user (HKCU\Software\Classes) to start this program with --auth-callback <url>: that
// second instance hands the URL to the running emulator (WM_COPYDATA) and exits.
// A small dialog stays open meanwhile ("open the page again" / "cancel").
#include "auth.h"
#include "hle.h"
#include "window.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <mutex>
#include <string>
#include <thread>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace auth {
namespace {

constexpr ULONG_PTR kCopyDataTag = 0x41555448;  // 'AUTH'

std::mutex g_lock;
int g_state = 0;  // 0 idle / pending, 1 done, -1 cancelled
std::string g_result, g_scheme, g_url;
unsigned g_session = 0;      // a new session ends the dialog of the previous one
DWORD g_dialog_thread = 0;

bool set_reg(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    HKEY k;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    LSTATUS r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

// ms-xal-...:// -> "<this exe>" --auth-callback "%1" (per user, no administrator rights needed)
bool register_scheme(const std::string& scheme) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring base = L"Software\\Classes\\" + widen(scheme);
    bool ok = set_reg(HKEY_CURRENT_USER, base, nullptr, L"URL:" + widen(scheme) + L" (iOS MCPE Emulator sign-in)") &&
              set_reg(HKEY_CURRENT_USER, base, L"URL Protocol", L"") &&
              set_reg(HKEY_CURRENT_USER, base + L"\\shell\\open\\command", nullptr,
                      L"\"" + std::wstring(exe) + L"\" --auth-callback \"%1\"");
    if (!ok) LOGW("auth: could not register the URL scheme %s", scheme.c_str());
    return ok;
}

void open_browser(const std::string& url) {
    ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

BOOL CALLBACK close_thread_window(HWND h, LPARAM) {
    PostMessageW(h, WM_CLOSE, 0, 0);
    return TRUE;
}
void close_dialog_locked() {
    if (g_dialog_thread) EnumThreadWindows(g_dialog_thread, close_thread_window, 0);
}

void dialog_thread(unsigned session, std::string url) {
    {
        std::lock_guard lk(g_lock);
        g_dialog_thread = GetCurrentThreadId();
    }
    const wchar_t* text =
        L"ブラウザで Microsoft アカウントにサインインしてください。\n\n"
        L"サインインが終わると、ブラウザが「iOSMCPE-Emulator を開きますか?」と確認するので、「開く」を選んでください。"
        L"ゲームに戻ってサインインが続きます。\n\n"
        L"[再試行] サインインのページをもう一度開く\n"
        L"[キャンセル] サインインをやめる";
    for (;;) {
        int r = MessageBoxW(nullptr, text, L"Microsoft アカウントでサインイン", MB_RETRYCANCEL | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
        std::lock_guard lk(g_lock);
        if (session != g_session || g_state != 0) break;  // finished meanwhile (the box was closed for us)
        if (r == IDRETRY) {
            open_browser(url);
            continue;
        }
        g_state = -1;
        LOGI("auth: sign-in cancelled");
        break;
    }
    std::lock_guard lk(g_lock);
    if (g_dialog_thread == GetCurrentThreadId()) g_dialog_thread = 0;
}

void start_browser(unsigned session, const std::string& u, const std::string& s) {
    if (!s.empty()) register_scheme(s);
    open_browser(u);
    std::thread(dialog_thread, session, u).detach();
}

}  // namespace

bool forward_callback(int argc, wchar_t** argv) {
    if (argc < 3 || std::wstring(argv[1]) != L"--auth-callback") return false;
    std::string url = narrow(argv[2]);
    HWND w = FindWindowW(L"iOSMCPEWindow", nullptr);
    if (!w) {
        MessageBoxW(nullptr, L"iOS MCPE Emulator が起動していないため、サインインの結果を渡せませんでした。", L"iOS MCPE Emulator", MB_OK | MB_ICONWARNING);
        return true;
    }
    COPYDATASTRUCT cd{kCopyDataTag, (DWORD)url.size() + 1, (void*)url.c_str()};
    SendMessageW(w, WM_COPYDATA, 0, (LPARAM)&cd);
    if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
    SetForegroundWindow(w);  // (started by the browser, which was in the foreground: allowed)
    return true;
}

bool on_copydata(const void* p) {
    auto* cd = static_cast<const COPYDATASTRUCT*>(p);
    if (cd->dwData != kCopyDataTag || !cd->lpData || !cd->cbData) return false;
    std::string url(static_cast<const char*>(cd->lpData), strnlen(static_cast<const char*>(cd->lpData), cd->cbData));
    std::lock_guard lk(g_lock);
    if (g_state != 0 || g_scheme.empty() || _strnicmp(url.c_str(), (g_scheme + ":").c_str(), g_scheme.size() + 1) != 0) {
        LOGW("auth: unexpected callback (no sign-in in progress or another scheme)");
        return true;
    }
    LOGI("auth: sign-in callback received");
    g_result = url;
    g_state = 1;
    close_dialog_locked();
    return true;
}

// debug: EMU_TEST_AUTH_WINDOW=<url> opens the sign-in window at start-up with the callback
// scheme ms-xal-test (a page that redirects to ms-xal-test://... ends it) and logs the result
void test_window_from_env() {
    const char* u = getenv("EMU_TEST_AUTH_WINDOW");
    if (!u) return;
    LOGI("auth test: WebView2 %savailable", webview_available() ? "" : "not ");
    webview_begin(0, u, "ms-xal-test", [](int st, const std::string& url) { LOGI("auth test: result %d %s", st, url.c_str()); });
}

}  // namespace auth

// Opens the sign-in page; the result is polled with emu_auth_poll. Returns 0.
HLE_FUNC(emu_auth_begin, int, (const char* url, const char* scheme)) {
    using namespace auth;
    std::string u = url ? url : "", s = scheme ? scheme : "";
    unsigned session;
    {
        std::lock_guard lk(g_lock);
        close_dialog_locked();
        webview_close();
        g_state = 0;
        g_result.clear();
        g_scheme = s;
        g_url = u;
        session = ++g_session;
    }
    LOGI("auth: sign-in started (callback scheme %s)", s.c_str());
    // the page in the emulator's own sign-in window (WebView2); EMU_AUTH_BROWSER=1 or no
    // WebView2 runtime: in the PC's browser
    if (!getenv("EMU_AUTH_BROWSER") && webview_available()) {
        webview_begin(session, u, s, [session, u, s](int st, const std::string& url) {
            {
                std::lock_guard lk(g_lock);
                if (session != g_session || g_state != 0) return;
                if (st == 1) { g_result = url; g_state = 1; return; }
                if (st == -1) { g_state = -1; LOGI("auth: sign-in cancelled"); return; }
            }
            LOGW("auth: the sign-in window failed, opening the page in the browser instead");
            start_browser(session, u, s);
        });
        return 0;
    }
    start_browser(session, u, s);
    return 0;
}

// 0 pending, 1 done (the callback URL in out, UTF-8), -1 cancelled
HLE_FUNC(emu_auth_poll, int, (char* out, u64 cap)) {
    using namespace auth;
    std::lock_guard lk(g_lock);
    if (g_state == 1 && out && cap > g_result.size()) {
        memcpy(out, g_result.c_str(), g_result.size() + 1);
        g_state = 0;
        g_scheme.clear();
        return 1;
    }
    if (g_state == -1) {
        g_state = 0;
        g_scheme.clear();
        return -1;
    }
    return 0;
}

HLE_FUNC(emu_auth_cancel, void, ()) {
    using namespace auth;
    std::lock_guard lk(g_lock);
    g_scheme.clear();
    g_state = 0;
    ++g_session;
    close_dialog_locked();
    webview_close();
}
