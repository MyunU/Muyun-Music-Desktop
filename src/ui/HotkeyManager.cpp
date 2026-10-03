#include "HotkeyManager.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QWindow>
#include <QTimer>
#include <QKeyEvent>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#ifdef MUYUN_SELFTES
#include <cstdio>
#define HKLOG(...) do { fprintf(stderr, "[hk] " __VA_ARGS__); fflush(stderr); } while (0)
#else
#define HKLOG(...) do { } while (0)
#endif

namespace Muyun {

// 热键 id（MOD_NOREPEAT：按住不放不连发）
static constexpr int kF11Id = 0xF11;
static constexpr int kEscId = 0xE5C;
#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

HotkeyManager::HotkeyManager(QObject *parent) : QObject(parent)
{
    qApp->installNativeEventFilter(this);
    // Qt 内兜底过滤器：全局热键注册丢了/被别的程序抢了 F11 时，只要键进到本应用仍能切全屏
    qApp->installEventFilter(this);
    m_toggleClock.start();
    // Esc 只在"播放页/舞台开着 且 前台是我们自己的窗"时占着，300ms 复查一次前台归属
    m_escTimer = new QTimer(this);
    m_escTimer->setInterval(300);
    connect(m_escTimer, &QTimer::timeout, this, [this]() { syncEsc(); });
}

HotkeyManager::~HotkeyManager()
{
    setEnabled(false);
    m_escWanted = false;
    setEscEnabled(false);
}

void HotkeyManager::attach(QWindow *window)
{
    m_window = window;
    if (!m_window) return;

    // 应用级激活状态（不是主窗 activeChanged！）：同进程弹层/兄弟窗抢焦点时应用仍 active，
    // 热键保持注册；只有整个应用退到后台才注销，避免抢其他程序的 F11。
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this]() {
        m_appActive = QGuiApplication::applicationState() != Qt::ApplicationInactive;
        HKLOG("appState 变化 → active=%d\n", int(m_appActive));
        refresh();
    });
    m_appActive = QGuiApplication::applicationState() != Qt::ApplicationInactive;

    // 可见性变化时干两件事：
    //  ① 维护"全屏意图位" m_wantFull：切全屏这件事有两个入口（系统热键、QML 按钮/Ctrl+Shift+F），
    //     只拿瞬时 visibility 判断"现在是不是全屏"，遇到中间态（切换过程中被最小化/恢复、
    //     舞台置顶窗插队）就会判反，表现为"全屏里再按 F11 没反应"。
    //  ② 延后一拍把热键注册搬到新的 HWND（Qt 进/出全屏可能重建原生窗口，旧注册随之作废）。
    connect(window, &QWindow::visibilityChanged, this, [this](QWindow::Visibility v) {
        // 只认"确定在全屏/确定回到窗口"的态；最小化等中间态不动意图位（恢复后还会再来信号）
        if (v == QWindow::FullScreen) m_wantFull = true;
        else if (v == QWindow::Windowed || v == QWindow::Maximized) m_wantFull = false;
        HKLOG("visibility=%d wantFull=%d\n", int(v), int(m_wantFull));
        QTimer::singleShot(0, this, [this]() { syncHwnd(); refresh(); });
    });

    refresh();

#ifdef Q_OS_WIN
#ifdef MUYUN_SELFTES
    // 自检：MUYUN_TEST_F11=秒数 → 定时 PostMessage(WM_HOTKEY)，验证 过滤器→全屏 链路
    const QByteArray t = qgetenv("MUYUN_TEST_F11");
    if (!t.isEmpty()) {
        int sec = t.toInt(); if (sec < 1) sec = 3;
        QTimer::singleShot(sec * 1000, this, [this]() {
            if (!m_window) return;
            HWND hwnd = reinterpret_cast<HWND>(m_window->winId());
            PostMessage(hwnd, WM_HOTKEY, kF11Id, MAKELPARAM(MOD_NOREPEAT, VK_F11));
        });
    }
#endif // MUYUN_SELFTES
#endif
}

void HotkeyManager::setKeepActive(bool on)
{
    if (m_keepActive == on) return;
    m_keepActive = on;
    HKLOG("keepActive=%d\n", int(on));
    refresh();
}

void HotkeyManager::refresh()
{
    // 窗口全屏期间**绝不注销**：这时主窗占满整屏，焦点很容易被输入法/舞台遗留子进程/系统
    // 弹窗叼走，applicationState 会误判成 Inactive——若照"失焦即注销"，用户就成了
    // "进得去全屏、按 F11 出不来"（用户两轮实测同一个症状）。窗口化时仍按老规矩让出 F11。
    setEnabled(m_appActive || m_keepActive || m_wantFull);
}

#ifdef Q_OS_WIN
/// 系统热键的注册挂在 HWND 上：HWND 一销毁（Qt 重建原生窗口）注册即被系统自动注销。
/// 这里比对 winId()，发现换了句柄就清掉"已注册"状态，交给随后的 setEnabled 重注册。
void HotkeyManager::syncHwnd()
{
    if (!m_window) return;
    const quintptr cur = reinterpret_cast<quintptr>(m_window->winId());
    if (!cur || cur == m_hwnd) return;
    HKLOG("HWND 变化 %p → %p，重定位热键注册\n", reinterpret_cast<void *>(m_hwnd),
          reinterpret_cast<void *>(cur));
    if (m_registered && m_hwnd)
        UnregisterHotKey(reinterpret_cast<HWND>(m_hwnd), kF11Id);   // 旧句柄多半已死，失败无所谓
    m_registered = false;
    if (m_escRegistered && m_hwnd)
        UnregisterHotKey(reinterpret_cast<HWND>(m_hwnd), kEscId);
    m_escRegistered = false;
    m_hwnd = cur;
}
#else
void HotkeyManager::syncHwnd() {}
#endif

void HotkeyManager::setForegroundProbe(std::function<bool()> probe)
{
    m_foregroundOk = std::move(probe);
}

void HotkeyManager::setEscGuard(bool on)
{
    if (m_escWanted == on) return;
    m_escWanted = on;
    HKLOG("EscGuard=%d\n", int(on));
    if (on) {
        if (!m_escTimer->isActive()) m_escTimer->start();
    } else {
        m_escTimer->stop();
    }
    syncEsc();
}

void HotkeyManager::syncEsc()
{
#ifdef Q_OS_WIN
    // 只有前台确实是我们自己的窗（主窗/舞台壳/其子窗）才占着 Esc，切到别的应用立刻让开
    const bool ours = !m_foregroundOk || m_foregroundOk();
    HKLOG("syncEsc: wanted=%d 前台归属判据=%d → ours=%d (已注册=%d)\n",
          int(m_escWanted), m_foregroundOk ? 1 : 0, int(ours), int(m_escRegistered));
    setEscEnabled(m_escWanted && ours);
#endif
}

void HotkeyManager::setEscEnabled(bool on)
{
#ifdef Q_OS_WIN
    syncHwnd();
    if (!m_hwnd) return;
    if (on == m_escRegistered) return;
    if (on) {
        const BOOL ok = RegisterHotKey(reinterpret_cast<HWND>(m_hwnd), kEscId,
                                       MOD_NOREPEAT, VK_ESCAPE);
        m_escRegistered = ok != FALSE;
        HKLOG("RegisterHotKey(Esc) hwnd=%p ok=%d err=%lu\n",
              reinterpret_cast<void *>(m_hwnd), int(ok), ok ? 0UL : GetLastError());
    } else {
        UnregisterHotKey(reinterpret_cast<HWND>(m_hwnd), kEscId);
        m_escRegistered = false;
    }
#endif
}

void HotkeyManager::setEnabled(bool on)
{
#ifdef Q_OS_WIN
    syncHwnd();
    if (!m_hwnd) return;
    if (on == m_registered) return;
    if (on) {
        // 注册失败（F11 被别的程序全局占用）→ 静默回退（Ctrl+Shift+F / 标题栏按钮仍可用）
        const BOOL ok = RegisterHotKey(reinterpret_cast<HWND>(m_hwnd), kF11Id,
                                       MOD_NOREPEAT, VK_F11);
        m_registered = ok != FALSE;
        HKLOG("RegisterHotKey hwnd=%p ok=%d err=%lu appActive=%d keep=%d\n",
              reinterpret_cast<void *>(m_hwnd), int(ok), ok ? 0UL : GetLastError(),
              int(m_appActive), int(m_keepActive));
    } else {
        UnregisterHotKey(reinterpret_cast<HWND>(m_hwnd), kF11Id);
        m_registered = false;
        HKLOG("UnregisterHotKey hwnd=%p\n", reinterpret_cast<void *>(m_hwnd));
    }
#else
    Q_UNUSED(on)
#endif
}

/// 两条来路（系统热键 / Qt 内按键兜底）共用；250ms 节流，防互撞双切与长按连发
void HotkeyManager::toggleFullScreen()
{
    if (!m_window) return;
    const qint64 now = m_toggleClock.elapsed();
    if (now - m_lastToggle < 250) return;
    m_lastToggle = now;

    const bool fsNow = m_wantFull || m_window->visibility() == QWindow::FullScreen;
    HKLOG("切全屏：当前全屏=%d（vis=%d want=%d）→ 目标=%s\n", int(fsNow),
          int(m_window->visibility()), int(m_wantFull), fsNow ? "窗口" : "全屏");
    if (fsNow) {
        m_wantFull = false;
        m_window->showNormal();
    } else {
        m_wantFull = true;
        m_window->showFullScreen();
    }
    // 切全屏很可能换 HWND：立刻确认注册还挂在当前句柄上（否则下一次 F11 就丢了）
    QTimer::singleShot(0, this, [this]() { syncHwnd(); refresh(); });
}

bool HotkeyManager::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *)
{
#ifdef Q_OS_WIN
    if (eventType != "windows_generic_MSG") return false;
    const MSG *m = static_cast<const MSG *>(message);
    if (m->message == WM_HOTKEY && m->wParam == kF11Id) {
        // 系统级热键先于输入法分发 → 直接在 C++ 切换主窗全屏
        HKLOG("收到 WM_HOTKEY(F11)（hwnd=%p）\n", reinterpret_cast<void *>(m->hwnd));
        toggleFullScreen();
        return true;   // 已消费
    }
    if (m->message == WM_HOTKEY && m->wParam == kEscId) {
        // 播放页/舞台开着时的 Esc：交给 main.cpp 走阶梯（页面裁判）。
        // 注册期间系统不会再投 WM_KEYDOWN，所以 QML Shortcut/页面 keydown 不会重复触发。
        HKLOG("收到 WM_HOTKEY(Esc)\n");
        emit escapeRequested();
        return true;
    }
#else
    Q_UNUSED(eventType) Q_UNUSED(message)
#endif
    return false;
}

bool HotkeyManager::eventFilter(QObject *obj, QEvent *ev)
{
    if (ev->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(ev);
        if (ke->key() == Qt::Key_F11 && !ke->isAutoRepeat()
            && !(ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
            HKLOG("兜底路径：F11 键进到 Qt 内（registered=%d）\n", int(m_registered));
            toggleFullScreen();
            return true;      // 已消费，避免同时被别的 F11 处理逻辑吃一次
        }
    }
    return QObject::eventFilter(obj, ev);
}

#ifdef MUYUN_SELFTES
/// 自检用：从进程内注入一次**真实** F11（走 RegisterHotKey 通路，不是 PostMessage 作弊）
void HotkeyManager::injectF11()
{
    HKLOG("注入真实 F11：registered=%d hwnd=%p vis=%d want=%d appActive=%d\n",
          int(m_registered), reinterpret_cast<void *>(m_hwnd),
          m_window ? int(m_window->visibility()) : -1, int(m_wantFull), int(m_appActive));
    fflush(stdout);
#ifdef Q_OS_WIN
    keybd_event(VK_F11, 0, 0, 0);
    Sleep(30);
    keybd_event(VK_F11, 0, KEYEVENTF_KEYUP, 0);
#endif
}

/// 自检用：注入真实 Esc（走 Qt 键队列 → Main.qml 的 ESC 阶梯，不是热键）
void HotkeyManager::injectEsc()
{
    HKLOG("注入真实 Esc：vis=%d want=%d\n",
          m_window ? int(m_window->visibility()) : -1, int(m_wantFull));
    fflush(stdout);
#ifdef Q_OS_WIN
    keybd_event(VK_ESCAPE, 0, 0, 0);
    Sleep(30);
    keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
#endif
}

bool HotkeyManager::isRegistered() const { return m_registered; }

bool HotkeyManager::escRegistered() const { return m_escRegistered; }

bool HotkeyManager::isFullScreen() const
{
    return m_wantFull || (m_window && m_window->visibility() == QWindow::FullScreen);
}
#endif // MUYUN_SELFTES

} // namespace Muyun
