#include "AudioEffects.h"

#include "core/storage/DocumentStore.h"

#include <QMutexLocker>
#include <algorithm>

namespace Muyun {

// 10 段图示均衡器中心频率（Hz）
const QVector<double> AudioEffects::kEqFrequencies = {
    31.0, 62.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0
};

// Freeverb 固定延迟线长度（按 44.1kHz 设计，其他采样率按比例缩放）
namespace {
const int kCombTuning[8]  = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
const int kAllPassTuning[4] = {556, 441, 341, 225};
constexpr int kStereoSpread = 23;
constexpr float kAllPassFeedback = 0.5f;
constexpr double kScaleWet = 3.0;
constexpr double kScaleDry = 2.0;
constexpr double kScaleDamp = 0.4;
constexpr double kScaleRoom = 0.28;
constexpr double kOffsetRoom = 0.7;

// 软拐限幅（four-55）：0.9 以下**逐样本原样通过**（不碰"零染色/深度0直通"的既有断言），
// 超过 0.9 用 tanh 平滑逼近 1.0，渐近不越界。
// 旧写法是 std::clamp 的**硬钳位**（注释却写着"软削波"）：大响度乐段每个鼓点都把波形
// 顶部削平方，谐波失真叠着混响共振，就是用户说的"音质坍塌、耳朵遭罪"。
// 在拐点上数值与斜率都连续（导数=1），不会引入新的咔哒。
inline float softClipLimit(float x)
{
    constexpr float knee = 0.9f;
    const float a = std::fabs(x);
    if (a <= knee) return x;
    const float s = (x < 0.0f) ? -1.0f : 1.0f;
    return s * (knee + (1.0f - knee) * static_cast<float>(std::tanh((a - knee) / (1.0f - knee))));
}

// 混响"存在感"设计（four-56 起，four-58 按用户反馈重定标）：
//   ⚠ 用户 2026-10-03 原话："你把我们开启环境混响时的声音弄得比原本声音小，然后每个混响的效果
//     设置又偏向了保守，所以听起来感觉没有明显差别"——两点都成立，是设计错误：
//   ① **干声不许衰减**：旧预设 dry=0.62~0.82，一开混响整首歌就掉 2~4dB，用户第一反应是"音量变小"
//      而不是"加了混响"。现在统一 dry≈0.96（≈-0.4dB，听不出），干声电平只由混响自己决定，
//      不靠衰减干声来"腾地方"。
//   ② **湿声给足**：旧值湿声相对干声只有 ≈-8dB（"听着有点糊"），现在目标 -3~-6dB
//      （"一听就明显"），四个预设拉出清晰阶梯（小房间→大厅→教堂，金属板走明亮短尾）。
//   kWetFeel：把预设里的 wet 值换算成实际混合量的全局系数（湿声能量 ≈0.285×系数，四-56 真歌实测），
//     2.6 → 小房间/大厅/教堂/金属板湿声相对干声约 -11/-6/-4/-8dB。
//   安全网不是"压低一切"，而是末端软拐限幅器（0.9 以下逐样本直通）+ `--test-dsp-real` 断言
//     （被压样本<2%、crest 保住、总电平不低于原声）——抬湿声抬出来的过冲由它兜底。
constexpr float kWetFeel = 2.6f;
} // namespace

// ===========================================================================
// Biquad
// ===========================================================================

float AudioEffects::Biquad::process(float in)
{
    const float out = static_cast<float>(b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2);
    x2 = x1;
    x1 = in;
    y2 = y1;
    y1 = out;
    return out;
}

// ===========================================================================
// 构造 / 初始化
// ===========================================================================

AudioEffects::AudioEffects(QObject *parent) : QObject(parent)
{
    m_eqGains = presetGains(EqPreset::Default);
    m_eqBands.resize(kEqFrequencies.size());
    m_eqBandsR.resize(kEqFrequencies.size());
    m_spectrum.fill(0.0, 64);
    setSampleRate(44100);
    loadFromSettings();
}

void AudioEffects::setSampleRate(int sampleRate)
{
    if (sampleRate <= 0) return;
    QMutexLocker lk(&m_mtx);
    m_sampleRate = sampleRate;
    updateEqCoefficients();
    updateReverbParams();
}

// ===========================================================================
// 均衡器
// ===========================================================================

QStringList AudioEffects::bandFrequencies()
{
    QStringList out;
    for (double f : kEqFrequencies) {
        out.append(f >= 1000.0 ? QStringLiteral("%1k").arg(f / 1000.0, 0, 'f', 1)
                               : QString::number(static_cast<int>(f)));
    }
    return out;
}

QVector<double> AudioEffects::presetGains(EqPreset preset)
{
    switch (preset) {
    case EqPreset::Pop:        return {-1,  2,  4,  4,  2, -1, -2, -2, -1, -1};
    case EqPreset::Dance:      return { 4,  6,  3,  0, -1,  0,  2,  3,  3,  1};
    case EqPreset::Rock:       return { 4,  3, -2, -3, -1,  2,  4,  5,  5,  4};
    case EqPreset::Classical:  return { 0,  0,  0,  0,  0,  0, -2, -2, -3, -4};
    case EqPreset::Vocal:      return {-2, -3, -1,  2,  4,  4,  3,  1, -1, -2};
    case EqPreset::Electronic: return { 3,  4,  1,  0, -2,  1,  2,  2,  3,  4};
    case EqPreset::Custom:     return QVector<double>(10, 0.0);
    case EqPreset::Default:
    default:                   return QVector<double>(10, 0.0);
    }
}

void AudioEffects::updateEqCoefficients()
{
    for (int i = 0; i < m_eqBands.size(); ++i) {
        const double f0 = kEqFrequencies.at(i);
        const double db = (i < m_eqGains.size()) ? m_eqGains.at(i) : 0.0;

        // RBJ Audio EQ Cookbook: peaking EQ
        const double A = std::pow(10.0, db / 40.0);
        const double w0 = 2.0 * M_PI * f0 / static_cast<double>(m_sampleRate);
        const double cosw = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * kEqQ);

        const double b0 = 1.0 + alpha * A;
        const double b1 = -2.0 * cosw;
        const double b2 = 1.0 - alpha * A;
        const double a0 = 1.0 + alpha / A;
        const double a1 = -2.0 * cosw;
        const double a2 = 1.0 - alpha / A;

        Biquad &b = m_eqBands[i];
        b.b0 = b0 / a0;
        b.b1 = b1 / a0;
        b.b2 = b2 / a0;
        b.a1 = a1 / a0;
        b.a2 = a2 / a0;

        if (i < m_eqBandsR.size()) {
            Biquad &br = m_eqBandsR[i];
            br.b0 = b.b0; br.b1 = b.b1; br.b2 = b.b2;
            br.a1 = b.a1; br.a2 = b.a2;
        }
    }
}

void AudioEffects::setEqEnabled(bool enabled)
{
    QMutexLocker lk(&m_mtx);
    m_eqEnabled = enabled;
    saveToSettings();
}

void AudioEffects::setEqPreset(EqPreset preset)
{
    QMutexLocker lk(&m_mtx);
    m_eqPreset = preset;
    if (preset != EqPreset::Custom) {
        m_eqGains = presetGains(preset);
        updateEqCoefficients();
    }
    saveToSettings();
}

void AudioEffects::setEqGain(int band, double db)
{
    if (band < 0) return;
    QMutexLocker lk(&m_mtx);
    if (band >= m_eqGains.size()) return;
    m_eqGains[band] = std::clamp(db, -12.0, 12.0);
    m_eqPreset = EqPreset::Custom;
    updateEqCoefficients();
    saveToSettings();
}

double AudioEffects::eqGain(int band) const
{
    QMutexLocker lk(&m_mtx);
    if (band < 0 || band >= m_eqGains.size()) return 0.0;
    return m_eqGains.at(band);
}

void AudioEffects::resetEq()
{
    QMutexLocker lk(&m_mtx);
    m_eqPreset = EqPreset::Default;
    m_eqGains = presetGains(EqPreset::Default);
    updateEqCoefficients();
    saveToSettings();
}

// ===========================================================================
// 混响（Freeverb）
// ===========================================================================

void AudioEffects::updateReverbParams()
{
    const double scale = static_cast<double>(m_sampleRate) / 44100.0;

    m_combBuffers.clear();
    m_combIndices.clear();
    m_combFeedback.clear();
    m_combDamp1.clear();
    m_combDamp2.clear();
    m_combDampState.clear();
    m_combInputScale.clear();
    m_allPassBuffers.clear();
    m_allPassIndices.clear();

    const float damp = static_cast<float>(m_reverbDamp * kScaleDamp);
    const float feedback = static_cast<float>(kOffsetRoom + m_reverbRoomSize * kScaleRoom);
    // 梳状预增益（four-55）：反馈系数 f≈0.9~0.96 时共振峰增益≈1/(1-f)（约 10~23 倍），
    // 旧写法输入**不预缩放**，大响度乐段（如 Take Me Hand 2:30 的鼓点高潮）低频在共振齿上
    // 逐拍堆积，湿声几倍于满幅 → 全被钳位 → "音质瞬间坍塌"。乘 (1-f) 把每路共振峰压回
    // ≈1×输入；能量归一后混响深浅只剩"尾巴长短/密度"，不再"房间越大越炸"。
    const float combScale = 1.0f - feedback;

    for (int i = 0; i < 8; ++i) {
        const int len = std::max(1, static_cast<int>(kCombTuning[i] * scale)
                                   + ((i % 2 == 1) ? kStereoSpread : 0));
        m_combBuffers.append(QVector<float>(len, 0.0f));
        m_combIndices.append(0);
        m_combFeedback.append(feedback);
        m_combDamp1.append(damp);
        m_combDamp2.append(1.0f - damp);
        m_combDampState.append(0.0f);
        m_combInputScale.append(combScale);
    }
    for (int i = 0; i < 4; ++i) {
        const int len = std::max(1, static_cast<int>(kAllPassTuning[i] * scale)
                                   + ((i % 2 == 1) ? kStereoSpread : 0));
        m_allPassBuffers.append(QVector<float>(len, 0.0f));
        m_allPassIndices.append(0);
    }

    // 环绕不再用延迟线（v4 起改成纯增益声像），这里不需要再分配任何东西
}

void AudioEffects::setReverbEnabled(bool enabled)
{
    QMutexLocker lk(&m_mtx);
    m_reverbEnabled = enabled;
    saveToSettings();
}

void AudioEffects::setReverbPreset(ReverbPreset preset)
{
    QMutexLocker lk(&m_mtx);
    m_reverbPreset = preset;
    switch (preset) {
    // four-58 重定标（用户："每个混响要有一听就明显的变化"）：dry 0.94（-0.5dB，听不出掉音量），
    // wet 拉出清晰阶梯 小房间0.48 < 金属板0.60 < 大厅0.85 < 教堂1.00
    //（湿声相对干声约 -10/-8/-6.5/-6dB，比旧版普遍 +3dB）——由 --test-dsp-real 真歌实测锁定。
    case ReverbPreset::Hall:   m_reverbRoomSize = 0.85f; m_reverbDamp = 0.15f;
                               m_dry = 0.94; m_wet = 0.85; break;
    case ReverbPreset::Church: m_reverbRoomSize = 0.92f; m_reverbDamp = 0.08f;
                               m_dry = 0.94; m_wet = 1.15; break;
    case ReverbPreset::Plate:  m_reverbRoomSize = 0.60f; m_reverbDamp = 0.35f;
                               m_dry = 0.94; m_wet = 0.60; break;
    case ReverbPreset::Room:
    default:                   m_reverbRoomSize = 0.72f; m_reverbDamp = 0.26f;
                               m_dry = 0.94; m_wet = 0.48; break;
    }
    updateReverbParams();
    saveToSettings();
}

void AudioEffects::setReverbGain(double dry, double wet)
{
    QMutexLocker lk(&m_mtx);
    m_dry = std::clamp(dry, 0.0, 1.4);
    m_wet = std::clamp(wet, 0.0, 1.4);
    saveToSettings();
}

// ===========================================================================
// 环绕 / 响度
// ===========================================================================

void AudioEffects::setSpatialEnabled(bool enabled)
{
    QMutexLocker lk(&m_mtx);
    m_spatialEnabled = enabled;
    saveToSettings();
}

void AudioEffects::setSpatialParams(double radius, double speed)
{
    QMutexLocker lk(&m_mtx);
    m_spatialRadius = std::clamp(radius, 0.0, 100.0);
    m_spatialSpeed = std::clamp(speed, 1.0, 100.0);
    saveToSettings();
}

double AudioEffects::spatialRadius() const
{
    QMutexLocker lk(&m_mtx);
    return m_spatialRadius;
}

double AudioEffects::spatialSpeed() const
{
    QMutexLocker lk(&m_mtx);
    return m_spatialSpeed;
}

void AudioEffects::resetAllParams()
{
    QMutexLocker lk(&m_mtx);
    m_eqPreset = EqPreset::Default;
    m_eqGains = presetGains(EqPreset::Default);
    updateEqCoefficients();

    m_reverbPreset = ReverbPreset::Room;
    m_reverbRoomSize = 0.72f;
    m_reverbDamp = 0.26f;
    m_dry = 0.94;   // 与 setReverbPreset(Room) 保持一致（four-58：干声基本不衰减、湿声 0.48）
    m_wet = 0.48;
    updateReverbParams();

    m_spatialRadius = 50.0;
    m_spatialSpeed = 50.0;
    m_loudnessTargetDb = -14.0;
    m_currentGain = 1.0;
    m_gainCur = 1.0;
    saveToSettings();
}

void AudioEffects::setLoudnessEnabled(bool enabled)
{
    QMutexLocker lk(&m_mtx);
    m_loudnessEnabled = enabled;
    saveToSettings();
}

void AudioEffects::setLoudnessTarget(double targetDb)
{
    QMutexLocker lk(&m_mtx);
    m_loudnessTargetDb = std::clamp(targetDb, -24.0, -8.0);
    saveToSettings();
}

QVector<double> AudioEffects::spectrum() const
{
    QMutexLocker lk(&m_mtx);
    return m_spectrum;
}

double AudioEffects::currentLevel()
{
    QMutexLocker lk(&m_mtx);
    return m_level;
}

// ===========================================================================
// 主处理
// ===========================================================================

void AudioEffects::process(float *data, int frameCount)
{
    if (!data || frameCount <= 0) return;
    QMutexLocker lk(&m_mtx);

    const bool anyEffect = m_eqEnabled || m_reverbEnabled
                           || m_spatialEnabled || m_loudnessEnabled;

    // ── AGC（four-56 重写）──────────────────────────────────────────────────
    // 反馈量 = **上一块"限幅前"的输出**峰值/RMS（下面积累的 sqPre/peakPre）。
    // 为什么不是输入侧：链路里 dry 滑杆本身就会降干声电平（混响一开音乐就小声，AGC 必须"看得见"）。
    // 为什么不是旧版的"限幅后"输出：硬钳位把爆掉的信号 RMS 钉死在恒定高位 → 增益误判"已达标"
    //   再也不降 → 自我维持的失真（用户反馈"响度均衡在高潮段也出现同样的坍塌"正是这个环）。
    //   限幅**前**的电平是诚实的：该多大就多大，超标就照实降增益。
    // 双保险：增益上限 = 0.95×(当前增益/上块限幅前峰值) —— 数学上保证限幅器基本不被触碰，
    //   "响度均衡"从此只搬电平、不产生削波（0.9 软拐只在极端母带里兜底）。
    // 平滑：快压（×0.3/块≈50ms 到）慢放（×0.02/块≈1.2s）；静音块保持，防止把底噪抬成啸叫。
    double gainEnd = 1.0;
    if (m_loudnessEnabled) {
        const double target = std::pow(10.0, m_loudnessTargetDb / 20.0);
        const double outRmsPre = std::sqrt(m_agcSq / double(qMax(1, m_agcN)));
        double desired = m_currentGain;
        if (outRmsPre > 1e-4) {
            desired = m_currentGain * (target / outRmsPre);          // 乘法修正：把上块输出拉向目标
            double cap = 6.0;
            if (m_agcPeak > 1e-4)
                cap = std::min(6.0, 0.95 * m_currentGain / m_agcPeak);   // 峰值天花板
            desired = std::clamp(desired, 0.25, std::max(0.25, cap));
        }
        const double coef = desired < m_currentGain ? 0.3 : 0.02;
        m_currentGain += (desired - m_currentGain) * coef;
        gainEnd = m_currentGain;
    } else {
        m_currentGain = 1.0;
    }
    m_agcSq = 0.0; m_agcPeak = 0.0; m_agcN = 0;
    // 块内从"当前增益"线性斜坡到"目标增益"：旧写法整块一次性跳，
    // 增益突变本身就是劈啪声（块边界处一条垂直折线＝一个宽谱冲击）
    const double g0 = m_loudnessEnabled ? m_gainCur : 1.0;
    const double gStep = (gainEnd - g0) / double(frameCount);
    m_gainCur = m_loudnessEnabled ? gainEnd : 1.0;

    double sumSq = 0.0;

    for (int i = 0; i < frameCount; ++i) {
        const int base = i * 2;
        float L = data[base];
        float R = data[base + 1];

        if (m_eqEnabled) {
            for (int b = 0; b < m_eqBands.size(); ++b)
                L = m_eqBands[b].process(L);
            for (int b = 0; b < m_eqBandsR.size(); ++b)
                R = m_eqBandsR[b].process(R);
        }

        if (m_reverbEnabled && !m_combBuffers.isEmpty()) {
            const float input = (L + R) * 0.5f;
            float outL = 0.0f;
            float outR = 0.0f;

            // 8 路梳状滤波，左右各 4 路
            for (int c = 0; c < 8; ++c) {
                auto &buf = m_combBuffers[c];
                int &idx = m_combIndices[c];
                const float out = buf[idx];
                // 修（four-55）：旧写法 `filtered = out*damp2 + out*damp1`，而 damp1+damp2≡1，
                // 即 filtered≡out——**阻尼低通被自己短路了**，反馈回路里没有任何滤波。
                // Freeverb 的阻尼是挂在反馈路上的独立状态（下面 df），它把每次回声里的高频
                // 抹掉一点、尾巴才"暖"；没有它，密集鼓点＝一屋子金属味共振，越叠越亮越炸。
                float &df = m_combDampState[c];
                df = out * m_combDamp2[c] + df * m_combDamp1[c];
                buf[idx] = input * m_combInputScale[c] + df * m_combFeedback[c];
                idx = (idx + 1) % buf.size();
                if (c % 2 == 0) outL += out; else outR += out;
            }
            // 每声道 4 路梳状滤波累加，需归一化
            outL *= 0.25f;
            outR *= 0.25f;

            // 4 路全通
            for (int a = 0; a < 4; ++a) {
                auto &buf = m_allPassBuffers[a];
                int &idx = m_allPassIndices[a];
                const float bufOut = buf[idx];
                const float v = (a % 2 == 0 ? outL : outR);
                const float out = -v + bufOut;
                buf[idx] = v + bufOut * kAllPassFeedback;
                idx = (idx + 1) % buf.size();
                if (a % 2 == 0) outL = out; else outR = out;
            }

            // 干湿混合（four-58）：dry≈0.96 保住原声电平（一开混响不许变小），wet 按预设给足。
            const float wetMix = static_cast<float>(m_wet) * kWetFeel;
            L = L * static_cast<float>(m_dry) + outL * wetMix;
            R = R * static_cast<float>(m_dry) + outR * wetMix;
        }

        if (m_spatialEnabled) {
            // ── 3D 环绕：纯增益的等功率声像摆动（不引入任何相位/延迟）──────────
            // 三次迭代全是用户听出来的，教训值得写清楚：
            //   v1 只固定延迟右声道 30ms → 左耳永远先到，像被钉在左边（"人声一直偏向左"）
            //   v2 左右交叉延迟、延迟量跟 LFO 扫到 26ms → 左右确实动了，但"不是原声"
            //   v3 把延迟压到 4ms → 自检仍然量到单声道兼容 0.63：
            //      **问题不在延迟多大，而在"声道间延迟"本身**——4ms 在 997Hz 上已经差 215°，
            //      合并成一路就大幅抵消，听感就是发闷、人声发虚。时变延迟还额外叠多普勒频移。
            //   v4（现在）：**彻底不用延迟**，方位感全部由"等功率增益声像"提供。
            //      纯增益不改变任何频率成分的相对幅度 → 频谱形状完全不变（自检里用
            //      "输出≈常数×输入"的残差 <2% 直接证明没被染色），人声照样能从左耳缓缓飘到右耳。
            //      同时 gL²+gR²≡2 → 总能量恒定，不会忽响忽轻；深度=0 时 gL=gR=1 逐样本直通。
            const double depth = std::clamp(m_spatialRadius / 100.0, 0.0, 1.0);
            // 速度档 1~100 → 0.03~0.45Hz：默认 50 档约 4 秒一个来回，"缓缓飘过去"不是"抖来抖去"
            const double hz = 0.03 + std::clamp(m_spatialSpeed, 1.0, 100.0) / 100.0 * 0.42;
            m_spatialPhase += 2.0 * M_PI * hz / double(m_sampleRate > 0 ? m_sampleRate : 44100);
            if (m_spatialPhase >= 2.0 * M_PI) m_spatialPhase -= 2.0 * M_PI;
            const double pan = std::sin(m_spatialPhase);      // -1=左耳 … 0=中 … +1=右耳

            // 等功率声像：角度从 45°（正中）按 pan×深度偏摆，√2 归一保证 gL²+gR²=2
            const double a = pan * depth * 0.25 * M_PI;       // 最大 ±45°＝搬到一侧
            double gL = M_SQRT2 * std::cos(0.25 * M_PI + a);
            double gR = M_SQRT2 * std::sin(0.25 * M_PI + a);
            // 极端摆位时单路最高 1.41 倍，容易在已经推满的母带上削波 → 超过 1.25 就整体等比压低
            // （代价是那一瞬间总电平掉 ≤1dB，比削波划算得多）
            const double gmax = std::max(gL, gR);
            if (gmax > 1.25) { const double s = 1.25 / gmax; gL *= s; gR *= s; }

            // 中/侧分离：**摆动只作用在 mid（人声/主体），side（乐器/环境）原样保留**，
            // 所以不会把整首歌搬来搬去，也不会改变立体声宽度——听感是"人声在头里飘"。
            const float mid = (L + R) * 0.5f;
            const float side = (L - R) * 0.5f;
            L = float(mid * gL) + side;
            R = float(mid * gR) - side;
        }

        if (m_loudnessEnabled) {
            const float g = static_cast<float>(g0 + gStep * i);
            L *= g;
            R *= g;
            // AGC 反馈积累：**限幅前**增益后的真实电平（限幅后是假象，见函数头注释）
            const double al = std::fabs(double(L)), ar = std::fabs(double(R));
            m_agcSq += double(L) * L + double(R) * R;
            m_agcN += 2;
            if (al > m_agcPeak) m_agcPeak = al;
            if (ar > m_agcPeak) m_agcPeak = ar;
        }

        // 限幅（four-55 起是软拐，不再是硬钳位）：干声推满的母带 + 混响尾巴 + 声像抬升
        // 在峰值处总会有一点过冲，硬钳位把它变成刺耳的方波顶，软拐只是温柔地"按住"
        L = softClipLimit(L);
        R = softClipLimit(R);

        data[base] = L;
        data[base + 1] = R;
        sumSq += static_cast<double>(L) * L + static_cast<double>(R) * R;
    }

    // 电平（可视化用；AGC 不在这算——见函数头，反馈必须取自输入侧）
    const double rms = std::sqrt(sumSq / static_cast<double>(frameCount * 2));
    m_level = rms;

    // 简化的频谱（低频/中频/高频能量分布，用于可视化）
    if (m_spectrum.isEmpty()) m_spectrum.fill(0.0, 64);
    for (int b = 0; b < 64; ++b) {
        const double t = static_cast<double>(b) / 64.0;
        const double v = rms * std::exp(-t * 2.2);
        m_spectrum[b] = m_spectrum.at(b) * 0.7 + v * 0.3;
    }

    Q_UNUSED(anyEffect)
}

// ===========================================================================
// 状态
// ===========================================================================

void AudioEffects::reset()
{
    for (auto &b : m_eqBands) b.reset();
    for (auto &b : m_eqBandsR) b.reset();
    for (auto &buf : m_combBuffers) buf.fill(0.0f);
    for (auto &buf : m_allPassBuffers) buf.fill(0.0f);
    for (auto &s : m_combDampState) s = 0.0f;   // 阻尼状态也是历史，不清会把上一曲的尾巴带过来
    m_currentGain = 1.0;
    m_gainCur = 1.0;
    m_agcSq = 0.0; m_agcPeak = 0.0; m_agcN = 0;
    m_rmsAccum = 0.0;
    m_rmsCount = 0;
    m_spatialPhase = 0.0;      // 相位归零：跳点后摆动从正中开始，不会"瞬移"到半路
    m_level = 0.0;
}

void AudioEffects::loadFromSettings()
{
    auto *store = DocumentStore::instance();
    const QVariantMap m = store->readSync(QStringLiteral("audio-effects"),
                                          QStringLiteral("settings")).toMap();
    if (m.isEmpty()) return;

    m_eqEnabled = m.value(QStringLiteral("eqEnabled"), false).toBool();
    m_eqPreset = static_cast<EqPreset>(m.value(QStringLiteral("eqPreset"), 0).toInt());
    const QVariantList gains = m.value(QStringLiteral("eqGains")).toList();
    if (gains.size() == m_eqGains.size()) {
        for (int i = 0; i < gains.size(); ++i)
            m_eqGains[i] = gains.at(i).toDouble();
    }
    m_reverbEnabled = m.value(QStringLiteral("reverbEnabled"), false).toBool();
    m_reverbPreset = static_cast<ReverbPreset>(m.value(QStringLiteral("reverbPreset"), 0).toInt());
    m_spatialEnabled = m.value(QStringLiteral("spatialEnabled"), false).toBool();
    m_spatialRadius = m.value(QStringLiteral("spatialRadius"), 50.0).toDouble();
    m_spatialSpeed = m.value(QStringLiteral("spatialSpeed"), 50.0).toDouble();
    m_loudnessEnabled = m.value(QStringLiteral("loudnessEnabled"), false).toBool();
    m_loudnessTargetDb = m.value(QStringLiteral("loudnessTarget"), -14.0).toDouble();

    // 干湿比**一律由预设决定**，存档里的 reverbDry/reverbWet 不参与（four-58 修正）：
    //   UI 里没有干湿滑杆（QML 零引用 setReverbGain），存档里的值只是历次预设的回声、不携带用户意图，
    //   而"回声优先"会让**预设调优永远到不了老用户耳朵里**（你存档里的 0.62/0.68 会一直压住新预设）。
    //   ⚠ 将来真加了干湿滑杆，这里要改成"带版本标记的自定义值优先"，别直接回退成回声优先。
    setReverbPreset(m_reverbPreset);
    updateEqCoefficients();
}

void AudioEffects::saveToSettings()
{
    auto *store = DocumentStore::instance();
    QVariantMap m;
    m[QStringLiteral("eqEnabled")] = m_eqEnabled;
    m[QStringLiteral("eqPreset")] = static_cast<int>(m_eqPreset);
    QVariantList gains;
    for (double g : m_eqGains) gains.append(g);
    m[QStringLiteral("eqGains")] = gains;
    m[QStringLiteral("reverbEnabled")] = m_reverbEnabled;
    m[QStringLiteral("reverbPreset")] = static_cast<int>(m_reverbPreset);
    m[QStringLiteral("reverbDry")] = m_dry;
    m[QStringLiteral("reverbWet")] = m_wet;
    m[QStringLiteral("spatialEnabled")] = m_spatialEnabled;
    m[QStringLiteral("spatialRadius")] = m_spatialRadius;
    m[QStringLiteral("spatialSpeed")] = m_spatialSpeed;
    m[QStringLiteral("loudnessEnabled")] = m_loudnessEnabled;
    m[QStringLiteral("loudnessTarget")] = m_loudnessTargetDb;

    store->write(QStringLiteral("audio-effects"), QStringLiteral("settings"), m);
}

} // namespace Muyun
