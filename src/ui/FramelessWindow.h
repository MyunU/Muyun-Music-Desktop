#pragma once

#include <QObject>
#include <QAbstractNativeEventFilter>

class QWindow;

namespace Muyun {

/**
 * @brief Windows 无边框窗口的原生缩放/最大化修复
 *
 * Qt::FramelessWindowHint 的窗口没有 WS_THICKFRAME，只能靠 QML 边缘 MouseArea
 * 调 startSystemResize —— 拖动抖动严重（Qt 已知问题）；且 showMaximized 时
 * frameless 窗口几何会向屏幕外溢出约一个边框宽度（顶部"缺一行"）。
 *
 * 本过滤器改用 Windows 标准做法（与 WPF/WinUI 无边框方案一致）：
 *  - attach 时给 HWND 补回 WS_THICKFRAME|WS_CAPTION 等样式；
 *  - WM_NCCALCSIZE 把非客户区归零（视觉上仍无边框），最大化时按边框宽度内缩
 *    → 修好"顶部缺一行"；
 *  - WM_NCHITTEST 对边缘/四角返回 HTLEFT 等 → 系统原生拖拽缩放，零抖动。
 * 命中边缘时消费消息；其余（含标题栏 startSystemMove）交回 Qt。
 */
class FramelessWindow : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
public:
    explicit FramelessWindow(QObject *parent = nullptr);
    ~FramelessWindow() override;

    void attach(QWindow *window);

    /// 绑定"样式位同步"用的窗口句柄。刻意与 attach 分开：
    /// attach 会补 WS_THICKFRAME 并激活 WM_NCCALCSIZE/WM_NCHITTEST（原生缩放会抖动，
    /// GUI 主路径已弃用），而 setMaximizedStyle 只想动 WS_MAXIMIZE 一个位。
    void bindMaxWindow(QWindow *window);
    /// 手动最大化时把 WS_MAXIMIZE 样式位补上/摘掉（第三方任务栏工具如 TranslucentTB
    /// 靠 IsZoomed() 认"最大化"；我们手动改几何从不带这个位 → 工具认不出）
    Q_INVOKABLE void setMaximizedStyle(bool on);

    /// 边缘拖拽缩放的**原子**几何写入：一次 setGeometry(x,y,w,h) → 平台层一次
    /// SetWindowPos（移动+缩放同一拍完成）。
    /// QML 里分别赋 width 再赋 x（或 height 再赋 y）看着等价，实际是两次独立的
    /// SetWindowPos：第一次先把窗口向右/向下撑开、第二次才把左/上边挪过去 ——
    /// 一帧内两次几何跳变，拖左/上边框时整界面就是肉眼可见的"激烈抖动"（右/下只改
    /// 一个维度所以不抖，正好对上用户报的症状）。必须一次写全。
    Q_INVOKABLE void applyWindowGeometry(int x, int y, int w, int h);

    /// 方案②：走 Windows 原生最大化（ShowWindow SW_MAXIMIZE），而不是手动赋几何。
    /// 好处：IsZoomed() 为真、任务栏工具/贴边/最小化后从任务栏恢复都按系统行为走，
    /// 还原目标由系统自己记（QML 不用再存 _rx/_ry/_rw/_rh）。
    /// 副作用处理：无边框 WS_POPUP 窗被 Windows 最大化后会整体下移一个标题栏高度
    /// （实测 1536x816 的窗落在 y=24）→ 这里补一次 SetWindowPos 对齐工作区。
    /// 返回 false 表示没有窗口句柄（非 Windows / 未 bind），QML 侧应退回手动几何方案。
    Q_INVOKABLE bool setNativeMaximized(bool on);
    /// 当前是否处于原生最大化态（IsZoomed）
    Q_INVOKABLE bool isNativeMaximized() const;

    /// 当前鼠标全局位置（逻辑像素，Windows 下直接读系统光标，不依赖窗口自身位置）。
    /// QML 的 mapToGlobal(m.x, m.y) 在窗口被拖动时会把「窗口新位置」和「鼠标旧局部坐标」
    /// 混在一起算出错的全局点，拖左/上边形成正反馈 → 剧烈抖动；这里绕开窗口几何，
    /// 取真实光标，天然稳定。
    Q_INVOKABLE QPointF globalCursorLogical() const;

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    QWindow *m_window = nullptr;      ///< 原生过滤器用（GUI 未 attach，保持 null）
    QWindow *m_maxWindow = nullptr;   ///< 手动最大化样式位用
};

} // namespace Muyun
