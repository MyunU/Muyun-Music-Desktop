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

} // namespace Muyun
