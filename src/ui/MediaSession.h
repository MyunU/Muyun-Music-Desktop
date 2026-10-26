#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>

class QWindow;
class QTimer;

namespace Muyun {

class PlayerController;

/**
 * @brief Windows 系统媒体控制（SMTC）会话
 *
 * 用户报的 bug：播放歌曲时 Windows 系统媒体播放组件（音量弹层的媒体卡、
 * 锁屏/通知中心的媒体控件、键盘上的多媒体键）识别不到暮云在放歌。
 *
 * 原因：程序从来没跟系统媒体传输控制（System Media Transport Controls）打过
 * 交道，Windows 自然不知道有个应用在播歌，也就没有会话、没有元数据、多媒体键
 * 更是无人接收。
 *
 * 本类负责：在 GUI 线程上用 SMTC interop 把自己的主窗口注册成系统媒体会话，
 * 之后每次切歌/播放状态变化就把 标题/歌手/封面/播放状态/进度 推给系统，
 * 并把系统面板上按下的 播放/暂停/上一首/下一首/拖动进度 转回 PlayerController。
 *
 * 为什么是手写 COM 声明（src/ui/MediaSession.cpp 里那堆结构体）：
 *  - 工程是 MinGW + CMake，不装 Windows SDK，也没有 C++/WinRT 的投影头；
 *  - SMTC 的接口在系统里是稳定的 WinRT ABI（IInspectable 派生 + 固定 vtable），
 *    手写声明与官方 SDK 头逐字节同构，效果与 cppwinrt 一致；
 *  - 好处是零构建依赖：任何 MinGW 都能编，不用把 SDK 头塞进仓库。
 *
 * 两条硬教训（照抄 HotkeyManager 的经验，别再踩）：
 *  1) 会话挂在 HWND 上。Qt 进出全屏会重建原生窗口，句柄一换旧会话就废了，
 *     必须比对新 winId() 重新 GetForWindow（否则全屏后就"系统又看不见了"）。
 *  2) SMTC 的事件回调不一定在 GUI 线程回来，所有动作都经队列投递到 GUI 线程，
 *     绝不在回调里直接动播放器。
 */
class MediaSession : public QObject
{
    Q_OBJECT
public:
    explicit MediaSession(PlayerController *player, QObject *parent = nullptr);
    ~MediaSession() override;

    /// 绑定主窗口并建立系统会话（主窗建好后调用）
    void attach(QWindow *window);

    /// 此刻是否真的拿到了系统媒体会话（拿不到=系统侧任何面板都不会出现我们）
    bool sessionActive() const;

    /// 内部：系统媒体键回调（Delegate 已投递到 GUI 线程）
    void handleSystemButton(int button);
    /// 内部：系统媒体面板拖动进度条
    void handleSystemSeek(qint64 ms);

#ifdef MUYUN_SELFTES
    /// 自检：推一条合成曲目并回读校验；返回空串=通过，否则是错误描述
    QString testPushAndVerify(const QVariantMap &song, bool playing);
    /// 自检：系统侧真正调过来多少次按钮回调（0 = 系统压根没把键送到我们这）
    int testSystemButtonCount() const;
#endif

private:
    void onSongChanged();
    void onPlayingChanged();
    void onPositionChanged();
    void onDurationChanged();
    /// HWND 被 Qt 重建后把会话搬到新窗口上
    void syncHwnd();
    bool ensureSession();
    void releaseSession();
    void pushMetadata();
    void pushPlaybackStatus();
    void pushButtons();
    void pushTimeline(bool force);

    struct Impl;
    Impl *d = nullptr;
};

} // namespace Muyun
