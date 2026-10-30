#pragma once

#include <QObject>
#include <QAbstractNativeEventFilter>
#include <QColor>
#include <QRect>

class QWindow;

namespace Muyun {

/**
 * @brief Windows 11 亚克力（Acrylic）窗口背景
 *
 * 让主窗变成真正的"毛玻璃"：内容画在半透明表面上，窗口背后由 DWM 做高斯模糊，
 * 桌面/其它窗口的颜色透进来。实现走官方 DWM 通道 DWMWA_SYSTEMBACKDROP_TYPE(=38)，
 * 这是 Win11 22H2+ 的正式接口（比老的 SetWindowCompositionAttribute 稳、无掉帧）。
 *
 * 实测要点（E:\code\Music\_scratch\micatest 验证过）：
 *  - 窗口必须有 alpha buffer（QML Window 里 color: "transparent" 即可），
 *    否则 Qt 用不透明 backing store，模糊层根本透不上来。
 *  - **必须把窗口样式补回标准边框**（WS_POPUP → WS_OVERLAPPEDWINDOW）。纯 WS_POPUP 的
 *    无边框窗请求 backdrop 时 DWM 只给一层暗色（实测对比度只掉 25%，不是模糊）；
 *    补齐边框后模糊才真正生效。边框本身被 WM_NCCALCSIZE 归零吃掉，视觉上仍是全透明。
 *  - Mica(=2) 在本机（Win11 26300）实测渲染成纯黑，不可用 → 只用 Acrylic(=3)。
 *  - 角落圆角由 DWMWA_WINDOW_CORNER_PREFERENCE(=33)=2 交给系统画（配合 DWM 阴影）。
 *
 * 关闭时把样式/边框/backdrop 全部还原，窗口回到不透明观感。
 */
class Backdrop : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(bool available READ available CONSTANT)

public:
    explicit Backdrop(QObject *parent = nullptr);
    ~Backdrop() override;

    /// 绑定主窗（可重复调用；HWND 变了会自动重挂）
    Q_INVOKABLE void attach(QWindow *window);

    /// 重新挂一次（窗口可见性/全屏切换后样式位被 Qt 改写时用）
    Q_INVOKABLE void refresh();

    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);

    /// 当前系统是否支持 DWM 亚克力（Win11 22H2+）
    bool available() const;

    /// 跟随主题深浅色（影响系统绘制的阴影/圆角取色）
    Q_INVOKABLE void setDark(bool dark);

    /// 诊断：把本进程当前所有顶层/子窗口（hwnd、类名、标题、样式、矩形、可见性）打到日志。
    /// 只用于排查"窗口边缘多出一圈框"来自哪个窗口；MUYUN_UI_DUMP=1 时由 main.cpp 定时调用。
    static void dumpProcessWindows(const char *tag);

    /// 玻璃是否正在接管窗口的非客户区计算。FramelessWindow 的 WM_NCCALCSIZE 分支据此让路：
    /// 两处都想改 rgrc[0]，谁后装谁先收到消息 → 实测是 FramelessWindow 抢先按"缩进一圈边框"
    /// 算（最大化态），把亚克力该用的工作区矩形顶掉，屏幕四边就留出一条边带。
    static bool glassActive();

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

signals:
    void enabledChanged();

private:
    void applyToWindow();          ///< 按 m_enabled 实际改 HWND
    void resetWindowStyles();      ///< 还原 WS_POPUP / 去掉 backdrop

    QWindow *m_window = nullptr;
    bool m_enabled = false;
    bool m_applied = false;        ///< 当前 HWND 上是否已生效（WM_NCCALCSIZE 归零的门闩）
    bool m_dark = true;

    /// WM_WINDOWPOSCHANGING 里记下的"本次将要变成的窗口矩形"。
    /// **只用于诊断打印**（MUYUN_GLASS_DEBUG 里对照 propos）：非客户区计算已改成直接用
    /// 系统给的 proposed（它本身就等于本次将生效的窗口矩形），不再依赖这份快照 ——
    /// 快照在最大化/全屏往返后可能残留旧坐标，曾把客户区缩到旧尺寸，窗口边上留一圈带子。
    QRect m_pendingRect;
    /// 贴边态（最大化/全屏）下的系统描边/圆角是否已关掉；只在状态切换时下 DWM 指令。
    bool m_edgeHints = false;
    static bool s_glassActive;
};

} // namespace Muyun
