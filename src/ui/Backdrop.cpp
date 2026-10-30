#include "Backdrop.h"
#include "WindowHwnd.h"

#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QString>
#include <QTimer>
#include <QWindow>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#include <windowsx.h>
#endif

#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_COLOR_NONE
#define DWMWA_COLOR_NONE 0xFFFFFFFE
#endif
#ifndef DWMWA_NCRENDERING_POLICY
#define DWMWA_NCRENDERING_POLICY 2
#endif
#ifndef DWMWA_DONOTRENDER
#define DWMWA_DONOTRENDER 2
#endif

namespace Muyun {

namespace {
#ifdef Q_OS_WIN
constexpr int kBackdropAcrylic = 3;   ///< DWMSBT_TRANSIENTWINDOW：亚克力（Mica=2 本机渲染成纯黑，弃用）
constexpr int kCornerRound     = 2;   ///< DWMWCP_ROUND
constexpr int kCornerDoNotRound = 1;  ///< DWMWCP_DONOTROUND
constexpr int kNoRender        = 2;   ///< DWMWA_DONOTRENDER：DWM 不画非客户区（标题栏/按钮/描边）

/// 全屏/贴边态：DWM 的圆角与 1px 描边必须关掉。
/// 全屏窗是 WS_POPUP，系统不当它是"最大化"，圆角偏好（=ROUND）会一直生效 →
/// 屏幕四角被切掉、看上去像"整块屏被套了一个黑框"；描边则会在边缘画一条浅线。
/// 另外禁用 DWM 非客户区渲染：窗口失焦时 DWM 会画"非活动标题栏"（左上角白带）
/// 和一圈描边（有焦深色=黑框、失焦浅色=白框），QML 在应用层盖不住 DWM 合成层。
void suppressSystemFrame(HWND hwnd)
{
    int corner = kCornerDoNotRound;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    int ncr = kNoRender;
    DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &ncr, sizeof(ncr));
    DWORD borderNone = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &borderNone, sizeof(borderNone));
}

/// Win11 22H2（build 22621）起才有 DWMWA_SYSTEMBACKDROP_TYPE
bool osSupportsBackdrop()
{
    static const bool ok = []() {
        OSVERSIONINFOEXW vi{};
        vi.dwOSVersionInfoSize = sizeof(vi);
        // RtlGetVersion 不受兼容层撒谎影响（GetVersionEx 会报 6.2）
        using RtlGetVersionFn = LONG(WINAPI *)(OSVERSIONINFOEXW *);
        if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
            auto fn = reinterpret_cast<RtlGetVersionFn>(
                reinterpret_cast<void *>(GetProcAddress(nt, "RtlGetVersion")));
            if (fn && fn(&vi) == 0) return vi.dwBuildNumber >= 22621;
        }
        return false;
    }();
    return ok;
}

/// 只认主屏合成器可用（远程桌面/无 DWM 时 backdrop 无效，避免白忙）
bool compositionEnabled()
{
    BOOL on = FALSE;
    return SUCCEEDED(DwmIsCompositionEnabled(&on)) && on;
}
#endif
} // namespace

void Backdrop::dumpProcessWindows(const char *tag)
{
#ifdef Q_OS_WIN
    struct Ctx {
        DWORD pid;
        const char *tag;
    } ctx{GetCurrentProcessId(), tag};

    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto *c = reinterpret_cast<Ctx *>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid != c->pid) return TRUE;
            wchar_t cls[128]{}, title[256]{};
            GetClassNameW(h, cls, 128);
            GetWindowTextW(h, title, 256);
            RECT wr{};
            GetWindowRect(h, &wr);
            const LONG_PTR st = GetWindowLongPtr(h, GWL_STYLE);
            const LONG_PTR ex = GetWindowLongPtr(h, GWL_EXSTYLE);
            int kids = 0;
            EnumChildWindows(h, [](HWND, LPARAM n) -> BOOL {
                ++*reinterpret_cast<int *>(n);
                return TRUE;
            }, reinterpret_cast<LPARAM>(&kids));
            qInfo().noquote()
                << QStringLiteral("[ui] %1 hwnd=%2 class=%3 title=%4 vis=%5 style=%6 ex=%7"
                                  " thick=%8 caption=%9 popup=%10 layered=%11 tool=%12"
                                  " rect=%13,%14,%15,%16 kids=%17")
                       .arg(QString::fromLatin1(c->tag),
                            QString::number(reinterpret_cast<quintptr>(h), 16),
                            QString::fromWCharArray(cls), QString::fromWCharArray(title),
                            QString::number(IsWindowVisible(h) ? 1 : 0),
                            QStringLiteral("0x%1").arg(st, 8, 16, QLatin1Char('0')),
                            QStringLiteral("0x%1").arg(ex, 8, 16, QLatin1Char('0')),
                            QString::number((st & WS_THICKFRAME) ? 1 : 0),
                            QString::number((st & WS_CAPTION) ? 1 : 0),
                            QString::number((st & WS_POPUP) ? 1 : 0),
                            QString::number((ex & WS_EX_LAYERED) ? 1 : 0),
                            QString::number((ex & WS_EX_TOOLWINDOW) ? 1 : 0))
                       .arg(wr.left)
                       .arg(wr.top)
                       .arg(wr.right)
                       .arg(wr.bottom)
                       .arg(kids);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
#else
    Q_UNUSED(tag)
#endif
}

Backdrop::Backdrop(QObject *parent) : QObject(parent)
{
    qApp->installNativeEventFilter(this);
#ifdef Q_OS_WIN
    // 诊断：MUYUN_GLASS_DEBUG=1 时每秒打一次窗口/客户区几何差（白框排查用，后台静默）。
    // 差 = 0 才说明客户区真的铺满窗口；>0 就是系统边框/拖动框留下来的band。
    if (!qEnvironmentVariableIsEmpty("MUYUN_GLASS_DEBUG")) {
        auto *t = new QTimer(this);
        t->setInterval(1000);
        QObject::connect(t, &QTimer::timeout, this, [this]() {
            if (!m_window) return;
            HWND h = hwndWithoutCreating(m_window);
            if (!h) return;
            RECT wr{}, cr{};
            if (!GetWindowRect(h, &wr) || !GetClientRect(h, &cr)) return;
            const LONG_PTR st = GetWindowLongPtr(h, GWL_STYLE);
            // 回读 DWM 的两个关键属性：描边色（NONE 才不会画那圈 1px 亮线）与圆角偏好。
            // 只打数字，便于判断"边上那条线"是系统画的还是 QML 画的。
            DWORD border = 0xFFFFFFFF;
            DwmGetWindowAttribute(h, DWMWA_BORDER_COLOR, &border, sizeof(border));
            int corner = -1;
            DwmGetWindowAttribute(h, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
            qInfo("[bd] tick vis=%d zoom=%d thick=%d popup=%d win=%ld,%ld,%ld,%ld client=%dx%d"
                  " diff=%dx%d border=%#lx corner=%d",
                  m_window->isVisible() ? 1 : 0, IsZoomed(h) ? 1 : 0,
                  (st & WS_THICKFRAME) ? 1 : 0, (st & WS_POPUP) ? 1 : 0, wr.left, wr.top,
                  wr.right, wr.bottom, cr.right, cr.bottom,
                  (wr.right - wr.left) - (cr.right - cr.left),
                  (wr.bottom - wr.top) - (cr.bottom - cr.top),
                  static_cast<unsigned long>(border), corner);
        });
        t->start();
    }
#endif
}

Backdrop::~Backdrop() = default;

bool Backdrop::s_glassActive = false;

bool Backdrop::glassActive()
{
    return s_glassActive;
}

bool Backdrop::available() const
{
#ifdef Q_OS_WIN
    return osSupportsBackdrop() && compositionEnabled();
#else
    return false;
#endif
}

void Backdrop::attach(QWindow *window)
{
    if (m_window == window && m_applied == m_enabled) return;
    m_window = window;
    m_applied = false;
    if (m_window) {
        // 窗口若还没落 HWND（未显示），等第一次可见再挂
        QObject::connect(m_window, &QWindow::visibleChanged, this,
                         [this]() { if (m_window && m_window->isVisible()) applyToWindow(); });
        applyToWindow();
    } else {
        emit enabledChanged();
    }
}

void Backdrop::refresh()
{
    if (!m_window) return;
    m_applied = false;
    // F11 全屏：样式位由 Qt 接管，别跟它抢（退出全屏时这次信号会再把 backdrop 挂回来）。
    // 但圆角/描边必须当场关掉：全屏窗是 WS_POPUP，系统不当它是最大化，之前设的
    // DWMWCP_ROUND 会留着 → 屏幕四个角被切掉，看起来就是"全屏后多了一圈黑框"。
    if (m_window->visibility() == QWindow::FullScreen) {
#ifdef Q_OS_WIN
        if (HWND h = hwndWithoutCreating(m_window)) suppressSystemFrame(h);
#endif
        return;
    }
    // ⚠ 必须延到下一轮事件循环再挂：visibleChanged/visibilityChanged 是在 Qt 自己的
    // 状态切换**过程中**发的，此刻同步改样式会被 Qt 随后的 SetWindowPos/SetWindowLongPtr
    // 覆盖，留下"样式已加边框、客户区却没归零"的半截状态 → DWM 把系统标题栏画出来
    // （v1.4.0 实测：F11 全屏往返后再最大化，系统标题栏糊在应用顶上）。延后一拍等 Qt 收工。
    QTimer::singleShot(0, this, [this]() { applyToWindow(); });
}

void Backdrop::setEnabled(bool on)
{
    if (m_enabled == on) return;
    m_enabled = on;
    applyToWindow();
    emit enabledChanged();
}

void Backdrop::setDark(bool dark)
{
    if (m_dark == dark) return;
    m_dark = dark;
    // 只需重刷 DWM 属性（样式不必再动）
    applyToWindow();
}

void Backdrop::applyToWindow()
{
#ifdef Q_OS_WIN
    if (!m_window || !m_window->isVisible()) return;
    // 全屏期间样式位归 Qt 管（它会把窗切回 WS_POPUP 盖满屏幕），别去动它；
    // 但圆角/描边要关（见 refresh() 里的说明），否则全屏四角是缺的。
    if (m_window->visibility() == QWindow::FullScreen) {
        if (HWND h = hwndWithoutCreating(m_window)) suppressSystemFrame(h);
        if (m_enabled) s_glassActive = true;
        return;
    }
    HWND hwnd = hwndWithoutCreating(m_window);
    if (!hwnd) return;
    m_applied = false;

    if (!m_enabled || !available()) {
        s_glassActive = false;
        resetWindowStyles();
        return;
    }

    // ---- 1) 样式：保持无边框，**不要补 WS_THICKFRAME** ----
    // ⚠ 根因（2026-10-31 实测锁定）：带 WS_THICKFRAME 的窗口，DWM 会按"标准可缩放窗"
    //    画一圈 1px 外沿描边（失焦=白、有焦=深色）。`DWMWA_BORDER_COLOR(34)` 在本系统
    //    上 Get/Set 都返回 E_FAIL（probe2/probe3 实测），所以任何"设 COLOR_NONE 关描边"
    //    的调用都静默失败，白框/黑框始终压在最外沿。而 DWM 合成层在 QML 之上，场景层
    //    永远盖不住它。**唯一可靠解法就是让 DWM 不当它是标准窗** —— 即不加 WS_THICKFRAME。
    //    缩放/最大化都不依赖它：缩放走 QML 手动 ResizeHandle（applyWindowGeometry），
    //    最大化走 FramelessWindow::setNativeMaximized(ShowWindow)。v1.3 也是纯 WS_POPUP 无
    //    THICKFRAME 的原生最大化，行为已验证。因此这里**不再 OR 进 WS_THICKFRAME**。
    //
    //   保留最小/最大化/系统菜单，但去掉"可缩放边框"语义；WS_CAPTION 依然绝不加（带
    //   CAPTION 时 DWM 会画真标题栏并在最大化/全屏往返时残留）。实测（MUYUN_BD_SKIP=thick
    //   等价态）左/右外沿 max 从 255(白) 降到 24/30（与内容同档），描边消失。
    //
    // ⚠ 门闩必须先上：下面的 SetWindowPos(SWP_FRAMECHANGED) 会同步发一次 WM_NCCALCSIZE，
    //    此时若 m_applied 还是 false，过滤器会让 DefWindowProc 按"有边框"算客户区。
    m_applied = true;
    // 诊断开关：MUYUN_BD_SKIP=extend,corner,border,thick,nudge（逗号分隔，跳过对应步骤）
    const QString skip = QString::fromLocal8Bit(qgetenv("MUYUN_BD_SKIP"));
    const auto skipped = [&skip](const char *name) {
        return skip.contains(QString::fromLatin1(name), Qt::CaseInsensitive);
    };
    const bool dbg = !qEnvironmentVariableIsEmpty("MUYUN_GLASS_DEBUG");
    // 建立过程逐步快照：样式位 + 窗口矩形 + 客户区 + Qt 侧几何/边距 + 内容项位置
    const auto dumpState = [&](const char *tag) {
        if (!dbg) return;
        RECT wr{}, cr{};
        GetWindowRect(hwnd, &wr);
        GetClientRect(hwnd, &cr);
        const LONG_PTR st = GetWindowLongPtr(hwnd, GWL_STYLE);
        const QMargins fm = m_window->frameMargins();
        QQuickItem *ci = nullptr;
        if (auto *qw = qobject_cast<QQuickWindow *>(m_window)) ci = qw->contentItem();
        qInfo("[bd] %s style=%#08lx thick=%d caption=%d popup=%d"
              " win=%ld,%ld,%ld,%ld client=%ld,%ld"
              " qt=%dx%d dpr=%.2f margins=%d,%d,%d,%d content=%s",
              tag, static_cast<unsigned long>(st), (st & WS_THICKFRAME) ? 1 : 0,
              (st & WS_CAPTION) ? 1 : 0, (st & WS_POPUP) ? 1 : 0, wr.left, wr.top, wr.right,
              wr.bottom, cr.right, cr.bottom, m_window->width(), m_window->height(),
              m_window->devicePixelRatio(), fm.left(), fm.top(), fm.right(), fm.bottom(),
              ci ? QStringLiteral("%1,%2 %3x%4")
                       .arg(ci->x()).arg(ci->y()).arg(ci->width()).arg(ci->height())
                       .toUtf8().constData()
                 : "-");
    };
    dumpState("apply-in");
    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    style &= ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME);
    style |= WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
    SetWindowLongPtr(hwnd, GWL_STYLE, style);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER
                     | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    dumpState("after-style");

    // ---- 2) 边框扩展满整窗（-1 margins），客户区才可能铺满 ----
    MARGINS mg{-1, -1, -1, -1};
    const HRESULT hrExtent = skipped("extend")
        ? 0x7FFFFFFF
        : DwmExtendFrameIntoClientArea(hwnd, &mg);

    // ---- 3) 系统绘制的深色阴影 / 圆角 ----
    BOOL dark = m_dark ? TRUE : FALSE;
    const HRESULT hrDark = DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                                                 sizeof(dark));
    int corner = kCornerRound;
    const HRESULT hrCorner = skipped("corner")
        ? 0x7FFFFFFF
        : DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner,
                                sizeof(corner));

    // ---- 4) 亚克力背景 ----
    int type = kBackdropAcrylic;
    const HRESULT hrBackdrop = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &type,
                                                     sizeof(type));

    // ---- 5) 不画窗口描边：带 WS_THICKFRAME 的窗口 DWM 会给一圈 1px 边框（浅色 = 白框），
    //         窗口化态在浅色壁纸下特别碍眼；DWMWA_COLOR_NONE 关掉它（Win11 22000+）
    DWORD borderNone = DWMWA_COLOR_NONE;
    const HRESULT hrBorder = skipped("border")
        ? 0x7FFFFFFF
        : DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &borderNone, sizeof(borderNone));
    // 窗口态也要禁用 DWM 非客户区渲染：失焦时 DWM 画的"非活动标题栏"（左上角白带）
    // 和描边（有焦深/失焦浅）都在 DWM 合成层，QML 盖不住，只能让 DWM 别画。
    int ncr = kNoRender;
    DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &ncr, sizeof(ncr));

    m_applied = true;
    s_glassActive = true;
    m_edgeHints = IsZoomed(hwnd) || m_window->visibility() == QWindow::FullScreen;
    // 窗口态也要显式写一遍圆角偏好：F11 全屏退回来时 suppressSystemFrame 给它设过
    // DONOTROUND，如果这里不恢复，窗口会一直方角到下次进出最大化才被 WM_SIZE 修正。
    if (!m_edgeHints && !skipped("corner")) {
        int winCorner = kCornerRound;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &winCorner, sizeof(winCorner));
    }
    if (dbg) {
        auto *qw = qobject_cast<QQuickWindow *>(m_window);
        const int colorA = qw ? qw->color().alpha() : -1;
        const int api = qw ? static_cast<int>(qw->rendererInterface()->graphicsApi()) : -1;
        qInfo("[bd] glass on hwnd=%p dark=%d backdrop=%ld corner=%ld extent=%ld darkmode=%ld"
              " border=%ld alpha=%d colorA=%d api=%d",
              hwnd, m_dark ? 1 : 0, static_cast<long>(hrBackdrop), static_cast<long>(hrCorner),
              static_cast<long>(hrExtent), static_cast<long>(hrDark),
              static_cast<long>(hrBorder),
              m_window->format().alphaBufferSize(), colorA, api);
        dumpState("after-dwm");
        // 延迟复核：Qt 可能在随后的 SetWindowPos/尺寸协商里重算边距并把内容整体内缩
        QTimer::singleShot(400, this, [this]() {
            if (!m_window || !m_window->isVisible()) return;
            HWND h = hwndWithoutCreating(m_window);
            if (!h) return;
            RECT wr{}, cr{};
            GetWindowRect(h, &wr);
            GetClientRect(h, &cr);
            const QMargins f = m_window->frameMargins();
            QString extra;
            if (auto *qw2 = qobject_cast<QQuickWindow *>(m_window)) {
                if (QQuickItem *ci = qw2->contentItem()) {
                    extra = QStringLiteral(" content=%1,%2 %3x%4")
                                .arg(ci->x())
                                .arg(ci->y())
                                .arg(ci->width())
                                .arg(ci->height());
                }
            }
            qInfo("[bd] +400ms win=%ld,%ld,%ld,%ld client=%ld,%ld qt=%dx%d margins=%d,%d,%d,%d%s",
                  wr.left, wr.top, wr.right, wr.bottom, cr.right, cr.bottom, m_window->width(),
                  m_window->height(), f.left(), f.top(), f.right(), f.bottom(),
                  extra.toUtf8().constData());
        });
    }
    // 连 frame 一起重画：改完样式后旧标题栏/边框的像素归 non-client 区管，
    // 光 invalidate 客户区不会擦掉它们（实测：F11 往返后那条系统标题栏就赖在顶上不走）
    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_FRAME | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);

    // ---- 6) 兜底：确认非客户区真的被抹平了。若此刻客户区仍小于窗口矩形（说明上面那次
    //         SWP_FRAMECHANGED 的 WM_NCCALCSIZE 被谁吃了），再逼一次重算，避免留下一圈
    //         系统边框（浅色壁纸下就是那圈"白框"）。
    {
        RECT wr{}, cr{};
        if (GetWindowRect(hwnd, &wr) && GetClientRect(hwnd, &cr)) {
            const int fw = (wr.right - wr.left) - (cr.right - cr.left);
            const int fh = (wr.bottom - wr.top) - (cr.bottom - cr.top);
            if ((fw > 0 || fh > 0) && !IsZoomed(hwnd)) {
                if (!qEnvironmentVariableIsEmpty("MUYUN_GLASS_DEBUG"))
                    qInfo("[bd] nc leftover %dx%d → redo SetWindowPos", fw, fh);
                SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                             SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER
                                 | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            }
        }
    }

    // ---- 7) 贴边态也要关系统描边 ----
    // 启动即最大化 / 全屏时状态不会再"变"，WM_SIZE 里那次状态切换守不住，这里补一发。
    // （之后的进出最大化/全屏由 nativeEventFilter 的 WM_SIZE 分支负责。）
    if (m_edgeHints) suppressSystemFrame(hwnd);

    // ---- 8) 这里**不要**做任何"强制 resize / nudge"去修正窗口边缘的一条浅色带。
    //         实测过两种写法都不成立：裸 SetWindowPos 尺寸 +1 再还原会让客户区比窗口大 1px；
    //         走 m_window->resize() 则让窗口永久变大 1px，F11 往返后还会越缩越大。
    //         那条"白框"的真因在 QML：窗口底渐变的顶部端色曾经是「白色 alpha 10」，
    //         窗口最上沿只有 4% 不透明度，桌面从那儿透上来。见 Theme::glassSheen()。
    dumpState("apply-out");
#endif
}

void Backdrop::resetWindowStyles()
{
#ifdef Q_OS_WIN
    if (!m_window || !m_window->isVisible()) return;
    HWND hwnd = hwndWithoutCreating(m_window);
    if (!hwnd) return;
    // 去掉 backdrop + 边框扩展，样式回到 Qt 的无边框窗（WS_POPUP 家族）
    int type = 0;                                   // DWMSBT_NONE
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &type, sizeof(type));
    DWORD borderDefault = 0xFFFFFFFF;               // DWMWA_COLOR_DEFAULT：恢复系统描边
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &borderDefault, sizeof(borderDefault));
    MARGINS mg{0, 0, 0, 0};
    DwmExtendFrameIntoClientArea(hwnd, &mg);

    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    style &= ~(WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    style |= WS_POPUP;
    SetWindowLongPtr(hwnd, GWL_STYLE, style);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER
                     | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    s_glassActive = false;
#endif
}

bool Backdrop::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    if (eventType != "windows_generic_MSG") return false;
    MSG *m = static_cast<MSG *>(message);
    // ⚠ 门闩用 m_enabled 而不是 m_applied：refresh()/applyToWindow() 里有"先清 m_applied、
    // 延后一拍再挂"的窗口期，这期间要是来一次 WM_NCCALCSIZE，DefWindowProc 就按"有边框"算客户区
    // → 一圈系统边框（浅色壁纸下表现为"白框"）留在窗口边缘。只要玻璃是开的，客户区就该归零。
    if (!m_window || !m_enabled) return false;
    HWND hwnd = hwndWithoutCreating(m_window);
    if (!hwnd || m->hwnd != hwnd) return false;

    // ---- 自愈式样式守护（兜底）----
    // WS_CAPTION / WS_THICKFRAME 一旦被 Qt/系统在任意过渡（F11 往返、最大化、最小化恢复、
    // 置顶切换）里带回来，DWM 就按"标准窗"画真标题栏（左上角冒最小化/最大化/关闭三连钮）
    // 和一整圈 1px 描边。上面的 WM_STYLECHANGING 里改 styleNew 不是所有系统都生效，
    // 所以这里在**每条消息**上复核实际样式：发现被带坏就当场剥掉并重画整框，把可能
    // 已画出的标题栏/描边像素清掉。正常态只多一次 GetWindowLongPtr，开销可忽略。
    // 剥掉后不会再带 WS_CAPTION，重画触发的下一轮消息不会再次命中 → 不会死循环。
    {
        const LONG_PTR curStyle = GetWindowLongPtr(hwnd, GWL_STYLE);
        if (curStyle & (WS_CAPTION | WS_THICKFRAME)) {
            const LONG_PTR clean = curStyle & ~static_cast<LONG_PTR>(WS_CAPTION | WS_THICKFRAME);
            SetWindowLongPtr(hwnd, GWL_STYLE, clean);
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_FRAME | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            if (!qEnvironmentVariableIsEmpty("MUYUN_GLASS_DEBUG"))
                qInfo("[bd] style guard stripped %#lx -> %#lx", static_cast<unsigned long>(curStyle),
                      static_cast<unsigned long>(clean));
        }
    }

    // ---- 样式位守护：不要让 DWM 把它当"标准可缩放窗" ----
    // 1) WS_CAPTION：左上角冒出来的"最小化/最大化/关闭"三连按钮就是它。进出 F11、
    //    最小化恢复、切换置顶时，Qt（或系统）会整份重写窗口样式；只要哪一次把
    //    WS_CAPTION 带回来，DWM 就认这是"真标题栏"，拿窗口标题去画它。
    // 2) WS_THICKFRAME：会让 DWM 画一圈 1px 外沿描边（失焦白、有焦深色），这是
    //    "黑框/白框"的根因（DWMWA_BORDER_COLOR(34) 本系统 E_FAIL，无法用属性关）。
    //    任何过渡（最大化/全屏/最小化恢复）若把它带回来，描边就会复现。所以也剥掉。
    if (m->message == WM_STYLECHANGING && m->lParam) {
        auto *ss = reinterpret_cast<STYLESTRUCT *>(m->lParam);
        ss->styleNew &= ~static_cast<DWORD>(WS_CAPTION | WS_THICKFRAME);
    }
    // ---- 激活状态切换：DWM 会把"非活动窗口描边"画回来 ----
    // 平时 applyToWindow() 里用 DWMWA_BORDER_COLOR=COLOR_NONE 关掉了那圈 1px 描边，
    // 但点击别的程序（本窗失活）时 DWM 会重新按系统"非活动描边色"画一圈（浅色主题
    // 下就是白条，用户报的"点其它程序时窗口边缘出现白色边框条纹"）。激活/失活各重写
    // 一次"无描边"，并把整窗（含 frame）标脏，让可能残留的旧描边像素被重画掉。
    if (m->message == WM_NCACTIVATE) {
        DWORD borderNone = DWMWA_COLOR_NONE;
        DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &borderNone, sizeof(borderNone));
        // 激活/失活时 DWM 会重画非活动标题栏与描边，这里再压一次 NCRENDERING=DISABLED。
        int ncr = kNoRender;
        DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &ncr, sizeof(ncr));
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
    }
    // ---- 非客户区一律不画 ----
    // 系统偶尔会自己发 WM_NCPAINT 去补画边框/标题栏（F11 往返、从最小化恢复时最明显），
    // 画出来就压在窗口边缘上 → 用户看到的那条白线/黑边。吞掉，交给 QML 画。
    if (m->message == WM_NCPAINT) {
        if (result) *result = 0;
        return true;
    }

    // 诊断用：记下本次将要生效的窗口矩形（MUYUN_GLASS_DEBUG 里与 propos 对照）。
    // 注意：本过滤器装得比 FramelessWindow 早，Qt 按"后装先收"派发 → 这里拿到的
    // WINDOWPOS 已经是 FramelessWindow 修正过最大化贴边之后的最终值。
    if (m->message == WM_WINDOWPOSCHANGING && m->lParam
        && !qEnvironmentVariableIsEmpty("MUYUN_GLASS_DEBUG")) {
        auto *wp = reinterpret_cast<WINDOWPOS *>(m->lParam);
        if (wp->cx > 0 && wp->cy > 0) {
            RECT gr{};
            if (!GetWindowRect(hwnd, &gr)) return false;
            const int x = (wp->flags & SWP_NOMOVE) ? gr.left : wp->x;
            const int y = (wp->flags & SWP_NOMOVE) ? gr.top : wp->y;
            m_pendingRect = QRect(x, y, wp->cx, wp->cy);
            static int wpn = 0;
            qInfo("[bd] wpos #%d %d,%d %dx%d flags=%#x", ++wpn, x, y, wp->cx, wp->cy,
                  static_cast<unsigned>(wp->flags));
        }
    }

    // ---- 拖动缩放结束：整窗(含 frame)重画一遍 ----
    // 拉边框时 Windows 按实时尺寸反复重排客户区，松手那一下偶尔会留下一像素没被 QML
    // 盖住的边（浅壁纸下就是拖动那一侧的一条白条）。结束时强制重画，成本只有一次。
    if (m->message == WM_EXITSIZEMOVE) {
        RedrawWindow(hwnd, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_FRAME | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }

    // ---- 贴边态的系统描边/圆角 ----
    // 最大化/全屏时窗口铺满工作区（或整屏），DWM 还按"带 WS_THICKFRAME 的普通窗"画
    // 一圈 1px 描边 + 圆角：全屏下就是屏幕边上的亮线、四角缺一块。
    // 这里在每次 WM_SIZE 里守一遍（进出最大化/全屏都会来），只在状态切换时真正下指令。
    if (m->message == WM_SIZE) {
        const bool edge = IsZoomed(hwnd) || m_window->visibility() == QWindow::FullScreen;
        if (edge != m_edgeHints) {
            m_edgeHints = edge;
            if (edge) {
                suppressSystemFrame(hwnd);
            } else {
                // 还原态只把圆角偏好放回去。描边色保持 NONE（applyToWindow 里设的常态），
                // 不要在每次进出最大化时去动它 —— DWM 属性写多了会在标题栏/边框上留下旧位图。
                int corner = kCornerRound;
                DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner,
                                      sizeof(corner));
            }
        }
    }

    // 亚克力生效时窗口带标准边框，客户区必须自己抹平：
    //  - 普通态：客户区 = 整个窗口矩形（边框被吃掉，内容铺到窗口边缘，圆角交给 DWM 裁）
    //  - 最大化：客户区 = **监视器工作区**。带边框的窗口最大化后，Windows 会把窗口矩形
    //    在工作区外各扩一圈（本机 125% DPI 是 9 物理像素）——那圈是边框该待的地方。
    //    若照抄系统建议的 rgrc[0]（= 扩过圈的窗口矩形），内容会宽出工作区：左边/顶部各丢
    //    9px 看不见、底部那 9px 钻进任务栏。所以这里按工作区重算，边框仍在工作区外，不动。
    //  - F11 全屏：客户区 = 整块屏幕（Qt 的全屏窗贴的是 rcMonitor，不是工作区）
    if (m->message == WM_NCCALCSIZE && m->wParam == TRUE && result && m->lParam) {
        auto *p = reinterpret_cast<NCCALCSIZE_PARAMS *>(m->lParam);
        const RECT proposed = p->rgrc[0];   // 系统建议的新客户区
        // ⚠ 普通态下 proposed **就是本次将生效的窗口矩形**（实测：窗口一直由我们把
        //   客户区抹平成窗口大小，系统按"客户区随窗口同量变化"推出来的建议值就等于
        //   新窗口矩形，wf22 日志里 propos == pend == 新尺寸）。所以这里什么都不用改，
        //   直接原样返回 0 —— 不要再用旧方案那样去抄 WM_WINDOWPOSCHANGING 记下的矩形：
        //   那份快照在最大化/全屏往返后会残留，一旦尺寸变小，客户区就被按旧坐标缩进去，
        //   窗口边上留出一圈没人画的带子（浅壁纸=白框、深色=黑框）。
        if (IsZoomed(hwnd)) {
            const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (mon && GetMonitorInfoW(mon, &mi)) p->rgrc[0] = mi.rcWork;
        } else if (m_window->visibility() == QWindow::FullScreen) {
            const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (mon && GetMonitorInfoW(mon, &mi)) p->rgrc[0] = mi.rcMonitor;
        }
        *result = 0;
        if (!qEnvironmentVariableIsEmpty("MUYUN_GLASS_DEBUG")) {
            static int n = 0;
            ++n;
            RECT gr{};
            GetWindowRect(hwnd, &gr);
            QQuickItem *ci = nullptr;
            if (auto *qw = qobject_cast<QQuickWindow *>(m_window)) ci = qw->contentItem();
            qInfo("[bd] nccalcsize #%d zoomed=%d fs=%d rgrc=%ld,%ld,%ld,%ld"
                  " getrect=%ld,%ld,%ld,%ld propos=%ld,%ld,%ld,%ld"
                  " qt=%d,%d %dx%d content=%s",
                  n, IsZoomed(hwnd) ? 1 : 0,
                  m_window->visibility() == QWindow::FullScreen ? 1 : 0, p->rgrc[0].left,
                  p->rgrc[0].top, p->rgrc[0].right, p->rgrc[0].bottom, gr.left, gr.top,
                  gr.right, gr.bottom, proposed.left, proposed.top, proposed.right,
                  proposed.bottom, m_window->x(), m_window->y(), m_window->width(),
                  m_window->height(),
                  ci ? QStringLiteral("%1,%2 %3x%4")
                           .arg(ci->x()).arg(ci->y()).arg(ci->width()).arg(ci->height())
                           .toUtf8().constData()
                     : "-");
        }
        return true;
    }

    // 最大化几何对齐交给 FramelessWindow 的 WM_WINDOWPOSCHANGING 分支（避免两处重复改）
#else
    Q_UNUSED(eventType) Q_UNUSED(message) Q_UNUSED(result)
#endif
    return false;
}

} // namespace Muyun
