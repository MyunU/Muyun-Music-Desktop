#pragma once

#include <QObject>
#include <QByteArray>
#include <QJsonObject>
#include <QVariantList>
#include <QPointer>
#include <QString>

#include <functional>

class QProcess;
class QLocalServer;
class QLocalSocket;
class QQuickWindow;
class QTimer;
class QEvent;

namespace Muyun {

class PlayerController;
class SettingsController;

/**
 * @brief 舞台引擎桥（方案C：独立进程）
 *
 * 负责拉起/杀死 MuyunStage.exe（WebView2 壳），把播放器的
 * 曲目/歌词/播放态/进度时钟推给舞台页面，并把页面的控制事件
 * 转回 PlayerController。主程序内不执行任何 JS。
 *
 * 通道（与 stage/src/ipc 对应，帧 = 4B 小端长度 + UTF-8 JSON）：
 *  - 命令管道  MuyunCmd.<pid> ：主进程 QLocalSocket → stage 管道服务端 → 页面
 *  - 事件管道  MuyunEv.<pid>  ：stage QLocalSocket → 主进程 QLocalServer
 */
class StageBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(bool unavailable READ unavailable NOTIFY unavailableChanged)
    /// 舞台页当前是否处于沉浸模式（页面 onImmersiveChange 回报；ESC 阶梯用）
    Q_PROPERTY(bool immersive READ immersive NOTIFY immersiveChanged)

public:
    explicit StageBridge(PlayerController *player, SettingsController *settings,
                         QObject *parent = nullptr);
    ~StageBridge() override;

    /// 主窗口就绪后调用：记录 HWND（owner 关联）并挂几何变化监视
    void attachWindow(QQuickWindow *window);

    /// 监听主窗移动（QWindow 无 positionChanged 信号，用事件过滤器）
    bool eventFilter(QObject *watched, QEvent *event) override;

    bool active() const { return m_active; }
    bool unavailable() const { return m_unavailable; }
    bool immersive() const { return m_immersive; }

    Q_INVOKABLE void open();
    Q_INVOKABLE void close();
    /// 主程序驱动舞台沉浸开关（fx 面板等仍可用；ESC 阶梯改用 sendEsc）
    Q_INVOKABLE void setImmersive(bool on);
    /// 把 Esc 交给舞台页处理——ESC 阶梯的唯一裁判是页面（只有它知道自己是否真在沉浸，
    /// 主程序那份 immersive 标志是异步回报的，会过期，早先"ESC 用一次就失灵"就卡在这）。
    /// 返回 false = 送不出去（页面没就绪/管道断），调用方应按本地阶梯自己收尾。
    Q_INVOKABLE bool sendEsc();

    // ---- fx 控制（QML fx 面板用；v1 仅透传） ----
    Q_INVOKABLE void fxSet(const QString &key, double value);
    Q_INVOKABLE void fxToggle(const QString &key);
    Q_INVOKABLE void fxPreset(int index);
    Q_INVOKABLE void fxReset();
    Q_INVOKABLE void requestFxState();
    /// 收藏态推送（QML 端在 favorite 变化时调用）
    Q_INVOKABLE void setFav(bool on);

    /// 请求页面回报节拍缓存状态（调试/自检用）
    Q_INVOKABLE void requestBeatProbe();
    /// 请求页面枚举指定矩形的可见元素（调试"黑块"用）
    Q_INVOKABLE void domProbe(int x, int y, int w, int h);

    /// 当前系统前台窗口是不是"我们的"（主窗 / 舞台壳窗及其子窗 / 本进程或舞台进程的窗）。
    /// Esc 系统热键只在返回 true 时才注册：既不抢别的程序的 Esc，又能在舞台抢走激活时仍收到键。
    bool ownsSystemForeground() const;
    /// 舞台壳窗句柄（页面回报；0=还没报到）
    Q_INVOKABLE quintptr stageHwnd() const { return m_stageHwnd; }

    /// 自检注入（--test-stage）：直接推一条曲目 + 歌词 + 时钟，绕过播放器状态
    Q_INVOKABLE void testInject(const QString &name, const QString &artist,
                                const QString &cover, const QVariantList &lines);

signals:
    void activeChanged();
    void unavailableChanged();
    void immersiveChanged();
    /// 页面控制事件（play/next/prev/seek/like/mode/queue/back）
    void ctrlEvent(const QString &action, double value);
    /// 页面 toast 提示
    void toastMessage(const QString &msg);
    /// fx 状态快照（面板 v1 可先不接）
    void fxStateReceived(const QJsonObject &state);
    /// 离线节拍缓存状态（beatprobe 回报：maps=已分析节拍数）
    void beatStatus(int maps, const QString &sample);

private slots:
    void onEvConnection();
    void onEvReadyRead();
    void onCmdConnected();
    void onClockTick();
    void onBoundsTick();
    void onProcFinished(int exitCode);

private:
    bool launch();
    void sendJson(const QJsonObject &obj);
    void handleEvent(const QJsonObject &obj);
    void pushTrackNow();
    void pushLyricsNow();
    void pushPlayMode();
    void pushBounds();
    void pushVisibility();   // 主窗激活/可见性 → 舞台暂停/恢复（后台不跑 JS）
    /// 依据「页面意图 m_wanted + 主窗是否最小化/隐藏」决定拉起或杀掉 stage
    void applyStage();
    void killStage();        // 实际终止进程（不改动 m_wanted）
    /// 把当前播放的本地音频文件挂到 web/track.bin 并补发带 url 的 track（离线节拍源）
    void tryAttachTrackAudio();
    void setActive(bool a);
    void setUnavailable(bool u);
    static QByteArray frame(const QByteArray &json);

    PlayerController *m_player = nullptr;
    SettingsController *m_settings = nullptr;
    QPointer<QQuickWindow> m_window;

    QProcess *m_proc = nullptr;
    QLocalServer *m_evServer = nullptr;         // 事件：stage → 主
    QLocalSocket *m_evSock = nullptr;
    QLocalSocket *m_cmdSock = nullptr;          // 命令：主 → stage
    QByteArray m_cmdPendingBuf;
    QByteArray m_evBuf;

    QTimer *m_clockTimer = nullptr;    // 50ms 进度时钟
    QTimer *m_boundsTimer = nullptr;   // 窗口几何节流
    QTimer *m_audioRetryTimer = nullptr; // 2s：等待播放音频落盘后挂载 track.bin
    bool   m_boundsDirty = false;

    bool m_active = false;         // 进程活着
    bool m_immersive = false;      // 舞台页沉浸模式（页面回报）
    bool m_wanted = false;         // 页面意图：希望舞台显示
    bool m_pageReady = false;      // stage 页面 ready
    bool m_engineReady = false;    // 引擎实例就绪
    bool m_unavailable = false;    // 环境缺失（不重试）
    bool m_stopping = false;       // 主动 close 中
    bool m_reopenPending = false;  // close 期间收到 open → 等进程退出后再拉
    int  m_generation = 0;         // 启动代际：异步回调只认最新一代
    /// WebView2 冷启动偶尔超过就绪预算（例如上一个实例的浏览器进程还没释放用户数据目录）。
    /// 超时后**重试一次**再放弃回退 lite，消除"舞台时有时无"；成功或彻底放弃时归零。
    int  m_engineRetries = 0;
    quintptr m_stageHwnd = 0;      // 舞台壳窗 HWND（壳经事件管道回报；判前台归属用）

    QString m_lastTrackId;
    QString m_attachedAudioPath;   // 已挂到 track.bin 的源文件路径（去重）
    QJsonObject m_lastTrack;       // 最近一次推送的 track 载荷（补 url 时复用）
    int m_coverJobSeq = 0;         // 封面下载代际（防串台）
};

} // namespace Muyun
