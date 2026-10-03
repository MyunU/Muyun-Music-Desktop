#pragma once

#include <QByteArray>
#include <QString>

namespace Muyun {

/**
 * @brief 音频标签写入器（下载完成后内嵌封面/歌词）
 *
 * - MP3  : ID3v2.3（TIT2/TPE1/TALB + APIC 封面 + USLT 歌词），已有旧 ID3v2 头则替换。
 * - FLAC : 替换/插入 VORBIS_COMMENT（METADATA_BLOCK_PICTURE + LYRICS）与 PICTURE 块。
 * 均为"临时文件 + 原子替换"策略，失败不留坏文件。
 */
class TagWriter
{
public:
    struct Payload {
        QString title;
        QString artist;
        QString album;
        QByteArray coverJpeg;    ///< 已转 JPEG 的封面字节（空=不写封面）
        QString lyrics;          ///< LRC 文本（空=不写歌词）
    };

    /// 按扩展名自动分派；不支持的格式返回 false（不碰文件）
    static bool write(const QString &filePath, const Payload &p);

private:
    static bool writeMp3(const QString &filePath, const Payload &p);
    static bool writeFlac(const QString &filePath, const Payload &p);
};

} // namespace Muyun
