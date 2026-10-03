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

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    QWindow *m_window = nullptr;
};

} // namespace Muyun
