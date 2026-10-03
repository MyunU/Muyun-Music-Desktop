#pragma once

#include <QString>
#include <QVector>

namespace Muyun {

/**
 * @brief 音效管线的 PCM 解码后端抽象
 *
 * 统一契约：**交错立体声 float（-1..1）+ 源采样率**。
 * 上层（EffectPlayer）拿到后再做 DSP（按源采样率）→ 重采样到设备率 → 环形缓冲。
 *
 * 之所以抽这一层：音效要"任何格式都能开"，而单一 minimp3 只认 MP3。
 * 现在有两个实现：
 *  - Mp3PcmSource：minimp3 + 帧头索引表（MP3 专用，时长样本级精确、零外部依赖）
 *  - FfmpegPcmSource：运行时调用 Qt 自带的 FFmpeg（覆盖 flac/m4a/ogg/opus/wav/ape…）
 * 还有第三层兜底：两个都打不开 → 上层回退 QMediaPlayer（音效不生效，UI 明说）。
 */
class PcmSource
{
public:
    virtual ~PcmSource() = default;

    /// 打开并解析出音频流；失败返回 false 并填 err
    virtual bool open(const QString &path, QString *err) = 0;

    /// 源采样率（DSP 就按这个率做）
    virtual int sampleRate() const = 0;
    /// 每声道总样本数；<=0 表示未知（UI 时长会退回容器估计）
    virtual qint64 totalSamples() const = 0;
    /// 时长（毫秒），未知返回 0
    qint64 durationMs() const
    {
        const int r = sampleRate();
        return (r > 0 && totalSamples() > 0) ? totalSamples() * 1000 / r : 0;
    }

    /// 精确定位：调用后 read() 从第 sample 个样本开始
    virtual bool seekToSample(qint64 sample) = 0;

    /// 读至多 maxFrames 帧；返回实际帧数，0=结束，-1=出错
    virtual int read(float *out, int maxFrames) = 0;

    /// 后端名（"minimp3" / "ffmpeg"），供自检与 UI 显示"这次是哪条路在解码"
    virtual QString backendName() const = 0;
};

/// 按文件挑最合适的后端（MP3 优先 minimp3，其余交给 FFmpeg）。失败返回 nullptr。
PcmSource *createPcmSource(const QString &path, QString *err);

/// FFmpeg 后端工厂（实现在 FfmpegPcmSource.cpp，类本身不外泄）
PcmSource *createFfmpegPcmSource();

/// 是否存在"万能后端"（FFmpeg 可用且 ABI 校验通过）——上层用它决定要不要试非 MP3
bool pcmHasUniversalBackend();

/// 万能后端的信息串（如 "avcodec 61 / avformat 61 …"），不可用时返回空
QString pcmUniversalBackendInfo();

/// 万能后端不可用的原因（用于日志/UI——静默降级最难查，必须能说清为什么）
QString pcmUniversalBackendError();

/// 只看文件头判断是否 MP3（ID3 或 MPEG 同步头）。注意 M4A 也常带 ID3，故不能当唯一依据
bool pcmLooksLikeMp3(const QString &path);

} // namespace Muyun
