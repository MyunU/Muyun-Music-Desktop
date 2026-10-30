#include "FramelessWindow.h"
#include "Backdrop.h"
#include "WindowHwnd.h"

#include <QApplication>
#include <QPointF>
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

void FramelessWindow::bindMaxWindow(QWindow *window)
{
    m_maxWindow = window;
}

void FramelessWindow::setMaximizedStyle(bool on)
{
#ifdef Q_OS_WIN
    if (!m_maxWindow) return;
    HWND hwnd = reinterpret_cast<HWND>(m_maxWindow->winId());
    if (!hwnd) return;
    const LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    const LONG_PTR next = on ? (style | WS_MAXIMIZE) : (style & ~WS_MAXIMIZE);
    if (next == style) return;
    SetWindowLongPtr(hwnd, GWL_STYLE, next);
    // 只刷样式不移动/缩放：手动最大化几何已是工作区大小，别让 DWM 再排一遍
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
#else
    Q_UNUSED(on)
#endif
}

void FramelessWindow::applyWindowGeometry(int x, int y, int w, int h)
{
    if (!m_maxWindow) return;
    // 一次 setGeometry = 平台层一次 SetWindowPos（位置+尺寸同一拍），
    // 不会出现"先撑开再挪边"的双跳；最小值/最大化尺寸钳制也由 Qt 统一处理。
    m_maxWindow->setGeometry(x, y, w, h);
}

// ---- 方案②：原生最大化 -------------------------------------------------------
// 实测（v1.3.0 frameless 窗，style=0x96000000 纯 WS_POPUP、无 WS_THICKFRAME）：
//   ShowWindow(SW_MAXIMIZE) → rect 变成 0,24,1536,840（工作区是 0,0,1536,816），
//   即整体下移了一个标题栏高度；IsZoomed=1、WS_MAXIMIZE 位被系统补上。
//   最大化态下 SetWindowPos 仍有效（带 SWP_NOZORDER|SWP_NOACTIVATE 即可），
//   ShowWindow(SW_RESTORE) 能原样退回最大化前的几何；最小化再从任务栏恢复也仍是最大化。
bool FramelessWindow::setNativeMaximized(bool on)
{
#ifdef Q_OS_WIN
    if (!m_maxWindow) return false;
    HWND hwnd = reinterpret_cast<HWND>(m_maxWindow->winId());
    if (!hwnd) return false;
    if (on) {
        ShowWindow(hwnd, SW_MAXIMIZE);
        // 修"最大化后整体下移一行"：按监视器工作区重新对齐（物理像素）
        const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (mon && GetMonitorInfoW(mon, &mi)) {
            const RECT &w = mi.rcWork;
            SetWindowPos(hwnd, nullptr, w.left, w.top, w.right - w.left, w.bottom - w.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    } else {
        ShowWindow(hwnd, SW_RESTORE);
    }
    return true;
#else
    Q_UNUSED(on)
    return false;
#endif
}

bool FramelessWindow::isNativeMaximized() const
{
#ifdef Q_OS_WIN
    if (!m_maxWindow) return false;
    HWND hwnd = reinterpret_cast<HWND>(m_maxWindow->winId());
    return hwnd && IsZoomed(hwnd);
#else
    return false;
#endif
}

QPointF FramelessWindow::globalCursorLogical() const
{
#ifdef Q_OS_WIN
    POINT p{};
    if (!GetCursorPos(&p)) return {};
    const qreal dpr = m_maxWindow ? m_maxWindow->devicePixelRatio() : 1.0;
    // GetCursorPos 返回物理像素；QML 的全局坐标是逻辑像素（DPR 缩放后）。
    // 直接读系统光标、不经过窗口自身几何，拖动过程中窗口怎么动都不受影响。
    return QPointF(static_cast<qreal>(p.x) / dpr, static_cast<qreal>(p.y) / dpr);
#else
    return {};
#endif
}

bool FramelessWindow::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    if (eventType != "windows_generic_MSG") return false;
    MSG *m = static_cast<MSG *>(message);

    // ---- 方案②：原生最大化 → 贴工作区对齐 ----
    // 系统对"无边框窗"做 SW_MAXIMIZE 时会把窗口整体下移一个边框高度（实测 125% DPI：
    // 工作区 1920x1020@0,0 → 实际 1920x1020@0,30），表现就是"顶部缺一栏 + 底部钻进任务栏"。
    // 而 ShowWindow 之后立刻 SetWindowPos 会被后续定位覆盖（最大化是异步完成的）。
    // 在 WM_WINDOWPOSCHANGING 里直接改 WINDOWPOS 最稳：Windows 按修正值定位，不闪不回弹。
    // 注：这里只在 IsZoomed 为真时改，不影响普通移动/缩放；也不改 Qt 的最大化状态判定。
    if (m_maxWindow && m->message == WM_WINDOWPOSCHANGING && m->lParam) {
        HWND maxHwnd = hwndWithoutCreating(m_maxWindow);
        if (maxHwnd && m->hwnd == maxHwnd && IsZoomed(maxHwnd)) {
            auto *wp = reinterpret_cast<WINDOWPOS *>(m->lParam);
            const HMONITOR mon = MonitorFromWindow(maxHwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (mon && GetMonitorInfoW(mon, &mi)) {
                const RECT &w = mi.rcWork;
                wp->x = w.left;
                wp->y = w.top;
                wp->cx = w.right - w.left;
                wp->cy = w.bottom - w.top;
            }
        }
    }

    if (!m_window) return false;
    HWND hwnd = hwndWithoutCreating(m_window);
    if (!hwnd || m->hwnd != hwnd) return false;

    // ---- 非客户区归零：无边框但保留系统行为；最大化时按边框宽度内缩（修顶部缺一行）----
    // 这条 attach 路径（会补 WS_THICKFRAME）仍不处理 WM_GETMINMAXINFO —— 实测会让 Qt 把窗口
    // 误判为已 Maximized 态（启动即 vis=Maximized），最大化按钮首点走 showNormal()（"无效"）。
    // 方案②的原生最大化对齐是另一条路径：上面只改"最大化态窗口矩形"的 WM_WINDOWPOSCHANGING
    // 分支，不动任何状态位，两者互不影响。
    if (m->message == WM_NCCALCSIZE && m->wParam == TRUE && result) {
        // 玻璃（DWM 亚克力）接管时让路：那边把普通态算成"窗口矩形"、最大化态算成"工作区"，
        // 是亚克力唯一正确的算法。这里若抢先按"缩进一圈边框"算（最大化），屏幕四边会留
        // 一条实色带（深色模式=黑框），而且两处都写 *result=0，谁先谁赢，结果不稳定。
        if (Muyun::Backdrop::glassActive()) return false;
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
