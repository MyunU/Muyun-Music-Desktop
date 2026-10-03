#pragma once

#include <QVector>

namespace Muyun {

/**
 * @brief 窗口化 sinc 重采样器（立体声，任意采样率比）
 *
 * 为什么需要：Qt6 的 QAudioSink 会**硬拒绝**设备不支持的格式，而 Windows 上
 * WASAPI 共享模式通常只认混音格式（实测本机只接受 48000/2ch/Float，
 * 44100 与 Int16 全被拒）。MP3 又几乎全是 44100，所以音效管线必须自带 SRC，
 * 否则"格式协商成功"就等于"必须重采样"。
 *
 * 实现：16 抽头 Blackman 窗低通，相位查表（64 相 + 相内线性插值）。
 * 相比线性插值，避免了上采样时的高频镜像失真；开销约 100 op/帧，可忽略。
 * 输入恒为立体声交错 float（-1..1）；单声道源在解码阶段已展开。
 */
class SincResampler
{
public:
    /// 配置采样率比；in==out 时 identity() 为真，调用方可直接跳过本类
    void configure(int inRate, int outRate);
    void reset();

    bool identity() const { return m_inRate == m_outRate && m_inRate > 0; }
    int inRate() const { return m_inRate; }
    int outRate() const { return m_outRate; }

    /**
     * @brief 喂入若干帧，取出立即可产出的帧
     * @param in       输入交错立体声（长度 = inFrames*2）
     * @param inFrames 输入帧数
     * @param out      输出缓冲（长度 = outCap*2）
     * @param outCap   输出缓冲容量（建议 inFrames*ratio + 32）
     * @return 实际产出帧数
     */
    int feed(const float *in, int inFrames, float *out, int outCap);

    /// outCap 至少要能给 feed 留这么多余量才不会丢帧（按最大产出估算）
    int suggestCap(int inFrames) const;

private:
    void buildTable();
    void trim();

    int m_inRate = 0;
    int m_outRate = 0;
    double m_step = 1.0;        ///< 每个输出帧前进的输入帧数 = inRate/outRate
    QVector<float> m_buf;       ///< 输入历史（交错立体声）
    qint64 m_base = 0;          ///< m_buf 第 0 帧的全局帧号
    qint64 m_totalIn = 0;       ///< 累计喂入的输入帧数
    double m_pos = 0.0;         ///< 下一个待产出帧对应的输入位置（全局帧号，浮点）

    static constexpr int kTaps = 16;
    static constexpr int kPhases = 64;
    QVector<float> m_tab;       ///< (kPhases+1) * kTaps，末相与首相重复便于插值
};

} // namespace Muyun
