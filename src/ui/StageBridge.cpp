#include "StageBridge.h"

#include "ui/PlayerController.h"
#include "ui/SettingsController.h"
#include "core/Types.h"
#include "core/network/HttpClient.h"
#include "core/storage/DocumentStore.h"
#include "core/utils/Crypto.h"

#include <QProcess>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QEvent>
#include <QCoreApplication>
#include <QDebug>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace Muyun {

#ifdef MUYUN_SELFTES
/// 生命周期排障用步印（发布版 SELFTES=OFF 自动消失）：舞台关闭崩溃就靠它定位到行
#define SBSTEP(...) do { fprintf(stderr, "[sb] " __VA_ARGS__); fflush(stderr); } while (0)
#else
#define SBSTEP(...) do { } while (0)
#endif

static QByteArray leUint32(quint32 v) {
    // 4 字节小端长度前缀（与 stage/src/ipc 对齐）
    char b[4] = {
        char(v & 0xFF), char((v >> 8) & 0xFF),
        char((v >> 16) & 0xFF), char((v >> 24) & 0xFF) };
    return QByteArray(b, 4);
}

StageBridge::StageBridge(PlayerController *player, SettingsController *settings,
                         QObject *parent)
    : QObject(parent), m_player(player), m_settings(settings)
{
    m_clockTimer = new QTimer(this);
    m_clockTimer->setInterval(50);
    connect(m_clockTimer, &QTimer::timeout, this, &StageBridge::onClockTick);

    m_boundsTimer = new QTimer(this);
    m_boundsTimer->setInterval(80);
    m_boundsTimer->setSingleShot(true);
    connect(m_boundsTimer, &QTimer::timeout, this, &StageBridge::onBoundsTick);

    m_audioRetryTimer = new QTimer(this);
    m_audioRetryTimer->setInterval(2000);
    connect(m_audioRetryTimer, &QTimer::timeout, this, &StageBridge::tryAttachTrackAudio);

    if (m_player) {
        connect(m_player, &PlayerController::currentSongChanged, this, [this]() {
            if (m_engineReady) pushTrackNow();
        });
        connect(m_player, &PlayerController::lyricChanged, this, [this]() {
            if (m_engineReady) pushLyricsNow();
        });
        connect(m_player, &PlayerController::playModeChanged, this, [this]() {
            if (m_engineReady) pushPlayMode();
        });
        connect(m_player, &PlayerController::isPlayingChanged, this, [this]() {
            if (m_engineReady) onClockTick();   // 立即同步播放态，不等下个 tick
        });
        connect(m_player, &PlayerController::durationChanged, this, [this]() {
            if (m_engineReady) onClockTick();
        });
    }
}

StageBridge::~StageBridge()
{
    if (m_proc && m_proc->state() != QProcess::NotRunning) {
        m_proc->kill();
        // 析构阶段不再 waitForFinished（避免与信号槽重入）；交给 OS 回收
    }
}

void StageBridge::attachWindow(QQuickWindow *window)
{
    m_window = window;
    if (!window) return;
    connect(window, &QWindow::visibilityChanged, this,
            [this](QWindow::Visibility) { m_boundsDirty = true; m_boundsTimer->start(); });
    connect(window, &QWindow::screenChanged, this,
            [this](QScreen *) { m_boundsDirty = true; m_boundsTimer->start(); });
    connect(window, &QWindow::widthChanged, this,
            [this]() { m_boundsDirty = true; m_boundsTimer->start(); });
    connect(window, &QWindow::heightChanged, this,
            [this]() { m_boundsDirty = true; m_boundsTimer->start(); });
    // 最小化/隐藏 → 舞台暂停（失焦不暂停，避免露出变暗主窗）
    connect(window, &QWindow::visibilityChanged, this,
            [this]() { m_boundsDirty = true; m_boundsTimer->start(); pushVisibility(); });
    // 移动（QWindow 无 positionChanged 信号）：用事件过滤器捕捉 Move
    window->installEventFilter(this);
}

bool StageBridge::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_window) {
        const int t = int(event->type());
        // QEvent::Move=83 / Resize=6（用数值避免额外 include，行为等价）
        if (t == 83 || t == 6) {
            m_boundsDirty = true;
            m_boundsTimer->start();
        }
    }
    return QObject::eventFilter(watched, event);
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

void StageBridge::open()
{
    if (m_unavailable) return;
    m_wanted = true;
    applyStage();
}

void StageBridge::close()
{
    m_wanted = false;
    applyStage();
}

// 依据页面意图 + 主窗可见性，决定拉起或杀掉 stage
void StageBridge::applyStage()
{
    bool minimized = false;
    if (m_window) {
        const auto vis = m_window->visibility();
        minimized = (vis == QWindow::Minimized || vis == QWindow::Hidden);
    }
    const bool shouldRun = m_wanted && !minimized;
    const bool running = m_proc && m_proc->state() != QProcess::NotRunning;
    if (qEnvironmentVariableIsSet("MUYUN_STAGE_DEBUG"))
        fprintf(stderr, "[stage-bridge] applyStage wanted=%d run=%d stopping=%d reopen=%d gen=%d\n",
                (int)m_wanted, (int)running, (int)m_stopping, (int)m_reopenPending, m_generation);

    if (shouldRun) {
        if (running) {
            // 已在跑：确保可见 + 同步几何
            if (!m_stopping) {
                sendJson(QJsonObject{{QStringLiteral("c"), QStringLiteral("vis")},
                                     {QStringLiteral("on"), 1}});
                m_boundsDirty = true;
                m_boundsTimer->start();
            } else {
                // 正在关闭中却又要开（快速关→开竞态）：记待重开，
                // 否则 open 意图被吞、进程退干净后不再拉起 → 舞台永久卡死
                m_reopenPending = true;
            }
        } else if (!m_stopping) {
            launch();
        } else {
            // 正在关闭中：等进程退出后由 onProcFinished 重开
            m_reopenPending = true;
        }
    } else if (running) {
        killStage();
    }
}

// 终止进程（不改动 m_wanted）；代际守卫避免与重开竞态
void StageBridge::killStage()
{
    ++m_generation;
    if (!m_proc) return;
    if (m_proc->state() == QProcess::NotRunning) {
        if (m_clockTimer) m_clockTimer->stop();
        if (m_boundsTimer) m_boundsTimer->stop();
        if (m_audioRetryTimer) m_audioRetryTimer->stop();
        m_engineReady = false;
        m_stopping = false;
        return;
    }
    m_stopping = true;
    if (m_clockTimer) m_clockTimer->stop();
    if (m_boundsTimer) m_boundsTimer->stop();
    if (m_audioRetryTimer) m_audioRetryTimer->stop();
    m_engineReady = false;
    sendJson(QJsonObject{{QStringLiteral("c"), QStringLiteral("quit")}});
    m_proc->terminate();
    const int gen = m_generation;
    QTimer::singleShot(1000, this, [this, gen]() {
        if (gen == m_generation && m_proc && m_proc->state() != QProcess::NotRunning)
            m_proc->kill();
    });
}

bool StageBridge::launch()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString exePath = appDir + QStringLiteral("/MuyunStage.exe");
    const QString webDir = appDir + QStringLiteral("/stage/web");
    if (!QFileInfo::exists(exePath) || !QFileInfo::exists(webDir)) {
        qWarning() << "[stage] helper missing:" << exePath << webDir;
        setUnavailable(true);
        return false;
    }

    const QString dataDir = DocumentStore::instance()->rootPath() + QStringLiteral("/stage");
    QDir().mkpath(dataDir);
    QDir().mkpath(webDir + QStringLiteral("/covers"));

    // 事件管道服务端
    // 管道名带启动序号：旧舞台壳被强杀后，其 msedgewebview2 子进程会存活数秒并
    // 继承壳的命名管道句柄——固定名会让新壳 CreateNamedPipe 失败(exit→"卡死"假象)。
    // 每次启动换名彻底避开与垂死句柄的冲突。
    static int s_launchSeq = 0;
    const QString pid = QString::number(QCoreApplication::applicationPid());
    const QString tag = pid + QStringLiteral(".") + QString::number(++s_launchSeq);
    const QString evName = QStringLiteral("MuyunEv.") + tag;
    const QString cmdName = QStringLiteral("MuyunCmd.") + tag;
    if (m_evServer) { m_evServer->close(); delete m_evServer; }
    QLocalServer::removeServer(evName);
    m_evServer = new QLocalServer(this);
    m_evServer->setSocketOptions(QLocalServer::UserAccessOption);
    connect(m_evServer, &QLocalServer::newConnection, this, &StageBridge::onEvConnection);
    if (!m_evServer->listen(evName)) {
        qWarning() << "[stage] ev server listen failed" << m_evServer->errorString();
        setUnavailable(true);
        return false;
    }

    // 启动 stage 进程
    if (!m_proc) {
        m_proc = new QProcess(this);
        m_proc->setProcessChannelMode(QProcess::MergedChannels);
        connect(m_proc, &QProcess::readyReadStandardOutput, this, [this]() {
            // stage 调试输出转发到日志（仅 --log 之外时有）
            const QByteArray out = m_proc->readAllStandardOutput();
            if (!out.trimmed().isEmpty()) qDebug().noquote() << "[stage-out]" << out.trimmed();
        });
        connect(m_proc, &QProcess::finished, this, &StageBridge::onProcFinished);
        connect(m_proc, &QProcess::errorOccurred, this,
                [this](QProcess::ProcessError err) {
            if (err == QProcess::FailedToStart) {
                qWarning() << "[stage] failed to start";
                setUnavailable(true);
            }
        });
    }

    QStringList args;
    args << QStringLiteral("--cmd-pipe=%1").arg(cmdName)
         << QStringLiteral("--ev-pipe=%1").arg(evName)
         << QStringLiteral("--web-dir=%1").arg(QDir::toNativeSeparators(webDir))
         << QStringLiteral("--data-dir=%1").arg(QDir::toNativeSeparators(dataDir))
         << QStringLiteral("--parent=%1").arg(QCoreApplication::applicationPid());
    if (qEnvironmentVariableIsSet("MUYUN_STAGE_DEBUG"))
        args << QStringLiteral("--log=%1").arg(
            QDir::toNativeSeparators(dataDir + QStringLiteral("/stage_shell.log")));
#ifdef Q_OS_WIN
    if (m_window) {
        HWND hwnd = reinterpret_cast<HWND>(m_window->winId());
        args << QStringLiteral("--owner=%1").arg(
            QString::number(reinterpret_cast<quintptr>(hwnd), 16));
        RECT rc{};
        if (GetWindowRect(hwnd, &rc)) {
            args << QStringLiteral("--x=%1").arg(rc.left)
                 << QStringLiteral("--y=%1").arg(rc.top)
                 << QStringLiteral("--w=%1").arg(rc.right - rc.left)
                 << QStringLiteral("--h=%1").arg(rc.bottom - rc.top);
        }
    }
#endif

    m_pageReady = m_engineReady = false;
    m_stageHwnd = 0;                      // 新壳会重新报到自己的句柄
    m_stopping = false;
    // 关键：新实例立即升代——作废上一轮 killStage 挂着的"1 秒强杀"守护。
    // 否则快速关→开时，守护定时器到点误杀刚起的新舞台进程
    // （表现为反复进出舞台后 "helper exited unexpectedly" + 卡死假象）。
    ++m_generation;
    if (qEnvironmentVariableIsSet("MUYUN_STAGE_DEBUG"))
        fprintf(stderr, "[stage-bridge] launch seq=%d gen=%d\n", s_launchSeq, m_generation);
    m_proc->start(exePath, args);

    // 命令管道连接：异步重试（stage 创建管道服务端需要一点时间）
    // 同样先摘链：abort() 同步触发的 errorOccurred 回调里读的是成员指针
    if (QLocalSocket *oldCmd = m_cmdSock) {
        m_cmdSock = nullptr;
        oldCmd->abort();
        oldCmd->deleteLater();
    }
    m_cmdSock = new QLocalSocket(this);
    m_cmdPendingBuf.clear();
    connect(m_cmdSock, &QObject::destroyed, this, [this, s = m_cmdSock]() {
        if (m_cmdSock == s) m_cmdSock = nullptr;      // 同事件管道：删了就别留着悬指针
    });
    connect(m_cmdSock, &QLocalSocket::connected, this, &StageBridge::onCmdConnected);
    connect(m_cmdSock, &QLocalSocket::errorOccurred, this, [this, cmdName]() {
        if (m_stopping || !m_cmdSock) return;   // 正在关闭/套接字已销毁，不再重连
        if (m_cmdSock->state() == QLocalSocket::UnconnectedState &&
            m_proc && m_proc->state() != QProcess::NotRunning) {
            QTimer::singleShot(300, this, [this, cmdName]() {
                if (m_cmdSock && m_cmdSock->state() == QLocalSocket::UnconnectedState &&
                    m_proc && m_proc->state() != QProcess::NotRunning)
                    m_cmdSock->connectToServer(cmdName);
            });
        }
    });
    m_cmdSock->connectToServer(cmdName);

    // 就绪超时：20 秒没 engineReady 就先重试一次，仍失败才放弃（回退 lite 舞台）。代际守卫。
    // 为什么 8 秒不够：WebView2 冷启动（尤其上一个实例的浏览器进程还在释放用户数据目录时）
    // 实测能超过 10 秒，直接放弃就变成用户看到的"舞台时有时无"。
    const int gen = m_generation;
    QTimer::singleShot(20000, this, [this, gen]() {
        if (gen == m_generation && m_proc && m_proc->state() != QProcess::NotRunning
            && !m_engineReady) {
            if (m_engineRetries < 1) {
                ++m_engineRetries;
                qWarning() << "[stage] 引擎就绪超时(20s)，重试一次";
                close();
                open();
            } else {
                qWarning() << "[stage] engine init timeout（重试后仍失败）→ 放弃，回退 lite 舞台";
                m_engineRetries = 0;      // 归零：下次用户主动打开仍有完整重试预算
                close();
            }
        }
    });
    return true;
}

void StageBridge::setImmersive(bool on)
{
    if (!m_active) return;
    // 权威状态由页面 onImmersiveChange 回报（immersive 事件），这里只发指令不本地猜测
    sendJson(QJsonObject{{QStringLiteral("c"), QStringLiteral("immersive")},
                         {QStringLiteral("on"), on ? 1 : 0}});
}

bool StageBridge::sendEsc()
{
    const bool reachable = m_active && m_pageReady && m_cmdSock
                           && m_cmdSock->state() == QLocalSocket::ConnectedState;
    SBSTEP("sendEsc: active=%d pageReady=%d cmdSock=%d → %s\n", int(m_active), int(m_pageReady),
           m_cmdSock ? 1 : 0, reachable ? "交页面走阶梯" : "送不出去，调用方自己收尾");
    if (!reachable) return false;
    sendJson(QJsonObject{{QStringLiteral("c"), QStringLiteral("key")},
                         {QStringLiteral("k"), QStringLiteral("esc")}});
    return true;
}

void StageBridge::onProcFinished(int exitCode)
{
    if (qEnvironmentVariableIsSet("MUYUN_STAGE_DEBUG"))
        fprintf(stderr, "[stage-bridge] procFinished code=%d stopping=%d\n", exitCode, (int)m_stopping);
    Q_UNUSED(exitCode)
    m_pageReady = m_engineReady = false;
    m_stageHwnd = 0;
    if (m_immersive) { m_immersive = false; emit immersiveChanged(); }
    m_clockTimer->stop();
#ifdef MUYUN_SELFTES
    fprintf(stderr, "[stage-bridge] procFinished evSock=%p cmdSock=%p\n",
            static_cast<void *>(m_evSock), static_cast<void *>(m_cmdSock));
    fflush(stderr);
#endif
    SBSTEP("procFinished: 清 evSock=%p\n", static_cast<void *>(m_evSock));
    // 先摘链再销毁：abort() 会**同步**触发该 socket 的 disconnected 回调，回调里也会清
    // m_evSock。若先 abort 再 deleteLater，第二次解引用就是空指针（实测崩在 deleteLater）。
    if (QLocalSocket *ev = m_evSock) {
        m_evSock = nullptr;
        ev->abort();
        ev->deleteLater();
    }
    SBSTEP("procFinished: 清 cmdSock=%p\n", static_cast<void *>(m_cmdSock));
    if (QLocalSocket *cmd = m_cmdSock) {
        m_cmdSock = nullptr;
        cmd->abort();
        cmd->deleteLater();
    }
    SBSTEP("procFinished: 套接字清完\n");
    m_cmdPendingBuf.clear();
    const bool wasActive = m_active;
    m_active = false;
    SBSTEP("procFinished: emit activeChanged (wasActive=%d)\n", int(wasActive));
    if (wasActive) emit activeChanged();
    SBSTEP("procFinished: activeChanged 返回\n");
    if (!m_stopping) {
        qWarning() << "[stage] helper exited unexpectedly";
        // 不自动重试：QML 端 stage.active=false 会回到 lite 样式
    }
    m_stopping = false;
#ifdef Q_OS_WIN
    // 焦点归还：舞台（WebView2）抢走过激活，进程死亡后 Windows 未必把焦点还给 owner，
    // 主窗 QML Shortcut 全部失灵（表现为再进歌词页后 ESC 无反应须点击一下）。显式唤回。
    // 多重尝试：即时一次 + 120ms 补一次 + 450ms 带 Alt 骗前台锁再补一次
    // （垂死舞台的前台交接是异步的，Windows 常把前台塞给别的程序而非 owner）。
    if (wasActive) {
        auto bring = [this](bool nudge) {
            if (!m_window) return;
            const auto vis = m_window->visibility();
            if (!m_window->isVisible() || vis == QWindow::Minimized || vis == QWindow::Hidden) return;
            HWND hwnd = reinterpret_cast<HWND>(m_window->winId());
            if (!hwnd || GetForegroundWindow() == hwnd) return;   // 已在前台，别打扰
            if (nudge) {
                keybd_event(VK_MENU, 0, 0, 0);                    // 单次 Alt 骗过前台锁
                keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
            }
            m_window->requestActivate();
            BringWindowToTop(hwnd);
            SetForegroundWindow(hwnd);
        };
        bring(false);
        QTimer::singleShot(120, this, [bring]() { bring(false); });
        QTimer::singleShot(450, this, [bring]() { bring(true); });
    }
#endif
    // 关闭期间若有新的 open/恢复意图 → 现在进程已退出，按当前意图重开
    if (m_reopenPending) {
        m_reopenPending = false;
        SBSTEP("procFinished: reopenPending → applyStage\n");
        applyStage();
    }
    SBSTEP("procFinished: 出口\n");
}

// ---------------------------------------------------------------------------
// 管道
// ---------------------------------------------------------------------------

void StageBridge::onEvConnection()
{
    QLocalSocket *sock = m_evServer->nextPendingConnection();
    if (!sock) return;
    // 换连接前先摘旧的：abort() 会同步触发该 socket 的 disconnected 回调，所以
    // **必须先清空成员指针**再 abort/deleteLater，否则回调与这里两次操作同一对象。
    if (QLocalSocket *old = m_evSock) {
        m_evSock = nullptr;
        old->abort();
        old->deleteLater();
    }
    m_evSock = sock;
    m_evBuf.clear();
    // 悬垂指针兜底（真凶）：这些 sock 是 deleteLater 异步销毁的，任何一条路径先把它
    // 删了而成员还指着它，onProcFinished 里再 abort()/deleteLater() 就是踩已释放内存
    // ——实测舞台关闭时 0xC0000005 崩在 QObject::deleteLater()。对象一死立刻清空成员。
    connect(sock, &QObject::destroyed, this, [this, sock]() {
        if (m_evSock == sock) m_evSock = nullptr;
    });
    connect(sock, &QLocalSocket::readyRead, this, &StageBridge::onEvReadyRead);
    connect(sock, &QLocalSocket::disconnected, this, [this, sock]() {
        if (m_evSock == sock) { m_evSock = nullptr; sock->deleteLater(); }
    });
}

static bool parseFrames(QByteArray &buf, std::function<void(const QByteArray &)> onFrame) {
    while (buf.size() >= 4) {
        const uchar *h = reinterpret_cast<const uchar *>(buf.constData());
        const quint32 len = quint32(h[0]) | (quint32(h[1]) << 8)
                          | (quint32(h[2]) << 16) | (quint32(h[3]) << 24);
        if (len == 0 || len > 32u * 1024u * 1024u) return false;
        if (buf.size() < int(4 + len)) break;
        const QByteArray frame = buf.mid(4, int(len));
        buf.remove(0, int(4 + len));
        onFrame(frame);
    }
    return true;
}

void StageBridge::onEvReadyRead()
{
    if (!m_evSock) return;
    m_evBuf += m_evSock->readAll();
    bool ok = parseFrames(m_evBuf, [this](const QByteArray &raw) {
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (doc.isObject()) handleEvent(doc.object());
    });
    if (!ok) { qWarning() << "[stage] bad event frame"; m_evBuf.clear(); }
}

void StageBridge::onCmdConnected()
{
    // 连上后立刻把状态推一遍（幂等）
    if (m_engineReady) {
        pushTrackNow();
        pushLyricsNow();
        pushPlayMode();
        pushBounds();
    }
}

void StageBridge::sendJson(const QJsonObject &obj)
{
    if (!m_cmdSock || m_cmdSock->state() != QLocalSocket::ConnectedState) return;
    const QByteArray json = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    m_cmdSock->write(leUint32(quint32(json.size())));
    m_cmdSock->write(json);
}

// ---------------------------------------------------------------------------
// 事件处理（stage → 主）
// ---------------------------------------------------------------------------

void StageBridge::handleEvent(const QJsonObject &obj)
{
    const QString e = obj.value(QStringLiteral("e")).toString();
    if (qEnvironmentVariableIsSet("MUYUN_STAGE_DEBUG"))
        fprintf(stderr, "[stage-bridge] event e=%s\n", qPrintable(e));

    if (e == QLatin1String("ready")) {
        m_pageReady = true;
        sendJson(QJsonObject{{QStringLiteral("c"), QStringLiteral("hello")},
                             {QStringLiteral("app"), QStringLiteral("MuyunMusic")}});
        return;
    }
    if (e == QLatin1String("engineReady")) {
        m_engineReady = true;
        m_engineRetries = 0;      // 起来了就归零，下次打开仍有完整重试预算
        setActive(true);
        m_clockTimer->start();
        pushTrackNow();
        pushLyricsNow();
        pushPlayMode();
        pushBounds();
        pushVisibility();   // 初始同步主窗可见性
        return;
    }
    if (e == QLatin1String("hwnd")) {
        // 壳窗句柄报到（判"前台是不是舞台"要用，Esc 系统热键的接管条件）
        m_stageHwnd = reinterpret_cast<quintptr>(
            std::strtoull(obj.value(QStringLiteral("h")).toString().toLatin1().constData(),
                          nullptr, 16));
        SBSTEP("壳窗句柄 = %p\n", reinterpret_cast<void *>(m_stageHwnd));
        return;
    }
    if (e == QLatin1String("immersive")) {
        const bool on = obj.value(QStringLiteral("on")).toBool();
        if (m_immersive != on) { m_immersive = on; emit immersiveChanged(); }
        return;
    }
    if (e == QLatin1String("ctrl")) {
        const QString a = obj.value(QStringLiteral("a")).toString();
        const double v = obj.value(QStringLiteral("v")).toDouble();
        if (a == QLatin1String("play")) m_player->togglePlay();
        else if (a == QLatin1String("next")) m_player->next();
        else if (a == QLatin1String("prev")) m_player->previous();
        else if (a == QLatin1String("seek")) m_player->seek(qint64(v));
        else if (a == QLatin1String("vol")) m_player->setVolume(qreal(v));
        else if (a == QLatin1String("mode")) m_player->cyclePlayMode();
        else emit ctrlEvent(a, v);   // like / queue / back 等交给 QML
        return;
    }
    if (e == QLatin1String("toast")) {
        emit toastMessage(obj.value(QStringLiteral("msg")).toString());
        return;
    }
    if (e == QLatin1String("fx")) {
        emit fxStateReceived(obj.value(QStringLiteral("state")).toObject());
        return;
    }
    if (e == QLatin1String("beat")) {
        emit beatStatus(obj.value(QStringLiteral("maps")).toInt(),
                        obj.value(QStringLiteral("sample")).toString());
        return;
    }
    if (e == QLatin1String("log")) {
        qDebug().noquote() << "[stage-web]" << obj.value(QStringLiteral("m")).toString();
        return;
    }
    if (e == QLatin1String("fatal")) {
        qWarning().noquote() << "[stage-web-fatal]" << obj.value(QStringLiteral("msg")).toString();
    }
}

void StageBridge::setActive(bool a)
{
    if (m_active == a) return;
    m_active = a;
    emit activeChanged();
}

void StageBridge::setUnavailable(bool u)
{
    if (m_unavailable == u) return;
    m_unavailable = u;
    emit unavailableChanged();
}

// ---------------------------------------------------------------------------
// 数据推送（主 → stage）
// ---------------------------------------------------------------------------

void StageBridge::pushTrackNow()
{
    const Song s = m_player->currentSong();
    m_lastTrack = QJsonObject{
        {QStringLiteral("c"), QStringLiteral("track")},
        {QStringLiteral("id"), s.identityKey()},
        {QStringLiteral("name"), s.name},
        {QStringLiteral("artist"), s.artist},
        {QStringLiteral("album"), s.album},
        {QStringLiteral("platform"), s.sourceCode()},
        {QStringLiteral("cover"), QString()},
        {QStringLiteral("url"), QString()},
    };
    m_attachedAudioPath.clear();
    sendJson(m_lastTrack);
    if (m_engineReady) m_audioRetryTimer->start();   // 等音频落盘后挂 track.bin

    // 封面：下载到 web/covers 下（同源 → canvas 不被 taint，粒子管线可用）
    // 本地歌曲封面是 file:// 路径（扫描器导出的内嵌封面），同样复制进 web/covers 供舞台同源加载
    if (s.cover.startsWith(QStringLiteral("file://")) || (!s.cover.startsWith(QStringLiteral("http"))
        && QDir::isAbsolutePath(s.cover))) {
        const QString localPath = s.cover.startsWith(QStringLiteral("file://"))
            ? QUrl(s.cover).toLocalFile() : s.cover;
        if (QFileInfo::exists(localPath)) {
            const QString hash = Crypto::md5Hex((localPath + QFileInfo(localPath).lastModified().toString()).toUtf8());
            const QString fileName = hash + QStringLiteral(".jpg");
            const QString savePath = QCoreApplication::applicationDirPath()
                + QStringLiteral("/stage/web/covers/") + fileName;
            if (!QFileInfo::exists(savePath))
                QFile::copy(localPath, savePath);
            m_lastTrack[QStringLiteral("cover")] = QStringLiteral("/covers/") + fileName;
            sendJson(m_lastTrack);
            return;
        }
    }
    if (s.cover.startsWith(QStringLiteral("http"))) {
        const QString hash = Crypto::md5Hex(s.cover.toUtf8());
        const QString fileName = hash + QStringLiteral(".jpg");
        const QString appDir = QCoreApplication::applicationDirPath();
        const QString savePath = appDir + QStringLiteral("/stage/web/covers/") + fileName;
        QFileInfo fi(savePath);
        if (fi.exists() && fi.size() > 0) {
            m_lastTrack[QStringLiteral("cover")] = QStringLiteral("/covers/") + fileName;
            sendJson(m_lastTrack);
            return;
        }
        HttpOptions opt;
        const QString src = s.sourceCode();
        if (src == QLatin1String("wy")) opt.referer = QStringLiteral("https://music.163.com/");
        else if (src == QLatin1String("tx")) opt.referer = QStringLiteral("https://y.qq.com/");
        opt.timeoutMs = 15000;
        const int seq = ++m_coverJobSeq;
        HttpClient::instance()->getAsync(s.cover, opt,
            [this, seq, fileName](HttpResponse r) {
                if (seq != m_coverJobSeq) return;   // 已切歌，丢弃
                if (!r.ok || r.body.isEmpty()) { emit toastMessage(QStringLiteral("舞台封面加载失败")); return; }
                const QString savePath = QCoreApplication::applicationDirPath()
                    + QStringLiteral("/stage/web/covers/") + fileName;
                QFile f(savePath);
                if (f.open(QIODevice::WriteOnly)) {
                    f.write(r.body);
                    f.close();
                    if (m_lastTrack.value(QStringLiteral("id")) ==
                        m_player->currentSong().identityKey()) {
                        m_lastTrack[QStringLiteral("cover")] = QStringLiteral("/covers/") + fileName;
                        sendJson(m_lastTrack);
                    }
                }
            });
    }
}

void StageBridge::pushLyricsNow()
{
    QJsonObject obj{
        {QStringLiteral("c"), QStringLiteral("lyrics")},
        {QStringLiteral("lines"), QJsonArray::fromVariantList(m_player->lyricLines())},
    };
    sendJson(obj);
}

void StageBridge::pushPlayMode()
{
    sendJson(QJsonObject{
        {QStringLiteral("c"), QStringLiteral("pmode")},
        {QStringLiteral("id"), m_player->playModeId()},
    });
}

void StageBridge::setFav(bool on)
{
    sendJson(QJsonObject{
        {QStringLiteral("c"), QStringLiteral("fav")},
        {QStringLiteral("on"), on ? 1 : 0},
    });
}

void StageBridge::testInject(const QString &name, const QString &artist,
                             const QString &cover, const QVariantList &lines)
{
    if (qEnvironmentVariableIsSet("MUYUN_STAGE_DEBUG"))
        fprintf(stderr, "[stage-bridge] testInject cover=[%s] lines=%d cmdSockState=%d\n",
                qPrintable(cover), (int)lines.size(),
                m_cmdSock ? (int)m_cmdSock->state() : -1);
    sendJson(QJsonObject{
        {QStringLiteral("c"), QStringLiteral("track")},
        {QStringLiteral("id"), QStringLiteral("test-") + name},
        {QStringLiteral("name"), name},
        {QStringLiteral("artist"), artist},
        {QStringLiteral("album"), QStringLiteral("自检")},
        {QStringLiteral("platform"), QStringLiteral("wy")},
        {QStringLiteral("cover"), cover},
    });
    sendJson(QJsonObject{
        {QStringLiteral("c"), QStringLiteral("lyrics")},
        {QStringLiteral("lines"), QJsonArray::fromVariantList(lines)},
    });
    sendJson(QJsonObject{
        {QStringLiteral("c"), QStringLiteral("clk")},
        {QStringLiteral("t"), 3000.0},
        {QStringLiteral("d"), 240000.0},
        {QStringLiteral("p"), 1},
    });
}

void StageBridge::tryAttachTrackAudio()
{
    if (!m_engineReady || m_lastTrack.isEmpty()) return;
    const QString src = m_player->currentAudioLocalPath();
    if (src.isEmpty() || src == m_attachedAudioPath) return;
    const QFileInfo fi(src);
    // 音频尚未下载完成（在线歌先落盘临时文件才播放）→ 下一轮再试
    if (!fi.exists() || fi.size() < 2048) return;
    m_attachedAudioPath = src;   // 记录，避免重复拷贝同一文件

    const QString dst = QCoreApplication::applicationDirPath()
                        + QStringLiteral("/stage/web/track.bin");
    const QString tmp = dst + QStringLiteral(".tmp");
    QFile::remove(tmp);
    if (!QFile::copy(src, tmp)) { m_attachedAudioPath.clear(); return; }
    QFile::remove(dst);
    if (!QFile::rename(tmp, dst)) { m_attachedAudioPath.clear(); return; }

    QJsonObject t = m_lastTrack;
    t[QStringLiteral("url")] = QStringLiteral("/track.bin");
    m_lastTrack = t;
    sendJson(t);
    pushLyricsNow();   // 引擎 setTrack 会清空歌词，补推一次
    if (m_audioRetryTimer) m_audioRetryTimer->stop();   // 已挂载，停止轮询
    if (qEnvironmentVariableIsSet("MUYUN_STAGE_DEBUG"))
        fprintf(stderr, "[stage-bridge] track.bin attached from %lld bytes\n",
                (long long)fi.size());
}

void StageBridge::requestBeatProbe()
{
    sendJson(QJsonObject{{QStringLiteral("c"), QStringLiteral("beatprobe")}});
}

void StageBridge::domProbe(int x, int y, int w, int h)
{
    sendJson(QJsonObject{{QStringLiteral("c"), QStringLiteral("domprobe")},
                         {QStringLiteral("x"), x}, {QStringLiteral("y"), y},
                         {QStringLiteral("w"), w}, {QStringLiteral("h"), h}});
}

bool StageBridge::ownsSystemForeground() const
{
#ifdef Q_OS_WIN
    const HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    // ① 本进程的窗（主窗、桌面歌词、fx 面板、各类弹层）
    if (pid == GetCurrentProcessId()) return true;
    // ② 舞台壳进程（MuyunStage.exe）自己的窗
    if (m_proc && m_proc->processId() && pid == static_cast<DWORD>(m_proc->processId())) return true;
    // ③ WebView2 子进程（msedgewebview2.exe）的窗：认"根祖先是不是壳窗"
    if (m_stageHwnd) {
        const HWND root = GetAncestor(fg, GA_ROOT);
        if (root && root == reinterpret_cast<HWND>(m_stageHwnd)) return true;
    }
    SBSTEP("前台归属判否：fg=%p pid=%lu (我们=%lu 舞台=%lld 壳=%p root=%p)\n",
           static_cast<void *>(fg), static_cast<unsigned long>(pid),
           static_cast<unsigned long>(GetCurrentProcessId()),
           m_proc ? static_cast<long long>(m_proc->processId()) : -1LL,
           reinterpret_cast<void *>(m_stageHwnd),
           static_cast<void *>(GetAncestor(fg, GA_ROOT)));
    return false;
#else
    return false;
#endif
}

void StageBridge::pushVisibility()
{
    // 最小化/隐藏 → 杀进程释放内存；恢复 → 若页面仍想要舞台则重开
    applyStage();
}

void StageBridge::pushBounds()
{
#ifdef Q_OS_WIN
    if (!m_window) return;
    HWND hwnd = reinterpret_cast<HWND>(m_window->winId());
    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return;
    const bool fs = m_window->visibility() == QWindow::FullScreen;
    sendJson(QJsonObject{
        {QStringLiteral("c"), QStringLiteral("bounds")},
        {QStringLiteral("x"), double(rc.left)},
        {QStringLiteral("y"), double(rc.top)},
        {QStringLiteral("w"), double(rc.right - rc.left)},
        {QStringLiteral("h"), double(rc.bottom - rc.top)},
        {QStringLiteral("fs"), fs ? 1 : 0},
    });
#endif
}

void StageBridge::onClockTick()
{
    if (!m_engineReady) return;
    sendJson(QJsonObject{
        {QStringLiteral("c"), QStringLiteral("clk")},
        {QStringLiteral("t"), double(m_player->position())},
        {QStringLiteral("d"), double(m_player->duration())},
        {QStringLiteral("p"), m_player->isPlaying() ? 1 : 0},
    });
}

void StageBridge::onBoundsTick()
{
    if (!m_boundsDirty || !m_active) return;
    m_boundsDirty = false;
    pushBounds();
}

// ---------------------------------------------------------------------------
// fx 控制透传
// ---------------------------------------------------------------------------

void StageBridge::fxSet(const QString &key, double value)
{
    sendJson(QJsonObject{{QStringLiteral("c"), QStringLiteral("fx")},
                         {QStringLiteral("op"), QStringLiteral("set")},
                         {QStringLiteral("k"), key},
                         {QStringLiteral("v"), value}});
}

void StageBridge::fxToggle(const QString &key)
{
    sendJson(QJsonObject{{QStringLiteral("c"), QStringLiteral("fx")},
                         {QStringLiteral("op"), QStringLiteral("toggle")},
                         {QStringLiteral("k"), key}});
}

void StageBridge::fxPreset(int index)
{
    sendJson(QJsonObject{{QStringLiteral("c"), QStringLiteral("fx")},
                         {QStringLiteral("op"), QStringLiteral("preset")},
                         {QStringLiteral("i"), index}});
}

void StageBridge::fxReset()
{
    sendJson(QJsonObject{{QStringLiteral("c"), QStringLiteral("fx")},
                         {QStringLiteral("op"), QStringLiteral("reset")}});
}

void StageBridge::requestFxState()
{
    sendJson(QJsonObject{{QStringLiteral("c"), QStringLiteral("fx")},
                         {QStringLiteral("op"), QStringLiteral("get")}});
}

} // namespace Muyun
