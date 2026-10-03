#include "Mp3FrameIndex.h"

#include <cstring>
#include <algorithm>

namespace Muyun {

namespace {

// MPEG 帧头（32bit）：
//   AAAAAAAAAAA VVL L PPPP SSSS XPCC MMEE BBBB
//   A=同步(11bit)  V=版本(2)  L=层(2)  P=码率(4)  S=采样率(2)  X=填充(1)  C=私有(1)  M=声道模式(2)
// 这里只需要 Layer III 的字段，够算帧长与样本数。

constexpr int kBitrateMpeg1L3[16] = { 0, 32, 40, 48, 56, 64, 80, 96,
                                      112, 128, 160, 192, 224, 256, 320, 0 };
constexpr int kBitrateMpeg2L3[16] = { 0, 8, 16, 24, 32, 40, 48, 56,
                                      64, 80, 96, 112, 128, 144, 160, 0 };
constexpr int kRateMpeg1[4]   = { 44100, 48000, 32000, 0 };
constexpr int kRateMpeg2[4]   = { 22050, 24000, 16000, 0 };   // MPEG2
constexpr int kRateMpeg25[4]  = { 11025, 12000, 8000, 0 };    // MPEG2.5

struct Header {
    bool ok = false;
    int sampleRate = 0;
    int channels = 0;       // 1=单声道（M 模式 3），其余按 2 处理
    int samples = 0;        // 每声道每帧样本数
    int frameBytes = 0;
    int version = 0;        // 0=2.5, 1=MPEG2, 2=MPEG1（内部编号）
};

Header parseHeader(const uchar *p, int avail)
{
    Header h;
    if (avail < 4) return h;
    const quint32 w = (quint32(p[0]) << 24) | (quint32(p[1]) << 16)
                    | (quint32(p[2]) << 8) | quint32(p[3]);
    if ((w & 0xFFE00000u) != 0xFFE00000u) return h;        // 11bit 同步

    const int versionBits = int((w >> 19) & 0x3);          // 0=2.5 1=保留 2=2 3=1
    const int layerBits   = int((w >> 17) & 0x3);          // 1=Layer III
    if (layerBits != 1) return h;                          // 只要 Layer III（.mp3）
    Header vh;
    if (versionBits == 3)      vh.version = 2;             // MPEG1
    else if (versionBits == 2) vh.version = 1;             // MPEG2
    else if (versionBits == 0) vh.version = 0;             // MPEG2.5
    else return h;                                        // 保留值

    const int brIdx  = int((w >> 12) & 0xF);
    const int srIdx  = int((w >> 10) & 0x3);
    const int pad    = int((w >> 9) & 0x1);
    const int chMode = int((w >> 6) & 0x3);

    if (brIdx == 0 || brIdx == 15 || srIdx == 3) return h; // 自由码率/保留 → 无法算帧长

    const int bitrateK = (vh.version == 2) ? kBitrateMpeg1L3[brIdx] : kBitrateMpeg2L3[brIdx];
    const int rate = (vh.version == 2) ? kRateMpeg1[srIdx]
                   : (vh.version == 1) ? kRateMpeg2[srIdx] : kRateMpeg25[srIdx];
    if (bitrateK <= 0 || rate <= 0) return h;

    const int samplesPerFrame = (vh.version == 2) ? 1152 : 576;   // Layer III
    // 帧长 = 样本数/8 * 码率(bps) / 采样率 + 填充（Layer III 通用式，MPEG1 与 2/2.5 同形）
    const int frameBytes = samplesPerFrame / 8 * bitrateK * 1000 / rate + pad;
    if (frameBytes < 24) return h;

    vh.ok = true;
    vh.sampleRate = rate;
    vh.channels = (chMode == 3) ? 1 : 2;
    vh.samples = samplesPerFrame;
    vh.frameBytes = frameBytes;
    return vh;
}

/// 跳过 ID3v2（含可选 footer）；返回音频起始偏移
qint64 skipId3v2(const QByteArray &d)
{
    if (d.size() < 10) return 0;
    if (!(uchar(d[0]) == 'I' && uchar(d[1]) == 'D' && uchar(d[2]) == '3')) return 0;
    const uchar flags = uchar(d[5]);
    const qint64 size = (qint64(uchar(d[6])) << 21) | (qint64(uchar(d[7])) << 14)
                      | (qint64(uchar(d[8])) << 7)  | qint64(uchar(d[9]));
    qint64 off = 10 + size;
    if (flags & 0x10) off += 10;                 // 有 footer
    return (off > 0 && off < d.size()) ? off : 0;
}

} // namespace

bool Mp3FrameIndex::build(const QByteArray &mp3Data)
{
    m_entries.clear();
    m_sampleRate = m_channels = 0;
    m_totalSamples = 0;
    m_firstByte = m_lastByte = 0;

    const int n = mp3Data.size();
    if (n < 96) return false;
    const uchar *p = reinterpret_cast<const uchar *>(mp3Data.constData());

    qint64 i = skipId3v2(mp3Data);
    // 末尾可能是 ID3v1（128B "TAG"）或 APEv2，帧扫描自然会在长度校验处停下，无需特判

    // 锁定第一帧：要求"本帧 + 下一帧"头都合法，避免把随机字节当同步
    Header first;
    for (; i + 4 <= n; ++i) {
        const Header h = parseHeader(p + i, n - int(i));
        if (!h.ok) continue;
        const qint64 nxt = i + h.frameBytes;
        if (nxt + 4 > n) { first = h; m_firstByte = i; i = nxt; break; }   // 整曲就一帧
        const Header h2 = parseHeader(p + nxt, n - int(nxt));
        if (h2.ok && h2.sampleRate == h.sampleRate) {
            first = h; m_firstByte = i; i = nxt;
            break;
        }
    }
    if (!first.ok) return false;

    m_sampleRate = first.sampleRate;
    m_channels = first.channels;
    m_entries.reserve(size_t(n / (first.frameBytes > 0 ? first.frameBytes : 1)) + 16);

    qint64 sample = 0;
    // 从第二帧起继续走（第一帧已计入）
    m_entries.append(Entry{ 0, m_firstByte });
    sample += first.samples;
    qint64 lastEnd = i;

    while (i + 4 <= n) {
        const Header h = parseHeader(p + i, n - int(i));
        if (!h.ok) {
            // 遇到非帧数据（补齐块/标签/损坏）：逐字节前移重新找同步；
            // 连续失败过多则收尾，避免在尾部垃圾上白扫
            ++i;
            if (i - lastEnd > first.frameBytes * 12) break;
            continue;
        }
        const qint64 end = i + h.frameBytes;
        if (end > n) break;                       // 越界：到这里就是文件尾
        m_entries.append(Entry{ sample, i });
        sample += h.samples;
        m_sampleRate = h.sampleRate;               // 极少数文件中途变率：以最后一段为准
        m_channels = h.channels;
        i = end;
        lastEnd = i;
    }
    m_totalSamples = sample;
    m_lastByte = lastEnd;
    return m_entries.size() >= 2 && m_sampleRate > 0 && m_totalSamples > 0;
}

qint64 Mp3FrameIndex::durationMs() const
{
    if (m_sampleRate <= 0 || m_totalSamples <= 0) return 0;
    return m_totalSamples * 1000 / m_sampleRate;
}

Mp3FrameIndex::FrameAt Mp3FrameIndex::frameAt(qint64 targetSample) const
{
    FrameAt fa;
    if (m_entries.isEmpty()) return fa;
    if (targetSample <= m_entries.first().sample) {
        fa.sample = m_entries.first().sample;
        fa.byte = m_entries.first().byte;
        return fa;
    }
    if (targetSample >= m_totalSamples) {
        const Entry &e = m_entries.last();
        fa.sample = e.sample;
        fa.byte = e.byte;
        return fa;
    }
    // 二分：找最后一个 sample <= target 的帧
    int lo = 0, hi = int(m_entries.size()) - 1;
    while (lo < hi) {
        const int mid = lo + (hi - lo + 1) / 2;
        if (m_entries.at(mid).sample <= targetSample) lo = mid;
        else hi = mid - 1;
    }
    fa.sample = m_entries.at(lo).sample;
    fa.byte = m_entries.at(lo).byte;
    return fa;
}

} // namespace Muyun
