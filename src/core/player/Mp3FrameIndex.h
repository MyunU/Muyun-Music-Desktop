#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

namespace Muyun {

/**
 * @brief MP3 帧索引器（只读帧头、不解码，故整曲扫描只需几毫秒）
 *
 * 用途：音效管线要支持"随时精确 seek + 一开始就知道准确时长"，
 * 而 minimp3 是纯流式解码器（没有 seek 能力），必须先有一张
 * "样本位置 → 文件字节偏移" 的表。
 *
 * 一次前向扫描记录每个音频帧的字节偏移与起始样本号，于是：
 *  - 时长 = 总样本 / 采样率（VBR 文件同样精确，不依赖 Xing/Info 头）
 *  - seek = 二分查表 → 直接从那个字节开始解码（样本级准确，无需估字节比例）
 *  - 顺带识别"这到底是不是 MP3"（M4A 常带 ID3 前缀，只看文件头会误判）
 */
class Mp3FrameIndex
{
public:
    /// 从已读入内存的 mp3 数据建表；失败（非 MP3/无有效帧）返回 false
    bool build(const QByteArray &mp3Data);

    bool valid() const { return !m_entries.isEmpty(); }

    /// 起始样本处即第一帧的采样率/声道（整曲一般恒定）
    int sampleRate() const { return m_sampleRate; }
    int channels() const { return m_channels; }

    /// 总样本数（每声道）
    qint64 totalSamples() const { return m_totalSamples; }
    /// 总时长（毫秒）
    qint64 durationMs() const;

    /// 音频数据在文件中的起始/结束字节（用于计算已缓冲比例等）
    qint64 firstByte() const { return m_firstByte; }
    qint64 lastByte() const { return m_lastByte; }

    /**
     * @brief 找覆盖 targetSample 的那一帧
     * @return 该帧的 [起始样本, 字节偏移]；targetSample 越界时夹到首/末帧
     */
    struct FrameAt { qint64 sample = 0; qint64 byte = 0; };
    FrameAt frameAt(qint64 targetSample) const;

    /// 帧数（诊断/自检用）
    int frameCount() const { return m_entries.size(); }

private:
    struct Entry {
        qint64 sample;     ///< 该帧第一个样本的序号（每声道）
        qint64 byte;       ///< 该帧头在 mp3Data 中的偏移
    };
    QVector<Entry> m_entries;
    int m_sampleRate = 0;
    int m_channels = 0;
    qint64 m_totalSamples = 0;
    qint64 m_firstByte = 0;
    qint64 m_lastByte = 0;
};

} // namespace Muyun
