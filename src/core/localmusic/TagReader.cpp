#include "TagReader.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <cmath>

namespace Muyun {

namespace {

inline quint32 be32(const char *p)
{
    return (static_cast<quint32>(static_cast<quint8>(p[0])) << 24) |
           (static_cast<quint32>(static_cast<quint8>(p[1])) << 16) |
           (static_cast<quint32>(static_cast<quint8>(p[2])) << 8) |
           static_cast<quint32>(static_cast<quint8>(p[3]));
}

inline quint16 be16(const char *p)
{
    return static_cast<quint16>((static_cast<quint8>(p[0]) << 8) |
                                static_cast<quint8>(p[1]));
}

/// ID3v2 syncsafe integer（每字节只用 7 位）
inline quint32 syncsafe(const char *p)
{
    return (static_cast<quint32>(p[0] & 0x7f) << 21) |
           (static_cast<quint32>(p[1] & 0x7f) << 14) |
           (static_cast<quint32>(p[2] & 0x7f) << 7) |
           static_cast<quint32>(p[3] & 0x7f);
}

// MPEG 音频帧的比特率表 [version][layer][index]
const int kMp3Bitrates[4][4][16] = {
    { {0,32,64,96,128,160,192,224,256,288,320,352,384,416,448,0},
      {0,32,48,56,64,80,96,112,128,160,192,224,256,320,384,0},
      {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0},
      {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0} },
    { {0,32,48,56,64,80,96,112,128,144,160,176,192,224,256,0},
      {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0},
      {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0},
      {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0} },
    { {0,32,48,56,64,80,96,112,128,144,160,176,192,224,256,0},
      {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0},
      {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0},
      {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0} },
    { {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
      {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
      {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
      {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0} }
};

const int kMp3SampleRates[4][4] = {
    {44100, 48000, 32000, 0},
    {22050, 24000, 16000, 0},
    {11025, 12000, 8000, 0},
    {0,0,0,0}
};

// ---------------------------------------------------------------------------
// 精确时长辅助（four-59：用户报"本地音乐列表时长与实际播放不符，播放后才正常"）
// ---------------------------------------------------------------------------

inline int le32(const char *p)
{
    return int(static_cast<quint8>(p[0])) |
           (int(static_cast<quint8>(p[1])) << 8) |
           (int(static_cast<quint8>(p[2])) << 16) |
           (int(static_cast<quint8>(p[3])) << 24);
}

inline qint64 le64(const char *p)
{
    qint64 v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | int(static_cast<quint8>(p[i]));
    return v;
}

/// 解析 MPEG 帧头：合法返回 true 并给出遍历所需的全部字段；version 3=MPEG1 2=MPEG2 1=MPEG2.5
bool parseMp3Header(const char *p, int &version, int &layer, int &bitrate,
                    int &sampleRate, int &padding, int &frameSize, int &samples)
{
    const quint8 b0 = static_cast<quint8>(p[0]);
    const quint8 b1 = static_cast<quint8>(p[1]);
    if (b0 != 0xFF || (b1 & 0xE0) != 0xE0) return false;
    const int vBits = (b1 >> 3) & 0x03;
    const int lBits = (b1 >> 1) & 0x03;
    if (vBits == 1 || lBits == 0) return false;   // 保留值
    version = (vBits == 3) ? 3 : (vBits == 2 ? 2 : 1);
    layer = 4 - lBits;                            // 01=LayerIII → 3
    const int brIdx = (static_cast<quint8>(p[2]) >> 4) & 0x0F;
    const int srIdx = (static_cast<quint8>(p[2]) >> 2) & 0x03;
    padding = (static_cast<quint8>(p[2]) >> 1) & 0x01;
    if (brIdx == 0 || brIdx == 15 || srIdx == 3) return false;  // free/保留 → 伪同步
    const int vRow = version == 3 ? 0 : (version == 2 ? 1 : 2);
    bitrate = kMp3Bitrates[vRow][layer - 1][brIdx];
    sampleRate = kMp3SampleRates[vRow][srIdx];
    if (bitrate <= 0 || sampleRate <= 0) return false;
    if (layer == 1) {                              // Layer I
        samples = 384;
        frameSize = (12 * bitrate * 1000 / sampleRate + padding) * 4;
    } else {                                       // Layer II/III
        const bool m1 = version == 3;
        samples = m1 ? 1152 : 576;
        frameSize = (m1 ? 144 : 72) * bitrate * 1000 / sampleRate + padding;
    }
    return frameSize >= 4;
}

/// 无 Xing/Info 头时的**精确** MP3 时长：从 start 起流式读文件、逐帧解析头并按帧长跳进，
/// 累计每帧样本数（CBR/VBR 都准——旧写法用首帧比特率×文件大小估算，VBR 必偏，
/// 用户报"列表时长与实际播放不符、播放后才正常"就是它）。每帧都做"下一帧也是合法帧头"
/// 的一致性校验抗伪同步；结构太怪时返回 0，调用方回退首帧码率估算。
qint64 mp3WalkDuration(QFile &f, qint64 start, qint64 audioEnd, qint64 *framesOut = nullptr)
{
    constexpr qint64 kChunk = 256 * 1024;
    constexpr qint64 kResyncBudget = 4 * 1024 * 1024;   // 伪同步乱扫预算（超=结构不可信）
    QByteArray buf;
    qint64 bufStart = -1;
    qint64 pos = start;
    qint64 totalSamples = 0;
    qint64 frameCount = 0;
    qint64 resyncLeft = kResyncBudget;
    int sampleRate = 0;

    auto refill = [&](qint64 at) -> bool {
        if (at + 8 > audioEnd) return false;
        f.seek(at);
        buf = f.read(qMin<qint64>(kChunk, audioEnd - at));
        bufStart = at;
        return buf.size() >= 8;
    };

    if (!refill(pos)) return 0;
    int guard = 0;
    while (pos + 8 <= audioEnd) {
        if (++guard > 50'000'000) return 0;              // 每轮至少前进 4 字节，纯防呆
        if (pos < bufStart || pos + 8 > bufStart + buf.size()) {
            if (!refill(pos)) break;
            if (pos < bufStart || pos + 8 > bufStart + buf.size()) break;
        }
        const qsizetype i = static_cast<qsizetype>(pos - bufStart);
        int version, layer, bitrate, sr, padding, frameSize, samples;
        if (!parseMp3Header(buf.constData() + i, version, layer, bitrate,
                            sr, padding, frameSize, samples)) {
            ++pos;
            if ((resyncLeft -= 1) < 0) return 0;
            continue;
        }
        if (sampleRate == 0) sampleRate = sr;

        const qint64 next = pos + frameSize;
        if (next + 8 > audioEnd) {                       // 最后一帧
            totalSamples += samples; ++frameCount;
            break;
        }
        if (next < bufStart || next + 8 > bufStart + buf.size()) {
            if (!refill(next)) {                         // 到尾
                totalSamples += samples; ++frameCount;
                break;
            }
        }
        const qsizetype j = static_cast<qsizetype>(next - bufStart);
        int v2, l2, b2, sr2, p2, fs2, s2;
        if (parseMp3Header(buf.constData() + j, v2, l2, b2, sr2, p2, fs2, s2)) {
            totalSamples += samples; ++frameCount;
            pos = next;
            sampleRate = sr;                             // 以首帧为准（版本/采样率中途跳变极罕见）
        } else {
            ++pos;                                       // 当前帧是伪同步 → 往后重找
            if ((resyncLeft -= 1) < 0) return 0;
        }
    }
    if (framesOut) *framesOut = frameCount;
    if (totalSamples <= 0 || sampleRate <= 0) return 0;
    const qint64 dur = totalSamples / sampleRate;
    return (dur > 0 && dur < 20 * 3600) ? dur : 0;
}

/// OGG 精确时长：读**最后一页**的 granule position（页内 PCM 样本计数）÷ 首页 ID 头的采样率
/// （旧写法只有一行注释、时长恒 0 → 列表显示 0:00，播放后才被纠正，同一条用户反馈）。
/// "OggS" 捕获串也可能出现在包数据里，所以从尾往回找并校验页头结构（version=0、段表完整可读）。
qint64 oggDurationSec(QFile &f, int *sampleRateOut)
{
    *sampleRateOut = 0;
    const qint64 size = f.size();
    if (size < 64) return 0;

    f.seek(0);
    const QByteArray head = f.read(64 * 1024);
    const qsizetype idPos = head.indexOf("\x01vorbis");
    if (idPos < 0 || idPos + 16 > head.size()) return 0;
    const int sampleRate = le32(head.constData() + idPos + 12);
    if (sampleRate <= 0) return 0;
    *sampleRateOut = sampleRate;

    const qint64 tailLen = qMin<qint64>(128 * 1024, size);
    f.seek(size - tailLen);
    const QByteArray tail = f.read(tailLen);
    for (qsizetype p = tail.size() - 27; p >= 0; --p) {
        if (tail.mid(p, 4) != QByteArray("OggS", 4)) continue;
        if (static_cast<quint8>(tail[p + 4]) != 0) continue;        // structure_version
        const int segCount = static_cast<quint8>(tail[p + 26]);
        const qsizetype headerLen = 27 + segCount;
        if (p + headerLen > tail.size()) continue;
        int segSum = 0;
        for (int s = 0; s < segCount; ++s)
            segSum += static_cast<int>(static_cast<quint8>(tail[p + 27 + s]));
        if (p + headerLen + segSum > tail.size()) continue;         // 页不完整 → 非真页头
        const qint64 granule = le64(tail.constData() + p + 6);
        if (granule <= 0) continue;
        const qint64 dur = granule / sampleRate;
        return (dur > 0 && dur < 20 * 3600) ? dur : 0;
    }
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------

QString TagReader::decodeText(const QByteArray &raw)
{
    if (raw.isEmpty()) return QString();
    const quint8 encoding = static_cast<quint8>(raw[0]);
    const QByteArray body = raw.mid(1);
    switch (encoding) {
    case 0x00: // ISO-8859-1
        return QString::fromLatin1(body);
    case 0x01: { // UTF-16 with BOM
        if (body.size() >= 2) {
            const quint8 b0 = static_cast<quint8>(body[0]);
            const quint8 b1 = static_cast<quint8>(body[1]);
            if (b0 == 0xFF && b1 == 0xFE)
                return QString::fromUtf16(
                    reinterpret_cast<const char16_t *>(body.constData() + 2),
                    static_cast<qsizetype>((body.size() - 2) / 2));
            if (b0 == 0xFE && b1 == 0xFF) {
                QString out;
                for (int i = 2; i + 1 < body.size(); i += 2)
                    out.append(QChar(static_cast<quint16>(
                        (static_cast<quint8>(body[i]) << 8) |
                        static_cast<quint8>(body[i + 1]))));
                return out;
            }
        }
        return QString::fromUtf16(
            reinterpret_cast<const char16_t *>(body.constData()),
            static_cast<qsizetype>(body.size() / 2));
    }
    case 0x02: // UTF-16BE
        return QString::fromUtf16(
            reinterpret_cast<const char16_t *>(body.constData()),
            static_cast<qsizetype>(body.size() / 2));
    case 0x03: // UTF-8
        return QString::fromUtf8(body);
    default:
        return QString::fromUtf8(body);
    }
}

QImage TagReader::parseId3Picture(const QByteArray &frame)
{
    // APIC: encoding(1) + mime(...\0) + type(1) + description(...\0) + data
    int pos = 1;
    int zero = frame.indexOf('\0', pos);
    if (zero < 0) return QImage();
    const QByteArray mime = frame.mid(pos, zero - pos);
    Q_UNUSED(mime)
    pos = zero + 1;
    if (pos >= frame.size()) return QImage();
    pos += 1; // picture type
    zero = frame.indexOf('\0', pos);
    if (zero < 0) return QImage();
    pos = zero + 1;
    const QByteArray imageData = frame.mid(pos);
    return QImage::fromData(imageData);
}

QByteArray TagReader::findId3Frame(const QByteArray &data, const char *frameId)
{
    const QByteArray target(frameId);
    qsizetype pos = 0;
    while (pos + 10 <= data.size()) {
        const QByteArray id = data.mid(pos, 4);
        if (id.isEmpty() || !isprint(static_cast<unsigned char>(id[0]))) return QByteArray();
        const quint32 size = syncsafe(data.constData() + pos + 4);
        if (size == 0) return QByteArray();
        if (id == target) return data.mid(pos + 10, static_cast<qsizetype>(size));
        pos += 10 + size;
    }
    return QByteArray();
}

// ---------------------------------------------------------------------------
// ID3v2 (MP3)
// ---------------------------------------------------------------------------

bool TagReader::readId3v2(QFile &f, LocalTags &tags)
{
    f.seek(0);
    const QByteArray header = f.read(10);
    if (header.size() < 10 || !header.startsWith("ID3")) return false;

    const quint32 tagSize = syncsafe(header.constData() + 6);
    if (tagSize == 0 || tagSize > 64u * 1024u * 1024u) return false;
    const QByteArray data = f.read(static_cast<qint64>(tagSize));

    auto text = [&](const char *id) {
        const QByteArray frame = findId3Frame(data, id);
        return frame.isEmpty() ? QString() : decodeText(frame).trimmed();
    };

    tags.title = text("TIT2");
    tags.artist = text("TPE1");
    tags.album = text("TALB");
    tags.albumArtist = text("TPE2");
    tags.composer = text("TCOM");
    tags.genre = text("TCON");
    tags.comment = text("COMM");
    tags.lyrics = text("USLT");

    const QString year = text("TDRC").isEmpty() ? text("TYER") : text("TDRC");
    tags.year = year.left(4).toInt();

    const QString track = text("TRCK");
    if (!track.isEmpty()) {
        const int slash = track.indexOf(QLatin1Char('/'));
        tags.trackNo = track.left(slash < 0 ? track.length() : slash).toInt();
        if (slash >= 0) tags.trackTotal = track.mid(slash + 1).toInt();
    }
    const QString disc = text("TPOS");
    if (!disc.isEmpty()) {
        const int slash = disc.indexOf(QLatin1Char('/'));
        tags.discNo = disc.left(slash < 0 ? disc.length() : slash).toInt();
        if (slash >= 0) tags.discTotal = disc.mid(slash + 1).toInt();
    }

    const QByteArray pic = findId3Frame(data, "APIC");
    if (!pic.isEmpty()) {
        const QImage img = parseId3Picture(pic);
        if (!img.isNull()) { tags.cover = img; tags.hasCover = true; }
    }
    return true;
}

bool TagReader::readMp3Info(QFile &f, AudioInfo &info)
{
    // 定位第一个 MPEG 帧头（跳过 ID3v2 与可能的 APE/ID3v1）
    f.seek(0);
    const QByteArray head = f.peek(10);
    qint64 offset = 0;
    if (head.startsWith("ID3")) {
        const quint32 tagSize = syncsafe(head.constData() + 6);
        offset = 10 + tagSize;
    }
    f.seek(offset);
    const QByteArray buf = f.read(4096);
    if (buf.isEmpty()) return false;

    for (int i = 0; i + 4 <= buf.size(); ++i) {
        const quint8 b0 = static_cast<quint8>(buf[i]);
        const quint8 b1 = static_cast<quint8>(buf[i + 1]);
        if (b0 != 0xFF || (b1 & 0xE0) != 0xE0) continue;

        const int versionBits = (b1 >> 3) & 0x03;
        const int layerBits = (b1 >> 1) & 0x03;
        if (versionBits == 1 || layerBits == 0) continue;

        static const int versionMap[4] = {3, -1, 2, 1}; // 11=MPEG1, 10=MPEG2, 00=MPEG2.5
        const int version = versionMap[versionBits];
        const int layer = 4 - layerBits; // 01=LayerIII -> 3
        const int bitrateIndex = (static_cast<quint8>(buf[i + 2]) >> 4) & 0x0F;
        const int sampleRateIndex = (static_cast<quint8>(buf[i + 2]) >> 2) & 0x03;
        const int padding = (static_cast<quint8>(buf[i + 2]) >> 1) & 0x01;

        int bitrate = 0;
        if (version == 3) bitrate = kMp3Bitrates[0][layer - 1][bitrateIndex];
        else bitrate = kMp3Bitrates[1][layer - 1][bitrateIndex];
        const int sampleRate = version == 3 ? kMp3SampleRates[0][sampleRateIndex]
                                            : kMp3SampleRates[version == 2 ? 1 : 2][sampleRateIndex];
        // bitrateIndex 15 = 自由/保留（表里常是 0 或 146 等垃圾值），sampleRateIndex 3 同理。
        // 用这类帧估时长会爆炸（如 7584:37:12），直接跳过找下一帧头
        if (bitrateIndex == 15 || bitrate <= 0 || bitrate > 1500 || sampleRate <= 0) continue;

        info.valid = true;
        info.format = QStringLiteral("MP3");
        info.codec = QStringLiteral("MPEG-1 Layer %1").arg(layer);
        info.bitrate = bitrate;
        info.sampleRate = sampleRate;
        info.lossless = false;

        // 时长判定顺序（four-59）＝**帧头遍历优先** → Xing/Info → 首帧码率估算。
        // 为什么不再 Xing 优先：实测遇到坏 Info 头（frames 字段写成真实值的 2 倍）——盲信它
        // 就得到"列表 520s、实际播放 260s、点歌播放后才正常"，正是用户报的那个 bug。
        // 遍历数的是真实帧，坏头骗不了它；Xing 只在遍历失败（结构太怪）时兜底。
        qint64 audioEnd = f.size();
        if (audioEnd > 128) {
            f.seek(audioEnd - 128);
            if (f.read(3) == QByteArray("TAG", 3)) audioEnd -= 128;   // 末尾 ID3v1 不算音频
        }
        qint64 walkedFrames = 0;
        const qint64 exactDur = mp3WalkDuration(f, offset + i, audioEnd, &walkedFrames);
        if (exactDur > 0) {
            info.durationSec = static_cast<int>(exactDur);
            if (f.size() > 0)
                info.bitrate = static_cast<int>(f.size() * 8 / (info.durationSec * 1000));
            return true;
        }

        // 遍历失败（伪同步超预算/结构不可信）→ Xing/Info VBR 头：duration = frames*samples/rate
        const int xin = buf.indexOf("Xing", i);
        const int inf = buf.indexOf("Info", i);
        const int xp = (xin >= 0 && (inf < 0 || xin < inf)) ? xin : inf;
        if (xp >= 0 && xp + 12 <= buf.size()) {
            const quint32 flags = be32(buf.constData() + xp + 4);
            if (flags & 0x1) {   // bit0 = frames 字段存在
                const quint32 frames = be32(buf.constData() + xp + 8);
                const int samplesPerFrame = (version == 3) ? 1152 : 576;
                const qint64 dur = frames > 0
                    ? static_cast<qint64>(frames) * samplesPerFrame / sampleRate : 0;
                // 上限防"假 Xing"（文件里恰好出现这四个字节 → frames 是垃圾 → 时长爆炸）
                if (dur > 0 && dur < 20 * 3600) {
                    info.durationSec = static_cast<int>(dur);
                    if (f.size() > 0)
                        info.bitrate = static_cast<int>(f.size() * 8
                                                        / (info.durationSec * 1000));
                    return true;
                }
            }
        }

        // 兜底：平均比特率估算（结构太怪的少数文件）。
        // 异常值（>20h）视为垃圾帧，继续找下一帧
        const qint64 audioBytes = f.size() - offset;
        if (bitrate > 0) {
            const int est = static_cast<int>(audioBytes * 8 / (bitrate * 1000));
            if (est > 0 && est < 20 * 3600) {
                info.durationSec = est;
                return true;
            }
            continue;
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// FLAC
// ---------------------------------------------------------------------------

bool TagReader::readFlac(QFile &f, LocalTags &tags, AudioInfo &info)
{
    f.seek(0);
    const QByteArray magic = f.read(4);
    if (magic != "fLaC") return false;

    info.valid = true;
    info.format = QStringLiteral("FLAC");
    info.codec = QStringLiteral("FLAC");
    info.lossless = true;

    bool last = false;
    while (!last && !f.atEnd()) {
        const QByteArray blockHeader = f.read(4);
        if (blockHeader.size() < 4) break;
        const quint8 type = static_cast<quint8>(blockHeader[0]) & 0x7F;
        last = (static_cast<quint8>(blockHeader[0]) & 0x80) != 0;
        const quint32 length = be32(blockHeader.constData()) & 0x00FFFFFF;
        if (length == 0) break;

        if (type == 0) { // STREAMINFO
            const QByteArray si = f.read(static_cast<qint64>(length));
            if (si.size() < 18) break;
            // FLAC 规范 STREAMINFO 位布局（大端位流）：
            //   minBlock(10) maxBlock(10) minFrame(24) maxFrame(24)  ← si[0..9]
            //   sampleRate(20) channels-1(3) bitsPS-1(5) totalSamples(36)  ← si[10..17]
            //   MD5(128)  ← si[18..33]
            // 故 sampleRate 起于 si[10]、totalSamples 起于 si[13] 低 4 位。
            // （旧实现把 totalSamples 读成 si[5..10]——落在帧大小字段上，24bit flac 时长爆炸成几百小时）
            quint64 packed = 0;
            for (int b = 10; b <= 17; ++b)
                packed = (packed << 8) | static_cast<quint64>(static_cast<quint8>(si[b]));
            const int sampleRate = static_cast<int>((packed >> 44) & 0xFFFFF);
            const int bits       = static_cast<int>((packed >> 36) & 0x1F) + 1;
            const quint64 totalSamples = packed & 0xFFFFFFFFFULL;
            info.sampleRate = sampleRate;
            info.bitsPerSample = bits;
            const qint64 dur = sampleRate > 0 ? static_cast<qint64>(totalSamples / sampleRate) : 0;
            if (dur > 0 && dur < 20 * 3600)   // 流式编码 totalSamples 可为 0（未知）→ 留给播放校准兜底
                info.durationSec = static_cast<int>(dur);
            if (info.durationSec > 0 && f.size() > 0)
                info.bitrate = static_cast<int>(f.size() * 8
                                                / (info.durationSec * 1000));
        } else if (type == 4) { // VORBIS_COMMENT
            const QByteArray vc = f.read(static_cast<qint64>(length));
            qsizetype pos = 4; // vendor length (4 bytes LE)
            if (pos > vc.size()) continue;
            quint32 vendorLen = 0;
            vendorLen |= static_cast<quint32>(static_cast<quint8>(vc[0]));
            vendorLen |= static_cast<quint32>(static_cast<quint8>(vc[1])) << 8;
            vendorLen |= static_cast<quint32>(static_cast<quint8>(vc[2])) << 16;
            vendorLen |= static_cast<quint32>(static_cast<quint8>(vc[3])) << 24;
            pos = 4 + vendorLen;
            if (pos + 4 > vc.size()) continue;
            quint32 count = 0;
            count |= static_cast<quint32>(static_cast<quint8>(vc[pos]));
            count |= static_cast<quint32>(static_cast<quint8>(vc[pos + 1])) << 8;
            count |= static_cast<quint32>(static_cast<quint8>(vc[pos + 2])) << 16;
            count |= static_cast<quint32>(static_cast<quint8>(vc[pos + 3])) << 24;
            pos += 4;
            for (quint32 i = 0; i < count && pos + 4 <= vc.size(); ++i) {
                quint32 len = 0;
                len |= static_cast<quint32>(static_cast<quint8>(vc[pos]));
                len |= static_cast<quint32>(static_cast<quint8>(vc[pos + 1])) << 8;
                len |= static_cast<quint32>(static_cast<quint8>(vc[pos + 2])) << 16;
                len |= static_cast<quint32>(static_cast<quint8>(vc[pos + 3])) << 24;
                pos += 4;
                if (pos + static_cast<qsizetype>(len) > vc.size()) break;
                const QString entry = QString::fromUtf8(vc.mid(pos, static_cast<qsizetype>(len)));
                pos += len;
                const int eq = entry.indexOf(QLatin1Char('='));
                if (eq < 0) continue;
                const QString key = entry.left(eq).toUpper();
                const QString value = entry.mid(eq + 1);
                if (key == QStringLiteral("TITLE")) tags.title = value;
                else if (key == QStringLiteral("ARTIST")) tags.artist = value;
                else if (key == QStringLiteral("ALBUM")) tags.album = value;
                else if (key == QStringLiteral("ALBUMARTIST")) tags.albumArtist = value;
                else if (key == QStringLiteral("DATE")) tags.year = value.left(4).toInt();
                else if (key == QStringLiteral("TRACKNUMBER")) tags.trackNo = value.toInt();
                else if (key == QStringLiteral("GENRE")) tags.genre = value;
                else if (key == QStringLiteral("LYRICS")) tags.lyrics = value;
            }
        } else if (type == 6) { // PICTURE
            const QByteArray pic = f.read(static_cast<qint64>(length));
            qsizetype p = 8; // type(4) + mime length(4)
            if (p > pic.size()) continue;
            quint32 mimeLen = be32(pic.constData() + 4);
            p = 8 + mimeLen;
            if (p + 4 > pic.size()) continue;
            quint32 descLen = be32(pic.constData() + p);
            p += 4 + descLen;
            p += 16; // width, height, depth, colors
            if (p + 4 > pic.size()) continue;
            quint32 dataLen = be32(pic.constData() + p);
            p += 4;
            if (p + static_cast<qsizetype>(dataLen) > pic.size()) continue;
            const QImage img = QImage::fromData(pic.mid(p, static_cast<qsizetype>(dataLen)));
            if (!img.isNull()) { tags.cover = img; tags.hasCover = true; }
        } else {
            f.skip(static_cast<qint64>(length));
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// OGG (Vorbis)
// ---------------------------------------------------------------------------

bool TagReader::readOgg(QFile &f, LocalTags &tags)
{
    f.seek(0);
    const QByteArray magic = f.read(4);
    if (magic != "OggS") return false;

    // 第二页通常是 comment header
    QByteArray data = f.read(64 * 1024);
    const int commentPos = data.indexOf("\x03vorbis", 1);
    if (commentPos < 0) return false;

    qsizetype pos = commentPos + 7;
    if (pos + 4 > data.size()) return false;
    quint32 vendorLen = 0;
    vendorLen |= static_cast<quint32>(static_cast<quint8>(data[pos]));
    vendorLen |= static_cast<quint32>(static_cast<quint8>(data[pos + 1])) << 8;
    vendorLen |= static_cast<quint32>(static_cast<quint8>(data[pos + 2])) << 16;
    vendorLen |= static_cast<quint32>(static_cast<quint8>(data[pos + 3])) << 24;
    pos += 4 + vendorLen;
    if (pos + 4 > data.size()) return false;
    quint32 count = 0;
    count |= static_cast<quint32>(static_cast<quint8>(data[pos]));
    count |= static_cast<quint32>(static_cast<quint8>(data[pos + 1])) << 8;
    count |= static_cast<quint32>(static_cast<quint8>(data[pos + 2])) << 16;
    count |= static_cast<quint32>(static_cast<quint8>(data[pos + 3])) << 24;
    pos += 4;

    for (quint32 i = 0; i < count && pos + 4 <= data.size(); ++i) {
        quint32 len = 0;
        len |= static_cast<quint32>(static_cast<quint8>(data[pos]));
        len |= static_cast<quint32>(static_cast<quint8>(data[pos + 1])) << 8;
        len |= static_cast<quint32>(static_cast<quint8>(data[pos + 2])) << 16;
        len |= static_cast<quint32>(static_cast<quint8>(data[pos + 3])) << 24;
        pos += 4;
        if (pos + static_cast<qsizetype>(len) > data.size()) break;
        const QString entry = QString::fromUtf8(data.mid(pos, static_cast<qsizetype>(len)));
        pos += len;
        const int eq = entry.indexOf(QLatin1Char('='));
        if (eq < 0) continue;
        const QString key = entry.left(eq).toUpper();
        const QString value = entry.mid(eq + 1);
        if (key == QStringLiteral("TITLE")) tags.title = value;
        else if (key == QStringLiteral("ARTIST")) tags.artist = value;
        else if (key == QStringLiteral("ALBUM")) tags.album = value;
        else if (key == QStringLiteral("ALBUMARTIST")) tags.albumArtist = value;
        else if (key == QStringLiteral("DATE")) tags.year = value.left(4).toInt();
        else if (key == QStringLiteral("TRACKNUMBER")) tags.trackNo = value.toInt();
        else if (key == QStringLiteral("GENRE")) tags.genre = value;
    }
    return true;
}

// ---------------------------------------------------------------------------
// MP4 / M4A
// ---------------------------------------------------------------------------

namespace {
struct Box {
    QString type;
    qint64 start = 0;
    qint64 size = 0;
};

Box readBox(QFile &f)
{
    Box box;
    const QByteArray header = f.read(8);
    if (header.size() < 8) return box;
    quint32 size = be32(header.constData());
    box.type = QString::fromLatin1(header.mid(4, 4));
    box.start = f.pos();
    if (size == 1) {
        const QByteArray large = f.read(8);
        if (large.size() < 8) return box;
        size = be32(large.constData() + 4);
        box.start = f.pos();
    } else if (size == 0) {
        box.size = 0;
        box.start = f.pos();
        return box;
    }
    box.size = size - (box.start - (f.pos() - (box.start - f.pos())));
    box.size = size;
    return box;
}
} // namespace

bool TagReader::readMp4(QFile &f, LocalTags &tags, AudioInfo &info)
{
    f.seek(0);

    std::function<bool(QFile &, qint64, qint64, int)> walk;
    walk = [&](QFile &file, qint64 end, qint64 base, int depth) -> bool {
        if (depth > 6) return false;
        bool found = false;
        while (file.pos() + 8 <= end) {
            const qint64 boxStart = file.pos();
            const QByteArray header = file.read(8);
            if (header.size() < 8) break;
            quint64 size = be32(header.constData());
            const QString type = QString::fromLatin1(header.mid(4, 4));
            qint64 contentStart = boxStart + 8;
            if (size == 1) {
                const QByteArray large = file.read(8);
                if (large.size() < 8) break;
                quint64 hi = be32(large.constData());
                quint64 lo = be32(large.constData() + 4);
                size = (hi << 32) | lo;
                contentStart = boxStart + 16;
            } else if (size == 0) {
                size = static_cast<quint64>(end - boxStart);
            }
            const qint64 boxEnd = boxStart + static_cast<qint64>(size);
            if (boxEnd > end || size < 8) break;

            if (type == QStringLiteral("moov") ||
                type == QStringLiteral("udta") ||
                type == QStringLiteral("trak") ||
                type == QStringLiteral("mdia")) {
                file.seek(contentStart);
                if (type == QStringLiteral("udta") || type == QStringLiteral("moov"))
                    file.seek(contentStart);
                if (walk(file, boxEnd, contentStart, depth + 1)) found = true;
            } else if (type == QStringLiteral("meta")) {
                // meta 内含 version/flags(4) 后再是子 box
                file.seek(contentStart + 4);
                if (walk(file, boxEnd, contentStart + 4, depth + 1)) found = true;
            } else if (type == QStringLiteral("ilst")) {
                file.seek(contentStart);
                found = true;
                while (file.pos() + 8 <= boxEnd) {
                    const qint64 itemStart = file.pos();
                    const QByteArray ih = file.read(8);
                    if (ih.size() < 8) break;
                    quint64 itemSize = be32(ih.constData());
                    const QString itemType = QString::fromLatin1(ih.mid(4, 4));
                    const qint64 itemEnd = itemStart + static_cast<qint64>(itemSize);
                    if (itemSize < 8 || itemEnd > boxEnd) break;

                    // 读取 data box
                    qint64 p = itemStart + 8;
                    file.seek(p);
                    const QByteArray dh = file.read(8);
                    QString value;
                    QByteArray coverData;
                    bool isImage = false;
                    if (dh.size() >= 8) {
                        quint64 dataSize = be32(dh.constData());
                        const QString dataType = QString::fromLatin1(dh.mid(4, 4));
                        if (dataType == QStringLiteral("data") && dataSize >= 16) {
                            const QByteArray payload = file.read(static_cast<qint64>(dataSize) - 16);
                            const QByteArray head = dh + file.peek(0);
                            Q_UNUSED(head)
                            // type indicator 位于 data box 内容前 4 字节（1=UTF-8, 2=UTF-16, 13=JPEG, 14=PNG）
                            const QByteArray full = file.read(0);
                            Q_UNUSED(full)
                            const QByteArray buf = payload;
                            if (buf.size() >= 4) {
                                const quint32 flag = be32(buf.constData());
                                const QByteArray body = buf.mid(4);
                                if (flag == 13 || flag == 14 || flag == 12 || flag == 27) {
                                    isImage = true;
                                    coverData = body;
                                } else if (flag == 1) {
                                    value = QString::fromUtf8(body);
                                } else if (flag == 2) {
                                    value = QString::fromUtf16(
                                        reinterpret_cast<const char16_t *>(body.constData()),
                                        static_cast<qsizetype>(body.size() / 2));
                                } else {
                                    value = QString::fromUtf8(body);
                                }
                            }
                        }
                    }
                    Q_UNUSED(isImage)

                    if (itemType == QStringLiteral("\xa9nam")) tags.title = value;
                    else if (itemType == QStringLiteral("\xa9ART")) tags.artist = value;
                    else if (itemType == QStringLiteral("\xa9alb")) tags.album = value;
                    else if (itemType == QStringLiteral("aART")) tags.albumArtist = value;
                    else if (itemType == QStringLiteral("\xa9day"))
                        tags.year = value.left(4).toInt();
                    else if (itemType == QStringLiteral("\xa9gen")) tags.genre = value;
                    else if (itemType == QStringLiteral("\xa9cmt")) tags.comment = value;
                    else if (itemType == QStringLiteral("covr") && !coverData.isEmpty()) {
                        const QImage img = QImage::fromData(coverData);
                        if (!img.isNull()) { tags.cover = img; tags.hasCover = true; }
                    }
                    file.seek(itemEnd);
                }
            } else if (type == QStringLiteral("mvhd")) {
                const QByteArray mvhd = file.read(static_cast<qint64>(size) - 8);
                if (mvhd.size() >= 20) {
                    const quint8 version = static_cast<quint8>(mvhd[0]);
                    if (version == 0) {
                        const quint32 timescale = be32(mvhd.constData() + 12);
                        const quint32 dur = be32(mvhd.constData() + 16);
                        if (timescale > 0) {
                            info.durationSec = static_cast<int>(dur / timescale);
                            info.sampleRate = timescale;
                        }
                    } else if (mvhd.size() >= 32) {
                        quint64 timescale = be32(mvhd.constData() + 20);
                        quint64 dur = be32(mvhd.constData() + 24);
                        if (timescale > 0) {
                            info.durationSec = static_cast<int>(dur / timescale);
                            info.sampleRate = static_cast<int>(timescale);
                        }
                    }
                }
            }
            file.seek(boxEnd);
        }
        return found;
    };

    const bool ok = walk(f, f.size(), 0, 0);
    if (ok) {
        info.valid = true;
        info.format = QStringLiteral("M4A");
        info.codec = QStringLiteral("AAC");
        info.lossless = false;
        if (info.durationSec > 0 && f.size() > 0)
            info.bitrate = static_cast<int>(f.size() * 8 / (info.durationSec * 1000));
    }
    return ok;
}

// ---------------------------------------------------------------------------
// WAV
// ---------------------------------------------------------------------------

bool TagReader::readWav(QFile &f, AudioInfo &info)
{
    f.seek(0);
    const QByteArray head = f.read(12);
    if (head.size() < 12 || !head.startsWith("RIFF") || head.mid(8, 4) != "WAVE")
        return false;

    while (f.pos() + 8 <= f.size()) {
        const QByteArray ch = f.read(8);
        if (ch.size() < 8) break;
        const quint32 size = be32(ch.constData() + 4) == 0 ? 0 : 0;
        quint32 chunkSize = 0;
        chunkSize |= static_cast<quint32>(static_cast<quint8>(ch[4]));
        chunkSize |= static_cast<quint32>(static_cast<quint8>(ch[5])) << 8;
        chunkSize |= static_cast<quint32>(static_cast<quint8>(ch[6])) << 16;
        chunkSize |= static_cast<quint32>(static_cast<quint8>(ch[7])) << 24;
        const QString type = QString::fromLatin1(ch.mid(0, 4));
        if (type == QStringLiteral("fmt ")) {
            const QByteArray fmt = f.read(chunkSize);
            if (fmt.size() >= 16) {
                const int sampleRate = static_cast<int>(
                    (static_cast<quint32>(static_cast<quint8>(fmt[7])) << 24) |
                    (static_cast<quint32>(static_cast<quint8>(fmt[6])) << 16) |
                    (static_cast<quint32>(static_cast<quint8>(fmt[5])) << 8) |
                    static_cast<quint32>(static_cast<quint8>(fmt[4])));
                const int bits = static_cast<quint8>(fmt[14]) |
                                 (static_cast<quint8>(fmt[15]) << 8);
                int byteRate = 0;
                if (fmt.size() >= 20) {
                    byteRate = static_cast<int>(
                        (static_cast<quint32>(static_cast<quint8>(fmt[11])) << 24) |
                        (static_cast<quint32>(static_cast<quint8>(fmt[10])) << 16) |
                        (static_cast<quint32>(static_cast<quint8>(fmt[9])) << 8) |
                        static_cast<quint32>(static_cast<quint8>(fmt[8])));
                }
                info.valid = true;
                info.format = QStringLiteral("WAV");
                info.codec = QStringLiteral("PCM");
                info.sampleRate = sampleRate;
                info.bitsPerSample = bits;
                info.lossless = true;
                info.bitrate = byteRate * 8 / 1000;
            }
        } else if (type == QStringLiteral("data")) {
            if (info.sampleRate > 0 && info.bitsPerSample > 0) {
                const double bytesPerSec = info.sampleRate * info.bitsPerSample / 8.0;
                if (bytesPerSec > 0)
                    info.durationSec = static_cast<int>(chunkSize / bytesPerSec);
            }
            f.skip(chunkSize);
        } else {
            f.skip(chunkSize);
        }
        Q_UNUSED(size)
        if ((chunkSize % 2) == 1) f.skip(1); // 对齐到偶数字节
    }
    return info.valid;
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

QStringList TagReader::supportedExtensions()
{
    return {QStringLiteral("*.mp3"), QStringLiteral("*.flac"),
            QStringLiteral("*.m4a"), QStringLiteral("*.ogg"),
            QStringLiteral("*.wav"), QStringLiteral("*.aac"),
            QStringLiteral("*.wma"), QStringLiteral("*.ape")};
}

bool TagReader::isAudioFile(const QString &filePath)
{
    const QString ext = QFileInfo(filePath).suffix().toLower();
    static const QStringList exts = {QStringLiteral("mp3"), QStringLiteral("flac"),
                                     QStringLiteral("m4a"), QStringLiteral("ogg"),
                                     QStringLiteral("wav"), QStringLiteral("aac"),
                                     QStringLiteral("wma"), QStringLiteral("ape")};
    return exts.contains(ext);
}

void TagReader::parseFileName(const QString &fileName, QString &title, QString &artist)
{
    title.clear();
    artist.clear();
    QString base = fileName;
    const int dot = base.lastIndexOf(QLatin1Char('.'));
    if (dot > 0) base = base.left(dot);

    static const QStringList seps = {QStringLiteral(" - "), QStringLiteral("-"),
                                     QStringLiteral("–"), QStringLiteral("—")};
    for (const auto &sep : seps) {
        const int idx = base.indexOf(sep);
        if (idx > 0 && idx < base.length() - 1) {
            artist = base.left(idx).trimmed();
            title = base.mid(idx + sep.length()).trimmed();
            return;
        }
    }
    title = base.trimmed();
}

LocalTags TagReader::readTags(const QString &filePath)
{
    LocalTags tags;
    AudioInfo info;
    read(filePath, tags, info);
    return tags;
}

AudioInfo TagReader::readAudioInfo(const QString &filePath)
{
    LocalTags tags;
    AudioInfo info;
    read(filePath, tags, info);
    return info;
}

void TagReader::read(const QString &filePath, LocalTags &tags, AudioInfo &info)
{
    tags = LocalTags();
    info = AudioInfo();

    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) return;

    const QByteArray magic = f.peek(12);
    if (magic.startsWith("ID3") || magic.startsWith("\xFF\xFB") ||
        magic.startsWith("\xFF\xF3") || magic.startsWith("\xFF\xF2")) {
        readId3v2(f, tags);
        readMp3Info(f, info);
    } else if (magic.startsWith("fLaC")) {
        readFlac(f, tags, info);
    } else if (magic.startsWith("OggS")) {
        if (readOgg(f, tags)) {
            info.valid = true;
            info.format = QStringLiteral("OGG");
            info.codec = QStringLiteral("Vorbis");
            info.lossless = false;
            // four-59：旧实现这里只有一行注释、时长恒 0（列表显示 0:00，播放后才被纠正）。
            // 现在读最后一页 granule position ÷ 首页 ID 头采样率 = 精确时长。
            int oggRate = 0;
            const qint64 oggDur = oggDurationSec(f, &oggRate);
            if (oggRate > 0) info.sampleRate = oggRate;
            if (oggDur > 0) {
                info.durationSec = static_cast<int>(oggDur);
                if (f.size() > 0)
                    info.bitrate = static_cast<int>(f.size() * 8 / (info.durationSec * 1000));
            }
        }
    } else if (magic.mid(4, 4) == "ftyp" || magic.mid(4, 8).contains("M4A")) {
        readMp4(f, tags, info);
    } else if (magic.startsWith("RIFF")) {
        readWav(f, info);
    }

    if (!info.valid) {
        // 未知格式但扩展名受支持时，至少标记为有效以便入库
        if (isAudioFile(filePath)) {
            info.valid = true;
            info.format = QFileInfo(filePath).suffix().toUpper();
        }
    }
    f.close();
}

} // namespace Muyun
