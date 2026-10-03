#pragma once

#include <QString>
#include <QByteArray>
#include <QImage>
#include <QFile>

namespace Muyun {

/// 本地音频文件的内嵌标签
struct LocalTags {
    QString title;
    QString artist;
    QString album;
    QString albumArtist;
    QString composer;
    QString genre;
    QString comment;
    QString lyrics;
    int year = 0;
    int trackNo = 0;
    int trackTotal = 0;
    int discNo = 0;
    int discTotal = 0;
    QImage cover;
    bool hasCover = false;

    bool isEmpty() const {
        return title.isEmpty() && artist.isEmpty() && album.isEmpty();
    }
};

/// 音频文件的技术信息
struct AudioInfo {
    bool valid = false;
    QString format;        // MP3 / FLAC / M4A / OGG / WAV
    QString codec;
    int durationSec = 0;
    int bitrate = 0;       // kbps
    int sampleRate = 0;
    int bitsPerSample = 0;
    bool lossless = false;
};

/**
 * @brief 轻量音频标签读取器
 *
 * 自研实现，覆盖常见格式：MP3(ID3v2)、FLAC/OGG(Vorbis Comment)、
 * MP4/M4A(ilst)、WAV(RIFF INFO)。不依赖第三方库，避免 ABI 与打包问题。
 */
class TagReader
{
public:
    /// 读取标签
    static LocalTags readTags(const QString &filePath);
    /// 读取技术信息（时长/码率/采样率）
    static AudioInfo readAudioInfo(const QString &filePath);
    /// 一次性读取
    static void read(const QString &filePath, LocalTags &tags, AudioInfo &info);

    /// 是否为支持的音频扩展名
    static bool isAudioFile(const QString &filePath);
    /// 支持的扩展名列表
    static QStringList supportedExtensions();

    /// 从文件名推断（"歌手 - 歌名" 形式）
    static void parseFileName(const QString &fileName, QString &title, QString &artist);

private:
    static bool readId3v2(QFile &f, LocalTags &tags);
    static bool readFlac(QFile &f, LocalTags &tags, AudioInfo &info);
    static bool readOgg(QFile &f, LocalTags &tags);
    static bool readMp4(QFile &f, LocalTags &tags, AudioInfo &info);
    static bool readWav(QFile &f, AudioInfo &info);
    static bool readMp3Info(QFile &f, AudioInfo &info);

    static QByteArray findId3Frame(const QByteArray &data, const char *frameId);
    static QString decodeText(const QByteArray &raw);
    static QImage parseId3Picture(const QByteArray &frame);
};

} // namespace Muyun
