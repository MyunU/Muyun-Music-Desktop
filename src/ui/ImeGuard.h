#pragma once

#include <QObject>
#include <QPointer>

class QWindow;
class QTimer;

namespace Muyun {

/**
 * @brief 窗口输入法上下文守护（修「输入法只能在输入框里切中英文」）
 *
 * 真因在 Qt 的 Windows 平台插件里：`QWindowsInputContext::updateEnabled()` 会问焦点对象
 * 要不要输入法（`Qt::ImEnabled`），答案是"不要"时就调
 * `ImmAssociateContext(hwnd, nullptr)` **把整个窗口的 IME 上下文摘掉**；
 * 只有焦点落进 TextInput/TextField 时才用 `ImmAssociateContextEx(..., IACE_DEFAULT)` 接回来。
 * 而 Ctrl+Space / Shift 这类中英切换键是**由 IME 自己**在消息循环里处理的——
 * 窗口没有 IME 上下文，键就到不了 IME，于是用户看到的就是
 * "焦点不在输入框时切不了中英文，必须先点进某个输入框"。
 * Qt Quick 里几乎所有点击都会把 activeFocus 给一个不接受输入法的普通 Item，
 * 所以这个状态是常态，不是偶发。
 *
 * 本类不改 Qt 的判断（那是插件内部），只做一件事：发现自家主窗口的 IME 上下文被摘了，
 * 就用 **Qt 自己恢复文本框时用的同一个调用**（IACE_DEFAULT）把默认上下文接回来。
 * 效果：任何焦点下都能切中英文；输入框里的合成/候选行为完全不变（Qt 给文本框开的
 * 就是这个默认上下文），也不影响用户自己"关闭输入法"的开关状态（那是上下文里的
 * conversion status，不是关联关系）。
 */
class ImeGuard : public QObject
{
    Q_OBJECT
public:
    explicit ImeGuard(QObject *parent = nullptr);

    /// 绑定主窗口（自检/调试也用它取句柄）
    void attach(QWindow *window);

    /// 此刻主窗口是否还挂着 IME 上下文（自检断言用）
    Q_INVOKABLE bool hasImeContext() const;

    /// 立刻检查一次并在被摘掉时接回；返回"这一次是否真的做了恢复"（自检用）
    Q_INVOKABLE bool ensureNow();

    /// 强制重走一次 IME 关联（摘掉再接回默认上下文）。
    /// 供「全屏歌词页关闭后」调用：那期间 Qt 可能摘掉上下文、焦点输入框的输入法
    /// 没被认领 → 等价"点窗外再点回"的激活往返，主动触发重建。
    Q_INVOKABLE void forceRefresh();

    /// 累计"发现上下文被摘掉并接回来"的次数（自检拿它证明断言不是空转）
    Q_INVOKABLE int restoreCount() const { return m_restores; }

private:
    void tick();

    QPointer<QWindow> m_window;
    QTimer *m_timer = nullptr;
    int m_restores = 0;
};

} // namespace Muyun
