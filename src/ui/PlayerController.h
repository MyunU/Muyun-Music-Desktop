#pragma once

#include "core/Types.h"
#include "core/player/PlayerEngine.h"

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QHash>
#include <QStringList>
#include <QVector>
#include <QTimer>

namespace Muyun {

/**
 * @brief 播放器控制器（暴露给 QML）
 *
 * 管理播放队列、播放模式、音量、进度、歌词与音质，
 * 负责把在线歌曲解析为实际播放地址后交给 PlayerEngine。
 */
class PlayerController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QVariantMap currentSong READ currentSongMap NOTIFY currentSongChanged)
    Q_PROPERTY(bool isPlaying READ isPlaying NOTIFY isPlayingChanged)
    Q_PROPERTY(bool isLoading READ isLoading NOTIFY isLoadingChanged)
    Q_PROPERTY(qint64 position READ position NOTIFY positionChanged)
    Q_PROPERTY(qint64 duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(qreal volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY mutedChanged)
    Q_PROPERTY(QString playMode READ playModeId WRITE setPlayModeId NOTIFY playModeChanged)
    Q_PROPERTY(QString playModeName READ playModeName NOTIFY playModeChanged)
    Q_PROPERTY(QString quality READ qualityId WRITE setQualityId NOTIFY qualityChanged)
    Q_PROPERTY(QVariantList playlist READ playlist NOTIFY playlistChanged)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY currentIndexChanged)
    Q_PROPERTY(QString playlistName READ playlistName WRITE setPlaylistName NOTIFY playlistNameChanged)
    Q_PROPERTY(QString currentQualityLabel READ currentQualityLabel NOTIFY currentQualityChanged)
    /// 版本号属性：任一歌曲改取源平台时 NOTIFY，驱动所有胶囊即时刷新（绑定它即可，勿用方法调用）
    Q_PROPERTY(int songPlatformVersion READ songPlatformVersion NOTIFY songPlatformChanged)
    /// 按实际音频文件大小算出的码率标签（如"实测 128k"），音源虚标时提醒用户
    Q_PROPERTY(QString measuredBitrateLabel READ measuredBitrateLabel NOTIFY measuredBitrateChanged)
    Q_PROPERTY(QVariantList lyricLines READ lyricLines NOTIFY lyricChanged)
    Q_PROPERTY(bool lyricLoading READ lyricLoading NOTIFY lyricLoadingChanged)
    Q_PROPERTY(QString currentLyricText READ currentLyricText NOTIFY currentLyricChanged)
    Q_PROPERTY(QString currentLyricTranslation READ currentLyricTranslation NOTIFY currentLyricChanged)

    // 音效状态
    Q_PROPERTY(bool eqEnabled READ eqEnabled WRITE setEqEnabled NOTIFY effectsChanged)
    Q_PROPERTY(int eqPreset READ eqPreset WRITE setEqPreset NOTIFY effectsChanged)
    Q_PROPERTY(bool reverbEnabled READ reverbEnabled WRITE setReverbEnabled NOTIFY effectsChanged)
    Q_PROPERTY(int reverbPreset READ reverbPreset WRITE setReverbPreset NOTIFY effectsChanged)
    Q_PROPERTY(bool spatialEnabled READ spatialEnabled WRITE setSpatialEnabled NOTIFY effectsChanged)
    /// 环绕半径/速度：**必须绑这两个属性**（面板曾写死 value:50，导致重启显示假默认、
    /// 且一动另一个滑杆就把存好的值覆盖回 50）
    Q_PROPERTY(double spatialRadius READ spatialRadius NOTIFY effectsChanged)
    Q_PROPERTY(double spatialSpeed READ spatialSpeed NOTIFY effectsChanged)
    Q_PROPERTY(bool loudnessEnabled READ loudnessEnabled WRITE setLoudnessEnabled NOTIFY effectsChanged)
    Q_PROPERTY(double loudnessTarget READ loudnessTarget WRITE setLoudnessTarget NOTIFY effectsChanged)
    /// 音效是否真的作用在音频流上（走了音效管线）。开启音效但音源不是 MP3 时为 false
    Q_PROPERTY(bool effectsLive READ effectsLive NOTIFY effectsRuntimeChanged)
    /// 音效已开启但当前音源无法生效（非 MP3 / 设备格式不支持）→ UI 必须提示用户
    Q_PROPERTY(bool effectsBypassed READ effectsBypassed NOTIFY effectsRuntimeChanged)
    /// 任一音效开关是否打开（供入口按钮高亮，一眼看出"音效开着"）
    Q_PROPERTY(bool effectsOn READ effectsOn NOTIFY effectsChanged)
    /// 音效输出设备/格式说明（如"耳机 (DAREU-A3 MAX) · 48000Hz"），未接管时为空
    Q_PROPERTY(QString effectDeviceInfo READ effectDeviceInfo NOTIFY effectsRuntimeChanged)

public:
    explicit PlayerController(QObject *parent = nullptr);

    QVariantMap currentSongMap() const;
    Song currentSong() const { return m_currentSong; }
    bool isPlaying() const;
    bool isLoading() const { return m_loading; }
    /// 单首取音频看门狗是否在计时（自检用：验证"选歌武装 / 出声或停止解除"）
    bool songWatchdogActive() const;
    qint64 position() const;
    qint64 duration() const;
    qreal volume() const;
    bool muted() const;
    QString playModeId() const;
    QString playModeName() const;
    QString qualityId() const;
    QVariantList playlist() const;
    int currentIndex() const { return m_currentIndex; }
    QString playlistName() const { return m_playlistName; }
    QString currentQualityLabel() const;
    QString measuredBitrateLabel() const { return m_measuredBitrate; }
    QVariantList lyricLines() const;
    bool lyricLoading() const { return m_lyricLoading; }
    /// 当前播放位置对应的歌词行文本（无歌词返回空）
    QString currentLyricText() const;
    /// 当前歌词行的翻译（无翻译返回空）
    QString currentLyricTranslation() const;

    void setVolume(qreal v);
    void setMuted(bool muted);
    void setPlayModeId(const QString &id);
    void setQualityId(const QString &id);
    void setPlaylistName(const QString &name);

    /// 按歌曲记忆"取源平台"：某首歌用它自己平台播不了时，用户单独为它指定一家平台拉音频。
    /// 只影响这一首（按 identityKey 记忆），不波及同平台其它歌曲。
    Q_INVOKABLE void setSongPlatform(const QVariantMap &song, const QString &code);
    /// 该歌曲实际取源平台（有记忆用记忆的，否则歌曲原平台）
    Q_INVOKABLE QString songPlatform(const QVariantMap &song) const;
    /// 该歌曲实际取源平台的显示名（内部把 sourceCode 转平台名，供胶囊直接显示）
    Q_INVOKABLE QString songPlatformName(const QVariantMap &song) const;
    /// 该歌曲是否被用户改过取源平台
    Q_INVOKABLE bool songPlatformOverridden(const QVariantMap &song) const;
    /// 可选平台 [{id,name}]（供胶囊弹菜单）
    Q_INVOKABLE QVariantList songPlatformOptions() const;
    /// 版本号：作为 Q_PROPERTY 暴露，绑定它可让所有胶囊在任一歌曲改平台后即时刷新
    int songPlatformVersion() const { return m_songPlatformVersion; }

    /// 播放指定歌曲（playlist 为播放上下文）
    Q_INVOKABLE void playSong(const QVariantMap &song, const QVariantList &playlist,
                              const QString &playlistId = QString(),
                              const QString &playlistName = QString());
    Q_INVOKABLE void playIndex(int index);
    /// 下一首播放：插入到当前播放队列的下一位置，且下一切歌时无视播放模式优先播它；无播放上下文则直接播放
    Q_INVOKABLE void insertNext(const QVariantMap &song);
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    Q_INVOKABLE void seek(qint64 ms);
    Q_INVOKABLE void seekRatio(qreal ratio);
    Q_INVOKABLE void cyclePlayMode();
    Q_INVOKABLE void setPlaybackRate(qreal rate);

    /// 队列操作
    Q_INVOKABLE void addToQueue(const QVariantMap &song);
    Q_INVOKABLE void removeFromQueue(int index);
    Q_INVOKABLE void clearQueue();
    Q_INVOKABLE void shufflePlaylist();

    /// 播放全部（传入歌曲列表）
    Q_INVOKABLE void playAll(const QVariantList &songs, const QString &playlistId,
                             const QString &playlistName);
    /// 单独重新获取当前歌曲歌词（不影响播放进度）
    Q_INVOKABLE void reloadLyric();

    /// 供 QML 显示的格式化时间
    Q_INVOKABLE QString formatTime(qint64 ms) const;

    /// 当前播放音频的本地文件路径（在线=已下载的临时文件；未就绪=空）
    Q_INVOKABLE QString currentAudioLocalPath() const;

    // ---- 音效控制（转发给 AudioEffects）----
    bool eqEnabled() const;
    int eqPreset() const;
    bool reverbEnabled() const;
    int reverbPreset() const;
    bool spatialEnabled() const;
    bool loudnessEnabled() const;
    double loudnessTarget() const;

    Q_INVOKABLE void setEqEnabled(bool on);
    Q_INVOKABLE void setEqPreset(int preset);
    Q_INVOKABLE void setEqGain(int band, double db);
    Q_INVOKABLE double eqGain(int band) const;
    Q_INVOKABLE void resetEq();
    Q_INVOKABLE void setReverbEnabled(bool on);
    Q_INVOKABLE void setReverbPreset(int preset);
    Q_INVOKABLE void setSpatialEnabled(bool on);
    Q_INVOKABLE void setSpatialParams(double radius, double speed);
    double spatialRadius() const;
    double spatialSpeed() const;
    /// 把所有音效参数恢复默认（不动四个开关）
    Q_INVOKABLE void resetAllEffects();
    Q_INVOKABLE void setLoudnessEnabled(bool on);
    Q_INVOKABLE void setLoudnessTarget(double db);
    Q_INVOKABLE QStringList eqBandLabels() const;

    bool effectsLive() const;
    bool effectsBypassed() const;
    bool effectsOn() const;
    QString effectDeviceInfo() const;

    /// 某本地文件被删除/移除 → 从播放队列剔除；若正是当前曲则停止（避免切到空文件卡死）
    Q_INVOKABLE void dropLocalSong(const QString &localPath);

signals:
    void currentSongChanged();
    void isPlayingChanged();
    void isLoadingChanged();
    void positionChanged();
    void durationChanged();
    void measuredBitrateChanged();
    /// 本地歌曲播放时实测时长与库记录不符 → 请求回写校准（main.cpp 连到 LibraryController）
    void localDurationKnown(const QString &localPath, int durationSec);

    void volumeChanged();
    void mutedChanged();
    void playModeChanged();
    void qualityChanged();
    void playlistChanged();
    void currentIndexChanged();
    void playlistNameChanged();
    void currentQualityChanged();
    void songPlatformChanged();
    void lyricChanged();
    void lyricLoadingChanged();
    void currentLyricChanged();
    void effectsChanged();
    /// 音效是否真正生效（管线接管/回退）——驱动入口按钮高亮与面板状态行
    void effectsRuntimeChanged();
    void playFailed(const QString &message);

private slots:
    void onEndOfMedia();
    void onEngineError(const QString &message);

private:
    void resolveAndPlay();
    /// 下一曲索引（非 const：会消费"下一首播放"覆写队列）
    int nextIndexByMode(bool userTriggered);
    /// 随机播放"不重复牌堆"：下一曲 = 牌堆当前曲的后一张；一轮播完重洗再续。
    /// 替代旧的"每次独立均匀随机"（歌单小时同曲短时间内重复率极高，用户反馈）
    int nextShuffleIndex();
    /// 随机播放"上一首"：回到本轮牌堆的前一张（已播过、且不重复）
    int prevShuffleIndex();
    /// （重新）洗牌生成不重复牌堆：当前曲放到队首，保证随机从"现在"继续
    void rebuildShuffleDeck();
    void loadLyric();
    void updateCurrentLyric();
    void saveState();
    void restoreState();
    /// 按实际音频文件大小/时长算实测码率，识别音源虚标（如标 Master 实际几 MB）
    void updateMeasuredBitrate();
    /// 当前歌封面为空且带 LX 元信息时，后台向脚本要封面（local 源 pic action，待办 #9）
    void maybeResolveCover();

    PlayerEngine *m_engine = nullptr;
    QVector<Song> m_playlist;
    Song m_currentSong;
    int m_currentIndex = -1;
    QString m_playlistId;
    QString m_playlistName;
    PlayMode m_playMode = PlayMode::Sequence;
    AudioQuality m_quality = AudioQuality::K320;
    AudioQuality m_actualQuality = AudioQuality::K320;
    bool m_actualQualityKnown = false;
    // 播放失败自动降档重试（音源返回坏文件时）
    int m_playRetry = 0;
    AudioQuality m_playTriedQ = AudioQuality::K320;
    void resolveAndPlayAt(AudioQuality startQ);
    /// 播放彻底失败（在线歌降档用尽/取不到链接）→ 自动切下一首而非停在原地；连败过多则止损
    void advanceOnPlayFailure(const QString &reason);
    QStringList m_pendingNext;   // “下一首播放”插入的歌 identityKey 队列：切歌时优先消费，无视播放模式
    QVector<int> m_shuffleDeck;  ///< 随机播放"不重复牌堆"：本轮内的播放顺序（播放列表索引），轮尽重洗
    int m_failStreak = 0;        // 连续自动跳歌失败计数（真正开播成功即清零，防整队列拉胯死循环）
    int m_lastFinishedIndex = -1; // 上一首播完的索引（advanceOnPlayFailure 跳过它，防牌堆回绕→重播同一首）
    bool m_wasPlaying = false;    // 当前曲是否已成功开播（开播后失败 → 不重试同曲，直接前进）
    bool m_loading = false;
    SongLyric m_lyric;
    bool m_lyricLoading = false;
    QString m_currentLyricText;
    QString m_currentLyricTranslation;
    QString m_measuredBitrate;
    QHash<QString, QString> m_songPlatform;   // identityKey → 取源平台（仅用户手动改过的）
    int m_songPlatformVersion = 0;
    // 单首取音频的总时限（跨所有降档重试）。在线歌取不到链接时，旧代码会沿音质降级链
    // 逐档真请求（每档 HttpClient 20s 超时，6 档最坏 2 分钟），这段时间 UI 钉在这首歌上——
    // 用户报「随机模式下老是一首歌播完以后还是这首歌」。到点仍没出声就按失败跳下一首。
    // ⚠ 只在"用户换歌/选歌"时重置，advanceOnPlayFailure 里**不重置**：
    //   否则整队列都挂时，每跳一首都续命 30s，永远退不出、也没熔断意义。
    QTimer m_songWatchdog;
    /// 为本曲重新武装单首时限
    void armSongWatchdog();
    /// 真播起来了 / 主动停下 → 解除看门狗
    void disarmSongWatchdog();
    /// 看门狗到点：本曲始终没出声 → 按失败处理，前进到下一首（不准停在本曲）
    void onSongWatchdogTimeout();
    /// playIndex 的内部实现。arm=false 供"失败自动前进"用：不续命单首时限，
    /// 整队列都挂时限时预算耗尽即停，不会永远跳下去
    void startAt(int index, bool armWatchdog);

    QTimer m_positionTimer;
};

} // namespace Muyun
