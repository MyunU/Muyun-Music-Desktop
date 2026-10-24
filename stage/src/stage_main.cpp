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
#include <cstdlib>
#include <cstdarg>
#include <cstdint>
#include <wchar.h>
#include <fcntl.h>    // _O_RDWR
#include <io.h>       // _open_osfhandle (MinGW 提供, MSVC 提供; 把 Win32 handle 变 CRT fd)

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
static std::string g_gpuMode = "hardware";      // --gpu-mode=hardware|nodc|nocpu|auto|software（也可用环境变量 MUYUN_STAGE_GPU_MODE 设）
static std::atomic<bool> g_glProbeArmed{false}; // bug1：页面 ready 后只做一次 WebGL 渲染器探测
static std::atomic<bool> g_fpsProbeArmed{false}; // 卡顿诊断：页面 ready 后采样一次 rAF 帧间隔
static bool g_fpsProbeOn = false;               // 环境变量 MUYUN_STAGE_FPS_PROBE=1 时开启

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

// ---------- 录屏透明 bug1 always-on 诊断（不依赖 --log；B 机拷回 stage_diag.txt 即可定位） ----------
// 每次启动重写 <data-dir>\stage_diag.txt，随初始化进度追加。核心问题：
// 核显机（显示器接核显/无独显直连）录屏时舞台窗口透明。--gpu-mode 控制策略：
//   auto(默认)=--disable-gpu-compositing（保独显渲染，关 GPU 合成，兼顾性能与录屏）
//   software=--disable-gpu（回退软件光栅化+GDI 合成，保证可录但粒子慢）
//   hardware=无 flag（纯独显最快，录屏可能仍透明）
// 诊断项：build 时间戳、运行 exe mtime（区分跑的是不是新构建）、env options
// 是否成功（旧 WebView2 runtime 缺 ICoreWebView2EnvironmentOptions → flag 被静默丢弃）、
// browser 版本、页面 ready 后 JS 探针报告实际 WebGL 渲染器
// （SwiftShader = 软件合成已生效；真实 GPU 名 = flag 未生效）。
static FILE* g_diag = nullptr;
static void D(const char* fmt, ...) {
    if (!g_diag) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_diag, fmt, ap);
    va_end(ap);
    fputc('\n', g_diag);
    fflush(g_diag);
}
static void OpenDiag() {
    std::wstring base = g_dataDir.empty() ? ExeDir() : g_dataDir;
    if (GetFileAttributesW(base.c_str()) == INVALID_FILE_ATTRIBUTES)
        CreateDirectoryW(base.c_str(), nullptr);
    std::wstring p = base + L"\\stage_diag.txt";
    // bug1 诊断（第6轮）: _wfopen_s(..., L"w, ccs=UTF-8") 在 MinGW-w64 + 该 WebView2 runtime 环境下
    // 打开失败 (B 机 stage_diag.txt 只有 3 字节 = 空/损坏), 导致全程 D() 静默无效 → 诊断全丢。
    // 改为 CreateFileW + 手动 FILE* 包装, 不依赖 CRT locale 字符串模式。
    const HANDLE hf = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) {
        SLog("OpenDiag CreateFileW FAILED err=%lu path=%ls", GetLastError(), p.c_str());
        return;
    }
    int fd = _open_osfhandle((intptr_t)hf, _O_RDWR);
    if (fd < 0) {
        SLog("OpenDiag _open_osfhandle FAILED path=%ls", p.c_str());
        CloseHandle(hf);
        return;
    }
    g_diag = _wfdopen(fd, L"w");
    if (!g_diag) {
        SLog("OpenDiag _wfdopen FAILED path=%ls", p.c_str());
        return;
    }
    SLog("OpenDiag OK path=%ls", p.c_str());
    D("=== stage_diag build=%s %s ===", __DATE__, __TIME__);
    FILETIME ftc, fta, ftw; SYSTEMTIME stw{};
    if (GetFileTime(GetModuleHandleW(nullptr), &ftc, &fta, &ftw) && FileTimeToSystemTime(&ftw, &stw))
        D("exe_mtime=%04d-%02d-%02d %02d:%02d:%02d", stw.wYear, stw.wMonth, stw.wDay,
          stw.wHour, stw.wMinute, stw.wSecond);
    OSVERSIONINFOW osv{}; osv.dwOSVersionInfoSize = sizeof(osv);
    if (GetVersionExW(&osv))
        D("os=Win%lu.%lu build %lu", osv.dwMajorVersion, osv.dwMinorVersion, osv.dwBuildNumber);
    D("cmdline=%s", WideToUtf8(GetCommandLineW()).c_str());
    D("gpuMode=%s", g_gpuMode.c_str());
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
                // bug1：1.5s 后跑一次 WebGL 渲染器探针（结果落 stage_diag.txt）
                if (!g_glProbeArmed.exchange(true)) SetTimer(g_hwnd, 9, 1500, nullptr);
                // 卡顿诊断：稍晚一点（等首帧/粒子稳定）再采 rAF 帧间隔
                if (g_fpsProbeOn && !g_fpsProbeArmed.exchange(true)) SetTimer(g_hwnd, 10, 4500, nullptr);
            }
            if (s.find("\"fatal\"") != std::string::npos)
                D("fatal event: %.512s", s.c_str());
            // bug1 探针回传：页面侧用 postMessage 报 WebGL 渲染器（ExecuteScript 完成值
            // 在本 runtime 上对多语句脚本不可靠，故走消息通道）
            {
                const size_t p = s.find("\"__glprobe\":");
                if (p != std::string::npos) D("glProbe(页面回传)=%.512s", s.c_str() + p);
            }
            // 卡顿诊断：页面把 rAF 帧间隔统计回传
            {
                const size_t p = s.find("\"__fpsprobe\":");
                if (p != std::string::npos) D("fpsProbe(页面回传)=%.512s", s.c_str() + p);
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
// 递归删除目录树 (缓存清理用): 文件先删再删空目录, 失败静默忽略。
// 用 SHFileOperation 最稳, 不递归进锁定的子目录。
static void RemoveDirectoryTree(const std::wstring& dir) {
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = dir.c_str();          // 需要末尾有 L'\0' 终止的宽串
    // pFrom 是双 null 结尾 (SHFileOperation 约定), 用临时缓冲区
    std::wstring tmp = dir + L'\0' + L'\0';
    op.pFrom = tmp.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    SHFileOperationW(&op);           // 返回值非 0 忽略: 缓存删除失败不该阻断启动
}

// bug1 (2026-10-24 轮): 手写 ICoreWebView2EnvironmentOptions 实现 (全接口链).
// 官方文档: "A default implementation is provided in WebView2EnvironmentOptions.h"
// (即 WRL Make<CoreWebView2EnvironmentOptions>(), 构造一个普通 COM 对象后传给
// CreateCoreWebView2EnvironmentWithOptions). 本工程是纯 Win32 + MinGW, 不引 WRL 头,
// 直接手写实现. 官方基类 CoreWebView2EnvironmentOptionsBase 继承的接口链是:
//   ICoreWebView2EnvironmentOptions (基, AdditionalBrowserArguments/Language/
//   TargetCompatibleBrowserVersion/AllowSingleSignOnUsingOSPrimaryAccount),
//   Options2 (ExclusiveUserDataFolderAccess),
//   Options3 (IsCustomCrashReportingEnabled),
//   Options4 (Get/SetCustomSchemeRegistrations),
//   Options5 (EnableTrackingPrevention, 官方默认 TRUE!),
//   Options6 (AreBrowserExtensionsEnabled),
//   Options7 (ChannelSearchKind / ReleaseChannels),   ← loader 强制探测, 缺失→E_INVALIDARG
//   Options8 (ScrollBarStyle).
// 实现必须整链: 2026-10-24 实测 WebView2Loader 的 pCreate 会对 options 对象
// QI(Options7)=C48D539F 探测三次, 只实现基接口的对象 → E_INVALIDARG (0x80070057),
// 环境创建失败 → 舞台黑屏/录屏透明. 官方默认值一律照抄 (见下各 getter).
// 关键: get_* 字符串必须用 CoTaskMemAlloc 分配 (调用方按约定 CoTaskMemFree);
// 未设置的字符串属性官方返回 nullptr (非空串) —— 这里也照做.
class EnvOptionsImpl : public ICoreWebView2EnvironmentOptions,
                       public ICoreWebView2EnvironmentOptions2,
                       public ICoreWebView2EnvironmentOptions3,
                       public ICoreWebView2EnvironmentOptions4,
                       public ICoreWebView2EnvironmentOptions5,
                       public ICoreWebView2EnvironmentOptions6,
                       public ICoreWebView2EnvironmentOptions7,
                       public ICoreWebView2EnvironmentOptions8 {
public:
    explicit EnvOptionsImpl(const wchar_t* args, const wchar_t* targetVersion)
        : m_args(args ? args : L""), m_targetVersion(targetVersion ? targetVersion : L"") {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ICoreWebView2EnvironmentOptions)) {
            *ppv = static_cast<ICoreWebView2EnvironmentOptions*>(this); AddRef();
            D("options QI: IUnknown/Options OK"); return S_OK;
        }
        if (IsEqualIID(riid, IID_ICoreWebView2EnvironmentOptions2)) {
            *ppv = static_cast<ICoreWebView2EnvironmentOptions2*>(this); AddRef();
            D("options QI: Options2 OK"); return S_OK;
        }
        if (IsEqualIID(riid, IID_ICoreWebView2EnvironmentOptions3)) {
            *ppv = static_cast<ICoreWebView2EnvironmentOptions3*>(this); AddRef();
            D("options QI: Options3 OK"); return S_OK;
        }
        if (IsEqualIID(riid, IID_ICoreWebView2EnvironmentOptions4)) {
            *ppv = static_cast<ICoreWebView2EnvironmentOptions4*>(this); AddRef();
            D("options QI: Options4 OK"); return S_OK;
        }
        if (IsEqualIID(riid, IID_ICoreWebView2EnvironmentOptions5)) {
            *ppv = static_cast<ICoreWebView2EnvironmentOptions5*>(this); AddRef();
            D("options QI: Options5 OK"); return S_OK;
        }
        if (IsEqualIID(riid, IID_ICoreWebView2EnvironmentOptions6)) {
            *ppv = static_cast<ICoreWebView2EnvironmentOptions6*>(this); AddRef();
            D("options QI: Options6 OK"); return S_OK;
        }
        if (IsEqualIID(riid, IID_ICoreWebView2EnvironmentOptions7)) {
            *ppv = static_cast<ICoreWebView2EnvironmentOptions7*>(this); AddRef();
            D("options QI: Options7 OK"); return S_OK;
        }
        if (IsEqualIID(riid, IID_ICoreWebView2EnvironmentOptions8)) {
            *ppv = static_cast<ICoreWebView2EnvironmentOptions8*>(this); AddRef();
            D("options QI: Options8 OK"); return S_OK;
        }
        D("options QI: E_NOINTERFACE iid=%08lX-%04hX-%04hX", riid.Data1, riid.Data2, riid.Data3);
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_rc; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG n = --m_rc; if (!n) delete this; return n; }

    // ---------- ICoreWebView2EnvironmentOptions (基) ----------
    HRESULT STDMETHODCALLTYPE get_AdditionalBrowserArguments(LPWSTR* value) override {
        if (!value) return E_POINTER;
        wchar_t* buf = (wchar_t*)CoTaskMemAlloc((m_args.size() + 1) * sizeof(wchar_t));
        if (!buf) { *value = nullptr; return E_OUTOFMEMORY; }
        wcscpy(buf, m_args.c_str());
        *value = buf;
        D("options get_AdditionalBrowserArguments -> '%ls'", m_args.c_str());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_AdditionalBrowserArguments(LPCWSTR value) override {
        m_args = value ? value : L"";
        return S_OK;
    }
    // 未设置的字符串属性官方返回 nullptr (AutoCoMemString::Copy() 当 m_string 为空时
    // 返回 nullptr, 调用方对 nullptr 免 CoTaskMemFree). 这里照做.
    HRESULT STDMETHODCALLTYPE get_Language(LPWSTR* value) override {
        if (!value) return E_POINTER;
        *value = nullptr; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_Language(LPCWSTR value) override {
        if (value) { m_language = value; }
        return S_OK;
    }
    // 官方默认把 TargetCompatibleBrowserVersion 设为 SDK 对应浏览器版本号
    // (CORE_WEBVIEW_TARGET_PRODUCT_VERSION). 本工程无该常量, 就用 loader 报告的
    // 本机已装 runtime 版本 (见 InitWebView), 保证在任意机器上都有合法版本串.
    HRESULT STDMETHODCALLTYPE get_TargetCompatibleBrowserVersion(LPWSTR* value) override {
        if (!value) return E_POINTER;
        if (m_targetVersion.empty()) { *value = nullptr; return S_OK; }
        wchar_t* buf = (wchar_t*)CoTaskMemAlloc((m_targetVersion.size() + 1) * sizeof(wchar_t));
        if (!buf) { *value = nullptr; return E_OUTOFMEMORY; }
        wcscpy(buf, m_targetVersion.c_str());
        *value = buf;
        D("options get_TargetCompatibleBrowserVersion -> '%ls'", m_targetVersion.c_str());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_TargetCompatibleBrowserVersion(LPCWSTR value) override {
        if (value) { m_targetVersion = value; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_AllowSingleSignOnUsingOSPrimaryAccount(BOOL* allow) override {
        if (!allow) return E_POINTER;
        *allow = m_allowSSO;   // 官方默认 FALSE
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_AllowSingleSignOnUsingOSPrimaryAccount(BOOL allow) override {
        m_allowSSO = allow ? TRUE : FALSE;
        return S_OK;
    }

    // ---------- ICoreWebView2EnvironmentOptions2 ----------
    HRESULT STDMETHODCALLTYPE get_ExclusiveUserDataFolderAccess(BOOL* value) override {
        if (!value) return E_POINTER;
        *value = m_exclUserData;   // 官方默认 FALSE
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_ExclusiveUserDataFolderAccess(BOOL value) override {
        m_exclUserData = value ? TRUE : FALSE;
        return S_OK;
    }

    // ---------- ICoreWebView2EnvironmentOptions3 ----------
    HRESULT STDMETHODCALLTYPE get_IsCustomCrashReportingEnabled(BOOL* value) override {
        if (!value) return E_POINTER;
        *value = m_customCrash;   // 官方默认 FALSE
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_IsCustomCrashReportingEnabled(BOOL value) override {
        m_customCrash = value ? TRUE : FALSE;
        return S_OK;
    }

    // ---------- ICoreWebView2EnvironmentOptions4 ----------
    HRESULT STDMETHODCALLTYPE GetCustomSchemeRegistrations(
        UINT32* count, ICoreWebView2CustomSchemeRegistration*** schemeRegistrations) override {
        if (!count || !schemeRegistrations) return E_POINTER;
        *count = 0; *schemeRegistrations = nullptr;   // 官方默认无自定义 scheme
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCustomSchemeRegistrations(
        UINT32, ICoreWebView2CustomSchemeRegistration**) override {
        return S_OK;   // 我们不注册自定义 scheme, 忽略即可
    }

    // ---------- ICoreWebView2EnvironmentOptions5 ----------
    HRESULT STDMETHODCALLTYPE get_EnableTrackingPrevention(BOOL* value) override {
        if (!value) return E_POINTER;
        *value = m_trackingPrevention;   // 官方默认 TRUE!
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_EnableTrackingPrevention(BOOL value) override {
        m_trackingPrevention = value ? TRUE : FALSE;
        return S_OK;
    }

    // ---------- ICoreWebView2EnvironmentOptions6 ----------
    HRESULT STDMETHODCALLTYPE get_AreBrowserExtensionsEnabled(BOOL* value) override {
        if (!value) return E_POINTER;
        *value = m_extensions;   // 官方默认 FALSE
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_AreBrowserExtensionsEnabled(BOOL value) override {
        m_extensions = value ? TRUE : FALSE;
        return S_OK;
    }

    // ---------- ICoreWebView2EnvironmentOptions7 ----------
    HRESULT STDMETHODCALLTYPE get_ChannelSearchKind(COREWEBVIEW2_CHANNEL_SEARCH_KIND* value) override {
        if (!value) return E_POINTER;
        *value = m_channelSearchKind;   // 官方默认 COREWEBVIEW2_CHANNEL_SEARCH_KIND_MOST_STABLE=0
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_ChannelSearchKind(COREWEBVIEW2_CHANNEL_SEARCH_KIND value) override {
        m_channelSearchKind = value;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_ReleaseChannels(COREWEBVIEW2_RELEASE_CHANNELS* value) override {
        if (!value) return E_POINTER;
        *value = m_releaseChannels;   // 官方默认 ALL = STABLE|BETA|DEV|CANARY = 0xF
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_ReleaseChannels(COREWEBVIEW2_RELEASE_CHANNELS value) override {
        m_releaseChannels = value;
        return S_OK;
    }

    // ---------- ICoreWebView2EnvironmentOptions8 ----------
    HRESULT STDMETHODCALLTYPE get_ScrollBarStyle(COREWEBVIEW2_SCROLLBAR_STYLE* value) override {
        if (!value) return E_POINTER;
        *value = m_scrollbarStyle;   // 官方默认 COREWEBVIEW2_SCROLLBAR_STYLE_DEFAULT=0
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_ScrollBarStyle(COREWEBVIEW2_SCROLLBAR_STYLE value) override {
        m_scrollbarStyle = value;
        return S_OK;
    }

private:
    std::wstring m_args;
    std::wstring m_language;
    std::wstring m_targetVersion;
    BOOL m_allowSSO = FALSE;
    BOOL m_exclUserData = FALSE;
    BOOL m_customCrash = FALSE;
    BOOL m_trackingPrevention = TRUE;   // 官方默认 EnableTrackingPrevention=TRUE
    BOOL m_extensions = FALSE;
    COREWEBVIEW2_CHANNEL_SEARCH_KIND m_channelSearchKind = COREWEBVIEW2_CHANNEL_SEARCH_KIND_MOST_STABLE;
    COREWEBVIEW2_RELEASE_CHANNELS m_releaseChannels = (COREWEBVIEW2_RELEASE_CHANNELS)(
        COREWEBVIEW2_RELEASE_CHANNELS_STABLE | COREWEBVIEW2_RELEASE_CHANNELS_BETA |
        COREWEBVIEW2_RELEASE_CHANNELS_DEV | COREWEBVIEW2_RELEASE_CHANNELS_CANARY);
    COREWEBVIEW2_SCROLLBAR_STYLE m_scrollbarStyle = COREWEBVIEW2_SCROLLBAR_STYLE_DEFAULT;
    std::atomic<ULONG> m_rc{1};
};

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
    // #5 WebView2 磁盘缓存会在版本更新后继续喂旧 engine.js，表现为"代码改了但现象不变"。
    //   Chromium 对 ES 模块的默认缓存策略非常激进（无 max-age 时按 30 天处理），
    //   虚拟主机映射下的本地文件也走 HTTP 缓存规则。每次启动前清掉 Cache 目录，
    //   localStorage（滚放 enabled / 序列 / 停留秒数）在 Local Storage/ 下，不受影响。
    {
        std::wstring cacheDir = data + L"\\EBWebView\\Default\\Cache";
        RemoveDirectoryTree(cacheDir);
        // Code Cache (ES 模块专用) 也一并清
        RemoveDirectoryTree(data + L"\\EBWebView\\Default\\Code Cache");
    }

    // #5 bug3 (2026-10-24): N卡 ShadowPlay / Xbox Game Bar 等 DXGI Desktop Duplication
    //   录屏无法捕获 WebView2 默认 D3D11 硬件合成表面 (录制画面里该窗透明, 底层 UI 透出;
    //   GDI/BitBlt 截图软件正常). 2026-10-24 第8轮: 旧 --disable-gpu 强制软件渲染，
    //   核显机虽可录但粒子卡顿(33489顶点全走CPU WARP)。改为 --disable-gpu-compositing:
    //   保持独显硬件渲染(不卡)，仅关 GPU 合成层(让 GDI/DXGI 可捕)。
    //   --gpu-mode=auto(默认): --disable-gpu-compositing
    //   --gpu-mode=software:   --disable-gpu (老方案，强制软件，保证可录但慢)
    //   --gpu-mode=hardware:   无 flag (纯独显，最快，但录屏可能仍透明)
    // bug1 (2026-10-24 轮): 核显机(显示器接核显/无独显直连)上仍透明 → 已实证根因:
    //   旧代码用 CLSID {8a03ec41,...} CoCreateInstance(ICoreWebView2EnvironmentOptions)
    //   恒失败 (本机 runtime 155.0.4283.62 实测 hr=0x80040154 REGDB_E_CLASSNOTREG;
    //   wvopt 探测 3 个候选 GUID 全 E_NOCLS; 注册表无此类 CLSID; SDK 无独立工厂),
    //   → --disable-gpu 从未生效, WebView2 一直 GPU 合成 → 录屏透明 (PrintWindow 自检
    //   全黑帧也是同根因). 修复路径: 不再 CoCreateInstance, 直接 new 手写的 EnvOptionsImpl
    //   传给 CreateEnvFn. 2026-10-24 实测: 只实现基接口的对象被 WebView2Loader 拒绝
    //   (pCreate hr=0x80070057 E_INVALIDARG, loader 对 options 连续 QI(Options7)=C48D539F
    //   三次), 必须实现官方那样完整的 Options2-8 整链 (见 EnvOptionsImpl 类头注释).
    //   官方默认实现就是 WRL Make<> 构造的普通 COM 对象, 这是标准用法, 任何 runtime 版本可用.
    //   2026-10-24 实测第二轮: Options2-8 整链都 QI 成功后 pCreate 仍 E_INVALIDARG,
    //   差异只剩 TargetCompatibleBrowserVersion = nullptr. 官方默认把它设为 SDK 版本串
    //   (CORE_WEBVIEW_TARGET_PRODUCT_VERSION), loader 用它解析"要用的 runtime 版本",
    //   null 版本串 → E_INVALIDARG. 修复: 用 loader 自己的
    //   GetAvailableCoreWebView2BrowserVersionString 查询本机已装 runtime 版本串
    //   (不带 options, 无鸡生蛋问题), 作为 TargetCompatibleBrowserVersion 传入
    //   (任何机器上都合法, 跨机可移植).
    std::wstring gpuArgs;
    if (g_gpuMode == "software") {
        gpuArgs = L"--disable-gpu";
        D("gpuMode=software → --disable-gpu (强制软件渲染，可录屏但极慢)");
    } else if (g_gpuMode == "auto") {
        // auto: 独显渲染 + 软件合成 + 关 DirectComposition。
        // 2026-10-24 实测 FPS avg=3.9 (22/22 帧 >25ms, max 4791ms) —— 会卡死，别当默认。
        gpuArgs = L"--disable-gpu-compositing --disable-direct-composition";
        D("gpuMode=auto → --disable-gpu-compositing --disable-direct-composition (可录屏但仅 ~4fps)");
    } else if (g_gpuMode == "nodc") {
        // 只关 DirectComposition，保留 GPU 合成（候选：兼顾性能与可捕获）
        gpuArgs = L"--disable-direct-composition";
        D("gpuMode=nodc → --disable-direct-composition (GPU 合成 + hwnd 交换链)");
    } else if (g_gpuMode == "nocpu") {
        // 只关 GPU 合成
        gpuArgs = L"--disable-gpu-compositing";
        D("gpuMode=nocpu → --disable-gpu-compositing (独显渲染 + CPU 合成)");
    } else {
        // hardware（默认）: 一个 flag 都不加。实测 144fps / p95 7.1ms / jank 0/179。
        gpuArgs = L"";
        D("gpuMode=hardware → 无 GPU flag (纯独显合成，最流畅)");
    }
    EnvOptionsImpl* penv = new EnvOptionsImpl(gpuArgs.c_str(), nullptr);
    // 查询本机可用 runtime 版本串 (loader 导出, 与 pCreate 同一 DLL)
    typedef HRESULT (STDMETHODCALLTYPE GetAvailFn)(
        LPCWSTR, LPWSTR*);
    LPWSTR availVer = nullptr;
    HRESULT hrAvail = E_NOTIMPL;
    GetAvailFn* pGetAvail = (GetAvailFn*)GetProcAddress(loader, "GetAvailableCoreWebView2BrowserVersionString");
    if (pGetAvail) {
        hrAvail = pGetAvail(nullptr, &availVer);
        D("GetAvailable...String hr=0x%08X ver='%ls'", (unsigned)hrAvail, availVer ? availVer : L"");
        SLog("GetAvailable...String hr=0x%x ver=%ls", (unsigned)hrAvail, availVer ? availVer : L"");
    } else {
        SLog("GetAvailableCoreWebView2BrowserVersionString NOT FOUND in loader");
    }
    if (SUCCEEDED(hrAvail) && availVer) {
        penv->put_TargetCompatibleBrowserVersion(availVer);  // 官方默认=SDK版本串, 这里=本机已装版本
        CoTaskMemFree(availVer);
    }
    D("envOpts=手写 EnvOptionsImpl OK, AdditionalBrowserArguments='%ls' 已生效路径", gpuArgs.c_str());
    SLog("env options: hand-rolled EnvOptionsImpl args=%ls", gpuArgs.c_str());
    const HRESULT hres = pCreate(nullptr, data.c_str(), penv, new EnvCb());
    D("pCreate hr=0x%08X", (unsigned)hres);
    SLog("CreateEnv pCreate hr=0x%x", (unsigned)hres);
    if (penv) penv->Release();
    return SUCCEEDED(hres);
}

HRESULT EnvCb::Invoke(HRESULT res, ICoreWebView2Environment* env) {
    SLog("EnvCb res=0x%x env=%p", (unsigned)res, (void*)env);
    if (FAILED(res) || !env) { PostQuit(4, "envcb"); return res; }
    g_env = env; g_env->AddRef();
    {   // bug1 诊断：WebView2 runtime 版本（判断 ICoreWebView2EnvironmentOptions 是否可用的旁证）
        LPWSTR ver = nullptr;
        if (SUCCEEDED(env->get_BrowserVersionString(&ver))) {
            D("browserVersion=%s", WideToUtf8(ver ? ver : L"(null)").c_str());
            CoTaskMemFree(ver);
        } else {
            D("browserVersion=unknown");
        }
    }
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

// bug1 录屏透明诊断：页面 ready 后探测实际 WebGL 渲染器，结果写 stage_diag.txt
//   SwiftShader/llvmpipe → --disable-gpu 生效（软件合成，DXGI 任何捕获路径都可见）
//   真实 GPU 名 → GPU 合成仍在跑（flag 未生效 / 运行时不支持），核显机录屏仍会透明
struct GlProbeCb : ICoreWebView2ExecuteScriptCompletedHandler {
    COM_REFcounts
    GlProbeCb() { m_iid = muyun::IID_ExScriptDone_L; }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT res, LPCWSTR result) override {
        D("glProbe res=0x%08X result=%s", (unsigned)res, WideToUtf8(result ? result : L"(null)").c_str());
        return S_OK;
    }
};

// 卡顿诊断：采样 180 帧或 3 秒的 rAF 帧间隔，统计 FPS/jank/p50/p95/max，
// 连同 engine 自己的 renderPerfState 一起 postMessage 回传（落 stage_diag.txt）。
struct FpsProbeCb : ICoreWebView2ExecuteScriptCompletedHandler {
    COM_REFcounts
    FpsProbeCb() { m_iid = muyun::IID_ExScriptDone_L; }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT res, LPCWSTR result) override {
        D("fpsProbe res=0x%08X result=%s", (unsigned)res, WideToUtf8(result ? result : L"(null)").c_str());
        return S_OK;
    }
};

static void RunFpsProbe() {
    if (!g_webview) return;
    static const wchar_t* JS =
        L"(function(){var d=[],L=0,n=0,S=0;"
        L"function t(p){if(!S)S=p;if(L)d.push(p-L);L=p;n++;"
        L"if(n<180&&(p-S)<3000){requestAnimationFrame(t);return;}"
        L"var a=d.slice().sort(function(x,y){return x-y;}),s=0,i;"
        L"for(i=0;i<d.length;i++)s+=d[i];"
        L"var j=0;for(i=0;i<d.length;i++)if(d[i]>25)j++;"
        L"var pf='{}';try{pf=JSON.stringify(window.__mineradioPerf||{});}catch(e){}"
        L"var m='FPS avg='+(1000*d.length/Math.max(1,s)).toFixed(1)"
        L"+' ms='+(s/Math.max(1,d.length)).toFixed(2)"
        L"+' p50='+a[(a.length*0.5)|0].toFixed(2)"
        L"+' p95='+a[(a.length*0.95)|0].toFixed(2)"
        L"+' max='+a[a.length-1].toFixed(2)"
        L"+' jank='+j+'/'+d.length"
        L"+' dpr='+(window.devicePixelRatio||1)"
        L"+' vw='+window.innerWidth+'x'+window.innerHeight"
        L"+' perf='+pf;"
        L"try{chrome.webview.postMessage({__fpsprobe:m});}catch(e2){}}"
        L"requestAnimationFrame(t);})()";
    const HRESULT hp = g_webview->ExecuteScript(JS, new FpsProbeCb());
    D("fpsProbe ExecuteScript hr=0x%08X", (unsigned)hp);
}

static void RunGlProbe() {
    if (!g_webview) return;
    // 第8轮定论：本 runtime 的 ExecuteScript 对多语句脚本的"完成值"回传不可靠
    // （IIFE / var+try 序列都返回 null，只有单表达式能回值）。因此探针结果不靠
    // 返回值，改用页面本来就用的 postMessage 通道回传，MsgCb 里落 stage_diag.txt。
    static const wchar_t* JS =
        L"var __glp='';try{"
        L"var __c=document.createElement('canvas');"
        L"var __g=__c.getContext('webgl')||__c.getContext('experimental-webgl');"
        L"if(!__g){__glp='GL:unsupported';}else{"
        L"var __e=__g.getExtension('WEBGL_debug_renderer_info');"
        L"var __v=(__e&&__g.getParameter(__e.UNMASKED_VENDOR_WEBGL))||'v?';"
        L"var __r=(__e&&__g.getParameter(__e.UNMASKED_RENDERER_WEBGL))||'r?';"
        L"__glp='GL:'+__v+'|'+__r"
        L"+(/swiftshader|llvmpipe|software/i.test(__v+' '+__r)?' [SOFTWARE]':' [HARDWARE]');"
        L"}}catch(x){__glp='GLERR:'+x;}"
        L"try{window.chrome.webview.postMessage({__glprobe:__glp});}catch(e2){}";
    const HRESULT hp = g_webview->ExecuteScript(JS, new GlProbeCb());
    D("glProbe ExecuteScript hr=0x%08X", (unsigned)hp);
    D("glProbe JS=%ls", JS);
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
        if (w == 9) { KillTimer(h, 9); RunGlProbe(); return 0; }   // bug1 WebGL 渲染器探针
        if (w == 10) { KillTimer(h, 10); RunFpsProbe(); return 0; } // 卡顿诊断 rAF 帧间隔采样
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
    // 环境变量出口：现场不改命令行/不重编译也能切 --gpu-mode（命令行优先级更高，后面覆盖）
    if (const char* e = getenv("MUYUN_STAGE_GPU_MODE")) if (*e) g_gpuMode = e;
    if (const char* e = getenv("MUYUN_STAGE_FPS_PROBE")) g_fpsProbeOn = (*e && *e != '0');
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
        else if (k == "--gpu-mode")    g_gpuMode   = v;
        else if (k == "--log")       fopen_s(&g_log, v.c_str(), "w");
    }
    SLog("args parsed: cmdPipe=%d webDir=%d owner=%s parent=%lu bounds=%ld,%ld,%ld,%ld probeLen=%zu gpuMode=%s",
         (int)g_cmdPipeName.empty(), (int)g_webDir.empty(), ownerStr.c_str(), parent,
         bx, by, bw, bh, g_probeJs.size(), g_gpuMode.c_str());
    OpenDiag();   // bug1：always-on 诊断（数据目录下的 stage_diag.txt，B 机可拷回）
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
        if (g_diag) { fclose(g_diag); g_diag = nullptr; }
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
    if (g_diag) { fclose(g_diag); g_diag = nullptr; }
    CoUninitialize();
    return (int)msg.wParam;
}