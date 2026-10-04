#include "PlayerEngine.h"

#include "EffectPlayer.h"
#include "PcmSource.h"
#include "core/network/HttpClient.h"
#include "core/utils/AudioUrl.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QDebug>
#include <QMetaObject>
#include <QMediaDevices>
#include <QDateTime>
#include <QElapsedTimer>

namespace Muyun {

// 音效运行时管线总开关。true = 音效开启且音源为可解码 MP3 时走 EffectPlayer（真出声）。
// 旧的"主线程同步解码整曲 + 设备格式写死"两个致命问题已解决（见 EffectPlayer 注释）。
static constexpr bool kEffectsRuntimeEnabled = true;

PlayerEngine::PlayerEngine(QObject *parent) : QObject(parent)
{
    m_effects = new AudioEffects(this);

    // QMediaPlayer 与 QAudioOutput 均懒创建（见 ensurePlayer/ensureAudioOutput）：
    // 探针实测 Qt6.8 FFmpeg 后端在 QMediaPlayer 构造时就建内部 AudioWorker/QAudioSink，
    // 打开 WASAPI 会话唤醒蓝牙耳机 → 启动瞬间电流声。首次播放才创建。

    m_positionTimer.setInterval(200);
    connect(&m_positionTimer, &QTimer::timeout, this, [this]() {
        // 音效管线的进度由 EffectPlayer 自己按时上报（它才知道交付了多少帧），
        // 这里只管 QMediaPlayer 路径，避免双份 positionChanged
        if (m_useEffect) return;
        if (m_playing && m_player) emit positionChanged(m_player->position());
    });

    // 起播 nudge：部分文件（尤其某些 mp3/带大 ID3 的）QMediaPlayer 的 play() 不激活 FFmpeg 解码器，
    // position 卡 0，必须有一次 seek 才起播（用户"拖进度条才播"就是这个）。起播后每 500ms 检查一次，
    // 若仍在播放但位置没动就补 setPosition(0)，最多 4 次直到解码器起来。
    m_nudgeTimer.setInterval(500);
    connect(&m_nudgeTimer, &QTimer::timeout, this, [this]() {
        if (m_useEffect || !m_player) { m_nudgeTimer.stop(); return; }
        if (m_player->position() >= 50) { m_nudgeTimer.stop(); return; }   // 已正常播放
        if (m_player->playbackState() != QMediaPlayer::PlayingState) { m_nudgeTimer.stop(); return; }
        if (++m_nudgeCount > 4) { m_nudgeTimer.stop(); return; }
        // 补 seek 激活解码；设备切换恢复未跳成时以目标位置为锚（否则会被拉回 0）
        const qint64 target = (m_resumeAfterSwitch && m_resumedPosition > 0)
                                  ? m_resumedPosition : 0;
        m_player->setPosition(target);
    });

    // 末尾 / 卡死兜底巡检（只在 QMediaPlayer 路径且真在自播时武装，见 m_watchArmed）
    // 常驻开着：未武装时首次判断就返回，500ms 一次空转开销可忽略（位置巡检本来就有）
    m_watchTimer.setInterval(500);
    connect(&m_watchTimer, &QTimer::timeout, this, [this]() {
        if (!m_watchArmed || m_useEffect || !m_player) { m_watchTimer.stop(); return; }
        const QMediaPlayer::PlaybackState st = m_player->playbackState();
        const qint64 pos = m_player->position();
        const qint64 dur = m_player->duration();
        if (st == QMediaPlayer::StoppedState) {
            // 播到末尾却只收到 StoppedState（EndOfMedia 丢了）→ 位置已贴到末尾就补发一次
            if (dur > 0 && pos >= dur - 1500) {
                m_watchArmed = false;
                m_stallTicks = 0;
                m_lastWatchPos = -1;
                m_playing = false;
                m_loading = false;
                m_positionTimer.stop();
                emit endOfMedia();
            }
            return;
        }
        if (st == QMediaPlayer::PlayingState && dur > 0) {
            if (m_lastWatchPos >= 0 && pos <= m_lastWatchPos) {
                if (++m_stallTicks >= 12) {   // 连续 6s 位置不前进 = 卡死
                    m_watchArmed = false;
                    m_stallTicks = 0;
                    m_lastWatchPos = -1;
                    m_playing = false;
                    m_loading = false;
                    m_positionTimer.stop();
                    emit errorOccurred(QStringLiteral("播放卡住，尝试其它音源"));
                    return;
                }
            } else {
                m_stallTicks = 0;
            }
        } else {
            m_stallTicks = 0;
        }
        // 每次巡检都更新基准（回退 seek 后位置会变小，基准必须跟上，否则会误判卡死）
        m_lastWatchPos = pos;
    });
    m_watchTimer.start();

    // 装载是异步的：loadFinished 回来才真正起播/回退，主线程全程不阻塞。
    m_effect = new EffectPlayer(m_effects, this);
    connect(m_effect, &EffectPlayer::positionChanged, this, &PlayerEngine::positionChanged);
    connect(m_effect, &EffectPlayer::durationChanged, this, &PlayerEngine::durationChanged);
    connect(m_effect, &EffectPlayer::loadFinished, this, &PlayerEngine::onEffectLoaded);
    connect(m_effect, &EffectPlayer::endOfMedia, this, [this]() {
        m_playing = false;
        m_loading = false;
        emit endOfMedia();
    });
    connect(m_effect, &EffectPlayer::playbackStateChanged, this,
            [this](int st) {
                // st 对齐 QMediaPlayer::PlaybackState：0=Stopped 2=Paused 3=Playing
                if (st == 3) { m_playing = true; m_loading = false; emit playbackStateChanged(static_cast<int>(State::Playing)); }
                else if (st == 2) { m_playing = false; emit playbackStateChanged(static_cast<int>(State::Paused)); }
                else { m_playing = false; emit playbackStateChanged(static_cast<int>(State::Stopped)); }
            });

    // 音频设备切换检测：QAudioOutput 不会自动跟随系统默认输出设备，
    // 用户拔插耳机/切换声卡后若不做切换，声音会继续从旧设备输出。
    // 用 QMediaDevices::audioOutputsChanged 信号监听，事件驱动无轮询开销。
    // ⚠️ 构造里**不得**调 onAudioOutputsChanged()：探针实测 QMediaDevices::defaultAudioOutput()
    // 即打开 WASAPI 会话（AudioSes.dll 加载）唤醒蓝牙耳机 → 启动电流声。初始记录挪到 ensurePlayer。
    m_mediaDevices = new QMediaDevices(this);
    connect(m_mediaDevices, &QMediaDevices::audioOutputsChanged,
            this, &PlayerEngine::onAudioOutputsChanged);

    // 启动即清扫 3 天前的播放缓存（在线音频缓存自动过期，避免无限膨胀）
    sweepAudioCache(3);
}

PlayerEngine::~PlayerEngine()
{
    stop();
}

void PlayerEngine::playFile(const QString &path)
{
    stop();
    m_pendingLocalPath = path;
    startPlayback(path);
}

void PlayerEngine::play(const QUrl &url, const QString &cacheKey)
{
    stop();
    if (url.isLocalFile()) {
        m_pendingLocalPath = url.toLocalFile();
        startPlayback(m_pendingLocalPath);
        return;
    }

    // 在线资源：先下载到缓存文件，再交给播放器
    m_loading = true;
    emit playbackStateChanged(static_cast<int>(State::Loading));

    const QString cacheDir = audioCacheDir();
    QDir().mkpath(cacheDir);
    // 关键：用歌曲稳定身份（identityKey+音质）做缓存名，而非 URL 的 hash——
    // LX 源每次解析出的签名 URL 都不同，按 URL 缓存永不命中（每次重下=播放慢根因）
    const QString key = cacheKey.isEmpty() ? url.toString() : cacheKey;
    const QString savePath = cachePathForKey(key);

    if (QFileInfo::exists(savePath) && QFileInfo(savePath).size() > 1024
        && isAudioFile(savePath)) {
        m_loading = false;
        m_pendingLocalPath = savePath;
        startPlayback(savePath);
        return;
    }
    if (QFileInfo::exists(savePath))
        QFile::remove(savePath);   // 残留的无效/半截缓存清掉重下

    auto *engine = this;
    HttpOptions opt;
    opt.referer = refererForAudioUrl(url.toString());
    HttpClient::instance()->downloadFile(url.toString(), savePath, opt,
        nullptr,
        [engine, savePath](bool ok, const QString &err) {
            QMetaObject::invokeMethod(engine, [engine, savePath, ok, err]() {
                engine->onDownloadFinished(ok, err);
                if (ok) {
                    // 校验下载内容是否为有效音频，避免把错误响应（JSON 等）喂给播放器/写进缓存
                    if (!isAudioFile(savePath)) {
                        QFile::remove(savePath);
                        engine->onDownloadFinished(false,
                            QStringLiteral("音源返回的不是有效音频（可能已失效）"));
                        return;
                    }
                    engine->startPlayback(savePath);
                }
            }, Qt::QueuedConnection);
        });
}

QString PlayerEngine::audioCacheDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::TempLocation)
           + QStringLiteral("/muyun-audio");
}

QString PlayerEngine::cachePathForKey(const QString &cacheKey)
{
    return audioCacheDir() + QStringLiteral("/") +
           QString::number(qHash(cacheKey), 16) + QStringLiteral(".audio");
}

QString PlayerEngine::cachedAudioFile(const QString &cacheKey)
{
    if (cacheKey.isEmpty()) return QString();
    const QString path = cachePathForKey(cacheKey);
    if (QFileInfo::exists(path) && QFileInfo(path).size() > 1024 && isAudioFile(path))
        return path;
    return QString();
}

bool PlayerEngine::isAudioFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = f.read(16);
    f.close();
    if (head.size() < 4) return false;
    // 明显是文本/JSON 错误响应 → 无效
    if (head.startsWith("{") || head.startsWith("[") || head.startsWith("<")) return false;
    return true;
}

int PlayerEngine::sweepAudioCache(int maxAgeDays)
{
    QDir d(audioCacheDir());
    if (!d.exists()) return 0;
    int removed = 0;
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-maxAgeDays);
    const auto entries = d.entryInfoList({QStringLiteral("*.audio")}, QDir::Files);
    for (const QFileInfo &fi : entries) {
        if (fi.lastModified() < cutoff && QFile::remove(fi.absoluteFilePath()))
            ++removed;
    }
    if (removed) qInfo() << "[cache] 清理过期播放缓存" << removed << "个";
    return removed;
}

void PlayerEngine::onDownloadFinished(bool ok, const QString &err)
{
    m_loading = false;
    if (!ok) {
        emit errorOccurred(err.isEmpty() ? QStringLiteral("音频下载失败") : err);
        emit playbackStateChanged(static_cast<int>(State::Stopped));
    }
}

QMediaPlayer *PlayerEngine::ensurePlayer()
{
    if (m_player) return m_player;
    m_player = new QMediaPlayer(this);

    connect(m_player, &QMediaPlayer::positionChanged, this,
            [this](qint64 pos) { emit positionChanged(pos); });
    connect(m_player, &QMediaPlayer::durationChanged, this,
            [this](qint64 dur) { emit durationChanged(dur); });
    connect(m_player, &QMediaPlayer::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState s) {
                if (s == QMediaPlayer::PlayingState) {
                    // 设备切换恢复：媒体加载就绪后跳到切设备前的位置（未就绪时挂起等状态信号）
                    tryApplyResumeSeek();
                    m_playing = true;
                    m_loading = false;
                    m_positionTimer.start();
                    emit playbackStateChanged(static_cast<int>(State::Playing));
                } else if (s == QMediaPlayer::PausedState) {
                    m_playing = false;
                    m_positionTimer.stop();
                    emit playbackStateChanged(static_cast<int>(State::Paused));
                } else {
                    m_playing = false;
                    m_positionTimer.stop();
                    emit playbackStateChanged(static_cast<int>(State::Stopped));
                }
            });
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus s) {
                if (s == QMediaPlayer::EndOfMedia) {
                    m_playing = false;
                    m_positionTimer.stop();
                    // 真 EndOfMedia 已到手 → 解除末尾巡检，否则会跟着补发第二遍
                    m_watchArmed = false;
                    m_watchTimer.stop();
                    emit endOfMedia();
                } else if (s == QMediaPlayer::LoadedMedia || s == QMediaPlayer::BufferedMedia) {
                    m_loading = false;
                    // 媒体就绪，此时 setPosition 才真正生效 → 尝试跳回切设备前的位置
                    tryApplyResumeSeek();
                    // 稳健起播：setSource 后立即 play() 偶发不生效（尤其本地文件），
                    // 加载完成再补一次 play()，修复"点了卡在 0:00、拖进度条才播"
                    if (m_wantPlay) m_player->play();
                }
            });
    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error e, const QString &s) {
                m_playing = false;
                m_loading = false;
                m_positionTimer.stop();
                emit errorOccurred(s);
                emit playbackStateChanged(static_cast<int>(State::Stopped));
                Q_UNUSED(e);
            });
    onAudioOutputsChanged();   // 首次播放时记录当前默认设备（供设备切换比对）
    return m_player;
}

void PlayerEngine::ensureAudioOutput()
{
    if (m_output) return;
    ensurePlayer();
    m_output = new QAudioOutput(this);
    m_output->setVolume(m_muted ? 0.0 : m_volume);
    m_player->setAudioOutput(m_output);
}

void PlayerEngine::startPlayback(const QString &localPath)
{
    if (!QFileInfo::exists(localPath)) {
        emit errorOccurred(QStringLiteral("音频文件不存在"));
        emit playbackStateChanged(static_cast<int>(State::Stopped));
        return;
    }
    m_currentLocalPath = localPath;
    m_resumeAfterSwitch = false;   // 新曲目不得继承设备切换的挂起跳转
    m_resumedPosition = -1;
    m_effectFailedPath.clear();    // 新曲目重新评估音效管线

    // 音效管线：只在"任一音效开启 + 音源是 MP3"时启用，其余一律 QMediaPlayer。
    // 走音效线时**不创建** QMediaPlayer/QAudioOutput：这俩各自会打开一个 WASAPI 会话
    // （启动爆音铁律），既然不听它们，就别把会话打开。
    if (wantEffectPipeline(localPath)) {
        startViaEffect(localPath, 0, true);
        return;
    }
    if (effectsActive()) {
        // 开着音效却没走音效管线 → 必须留下原因（静默降级最难查）
        QString why;
        if (!qFuzzyCompare(m_rate, 1.0)) why = QStringLiteral("倍速 %1x 与音效互斥").arg(m_rate);
        else if (!localPath.isEmpty() && localPath == m_effectFailedPath) why = QStringLiteral("本曲已尝试失败");
        else if (!pcmLooksLikeMp3(localPath)) why = pcmHasUniversalBackend()
                    ? QStringLiteral("未知容器，FFmpeg 也认不出") : pcmUniversalBackendError();
        else why = pcmUniversalBackendError();
        qInfo() << "[fx] 音效开启但本曲不走音效管线：" << why << localPath;
    }
    startViaPlayer(localPath, 0, true);
}

bool PlayerEngine::wantEffectPipeline(const QString &localPath) const
{
    if (!kEffectsRuntimeEnabled) return false;
    if (!effectsActive()) return false;
    // 音效管线不支持倍速（变速要交回 FFmpeg 后端），非 1.0 倍速就不走音效
    if (!qFuzzyCompare(m_rate, 1.0)) return false;
    if (localPath.isEmpty()) return false;
    if (localPath == m_effectFailedPath) return false;   // 这首已经试过、已知不行
    // 格式闸门放开：MP3 走 minimp3（时长样本级精确），其余格式只要有 FFmpeg 后端就都能开音效。
    // 真的解不开（未知容器/坏文件/FFmpeg 缺失）由异步装载失败自动回退，不会"点了没声"。
    if (pcmLooksLikeMp3(localPath)) return true;
    return pcmHasUniversalBackend();
}

void PlayerEngine::startViaEffect(const QString &localPath, qint64 fromMs, bool autoplay)
{
    if (!m_effect) return;
    // 停掉普通内核（没创建过就跳过，别为"不听的东西"打开音频会话）
    if (m_player) {
        m_nudgeTimer.stop();
        m_player->stop();
        m_player->setSource(QUrl());
    }
    m_watchTimer.stop();      // 音效内核自己报 endOfMedia，末尾/卡死兜底交给它
    m_watchArmed = false;
    m_positionTimer.stop();

    m_useEffect = true;
    m_effectPending = true;
    m_wantPlay = autoplay;
    m_playing = false;
    m_loading = autoplay;
    emit playbackStateChanged(static_cast<int>(autoplay ? State::Loading : State::Paused));
    emit effectsRuntimeChanged();
    // 注意：**不要**在这里查 QMediaDevices::defaultAudioOutput()。Qt 多媒体后端
    // 第一次被接触要 1 秒级初始化（实测 1.1s），放在点击同步路径里就是"点一下卡一下"；
    // 交给装载完成后的 onEffectLoaded（异步续作）去做，点击手感即时。
    m_effect->loadAsync(localPath, fromMs);   // 异步：主线程立刻返回，UI 不卡
}

void PlayerEngine::startViaPlayer(const QString &localPath, qint64 fromMs, bool autoplay)
{
    m_useEffect = false;
    m_effectPending = false;
    if (m_effect) m_effect->stop();
    ensurePlayer();
    ensureAudioOutput();

    m_wantPlay = autoplay;
    m_playing = false;
    m_loading = autoplay;
    // 末尾/卡死兜底：新曲从头播放、且是自启动 → 武装巡检
    m_watchArmed = autoplay;
    m_stallTicks = 0;
    m_lastWatchPos = -1;
    m_player->setSource(QUrl::fromLocalFile(localPath));
    if (fromMs > 0) {              // 复用"媒体就绪后择机跳回"机制（未就绪时 setPosition 会被吞）
        m_resumeAfterSwitch = true;
        m_resumedPosition = fromMs;
    }
    if (autoplay) {
        emit playbackStateChanged(static_cast<int>(State::Loading));
        m_player->play();
        if (!qFuzzyCompare(m_rate, 1.0)) m_player->setPlaybackRate(m_rate);
        m_nudgeCount = 0;
        m_nudgeTimer.start();      // 部分文件需补一次 seek 才激活解码
    } else {
        emit playbackStateChanged(static_cast<int>(State::Paused));
    }
    emit effectsRuntimeChanged();
}

void PlayerEngine::onEffectLoaded(bool ok, const QString &reason)
{
    m_effectPending = false;
    if (!m_useEffect) return;      // 装载期间用户已切歌/停止：迟到的回调直接丢
    if (!ok) {
        // 非 MP3 / 设备格式谈不拢 → 自动回退普通内核，绝不"点了没声"
        m_effectFailedPath = m_currentLocalPath;
        qWarning() << "[fx] 音效管线不可用：" << reason << "→ 回退普通播放";
        startViaPlayer(m_currentLocalPath, position(), m_wantPlay);
        return;
    }
    // 设备监视基线：原本在 ensurePlayer() 里记，纯音效播放不走那里，
    // 就在装载完成后补记一次（此时后端已被 EffectPlayer 的协商过程唤醒，查询很便宜）
    if (!m_deviceInit) onAudioOutputsChanged();
    if (m_wantPlay) m_effect->play();
    emit effectsRuntimeChanged();
}

bool PlayerEngine::effectsBypassed() const
{
    if (m_effectPending) return false;              // 还在异步装载，先不判"未生效"
    return effectsActive() && !m_useEffect && hasSource();
}

bool PlayerEngine::effectsActive() const
{
    return m_effects && (m_effects->eqEnabled() || m_effects->reverbEnabled()
                         || m_effects->spatialEnabled() || m_effects->loudnessEnabled());
}

// 是否像 MP3 的判断统一走 PcmSource 的 pcmLooksLikeMp3（解码后端选择也在那里）

void PlayerEngine::reevaluateEffects()
{
    if (!kEffectsRuntimeEnabled) return;
    if (m_currentLocalPath.isEmpty() || !QFileInfo::exists(m_currentLocalPath)) return;

    const bool want = wantEffectPipeline(m_currentLocalPath);
    // 关键改动：音效管线是**逐块读参数**的实时 DSP，已经在管线上时，
    // 改 EQ/混响/环绕/响度都即时生效，不需要重解码整曲（旧写法每次都重载 → 卡顿）。
    if (want == m_useEffect) return;

    const qint64 pos = position();
    const bool wasPlaying = isPlaying() || (m_effectPending && m_wantPlay);
    if (want) startViaEffect(m_currentLocalPath, pos, wasPlaying);
    else startViaPlayer(m_currentLocalPath, pos, wasPlaying);
}

void PlayerEngine::pause()
{
    m_wantPlay = false;
    if (m_useEffect) { if (m_effect) m_effect->pause(); return; }
    if (m_player) m_player->pause();
}

void PlayerEngine::resume()
{
    m_wantPlay = true;
    if (m_useEffect) { if (m_effect) m_effect->play(); return; }
    if (m_player && m_player->source().isValid())
        m_player->play();
}

void PlayerEngine::stop()
{
    m_positionTimer.stop();
    m_nudgeTimer.stop();
    m_watchTimer.stop();      // 主动停止：解除末尾/卡死兜底，别在收尾时误报
    m_watchArmed = false;
    m_nudgeCount = 0;
    m_wantPlay = false;
    m_resumeAfterSwitch = false;   // 主动停止后不得再"跳回"旧位置
    m_resumedPosition = -1;
    m_effectPending = false;
    if (m_effect) m_effect->stop();     // 不论是否在音效路径都要停（在途装载会被 m_useEffect 判废弃）
    m_useEffect = false;
    if (m_player) {
        m_player->stop();
        m_player->setSource(QUrl());
    }
    m_playing = false;
    m_loading = false;
    emit effectsRuntimeChanged();
}

bool PlayerEngine::isPlaying() const
{
    if (m_useEffect) return m_effect && m_effect->isPlaying();
    return m_player && m_player->playbackState() == QMediaPlayer::PlayingState;
}

bool PlayerEngine::hasSource() const
{
    if (m_useEffect) return true;
    return m_player && m_player->source().isValid();
}

QUrl PlayerEngine::sourceUrl() const
{
    if (m_useEffect) return QUrl::fromLocalFile(m_currentLocalPath);
    return m_player ? m_player->source() : QUrl();
}

qint64 PlayerEngine::position() const
{
    if (m_useEffect) return m_effect ? m_effect->position() : 0;
    return m_player ? m_player->position() : 0;
}
qint64 PlayerEngine::duration() const
{
    if (m_useEffect) return m_effect ? m_effect->duration() : 0;
    return m_player ? m_player->duration() : 0;
}

void PlayerEngine::seek(qint64 ms)
{
    if (m_useEffect) {
        // 装载未就绪时也接受 seek：EffectPlayer 内部会把它记成"起播位置"，就绪后自动落点
        m_effect->seek(ms);
        return;
    }
    if (m_player) m_player->setPosition(ms);
}

void PlayerEngine::setVolume(qreal volume)
{
    m_volume = qBound(0.0, volume, 1.0);
    const qreal eff = m_muted ? 0.0 : m_volume;
    if (m_useEffect) { m_effect->setVolume(eff); return; }
    if (m_output) m_output->setVolume(eff);
}

qreal PlayerEngine::volume() const { return m_volume; }

void PlayerEngine::setMuted(bool muted)
{
    m_muted = muted;
    const qreal eff = muted ? 0.0 : m_volume;
    if (m_useEffect) { m_effect->setVolume(eff); return; }
    if (m_output) m_output->setVolume(eff);
}

bool PlayerEngine::isMuted() const { return m_muted; }

void PlayerEngine::setPlaybackRate(qreal rate)
{
    m_rate = rate;
    if (!m_useEffect && m_player) m_player->setPlaybackRate(rate);
    // 倍速与音效管线互斥：1.0x↔非 1.0x 之间变化时要重新评估走哪条内核
    reevaluateEffects();
}

PlayerEngine::State PlayerEngine::state() const
{
    if (m_useEffect) {
        if (m_effectPending) return m_loading ? State::Loading : State::Paused;
        return (m_effect && m_effect->isPlaying()) ? State::Playing : State::Paused;
    }
    if (!m_player) return m_loading ? State::Loading : State::Stopped;
    switch (m_player->playbackState()) {
    case QMediaPlayer::PlayingState: return State::Playing;
    case QMediaPlayer::PausedState:  return State::Paused;
    default:                         return m_loading ? State::Loading : State::Stopped;
    }
}

QVector<int> PlayerEngine::spectrum() const
{
    // QMediaPlayer 无实时频谱数据；走音效管线时 DSP 侧有电平/频带信息，先占位返回空
    return {};
}

QString PlayerEngine::audioDeviceName() const
{
    if (m_useEffect && m_effect) {
        const QString n = m_effect->deviceName();
        return n.isEmpty() ? QStringLiteral("默认") : n;
    }
    if (!m_output) return QStringLiteral("默认");
    const QAudioDevice dev = m_output->device();
    return dev.isNull() ? QStringLiteral("默认") : dev.description();
}

QStringList PlayerEngine::audioDeviceNames() const
{
    QStringList names;
    for (const QAudioDevice &d : QMediaDevices::audioOutputs())
        names << d.description();
    return names;
}

qint64 PlayerEngine::effectStarvedMs() const
{
    return (m_effect && m_useEffect) ? m_effect->starvedMs() : 0;
}

int PlayerEngine::effectOutputRate() const
{
    return m_effect ? m_effect->outputSampleRate() : 0;
}

int PlayerEngine::effectSourceRate() const
{
    return m_effect ? m_effect->sourceSampleRate() : 0;
}

QString PlayerEngine::effectBackend() const
{
    return (m_effect && m_useEffect) ? m_effect->backendName() : QString();
}

void PlayerEngine::reattachToCurrentDevice()
{
    onDefaultDeviceChanged(QMediaDevices::defaultAudioOutput());
}

// ---------------------------------------------------------------------------
// 音频设备切换检测（事件驱动，无轮询）
// ---------------------------------------------------------------------------
void PlayerEngine::onAudioOutputsChanged()
{
    // 纯音效播放时 m_player 可能从未创建（不占音频会话），但设备切换同样要能发现
    if (!m_player && !m_useEffect) return;
    const QAudioDevice cur = QMediaDevices::defaultAudioOutput();
    if (cur.isNull()) return;
    if (!m_deviceInit) {
        m_lastDevice = cur;
        m_deviceInit = true;
        return;
    }
    if (cur.id() == m_lastDevice.id()) return;

    m_lastDevice = cur;
    onDefaultDeviceChanged(cur);
}

void PlayerEngine::tryApplyResumeSeek()
{
    if (!m_player || !m_resumeAfterSwitch || m_resumedPosition <= 0) return;
    // setPosition 只在媒体加载就绪后才生效（未加载时会被 FFmpeg 后端静默丢弃）
    const QMediaPlayer::MediaStatus st = m_player->mediaStatus();
    if (st != QMediaPlayer::LoadedMedia && st != QMediaPlayer::BufferedMedia) return;
    m_player->setPosition(m_resumedPosition);
    m_resumeAfterSwitch = false;
    m_resumedPosition = -1;
}

void PlayerEngine::onDefaultDeviceChanged(const QAudioDevice &device)
{
    if (device.isNull()) return;

    // 音效管线：在自己的 QAudioSink 上换设备（内部重新协商格式并保持位置/播放状态）。
    // 新设备连格式都谈不拢时，回退普通内核——宁可没音效，也别哑掉。
    if (m_useEffect) {
        const qint64 pos = position();
        const bool wasPlaying = m_wantPlay;
        if (!m_effect || !m_effect->onDeviceChanged(device)) {
            qWarning() << "[fx] 新输出设备不支持音效管线 → 回退普通播放";
            m_effectFailedPath = m_currentLocalPath;
            startViaPlayer(m_currentLocalPath, pos, wasPlaying);
        }
        emit audioDeviceChanged(device.description());
        return;
    }

    if (!m_output) return;   // 从未播放过 → 没有音频流需要重挂（播放时会用当时默认设备创建）

    const bool wasPlaying = m_player->playbackState() == QMediaPlayer::PlayingState;
    const qint64 pos = m_player->position();
    const QUrl source = m_player->source();

    // 1. 停止当前播放并清空源（避免 output 析构时仍有活跃流）
    m_positionTimer.stop();
    m_player->stop();
    m_player->setSource(QUrl());

    // 2. 释放旧 output 并在新设备上重建
    delete m_output;
    m_output = nullptr;
    m_output = new QAudioOutput(device, this);
    m_output->setVolume(m_muted ? 0.0 : m_volume);
    m_player->setAudioOutput(m_output);

    // 3. 恢复媒体源 + 挂起"跳回原位"（在 setSource 之前置标志；
    //    实际 setPosition 由 LoadedMedia/BufferedMedia/PlayingState 信号择机执行）
    if (!source.isEmpty()) {
        if (pos > 0) {
            m_resumeAfterSwitch = true;
            m_resumedPosition = pos;
        }
        m_player->setSource(source);
        if (wasPlaying) {
            m_wantPlay = true;     // 与 startPlayback 一致：LoadedMedia 时补 play()
            m_player->play();
            m_positionTimer.start();
            // 兜底：恢复 seek 被解码器吞掉时 nudge 会以目标位置补跳（见 nudge lambda）
            m_nudgeCount = 0;
            m_nudgeTimer.start();
        }
    }

    emit audioDeviceChanged(device.description());
}

} // namespace Muyun
