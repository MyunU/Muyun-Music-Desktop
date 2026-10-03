#include "PlayerController.h"

#include "core/music/MusicSdk.h"
#include "core/lyrics/LyricParser.h"
#include "core/localmusic/TagReader.h"
#include "core/storage/DocumentStore.h"
#include "core/utils/Format.h"

#include <QtConcurrent>
#include <QFutureWatcher>
#include <QThread>
#include <QUrl>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QDateTime>
#include <algorithm>

namespace Muyun {

PlayerController::PlayerController(QObject *parent) : QObject(parent)
{
    m_engine = new PlayerEngine(this);

    connect(m_engine, &PlayerEngine::positionChanged, this,
            [this]() { emit positionChanged(); updateCurrentLyric(); });
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
                if (m_engine->state() == PlayerEngine::State::Playing)
                    m_failStreak = 0;   // 真播起来了：失败熔断计数清零
                emit isPlayingChanged();
            });
    connect(m_engine, &PlayerEngine::endOfMedia, this, &PlayerController::onEndOfMedia);
    connect(m_engine, &PlayerEngine::errorOccurred, this, &PlayerController::onEngineError);
    // 音效管线接管/回退 → 通知 UI（入口高亮、面板状态行）
    connect(m_engine, &PlayerEngine::effectsRuntimeChanged, this,
            [this]() { emit effectsRuntimeChanged(); emit isPlayingChanged(); });

    restoreState();
}

// ---------------------------------------------------------------------------
// 属性
// ---------------------------------------------------------------------------

QVariantMap PlayerController::currentSongMap() const { return m_currentSong.toMap(); }
bool PlayerController::isPlaying() const { return m_engine->isPlaying(); }
qint64 PlayerController::position() const { return m_engine->position(); }
qint64 PlayerController::duration() const { return m_engine->duration(); }
qreal PlayerController::volume() const { return m_engine->volume(); }
bool PlayerController::muted() const { return m_engine->isMuted(); }

QString PlayerController::playModeId() const { return Muyun::playModeId(m_playMode); }
QString PlayerController::playModeName() const { return Muyun::playModeName(m_playMode); }
QString PlayerController::qualityId() const { return Muyun::qualityId(m_quality); }

QString PlayerController::currentQualityLabel() const
{
    if (m_currentSong.isLocal()) return QStringLiteral("本地文件");
    if (!m_actualQualityKnown) return QStringLiteral("获取中");
    return Muyun::qualityName(m_actualQuality);
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
    emit qualityChanged();
    saveState();
    // 切换音质后重新解析当前歌曲
    if (!m_currentSong.id.isEmpty() && !m_currentSong.isLocal()) resolveAndPlay();
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
    saveState();
}

void PlayerController::playIndex(int index)
{
    if (index < 0 || index >= m_playlist.size()) return;
    m_currentIndex = index;
    m_currentSong = m_playlist.at(index);
    emit currentIndexChanged();
    emit currentSongChanged();
    loadLyric();
    resolveAndPlay();
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

void PlayerController::pause() { m_engine->pause(); emit isPlayingChanged(); }
void PlayerController::resume() { m_engine->resume(); emit isPlayingChanged(); }
void PlayerController::stop() { m_engine->stop(); emit isPlayingChanged(); }

void PlayerController::seek(qint64 ms) { m_engine->seek(ms); emit positionChanged(); }

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

// 取更低的音质（枚举 K128=0 … Master=6，降档即 -1）；已是最低则返回 false
static bool lowerQuality(AudioQuality q, AudioQuality *out)
{
    const int v = static_cast<int>(q);
    if (v <= 0) return false;
    *out = static_cast<AudioQuality>(v - 1);
    return true;
}

void PlayerController::resolveAndPlay()
{
    if (m_currentSong.isLocal()) {
        m_actualQualityKnown = false;
        emit currentQualityChanged();
        m_engine->playFile(m_currentSong.localPath);
        return;
    }
    m_playRetry = 0;
    resolveAndPlayAt(m_quality);
}

void PlayerController::resolveAndPlayAt(AudioQuality startQ)
{
    const Song song = m_currentSong;

    // 播放缓存优先：按降级链查已缓存文件，命中则直接播本地文件——完全绕开音源解析
    // （缓存过的歌重播无需音源脚本在线拉取，音源下线/无网也能播）
    for (const AudioQuality q : qualityFallbackChain(startQ)) {
        const QString cached = PlayerEngine::cachedAudioFile(
            song.identityKey() + QLatin1Char('@') + Muyun::qualityId(q));
        if (cached.isEmpty()) continue;
        m_playRetry = 0;
        m_playTriedQ = q;
        m_actualQuality = q;
        m_actualQualityKnown = true;
        emit currentQualityChanged();
        m_engine->playFile(cached);
        return;
    }

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
        if (n <= 1) return 0;
        return static_cast<int>(QRandomGenerator::global()->bounded(n));
    case PlayMode::Loop:
        return (m_currentIndex + 1) % n;
    case PlayMode::Sequence:
    default:
        if (m_currentIndex + 1 >= n) return -1; // 顺序播放到末尾停止
        return m_currentIndex + 1;
    }
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
                        ? static_cast<int>(QRandomGenerator::global()->bounded(m_playlist.size()))
                        : qMax(0, m_currentIndex - 1);
    playIndex(idx);
}

void PlayerController::onEndOfMedia()
{
    const int idx = nextIndexByMode(false);
    if (idx < 0) { emit isPlayingChanged(); return; }
    playIndex(idx);
}

void PlayerController::onEngineError(const QString &message)
{
    // 在线歌：QMediaPlayer 打不开（音源返回坏/失效文件）→ 自动降一档重试，
    // 逐级降到底仍失败才报错（用户"换源才好"的体验由这里兜底）
    if (!m_currentSong.isLocal() && m_playRetry < 6) {
        AudioQuality lower;
        if (lowerQuality(m_playTriedQ, &lower)) {
            ++m_playRetry;
            emit playFailed(QStringLiteral("当前音质无法播放，降档重试…"));
            resolveAndPlayAt(lower);
            return;
        }
    }
    advanceOnPlayFailure(message.isEmpty() ? QStringLiteral("播放失败") : message);
}

// 在线歌彻底失败（降档重试都用了）→ 直接播下一首而不是停在原地等用户点。
// 连败熔断：整队列都拉不到时跳了 N 首仍失败 → 止损报错，不死循环。
// 本地歌失败不跳（多半是文件没了，跳了也没意义）。
void PlayerController::advanceOnPlayFailure(const QString &reason)
{
    if (!m_currentSong.isLocal() && m_playlist.size() > 1 &&
        m_failStreak < qMax(6, m_playlist.size())) {
        ++m_failStreak;
        int idx = nextIndexByMode(false);
        if (idx == m_currentIndex)                    // 单曲循环：强制前进一格，别卡在同一首
            idx = (m_currentIndex + 1) % m_playlist.size();
        if (idx >= 0 && idx < m_playlist.size()) {
            emit playFailed(reason + QStringLiteral("，已自动播放下一首"));
            playIndex(idx);
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
        for (int attempt = 0; attempt < 3; ++attempt) {
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

} // namespace Muyun
