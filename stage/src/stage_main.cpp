// ============================================================
//  MuyunStage — 暮云音乐 Mineradio 舞台引擎宿主（方案C：独立进程）
//
//  · 纯 Win32 + WebView2 COM（官方 SDK，动态加载 WebView2Loader.dll）
//  · 零 Qt / 零 JS 框架：JS 只活在 WebView2 页面进程里，本 exe 只是壳
//  · 生命周期：主程序 QProcess 拉起；主程序退出/管道断开 → 自动退出
//  · IPC：命令管道(服务端, 主→壳→页面) + 事件管道(客户端, 页面→壳→主)
//    帧格式：4 字节小端长度 + UTF-8 JSON
// ============================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <string>
#include <deque>
#include <vector>
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <cstdint>

#include "WebView2.h"
#include "com_guids.h"
#include "ipc.h"

// ---------- COM 回调样板 ----------
#define COM_REFcounts \
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override { \
        if (!ppv) return E_POINTER; \
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, m_iid)) { \
            *ppv = static_cast<void*>(this); AddRef(); return S_OK; } \
        *ppv = nullptr; return E_NOINTERFACE; } \
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_rc; } \
    ULONG STDMETHODCALLTYPE Release() override { const ULONG n = --m_rc; if (!n) delete this; return n; } \
    std::atomic<ULONG> m_rc{1}; \
    GUID m_iid = muyun::IID_Zero_L;

// ---------- 全局（单 UI 线程，WebView2 调用只在消息循环线程） ----------
static HWND        g_hwnd       = nullptr;
static ICoreWebView2Environment*       g_env       = nullptr;
static ICoreWebView2Controller*        g_controller = nullptr;
static ICoreWebView2*                  g_webview    = nullptr;
static ICoreWebView2_3*                g_webview3   = nullptr;
static muyun::EvPipe   g_ev;          // 页面事件 → 主程序
static muyun::CmdPipe  g_cmd;         // 主程序 → 页面命令
static std::wstring    g_cmdPipeName, g_evPipeName, g_webDir, g_dataDir;
static std::string     g_startUrl;
static bool  g_pageReady = false;
static bool  g_exiting   = false;
static HANDLE g_cmdThread = nullptr;
static std::deque<std::string> g_pending;      // 页面就绪前的命令缓存
static std::string  g_probeJs;                 // --probe 自检模式
static const wchar_t* g_probeOut = nullptr;
static bool          g_probeKeep = false;      // --probe-keep=1：探测后不退出（供截图/按键续测）
static int g_probeDelayMs = 0;                 // --probe-delay
static int g_evRetries = 30;                   // --ev-retries（独立调试时设 1 避免启动等待）

static std::string WideToUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, n - 1, s.data(), n - 1, nullptr, nullptr);
    return s;
}
static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    PathRemoveFileSpecW(buf);
    return buf;
}

static void SLog(const char* fmt, ...);   // 前置声明（PostQuit 诊断要用）

static void PostQuit(int code, const char* tag = "?") {
    if (g_exiting) return;
    g_exiting = true;
    SLog("PostQuit(%d) tag=%s tid=%lu", code, tag, GetCurrentThreadId());   // 诊断：谁先敲退
    // 打断命令线程可能挂着的 ConnectNamedPipe / ReadFile
    if (g_cmdThread) CancelSynchronousIo(g_cmdThread);
    if (g_hwnd) PostMessageW(g_hwnd, WM_QUIT, code, 0);
    else PostQuitMessage(code);
}

// ---------- 调试日志（--log <path> 开启；写文件 + 立即 flush） ----------
static FILE* g_log = nullptr;
static void SLog(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// ---------- 页面事件外发 ----------
static void SendEvent(const std::string& json) {
    g_ev.write(json);
}

// ---------- 命令投递 ----------
static void DeliverToPage(const std::string& json) {
    // 先由外壳处理控制类命令（bounds/show/hide/quit），其余透传页面
    std::string cmd;
    if (muyun::jsonGetString(json, "c", cmd)) {
        if (cmd == "bounds") {
            double x=0,y=0,w=0,h=0,fs=0;
            muyun::jsonGetNumber(json,"x",x); muyun::jsonGetNumber(json,"y",y);
            muyun::jsonGetNumber(json,"w",w); muyun::jsonGetNumber(json,"h",h);
            muyun::jsonGetNumber(json,"fs",fs);
            SetWindowPos(g_hwnd, HWND_TOP, (int)x, (int)y, (int)w, (int)h,
                         SWP_NOACTIVATE | (fs ? SWP_SHOWWINDOW : 0));
            return;
        }
        if (cmd == "show")  { ShowWindow(g_hwnd, SW_SHOWNA); if (g_controller) g_controller->put_IsVisible(TRUE); SetWindowPos(g_hwnd, HWND_TOP, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE); return; }
        if (cmd == "hide")  { ShowWindow(g_hwnd, SW_HIDE); if (g_controller) g_controller->put_IsVisible(FALSE); return; }
        if (cmd == "vis") {
            // 主程序可见性联动：隐藏时停渲染并让页面 document.hidden=true → 引擎 deep-sleep
            double on = 0; muyun::jsonGetNumber(json, "on", on);
            if (g_controller) g_controller->put_IsVisible(on ? TRUE : FALSE);
            ShowWindow(g_hwnd, on ? SW_SHOWNA : SW_HIDE);
            return;
        }
        if (cmd == "quit")  { PostQuit(0, "cmd-quit"); return; }
    }
    if (!g_webview || !g_pageReady) {
        if (g_pending.size() < 4096) g_pending.push_back(json);
        return;
    }
    // 用 WebView2 原生结构化消息（UTF-8 无损，免 base64/转义）
    const std::wstring wjson = Utf8ToWide(json);
    const HRESULT hr = g_webview->PostWebMessageAsJson(wjson.c_str());
    if (FAILED(hr) && g_log) SLog("PostWebMessageAsJson failed hr=0x%x len=%zu", (unsigned)hr, json.size());
}

static void FlushPending() {
    std::deque<std::string> q;
    q.swap(g_pending);
    for (const auto& m : q) DeliverToPage(m);
}

// ---------- 命令管道线程 ----------
static DWORD WINAPI CmdThread(LPVOID) {
    if (!g_cmd.listen(g_cmdPipeName)) {
        char wbuf[512] = {};
        WideCharToMultiByte(CP_UTF8, 0, muyun::CmdPipe::lastName().c_str(), -1,
                            wbuf, sizeof(wbuf) - 1, nullptr, nullptr);
        SLog("CmdPipe listen FAILED name=%s err=%lu", wbuf, (unsigned)muyun::CmdPipe::lastErr());
        PostQuit(3, "cmd-listen-fail"); return 0;
    }
    SLog("CmdPipe connected");
    std::string msg;
    while (g_cmd.read(msg)) {
        std::string* heap = new std::string(std::move(msg));
        if (!PostMessageW(g_hwnd, WM_APP + 1, 0, (LPARAM)heap)) { delete heap; break; }
    }
    PostQuit(0, "cmd-read-end");   // 主程序断开管道 → 退出
    return 0;
}

// ---------- 父进程看门狗 ----------
struct WatchdogArgs { DWORD pid; };
static DWORD WINAPI WatchdogThread(LPVOID p) {
    WatchdogArgs* a = (WatchdogArgs*)p;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, a->pid);
    delete a;
    if (h) { WaitForSingleObject(h, INFINITE); CloseHandle(h); PostQuit(0, "watchdog-parent-died"); }
    return 0;
}

// ---------- COM 回调 ----------
struct EnvCb : ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
    COM_REFcounts
    EnvCb() { m_iid = muyun::IID_EnvHandler_L; }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT res, ICoreWebView2Environment* env) override;
};
struct CtrlCb : ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
    COM_REFcounts
    CtrlCb() { m_iid = muyun::IID_CtrlHandler_L; }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT res, ICoreWebView2Controller* ctrl) override;
};
struct NavCb : ICoreWebView2NavigationCompletedEventHandler {
    COM_REFcounts
    NavCb() { m_iid = muyun::IID_NavDoneHandler_L; }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) override;
};
struct MsgCb : ICoreWebView2WebMessageReceivedEventHandler {
    COM_REFcounts
    MsgCb() { m_iid = muyun::IID_MsgHandler_L; }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*,
                                     ICoreWebView2WebMessageReceivedEventArgs* args) override {
        LPWSTR json = nullptr;
        if (args && SUCCEEDED(args->get_WebMessageAsJson(&json)) && json) {
            std::string s = WideToUtf8(json);
            CoTaskMemFree(json);
            SLog("MsgCb event: %.160s", s.c_str());
            // 页面就绪信号：翻转 ready 并回灌积压命令
            if (!g_pageReady && s.find("\"ready\"") != std::string::npos) {
                g_pageReady = true;
                SLog("page ready → flush pending=%zu", g_pending.size());
                FlushPending();
            }
            SendEvent(s);
        }
        return S_OK;
    }
};
struct ProbeCb : ICoreWebView2ExecuteScriptCompletedHandler {
    COM_REFcounts
    ProbeCb() { m_iid = muyun::IID_ExScriptDone_L; }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT res, LPCWSTR result) override {
        FILE* f = nullptr;
        if (g_probeOut) _wfopen_s(&f, g_probeOut, L"wb");
        if (f) {
            const std::string s = WideToUtf8(result ? result : L"");
            fwrite(s.data(), 1, s.size(), f);
            fclose(f);
        }
        if (!g_probeKeep) PostQuit(res, "probe-done");   // keep=1：留在现场，由外部继续操作/截图
        return S_OK;
    }
};

// ---------- WebView2 初始化链 ----------
typedef HRESULT (STDMETHODCALLTYPE CreateEnvFn)(
    LPCWSTR, LPCWSTR, ICoreWebView2EnvironmentOptions*, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);

static bool InitWebView(HWND hwnd) {
    HMODULE loader = nullptr;
    std::wstring ld = ExeDir() + L"\\WebView2Loader.dll";
    loader = LoadLibraryW(ld.c_str());
    if (!loader) loader = LoadLibraryW(L"WebView2Loader.dll");
    if (!loader) { SLog("WebView2Loader.dll NOT FOUND"); return false; }
    auto pCreate = (CreateEnvFn*)GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions");
    if (!pCreate) { SLog("CreateCoreWebView2EnvironmentWithOptions NOT FOUND"); return false; }

    std::wstring data = g_dataDir.empty() ? ExeDir() + L"\\webview2-data" : g_dataDir;
    SLog("CreateEnv begin data=%ls", data.c_str());
    return SUCCEEDED(pCreate(nullptr, data.c_str(), nullptr, new EnvCb()));
}

HRESULT EnvCb::Invoke(HRESULT res, ICoreWebView2Environment* env) {
    SLog("EnvCb res=0x%x env=%p", (unsigned)res, (void*)env);
    if (FAILED(res) || !env) { PostQuit(4, "envcb"); return res; }
    g_env = env; g_env->AddRef();
    const HRESULT hr = g_env->CreateCoreWebView2Controller(g_hwnd, new CtrlCb());
    if (FAILED(hr)) PostQuit(5, "createctrl");
    return hr;
}

HRESULT CtrlCb::Invoke(HRESULT res, ICoreWebView2Controller* ctrl) {
    SLog("CtrlCb res=0x%x ctrl=%p", (unsigned)res, (void*)ctrl);
    if (FAILED(res) || !ctrl) { PostQuit(6, "ctrlcb"); return res; }
    g_controller = ctrl; g_controller->AddRef();
    HRESULT hrg = g_controller->get_CoreWebView2(&g_webview);
    SLog("get_CoreWebView2 hr=0x%x webview=%p", (unsigned)hrg, (void*)g_webview);
    if (!g_webview) { PostQuit(7, "get-webview"); return E_FAIL; }

    // 设置
    ICoreWebView2Settings* st = nullptr;
    if (SUCCEEDED(g_webview->get_Settings(&st)) && st) {
        st->put_AreDevToolsEnabled(TRUE);
        st->put_AreDefaultContextMenusEnabled(FALSE);
        st->put_IsStatusBarEnabled(FALSE);
        st->put_IsZoomControlEnabled(FALSE);
        st->Release();
    }
    // ICoreWebView2_3：postMessage 通道 + 虚拟主机映射
    const HRESULT hq = g_webview->QueryInterface(muyun::IID_ICWV2_3_L, (void**)&g_webview3);
    SLog("QI ICWV2_3 hr=0x%x wv3=%p webdir=[%ls]", (unsigned)hq, (void*)g_webview3, g_webDir.c_str());
    if (SUCCEEDED(hq) && g_webview3) {
        const HRESULT h1 = g_webview3->add_WebMessageReceived(new MsgCb(), nullptr);
        const HRESULT h2 = g_webview3->SetVirtualHostNameToFolderMapping(
            L"muyunstage.local", g_webDir.c_str(),
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        SLog("add_WebMessageReceived hr=0x%x SetVirtualHost hr=0x%x", (unsigned)h1, (unsigned)h2);
    }
    g_webview->add_NavigationCompleted(new NavCb(), nullptr);

    RECT rc; GetClientRect(g_hwnd, &rc);
    g_controller->put_Bounds(rc);
    g_controller->put_IsVisible(TRUE);

    std::wstring url = g_startUrl.empty()
        ? std::wstring(L"https://muyunstage.local/index.html") : Utf8ToWide(g_startUrl);
    const HRESULT hn = g_webview->Navigate(url.c_str());
    SLog("Navigate hr=0x%x url=%ls", (unsigned)hn, url.c_str());
    return S_OK;
}

static void RunProbe() {
    if (!g_probeJs.empty() && g_webview) {
        std::wstring js = Utf8ToWide(g_probeJs);
        const HRESULT hp = g_webview->ExecuteScript(js.c_str(), new ProbeCb());
        SLog("probe ExecuteScript hr=0x%x", (unsigned)hp);
    } else {
        PostQuit(0, "runprobe-empty");
    }
}

HRESULT NavCb::Invoke(ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) {
    BOOL ok = FALSE;
    if (args) args->get_IsSuccess(&ok);
    SLog("NavCb isSuccess=%d probeLen=%zu delay=%d probe=[%s]",
         (int)ok, g_probeJs.size(), g_probeDelayMs, g_probeJs.c_str());
    if (g_probeJs.empty()) return S_OK;         // 生产模式：不干预
    if (g_probeDelayMs > 0) SetTimer(g_hwnd, 7, (UINT)g_probeDelayMs, nullptr);
    else RunProbe();
    return S_OK;
}

// ---------- 窗口 ----------
static HWND g_ownerHwnd = nullptr;   // 启动参数记录的 owner（主窗），仅供引用
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_APP + 1: {
        std::string* s = (std::string*)l;
        DeliverToPage(*s);
        delete s;
        return 0;
    }
    // 焦点说明：WebView2 点击会程序性激活壳窗，无法从壳侧干净拦截（MA_NOACTIVATE/WM_ACTIVATE
    // 弹回都会把 mouseup 甩走、按钮失灵）。正解在主程序侧：舞台关闭时 onProcFinished 用
    // bring() 把前台还给主窗（见 StageBridge.cpp），F11 靠 keepActive 在舞台存活期保持注册。
    case WM_TIMER: {
        if (w == 7) { KillTimer(h, 7); RunProbe(); return 0; }
        return 0;
    }
    case WM_SIZE: {
        if (g_controller) { RECT rc; GetClientRect(h, &rc); g_controller->put_Bounds(rc); }
        return 0;
    }
    case WM_CLOSE: PostQuit(0, "wm-close"); return 0;
    case WM_DESTROY: PostQuit(0, "wm-destroy"); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // 高 DPI：物理像素坐标由主程序算好传入
    if (HMODULE u = GetModuleHandleW(L"user32.dll")) {
        typedef BOOL (WINAPI* Fn)(HANDLE);
        auto p = (Fn)GetProcAddress(u, "SetProcessDpiAwarenessContext");
        if (p) p((HANDLE)(-4) /*PER_MONITOR_AWARE_V2*/);
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // 参数解析（宽字符命令行，兼容中文路径）
    int nargs = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &nargs);
    DWORD parent = 0;
    long bx=0, by=0, bw=1280, bh=720;
    std::string ownerStr;
    for (int i = 1; i < nargs; ++i) {
        std::string a = WideToUtf8(argvW[i]);
        auto eq = a.find('=');
        const std::string k = a.substr(0, eq), v = eq == std::string::npos ? "" : a.substr(eq + 1);
        if      (k == "--cmd-pipe")  g_cmdPipeName = Utf8ToWide(v);
        else if (k == "--ev-pipe")   g_evPipeName  = Utf8ToWide(v);
        else if (k == "--web-dir")   g_webDir      = Utf8ToWide(v);
        else if (k == "--data-dir")  g_dataDir     = Utf8ToWide(v);
        else if (k == "--url")       g_startUrl    = v;
        else if (k == "--parent")    parent = (DWORD)std::stoul(v);
        else if (k == "--owner")     ownerStr = v;
        else if (k == "--x") bx = std::stol(v);
        else if (k == "--y") by = std::stol(v);
        else if (k == "--w") bw = std::stol(v);
        else if (k == "--h") bh = std::stol(v);
        else if (k == "--probe")     g_probeJs = v;
        else if (k == "--probe-out") g_probeOut = _wcsdup(Utf8ToWide(v).c_str());
        else if (k == "--probe-keep") g_probeKeep = (v == "1");
        else if (k == "--probe-delay") g_probeDelayMs = (int)std::stol(v);
        else if (k == "--ev-retries")  g_evRetries = (int)std::stol(v);
        else if (k == "--log")       fopen_s(&g_log, v.c_str(), "w");
    }
    SLog("args parsed: cmdPipe=%d webDir=%d owner=%s parent=%lu bounds=%ld,%ld,%ld,%ld probeLen=%zu",
         (int)g_cmdPipeName.empty(), (int)g_webDir.empty(), ownerStr.c_str(), parent,
         bx, by, bw, bh, g_probeJs.size());
    if (g_cmdPipeName.empty() || g_webDir.empty()) {
        // 调试日志：把收到的命令行写出来
        if (FILE* f = fopen("stage_argparse.log", "wb")) {
            fprintf(f, "argc=%d cmdline=[%s]\n", nargs, WideToUtf8(GetCommandLineW()).c_str());
            fprintf(f, "cmdPipeEmpty=%d webDirEmpty=%d\n",
                    (int)g_cmdPipeName.empty(), (int)g_webDir.empty());
            fclose(f);
        }
        MessageBoxW(nullptr, L"缺少 --cmd-pipe / --web-dir 参数", L"MuyunStage", MB_OK);
        return 2;
    }

    // 窗口（WS_POPUP + 不抢焦点 + 不进 Alt-Tab）
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"MuyunStageWindow";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"暮云音乐 · 舞台", WS_POPUP,
        bx, by, bw, bh, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) return 3;

    // owner 关联：跨进程挂到主窗口（自动跟随 Z 序/最小化/隐藏）
    if (!ownerStr.empty()) {
        HWND owner = (HWND)(uintptr_t)std::strtoull(ownerStr.c_str(), nullptr, 16);
        if (owner && IsWindow(owner)) {
            g_ownerHwnd = owner;
            SetWindowLongPtrW(g_hwnd, GWLP_HWNDPARENT, (LONG_PTR)owner);
        }
    }
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);

    // 事件管道（连主程序的服务端；后台线程重试，不阻塞 UI）
    if (!g_evPipeName.empty()) {
        struct EvArgs { std::wstring name; int retries; };
        auto *a = new EvArgs{g_evPipeName, g_evRetries};
        HANDLE h = CreateThread(nullptr, 0, [](LPVOID pp) -> DWORD {
            auto *arg = (EvArgs*)pp;
            const bool ok = g_ev.connect(arg->name, 300, arg->retries);
            SLog("ev connect ok=%d", (int)ok);
            if (ok) {
                // 报上壳窗句柄：主程序要靠它判断"当前前台是不是舞台"（Esc 系统热键的接管条件）
                char buf[80];
                sprintf_s(buf, "{\"e\":\"hwnd\",\"h\":\"%llx\"}", (unsigned long long)(uintptr_t)g_hwnd);
                SendEvent(buf);
            }
            delete arg;
            return 0;
        }, a, 0, nullptr);
        if (h) CloseHandle(h);
    }
    // 命令管道服务端线程
    g_cmdThread = CreateThread(nullptr, 0, CmdThread, nullptr, 0, nullptr);
    // 父进程看门狗
    if (parent) CreateThread(nullptr, 0, WatchdogThread, new WatchdogArgs{parent}, 0, nullptr);

    if (!InitWebView(g_hwnd)) {
        SendEvent("{\"e\":\"fatal\",\"msg\":\"webview2 init failed\"}");
        return 8;
    }

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_webview3) g_webview3->Release();
    if (g_webview) g_webview->Release();
    if (g_controller) g_controller->Release();
    if (g_env) g_env->Release();
    // 等命令线程退出（CancelSynchronousIo 已在 PostQuit 发出）
    if (g_cmdThread) {
        WaitForSingleObject(g_cmdThread, 3000);
        CloseHandle(g_cmdThread);
    }
    g_cmd.close();
    g_ev.close();
    CoUninitialize();
    return (int)msg.wParam;
}