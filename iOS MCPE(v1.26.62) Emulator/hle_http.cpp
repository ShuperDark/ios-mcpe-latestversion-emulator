// HTTP(S) for the guest's NSURLSession / NSURLConnection (guest/Foundation/NSURL.m), on top
// of WinHTTP: TLS, certificate checks, proxies, redirects and gzip are Windows' own.
//
// The guest runs each request on a worker thread of its own and calls these blocking
// functions in order: open -> send -> receive (status and headers) -> read... -> close.
// emu_http_cancel may be called from another thread at any time (it aborts a pending call).
#include "hle.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#pragma comment(lib, "winhttp.lib")

namespace {

// NSURLError codes
enum : int {
    kErrCancelled = -999, kErrBadURL = -1000, kErrTimedOut = -1001, kErrUnsupportedURL = -1002,
    kErrCannotFindHost = -1003, kErrCannotConnect = -1004, kErrConnectionLost = -1005, kErrNotConnected = -1009,
    kErrRedirectLoop = -1007, kErrBadServerResponse = -1011, kErrSecureConnectionFailed = -1200,
    kErrServerCertUntrusted = -1202, kErrUnknown = -1,
};

int map_error(DWORD e) {
    switch (e) {
    case ERROR_WINHTTP_TIMEOUT: return kErrTimedOut;
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return kErrCannotFindHost;
    case ERROR_WINHTTP_CANNOT_CONNECT: return kErrCannotConnect;
    case ERROR_WINHTTP_CONNECTION_ERROR: return kErrConnectionLost;
    case ERROR_WINHTTP_OPERATION_CANCELLED: return kErrCancelled;
    case ERROR_WINHTTP_INVALID_URL: return kErrBadURL;
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME: return kErrUnsupportedURL;
    case ERROR_WINHTTP_REDIRECT_FAILED: return kErrRedirectLoop;
    case ERROR_WINHTTP_INVALID_SERVER_RESPONSE: return kErrBadServerResponse;
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
    case ERROR_WINHTTP_SECURE_CERT_REVOKED:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE: return kErrServerCertUntrusted;
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
    case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED: return kErrSecureConnectionFailed;
    default: return kErrUnknown;
    }
}

struct Request {
    HINTERNET connect = nullptr, request = nullptr;
    std::atomic<bool> cancelled{false};
    int error = 0;
    std::string url;
};

std::mutex g_lock;
std::unordered_map<u64, Request*> g_requests;
u64 g_next = 1;
HINTERNET g_session;
const bool g_trace = getenv("EMU_TRACE_HTTP") != nullptr;

HINTERNET session() {
    static std::once_flag once;
    std::call_once(once, [] {
        // the system's proxy settings (WinINet / "automatic proxy"), like NSURLSession on a device
        g_session = WinHttpOpen(L"minecraftpe/1.26.62 CFNetwork Darwin", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!g_session)  // (before Windows 8.1)
            g_session = WinHttpOpen(L"minecraftpe/1.26.62 CFNetwork Darwin", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (g_session) {
            DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x2000 /* TLS 1.3 */;
            if (!WinHttpSetOption(g_session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
                protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
                WinHttpSetOption(g_session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
            }
            DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;  // NSURLSession decodes gzip / deflate itself
            WinHttpSetOption(g_session, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof decompress);
            DWORD http2 = WINHTTP_PROTOCOL_FLAG_HTTP2;
            WinHttpSetOption(g_session, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &http2, sizeof http2);
        } else {
            LOGE("http: WinHttpOpen failed (error %lu)", GetLastError());
        }
    });
    return g_session;
}

Request* get(u64 h) {
    std::lock_guard lk(g_lock);
    auto it = g_requests.find(h);
    return it == g_requests.end() ? nullptr : it->second;
}

bool fail(Request* r, DWORD e) {
    r->error = r->cancelled ? kErrCancelled : map_error(e);
    if (g_trace || r->error != kErrCancelled) LOGI("http: %s failed (WinHTTP error %lu -> %d)", r->url.c_str(), e, r->error);
    return false;
}

}  // namespace

// method / url: UTF-8. headers: "Name: value\r\n" lines. timeout: seconds (0 = 60).
// Returns a handle (0 = error, see *error).
HLE_FUNC(emu_http_open, u64, (const char* method, const char* url, const char* headers, double timeout, int* error)) {
    *error = 0;
    HINTERNET s = session();
    if (!s) { *error = kErrNotConnected; return 0; }
    std::wstring wurl = widen(url ? url : "");
    URL_COMPONENTS uc{sizeof uc};
    wchar_t host[256] = {}, path[8192] = {}, extra[8192] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 8192;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 8192;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) || (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS)) {
        LOGW("http: unsupported url %s", url ? url : "(null)");
        *error = kErrUnsupportedURL;
        return 0;
    }
    auto* r = new Request;
    r->url = url;
    r->connect = WinHttpConnect(s, host, uc.nPort, 0);
    std::wstring object = std::wstring(path) + extra;
    if (object.empty()) object = L"/";
    if (r->connect)
        r->request = WinHttpOpenRequest(r->connect, widen(method && *method ? method : "GET").c_str(), object.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!r->request) {
        fail(r, GetLastError());
        *error = r->error;
        if (r->connect) WinHttpCloseHandle(r->connect);
        delete r;
        return 0;
    }
    int ms = (int)((timeout > 0 ? timeout : 60.0) * 1000.0);
    WinHttpSetTimeouts(r->request, ms, ms, ms, ms);
    // cookies are the guest's business (NSHTTPCookieStorage): WinHTTP must not add its own
    DWORD flags = WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption(r->request, WINHTTP_OPTION_DISABLE_FEATURE, &flags, sizeof flags);
    if (headers && *headers) {
        std::wstring wh = widen(headers);
        WinHttpAddRequestHeaders(r->request, wh.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }
    std::lock_guard lk(g_lock);
    u64 h = g_next++;
    g_requests[h] = r;
    if (g_trace) LOGI("http: [%llu] %s %s", (unsigned long long)h, method ? method : "GET", url);
    return h;
}

// Sends the request with its body (may be empty). Returns 0, or an NSURLError code.
HLE_FUNC(emu_http_send, int, (u64 h, const void* body, u64 len)) {
    Request* r = get(h);
    if (!r) return kErrUnknown;
    if (r->cancelled || !r->request) return kErrCancelled;
    if (!WinHttpSendRequest(r->request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (void*)(len ? body : nullptr), (DWORD)len, (DWORD)len, 0) ||
        !WinHttpReceiveResponse(r->request, nullptr)) {
        fail(r, GetLastError());
        return r->error;
    }
    return 0;
}

// The response: *status, and the header lines (without the status line) in out (UTF-8,
// "Name: value\r\n"...). Returns the length the headers need (call again with a larger
// buffer when it is >= cap), or -1.
HLE_FUNC(emu_http_response, s64, (u64 h, int* status, char* out, u64 cap)) {
    Request* r = get(h);
    if (!r || !r->request) return -1;
    DWORD code = 0, sz = sizeof code;
    WinHttpQueryHeaders(r->request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz,
                        WINHTTP_NO_HEADER_INDEX);
    *status = (int)code;
    DWORD bytes = 0;
    WinHttpQueryHeaders(r->request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
    std::wstring w(bytes / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(r->request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, w.data(), &bytes,
                             WINHTTP_NO_HEADER_INDEX))
        w.clear();
    w.resize(bytes / sizeof(wchar_t));
    std::string s = narrow(w);
    size_t nl = s.find("\r\n");  // drop the status line
    s = nl == std::string::npos ? std::string() : s.substr(nl + 2);
    if (out && cap > s.size()) {
        memcpy(out, s.data(), s.size());
        out[s.size()] = 0;
    }
    if (g_trace) LOGI("http: [%llu] status %lu", (unsigned long long)h, code);
    return (s64)s.size();
}

// The final URL (after redirects), UTF-8. Returns the needed length.
HLE_FUNC(emu_http_url, s64, (u64 h, char* out, u64 cap)) {
    Request* r = get(h);
    if (!r || !r->request) return -1;
    DWORD bytes = 0;
    WinHttpQueryOption(r->request, WINHTTP_OPTION_URL, nullptr, &bytes);
    std::wstring w(bytes / sizeof(wchar_t) + 1, L'\0');
    std::string s = WinHttpQueryOption(r->request, WINHTTP_OPTION_URL, w.data(), &bytes) ? narrow(w.c_str()) : r->url;
    if (out && cap > s.size()) {
        memcpy(out, s.data(), s.size());
        out[s.size()] = 0;
    }
    return (s64)s.size();
}

// Reads body bytes: > 0 bytes read, 0 at the end, < 0 an NSURLError code.
HLE_FUNC(emu_http_read, s64, (u64 h, void* buf, u64 n)) {
    Request* r = get(h);
    if (!r) return kErrUnknown;
    if (r->cancelled || !r->request) return kErrCancelled;
    DWORD got = 0;
    if (!WinHttpReadData(r->request, buf, (DWORD)std::min<u64>(n, 1u << 20), &got)) {
        fail(r, GetLastError());
        return r->error;
    }
    return got;
}

// From any thread: makes the pending and the following calls fail with NSURLErrorCancelled.
HLE_FUNC(emu_http_cancel, void, (u64 h)) {
    std::lock_guard lk(g_lock);
    auto it = g_requests.find(h);
    if (it == g_requests.end()) return;
    it->second->cancelled = true;
    // closing the request handle aborts a blocking WinHTTP call (the handle is closed again in emu_http_close: harmless)
    WinHttpCloseHandle(it->second->request);
    it->second->request = nullptr;
}

HLE_FUNC(emu_http_close, void, (u64 h)) {
    Request* r;
    {
        std::lock_guard lk(g_lock);
        auto it = g_requests.find(h);
        if (it == g_requests.end()) return;
        r = it->second;
        g_requests.erase(it);
    }
    if (r->request) WinHttpCloseHandle(r->request);
    if (r->connect) WinHttpCloseHandle(r->connect);
    delete r;
}
