#include "PcmSource.h"
#include "Mp3FrameIndex.h"
#include "FFmpegBridge.h"

#include <QFile>
#include <cstring>

extern "C" {
#include "vendor/minimp3/minimp3.h"
}

namespace Muyun {

/**
 * @brief MP3 专用后端：minimp3 + 帧头索引表
 *
 * 保留它而不是全交给 FFmpeg，有两个理由：
 *  1. 时长是**逐帧累加**出来的，VBR 文件样本级精确（自检里与整曲解码比对误差 0ms），
 *     而容器 duration 只是估计；
 *  2. 零外部依赖 —— 万一随包的 FFmpeg dll 缺失/版本不匹配（ABI 护栏会直接禁用该路径），
 *     MP3 这条最常见的路仍然能出声。
 */
class Mp3PcmSource : public PcmSource
{
public:
    ~Mp3PcmSource() override = default;

    bool open(const QString &path, QString *err) override
    {
        QFile f(path);
        if (path.isEmpty() || !f.open(QIODevice::ReadOnly)) {
            if (err) *err = QStringLiteral("文件打不开");
            return false;
        }
        const qint64 sz = f.size();
        if (sz < 96) { if (err) *err = QStringLiteral("文件过短"); return false; }
        if (sz > qint64(256) * 1024 * 1024) { if (err) *err = QStringLiteral("文件过大"); return false; }
        m_data = f.readAll();
        f.close();

        if (!m_idx.build(m_data)) {
            m_data.clear();
            if (err) *err = QStringLiteral("不是可解码的 MP3");
            return false;
        }
        m_rate = m_idx.sampleRate();
        m_total = m_idx.totalSamples();
        m_bytePos = m_idx.firstByte();
        m_skip = 0;
        m_frameSamples = m_frameOff = 0;
        m_done = false;
        mp3dec_init(&m_dec);
        m_raw.resize(MINIMP3_MAX_SAMPLES_PER_FRAME);
        return true;
    }

    int sampleRate() const override { return m_rate; }
    qint64 totalSamples() const override { return m_total; }
    QString backendName() const override { return QStringLiteral("minimp3"); }

    bool seekToSample(qint64 sample) override
    {
        if (!m_idx.valid() || m_rate <= 0) return false;
        if (sample < 0) sample = 0;
        if (sample > m_total) sample = m_total;
        const auto fa = m_idx.frameAt(sample);
        m_bytePos = fa.byte;
        m_skip = qMax<qint64>(0, sample - fa.sample);
        m_frameSamples = m_frameOff = 0;
        m_done = false;
        mp3dec_init(&m_dec);            // 跳点必须重置解码器状态（比特池不跨帧）
        return true;
    }

    int read(float *out, int maxFrames) override
    {
        if (!out || maxFrames <= 0) return 0;
        int produced = 0;
        while (produced < maxFrames) {
            if (m_frameOff >= m_frameSamples) {
                if (!pumpOneFrame()) break;
                if (m_frameSamples <= 0) continue;
            }
            const int take = qMin(maxFrames - produced, m_frameSamples - m_frameOff);
            if (take <= 0) continue;
            std::memcpy(out + qsizetype(produced) * 2,
                        m_frameBuf.constData() + qsizetype(m_frameOff) * 2,
                        size_t(take) * 2 * sizeof(float));
            produced += take;
            m_frameOff += take;
        }
        return produced;
    }

private:
    /// 解一帧（含 seek 后的 leading 丢弃）；false = 到文件尾
    bool pumpOneFrame()
    {
        m_frameSamples = 0;
        m_frameOff = 0;
        if (m_done || m_data.isEmpty()) return false;

        // 连续跳过的坏块计数：太多就认为到尾部了，别在垃圾数据上空转
        for (int tries = 0; tries < 64; ++tries) {
            const qint64 remain = m_data.size() - m_bytePos;
            if (remain <= 4) { m_done = true; return false; }

            mp3dec_frame_info_t info{};
            const int samples = mp3dec_decode_frame(
                &m_dec,
                reinterpret_cast<const uint8_t *>(m_data.constData()) + m_bytePos,
                int(qMin<qint64>(remain, qint64(1) << 20)),
                m_raw.data(), &info);
            if (info.frame_bytes <= 0) {
                m_bytePos += 1;                       // 逐字节重新找同步
                if (m_bytePos >= m_idx.lastByte()) { m_done = true; return false; }
                continue;
            }
            m_bytePos += info.frame_bytes;
            if (samples <= 0) continue;               // 标签/元数据帧
            const int rate = info.hz > 0 ? info.hz : m_rate;
            if (rate != m_rate) continue;             // 中途变率：跳过该帧，保持格式稳定

            const int ch = info.channels > 0 ? info.channels : 2;
            int frames = samples;
            // seek 后的 leading 样本先扣掉（样本级精确落点）
            if (m_skip > 0) {
                const qint64 drop = qMin<qint64>(m_skip, samples);
                m_skip -= drop;
                if (drop >= samples) continue;
                const int keep = samples - int(drop);
                // 就地把保留部分前移（mono/stereo 都按"帧"为单位搬）
                if (ch == 1) {
                    for (int i = 0; i < keep; ++i) m_raw[i] = m_raw[int(drop) + i];
                } else {
                    for (int i = 0; i < keep * 2; ++i)
                        m_raw[i] = m_raw[qsizetype(drop) * 2 + i];
                }
                frames = keep;
            }

            m_frameBuf.resize(qsizetype(frames) * 2);
            if (ch == 1) {
                for (int i = 0; i < frames; ++i) {
                    const float v = m_raw[i] / 32768.0f;
                    m_frameBuf[i * 2] = v;
                    m_frameBuf[i * 2 + 1] = v;
                }
            } else {
                for (int i = 0; i < frames * 2; ++i)
                    m_frameBuf[i] = m_raw[i] / 32768.0f;
            }
            m_frameSamples = frames;
            m_frameOff = 0;
            if (m_bytePos >= m_idx.lastByte()) m_done = true;   // 下一帧就是尾
            return true;
        }
        m_done = true;
        return false;
    }

    QByteArray m_data;                 // 整曲字节（后台线程私有）
    Mp3FrameIndex m_idx;
    mp3dec_t m_dec{};
    QVector<int16_t> m_raw;
    QVector<float> m_frameBuf;
    int m_rate = 0;
    qint64 m_total = 0;
    qint64 m_bytePos = 0;
    qint64 m_skip = 0;
    int m_frameSamples = 0;
    int m_frameOff = 0;
    bool m_done = false;
};

bool pcmLooksLikeMp3(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray h = f.read(4);
    f.close();
    if (h.startsWith("ID3")) return true;
    if (h.size() >= 2 && static_cast<quint8>(h[0]) == 0xFF
        && (static_cast<quint8>(h[1]) & 0xE0) == 0xE0) return true;
    return false;
}

PcmSource *createFfmpegPcmSource();   // 定义在 FfmpegPcmSource.cpp

/// 按"最精确优先"的顺序挑后端：MP3 用 minimp3（样本级时长），其余交给 FFmpeg
PcmSource *createPcmSource(const QString &path, QString *err)
{
    QString mp3Err, ffErr;

    if (pcmLooksLikeMp3(path)) {
        auto *m = new Mp3PcmSource();
        if (m->open(path, &mp3Err)) return m;
        delete m;
    }
    if (FF().available()) {
        auto *f = createFfmpegPcmSource();
        if (f) {
            if (f->open(path, &ffErr)) return f;
            delete f;
        }
    } else {
        ffErr = FF().error().isEmpty() ? QStringLiteral("FFmpeg 不可用") : FF().error();
    }

    if (err) {
        QString both;
        if (!mp3Err.isEmpty()) both += QStringLiteral("MP3：") + mp3Err;
        if (!ffErr.isEmpty()) both += (both.isEmpty() ? QString() : QStringLiteral("；"))
                                     + QStringLiteral("FFmpeg：") + ffErr;
        if (both.isEmpty()) both = QStringLiteral("没有可用的解码后端");
        *err = both;
    }
    return nullptr;
}

} // namespace Muyun
