// The window of the guest's WKWebView (guest/UIKit/WebKit.m): a WebView2 window on a thread of
// its own. The Xbox sign-in shows its second step (the Xbox profile / consent pages, in
// XALWebKitViewControllerIos) in a WKWebView after the Microsoft account page.
//
// The guest polls the events (a navigation starts / finished / failed, the window was closed,
// a script's result) and passes them to the web view's navigation delegate like WebKit does.
// http(s) navigations go ahead right away (the delegate only gets to see them); other schemes
// (the app's own callback URLs) are stopped and only reported.
#include "common.h"
#include "hle.h"
#include "vfs.h"
#include "window.h"
#include "wv2_callbacks.h"

#include <objbase.h>

#include <algorithm>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

enum EventKind { EvNavStart = 1, EvNavDone = 2, EvClosed = 3, EvScript = 4 };
struct Event {
    int kind;
    int arg;  // EvNavDone: 0 success, -1 cancelled, else the WebView2 error status + 1; EvScript: the token
    std::string text;  // the URL / the script's result (JSON; "\x01" when it failed)
};

struct View {
    u64 id = 0;
    std::wstring url, headers;
    HWND hwnd = nullptr;
    ICoreWebView2Environment* env = nullptr;
    ICoreWebView2Controller* ctl = nullptr;
    ICoreWebView2* view = nullptr;
    std::mutex lock;
    std::deque<Event> events;
    bool closed = false;
    void push(Event e) {
        std::lock_guard lk(lock);
        events.push_back(std::move(e));
    }
};

std::mutex g_lock;
std::map<u64, std::shared_ptr<View>> g_views;
u64 g_next = 1;
constexpr UINT WM_EMU_EVAL = WM_APP + 1, WM_EMU_CLOSE = WM_APP + 2;
struct EvalRequest { int token; std::wstring script; };

std::shared_ptr<View> find(u64 id) {
    std::lock_guard lk(g_lock);
    auto it = g_views.find(id);
    return it == g_views.end() ? nullptr : it->second;
}

std::wstring take(LPWSTR s) {
    std::wstring r = s ? s : L"";
    if (s) CoTaskMemFree(s);
    return r;
}

bool is_web(const std::wstring& u) {
    for (const wchar_t* p : {L"http:", L"https:", L"about:", L"data:", L"file:"})
        if (_wcsnicmp(u.c_str(), p, wcslen(p)) == 0) return true;
    return false;
}

LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* v = reinterpret_cast<View*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
    case WM_SIZE:
        if (v && v->ctl) {
            RECT r;
            GetClientRect(h, &r);
            v->ctl->put_Bounds(r);
        }
        return 0;
    case WM_EMU_EVAL: {
        std::unique_ptr<EvalRequest> req(reinterpret_cast<EvalRequest*>(lp));
        if (!v || !v->view) {
            if (v) v->push({EvScript, req->token, "\x01"});
            return 0;
        }
        int token = req->token;
        View* vp = v;
        v->view->ExecuteScript(req->script.c_str(), new ScriptDone([vp, token](HRESULT hr, LPCWSTR json) -> HRESULT {
            vp->push({EvScript, token, SUCCEEDED(hr) && json ? narrow(json) : std::string("\x01")});
            return S_OK;
        }));
        return 0;
    }
    case WM_EMU_CLOSE:  // closed by the guest (the web view went away): no "closed" event
        if (v) v->closed = true;
        DestroyWindow(h);
        return 0;
    case WM_CLOSE:
        if (v && !v->closed) {
            v->closed = true;
            v->push({EvClosed, 0, ""});
            LOGI("webview: closed by the user");
        }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProcW(h, msg, wp, lp);
    }
}

void run(std::shared_ptr<View> sv) {
    View* v = sv.get();
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
        wc.lpszClassName = L"iOSMCPEWebViewWindow";
        RegisterClassExW(&wc);
    });
    HWND owner = (HWND)window::native_handle();
    UINT dpi = owner ? GetDpiForWindow(owner) : 96;
    int w = MulDiv(500, dpi, 96), hgt = MulDiv(720, dpi, 96);
    RECT o{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (owner) GetWindowRect(owner, &o);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromRect(&o, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& wa = mi.rcWork;
    w = std::min<int>(w, wa.right - wa.left);
    hgt = std::min<int>(hgt, (wa.bottom - wa.top) * 9 / 10);
    int x = std::max<int>(wa.left, std::min<int>((o.left + o.right - w) / 2, wa.right - w));
    int y = std::max<int>(wa.top, std::min<int>((o.top + o.bottom - hgt) / 2, wa.bottom - hgt));
    v->hwnd = CreateWindowExW(0, L"iOSMCPEWebViewWindow", L"Xbox", WS_OVERLAPPEDWINDOW, x, y, w, hgt, owner, nullptr, inst, nullptr);
    SetWindowLongPtrW(v->hwnd, GWLP_USERDATA, (LONG_PTR)v);
    {
        std::lock_guard lk(v->lock);
        if (v->closed) {  // closed by the guest before the window existed
            DestroyWindow(v->hwnd);
            v->hwnd = nullptr;
            CoUninitialize();
            return;
        }
    }
    ShowWindow(v->hwnd, SW_SHOW);
    SetForegroundWindow(v->hwnd);
    std::wstring data = (std::filesystem::path(widen(vfs::host_data_dir())).parent_path() / L"WebView2").wstring();
    auto fail = [v](const char* what, HRESULT hr) {
        LOGW("webview: %s failed (0x%lx)", what, hr);
        if (!v->closed) {
            v->closed = true;
            v->push({EvClosed, 0, ""});
        }
        PostMessageW(v->hwnd, WM_EMU_CLOSE, 0, 0);
    };
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, data.c_str(), nullptr, new EnvDone([v, fail](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
        if (FAILED(hr) || !env) { fail("environment", hr); return S_OK; }
        v->env = env;
        env->AddRef();
        return env->CreateCoreWebView2Controller(v->hwnd, new CtlDone([v, fail](HRESULT hr, ICoreWebView2Controller* ctl) -> HRESULT {
            if (FAILED(hr) || !ctl) { fail("controller", hr); return S_OK; }
            v->ctl = ctl;
            ctl->AddRef();
            ctl->get_CoreWebView2(&v->view);
            RECT r;
            GetClientRect(v->hwnd, &r);
            ctl->put_Bounds(r);
            EventRegistrationToken tok;
            v->view->add_NavigationStarting(new NavStarting([v](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* a) -> HRESULT {
                LPWSTR u = nullptr;
                a->get_Uri(&u);
                std::wstring uri = take(u);
                if (!is_web(uri)) a->put_Cancel(TRUE);  // the app's own scheme: only reported
                v->push({EvNavStart, 0, narrow(uri)});
                return S_OK;
            }), &tok);
            v->view->add_NavigationCompleted(new NavCompleted([v](ICoreWebView2* s, ICoreWebView2NavigationCompletedEventArgs* a) -> HRESULT {
                BOOL ok = FALSE;
                a->get_IsSuccess(&ok);
                COREWEBVIEW2_WEB_ERROR_STATUS st = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                a->get_WebErrorStatus(&st);
                LPWSTR u = nullptr;
                s->get_Source(&u);
                // (a navigation stopped above is "cancelled": not an error of the page)
                int code = ok ? 0 : st == COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED ? -1 : (int)st + 1;
                v->push({EvNavDone, code, narrow(take(u))});
                return S_OK;
            }), &tok);
            ICoreWebView2_18* v18 = nullptr;
            if (SUCCEEDED(v->view->QueryInterface(IID_ICoreWebView2_18, (void**)&v18)) && v18) {
                v18->add_LaunchingExternalUriScheme(new ExtScheme([v](ICoreWebView2*, ICoreWebView2LaunchingExternalUriSchemeEventArgs* a) -> HRESULT {
                    LPWSTR u = nullptr;
                    a->get_Uri(&u);
                    a->put_Cancel(TRUE);
                    v->push({EvNavStart, 0, narrow(take(u))});
                    return S_OK;
                }), &tok);
                v18->Release();
            }
            v->view->add_NewWindowRequested(new NewWindow([v](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a) -> HRESULT {
                LPWSTR u = nullptr;
                a->get_Uri(&u);
                std::wstring uri = take(u);
                a->put_Handled(TRUE);
                v->view->Navigate(uri.c_str());
                return S_OK;
            }), &tok);
            // the request's headers (Xal passes some) need NavigateWithWebResourceRequest
            ICoreWebView2_2* v2 = nullptr;
            ICoreWebView2Environment2* e2 = nullptr;
            ICoreWebView2WebResourceRequest* req = nullptr;
            if (!v->headers.empty() && SUCCEEDED(v->view->QueryInterface(IID_ICoreWebView2_2, (void**)&v2)) && v2 &&
                SUCCEEDED(v->env->QueryInterface(IID_ICoreWebView2Environment2, (void**)&e2)) && e2 &&
                SUCCEEDED(e2->CreateWebResourceRequest(v->url.c_str(), L"GET", nullptr, v->headers.c_str(), &req)) && req) {
                v2->NavigateWithWebResourceRequest(req);
            } else {
                v->view->Navigate(v->url.c_str());
            }
            if (req) req->Release();
            if (e2) e2->Release();
            if (v2) v2->Release();
            return S_OK;
        }));
    }));
    if (FAILED(hr)) fail("WebView2", hr);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    {
        std::lock_guard lk(v->lock);
        v->hwnd = nullptr;
    }
    if (v->ctl) { v->ctl->Close(); v->ctl->Release(); v->ctl = nullptr; }
    if (v->view) { v->view->Release(); v->view = nullptr; }
    if (v->env) { v->env->Release(); v->env = nullptr; }
    CoUninitialize();
}

}  // namespace

// url: UTF-8; headers: "Name: value\r\n" lines (may be empty). Returns the view's id.
HLE_FUNC(emu_webview_open, u64, (const char* url, const char* headers)) {
    auto v = std::make_shared<View>();
    v->url = widen(url ? url : "");
    v->headers = widen(headers ? headers : "");
    {
        std::lock_guard lk(g_lock);
        v->id = g_next++;
        g_views[v->id] = v;
    }
    LOGI("webview: [%llu] opening %s", (unsigned long long)v->id, std::string(url ? url : "").substr(0, 120).c_str());
    std::thread(run, v).detach();
    return v->id;
}

// The next event: 1 and *kind / *arg / text in out (UTF-8, cut at cap), 0 none, -1 no such view.
HLE_FUNC(emu_webview_poll, int, (u64 id, int* kind, int* arg, char* out, u64 cap)) {
    auto v = find(id);
    if (!v) return -1;
    std::lock_guard lk(v->lock);
    if (v->events.empty()) return 0;
    Event e = std::move(v->events.front());
    v->events.pop_front();
    *kind = e.kind;
    *arg = e.arg;
    if (out && cap) {
        size_t n = std::min<size_t>(e.text.size(), cap - 1);
        memcpy(out, e.text.data(), n);
        out[n] = 0;
    }
    return 1;
}

// Runs a script in the page; the result comes as an EvScript event with this token.
HLE_FUNC(emu_webview_eval, void, (u64 id, int token, const char* script)) {
    auto v = find(id);
    if (!v) return;
    std::lock_guard lk(v->lock);
    if (!v->hwnd) {
        v->events.push_back({EvScript, token, "\x01"});
        return;
    }
    PostMessageW(v->hwnd, WM_EMU_EVAL, 0, (LPARAM) new EvalRequest{token, widen(script ? script : "")});
}

HLE_FUNC(emu_webview_close, void, (u64 id)) {
    std::shared_ptr<View> v;
    {
        std::lock_guard lk(g_lock);
        auto it = g_views.find(id);
        if (it == g_views.end()) return;
        v = it->second;
        g_views.erase(it);  // (the window thread keeps its own reference)
    }
    std::lock_guard lk(v->lock);
    v->closed = true;
    if (v->hwnd) PostMessageW(v->hwnd, WM_EMU_CLOSE, 0, 0);
    LOGI("webview: [%llu] closed", (unsigned long long)id);
}
