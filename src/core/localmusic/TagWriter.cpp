#include "TagWriter.h"

#include <QFile>
#include <QFileInfo>
#include <QByteArray>
#include <QThread>
#include <QDebug>

namespace Muyun {

namespace {

// ---------------- 小工具 ----------------

QByteArray be32Bytes(quint32 v)
{
    char b[4] = {
        char((v >> 24) & 0xFF), char((v >> 16) & 0xFF),
        char((v >> 8) & 0xFF),  char(v & 0xFF)
    };
    return QByteArray(b, 4);
}

QByteArray le32Bytes(quint32 v)
{
    char b[4] = {
        char(v & 0xFF), char((v >> 8) & 0xFF),
        char((v >> 16) & 0xFF), char((v >> 24) & 0xFF)
    };
    return QByteArray(b, 4);
}

QByteArray syncsafe(quint32 v)
{
    char b[4] = {
        char((v >> 21) & 0x7F), char((v >> 14) & 0x7F),
        char((v >> 7) & 0x7F),  char(v & 0x7F)
    };
    return QByteArray(b, 4);
}

quint32 syncsafeRead(const QByteArray &a, int off)
{
    return (quint32(quint8(a[off])) << 21) | (quint32(quint8(a[off + 1])) << 14)
         | (quint32(quint8(a[off + 2])) << 7)  |  quint32(quint8(a[off + 3]));
}

/// 覆写目标文件（数据已全部在内存）。Windows 下 temp+rename 常被索引/杀软瞬时锁定，
/// 故直接以 ReadWrite 打开原文件重写；失败重试几次。
bool writeWholeFile(const QString &path, const QByteArray &data)
{
    // 只读文件（如从只读源复制而来）先解除只读，否则打不开写
    if (!QFileInfo(path).isWritable())
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner
                                  | QFile::ReadUser | QFile::WriteUser);
    for (int attempt = 0; attempt < 5; ++attempt) {
        QFile f(path);
        if (f.open(QIODevice::ReadWrite)) {
            if (f.write(data) == data.size()) { f.resize(data.size()); f.flush(); f.close(); return true; }
            f.close();
        }
        QThread::msleep(120);
    }
    qWarning() << "[tagwrite] write fail" << path;
    return false;
}

// ---------------- ID3v2.3 帧 ----------------

QByteArray id3TextFrame(const char *id, const QString &text)
{
    QByteArray data;
    data.append(char(0x03));                       // UTF-8
    data.append(text.toUtf8());
    return QByteArray(id, 4) + syncsafe(quint32(data.size()))   // v2.4: 帧大小也是 syncsafe
         + QByteArray(2, '\0') + data;
}

QByteArray id3ApicFrame(const QByteArray &jpeg)
{
    QByteArray data;
    data.append(char(0x00));                       // 文本编码 ISO-8859-1
    data.append("image/jpeg");
    data.append('\0');
    data.append(char(0x03));                       // 封面类型：Front cover
    data.append('\0');                             // 空描述（终结符）
    data.append(jpeg);
    return QByteArray("APIC") + syncsafe(quint32(data.size()))
         + QByteArray(2, '\0') + data;
}

QByteArray id3UsltFrame(const QString &lyrics)
{
    QByteArray data;
    data.append(char(0x03));                       // UTF-8
    data.append("chi");                            // 语言
    data.append('\0');                             // 空描述
    data.append(lyrics.toUtf8());
    return QByteArray("USLT") + syncsafe(quint32(data.size()))
         + QByteArray(2, '\0') + data;
}

// ---------------- FLAC 块 ----------------

QByteArray flacPictureBlock(const QByteArray &jpeg)
{
    QByteArray d;
    const QByteArray mime = "image/jpeg";
    d += be32Bytes(3);                     // 类型：Front cover（规范首字段是 picture type！）
    d += be32Bytes(quint32(mime.size()));  d += mime;
    d += be32Bytes(0);                     // 描述
    d += be32Bytes(0);                     // 宽
    d += be32Bytes(0);                     // 高
    d += be32Bytes(0);                     // 位深
    d += be32Bytes(0);                     // 颜色数
    d += be32Bytes(quint32(jpeg.size()));  d += jpeg;
    return d;
}

QByteArray flacVorbisEntry(const QByteArray &kv)
{
    return le32Bytes(quint32(kv.size())) + kv;
}

} // namespace

// ===========================================================================
// MP3：ID3v2.3（已有旧 ID3v2 头则整体替换）
// ===========================================================================

bool TagWriter::writeMp3(const QString &filePath, const Payload &p)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = f.read(10);
    qint64 audioStart = 0;
    if (head.size() == 10 && head.startsWith("ID3"))
        audioStart = 10 + qint64(syncsafeRead(head, 6));   // 跳过旧 ID3v2
    f.seek(audioStart);
    const QByteArray audio = f.readAll();
    f.close();

    QByteArray frames;
    if (!p.title.isEmpty())  frames += id3TextFrame("TIT2", p.title);
    if (!p.artist.isEmpty()) frames += id3TextFrame("TPE1", p.artist);
    if (!p.album.isEmpty())  frames += id3TextFrame("TALB", p.album);
    if (!p.coverJpeg.isEmpty()) frames += id3ApicFrame(p.coverJpeg);
    if (!p.lyrics.isEmpty())      frames += id3UsltFrame(p.lyrics);
    if (frames.isEmpty()) return true;   // 没内容可写，原样保留

    QByteArray tag("ID3");
    tag.append(char(0x04)); tag.append(char(0x00)); tag.append(char(0x00));   // v2.4
    tag += syncsafe(quint32(frames.size()));
    tag += frames;

    return writeWholeFile(filePath, tag + audio);
}

// ===========================================================================
// FLAC：替换 VORBIS_COMMENT(4)/PICTURE(6) 块，其余块原样保留
// ===========================================================================

bool TagWriter::writeFlac(const QString &filePath, const Payload &p)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    if (!f.read(4).startsWith("fLaC")) { f.close(); return false; }

    struct Block { int type; QByteArray data; };
    QVector<Block> blocks;
    bool last = false;
    while (!last) {
        const QByteArray hdr = f.read(4);
        if (hdr.size() < 4) { f.close(); return false; }
        last = (quint8(hdr[0]) & 0x80) != 0;
        const int type = quint8(hdr[0]) & 0x7F;
        const quint32 len = (quint32(quint8(hdr[1])) << 16)
                          | (quint32(quint8(hdr[2])) << 8) | quint8(hdr[3]);
        const QByteArray data = f.read(qint64(len));
        if (data.size() != int(len)) { f.close(); return false; }
        if (type != 4 && type != 6)
            blocks.append({type, data});
    }
    const QByteArray audio = f.readAll();
    f.close();

    // 新 VORBIS_COMMENT（含 base64 图片，兼容只读 comment 的播放器）
    QVector<QByteArray> comments;
    if (!p.title.isEmpty())
        comments << flacVorbisEntry("TITLE=" + p.title.toUtf8());
    if (!p.artist.isEmpty())
        comments << flacVorbisEntry("ARTIST=" + p.artist.toUtf8());
    if (!p.album.isEmpty())
        comments << flacVorbisEntry("ALBUM=" + p.album.toUtf8());
    if (!p.lyrics.isEmpty())
        comments << flacVorbisEntry("LYRICS=" + p.lyrics.toUtf8());
    if (!p.coverJpeg.isEmpty())
        comments << flacVorbisEntry("METADATA_BLOCK_PICTURE="
                                    + flacPictureBlock(p.coverJpeg).toBase64());
    if (comments.isEmpty() && p.coverJpeg.isEmpty()) return true;

    QByteArray vc;
    vc += le32Bytes(0);                                   // vendor 空
    vc += le32Bytes(quint32(comments.size()));
    for (const auto &c : comments) vc += c;

    // 组装：STREAMINFO 之后插 VORBIS_COMMENT + PICTURE
    QByteArray out("fLaC");
    QVector<Block> merged;
    bool inserted = false;
    for (const auto &b : blocks) {
        merged.append(b);
        if (!inserted && b.type == 0) {
            merged.append({4, vc});
            if (!p.coverJpeg.isEmpty()) merged.append({6, flacPictureBlock(p.coverJpeg)});
            inserted = true;
        }
    }
    if (!inserted) {   // 无 STREAMINFO（异常文件）：全插最前
        if (!p.coverJpeg.isEmpty()) merged.prepend({6, flacPictureBlock(p.coverJpeg)});
        merged.prepend({4, vc});
    }
    for (int i = 0; i < merged.size(); ++i) {
        char h0 = char(merged[i].type);
        if (i == merged.size() - 1) h0 |= char(0x80);
        out.append(h0);
        const int len = merged[i].data.size();
        out.append(char((len >> 16) & 0xFF));
        out.append(char((len >> 8) & 0xFF));
        out.append(char(len & 0xFF));
        out.append(merged[i].data);
    }
    out += audio;
    return writeWholeFile(filePath, out);
}

namespace {

/// MP3 帧同步：ID3v2 头，或 MPEG 帧 sync（11 位全 1：0xFF 且次字节高 3 位全 1）
bool looksLikeMp3(const QByteArray &m)
{
    if (m.startsWith("ID3")) return true;
    if (m.size() >= 2) {
        const quint8 b0 = static_cast<quint8>(m[0]);
        const quint8 b1 = static_cast<quint8>(m[1]);
        if (b0 == 0xFF && (b1 & 0xE0) == 0xE0) return true;   // MPEG1/2/2.5 Layer I/II/III
    }
    return false;
}

} // namespace

bool TagWriter::write(const QString &filePath, const Payload &p)
{
    // 按文件头 magic 分派（音源可能"flac 后缀装 mp3 内容"，按扩展名写会损坏文件）
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray magic = f.peek(12);
    f.close();

    if (looksLikeMp3(magic))
        return writeMp3(filePath, p);
    if (magic.startsWith("fLaC"))
        return writeFlac(filePath, p);
    return false;   // 其他格式暂不支持（静默跳过，不算错误）
}

} // namespace Muyun
