#pragma once

#include <QObject>

class QQuickView;
class QQmlEngine;
class QTimer;

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
    /// 置顶态下鼠标是否悬停在歌词窗矩形内（穿透窗收不到 Qt 鼠标事件，靠 C++ 全局光标轮询得出；#14）
    Q_PROPERTY(bool pinnedHover READ pinnedHover NOTIFY pinnedHoverChanged)

public:
    DesktopLyricsController(PlayerController *player, QQmlEngine *engine,
                            QObject *parent = nullptr);

    bool isVisible() const;

    QString color() const { return m_color; }
    void setColor(const QString &c);
    int sizePx() const { return m_sizePx; }
    void setSizePx(int px);
    bool isPinned() const { return m_pinned; }
    bool pinnedHover() const { return m_pinnedHover; }

    Q_INVOKABLE void toggle();
    Q_INVOKABLE void show();
    Q_INVOKABLE void hide();
    /// 置顶 = 点击穿透（不可选中，显示在最上层）；取消置顶回到编辑态
    Q_INVOKABLE void setPinned(bool on);
    /// 悬停「取消置顶」钮时临时让窗可点 / 移开后恢复穿透（不重建窗口、保持最顶层）
    Q_INVOKABLE void setInteractiveForUnpin(bool on);
    /// 自检用：歌词窗平台句柄 + 全局中心点（判断光标是否落在窗内、检查穿透扩展样式）
    quintptr winIdForTest() const;
    QPoint centerGlobalForTest() const;
    /// 自检用：「取消置顶」按钮的几何中心（Qt 全局逻辑坐标）；找不到返回无效点
    QPointF unpinButtonGlobalForTest() const;
    /// 把桌面歌词窗抬到前台（自检用：模拟"焦点被自家另一个窗叼走"，#19 断言①）
    Q_INVOKABLE void requestActivate();
    /// 按当前字号算出能同时放下"原文 + 译文"的窗高
    /// （窗高写死 100px 时，大字号两行会顶到边界把译文裁掉——"只显示原文"的成因之一）
    int neededHeight() const;

signals:
    void visibleChanged();
    void styleChanged();
    void pinnedChanged();
    void pinnedHoverChanged();

private:
    void ensureView();
    Qt::WindowFlags windowFlags() const;
    void loadStyle();
    /// 全局光标轮询：置顶态下判断鼠标是否落在歌词窗矩形内（穿透窗收不到 Qt 鼠标事件）
    void pollHover();
    /// 直管 Win32 的 WS_EX_TRANSPARENT（点击穿透），只改扩展样式、不重建原生窗口
    void applyClickThrough(bool through);

    PlayerController *m_player = nullptr;
    QQmlEngine *m_engine = nullptr;
    QQuickView *m_view = nullptr;
    QTimer *m_hoverTimer = nullptr;

    QString m_color;
    int m_sizePx = 28;
    bool m_pinned = false;
    bool m_pinnedHover = false;
};

} // namespace Muyun
