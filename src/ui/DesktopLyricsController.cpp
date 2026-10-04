#include "DesktopLyricsController.h"
#include "ui/PlayerController.h"
#include "core/storage/DocumentStore.h"

#include <QQuickView>
#include <QQuickItem>
#include <QQmlEngine>
#include <QScreen>
#include <QGuiApplication>
#include <QWindow>
#include <QUrl>
#include <QRect>
#include <QPoint>
#include <QPointF>
#include <QSurfaceFormat>
#include <QTimer>
#include <QCursor>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace Muyun {

DesktopLyricsController::DesktopLyricsController(PlayerController *player,
                                                 QQmlEngine *engine, QObject *parent)
    : QObject(parent), m_player(player), m_engine(engine)
{
    loadStyle();
    // 置顶窗是"点击穿透"的，收不到任何 Qt 鼠标事件 → 悬停判断只能靠全局光标轮询。
    // 100ms 足够跟手又不费；仅在"窗口可见且置顶"时才真正计算。
    m_hoverTimer = new QTimer(this);
    m_hoverTimer->setInterval(100);
    connect(m_hoverTimer, &QTimer::timeout, this, [this]() { pollHover(); });
}

void DesktopLyricsController::loadStyle()
{
    auto *store = DocumentStore::instance();
    m_color = store->readSync(QStringLiteral("desktop-lyrics"),
                              QStringLiteral("color")).toString();
    if (m_color.isEmpty()) m_color = QStringLiteral("#ff5a5f");
    m_sizePx = store->readSync(QStringLiteral("desktop-lyrics"),
                               QStringLiteral("sizePx")).toInt();
    if (m_sizePx < 12 || m_sizePx > 96) m_sizePx = 28;
}

Qt::WindowFlags DesktopLyricsController::windowFlags() const
{
    // 置顶态**不**用 Qt::WindowTransparentForInput：那个是 Qt 的"窗口行为开关"，Qt 平台层
    // 会自己按它拦鼠标事件。一旦设了，就算之后手动把 Win32 的 WS_EX_TRANSPARENT 摘掉、
    // WindowFromPoint 也真的命中我们窗口，点击仍然到不了 QML（实测"取消置顶"钮点了没反应
    // 的真因）。所以穿透改由本类直接用 WS_EX_TRANSPARENT 管，Qt 的 flags 始终不声明透明。
    return Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool;
}

// 直接管 Win32 的 WS_EX_TRANSPARENT（点击穿透）：
//   设 → OS 命中测试把鼠标漏给下层窗口；摘 → 鼠标回到本窗。
// 只改扩展样式，不 setFlags()（setFlags 会重建原生窗口，连带位置/可见性/置顶全丢）。
void DesktopLyricsController::applyClickThrough(bool through)
{
#ifdef Q_OS_WIN
    if (!m_view) return;
    const HWND hwnd = reinterpret_cast<HWND>(m_view->winId());
    if (!hwnd) return;
    const LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    const LONG_PTR want = through ? (ex | static_cast<LONG_PTR>(WS_EX_TRANSPARENT))
                                  : (ex & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
    if (want != ex) {
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, want);
        // 通知 DWM 按新扩展样式重排，避免窗口边缘残留旧状态
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    }
#endif
}

void DesktopLyricsController::ensureView()
{
    if (m_view) return;

    // 复用主 engine → 直接共享 player / theme / deskLyrics contextProperty
    m_view = new QQuickView(m_engine, nullptr);
    // 逐像素 alpha：让顶层窗本身透明，只显示 QML 内容（半透明条 + 描边文字）
    QSurfaceFormat fmt = m_view->format();
    fmt.setAlphaBufferSize(8);
    m_view->setFormat(fmt);
    m_view->setColor(Qt::transparent);
    m_view->setResizeMode(QQuickView::SizeRootObjectToView);
    m_view->setFlags(windowFlags());
    m_view->setSource(QUrl(QStringLiteral("qrc:/qml/DesktopLyricsView.qml")));
    m_view->resize(760, neededHeight());
    m_view->setTitle(QStringLiteral("暮云 - 桌面歌词"));

    // 初始位置：主屏底部居中
    if (auto *screen = QGuiApplication::primaryScreen()) {
        const QRect geo = screen->availableGeometry();
        m_view->setX(geo.x() + (geo.width() - m_view->width()) / 2);
        m_view->setY(geo.y() + geo.height() - m_view->height() - 60);
    }
    connect(m_view, &QWindow::visibleChanged, this,
            &DesktopLyricsController::visibleChanged);
    applyClickThrough(m_pinned);   // 初态按置顶态同步一次穿透（通常 false）
}

// 两行（原文 + 译文）都要放得下：窗高按字号算，
// 否则"特大"档两行接近 100px 上限，译文会被裁掉——正是"有时只显示原文"的一种成因
int DesktopLyricsController::neededHeight() const
{
    const double mainH = m_sizePx * 1.30;                    // 原文行高
    const double transH = qMax(11.0, m_sizePx * 0.55) * 1.30; // 译文行高
    const double total = mainH + transH + 4 /*spacing*/ + 24 /*上下留白*/;
    return qMax(100, static_cast<int>(total) + 8);
}

bool DesktopLyricsController::isVisible() const
{
    return m_view && m_view->isVisible();
}

void DesktopLyricsController::show()
{
    ensureView();
    m_view->show();
    m_view->raise();
    if (!m_hoverTimer->isActive()) m_hoverTimer->start();
    emit visibleChanged();
}

void DesktopLyricsController::hide()
{
    if (m_view) {
        m_view->hide();
        if (m_pinnedHover) { m_pinnedHover = false; emit pinnedHoverChanged(); }
        m_hoverTimer->stop();
        emit visibleChanged();
    }
}

void DesktopLyricsController::toggle()
{
    if (!isVisible()) {
        setPinned(false);
        show();                       // 隐藏 → 编辑态
    } else if (m_pinned) {
        setPinned(false);             // 置顶 → 编辑态（释放置顶层）
    } else {
        setPinned(true);              // 编辑态 → 置顶（点击穿透）
    }
}

void DesktopLyricsController::requestActivate()
{
    ensureView();
    if (!m_view) return;
    m_view->show();
    m_view->raise();
    m_view->requestActivate();
#ifdef Q_OS_WIN
    if (HWND h = reinterpret_cast<HWND>(m_view->winId())) {
        SetForegroundWindow(h);
        SetWindowPos(h, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }
#endif
}

void DesktopLyricsController::setPinned(bool on)
{
    if (m_pinned == on) return;
    m_pinned = on;
    if (m_view) {
        // 不 setFlags()：那会重建原生窗口，位置/可见性/置顶都会丢（老代码还要额外存位置补回来）
        applyClickThrough(on);
        if (on) m_view->raise();
    }
    // 取消置顶时清掉悬停态（编辑态靠 QML 自己的 hoverLayer，不走这条）
    if (!on && m_pinnedHover) { m_pinnedHover = false; emit pinnedHoverChanged(); }
    emit pinnedChanged();
}

// 置顶态"放开穿透"的判据区域 = **「取消置顶」按钮本身**（加一点余量）。
// ⚠ 用户 2026-10-04 二次澄清：进歌词带要**显钮**（命中区=整窗，见 pollHover 的 showBtn），
//   但**只有钮那一小块需要能点**——其余区域必须继续穿透，否则挡住桌面。
// 所以"显钮"和"放开穿透"是两个区域：整窗 vs 按钮。
// ⚠ 全程在 Win32 物理坐标域算：QQuickItem::mapToGlobal 是 Qt 逻辑坐标，必须乘
//   devicePixelRatio 再和 GetCursorPos 比（dpr≠1 时错域 = 恒假或恒真，已咬过两次）。
QRect DesktopLyricsController::pinnedHitRectPhysical() const
{
    if (!m_view) return QRect();
    QQuickItem *root = m_view->rootObject();
    if (!root) return QRect();
    QQuickItem *btn = root->findChild<QQuickItem*>(QStringLiteral("unpinBtnObj"));
    if (!btn) return QRect();   // QML 未解析完：调用方回退整窗判据
    const QScreen *scr = m_view->screen() ? m_view->screen()
                                          : QGuiApplication::primaryScreen();
    const qreal dpr = scr ? scr->devicePixelRatio() : 1.0;
    const qreal pad = 6.0;   // 逻辑像素余量，别让命中区只剩半个像素
    const QPointF tl = btn->mapToGlobal(QPointF(-pad, -pad));
    const QPointF br = btn->mapToGlobal(QPointF(btn->width() + pad, btn->height() + pad));
    const QRect r(QPoint(qRound(tl.x() * dpr), qRound(tl.y() * dpr)),
                  QPoint(qRound(br.x() * dpr), qRound(br.y() * dpr)));
    return r.isEmpty() ? QRect() : r.normalized();
}

// 置顶窗点击穿透、收不到 Qt 鼠标事件 → 用全局光标位置判断"鼠标是否落在可点区域"。
// 命中即让 QML 显出「取消置顶」钮（见 DesktopLyricsView.qml：pinned && pinnedHover）。
// ⚠ 关键：穿透状态下连 HoverHandler 都收不到事件，所以"让这一小块能点"必须由这里主动切——
//   命中→临时去 WS_EX_TRANSPARENT（可点）；离开→恢复穿透。全程只动扩展样式，不重建窗口。
void DesktopLyricsController::pollHover()
{
    // ⚠ 判据刻意**不看 m_view->isVisible()**：置顶窗是 Qt::Tool + 逐像素透明，Qt 的内部可见性
    //   标志在这种窗上会失真（实测 isVisible 谎报 false），一旦拿来早退，pollHover 永远不算、
    //   pinnedHover 永远 0（就是"取消置顶钮不显示"的真因）。窗的真实显隐由 QML 侧负责；
    //   这里只要"已置顶且 view 存在"就照常算。
    if (!m_pinned || !m_view) {
        if (m_pinnedHover) { m_pinnedHover = false; emit pinnedHoverChanged(); }
        return;
    }
#ifdef Q_OS_WIN
    // 命中判据只用 Win32 物理坐标：GetCursorPos 与 GetWindowRect 天然同域，dpr≠1 时也不变。
    // Qt 那套（mapToGlobal + QCursor::pos）要靠 Qt 自己做逻辑↔物理换算，换算一有偏差
    // contains 就恒假 → pinnedHover 恒 0 → 取消置顶钮不显示 / 点不动复发。这类问题已咬过两次：
    //   ① position() 与 QCursor::pos() 不同域（改用 mapToGlobal 修过）；
    //   ② 实测 dpr=1.25 下 GetWindowRect=(485,820)-(1435,945) 而 mapToGlobal=(388,656)，
    //      同一个左上角差一个 1.25 倍，自检 5 次里跑出 1 次 hover 恒 0。
    // 结论：判据别压在 Qt 的坐标换算上，问系统要物理矩形最稳。
    const HWND hwnd = reinterpret_cast<HWND>(m_view->winId());
    POINT cur{};
    if (!hwnd || !GetCursorPos(&cur)) return;
    // ① 显钮区 = 整窗：进歌词带就让 QML 显出「取消置顶」钮（钮本身"悬停才出现"，
    //    命中区要是按钮的话用户根本不知道该往哪移才能看到它）。
    RECT wrc{};
    if (!GetWindowRect(hwnd, &wrc)) return;
    const QRect winRect(QPoint(wrc.left, wrc.top), QPoint(wrc.right - 1, wrc.bottom - 1));
    const bool showBtn = winRect.contains(QPoint(cur.x, cur.y));
    // ② 可点区 = 按钮本身：只有鼠标压到那一小块才摘 WS_EX_TRANSPARENT，
    //    其余区域继续穿透（否则挡住桌面）。
    const QRect btnRect = pinnedHitRectPhysical();
    const bool interact = !btnRect.isNull() && btnRect.contains(QPoint(cur.x, cur.y));
    if (showBtn != m_pinnedHover) {
        m_pinnedHover = showBtn;
        emit pinnedHoverChanged();
    }
    setInteractiveForUnpin(interact);   // 钮上才放开穿透，移开立刻恢复
#else
    // 非 Windows 没有 WS_EX_TRANSPARENT 这套，用整窗几何比光标（逻辑坐标同域）
    const QPoint g = QCursor::pos();
    const QPoint tl = m_view->mapToGlobal(QPoint(0, 0));
    const bool showBtn = QRectF(tl, m_view->size()).contains(g);
    if (showBtn != m_pinnedHover) {
        m_pinnedHover = showBtn;
        setInteractiveForUnpin(showBtn);
        emit pinnedHoverChanged();
    }
#endif
}

void DesktopLyricsController::setInteractiveForUnpin(bool on)
{
#ifdef Q_OS_WIN
    applyClickThrough(!on);   // 悬停可点=true → 摘掉穿透
#else
    Q_UNUSED(on)
#endif
}

quintptr DesktopLyricsController::winIdForTest() const
{
    return m_view ? reinterpret_cast<quintptr>(m_view->winId()) : quintptr(0);
}

QPoint DesktopLyricsController::centerGlobalForTest() const
{
    if (!m_view) return QPoint(0, 0);
    const QPoint p = m_view->mapToGlobal(QPoint(0, 0));   // 与 pollHover 同用 mapToGlobal（逻辑坐标）
    const QSize s = m_view->size();
    return QPoint(p.x() + s.width() / 2, p.y() + s.height() / 2);
}

QPointF DesktopLyricsController::unpinButtonGlobalForTest() const
{
    if (!m_view) return QPointF();
    QQuickItem *root = m_view->rootObject();
    if (!root) return QPointF();
    // 按钮只有"置顶+悬停"时才可见，但布局不受 visible 影响 → 坐标始终取得到
    if (QQuickItem *btn = root->findChild<QQuickItem*>(QStringLiteral("unpinBtnObj")))
        return btn->mapToGlobal(btn->boundingRect().center());
    return QPointF();
}

QRect DesktopLyricsController::unpinButtonHitRectForTest() const
{
    return pinnedHitRectPhysical();
}

bool DesktopLyricsController::clickThroughForTest() const
{
#ifdef Q_OS_WIN
    if (!m_view) return false;
    const HWND hwnd = reinterpret_cast<HWND>(m_view->winId());
    if (!hwnd) return false;
    return (GetWindowLongPtr(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0;
#else
    return false;
#endif
}

void DesktopLyricsController::setColor(const QString &c)
{
    if (m_color == c || c.isEmpty()) return;
    m_color = c;
    DocumentStore::instance()->write(QStringLiteral("desktop-lyrics"),
                                     QStringLiteral("color"), c);
    emit styleChanged();
}

void DesktopLyricsController::setSizePx(int px)
{
    px = qBound(12, px, 96);
    if (m_sizePx == px) return;
    m_sizePx = px;
    DocumentStore::instance()->write(QStringLiteral("desktop-lyrics"),
                                     QStringLiteral("sizePx"), px);
    // 字号变了要跟着改窗高，否则大字号下译文被裁掉；保持底边不动，视觉上只是往上长
    if (m_view) {
        const int oldH = m_view->height();
        const int newH = neededHeight();
        if (newH != oldH) {
            const QPoint pos = m_view->position();
            m_view->resize(m_view->width(), newH);
            m_view->setPosition(pos.x(), pos.y() + oldH - newH);
        }
    }
    emit styleChanged();
}

} // namespace Muyun
