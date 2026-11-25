// The Microsoft account sign-in in the PC's browser (see auth.cpp).
#pragma once
#include <functional>
#include <string>

namespace auth {
// The process was started by the browser with --auth-callback <url>: hands the URL to the
// running emulator. Returns true when this process should exit right away.
bool forward_callback(int argc, wchar_t** argv);
// WM_COPYDATA of the emulator's window (p: COPYDATASTRUCT*). Returns true when it was ours.
bool on_copydata(const void* p);
void test_window_from_env();  // debug: EMU_TEST_AUTH_WINDOW
// the sign-in window with an embedded web view (auth_webview.cpp). done(1, url) with the
// callback URL, done(-1, "") when the user closed it, done(-2, "") when WebView2 failed.
bool webview_available();
void webview_begin(unsigned id, const std::string& url, const std::string& scheme, std::function<void(int, const std::string&)> done);
void webview_close();
}  // namespace auth
