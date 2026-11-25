// COM callback objects around lambdas for the WebView2 handler interfaces (one Invoke each),
// shared by the sign-in window (auth_webview.cpp) and WKWebView (web_view.cpp).
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>

#include "WebView2.h"

#include <atomic>
#include <utility>

#define EMU_WV2_HANDLER(NAME, IFACE, PARAMS, ARGS)                                                     \
    template <class F>                                                                                 \
    struct NAME : IFACE {                                                                              \
        std::atomic<ULONG> ref{1};                                                                     \
        F f;                                                                                           \
        explicit NAME(F x) : f(std::move(x)) {}                                                        \
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** p) override {                     \
            if (riid == IID_IUnknown || riid == IID_##IFACE) { *p = this; AddRef(); return S_OK; }     \
            *p = nullptr;                                                                              \
            return E_NOINTERFACE;                                                                      \
        }                                                                                              \
        ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }                                    \
        ULONG STDMETHODCALLTYPE Release() override { ULONG r = --ref; if (!r) delete this; return r; } \
        HRESULT STDMETHODCALLTYPE Invoke PARAMS override { return f ARGS; }                            \
    };

EMU_WV2_HANDLER(EnvDone, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, (HRESULT hr, ICoreWebView2Environment* e), (hr, e))
EMU_WV2_HANDLER(CtlDone, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, (HRESULT hr, ICoreWebView2Controller* c), (hr, c))
EMU_WV2_HANDLER(NavStarting, ICoreWebView2NavigationStartingEventHandler, (ICoreWebView2* s, ICoreWebView2NavigationStartingEventArgs* a), (s, a))
EMU_WV2_HANDLER(NavCompleted, ICoreWebView2NavigationCompletedEventHandler, (ICoreWebView2* s, ICoreWebView2NavigationCompletedEventArgs* a), (s, a))
EMU_WV2_HANDLER(NewWindow, ICoreWebView2NewWindowRequestedEventHandler, (ICoreWebView2* s, ICoreWebView2NewWindowRequestedEventArgs* a), (s, a))
EMU_WV2_HANDLER(ExtScheme, ICoreWebView2LaunchingExternalUriSchemeEventHandler, (ICoreWebView2* s, ICoreWebView2LaunchingExternalUriSchemeEventArgs* a), (s, a))
EMU_WV2_HANDLER(ScriptDone, ICoreWebView2ExecuteScriptCompletedHandler, (HRESULT hr, LPCWSTR json), (hr, json))
