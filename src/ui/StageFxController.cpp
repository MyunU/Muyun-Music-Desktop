#include "StageFxController.h"
#include "StageBridge.h"

#include <QQuickView>
#include <QQmlEngine>
#include <QScreen>
#include <QGuiApplication>
#include <QWindow>
#include <QUrl>
#include <QRect>
#include <QSurfaceFormat>

#include <cstdio>

namespace Muyun {

StageFxController::StageFxController(StageBridge *stage, QQmlEngine *engine,
                                     QObject *parent)
    : QObject(parent), m_stage(stage), m_engine(engine)
{
    // 舞台进程被杀（最小化/退出样式）→ 面板同步隐藏，避免"孤儿面板"
    if (m_stage) {
        connect(m_stage, &StageBridge::activeChanged, this, [this]() {
            if (m_stage && !m_stage->active()) hide();
        });
    }
}

void StageFxController::ensureView()
{
    if (m_view) return;

    // 复用主 engine → 直接共享 stage/theme contextProperty（桌面歌词同款模式）
    m_view = new QQuickView(m_engine, nullptr);
    QSurfaceFormat fmt = m_view->format();
    fmt.setAlphaBufferSize(8);          // 逐像素 alpha：圆角外透明
    m_view->setFormat(fmt);
    m_view->setColor(Qt::transparent);
    m_view->setResizeMode(QQuickView::SizeRootObjectToView);
    // StaysOnTop：舞台窗本身非 topmost，面板浮在其上
    m_view->setFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    m_view->setSource(QUrl(QStringLiteral("qrc:/qml/StageFxPanel.qml")));
    m_view->setTitle(QStringLiteral("暮云 - 视觉控制台"));

    // 初始尺寸：右侧窄面板，高度按屏幕自适应
    int h = 720;
    if (auto *screen = QGuiApplication::primaryScreen())
        h = qMin(720, screen->availableGeometry().height() - 80);
    m_view->resize(340, h);

    connect(m_view, &QWindow::visibleChanged, this,
            &StageFxController::visibleChanged);
}

void StageFxController::positionNearWindow()
{
    if (!m_view) return;
    // 主屏可用区右上角内缩（舞台窗覆盖主窗，通常即全屏舞台的右侧）
    if (auto *screen = QGuiApplication::primaryScreen()) {
        const QRect geo = screen->availableGeometry();
        m_view->setX(geo.right() - m_view->width() - 24);
        m_view->setY(geo.top() + 24);
        if (qEnvironmentVariableIsSet("MUYUN_STAGE_DEBUG"))
            fprintf(stderr, "[stage-fx] geo=%d,%d,%dx%d view=%dx%d pos=%d,%d\n",
                    geo.x(), geo.y(), geo.width(), geo.height(),
                    m_view->width(), m_view->height(), m_view->x(), m_view->y());
    }
}

bool StageFxController::isVisible() const
{
    return m_view && m_view->isVisible();
}

void StageFxController::show()
{
    ensureView();
    positionNearWindow();
    m_view->show();
    m_view->raise();
    // 拉取引擎当前 fx 状态回显（舞台 active 才有意义；未连接时桥会静默丢弃）
    if (m_stage && m_stage->active())
        m_stage->requestFxState();
    emit visibleChanged();
}

void StageFxController::hide()
{
    if (m_view) {
        m_view->hide();
        emit visibleChanged();
    }
}

void StageFxController::toggle()
{
    if (isVisible()) hide();
    else show();
}

} // namespace Muyun
