#pragma once

#include <QObject>

class QQuickView;
class QQmlEngine;

namespace Muyun {

class StageBridge;

/**
 * @brief 舞台 fx 视觉控制台面板控制器
 *
 * 用 QQuickView 承载一个独立置顶小窗（右侧浮层），
 * 提供预设/滑块/开关，经 StageBridge::fx* 通道透传给舞台页面的
 * mineradio 引擎（setFxValue/toggleFx/setPreset/resetFx）。
 *
 * 为什么不是主窗口里的 Popup：舞台窗（MuyunStage.exe）覆盖在主窗之上，
 * 主窗弹层会被舞台遮挡。面板自置 topmost 浮在舞台上，边调参边看效果。
 * 桌面歌词同款模式（QQuickView 复用主 engine → 共享 stage/theme contextProperty）。
 */
class StageFxController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool visible READ isVisible NOTIFY visibleChanged)

public:
    StageFxController(StageBridge *stage, QQmlEngine *engine,
                      QObject *parent = nullptr);

    bool isVisible() const;

    Q_INVOKABLE void toggle();
    Q_INVOKABLE void show();
    Q_INVOKABLE void hide();

signals:
    void visibleChanged();

private:
    void ensureView();
    void positionNearWindow();

    StageBridge *m_stage = nullptr;
    QQmlEngine *m_engine = nullptr;
    QQuickView *m_view = nullptr;
};

} // namespace Muyun
