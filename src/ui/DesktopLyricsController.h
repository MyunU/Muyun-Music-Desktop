#pragma once

#include <QObject>

class QQuickView;
class QQmlEngine;

namespace Muyun {

class PlayerController;

/**
 * @brief 桌面歌词独立窗口控制器
 *
 * 用 QQuickView（本身即一个顶层 Window）承载桌面歌词 QML，
 * 首次切换时惰性创建，show()/hide() 稳定可靠（避免 QML 嵌套 Window
 * 隐藏后再置 visible=true 无法重新映射的问题）。
 * 复用主 engine，直接共享 player / theme / settings contextProperty。
 *
 * 三态循环（mic 按钮）：隐藏 → 编辑态（可拖动/右键设置）→ 置顶态
 * （点击穿透、永远最上层）→ 编辑态。颜色/字号持久化到 DocumentStore。
 */
class DesktopLyricsController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool visible READ isVisible NOTIFY visibleChanged)
    Q_PROPERTY(QString color READ color WRITE setColor NOTIFY styleChanged)
    Q_PROPERTY(int sizePx READ sizePx WRITE setSizePx NOTIFY styleChanged)
    Q_PROPERTY(bool pinned READ isPinned NOTIFY pinnedChanged)

public:
    DesktopLyricsController(PlayerController *player, QQmlEngine *engine,
                            QObject *parent = nullptr);

    bool isVisible() const;

    QString color() const { return m_color; }
    void setColor(const QString &c);
    int sizePx() const { return m_sizePx; }
    void setSizePx(int px);
    bool isPinned() const { return m_pinned; }

    Q_INVOKABLE void toggle();
    Q_INVOKABLE void show();
    Q_INVOKABLE void hide();
    /// 置顶 = 点击穿透（不可选中，显示在最上层）；取消置顶回到编辑态
    Q_INVOKABLE void setPinned(bool on);
    /// 按当前字号算出能同时放下"原文 + 译文"的窗高
    /// （窗高写死 100px 时，大字号两行会顶到边界把译文裁掉——"只显示原文"的成因之一）
    int neededHeight() const;

signals:
    void visibleChanged();
    void styleChanged();
    void pinnedChanged();

private:
    void ensureView();
    Qt::WindowFlags windowFlags() const;
    void loadStyle();

    PlayerController *m_player = nullptr;
    QQmlEngine *m_engine = nullptr;
    QQuickView *m_view = nullptr;

    QString m_color;
    int m_sizePx = 28;
    bool m_pinned = false;
};

} // namespace Muyun
