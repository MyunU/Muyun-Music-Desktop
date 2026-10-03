#include "EffectPlayer.h"

#include "PcmSource.h"
#include "core/audio/AudioEffects.h"
#include "core/audio/SincResampler.h"

#include <QFile>
#include <QAudioSink>
#include <QAudioFormat>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QMetaObject>
#include <QDebug>
#include <QElapsedTimer>
#include <algorithm>
#include <cstring>

namespace Muyun {

namespace {

/// 一次最多产出多少输出帧（1152 源样本，上采样 2 倍足够；更大倍率按 suggestCap 动态扩）
constexpr int kMaxOutFrames = 1152 * 2 + 64;
/// 单次写环形缓冲的最大字节数（超过则分批）
constexpr int kMaxPushBytes = 64 * 1024;

int sampleBytesOf(QAudioFormat::SampleFormat f)
{
    switch (f) {
    case QAudioFormat::Int16: return 2;
    case QAudioFormat::Int32: return 4;
    case QAudioFormat::Float: return 4;
    default:                  return 0;
    }
}

inline void storeSample(char *dst, QAudioFormat::SampleFormat f, float v)
{
    v = std::clamp(v, -1.0f, 1.0f);
    switch (f) {
    case QAudioFormat::Int16:
        *reinterpret_cast<int16_t *>(dst) = static_cast<int16_t>(v * 32767.0f);
        break;
    case QAudioFormat::Int32:
        *reinterpret_cast<int32_t *>(dst) = static_cast<int32_t>(v * 2147483647.0f);
        break;
    default:
        *reinterpret_cast<float *>(dst) = v;
        break;
    }
}

} // namespace

// ===========================================================================
// PullDevice
// ===========================================================================

EffectPlayer::PullDevice::PullDevice(EffectPlayer *owner)
    : QIODevice(owner), m_own(owner)
{
    open(QIODevice::ReadOnly);
}

qint64 EffectPlayer::PullDevice::readData(char *out, qint64 maxlen)
{
    return m_own->pullRead(out, maxlen);
}

qint64 EffectPlayer::PullDevice::bytesAvailable() const
{
    // ⚠ 核心坑：不重写这个（和 atEnd）的话，Qt6 FFmpeg 后端永远不调 readData
    QMutexLocker lk(&m_own->m_mtx);
    if (m_own->m_sh.used > 0) return m_own->m_sh.used;
    return m_own->m_sh.eof ? 0 : 4096;      // 欠载也报"有"，让流维持
}

bool EffectPlayer::PullDevice::atEnd() const
{
    QMutexLocker lk(&m_own->m_mtx);
    return m_own->m_sh.eof && m_own->m_sh.used == 0;
}

// ===========================================================================
// EffectPlayer：生命周期
// ===========================================================================

EffectPlayer::EffectPlayer(AudioEffects *fx, QObject *parent) : QObject(parent), m_fx(fx)
{
    m_thread = new Pipeline(this);

    m_probe.setInterval(200);      // 与 QMediaPlayer 路径同频（200ms 上报一次位置）
    connect(&m_probe, &QTimer::timeout, this, [this]() {
        const qint64 ms = position();
        if (ms != m_lastReportedMs) {
            m_lastReportedMs = ms;
            emit positionChanged(ms);
        }
        bool finished = false;
        {
            QMutexLocker lk(&m_mtx);
            finished = m_sh.eof && m_sh.used == 0 && m_playing;
        }
        if (finished && !m_endEmitted) {
            m_endEmitted = true;
            m_playing = false;
            m_pendingPlay = false;
            m_probe.stop();
            closeSink();
            emit playbackStateChanged(0);        // Stopped
            emit endOfMedia();
        }
    });
}

EffectPlayer::~EffectPlayer()
{
    m_quit.storeRelaxed(1);
    {
        QMutexLocker lk(&m_mtx);
        m_sh.cmd = CmdQuit;
        m_sh.wantWork = false;
        m_sh.running = false;
    }
    m_cond.wakeAll();
    if (m_thread && m_thread->isRunning()) {
        if (!m_thread->wait(2000)) {
            qWarning() << "[fx] 解码线程 2s 未退出，强制终止";
            m_thread->terminate();
            m_thread->wait(500);
        }
    }
    delete m_thread;
    m_thread = nullptr;
    closeSink();
}

void EffectPlayer::postCmd(Cmd c, const QString &path, qint64 ms)
{
    QMutexLocker lk(&m_mtx);
    m_sh.cmd = c;
    if (!path.isEmpty()) m_sh.path = path;
    m_sh.argMs = ms;
    lk.unlock();
    m_cond.wakeAll();
}

void EffectPlayer::clearRingLocked()
{
    m_sh.wpos = 0;
    m_sh.rpos = 0;
    m_sh.used = 0;
}


// ---------------------------------------------------------------------------
// 装载：立即返回，主线程零阻塞
// ---------------------------------------------------------------------------

void EffectPlayer::loadAsync(const QString &mp3Path, qint64 startMs)
{
    m_loaded = false;
    m_pendingPlay = false;
    m_endEmitted = false;
    m_durationMs = 0;
    m_starvedMs = 0;
    m_lastReportedMs = -1;
    m_delivered.storeRelaxed(0);

    closeSink();
    {
        QMutexLocker lk(&m_mtx);
        clearRingLocked();
        m_sh.running = false;
        m_sh.eof = false;
        m_sh.haveFormat = false;
        m_sh.wantWork = m_playing;
        m_sh.paused = !m_playing;
        // 位置基准先按起点记（duration 未知，先给 0 起点；onIndexed 后重算）
        m_sh.pendingStartMs = qMax<qint64>(0, startMs);
    }
    if (!m_thread->isRunning()) m_thread->start(QThread::LowPriority);
    postCmd(CmdLoad, mp3Path, startMs);
}

/// 后台建表完成 → 主线程：协商设备格式 → 广播就绪
void EffectPlayer::onIndexed(bool ok, const QString &reason,
                             qint64 durationMs, int srcRate, qint64 totalSamples)
{
    Q_UNUSED(totalSamples)
    if (!ok) {
        m_loaded = false;
        m_pendingPlay = false;
        qWarning() << "[fx] 装载失败：" << reason;
        emit loadFinished(false, reason);
        return;
    }
    m_srcRate = srcRate;

    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (!negotiateFormat(dev)) {
        m_loaded = false;
        m_pendingPlay = false;
        emit loadFinished(false, QStringLiteral("输出设备不接受任何候选格式"));
        return;
    }

    m_durationMs = durationMs;
    m_loaded = true;
    // 起播点：装载期间用户可能已经 seek 过，pendingStartMs 是最终意图
    const qint64 startMs = [this]() { QMutexLocker lk(&m_mtx); return m_sh.pendingStartMs; }();
    m_baseFrame = startMs * qMax(1, m_outRate) / 1000;
    m_delivered.storeRelaxed(0);
    m_lastReportedMs = startMs;
    if (startMs > 0) postCmd(CmdSeek, QString(), startMs);   // 落点（重采样/滤波器一并复位）

    emit durationChanged(m_durationMs);
    emit loadFinished(true, QString());
    if (m_pendingPlay) { m_pendingPlay = false; play(); }
}

// ---------------------------------------------------------------------------
// 播放控制
// ---------------------------------------------------------------------------

void EffectPlayer::play()
{
    if (m_quit.loadRelaxed()) return;
    if (!m_loaded) { m_pendingPlay = true; return; }

    {
        QMutexLocker lk(&m_mtx);
        m_sh.wantWork = true;
        m_sh.paused = false;
        m_sh.eof = false;              // 曲终重播：允许后台重新产出
    }
    m_cond.wakeAll();

    if (!m_sink) {
        if (!m_pull) m_pull = new PullDevice(this);
        const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
        if (!openSink(dev)) {
            qWarning() << "[fx] 输出设备不可用，音效播放无法开始";
            return;
        }
    }
    if (!m_pull) m_pull = new PullDevice(this);

    applyVolume();
    m_sink->start(m_pull);
    if (m_sink->state() != QAudio::ActiveState) {
        qWarning() << "[fx] sink 未进入 Active：state=" << int(m_sink->state())
                   << "err=" << int(m_sink->error());
    }
    if (!m_playing) {
        m_playing = true;
        m_endEmitted = false;
        m_probe.start();
        emit playbackStateChanged(3);   // Playing
    }
}

void EffectPlayer::pause()
{
    if (!m_playing) return;
    m_playing = false;
    m_probe.stop();
    {
        QMutexLocker lk(&m_mtx);
        m_sh.paused = true;
        m_sh.wantWork = false;
    }
    // 不用 suspend()（实测 resume 后不再拉流）。stop 丢掉在途数据：
    // 位置基准 = 当前位置 - 已在途的部分，恢复时从这里续
    if (m_sink) m_sink->stop();
    m_cond.wakeAll();
    emit playbackStateChanged(2);       // Paused
}

void EffectPlayer::stop()
{
    m_playing = false;
    m_pendingPlay = false;
    m_probe.stop();
    closeSink();
    {
        QMutexLocker lk(&m_mtx);
        m_sh.paused = true;
        m_sh.wantWork = false;
        m_sh.running = false;
        m_sh.eof = false;
        clearRingLocked();
    }
    postCmd(CmdUnload);
    m_baseFrame = 0;
    m_delivered.storeRelaxed(0);
    m_durationMs = 0;
    m_loaded = false;
    emit playbackStateChanged(0);       // Stopped
}

void EffectPlayer::seek(qint64 ms)
{
    if (m_durationMs > 0) ms = qBound<qint64>(0, ms, m_durationMs);
    if (ms < 0) ms = 0;

    if (!m_loaded) {
        // 装载还没回来：记住目标，onIndexed 就绪后自动落点（seek 不会丢）
        QMutexLocker lk(&m_mtx);
        m_sh.pendingStartMs = ms;
        return;
    }
    m_baseFrame = ms * qMax(1, m_outRate) / 1000;
    m_delivered.storeRelaxed(0);
    m_lastReportedMs = ms;
    m_endEmitted = false;

    {
        QMutexLocker lk(&m_mtx);
        clearRingLocked();
        m_sh.eof = false;
    }
    postCmd(CmdSeek, QString(), ms);

    // 播放中：重启 sink 丢弃它内部的旧位置数据，使跳转立刻见效
    if (m_playing && m_sink) {
        m_sink->stop();
        m_sink->start(m_pull);
    }
    emit positionChanged(ms);
}

void EffectPlayer::setVolume(qreal v)
{
    m_volume = qBound(0.0, v, 1.0);
    applyVolume();
}

void EffectPlayer::applyVolume()
{
    if (m_sink) m_sink->setVolume(static_cast<float>(m_volume));
}

// ---------------------------------------------------------------------------
// 位置：只算真实内容帧（欠载补的静音不计），再扣除声卡在途量
// ---------------------------------------------------------------------------

qint64 EffectPlayer::inFlightFrames() const
{
    if (!m_sink || !m_playing) return 0;
    const qint64 free = m_sink->bytesFree();
    qint64 n = (m_bufferBytes - free) / qMax<qint64>(1, qint64(m_bytesPerFrame));
    return n < 0 ? 0 : n;
}

qint64 EffectPlayer::position() const
{
    if (m_outRate <= 0) return m_baseFrame * 1000 / qMax(1, m_srcRate);
    const qint64 frames = m_baseFrame + m_delivered.loadRelaxed() - inFlightFrames();
    qint64 ms = frames * 1000 / m_outRate;
    if (ms < 0) ms = 0;
    if (m_durationMs > 0 && ms > m_durationMs) ms = m_durationMs;
    return ms;
}

qint64 EffectPlayer::starvedMs() const
{
    QMutexLocker lk(&m_mtx);
    return m_starvedMs;
}

int EffectPlayer::frameCount() const
{
    QMutexLocker lk(&m_mtx);
    return m_sh.frames;
}

QString EffectPlayer::backendName() const
{
    QMutexLocker lk(&m_mtx);
    return m_sh.backend;
}

bool EffectPlayer::onDeviceChanged(const QAudioDevice &device)
{
    const bool wasPlaying = m_playing;
    const qint64 ms = position();
    closeSink();
    if (!negotiateFormat(device)) {
        qWarning() << "[fx] 新设备协商失败";
        return false;
    }
    m_baseFrame = ms * qMax(1, m_outRate) / 1000;
    m_delivered.storeRelaxed(0);
    if (wasPlaying) m_playing = false;    // 交给 play() 重走状态机
    postCmd(CmdSeek, QString(), ms);
    if (wasPlaying) play();
    return true;
}

// ---------------------------------------------------------------------------
// 格式协商 / sink
// ---------------------------------------------------------------------------

bool EffectPlayer::negotiateFormat(const QAudioDevice &device)
{
    if (device.isNull()) { m_deviceName.clear(); return false; }

    int srcRate = 0, srcCh = 2;
    {
        QMutexLocker lk(&m_mtx);
        srcRate = m_sh.srcRate;
        srcCh = m_sh.srcCh > 0 ? m_sh.srcCh : 2;
    }
    if (srcRate <= 0) return false;

    const QAudioFormat pref = device.preferredFormat();
    auto mk = [](int rate, int ch, QAudioFormat::SampleFormat sf) {
        QAudioFormat f;
        f.setSampleRate(rate);
        f.setChannelCount(ch);
        f.setSampleFormat(sf);
        return f;
    };

    // 候选顺序讲究：先"源格式直通"（零重采样、最保真），再设备首选格式，
    // 再 Windows 最常见的混音格式（本机实测只认 48000/2ch/Float，44100/Int16 全被拒），
    // 最后才退单声道。
    QVector<QAudioFormat> cands;
    cands << mk(srcRate, 2, QAudioFormat::Int16)
          << mk(srcRate, 2, QAudioFormat::Float)
          << mk(srcRate, srcCh, QAudioFormat::Int16)
          << mk(srcRate, srcCh, QAudioFormat::Float);
    if (pref.isValid() && pref.sampleRate() > 0 && pref.channelCount() > 0
        && sampleBytesOf(pref.sampleFormat()) > 0)
        cands << pref;
    cands << mk(48000, 2, QAudioFormat::Float)
          << mk(48000, 2, QAudioFormat::Int16)
          << mk(44100, 2, QAudioFormat::Float)
          << mk(44100, 2, QAudioFormat::Int16)
          << mk(48000, 1, QAudioFormat::Float)
          << mk(44100, 1, QAudioFormat::Int16);

    QAudioFormat chosen;
    for (const QAudioFormat &f : cands) {
        if (!f.isValid() || f.sampleRate() <= 0 || f.channelCount() <= 0) continue;
        if (sampleBytesOf(f.sampleFormat()) <= 0) continue;
        if (device.isFormatSupported(f)) { chosen = f; break; }
    }
    if (!chosen.isValid()) {
        qWarning() << "[fx] 设备不接受任何候选格式：" << device.description()
                   << "首选" << pref.sampleRate() << pref.channelCount()
                   << int(pref.sampleFormat());
        return false;
    }

    m_format = chosen;
    m_outRate = chosen.sampleRate();
    m_outCh = chosen.channelCount();
    m_bytesPerFrame = qint64(m_outCh) * sampleBytesOf(chosen.sampleFormat());
    m_deviceName = device.description();
    m_bufferBytes = qint64(kSinkBufferMs) * m_outRate * m_bytesPerFrame / 1000;

    {
        QMutexLocker lk(&m_mtx);
        m_sh.outRate = m_outRate;
        m_sh.outCh = m_outCh;
        m_sh.outFmt = chosen.sampleFormat();
        const qint64 cap = qint64(kRingSeconds) * m_outRate * m_bytesPerFrame;
        if (m_sh.ring.size() != cap) m_sh.ring = QByteArray(int(cap), '\0');
        clearRingLocked();
        m_sh.haveFormat = true;
    }
    qInfo() << "[fx] 输出" << m_outRate << "Hz" << m_outCh << "ch fmt" << int(chosen.sampleFormat())
            << "源" << srcRate << "Hz" << m_deviceName;
    return true;
}

bool EffectPlayer::openSink(const QAudioDevice &device)
{
    if (!negotiateFormat(device)) return false;
    if (!m_pull) m_pull = new PullDevice(this);
    m_sink = new QAudioSink(device, m_format, this);
    m_sink->setBufferSize(qint64(m_bufferBytes));
    applyVolume();
    return true;
}

void EffectPlayer::closeSink()
{
    if (m_sink) { m_sink->stop(); delete m_sink; m_sink = nullptr; }
    if (m_pull) { m_pull->close(); delete m_pull; m_pull = nullptr; }
}

// ---------------------------------------------------------------------------
// 主线程被 sink 拉取：只搬运字节，不做解码/DSP
// ---------------------------------------------------------------------------

qint64 EffectPlayer::pullRead(char *out, qint64 maxlen)
{
    if (maxlen <= 0 || m_bytesPerFrame <= 0) return 0;
    qint64 need = maxlen - (maxlen % m_bytesPerFrame);
    if (need <= 0) need = m_bytesPerFrame;

    QMutexLocker lk(&m_mtx);
    if (m_sh.used == 0) {
        if (m_sh.eof) return 0;                    // 真曲终 → sink 排空转 Idle
        m_starvedMs += need * 1000 / qMax<qint64>(1, qint64(m_outRate) * m_bytesPerFrame);
        std::memset(out, 0, size_t(need));
        lk.unlock();
        m_cond.wakeAll();
        return need;
    }

    // 不足一帧：补满一帧静音，等后台产出（不计入位置）
    const qint64 whole = m_sh.used - (m_sh.used % m_bytesPerFrame);
    if (whole <= 0) {
        std::memset(out, 0, size_t(need));
        lk.unlock();
        m_cond.wakeAll();
        return need;
    }
    const qint64 take = qMin(need, whole);
    const char *src = m_sh.ring.constData();
    char *dst = out;
    qint64 left = take;
    while (left > 0) {
        const qint64 tail = m_sh.ring.size() - m_sh.rpos;
        const qint64 part = qMin(left, tail);
        std::memcpy(dst, src + m_sh.rpos, size_t(part));
        dst += part;
        m_sh.rpos = (m_sh.rpos + part) % m_sh.ring.size();
        left -= part;
    }
    m_sh.used -= take;
    m_delivered.fetchAndAddRelaxed(int(take / m_bytesPerFrame));
    lk.unlock();
    m_cond.wakeAll();          // 腾出空位，叫醒可能在等的后台线程
    return take;
}

// ===========================================================================
// 后台管线线程
// ===========================================================================
//
// 循环：取命令 → （等格式协商）→ 解码一帧 → DSP → 重采样 → 转设备格式 → 写环形缓冲
// 全程不持锁做重活（读文件/解码/DSP 都在锁外），只在写缓冲和取命令时加锁。

void EffectPlayer::Pipeline::run()
{
    EffectPlayer *p = m_own;

    // 解码后端（minimp3 / FFmpeg 由工厂按文件挑），只在本线程访问
    PcmSource *src = nullptr;
    int srcRate = 0;
    bool produced = false;              ///< src 已就绪可产出
    bool srcEof = false;                ///< 解码器已读到尾
    qint64 tailLeftMs = 0;              ///< 曲尾还须补的静音（让混响自然衰减）

    SincResampler res;
    bool resConfigured = false;
    QVector<float> work;                ///< 解码输出（源采样率，送 DSP）
    QVector<float> workOut;             ///< 重采样输出（设备采样率）
    QByteArray stash;                   ///< 还没挤进环形缓冲的字节

    struct Job {
        Cmd cmd = CmdNone;
        QString path;
        qint64 argMs = 0;
    };

    auto takeJob = [&]() -> Job {
        QMutexLocker lk(&p->m_mtx);
        Job j;
        j.cmd = p->m_sh.cmd;
        p->m_sh.cmd = CmdNone;
        j.path = p->m_sh.path;
        j.argMs = p->m_sh.argMs;
        return j;
    };

    auto outParams = [&]() {
        struct Out { int rate; int ch; QAudioFormat::SampleFormat fmt; } o{ 0, 2, QAudioFormat::Float };
        QMutexLocker lk(&p->m_mtx);
        o.rate = p->m_sh.outRate;
        o.ch = p->m_sh.outCh;
        o.fmt = p->m_sh.outFmt;
        return o;
    };

    // 把 stash 写进环形缓冲；缓冲满就等消费者腾位（可被命令/退出打断）
    auto flushStash = [&]() -> bool {
        while (!stash.isEmpty()) {
            if (p->m_quit.loadRelaxed()) return false;
            QMutexLocker lk(&p->m_mtx);
            if (p->m_sh.cmd != CmdNone) return false;         // 有命令：先回主循环处理
            if (!p->m_sh.running) return false;               // 已卸载
            if (!p->m_sh.wantWork) { p->m_cond.wait(&p->m_mtx, 20); continue; }
            const qint64 cap = p->m_sh.ring.size();
            if (cap <= 0) { stash.clear(); return true; }
            const qint64 room = cap - p->m_sh.used;
            if (room <= 0) { p->m_cond.wait(&p->m_mtx, 20); continue; }

            const qint64 n = qMin<qint64>(qMin(room, stash.size()), kMaxPushBytes);
            const char *in = stash.constData();
            char *ring = p->m_sh.ring.data();
            const qint64 tailRoom = cap - p->m_sh.wpos;
            const qint64 part1 = qMin(n, tailRoom);
            std::memcpy(ring + p->m_sh.wpos, in, size_t(part1));
            if (n > part1) std::memcpy(ring, in + part1, size_t(n - part1));
            p->m_sh.wpos = (p->m_sh.wpos + n) % cap;
            p->m_sh.used += n;
            stash.remove(0, int(n));
            lk.unlock();
            p->m_cond.wakeAll();
        }
        return true;
    };

    // 源采样率立体声 → DSP → 重采样 → 设备格式字节 → stash
    auto emitFrames = [&](const float *st, int frames) -> bool {
        if (frames <= 0) return true;
        p->m_fx->process(const_cast<float *>(st), frames);     // 参数实时生效（锁在内部）

        const auto o = outParams();
        if (o.rate <= 0 || o.ch <= 0) return true;
        if (!resConfigured || res.inRate() != srcRate || res.outRate() != o.rate) {
            res.configure(srcRate > 0 ? srcRate : o.rate, o.rate);
            resConfigured = true;
        }
        // 输出容量按实际倍率给足（8k 源进 48k 设备是 6 倍上采样，固定缓冲会持续欠载）
        const int outCap = qMax(kMaxOutFrames, res.suggestCap(frames));
        if (workOut.size() < qsizetype(outCap) * 2) workOut.resize(qsizetype(outCap) * 2);

        int got = 0;
        if (res.identity()) {
            got = qMin(frames, outCap);
            std::memcpy(workOut.data(), st, size_t(got) * 2 * sizeof(float));
        } else {
            got = res.feed(st, frames, workOut.data(), outCap);
        }
        if (got <= 0) return true;

        const int bps = sampleBytesOf(o.fmt);
        QByteArray bytes;
        bytes.resize(got * o.ch * bps);
        char *w = bytes.data();
        for (int i = 0; i < got; ++i) {
            const float L = workOut[i * 2];
            const float R = workOut[i * 2 + 1];
            if (o.ch == 1) {
                storeSample(w, o.fmt, (L + R) * 0.5f);
                w += bps;
            } else {
                storeSample(w, o.fmt, L); w += bps;
                storeSample(w, o.fmt, R); w += bps;
                for (int c = 3; c <= o.ch; ++c) { storeSample(w, o.fmt, 0.0f); w += bps; }
            }
        }
        stash.append(bytes);
        return flushStash();
    };

    auto notifyIndexed = [&](bool ok, const QString &why, qint64 durMs, int rate, qint64 tot) {
        QMetaObject::invokeMethod(p, [p, ok, why, durMs, rate, tot]() {
            p->onIndexed(ok, why, durMs, rate, tot);
        }, Qt::QueuedConnection);
    };

    auto closeSource = [&]() {
        delete src;
        src = nullptr;
        produced = false;
        srcEof = false;
        tailLeftMs = 0;
        stash.clear();
    };

    // 跳点/换曲都要复位：缓冲清空、滤波器历史清零、重采样相位重来
    auto hardReset = [&]() {
        stash.clear();
        {
            QMutexLocker lk(&p->m_mtx);
            p->clearRingLocked();
            p->m_sh.eof = false;
        }
        p->m_fx->reset();
        res.reset();
        resConfigured = false;
        srcEof = false;
        tailLeftMs = 0;
    };

    forever {
        if (p->m_quit.loadRelaxed()) break;
        const Job job = takeJob();

        // ---------------- 装载：交给工厂挑后端（MP3→minimp3，其余→FFmpeg） ----------------
        if (job.cmd == CmdLoad) {
            closeSource();
            QString err;
            src = createPcmSource(job.path, &err);
            if (!src) {
                qWarning() << "[fx] 解码后端都打不开该文件：" << err;
                notifyIndexed(false, err, 0, 0, 0);
                continue;
            }
            srcRate = src->sampleRate();
            const qint64 total = src->totalSamples();
            const qint64 durMs = src->durationMs();
            if (srcRate <= 0 || durMs <= 0) {
                notifyIndexed(false, QStringLiteral("解码器给不出采样率/时长"), 0, 0, 0);
                closeSource();
                continue;
            }
            {
                QMutexLocker lk(&p->m_mtx);
                p->m_sh.srcRate = srcRate;
                p->m_sh.srcCh = 2;                 // 两个后端都统一成交错立体声
                p->m_sh.totalSamples = total;
                p->m_sh.backend = src->backendName();
                p->m_sh.running = true;
                p->m_sh.eof = false;
                p->m_sh.haveFormat = false;        // 等主线程协商输出格式
                p->m_sh.pendingStartMs = qMax<qint64>(0, job.argMs);
            }
            p->m_fx->setSampleRate(srcRate);
            hardReset();
            if (job.argMs > 0) src->seekToSample(job.argMs * srcRate / 1000);
            produced = true;
            notifyIndexed(true, QString(), durMs, srcRate, total);
            continue;
        }
        if (job.cmd == CmdUnload) {
            closeSource();
            QMutexLocker lk(&p->m_mtx);
            p->m_sh.running = false;
            p->m_sh.eof = false;
            continue;
        }
        if (job.cmd == CmdSeek) {
            if (src) {
                hardReset();
                src->seekToSample(job.argMs * qMax(1, srcRate) / 1000);
            }
            continue;
        }
        if (job.cmd == CmdQuit) break;

        // ---------------- 等主线程把输出格式协商好 ----------------
        {
            QMutexLocker lk(&p->m_mtx);
            while (!p->m_quit.loadRelaxed() && p->m_sh.cmd == CmdNone
                   && !p->m_sh.haveFormat && p->m_sh.running) {
                p->m_cond.wait(&p->m_mtx, 30);
            }
        }
        if (!produced || !src) {
            QMutexLocker lk(&p->m_mtx);
            if (!p->m_quit.loadRelaxed() && p->m_sh.cmd == CmdNone) p->m_cond.wait(&p->m_mtx, 60);
            continue;
        }

        // ---------------- 暂停/停止：不产出 ----------------
        {
            QMutexLocker lk(&p->m_mtx);
            if (!p->m_sh.wantWork || p->m_sh.paused || p->m_sh.eof) {
                p->m_cond.wait(&p->m_mtx, 50);
                continue;
            }
        }

        // ---------------- 曲尾：补静音让混响自然衰减完，再报 eof ----------------
        if (srcEof) {
            if (tailLeftMs <= 0) tailLeftMs = EffectPlayer::kTailMs;
            const int frames = qMax(1, srcRate / 40);                 // 25ms 一块
            work.assign(qsizetype(frames) * 2, 0.0f);
            if (!emitFrames(work.constData(), frames)) continue;
            tailLeftMs -= frames * 1000 / qMax(1, srcRate);
            if (tailLeftMs <= 0) {
                QMutexLocker lk(&p->m_mtx);
                p->m_sh.eof = true;
                lk.unlock();
                p->m_cond.wakeAll();
            }
            continue;
        }

        // ---------------- 解码一块 → DSP → 重采样 → 环形缓冲 ----------------
        constexpr int kChunkFrames = 1024;        // ≈23ms，DSP 单次持锁时间很短
        work.resize(qsizetype(kChunkFrames) * 2);
        const int got = src->read(work.data(), kChunkFrames);
        if (got <= 0) {
            srcEof = true;                        // 0=读完，-1=出错：都按到尾处理，别死循环
            continue;
        }
        emitFrames(work.constData(), got);
    }

    closeSource();
    {
        QMutexLocker lk(&p->m_mtx);
        p->m_sh.running = false;
    }
}

} // namespace Muyun
