#include "PlayerController.h"

#include "AudioPreloader.h"
#include "core/player/PlayerEngine.h"
#include "core/music/MusicSdk.h"
#include "core/lyrics/LyricParser.h"
#include "core/localmusic/TagReader.h"
#include "core/storage/DocumentStore.h"
#include "core/utils/Format.h"
#include "core/network/HttpClient.h"

#include <QtConcurrent>
#include <QFutureWatcher>
#include <QThread>
#include <QUrl>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QDateTime>
#include <algorithm>

namespace Muyun {

// 单首取音频的总时限（ms）。MUYUN_SONG_WATCH_MS 可覆盖（自检压到几百毫秒，不必真等 30s）
static int songWatchdogMs()
{
    const QByteArray e = qgetenv("MUYUN_SONG_WATCH_MS");
    if (!e.isEmpty()) {
        const int v = e.toInt();
        if (v > 0) return v;
    }
    return 30000;
}

// 取更低的音质（枚举 K128=0 … Master=6，降档即 -1）；已是最低则返回 false
static bool lowerQuality(AudioQuality q, AudioQuality *out)
{
    const int v = static_cast<int>(q);
    if (v <= 0) return false;
    *out = static_cast<AudioQuality>(v - 1);
    return true;
}

// ⚠ 音质标签恒等于用户选择的档位（见 currentQualityLabel），与菜单 ✓ 同源，永不"实际档顶替选择"。
//   m_actualQuality 只记录脚本真正返回的归一化档，供内部降档/校验逻辑用，不驱动对外显示。

PlayerController::PlayerController(QObject *parent) : QObject(parent)
{
    m_engine = new PlayerEngine(this);

    connect(m_engine, &PlayerEngine::positionChanged, this,
            [this]() {
                // seek 乐观值：引擎追上目标位置（允许 100ms 误差）后清除
                if (m_pendingSeekMs >= 0 && m_engine->position() >= m_pendingSeekMs - 100) {
                    m_pendingSeekMs = -1;
                    m_seekClearTimer.stop();
                }
                emit positionChanged();
                updateCurrentLyric();
            });
    connect(m_engine, &PlayerEngine::durationChanged, this,
            [this]() {
                emit durationChanged();
                updateMeasuredBitrate();
                // 本地歌曲时长自校准：播放器拿到的真实时长与库记录差 >3s → 回写
                // （VBR MP3 无 Xing 头时扫描器估算是错的，以播放器实测为准）
                if (m_currentSong.isLocal() && !m_currentSong.localPath.isEmpty()) {
                    const qint64 real = m_engine->duration();
                    const int stored = static_cast<int>(m_currentSong.duration);
                    if (real >= 1000 && qAbs(real / 1000 - stored) > 3)
                        emit localDurationKnown(m_currentSong.localPath,
                                                static_cast<int>(real / 1000));
                }
            });
    connect(m_engine, &PlayerEngine::playbackStateChanged, this,
            [this]() {
                if (m_engine->state() == PlayerEngine::State::Playing) {
                    // 时长校验：引擎实际时长与平台接口时长不一致 → 文件损坏/不完整 → 停掉重下
                    // ⚠ 单位：engine.duration() 毫秒，m_currentSong.duration 秒（Types.h L134）
                    // ⚠ 必须只在校验武装时做（m_durationCheckArmed）：切歌瞬间 stop() 的信号链
                    //   会误入这个分支——那时引擎还在播**旧歌**（duration 是旧歌的），拿新歌 API
                    //   时长去比必然"不符"→ 又 stop() → 无限递归 → 栈溢出（0xC00000FD，程序
                    //   静默消失无弹窗）。armed 只在"当前曲刚发起播放"时为 true，开播校验一次即 disarm。
                    if (m_durationCheckArmed) {
                        m_durationCheckArmed = false;   // 先 disarm：本次校验只做一次，防信号链重入
                        const qint64 engineDur = m_engine->duration();
                        const qint64 apiDurMs = static_cast<qint64>(std::llround(m_currentSong.duration * 1000.0));
                        // ⚠ 开播时长校验只做**日志观察**，不再删缓存/降档重下：
                        //   ① 音源脚本的 interval 是估计值，常与实际音频差几十秒（实测差 37s），
                        //     绝对差 >3s 会把这类正常文件误杀 → 删缓存 → 降档重下同一文件 → 循环失败；
                        //   ② 试听片段（几秒~几十秒预览）本身是完整音频，能进 Playing 就该能播，
                        //     1.1.4 就是直接播的——拿它当"坏文件"删掉重下还是同一试听，必然失败；
                        //   ③ 真坏文件（半截/损坏）会在播放中报 errorOccurred → onEngineError
                        //     已有删缓存 + 降档兜底，不需要这里重复拦截。
                        if (engineDur > 0 && apiDurMs > 0 && engineDur < apiDurMs / 10)
                            qWarning() << "[duration] 文件时长异常偏短（< 声明 10%）：实际"
                                       << engineDur << "ms vs 声明" << apiDurMs << "ms ("
                                       << m_currentSong.name << ")";
                    }
                    m_wasPlaying = true;  // 记住已开播：开播后失败不重试同曲
                    m_failStreak = 0;    // 真播起来了：失败熔断计数清零
                    m_lastFinishedIndex = -1; // 新曲开播 → 清除上一首记录
                    disarmSongWatchdog(); // 出声了：单首时限解武装
                }
                emit isPlayingChanged();
            });
    connect(m_engine, &PlayerEngine::endOfMedia, this, &PlayerController::onEndOfMedia);
    connect(m_engine, &PlayerEngine::errorOccurred, this, &PlayerController::onEngineError);
    // 音效管线接管/回退 → 通知 UI（入口高亮、面板状态行）
    connect(m_engine, &PlayerEngine::effectsRuntimeChanged, this,
            [this]() { emit effectsRuntimeChanged(); emit isPlayingChanged(); });

    // 单首取音频总时限：到点仍没出声就按失败前进（见 m_songWatchdog 注释）
    m_songWatchdog.setSingleShot(true);
    connect(&m_songWatchdog, &QTimer::timeout, this,
            &PlayerController::onSongWatchdogTimeout);

    // seek 乐观值超时清除（2s 兜底，正常情况引擎追上前就清了）
    m_seekClearTimer.setSingleShot(true);
    m_seekClearTimer.setInterval(2000);
    connect(&m_seekClearTimer, &QTimer::timeout, this,
            [this]() { m_pendingSeekMs = -1; });

    // 预缓存：切歌/歌单变化 → 把接下来几首静默下载到缓存目录
    m_preloader = new AudioPreloader(this);
    connect(this, &PlayerController::currentSongChanged, this,
            &PlayerController::restartPreloader);
    connect(this, &PlayerController::playlistChanged, this,
            &PlayerController::restartPreloader);

    restoreState();
    restartPreloader();
}

// ---------------------------------------------------------------------------
// 属性
// ---------------------------------------------------------------------------

QVariantMap PlayerController::currentSongMap() const { return m_currentSong.toMap(); }
bool PlayerController::isPlaying() const { return m_engine->isPlaying(); }
qint64 PlayerController::position() const
{
    if (m_pendingSeekMs >= 0) return m_pendingSeekMs;
    return m_engine->position();
}
qint64 PlayerController::duration() const { return m_engine->duration(); }
qreal PlayerController::volume() const { return m_engine->volume(); }
bool PlayerController::muted() const { return m_engine->isMuted(); }

QString PlayerController::playModeId() const { return Muyun::playModeId(m_playMode); }
QString PlayerController::playModeName() const { return Muyun::playModeName(m_playMode); }
QString PlayerController::qualityId() const { return Muyun::qualityId(m_quality); }

QString PlayerController::currentQualityLabel() const
{
    if (m_currentSong.isLocal()) return QStringLiteral("本地文件");
    // ⚠ 音质标签**永远等于用户选择的档位**（m_quality），与音质菜单的 ✓ 同源 → 两者恒一致。
    //   不再显示"实际拿到的归一化档"——之前那样会导致：选 24Bit 但音源只有 FLAC 时标签跳成
    //   FLAC（与菜单不符）；有缓存时切音质标签也不动。用户明确要求跟随选择（五-76）。
    //   实测档 m_actualQuality 仍保留给内部逻辑（如降档判断），只是不再驱动这个对外标签。
    return Muyun::qualityName(m_quality);
}

// 实测码率 = 音频文件字节×8 / 时长秒。音源常虚标（标 Master 实际只给 320k/128k 的量），
// 用文件大小反推真实档位，供播放条音质标签提示用户。
void PlayerController::updateMeasuredBitrate()
{
    const qint64 durMs = m_engine->duration();
    const QString path = currentAudioLocalPath();
    QString label;
    if (durMs >= 5000 && !path.isEmpty() && QFileInfo::exists(path)) {
        const qint64 bytes = QFileInfo(path).size();
        if (bytes > 0) {
            const double kbps = bytes * 8.0 / (durMs / 1000.0) / 1000.0;
            if (kbps >= 24)
                label = QStringLiteral("实测 %1k").arg(qRound(kbps / 8.0) * 8);
        }
    }
    if (label != m_measuredBitrate) {
        m_measuredBitrate = label;
        emit measuredBitrateChanged();
    }
}

// 封面懒加载（待办 #9）：LX local 源的 pic action。搜索/榜单给的都是内置五源的封面，
// 只有"脚本解析出的歌、又没带封面"时才值得跑一次网络（脚本给不出就静默保持空）。
void PlayerController::maybeResolveCover()
{
    if (!m_currentSong.cover.isEmpty() || m_currentSong.isLocal() || !m_currentSong.hasLx)
        return;
    const Song song = m_currentSong;
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, song]() {
        const QString cover = watcher->result();
        watcher->deleteLater();
        if (cover.isEmpty()) return;
        if (m_currentSong.identityKey() != song.identityKey()) return;   // 已切歌
        if (!m_currentSong.cover.isEmpty()) return;
        m_currentSong.cover = cover;
        emit currentSongChanged();
    });
    MusicSdk *sdk = MusicSdk::instance();
    watcher->setFuture(QtConcurrent::run([sdk, song]() { return sdk->resolveCover(song); }));
}

QVariantList PlayerController::playlist() const
{
    QVariantList out;
    for (const auto &s : m_playlist) out.append(s.toMap());
    return out;
}

QVariantList PlayerController::lyricLines() const { return m_lyric.toList(); }

QString PlayerController::currentLyricText() const { return m_currentLyricText; }
QString PlayerController::currentLyricTranslation() const { return m_currentLyricTranslation; }

void PlayerController::updateCurrentLyric()
{
    const double sec = static_cast<double>(m_engine->position()) / 1000.0;
    int idx = -1;
    // 线性查找当前行（歌词行数通常几十行，足够快；position 每 200ms 更新一次）
    for (int i = 0; i < m_lyric.lines.size(); ++i) {
        if (m_lyric.lines.at(i).time <= sec) idx = i;
        else break;
    }
    QString text, trans;
    if (idx >= 0) {
        text = m_lyric.lines.at(idx).text;
        trans = m_lyric.lines.at(idx).translation;
    } else if (!m_lyric.lines.isEmpty() && !m_currentSong.name.isEmpty()) {
        // 前奏/间奏：还没有唱到的行——显示「♪ 歌名」占位，播放条不空白
        text = QStringLiteral("♪ ") + m_currentSong.name;
    }
    if (text != m_currentLyricText || trans != m_currentLyricTranslation) {
        m_currentLyricText = text;
        m_currentLyricTranslation = trans;
        emit currentLyricChanged();
    }
}

// ---------------------------------------------------------------------------
// 设置
// ---------------------------------------------------------------------------

void PlayerController::setVolume(qreal v)
{
    m_engine->setVolume(v);
    emit volumeChanged();
    saveState();
}

void PlayerController::setMuted(bool muted)
{
    m_engine->setMuted(muted);
    emit mutedChanged();
    saveState();
}

void PlayerController::setPlayModeId(const QString &id)
{
    m_playMode = Muyun::playModeFromId(id);
    emit playModeChanged();
    saveState();
}

void PlayerController::setQualityId(const QString &id)
{
    bool ok = false;
    const AudioQuality q = Muyun::qualityFromId(id, &ok);
    if (!ok) return;
    m_quality = q;
    // ⚠ 用户重选音质 → 实测标志作废：标签先显示新选的档，重新解析播放拿到实测后才更新。
    m_actualQualityKnown = false;
    m_wantedQuality = q;   // 本轮取源只命中这一档的缓存（无则下载），不被旧低档缓存顶替
#ifdef MUYUN_SELFTES
    qInfo() << "[quality] setQualityId ->" << id << "label=" << currentQualityLabel();
#endif
    emit qualityChanged();
    emit currentQualityChanged();   // #12：音质标签随选择即时刷新（不再等播放）
    saveState();
    // 切换音质后重新解析当前歌曲。⚠ 不走 resolveAndPlay()（那里会把 m_wantedQuality 重置成
    // m_quality——虽然这里也是 q，但保持单一路径、避免重复清标志/重复 emit）。
    if (!m_currentSong.id.isEmpty() && !m_currentSong.isLocal()) {
        m_playRetry = 0;
        m_wasPlaying = false;
        resolveAndPlayAt(m_quality);
    }
}

void PlayerController::setPlaylistName(const QString &name)
{
    if (m_playlistName == name) return;
    m_playlistName = name;
    emit playlistNameChanged();
}

void PlayerController::setSongPlatform(const QVariantMap &song, const QString &code)
{
    const Song s = Song::fromMap(song);
    if (s.id.isEmpty()) return;
    const QString key = s.identityKey();
    const QString own = s.lx.source.isEmpty() ? platformSourceCode(s.platform) : s.lx.source;
    const QString want = (code == own) ? QString() : code;   // 选回原平台=清除记忆
    auto it = m_songPlatform.find(key);
    if (want.isEmpty()) {
        if (it == m_songPlatform.end()) return;
        m_songPlatform.erase(it);
    } else {
        if (it != m_songPlatform.end() && it.value() == want) return;
        m_songPlatform.insert(key, want);
    }
    // 实时持久化
    QVariantMap doc;
    for (auto i = m_songPlatform.constBegin(); i != m_songPlatform.constEnd(); ++i)
        doc[i.key()] = i.value();
    DocumentStore::instance()->write(QStringLiteral("player"),
                                     QStringLiteral("songPlatforms"), doc);
    ++m_songPlatformVersion;
    emit songPlatformChanged();
    // 改的正是当前播放曲 → 立即按新平台重新取源播放（正在失效时可当场切活）
    if (!m_currentSong.id.isEmpty() && !m_currentSong.isLocal()
        && m_currentSong.identityKey() == key)
        resolveAndPlay();
}

QString PlayerController::songPlatform(const QVariantMap &song) const
{
    const Song s = Song::fromMap(song);
    const QString own = s.lx.source.isEmpty() ? platformSourceCode(s.platform) : s.lx.source;
    const auto it = m_songPlatform.constFind(s.identityKey());
    return it == m_songPlatform.constEnd() ? own : it.value();
}

QString PlayerController::songPlatformName(const QVariantMap &song) const
{
    return platformName(platformFromSourceCode(songPlatform(song)));
}

bool PlayerController::songPlatformOverridden(const QVariantMap &song) const
{
    const Song s = Song::fromMap(song);
    return m_songPlatform.contains(s.identityKey());
}

QVariantList PlayerController::songPlatformOptions() const
{
    QVariantList out;
    for (const auto &item : MusicSdk::instance()->sourceInfos()) {
        const QVariantMap info = item.toMap();
        QVariantMap m;
        m[QStringLiteral("id")] = info.value(QStringLiteral("code"));
        m[QStringLiteral("name")] = info.value(QStringLiteral("name"));
        out.append(m);
    }
    return out;
}

// ---- 音效 ----
bool PlayerController::eqEnabled() const { return m_engine->effects()->eqEnabled(); }
int PlayerController::eqPreset() const
{
    return static_cast<int>(m_engine->effects()->eqPreset());
}
bool PlayerController::reverbEnabled() const { return m_engine->effects()->reverbEnabled(); }
int PlayerController::reverbPreset() const
{
    return static_cast<int>(m_engine->effects()->reverbPreset());
}
bool PlayerController::spatialEnabled() const { return m_engine->effects()->spatialEnabled(); }
bool PlayerController::loudnessEnabled() const { return m_engine->effects()->loudnessEnabled(); }
double PlayerController::loudnessTarget() const { return m_engine->effects()->loudnessTarget(); }

void PlayerController::setEqEnabled(bool on)
{
    m_engine->effects()->setEqEnabled(on);
    m_engine->reevaluateEffects();
    emit effectsChanged();
}
void PlayerController::setEqPreset(int preset)
{
    m_engine->effects()->setEqPreset(static_cast<AudioEffects::EqPreset>(preset));
    m_engine->reevaluateEffects();
    emit effectsChanged();
}
void PlayerController::setEqGain(int band, double db)
{
    m_engine->effects()->setEqGain(band, db);
    emit effectsChanged();
}
double PlayerController::eqGain(int band) const { return m_engine->effects()->eqGain(band); }
void PlayerController::resetEq()
{
    m_engine->effects()->resetEq();
    emit effectsChanged();
}
void PlayerController::setReverbEnabled(bool on)
{
    m_engine->effects()->setReverbEnabled(on);
    m_engine->reevaluateEffects();
    emit effectsChanged();
}
void PlayerController::setReverbPreset(int preset)
{
    m_engine->effects()->setReverbPreset(static_cast<AudioEffects::ReverbPreset>(preset));
    m_engine->reevaluateEffects();
    emit effectsChanged();
}
void PlayerController::setSpatialEnabled(bool on)
{
    m_engine->effects()->setSpatialEnabled(on);
    m_engine->reevaluateEffects();
    emit effectsChanged();
}
void PlayerController::setSpatialParams(double radius, double speed)
{
    m_engine->effects()->setSpatialParams(radius, speed);
    emit effectsChanged();
}

double PlayerController::spatialRadius() const { return m_engine->effects()->spatialRadius(); }
double PlayerController::spatialSpeed() const { return m_engine->effects()->spatialSpeed(); }

void PlayerController::resetAllEffects()
{
    m_engine->effects()->resetAllParams();
    emit effectsChanged();     // 面板所有滑杆/预设跟着回位（NOTIFY 驱动绑定重算）
}
void PlayerController::setLoudnessEnabled(bool on)
{
    m_engine->effects()->setLoudnessEnabled(on);
    m_engine->reevaluateEffects();
    emit effectsChanged();
}
void PlayerController::setLoudnessTarget(double db)
{
    m_engine->effects()->setLoudnessTarget(db);
    emit effectsChanged();
}
QStringList PlayerController::eqBandLabels() const { return AudioEffects::bandFrequencies(); }

// 音效是否真的作用在音频流上 / 是否被旁路 / 任一开关是否打开
bool PlayerController::effectsLive() const { return m_engine->usingEffectPipeline(); }
bool PlayerController::effectsBypassed() const { return m_engine->effectsBypassed(); }
bool PlayerController::effectsOn() const
{
    auto *fx = m_engine->effects();
    return fx->eqEnabled() || fx->reverbEnabled() || fx->spatialEnabled() || fx->loudnessEnabled();
}
QString PlayerController::effectDeviceInfo() const
{
    if (!m_engine->usingEffectPipeline()) return QString();
    // 把"哪条解码路"也显示出来：用户能一眼确认 flac/m4a 是真被 FFmpeg 解了，
    // 而不是悄悄走了普通内核
    const QString be = m_engine->effectBackend();
    QString tail;
    if (be.compare(QLatin1String("ffmpeg"), Qt::CaseInsensitive) == 0) tail = QStringLiteral("FFmpeg");
    else if (!be.isEmpty()) tail = be;
    const QString dev = m_engine->audioDeviceName();
    const int rate = m_engine->effectOutputRate();
    QString out = QStringLiteral("%1 · %2Hz").arg(dev).arg(rate);
    if (!tail.isEmpty()) out += QStringLiteral(" · %1").arg(tail);
    return out;
}

QString PlayerController::formatTime(qint64 ms) const
{
    return Muyun::Format::duration(ms / 1000.0);
}

QString PlayerController::currentAudioLocalPath() const
{
    const QUrl u = m_engine->sourceUrl();
    return u.isLocalFile() ? u.toLocalFile() : QString();
}

// ---------------------------------------------------------------------------
// 播放控制
// ---------------------------------------------------------------------------

void PlayerController::playSong(const QVariantMap &song, const QVariantList &list,
                                const QString &playlistId, const QString &playlistName)
{
    const Song target = Song::fromMap(song);
    m_pendingNext.clear();   // 用户明确点歌播放：先前"下一首播放"的意图作废
    if (!list.isEmpty()) {
        m_playlist.clear();
        for (const auto &item : list) m_playlist.append(Song::fromMap(item.toMap()));
        emit playlistChanged();
    }
    if (!playlistId.isEmpty()) m_playlistId = playlistId;
    if (!playlistName.isEmpty()) m_playlistName = playlistName;

    // 定位索引
    int idx = -1;
    for (int i = 0; i < m_playlist.size(); ++i) {
        if (m_playlist.at(i).identityKey() == target.identityKey()) { idx = i; break; }
    }
    if (idx < 0) {
        m_playlist.prepend(target);
        idx = 0;
        emit playlistChanged();
    }
    m_currentIndex = idx;
    m_currentSong = m_playlist.at(idx);
    emit currentIndexChanged();
    emit currentSongChanged();

    loadLyric();
    resolveAndPlay();
    maybeResolveCover();
    armSongWatchdog();   // 用户点歌 → 重新武装单首时限
    saveState();
}

void PlayerController::playIndex(int index)
{
    startAt(index, true);
}

void PlayerController::startAt(int index, bool armWatchdog)
{
    if (index < 0 || index >= m_playlist.size()) return;
    m_currentIndex = index;
    m_currentSong = m_playlist.at(index);
    emit currentIndexChanged();
    emit currentSongChanged();
    loadLyric();
    resolveAndPlay();
    maybeResolveCover();
    if (armWatchdog) armSongWatchdog();   // 用户换歌/播完接续 → 重新武装单首时限
    saveState();
}

void PlayerController::insertNext(const QVariantMap &song)
{
    const Song s = Song::fromMap(song);
    if (s.id.isEmpty() && s.localPath.isEmpty()) return;

    // 无播放上下文：直接播放这首
    if (m_playlist.isEmpty() || m_currentIndex < 0) {
        playSong(song, QVariantList(), QString(), QString());
        return;
    }
    m_playlist.insert(m_currentIndex + 1, s);
    // 记入覆写队列：到下一曲时无视播放模式优先播它（随机/单曲循环下也生效）
    m_pendingNext.append(s.identityKey());
    emit playlistChanged();
    saveState();
}

void PlayerController::playAll(const QVariantList &songs, const QString &playlistId,
                               const QString &playlistName)
{
    if (songs.isEmpty()) return;
    m_playlist.clear();
    m_pendingNext.clear();   // 新队列上下文，旧的"下一首播放"意图作废
    for (const auto &item : songs) m_playlist.append(Song::fromMap(item.toMap()));
    m_playlistId = playlistId;
    m_playlistName = playlistName;
    emit playlistChanged();
    emit playlistNameChanged();
    playIndex(0);
}

void PlayerController::togglePlay()
{
    if (m_engine->isPlaying()) { m_engine->pause(); emit isPlayingChanged(); }
    else if (!m_currentSong.id.isEmpty()) {
        if (m_engine->hasSource()) {
            m_engine->resume();
        } else {
            // 重启恢复的歌曲（播放器无源）：重新解析播放，而非无效的 resume
            resolveAndPlay();
        }
        emit isPlayingChanged();
    }
    else if (!m_playlist.isEmpty()) playIndex(0);
}

void PlayerController::pause() { m_engine->pause(); disarmSongWatchdog(); m_durationCheckArmed = false; emit isPlayingChanged(); }
void PlayerController::resume() { m_engine->resume(); emit isPlayingChanged(); }
void PlayerController::stop() { m_engine->stop(); disarmSongWatchdog(); m_durationCheckArmed = false; emit isPlayingChanged(); }

void PlayerController::seek(qint64 ms) { m_pendingSeekMs = ms; m_seekClearTimer.start(); m_engine->seek(ms); emit positionChanged(); }

void PlayerController::seekRatio(qreal ratio)
{
    const qint64 total = m_engine->duration();
    if (total > 0) seek(static_cast<qint64>(total * qBound(0.0, ratio, 1.0)));
}

void PlayerController::cyclePlayMode()
{
    switch (m_playMode) {
    case PlayMode::Sequence: m_playMode = PlayMode::Loop; break;
    case PlayMode::Loop:     m_playMode = PlayMode::Single; break;
    case PlayMode::Single:   m_playMode = PlayMode::Shuffle; break;
    case PlayMode::Shuffle:  m_playMode = PlayMode::Sequence; break;
    }
    emit playModeChanged();
    saveState();
}

void PlayerController::setPlaybackRate(qreal rate) { m_engine->setPlaybackRate(rate); }

// ---------------------------------------------------------------------------
// 队列
// ---------------------------------------------------------------------------

void PlayerController::addToQueue(const QVariantMap &song)
{
    const Song s = Song::fromMap(song);
    const int insertAt = m_currentIndex >= 0 ? m_currentIndex + 1 : m_playlist.size();
    m_playlist.insert(insertAt, s);
    emit playlistChanged();
    saveState();
}

void PlayerController::removeFromQueue(int index)
{
    if (index < 0 || index >= m_playlist.size()) return;
    m_playlist.removeAt(index);
    if (m_currentIndex > index) {
        --m_currentIndex;
        emit currentIndexChanged();
    } else if (m_currentIndex == index && !m_playlist.isEmpty()) {
        m_currentIndex = qMin(m_currentIndex, m_playlist.size() - 1);
        m_currentSong = m_playlist.at(m_currentIndex);
        emit currentIndexChanged();
        emit currentSongChanged();
        // 删除的是当前曲：切播队列里的新当前曲，避免 UI 与声音不一致
        loadLyric();
        resolveAndPlay();
    } else if (m_playlist.isEmpty()) {
        // 队列删空：停止播放并清空当前曲。
        // 必须清 m_currentSong——否则正在异步解析中的旧任务完成后
        // identityKey 校验通过，会把刚删掉的歌又播出来（竞态）。
        m_currentIndex = -1;
        m_currentSong = Song();
        m_engine->stop();
        loadLyric();
        emit currentIndexChanged();
        emit currentSongChanged();
    }
    emit playlistChanged();
    saveState();
}

void PlayerController::clearQueue()
{
    m_playlist.clear();
    m_pendingNext.clear();
    m_currentIndex = -1;
    m_currentSong = Song();   // 同上：清空当前曲使在途解析任务失效
    m_engine->stop();
    loadLyric();
    emit playlistChanged();
    emit currentIndexChanged();
    emit currentSongChanged();
    saveState();
}

void PlayerController::dropLocalSong(const QString &localPath)
{
    if (localPath.isEmpty()) return;
    const bool curIsTarget = m_currentSong.isLocal() && m_currentSong.localPath == localPath;
    int removedBefore = 0;
    QVector<Song> kept;
    for (int i = 0; i < m_playlist.size(); ++i) {
        const Song &s = m_playlist.at(i);
        if (s.isLocal() && s.localPath == localPath) { if (i < m_currentIndex) ++removedBefore; continue; }
        kept.append(s);
    }
    if (kept.size() == m_playlist.size()) return;   // 队列里没有该曲，无需处理
    m_playlist = kept;
    if (curIsTarget) {
        // 当前正在播放的本地曲被删：停止并清当前（在线曲不受影响，只有删到自己才停）
        m_currentIndex = -1;
        m_currentSong = Song();
        m_engine->stop();
        loadLyric();
        emit currentIndexChanged();
        emit currentSongChanged();
    } else {
        m_currentIndex -= removedBefore;
        if (m_currentIndex >= m_playlist.size()) m_currentIndex = m_playlist.isEmpty() ? -1 : 0;
        if (m_currentIndex < 0 && !m_playlist.isEmpty()) m_currentIndex = 0;
    }
    emit playlistChanged();
    saveState();
}

void PlayerController::shufflePlaylist()
{
    if (m_playlist.isEmpty()) return;
    auto *rng = QRandomGenerator::global();
    for (int i = m_playlist.size() - 1; i > 0; --i)
        m_playlist.swapItemsAt(i, static_cast<int>(rng->bounded(i + 1)));
    // 当前歌曲移到最前
    if (m_currentIndex > 0 && m_currentIndex < m_playlist.size()) {
        const Song cur = m_currentSong;
        m_playlist.move(m_currentIndex, 0);
        m_currentIndex = 0;
        m_currentSong = cur;
        emit currentIndexChanged();
    }
    emit playlistChanged();
    saveState();
}

// ---------------------------------------------------------------------------
// 内部
// ---------------------------------------------------------------------------

void PlayerController::resolveAndPlay()
{
    m_wantedQuality = m_quality;   // 常规播放发起：用户想要的档 = 当前选择（缓存只命中这一档）
    m_durationCheckArmed = true;   // 发起播放：武装时长校验（开播校验一次后自动 disarm）
    // ⚠ 换曲瞬间就作废上一首的实测档并通知刷新：否则新曲走下载期间（无缓存、要几秒），
    //   currentQualityLabel 仍返回上一首的 m_actualQuality → "自动切下一首后标签停在上一首档位"。
    //   先回落显示用户选择的档（m_quality），本曲真正播成时 resolveAndPlayAt 会再设实测并 emit。
    m_actualQualityKnown = false;
    emit currentQualityChanged();
    if (m_currentSong.isLocal()) {
        m_engine->playFile(m_currentSong.localPath);
        return;
    }
    m_playRetry = 0;
    m_wasPlaying = false;
    resolveAndPlayAt(m_quality);
}

// ---------------------------------------------------------------------------
// 单首取音频总时限（30s，跨所有降档重试）
// ---------------------------------------------------------------------------
// 背景：在线歌取不到链接时会沿音质降级链逐档真请求，每档 HttpClient 20s 超时，
// 6 档最坏要等 2 分钟。这段时间 UI 钉在这首歌上（loading 转圈），用户体验就是
// 「一首歌播完以后还是这一首歌」。到点仍没出声 → 按失败处理、前进到下一首，
// 而不是无限等下去。只在"用户选歌/换歌/恢复状态"时重新武装；
// advanceOnPlayFailure 故意不武装（见头文件注释）。
void PlayerController::armSongWatchdog()
{
    m_songWatchdog.start(songWatchdogMs());
}

bool PlayerController::songWatchdogActive() const
{
    return m_songWatchdog.isActive();
}

void PlayerController::disarmSongWatchdog()
{
    m_songWatchdog.stop();
}

void PlayerController::onSongWatchdogTimeout()
{
    // 已经出声、或压根没在加载 → 无需处理（防误报：失败降档链走完后 m_loading 已清）
    if (m_engine->state() == PlayerEngine::State::Playing || !m_loading)
        return;
    m_loading = false;
    emit isLoadingChanged();
    emit playFailed(QStringLiteral("这首歌迟迟取不到音频，已跳过"));
    if (m_playlist.size() <= 1) { emit isPlayingChanged(); return; }  // 只有这一首：跳无可跳，停下
    // 前进到下一首（随机模式牌堆天然不回弹；顺序播放到末尾才停）
    int idx = nextIndexByMode(false);
    if (idx < 0) { emit isPlayingChanged(); return; }
    if (idx == m_currentIndex && m_playlist.size() > 1)
        idx = (m_currentIndex + 1) % m_playlist.size();
    if (idx >= 0 && idx < m_playlist.size()) startAt(idx, false);
}

void PlayerController::resolveAndPlayAt(AudioQuality startQ)
{
    const Song song = m_currentSong;
    m_durationCheckArmed = true;   // 每次发起取源/播放都重新武装时长校验（开播校验一次即 disarm）

    // 播放缓存优先：但**只命中"用户选择的那一档"（m_wantedQuality）的缓存**。
    //   绝不能让更低档的旧缓存顶替用户的选择——否则换到一首之前播过低档、留了旧缓存的歌时，
    //   会直接播旧档并把播放条标签显示成旧的（与音质菜单所选不符，要手动重选才刷新）。
    //   起点档没有有效缓存 → 走下面的下载（下载内部才沿降级链取源，拿到什么档就显示什么档）。
    // 损坏/不完整的缓存由 verifyCacheDecodable 硬校验兜底：坏文件删掉 → 视作无缓存 → 下载。
    {
        const QString cached = PlayerEngine::cachedAudioFile(
            song.identityKey() + QLatin1Char('@') + Muyun::qualityId(m_wantedQuality), song.duration);
        if (!cached.isEmpty()) {
            if (!PlayerEngine::verifyCacheDecodable(cached, song.duration)) {
                QFile::remove(cached);   // 坏缓存：删掉，绝不进播放器，转去下载
            } else {
                m_playRetry = 0;
                m_playTriedQ = m_wantedQuality;
                m_actualQuality = m_wantedQuality;
                m_actualQualityKnown = true;
                emit currentQualityChanged();
                m_engine->playFile(cached);
                return;
            }
        }
    }

    // 无有效缓存 → 重新获取数据。
    // 新流程：获取数据 → 对比时长 → 匹配则播放 → 不匹配/失败则降档重试 → 全败跳过。
    m_loading = true;
    emit isLoadingChanged();

    m_playTriedQ = startQ;

    auto *watcher = new QFutureWatcher<QPair<QString, AudioQuality>>(this);
    connect(watcher, &QFutureWatcher<QPair<QString, AudioQuality>>::finished,
            this, [this, watcher, song]() {
                const auto result = watcher->result();
                watcher->deleteLater();
                m_loading = false;
                emit isLoadingChanged();

                // 歌曲可能已被切换，忽略过期结果
                if (m_currentSong.identityKey() != song.identityKey()) return;

                if (result.first.isEmpty()) {
                    // 该音质取不到链接 → 降一档再试；到底仍失败才报错
                    AudioQuality lower;
                    if (m_playRetry < 6 && lowerQuality(m_playTriedQ, &lower)) {
                        ++m_playRetry;
                        emit playFailed(QStringLiteral("该音质不可用，正在降档重试…"));
                        resolveAndPlayAt(lower);
                    } else {
                        advanceOnPlayFailure(QStringLiteral("无法获取播放链接，尝试其它音源"));
                    }
                    return;
                }
                // 内部记录脚本实际给的档（供降档/校验等逻辑用）；对外标签恒等于用户选择，
                // 不受这里影响（见 currentQualityLabel）。
                m_actualQuality = result.second;
                m_actualQualityKnown = true;
                emit currentQualityChanged();
                // 缓存 key = 歌曲稳定身份 + 实际音质（在线 URL 每次签名不同，不能拿 URL 当 key）
                m_engine->play(QUrl(result.first),
                               song.identityKey() + QLatin1Char('@') + Muyun::qualityId(result.second));
                // 无论哪条播放路径（列表播放/重启恢复/直接播放）都确保歌词被加载
                loadLyric();
            });

    MusicSdk *sdk = MusicSdk::instance();
    // 该歌曲的取源平台记忆（主线程查，worker 线程不碰共享 map）
    const QString preferPlat = m_songPlatform.value(song.identityKey());
    watcher->setFuture(QtConcurrent::run([sdk, song, startQ, preferPlat]() {
        AudioQuality actual = startQ;
        Song playSong = song;
        const QString ownCode = song.lx.source.isEmpty()
                                    ? platformSourceCode(song.platform) : song.lx.source;
        // 用户为这首歌指定了取源平台且与原平台不同 → 跨平台找同名歌，从指定平台拉
        if (!preferPlat.isEmpty() && preferPlat != ownCode) {
            FindMusicRequest req;
            req.name = song.name;
            req.singer = song.artist;
            req.albumName = song.album;
            req.interval = Format::duration(song.duration);
            req.source = ownCode;
            const QVector<Song> cands = sdk->findMusic(req, 8);
            for (const auto &c : cands) {
                const QString cc = c.lx.source.isEmpty()
                                       ? platformSourceCode(c.platform) : c.lx.source;
                if (cc == preferPlat) { playSong = c; break; }
            }
        }
        const QString url = sdk->resolveUrl(playSong, startQ, &actual);
        return qMakePair(url, actual);
    }));
}

int PlayerController::nextIndexByMode(bool userTriggered)
{
    Q_UNUSED(userTriggered)
    if (m_playlist.isEmpty()) return -1;
    const int n = m_playlist.size();

    // "下一首播放"覆写：用户右键插的歌优先，无视播放模式（随机/单曲循环也生效）。
    // 取第一个仍在队列里、且不是当前曲的；已播/被删的自动作废顺延到下一个。
    while (!m_pendingNext.isEmpty()) {
        const QString key = m_pendingNext.takeFirst();
        if (key == m_currentSong.identityKey()) continue;
        const int base = m_currentIndex < 0 ? -1 : m_currentIndex;
        for (int step = 1; step <= n; ++step) {
            const int i = (base + step + n * 2) % n;   // 从当前曲向后环形查找
            if (m_playlist.at(i).identityKey() == key) return i;
        }
    }

    switch (m_playMode) {
    case PlayMode::Single:
        return m_currentIndex;
    case PlayMode::Shuffle:
        return nextShuffleIndex();
    case PlayMode::Loop:
        return (m_currentIndex + 1) % n;
    case PlayMode::Sequence:
    default:
        if (m_currentIndex + 1 >= n) return -1; // 顺序播放到末尾停止
        return m_currentIndex + 1;
    }
}

// ---------------------------------------------------------------------------
// 随机播放：不重复牌堆
// ---------------------------------------------------------------------------
// 旧实现是"每次独立均匀随机"——同一首歌在短时间内被再抽中的概率极高
// （歌单小的时候尤其明显，用户反馈"随机容易重复播放"）。改为：
//   - 维护一张"本轮播放顺序"牌堆（播放列表索引的洗牌序列），下一曲按序取下一张
//     → 一轮之内绝不重复、每首必播；
//   - 一轮播完（牌堆走完）→ 重洗开新一轮继续（随机播放不因一轮结束而停）；
//   - 当前曲永远放在牌堆首：随机从"现在"继续，而不是跳到队列另一头；
//   - 上一首 = 本轮牌堆的前一张（真正"回到上一首"），比旧版"再随机一次"自然得多。

void PlayerController::rebuildShuffleDeck()
{
    m_shuffleDeck.clear();
    const int n = m_playlist.size();
    if (n <= 0) return;
    for (int i = 0; i < n; ++i) m_shuffleDeck.append(i);
    auto *rng = QRandomGenerator::global();
    for (int i = n - 1; i > 0; --i)
        m_shuffleDeck.swapItemsAt(i, static_cast<int>(rng->bounded(i + 1)));
    // 当前曲放到牌堆首
    if (m_currentIndex >= 0) {
        const int cur = m_shuffleDeck.indexOf(m_currentIndex);
        if (cur > 0) m_shuffleDeck.move(cur, 0);
    }
}

int PlayerController::nextShuffleIndex()
{
    const int n = m_playlist.size();
    if (n <= 1) return 0;
    // 队列增删/换列表后大小对不上 → 牌堆作废重建
    if (m_shuffleDeck.size() != n) rebuildShuffleDeck();
    // 当前曲可能不在牌堆里（手动点歌/重启恢复）→ 重建（当前曲会到队首）
    int pos = m_shuffleDeck.indexOf(m_currentIndex);
    if (pos < 0) {
        rebuildShuffleDeck();
        pos = m_shuffleDeck.indexOf(m_currentIndex);
    }
    if (pos < 0)   // 极端兜底（理论上到不了）：退化成纯随机
        return static_cast<int>(QRandomGenerator::global()->bounded(n));
    if (pos + 1 < m_shuffleDeck.size()) return m_shuffleDeck.at(pos + 1);
    // 一轮播完 → 重洗开新一轮；当前曲在新牌堆首，下一曲取第 2 张（必非当前曲）
    rebuildShuffleDeck();
    return m_shuffleDeck.size() > 1 ? m_shuffleDeck.at(1) : m_shuffleDeck.at(0);
}

int PlayerController::prevShuffleIndex()
{
    const int n = m_playlist.size();
    if (n <= 1) return 0;
    if (m_shuffleDeck.size() != n) rebuildShuffleDeck();
    const int pos = m_shuffleDeck.indexOf(m_currentIndex);
    if (pos > 0) return m_shuffleDeck.at(pos - 1);   // 本轮内"上一首"就是牌堆前一张
    return m_currentIndex;                            // 已在牌堆首：留在当前曲（重新开始）
}

void PlayerController::next()
{
    const int idx = nextIndexByMode(true);
    if (idx < 0) { m_engine->stop(); emit isPlayingChanged(); return; }
    playIndex(idx);
}

void PlayerController::previous()
{
    if (m_playlist.isEmpty()) return;
    // 播放超过 3 秒时，上一首先回到开头
    if (m_engine->position() > 3000) { seek(0); return; }
    const int idx = m_playMode == PlayMode::Shuffle
                        ? prevShuffleIndex()
                        : qMax(0, m_currentIndex - 1);
    playIndex(idx);
}

void PlayerController::onEndOfMedia()
{
    m_lastFinishedIndex = m_currentIndex;   // 记住刚播完的曲，advanceOnPlayFailure 跳过它
    const int idx = nextIndexByMode(false);
    if (idx < 0) { emit isPlayingChanged(); return; }
    playIndex(idx);
}

void PlayerController::onEngineError(const QString &message)
{
    // ⚠ 先删坏缓存——**无论是否已开播**（这是 1.1.7 仍"播放失败、手清缓存才好"的根因）：
    //   缓存命中坏文件时，QMediaPlayer 打开即失败、**从未进入 Playing** → m_wasPlaying 一直是
    //   false → 若只在下面 m_wasPlaying 分支里删缓存，坏文件永远不会被删；而降档重试走
    //   resolveAndPlayAt，缓存查询用的是 m_wantedQuality（用户选的档，降档递归**不变**）→
    //   于是每次都命中同一个坏缓存 → 6 次降档全撞同一个坏文件 → advanceOnPlayFailure
    //   → 表现为"怎么都播不了，手动清 %TEMP%/muyun-audio 就好了"。
    //   删掉后：降档重试查同一 key → 已不存在 → 转去下载 → 拿到完整文件 → 正常播放。
    //
    //   **即时失败（m_wasPlaying==false）** 额外清光同歌所有音质档的缓存：
    //   坏缓存可能不止当前这一档（上次降档链留下的其他档也可能坏），
    //   清干净后降档重试不会碰到第二块坏石头。
    const bool wasPlaying = m_wasPlaying;   // 记住原始值，后面判断"即时失败 vs 中途断开"
    {
        const QString songKey = m_currentSong.identityKey();
        if (wasPlaying) {
            // 中途断开：只删当前音质这一档
            const QString badKey = songKey + QLatin1Char('@') + Muyun::qualityId(m_playTriedQ);
            const QString badPath = PlayerEngine::cachePathForKey(badKey);
            if (QFile::exists(badPath)) QFile::remove(badPath);
        } else {
            // 即时失败（缓存坏/QMediaPlayer 打开即报错）：清光所有档
            const auto allQ = {AudioQuality::Master, AudioQuality::Atmos, AudioQuality::HiRes,
                               AudioQuality::Flac24Bit, AudioQuality::Flac, AudioQuality::K320,
                               AudioQuality::K128};
            for (auto q : allQ) {
                const QString badPath = PlayerEngine::cachePathForKey(
                    songKey + QLatin1Char('@') + Muyun::qualityId(q));
                if (QFile::exists(badPath)) QFile::remove(badPath);
            }
        }
    }

    // 开播后失败：如果位置已贴末尾（实质播完了）→ 不重试，直接前进。
    //   但如果在中间（临时中断/缓存损坏）→ 仍走降档重试，别误跳。
    if (wasPlaying) {
        m_wasPlaying = false;
        const qint64 dur = m_engine->duration();
        const qint64 pos = m_engine->position();
        if (dur > 0 && pos >= dur - 3000) {
            // 贴末尾 → 视为播完，静默前进
            ++m_failStreak;
            int idx = nextIndexByMode(false);
            if (idx == m_currentIndex || idx == m_lastFinishedIndex)
                idx = (idx + 1) % m_playlist.size();
            if (idx >= 0 && idx < m_playlist.size()) {
                startAt(idx, false);
            } else {
                m_failStreak = 0;
                emit isPlayingChanged();
            }
            return;
        }
        // 中间中断 → 走正常降档重试（m_playRetry 不重置，沿用当前计数）
    }
    // 在线歌：QMediaPlayer 打不开（音源返回坏/失效文件）→ 自动降一档重试，
    // 逐级降到底仍失败才报错（用户"换源才好"的体验由这里兜底）。
    // ⚠ 即时失败（wasPlaying==false，坏缓存被清后重试）→ **不弹报错**，静默降档。
    //   用户看到的就是"加载→出声"，不会看到"音源返回的不是有效音频"那种旧报错。
    if (!m_currentSong.isLocal() && m_playRetry < 6) {
        AudioQuality lower;
        if (lowerQuality(m_playTriedQ, &lower)) {
            ++m_playRetry;
            if (wasPlaying) {
                emit playFailed(QStringLiteral("当前音质无法播放，降档重试…"));
            }
            resolveAndPlayAt(lower);
            return;
        }
    }
    advanceOnPlayFailure(message.isEmpty() ? QStringLiteral("播放失败") : message);
}

// 在线歌彻底失败（降档重试都用了）→ 直接播下一首而不是停在原地等用户点。
// 连败熔断：整队列都拉不到时跳了 N 首仍失败 → 止损报错，不死循环。
// 本地歌失败不跳（多半是文件没了，跳了也没意义）。
// 熔断后不清零 m_failStreak 之外的状态，但**重置熔断计数**：
//   否则用户稍后手动换歌会一直被"上一轮的连败"挡着。
void PlayerController::advanceOnPlayFailure(const QString &reason)
{
    const int limit = qMax(6, m_playlist.size());
    if (!m_currentSong.isLocal() && m_playlist.size() > 1 && m_failStreak < limit) {
        ++m_failStreak;
        int idx = nextIndexByMode(false);
        if (idx == m_currentIndex || idx == m_lastFinishedIndex)
            // 牌堆回绕可能指回刚播完/正失败的曲 → 强制顺跳一格，别原地打转
            idx = (idx + 1) % m_playlist.size();
        if (idx >= 0 && idx < m_playlist.size()) {
            emit playFailed(reason + QStringLiteral("，已自动播放下一首"));
            // arm=false：整队列都挂时不续命单首时限（限时预算耗尽即停）
            startAt(idx, false);
            return;
        }
        // idx<0：顺序播放已到队尾 → 按常规停下
    }
    m_failStreak = 0;
    emit playFailed(reason);
}

void PlayerController::loadLyric()
{
    m_lyric = SongLyric();
    m_currentLyricText.clear();
    m_currentLyricTranslation.clear();
    m_lyricLoading = true;
    emit lyricChanged();
    emit currentLyricChanged();
    emit lyricLoadingChanged();
    if (m_currentSong.id.isEmpty()) {
        m_lyricLoading = false;
        emit lyricLoadingChanged();
        return;
    }

    const Song song = m_currentSong;
    auto *watcher = new QFutureWatcher<SongLyric>(this);
    connect(watcher, &QFutureWatcher<SongLyric>::finished, this, [this, watcher, song]() {
        const SongLyric lyric = watcher->result();
        watcher->deleteLater();
        if (m_currentSong.identityKey() != song.identityKey()) return; // 已切歌，新一轮加载还在跑
        m_lyric = lyric;
        m_lyricLoading = false;
        emit lyricChanged();
        emit lyricLoadingChanged();
        updateCurrentLyric();
    });
    MusicSdk *sdk = MusicSdk::instance();
    watcher->setFuture(QtConcurrent::run([sdk, song]() {
        SongLyric lyric;
        if (song.isLocal()) {
            // 本地歌曲：优先外挂 .lrc
            const QString lrcPath = song.localPath.left(song.localPath.lastIndexOf(QLatin1Char('.'))) +
                                    QStringLiteral(".lrc");
            QFile f(lrcPath);
            if (f.open(QIODevice::ReadOnly)) {
                lyric = LyricParser::parseLrc(QString::fromUtf8(f.readAll()));
                f.close();
            }
            // 外挂没有 → 读内嵌歌词（下载时 TagWriter 写入的 USLT/LYRICS）
            if (lyric.lines.isEmpty()) {
                const LocalTags emb = TagReader::readTags(song.localPath);
                if (!emb.lyrics.isEmpty())
                    lyric = LyricParser::parseLrc(emb.lyrics);
            }
            return lyric;
        }
        // 歌词接口偶发失败（实测 rawLen=0 概率出现），按"解析后无行"判定，最多重试 3 次
        SongLyric rawFinal;
        for (int attempt = 0; attempt < 3 && !HttpClient::shuttingDown(); ++attempt) {
            rawFinal = sdk->resolveLyric(song);
            if (!rawFinal.rawLrc.isEmpty()) {
                // 确认能解析出至少一行歌词才算成功
                const SongLyric probe = LyricParser::parseLrc(rawFinal.rawLrc);
                if (!probe.lines.isEmpty()) break;
            }
            if (attempt < 2) QThread::msleep(400 * (attempt + 1)); // 400/800ms 递增退避
        }
        lyric = LyricParser::parseLrc(rawFinal.rawLrc);
        lyric.rawLrc = rawFinal.rawLrc;
        if (!rawFinal.rawTranslation.isEmpty()) {
            LyricParser::mergeTranslation(lyric, rawFinal.rawTranslation, false);
            lyric.rawTranslation = rawFinal.rawTranslation;
            lyric.hasTranslation = true;
        }
        if (!rawFinal.rawRoman.isEmpty()) {
            LyricParser::mergeTranslation(lyric, rawFinal.rawRoman, true);
            lyric.rawRoman = rawFinal.rawRoman;
            lyric.hasRoman = true;
        }
        return lyric;
    }));
}

void PlayerController::reloadLyric()
{
    // 只重拉歌词数据，播放进度不受影响
    loadLyric();
    updateCurrentLyric();
}

void PlayerController::saveState()
{
    auto *store = DocumentStore::instance();
    store->write(QStringLiteral("player"), QStringLiteral("volume"), m_engine->volume());
    store->write(QStringLiteral("player"), QStringLiteral("isMuted"), m_engine->isMuted());
    store->write(QStringLiteral("player"), QStringLiteral("playMode"), playModeId());
    store->write(QStringLiteral("player"), QStringLiteral("quality"), qualityId());
    store->write(QStringLiteral("player"), QStringLiteral("playlistId"), m_playlistId);
    store->write(QStringLiteral("player"), QStringLiteral("playlistName"), m_playlistName);
    store->write(QStringLiteral("player"), QStringLiteral("currentSong"), m_currentSong.toMap());
    // 队列快照 + 当前索引（重启后播放队列不丢失）
    QVariantList list;
    for (const auto &s : m_playlist) list.append(s.toMap());
    store->write(QStringLiteral("player"), QStringLiteral("queue"), list);
    store->write(QStringLiteral("player"), QStringLiteral("currentIndex"), m_currentIndex);
}

void PlayerController::restoreState()
{
    auto *store = DocumentStore::instance();
    const qreal vol = store->readSync(QStringLiteral("player"), QStringLiteral("volume"), 0.8).toDouble();
    m_engine->setVolume(vol);
    m_engine->setMuted(store->readSync(QStringLiteral("player"), QStringLiteral("isMuted"), false).toBool());
    m_playMode = Muyun::playModeFromId(
        store->readSync(QStringLiteral("player"), QStringLiteral("playMode"),
                        QStringLiteral("sequence")).toString());
    bool ok = false;
    m_quality = Muyun::qualityFromId(
        store->readSync(QStringLiteral("player"), QStringLiteral("quality"),
                        QStringLiteral("320k")).toString(), &ok);
    m_playlistId = store->readSync(QStringLiteral("player"), QStringLiteral("playlistId")).toString();
    m_playlistName = store->readSync(QStringLiteral("player"), QStringLiteral("playlistName")).toString();
    // 按歌曲记忆的取源平台覆盖表（identityKey → 平台）
    const QVariantMap spDoc = store->readSync(QStringLiteral("player"),
                                              QStringLiteral("songPlatforms")).toMap();
    for (auto i = spDoc.constBegin(); i != spDoc.constEnd(); ++i)
        m_songPlatform.insert(i.key(), i.value().toString());
    const QVariantMap songMap =
        store->readSync(QStringLiteral("player"), QStringLiteral("currentSong")).toMap();
    if (!songMap.isEmpty()) {
        m_currentSong = Song::fromMap(songMap);
        emit currentSongChanged();
        loadLyric();  // 恢复上次歌曲的歌词
    }

    // 恢复播放队列（旧版本存档无 queue 键时走下方兜底）
    const QVariantList queueList =
        store->readSync(QStringLiteral("player"), QStringLiteral("queue")).toList();
    if (!queueList.isEmpty() && !m_currentSong.id.isEmpty()) {
        m_playlist.clear();
        for (const auto &item : queueList) m_playlist.append(Song::fromMap(item.toMap()));
        // 按 identityKey 重新定位当前曲（防止存档错位）
        int idx = -1;
        for (int i = 0; i < m_playlist.size(); ++i) {
            if (m_playlist.at(i).identityKey() == m_currentSong.identityKey()) { idx = i; break; }
        }
        if (idx >= 0) {
            m_currentIndex = idx;
        } else {
            // 当前曲不在队列快照里（理论上不该发生）：插回队首
            m_playlist.prepend(m_currentSong);
            m_currentIndex = 0;
        }
        emit playlistChanged();
        emit currentIndexChanged();
    } else if (queueList.isEmpty() && !m_currentSong.id.isEmpty()) {
        // 兼容旧存档：至少让当前播放的歌出现在队列里
        m_playlist.clear();
        m_playlist.append(m_currentSong);
        m_currentIndex = 0;
        emit playlistChanged();
        emit currentIndexChanged();
    }
}

// 预缓存重启：切歌/歌单变化时，让 AudioPreloader 从当前曲之后逐首下载到缓存
void PlayerController::restartPreloader()
{
    if (!m_preloader) return;
    m_preloader->restart(m_playlist, m_currentIndex, m_quality);
}

} // namespace Muyun
