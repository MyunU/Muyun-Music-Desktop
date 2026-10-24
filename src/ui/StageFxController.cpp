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
    m_view->setTitle(QStringLiteral("暮云 - 视觉控制台"));

    // 初始尺寸：右侧面板，高度按屏幕自适应
    // #5 BUG5: 从 340 加宽到 440，之前 340 太窄 —— 用户报"滚放排列右边被吞"
    // 且导入图片按钮的 hint 文字("可多选·按序列匹配")与主按钮文字挤压重叠。
    // 440 给滚动序列项留出 delBtn(22) + text elide 的空间，也让按钮行不再挤压。
    int h = 860;
    if (auto *screen = QGuiApplication::primaryScreen())
        h = qMin(860, screen->availableGeometry().height() - 80);
    m_view->resize(440, h);

    connect(m_view, &QWindow::visibleChanged, this,
            &StageFxController::visibleChanged);

    // 等 QQuickView::statusChanged → Ready 再 position/raise/fxState，否则 show 后
    // 窗口可能还停在初始位置或大小。Error 时打印 stderr 便于定位。
    connect(m_view, &QQuickView::statusChanged, this, [this](QQuickView::Status s) {
        if (s == QQuickView::Ready && m_pendingShow) {
            m_pendingShow = false;
            doShowWindow();
        } else if (s == QQuickView::Error) {
            fprintf(stderr, "[stage-fx] QML load FAILED url=%s\n",
                    qPrintable(m_view->source().toString()));
        }
    });

    // 延后 setSource，让它和后续 show 在同一 event loop tick 之外的异步链
    m_view->setSource(QUrl(QStringLiteral("qrc:/qml/StageFxPanel.qml")));
}

// 把窗口显示到屏幕右上角（真正执行 show/raise/position 的辅助函数）
void StageFxController::doShowWindow()
{
    if (!m_view) return;
    positionNearWindow();
    m_view->show();
    m_view->raise();
    if (m_stage && m_stage->active())
        m_stage->requestFxState();
    emit visibleChanged();
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
    // QQuickView::setSource 是异步的；在 Ready 之前 show() 会显示一个空白/透明窗口
    // （用户点 fx-fab 看不到内容，就是这种情况）。
    // 但即使 QML 有 warning（如 Drag.onFinished），QQuickView 也不会进入 Ready，
    // 所以必须立即 show()——窗口会先显示，QML 加载完后内容自动出现。
    // statusChanged → Ready 时补做 position/raise/fxState（首次 show 时 view 可能还没定位好）。
    doShowWindow();
    if (m_view->status() != QQuickView::Ready) {
        m_pendingShow = true;
    }
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
    // 已显示 → 隐藏；挂起显示中（view 未 Ready）→ 取消挂起；否则 → 请求显示
    if (isVisible()) {
        hide();
    } else if (m_pendingShow) {
        m_pendingShow = false;
        emit visibleChanged();
    } else {
        show();
    }
}

} // namespace Muyun
