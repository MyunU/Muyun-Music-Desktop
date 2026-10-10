#pragma once

#include <QObject>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QTimer>
#include <QUrl>

#include "core/audio/AudioEffects.h"

namespace Muyun {

class EffectPlayer;

/**
 * @brief 播放引擎（双内核）
 *
 * 默认内核：QMediaPlayer + QAudioOutput（FFmpeg 后端解码，mp3/aac/flac 都稳）。
 * 音效内核：EffectPlayer（minimp3 解码 + AudioEffects DSP + 重采样 + QAudioSink）。
 *
 * 切换规则（都只在"任一音效开启"时才用音效内核）：
 *  - 任一音效开 + 音源是 MP3      → 音效内核（真出声、参数即时生效）
 *  - 任一音效开 + 其它格式         → QMediaPlayer，并置 effectsBypassed 供 UI 提示
 *  - 音效全关                     → QMediaPlayer（原始稳定路径，支持变速）
 *  - 音效内核装载失败（格式谈不拢/文件坏）→ 自动回退 QMediaPlayer，不中断播放
 */
class PlayerEngine : public QObject
{
    Q_OBJECT
public:
    explicit PlayerEngine(QObject *parent = nullptr);
    ~PlayerEngine() override;

    /// 播放本地文件
    void playFile(const QString &path);
    /// 播放网络/本地 URL（在线资源先下载到临时文件再播放）。
    /// cacheKey：歌曲稳定身份（identityKey+音质）。在线 URL 每次解析都带新签名，
    /// 必须用 cacheKey 做缓存文件名，否则永远不命中缓存（每次重下=播放慢的根因）。
    void play(const QUrl &url, const QString &cacheKey = QString());
    /// 查询播放缓存：cacheKey（identityKey@音质）命中且文件有效则返回本地路径，否则空。
    /// 供上层在解析音源**之前**先走缓存——缓存歌重播无需音源脚本在线拉取。
    /// expectedDurationSec>0 时读真实时长对比：半截/损坏缓存直接删除返回空，
    /// 避免命中坏文件 → 播到中间 FFmpeg 报错 → 误跳下一首。
    static QString cachedAudioFile(const QString &cacheKey, double expectedDurationSec = 0.0);
    /// 播放前**解码实测**：用解码后端（minimp3/FFmpeg）试 open + 读一帧 + 时长对比。
    /// 拦截 cachedAudioFile 轻校验放行的坏缓存——头部完整（FLAC STREAMINFO / MP3 Xing /
    /// M4A moov 声称完整时长）但实际数据损坏/解不出数据的文件，读时长对得上却一帧都解不出，
    /// 直接喂给播放器就是崩溃/卡死。所有格式统一走这条硬校验（MP3 之外以前没有防御）。
    /// 返回 false 表示文件损坏，调用方应删除并降档重试。
    static bool verifyCacheDecodable(const QString &path, double expectedDurationSec);
    /// 播放缓存目录（%TEMP%/muyun-audio）。公开供启动清扫/自检定位文件。
    static QString audioCacheDir();
    /// cacheKey 对应的缓存文件路径（play 写盘与 cachedAudioFile 查询共用，防散列式漂移）
    static QString cachePathForKey(const QString &cacheKey);
    /// 清扫过期播放缓存（>maxAgeDays 天的 .audio），返回删除数；启动时自动跑一次
    static int sweepAudioCache(int maxAgeDays = 3);
    /// 删除当前播放的缓存文件（仅当文件在缓存目录内时），供上层「缓存损坏→删掉重下」用
    void deleteCurrentCache();
    void pause();
    void resume();
    void stop();

    bool isPlaying() const;
    /// 是否已加载播放源（重启恢复的歌曲尚未解析，为 false）
    bool hasSource() const;
    /// 当前媒体源（在线=下载后的本地临时文件 URL，本地=file url；未加载=空）
    QUrl sourceUrl() const;
    qint64 position() const;      // 毫秒
    qint64 duration() const;      // 毫秒

    void seek(qint64 ms);
    void setVolume(qreal volume); // 0.0 - 1.0
    qreal volume() const;
    void setMuted(bool muted);
    bool isMuted() const;
    void setPlaybackRate(qreal rate);

    /// 音效引擎（接口保留，UI 调节用）
    AudioEffects *effects() const { return m_effects; }

    /// 音效参数变化后重新评估播放后端：需要在两条内核之间切换时才切换。
    /// 走音效内核时参数是**即时生效**的（后台逐块读参数），不再重解码整曲。
    void reevaluateEffects();
    /// 当前是否走音效管线（供 UI 提示"音效未生效"）
    bool usingEffectPipeline() const { return m_useEffect; }
    /// 音效已开启但当前音源无法走音效管线（非 MP3 / 设备格式谈不拢）→ UI 提示
    bool effectsBypassed() const;
    /// 音效管线是否正在异步装载（期间不要判定"未生效"）
    bool effectLoading() const { return m_effectPending; }

    /// 音效管线诊断（不在音效管线上时返回 0）
    qint64 effectStarvedMs() const;
    int effectOutputRate() const;
    int effectSourceRate() const;
    /// 实际解码后端："minimp3" / "ffmpeg"（不在音效管线上时为空）
    QString effectBackend() const;

    enum class State { Stopped, Playing, Paused, Loading };
    State state() const;

    /// 音频可视化频谱（占位，QMediaPlayer 无直接频谱数据）
    QVector<int> spectrum() const;

    /// 当前使用的音频输出设备名（用于 UI 显示，可为空）
    QString audioDeviceName() const;
    /// 全部可用输出设备（id / 名称）
    QStringList audioDeviceNames() const;
    /// 重新附着到当前系统默认输出设备（保留播放位置/状态）。
    /// 供 UI"修复声音"入口与设备切换回归自检调用；等价于收到一次默认设备变更。
    void reattachToCurrentDevice();

signals:
    void positionChanged(qint64 position);
    void durationChanged(qint64 duration);
    void playbackStateChanged(int state);
    void errorOccurred(const QString &message);
    void endOfMedia();
    void audioDeviceChanged(const QString &name);
    /// 音效是否真正生效（音效管线接管 / 回退普通内核）变化时发出，驱动 UI 提示
    void effectsRuntimeChanged();

private slots:
    void onDownloadFinished(bool ok, const QString &err);
    void onAudioOutputsChanged();
    void onDefaultDeviceChanged(const QAudioDevice &device);

private:
    void startPlayback(const QString &localPath);
    /// 走 QMediaPlayer 内核（音效关闭或音效不可用时）
    void startViaPlayer(const QString &localPath, qint64 fromMs, bool autoplay);
    /// 走音效内核（异步装载；失败自动回退 startViaPlayer）
    void startViaEffect(const QString &localPath, qint64 fromMs, bool autoplay);
    void onEffectLoaded(bool ok, const QString &reason);
    void onPlayerError(QMediaPlayer::Error error, const QString &errString);
    bool effectsActive() const;
    /// 当前条件下是否应该走音效管线（格式能不能解、倍速冲突、已知失败都算在内）
    bool wantEffectPipeline(const QString &localPath) const;
    /// 懒创建 QAudioOutput：Qt6 构造 QAudioOutput 即打开默认输出端点（启动即唤醒
    /// 蓝牙耳机等设备，有概率产生一次性爆音），故仅在首次真正播放时创建。
    void ensureAudioOutput();
    /// 懒创建 QMediaPlayer：Qt6.8 FFmpeg 后端构造即建内部 QAudioSink 打开 WASAPI 会话
    /// （探针实测 AudioSes.dll 在构造瞬间载入）→ 同样仅首次播放时创建，之后常驻。
    QMediaPlayer *ensurePlayer();
    /// 判断文件头是否为有效音频（排除 JSON/HTML 错误响应）
    static bool isAudioFile(const QString &path);
    /// 设备切换后的"跳回原位置"：仅在媒体已加载（Loaded/Buffered）时生效——
    /// setPosition 在 setSource 后媒体未加载完会被 FFmpeg 后端静默丢弃（切设备从头播放的根因）
    void tryApplyResumeSeek();

    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_output = nullptr;
    AudioEffects *m_effects = nullptr;
    EffectPlayer *m_effect = nullptr;
    bool m_useEffect = false;
    bool m_effectPending = false;      ///< 音效管线正在异步装载
    QString m_effectFailedPath;        ///< 该文件试过音效但不可用（避免来回重试）
    bool m_wantPlay = false;   // 期望播放：LoadedMedia 时补 play()，修本地起播竞态
    QTimer m_nudgeTimer;       // 起播后若卡在 0（部分文件 play 不激活解码器）补 seek 救活
    int m_nudgeCount = 0;
    QString m_currentLocalPath;

    // 「播完却停在原曲」/「播放卡死」兜底（QMediaPlayer 路径专用）：
    // 1) 有些文件 FFmpeg 后端播到末尾只发 StoppedState、不发 EndOfMedia（尾部异常数据、
    //    被截断的播放缓存都会这样）→ 上层永远收不到 endOfMedia，UI 就钉在这一首歌上；
    // 2) 解码卡住 / 下载残文件时状态是 Playing 但位置永远不动 → 同样钉在原地。
    // 500ms 巡检一次：停在末尾就补发 endOfMedia；连续 6s 位置不前进就报 errorOccurred，
    // 让上层走"降档重试 / 自动跳下一首"。音效管线不用这个（EffectPlayer 自己报 endOfMedia）。
    QTimer m_watchTimer;
    bool m_watchArmed = false;   ///< 本曲进入 QMediaPlayer 路径且在自启动播放（stop/收尾会清）
    int m_stallTicks = 0;        ///< 位置连续不前进的巡检次数
    qint64 m_lastWatchPos = -1;  ///< 上次巡检的位置（ms）

    qreal m_volume = 0.8;
    qreal m_rate = 1.0;              ///< 期望倍速：音效管线不支持变速，非 1.0 时交回普通内核
    bool m_muted = false;
    bool m_playing = false;
    bool m_loading = false;
    /// 重入保护：stop() 内部 m_effect->stop() / m_player->stop() 会同步发 Stopped 信号，
    /// 上层（PlayerController）的 playbackStateChanged 里若再调 stop()，就无限递归 → 栈溢出
    /// （实测 0xC00000FD：切歌瞬间旧歌还在 Playing，时长校验误判"时长异常"→ 又 stop()）。
    /// 已在 stop() 中则直接返回，不让信号链重入。
    bool m_stopping = false;
    /// seek 期间抑制 QMediaPlayer 状态变化信号（stop→setPosition→play 会闪 Stopped→Playing）
    bool m_suppressStateChange = false;
    QTimer m_positionTimer;
    QString m_pendingLocalPath;

    // 音频设备切换检测：用 QMediaDevices::audioOutputsChanged 信号监听
    // 系统输出设备变化（含默认设备切换），事件驱动，无轮询开销。
    QMediaDevices *m_mediaDevices = nullptr;
    QAudioDevice m_lastDevice;
    bool m_deviceInit = false;
    qint64 m_resumedPosition = -1;
    bool m_resumeAfterSwitch = false;
};

} // namespace Muyun
