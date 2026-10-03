#include "PcmSource.h"
#include "FFmpegBridge.h"

#include <QFileInfo>
#include <QElapsedTimer>
#include <QDebug>
#include <cstring>
#include <errno.h>

namespace Muyun {

namespace {

constexpr int kMaxAudioFrameSamples = 8192;      // 单帧上限（Opus 5760 / FLAC 4608 都够）

int64_t rescaleQ(int64_t v, AVRational from, AVRational to)
{
    // av_rescale_q 的等价实现（避免再多解析一个符号）：v * from.num * to.den / (from.den * to.num)
    const long double num = static_cast<long double>(v) * from.num * to.den;
    const long double den = static_cast<long double>(from.den) * to.num;
    if (den == 0.0L) return 0;
    return static_cast<int64_t>(num / den);
}

} // namespace

/**
 * @brief 通用解码后端：运行时调用 Qt 自带的 FFmpeg
 *
 * 覆盖 QMediaPlayer 能播的一切格式（flac / m4a-aac / ogg-vorbis / opus / wav / ape / wma…），
 * 这样"开音效"不再挑音源格式。所有 FFmpeg 结构体字段访问都由 FFmpegBridge 的
 * ABI 主版本校验兜底（版本对不上就直接不用这条路径）。
 *
 * 输出契约：交错立体声 float，采样率 = 源采样率（**不做重采样**，
 * 交给上层统一用 SincResampler 处理，DSP 也保持在源采样率上做）。
 */
class FfmpegPcmSource : public PcmSource
{
public:
    ~FfmpegPcmSource() override { closeAll(); }

    bool open(const QString &path, QString *err) override
    {
        FFmpegBridge &ff = FF();
        if (!ff.available()) {
            if (err) *err = ff.error().isEmpty() ? QStringLiteral("FFmpeg 不可用") : ff.error();
            return false;
        }
        closeAll();

        AVFormatContext *fmt = nullptr;
        const QByteArray utf8 = path.toUtf8();
        if (ff.format_open_input(&fmt, utf8.constData(), nullptr, nullptr) < 0 || !fmt) {
            if (err) *err = QStringLiteral("FFmpeg 打不开文件");
            return false;
        }
        m_fmt = fmt;

        if (ff.format_find_stream_info(m_fmt, nullptr) < 0) {
            if (err) *err = QStringLiteral("FFmpeg 解析流信息失败");
            closeAll();
            return false;
        }
        m_streamIdx = ff.find_best_stream(m_fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (m_streamIdx < 0 || m_streamIdx >= int(m_fmt->nb_streams)) {
            if (err) *err = QStringLiteral("没有音频流");
            closeAll();
            return false;
        }
        AVStream *st = m_fmt->streams[m_streamIdx];
        m_tb = st->time_base;
        AVCodecParameters *par = st->codecpar;

        const AVCodec *dec = ff.find_decoder(par->codec_id);
        if (!dec) { if (err) *err = QStringLiteral("没有对应解码器"); closeAll(); return false; }
        m_ctx = ff.codec_alloc_context3(dec);
        if (!m_ctx || ff.codec_parameters_to_context(m_ctx, par) < 0) {
            if (err) *err = QStringLiteral("复制解码参数失败"); closeAll(); return false;
        }
        m_ctx->pkt_timebase = m_tb;
        if (ff.codec_open2(m_ctx, dec, nullptr) < 0) {
            if (err) *err = QStringLiteral("打开解码器失败"); closeAll(); return false;
        }

        m_rate = m_ctx->sample_rate > 0 ? m_ctx->sample_rate : 44100;
        m_pkt = ff.packet_alloc();
        m_frame = ff.frame_alloc();
        if (!m_pkt || !m_frame) { if (err) *err = QStringLiteral("FFmpeg 分配帧失败"); closeAll(); return false; }

        if (!setupSwr(err)) { closeAll(); return false; }

        // 时长：优先容器 duration（微秒），退回该流的 duration（流时基单位）。
        // 注意 7.1 的 AVCodecParameters 没有 duration 字段，要用 AVStream::duration
        m_totalSamples = 0;
        if (m_fmt->duration > 0)
            m_totalSamples = rescaleQ(m_fmt->duration, { 1, 1000000 }, { 1, m_rate });
        else if (st->duration > 0 && st->duration != int64_t(AV_NOPTS_VALUE) && m_tb.num > 0)
            m_totalSamples = rescaleQ(st->duration, m_tb, { 1, m_rate });
        if (m_totalSamples < 0) m_totalSamples = 0;

        m_pos = 0;
        m_eof = false;
        m_frameSamples = m_frameOff = 0;
        return true;
    }

    int sampleRate() const override { return m_rate; }
    qint64 totalSamples() const override { return m_totalSamples; }
    QString backendName() const override { return QStringLiteral("ffmpeg"); }

    bool seekToSample(qint64 sample) override
    {
        if (!m_fmt || !m_ctx || !FF().seek_frame) return false;
        QElapsedTimer diag; diag.start();
        if (sample < 0) sample = 0;
        const int64_t ts = (m_tb.num > 0) ? rescaleQ(sample, { 1, m_rate }, m_tb) : 0;
        const int seekRet = FF().seek_frame(m_fmt, m_streamIdx, ts, AVSEEK_FLAG_BACKWARD);
        int seekRet2 = 0;
        if (seekRet < 0) {
            // 有些容器只认"全局微秒 + 流号 -1"
            const int64_t usec = sample * 1000000LL / qMax(1, m_rate);
            seekRet2 = FF().seek_frame(m_fmt, -1, usec, AVSEEK_FLAG_BACKWARD);
            if (seekRet2 < 0) return false;
        }
        FF().codec_flush_buffers(m_ctx);
        m_eof = false;
        m_err = false;
        m_drained = false;              // ⚠ 之前漏了：EOF 之后往回 seek 会立刻"没数据"
        m_frameSamples = m_frameOff = 0;
        m_pos = 0;

        // 丢弃 leading：定位点通常落在关键帧之前。guard 必须把"整帧丢弃"也算进预算，
        // 否则一旦落点偏后就会一路丢到文件尾（实测 M4A 长音频因此变成"1/3 速播放"）
        qint64 guard = 0;
        const qint64 limit = qMax<qint64>(m_rate * 3, 1);
        while (m_pos + (m_frameSamples - m_frameOff) < sample && guard < limit) {
            if (m_frameOff < m_frameSamples) {
                const int drop = int(qMin<qint64>(sample - m_pos, m_frameSamples - m_frameOff));
                m_frameOff += drop; m_pos += drop; guard += drop;
                continue;
            }
            if (!pumpOneFrame()) break;
            guard += m_frameSamples;
        }
        if (m_pos < sample) {
            const int drop = int(qMin<qint64>(sample - m_pos, m_frameSamples - m_frameOff));
            m_frameOff += drop; m_pos += drop; guard += drop;
        }
        // seek 落空基本只有一种原因：文件被截断/头部时长虚高（缓存里常见），
        // 此时上层会自然收尾，不该当成管线故障——但要在日志里留痕，否则现场无从判断
        if (m_pos + (m_frameSamples - m_frameOff) < sample)
            qWarning() << "[fx] FFmpeg seek 落空（文件疑似截断）：目标样本" << sample
                       << "实落" << m_pos << "seekRet" << seekRet << seekRet2
                       << "首读错误码" << m_lastRead;
        else
            qDebug() << "[fx] FFmpeg seek ok：目标样本" << sample << "实落" << m_pos
                     << "丢弃" << guard << "用时ms" << diag.elapsed();
        return true;
    }

    int read(float *out, int maxFrames) override
    {
        if (!out || maxFrames <= 0) return 0;
        int produced = 0;
        while (produced < maxFrames) {
            if (m_frameOff >= m_frameSamples) {
                if (m_eof) break;
                if (!pumpOneFrame()) { if (m_err) return produced > 0 ? produced : -1; break; }
                if (m_frameSamples <= 0) continue;      // 这帧没产出（EAGAIN），继续喂
            }
            const int take = qMin(maxFrames - produced, m_frameSamples - m_frameOff);
            if (take <= 0) continue;
            std::memcpy(out + qsizetype(produced) * 2,
                        m_frameBuf.constData() + qsizetype(m_frameOff) * 2,
                        size_t(take) * 2 * sizeof(float));
            produced += take;
            m_frameOff += take;
            m_pos += take;
        }
        return produced;
    }

private:
    bool setupSwr(QString *err)
    {
        FFmpegBridge &ff = FF();
        if (m_swr && ff.swr_free) ff.swr_free(&m_swr);
        AVChannelLayout outL = AV_CHANNEL_LAYOUT_STEREO;
        int r = ff.swr_alloc_set_opts2(&m_swr, &outL, AV_SAMPLE_FMT_FLT, m_rate,
                                       &m_ctx->ch_layout, m_ctx->sample_fmt, m_ctx->sample_rate,
                                       0, nullptr);
        if (r < 0 || !m_swr) {
            if (err) *err = QStringLiteral("建立重采样上下文失败：") + ff.errMsg(r);
            return false;
        }
        r = ff.swr_init(m_swr);
        if (r < 0) {
            if (err) *err = QStringLiteral("初始化重采样失败：") + ff.errMsg(r);
            return false;
        }
        m_frameBuf.resize(qsizetype(kMaxAudioFrameSamples) * 2);
        return true;
    }

    /// 解出一帧并转成交错立体声 float；返回 false 表示没有更多帧（EOF 或错误）
    bool pumpOneFrame()
    {
        FFmpegBridge &ff = FF();
        m_frameSamples = 0;
        m_frameOff = 0;

        for (int spins = 0; spins < 64; ++spins) {
            if (!m_drained) {
                const int rr = ff.read_frame(m_fmt, m_pkt);
                m_lastRead = rr;                     // 诊断用：seek 后读不到数据时看这里
                if (rr < 0) {
                    // 文件读完：把解码器里剩下的帧冲出来
                    m_drained = true;
                    if (ff.codec_send_packet(m_ctx, nullptr) < 0) { m_eof = true; return false; }
                } else {
                    if (m_pkt->stream_index != m_streamIdx) {
                        ff.packet_unref(m_pkt);
                        continue;
                    }
                    const int sr = ff.codec_send_packet(m_ctx, m_pkt);
                    ff.packet_unref(m_pkt);
                    if (sr == AVERROR(EAGAIN)) {
                        // 解码器满：先收一帧再喂
                        if (tryReceive()) return true;
                        continue;
                    }
                    if (sr < 0) continue;               // 丢包继续
                }
            } else if (m_eof) {
                return false;
            }
            if (tryReceive()) return true;
            if (m_eof) return false;
            if (m_drained) { m_eof = true; return false; }
        }
        return false;
    }

    bool tryReceive()
    {
        FFmpegBridge &ff = FF();
        ff.frame_unref(m_frame);
        const int r = ff.codec_receive_frame(m_ctx, m_frame);
        if (r == AVERROR(EAGAIN)) return false;
        if (r == AVERROR_EOF) { m_eof = true; return false; }
        if (r < 0) { m_err = true; m_eof = true; return false; }

        int n = m_frame->nb_samples;
        if (n <= 0) return false;
        // 极端情况：单帧比预留缓冲大（少见编码），扩一下
        if (n > kMaxAudioFrameSamples) {
            m_frameBuf.resize(qsizetype(n + 64) * 2);
            // 注意：kMaxAudioFrameSamples 是常量上限，这里改用动态容量
        }
        if (m_frameBuf.size() < qsizetype(n) * 2) m_frameBuf.resize(qsizetype(n) * 2);

        uint8_t *dst = reinterpret_cast<uint8_t *>(m_frameBuf.data());
        const int got = ff.swr_convert(m_swr, &dst, n,
                                       const_cast<const uint8_t **>(m_frame->data), n);
        if (got <= 0) return false;              // 空帧：当作没产出，外层继续要下一帧

        // 位置以 pts 为准（seek 后能对上号），没 pts 就按累加
        const int64_t pts = m_frame->best_effort_timestamp;
        if (pts != int64_t(AV_NOPTS_VALUE) && m_tb.num > 0)
            m_pos = rescaleQ(pts, m_tb, { 1, m_rate });

        m_frameSamples = got;
        m_frameOff = 0;
        return true;
    }

    void closeAll()
    {
        FFmpegBridge &ff = FF();
        if (m_swr && ff.swr_free) ff.swr_free(&m_swr);
        m_swr = nullptr;
        if (m_frame && ff.frame_free) ff.frame_free(&m_frame);
        m_frame = nullptr;
        if (m_pkt && ff.packet_free) ff.packet_free(&m_pkt);
        m_pkt = nullptr;
        if (m_ctx && ff.codec_free_context) ff.codec_free_context(&m_ctx);
        m_ctx = nullptr;
        if (m_fmt && ff.format_close_input) ff.format_close_input(&m_fmt);
        m_fmt = nullptr;
        m_streamIdx = -1;
        m_rate = 0;
        m_totalSamples = 0;
        m_pos = 0;
        m_frameSamples = m_frameOff = 0;
        m_eof = m_drained = m_err = false;
    }

    AVFormatContext *m_fmt = nullptr;
    AVCodecContext *m_ctx = nullptr;
    AVPacket *m_pkt = nullptr;
    AVFrame *m_frame = nullptr;
    SwrContext *m_swr = nullptr;
    int m_streamIdx = -1;
    AVRational m_tb{ 0, 1 };
    int m_rate = 0;
    qint64 m_totalSamples = 0;
    qint64 m_pos = 0;                 ///< 下一帧要输出的样本序号（每声道）
    QVector<float> m_frameBuf;        ///< 当前帧（交错立体声）
    int m_frameSamples = 0;
    int m_frameOff = 0;
    bool m_eof = false;
    bool m_drained = false;           ///< 已读到文件尾并送过 NULL 包
    bool m_err = false;
    int m_lastRead = 0;               ///< 最近一次 av_read_frame 返回值（诊断）
};

bool pcmHasUniversalBackend()
{
    return FF().available();
}

QString pcmUniversalBackendInfo()
{
    return FF().available() ? FF().versionText() : QString();
}

QString pcmUniversalBackendError()
{
    if (FF().available()) return QString();
    return FF().error().isEmpty() ? QStringLiteral("FFmpeg 后端不可用") : FF().error();
}

/// 供 createPcmSource() 使用的工厂（类定义不外泄到头文件）
PcmSource *createFfmpegPcmSource()
{
    return new FfmpegPcmSource();
}

} // namespace Muyun
