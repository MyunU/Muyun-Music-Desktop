#include "ImeGuard.h"

#include <QWindow>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#include <imm.h>
#endif

#ifdef MUYUN_SELFTES
#include <cstdio>
#define IMELOG(...) do { fprintf(stderr, "[ime] " __VA_ARGS__); fflush(stderr); } while (0)
#else
#define IMELOG(...) do { } while (0)
#endif

namespace Muyun {

ImeGuard::ImeGuard(QObject *parent) : QObject(parent)
{
    m_timer = new QTimer(this);
    // 200ms：Qt 是在"焦点变更"那一刻同步摘上下文的，用户点完再按切换键至少隔几百毫秒，
    // 这个周期既兜得住又不产生可感开销（一次 ImmGetContext 是微秒级）。
    m_timer->setInterval(200);
    connect(m_timer, &QTimer::timeout, this, &ImeGuard::tick);
    m_timer->start();
}

void ImeGuard::attach(QWindow *window)
{
    m_window = window;
    tick();
}

#ifdef Q_OS_WIN
static HWND hwndOf(const QPointer<QWindow> &w)
{
    if (!w) return nullptr;
    return reinterpret_cast<HWND>(w->winId());
}
#endif

bool ImeGuard::hasImeContext() const
{
#ifdef Q_OS_WIN
    const HWND h = hwndOf(m_window);
    if (!h) return false;
    HIMC c = ImmGetContext(h);
    if (c) ImmReleaseContext(h, c);
    return c != nullptr;
#else
    return true;   // 非 Windows 没有这个问题
#endif
}

bool ImeGuard::ensureNow()
{
#ifdef Q_OS_WIN
    // 自检对照开关：关掉守护，--test-ime 就该抓到"上下文没了 / 切换键失效"，
    // 证明那些断言不是空转（MUYUN_NO_IME_GUARD=1）
    if (qEnvironmentVariableIsSet("MUYUN_NO_IME_GUARD")) return false;

    const HWND h = hwndOf(m_window);
    if (!h || !IsWindow(h)) return false;
    if (!IsWindowVisible(h)) return false;      // 隐藏到托盘时不折腾

    HIMC c = ImmGetContext(h);
    if (c) {                                     // 上下文还在，什么都不用做
        ImmReleaseContext(h, c);
        return false;
    }
    // 与 Qt 给文本框开输入法用的是同一个调用：把默认 IME 上下文接回窗口
    ImmAssociateContextEx(h, nullptr, IACE_DEFAULT);
    ++m_restores;
    IMELOG("窗口 IME 上下文被 Qt 摘掉了，已接回默认上下文（hwnd=%p 第 %d 次）\n",
           reinterpret_cast<void *>(h), m_restores);
    return true;
#else
    return false;
#endif
}

void ImeGuard::tick()
{
    ensureNow();
}

void ImeGuard::forceRefresh()
{
#ifdef Q_OS_WIN
    const HWND h = hwndOf(m_window);
    if (!h || !IsWindow(h)) return;
    if (!IsWindowVisible(h)) return;
    // 模拟"点窗外再点回"的窗口激活往返：Qt 只在收到 WM_ACTIVATE / WM_SETFOCUS
    // 时才重建输入上下文（QWindowsInputContext::updateEnabled 由激活事件驱动，
    // QML 内焦点切换不触发）。只发消息不真实切换窗口，无闪动。
    SendMessage(h, WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, 0), 0);
    SendMessage(h, WM_SETFOCUS, 0, 0);
    ++m_restores;
    IMELOG("模拟窗口激活往返重建 IME（hwnd=%p 第 %d 次）\n",
           reinterpret_cast<void *>(h), m_restores);
#else
    // 非 Windows 无此问题
#endif
}

} // namespace Muyun
