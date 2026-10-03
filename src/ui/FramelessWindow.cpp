#include "FramelessWindow.h"

#include <QApplication>
#include <QWindow>

#ifdef Q_OS_WIN
#include <windows.h>
#include <windowsx.h>
#endif

namespace Muyun {

FramelessWindow::FramelessWindow(QObject *parent) : QObject(parent)
{
    qApp->installNativeEventFilter(this);
}

FramelessWindow::~FramelessWindow() = default;

void FramelessWindow::attach(QWindow *window)
{
#ifdef Q_OS_WIN
    m_window = window;
    if (!m_window) return;
    HWND hwnd = reinterpret_cast<HWND>(m_window->winId());
    if (!hwnd) return;
    // 只补 WS_THICKFRAME（原生缩放所需）+ 系统菜单/最小最大化框；
    // **不加 WS_CAPTION**：frameless 窗带 WS_CAPTION 时，原生缩放过程中 DWM 会去动画那个
    // 隐形标题栏，导致拖边缩放严重抖动。缩放靠 WS_THICKFRAME + 我们的 WM_NCHITTEST 足够。
    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    SetWindowLongPtr(hwnd, GWL_STYLE,
                     style | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
#else
    Q_UNUSED(window)
#endif
}

bool FramelessWindow::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    if (eventType != "windows_generic_MSG" || !m_window) return false;
    MSG *m = static_cast<MSG *>(message);
    HWND hwnd = reinterpret_cast<HWND>(m_window->winId());
    if (!hwnd || m->hwnd != hwnd) return false;

    // ---- 非客户区归零：无边框但保留系统行为；最大化时按边框宽度内缩（修顶部缺一行）----
    // 注：不处理 WM_GETMINMAXINFO / WM_WINDOWPOSCHANGING —— 实测它们会让 Qt 把窗口误判为
    // 已 Maximized 态（启动即 vis=Maximized），导致最大化按钮首点走了 showNormal()（"无效"）。
    if (m->message == WM_NCCALCSIZE && m->wParam == TRUE && result) {
        auto *p = reinterpret_cast<NCCALCSIZE_PARAMS *>(m->lParam);
        if (IsZoomed(hwnd)) {
            const UINT dpi = GetDpiForWindow(hwnd);
            const int fx = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi)
                         + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            p->rgrc[0].left   += fx;
            p->rgrc[0].top    += fx;
            p->rgrc[0].right  -= fx;
            p->rgrc[0].bottom -= fx;
        }
        *result = 0;
        return true;
    }

    // ---- 边缘命中测试 → 系统原生缩放（替代 QML MouseArea startSystemResize 的抖动方案）----
    if (m->message == WM_NCHITTEST && result) {
        if (IsZoomed(hwnd) || m_window->visibility() == QWindow::FullScreen)
            return false;                       // 最大化/全屏不缩放；交回 Qt
        const POINT pt = { GET_X_LPARAM(m->lParam), GET_Y_LPARAM(m->lParam) };
        RECT rc;
        if (!GetWindowRect(hwnd, &rc)) return false;
        const UINT dpi = GetDpiForWindow(hwnd);
        const int border = qMax(6, static_cast<int>(8.0 * dpi / 96.0));
        const bool left   = pt.x < rc.left + border;
        const bool right  = pt.x >= rc.right - border;
        const bool top    = pt.y < rc.top + border;
        const bool bottom = pt.y >= rc.bottom - border;
        qintptr ht = 0;
        if (top && left)          ht = HTTOPLEFT;
        else if (top && right)    ht = HTTOPRIGHT;
        else if (bottom && left)  ht = HTBOTTOMLEFT;
        else if (bottom && right) ht = HTBOTTOMRIGHT;
        else if (left)            ht = HTLEFT;
        else if (right)           ht = HTRIGHT;
        else if (top)             ht = HTTOP;
        else if (bottom)          ht = HTBOTTOM;
        else return false;        // 客户区：标题栏拖动等仍由 Qt 处理
        *result = ht;
        return true;
    }
#else
    Q_UNUSED(eventType) Q_UNUSED(message) Q_UNUSED(result)
#endif
    return false;
}

} // namespace Muyun
