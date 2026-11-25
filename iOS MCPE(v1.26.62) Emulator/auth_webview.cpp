// The sign-in window: the Microsoft account page in an embedded web view (WebView2, part of
// Windows 11 / Edge), like iOS shows it in ASWebAuthenticationSession. The redirect to the
// callback scheme (ms-xal-...://auth?...) is caught before the web view would hand it to
// Windows, so nothing depends on the browser asking "open this app?" (which a browser does
// not do for a redirect that comes without a click, e.g. after an approval on the phone).
//
// The window runs on a thread of its own (WebView2 needs an STA thread with a message loop).
#include "auth.h"
#include "common.h"
#include "vfs.h"
#include "window.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>

#include "wv2_callbacks.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#pragma comment(lib, "version.lib")

namespace auth {
namespace {

struct Session {
    unsigned id;
    std::string url, scheme;
    std::function<void(int, const std::string&)> done;  // (1, callback url) / (-1, "")
    HWND hwnd = nullptr;
    ICoreWebView2Environment* env = nullptr;
    ICoreWebView2Controller* ctl = nullptr;
    ICoreWebView2* view = nullptr;
    bool finished = false;
};

std::mutex g_lock;
HWND g_hwnd = nullptr;  // the open sign-in window
unsigned g_open_id = 0;

std::wstring take(LPWSTR s) {
    std::wstring r = s ? s : L"";
    if (s) CoTaskMemFree(s);
    return r;
}

bool is_callback(const Session& s, const std::wstring& uri) {
    std::string u = narrow(uri);
    return !s.scheme.empty() && _strnicmp(u.c_str(), (s.scheme + ":").c_str(), s.scheme.size() + 1) == 0;
}

void finish(Session& s, int state, const std::string& url) {
    if (s.finished) return;
    s.finished = true;
    s.done(state, url);
    if (s.hwnd) PostMessageW(s.hwnd, WM_CLOSE, 0, 0);
}

LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* s = reinterpret_cast<Session*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
    case WM_SIZE:
        if (s && s->ctl) {
            RECT r;
            GetClientRect(h, &r);
            s->ctl->put_Bounds(r);
        }
        return 0;
    case WM_CLOSE:
        if (s && !s->finished) {  // closed by the user: the sign-in is cancelled
            LOGI("auth: sign-in window closed");
            finish(*s, -1, "");
        }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default: return DefWindowProcW(h, msg, wp, lp);
    }
}

void run(Session* s) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    static std::once_flag reg;
    std::call_once(reg, [&] {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = wndproc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"iOSMCPEAuthWindow";
        RegisterClassExW(&wc);
    });
    // a phone-sized window over the middle of the emulator's window
    HWND owner = (HWND)window::native_handle();
    UINT dpi = owner ? GetDpiForWindow(owner) : 96;
    int w = MulDiv(500, dpi, 96), hgt = MulDiv(720, dpi, 96);
    RECT o{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (owner) GetWindowRect(owner, &o);
    // within the work area of the emulator's monitor (not under the taskbar)
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromRect(&o, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& wa = mi.rcWork;
    w = std::min<int>(w, wa.right - wa.left);
    hgt = std::min<int>(hgt, (wa.bottom - wa.top) * 9 / 10);
    int x = (o.left + o.right - w) / 2, y = (o.top + o.bottom - hgt) / 2;
    x = std::max<int>(wa.left, std::min<int>(x, wa.right - w));
    y = std::max<int>(wa.top, std::min<int>(y, wa.bottom - hgt));
    s->hwnd = CreateWindowExW(0, L"iOSMCPEAuthWindow", L"Microsoft アカウントでサインイン", WS_OVERLAPPEDWINDOW, x, y, w, hgt, owner, nullptr,
                              inst, nullptr);
    SetWindowLongPtrW(s->hwnd, GWLP_USERDATA, (LONG_PTR)s);
    ShowWindow(s->hwnd, SW_SHOW);
    SetForegroundWindow(s->hwnd);
    {
        std::lock_guard lk(g_lock);
        g_hwnd = s->hwnd;
    }
    // the web view's profile (cookies: "stay signed in") next to the emulator's data
    std::wstring data = (std::filesystem::path(widen(vfs::host_data_dir())).parent_path() / L"WebView2").wstring();
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, data.c_str(), nullptr, new EnvDone([s](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(hr) || !env) {
                LOGW("auth: WebView2 environment failed (0x%lx)", hr);
                finish(*s, -2, "");
                return S_OK;
            }
            s->env = env;
            env->AddRef();
            return env->CreateCoreWebView2Controller(s->hwnd, new CtlDone([s](HRESULT hr, ICoreWebView2Controller* ctl) -> HRESULT {
                if (FAILED(hr) || !ctl) {
                    LOGW("auth: WebView2 controller failed (0x%lx)", hr);
                    finish(*s, -2, "");
                    return S_OK;
                }
                s->ctl = ctl;
                ctl->AddRef();
                ctl->get_CoreWebView2(&s->view);
                RECT r;
                GetClientRect(s->hwnd, &r);
                ctl->put_Bounds(r);
                EventRegistrationToken tok;
                // the redirect to the callback scheme ends the sign-in
                s->view->add_NavigationStarting(new NavStarting([s](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* a) -> HRESULT {
                    LPWSTR u = nullptr;
                    a->get_Uri(&u);
                    std::wstring uri = take(u);
                    if (is_callback(*s, uri)) {
                        a->put_Cancel(TRUE);
                        LOGI("auth: sign-in callback received (web view)");
                        finish(*s, 1, narrow(uri));
                    }
                    return S_OK;
                }), &tok);
                // (newer runtimes report a custom scheme here instead of as a navigation)
                ICoreWebView2_18* v18 = nullptr;
                if (SUCCEEDED(s->view->QueryInterface(IID_ICoreWebView2_18, (void**)&v18)) && v18) {
                    v18->add_LaunchingExternalUriScheme(new ExtScheme([s](ICoreWebView2*, ICoreWebView2LaunchingExternalUriSchemeEventArgs* a) -> HRESULT {
                        LPWSTR u = nullptr;
                        a->get_Uri(&u);
                        std::wstring uri = take(u);
                        if (is_callback(*s, uri)) {
                            a->put_Cancel(TRUE);
                            LOGI("auth: sign-in callback received (web view, external scheme)");
                            finish(*s, 1, narrow(uri));
                        }
                        return S_OK;
                    }), &tok);
                    v18->Release();
                }
                // pop-ups (help links, "create one!") open in the same view
                s->view->add_NewWindowRequested(new NewWindow([s](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a) -> HRESULT {
                    LPWSTR u = nullptr;
                    a->get_Uri(&u);
                    std::wstring uri = take(u);
                    a->put_Handled(TRUE);
                    if (is_callback(*s, uri)) finish(*s, 1, narrow(uri));
                    else s->view->Navigate(uri.c_str());
                    return S_OK;
                }), &tok);
                s->view->Navigate(widen(s->url).c_str());
                return S_OK;
            }));
        }));
    if (FAILED(hr)) {
        LOGW("auth: WebView2 is not available (0x%lx)", hr);
        finish(*s, -2, "");
    }
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    {
        std::lock_guard lk(g_lock);
        if (g_hwnd == s->hwnd) g_hwnd = nullptr;
    }
    if (s->ctl) { s->ctl->Close(); s->ctl->Release(); }
    if (s->view) s->view->Release();
    if (s->env) s->env->Release();
    delete s;
    CoUninitialize();
}

}  // namespace

bool webview_available() {
    LPWSTR v = nullptr;
    HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &v);
    bool ok = SUCCEEDED(hr) && v;
    if (v) CoTaskMemFree(v);
    return ok;
}

void webview_begin(unsigned id, const std::string& url, const std::string& scheme, std::function<void(int, const std::string&)> done) {
    webview_close();
    auto* s = new Session;
    s->id = id;
    s->url = url;
    s->scheme = scheme;
    s->done = std::move(done);
    std::thread(run, s).detach();
}

void webview_close() {
    std::lock_guard lk(g_lock);
    if (g_hwnd) {
        // (the window's session is over: its close must not report a cancel)
        auto* s = reinterpret_cast<Session*>(GetWindowLongPtrW(g_hwnd, GWLP_USERDATA));
        if (s) s->finished = true;
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        g_hwnd = nullptr;
    }
}

}  // namespace auth
