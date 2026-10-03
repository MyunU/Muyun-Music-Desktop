#include "DesktopLyricsController.h"
#include "ui/PlayerController.h"
#include "core/storage/DocumentStore.h"

#include <QQuickView>
#include <QQmlEngine>
#include <QScreen>
#include <QGuiApplication>
#include <QWindow>
#include <QUrl>
#include <QRect>
#include <QPoint>
#include <QSurfaceFormat>

namespace Muyun {

DesktopLyricsController::DesktopLyricsController(PlayerController *player,
                                                 QQmlEngine *engine, QObject *parent)
    : QObject(parent), m_player(player), m_engine(engine)
{
    loadStyle();
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
    Qt::WindowFlags flags = Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool;
    if (m_pinned) flags |= Qt::WindowTransparentForInput;   // 点击穿透
    return flags;
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
    emit visibleChanged();
}

void DesktopLyricsController::hide()
{
    if (m_view) {
        m_view->hide();
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

void DesktopLyricsController::setPinned(bool on)
{
    if (m_pinned == on) return;
    m_pinned = on;
    if (m_view) {
        // 保留位置（改 flags 可能重建平台窗口）
        const QPoint pos = m_view->position();
        m_view->setFlags(windowFlags());
        m_view->setPosition(pos);
        if (on) m_view->raise();
    }
    emit pinnedChanged();
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
