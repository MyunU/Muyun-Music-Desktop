#pragma once

#include <QObject>
#include <QByteArray>
#include <QIODevice>
#include <QString>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QTimer>
#include <QAtomicInt>
#include <QAudioFormat>

class QAudioSink;
class QAudioDevice;

namespace Muyun {

class AudioEffects;

/**
 * @brief 音效播放管线：解码与 DSP 全在后台线程，主线程只搬运字节
 *
 * 数据流
 *   后台线程：读文件 → Mp3FrameIndex 建帧表 → minimp3 逐帧解码 → int16 → float 立体声
 *             → AudioEffects.process（EQ/混响/环绕/响度，按源采样率，UI 改参即时生效）
 *             → SincResampler（源率 → 设备率）→ 声道/样本格式转换 → 环形缓冲
 *   主线程  ：QAudioSink 调 readData，从环形缓冲 memcpy（零解码、零 DSP）
 *
 * 四条实测硬约束（Qt 6.8.3 FFmpeg 后端 + Windows WASAPI，逐条用探针验证）
 *  1. QAudioSink 在**主线程**用定时器拉 readData → 把解码/DSP 放进 readData 就是"开音效卡 UI"。
 *  2. 自定义 QIODevice **必须重写 bytesAvailable()/atEnd()**，否则后端认为"没有数据"，
 *     readData 一次都不会被调用 —— 旧实现听不见声音的真因（跟格式无关）。
 *  3. suspend() 之后 resume() 不再拉流（state 永久停在 Suspended）→ 暂停只能 stop()，
 *     恢复用同一个 QIODevice 再 start()（实测可续）。
 *  4. 默认输出设备常常只认混音格式（本机实测仅 48000/2ch/Float 被接受，
 *     44100 与 Int16 一律拒绝）→ 必须格式协商 + 重采样；谈不拢就报失败，上层回退 QMediaPlayer。
 *
 * 位置语义（与 QMediaPlayer 对齐）：position = seek 基准 + **已交付的真实内容帧** −
 *  声卡在途量；欠载补的静音不计入位置，所以进度不会跑飞，暂停/seek 立即反映。
 */
class EffectPlayer : public QObject
{
    Q_OBJECT
public:
    explicit EffectPlayer(AudioEffects *fx, QObject *parent = nullptr);
    ~EffectPlayer() override;

    /// 异步装载：立即返回。就绪后在主线程发 loadFinished；未完成时 play() 会被记住
    void loadAsync(const QString &mp3Path, qint64 startMs = 0);
    bool loaded() const { return m_loaded; }

    void play();
    void pause();
    void stop();
    void seek(qint64 ms);
    void setVolume(qreal v);
    qreal volume() const { return m_volume; }
    bool isPlaying() const { return m_playing; }

    qint64 position() const;                 ///< 毫秒
    qint64 duration() const { return m_durationMs; }

    /// 默认输出设备变化：重新协商格式 + 重建 sink，保持位置与播放/暂停状态
    bool onDeviceChanged(const QAudioDevice &device);

    // ---- 诊断 / 自检 ----
    QString deviceName() const { return m_deviceName; }
    QString backendName() const;                 ///< 解码后端："minimp3" / "ffmpeg"
    int outputSampleRate() const { return m_outRate; }
    int sourceSampleRate() const { return m_srcRate; }
    int frameCount() const;                    ///< 帧表条目数（0=还没建表）
    qint64 starvedMs() const;                  ///< 欠载补静音累计（正常应≈0）

signals:
    void loadFinished(bool ok, const QString &reason);
    void positionChanged(qint64 ms);
    void durationChanged(qint64 ms);
    void playbackStateChanged(int state);    ///< 对齐 QMediaPlayer::PlaybackState 数值
    void endOfMedia();

private:
    enum Cmd { CmdNone, CmdLoad, CmdSeek, CmdUnload, CmdSetFormat, CmdQuit };

    /// 主线程 ↔ 后台线程共享状态（一律在 m_mtx 下访问）
    struct Shared
    {
        Cmd cmd = CmdNone;
        QString path;
        qint64 argMs = 0;                  ///< load 起点 / seek 目标
        qint64 pendingStartMs = 0;         ///< 装载完成后要落到的起点

        bool running = false;              ///< 已装载可产出
        bool wantWork = false;             ///< 需要后台继续产出（播放中）
        bool paused = true;
        bool eof = false;                  ///< 后台已产完（含混响尾音）
        bool haveFormat = false;           ///< 主线程已协商好输出格式
        qint64 userSeekMs = -1;            ///< 装载期间用户 seek 的目标（就绪后补做）

        int srcRate = 0;
        int srcCh = 0;
        int frames = 0;                ///< 帧表条目数（minimp3 后端才有意义）
        QString backend;               ///< 实际用的解码后端："minimp3" / "ffmpeg"
        qint64 totalSamples = 0;

        int outRate = 0;
        int outCh = 2;
        QAudioFormat::SampleFormat outFmt = QAudioFormat::Float;

        QByteArray ring;                   ///< 设备格式字节的环形缓冲
        qint64 wpos = 0, rpos = 0, used = 0;
    };

    /// 后台解码线程（常驻复用，不随切歌重建）
    class Pipeline : public QThread
    {
    public:
        explicit Pipeline(EffectPlayer *owner) : m_own(owner) {}
    protected:
        void run() override;
    private:
        EffectPlayer *m_own = nullptr;
    };
    friend class Pipeline;

    /// QAudioSink 的供数口（在主线程被调用）
    class PullDevice : public QIODevice
    {
    public:
        explicit PullDevice(EffectPlayer *owner);
        qint64 bytesAvailable() const override;   ///< ⚠ 不重写＝后端永远不拉
        bool atEnd() const override;
    protected:
        qint64 readData(char *out, qint64 maxlen) override;
        qint64 writeData(const char *, qint64 len) override { return len; }
    private:
        EffectPlayer *m_own = nullptr;
    };
    friend class PullDevice;

    // ---- 主线程侧 ----
    bool negotiateFormat(const QAudioDevice &device);
    bool openSink(const QAudioDevice &device);
    void closeSink();
    void applyVolume();
    void postCmd(Cmd c, const QString &path = QString(), qint64 ms = 0);
    void clearRingLocked();
    qint64 inFlightFrames() const;
    qint64 pullRead(char *out, qint64 maxlen);

    /// 后台建表结果回主线程（协商格式 + 通知上层）
    void onIndexed(bool ok, const QString &reason, qint64 durationMs,
                   int srcRate, qint64 totalSamples);

    AudioEffects *m_fx = nullptr;
    Pipeline *m_thread = nullptr;

    mutable QMutex m_mtx;
    QWaitCondition m_cond;
    Shared m_sh;
    QAtomicInt m_quit { 0 };

    QAudioSink *m_sink = nullptr;
    PullDevice *m_pull = nullptr;
    QString m_deviceName;
    QAudioFormat m_format;
    int m_outRate = 0;
    int m_outCh = 2;
    qint64 m_bytesPerFrame = 8;
    int m_srcRate = 0;
    qint64 m_bufferBytes = 0;              ///< 我方设定的 sink 内部缓冲容量

    bool m_loaded = false;
    bool m_playing = false;
    bool m_pendingPlay = false;            ///< 未就绪时就调了 play()
    qreal m_volume = 0.8;
    qint64 m_durationMs = 0;
    qint64 m_baseFrame = 0;                ///< 位置基准（seek 落点，输出帧序号）
    QAtomicInteger<qint64> m_delivered { 0 };  ///< 已交付给声卡的真实内容帧数
    qint64 m_starvedMs = 0;
    bool m_endEmitted = false;
    QTimer m_probe;                        ///< 位置上报 + 结束判定
    qint64 m_lastReportedMs = -1;

    static constexpr int kRingSeconds = 2;        ///< 环形缓冲容量（吸收主线程抖动）
    // ⚠ 旧值 120ms 太小：拖标题栏时 GUI 线程狂刷 → CPU 争抢 → Pipeline 线程
    //   被调度延迟 → 环形缓冲供数不及 → 120ms 秒空 → 音频卡住（用户报"拖窗就卡"）。
    //   QMediaPlayer 路径没这问题是因为 FFmpeg 后端默认缓冲 ~5s。1500ms 兜住长时间拖动，
    //   延迟增加 ~1.4s（用户无感，比卡住好）。
    static constexpr int kSinkBufferMs = 1500;    ///< sink 内部缓冲（暂停尾巴/延迟）
    static constexpr int kTailMs = 700;           ///< 曲尾留给混响衰减的静音
};

} // namespace Muyun
