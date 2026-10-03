#pragma once

#include <QObject>
#include <QVector>
#include <QVariantMap>
#include <QMutex>
#include <cmath>

namespace Muyun {

/**
 * @brief 音频 DSP 引擎
 *
 * 处理立体声 float PCM（-1.0 ~ 1.0）。实现四类效果：
 *  - 10 段图示均衡器：biquad peaking（RBJ Audio EQ Cookbook 公式）
 *  - 环境混响：Freeverb（Jezar 公开算法：8 路梳状滤波 + 4 路全通；
 *    梳状输入按 (1-feedback) 预缩放 + 反馈路阻尼低通，见 four-55）
 *  - 3D 环绕：LFO 驱动的等功率增益声像（v4 起**不用任何延迟**，见 .cpp）
 *  - 响度均衡：RMS 测量 + 自动增益
 *  - 出口统一过软拐限幅（0.9 以下原样通过）
 *
 * 全部为成熟公开算法，无第三方依赖。
 *
 * 线程模型（音效管线用后台线程产出音频，UI 在主线程调参）：
 *  - 会被后台线程调用的只有 process()/reset()/setSampleRate()，
 *    其余 setter/getter 都在主线程；两者共享滤波器系数与延迟线，
 *    因此全部入口用同一把互斥锁保护（锁粒度＝一次 process 调用，约 0.1ms 级）。
 *  - 参数改动**即时生效**，不需要重新解码整曲（旧实现必须重解码，是卡顿根因之一）。
 *  - 约定：public 加锁的方法只调用 private 不加锁的方法，public 之间不互相调用
 *    （loadFromSettings 例外：它调 setReverbPreset，故自身不加锁）。
 */
class AudioEffects : public QObject
{
    Q_OBJECT
public:
    explicit AudioEffects(QObject *parent = nullptr);

    /// 均衡器预设 ID
    enum class EqPreset { Default, Pop, Dance, Rock, Classical, Vocal, Electronic, Custom };
    Q_ENUM(EqPreset)

    /// 混响预设 ID
    enum class ReverbPreset { Room, Hall, Church, Plate };
    Q_ENUM(ReverbPreset)

    // ---- 参数控制 ----
    void setSampleRate(int sampleRate);
    int sampleRate() const { return m_sampleRate; }

    void setEqEnabled(bool enabled);
    bool eqEnabled() const { return m_eqEnabled; }
    void setEqPreset(EqPreset preset);
    EqPreset eqPreset() const { return m_eqPreset; }
    /// 设置某一段增益（dB，-12 ~ +12）；会自动切到 Custom 预设
    void setEqGain(int band, double db);
    double eqGain(int band) const;
    void resetEq();
    static QVector<double> presetGains(EqPreset preset);
    static QStringList bandFrequencies();

    void setReverbEnabled(bool enabled);
    bool reverbEnabled() const { return m_reverbEnabled; }
    void setReverbPreset(ReverbPreset preset);
    ReverbPreset reverbPreset() const { return m_reverbPreset; }
    void setReverbGain(double dry, double wet);   // 0 ~ 1.4

    void setSpatialEnabled(bool enabled);
    bool spatialEnabled() const { return m_spatialEnabled; }
    /**
     * @brief 3D 环绕参数
     * @param radius 深度/宽度 0~100：0=几乎不处理，100=最宽的展宽 + 最大的环绕摆幅
     * @param speed  摆动速度 1~100：映射成 0.03~0.45Hz 的 LFO（默认 50 档约 4 秒一个来回，
     *               即"人声从左耳缓缓移到右耳再回来"）
     */
    void setSpatialParams(double radius, double speed);  // 0~100, 1~100
    /// 环绕参数读回（UI 必须绑这两个，不能写死初值：写死会让滑杆一移动就把存好的值覆盖回默认）
    double spatialRadius() const;
    double spatialSpeed() const;

    void setLoudnessEnabled(bool enabled);
    bool loudnessEnabled() const { return m_loudnessEnabled; }
    void setLoudnessTarget(double targetDb);
    double loudnessTarget() const { return m_loudnessTargetDb; }

    /// 可视化用的当前输出电平（0 ~ 1）—— 由音频线程写入，跨线程读，故加锁（实现在 cpp）
    double currentLevel();
    /// 最近的频谱（用于可视化，长度 64）
    QVector<double> spectrum() const;

    // ---- 处理 ----
    /// 就地处理交错立体声 PCM
    void process(float *data, int frameCount);

    /// 从设置恢复 / 保存
    void loadFromSettings();
    void saveToSettings();

    /// 重置所有内部状态（切歌时调用）
    void reset();

    /**
     * @brief 把所有**参数**恢复默认（EQ 曲线归零、混响回"小房间"、环绕回 50/50、响度目标回 -14）
     *
     * 刻意**不动四个开关**：用户点「恢复默认」是想把拧过的旋钮归零，
     * 不是想被悄悄关掉正在用的音效。落盘 + 发 effectsChanged，UI 跟着回位。
     */
    void resetAllParams();

private:
    struct Biquad {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
        void reset() { x1 = x2 = y1 = y2 = 0.0; }
        float process(float in);
    };

    void updateEqCoefficients();
    void updateReverbParams();

    /// 跨线程保护（后台音频线程 process vs 主线程调参）。可重入：
    /// setter 里会调 saveToSettings/updateReverbParams 等同样加锁的入口。
    mutable QRecursiveMutex m_mtx;

    int m_sampleRate = 44100;

    // EQ
    bool m_eqEnabled = false;
    EqPreset m_eqPreset = EqPreset::Default;
    QVector<double> m_eqGains;
    QVector<Biquad> m_eqBands;    // 左声道
    QVector<Biquad> m_eqBandsR;   // 右声道（独立历史状态）
    static const QVector<double> kEqFrequencies;
    static constexpr double kEqQ = 1.4;

    // 混响（Freeverb）
    bool m_reverbEnabled = false;
    ReverbPreset m_reverbPreset = ReverbPreset::Room;
    double m_dry = 0.82;
    double m_wet = 0.42;
    QVector<QVector<float>> m_combBuffers;
    QVector<int> m_combIndices;
    QVector<float> m_combFeedback;
    QVector<float> m_combDamp1;
    QVector<float> m_combDamp2;
    QVector<float> m_combDampState;   ///< 反馈路上阻尼低通的状态（Freeverb df1；缺了它阻尼等于没接）
    QVector<float> m_combInputScale;  ///< 梳状输入预增益 (1-feedback)：把共振峰增益压回 ≈1×输入，防大响度爆炸
    QVector<QVector<float>> m_allPassBuffers;
    QVector<int> m_allPassIndices;
    float m_reverbDamp = 0.2f;
    float m_reverbRoomSize = 0.75f;

    // 环绕（LFO 驱动的等功率增益声像；**故意不用延迟线**，见 .cpp 里 v1→v4 的教训）
    bool m_spatialEnabled = false;
    double m_spatialRadius = 50.0;
    double m_spatialSpeed = 50.0;
    double m_spatialPhase = 0.0;       ///< LFO 相位（弧度，始终折回 [0,2π)）

    // 响度
    bool m_loudnessEnabled = false;
    double m_loudnessTargetDb = -14.0;
    double m_currentGain = 1.0;
    double m_gainCur = 1.0;          ///< AGC 当前输出增益（块末→下块首线性斜坡用，four-56）
    // AGC 反馈积累：本块"限幅前"输出的平方和/峰值/样本数（four-56——反馈必须取限幅前的真实电平）
    double m_agcSq = 0.0;
    double m_agcPeak = 0.0;
    int m_agcN = 0;
    double m_rmsAccum = 0.0;
    int m_rmsCount = 0;

    // 可视化
    double m_level = 0.0;
    QVector<double> m_spectrum;
};

} // namespace Muyun
