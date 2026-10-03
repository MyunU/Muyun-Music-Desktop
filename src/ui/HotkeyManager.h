#pragma once

#include <QObject>
#include <QPointer>
#include <QAbstractNativeEventFilter>
#include <QElapsedTimer>
#include <functional>

class QWindow;
class QTimer;

namespace Muyun {

/**
 * @brief F11 全局热键守护
 *
 * QML Shortcut 依赖按键进入 Qt 窗口队列，输入法/微信等低级钩子可能把 F11
 * 吞掉（用户实测 F11 无效的真凶）。改用 Win32 RegisterHotKey：系统层先于
 * 输入法分发，稳。为避免抢其他程序的热键，只在应用处于前台时注册，退到
 * 后台注销。
 *
 * 关键：注册条件用**应用级**激活状态（applicationStateChanged），而非主窗
 * activeChanged——否则歌词页内的弹层/兄弟顶层窗抢走焦点时主窗 isActive()
 * 变 false 会误注销，导致全屏歌词界面 F11 失灵。另提供 setKeepActive()
 * 供 mineradio 舞台（独立进程窗）激活期间强制保活。
 *
 * 两条硬教训（都修过，别再回退）：
 *  1) RegisterHotKey 的注册**随 HWND 一起死**。Qt 在进/出全屏（以及换屏、
 *     改 flags、DPI 变化）时会重建原生窗口，句柄一变，系统自动注销热键。
 *     若只按"该不该注册"的布尔缓存判断（旧实现 if (on == m_registered) return），
 *     就会出现「窗口化按 F11 能进全屏，全屏里再按 F11 毫无反应」——
 *     因为进全屏那次用的是旧句柄上的注册，出全屏的键再也收不到。
 *     故每次刷新都比对 winId()，句柄变了就在**新句柄**上重注册。
 *  2) 全局热键可能被别的程序先占用（RegisterHotKey 直接失败），或某条路径上
 *     注册仍然丢了。再兜一层 Qt 内事件过滤器：F11 键进到来就直接切全屏。
 *     两路共用一个带节流的 toggle，绝不双切（全局热键命中时系统不会再把
 *     WM_KEYDOWN 投递给焦点窗口，天然互斥；节流只是保险）。
 */
class HotkeyManager : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
public:
    explicit HotkeyManager(QObject *parent = nullptr);
    ~HotkeyManager() override;

    /// 绑定主窗口：热键消息投递其 HWND，并按应用激活状态动态注册/注销
    void attach(QWindow *window);

    /// 舞台等独立进程窗激活期间保活（true 时即便应用失焦也保持注册）
    Q_INVOKABLE void setKeepActive(bool on);

    /**
     * @brief 播放页/舞台开着时把 **Esc 也交给系统热键通道**
     *
     * 舞台是独立进程的窗，WebView2 一点就把系统激活叼走：主窗的 QML Shortcut 收不到键，
     * 页面 document 也可能没焦点（焦点停在壳窗/WebView2 子窗上）→ 用户按 Esc 谁都没接到。
     * F11 早就走系统热键所以"全程有效"，Esc 用同一招才对。
     * 只在"当前系统前台确实是我们自己的窗"（主窗/舞台壳/其子窗/本进程窗）时才真的注册，
     * 切到别的应用就自动让开——不抢人家的 Esc。
     */
    Q_INVOKABLE void setEscGuard(bool on);
    /// 注入"前台归属"判据（main.cpp 把 StageBridge::ownsSystemForeground 交进来）
    void setForegroundProbe(std::function<bool()> probe);

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;
    /// Qt 内兜底：F11 键（未被别的程序吞走时）直接切全屏，与热键同语义
    bool eventFilter(QObject *obj, QEvent *ev) override;

signals:
    /// 系统热键通道收到的 Esc（注册期间 Qt 键队列里不会再有这条键，不会双触发）
    void escapeRequested();

public:     // 注意：signals: 之后的成员默认是信号，必须显式写 public:（否则 moc 会替方法生成信号实现→重复定义）
#ifdef MUYUN_SELFTES
    /// 自检专用（发布版 SELFTES=OFF 时根本不存在）
    Q_INVOKABLE void injectF11();          ///< 进程内注入一次真实 F11（走系统热键通路）
    Q_INVOKABLE void injectEsc();          ///< 进程内注入一次真实 Esc（走 Qt 键队列→ESC 阶梯）
    Q_INVOKABLE bool escRegistered() const; ///< 此刻是否持有 Esc 系统热键注册
    Q_INVOKABLE bool isRegistered() const; ///< 此刻是否真持有 F11 注册
    Q_INVOKABLE bool isFullScreen() const; ///< 全屏意图位
#endif

private:
    void setEnabled(bool on);
    void refresh();                       ///< 依据 (appActive || keepActive) 重算注册
    void syncHwnd();                      ///< HWND 被 Qt 重建后把注册搬过去
    void toggleFullScreen();              ///< 两条路径共用的全屏切换（带节流）
    void syncEsc();                       ///< 按"前台是不是我们的窗"决定 Esc 注册与否
    void setEscEnabled(bool on);

    QPointer<QWindow> m_window;
    quintptr m_hwnd = 0;                  ///< 当前注册所在的原生窗口句柄（0=未注册）
    bool m_registered = false;
    bool m_appActive = false;
    bool m_keepActive = false;
    bool m_wantFull = false;              ///< 全屏意图位（visibility() 被遮挡时会谎报）
    QElapsedTimer m_toggleClock;          ///< 节流计时
    qint64 m_lastToggle = 0;              ///< 上次切全屏的时刻（ms）
    std::function<bool()> m_foregroundOk; ///< 前台归属判据
    QTimer *m_escTimer = nullptr;         ///< 300ms 复查前台（Esc 只在需要时占着）
    bool m_escWanted = false;
    bool m_escRegistered = false;
};

} // namespace Muyun
