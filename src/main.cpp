#include <QApplication>
#include <QCursor>
#include <QScreen>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QQuickItem>
#include <QMouseEvent>

class QQuickMouseEvent;   // 定义在 Qt 私有头里；自检只往 clicked 信号塞空指针，前向声明够用
#include <functional>
#include <QIcon>
#include <QFont>
#include <QLocale>
#include <QDebug>
#include <QThreadPool>
#include <QTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QThread>
#include <QProcess>
#include <QPointer>
#include <memory>

#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#ifdef MUYUN_SELFTES
#include <imm.h>      // --test-ime：读/断言窗口的 IME 上下文与转换状态
#endif
#endif

#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSet>

#include <algorithm>
#include <cstdlib>

#include "core/storage/DocumentStore.h"
#include "core/music/MusicSdk.h"
#include "core/network/HttpClient.h"
#include "ui/PlayerController.h"
#include "ui/SearchController.h"
#include "ui/LibraryController.h"
#include "ui/SyncController.h"
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include "ui/HomeController.h"
#include "ui/Theme.h"
#include "ui/SettingsController.h"
#include "ui/TrayController.h"
#include "ui/DownloadController.h"
#include "ui/DesktopLyricsController.h"
#include "ui/StageFxController.h"
#include "ui/LxSyncServer.h"
#ifdef MUYUN_SELFTES
#include "core/sync/LxMockClient.h"
#include "core/music/lx/LxScriptEngine.h"
#include "core/music/lx/test/LxProtocolCheck.h"
#endif // MUYUN_SELFTES
#include <QElapsedTimer>
#include <functional>
#include "ui/HotkeyManager.h"
#include "ui/ImeGuard.h"
#include "ui/FramelessWindow.h"
#include "ui/SyncController.h"
#include "ui/StageBridge.h"
#include "core/audio/AudioEffects.h"
#include "core/localmusic/TagReader.h"
#include "core/localmusic/TagWriter.h"
#include "core/audio/AudioEffects.h"
#include "core/player/EffectPlayer.h"
#include "core/player/PcmSource.h"
#include "core/player/PlayerEngine.h"
#include "core/lyrics/LyricParser.h"
#include <QThread>
#include <QtMath>
#include <functional>
#include "vendor/minimp3/minimp3.h"
#include <QImage>
#include <QBuffer>
#include <QDirIterator>
#include <QFileInfo>
#include "core/utils/Crypto.h"
#include "core/update/UpdateChecker.h"

using namespace Muyun;

#ifdef MUYUN_SELFTES
/// DSP 自检：验证均衡器与混响确实改变了信号
static const char *u8(const QString &s);   // UTF-8 输出（定义在下方自检工具区）
static int runDspSelfTest()
{
    // 本自检的 setter 会写 audio-effects 设置文档 → 必须隔离（四-56 补防呆，与 --test-effects* 同规格）
    if (qEnvironmentVariableIsEmpty("MUYUN_STORE_ROOT")) {
        printf("[FAIL] 请先设 MUYUN_STORE_ROOT=<临时目录> 再跑（本自检会写音效设置）\n");
        return 1;
    }
    AudioEffects fx;
    fx.setSampleRate(44100);

    const int frames = 4096;
    QVector<float> buf(frames * 2);

    auto fillTone = [&buf, frames](double freq) {
        for (int i = 0; i < frames; ++i) {
            const double v = std::sin(2.0 * M_PI * freq * i / 44100.0) * 0.5;
            buf[i * 2] = static_cast<float>(v);
            buf[i * 2 + 1] = static_cast<float>(v);
        }
    };

    auto rms = [&buf, frames]() {
        double sum = 0.0;
        for (int i = 0; i < frames * 2; ++i) sum += double(buf[i]) * buf[i];
        return std::sqrt(sum / double(frames * 2));
    };

    printf("DSP self-test\n");
    int dspFail = 0;   // 所有断言计入返回码（"打印 PASS 但 return 0"＝假绿，四-53 教训）

    // --- 均衡器 ---
    fillTone(40.0);
    const double before = rms();
    fx.setEqEnabled(true);
    fx.setEqPreset(AudioEffects::EqPreset::Rock);   // 31Hz +4dB / 62Hz +3dB
    fx.process(buf.data(), frames);
    const double afterEq = rms();
    printf("  EQ   40Hz: %.5f -> %.5f  %s\n", before, afterEq,
           afterEq > before * 1.05 ? "[PASS](boosted)" : "[FAIL]");

    // 高频衰减验证（古典预设高频 -4dB）
    fillTone(8000.0);
    const double highBefore = rms();
    fx.setEqPreset(AudioEffects::EqPreset::Classical);
    fx.process(buf.data(), frames);
    const double highAfter = rms();
    printf("  EQ   8kHz: %.5f -> %.5f  %s\n", highBefore, highAfter,
           highAfter < highBefore * 0.95 ? "[PASS](cut)" : "[FAIL]");

    // --- 混响（应该有能量尾巴）---
    fx.setEqEnabled(false);
    fx.setReverbEnabled(true);
    fx.setReverbPreset(AudioEffects::ReverbPreset::Hall);
    fillTone(1000.0);
    // 先处理一段有声信号，再喂静音，检查是否仍有输出（混响尾巴）
    fx.process(buf.data(), frames);
    for (int i = 0; i < frames * 2; ++i) buf[i] = 0.0f;   // 静音输入
    fx.process(buf.data(), frames);
    const double tail = rms();
    printf("  Reverb tail: %.6f  %s\n", tail,
           tail > 1e-5 ? "[PASS](has tail)" : "[FAIL](no tail)");

    // --- 响度均衡 ---
    // 注意：**每块重新灌同一段正弦**（旧写法只灌一次、就地反复处理 → 输出回灌成输入，
    // AGC 收敛点数学上不确定，量出来的"未收敛"是测试假象——同族坑见空间段"复利"教训）。
    fx.setReverbEnabled(false);
    fx.setLoudnessEnabled(true);
    fx.setLoudnessTarget(-14.0);
    fx.reset();
    fillTone(440.0);
    QVector<float> toneCopy = buf;
    for (int i = 0; i < 60; ++i) {                       // 60 块（12kHz 块率下 ≈ 2.5s）足够 attack 收敛
        for (int k = 0; k < frames * 2; ++k) buf[k] = toneCopy[k];
        fx.process(buf.data(), frames);
    }
    const double loudOut = rms();
    const double target = std::pow(10.0, -14.0 / 20.0);
    const bool agcOk = std::fabs(loudOut - target) < target * 0.5;
    printf("  Loudness: %.5f (target %.5f)  %s\n", loudOut, target,
           agcOk ? "[PASS]" : "[FAIL](AGC 未收敛到目标)");
    if (!agcOk) ++dspFail;   // 旧版只打 [WARN] 不计失败——又是一种"假绿"形态

    // --- 3D 环绕：既要真的左右扫，又绝不能毁原声 ---
    // 三次迭代全是用户听出来的：v1 只延迟右声道→恒定偏左；v2 让 26ms 延迟跟着 LFO 扫→
    // 左右确实动了，但**时变大延迟＝持续多普勒频移 + 左右不同步梳状滤波**→"不是原声"；
    // v3 改用纯增益声像（零染色）+ 4ms 以内点缀。所以四组断言缺一不可：
    // 扫幅、左右对称、**不毁原声（响度/单声道兼容/深度0逐样本直通）**。
    {
        // 双音输入（440Hz 在左、997Hz 在右）→ 中置与侧向分量都有，覆盖 mid/side 两条路
        auto fillStereo = [&buf, frames]() {
            for (int i = 0; i < frames; ++i) {
                buf[i * 2] = static_cast<float>(std::sin(2.0 * M_PI * 440.0 * i / 44100.0) * 0.45);
                buf[i * 2 + 1] = static_cast<float>(std::sin(2.0 * M_PI * 997.0 * i / 44100.0) * 0.45);
            }
        };
        fx.setLoudnessEnabled(false);
        fx.setSpatialEnabled(true);
        fx.setSpatialParams(80.0, 100.0);      // 深度大、摆速快，几秒内能跑完几个来回
        fx.reset();
        double dMin = 1e9, dMax = -1e9, dSum = 0.0;
        double lvlMin = 1e9, lvlMax = -1e9;
        int blocks = 0;
        for (int b = 0; b < 60; ++b) {
            fillStereo();              // 每块重新灌同一段信号：否则增益逐块累积，量的是"复利"
            double inSum = 0.0;
            for (int i = 0; i < frames * 2; ++i) inSum += double(buf[i]) * buf[i];
            const double inRms = std::sqrt(inSum / double(frames * 2));
            fx.process(buf.data(), frames);
            double eL = 0.0, eR = 0.0, eOut = 0.0;
            for (int i = 0; i < frames; ++i) {
                const double l = buf[i * 2], r = buf[i * 2 + 1];
                eL += l * l; eR += r * r;
                eOut += l * l + r * r;
            }
            const double d = (eR - eL) / qMax(1e-12, eR + eL);   // -1 全左 … +1 全右
            if (b >= 4) {                       // 头几块是延迟线填充与相位起点，不计极值
                dMin = std::min(dMin, d); dMax = std::max(dMax, d);
                dSum += d; ++blocks;
            }
            const double ratio = std::sqrt(eOut / double(frames * 2)) / inRms;
            lvlMin = std::min(lvlMin, ratio); lvlMax = std::max(lvlMax, ratio);
        }
        const double dMean = blocks ? dSum / blocks : 0.0;
        const bool swings = dMax > 0.10 && dMin < -0.10;
        // 判"有没有长期偏一侧"要看**左右极值是否对称**：均值会受"测试停在周期哪个相位"
        // 影响（截断在半程就会偏一点），极值对称才是"不恒定偏左"的本质证据
        const bool symmetric = std::fabs(dMax + dMin) < 0.15;
        printf("  Spatial swing: min=%+.3f max=%+.3f 极值不对称度=%.3f 均值=%+.3f  %s\n",
               dMin, dMax, std::fabs(dMax + dMin), dMean,
               (swings && symmetric) ? "[PASS](左右都扫到、两端对称不偏左)" : "[FAIL](没有左右环绕)");
        if (!(swings && symmetric)) ++dspFail;

        // 不毁原声①：等功率保证总能量恒定（gL²+gR²≡2），只允许防削波限幅带来的轻微下拉
        const bool levelOk = lvlMax <= 1.02 && lvlMin >= 0.80;
        printf("  Spatial level: 输出/输入响度比 %.3f~%.3f  %s\n", lvlMin, lvlMax,
               levelOk ? "[PASS](总能量恒定，不会忽响忽轻)" : "[FAIL](改变了响度)");
        if (!levelOk) ++dspFail;

        // 不毁原声②：**不染色**——把 LFO 调到最慢（0.03Hz，一块 93ms 内增益几乎不变），
        // 用**中置输入（L==R，side=0）**，此时纯增益声像必须满足"输出 = 常数 × 输入"。
        // 只要引入滤波/延迟，这个关系就破：延迟一个正弦得到的是相移正弦，不是它的标量倍
        // （440Hz 差 4ms 就偏 634°，残差直接爆到 0.9 以上）→ v2/v3 的延迟方案在这里必被抓。
        // 注意：这里不能用"左440/右997"的双音——中/侧分离本身就会跨声道混合，
        // 那是正常的声像行为而不是染色，标量模型不成立（我第一版就误判成 0.138）。
        fx.setSpatialParams(100.0, 1.0);
        fx.reset();
        // 先让相位跑到明显偏一侧的位置，保证测的是"正在摆"而不是"恰好在正中＝直通"
        {
            fillTone(440.0);
            QVector<float> warm = buf;
            for (int b = 0; b < 300; ++b) {          // 300×93ms ≈ 28s → 0.03Hz 约 0.85 个周期
                for (int i = 0; i < frames * 2; ++i) buf[i] = warm[i];
                fx.process(buf.data(), frames);
            }
        }
        fillTone(440.0);
        QVector<float> inCopy = buf;
        fx.process(buf.data(), frames);
        double worstResid = 0.0;
        for (int ch = 0; ch < 2; ++ch) {
            double num = 0.0, den = 0.0;
            for (int i = 0; i < frames; ++i) {
                num += double(inCopy[i * 2 + ch]) * buf[i * 2 + ch];
                den += double(inCopy[i * 2 + ch]) * inCopy[i * 2 + ch];
            }
            const double alpha = den > 1e-12 ? num / den : 0.0;
            double res = 0.0, sig = 0.0;
            for (int i = 0; i < frames; ++i) {
                const double d = double(buf[i * 2 + ch]) - alpha * double(inCopy[i * 2 + ch]);
                res += d * d;
                sig += double(buf[i * 2 + ch]) * buf[i * 2 + ch];
            }
            worstResid = std::max(worstResid, std::sqrt(res / qMax(1e-12, sig)));
        }
        const bool colorOk = worstResid < 0.02;
        printf("  Spatial coloration: 残差=%.4f（输出≈常数×输入）  %s\n", worstResid,
               colorOk ? "[PASS](纯增益、零染色)" : "[FAIL](波形形状被改变＝不是原声)");
        if (!colorOk) ++dspFail;

        // 不毁原声③：深度=0 必须**逐样本**等于直通（不是"近似透明"，是 bit 级不动）
        fx.setSpatialParams(0.0, 50.0);
        fx.reset();
        double maxDiff = 0.0;
        for (int b = 0; b < 20; ++b) {
            fillStereo();
            QVector<float> ref = buf;
            fx.process(buf.data(), frames);
            for (int i = 0; i < frames * 2; ++i)
                maxDiff = std::max(maxDiff, std::fabs(double(buf[i]) - double(ref[i])));
        }
        const bool bypassOk = maxDiff < 1e-6;
        printf("  Spatial depth=0 bypass: maxDiff=%.2e  %s\n", maxDiff,
               bypassOk ? "[PASS](逐样本等于原声)" : "[FAIL](深度0仍在改信号)");
        if (!bypassOk) ++dspFail;
        fx.setSpatialEnabled(false);
    }

    // ── 高燃大鼓段落不毁音质（four-55：用户实测 Take Me Hand 2:30 进鼓点高潮时，
    //    「环境混响 + 3D环绕」一开音质立刻坍塌、刺耳）──
    // 三个真凶：① 梳状反馈路的阻尼被写成 `out*damp2 + out*damp1`（damp1+damp2≡1 ⇒ ≡out，
    //      自己把自己短路）→ 反馈环里没有任何滤波，密集鼓点越叠越亮越炸；
    //   ② 梳状输入不预缩放，共振峰增益≈1/(1-f)（Room≈10×、Church≈23×）→ 大响度必爆；
    //   ③ 末端 std::clamp 是**硬钳位**（注释却写着"软削波"）→ 爆掉的所有波头顶被削成方波。
    // 断言不靠耳朵：越界=0、被软拐"真压过"的样本占比<2%、输出/输入 RMS 比在合理窗口、
    // 且混响尾巴仍在（防止反向过度：把混响修成静音也算事故）。
    {
        auto fillLoud = [&buf, frames]() {
            for (int i = 0; i < frames; ++i) {
                const double t = static_cast<double>(i) / 44100.0;
                const int beat = static_cast<int>(t / 0.25) % 8;          // 0.25s 一击，两小节
                const double env = (beat % 4 == 0) ? 1.0 : 0.55;          // 重拍 kick
                const double kick = std::sin(2.0 * M_PI * 60.0 * t) * 0.50 * env;   // 中置低频：最容易在梳状齿上堆积
                const double body = std::sin(2.0 * M_PI * 180.0 * t) * 0.18
                                  + std::sin(2.0 * M_PI * 430.0 * t) * 0.12;
                const double hats = std::sin(2.0 * M_PI * 7500.0 * t) * 0.12;       // 反相＝宽 side（走声像路）
                buf[i * 2]     = static_cast<float>(kick + body + hats);
                buf[i * 2 + 1] = static_cast<float>(kick + body - hats);
            }
        };
        fx.setReverbEnabled(true);
        fx.setReverbPreset(AudioEffects::ReverbPreset::Church);   // 反馈系数最高＝最容易炸的预设
        fx.setSpatialEnabled(true);
        fx.setSpatialParams(100.0, 50.0);                         // 最深环绕（单路增益顶到限幅帽 1.25）
        fx.reset();
        int over = 0, sat = 0, total = 0;
        double inSum = 0.0, outSum = 0.0, peak = 0.0;
        for (int b = 0; b < 100; ++b) {                           // ≈9.3s：足够让共振堆到稳态
            fillLoud();
            for (int i = 0; i < frames * 2; ++i) inSum += double(buf[i]) * buf[i];
            fx.process(buf.data(), frames);
            for (int i = 0; i < frames * 2; ++i) {
                const double v = std::fabs(double(buf[i]));
                outSum += double(buf[i]) * buf[i];
                peak = std::max(peak, v);
                if (v > 1.0) ++over;
                else if (v > 0.985) ++sat;    // 软拐输出>0.985 ⇔ 限幅前信号≥1.03＝真被压过
                ++total;
            }
        }
        for (int i = 0; i < frames * 2; ++i) buf[i] = 0.0f;       // 收鼓
        fx.process(buf.data(), frames);
        const double tailAfterDrop = rms();
        const double rmsRatio = std::sqrt(outSum / qMax(1e-12, inSum));
        const double satPct = 100.0 * sat / double(total);
        // 被压阈值 8%（four-58 调）：四-58 按用户要求把混响湿声抬到"一听就明显"（教堂 1.15），
        // 本段素材是**持续 60Hz 正弦正撞梳状共振齿**的人造最坏情况，软拐限幅会在鼓点峰上做
        // 1~2dB 压制 → 被压样本 ≈5%（实测）。这是"响混响"的设计代价，不是四-55 那种坍塌
        // （那时是硬钳位 + 30~80% 被压）。**真歌上的护栏在 --test-dsp-real：<2%，实测 ≤0.48%**。
        // 越界=0 与 crest 保住这两条不放宽——那是"音质坍塌"的本体。
        const bool dropOk = over == 0 && peak <= 1.0 && satPct < 8.0
                            && rmsRatio > 0.45 && rmsRatio < 1.25
                            && tailAfterDrop > 1e-5;
        printf("  Loud drop (Church+depth100): peak=%.3f 被压%%=%.2f 越界=%d RMS比=%.3f 尾=%.6f  %s\n",
               peak, satPct, over, rmsRatio, tailAfterDrop,
               dropOk ? "[PASS](高燃鼓点不坍塌)" : "[FAIL](共振爆炸/硬削波/混响被修没)");
        if (!dropOk) ++dspFail;
        fx.setReverbEnabled(false);
        fx.setSpatialEnabled(false);
    }

    printf("DSP self-test done (%d fail)\n", dspFail);
    return dspFail == 0 ? 0 : 1;
}

/// DSP 真歌自检（four-56）：把音效链作用在**真实歌曲的高潮窗口**上，量化两件事——
///   ① 不坍塌：peak≤1、越界=0、被限样本占比<2%、波峰因数(crest=peak/rms) 不低于原声的一半
///      （crest 掉没是"动态被削平＝听感坍塌"的指纹，比总 RMS 诚实）；
///   ② 有存在感：湿声残差（输出对原声做最优标量拟合后的残差能量比）≥ 下限——
///      用户抱怨"环境混响不仔细听听不出分别"，这条把它变成数字。
/// 用法：MuyunMusic.exe --test-dsp-real <音频文件> [startSec=150] [durSec=12]
///   ⚠ 必须 MUYUN_STORE_ROOT 隔离（会写音效设置）
static int runDspRealSelfTest(const QStringList &args)
{
    if (qEnvironmentVariableIsEmpty("MUYUN_STORE_ROOT")) {
        printf("[FAIL] 请先设 MUYUN_STORE_ROOT=<临时目录> 再跑（本自检会写音效设置）\n");
        return 1;
    }
    const int ti = args.indexOf(QStringLiteral("--test-dsp-real"));
    const QString path = (ti >= 0 && ti + 1 < args.size()) ? args.at(ti + 1) : QString();
    const double startSec = (ti + 2 < args.size()) ? args.at(ti + 2).toDouble() : 150.0;
    const double durSec   = (ti + 3 < args.size()) ? args.at(ti + 3).toDouble() : 12.0;
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        // 素材没了（在线播放缓存 3 天自清）→ 记 SKIP 不算失败
        printf("[SKIP] --test-dsp-real 缺素材（用法 --test-dsp-real <音频文件> [startSec] [durSec]）\n");
        return 0;
    }

    QString err;
    PcmSource *src = createPcmSource(path, &err);
    if (!src) {
        printf("[SKIP] --test-dsp-real 解码失败（%s）——素材问题不算管线故障\n", qPrintable(err));
        return 0;
    }
    const int rate = src->sampleRate();
    const qint64 from = qint64(startSec * rate);
    if (!src->seekToSample(from)) { /*  seek 不动也从 0 继续测，只是窗口偏移 */ }
    QVector<float> pcm;
    {
        const int want = int(durSec * rate);
        float tmp[2048 * 2];
        while (pcm.size() < want * 2) {
            const int n = src->read(tmp, qMin(2048, (want * 2 - pcm.size()) / 2));
            if (n <= 0) break;
            for (int i = 0; i < n * 2; ++i) pcm.append(tmp[i]);
        }
    }
    const QString backend = src->backendName();
    delete src;
    if (pcm.size() < rate * 2 * 2) {   // 至少 2 秒
        printf("[FAIL] 素材太短（%d 样本）\n", int(pcm.size() / 2));
        return 1;
    }

    struct Metrics {
        double peak = 0.0, rms = 0.0, crest = 0.0, satPct = 0.0, wetResid = 0.0;
        int over = 0;
    };
    // 原声基准（全旁路）：crest/峰值的参照系
    auto bypassMetrics = [&]() {
        Metrics m;
        double sq = 0.0;
        for (int i = 0; i < pcm.size(); ++i) {
            const double v = std::fabs(double(pcm[i]));
            m.peak = std::max(m.peak, v);
            sq += double(pcm[i]) * pcm[i];
        }
        m.rms = std::sqrt(sq / double(pcm.size()));
        m.crest = m.peak / qMax(1e-9, m.rms);
        return m;
    };
    // 跑一个场景：configure 里开开关/调参；块长与 EffectPlayer 一致（25ms）
    auto runCase = [&](std::function<void(AudioEffects &)> configure) {
        Metrics m;
        AudioEffects fx;
        fx.setSampleRate(rate);
        configure(fx);
        fx.reset();
        QVector<float> buf = pcm;
        const int block = qMax(64, rate / 40);
        double sq = 0.0;
        // 湿声残差用"输出=常数×输入"的最优拟合（和染色检测同源，但这里要的就是"被加了什么"）
        double num = 0.0, den = 0.0;
        for (int off = 0; off + block * 2 <= buf.size(); off += block * 2) {
            fx.process(buf.data() + off, block);
        }
        for (int i = 0; i < buf.size(); ++i) {
            const double v = std::fabs(double(buf[i]));
            m.peak = std::max(m.peak, v);
            if (v > 1.0) ++m.over;
            else if (v > 0.985) m.satPct += 1.0;
            sq += double(buf[i]) * buf[i];
            num += double(buf[i]) * double(pcm[i]);
            den += double(pcm[i]) * double(pcm[i]);
        }
        m.satPct = 100.0 * m.satPct / double(buf.size());
        m.rms = std::sqrt(sq / double(buf.size()));
        m.crest = m.peak / qMax(1e-9, m.rms);
        const double alpha = den > 1e-12 ? num / den : 0.0;
        double res = 0.0;
        for (int i = 0; i < buf.size(); ++i) {
            const double d = double(buf[i]) - alpha * double(pcm[i]);
            res += d * d;
        }
        m.wetResid = std::sqrt(res / qMax(1e-12, sq));
        return m;
    };

    const Metrics ref = bypassMetrics();
    printf("DSP real-material test\n");
    printf("  file=%s backend=%s window=%.0f~%.0fs rate=%d 原声: peak=%.3f rms=%.4f crest=%.2f\n",
           qPrintable(QFileInfo(path).fileName()), qPrintable(backend),
           startSec, startSec + durSec, rate, ref.peak, ref.rms, ref.crest);

    int fail = 0;
    // ⚠ 每个场景先把四个开关**全关**：AudioEffects 构造会读设置文档，上一场景的开关已落盘，
    //   不显式归零就会串扰（实测过：loudness-only 场景沿用了上一场景的混响，数字一模一样）。
    // rmsMin/rmsMax：电平窗口。混响/环绕场景用"相对原声"，**下限 0.90 是四-58 立的硬门**——
    //   用户原话"开启环境混响时的声音比原本声音小"，所以混响开起来绝不许掉电平（dry 已改成 0.96）；
    //   响度场景**不能**这样断言——响度均衡的设计就是压/抬到目标电平（-14dB≈0.2），用绝对窗口。
    // 湿残差（wetMin~wetMax）：用户要"每个混响一听就明显的变化"，所以四个预设各自下限不同，
    //   且阶梯分明（小房间 < 金属板 < 大厅 < 教堂），上限只防"泡糊成一片"。
    struct Case { const char *name; std::function<void(AudioEffects &)> cfg;
                  double wetMin, wetMax; double rmsMin, rmsMax; bool relativeRms; };
    // 湿残差下限 = "明显区"的设计目标（真歌实测口径）：
    //   小房间 0.27≈-10.5dB / 金属板 0.37≈-7.5dB / 大厅 0.40≈-7dB / 教堂 0.44≈-6dB（wet/dry）。
    //   谁把预设改回保守（旧值只有 0.25~0.34），这里立刻 FAIL。
    const QVector<Case> cases = {
        {"混响·小房间", [](AudioEffects &fx) {
            fx.setEqEnabled(false); fx.setReverbEnabled(false);
            fx.setSpatialEnabled(false); fx.setLoudnessEnabled(false);
            fx.setReverbPreset(AudioEffects::ReverbPreset::Room);
            fx.setReverbEnabled(true);
        }, 0.27, 0.55, 0.90, 1.45, true },
        {"混响·金属板", [](AudioEffects &fx) {
            fx.setEqEnabled(false); fx.setReverbEnabled(false);
            fx.setSpatialEnabled(false); fx.setLoudnessEnabled(false);
            fx.setReverbPreset(AudioEffects::ReverbPreset::Plate);
            fx.setReverbEnabled(true);
        }, 0.37, 0.60, 0.90, 1.45, true },
        {"混响·大厅", [](AudioEffects &fx) {
            fx.setEqEnabled(false); fx.setReverbEnabled(false);
            fx.setSpatialEnabled(false); fx.setLoudnessEnabled(false);
            fx.setReverbPreset(AudioEffects::ReverbPreset::Hall);
            fx.setReverbEnabled(true);
        }, 0.40, 0.65, 0.90, 1.50, true },
        {"混响·教堂", [](AudioEffects &fx) {
            fx.setEqEnabled(false); fx.setReverbEnabled(false);
            fx.setSpatialEnabled(false); fx.setLoudnessEnabled(false);
            fx.setReverbPreset(AudioEffects::ReverbPreset::Church);
            fx.setReverbEnabled(true);
        }, 0.44, 0.72, 0.90, 1.55, true },
        {"reverb(church)+spatial100", [](AudioEffects &fx) {
            fx.setEqEnabled(false); fx.setReverbEnabled(false);
            fx.setSpatialEnabled(false); fx.setLoudnessEnabled(false);
            fx.setReverbEnabled(true);
            fx.setReverbPreset(AudioEffects::ReverbPreset::Church);
            fx.setSpatialEnabled(true);
            fx.setSpatialParams(100.0, 50.0);
        }, 0.45, 0.72, 0.90, 1.55, true },
        {"reverb(hall)+spatial50", [](AudioEffects &fx) {
            fx.setEqEnabled(false); fx.setReverbEnabled(false);
            fx.setSpatialEnabled(false); fx.setLoudnessEnabled(false);
            fx.setReverbEnabled(true);
            fx.setReverbPreset(AudioEffects::ReverbPreset::Hall);
            fx.setSpatialEnabled(true);
            fx.setSpatialParams(50.0, 50.0);
        }, 0.38, 0.65, 0.90, 1.50, true },
        {"reverb+spatial+loudness", [](AudioEffects &fx) {
            fx.setEqEnabled(false); fx.setReverbEnabled(false);
            fx.setSpatialEnabled(false); fx.setLoudnessEnabled(false);
            fx.setReverbEnabled(true);
            fx.setReverbPreset(AudioEffects::ReverbPreset::Church);
            fx.setSpatialEnabled(true);
            fx.setSpatialParams(50.0, 50.0);
            fx.setLoudnessEnabled(true);
            fx.setLoudnessTarget(-14.0);
        }, 0.0, 0.0, 0.05, 0.45, false },
        {"loudness-only", [](AudioEffects &fx) {
            fx.setEqEnabled(false); fx.setReverbEnabled(false);
            fx.setSpatialEnabled(false); fx.setLoudnessEnabled(false);
            fx.setLoudnessEnabled(true);
            fx.setLoudnessTarget(-14.0);
        }, 0.0, 0.0, 0.05, 0.45, false },
    };
    for (const Case &c : cases) {
        const Metrics m = runCase(c.cfg);
        // 不坍塌三件套：不越界、被压占比<2%、crest 不低于原声 0.55×（削平＝塌）；
        // 电平窗口按场景类型取（相对原声 or 响度目标的绝对窗口，见 struct 注释）
        const double rmsLo = c.relativeRms ? ref.rms * c.rmsMin : c.rmsMin;
        const double rmsHi = c.relativeRms ? ref.rms * c.rmsMax : c.rmsMax;
        const bool clean = m.over == 0 && m.satPct < 2.0 && m.crest >= ref.crest * 0.55
                           && m.rms >= rmsLo && m.rms <= rmsHi;
        // 存在感窗口：混响场景湿残差必须"一听就明显"又不"泡糊"；纯响度场景不设区间
        const bool wetOk = (c.wetMax <= 0.0) || (m.wetResid >= c.wetMin && m.wetResid <= c.wetMax);
        const bool ok = clean && wetOk;
        printf("  %-26s peak=%.3f 越界=%d 被压%%=%.2f crest=%.2f(原%.2f) rms=%.3f(原%.3f,%.2fdB) 湿残差=%.3f  %s\n",
               c.name, m.peak, m.over, m.satPct, m.crest, ref.crest, m.rms, ref.rms,
               20.0 * std::log10(qMax(1e-9, m.rms / qMax(1e-9, ref.rms))), m.wetResid,
               ok ? "[PASS](明显且不坍塌)" : "[FAIL](坍塌/听不出/电平异常)");
        if (!ok) ++fail;
    }
    printf("DSP real-material test done (%d fail)\n", fail);
    return fail == 0 ? 0 : 1;
}

/// 自检模式统一出口：落盘 + 等线程池 + _exit 跳过静态析构。
/// （QtConcurrent 线程池/平台插件在进程 teardown 时偶发挂起，只影响 --test-* 命令，
///   产品 GUI 走 app.exec()→quit() 正常退出不受影响的；此处一并修掉。）
static int finishSelfTest(int code)
{
    fflush(stdout);
    fflush(stderr);
    QElapsedTimer tAll; tAll.start();
    Muyun::DocumentStore::instance()->flushAll();
    // ⚠ 等线程池之前必须先中止在途网络请求：首页预热/歌词解析/音源解析等任务卡在网络
    // 超时上不会自己回来，waitForDone 每次都白等满 5 秒——这就是 --test-ui"最后一条 PASS
    // 后长时间不退出"的真因（第 1、2 次 grabWindow 实测各 126/118ms，全程 5 秒全花在
    // waitForDone 上）。顺序与产品退出处一致：置"正在退出" → cancelAll → 再等。
    QElapsedTimer tWait; tWait.start();
    Muyun::HttpClient::beginShutdown();
    Muyun::HttpClient::instance()->cancelAll();
    const bool poolDone = QThreadPool::globalInstance()->waitForDone(5000);
    printf("[selftest-exit] code=%d waitForDone=%s(%lldms, 剩活跃线程=%d) 合计=%lldms\n",
           code, poolDone ? "完成" : "超时(有任务卡住)",
           static_cast<long long>(tWait.elapsed()),
           QThreadPool::globalInstance()->activeThreadCount(),
           static_cast<long long>(tAll.elapsed()));
    fflush(stdout);
    std::_Exit(code);   // 跳过静态析构（QtConcurrent/平台插件 teardown 偶发挂起）
    return code;   // 不可达
}

/// AES 自检：使用 FIPS-197 标准向量验证加密实现
static int runCryptoSelfTest()
{
    const QByteArray key = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
    const QByteArray plain = QByteArray::fromHex("00112233445566778899aabbccddeeff");
    const QByteArray cipher = Crypto::aes128EcbEncrypt(plain, key);
    const QByteArray expected = QByteArray::fromHex("69c4e0d86a7b0430d8cdb78070b4c55a");

    printf("AES-128-ECB test\n");
    printf("  plain    : %s\n", plain.toHex().constData());
    printf("  cipher[:16]: %s\n", cipher.left(16).toHex().constData());
    printf("  expected : %s\n", expected.toHex().constData());
    printf("  RESULT   : %s\n",
           cipher.left(16) == expected ? "PASS" : "FAIL");

    // MD5 自检
    const QString md5 = Crypto::md5Hex("hello");
    printf("MD5(\"hello\") = %s (expect 5d41402abc4b2a76b9719d911017c592)\n",
           qPrintable(md5));

    // RSA 自检（网易云公钥，1024 位）
    const QString pub = QStringLiteral(
        "-----BEGIN PUBLIC KEY-----\n"
        "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDgtQn2JZ34ZC28NWYpAUd98iZ37BUrX/aKzmFbt7clFSs6sXqHauqKWqdtLkF2KexO40H1YTX8z2lSgBBOAxLsvaklV8k4cBFK9snQXE9/DDaFt6Rr7iVZMldczhC0JNgTz+SHXT6CBHuX3e9SdB1Ua44oncaTWz7OBGLbCiK45wIDAQAB\n"
        "-----END PUBLIC KEY-----");
    const QString enc = Crypto::rsaNoPaddingEncryptHex("0123456789abcdef", pub);
    printf("RSA(len=%d): %s\n", enc.length(), qPrintable(enc.left(40)));
    printf("  RESULT   : %s\n", enc.length() == 256 ? "PASS(len=256)" : "FAIL");

    // AES-128-ECB 解密回归（FIPS-197 向量反向；invShiftRows 曾写错致 QRC/KRC 歌词解密+洛雪同步全坏）
    const QByteArray dec = Crypto::aes128EcbDecrypt(expected, key);
    printf("AES-ECB decrypt roundtrip : %s\n", dec == plain ? "PASS" : "FAIL");
    // 多块 + PKCS7
    const QByteArray msg = "lx-music auth::\nsample-key-body\nDevice\nlx_music_mobile";
    const QByteArray rt = Crypto::aes128EcbDecrypt(Crypto::aes128EcbEncrypt(msg, key), key);
    printf("AES-ECB pkcs7 multi-block : %s\n", rt == msg ? "PASS" : "FAIL");
    return (dec == plain && rt == msg) ? 0 : 1;
}

/// 音源脚本协议自检：
///  ① 内置协议一致性脚本（全离线）——把官方协议逐条变成断言，缺哪个 API 直接 FAIL；
///  ② 可选在线腿：给了 URL 就走"下载 → 新版特征校验 → QuickJS 真加载"（与导入同一条路）。
/// 用法：MuyunMusic.exe --test-lxsource [url]
static int runLxSourceSelfTest(const QString &url)
{
    printf("=== 音源脚本协议自检 ===\n");
    int fail = 0;

    // ---- ① 内置协议一致性脚本 ----
    {
        const QString path = QDir::tempPath()
            + QStringLiteral("/muyun-lx-proto-%1.js").arg(QDateTime::currentMSecsSinceEpoch());
        bool written = false;
        {
            QFile pf(path);
            if (pf.open(QIODevice::WriteOnly)) {
                pf.write(Muyun::LxTest::kProtocolCheckScript);
                pf.close();
                written = true;
            }
        }
        if (!written) {
            printf("[FAIL] 无法写出协议自检脚本到临时目录\n");
            ++fail;
        } else {
            Muyun::LxScriptEngine engine;   // 独立引擎：不碰用户正在用的那份音源
            QString err;
            const bool loaded = engine.loadScript(path, &err);
            printf("[%s] 协议脚本加载并 send('inited')（%s）\n", loaded ? "PASS" : "FAIL",
                   u8(loaded ? QStringLiteral("ok") : err));
            if (!loaded) {
                ++fail;
            } else {
                const QVariantMap info = engine.scriptInfo();
                const QVariantList problems =
                    info.value(QStringLiteral("__protocolReport")).toList();
                const int checks = info.value(QStringLiteral("__protocolChecks")).toInt();
                printf("     协议断言 %d 项，未通过 %d 项\n", checks, int(problems.size()));
                for (const QVariant &p : problems) {
                    printf("[FAIL] 协议断言：%s\n", u8(p.toString()));
                    ++fail;
                }
                if (problems.isEmpty() && checks > 0)
                    printf("[PASS] lx.utils / currentScriptInfo / 宿主定时器 全部符合协议\n");

                // inited.sources 按协议收敛：非法源、非法音质都必须被剔掉
                const QVariantMap sources = info.value(QStringLiteral("sources")).toMap();
                const bool srcFiltered = !sources.contains(QStringLiteral("xm"))
                                         && sources.contains(QStringLiteral("tx"));
                printf("[%s] sources 过滤非法源（xm 剔除、tx 保留）\n",
                       srcFiltered ? "PASS" : "FAIL");
                if (!srcFiltered) ++fail;

                const QStringList txQualitys = engine.declaredQualitys(QStringLiteral("tx"));
                const bool qFiltered = (txQualitys == QStringList({ QStringLiteral("128k"),
                                                                   QStringLiteral("flac"),
                                                                   QStringLiteral("flac24bit") }));
                printf("[%s] qualitys 过滤非法音质（master 被剔 → %s）\n",
                       qFiltered ? "PASS" : "FAIL", qPrintable(txQualitys.join(QLatin1Char(','))));
                if (!qFiltered) ++fail;

                const QStringList txActions =
                    sources.value(QStringLiteral("tx")).toMap()
                           .value(QStringLiteral("actions")).toStringList();
                const bool aFiltered = (txActions == QStringList({ QStringLiteral("musicUrl") }));
                printf("[%s] actions 过滤（非 local 源只留 musicUrl → %s）\n",
                       aFiltered ? "PASS" : "FAIL", qPrintable(txActions.join(QLatin1Char(','))));
                if (!aFiltered) ++fail;

                // 真实派发：handler 在 setTimeout 里 resolve，证明定时器接进了微任务泵；
                // 返回值里带 musicInfo.duration 的**类型与严格相等结果**——
                // 宿主→JS 的数字一旦退化成字符串，脚本的 ===260 / !==200 就会全错（四-61 真踩过）
                QVariantMap mi;
                mi[QStringLiteral("songmid")] = QStringLiteral("MID123");
                mi[QStringLiteral("duration")] = 260.0;
                QString e2;
                const QString link = engine.musicUrl(QStringLiteral("tx"), mi,
                                                     QStringLiteral("320k"), &e2);
                const QString want = QStringLiteral(
                    "https://proto.example.com/play?mid=MID123&dur=number:true");
                const bool linkOk = link == want;
                printf("[%s] request 事件派发 + 定时器里 resolve 收得住 + 数字不退化成字符串（%s）\n",
                       linkOk ? "PASS" : "FAIL",
                       qPrintable(linkOk ? link : (e2.isEmpty() ? link : e2)));
                if (!linkOk) ++fail;
            }
            QFile::remove(path);
        }
    }

    // ---- ② 可选在线腿 ----
    if (url.isEmpty()) {
        printf("[SKIP] 未给 URL，跳过在线导入腿（--test-lxsource <url> 可验真实脚本）\n");
        return fail == 0 ? 0 : 1;
    }
    const QString savePath = QDir::tempPath()
                             + QStringLiteral("/muyun-lxsource-test-%1.js").arg(QDateTime::currentMSecsSinceEpoch());

    // 1) 走与 importLxSourceUrl 相同的下载通道
    bool dlOk = false;
    QString dlErr;
    {
        QEventLoop loop;
        HttpClient::instance()->downloadFile(url, savePath, HttpOptions(), nullptr,
            [&](bool ok, const QString &err) {
                dlOk = ok; dlErr = err;
                QMetaObject::invokeMethod(&loop, &QEventLoop::quit, Qt::QueuedConnection);
            });
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    printf("[%s] 下载 %s（%s）\n", dlOk ? "PASS" : "FAIL", qPrintable(dlOk ? "ok" : dlErr), qPrintable(url));
    if (!dlOk) { QFile::remove(savePath); return 1; }

    // 2) 校验（与 importLxSourceFile 同规则：只认新版 globalThis.lx 协议）
    QFile f(savePath);
    QString content;
    if (f.open(QIODevice::ReadOnly)) { content = QString::fromUtf8(f.readAll()); f.close(); }
    const bool valid = Muyun::LxScriptEngine::looksLikeLxScript(content);
    printf("[%s] 音源特征校验（新版 globalThis.lx / EVENT_NAMES）\n", valid ? "PASS" : "FAIL");
    if (!valid) { QFile::remove(savePath); return 2; }

    // 3) QuickJS 真实加载（脚本须能 send('inited') 完成初始化）
    QString err;
    const bool loaded = MusicSdk::instance()->loadLxScript(savePath, &err);
    printf("[%s] QuickJS 加载初始化：%s\n", loaded ? "PASS" : "FAIL", qPrintable(loaded ? "ok" : err));

    QFile::remove(savePath);
    return (loaded && fail == 0) ? 0 : 3;
}

/// 下载端到端自检：加载 LX 脚本 → 搜索 → 加入下载队列 → 跑事件循环直到完成/失败
/// 用法：MuyunMusic.exe --test-download [关键词] [音质]
static int runDownloadSelfTest(const QString &keyword, const QString &qualityIdArg)
{
    printf("=== 下载自检：搜索 \"%s\" 音质=%s ===\n",
           qPrintable(keyword), qPrintable(qualityIdArg));

    // 1) 手动加载第一个可用的 LX 脚本（自检模式跳过 SettingsController）
    QString srcDir = DocumentStore::instance()->rootPath() + QStringLiteral("/lx-sources");
    QDir d(srcDir);
    const QStringList js = d.entryList({QStringLiteral("*.js")}, QDir::Files);
    if (js.isEmpty()) {
        printf("[FATAL] no LX scripts in %s\n", qPrintable(srcDir));
        return 1;
    }
    QString scriptPath = d.absoluteFilePath(js.first());
    QString err;
    if (!MusicSdk::instance()->loadLxScript(scriptPath, &err)) {
        printf("[WARN] load script failed: %s\n", qPrintable(err));
    } else {
        printf("[OK] script loaded: %s\n", qPrintable(js.first()));
    }

    // 2) 搜索
    SearchResult r = MusicSdk::instance()->searchAll(keyword, 1, 10);
    printf("[OK] searchAll returned %d songs\n", r.songs.size());
    if (r.songs.isEmpty()) { printf("[FATAL] no results\n"); return 2; }
    Song target = r.songs.first();
    printf("  target: %s - %s [%s]\n", qPrintable(target.artist),
           qPrintable(target.name), qPrintable(target.sourceCode()));

    // 3) 加入下载队列（DownloadController 是 QObject，用栈对象即可）
    DownloadController dl;
    // 覆盖到独立测试目录，避免污染用户默认路径
    const QString testDir = DocumentStore::instance()->rootPath()
                            + QStringLiteral("/_download_test");
    dl.setDownloadPath(testDir);
    dl.setDownloadQuality(qualityIdArg);
    QDir().mkpath(testDir);

    bool added = dl.addDownload(target.toMap(), qualityIdArg);
    if (!added) { printf("[FATAL] addDownload rejected\n"); return 3; }

    const QString expectPath = dl.items().last().toMap()
                                   .value(QStringLiteral("savePath")).toString();
    printf("[OK] added, savePath=%s\n", qPrintable(expectPath));

    // 4) 跑事件循环等待完成或失败（最多 120 秒）
    QElapsedTimer t; t.start();
    int lastStatus = -1;
    qint64 lastReport = 0;
    while (t.elapsed() < 120000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        const QVariantList items = dl.items();
        if (items.isEmpty()) continue;
        QVariantMap m = items.last().toMap();
        const int status = m.value(QStringLiteral("status")).toInt();
        const qint64 recv = m.value(QStringLiteral("received")).toLongLong();
        const qint64 tot  = m.value(QStringLiteral("total")).toLongLong();
        if (status != lastStatus) {
            printf("[T+%llds] status %d -> %d\n",
                   t.elapsed() / 1000, lastStatus, status);
            lastStatus = status;
            lastReport = t.elapsed();
        } else if (status == 2 /*downloading*/ && t.elapsed() - lastReport > 5000) {
            printf("[T+%llds] downloading %lld / %lld bytes\n",
                   t.elapsed() / 1000, recv, tot);
            lastReport = t.elapsed();
        }
        if (status == 3 /*completed*/ || status == 4 /*failed*/ || status == 5) {
            QString e = m.value(QStringLiteral("error")).toString();
            if (!e.isEmpty()) printf("  error: %s\n", qPrintable(e));
            QFile f(expectPath);
            if (f.exists()) {
                printf("[FILE] %s size=%lld\n", qPrintable(expectPath),
                       (qint64)f.size());
            }
            // 测试完清理（临时：MUYUN_KEEP_DL=1 时保留文件供 --test-tag 复验真实 flac）
            if (!qEnvironmentVariableIsSet("MUYUN_KEEP_DL")) {
                f.remove();
                QDir(testDir).rmdir(QStringLiteral("."));
            } else {
                printf("[KEEP] %s\n", qPrintable(expectPath));
            }
            return status == 3 ? 0 : 4;
        }
    }
    printf("[TIMEOUT] 120s\n");
    return 5;
}

/// 下载并发/取消自检：一次加入 4 首歌（并发上限 2），中途取消第 1 项，验证其它继续
static int runDownloadConcurrencyTest(const QString &keyword, const QString &qualityIdArg)
{
    printf("=== 下载并发/取消自检：\"%s\" 音质=%s ===\n",
           qPrintable(keyword), qPrintable(qualityIdArg));

    QString srcDir = DocumentStore::instance()->rootPath() + QStringLiteral("/lx-sources");
    QDir d(srcDir);
    const QStringList js = d.entryList({QStringLiteral("*.js")}, QDir::Files);
    if (js.isEmpty()) { printf("[FATAL] no LX scripts\n"); return 1; }
    QString err;
    MusicSdk::instance()->loadLxScript(d.absoluteFilePath(js.first()), &err);

    SearchResult r = MusicSdk::instance()->searchAll(keyword, 1, 10);
    if (r.songs.size() < 3) { printf("[FATAL] only %d results\n", r.songs.size()); return 2; }

    DownloadController dl;
    const QString testDir = DocumentStore::instance()->rootPath() + QStringLiteral("/_download_test2");
    dl.setDownloadPath(testDir);
    dl.setDownloadQuality(qualityIdArg);
    QDir().mkpath(testDir);

    QString firstId;
    for (int i = 0; i < 3 && i < r.songs.size(); ++i) {
        dl.addDownload(r.songs.at(i).toMap(), qualityIdArg);
        if (i == 0) {
            QVariantMap m = dl.items().last().toMap();
            firstId = m.value(QStringLiteral("id")).toString();
        }
    }
    const int total = dl.items().size();
    printf("[OK] queued %d items (maxConcurrent=2), firstId=%s\n", total, qPrintable(firstId));
    (void)total;

    // 1s 后取消第一项
    QElapsedTimer t; t.start();
    bool cancelledAt = false;
    while (t.elapsed() < 60000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (!cancelledAt && t.elapsed() > 1000) {
            cancelledAt = true;
            QVariantMap m0 = dl.items().first().toMap();
            printf("[T+%llds] cancel first item (was status=%d progress=%lld/%lld)\n",
                   t.elapsed() / 1000, m0.value(QStringLiteral("status")).toInt(),
                   m0.value(QStringLiteral("received")).toLongLong(),
                   m0.value(QStringLiteral("total")).toLongLong());
            dl.cancelDownload(firstId);
        }
        // 状态快照
        QVariantList items = dl.items();
        int done = 0, cancel = 0, active = 0, failed = 0;
        QStringList failInfo;
        for (const auto &v : items) {
            QVariantMap m = v.toMap();
            int s = m.value(QStringLiteral("status")).toInt();
            if (s == 3) ++done;
            else if (s == 4) { ++failed; failInfo << QString("[%1] %2")
                    .arg(qPrintable(m.value("song").toMap().value("name").toString()),
                         qPrintable(m.value("error").toString())); }
            else if (s == 5) ++cancel;
            else active++;
        }
        static int lastActive = -1;
        if (lastActive != active || done + cancel + failed >= (int)items.size()) {
            printf("[T+%llds] active=%d done=%d cancel=%d fail=%d\n",
                   t.elapsed() / 1000, active, done, cancel, failed);
            for (const auto &fi : failInfo) printf("    %s\n", qPrintable(fi));
            lastActive = active;
        }
        if (active == 0) break;
    }

    // 校验：被取消的那项文件应被清理
    QDir(testDir).removeRecursively();
    printf("[DONE]\n");
    return 0;
}

/// 音源自检：验证加密实现与各平台接口连通性
static int runSourceSelfTest(const QString &keyword)
{
    auto *sdk = MusicSdk::instance();
    printf("=== 音源自检：搜索 \"%s\" ===\n", qPrintable(keyword));


    int totalOk = 0;
    for (const auto &code : sdk->sourceCodes()) {
        auto *src = sdk->source(code);
        if (!src) {
            printf("[%s] 未注册\n", qPrintable(code));
            continue;
        }
        const SearchResult r = src->searchSongs(keyword, 1, 5);
        printf("[%s] %s -> %d 首\n", qPrintable(code),
               qPrintable(src->name()), r.songs.size());
        for (int i = 0; i < qMin(3, r.songs.size()); ++i) {
            const Song &s = r.songs.at(i);
            printf("    %d. %s - %s (%s) %.0fs cover=%.60s\n", i + 1,
                   qPrintable(s.name), qPrintable(s.artist),
                   qPrintable(s.album), s.duration, qPrintable(s.cover));
        }
        // 播放链接与歌词解析验证（取首条）
        if (!r.songs.isEmpty()) {
            const Song &first = r.songs.first();
            AudioQuality actual = AudioQuality::K320;
            const QString url = sdk->resolveUrl(first, AudioQuality::K320, &actual);
            printf("    [URL] %s\n", url.isEmpty()
                       ? "(EMPTY) resolve failed"
                       : qPrintable(url.left(110)));
            const SongLyric ly = sdk->resolveLyric(first);
            printf("    [LYRIC] rawLen=%d hasTrans=%d\n",
                   ly.rawLrc.length(), ly.hasTranslation ? 1 : 0);
        }
        totalOk += r.songs.size();
    }
    printf("合计 %d 首\n", totalOk);
    return 0;
}

static int runHotSelfTest()
{
    auto *sdk = MusicSdk::instance();
    auto *wy = sdk->source(QStringLiteral("wy"));
    if (!wy) { printf("[wy] 未注册\n"); return 1; }

    printf("=== 搜索联想数据源（searchAll \"枫\" 首位应是周杰伦） ===\n");
    const SearchResult sugR = sdk->searchAll(QStringLiteral("枫"), 1, 10);
    QString firstTerm;
    if (!sugR.songs.isEmpty())
        firstTerm = sugR.songs.first().name + QLatin1Char(' ') + sugR.songs.first().artist;
    printf("  首位: %s\n", qPrintable(firstTerm));
    const bool sugOk = firstTerm.contains(QStringLiteral("周杰伦"));
    printf("[%s] 联想首位为热门原唱\n", sugOk ? "PASS" : "FAIL");
    int hotFail = sugOk ? 0 : 1;

    printf("=== 热搜歌曲 ===\n");
    const QVariantList songs = wy->hotSearchSongs();
    printf("共 %d 条\n", songs.size());
    for (int i = 0; i < qMin(5, songs.size()); ++i) {
        const QVariantMap m = songs.at(i).toMap();
        printf("  %d. word=%s | song=%s - %s\n", i + 1,
               qPrintable(m.value("word").toString()),
               qPrintable(m.value("name").toString()),
               qPrintable(m.value("artist").toString()));
    }

    printf("=== 热搜歌手 ===\n");
    const QVariantList artists = wy->topArtists(10);
    printf("共 %d 条\n", artists.size());
    for (int i = 0; i < qMin(5, artists.size()); ++i) {
        const QVariantMap m = artists.at(i).toMap();
        printf("  %d. id=%s name=%s avatar=%s\n", i + 1,
               qPrintable(m.value("id").toString()),
               qPrintable(m.value("name").toString()),
               qPrintable(m.value("avatar").toString().left(60)));
    }

    printf("=== 歌单广场分类 ===\n");
    const QVariantList cats = wy->playlistCategories();
    printf("groups=%d\n", cats.size());
    for (int i = 0; i < qMin(3, cats.size()); ++i) {
        const QVariantMap m = cats.at(i).toMap();
        printf("  [%s] %s\n", qPrintable(m.value("group").toString()),
               qPrintable(m.value("tags").toStringList().join(",")));
    }

    printf("=== 分类下歌单(华语/hot) ===\n");
    bool hasMore = false;
    const QVector<PlaylistSummary> pls = wy->explorePlaylists(
        QStringLiteral("华语"), QStringLiteral("hot"), 1, 10, &hasMore);
    printf("count=%d hasMore=%d\n", pls.size(), hasMore);
    for (int i = 0; i < qMin(5, pls.size()); ++i) {
        const PlaylistSummary &p = pls.at(i);
        printf("  %d. id=%s name=%s play=%lld\n", i + 1,
               qPrintable(p.id), qPrintable(p.name), p.playCount);
    }
    return hotFail;
}

/// 歌单广场自检：五家平台 分类 + 分类下歌单
/// 用法：MuyunMusic.exe --test-explore [指定标签名]
static int runExploreSelfTest(const QString &tagName)
{
    auto *sdk = MusicSdk::instance();
    int fail = 0;
    const QStringList codes{QStringLiteral("wy"), QStringLiteral("tx"), QStringLiteral("kg"),
                            QStringLiteral("kw"), QStringLiteral("mg")};
    for (const QString &code : codes) {
        MusicSource *src = sdk->source(code);
        if (!src) { printf("[%s] 未注册\n", qPrintable(code)); ++fail; continue; }
        const QVariantList cats = src->playlistCategories();
        QStringList tags;
        for (const auto &c : cats)
            tags << c.toMap().value(QStringLiteral("tags")).toStringList();
        printf("=== [%s] 分类=%d 组 / 标签=%d 个 ===\n", qPrintable(code),
               cats.size(), tags.size());
        bool hasMore = false;
        const QVector<PlaylistSummary> all = src->explorePlaylists(
            QStringLiteral("全部"), QStringLiteral("hot"), 1, 12, &hasMore);
        printf("  全部/hot=%d hasMore=%d", all.size(), hasMore);
        if (!all.isEmpty()) printf("  首条=%s", qPrintable(all.first().name));
        printf("\n");
        if (all.isEmpty()) ++fail;
        const QString tag = (tagName.isEmpty() || !tags.contains(tagName))
                                ? (tags.isEmpty() ? QString() : tags.first()) : tagName;
        if (!tag.isEmpty()) {
            hasMore = false;
            const QVector<PlaylistSummary> one = src->explorePlaylists(
                tag, QStringLiteral("hot"), 1, 12, &hasMore);
            printf("  [%s]=%d", qPrintable(tag), one.size());
            if (!one.isEmpty()) printf("  首条=%s", qPrintable(one.first().name));
            printf("\n");
            if (one.isEmpty()) ++fail;
        }
    }
    return fail == 0 ? 0 : 1;
}

#ifdef Q_OS_WIN
static void procMemSample(quint64 &working, quint64 &priv) {
    PROCESS_MEMORY_COUNTERS_EX pmc{}; pmc.cb = sizeof(pmc);
    working = priv = 0;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        working = pmc.WorkingSetSize; priv = pmc.PrivateUsage;
    }
}
#endif

static void collectCoverUrls(const QJsonValue &v, QSet<QString> &out)
{
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (it.key() == QStringLiteral("cover") && it.value().isString()) {
                const QString s = it.value().toString();
                if (s.startsWith(QStringLiteral("http"))) out.insert(s);
            } else {
                collectCoverUrls(it.value(), out);
            }
        }
    } else if (v.isArray()) {
        const QJsonArray a = v.toArray();
        for (const auto &x : a) collectCoverUrls(x, out);
    }
}

/// 内存归因自检：证明封面"全分辨率解码 vs 限尺寸解码"的位图内存差
/// 用法：MuyunMusic.exe --test-mem [张数=24]
static int runMemSelfTest(int limit)
{
    printf("=== 内存归因自检 ===\n");
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        printf("进程 工作集=%.1fMB 私有集=%.1fMB 峰值工作集=%.1fMB\n",
               pmc.WorkingSetSize / 1048576.0, pmc.PrivateUsage / 1048576.0,
               pmc.PeakWorkingSetSize / 1048576.0);
    }
#endif
    const QString cachePath = DocumentStore::instance()->rootPath()
        + QStringLiteral("/store/home-cache.json");
    QFile f(cachePath);
    if (!f.open(QIODevice::ReadOnly)) { printf("[FATAL] 无法读 %s\n", qPrintable(cachePath)); return 1; }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    QSet<QString> urls;
    collectCoverUrls(doc.object(), urls);
    printf("首页缓存封面 URL 去重后: %d 张\n", (int)urls.size());
    if (urls.isEmpty()) { printf("[WARN] 无封面 URL，跳过下载对比\n"); return 0; }

    const QStringList list = QStringList{urls.constBegin(), urls.constEnd()};
    const int n = qMin(limit, (int)list.size());
    qint64 fullBytes = 0, smallBytes = 0, fullW = 0, fullH = 0;
    int okCount = 0;
    HttpOptions opt; opt.timeoutMs = 15000;
    for (int i = 0; i < n; ++i) {
        const HttpResponse r = HttpClient::instance()->get(list[i], opt);
        if (!r.ok || r.body.isEmpty()) continue;
        QImage full = QImage::fromData(r.body);
        if (full.isNull()) continue;
        if (i == 0) { fullW = full.width(); fullH = full.height(); }
        QImage small = full.scaled(160, 160, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        fullBytes += full.sizeInBytes();
        smallBytes += small.sizeInBytes();
        ++okCount;
    }
    if (okCount == 0) { printf("[FATAL] 封面下载全部失败（网络？）\n"); return 2; }
    printf("抽样 %d 张（首张原图约 %lldx%lld）：\n", okCount, fullW, fullH);
    printf("  全分辨率解码  合计 %.1fMB  平均 %.0fKB/张\n",
           fullBytes / 1048576.0, fullBytes / 1024.0 / okCount);
    printf("  限160px解码   合计 %.1fMB  平均 %.0fKB/张\n",
           smallBytes / 1048576.0, smallBytes / 1024.0 / okCount);
    printf("  → 缩略图限尺寸可省 %.1f%%（%d 张即 %.1fMB → %.1fMB）\n",
           100.0 * (1.0 - double(smallBytes) / double(fullBytes)), okCount,
           fullBytes / 1048576.0, smallBytes / 1048576.0);
    return 0;
}

static int runHomeSelfTest()
{
    auto *sdk = MusicSdk::instance();
    for (const auto &code : QStringList{QStringLiteral("wy"), QStringLiteral("tx"),
                                         QStringLiteral("kg"), QStringLiteral("kw"),
                                         QStringLiteral("mg")}) {
        auto *src = sdk->source(code);
        if (!src) { printf("[%s] not registered\n", qPrintable(code)); continue; }
        const QVector<ToplistInfo> tls = src->getToplists();
        const QVector<PlaylistSummary> recs = src->getRecommendPlaylists(6);
        printf("[%s] toplists=%d recPlaylists=%d\n", qPrintable(code),
               tls.size(), recs.size());
        for (int i = 0; i < qMin(3, tls.size()); ++i)
            printf("    top: id=%s name=%s cover=%.70s\n", qPrintable(tls.at(i).id),
                   qPrintable(tls.at(i).name), qPrintable(tls.at(i).cover));
        // 全量实测：逐榜单拉 3 首，输出每个榜单的返回数量，便于发现死 id
        for (const auto &t : tls) {
            const SearchResult r = src->getToplist(t.id, 1, 3);
            printf("    [detail] %s id=%s songs=%d\n", qPrintable(t.name),
                   qPrintable(t.id), r.songs.size());
        }
        if (!tls.isEmpty()) {
            const SearchResult r = src->getToplist(tls.first().id, 1, 50);
            printf("    [%s] detail songs=%d\n", qPrintable(tls.first().name), r.songs.size());
            for (int i = 0; i < qMin(3, r.songs.size()); ++i)
                printf("      song cover=%.70s\n", qPrintable(r.songs.at(i).cover));
        }
        if (!recs.isEmpty()) {
            printf("    rec[0] id=%s cover=%.48s playCount=%lld\n",
                   qPrintable(recs.first().id), qPrintable(recs.first().cover),
                   static_cast<long long>(recs.first().playCount));
            const Playlist pl = src->getPlaylistDetail(recs.first().id);
            printf("    [%s] playlistDetail songs=%d\n", qPrintable(recs.first().name),
                   pl.songs.size());
            for (int i = 0; i < qMin(2, pl.songs.size()); ++i) {
                const Song &s = pl.songs.at(i);
                printf("      %s | %s | %.48s | hash=%s types=%d\n",
                       qPrintable(s.name.left(24)), qPrintable(s.artist.left(18)),
                       qPrintable(s.cover), qPrintable(s.lx.hash),
                       static_cast<int>(s.lx.types.size()));
            }
            if (!pl.songs.isEmpty()) {
                AudioQuality actual = AudioQuality::K128;
                const QString url = sdk->resolveUrl(pl.songs.first(), AudioQuality::K320, &actual);
                printf("      playUrl=%s (actual=%d)\n", qPrintable(url.left(72)),
                       static_cast<int>(actual));
            }
        }
    }
    return 0;
}

static const char *u8(const QString &s);   // 定义在下方自检工具区（UTF-8 输出）

/// 把版本号的最后一段 +1（1.1.0 → 1.1.1，1.1.99 → 1.2.0）。
/// 更新自检的夹具版本必须用它在"本程序当前版本"上推导：写死具体版本号在升版后会连锁变红。
static QString bumpLast(const QString &v)
{
    QStringList parts = v.split(QLatin1Char('.'));
    for (int i = parts.size() - 1; i >= 0; --i) {
        if (parts.at(i).toInt() < 99) {
            parts[i] = QString::number(parts.at(i).toInt() + 1);
            break;
        }
        parts[i] = QStringLiteral("0");
    }
    return parts.join(QLatin1Char('.'));
}

/// 更新提示自检（全离线，不碰网络）：
///   ① 纯逻辑：版本规范化 / 数值段比较 / 忽略规则 / 两种清单格式 / 垃圾输入；
///   ② 真链路：本地清单文件 → 工作线程读取 → 回主线程落地 → 发 updateFound；
///      再验"每次启动查一次"（会话级不重查）、开关关掉不查、"不再提醒"压住自动弹窗但不挡手动检查、
///      远端出现比忽略版本更新的会重新提醒、「稍后」压住本会话自动弹窗、同版本判 uptodate、
///      坏清单判 failed 且不弹窗。每个"自动检查"场景用新实例模拟"下次启动"（会话标记归零）。
/// 用法：MuyunMusic.exe --test-update   （必须隔离 MUYUN_STORE_ROOT，本自检会写设置文档）
static int runUpdateSelfTest()
{
    if (qEnvironmentVariableIsEmpty("MUYUN_STORE_ROOT")) {
        printf("[FAIL] 请先设 MUYUN_STORE_ROOT=<临时目录> 再跑（本自检会写更新设置）\n");
        return 1;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("=== 更新提示自检 ===\n");
    int fail = 0;
    auto check = [&fail](const char *name, bool ok, const QString &detail = QString()) {
        printf("[%s] %s", ok ? "PASS" : "FAIL", name);
        if (!detail.isEmpty()) printf("  （%s）", u8(detail));
        printf("\n");
        if (!ok) ++fail;
    };

    // ---------- ① 纯逻辑 ----------
    check("版本规范化 v1.2.3-beta.1 → 1.2.3",
          UpdateChecker::normalizeVersion(QStringLiteral("v1.2.3-beta.1"))
              == QStringLiteral("1.2.3"),
          UpdateChecker::normalizeVersion(QStringLiteral("v1.2.3-beta.1")));
    check("段数不等按补 0 比较：1.0 == 1.0.0",
          UpdateChecker::compareVersion(QStringLiteral("1.0"), QStringLiteral("1.0.0")) == 0);
    check("数值段比较：1.10.0 > 1.9.9（不是字符串比较）",
          UpdateChecker::compareVersion(QStringLiteral("1.10.0"), QStringLiteral("1.9.9")) > 0);
    check("1.0.0 < 1.0.1",
          UpdateChecker::compareVersion(QStringLiteral("1.0.0"), QStringLiteral("1.0.1")) < 0);
    check("远端更旧不提醒", !UpdateChecker::shouldNotify(QStringLiteral("0.9.0"),
                                                         QStringLiteral("1.0.0"), QString()));
    check("同版本不提醒", !UpdateChecker::shouldNotify(QStringLiteral("1.0.0"),
                                                       QStringLiteral("1.0.0"), QString()));
    check("远端更新要提醒", UpdateChecker::shouldNotify(QStringLiteral("1.0.1"),
                                                         QStringLiteral("1.0.0"), QString()));
    check("已忽略的那个版本不再提醒", !UpdateChecker::shouldNotify(
              QStringLiteral("1.0.1"), QStringLiteral("1.0.0"), QStringLiteral("1.0.1")));
    check("比已忽略版本更新的仍要提醒", UpdateChecker::shouldNotify(
              QStringLiteral("1.0.2"), QStringLiteral("1.0.0"), QStringLiteral("1.0.1")));

    // 夹具版本号一律由"本程序当前版本"推导，不能写死：以前写死 "1.1.0"，等程序真的升到
    // 1.1.0 时，shouldNotify 拿它跟本地一比判成同版本 → 不提醒 → 这条及其后 6 项连锁全红
    // （2026-10-04 发版 v1.1.0 时就是这样暴露的）。
    const QString curVer    = UpdateChecker::normalizeVersion(QStringLiteral(MUYUN_VERSION));
    const QString newerVer  = bumpLast(curVer);    // 比当前版本新 → 该提醒
    const QString newestVer = bumpLast(newerVer);  // 比"已忽略版本"还新 → 该重新提醒

    // version.json 形态（开源仓库根目录放的文件）
    const QByteArray feedV = QStringLiteral(
        R"({"version":"%1","notes":"①修A\n②修B","pageUrl":"https://github.com/x/y/releases/tag/v%1",
            "downloadUrl":"https://github.com/x/y/releases/download/v%1/setup.exe","pubDate":"2026-10-10"})")
        .arg(newerVer).toUtf8();
    UpdateInfo i1; QString e1;
    const bool ok1 = UpdateChecker::parseFeedJson(feedV, &i1, &e1);
    check("version.json 解析（版本/说明/直链）",
          ok1 && i1.version == newerVer && i1.notes.contains(QStringLiteral("修B"))
              && i1.downloadUrl.endsWith(QStringLiteral("setup.exe")),
          e1);

    // GitHub Releases API 形态（raw 拿不到时的兜底）
    const QByteArray feedApi = QStringLiteral(
        R"({"tag_name":"v%1","body":"说明","html_url":"https://github.com/x/y/releases/tag/v%1",
            "assets":[{"name":"checksums.txt","browser_download_url":"https://x/c.txt"},
                      {"name":"MuyunMusic-%1-win64-setup.exe","browser_download_url":"https://x/setup.exe"}]})")
        .arg(newestVer).toUtf8();
    UpdateInfo i2; QString e2;
    const bool ok2 = UpdateChecker::parseFeedJson(feedApi, &i2, &e2);
    check("Releases API 解析（tag_name/body/assets 自动挑安装器）",
          ok2 && i2.version == newestVer
              && i2.downloadUrl == QStringLiteral("https://x/setup.exe")
              && i2.pageUrl == QStringLiteral("https://github.com/x/y/releases/tag/v") + newestVer,
          e2);

    UpdateInfo i3; QString e3;
    check("垃圾输入（404 网页）判失败且给出原因，不崩",
          !UpdateChecker::parseFeedJson(QByteArray("<html>404</html>"), &i3, &e3) && !e3.isEmpty(),
          e3);
    UpdateInfo i4;
    check("缺版本号的清单判无效",
          !UpdateChecker::parseFeedJson(QByteArray(R"({"notes":"x"})"), &i4));

    // ---------- ② 真链路：本地清单当远程清单 ----------
    const QString dir = QDir::tempPath() + QStringLiteral("/muyun_probe");
    QDir().mkpath(dir);
    const QString feedPath = dir + QStringLiteral("/update_feed.json");
    auto writeFeed = [&feedPath](const QByteArray &body) {
        QFile f(feedPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        f.write(body);
        f.close();
        return true;
    };
    qputenv("MUYUN_UPDATE_FEED_FILE", feedPath.toLocal8Bit());
    qunsetenv("MUYUN_UPDATE_DISABLE");

    auto pump = [](int ms) {
        const qint64 end = QDateTime::currentMSecsSinceEpoch() + ms;
        while (QDateTime::currentMSecsSinceEpoch() < end)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    };
    // 一次性清掉上次运行可能留下的忽略记录（隔离目录可能复用）
    { UpdateChecker c0; c0.clearIgnoredVersion(); }
    // 每个"自动检查"场景新建实例：模拟"下次启动"（会话级"已查过"标记归零）
    auto newChecker = [&]() {
        auto *c = new UpdateChecker();
        c->setAutoCheckEnabled(true);
        return c;
    };
    auto waitIdle = [&pump](UpdateChecker *c, int timeoutMs) {
        const qint64 end = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
        while (c->checking() && QDateTime::currentMSecsSinceEpoch() < end) pump(30);
        pump(80);   // 再泵一下，让 queued 调用落地
    };

    // ① 每次启动查一次（无节流）+ 同一进程内不重复查（会话级）
    check("清单文件写入成功", writeFeed(feedV));
    int found = 0; QString foundVer;
    UpdateChecker *checker = newChecker();
    QObject::connect(checker, &UpdateChecker::updateFound, checker,
                     [&found, &foundVer](const QString &v, const QString &) {
        ++found; foundVer = v;
    });
    checker->autoCheck();
    waitIdle(checker, 4000);
    check("每次启动自动检查：拉到更新版本并发 updateFound",
          found == 1 && foundVer == newerVer
              && checker->status() == QStringLiteral("available"),
          QStringLiteral("found=%1 ver=%2 status=%3").arg(found).arg(foundVer, checker->status()));

    // ② 会话级：同一进程内第二次 autoCheck 不该再查（替代旧"每天最多一次"）
    checker->autoCheck();
    waitIdle(checker, 1500);
    check("每次启动只查一次：同一进程内第二次 autoCheck 不查", found == 1,
          QStringLiteral("found=%1").arg(found));

    // ③ 关掉「启动时自动检查」→ 新实例也不查
    {
        auto *c2 = newChecker();
        c2->setAutoCheckEnabled(false);
        int f2 = 0;
        QObject::connect(c2, &UpdateChecker::updateFound, c2,
                         [&f2](const QString &, const QString &) { ++f2; });
        c2->autoCheck();
        waitIdle(c2, 1200);
        check("关掉启动自动检查后也不查", f2 == 0, QStringLiteral("found=%1").arg(f2));
        delete c2;
    }

    // ④ 「不再提醒」→ 版本号落盘 + 压住自动弹窗（新实例 = 下次启动）
    checker->ignoreLatestVersion();
    const QString saved = DocumentStore::instance()
                              ->readSync(QStringLiteral("general"),
                                         QStringLiteral("updateIgnoredVersion"), QString())
                              .toString();
    {
        auto *c4 = newChecker();
        int f4 = 0;
        QObject::connect(c4, &UpdateChecker::updateFound, c4,
                         [&f4](const QString &, const QString &) { ++f4; });
        c4->autoCheck();
        waitIdle(c4, 4000);
        check("不再提醒：版本号落盘且自动检查不再弹窗",
              saved == newerVer && f4 == 0
                  && c4->status() == QStringLiteral("ignored"),
              QStringLiteral("存档=%1 status=%2").arg(saved, c4->status()));
        delete c4;
    }

    // ⑤ 手动检查是用户主动问的 → 忽略过也要给结果
    checker->checkForUpdates();
    waitIdle(checker, 4000);
    check("手动检查不受「不再提醒」影响",
          found == 2 && foundVer == newerVer
              && checker->status() == QStringLiteral("available"),
          QStringLiteral("found=%1 status=%2").arg(found).arg(checker->status()));

    // ⑥ 远端出现比忽略版本更新的 → 自动检查重新提醒（新实例 = 下次启动）
    writeFeed(feedApi);
    {
        auto *c6 = newChecker();
        int f6 = 0; QString v6;
        QObject::connect(c6, &UpdateChecker::updateFound, c6,
                         [&f6, &v6](const QString &v, const QString &) { ++f6; v6 = v; });
        c6->autoCheck();
        waitIdle(c6, 4000);
        check("远端出现更新版本（比已忽略版本新）→ 自动检查重新提醒",
              f6 == 1 && v6 == newestVer
                  && c6->status() == QStringLiteral("available"),
              QStringLiteral("found=%1 ver=%2").arg(f6).arg(v6));
        delete c6;
    }

    // ⑦ 「稍后」→ 本会话不再自动弹（同版本）；出更新版本会再弹（新实例模拟下次启动）
    checker->checkForUpdates();
    waitIdle(checker, 4000);
    check("手动检查给「稍后」提供版本", checker->status() == QStringLiteral("available"));
    checker->snoozeLatestVersion();
    check("「稍后」后状态变为 snoozed（会话级）",
          checker->status() == QStringLiteral("snoozed"), checker->statusText());
    checker->checkForUpdates();
    waitIdle(checker, 4000);
    check("手动检查不受「稍后」影响（用户主动问仍给结果）",
          checker->status() == QStringLiteral("available"),
          checker->statusText());
    {
        auto *c7 = newChecker();   // 下次启动：同版本仍被「稍后」压住（会话标记不跨进程）
        int f7 = 0;
        QObject::connect(c7, &UpdateChecker::updateFound, c7,
                         [&f7](const QString &, const QString &) { ++f7; });
        c7->autoCheck();
        waitIdle(c7, 4000);
        check("「稍后」不落盘：同版本在下次启动仍会提醒（每次上线触发一次）",
              f7 == 1 && c7->status() == QStringLiteral("available"),
              QStringLiteral("found=%1 status=%2").arg(f7).arg(c7->status()));
        delete c7;
    }

    // ⑧ 远端与本地同版本 → 已是最新（写"当前版本"而不是写死 1.0.0）
    writeFeed(QStringLiteral(R"({"version":"%1","notes":""})").arg(curVer).toUtf8());
    checker->checkForUpdates();
    waitIdle(checker, 4000);
    check("远端与本地同版本 → uptodate 且不弹窗",
          found == 4 && checker->status() == QStringLiteral("uptodate"), checker->statusText());

    // ⑨ 坏清单 → failed（不崩、不弹窗）
    writeFeed("<html>404</html>");
    checker->checkForUpdates();
    waitIdle(checker, 4000);
    check("坏清单 → failed 且不弹窗",
          found == 4 && checker->status() == QStringLiteral("failed"), checker->statusText());
    delete checker;

    // ⑩ 可选：真网络链路（设了 MUYUN_UPDATE_LIVE_URL 才跑）。默认的 GitHub 地址在本机可能
    //    连不上（国内网络常见），此时用本地 HTTP 服务/镜像地址验证"HTTP 取回 → 回主线程 → 落地"
    //    这条腿；开源后仓库真地址也可直接拿它验（例：MUYUN_UPDATE_LIVE_URL=<raw version.json>）。
    const QString liveUrl = qEnvironmentVariable("MUYUN_UPDATE_LIVE_URL");
    if (!liveUrl.isEmpty()) {
        qunsetenv("MUYUN_UPDATE_FEED_FILE");   // 改走网络，不再读本地文件
        qputenv("MUYUN_UPDATE_FEED", liveUrl.toLocal8Bit());
        UpdateChecker liveChecker;
        liveChecker.setAutoCheckEnabled(true);
        liveChecker.checkForUpdates();
        waitIdle(&liveChecker, 12000);
        check("真网络链路：HTTP 清单能取回并解析（MUYUN_UPDATE_LIVE_URL）",
              liveChecker.status() != QStringLiteral("failed"),
              QStringLiteral("%1 → %2").arg(liveUrl, liveChecker.statusText()));
    } else {
        printf("[SKIP] 未设 MUYUN_UPDATE_LIVE_URL，跳过真网络链路用例\n");
    }

    printf("%s 更新提示自检（%d 项失败）\n", fail == 0 ? "[PASS]" : "[FAIL]", fail);
    fflush(stdout);
    return fail == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// 自检公用小工具（文件级静态：UI 自检的步骤回调在 main 的 if 块退出之后才跑，
// 用局部 lambda 做 helper 会被按引用捕获 → 栈帧被覆盖 → 悬垂）
// ---------------------------------------------------------------------------

/// 输出用：QString → UTF-8 字节。控制台已 SetConsoleOutputCP(CP_UTF8)，
/// 而 qPrintable() 走 toLocal8Bit（GBK）→ 中文详情会乱码；自检输出统一用它。
/// 4 个轮转缓冲：一条 printf 里放多个 u8() 也不会互相踩。
static const char *u8(const QString &s)
{
    static QByteArray buf[4];
    static int idx = 0;
    idx = (idx + 1) % 4;
    buf[idx] = s.toUtf8();
    return buf[idx].constData();
}

/// 沿可视树按 objectName 找控件（Popup 的内容挂在 Overlay 上，findChildren 走 QObject
/// 父子链找不到 → 只能从窗口 contentItem 递归 childItems）
static QList<QObject *> findUiItems(QQuickWindow *win, const QString &name)
{
    QList<QObject *> hits;
    if (!win) return hits;
    std::function<void(QQuickItem *)> rec = [&](QQuickItem *it) {
        if (!it) return;
        if (it->objectName() == name) hits.append(it);
        for (QQuickItem *c : it->childItems()) rec(c);
    };
    rec(win->contentItem());
    return hits;
}

static QObject *findUiItem(QQuickWindow *win, const QString &name)
{
    const QList<QObject *> hits = findUiItems(win, name);
    return hits.isEmpty() ? nullptr : hits.first();
}

/// 泵事件 ms 毫秒（等异步落地用）
static void pumpFor(int ms)
{
    const qint64 end = QDateTime::currentMSecsSinceEpoch() + ms;
    while (QDateTime::currentMSecsSinceEpoch() < end)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

/// 等一次更新检查跑完（再多泵一会儿，让 queued 回调落地）
static void waitCheckIdle(UpdateChecker *c, int timeoutMs)
{
    if (!c) return;
    const qint64 end = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
    while (c->checking() && QDateTime::currentMSecsSinceEpoch() < end) pumpFor(30);
    pumpFor(120);
}

/// 合成一次 MouseArea 点击。⚠ MouseArea::clicked 带 QQuickMouseEvent* 参数，
/// 无参 invokeMethod 匹配不上（静默不执行）→ 必须显式塞空指针
static bool clickMouseAreaObj(QObject *area)
{
    if (!area) return false;
    const bool ok = QMetaObject::invokeMethod(area, "clicked",
                                              Q_ARG(QQuickMouseEvent *, nullptr));
    pumpFor(200);
    return ok;
}

/// 把"上次检查时间"改成 ms 毫秒之前（自检用它绕过"每天最多一次"的节流）
static void setLastCheckAgoMs(qint64 ms)
{
    DocumentStore::instance()->writeSync(QStringLiteral("general"),
                                         QStringLiteral("updateLastCheckAt"),
                                         QDateTime::currentMSecsSinceEpoch() - ms);
}

/// 写一个清单文件（自检用 MUYUN_UPDATE_FEED_FILE 注入本地清单，全程不联网）
static bool writeUpdateFeedFile(const QString &path, const QByteArray &body)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(body);
    f.close();
    return true;
}

#endif // MUYUN_SELFTES
// ===========================================================================
// 单实例守护（2026-10-04 重写：权限不一致时也不再双开）
// ===========================================================================

/// 守护判定结果
enum class InstanceOutcome {
    Skipped,     ///< 自检模式：不抢单实例（允许并行调试）
    Primary,     ///< 本进程是唯一实例，唤起监听已就绪
    Raised,      ///< 已有实例且唤起成功 → 本进程静默退出
    Blocked      ///< 已有实例但够不着（多半是权限不一致）→ 提示后退出，绝不双开
};

#ifdef Q_OS_WIN
/**
 * @brief 同会话"已经有一个实例"的凭据：命名互斥体
 *
 * 为什么不能只靠命名管道判重：管理员实例建的管道，普通权限进程连不上
 * （Qt 默认 UserAccessOption + Windows 强制完整性控制 MIC），旧写法把"连不上"
 * 当成"没有别的实例"，于是双开（README 已知问题③）。互斥体只要**读语义**就能跨权限判出来：
 *  ① 安全描述符给 NULL DACL —— 默认 DACL 只放行创建者，换个权限连打开都打不开；
 *  ② 只申请 SYNCHRONIZE —— MIC 禁止"低权限写高权限对象"，要 ALL_ACCESS 必被
 *     ACCESS_DENIED 挡死，而 SYNCHRONIZE 属读语义，能过。
 * 名字用 Local\ 前缀（当前登录会话内唯一，普通用户不需要 SeCreateGlobalPrivilege）。
 */
static void *acquireInstanceMutex(bool *alreadyRunning, QString *diag)
{
    if (alreadyRunning) *alreadyRunning = false;
    SECURITY_DESCRIPTOR sd;
    if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)) {
        if (diag) *diag = QStringLiteral("初始化安全描述符失败 err=%1").arg(GetLastError());
        return nullptr;
    }
    // NULL DACL（bDaclPresent=TRUE + ACL=nullptr）= 所有人都有全部访问权
    if (!SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE)) {
        if (diag) *diag = QStringLiteral("设置 DACL 失败 err=%1").arg(GetLastError());
        return nullptr;
    }
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    sa.lpSecurityDescriptor = &sd;

    HANDLE h = CreateMutexExW(&sa, L"Local\\MuyunMusic.singleInstance", 0, SYNCHRONIZE);
    const DWORD err = GetLastError();
    if (!h) {
        // 拿不到句柄也可能是"对象存在但我没权限"——那同样说明已有实例在跑，不能当没有
        if (err == ERROR_ACCESS_DENIED) {
            if (alreadyRunning) *alreadyRunning = true;
            if (diag) *diag = QStringLiteral("互斥体存在但当前权限打不开（err=5）");
        } else {
            if (diag) *diag = QStringLiteral("CreateMutexExW 失败 err=%1").arg(err);
        }
        return nullptr;
    }
    const bool exists = (err == ERROR_ALREADY_EXISTS);
    if (alreadyRunning) *alreadyRunning = exists;
    if (diag) *diag = exists ? QStringLiteral("互斥体已存在（已有实例）")
                             : QStringLiteral("互斥体新建（本进程是唯一实例）");
    return h;   // 句柄故意不关：进程活着，对象就活着
}
#endif // Q_OS_WIN

/// 通知已运行实例把主界面唤到前台。三次机会 × 400ms：
/// 对方可能正在启动（安装版冷启动慢），一次连不上就判"没有实例"也是旧版双开的成因之一。
static bool raiseExistingInstance(const QString &key, QString *diag)
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        QLocalSocket probe;
        probe.connectToServer(key);
        if (probe.waitForConnected(600)) {
            probe.write("raise");
            probe.flush();
            probe.waitForBytesWritten(400);
            probe.disconnectFromServer();
            if (diag) *diag = QStringLiteral("唤起成功（第%1次尝试）").arg(attempt + 1);
            return true;
        }
        if (diag)
            *diag = QStringLiteral("唤起失败（第%1次）error=%2 %3")
                        .arg(attempt + 1).arg(int(probe.error())).arg(probe.errorString());
        if (attempt < 2) QThread::msleep(400);
    }
    return false;
}

int main(int argc, char *argv[])
{
    // 把 Qt 内部消息（含 QML 报错）直接写到 stderr，便于诊断
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &, const QString &msg) {
        const char *level = "INFO";
        if (type == QtWarningMsg) level = "WARN";
        else if (type == QtCriticalMsg) level = "CRIT";
        else if (type == QtFatalMsg) level = "FATAL";
        fprintf(stderr, "[%s] %s\n", level, qPrintable(msg));
        fflush(stderr);
    });

    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

#ifdef Q_OS_WIN
    // 让控制台以 UTF-8 显示中文，避免自检输出乱码
    SetConsoleOutputCP(CP_UTF8);
#endif

    // QApplication（QtWidgets）：系统托盘 QSystemTrayIcon 需要
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("MuyunMusic"));
    app.setApplicationDisplayName(QStringLiteral("暮云音乐"));
    app.setOrganizationName(QStringLiteral("Muyun"));
    app.setApplicationVersion(QStringLiteral(MUYUN_VERSION));
    app.setWindowIcon(QIcon(QStringLiteral(":/resources/app.svg")));

    QLocale::setDefault(QLocale(QLocale::Chinese, QLocale::China));
    QFont defaultFont(QStringLiteral("Microsoft YaHei UI"), 10);
    defaultFont.setHintingPreference(QFont::PreferFullHinting);
    app.setFont(defaultFont);

    QQuickStyle::setStyle(QStringLiteral("Basic"));

    const QStringList args = app.arguments();

    // 同步自检用隔离数据根目录，避免污染用户真实收藏/歌单
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-sync")))
        qputenv("MUYUN_STORE_ROOT",
                (QDir::tempPath() + QStringLiteral("/muyun-synctest")).toLocal8Bit());
#endif // MUYUN_SELFTES
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-lxsync")))
        qputenv("MUYUN_STORE_ROOT",
                (QDir::tempPath() + QStringLiteral("/muyun-lxsynctest")).toLocal8Bit());
#endif // MUYUN_SELFTES
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-ui")))
        qputenv("MUYUN_STORE_ROOT",
                (QDir::tempPath() + QStringLiteral("/muyun-uitest")).toLocal8Bit());
#endif // MUYUN_SELFTES
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-queue")))
        qputenv("MUYUN_STORE_ROOT",
                (QDir::tempPath() + QStringLiteral("/muyun-queuetest")).toLocal8Bit());
#endif // MUYUN_SELFTES
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-advance")))
        qputenv("MUYUN_STORE_ROOT",
                (QDir::tempPath() + QStringLiteral("/muyun-advancetest")).toLocal8Bit());
#endif // MUYUN_SELFTES
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-update")))
        qputenv("MUYUN_STORE_ROOT",
                (QDir::tempPath() + QStringLiteral("/muyun-updatetest")).toLocal8Bit());
    if (args.contains(QStringLiteral("--test-update-ui")))
        qputenv("MUYUN_STORE_ROOT",
                (QDir::tempPath() + QStringLiteral("/muyun-updateuitest")).toLocal8Bit());
#endif // MUYUN_SELFTES

    auto *store = DocumentStore::instance();

    // ---- 跨线程警告预防（待办 #3）----
    // HttpClient 单例的父是 qApp，若它第一次被"音源解析/歌词/下载"等 worker 线程
    // 触发创建，就会报 "Cannot create children for a parent that is in a different
    // thread"。必须由主线程抢先创建（DocumentStore/MusicSdk 已在主线程早建，只差它）。
    Muyun::HttpClient::instance();

    // ---- 单实例守护 ----
    // 再次启动程序 → 通知已运行实例（含最小化到托盘托管的）唤起主界面，本进程静默退出。
    // 自检模式（--test-*）不受限，允许多实例并行调试；两个例外见下面 guardActive：
    // --test-single-instance（父：当主实例跑）与 --single-instance-probe（子：当第二实例跑）。
    // 关键：探测+抢注监听必须放在一切重初始化之前。此前 listen 在 main 末尾，安装版冷启动
    // 慢（杀软扫描/QML 编译），双击两下都探测失败 → 双双起飞（单实例失效根因）。
    // 判重顺序：先命名互斥体（跨权限也判得出"有没有别人"），再用管道唤起（够得着就唤醒）。
    static const QString kSingleKey = QStringLiteral("MuyunMusic.singleInstance");
    const bool isSelfTest = std::any_of(args.cbegin(), args.cend(),
        [](const QString &a) { return a.startsWith(QStringLiteral("--test-")); });
    struct RaiseBox { QPointer<QQuickWindow> win; bool pending = false; int hits = 0; };
    auto raiseBox = std::make_shared<RaiseBox>();   // 被监听回调持有，shared_ptr 管理生命周期
    auto raiseWindow = [](QQuickWindow *w) {
        if (!w) return;
        if (w->visibility() == QWindow::Minimized) w->showNormal();
        w->show();
        w->raise();
        w->requestActivate();
#ifdef Q_OS_WIN
        if (HWND hwnd = reinterpret_cast<HWND>(w->winId())) {
            SetForegroundWindow(hwnd);
            SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        }
#endif
    };

    const bool probeChild = args.contains(QStringLiteral("--single-instance-probe"));
    const bool guardActive = !isSelfTest || probeChild
                             || args.contains(QStringLiteral("--test-single-instance"));
    InstanceOutcome outcome = InstanceOutcome::Skipped;
    QString instanceDiag;
#ifdef Q_OS_WIN
    bool otherInstanceRunning = false;
    void *instanceMutex = acquireInstanceMutex(&otherInstanceRunning, &instanceDiag);
    Q_UNUSED(instanceMutex);   // 故意不关：本进程活着，互斥体就活着
#else
    const bool otherInstanceRunning = false;
#endif

    if (!guardActive) {
        outcome = InstanceOutcome::Skipped;
    } else if (otherInstanceRunning) {
        // 已有实例：够得着就唤起它；够不着（权限不一致 / 管道没建好）也**不再自己起一个**
        outcome = raiseExistingInstance(kSingleKey, &instanceDiag)
                      ? InstanceOutcome::Raised : InstanceOutcome::Blocked;
    } else {
        QLocalServer::removeServer(kSingleKey);   // 清理上次异常退出的残留监听（只在主实例路径做）
        auto *singleSrv = new QLocalServer(&app);
        // Qt 默认 UserAccessOption：只有同用户同权限能连。改 WorldAccessOption，
        // 让"先开普通权限、再以管理员权限启动"这一侧至少连得上、能唤起。
        singleSrv->setSocketOptions(QLocalServer::WorldAccessOption);
        QObject::connect(singleSrv, &QLocalServer::newConnection, singleSrv,
                         [singleSrv, raiseBox, raiseWindow]() {
            if (QLocalSocket *c = singleSrv->nextPendingConnection()) {
                c->readAll();
                c->disconnectFromServer();
                c->deleteLater();
            }
            ++raiseBox->hits;
            if (QQuickWindow *w = raiseBox->win.data()) raiseWindow(w);
            else raiseBox->pending = true;   // 启动中：主界面就绪后补唤起
        });
        if (singleSrv->listen(kSingleKey)) {
            outcome = InstanceOutcome::Primary;
        } else if (raiseExistingInstance(kSingleKey, &instanceDiag)) {
            // 极小概率竞态：判重之后、监听建好之前被另一个实例抢先 → 唤起它
            outcome = InstanceOutcome::Raised;
        } else {
            // 建不起监听又唤不起别人：继续自己跑，兜底不误杀（与老行为一致）
            outcome = InstanceOutcome::Skipped;
            qWarning() << "[single] 监听失败且无法唤起已有实例";
        }
    }

#ifdef MUYUN_SELFTES
    // 探针子实例：只跑上面这套判定，把结论打到 stdout 后退出（不建窗口、不碰数据）
    if (probeChild) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        static const char *kNames[] = { "Skipped", "Primary", "Raised", "Blocked" };
        printf("PROBE 判定=%s 说明=%s\n", kNames[int(outcome)],
               u8(instanceDiag.isEmpty() ? QStringLiteral("-") : instanceDiag));
        fflush(stdout);
        return finishSelfTest(0);
    }
#endif // MUYUN_SELFTES

    // ---- 单实例守护自检 ----
    // 父进程按正常流程当"唯一实例"，再派子进程当"第二个实例"，断言：
    //  ① 子进程绝不判成 Primary（不双开）；② 子进程能把"唤起"送到父进程（真唤起了窗口）。
    // 跨权限（管理员↔普通）那一侧本机没法自动化（要 UAC 弹窗），但判重靠的是
    // "命名互斥体 + 只申请 SYNCHRONIZE"，与权限无关；子进程够不着管道时会判 Blocked 而不是双开。
    // 用法：MuyunMusic.exe --test-single-instance
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-single-instance"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        printf("=== 单实例守护自检 ===\n");
        int fail = 0;

        const bool primary = (outcome == InstanceOutcome::Primary);
        if (!primary) {
            // 机器上已经有一个暮云在跑（比如用户开着的开发版）→ 本自检没法当主实例
            printf("[SKIP] 已有实例在运行（判定=%d），单实例自检需要一个干净的主实例\n",
                   int(outcome));
            fflush(stdout);
            return finishSelfTest(0);
        }
        printf("[PASS] 本进程判为唯一实例并建起唤起监听\n");

        // 派子实例：必须"不双开"，且把唤起打到我们头上
        auto runProbe = [raiseBox](int round) -> QString {
            QProcess child;
            const QString exe = QCoreApplication::applicationFilePath();
            child.setProgram(exe);
            child.setArguments({ QStringLiteral("--single-instance-probe") });
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("MUYUN_QUIET_SINGLE_INSTANCE"), QStringLiteral("1"));
            child.setProcessEnvironment(env);
            child.start();
            if (!child.waitForStarted(10000))
                return QStringLiteral("[FATAL] 子进程起不来 %1").arg(exe);
            // 一边等一边泵事件：QLocalServer 收连接要事件循环，
            // 干等 waitForFinished 会把子进程的唤起饿死（假失败）
            const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 25000;
            while (child.state() != QProcess::NotRunning
                   && QDateTime::currentMSecsSinceEpoch() < deadline)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            if (child.state() != QProcess::NotRunning) {
                child.kill();
                child.waitForFinished(2000);
                return QStringLiteral("[FATAL] 第%1个子实例超时未退出").arg(round);
            }
            return QString::fromUtf8(child.readAllStandardOutput()).trimmed();
        };

        const int hitsBefore = raiseBox->hits;
        const QString out1 = runProbe(1);
        printf("     子实例输出：%s\n", u8(out1.isEmpty() ? QStringLiteral("(空)") : out1));
        const bool notDouble = out1.contains(QStringLiteral("判定=Raised"))
                               || out1.contains(QStringLiteral("判定=Blocked"));
        printf("[%s] 第二个实例没有变成主实例（不双开）\n", notDouble ? "PASS" : "FAIL");
        if (!notDouble) ++fail;
        const bool woke = (raiseBox->hits > hitsBefore);
        printf("[%s] 子实例把「唤起」送到了主实例（收到 %d 次）\n",
               woke ? "PASS" : "FAIL", raiseBox->hits - hitsBefore);
        if (!woke) ++fail;

        // 连开两个也不双开（旧版"双击两下双双起飞"就是这条没兜住）
        const int hits2 = raiseBox->hits;
        const QString out2 = runProbe(2);
        const bool notDouble2 = out2.contains(QStringLiteral("判定=Raised"))
                                || out2.contains(QStringLiteral("判定=Blocked"));
        printf("[%s] 连续第二次启动仍不双开（%s）\n", notDouble2 ? "PASS" : "FAIL",
               u8(out2.split(QLatin1Char('\n')).last()));
        if (!notDouble2 || raiseBox->hits <= hits2) ++fail;

        printf("单实例守护自检结束（%d 项失败）\n", fail);
        fflush(stdout);
        return finishSelfTest(fail == 0 ? 0 : 4);
    }
#endif // MUYUN_SELFTES

    // 上面那套判定落地：已有实例 → 唤起它并静默退出；够不着 → 明说后退出（绝不双开）。
    if (outcome == InstanceOutcome::Raised)
        return 0;
    if (outcome == InstanceOutcome::Blocked) {
        // 够不着的那个实例：多半是以管理员权限先启动的。明说，别静默双开。
        const QString msg = QStringLiteral(
            "暮云音乐已经在运行了，但这个实例够不着它（通常是已运行的那个用了管理员权限）。\n\n"
            "请先在任务栏 / 系统托盘找到已打开的窗口；\n"
            "如果确实要用当前权限再开一个，请先退出已运行的实例。");
        qWarning().noquote() << "[single] 已有实例但无法唤起：" << instanceDiag;
        if (!qEnvironmentVariableIsSet("MUYUN_QUIET_SINGLE_INSTANCE")) {
#ifdef Q_OS_WIN
            MessageBoxW(nullptr, reinterpret_cast<LPCWSTR>(msg.utf16()),
                        L"暮云音乐", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
#endif
        }
        return 0;
    }

    // 自检模式
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-crypto")))
        return finishSelfTest(runCryptoSelfTest());
#endif // MUYUN_SELFTES

#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-lxsource"))) {
        const int si = args.indexOf(QStringLiteral("--test-lxsource"));
        // 不传 URL 就只跑内置协议一致性（离线可重复）；传了才加跑真脚本在线腿
        const QString url = (si + 1 < args.size() && !args.at(si + 1).startsWith(QStringLiteral("--")))
            ? args.at(si + 1)
            : QString();
        return finishSelfTest(runLxSourceSelfTest(url));
    }
#endif // MUYUN_SELFTES

#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-dsp")))
        return finishSelfTest(runDspSelfTest());
    if (args.contains(QStringLiteral("--test-dsp-real")))
        return finishSelfTest(runDspRealSelfTest(args));
#endif // MUYUN_SELFTES

    // 音频启动探针：AudioSes.dll 仅在进程真正打开 WASAPI 音频会话时载入 →
    // 验证 PlayerEngine 全懒创建后启动不再碰设备（对照：裸 QMediaPlayer 构造即加载）
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-audio-probe"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef Q_OS_WIN
        auto loaded = []() { return GetModuleHandleA("AudioSes.dll") != nullptr; };
        const bool b0 = loaded();
        auto *eng = new Muyun::PlayerEngine(qApp);
        const bool b1 = loaded();
        eng->setVolume(0.5);
        eng->setMuted(false);
        QCoreApplication::processEvents();
        const bool b2 = loaded();
        printf("[%s] PlayerEngine构造后AudioSes=%d(期望0) setVolume/事件泵后=%d(期望0)\n",
               (!b0 && !b1 && !b2) ? "PASS" : "FAIL", int(b1), int(b2));
        // 诊断步骤（会主动加载 AudioSes，放在断言之后）
        printf("[iso] 基线=%d（下面逐步找加载点）\n", int(loaded()));
        { QAudioDevice d = QMediaDevices::defaultAudioOutput(); Q_UNUSED(d);
          printf("[iso] defaultAudioOutput()   AudioSes=%d\n", int(loaded())); }
        fflush(stdout);
        finishSelfTest((!b0 && !b1 && !b2) ? 0 : 1);
#else
        finishSelfTest(0);
#endif
    }
#endif // MUYUN_SELFTES

#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-hot")))
        return finishSelfTest(runHotSelfTest());
#endif // MUYUN_SELFTES

#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-explore"))) {
        const int i = args.indexOf(QStringLiteral("--test-explore"));
        const QString tag = (i + 1 < args.size() && !args.at(i + 1).startsWith(QLatin1String("--")))
                                ? args.at(i + 1) : QString();
        return finishSelfTest(runExploreSelfTest(tag));
    }
#endif // MUYUN_SELFTES

#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-home")))
        return finishSelfTest(runHomeSelfTest());
#endif // MUYUN_SELFTES

#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-update")))
        return finishSelfTest(runUpdateSelfTest());
#endif // MUYUN_SELFTES

#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-mem"))) {
        const int mi = args.indexOf(QStringLiteral("--test-mem"));
        int lim = 24;
        if (mi + 1 < args.size() && !args.at(mi + 1).startsWith(QStringLiteral("--")))
            lim = args.at(mi + 1).toInt();
        return finishSelfTest(runMemSelfTest(lim));
    }
#endif // MUYUN_SELFTES

#ifdef MUYUN_SELFTES
    const int testIdx = args.indexOf(QStringLiteral("--test-search"));
    if (testIdx >= 0) {
        const QString kw = (testIdx + 1 < args.size()) ? args.at(testIdx + 1)
                                                      : QStringLiteral("周杰伦");
        return finishSelfTest(runSourceSelfTest(kw));
    }

    const int dlIdx = args.indexOf(QStringLiteral("--test-download"));
    if (dlIdx >= 0) {
        const QString kw = (dlIdx + 1 < args.size() && !args.at(dlIdx + 1).startsWith("--"))
                           ? args.at(dlIdx + 1) : QStringLiteral("晴天");
        const QString q = (dlIdx + 2 < args.size() && !args.at(dlIdx + 2).startsWith("--"))
                          ? args.at(dlIdx + 2) : QStringLiteral("320k");
        return finishSelfTest(runDownloadSelfTest(kw, q));
    }

    const int dcIdx = args.indexOf(QStringLiteral("--test-download-concurrent"));
    if (dcIdx >= 0) {
        const QString kw = (dcIdx + 1 < args.size() && !args.at(dcIdx + 1).startsWith("--"))
                           ? args.at(dcIdx + 1) : QStringLiteral("周杰伦");
        const QString q = (dcIdx + 2 < args.size() && !args.at(dcIdx + 2).startsWith("--"))
                          ? args.at(dcIdx + 2) : QStringLiteral("320k");
        return runDownloadConcurrencyTest(kw, q);
    }
#endif // MUYUN_SELFTES

    MusicSdk::instance();

    auto *player = new PlayerController(qApp);
    auto *search = new SearchController(qApp);
    auto *library = new LibraryController(qApp);
    auto *home = new HomeController(qApp);
    auto *theme = new Theme(qApp);
    auto *settings = new SettingsController(library, qApp);
    // 应用自身更新提示（开源方案 A：纯 GitHub 清单；见 core/update/UpdateChecker）
    auto *updater = new UpdateChecker(qApp);
    auto *tray = new TrayController(player, qApp);
    auto *downloads = new DownloadController(qApp);
    auto *sync = new SyncController(library, qApp);

    // 下载完成后触发本地库重扫，新文件出现在「本地音乐」
    QObject::connect(downloads, &DownloadController::rescanRequested,
                     library, &LibraryController::rescan);

    // 本地歌曲时长自校准：播放器实测时长回写库（VBR 估算误差）
    QObject::connect(player, &PlayerController::localDurationKnown,
                     library, &LibraryController::fixLocalDuration);

    // 本地文件被删除/移除 → 从播放队列剔除该曲（避免切歌撞空文件卡住在线播放）
    QObject::connect(library, &LibraryController::localSongRemoved,
                     player, &PlayerController::dropLocalSong);

    QQmlApplicationEngine engine;

    // 打包目录下 windeployqt 会把 QML 模块放到 <exe>/qml，需显式加入导入路径
    engine.addImportPath(QCoreApplication::applicationDirPath() + QStringLiteral("/qml"));

    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"),
                                             QStringLiteral(MUYUN_VERSION));
    engine.rootContext()->setContextProperty(QStringLiteral("appDataPath"),
                                             store->rootPath());
    engine.rootContext()->setContextProperty(QStringLiteral("player"), player);
    engine.rootContext()->setContextProperty(QStringLiteral("searcher"), search);
    engine.rootContext()->setContextProperty(QStringLiteral("library"), library);
    engine.rootContext()->setContextProperty(QStringLiteral("home"), home);
    engine.rootContext()->setContextProperty(QStringLiteral("theme"), theme);
    engine.rootContext()->setContextProperty(QStringLiteral("settings"), settings);
    engine.rootContext()->setContextProperty(QStringLiteral("updater"), updater);
    engine.rootContext()->setContextProperty(QStringLiteral("tray"), tray);
    engine.rootContext()->setContextProperty(QStringLiteral("downloads"), downloads);
    engine.rootContext()->setContextProperty(QStringLiteral("sync"), sync);
    engine.rootContext()->setContextProperty(QStringLiteral("lxsync"), sync->lxServer());

    // 桌面歌词：QQuickView 承载的独立置顶窗，复用主 engine 共享 context
    auto *deskLyrics = new DesktopLyricsController(player, &engine, qApp);
    engine.rootContext()->setContextProperty(QStringLiteral("deskLyrics"), deskLyrics);

    // 舞台引擎桥（方案C：独立 MuyunStage.exe + WebView2）
    auto *stage = new StageBridge(player, settings, qApp);
    engine.rootContext()->setContextProperty(QStringLiteral("stage"), stage);

    // F11 全局热键守护（主窗激活期间注册，绕开输入法/钩子吞键；命中直接切全屏，无需 QML 接线）
    auto *hotkey = new HotkeyManager(qApp);
    // Esc 也要走同一条系统热键通道：舞台（独立进程 + WebView2）会叼走系统激活，
    // 那时主窗的 QML Shortcut 和页面 keydown 都可能收不到键（用户实测 ESC 失灵的真身）。
    // 注册与否由 QML 决定（它同时知道播放页/舞台是否开着），且只在"前台是我们的窗"时占着。
    engine.rootContext()->setContextProperty(QStringLiteral("hotkey"), hotkey);

    // 输入法上下文守护：Qt 在"焦点对象不接受输入法"时会把整个窗口的 IME 摘掉，
    // 于是焦点不在输入框时 Ctrl+Space / Shift 切不了中英文（用户报的已知问题①）。
    auto *imeGuard = new ImeGuard(qApp);
    engine.rootContext()->setContextProperty(QStringLiteral("imeGuard"), imeGuard);

    // Windows 无边框：原生边缘缩放 + 最大化几何修复（替代 QML MouseArea 抖动方案）
    auto *frameless = new FramelessWindow(qApp);
    // QML 侧 toggleMaximize 时同步 WS_MAXIMIZE 样式位（HANDOFF #11，第三方任务栏工具认最大化）
    engine.rootContext()->setContextProperty(QStringLiteral("frameless"), frameless);

    // 舞台 fx 视觉控制台：独立置顶浮层小窗（QQuickView 复用主 engine，
    // 浮在舞台窗之上，经 stage 的 fx* 通道透传引擎）
    auto *stageFx = new StageFxController(stage, &engine, qApp);
    engine.rootContext()->setContextProperty(QStringLiteral("stageFx"), stageFx);

    // 舞台端到端自检：拉起 MuyunStage → 等引擎就绪握手 → 注入合成曲目/歌词
    // 用法：MuyunMusic.exe --test-stage
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-stage"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        printf("=== 舞台端到端自检 ===\n");
        fflush(stdout);
        bool gotEngineReady = false;
        int ctrlSeen = 0;
        QObject::connect(stage, &StageBridge::activeChanged, [&]() {
            if (stage->active() && !gotEngineReady) {
                gotEngineReady = true;
                printf("[OK] engineReady 握手完成，注入合成曲目\n");
                fflush(stdout);
                QVariantList words1;
                auto mkWord = [](const QString &t, double s, double e) {
                    return QVariantMap{{QStringLiteral("text"), t},
                                       {QStringLiteral("startTime"), s},
                                       {QStringLiteral("endTime"), e}};
                };
                words1 << mkWord(QStringLiteral("暮"), 0.0, 0.4)
                       << mkWord(QStringLiteral("云"), 0.4, 0.8)
                       << mkWord(QStringLiteral("自"), 0.8, 1.2)
                       << mkWord(QStringLiteral("检"), 1.2, 1.6)
                       << mkWord(QStringLiteral("舞"), 1.6, 2.0)
                       << mkWord(QStringLiteral("台"), 2.0, 2.4)
                       << mkWord(QStringLiteral("就绪"), 2.4, 3.6);
                QVariantList lines;
                lines << QVariantMap{{QStringLiteral("time"), 0.0},
                                     {QStringLiteral("text"), QStringLiteral("暮云自检舞台就绪")},
                                     {QStringLiteral("words"), words1}};
                lines << QVariantMap{{QStringLiteral("time"), 4.0},
                                     {QStringLiteral("text"), QStringLiteral("封面粒子正在渲染")}};
                const QString coverFile = QCoreApplication::applicationDirPath()
                    + QStringLiteral("/stage/web/covers/test.png");
                const QString coverUrl = QFileInfo::exists(coverFile)
                    ? QStringLiteral("/covers/test.png") : QString();
                // 延迟注入：避开 engineReady 处理里 pushTrackNow（空播放器曲目）的覆盖
                QTimer::singleShot(600, stage, [stage, coverUrl, lines]() {
                    stage->testInject(QStringLiteral("自检曲目"), QStringLiteral("暮云"),
                                      coverUrl, lines);
                });
            }
        });
        QObject::connect(stage, &StageBridge::ctrlEvent, [&](const QString &a, double) {
            ++ctrlSeen;
            printf("[EVT] ctrl=%s\n", qPrintable(a));
            fflush(stdout);
        });
        QObject::connect(stage, &StageBridge::toastMessage, [](const QString &m) {
            printf("[EVT] toast=%s\n", qPrintable(m));
            fflush(stdout);
        });

        stage->open();
        QElapsedTimer t; t.start();
        qint64 lastMark = 0;
        bool probed = false;
        while (t.elapsed() < 20000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            if (!probed && gotEngineReady && t.elapsed() > 6000) {
                probed = true;
                stage->domProbe(800, 600, 480, 220);   // 右下象限（黑块区域）
                fprintf(stderr, "[test] domProbe sent\n");
            }
            if (t.elapsed() - lastMark > 1000) {
                lastMark = t.elapsed();
                fprintf(stderr, "[test] t=%lldms active=%d ready=%d\n",
                        (long long)t.elapsed(), (int)stage->active(), (int)gotEngineReady);
            }
            if (gotEngineReady && t.elapsed() > 9000) break;   // 就绪后再观察 3~6s
            if (!gotEngineReady && t.elapsed() > 15000) break; // 超时
        }
        fprintf(stderr, "[test] loop exited at %lldms, closing\n", (long long)t.elapsed());
        printf("[%s] engineReady=%s ctrlSeen=%d\n",
               gotEngineReady ? "PASS" : "FAIL",
               gotEngineReady ? "yes" : "no", ctrlSeen);
        fflush(stdout);
        stage->close();
        fprintf(stderr, "[test] close() returned, draining\n");
        // 让 terminate → finished → onProcFinished 在事件循环里干净收尾
        {
            QElapsedTimer d; d.start();
            while (d.elapsed() < 1500)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        }
        fprintf(stderr, "[test] returning %d\n", gotEngineReady ? 0 : 4);
        return finishSelfTest(gotEngineReady ? 0 : 4);
    }
#endif // MUYUN_SELFTES

    // 标签写入自检：复制本地音频到 temp → 读时长（Xing VBR 验证）→ TagWriter 写
    // 标题/歌词/封面 → TagReader 读回断言 → 二次写入验证替换不损坏
    // 用法：MuyunMusic.exe --test-tag [音频文件路径，缺省自动找本地库第一个]
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-tag"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        printf("=== 标签内嵌自检 ===\n");

        // 0) FLAC STREAMINFO 偏移回归（合成头，不依赖外部文件）：
        //    44100Hz/2ch/24bit/259s=11421900 样本；帧大小故意非零——
        //    旧实现把 totalSamples 读成 si[5..10]（落在帧大小字段）→ 时长爆炸成几百小时
        {
            QByteArray si(34, '\0');
            si[0] = char(0x10); si[1] = 0;   // min block 4096
            si[2] = char(0x10); si[3] = 0;   // max block 4096
            si[4] = 0; si[5] = 0x08; si[6] = 0;      // min frame 2048
            si[7] = 0; si[8] = 0x20; si[9] = 0;      // max frame 8192
            const quint64 packed = (quint64(44100) << 44) | (quint64(1) << 41)
                                 | (quint64(23) << 36) | quint64(11421900);
            for (int b = 0; b < 8; ++b) si[10 + b] = char((packed >> (56 - 8 * b)) & 0xFF);
            QByteArray file = "fLaC";
            file += char(0x80);                       // last=1, type=STREAMINFO
            file += char(0); file += char(0); file += char(34);
            file += si;
            const QString fp = QDir::tempPath() + QStringLiteral("/muyun-flac-hdr.flac");
            QFile wf(fp);
            if (wf.open(QIODevice::WriteOnly)) { wf.write(file); wf.close(); }
            LocalTags ft; AudioInfo ai;
            TagReader::read(fp, ft, ai);
            QFile::remove(fp);
            const bool flacOk = ai.durationSec == 259 && ai.sampleRate == 44100;
            printf("[%s] FLAC STREAMINFO：时长=%ds(期望259) 采样率=%d(期望44100)\n",
                   flacOk ? "PASS" : "FAIL", ai.durationSec, ai.sampleRate);
            if (!flacOk) return finishSelfTest(4);
        }

        const int ti = args.indexOf(QStringLiteral("--test-tag"));
        QString src = (ti + 1 < args.size() && !args.at(ti + 1).startsWith(QStringLiteral("--")))
                          ? args.at(ti + 1) : QString();
        if (src.isEmpty()) {
            const QVariantMap doc = DocumentStore::instance()->readAll(QStringLiteral("local-music"));
            bool found = false;
            for (const auto &folder : doc.value(QStringLiteral("folders")).toStringList()) {
                QDirIterator it(folder, {QStringLiteral("*.mp3"), QStringLiteral("*.flac")},
                                QDir::Files, QDirIterator::Subdirectories);
                if (it.hasNext()) { src = it.next(); found = true; break; }
            }
            if (!found) { printf("[FATAL] 未找到 mp3/flac 测试文件\n"); return finishSelfTest(1); }
        }
        printf("[OK] 测试源：%s\n", qPrintable(src));

        const QString suffix = QFileInfo(src).suffix().toLower();
        // 写到源文件同目录（贴近真实下载落盘位置；%TEMP% 常被杀软/索引锁定，不代表真实场景）
        const QString tmp1 = QFileInfo(src).absolutePath()
                             + QStringLiteral("/_muyun_tagtest_.") + suffix;
        QFile::remove(tmp1);
        if (!QFile::copy(src, tmp1)) { printf("[FATAL] 复制失败\n"); return finishSelfTest(1); }

        // 1) 时长（Xing VBR 修复验证 + four-59 帧头遍历/OGG granule 精度）
        LocalTags tags0; AudioInfo info0;
        TagReader::read(tmp1, tags0, info0);
        printf("[INFO] 时长=%ds (%dm%02ds) bitrate=%dkbps 格式=%s\n",
               info0.durationSec, info0.durationSec / 60, info0.durationSec % 60,
               info0.bitrate, qPrintable(info0.format));

        // 1b) **列表时长必须等于解码器实测时长**（four-59：用户报"本地音乐列表时长与实际播放不符，
        //     点歌播放后才正常"）。参照物用 PcmSource（minimp3 帧表 / FFmpeg 索引），差 ≤1s 才算准。
        //     这条断言就是给"扫描时把时长算错、靠播放兜底纠正"这个 bug 立的。
        bool durPass = true;
        {
            QString perr;
            PcmSource *ps = createPcmSource(tmp1, &perr);
            const qint64 realMs = ps ? ps->durationMs() : 0;
            const QString backend = ps ? ps->backendName() : QString();
            delete ps;
            const qint64 tagMs = qint64(info0.durationSec) * 1000;
            const qint64 diffMs = qAbs(tagMs - realMs);
            // 参照物拿不到（后端打不开）时只要求时长>0，不误报
            durPass = info0.durationSec > 0 && (realMs <= 0 || diffMs <= 1000);
            printf("[%s] 时长精度：标签/列表=%lldms 解码实测=%lldms(后端=%s) 差=%lldms(容差1000)\n",
                   durPass ? "PASS" : "FAIL", (long long)tagMs, (long long)realMs,
                   qPrintable(backend), (long long)diffMs);
        }

        // 2) 生成测试封面（橙粉渐变 300x300 → JPEG）
        QImage img(300, 300, QImage::Format_RGB32);
        img.fill(QColor(255, 140, 60));
        for (int y = 0; y < 300; ++y)
            for (int x = 0; x < 300; ++x)
                img.setPixelColor(x, y, QColor(255 - x / 2, 100 + y / 3, 200 - x / 4));
        QByteArray jpg;
        { QBuffer b(&jpg); b.open(QIODevice::WriteOnly); img.save(&b, "JPEG", 90); }

        TagWriter::Payload p;
        p.title = QStringLiteral("暮云标签测试");
        p.artist = QStringLiteral("质检员");
        p.album = QStringLiteral("QA");
        p.coverJpeg = jpg;
        p.lyrics = QStringLiteral("[00:00.00]测试歌词第一行\n[00:05.00]测试歌词第二行");

        bool pass = durPass;   // four-59：时长精度不合格则整体失败
        // TagWriter 只支持 MP3/FLAC（其他格式静默返回 false，见 TagWriter::write 注释）：
        // OGG/M4A 这类素材只验时长精度，写标签轮次记 SKIP，别让"格式不支持"污染回归（four-59）
        const bool tagWritable = info0.format == QStringLiteral("MP3")
                                 || info0.format == QStringLiteral("FLAC");
        if (!tagWritable) {
            printf("[SKIP] 标签写入轮次：%s 不在 TagWriter 支持范围（仅 MP3/FLAC），本素材只验时长精度\n",
                   qPrintable(info0.format));
        }
        for (int round = 1; tagWritable && round <= 2; ++round) {
            if (!TagWriter::write(tmp1, p)) { printf("[FAIL] 第%d轮写入失败\n", round); pass = false; break; }
            LocalTags tags; AudioInfo info;
            TagReader::read(tmp1, tags, info);
            const bool tOk = tags.title == p.title && tags.artist == p.artist;
            const bool lOk = tags.lyrics.contains(QStringLiteral("测试歌词第一行"));
            const bool cOk = tags.hasCover && !tags.cover.isNull();
            const bool dOk = info.durationSec > 0 && info.valid;
            printf("[%s] 第%d轮 文本=%d 歌词=%d 封面=%d(%dx%d) 时长=%ds\n",
                   (tOk && lOk && cOk && dOk) ? "PASS" : "FAIL",
                   round, int(tOk), int(lOk), int(cOk),
                   tags.cover.width(), tags.cover.height(), info.durationSec);
            if (!(tOk && lOk && cOk && dOk)) { pass = false; break; }
        }
        QFile::remove(tmp1);
        printf("[%s] 标签内嵌自检结束\n", pass ? "PASS" : "FAIL");
        return finishSelfTest(pass ? 0 : 4);
    }
#endif // MUYUN_SELFTES

    // 音效管线自检：minimp3 解码 + AudioEffects 处理，验证"音效确实改变音频"（RMS 前后对比，不靠耳朵）
    // 用法：MuyunMusic.exe --test-effects [mp3 路径，缺省自动找]
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-effects"))) {
        // 防呆：本组自检会写 audio-effects 设置文档，不隔离就会污染用户真实设置
        // （实测踩过：早期跑漏了 MUYUN_STORE_ROOT，把用户设置改成"10 段全 +10dB + 混响 + 响度"）
        if (qEnvironmentVariableIsEmpty("MUYUN_STORE_ROOT")) {
            printf("[FAIL] 请先设 MUYUN_STORE_ROOT=<临时目录> 再跑（本自检会写音效设置）\n");
            return finishSelfTest(4);
        }
        setvbuf(stdout, nullptr, _IONBF, 0);
        printf("=== 音效管线自检 ===\n");
        const int ei = args.indexOf(QStringLiteral("--test-effects"));
        QString src = (ei + 1 < args.size() && !args.at(ei + 1).startsWith(QStringLiteral("--")))
                          ? args.at(ei + 1) : QString();
        if (src.isEmpty()) {
            const QVariantMap doc = DocumentStore::instance()->readAll(QStringLiteral("local-music"));
            QStringList dirs = doc.value(QStringLiteral("folders")).toStringList();
            dirs << QStringLiteral("E:/Music");
            bool found = false;
            for (const auto &folder : dirs) {
                QDirIterator it(folder, {QStringLiteral("*.mp3")}, QDir::Files,
                                QDirIterator::Subdirectories);
                if (it.hasNext()) { src = it.next(); found = true; break; }
            }
            if (!found) { printf("[SKIP] 未找到 mp3 测试文件\n"); return finishSelfTest(0); }
        }
        printf("[OK] 测试源：%s\n", qPrintable(src));

        QFile f(src);
        if (!f.open(QIODevice::ReadOnly)) { printf("[FAIL] 打不开\n"); return finishSelfTest(4); }
        const QByteArray file = f.readAll();
        f.close();

        // 解码前 30 秒为 float 立体声
        mp3dec_t dec;
        mp3dec_frame_info_t info;
        mp3dec_init(&dec);
        QVector<short> frame(MINIMP3_MAX_SAMPLES_PER_FRAME);
        QVector<float> pcm;
        const uint8_t *ptr = reinterpret_cast<const uint8_t *>(file.constData());
        int remaining = file.size();
        int rate = 0, ch = 0;
        while (remaining > 0) {
            const int samples = mp3dec_decode_frame(&dec, ptr, remaining, frame.data(), &info);
            if (info.frame_bytes <= 0) break;
            ptr += info.frame_bytes; remaining -= info.frame_bytes;
            if (samples <= 0) continue;
            if (info.hz > 0) rate = info.hz;
            if (info.channels > 0) ch = info.channels;
            for (int i = 0; i < samples * info.channels; ++i)
                pcm.append(static_cast<float>(frame[i]) / 32768.0f);
            if (pcm.size() > static_cast<qsizetype>(rate) * 2 * 30) break;   // ~30s
        }
        const bool decodeOk = pcm.size() > 1000 && rate >= 8000 && rate <= 96000 && (ch == 1 || ch == 2);
        printf("[%s] 解码：样本=%lld 采样率=%d 声道=%d\n",
               decodeOk ? "PASS" : "FAIL", (long long)pcm.size(), rate, ch);
        if (!decodeOk) return finishSelfTest(4);

        // mono→stereo 供 AudioEffects
        QVector<float> stereo;
        if (ch == 1) { for (float s : pcm) { stereo.append(s); stereo.append(s); } }
        else stereo = pcm;
        const int frames = stereo.size() / 2;

        auto rms = [](const QVector<float> &x) {
            double acc = 0; for (float v : x) acc += double(v) * v;
            return std::sqrt(acc / qMax(1, x.size()));
        };
        const double rmsRaw = rms(stereo);

        AudioEffects fx;
        fx.setSampleRate(rate);
        fx.setEqEnabled(true);
        fx.setEqPreset(AudioEffects::EqPreset::Rock);
        for (int b = 0; b < AudioEffects::bandFrequencies().size(); ++b)
            fx.setEqGain(b, 10.0);
        fx.setReverbEnabled(true);
        fx.reset();
        QVector<float> processed = stereo;
        for (int off = 0; off < frames; off += 2048)
            fx.process(processed.data() + off * 2, qMin(2048, frames - off));
        const double rmsFx = rms(processed);

        const bool fxChanged = rmsFx > 0 && std::fabs(rmsFx - rmsRaw) > rmsRaw * 0.05;
        printf("[%s] 处理：RMS 原始=%.4f 音效后=%.4f（变化 %.0f%%）\n",
               fxChanged ? "PASS" : "FAIL", rmsRaw, rmsFx,
               rmsRaw > 0 ? std::fabs(rmsFx - rmsRaw) / rmsRaw * 100 : 0);

        // 集成①：EffectPlayer 异步装载（建帧表 + 协商设备格式）。
        //   断言 A：loadAsync 本身几乎不占主线程（旧实现在主线程解码整曲 = 卡 UI）
        //   断言 B：帧表算出的时长 == 逐帧解码的真实时长（验证 Mp3FrameIndex 的帧长计算）
        AudioEffects fx2; fx2.setEqEnabled(true);
        Muyun::EffectPlayer ep(&fx2);
        bool loadDone = false, loadOk = false; QString why;
        QObject::connect(&ep, &Muyun::EffectPlayer::loadFinished,
                         [&](bool ok, const QString &r) { loadDone = true; loadOk = ok; why = r; });
        const qint64 callT0 = QDateTime::currentMSecsSinceEpoch();
        ep.loadAsync(src, 0);
        const qint64 callBlockMs = QDateTime::currentMSecsSinceEpoch() - callT0;
        const qint64 waitT0 = QDateTime::currentMSecsSinceEpoch();
        while (!loadDone && QDateTime::currentMSecsSinceEpoch() - waitT0 < 8000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        const qint64 durMs = ep.duration();

        // 真实时长：整曲逐帧解码数样本（只解码不做 DSP，作为"标准答案"）
        mp3dec_t dec2; mp3dec_init(&dec2);
        mp3dec_frame_info_t info2{};
        QVector<short> fr2(MINIMP3_MAX_SAMPLES_PER_FRAME);
        const uint8_t *q = reinterpret_cast<const uint8_t *>(file.constData());
        int left2 = file.size();
        qint64 realSamples = 0, realRate = 0;
        while (left2 > 0) {
            const int s2 = mp3dec_decode_frame(&dec2, q, left2, fr2.data(), &info2);
            if (info2.frame_bytes <= 0) break;
            q += info2.frame_bytes; left2 -= info2.frame_bytes;
            if (s2 <= 0) continue;
            realSamples += s2;
            if (info2.hz > 0) realRate = info2.hz;
        }
        const qint64 realMs = realRate > 0 ? realSamples * 1000 / realRate : 0;
        const qint64 durErr = std::llabs(realMs - durMs);
        const bool durOk = loadOk && durMs > 1000 && realMs > 0 && durErr <= 120;
        const bool asyncOk = loadOk && callBlockMs <= 30;

        printf("[%s] EffectPlayer 异步装载 ok=%d(%s) 主线程占用=%lldms（应≤30ms）\n",
               (loadOk && asyncOk) ? "PASS" : "FAIL", int(loadOk), qPrintable(why),
               (long long)callBlockMs);
        printf("[%s] 帧表时长=%lldms 逐帧解码时长=%lldms 误差=%lldms（应≤120ms）\n",
               durOk ? "PASS" : "FAIL", (long long)durMs, (long long)realMs, (long long)durErr);
        printf("[INFO] 帧数=%d 源采样率=%d 协商输出=%dHz 设备=%s\n",
               ep.frameCount(), ep.sourceSampleRate(), ep.outputSampleRate(),
               qPrintable(ep.deviceName()));

        // 集成②：音效配置必须"记住用户习惯"（模拟重启：新建一个 AudioEffects 读同一份设置），
        // 且「恢复默认」只归零参数、不动四个开关
        bool keepOk = false, resetOk = false;
        {
            Muyun::AudioEffects a;
            a.setSpatialParams(31.0, 73.0);
            a.setEqGain(0, -5.0);
            a.setLoudnessTarget(-11.0);
            a.setEqEnabled(true);
            Muyun::DocumentStore::instance()->flushAll();

            Muyun::AudioEffects b;      // 等价于"重启后再读一次设置"
            keepOk = qFuzzyCompare(b.spatialRadius(), 31.0)
                     && qFuzzyCompare(b.spatialSpeed(), 73.0)
                     && qFuzzyCompare(b.eqGain(0), -5.0)
                     && qFuzzyCompare(b.loudnessTarget(), -11.0)
                     && b.eqEnabled();
            printf("[%s] 配置持久化（模拟重启再读）：环绕=%g/%g EQ1=%g 响度=%g 开关=%d\n",
                   keepOk ? "PASS" : "FAIL", b.spatialRadius(), b.spatialSpeed(),
                   b.eqGain(0), b.loudnessTarget(), int(b.eqEnabled()));

            a.resetAllParams();
            resetOk = qFuzzyCompare(a.spatialRadius(), 50.0)
                      && qFuzzyCompare(a.spatialSpeed(), 50.0)
                      && qFuzzyCompare(a.eqGain(0), 0.0)
                      && qFuzzyCompare(a.loudnessTarget(), -14.0)
                      && a.eqEnabled();          // 开关必须保持原样（不能被"恢复默认"顺手关掉）
            printf("[%s] 恢复默认：只归零参数（环绕=%g/%g EQ1=%g）且开关仍=%d\n",
                   resetOk ? "PASS" : "FAIL", a.spatialRadius(), a.spatialSpeed(),
                   a.eqGain(0), int(a.eqEnabled()));
            a.setEqEnabled(false);
            Muyun::DocumentStore::instance()->flushAll();
        }

        const bool pass = decodeOk && fxChanged && loadOk && durOk && asyncOk
                          && keepOk && resetOk;
        printf("[%s] 音效管线自检结束\n", pass ? "PASS" : "FAIL");
        return finishSelfTest(pass ? 0 : 4);
    }
#endif // MUYUN_SELFTES

    // 音效管线**真实出声**自检：真播放 + 真开音效，逐项断言"数据确实变了"
    //   1) 确实走的是音效管线（usingEffectPipeline），不是悄悄回退
    //   2) 主线程心跳不被卡（后台解码，UI 不冻结）
    //   3) 位置按墙钟推进；暂停冻结；seek 落点并继续推进
    //   4) 切设备续播、切歌不崩、关音效自动回退普通内核
    //   5) 非 MP3 音源被正确标记为"音效未生效"且仍能播
    // 用法：MuyunMusic.exe --test-effects-live [mp3] [mp3-2]
    //   ⚠ 会写设置文档（EQ 开关等），跑之前请设 MUYUN_STORE_ROOT=<临时目录> 隔离，
    //     否则污染用户设置；与其他自检**不要共用**同一个隔离目录（会互相污染，实测假失败）
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-effects-live"))) {
        if (qEnvironmentVariableIsEmpty("MUYUN_STORE_ROOT")) {
            printf("[FAIL] 请先设 MUYUN_STORE_ROOT=<临时目录> 再跑（本自检会写音效设置）\n");
            return finishSelfTest(4);
        }
        setvbuf(stdout, nullptr, _IONBF, 0);
        printf("=== 音效管线真实出声自检 ===\n");
        const int li = args.indexOf(QStringLiteral("--test-effects-live"));
        QString src1, src2;
        if (li + 1 < args.size() && !args.at(li + 1).startsWith(QStringLiteral("--")))
            src1 = args.at(li + 1);
        if (li + 2 < args.size() && !args.at(li + 2).startsWith(QStringLiteral("--")))
            src2 = args.at(li + 2);

        // 自动找测试素材：优先缓存目录里的真 mp3（在线播放落盘就是它），其次用户曲库
        auto sniff = [](const QString &p, const char *magic) {
            QFile f(p);
            if (!f.open(QIODevice::ReadOnly)) return false;
            const QByteArray h = f.read(4);
            f.close();
            return h.startsWith(magic);
        };
        auto findAny = [](const QStringList &exts, bool wantMp3) {
            QStringList dirs;
            const QVariantMap doc = Muyun::DocumentStore::instance()->readAll(QStringLiteral("local-music"));
            dirs << doc.value(QStringLiteral("folders")).toStringList()
                 << QStringLiteral("E:/Music") << QDir::tempPath();
            for (const QString &d : dirs) {
                if (d.isEmpty() || !QDir(d).exists()) continue;
                QDirIterator it(d, exts, QDir::Files, QDirIterator::Subdirectories);
                while (it.hasNext()) {
                    const QString p = it.next();
                    if (p.size() < 4) continue;
                    const QFileInfo fi(p);
                    if (fi.size() < 200000) continue;
                    if (wantMp3) {
                        // .audio 缓存无扩展名：认 ID3 或 MPEG 同步头
                        QFile f(p);
                        if (!f.open(QIODevice::ReadOnly)) continue;
                        const QByteArray h = f.read(3);
                        f.close();
                        const bool id3 = h.startsWith("ID3");
                        if (!id3 && fi.suffix().compare(QStringLiteral("mp3"), Qt::CaseInsensitive) != 0)
                            continue;
                    }
                    return p;
                }
            }
            return QString();
        };
        if (src1.isEmpty()) {
            // 缓存里的 .audio 直接当素材（多数是 mp3），逐个嗅探
            QDir d(QDir::tempPath() + QStringLiteral("/muyun-audio"));
            const auto files = d.entryInfoList(QDir::Files, QDir::Time);
            for (const QFileInfo &fi : files) {
                if (fi.size() < 300000) continue;
                if (sniff(fi.absoluteFilePath(), "ID3")) { src1 = fi.absoluteFilePath(); break; }
            }
        }
        if (src1.isEmpty()) src1 = findAny({QStringLiteral("*.mp3")}, true);
        if (src2.isEmpty()) {
            QDir d(QDir::tempPath() + QStringLiteral("/muyun-audio"));
            const auto files = d.entryInfoList(QDir::Files, QDir::Time);
            for (const QFileInfo &fi : files) {
                if (fi.size() < 300000) continue;
                const QString p = fi.absoluteFilePath();
                if (p == src1) continue;
                if (sniff(p, "ID3")) { src2 = p; break; }
            }
        }
        QString flac;
        {
            QDir d(QDir::tempPath() + QStringLiteral("/muyun-audio"));
            for (const QFileInfo &fi : d.entryInfoList(QDir::Files)) {
                if (fi.size() < 300000) continue;
                if (sniff(fi.absoluteFilePath(), "fLaC")) { flac = fi.absoluteFilePath(); break; }
            }
        }
        if (src1.isEmpty() || src2.isEmpty()) {
            printf("[SKIP] 找不到两个可用 mp3 素材（src1=%s src2=%s）\n",
                   qPrintable(src1), qPrintable(src2));
            return finishSelfTest(0);
        }
        printf("[INFO] 素材1=%s\n[INFO] 素材2=%s\n[INFO] 非mp3=%s\n",
               qPrintable(src1), qPrintable(src2), qPrintable(flac.isEmpty() ? QStringLiteral("(无)") : flac));

        auto *eng = new Muyun::PlayerEngine(qApp);
        // 真开音效：EQ（摇滚）+ 混响，二者都会显著改变波形
        eng->effects()->setEqEnabled(true);
        eng->effects()->setEqPreset(Muyun::AudioEffects::EqPreset::Rock);
        eng->effects()->setReverbEnabled(true);

        // 主线程心跳：20ms 定时器记录最大间隔；后台解码若跑在主线程上会被拉大
        int heartbeats = 0;
        QElapsedTimer hbT; hbT.start();
        qint64 hbMax = 0, hbLast = 0;
        QTimer hb; hb.setInterval(20);
        QObject::connect(&hb, &QTimer::timeout, [&]() {
            const qint64 now = hbT.elapsed();
            const qint64 gap = now - hbLast;
            hbLast = now;
            if (heartbeats > 2 && gap > hbMax) hbMax = gap;   // 头几次含启动开销，跳过
            ++heartbeats;
        });
        hb.start();

        auto wait = [](int ms) {
            const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
            while (QDateTime::currentMSecsSinceEpoch() - t0 < ms)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        };
        auto waitState = [&](Muyun::PlayerEngine::State s, int timeoutMs) {
            const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
            while (eng->state() != s && QDateTime::currentMSecsSinceEpoch() - t0 < timeoutMs)
                wait(30);
            return eng->state() == s;
        };

        bool all = true;
        // 注意：捕获型 lambda 不能带可变参数，这里用宏
#define CK(ok, ...) do { printf("[%s] ", (ok) ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!(ok)) all = false; } while (0)

        // ---- 1) 开音效播 MP3：必须真的走音效管线 ----
        const qint64 playT0 = QDateTime::currentMSecsSinceEpoch();
        eng->playFile(src1);
        const qint64 blockMs = QDateTime::currentMSecsSinceEpoch() - playT0;
        // 首次接触 Qt 多媒体后端要一次性初始化（实测约 1.1s，普通播放路径同样要付），
        // 这里只拦"明显超量"的阻塞；真正证明"解码不在主线程"的是第二次播放（见第 7 步）
        CK(blockMs <= 1300, "首次 playFile 主线程占用 %lldms（含后端一次性初始化，应≤1300ms）", (long long)blockMs);
        const bool toPlaying = waitState(Muyun::PlayerEngine::State::Playing, 6000);
        CK(toPlaying, "开音效后进入 Playing（state=%d）", int(eng->state()));
        CK(eng->usingEffectPipeline(), "确实走音效管线（不是偷偷回退普通内核）");
        CK(eng->duration() > 1000, "时长=%lldms", (long long)eng->duration());

        // ---- 2) 位置推进 ----
        const qint64 p0 = eng->position();
        wait(2000);
        const qint64 p1 = eng->position();
        const qint64 adv = p1 - p0;
        CK(adv >= 1200 && adv <= 2900, "2 秒内位置推进 %lldms（应≈2000ms）", (long long)adv);
        CK(hbMax <= 200, "主线程心跳最大间隔 %lldms（应≤200ms，UI 不卡）", (long long)hbMax);

        // ---- 3) 暂停：位置必须冻结 ----
        eng->pause();
        wait(200);
        const qint64 pa0 = eng->position();
        wait(1000);
        const qint64 pa1 = eng->position();
        CK(std::llabs(pa1 - pa0) <= 120, "暂停 1 秒位置冻结 %lld→%lldms", (long long)pa0, (long long)pa1);
        eng->resume();
        wait(1200);
        const qint64 pa2 = eng->position();
        CK(pa2 > pa1 + 600, "恢复后继续推进 →%lldms", (long long)pa2);

        // ---- 4) seek 落点 + 继续推进 ----
        const qint64 dur = eng->duration();
        const qint64 target = dur * 3 / 5;
        eng->seek(target);
        wait(300);
        const qint64 s0 = eng->position();
        CK(std::llabs(s0 - target) <= 1500, "seek 到 %lldms 落在 %lldms（容差1.5s）",
              (long long)target, (long long)s0);
        wait(1500);
        const qint64 s1 = eng->position();
        CK(s1 > s0 + 800, "seek 后继续推进 →%lldms", (long long)s1);
        // 往回 seek（考验已解码区外的重新定位）
        eng->seek(2000);
        wait(1200);
        const qint64 s2 = eng->position();
        CK(s2 > 2000 && s2 < 6000, "回退 seek 到 2s 后=%lldms", (long long)s2);

        // ---- 5) 改参数即时生效（不重载、不中断） ----
        const qint64 e0 = eng->position();
        eng->effects()->setEqPreset(Muyun::AudioEffects::EqPreset::Classical);
        eng->effects()->setEqGain(0, 6.0);
        eng->effects()->setLoudnessEnabled(true);
        // 顺带打开 3D 环绕：它会改左右增益并引入分数延迟，最容易在管线上暴露
        // "改参数就断流/爆音"一类问题，所以真实出声路径也要过一遍
        eng->effects()->setSpatialEnabled(true);
        eng->effects()->setSpatialParams(70.0, 60.0);
        // 混响也开（four-57：用户报"环境混响失效"，而这条腿此前从没被真实出声验证过——
        // 混响是唯一带内部状态（梳状延迟线）的效果，开/关瞬间最容易出幺蛾子）
        eng->effects()->setReverbEnabled(true);
        eng->effects()->setReverbPreset(Muyun::AudioEffects::ReverbPreset::Hall);
        eng->reevaluateEffects();     // 已在管线上：应当什么都不重启
        wait(1000);
        const qint64 e1 = eng->position();
        CK(eng->usingEffectPipeline() && e1 > e0 + 500,
              "播放中改 EQ/响度/环绕/混响不中断（%lld→%lldms，仍在音效管线）", (long long)e0, (long long)e1);
        eng->effects()->setSpatialEnabled(false);
        eng->effects()->setReverbEnabled(false);

        // ---- 6) 切设备续播 ----
        const qint64 d0 = eng->position();
        eng->reattachToCurrentDevice();
        wait(1200);
        const qint64 d1 = eng->position();
        CK(eng->usingEffectPipeline() && d1 >= d0 - 600 && d1 <= d0 + 2500,
              "切设备后续播 %lld→%lldms（音效管线保持）", (long long)d0, (long long)d1);

        // ---- 7) 切歌（另一首 mp3，仍开音效）----
        const qint64 playT1 = QDateTime::currentMSecsSinceEpoch();
        eng->playFile(src2);
        const qint64 blockMs1 = QDateTime::currentMSecsSinceEpoch() - playT1;
        CK(blockMs1 <= 100, "第二次 playFile 主线程占用 %lldms（应≤100ms = 解码/DSP 都不在主线程）",
              (long long)blockMs1);
        const bool c1 = waitState(Muyun::PlayerEngine::State::Playing, 6000);
        wait(1500);
        CK(c1 && eng->usingEffectPipeline() && eng->position() > 500,
              "切第二首仍走音效管线 position=%lldms dur=%lldms",
              (long long)eng->position(), (long long)eng->duration());
        printf("[INFO] 音效管线：源%dHz→输出%dHz 设备=%s 欠载补静音=%lldms（应≈0）\n",
               eng->effectSourceRate(), eng->effectOutputRate(),
               qPrintable(eng->audioDeviceName()), (long long)eng->effectStarvedMs());

        // ---- 8) 关音效自动回退普通内核，且不中断 ----
        eng->effects()->setEqEnabled(false);
        eng->effects()->setReverbEnabled(false);
        eng->effects()->setSpatialEnabled(false);
        eng->effects()->setLoudnessEnabled(false);
        eng->reevaluateEffects();
        CK(!eng->usingEffectPipeline(), "关音效后回到普通内核");
        const qint64 f0 = eng->position();
        wait(2000);
        const qint64 f1 = eng->position();
        CK(f1 > f0 + 1000, "回退普通内核后续播 %lld→%lldms", (long long)f0, (long long)f1);

        // ---- 9) FLAC 音源：自四-51 起必须也走音效管线（FFmpeg 后端），不再是旁路 ----
        if (!flac.isEmpty()) {
            eng->effects()->setEqEnabled(true);
            eng->playFile(flac);
            const bool g1 = waitState(Muyun::PlayerEngine::State::Playing, 8000);
            wait(1200);
            CK(g1 && eng->usingEffectPipeline() && eng->position() > 300,
                  "flac 也走音效管线：后端=%s bypassed=%d position=%lldms dur=%lldms",
                  qPrintable(eng->effectBackend()), int(eng->effectsBypassed()),
                  (long long)eng->position(), (long long)eng->duration());
            eng->effects()->setEqEnabled(false);
        } else {
            printf("[SKIP] 没有 flac 素材，跳过 FLAC 分支\n");
        }

        // ---- 9b) 伪音频（有 ID3 头但没有效帧）：必须自动回退普通内核，且不崩 ----
        {
            const QString bogus = QDir::tempPath() + QStringLiteral("/muyun_fx_bogus.audio");
            QFile bf(bogus);
            if (bf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                QByteArray junk = QByteArrayLiteral("ID3\x03\x00\x00\x00\x00\x00\x00");
                junk.append(QByteArray(256 * 1024, '\x7F'));      // 全是无效帧数据
                bf.write(junk);
                bf.close();
                eng->effects()->setEqEnabled(true);
                eng->playFile(bogus);
                wait(2500);
                CK(!eng->usingEffectPipeline() && eng->effectsBypassed(),
                   "坏文件自动回退普通内核：live=%d bypassed=%d（不能卡死也不能哑）",
                   int(eng->usingEffectPipeline()), int(eng->effectsBypassed()));
                eng->effects()->setEqEnabled(false);
                eng->stop();
                QFile::remove(bogus);
            }
        }

        // ---- 10) 快速连发指令不崩（暂停/恢复/seek/停止交叉） ----
        eng->playFile(src1);
        wait(900);
        for (int i = 0; i < 6; ++i) {
            eng->seek(3000 + i * 4000);
            eng->pause();
            eng->resume();
            wait(120);
        }
        eng->stop();
        wait(400);
        CK(eng->state() == Muyun::PlayerEngine::State::Stopped, "交叉指令后正常停止");
        // 说明：音效播放阶段的严格检查在前面（心跳 ≤200ms，实测 21ms）。
        // 这里的全局最大值必然包含"回退普通内核"时的一次性开销（ensurePlayer/ensureAudioOutput
        // 建 FFmpeg + 开 WASAPI 会话，实测 150~250ms），那是普通路径固有成本、与音效管线无关，
        // 所以阈值放宽到 400ms，别把它当成音效管线的卡顿。
        CK(hbMax <= 400, "全程主线程最大心跳间隔 %lldms（含一次性内核初始化，应≤400ms）", (long long)hbMax);

        eng->stop();
        delete eng;
#undef CK
        printf("[%s] 音效真实出声自检结束\n", all ? "PASS" : "FAIL");
        return finishSelfTest(all ? 0 : 4);
    }
#endif // MUYUN_SELFTES

    // 音效"全格式"自检：每种能找到的格式都要求**真的走音效管线**，
    // 并把时长与 QMediaPlayer 交叉比对（证明 FFmpeg 后端读的是同一个真相）。
    // 用法：MuyunMusic.exe --test-effects-formats [素材目录]
    //   ⚠ 必须 MUYUN_STORE_ROOT 隔离（会写音效设置）
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-effects-formats"))) {
        if (qEnvironmentVariableIsEmpty("MUYUN_STORE_ROOT")) {
            printf("[FAIL] 请先设 MUYUN_STORE_ROOT=<临时目录> 再跑（本自检会写音效设置）\n");
            return finishSelfTest(4);
        }
        setvbuf(stdout, nullptr, _IONBF, 0);
        printf("=== 音效全格式自检 ===\n");
        printf("[INFO] 万能后端：%s%s\n", pcmHasUniversalBackend() ? "可用 " : "不可用 ",
               qPrintable(pcmHasUniversalBackend() ? pcmUniversalBackendInfo()
                                                   : pcmUniversalBackendError()));

        // 素材来源：命令行目录 → 播放缓存 → 用户曲库
        const int fi = args.indexOf(QStringLiteral("--test-effects-formats"));
        QStringList roots;
        if (fi + 1 < args.size() && !args.at(fi + 1).startsWith(QStringLiteral("--")))
            roots << args.at(fi + 1);
        roots << PlayerEngine::audioCacheDir() << QStringLiteral("E:/Music");

        // 按魔数认格式（在线音频落盘都是无扩展名的 .audio，绝不能看后缀）
        auto kindOf = [](const QString &p) -> QString {
            QFile f(p);
            if (!f.open(QIODevice::ReadOnly)) return QString();
            const QByteArray h = f.read(12);
            f.close();
            if (h.size() < 4) return QString();
            if (h.startsWith("ID3")) return QStringLiteral("mp3");
            if (h.startsWith("fLaC")) return QStringLiteral("flac");
            if (h.startsWith("OggS")) return QStringLiteral("ogg");
            if (h.startsWith("RIFF")) return QStringLiteral("wav");
            if (h.size() >= 12 && h.mid(4, 4) == "ftyp") return QStringLiteral("m4a");
            if (static_cast<quint8>(h[0]) == 0xFF && (static_cast<quint8>(h[1]) & 0xE0) == 0xE0)
                return QStringLiteral("mp3");
            return QString();
        };
        // 每种格式最多收 3 个候选（从大到小）：缓存里常有"头部时长虚高/被截断"的坏文件，
        // 多候选才能把"文件问题"和"我们的解码/seek 问题"分开
        QHash<QString, QStringList> pick;
        for (const QString &r : roots) {
            QDir d(r);
            if (!d.exists()) continue;
            QDirIterator it(r, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString p = it.next();
                if (QFileInfo(p).size() < 200000) continue;
                const QString k = kindOf(p);
                if (k.isEmpty()) continue;
                QStringList &lst = pick[k];
                if (!lst.contains(p) && lst.size() < 3) lst << p;
            }
        }
        for (auto it = pick.begin(); it != pick.end(); ++it) {
            QStringList &lst = it.value();
            std::sort(lst.begin(), lst.end(), [](const QString &a, const QString &b) {
                return QFileInfo(a).size() > QFileInfo(b).size();
            });
        }
        if (pick.isEmpty()) { printf("[SKIP] 一个音频素材都没找到\n"); return finishSelfTest(0); }

        // QMediaPlayer 参照时长（证明我们解出来的时长不是自说自话）
        auto refDurationMs = [](const QString &p) {
            QMediaPlayer mp;
            QAudioOutput ao;
            mp.setAudioOutput(&ao);
            mp.setSource(QUrl::fromLocalFile(p));
            const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
            while (mp.duration() <= 0 && QDateTime::currentMSecsSinceEpoch() - t0 < 6000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            const qint64 d = mp.duration();
            mp.stop();
            return d;
        };

        auto *eng = new Muyun::PlayerEngine(qApp);
        eng->effects()->setEqEnabled(true);
        eng->effects()->setEqPreset(Muyun::AudioEffects::EqPreset::Rock);

        // 注意：processEvents 的超时参数是"最多处理这么久"，没事件就立刻返回，
        // 不能当 sleep 用（第一版拿它当 1.5 秒等待，结果"推进=0ms"是假失败）
        auto waitMs = [](int ms) {
            const qint64 end = QDateTime::currentMSecsSinceEpoch() + ms;
            while (QDateTime::currentMSecsSinceEpoch() < end)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        };

        // 一次试验：返回该候选的逐项结果
        struct Trial {
            bool playing = false, live = false, durOk = false, advOk = false, frozen = false;
            bool seekOk = false, truncated = false;
            QString backend; qint64 dur = 0, ref = 0, adv = 0, seekTo = 0, landed = 0, after = 0;
            qint64 starved = 0;
            bool coreOk() const { return playing && live && durOk && advOk && frozen; }
            bool fullOk() const { return coreOk() && seekOk; }
        };
        auto trial = [&](const QString &file) {
            Trial t;
            eng->stop();
            waitMs(120);
            eng->playFile(file);
            const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
            while (eng->state() != Muyun::PlayerEngine::State::Playing
                   && QDateTime::currentMSecsSinceEpoch() - t0 < 8000)
                waitMs(50);
            t.playing = eng->state() == Muyun::PlayerEngine::State::Playing;
            t.live = eng->usingEffectPipeline();
            t.backend = eng->effectBackend();
            t.dur = eng->duration();
            const qint64 pos0 = eng->position();
            waitMs(1600);
            t.adv = eng->position() - pos0;
            t.advOk = t.adv >= 1000;
            eng->pause();
            waitMs(400);
            const qint64 p0 = eng->position();
            waitMs(500);
            t.frozen = std::llabs(eng->position() - p0) <= 250;
            eng->resume();
            t.seekTo = t.dur > 0 ? t.dur * 2 / 5 : 5000;
            eng->seek(t.seekTo);
            waitMs(600);
            t.landed = eng->position();
            waitMs(1200);
            t.after = eng->position();
            // seek 到"没有数据"的区域时管线会自然收尾（state→Stopped）：
            // 那是文件本身被截断/头部时长虚高，不是管线故障
            t.truncated = eng->state() == Muyun::PlayerEngine::State::Stopped
                          || eng->effectStarvedMs() > 300;
            t.seekOk = !t.truncated && std::llabs(t.landed - t.seekTo) <= 2000
                       && t.after > t.landed + 500;
            t.ref = refDurationMs(file);
            t.durOk = t.dur > 1000 && t.ref > 1000 && std::llabs(t.dur - t.ref) <= 2000;
            t.starved = eng->effectStarvedMs();
            return t;
        };

        bool all = true;
        int tried = 0, okCount = 0;
        const QStringList order = { QStringLiteral("mp3"), QStringLiteral("flac"),
                                    QStringLiteral("m4a"), QStringLiteral("ogg"),
                                    QStringLiteral("wav") };
        for (const QString &k : order) {
            if (!pick.contains(k)) { printf("[SKIP] 没有 %s 素材\n", qPrintable(k)); continue; }
            ++tried;
            printf("\n--- %s ---\n", qPrintable(k.toUpper()));
            bool kindOk = false;
            for (const QString &file : pick.value(k)) {
                const Trial t = trial(file);
                printf("    %s %s live=%d 后端=%s 时长=%lldms(参照%lldms 差%lldms) 推进=%lldms "
                       "暂停冻结=%d seek %lld→%lld→%lldms 欠载=%lldms  %s\n",
                       qPrintable(QFileInfo(file).fileName()),
                       t.coreOk() ? "core=OK " : "core=BAD", int(t.live), qPrintable(t.backend),
                       (long long)t.dur, (long long)t.ref, (long long)std::llabs(t.dur - t.ref),
                       (long long)t.adv, int(t.frozen), (long long)t.seekTo, (long long)t.landed,
                       (long long)t.after, (long long)t.starved,
                       t.fullOk() ? "→ PASS" : (t.truncated ? "→ 文件截断（换候选）" : "→ FAIL"));
                if (t.fullOk()) { kindOk = true; break; }
                if (t.coreOk() && t.truncated) continue;      // 核心能跑，只是这个文件坏/截断
                if (t.coreOk() && !t.truncated && !t.seekOk) { kindOk = false; }
            }
            if (kindOk) ++okCount; else all = false;
            printf("[%s] %s 音效可用\n", kindOk ? "PASS" : "FAIL", qPrintable(k));
        }
        printf("\n[%s] 全格式自检：%d 种格式尝试，%d 种通过\n", all ? "PASS" : "FAIL", tried, okCount);
        if (tried < 2) printf("[NOTE] 只找到 1 种格式素材，覆盖面有限（多播几首歌攒够缓存再跑）\n");
        eng->effects()->setEqEnabled(false);
        eng->stop();
        delete eng;
        return finishSelfTest(all ? 0 : 4);
    }
#endif // MUYUN_SELFTES

    // 歌词译文配对自检：纯数据、离线、不写设置。
    // 复现并锁死"桌面歌词有时只显示原文"的三种成因（旧算法：全局找最近 + 已填就丢弃 + 容差 0.5s）
    // 用法：MuyunMusic.exe --test-lyric-merge
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-lyric-merge"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        printf("=== 歌词译文配对自检 ===\n");
        bool all = true;
#define LK(ok, ...) do { printf("[%s] ", (ok) ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!(ok)) all = false; } while (0)

        // A) 行数相同、译文时间轴整体偏移 1.2s（在线歌词极常见）
        //    旧算法容差 0.5s → 一条都配不上 → 桌面歌词永远没译文
        {
            auto *ly = new Muyun::SongLyric(Muyun::LyricParser::parseLrc(
                "[00:10.00]第一行\n[00:20.00]第二行\n[00:30.00]第三行"));
            Muyun::LyricParser::mergeTranslation(*ly,
                "[00:11.20]line one\n[00:21.30]line two\n[00:30.90]line three", false);
            int got = 0;
            for (const auto &l : ly->lines) if (!l.translation.isEmpty()) ++got;
            LK(got == 3 && ly->hasTranslation, "A 时间轴整体偏移：3 行全配上译文 = %d/3", got);
            LK(ly->lines.at(1).translation == QStringLiteral("line two"),
               "A 配对内容正确（第二行=%s）", qPrintable(ly->lines.at(1).translation));
            delete ly;
        }

        // B) 行数不同（译文多一行）：旧算法会"抢已填的行"导致中间出现空洞
        {
            auto *ly = new Muyun::SongLyric(Muyun::LyricParser::parseLrc(
                "[00:05.00]a1\n[00:05.05]a2\n[00:15.00]a3\n[00:25.00]a4"));
            Muyun::LyricParser::mergeTranslation(*ly,
                "[00:05.02]t1\n[00:05.06]t2\n[00:14.80]t3\n[00:40.00]多余行", false);
            int holes = 0;
            for (const auto &l : ly->lines) if (l.translation.isEmpty()) ++holes;
            LK(holes == 0, "B 行数不等 + 时间相近：主歌词 4 行无空洞（空洞=%d）", holes);
            LK(ly->lines.at(2).translation == QStringLiteral("t3"),
               "B 顺序单调不乱配（第三行=%s）", qPrintable(ly->lines.at(2).translation));
            delete ly;
        }

        // C) 时间差太远（>3s）不能硬凑
        {
            auto *ly = new Muyun::SongLyric(Muyun::LyricParser::parseLrc(
                "[00:10.00]only\n[00:60.00]other"));
            Muyun::LyricParser::mergeTranslation(*ly, "[00:40.00]far-away", false);
            bool untouched = true;
            for (const auto &l : ly->lines) if (!l.translation.isEmpty()) untouched = false;
            LK(untouched && !ly->hasTranslation, "C 相差 30s 的孤立译文不乱贴（保持空）");
            delete ly;
        }

        // D) 罗马音与译文互不干扰（同一行两种附属文本都要在）
        {
            auto *ly = new Muyun::SongLyric(Muyun::LyricParser::parseLrc(
                "[00:10.00]言叶も\n[00:20.00]消えてく"));
            Muyun::LyricParser::mergeTranslation(*ly, "[00:10.40]words fade", false);
            Muyun::LyricParser::mergeTranslation(*ly, "[00:10.60]kotoba mo", true);
            LK(ly->lines.at(0).translation == QStringLiteral("words fade")
               && ly->lines.at(0).roman == QStringLiteral("kotoba mo")
               && ly->hasTranslation && ly->hasRoman,
               "D 译文与罗马音共存（%s / %s）",
               qPrintable(ly->lines.at(0).translation), qPrintable(ly->lines.at(0).roman));
            delete ly;
        }
        // E) 桌面歌词窗高必须容得下"原文 + 译文"两行（写死 100px 时大字号会把译文裁掉）
        {
            Muyun::DesktopLyricsController dl(nullptr, nullptr);   // 只查几何，不建窗
            int prev = 0;
            bool grow = true, fits = true;
            for (int px : {22, 28, 36, 46}) {
                dl.setSizePx(px);
                const int h = dl.neededHeight();
                const double need = px * 1.30 + qMax(11.0, px * 0.55) * 1.30 + 4 + 24;
                if (h < need) fits = false;          // 容不下两行
                if (h < prev) grow = false;          // 字号变大窗高不能缩（小字号有 100px 下限，允许持平）
                prev = h;
                printf("     字号 %d → 窗高 %dpx（两行需要 %.0fpx）%s\n", px, h, need,
                       h >= need ? "" : "  ← 会被裁掉！");
            }
            LK(fits && grow, "E 桌面歌词窗高容得下原文+译文（各字号都不裁剪、且不随字号缩小）");
            dl.setSizePx(28);
        }
#undef LK
        printf("[%s] 歌词译文配对自检结束\n", all ? "PASS" : "FAIL");
        return finishSelfTest(all ? 0 : 4);
    }
#endif // MUYUN_SELFTES

    // 局域网同步自检：导入→导出→起 HTTP 服务→回环 GET/POST→校验（全程可自动验证）
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-sync"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        printf("=== 局域网同步自检 ===\n");
        QDir(QDir::tempPath() + QStringLiteral("/muyun-synctest"))
            .removeRecursively();   // 清空隔离根目录，保证从空库开始
        auto *lib = new LibraryController(qApp);
        auto *sc = new SyncController(lib, qApp);
        sc->setPort(8791);   // 测试端口，避开常用

        auto mkSong = [](const QString &id, const QString &nm) {
            Song s; s.id = id; s.name = nm; s.artist = QStringLiteral("测试歌手");
            s.platform = Platform::Netease; s.hasLx = true; s.lx.source = QStringLiteral("wy");
            return s.toMap();
        };
        // 1) 导入两条在线收藏
        QVariantMap seed;
        seed[QStringLiteral("favorites")] = QVariantList{ mkSong("t1", QStringLiteral("同步曲一")),
                                                          mkSong("t2", QStringLiteral("同步曲二")) };
        const int seedAdded = lib->importOnlineLibrary(seed);
        printf("[%s] 导入 2 条：added=%d\n", seedAdded == 2 ? "PASS" : "FAIL", seedAdded);

        // 2) 导出应含 2 条
        const QVariantMap exp = lib->exportOnlineLibrary();
        const int expFav = exp.value(QStringLiteral("favorites")).toList().size();
        printf("[%s] 导出 favorites=%d\n", expFav == 2 ? "PASS" : "FAIL", expFav);

        // 3) 起服务 + 回环 GET /api/export
        sc->setEnabled(true);
        const bool listening = sc->enabled();
        printf("[%s] 服务监听：%s\n", listening ? "PASS" : "FAIL", qPrintable(sc->shareUrl()));

        QNetworkAccessManager nam;
        QEventLoop loop;
        bool getOk = false;
        {
            QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:8791/api/export")));
            auto *reply = nam.get(req);
            QObject::connect(reply, &QNetworkReply::finished, &loop, [&]() {
                const QJsonDocument d = QJsonDocument::fromJson(reply->readAll());
                getOk = d.isObject()
                        && d.object().value(QStringLiteral("favorites")).toArray().size() == 2;
                reply->deleteLater();
                loop.quit();
            });
            QTimer::singleShot(5000, &loop, &QEventLoop::quit);
            loop.exec();
        }
        printf("[%s] 回环 GET /api/export → 2 条\n", getOk ? "PASS" : "FAIL");

        // 4) 回环 POST /api/import：t1(重复)+t3(新) → 应新增 1
        bool postOk = false;
        {
            QVariantMap payload;
            payload[QStringLiteral("favorites")] = QVariantList{ mkSong("t1", QStringLiteral("同步曲一")),
                                                                 mkSong("t3", QStringLiteral("同步曲三")) };
            QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:8791/api/import")));
            req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
            auto *reply = nam.post(req, QJsonDocument::fromVariant(payload).toJson(QJsonDocument::Compact));
            QObject::connect(reply, &QNetworkReply::finished, &loop, [&]() {
                const QJsonDocument d = QJsonDocument::fromJson(reply->readAll());
                postOk = d.object().value(QStringLiteral("added")).toInt() == 1;
                reply->deleteLater();
                loop.quit();
            });
            QTimer::singleShot(5000, &loop, &QEventLoop::quit);
            loop.exec();
        }
        printf("[%s] 回环 POST /api/import → added=1\n", postOk ? "PASS" : "FAIL");
        const int finalFav = lib->exportOnlineLibrary().value(QStringLiteral("favorites")).toList().size();
        printf("[%s] 合并去重后 favorites=%d（期望 3）\n", finalFav == 3 ? "PASS" : "FAIL", finalFav);

        sc->setEnabled(false);
        const bool pass = seedAdded == 2 && expFav == 2 && listening && getOk && postOk && finalFav == 3;
        printf("[%s] 局域网同步自检结束\n", pass ? "PASS" : "FAIL");
        return finishSelfTest(pass ? 0 : 4);
    }
#endif // MUYUN_SELFTES

    // 洛雪移动版同步协议自检：mock 手机端走 /hello→/id→/ah(RSA)→WS→message2call→快照合并全链路
    // 用法：MuyunMusic.exe --test-lxsync
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-lxsync"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        printf("=== 洛雪同步协议自检 ===\n");
        QDir(QDir::tempPath() + QStringLiteral("/muyun-lxsynctest")).removeRecursively();
        auto *lib = new Muyun::LibraryController(qApp);
        auto *sc = new Muyun::SyncController(lib, qApp);
        sc->setPort(8795);
        sc->setEnabled(true);
        auto *lx = sc->lxServer();

        // ---- 播种电脑侧库：2 收藏 + 1 歌单 ----
        auto mkSong = [](const QString &id, const QString &nm) {
            Muyun::Song s; s.id = id; s.name = nm; s.artist = QStringLiteral("电脑歌手");
            s.platform = Muyun::Platform::Netease; s.duration = 240;
            s.hasLx = true; s.lx.source = QStringLiteral("wy");
            s.lx.songmid = id; s.lx.interval = QStringLiteral("4:00");
            return s.toMap();
        };
        QVariantMap seed;
        seed[QStringLiteral("favorites")] = QVariantList{ mkSong("101001", QStringLiteral("电脑收藏A")),
                                                          mkSong("101002", QStringLiteral("电脑收藏B")) };
        lib->importOnlineLibrary(seed);
        const QString plId = lib->createPlaylist(QStringLiteral("电脑歌单"));
        lib->addToPlaylist(plId, mkSong("101003", QStringLiteral("电脑歌单曲")));

        auto mkLxMusic = [](const QString &songmid, const QString &nm) {
            QJsonObject meta;
            meta[QStringLiteral("songId")] = songmid;
            meta[QStringLiteral("albumName")] = QString();
            meta[QStringLiteral("picUrl")] = QString();
            QJsonObject o;
            o[QStringLiteral("id")] = QStringLiteral("wy_") + songmid;
            o[QStringLiteral("name")] = nm;
            o[QStringLiteral("singer")] = QStringLiteral("手机歌手");
            o[QStringLiteral("source")] = QStringLiteral("wy");
            o[QStringLiteral("interval")] = QStringLiteral("3:33");
            o[QStringLiteral("meta")] = meta;
            return o;
        };
        // ---- 手机侧库：1 收藏 + 1 歌单 ----
        QJsonObject phoneList;
        phoneList[QStringLiteral("defaultList")] = QJsonArray();
        phoneList[QStringLiteral("loveList")] = QJsonArray{ mkLxMusic("202001", QStringLiteral("手机收藏X")) };
        QJsonObject phonePl;
        phonePl[QStringLiteral("id")] = QStringLiteral("phone-pl-1");
        phonePl[QStringLiteral("name")] = QStringLiteral("手机歌单");
        phonePl[QStringLiteral("source")] = QString();
        phonePl[QStringLiteral("sourceListId")] = QString();
        phonePl[QStringLiteral("locationUpdateTime")] = 0;
        phonePl[QStringLiteral("list")] = QJsonArray{ mkLxMusic("202002", QStringLiteral("手机歌单曲Y")) };
        phoneList[QStringLiteral("userList")] = QJsonArray{ phonePl };

        auto *mock = new Muyun::LxMockClient(qApp);
        mock->setPhoneLibrary(phoneList);
        int modeRequests = 0;
        QObject::connect(lx, &Muyun::LxSyncServer::modeDialogRequested, qApp,
                         [&modeRequests, lx](const QString &dev) {
            ++modeRequests;
            printf("[INFO] 同步模式请求：%s → 自动选 merge_remote_local\n", qPrintable(dev));
            QTimer::singleShot(150, [lx]() { lx->answerSyncMode(QStringLiteral("merge_remote_local")); });
        });
        QObject::connect(lx, &Muyun::LxSyncServer::syncEvent, qApp,
                         [](const QString &t) { printf("[server] %s\n", qPrintable(t)); });
        QObject::connect(mock, &Muyun::LxMockClient::logLine, qApp,
                         [](const QString &t) { printf("[mock]   %s\n", qPrintable(t)); });
        QObject::connect(mock, &Muyun::LxMockClient::done, qApp,
                         [](bool pass, const QString &d) {
            printf("[mock]   %s %s\n", pass ? "done✓" : "done✗", qPrintable(d));
        });

        // 条件等待小工具
        auto waitUntil = [](const std::function<bool()> &cond, int ms) {
            QEventLoop loop; QElapsedTimer et; et.start();
            QTimer poll;
            QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
                if (cond() || et.elapsed() > ms) loop.quit();
            });
            poll.start(50);
            loop.exec();
            return cond();
        };
        auto pcHasFav = [lib](const QString &id) {
            const QVariantList favs = lib->favorites();
            for (const QVariant &v : favs)
                if (v.toMap().value(QStringLiteral("id")).toString() == id
                    && v.toMap().value(QStringLiteral("platform")).toString() == QLatin1String("netease"))
                    return true;
            return false;
        };
        auto phoneLoveHas = [mock](const QString &musicId) {
            const QJsonArray love = mock->phoneLibrary().value(QStringLiteral("loveList")).toArray();
            for (const QJsonValue &v : love)
                if (v.toObject().value(QStringLiteral("id")).toString() == musicId) return true;
            return false;
        };
        auto phoneHasPlaylist = [mock](const QString &name) {
            const QJsonArray ul = mock->phoneLibrary().value(QStringLiteral("userList")).toArray();
            for (const QJsonValue &v : ul)
                if (v.toObject().value(QStringLiteral("name")).toString() == name) return true;
            return false;
        };
        auto pcHasPlaylist = [lib](const QString &name, const QString &songId) {
            const QVariantList pls = lib->playlists();
            for (const QVariant &v : pls) {
                const QVariantMap pm = v.toMap();
                if (pm.value(QStringLiteral("name")).toString() != name) continue;
                if (songId.isEmpty()) return true;
                for (const QVariant &s : pm.value(QStringLiteral("songs")).toList())
                    if (s.toMap().value(QStringLiteral("id")).toString() == songId) return true;
                return false;
            }
            return false;
        };

        // ---- 阶段 1：首连口令鉴权 + 双端非空 → 模式合并 ----
        mock->start(8795, lx->authCode());
        const bool s1 = waitUntil([&]() { return mock->finishedCalls() >= 1; }, 15000);
        printf("[%s] 首连全链路完成（hello/id/ah/WS/RPC）\n", s1 ? "PASS" : "FAIL");
        const bool m1 = pcHasFav("202001") && pcHasPlaylist(QStringLiteral("手机歌单"), "202002");
        printf("[%s] 手机库已并入电脑（收藏+歌单）\n", m1 ? "PASS" : "FAIL");
        const bool m2 = phoneLoveHas("wy_101001") && phoneHasPlaylist(QStringLiteral("电脑歌单"));
        printf("[%s] 电脑库已下发手机（收藏+歌单）\n", m2 ? "PASS" : "FAIL");
        const bool m3 = modeRequests == 1;
        printf("[%s] 模式选择弹窗按预期触发 1 次（实际 %d）\n", m3 ? "PASS" : "FAIL", modeRequests);

        // ---- 阶段 2：电脑收藏变更 → 自动推给手机 ----
        QVariantMap seed2;
        seed2[QStringLiteral("favorites")] = QVariantList{ mkSong("101009", QStringLiteral("电脑新收藏C")) };
        lib->importOnlineLibrary(seed2);
        const bool s2 = waitUntil([&]() { return phoneLoveHas("wy_101009"); }, 8000);
        printf("[%s] 电脑→手机 增量推送\n", s2 ? "PASS" : "FAIL");

        // ---- 阶段 3：手机加收藏 → 电脑接收应用 ----
        // 注：mock 以它视角的库（首次下发的 3 收藏）为基线发 list_music_add，
        // 而电脑端 phase2 已本地新增 101009 —— 属真实并发改动场景，
        // 重连时由快照三方合并收敛（阶段 4 验证）。
        mock->phoneAddFavorite(mkLxMusic("202003", QStringLiteral("手机新增Z")));
        const bool s3 = waitUntil([&]() {
            return phoneLoveHas("wy_202003")
                && mock->getListDataCalls() >= 1;   // 电脑端已收到并应答
        }, 8000);
        printf("[%s] 手机→电脑 action 应用（并发改动交快照收敛）\n", s3 ? "PASS" : "FAIL");

        // ---- 阶段 4：断线重连 → 快照静默合并（不应再弹模式） ----
        const int modesBefore = modeRequests;
        mock->disconnectNow();
        waitUntil([&]() { return lx->onlineCount() == 0; }, 5000);
        QVariantMap seed3;
        seed3[QStringLiteral("favorites")] = QVariantList{ mkSong("101020", QStringLiteral("重连前新增D")) };
        lib->importOnlineLibrary(seed3);
        mock->reconnect(8795);
        const bool s4 = waitUntil([&]() { return mock->finishedCalls() >= 2; }, 15000);
        printf("[%s] 重连同步完成（走设备快照）\n", s4 ? "PASS" : "FAIL");
        const bool s4b = modeRequests == modesBefore;
        printf("[%s] 重连未再弹模式选择（快照路径）\n", s4b ? "PASS" : "FAIL");
        const bool s4c = waitUntil([&]() { return phoneLoveHas("wy_101020"); }, 8000);
        printf("[%s] 重连收敛：断线期间的电脑新增补到手机\n", s4c ? "PASS" : "FAIL");

        sc->setEnabled(false);
        const bool pass = s1 && m1 && m2 && m3 && s2 && s3 && s4 && s4b && s4c;
        printf("[%s] 洛雪同步协议自检结束\n", pass ? "PASS" : "FAIL");
        return finishSelfTest(pass ? 0 : 4);
    }
#endif // MUYUN_SELFTES

    // 本地播放自检：直接喂 QMediaPlayer 一个本地文件，看状态机/位置是否推进（复现"点了不播"）
    // 用法：MuyunMusic.exe --test-play [音频文件]
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-play"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        printf("=== 本地播放自检 ===\n");
        const int pi = args.indexOf(QStringLiteral("--test-play"));
        QString src = (pi + 1 < args.size() && !args.at(pi + 1).startsWith(QStringLiteral("--")))
                          ? args.at(pi + 1) : QString();
        if (src.isEmpty() || !QFileInfo::exists(src)) {
            QDirIterator it(QStringLiteral("E:/Music"), {QStringLiteral("*.mp3"), QStringLiteral("*.flac"),
                              QStringLiteral("*.m4a")}, QDir::Files, QDirIterator::Subdirectories);
            if (it.hasNext()) src = it.next();
        }
        if (src.isEmpty() || !QFileInfo::exists(src)) { printf("[SKIP] 无本地音频文件\n"); return finishSelfTest(0); }
        printf("[INFO] 文件：%s (%lld bytes)\n", qPrintable(src), (long long)QFileInfo(src).size());
        {
            QFile mf(src); if (mf.open(QIODevice::ReadOnly)) {
                const QByteArray m = mf.read(4); mf.close();
                QString hex; for (char c : m) hex += QString("%1 ").arg(quint8(c),2,16,QChar('0'));
                printf("[INFO] magic=%s ascii=%s\n", qPrintable(hex), qPrintable(QString::fromLatin1(m)));
            }
        }
        QMediaPlayer mp;
        QAudioOutput ao;
        mp.setAudioOutput(&ao);
        QObject::connect(&mp, &QMediaPlayer::mediaStatusChanged, [](QMediaPlayer::MediaStatus s){
            printf("[STATUS] mediaStatus=%d\n", int(s)); });
        QObject::connect(&mp, &QMediaPlayer::playbackStateChanged, [](QMediaPlayer::PlaybackState s){
            printf("[STATUS] playbackState=%d\n", int(s)); });
        QObject::connect(&mp, &QMediaPlayer::errorOccurred, [](QMediaPlayer::Error e, const QString& m){
            printf("[ERROR] %d %s\n", int(e), qPrintable(m)); });
        // 复刻 app 的 playFile 序列：先 stop()+setSource(空)（模拟 stop 清源），再 setSource+play
        mp.stop();
        mp.setSource(QUrl());
        QCoreApplication::processEvents();
        mp.setSource(QUrl::fromLocalFile(src));
        mp.play();
        QElapsedTimer t; t.start();
        qint64 lastPos = -1;
        while (t.elapsed() < 4000) {
            QCoreApplication::processEvents();
            if (mp.position() != lastPos) { lastPos = mp.position();
                printf("[POS] %lldms dur=%lldms state=%d\n", (long long)mp.position(),
                       (long long)mp.duration(), int(mp.playbackState())); }
            QThread::msleep(50);
        }
        const bool played = mp.position() > 1000;
        printf("[%s] 原始 4秒后 position=%lldms duration=%lldms\n", played?"PASS":"FAIL",
               (long long)mp.position(), (long long)mp.duration());
        if (!played) {
            // 试 nudge：play 卡住后补一次 seek 到 0，看能否激活解码
            mp.setPosition(0);
            QElapsedTimer t3; t3.start();
            while (t3.elapsed() < 2500) { QCoreApplication::processEvents(); QThread::msleep(50); }
            printf("[%s] setPosition(0) nudge 后 position=%lldms state=%d\n",
                   mp.position() > 500 ? "PASS" : "FAIL", (long long)mp.position(), int(mp.playbackState()));
        }
        mp.stop();

        // 对照：给一个能播的 mp3 用 TagWriter 打标签后是否还能播（查标签是否损坏文件）
        if (src.toLower().endsWith(QStringLiteral(".mp3")) || true) {
            const QString tw = QFileInfo(src).absolutePath() + QStringLiteral("/_tw_probe.mp3");
            QFile::remove(tw);
            if (QFile::copy(src, tw)) {
                TagWriter::Payload p;
                p.title = QStringLiteral("标签播放对照"); p.artist = QStringLiteral("QA");
                p.lyrics = QStringLiteral("[00:00.00]测试");
                const bool wr = TagWriter::write(tw, p);
                QMediaPlayer mp2; QAudioOutput ao2; mp2.setAudioOutput(&ao2);
                mp2.setSource(QUrl::fromLocalFile(tw));
                mp2.play();
                QElapsedTimer t2; t2.start();
                while (t2.elapsed() < 3000) { QCoreApplication::processEvents(); QThread::msleep(50); }
                const bool played2 = mp2.position() > 800;
                printf("[%s] TagWriter写入=%d → 打标签后 position=%lldms\n",
                       played2?"PASS":"FAIL", int(wr), (long long)mp2.position());
                mp2.stop();
                QFile::remove(tw);
                // 若原文件能播、打标签后不能播 → 标签损坏文件
                if (played && !played2)
                    printf("[DIAG] ⚠ TagWriter 可能损坏了可播放性！\n");
            }
        }
        return finishSelfTest(played ? 0 : 4);
    }
#endif // MUYUN_SELFTES

    // 音频设备切换续播自检：真实 PlayerEngine 播放 → 模拟默认输出设备重插 → 校验位置不回 0
    // 用法：MuyunMusic.exe --test-device-switch [文件]（缺省自动生成 10s 正弦波 WAV）
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-device-switch"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        printf("=== 设备切换续播自检 ===\n");
        const int di = args.indexOf(QStringLiteral("--test-device-switch"));
        QString src = (di + 1 < args.size() && !args.at(di + 1).startsWith(QStringLiteral("--")))
                          ? args.at(di + 1) : QString();
        if (src.isEmpty() || !QFileInfo::exists(src)) {
            // 自生成 10s 8kHz 16bit 单声道正弦波 WAV（测试自包含，不依赖用户曲库）
            src = QDir::tempPath() + QStringLiteral("/muyun-devswitch-tone.wav");
            QFile w(src);
            if (w.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                const int rate = 8000, secs = 10;
                const quint32 dataBytes = quint32(rate * secs * 2);
                QByteArray hdr;
                auto u32 = [](quint32 v){ QByteArray b(4,0); b[0]=char(v&0xFF); b[1]=char((v>>8)&0xFF); b[2]=char((v>>16)&0xFF); b[3]=char((v>>24)&0xFF); return b; };
                auto u16 = [](quint16 v){ QByteArray b(2,0); b[0]=char(v&0xFF); b[1]=char((v>>8)&0xFF); return b; };
                hdr += "RIFF" + u32(36 + dataBytes) + "WAVEfmt ";
                hdr += u32(16) + u16(1) + u16(1) + u32(rate) + u32(rate * 2) + u16(2) + u16(16);
                hdr += "data" + u32(dataBytes);
                w.write(hdr);
                QByteArray pcm; pcm.resize(int(dataBytes));
                for (quint32 i = 0; i < dataBytes / 2; ++i) {
                    const double t = double(i) / rate;
                    const short s = short(12000 * qSin(2 * M_PI * 440.0 * t));
                    pcm.replace(int(i * 2), 2, reinterpret_cast<const char *>(&s), 2);
                }
                w.write(pcm);
                w.close();
            }
        }
        if (src.isEmpty() || !QFileInfo::exists(src)) { printf("[SKIP] 无可用音频\n"); return finishSelfTest(0); }
        printf("[INFO] 文件：%s (%lld bytes)\n", qPrintable(src), (long long)QFileInfo(src).size());

        auto *eng = new Muyun::PlayerEngine(qApp);
        eng->playFile(src);

        auto waitMs = [](int ms) {
            QElapsedTimer t; t.start();
            while (t.elapsed() < ms) { QCoreApplication::processEvents(QEventLoop::AllEvents, 30); QThread::msleep(10); }
        };
        // 等到播放推进过 3 秒
        QElapsedTimer probe; probe.start();
        while (eng->position() < 3000 && probe.elapsed() < 15000) waitMs(100);
        const qint64 before = eng->position();
        printf("[%s] 切设备前 position=%lldms（播放推进正常）\n",
               before > 1000 ? "PASS" : "FAIL", (long long)before);

        // 模拟系统默认输出设备变化（拔插耳机同路径：stop→重建 output→setSource→play→择机跳回）
        eng->reattachToCurrentDevice();
        waitMs(3000);
        const qint64 after = eng->position();
        const bool resumed = after >= before - 500;   // 允许 0.5s 容差（seek 精度）
        printf("[%s] 切设备后 position=%lldms（应≈%lldms 续播，而非回 0）\n",
               resumed ? "PASS" : "FAIL", (long long)after, (long long)before);

        eng->stop();
        delete eng;
        return finishSelfTest((before > 1000 && resumed) ? 0 : 4);
    }
#endif // MUYUN_SELFTES

    // 播放队列行为自检（隔离数据目录=无音源脚本，全部离线可跑）：
    //   1) "下一首播放"在单曲循环模式下也优先生效（不被播放模式吞掉）
    //   2) 在线歌解析失败自动切下一首（而非停在原地）
    //   3) 播放缓存命中 → 直接播本地文件，完全不碰音源
    // 用法：MuyunMusic.exe --test-queue
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-queue"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        printf("=== 播放队列行为自检 ===\n");
        auto waitMs = [](int ms) {
            QElapsedTimer t; t.start();
            while (t.elapsed() < ms) { QCoreApplication::processEvents(QEventLoop::AllEvents, 30); QThread::msleep(10); }
        };
        auto makeWav = [](const QString &path) {   // 1s 8kHz 16bit 静音 WAV（>1KB，缓存有效性校验可过）
            QFile w(path);
            if (!w.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
            const quint32 rate = 8000, dataBytes = rate * 2;
            auto u32 = [](quint32 v){ QByteArray b(4,0); b[0]=char(v&0xFF); b[1]=char((v>>8)&0xFF); b[2]=char((v>>16)&0xFF); b[3]=char((v>>24)&0xFF); return b; };
            auto u16 = [](quint16 v){ QByteArray b(2,0); b[0]=char(v&0xFF); b[1]=char((v>>8)&0xFF); return b; };
            w.write("RIFF" + u32(36 + dataBytes) + "WAVEfmt " + u32(16) + u16(1) + u16(1)
                    + u32(rate) + u32(rate * 2) + u16(2) + u16(16) + "data" + u32(dataBytes));
            w.write(QByteArray(int(dataBytes), '\0'));
            w.close();
        };
        auto localSong = [&](const QString &tag) {
            const QString p = QDir::tempPath() + QStringLiteral("/muyun-q-%1.wav").arg(tag);
            makeWav(p);
            QVariantMap m;
            m[QStringLiteral("id")] = p;
            m[QStringLiteral("name")] = QStringLiteral("T-") + tag;
            m[QStringLiteral("artist")] = QStringLiteral("test");
            m[QStringLiteral("duration")] = 1.0;
            m[QStringLiteral("platform")] = QStringLiteral("local");
            m[QStringLiteral("localPath")] = p;
            return m;
        };
        auto onlineSong = [](const QString &tag) {
            QVariantMap m;
            m[QStringLiteral("id")] = QStringLiteral("q-") + tag;
            m[QStringLiteral("name")] = QStringLiteral("Q-") + tag;
            m[QStringLiteral("artist")] = QStringLiteral("test");
            m[QStringLiteral("duration")] = 100.0;
            m[QStringLiteral("platform")] = QStringLiteral("netease");
            QVariantMap lx;
            lx[QStringLiteral("source")] = QStringLiteral("lxtest");  // 不存在的音源 → resolveUrl 秒回空
            m[QStringLiteral("lx")] = lx;
            return m;
        };
        QStringList failMsgs;
        QObject::connect(player, &PlayerController::playFailed,
                         [&failMsgs](const QString &s) { failMsgs << s; });

        // -- 1) 下一首播放覆写（单曲循环下 next() 也应播插入曲）--
        player->setPlayModeId(QStringLiteral("single"));
        player->playSong(localSong(QStringLiteral("a")),
                         { localSong(QStringLiteral("a")), localSong(QStringLiteral("b")), localSong(QStringLiteral("c")) });
        waitMs(250);
        player->insertNext(localSong(QStringLiteral("c")));
        player->next();
        waitMs(250);
        const bool overrideOk = player->currentSong().name == QStringLiteral("T-c");
        printf("[%s] 1) 单曲循环下“下一首播放”生效（当前=%s，期望 T-c）\n",
               overrideOk ? "PASS" : "FAIL", qPrintable(player->currentSong().name));

        // -- 2) 在线歌失败自动跳下一首（无音源→解析必失败）--
        player->setPlayModeId(QStringLiteral("loop"));
        failMsgs.clear();
        player->playSong(onlineSong(QStringLiteral("x")),
                         { onlineSong(QStringLiteral("x")), onlineSong(QStringLiteral("y")) });
        bool advanced = false;
        {
            QElapsedTimer t2; t2.start();
            while (t2.elapsed() < 15000) {
                waitMs(100);
                if (player->currentSong().name == QStringLiteral("Q-y")) { advanced = true; break; }
            }
        }
        const bool skipAnnounced = !failMsgs.filter(QStringLiteral("已自动播放下一首")).isEmpty();
        const bool autoNextOk = advanced && skipAnnounced;
        printf("[%s] 2) 音源失败自动切下一首（切到=%s，自动跳提示=%d）\n",
               autoNextOk ? "PASS" : "FAIL", qPrintable(player->currentSong().name), int(skipAnnounced));

        // -- 3) 缓存命中免音源（预写缓存文件 → 直接播本地缓存，无任何失败播报）--
        const QVariantMap cs = onlineSong(QStringLiteral("z"));
        const QString cachePath = Muyun::PlayerEngine::cachePathForKey(
            Muyun::Song::fromMap(cs).identityKey() + QLatin1Char('@')
            + Muyun::qualityId(Muyun::AudioQuality::K320));
        QDir().mkpath(Muyun::PlayerEngine::audioCacheDir());
        makeWav(cachePath);
        const bool hitOk = Muyun::PlayerEngine::cachedAudioFile(
            Muyun::Song::fromMap(cs).identityKey() + QLatin1Char('@')
            + Muyun::qualityId(Muyun::AudioQuality::K320)) == cachePath;
        failMsgs.clear();
        player->playSong(cs, { cs });
        waitMs(400);
        const bool playedFromCache = hitOk && player->currentSong().name == QStringLiteral("Q-z")
            && failMsgs.isEmpty()
            && QDir::cleanPath(player->currentAudioLocalPath()).compare(
                   QDir::cleanPath(cachePath), Qt::CaseInsensitive) == 0;
        printf("[%s] 3) 缓存歌直接播放不碰音源（查询命中=%d，播放源=%s）\n",
               playedFromCache ? "PASS" : "FAIL", int(hitOk),
               qPrintable(player->currentAudioLocalPath()));

        // -- 4) 随机播放不重复（牌堆一轮内不重复、每首必播；轮尽重洗继续）--
        auto makeWavDur = [&](const QString &path, double sec) {
            QFile w(path);
            if (!w.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
            const quint32 rate = 8000;
            const quint32 dataBytes = quint32(rate * 2) * quint32(sec);
            auto u32 = [](quint32 v){ QByteArray b(4,0); b[0]=char(v&0xFF); b[1]=char((v>>8)&0xFF); b[2]=char((v>>16)&0xFF); b[3]=char((v>>24)&0xFF); return b; };
            auto u16 = [](quint16 v){ QByteArray b(2,0); b[0]=char(v&0xFF); b[1]=char((v>>8)&0xFF); return b; };
            w.write("RIFF" + u32(36 + dataBytes) + "WAVEfmt " + u32(16) + u16(1) + u16(1)
                    + u32(rate) + u32(rate * 2) + u16(2) + u16(16) + "data" + u32(dataBytes));
            w.write(QByteArray(int(dataBytes), '\0'));
            w.close();
        };
        auto shSong = [&](const QString &tag) {
            const QString p = QDir::tempPath() + QStringLiteral("/muyun-sh-%1.wav").arg(tag);
            makeWavDur(p, 30.0);   // 30s 静音：测试期间不会自然播完触发切歌
            QVariantMap m;
            m[QStringLiteral("id")] = p;
            m[QStringLiteral("name")] = QStringLiteral("S-") + tag;
            m[QStringLiteral("artist")] = QStringLiteral("test");
            m[QStringLiteral("duration")] = 30.0;
            m[QStringLiteral("platform")] = QStringLiteral("local");
            m[QStringLiteral("localPath")] = p;
            return m;
        };
        player->setPlayModeId(QStringLiteral("shuffle"));
        const QStringList shTags = { QStringLiteral("a"), QStringLiteral("b"),
                                     QStringLiteral("c"), QStringLiteral("d"), QStringLiteral("e") };
        QVariantList shList;
        for (const QString &t : shTags) shList.append(shSong(t));
        player->playSong(shSong(QStringLiteral("a")), shList);
        waitMs(300);
        // 首轮：从当前曲出发走完牌堆剩下的 4 张 → 全部不同、且都与当前曲不同
        QStringList seq;
        seq << player->currentSong().name;   // S-a
        bool shuffleOk = true;
        for (int i = 0; i < 4 && shuffleOk; ++i) {
            player->next();
            waitMs(300);
            const QString cur = player->currentSong().name;
            if (seq.contains(cur)) shuffleOk = false;   // 一轮内出现重复 → 算法失败
            seq << cur;
        }
        // 第 5 下 next：一轮走完 → 重洗开新一轮，不能停在原地/不能立刻重放刚播完的
        player->next();
        waitMs(300);
        const QString round2 = player->currentSong().name;
        const bool round2Ok = !round2.isEmpty() && round2 != seq.last();
        const QSet<QString> uniqueSeq(seq.begin(), seq.end());   // Qt6 无 toSet()
        shuffleOk = shuffleOk && round2Ok
            && seq.size() == 5              // 当前曲 + 首轮剩下的 4 首
            && uniqueSeq.size() == 5;       // 首轮 5 首全不同（一轮内不重复、每首必播）
        printf("[%s] 4) 随机播放一轮内不重复（序列=%s，第二轮=%s）\n",
               shuffleOk ? "PASS" : "FAIL",
               qPrintable(seq.join(QLatin1String(">"))), qPrintable(round2));

        // 清理测试产物
        QFile::remove(cachePath);
        for (const QString tag : { QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c") })
            QFile::remove(QDir::tempPath() + QStringLiteral("/muyun-q-%1.wav").arg(tag));
        for (const QString tag : shTags)
            QFile::remove(QDir::tempPath() + QStringLiteral("/muyun-sh-%1.wav").arg(tag));
        player->stop();
        const int rc = (overrideOk && autoNextOk && playedFromCache && shuffleOk) ? 0 : 4;
        printf("=== 队列自检 %s ===\n", rc == 0 ? "PASS" : "FAIL");
        return finishSelfTest(rc);
    }
#endif // MUYUN_SELFTES

    // 在线歌失败后"前进式跳歌 + 单首时限"自检（用户 2026-10-04 报：随机模式下
    // 一首歌播完还是这一首、下一首卡住就不动）。覆盖：
    //   1) 随机模式整队列取不到音频 → 前进式跳歌，不重放当前曲、不回弹；
    //   2) 快速失败不误触单首时限（看门狗不误报）；
    //   3) 选歌武装 / 主动停止解除看门狗；
    //   4) 不变式：正在出声时看门狗不得计时（真出声 = 已解武装）。
    // 用法：MuyunMusic.exe --test-advance
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-advance"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        printf("=== 失败前进 / 单首时限自检 ===\n");
        qputenv("MUYUN_SONG_WATCH_MS", "800");   // 压短看门狗，自检不必等 30s

        auto waitMs = [](int ms) {
            QElapsedTimer t; t.start();
            while (t.elapsed() < ms) { QCoreApplication::processEvents(QEventLoop::AllEvents, 30); QThread::msleep(10); }
        };
        auto makeWav = [](const QString &path) {   // 1s 8kHz 16bit 静音 WAV
            QFile w(path);
            if (!w.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
            const quint32 rate = 8000, dataBytes = rate * 2;
            auto u32 = [](quint32 v) {
                QByteArray b(4, 0);
                b[0] = char(v & 0xFF); b[1] = char((v >> 8) & 0xFF);
                b[2] = char((v >> 16) & 0xFF); b[3] = char((v >> 24) & 0xFF);
                return b;
            };
            auto u16 = [](quint16 v) {
                QByteArray b(2, 0);
                b[0] = char(v & 0xFF); b[1] = char((v >> 8) & 0xFF);
                return b;
            };
            w.write("RIFF" + u32(36 + dataBytes) + "WAVEfmt " + u32(16) + u16(1) + u16(1)
                    + u32(rate) + u32(rate * 2) + u16(2) + u16(16) + "data" + u32(dataBytes));
            w.write(QByteArray(int(dataBytes), '\0'));
            w.close();
            return true;
        };
        auto localSong = [&](const QString &tag) {
            const QString p = QDir::tempPath() + QStringLiteral("/muyun-adv-%1.wav").arg(tag);
            makeWav(p);
            QVariantMap m;
            m[QStringLiteral("id")] = p;
            m[QStringLiteral("name")] = QStringLiteral("L-") + tag;
            m[QStringLiteral("artist")] = QStringLiteral("test");
            m[QStringLiteral("duration")] = 1.0;
            m[QStringLiteral("platform")] = QStringLiteral("local");
            m[QStringLiteral("localPath")] = p;
            return m;
        };
        // 不存在的音源 → resolveUrl 秒回空，全程不联网
        auto badSong = [](const QString &tag) {
            QVariantMap m;
            m[QStringLiteral("id")] = QStringLiteral("adv-") + tag;
            m[QStringLiteral("name")] = QStringLiteral("B-") + tag;
            m[QStringLiteral("artist")] = QStringLiteral("test");
            m[QStringLiteral("duration")] = 100.0;
            m[QStringLiteral("platform")] = QStringLiteral("netease");
            QVariantMap lx;
            lx[QStringLiteral("source")] = QStringLiteral("lxtest");
            m[QStringLiteral("lx")] = lx;
            return m;
        };

        QStringList failMsgs, failSeq;
        QObject::connect(player, &PlayerController::playFailed,
                         [&failMsgs, &failSeq, player](const QString &s) {
                             failMsgs << s;
                             failSeq << player->currentSong().name;
                         });

        // -- 1) 随机模式：整队列取不到音频 → 前进式跳歌，不重放、不回弹 --
        const QStringList advTags = { QStringLiteral("a"), QStringLiteral("b"),
                                       QStringLiteral("c"), QStringLiteral("d") };
        QVariantList badList;
        for (const QString &t : advTags) badList.append(badSong(t));
        player->setPlayModeId(QStringLiteral("shuffle"));
        failMsgs.clear();
        failSeq.clear();
        const QString first = badSong(QStringLiteral("a")).value(QStringLiteral("name")).toString();
        player->playSong(badSong(QStringLiteral("a")), badList);
        {
            QElapsedTimer t; t.start();
            int lastN = -1;
            while (t.elapsed() < 20000) {
                waitMs(200);
                if (failSeq.size() == lastN) break;   // 连续一拍没有新失败 → 已收敛
                lastN = failSeq.size();
            }
        }
        bool backtrack = false;
        for (int i = 1; i < failSeq.size(); ++i)
            if (failSeq.at(i) == failSeq.at(i - 1)) backtrack = true;
        const bool movedOn = player->currentSong().name != first;
        const bool autoSkip = !failMsgs.filter(QStringLiteral("已自动播放下一首")).isEmpty();
        const bool advanceOk = movedOn && !backtrack && autoSkip
                               && !player->isPlaying() && failSeq.size() >= 3;
        printf("[%s] 1) 随机模式失败前进式跳歌（首次=%s 结束=%s 序列=%s 不回弹=%d）\n",
               advanceOk ? "PASS" : "FAIL", qPrintable(first),
               qPrintable(player->currentSong().name),
               qPrintable(failSeq.join(QLatin1String(">"))), int(!backtrack));

        // -- 2) 看门狗不误报：上面是"秒回空"的快速失败，不该出现"迟迟取不到音频" --
        const bool noFalsePositive = failMsgs.filter(QStringLiteral("迟迟取不到音频")).isEmpty();
        printf("[%s] 2) 快速失败不误触单首时限\n", noFalsePositive ? "PASS" : "FAIL");

        // -- 3) 选歌即武装；主动停止一定解除 --
        player->setPlayModeId(QStringLiteral("sequence"));
        player->playSong(localSong(QStringLiteral("a")), { localSong(QStringLiteral("a")) });
        const bool armedNow = player->songWatchdogActive();
        player->stop();
        const bool wdStopOk = armedNow && !player->songWatchdogActive();
        printf("[%s] 3) 选歌武装 / 停止解除（武装=%d 停止后=%d）\n",
               wdStopOk ? "PASS" : "FAIL", int(armedNow), int(player->songWatchdogActive()));

        // -- 4) 不变式：正在出声时看门狗不得计时（真出声 = 已解武装）--
        player->playSong(localSong(QStringLiteral("a")), { localSong(QStringLiteral("a")) });
        waitMs(1500);
        const bool playing = player->isPlaying();
        const bool armedWhilePlaying = player->songWatchdogActive();
        const bool invariantOk = !(playing && armedWhilePlaying);
        printf("[%s] 4) 出声时不误计时（播放中=%d 计时中=%d）\n",
               invariantOk ? "PASS" : "FAIL", int(playing), int(armedWhilePlaying));

        for (const QString &tag : advTags)
            QFile::remove(QDir::tempPath() + QStringLiteral("/muyun-adv-%1.wav").arg(tag));
        QFile::remove(QDir::tempPath() + QStringLiteral("/muyun-adv-a.wav"));
        player->stop();
        qunsetenv("MUYUN_SONG_WATCH_MS");
        const int rc = (advanceOk && noFalsePositive && wdStopOk && invariantOk) ? 0 : 4;
        printf("=== 失败前进自检 %s ===\n", rc == 0 ? "PASS" : "FAIL");
        return finishSelfTest(rc);
    }
#endif // MUYUN_SELFTES
    // fx 视觉控制台自检：拉起舞台 → 打开面板 → 验证 fx 状态回显
    // 用法：MuyunMusic.exe --test-fx [保持秒数，默认20]
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-fx"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        printf("=== fx 视觉控制台自检 ===\n");
        const int holdIdx = args.indexOf(QStringLiteral("--test-fx")) + 1;
        const int holdSec = (holdIdx < args.size() && args[holdIdx].toInt() > 0)
                                ? args[holdIdx].toInt() : 20;
        bool gotEngineReady = false;
        int fxStates = 0;
        QObject::connect(stage, &StageBridge::activeChanged, [&]() {
            if (stage->active() && !gotEngineReady) {
                gotEngineReady = true;
                printf("[OK] engineReady，打开 fx 面板\n");
                fflush(stdout);
                stageFx->show();
            }
        });
        QObject::connect(stage, &StageBridge::fxStateReceived, [&](const QJsonObject &st) {
            ++fxStates;
            printf("[EVT] fx state keys=%d preset=%d intensity=%.2f cinema=%d\n",
                   st.size(), st.value(QStringLiteral("preset")).toInt(-1),
                   st.value(QStringLiteral("intensity")).toDouble(-1),
                   st.value(QStringLiteral("cinema")).toBool(false) ? 1 : 0);
            fflush(stdout);
        });

        stage->open();
        QElapsedTimer t; t.start();
        qint64 lastMark = 0;
        const qint64 budget = (holdSec + 15) * 1000LL;
        while (t.elapsed() < budget) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            if (t.elapsed() - lastMark > 2000) {
                lastMark = t.elapsed();
                fprintf(stderr, "[test-fx] t=%lldms active=%d panel=%d fxStates=%d\n",
                        (long long)t.elapsed(), (int)stage->active(),
                        (int)stageFx->isVisible(), fxStates);
            }
            if (gotEngineReady && fxStates > 0 && t.elapsed() > holdSec * 1000LL) break;
        }
        printf("[%s] engineReady=%s panelShown=%d fxStates=%d\n",
               (gotEngineReady && stageFx->isVisible() && fxStates > 0) ? "PASS" : "FAIL",
               gotEngineReady ? "yes" : "no", (int)stageFx->isVisible(), fxStates);
        fflush(stdout);
        stageFx->hide();
        stage->close();
        {
            QElapsedTimer d; d.start();
            while (d.elapsed() < 1500)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        }
        return finishSelfTest((gotEngineReady && fxStates > 0) ? 0 : 4);
    }
#endif // MUYUN_SELFTES

    // 重开竞态自检：open→ready→close→open→ready，验证不再崩溃（问题4）
    // 用法：MuyunMusic.exe --test-stage-reopen
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-stage-reopen"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        printf("=== 舞台重开竞态自检 ===\n");
        int readyCount = 0;
        QObject::connect(stage, &StageBridge::activeChanged, [&]() {
            if (stage->active()) ++readyCount;
        });
        auto waitReadyOrTimeout = [&](int want, int ms) {
            QElapsedTimer t; t.start();
            while (t.elapsed() < ms && readyCount < want)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        };
        // 第一轮
        stage->open();
        waitReadyOrTimeout(1, 12000);
        printf("[round1] ready=%d\n", readyCount);
        // 关闭
        stage->close();
        { QElapsedTimer d; d.start(); while (d.elapsed() < 1500)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50); }
        // 立即重开（模拟用户退出全屏后马上再开）
        stage->open();
        waitReadyOrTimeout(2, 12000);
        printf("[round2] ready=%d\n", readyCount);
        const bool pass = readyCount >= 2;
        printf("[%s] reopen readyCount=%d\n", pass ? "PASS" : "FAIL", readyCount);
        stage->close();
        { QElapsedTimer d; d.start(); while (d.elapsed() < 1500)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50); }
        return finishSelfTest(pass ? 0 : 4);
    }
#endif // MUYUN_SELFTES

    // 舞台快速开关循环压力自检：open→ready→停留抖动→close（连点两次，隔几轮加
    // "关闭中反手重开再关"竞态）×N。复现用户"反复进出沉浸/ESC 多次后卡死崩溃"场景。
    // 用法：MuyunMusic.exe --test-stage-cycle [轮数，默认10]
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-stage-cycle"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        printf("=== 舞台快速开关循环压力自检 ===\n");
        const int ci = args.indexOf(QStringLiteral("--test-stage-cycle"));
        int cycles = (ci + 1 < args.size() && !args.at(ci + 1).startsWith(QStringLiteral("--")))
                         ? args.at(ci + 1).toInt() : 10;
        if (cycles < 2) cycles = 2;
        int ready = 0, done = 0;
        bool hung = false;
        QObject::connect(stage, &StageBridge::activeChanged,
                         [&]() { if (stage->active()) ++ready; });
        auto pump = [](int ms) {
            QElapsedTimer t; t.start();
            while (t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
        };
        for (int i = 0; i < cycles; ++i) {
            QElapsedTimer t; t.start();
            stage->open();
            while (!stage->active() && t.elapsed() < 12000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
            if (!stage->active()) { hung = true; printf("[cycle %d] 开台超时\n", i + 1); break; }
            pump(60 + (i * 137) % 340);              // 模拟人手停留抖动
            stage->close();
            stage->close();                          // 连点两次（幂等压力）
            if (i % 3 == 1) {                        // 关闭中反手重开再关（reopen 竞态压力）
                pump(80);
                stage->open();
                pump(80);
                stage->close();
            }
            t.restart();
            while (stage->active() && t.elapsed() < 6000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
            if (stage->active()) { hung = true; printf("[cycle %d] 关台超时\n", i + 1); break; }
            ++done;
            printf("[cycle %d] ok ready=%d\n", i + 1, ready);
        }
        const bool pass = !hung && done == cycles && !stage->unavailable();
        printf("[%s] 循环 %d/%d hung=%d unavailable=%d\n",
               pass ? "PASS" : "FAIL", done, cycles, int(hung), int(stage->unavailable()));
        stage->close();
        pump(800);
        return finishSelfTest(pass ? 0 : 4);
    }
#endif // MUYUN_SELFTES

    // 离线节拍端到端自检：真实播放一首歌 → track.bin 挂载 → 引擎离线分析 → 节拍数>0
    // 用法：MuyunMusic.exe --test-stage-beat [关键词]
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-stage-beat"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        printf("=== 离线节拍端到端自检 ===\n");
        const int bi = args.indexOf(QStringLiteral("--test-stage-beat"));
        const QString kw = (bi + 1 < args.size() && !args.at(bi + 1).startsWith(QStringLiteral("--")))
                           ? args.at(bi + 1) : QStringLiteral("晴天");
        QString srcDir = DocumentStore::instance()->rootPath() + QStringLiteral("/lx-sources");
        QDir d(srcDir);
        const QStringList js = d.entryList({QStringLiteral("*.js")}, QDir::Files);
        if (js.isEmpty()) { printf("[FATAL] no LX scripts\n"); return 1; }
        QString err;
        MusicSdk::instance()->loadLxScript(d.absoluteFilePath(js.first()), &err);
        SearchResult r = MusicSdk::instance()->searchAll(kw, 1, 5);
        if (r.songs.isEmpty()) { printf("[FATAL] search empty for %s\n", qPrintable(kw)); return 2; }
        const Song target = r.songs.first();
        printf("[OK] 播放：%s - %s\n", qPrintable(target.artist), qPrintable(target.name));
        QVariantList list; list << target.toMap();
        player->playSong(target.toMap(), list, QString(), QStringLiteral("自检"));
        int beatMaps = 0; QString beatSample;
        QObject::connect(stage, &StageBridge::beatStatus, [&](int m, const QString &s) {
            beatMaps = qMax(beatMaps, m);   // latch 峰值（probe 回报 0 不覆盖分析成功值）
            beatSample = s;
        });
        stage->open();
        QElapsedTimer t; t.start();
        qint64 lastProbe = 0;
        while (t.elapsed() < 55000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            if (t.elapsed() - lastProbe > 2500) {
                lastProbe = t.elapsed();
                stage->requestBeatProbe();
                fprintf(stderr, "[beat-test] t=%lldms active=%d beatMaps=%d %s\n",
                        (long long)t.elapsed(), (int)stage->active(), beatMaps,
                        qPrintable(beatSample));
            }
        }
        printf("[%s] beatMaps=%d sample=%s\n",
               beatMaps > 0 ? "PASS" : "FAIL", beatMaps, qPrintable(beatSample));
        player->stop();
        stage->close();
        { QElapsedTimer dd; dd.start();
          while (dd.elapsed() < 1500) QCoreApplication::processEvents(QEventLoop::AllEvents, 50); }
        return beatMaps > 0 ? 0 : 4;
    }
#endif // MUYUN_SELFTES

    // 频谱注入端到端自检：真实播放 → track.bin 挂载 → 离线 FFT 帧表 →
    // 假 analyser 读数非零（uBass/uEnergy 动起来）
    // 用法：MuyunMusic.exe --test-stage-spectrum [关键词]
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-stage-spectrum"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        printf("=== 频谱注入端到端自检 ===\n");
        const int si = args.indexOf(QStringLiteral("--test-stage-spectrum"));
        const QString kw = (si + 1 < args.size() && !args.at(si + 1).startsWith(QStringLiteral("--")))
                           ? args.at(si + 1) : QStringLiteral("晴天");
        QString srcDir = DocumentStore::instance()->rootPath() + QStringLiteral("/lx-sources");
        QDir d(srcDir);
        const QStringList js = d.entryList({QStringLiteral("*.js")}, QDir::Files);
        if (js.isEmpty()) { printf("[FATAL] no LX scripts\n"); return 1; }
        QString err;
        MusicSdk::instance()->loadLxScript(d.absoluteFilePath(js.first()), &err);
        SearchResult r = MusicSdk::instance()->searchAll(kw, 1, 5);
        if (r.songs.isEmpty()) { printf("[FATAL] search empty for %s\n", qPrintable(kw)); return 2; }
        const Song target = r.songs.first();
        printf("[OK] 播放：%s - %s\n", qPrintable(target.artist), qPrintable(target.name));
        QVariantList list; list << target.toMap();
        player->playSong(target.toMap(), list, QString(), QStringLiteral("自检"));
        bool specReady = false;
        double maxUEnergy = 0.0, maxUBass = 0.0;
        QString lastSample;
        QObject::connect(stage, &StageBridge::beatStatus, [&](int, const QString &s) {
            lastSample = s;
            if (s.contains(QStringLiteral("spec=ready"))) specReady = true;
            // 解析 uB=0.xxx uE=0.xxx
            const auto grab = [&s](const QString &key) -> double {
                int i = s.indexOf(key);
                if (i < 0) return 0.0;
                i += key.size();
                int j = i;
                while (j < s.size() && (s.at(j).isDigit() || s.at(j) == QLatin1Char('.') ||
                                        s.at(j) == QLatin1Char('-'))) j++;
                return s.mid(i, j - i).toDouble();
            };
            maxUBass = qMax(maxUBass, grab(QStringLiteral("uB=")));
            maxUEnergy = qMax(maxUEnergy, grab(QStringLiteral("uE=")));
        });
        stage->open();
        QElapsedTimer t; t.start();
        qint64 lastProbe = 0;
        while (t.elapsed() < 60000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            if (t.elapsed() - lastProbe > 2500) {
                lastProbe = t.elapsed();
                stage->requestBeatProbe();
                fprintf(stderr, "[spec-test] t=%lldms active=%d %s\n",
                        (long long)t.elapsed(), (int)stage->active(), qPrintable(lastSample));
            }
            // 就绪且能量已观察到位（连续多帧非零）再收
            if (specReady && maxUEnergy > 0.05 && t.elapsed() > 20000) break;
        }
        const bool pass = specReady && maxUEnergy > 0.02;
        printf("[%s] specReady=%d maxUBass=%.3f maxUEnergy=%.3f sample=%s\n",
               pass ? "PASS" : "FAIL", (int)specReady, maxUBass, maxUEnergy, qPrintable(lastSample));
        player->stop();
        stage->close();
        { QElapsedTimer dd; dd.start();
          while (dd.elapsed() < 1500) QCoreApplication::processEvents(QEventLoop::AllEvents, 50); }
        return finishSelfTest(pass ? 0 : 4);
    }
#endif // MUYUN_SELFTES

    const QUrl url(QStringLiteral("qrc:/qml/Main.qml"));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, []() { QCoreApplication::exit(1); },
                     Qt::QueuedConnection);
    engine.load(url);

    if (engine.rootObjects().isEmpty()) {
        // 明文输出，便于排查（控制台版本可见）
        fprintf(stderr, "QML load failed: rootObjects is empty\n");
        fprintf(stderr, "importPaths://n");
        for (const auto &p : engine.importPathList())
            fprintf(stderr, "  %s\n", qPrintable(p));
        fflush(stderr);
        return 1;
    }

    // 启动时强制前置主窗口（QML 的 requestActivate 受 Windows 前台锁策略影响可能失效）
    auto *mainWin = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    stage->attachWindow(mainWin);
    hotkey->attach(mainWin);
    imeGuard->attach(mainWin);
    // 手动最大化样式位同步（#11）：只需窗口句柄，不激活原生过滤器（见 FramelessWindow 头注释）
    frameless->bindMaxWindow(mainWin);
    // mineradio 舞台是独立进程窗，它抢焦点时本应用会判为"后台"→ 热键保活，
    // 保证舞台全屏界面里 F11 仍能切主窗全屏。
    QObject::connect(stage, &Muyun::StageBridge::activeChanged, hotkey,
                     [stage, hotkey]() { hotkey->setKeepActive(stage->active()); });
    // Esc 系统热键：前台归属判据 + 命中后走阶梯（舞台开着交页面裁判，否则交 QML 的 runEscLadder）
    hotkey->setForegroundProbe([stage]() { return stage->ownsSystemForeground(); });
    QObject::connect(hotkey, &HotkeyManager::escapeRequested, &app, [mainWin]() {
        // 阶梯判断统一交给 QML（runEscLadder 自己会决定"交页面裁判"还是"退播放页/退全屏"），
        // 这里别抢着判断：舞台正在退出时 active 还是真、页面仍能收命令，
        // 在 C++ 里先 sendEsc 就会把"退窗口全屏"那一步吃掉。
        if (mainWin) QMetaObject::invokeMethod(mainWin, "runEscLadder");
    });
    // 原生缩放（WS_THICKFRAME + WM_NCHITTEST）与 Qt frameless 协作会抖动，改用 QML 手动缩放，
    // 故不再 attach（FramelessWindow 因 m_window 为空而惰性）。
    // frameless->attach(mainWin);
#ifdef Q_OS_WIN
    if (mainWin) {
        HWND hwnd = reinterpret_cast<HWND>(mainWin->winId());
        SetForegroundWindow(hwnd);
        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }
#endif

    // 最大化诊断：走与标题栏按钮完全相同的 Qt showMaximized() 路径，报告几何/可见态
#ifdef MUYUN_SELFTES
    if (qEnvironmentVariableIsSet("MUYUN_TEST_MAX") && mainWin) {
        auto dump = [mainWin](const char *tag) {
            const QRect g = mainWin->geometry();
#ifdef Q_OS_WIN
            const HWND hw = reinterpret_cast<HWND>(mainWin->winId());
            const LONG_PTR st = hw ? GetWindowLongPtr(hw, GWL_STYLE) : 0;
            const bool thick = st & WS_THICKFRAME;
            const bool caption = st & WS_CAPTION;
            const bool z = hw && IsZoomed(hw);
            printf("[MAXTEST] %s geo=%dx%d@%d,%d vis=%d state=0x%x IsZoomed=%d style=0x%llx THICK=%d CAPTION=%d\n",
                   tag, g.width(), g.height(), g.x(), g.y(),
                   int(mainWin->visibility()), int(mainWin->windowState()), int(z),
                   (unsigned long long)st, int(thick), int(caption));
#else
            printf("[MAXTEST] %s geo=%dx%d@%d,%d\n", tag, g.width(), g.height(), g.x(), g.y());
#endif
            fflush(stdout);
        };
        QTimer::singleShot(1500, mainWin, [mainWin, dump, frameless]() {
            dump("before");
            // 手动几何最大化（与 Main.qml toggleMaximize 同机制，不碰 showMaximized）
            const QRect avail = mainWin->screen()->availableGeometry();
            mainWin->setGeometry(avail);
            QTimer::singleShot(1000, mainWin, [mainWin, dump, frameless]() {
                dump("afterManualMax");
                // #11：补 WS_MAXIMIZE 样式位 → IsZoomed 应变真（第三方任务栏工具认最大化）
                frameless->setMaximizedStyle(true);
                QTimer::singleShot(400, mainWin, [mainWin, dump, frameless]() {
                    dump("afterStyleBit");
                    frameless->setMaximizedStyle(false);
                    mainWin->showNormal();
                    mainWin->setGeometry(200, 150, 1200, 720);
                    QTimer::singleShot(800, mainWin, [mainWin, dump]() {
                        dump("afterRestore");
                        QCoreApplication::quit();
                    });
                });
            });
        });
    }
#endif // MUYUN_SELFTES

    // 单实例守护·接线：监听与 raise 处理已提前到 main 开头（见"单实例守护"注释）；
    // 此处窗口已建好，登记唤起目标并补投启动期间收到的 raise
    if (mainWin) raiseBox->win = mainWin;
    if (raiseBox->pending) {
        raiseBox->pending = false;
        raiseWindow(mainWin);
    }

    // GUI 稳态内存采样：加载界面后每 2 秒打印工作集，20 秒退出
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-mem-gui"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        auto *mt = new QTimer(&app);
        int *tick = new int(0);
        QObject::connect(mt, &QTimer::timeout, [&app, tick]() {
            ++(*tick);
#ifdef Q_OS_WIN
            quint64 ws = 0, pv = 0; procMemSample(ws, pv);
            printf("[mem-gui] t=%ds 工作集=%.1fMB 私有集=%.1fMB\n",
                   *tick * 2, ws / 1048576.0, pv / 1048576.0);
#endif
            fflush(stdout);
            if (*tick >= 10) { delete tick; QCoreApplication::quit(); }
        });
        mt->start(2000);
    }
#endif // MUYUN_SELFTES

    // UI 交互自检：设置"关于"置底 + 收藏页搜索框 + 取消喜欢保持滚动位置
    // 用法：MuyunMusic.exe --test-ui
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-ui"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        QTimer::singleShot(1700, &app, [search, player]() {
            // 平台选择持久化断言：setPlatform → 立即落盘可回读
            search->setPlatform(QStringLiteral("kw"));
            const QString saved = Muyun::DocumentStore::instance()->readSync(
                QStringLiteral("feature"), QStringLiteral("searchPlatform")).toString();
            const bool ok = saved == QStringLiteral("kw");
            printf("[%s] 平台选择实时持久化（回读=%s）\n", ok ? "PASS" : "FAIL", qPrintable(saved));
            search->setPlatform(QString());   // 复位为全部
            // 按歌曲取源平台断言：为某首歌单独指定平台 → 只影响这首歌、落盘可回读、其它歌不受波及
            QVariantMap probe;
            probe[QStringLiteral("id")] = QStringLiteral("probe-mid-1");
            probe[QStringLiteral("name")] = QStringLiteral("探测曲");
            probe[QStringLiteral("platform")] = QStringLiteral("netease");   // song map 用 platformId
            player->setSongPlatform(probe, QStringLiteral("tx"));
            const bool effOk = player->songPlatform(probe) == QStringLiteral("tx")
                               && player->songPlatformOverridden(probe);
            const QVariantMap savedDoc = Muyun::DocumentStore::instance()->readSync(
                QStringLiteral("player"), QStringLiteral("songPlatforms")).toMap();
            const bool persistOk = savedDoc.contains(QStringLiteral("wy:probe-mid-1"))
                                   && savedDoc.value(QStringLiteral("wy:probe-mid-1")).toString() == QStringLiteral("tx");
            // 另一首同平台歌不受影响
            QVariantMap other;
            other[QStringLiteral("id")] = QStringLiteral("probe-mid-2");
            other[QStringLiteral("platform")] = QStringLiteral("netease");
            const bool isolatedOk = !player->songPlatformOverridden(other)
                                    && player->songPlatform(other) == QStringLiteral("wy");
            printf("[%s] 按歌曲取源平台（生效=%d 落盘=%d 不波及他曲=%d）\n",
                   (effOk && persistOk && isolatedOk) ? "PASS" : "FAIL",
                   int(effOk), int(persistOk), int(isolatedOk));
            player->setSongPlatform(probe, QString());   // 清除记忆
            if (!ok || !(effOk && persistOk && isolatedOk)) finishSelfTest(4);
        });
        QTimer::singleShot(1800, &app, [mainWin, library]() {
            // 播 40 首收藏（保证出现滚动条）
            auto mk = [](const QString &id, const QString &nm, const QString &ar) {
                Muyun::Song s; s.id = id; s.name = nm; s.artist = ar;
                s.platform = Muyun::Platform::Netease; s.duration = 200;
                s.hasLx = true; s.lx.source = QStringLiteral("wy"); s.lx.songmid = id;
                return s.toMap();
            };
            QVariantMap seed;
            QVariantList favs;
            for (int i = 0; i < 40; ++i)
                favs.append(mk(QString::number(900000 + i),
                               QStringLiteral("测试曲%1").arg(i + 1),
                               i % 3 == 0 ? QStringLiteral("歌手甲") : QStringLiteral("歌手乙")));
            seed[QStringLiteral("favorites")] = favs;
            library->importOnlineLibrary(seed);

            QObject *ro = mainWin;
            QMetaObject::invokeMethod(ro, "openSettings", Q_ARG(QVariant, QStringLiteral("local")));
        });
        QTimer::singleShot(2600, &app, [mainWin]() {
            const QString p1 = QDir::tempPath() + QStringLiteral("/muyun_ui_settings.png");
            mainWin->grabWindow().save(p1);
            // 关设置 → 收藏页
            if (auto *sp = mainWin->findChild<QObject*>("settingsPanelObj"))
                QMetaObject::invokeMethod(sp, "close");
            QMetaObject::invokeMethod(mainWin, "goPage", Q_ARG(QVariant, QStringLiteral("favorites")));
        });
        // 滚动到中部 → 取消该处可见歌曲的喜欢 → 校验滚动保持
        static QObject *view = nullptr;
        static QVariant victim;
        static double yBefore = 0, yAfter = 0;
        QTimer::singleShot(3400, &app, [mainWin, library]() {
            view = mainWin->findChild<QObject*>("listPageView");
            if (!view) { printf("[FAIL] 找不到列表视图\n"); finishSelfTest(4); }
            view->setProperty("contentY", 1000.0);
        });
        QTimer::singleShot(3800, &app, [library]() {
            yBefore = view->property("contentY").toDouble();
            const QVariantList favs = library->favorites();
            const int idx = qBound(0, int(yBefore / 62.0), favs.size() - 1);  // 行高62
            victim = favs.at(idx);
            library->toggleFavorite(victim.toMap());
        });
        QTimer::singleShot(4400, &app, [mainWin, library]() {
            yAfter = view->property("contentY").toDouble();
            const int countAfter = view->property("count").toInt();
            // 必须同时满足：① 列表真的刷新了（40→39，防止 handler 抛错中断造成"没刷新=没跳"假通过）
            // ② 滚动保持（取消喜欢不跳顶）
            const bool refreshed = countAfter == 39;
            const bool kept = refreshed && yAfter > yBefore - 200;
            printf("[%s] 取消喜欢刷新+保滚动 count=%d(期望39) before=%.0f after=%.0f\n",
                   kept ? "PASS" : "FAIL", countAfter, yBefore, yAfter);
            fflush(stdout);
            const QString p2 = QDir::tempPath() + QStringLiteral("/muyun_ui_favorites.png");
            mainWin->grabWindow().save(p2);
            finishSelfTest(kept ? 0 : 4);
        });
    }
#endif // MUYUN_SELFTES

    // F11 全屏链路自检：打开歌词页 → 模拟系统热键 WM_HOTKEY → 校验进入全屏 → 再按退出
    // 用法：MuyunMusic.exe --test-f11
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-f11"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef Q_OS_WIN
        static bool entered = false, exited = false;
        QTimer::singleShot(1500, &app, [mainWin]() {
            QMetaObject::invokeMethod(mainWin, "openLyricsPage");   // 进入全屏歌词页
        });
        auto postF11 = [mainWin]() {
            HWND hwnd = reinterpret_cast<HWND>(mainWin->winId());
            PostMessage(hwnd, 0x0312 /*WM_HOTKEY*/, 0xF11, MAKELPARAM(0x4000 /*MOD_NOREPEAT*/, 0x74 /*VK_F11*/));
        };
        QTimer::singleShot(2300, &app, [mainWin, postF11]() {
            postF11();   // 模拟按 F11
        });
        QTimer::singleShot(3000, &app, [mainWin, postF11]() {
            entered = (mainWin->visibility() == QWindow::FullScreen);
            printf("[%s] 歌词页内 F11 → 进入全屏（visibility=%d）\n",
                   entered ? "PASS" : "FAIL", int(mainWin->visibility()));
            if (entered) postF11();          // 再按退出
        });
        QTimer::singleShot(3700, &app, [mainWin]() {
            exited = (mainWin->visibility() != QWindow::FullScreen);
            printf("[%s] 再按 F11 → 退出全屏（visibility=%d）\n",
                   exited ? "PASS" : "FAIL", int(mainWin->visibility()));
            fflush(stdout);
            finishSelfTest((entered && exited) ? 0 : 4);
        });
#else
        printf("[SKIP] 非 Windows\n"); return finishSelfTest(0);
#endif
    }
#endif // MUYUN_SELFTES

    // F11 **真实按键**自检：不用 PostMessage 作弊，而是进程内 keybd_event 注入真 F11，
    // 必须穿过 RegisterHotKey/系统按键通路才算通。专治"窗口化按 F11 能进全屏、
    // 全屏里再按 F11 出不来"（--test-f11 那种 PostMessage 版测不出注册丢失）。
    // 用法：MuyunMusic.exe --test-f11-live
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-f11-live"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        struct St { bool e1 = false, x1 = false, e2 = false, x2 = false; bool reg = false; } st;
        auto check = [&st, mainWin, hotkey](const char *tag, bool wantFull) {
            const bool isFull = mainWin && mainWin->visibility() == QWindow::FullScreen;
            const bool ok = (isFull == wantFull);
            st.reg = hotkey->isRegistered();
            printf("[%s] %s：visibility=%d 期望全屏=%d 实际=%d 热键已注册=%d\n",
                   ok ? "PASS" : "FAIL", tag, int(mainWin->visibility()),
                   int(wantFull), int(isFull), int(st.reg));
            fflush(stdout);
            return ok;
        };
        auto inject = [hotkey]() { QMetaObject::invokeMethod(hotkey, "injectF11"); };
        QTimer::singleShot(1600, &app, [mainWin, hotkey, &st, inject]() {
            printf("起点：visibility=%d 热键已注册=%d\n", int(mainWin->visibility()),
                   int(hotkey->isRegistered()));
            fflush(stdout);
            inject();                                   // 窗口化 → 全屏
        });
        QTimer::singleShot(2600, &app, [&st, mainWin, hotkey, check, inject]() {
            st.e1 = check("第 1 次 F11（期望进全屏）", true); inject();   // 全屏 → 窗口化
        });
        QTimer::singleShot(3600, &app, [&st, mainWin, hotkey, check, inject]() {
            st.x1 = check("第 2 次 F11（期望退全屏）", false); inject();  // 再进
        });
        QTimer::singleShot(4600, &app, [&st, mainWin, hotkey, check, inject]() {
            st.e2 = check("第 3 次 F11（期望进全屏）", true); inject();   // 再出
        });
        QTimer::singleShot(5600, &app, [&st, mainWin, hotkey, check]() {
            st.x2 = check("第 4 次 F11（期望退全屏）", false);
            const bool all = st.e1 && st.x1 && st.e2 && st.x2;
            printf("%s F11 真实按键闭环%s\n", all ? "[PASS]" : "[FAIL]",
                   all ? "（进/出全屏各两轮）" : "（有环节没吃到键）");
            fflush(stdout);
            finishSelfTest(all ? 0 : 4);
        });
    }
#endif // MUYUN_SELFTES

    // 用户真实路径的 F11 自检：先开「全屏播放页」(点播放条封面进的那页)，三种样式各来一轮
    // ——真按 F11 必须能进全屏，再真按一次必须能出全屏（"全屏出不来"就是死在第 2 步）。
    // 舞台(mineradio)样式额外确认热键没被独立进程窗抢走。
    // 用法：MuyunMusic.exe --test-f11-page   （建议 MUYUN_STORE_ROOT 隔离，别写脏用户设置）
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-f11-page"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        struct St { bool cin = false, cout = false, ain = false, aout = false,
                          sin = false, sout = false, fin = false, e1 = false, e2 = false; } st;
        const QString origStyle = settings->playerStyle();
        auto shot = [mainWin](const char *name) {
            if (!mainWin) return;
            const QString p = QDir::tempPath() + QStringLiteral("/f11page_") + name + QStringLiteral(".png");
            mainWin->grabWindow().save(p);
            printf("    截图 %s\n", qPrintable(p));
        };
        auto inject = [hotkey]() { QMetaObject::invokeMethod(hotkey, "injectF11"); };
        auto injectEsc = [hotkey]() { QMetaObject::invokeMethod(hotkey, "injectEsc"); };
        auto expect = [mainWin, hotkey, stage](bool wantFull, const char *tag) {
            const bool isFull = mainWin && mainWin->visibility() == QWindow::FullScreen;
            const bool ok = (isFull == wantFull);
            printf("[%s] %s：visibility=%d 期望全屏=%d 实际全屏=%d 热键注册=%d 舞台=%d\n",
                   ok ? "PASS" : "FAIL", tag, int(mainWin->visibility()), int(wantFull),
                   int(isFull), int(hotkey->isRegistered()), int(stage->active()));
            fflush(stdout);
            return ok;
        };
        auto setStyle = [settings](const char *s) {
            settings->setPlayerStyle(QLatin1String(s));
            printf("    切换播放页样式 → %s\n", s); fflush(stdout);
        };
        using Step = std::pair<int, std::function<void()>>;   // (该步前等待 ms, 动作)
        auto steps = std::make_shared<std::vector<Step>>();
        steps->push_back({600, [mainWin]() {
            printf("== 打开全屏播放页（点封面那条路）==\n"); fflush(stdout);
            QMetaObject::invokeMethod(mainWin, "openLyricsPage");
        }});
        // —— 经典 ——
        steps->push_back({700, [setStyle]() { setStyle("classic"); }});
        steps->push_back({700, [inject]() { inject(); }});
        steps->push_back({1300, [&st, expect, shot, inject]() {
            st.cin = expect(true, "经典页·真按 F11 → 进全屏"); shot("classic_in"); inject(); }});
        steps->push_back({1300, [&st, expect, shot]() {
            st.cout = expect(false, "经典页·再真按 F11 → 出全屏"); shot("classic_out"); }});
        // —— Apple Music ——
        steps->push_back({700, [setStyle]() { setStyle("amll"); }});
        steps->push_back({700, [inject]() { inject(); }});
        steps->push_back({1300, [&st, expect, shot, inject]() {
            st.ain = expect(true, "Apple 页·真按 F11 → 进全屏"); shot("amll_in"); inject(); }});
        steps->push_back({1300, [&st, expect, shot]() {
            st.aout = expect(false, "Apple 页·再真按 F11 → 出全屏"); shot("amll_out"); }});
        // —— 舞台（独立进程窗抢焦点，历史上就是它把热键弄丢的）——
        steps->push_back({700, [setStyle]() { setStyle("mineradio"); }});
        steps->push_back({4500, [inject]() { inject(); }});
        steps->push_back({1500, [&st, expect, shot, inject]() {
            st.sin = expect(true, "舞台页·真按 F11 → 进全屏"); shot("stage_in"); inject(); }});
        steps->push_back({1500, [&st, expect, shot, setStyle, origStyle]() {
            st.sout = expect(false, "舞台页·再真按 F11 → 出全屏"); shot("stage_out");
            setStyle(origStyle.toLatin1().constData()); }});
        // —— ESC 阶梯（用户 2026-10-02 定：先退全屏播放页，再退窗口全屏）——
        steps->push_back({900, [setStyle]() { setStyle("classic"); }});
        steps->push_back({900, [inject]() { inject(); }});   // 播放页开着按 F11 → 进全屏
        steps->push_back({1500, [&st, expect, injectEsc]() {
            st.fin = expect(true, "阶梯·播放页开着 F11 → 进全屏");
            injectEsc();                                     // 第 1 下 Esc：只该退播放页
        }});
        steps->push_back({1300, [&st, mainWin, hotkey, stage, injectEsc, shot]() {
            QObject *pg = mainWin ? mainWin->findChild<QObject*>("lyricsPageObj") : nullptr;
            const bool pageGone = pg && !pg->property("visible").toBool();
            const bool stillFull = mainWin && mainWin->visibility() == QWindow::FullScreen;
            st.e1 = pageGone && stillFull;
            printf("[%s] 阶梯·第 1 下 Esc：播放页已退=%d 且仍在全屏=%d\n",
                   st.e1 ? "PASS" : "FAIL", int(pageGone), int(stillFull));
            fflush(stdout);
            shot("esc1");
            injectEsc();                                     // 第 2 下 Esc：该退窗口全屏
        }});
        steps->push_back({1300, [&st, expect, shot]() {
            st.e2 = expect(false, "阶梯·第 2 下 Esc → 退窗口全屏");
            shot("esc2");
        }});
        steps->push_back({1200, [&st]() {
            const bool all = st.cin && st.cout && st.ain && st.aout && st.sin && st.sout
                             && st.fin && st.e1 && st.e2;
            printf("%s 全屏播放页三样式 F11 闭环 + ESC 阶梯（退播放页→退全屏）\n",
                   all ? "[PASS]" : "[FAIL]");
            fflush(stdout);
            finishSelfTest(all ? 0 : 4);
        }});
        auto idx = std::make_shared<size_t>(0);
        auto run = std::make_shared<std::function<void()>>();
        *run = [steps, idx, run, &app]() {
            if (*idx >= steps->size()) return;
            const Step s = (*steps)[*idx];
            ++(*idx);
            QTimer::singleShot(s.first, &app, [s, run]() { s.second(); (*run)(); });
        };
        QTimer::singleShot(1500, &app, [run]() { (*run)(); });
    }
#endif // MUYUN_SELFTES

    // 歌单广场提速自检（四-59，**需要网络**）：验证"缓存命中＝同步秒回"这条提速的核心行为
    //   ① 首次拉取（wy/全部/hot）走网络 → 必须拿到非空结果（否则缓存无从谈起）
    //   ② 同参数第二次调用 → exploreReady 必须**在调用返回前**就发出（说明命中缓存、没走网络）
    //   ③ warmExplorePlatforms() 预热后切另一个平台（tx）→ 同样同步秒回
    // 用法：MuyunMusic.exe --test-explore-fast
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-explore-fast"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        printf("=== 歌单广场提速自检 ===\n");
        static int results = 0;
        static int lastSize = 0;
        QObject::connect(home, &Muyun::HomeController::exploreReady, home,
                         [](const QVariantList &l, const QString &, int, bool) {
                             ++results;
                             lastSize = l.size();
                         });
        static bool netOk = false, cacheOk = false, warmOk = false;
        QTimer::singleShot(600, &app, [home]() {
            results = 0;
            home->loadExplorePlaylists(QStringLiteral("wy"), QStringLiteral("全部"),
                                       QStringLiteral("hot"), 1);
        });
        QTimer::singleShot(5000, &app, [home]() {
            netOk = results >= 1 && lastSize > 0;
            printf("[%s] 首次拉取 wy/全部/hot：回调 %d 次、条数 %d\n",
                   netOk ? "PASS" : "FAIL", results, lastSize);
            // ② 同参数再来一次：命中缓存 → 信号在**调用内**同步发出
            const int before = results;
            QElapsedTimer t; t.start();
            home->loadExplorePlaylists(QStringLiteral("wy"), QStringLiteral("全部"),
                                       QStringLiteral("hot"), 1);
            const qint64 dt = t.elapsed();
            cacheOk = (results == before + 1) && dt < 100;
            printf("[%s] 二次同参数：同步命中缓存（调用内回调 %d 次，耗时 %lldms，期望 1 次且 <100ms）\n",
                   cacheOk ? "PASS" : "FAIL", results - before, (long long)dt);
            home->warmExplorePlatforms();   // ③ 并行预热五平台
        });
        QTimer::singleShot(11000, &app, [home]() {
            const int before = results;
            QElapsedTimer t; t.start();
            home->loadExplorePlaylists(QStringLiteral("tx"), QStringLiteral("全部"),
                                       QStringLiteral("hot"), 1);
            const qint64 dt = t.elapsed();
            warmOk = (results == before + 1) && dt < 100;
            printf("[%s] 预热后切平台 tx：同步命中缓存（调用内回调 %d 次，耗时 %lldms）\n",
                   warmOk ? "PASS" : "FAIL", results - before, (long long)dt);
            const bool all = netOk && cacheOk && warmOk;
            printf("%s 歌单广场提速自检结束\n", all ? "PASS" : "FAIL");
            fflush(stdout);
            finishSelfTest(all ? 0 : 4);
        });
    }
#endif // MUYUN_SELFTES

    // 收藏在线歌单自检（全离线合成数据）：收藏→进侧栏歌单列表、歌曲落库去重；
    // 平台两套串（platformId "netease" 与音源码 "wy"）必须认成同一张；再点一次=取消收藏。
    // 用法：MuyunMusic.exe --test-collect   （建议 MUYUN_STORE_ROOT 隔离）
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-collect"))) {
        // 防呆：本自检会往真实收藏/歌单里写数据。没隔离就跑 = 污染用户数据
        // （2026-10-03 的教训：--test-batch-ui 未隔离裸跑，把用户 47 首收藏整页删光）
        if (qEnvironmentVariableIsEmpty("MUYUN_STORE_ROOT")) {
            printf("[FAIL] 请先设 MUYUN_STORE_ROOT=<临时目录> 再跑（本自检会写收藏/歌单）\n");
            return finishSelfTest(4);
        }
        setvbuf(stdout, nullptr, _IONBF, 0);
        QTimer::singleShot(1200, &app, [library]() {
            auto mk = [](const QString &id, const QString &nm) {
                Muyun::Song s; s.id = id; s.name = nm; s.artist = QStringLiteral("测试歌手");
                s.platform = Muyun::Platform::Netease; s.duration = 180;
                s.hasLx = true; s.lx.source = QStringLiteral("wy"); s.lx.songmid = id;
                return s.toMap();
            };
            QVariantList songs;
            songs << mk(QStringLiteral("70001"), QStringLiteral("收藏测试A"));
            songs << mk(QStringLiteral("70002"), QStringLiteral("收藏测试B"));
            songs << mk(QStringLiteral("70001"), QStringLiteral("收藏测试A重复"));   // 应被去重
            const QString sid = QStringLiteral("987654321");
            QVariantMap pl;
            pl[QStringLiteral("id")] = sid;
            pl[QStringLiteral("name")] = QStringLiteral("暮云测试歌单");
            pl[QStringLiteral("platform")] = QStringLiteral("netease");
            pl[QStringLiteral("cover")] = QStringLiteral("https://example.com/c.jpg");
            pl[QStringLiteral("creator")] = QStringLiteral("测试创建者");

            const int before = library->playlists().size();
            const bool added = library->toggleCollectPlaylist(pl, songs);
            const int after = library->playlists().size();
            const bool byId = library->isPlaylistCollected(QStringLiteral("netease"), sid);
            const bool byCode = library->isPlaylistCollected(QStringLiteral("wy"), sid);
            const QString pid = library->collectedPlaylistId(QStringLiteral("wy"), sid);
            const QVariantList back = library->playlistSongs(pid);
            const bool otherNotMine = !library->isPlaylistCollected(QStringLiteral("netease"),
                                                                   QStringLiteral("000000000"));
            // 换一种平台串再点：应当认成同一张 → 取消收藏
            QVariantMap pl2 = pl;
            pl2[QStringLiteral("platform")] = QStringLiteral("wy");
            const bool toggledOff = library->toggleCollectPlaylist(pl2, songs);
            const int finalCount = library->playlists().size();

            struct { const char *name; bool ok; } cases[] = {
                { "收藏成功", added },
                { "出现在歌单列表（数量 +1）", after == before + 1 },
                { "按 platformId 查到已收藏", byId },
                { "按音源码 wy 也查到（两套串认同一张）", byCode },
                { "歌曲落库且按 identityKey 去重（3 首存 2 首）", back.size() == 2 },
                { "别的歌单没被误判成已收藏", otherNotMine },
                { "再点一次=取消收藏", !toggledOff },
                { "取消后歌单列表回到原样", finalCount == before },
            };
            bool pass = true;
            int nCases = 0;
            for (const auto &c : cases) {
                printf("[%s] %s\n", c.ok ? "PASS" : "FAIL", c.name);
                pass = pass && c.ok;
                ++nCases;
            }
            printf("%s 收藏在线歌单（%d 项）\n", pass ? "[PASS]" : "[FAIL]", nCases);
            fflush(stdout);
            finishSelfTest(pass ? 0 : 4);
        });
    }
#endif // MUYUN_SELFTES

    // 收藏歌单按钮 UI 自检（离线合成，不碰网络）：把界面切到"从广场点进来的在线歌单"，
    // 断言①按钮出现且初始未收藏；②点一下 → 侧栏歌单列表真多了一条、按钮转"已收藏"；
    // ③再点 → 取消收藏、列表回到原样。附截图（列表页头部 + 侧栏）。
    // 用法：MuyunMusic.exe --test-collect-ui   （建议 MUYUN_STORE_ROOT 隔离）
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-collect-ui"))) {
        if (qEnvironmentVariableIsEmpty("MUYUN_STORE_ROOT")) {
            printf("[FAIL] 请先设 MUYUN_STORE_ROOT=<临时目录> 再跑（本自检会写收藏/歌单）\n");
            return finishSelfTest(4);
        }
        setvbuf(stdout, nullptr, _IONBF, 0);
        static QObject *lp = nullptr;
        struct S { int before = 0; bool btn = false, initUn = false, added = false,
                        named = false, nowCol = false, uncol = false, back = false; } st;
        auto songsOf = []() {
            auto mk = [](const QString &id, const QString &nm) {
                Muyun::Song s; s.id = id; s.name = nm; s.artist = QStringLiteral("测试歌手");
                s.platform = Muyun::Platform::Netease; s.duration = 200;
                s.hasLx = true; s.lx.source = QStringLiteral("wy"); s.lx.songmid = id;
                return s.toMap();
            };
            QVariantList l;
            l << mk(QStringLiteral("71001"), QStringLiteral("UI收藏曲一"))
              << mk(QStringLiteral("71002"), QStringLiteral("UI收藏曲二"));
            return l;
        };
        QTimer::singleShot(1500, &app, [mainWin, library, &st, songsOf]() {
            lp = mainWin ? mainWin->findChild<QObject*>("listPageObj") : nullptr;
            if (!lp) { printf("[FAIL] 找不到列表页对象 listPageObj\n"); finishSelfTest(4); return; }
            st.before = library->playlists().size();
            QVariantMap pl;
            pl[QStringLiteral("id")] = QStringLiteral("987654321");
            pl[QStringLiteral("name")] = QStringLiteral("UI测试在线歌单");
            pl[QStringLiteral("platform")] = QStringLiteral("netease");
            mainWin->setProperty("onlinePl", pl);
            mainWin->setProperty("currentPage", QStringLiteral("playlistDetail"));
            mainWin->setProperty("playContextId", QStringLiteral("online-987654321"));
            mainWin->setProperty("listTitle", QStringLiteral("UI测试在线歌单"));
            mainWin->setProperty("songList", songsOf());
            printf("已把界面切到「广场点进来的在线歌单」状态（歌单数=%d）\n", st.before);
            fflush(stdout);
        });
        QTimer::singleShot(2200, &app, [mainWin, &st]() {
            st.btn = lp->property("showCollect").toBool();
            st.initUn = !lp->property("collected").toBool();
            mainWin->grabWindow().save(QDir::tempPath() + QStringLiteral("/collect_ui_before.png"));
            QMetaObject::invokeMethod(lp, "collectRequested");   // 模拟点「收藏歌单」
        });
        QTimer::singleShot(2900, &app, [mainWin, library, &st]() {
            const auto pls = library->playlists();
            st.added = pls.size() == st.before + 1;
            QString nm;
            for (const auto &v : pls) {
                const QVariantMap m = v.toMap();
                if (m.value(QStringLiteral("isOnlineImported")).toBool())
                    nm = m.value(QStringLiteral("name")).toString();
            }
            st.named = (nm == QStringLiteral("UI测试在线歌单"));
            st.nowCol = lp->property("collected").toBool();
            mainWin->grabWindow().save(QDir::tempPath() + QStringLiteral("/collect_ui_after.png"));
            QMetaObject::invokeMethod(lp, "collectRequested");   // 再点 = 取消收藏
        });
        QTimer::singleShot(3600, &app, [library, &st]() {
            st.uncol = !lp->property("collected").toBool();
            st.back = library->playlists().size() == st.before;
            struct { const char *name; bool ok; } cases[] = {
                { "在线歌单页出现「收藏歌单」按钮", st.btn },
                { "初始状态为未收藏", st.initUn },
                { "点一下 → 侧栏歌单列表 +1", st.added },
                { "新歌单名取自在线歌单", st.named },
                { "按钮转为「已收藏」", st.nowCol },
                { "再点 → 取消收藏（按钮回退）", st.uncol },
                { "取消后歌单列表回到原样", st.back },
            };
            bool pass = true;
            int n = 0;
            for (const auto &c : cases) {
                printf("[%s] %s\n", c.ok ? "PASS" : "FAIL", c.name);
                pass = pass && c.ok;
                ++n;
            }
            printf("%s 收藏歌单按钮 UI（%d 项）｜截图 %s/collect_ui_before.png、collect_ui_after.png\n",
                   pass ? "[PASS]" : "[FAIL]", n, qPrintable(QDir::tempPath()));
            fflush(stdout);
            finishSelfTest(pass ? 0 : 4);
        });
    }
#endif // MUYUN_SELFTES

    // 全屏播放页·浅色主题取证：切浅色 → 打开播放页 → 经典/Apple 用**窗口内抓取**
    // （整屏抓会被别的程序挡住），舞台样式抓整屏（舞台是独立置顶窗）。
    // 用法：MuyunMusic.exe --test-page-light
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-page-light"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        static bool pageVisible = false;
        auto shot = [mainWin](const char *name, bool desktop = false) {
            const QString p = QDir::tempPath() + QStringLiteral("/pagelight_") + name + QStringLiteral(".png");
            if (desktop) {
                if (auto *scr = QGuiApplication::primaryScreen()) scr->grabWindow(0).save(p);
            } else if (mainWin) {
                mainWin->grabWindow().save(p);
            }
            printf("    截图 %s\n", qPrintable(p)); fflush(stdout);
        };
        QTimer::singleShot(1200, &app, [theme, settings, mainWin]() {
            theme->setDark(false);
            settings->setPlayerStyle(QStringLiteral("classic"));
            QMetaObject::invokeMethod(mainWin, "openLyricsPage");
        });
        QTimer::singleShot(2600, &app, [mainWin, settings, shot]() {
            if (auto *pg = mainWin->findChild<QObject*>("lyricsPageObj"))
                pageVisible = pg->property("visible").toBool();
            printf("[%s] 浅色·经典样式（播放页可见=%d）\n", pageVisible ? "PASS" : "FAIL",
                   int(pageVisible));
            shot("classic");
            settings->setPlayerStyle(QStringLiteral("amll"));
        });
        QTimer::singleShot(3800, &app, [settings, shot]() {
            printf("[INFO] 浅色·Apple Music 样式\n");
            shot("amll");
            settings->setPlayerStyle(QStringLiteral("mineradio"));
        });
        QTimer::singleShot(8000, &app, [mainWin, settings, theme, shot]() {
            printf("[INFO] 浅色·Mineradio 舞台样式（舞台为独立置顶窗，抓整屏）\n");
            shot("mineradio", true);
            // 收尾：关掉播放页、恢复深色，别把用户环境留在浅色+舞台态
            QMetaObject::invokeMethod(mainWin, "closeLyricsPage");
            settings->setPlayerStyle(QStringLiteral("amll"));
            theme->setDark(true);
        });
        QTimer::singleShot(9500, &app, []() {
            fflush(stdout);
            finishSelfTest(pageVisible ? 0 : 4);
        });
    }
#endif // MUYUN_SELFTES

    // ESC 交页面裁判的端到端自检：开舞台 → stage.sendEsc() 应送达 → 页面回传 back
    // → 播放页关闭、舞台退出。旧实现是主程序拿自己那份（可能过期的）immersive 标志猜阶梯，
    // 标志一过期就变成"每次 Esc 只重发退沉浸指令"，用户看到的就是 ESC 失灵。
    // 用法：MuyunMusic.exe --test-stage-esc
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-stage-esc"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        static bool opened = false, entered = false, onlyImm = false, closed = false,
                    fsOn = false, fsOff = false;
        auto pageVisible = [mainWin]() {
            QObject *pg = mainWin ? mainWin->findChild<QObject*>("lyricsPageObj") : nullptr;
            return pg ? pg->property("visible").toBool() : false;
        };
        auto fullOn = [mainWin]() {
            return mainWin ? mainWin->property("fullScreenOn").toBool() : false;
        };
        auto dump = [mainWin, stage, pageVisible, fullOn](const char *tag) {
            printf("    %s：舞台active=%d immersive=%d 播放页visible=%d 窗口全屏=%d\n", tag,
                   int(stage->active()), int(stage->immersive()), int(pageVisible()), int(fullOn()));
            fflush(stdout);
        };
        auto raiseOwn = [mainWin]() {
            // 自检从控制台被拉起，前台常是 conhost；先把主窗抬到前台（等价于用户点了一下窗口），
            // Esc 系统热键的"前台是我们的窗"判据才会放行——这正是真实使用场景。
#ifdef Q_OS_WIN
            if (!mainWin) return;
            HWND hw = reinterpret_cast<HWND>(mainWin->winId());
            if (!hw) return;
            keybd_event(VK_MENU, 0, 0, 0);                 // 单次 Alt 骗过前台锁
            keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
            const HWND fg = GetForegroundWindow();
            const DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
            const DWORD myTid = GetCurrentThreadId();
            if (fgTid && fgTid != myTid) AttachThreadInput(myTid, fgTid, TRUE);
            BringWindowToTop(hw);
            SetForegroundWindow(hw);
            if (fgTid && fgTid != myTid) AttachThreadInput(myTid, fgTid, FALSE);
            mainWin->requestActivate();
#endif
        };
        auto pressEsc = [hotkey, raiseOwn, &app](const char *tag) {
            raiseOwn();
            // 等 syncEsc 的 300ms 复查周期把注册补上
            QTimer::singleShot(500, &app, [hotkey, tag]() {
                printf("    %s：注入真实 Esc（Esc 系统热键已注册=%d）\n", tag,
                       int(hotkey->escRegistered()));
                fflush(stdout);
                QMetaObject::invokeMethod(hotkey, "injectEsc");
            });
        };
        const QString origStyle = settings->playerStyle();
        // ── 顺序执行器（不再用"绝对时刻定时器"）──
        // 舞台是独立进程 + WebView2，冷启动耗时抖动很大（实测 8 秒固定等待会偶发
        // "舞台还没起来"→ 后面 4 步全部连锁假失败）。改成**轮询等到就绪再继续**，
        // 并且每步做完才排下一步（绝对时刻定时器 + 步骤内泵事件会重入，见四-52 教训）。
        using SStep = std::pair<int, std::function<void()>>;
        auto ssteps = std::make_shared<std::vector<SStep>>();
        auto sidx = std::make_shared<size_t>(0);
        auto srun = std::make_shared<std::function<void()>>();
        *srun = [ssteps, sidx, srun, &app]() {
            if (*sidx >= ssteps->size()) return;
            const SStep s = ssteps->at(*sidx);
            QTimer::singleShot(s.first, &app, [s, sidx, srun, ssteps]() {
                s.second();
                ++(*sidx);
                if (*sidx < ssteps->size()) (*srun)();
            });
        };

        ssteps->push_back({1200, [settings, mainWin]() {
            settings->setPlayerStyle(QStringLiteral("mineradio"));
            QMetaObject::invokeMethod(mainWin, "toggleFullScreen");   // 先窗口全屏（阶梯最后一步要退它）
            QMetaObject::invokeMethod(mainWin, "openLyricsPage");
        }});

        // 起点：轮询等舞台 active（上限 30 秒）。舞台是独立进程 + WebView2，冷启动
        // 实测能到十几秒；固定睡 8 秒就偶发"还没起来"→ 后面 4 步连锁假失败。
        // 只等待、不重复发 open()：反复开关正是历史上"舞台卡死"的成因（见四-25 代际串行化）。
        ssteps->push_back({0, [stage, dump, fullOn, pressEsc]() {
            const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 55000;
            while (!stage->active() && QDateTime::currentMSecsSinceEpoch() < deadline)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            opened = stage->active();
            fsOn = fullOn();
            dump("起点");
            printf("[%s] 舞台已就绪（active=%d 窗口全屏=%d）\n",
                   (opened && fsOn) ? "PASS" : "FAIL", int(opened), int(fsOn));
        }});

        // 稳定期：舞台 active 只代表 WebView2 引擎建好，页面自己的 JS 监听还要一点时间；
        // 太早发 immersive 会被吃掉（旧版靠"固定睡 8 秒"歪打正着，这里显式等 4 秒再发）
        ssteps->push_back({4000, [stage]() { stage->setImmersive(true); }});

        // 进沉浸：轮询等 immersive（引擎刚初始化时第一条回报可能被吃掉，所以边等边补发）
        ssteps->push_back({0, [stage, dump, pageVisible, fullOn, pressEsc]() {
            const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 10000;
            qint64 nextNudge = QDateTime::currentMSecsSinceEpoch() + 1500;
            while (!stage->immersive() && QDateTime::currentMSecsSinceEpoch() < deadline) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                if (now >= nextNudge) { stage->setImmersive(true); nextNudge = now + 1500; }
            }
            entered = stage->immersive() && pageVisible() && fullOn();
            dump("进沉浸后");
            printf("[%s] 已进入沉浸（页面回报一致、窗口仍全屏）\n", entered ? "PASS" : "FAIL");
            pressEsc("阶梯第 1 下");                         // 只该退沉浸
        }});
        ssteps->push_back({2600, [stage, dump, pageVisible, fullOn, pressEsc]() {
            // 必须建立在"确实进过沉浸"之上，否则沉浸没进去时这条会假通过（实测踩过）
            onlyImm = entered && !stage->immersive() && stage->active() && pageVisible() && fullOn();
            dump("ESC 第 1 下之后");
            printf("[%s] 第 1 下只退沉浸（播放页/舞台/全屏都还在）\n", onlyImm ? "PASS" : "FAIL");
            pressEsc("阶梯第 2 下");                         // 该退播放页
        }});
        ssteps->push_back({3200, [stage, dump, pageVisible, fullOn, pressEsc]() {
            closed = !pageVisible() && !stage->active() && fullOn();
            dump("ESC 第 2 下之后");
            printf("[%s] 第 2 下退播放页并收掉舞台（窗口仍全屏）\n", closed ? "PASS" : "FAIL");
            pressEsc("阶梯第 3 下");                         // 该退窗口全屏（用户报的就是这一下）
        }});
        ssteps->push_back({3000, [mainWin, stage, settings, origStyle, dump, fullOn, pageVisible]() {
            dump("ESC 第 3 下之后");
            fsOff = !fullOn();
            printf("[%s] 第 3 下退窗口全屏（回到窗口态）\n", fsOff ? "PASS" : "FAIL");
            settings->setPlayerStyle(origStyle);
            QMetaObject::invokeMethod(mainWin, "closeLyricsPage");   // 兜底收尾
            fflush(stdout);
            finishSelfTest((opened && fsOn && entered && onlyImm && closed && fsOff) ? 0 : 4);
        }});
        (*srun)();
    }
#endif // MUYUN_SELFTES

    // 最大化按钮回归：全屏里点最大化必须"先退全屏"，且还原目标只能是真窗口几何——
    // 旧版把全屏几何当成还原目标记下来，第二次点就"还原到还是那么大"，看着就是按了没反应。
    // 用法：MuyunMusic.exe --test-maximize
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-maximize"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        static int w0 = 0, h0 = 0;
        static bool fs = false, exitFs = false, didMax = false, didRestore = false;
        auto call = [mainWin](const char *m) { QMetaObject::invokeMethod(mainWin, m); };
        auto prop = [mainWin](const char *p) { return mainWin->property(p).toBool(); };
        QTimer::singleShot(1500, &app, [mainWin, &w0, &h0, call]() {
            w0 = mainWin->width(); h0 = mainWin->height();
            printf("基线窗口几何 %dx%d\n", w0, h0);
            call("toggleFullScreen");            // 进 F11 全屏
        });
        QTimer::singleShot(2200, &app, [mainWin, prop, &fs, call]() {
            fs = prop("fullScreenOn");
            printf("[%s] 已进入全屏（fullScreenOn=%d）\n", fs ? "PASS" : "FAIL", int(fs));
            call("toggleMaximize");              // 全屏里点最大化 → 该先退全屏
        });
        QTimer::singleShot(2900, &app, [mainWin, prop, &exitFs, call]() {
            exitFs = !prop("fullScreenOn") && !prop("maximized");
            printf("[%s] 全屏里点最大化：先退回窗口态（fullScreenOn=%d maximized=%d）\n",
                   exitFs ? "PASS" : "FAIL", int(prop("fullScreenOn")), int(prop("maximized")));
            call("toggleMaximize");              // 再点 → 最大化
        });
        QTimer::singleShot(3600, &app, [mainWin, prop, &didMax, call]() {
            const QRect av = mainWin->screen() ? mainWin->screen()->availableGeometry()
                                               : QRect(0, 0, 99999, 99999);
            didMax = prop("maximized") && mainWin->width() >= av.width() - 4
                     && mainWin->height() >= av.height() - 4;
            printf("[%s] 点最大化 → 铺满工作区（%dx%d vs 工作区 %dx%d）\n",
                   didMax ? "PASS" : "FAIL", mainWin->width(), mainWin->height(),
                   av.width(), av.height());
            call("toggleMaximize");              // 再点 → 还原
        });
        QTimer::singleShot(4300, &app, [mainWin, prop, &didRestore, w0, h0]() {
            didRestore = !prop("maximized")
                         && qAbs(mainWin->width() - w0) <= 6 && qAbs(mainWin->height() - h0) <= 6;
            printf("[%s] 再点 → 还原到基线（%dx%d，maximized=%d）\n",
                   didRestore ? "PASS" : "FAIL", mainWin->width(), mainWin->height(),
                   int(prop("maximized")));
            const bool all = fs && exitFs && didMax && didRestore;
            printf("%s 最大化按钮三步闭环\n", all ? "[PASS]" : "[FAIL]");
            fflush(stdout);
            finishSelfTest(all ? 0 : 4);
        });
    }
#endif // MUYUN_SELFTES

    // ESC 阶梯端到端（补了第④⑤步之后）：
    //   ③ 窗口全屏 → 退全屏（回归）  ④ 最大化 → 退最大化（新增）
    //   弹层在场 → Esc 归弹层，阶梯**不许**抢（否则"关菜单"变成"关窗口"）
    //   ⑤ 普通态 → 关闭主窗口（走 root.close() → onClosing 按 exitAction 分流）
    // 用法：MuyunMusic.exe --test-esc-ladder   （必须 MUYUN_STORE_ROOT 隔离：会开弹层/关窗）
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-esc-ladder"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        printf("=== ESC 阶梯自检（含新增两步）===\n");
        if (qEnvironmentVariableIsEmpty("MUYUN_STORE_ROOT")) {
            printf("[FAIL] 必须设 MUYUN_STORE_ROOT 隔离后再跑（会开弹层、触发关窗）\n");
            return finishSelfTest(4);
        }
        if (!mainWin) { printf("[FAIL] 没有主窗口\n"); return finishSelfTest(4); }

        static bool exitFsOk = false, exitMaxOk = false, popupSafeOk = false, closeOk = false;
        static bool escDialogOk = false, lyricsFocusCloseOk = false, stalePopupCloseOk = false;
        static bool didFs = false, didMax = false;
        auto prop = [mainWin](const char *p) { return mainWin->property(p).toBool(); };
        auto call = [mainWin](const char *m) { QMetaObject::invokeMethod(mainWin, m); };
        auto raiseOwn = [mainWin]() {
            // 键要落到我们的窗：先把主窗抬到前台（等价于用户点了一下窗口）
#ifdef Q_OS_WIN
            HWND hw = reinterpret_cast<HWND>(mainWin->winId());
            if (!hw) return;
            keybd_event(VK_MENU, 0, 0, 0);
            keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
            const HWND fg = GetForegroundWindow();
            const DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
            const DWORD myTid = GetCurrentThreadId();
            if (fgTid && fgTid != myTid) AttachThreadInput(myTid, fgTid, TRUE);
            BringWindowToTop(hw);
            SetForegroundWindow(hw);
            if (fgTid && fgTid != myTid) AttachThreadInput(myTid, fgTid, FALSE);
            mainWin->requestActivate();
#endif
        };
        auto pressEsc = [raiseOwn](const char *tag) {
            raiseOwn();
            QTimer::singleShot(400, qApp, [tag]() {
                printf("    %s：注入真实 Esc\n", tag);
                fflush(stdout);
#ifdef Q_OS_WIN
                keybd_event(VK_ESCAPE, 0, 0, 0);
                Sleep(30);
                keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
#endif
            });
        };
        auto findByObj = [mainWin](const char *objName) -> QObject * {
            return mainWin->findChild<QObject*>(QString::fromLatin1(objName));
        };

        using EStep = std::pair<int, std::function<void()>>;
        auto steps = std::make_shared<std::vector<EStep>>();
        auto idx = std::make_shared<size_t>(0);
        auto run = std::make_shared<std::function<void()>>();
        *run = [steps, idx, run]() {
            if (*idx >= steps->size()) return;
            const EStep s = steps->at(*idx);
            QTimer::singleShot(s.first, qApp, [s, idx, run, steps]() {
                s.second();
                ++(*idx);
                if (*idx < steps->size()) (*run)();
            });
        };

        // 起点：把窗口恢复到"窗口态、没最大化、没弹层"
        steps->push_back({600, [mainWin, prop, call]() {
            if (prop("fullScreenOn")) call("toggleFullScreen");
            if (prop("maximized")) call("toggleMaximize");
            QMetaObject::invokeMethod(mainWin, "closeLyricsPage");
            QCoreApplication::processEvents();
            printf("    基线：全屏=%d 最大化=%d 可见=%d\n", int(prop("fullScreenOn")),
                   int(prop("maximized")), int(mainWin->isVisible()));
        }});
        // ③ 进全屏 → 断前置 → 按 Esc → 断后置
        steps->push_back({500, [call]() { call("toggleFullScreen"); }});
        steps->push_back({500, [prop, pressEsc]() {
            didFs = prop("fullScreenOn");
            printf("    第③步前置：确实在窗口全屏里=%d\n", int(didFs));
            pressEsc("阶梯第③步");
        }});
        steps->push_back({900, [prop, call]() {
            exitFsOk = !prop("fullScreenOn");
            printf("[%s] 第③步：Esc 退窗口全屏（现在全屏=%d）\n",
                   (didFs && exitFsOk) ? "PASS" : "FAIL", int(prop("fullScreenOn")));
            call("toggleMaximize");          // 紧接着验第④步
        }});
        // ④ 最大化 → Esc 退出最大化（新增）
        steps->push_back({500, [prop, pressEsc]() {
            didMax = prop("maximized");
            printf("    第④步前置：确实在最大化态=%d\n", int(didMax));
            pressEsc("阶梯第④步");
        }});
        steps->push_back({900, [prop, findByObj]() {
            exitMaxOk = !prop("maximized");
            printf("[%s] 第④步：Esc 退出最大化（新增；现在最大化=%d）\n",
                   (didMax && exitMaxOk) ? "PASS" : "FAIL", int(prop("maximized")));
            if (QObject *panel = findByObj("settingsPanelObj"))
                QMetaObject::invokeMethod(panel, "open");
            QCoreApplication::processEvents();
        }});
        // 弹层在场：Esc 归弹层，阶梯一步都不许动
        steps->push_back({500, [findByObj, pressEsc]() {
            QObject *panel = findByObj("settingsPanelObj");
            printf("    弹层前置：设置面板 visible=%d\n",
                   panel ? int(panel->property("visible").toBool()) : -1);
            pressEsc("弹层在场");
        }});
        steps->push_back({900, [prop, findByObj, mainWin]() {
            popupSafeOk = mainWin->isVisible() && !prop("fullScreenOn") && !prop("maximized");
            printf("[%s] 弹层在场时 Esc 没被阶梯抢走（窗口还在、窗口状态一步没动）\n",
                   popupSafeOk ? "PASS" : "FAIL");
            if (QObject *panel = findByObj("settingsPanelObj"))
                QMetaObject::invokeMethod(panel, "close");
            QCoreApplication::processEvents();
        }});
        // ⑤ 普通态 → 关闭主窗口（onClosing 按 exitAction 分流；隔离档默认"询问"）
        steps->push_back({400, [pressEsc]() { pressEsc("阶梯第⑤步"); }});
        steps->push_back({900, [mainWin, findByObj]() {
            QObject *dlg = findByObj("exitDialogObj");
            const bool asked = dlg && dlg->property("visible").toBool();
            const bool hidden = !mainWin->isVisible();
            closeOk = asked || hidden;
            printf("[%s] 第⑤步：Esc 关闭主窗口（%s）\n", closeOk ? "PASS" : "FAIL",
                   asked ? "弹出退出确认（按设置分流）" : (hidden ? "窗口已关闭/隐藏" : "什么都没发生"));
        }});
        // ⑥ exitDialog 打开时按 Esc → 该关掉确认框，而不是什么都不做（#19 候选病因③）
        steps->push_back({400, []() {
            // 注意：不能像 pressEsc 那样先 raiseOwn() 抬主窗——那会把焦点从确认框
            // 抢回主窗，CloseOnEscape 就不触发了（测试假阴性）。真实场景确认框有焦点，
            // 这里直接注入 Esc。
            printf("    第⑥步：确认框在场（有焦点），注入真实 Esc\n");
            fflush(stdout);
            QTimer::singleShot(400, qApp, []() {
#ifdef Q_OS_WIN
                keybd_event(VK_ESCAPE, 0, 0, 0);
                Sleep(30);
                keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
#endif
            });
        }});
        steps->push_back({900, [mainWin, findByObj]() {
            QObject *dlg = findByObj("exitDialogObj");
            const bool closed = dlg && !dlg->property("visible").toBool();
            const bool winAlive = mainWin->isVisible();
            escDialogOk = closed && winAlive && !mainWin->property("fullScreenOn").toBool()
                          && !mainWin->property("maximized").toBool();
            printf("[%s] 第⑥步：确认框打开时 Esc 关掉确认框（框关=%d 窗口还在=%d）\n",
                   escDialogOk ? "PASS" : "FAIL", int(closed), int(winAlive));
        }});
        // ⑦（#19 断言①）焦点交给自家另一个窗（桌面歌词）→ 主窗没 active →
        //    按 Esc 仍要能关掉主窗（系统热键接管，不再两头都收不到）
        steps->push_back({400, [deskLyrics]() {
            // 不抬主窗：直接把桌面歌词窗抬到前台，模拟用户刚点过它
            deskLyrics->requestActivate();
            printf("    第⑦步前置：桌面歌词窗已抬前台（主窗不再 active）\n");
            fflush(stdout);
            QTimer::singleShot(400, qApp, []() {
                printf("    第⑦步：注入真实 Esc（主窗非前台）\n");
                fflush(stdout);
#ifdef Q_OS_WIN
                keybd_event(VK_ESCAPE, 0, 0, 0);
                Sleep(30);
                keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
#endif
            });
        }});
        steps->push_back({900, [mainWin, findByObj]() {
            QObject *dlg = findByObj("exitDialogObj");
            // 主窗应该真的走了一次"关闭"（exitAction=ask → 弹确认框）
            lyricsFocusCloseOk = dlg && dlg->property("visible").toBool()
                                 && mainWin->isVisible();
            printf("[%s] 第⑦步：焦点在桌面歌词窗时 Esc 仍关得掉主窗（确认框已弹=%d）\n",
                   lyricsFocusCloseOk ? "PASS" : "FAIL", int(lyricsFocusCloseOk));
            // 关掉确认框，回到普通态
            if (dlg && dlg->property("visible").toBool()) {
                QMetaObject::invokeMethod(dlg, "close");
                QCoreApplication::processEvents();
            }
        }});
        // ⑧（#19 断言②）刚 close 掉弹层、紧接着按 Esc → 弹层已退场，Esc 该归阶梯
        steps->push_back({300, [findByObj]() {
            if (QObject *panel = findByObj("settingsPanelObj")) {
                QMetaObject::invokeMethod(panel, "open");
                QCoreApplication::processEvents();
                QTimer::singleShot(200, qApp, [panel]() {
                    QMetaObject::invokeMethod(panel, "close");
                    // 同帧紧接着注入 Esc（弹层刚关，焦点可能还没来得及复位）
                    QTimer::singleShot(80, qApp, []() {
                        printf("    第⑧步：弹层刚关、80ms 后注入 Esc\n");
                        fflush(stdout);
#ifdef Q_OS_WIN
                        keybd_event(VK_ESCAPE, 0, 0, 0);
                        Sleep(30);
                        keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
#endif
                    });
                });
            } else {
                printf("    第⑧步：找不到设置面板，跳过\n");
            }
        }});
        steps->push_back({900, [mainWin, findByObj]() {
            QObject *dlg = findByObj("exitDialogObj");
            stalePopupCloseOk = dlg && dlg->property("visible").toBool();
            printf("[%s] 第⑧步：弹层刚关就按 Esc → 阶梯生效（确认框已弹=%d）\n",
                   stalePopupCloseOk ? "PASS" : "FAIL", int(stalePopupCloseOk));
            const bool all = didFs && exitFsOk && didMax && exitMaxOk && popupSafeOk
                             && closeOk && escDialogOk && lyricsFocusCloseOk
                             && stalePopupCloseOk;
            printf("ESC 阶梯自检结束（%s）\n", all ? "全绿" : "有失败项");
            fflush(stdout);
            finishSelfTest(all ? 0 : 4);
        }});
        (*run)();
    }
#endif // MUYUN_SELFTES

    // 桌面歌词"置顶态悬停取消置顶"自检（用户反馈：置顶后没显示取消置顶按钮）
    //   根因：置顶窗是点击穿透的，收不到 Qt 鼠标事件 → 悬停只能靠 C++ 全局光标轮询。
    //   硬断言：置顶 + 光标移到窗内 → deskLyrics.pinnedHover 变真且 WS_EX_TRANSPARENT 被摘掉
    //   （钮才可点）；光标移出 → pinnedHover 变假且穿透恢复。全程不重建窗口。
    // 用法：MuyunMusic.exe --test-desklyric-pin   （需 MUYUN_STORE_ROOT 隔离）
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-desklyric-pin"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        printf("=== 桌面歌词置顶悬停自检 ===\n");
        if (qEnvironmentVariableIsEmpty("MUYUN_STORE_ROOT")) {
            printf("[FAIL] 必须设 MUYUN_STORE_ROOT 隔离后再跑\n");
            return finishSelfTest(4);
        }
#ifndef Q_OS_WIN
        printf("[SKIP] 非 Windows\n");
        return finishSelfTest(0);
#else
        auto pumpMs = [](int ms) {
            const qint64 end = QDateTime::currentMSecsSinceEpoch() + ms;
            while (QDateTime::currentMSecsSinceEpoch() < end)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        };
        int fail = 0;
        auto check = [&fail](const char *name, bool ok, const QString &d = QString()) {
            printf("[%s] %s%s\n", ok ? "PASS" : "FAIL", name,
                   d.isEmpty() ? "" : qPrintable(QStringLiteral("  （") + d + QStringLiteral("）")));
            if (!ok) ++fail;
        };
        QCursor::setPos(20, 20);   // 先把光标挪到角落，避免上一轮残留位置让 pollHover 立刻解除穿透
        deskLyrics->show();
        deskLyrics->setPinned(true);
        pumpMs(300);
        check("置顶态已建立（pinned=1）", deskLyrics->isPinned());

        auto hwndDl = [deskLyrics]() -> HWND {
            return reinterpret_cast<HWND>(deskLyrics->winIdForTest());
        };
        auto transparentNow = [&hwndDl]() {
            const HWND h = hwndDl();
            if (!h) return false;
            return (GetWindowLongPtr(h, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0;
        };
        check("置顶且光标不在命中区内 → 保持穿透（WS_EX_TRANSPARENT=1）", transparentNow());

        // 光标移到歌词窗中心 → 必须放开穿透、显示取消置顶钮。
        // 这是用户 2026-10-04 二次反馈的核心诉求：鼠标一进入歌词带就要显钮，
        // 不必非要压到那小块按钮上（按钮本身"悬停才出现"，用户根本不知道该往哪移）。
        const QPoint c = deskLyrics->centerGlobalForTest();
        QCursor::setPos(c);
        pumpMs(350);
        check("光标压在歌词带中间 → 显取消置顶钮（pinnedHover=1）",
              deskLyrics->pinnedHover(),
              QStringLiteral("带中 hover=%1").arg(int(deskLyrics->pinnedHover())));
        check("光标压在歌词带中间 → 仍穿透（WS_EX_TRANSPARENT=1，只有钮那块可点）",
              transparentNow());
        // 光标移到「取消置顶」钮上 → 悬停命中 → 放开穿透
        const QPointF btn = deskLyrics->unpinButtonGlobalForTest();
        check("能取到取消置顶钮坐标", !btn.isNull());
        if (!btn.isNull()) {
            QCursor::setPos(btn.toPoint());   // Qt 逻辑坐标，Qt 自己换算成物理坐标
            pumpMs(350);
            check("光标移到钮上 → pinnedHover=1", deskLyrics->pinnedHover(),
                  QStringLiteral("hover=%1").arg(int(deskLyrics->pinnedHover())));
            check("悬停时临时解除穿透（钮可点）", !transparentNow());

            // 放开穿透的区域 = 整窗（用户 2026-10-04 二次反馈：必须"进歌词带就显钮"）
            const QRect hitRect = deskLyrics->unpinButtonHitRectForTest();
            RECT wrc{};
            const bool gotWin = hwndDl() && GetWindowRect(hwndDl(), &wrc);
            const int winW = gotWin ? (int)(wrc.right - wrc.left) : 0;
            check("放开穿透的命中区 ≈ 按钮大小（< 半窗宽）",
                  !hitRect.isNull() && gotWin && hitRect.width() < winW * 3 / 4,
                  QStringLiteral("命中区宽=%1 整窗宽=%2").arg(hitRect.width()).arg(winW));

            // 光标移到远处 → 离开命中区 → 应恢复穿透
            QCursor::setPos(20, 20);
            pumpMs(350);
            check("光标移出命中区 → pinnedHover=0", !deskLyrics->pinnedHover(),
                  QStringLiteral("hover=%1").arg(int(deskLyrics->pinnedHover())));
            check("离开后恢复穿透", transparentNow());
        }

        deskLyrics->hide();
        printf("%s 桌面歌词置顶悬停自检（%d 项失败）\n", fail == 0 ? "[PASS]" : "[FAIL]", fail);
        fflush(stdout);
        return finishSelfTest(fail == 0 ? 0 : 4);
#endif
    }
#endif // MUYUN_SELFTES

    // 桌面歌词"置顶后点『取消置顶』无反应"自检（用户 2026-10-04 报；--test-desklyric-pin 修的是
    //   "钮不显示"，那条只用 QCursor::setPos 挪**软件**光标，只能证明"WS_EX_TRANSPARENT 被摘掉了"，
    //   证明不了"真点击能送到按钮"——这是这次反馈的真问题）。这里分三段验：
    //     ① 命中测试 WindowFromPoint(按钮的屏幕像素) 必须是我们窗口 → 穿透没真解除就点不到
    //     ② 命中通过后把真光标移到钮上、SendInput 真点一下（不过①就跳过，绝不盲点用户界面）
    //     ③ 结果 deskLyrics.isPinned 必须变假
    // 用法：MuyunMusic.exe --test-desklyric-click   （需 MUYUN_STORE_ROOT 隔离）
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-desklyric-click"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        printf("=== 桌面歌词置顶真点击自检 ===\n");
        if (qEnvironmentVariableIsEmpty("MUYUN_STORE_ROOT")) {
            printf("[FAIL] 必须设 MUYUN_STORE_ROOT 隔离后再跑（会用真鼠标在屏幕上点一下）\n");
            return finishSelfTest(4);
        }
#ifndef Q_OS_WIN
        printf("[SKIP] 非 Windows\n");
        return finishSelfTest(0);
#else
        auto pumpMs = [](int ms) {
            const qint64 end = QDateTime::currentMSecsSinceEpoch() + ms;
            while (QDateTime::currentMSecsSinceEpoch() < end)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        };
        int fail = 0;
        auto check = [&fail](const char *name, bool ok, const QString &d = QString()) {
            printf("[%s] %s%s\n", ok ? "PASS" : "FAIL", name,
                   d.isEmpty() ? "" : qPrintable(QStringLiteral("  （") + d + QStringLiteral("）")));
            if (!ok) ++fail;
        };
        auto endTest = [&]() {
            QCursor::setPos(20, 20);
            deskLyrics->hide();
            printf("%s 桌面歌词置顶真点击自检（%d 项失败）\n", fail == 0 ? "[PASS]" : "[FAIL]", fail);
            fflush(stdout);
            return finishSelfTest(fail == 0 ? 0 : 4);
        };
        QCursor::setPos(20, 20);
        deskLyrics->show();
        deskLyrics->setPinned(true);
        pumpMs(350);
        check("置顶态已建立（pinned=1）", deskLyrics->isPinned());

        const HWND h = reinterpret_cast<HWND>(deskLyrics->winIdForTest());
        auto exStyleTxt = [h]() -> QString {
            if (!h) return QStringLiteral("无句柄");
            const LONG_PTR e = GetWindowLongPtr(h, GWL_EXSTYLE);
            return QStringLiteral("EXSTYLE=0x%1 TRANSPARENT=%2 NOACTIVATE=%3 TOPMOST=%4 LAYERED=%5 TOOL=%6")
                .arg(quintptr(e), 0, 16)
                .arg((e & WS_EX_TRANSPARENT) != 0 ? 1 : 0)
                .arg((e & WS_EX_NOACTIVATE) != 0 ? 1 : 0)
                .arg((e & WS_EX_TOPMOST) != 0 ? 1 : 0)
                .arg((e & WS_EX_LAYERED) != 0 ? 1 : 0)
                .arg((e & WS_EX_TOOLWINDOW) != 0 ? 1 : 0);
        };
        printf("    置顶初态 %s\n", qPrintable(exStyleTxt()));

        // ---- 反向穿透：光标**不在**歌词带内时，整窗必须保持穿透（WS_EX_TRANSPARENT=1）
        //   → WindowFromPoint 打在窗中心不应命中本窗，点击漏给下层。
        //   这是"不挡住桌面"的护栏：hover 区之外必须穿透，别整窗永久吃掉桌面点击。
        if (h) {
            RECT rc{}; GetWindowRect(h, &rc);
            const POINT centerPix = { LONG(rc.left + (rc.right - rc.left) / 2),
                                      LONG(rc.top + (rc.bottom - rc.top) / 2) };
            const HWND hitCenter = WindowFromPoint(centerPix);
            check("光标不在歌词带内 → 点击漏给下层（不落在本窗）",
                  hitCenter != h,
                  QStringLiteral("WindowFromPoint=%1 本窗=%2 像素点=%3,%4")
                      .arg(reinterpret_cast<quintptr>(hitCenter), 0, 16)
                      .arg(reinterpret_cast<quintptr>(h), 0, 16)
                      .arg(centerPix.x).arg(centerPix.y));
            const QRect hitRect = deskLyrics->unpinButtonHitRectForTest();
            check("放开穿透的命中区 ≈ 按钮大小（< 半窗宽）",
                  !hitRect.isNull() && hitRect.width() < (rc.right - rc.left) * 3 / 4,
                  QStringLiteral("命中区=%d×%d 整窗=%d×%d")
                      .arg(hitRect.width()).arg(hitRect.height())
                      .arg(rc.right - rc.left).arg(rc.bottom - rc.top));
        }

        // ---- 坐标空间全量诊断：dpr≠1 时 Qt 逻辑坐标与 Win32 物理坐标可能不同域 ----
        // 先不挪光标、不假设任何 Qt 坐标是对的，只看三件事：
        //   ① Win32 眼里这个窗真实在哪（GetWindowRect，物理像素）
        //   ② 在它自己的物理中心做命中测试，命中到的到底是不是它
        //   ③ Qt 的 mapToGlobal 报的位置和 ① 差多少
        if (h) {
            wchar_t cls[64]; GetClassNameW(h, cls, 63);
            RECT rc{}; GetWindowRect(h, &rc);
            const POINT rcCenter = { LONG(rc.left + (rc.right - rc.left) / 2),
                                     LONG(rc.top + (rc.bottom - rc.top) / 2) };
            POINT realCur{}; GetCursorPos(&realCur);
            const QPoint qPos = QCursor::pos();
            QScreen *scr = qApp->primaryScreen();
            QWindow *qw0 = QWindow::fromWinId(reinterpret_cast<WId>(deskLyrics->winIdForTest()));
            const QPoint tlG = qw0 ? qw0->mapToGlobal(QPoint(0, 0)) : QPoint(-1, -1);
            printf("    [坐标] 窗类名=%s\n",
                   QString::fromWCharArray(cls).toLocal8Bit().constData());
            printf("    [坐标] GetWindowRect(物理)=(%d,%d)-(%d,%d) 尺寸=%d×%d 中心=%d,%d\n",
                   rc.left, rc.top, rc.right, rc.bottom,
                   rc.right - rc.left, rc.bottom - rc.top, rcCenter.x, rcCenter.y);
            if (qw0) {
                printf("    [坐标] Qt position=(%d,%d)  geometry=(%d,%d,%d×%d)  mapToGlobal(0,0)=(%d,%d)\n",
                       qw0->position().x(), qw0->position().y(),
                       qw0->geometry().x(), qw0->geometry().y(),
                       qw0->geometry().width(), qw0->geometry().height(),
                       tlG.x(), tlG.y());
            }
            printf("    [坐标] QCursor::pos()(逻辑)=%d,%d  GetCursorPos(物理)=%d,%d\n",
                   qPos.x(), qPos.y(), realCur.x, realCur.y);
            if (scr) {
                printf("    [坐标] 主屏 几何(逻辑)=%d,%d,%d×%d  dpr=%g\n",
                       scr->geometry().x(), scr->geometry().y(),
                       scr->geometry().width(), scr->geometry().height(),
                       scr->devicePixelRatio());
            }
            const qreal dpr0 = scr ? scr->devicePixelRatio() : 1.0;
            const QPoint diff = tlG - QPoint(rc.left, rc.top);
            printf("    [坐标] mapToGlobal(0,0) 与 GetWindowRect 左上角之差 = (%d,%d)  [应为 0 或 0/dpr]\n",
                   diff.x(), diff.y());
            const HWND hitC = WindowFromPoint(rcCenter);
            wchar_t cls2[64]; GetClassNameW(hitC, cls2, 63);
            printf("    [坐标] 命中测试A：WindowFromPoint(窗物理中心 %d,%d) = %p(%s)  期望=%p  一致=%d\n",
                   rcCenter.x, rcCenter.y, reinterpret_cast<void *>(hitC),
                   QString::fromWCharArray(cls2).toLocal8Bit().constData(),
                   reinterpret_cast<void *>(h), int(hitC == h));
            const POINT ptFromQt = { LONG(qRound(tlG.x() * dpr0)), LONG(qRound(tlG.y() * dpr0)) };
            const HWND hitQ = WindowFromPoint(ptFromQt);
            wchar_t cls3[64]; GetClassNameW(hitQ, cls3, 63);
            printf("    [坐标] 命中测试B：WindowFromPoint(mapToGlobal×dpr %d,%d) = %p(%s)  期望=%p  一致=%d\n",
                   ptFromQt.x, ptFromQt.y, reinterpret_cast<void *>(hitQ),
                   QString::fromWCharArray(cls3).toLocal8Bit().constData(),
                   reinterpret_cast<void *>(h), int(hitQ == h));
        }

        // 光标移到按钮上（Qt 逻辑坐标，与 pollHover 读的 QCursor::pos() 同域）
        const QPointF btnGlobal = deskLyrics->unpinButtonGlobalForTest();
        if (btnGlobal.isNull()) {
            printf("[FAIL] 找不到『取消置顶』按钮（unpinBtnObj）\n");
            fail++;
            return endTest();
        }
        printf("    按钮中心(逻辑)=%.1f,%.1f  歌词窗中心=%d,%d\n",
               btnGlobal.x(), btnGlobal.y(),
               deskLyrics->centerGlobalForTest().x(), deskLyrics->centerGlobalForTest().y());
        // 层级诊断：主窗若也带 TOPMOST，就会和置顶的桌面歌词抢同一层；再直接问系统
        // 「压在我窗上面的是谁」，比猜坐标可靠。
        if (h && mainWin) {
            const HWND hm = reinterpret_cast<HWND>(mainWin->winId());
            RECT mr{}; GetWindowRect(hm, &mr);
            const LONG_PTR me = GetWindowLongPtr(hm, GWL_EXSTYLE);
            wchar_t mc[64]; GetClassNameW(hm, mc, 63);
            printf("    [层级] 主窗=%p(%s) EXSTYLE=0x%1lx TOPMOST=%d TRANSPARENT=%d 矩形=(%d,%d,%d,%d)\n",
                   reinterpret_cast<void *>(hm),
                   QString::fromWCharArray(mc).toLocal8Bit().constData(),
                   quintptr(me), int((me & WS_EX_TOPMOST) != 0),
                   int((me & WS_EX_TRANSPARENT) != 0),
                   mr.left, mr.top, mr.right, mr.bottom);
            const HWND prev = GetWindow(h, GW_HWNDPREV);   // 同一个层级带里压在我上面的窗
            if (prev) {
                wchar_t pc[64]; GetClassNameW(prev, pc, 63);
                const LONG_PTR pe = GetWindowLongPtr(prev, GWL_EXSTYLE);
                printf("    [层级] 压在我窗上面的 GW_HWNDPREV=%p(%s) TOPMOST=%d TRANSPARENT=%d 可见=%d\n",
                       reinterpret_cast<void *>(prev),
                       QString::fromWCharArray(pc).toLocal8Bit().constData(),
                       int((pe & WS_EX_TOPMOST) != 0), int((pe & WS_EX_TRANSPARENT) != 0),
                       int(IsWindowVisible(prev)));
            } else {
                printf("    [层级] GW_HWNDPREV=空（我上面没人）\n");
            }
        }
        // 诊断：Qt 内部 flags 是否仍以为窗是"透明"（这是"点击到不了 QML"的嫌疑点）
        if (QWindow *w = QWindow::fromWinId(reinterpret_cast<WId>(deskLyrics->winIdForTest()))) {
            printf("    Qt侧 WindowTransparentForInput=%d  isActive=%d  visible=%d\n",
                   int((w->flags() & Qt::WindowTransparentForInput) != 0),
                   int(w->isActive()), int(w->isVisible()));
            if (QQuickWindow *qw = qobject_cast<QQuickWindow *>(w)) {
                if (QQuickItem *rootItem = qw->contentItem()) {
                    if (QQuickItem *btnItem = rootItem->findChild<QQuickItem*>(QStringLiteral("unpinBtnObj"))) {
                        const QPointF now = btnItem->mapToGlobal(btnItem->boundingRect().center());
                        printf("    钮当前状态: visible=%d enabled=%d 中心现=%g,%g (移动=%g,%g)\n",
                               int(btnItem->isVisible()), int(btnItem->isEnabled()),
                               now.x(), now.y(), now.x() - btnGlobal.x(), now.y() - btnGlobal.y());
                    }
                }
            }
        }
        QCursor::setPos(btnGlobal.toPoint());
        pumpMs(350);   // 越过 100ms 轮询周期
        check("光标在钮上 → pinnedHover=1", deskLyrics->pinnedHover(),
              QStringLiteral("hover=%1").arg(int(deskLyrics->pinnedHover())));
        printf("    悬停态   %s\n", qPrintable(exStyleTxt()));
        const bool notTransparent = h && (GetWindowLongPtr(h, GWL_EXSTYLE) & WS_EX_TRANSPARENT) == 0;
        check("悬停时穿透已摘除（EXSTYLE 的 TRANSPARENT=0）", notTransparent);

        // ① 命中测试：屏幕像素坐标下那个点是不是我们窗口（Qt 逻辑坐标 → 物理像素）
        const qreal dpr = qApp->primaryScreen() ? qApp->primaryScreen()->devicePixelRatio() : 1.0;
        const POINT ptPix = { LONG(qRound(btnGlobal.x() * dpr)), LONG(qRound(btnGlobal.y() * dpr)) };
        const HWND hit = WindowFromPoint(ptPix);
        auto className = [](HWND w) -> QString {
            if (!w) return QStringLiteral("空");
            wchar_t b[64];
            GetClassNameW(w, b, 63);
            return QString::fromWCharArray(b);
        };
        check("命中测试：那个屏幕点落在我窗口上", hit == h,
              QStringLiteral("WindowFromPoint=%1(%2) 期望=%3 dpr=%4 像素点=%5,%6")
                  .arg(reinterpret_cast<quintptr>(hit), 0, 16).arg(className(hit))
                  .arg(reinterpret_cast<quintptr>(h), 0, 16)
                  .arg(dpr, 0, 'f', 2).arg(ptPix.x).arg(ptPix.y));

        // ② 真点击（只在①通过时做，否则会打到用户别的窗口上）
        bool sent = false;
        if (hit == h) {
            QCursor::setPos(btnGlobal.toPoint());
            INPUT in[2] = {};
            in[0].type = INPUT_MOUSE;
            in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            in[1].type = INPUT_MOUSE;
            in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            sent = (SendInput(2, in, sizeof(in[0])) == 2);
        }
        pumpMs(450);
        check("真点击送达 → 已取消置顶（pinned=0）", sent && !deskLyrics->isPinned(),
              QStringLiteral("点下次数=%1 pinned=%2 hover=%3")
                  .arg(int(sent)).arg(int(deskLyrics->isPinned())).arg(int(deskLyrics->pinnedHover())));
        printf("    点击后   %s\n", qPrintable(exStyleTxt()));

        return endTest();
#endif
    }
#endif // MUYUN_SELFTES

    // 输入法上下文守护自检（用户报的已知问题①：焦点不在输入框时切不了中英文）
    // 硬断言：把 activeFocus 交给一个不接受输入法的普通 Item —— Qt 的
    //   QWindowsInputContext::updateEnabled() 会当场 ImmAssociateContext(hwnd, NULL)
    //   把窗口 IME 摘掉；守护必须把它接回来，否则 Ctrl+Space / Shift 根本进不了 IME。
    // 真按键腿：注入一次 Ctrl+Space 看窗口的中英/开关位会不会翻。基线（输入框里）都不翻，
    //   说明本机 IME 没把这个快捷键配上 → 那条只打 [SKIP]，不假绿也不假红。
    // 用法：MuyunMusic.exe --test-ime
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-ime"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        printf("=== 输入法上下文守护自检 ===\n");
        if (qEnvironmentVariableIsEmpty("MUYUN_STORE_ROOT")) {
            printf("[FAIL] 必须设 MUYUN_STORE_ROOT 隔离后再跑（会改焦点与输入法状态）\n");
            return finishSelfTest(4);
        }
#ifndef Q_OS_WIN
        printf("[SKIP] 非 Windows，无此问题\n");
        return finishSelfTest(0);
#else
        if (!mainWin) { printf("[FAIL] 没有主窗口\n"); return finishSelfTest(4); }
        auto hwndMain = [mainWin]() { return reinterpret_cast<HWND>(mainWin->winId()); };
        auto imeContext = [hwndMain]() {
            const HWND h = hwndMain();
            if (!h) return false;
            HIMC c = ImmGetContext(h);
            if (c) ImmReleaseContext(h, c);
            return c != nullptr;
        };
        // 输入法状态 = 开关位 + 转换模式（中英/全半角都体现在这里）
        auto imeState = [hwndMain](quint32 *out) {
            const HWND h = hwndMain();
            if (!h) return false;
            HIMC c = ImmGetContext(h);
            if (!c) return false;
            DWORD flags = 0, sent = 0;
            const BOOL ok = ImmGetConversionStatus(c, &flags, &sent);
            const BOOL opened = ImmGetOpenStatus(c);
            ImmReleaseContext(h, c);
            if (!ok) return false;
            *out = (opened ? 0x10000u : 0u) | quint32(flags);
            return true;
        };
        auto raiseOwn = [mainWin]() {
            const HWND hw = reinterpret_cast<HWND>(mainWin->winId());
            if (!hw) return;
            keybd_event(VK_MENU, 0, 0, 0);
            keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
            const HWND fg = GetForegroundWindow();
            const DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
            const DWORD myTid = GetCurrentThreadId();
            if (fgTid && fgTid != myTid) AttachThreadInput(myTid, fgTid, TRUE);
            BringWindowToTop(hw);
            SetForegroundWindow(hw);
            if (fgTid && fgTid != myTid) AttachThreadInput(myTid, fgTid, FALSE);
            mainWin->requestActivate();
        };
        auto pressCtrlSpace = []() {
            keybd_event(VK_CONTROL, 0, 0, 0);
            Sleep(20);
            keybd_event(VK_SPACE, 0, 0, 0);
            Sleep(20);
            keybd_event(VK_SPACE, 0, KEYEVENTF_KEYUP, 0);
            Sleep(20);
            keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
        };
        auto pumpFor = [](int ms) {
            QElapsedTimer t; t.start();
            while (t.elapsed() < ms)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        };

        // ⚠ 步骤回调在 if 块退出之后才跑：跨回调的变量一律 static，
        //    小工具按**值**捕获（按引用捕获块内局部 lambda 会悬垂，见四-60 教训）
        static bool ctxAtStartup = false, ctxInField = false, ctxAfterBlur = false;
        static bool baselineToggles = false, outsideToggles = false;
        using IStep = std::pair<int, std::function<void()>>;
        auto steps = std::make_shared<std::vector<IStep>>();
        auto idx = std::make_shared<size_t>(0);
        auto run = std::make_shared<std::function<void()>>();
        *run = [steps, idx, run]() {
            if (*idx >= steps->size()) return;
            const IStep s = steps->at(*idx);
            QTimer::singleShot(s.first, qApp, [s, idx, run, steps]() {
                s.second();
                ++(*idx);
                if (*idx < steps->size()) (*run)();
            });
        };

        // ① **启动态先测**：用户报的就是"没点过输入框时切不了"。
        //    先点输入框再测会把坏状态遮掉（Qt 一有文本框拿到焦点就自己把上下文开回来）。
        steps->push_back({700, [mainWin, raiseOwn, imeContext, imeState, pressCtrlSpace,
                                pumpFor]() {
            raiseOwn();
            pumpFor(200);
            ctxAtStartup = imeContext();
            quint32 s0 = 0, s1 = 0;
            const bool got0 = imeState(&s0);
            pressCtrlSpace();
            pumpFor(400);
            const bool got1 = imeState(&s1);
            printf("[%s] 启动后没点过任何输入框：窗口挂着 IME 上下文=%d（读得到输入法状态=%d）\n",
                   (ctxAtStartup && got0) ? "PASS" : "FAIL", int(ctxAtStartup), int(got0));
        }});
        // ② 基线：焦点进搜索框（TextInput），量一次 Ctrl+Space 到底切不切得动
        steps->push_back({200, [mainWin, raiseOwn, imeContext, imeState, pressCtrlSpace,
                                pumpFor]() {
            if (QObject *si = mainWin->findChild<QObject*>(QStringLiteral("searchInputObj")))
                QMetaObject::invokeMethod(si, "forceActiveFocus");
            pumpFor(300);
            ctxInField = imeContext();
            quint32 m0 = 0, m1 = 0;
            const bool got0 = imeState(&m0);
            pressCtrlSpace();
            pumpFor(400);
            const bool got1 = imeState(&m1);
            baselineToggles = got0 && got1 && (m0 != m1);
            printf("[%s] 焦点在搜索框时窗口挂着 IME 上下文\n", ctxInField ? "PASS" : "FAIL");
            printf("     基线（输入框里按 Ctrl+Space）：%08X → %08X，%s\n", m0, m1,
                   baselineToggles ? "有变化" : "无变化");
        }});
        // ③ 把焦点交给不接受输入法的普通 Item：Qt 的 updateEnabled() 会在这里摘掉上下文
        steps->push_back({200, [mainWin, imeContext]() {
            if (QQuickItem *ci = mainWin->contentItem())
                QMetaObject::invokeMethod(ci, "forceActiveFocus");
            QCoreApplication::processEvents();
            printf("     焦点刚交给非输入框 Item 的瞬间：IME 上下文还在=%d\n",
                   int(imeContext()));
        }});
        // ④ 守护必须在 200ms 周期内把它接回来 —— 这条就是修复本身
        steps->push_back({700, [imeContext, raiseOwn, imeState, pressCtrlSpace, pumpFor]() {
            ctxAfterBlur = imeContext();
            printf("[%s] 焦点不在输入框时窗口仍挂着 IME 上下文（守护已接回）\n",
                   ctxAfterBlur ? "PASS" : "FAIL");
            raiseOwn();
            pumpFor(200);
            quint32 m0 = 0, m1 = 0;
            const bool got0 = imeState(&m0);
            pressCtrlSpace();
            pumpFor(400);
            const bool got1 = imeState(&m1);
            outsideToggles = got0 && got1 && (m0 != m1);
            printf("     非输入框焦点按 Ctrl+Space：%08X → %08X，%s\n", m0, m1,
                   outsideToggles ? "有变化" : "无变化");
        }});
        steps->push_back({100, [imeGuard]() {
            int fail = 0;
            if (!ctxAtStartup) {
                printf("[FAIL] 启动后没点过输入框时窗口没有 IME 上下文 → 切换键进不了 IME\n");
                ++fail;
            }
            if (!ctxInField) {
                printf("[FAIL] 基线就不对：焦点在输入框时窗口也没有 IME 上下文\n");
                ++fail;
            }
            if (!ctxAfterBlur) {
                printf("[FAIL] 焦点离开输入框后窗口没有 IME 上下文 → Ctrl+Space 进不了 IME\n");
                ++fail;
            }
            // 对照证据：守护真接回过（MUYUN_NO_IME_GUARD=1 时这里是 0，上面几条会红）
            const int restores = imeGuard->restoreCount();
            printf("[%s] 守护确实把被 Qt 摘掉的 IME 上下文接回来了（累计 %d 次）\n",
                   restores > 0 ? "PASS" : "FAIL", restores);
            if (restores <= 0) ++fail;
            if (baselineToggles && !outsideToggles) {
                printf("[FAIL] 输入框里能切、外面切不了（用户报的现象还在）\n");
                ++fail;
            } else if (!baselineToggles) {
                printf("[SKIP] 本机输入法没把 Ctrl+Space 配成切换键，真按键腿不计入判定\n");
            } else {
                printf("[PASS] 任何焦点下 Ctrl+Space 都能切中英文\n");
            }
            printf("输入法守护自检结束（%d 项失败）\n", fail);
            fflush(stdout);
            finishSelfTest(fail == 0 ? 0 : 4);
        }});
        (*run)();
        // 不 return：步骤靠 app.exec() 的定时器驱动，出口在最后一个步骤里（finishSelfTest）
#endif // Q_OS_WIN
    }
#endif // MUYUN_SELFTES

    // 批量操作·数据层自检（全离线合成）：批量取消收藏 / 批量移出歌单 / 批量下载的跳过规则。
    // 用法：MuyunMusic.exe --test-batch   （建议 MUYUN_STORE_ROOT 隔离）
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-batch"))) {
        // 防呆：本自检会写收藏/歌单/下载队列，没隔离就跑会污染用户真实数据
        if (qEnvironmentVariableIsEmpty("MUYUN_STORE_ROOT")) {
            printf("[FAIL] 请先设 MUYUN_STORE_ROOT=<临时目录> 再跑（本自检会写收藏/歌单）\n");
            return finishSelfTest(4);
        }
        setvbuf(stdout, nullptr, _IONBF, 0);
        QTimer::singleShot(1200, &app, [library, downloads]() {
            auto mk = [](const QString &id, const QString &nm, bool local = false) {
                Muyun::Song s; s.id = id; s.name = nm; s.artist = QStringLiteral("批量歌手");
                s.duration = 200;
                if (local) { s.platform = Muyun::Platform::Local;
                             s.localPath = QStringLiteral("C:/x/") + nm + QStringLiteral(".mp3"); }
                else { s.platform = Muyun::Platform::Netease; s.hasLx = true;
                       s.lx.source = QStringLiteral("wy"); s.lx.songmid = id; }
                return s.toMap();
            };
            QVariantList favs;
            favs << mk("80001", "收藏A") << mk("80002", "收藏B")
                 << mk("80003", "收藏C") << mk("80004", "收藏D");
            QVariantMap seed; seed[QStringLiteral("favorites")] = favs;
            library->importOnlineLibrary(seed);
            const int fav0 = library->favorites().size();
            QStringList fk;
            fk << library->identityOf(favs.at(0).toMap()) << library->identityOf(favs.at(2).toMap());
            const int rmFav = library->removeFavorites(fk);
            const int fav1 = library->favorites().size();
            const bool aGone = !library->isFavorite(favs.at(0).toMap());
            const bool bKept = library->isFavorite(favs.at(1).toMap());

            const QString pid = library->createPlaylist(QStringLiteral("批量测试歌单"));
            QVariantList ps;
            ps << mk("80011", "P1") << mk("80012", "P2") << mk("80013", "P3");
            for (const auto &v : ps) library->addToPlaylist(pid, v.toMap());
            const int pl0 = library->playlistSongs(pid).size();
            QStringList pk;
            pk << library->identityOf(ps.at(0).toMap()) << library->identityOf(ps.at(2).toMap());
            const int rmPl = library->removeSongsFromPlaylist(pid, pk);
            const QVariantList left = library->playlistSongs(pid);
            const bool midKept = left.size() == 1
                && left.at(0).toMap().value(QStringLiteral("name")).toString() == QStringLiteral("P2");
            const int rmGhost = library->removeSongsFromPlaylist(pid, QStringList() << QStringLiteral("没有这个key"));

            QVariantList locals; locals << mk("90001", "本地1", true) << mk("90002", "本地2", true);
            const int addedLocal = downloads->addDownloads(locals);
            QVariantList one; one << mk("80021", "批量在线曲");
            const int items0 = downloads->items().size();
            const int addedOnline = downloads->addDownloads(one);
            const int items1 = downloads->items().size();
            const int addedDup = downloads->addDownloads(one);
            // 带**显式音质**的批量入队（four-56 待办#1）：第二参数必须落到 requestedQuality；
            // 空参=跟随全局默认（上一条用例即覆盖），这条专测"选档"这条腿——断言数据真变了。
            QVariantList one2; one2 << mk("80022", "批量音质曲");
            const int addedQ = downloads->addDownloads(one2, QStringLiteral("128k"));
            QString qApplied;
            for (const auto &v : downloads->items()) {
                const QVariantMap m = v.toMap();
                if (m.value(QStringLiteral("song")).toMap().value(QStringLiteral("name")).toString()
                        == QStringLiteral("批量音质曲"))
                    qApplied = m.value(QStringLiteral("requestedQuality")).toString();
            }
            const bool qualityOk = addedQ == 1 && qApplied == QStringLiteral("128k");
            // 收尾：把刚入队的任务撤掉，别在用户机器上留下下载
            const QVariantList its = downloads->items();
            for (const auto &v : its) {
                const QVariantMap m = v.toMap();
                const QString nm = m.value(QStringLiteral("song")).toMap()
                                      .value(QStringLiteral("name")).toString();
                if (nm == QStringLiteral("批量在线曲") || nm == QStringLiteral("批量音质曲")) {
                    const QString id = m.value(QStringLiteral("id")).toString();
                    downloads->cancelDownload(id);
                    downloads->removeItem(id);
                }
            }
            library->deletePlaylist(pid);

            struct { const char *name; bool ok; } cases[] = {
                { "种子收藏 4 首", fav0 >= 4 },
                { "批量取消收藏返回条数=2", rmFav == 2 },
                { "收藏数 4→2", fav1 == fav0 - 2 },
                { "被选的没了、没选的还在", aGone && bKept },
                { "歌单 3 首", pl0 == 3 },
                { "批量移出返回条数=2 且留下的是中间那首", rmPl == 2 && midKept },
                { "移不存在的 key 返回 0（不误改）", rmGhost == 0 },
                { "本地歌批量下载全部跳过", addedLocal == 0 },
                { "在线歌入队 1 条", addedOnline == 1 && items1 == items0 + 1 },
                { "重复入队被跳过", addedDup == 0 },
                { "批量显式音质落进 requestedQuality=128k", qualityOk },
            };
            bool pass = true; int n = 0;
            for (const auto &c : cases) {
                printf("[%s] %s\n", c.ok ? "PASS" : "FAIL", c.name);
                pass = pass && c.ok; ++n;
            }
            printf("%s 批量操作数据层（%d 项）\n", pass ? "[PASS]" : "[FAIL]", n);
            fflush(stdout);
            finishSelfTest(pass ? 0 : 4);
        });
    }
#endif // MUYUN_SELFTES

    // 批量操作·交互层自检：在收藏页开批量 → 勾选/全选 → 批量移除（走 QML 信号→Main 处理→C++ 落库），
    // 断言"数据真的变了"（收藏条数、勾选条数），并截图工具条。
    // 用法：MuyunMusic.exe --test-batch-ui   （建议 MUYUN_STORE_ROOT 隔离）
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-batch-ui"))) {
        // 防呆（血的教训）：本自检会"整页全选 → 批量移除"，未隔离裸跑 = 直接删用户的收藏
        // （2026-10-03 08:42 实测踩过：用户 47 首收藏被清空，已从洛雪同步快照恢复）
        if (qEnvironmentVariableIsEmpty("MUYUN_STORE_ROOT")) {
            printf("[FAIL] 请先设 MUYUN_STORE_ROOT=<临时目录> 再跑（本自检会批量删收藏）\n");
            return finishSelfTest(4);
        }
        setvbuf(stdout, nullptr, _IONBF, 0);
        static QObject *lp = nullptr;
        static QVariantList seeded;
        struct S { int fav0 = 0, fav1 = 0; bool show = false, modeOn = false, picked2 = false,
                        allOn = false, cleared = false, dlAdded = false;
                   // four-56 批量音质选择
                   bool pillFound = false, popClicked = false, nameShown = false,
                        dlQualityOk = false, qidCleared = false; QString wantName; } st;
        QTimer::singleShot(1500, &app, [mainWin, library, downloads, &st]() {
            lp = mainWin ? mainWin->findChild<QObject*>("listPageObj") : nullptr;
            if (!lp) { printf("[FAIL] 找不到列表页对象\n"); finishSelfTest(4); return; }
            auto mk = [](const QString &id, const QString &nm) {
                Muyun::Song s; s.id = id; s.name = nm; s.artist = QStringLiteral("批量歌手");
                s.platform = Muyun::Platform::Netease; s.duration = 200; s.hasLx = true;
                s.lx.source = QStringLiteral("wy"); s.lx.songmid = id;
                return s.toMap();
            };
            seeded.clear();
            for (int i = 0; i < 6; ++i)
                seeded << mk(QString::number(81000 + i), QStringLiteral("UI批量曲%1").arg(i + 1));
            QVariantMap seed; seed[QStringLiteral("favorites")] = seeded;
            library->importOnlineLibrary(seed);
            st.fav0 = library->favorites().size();
            // "128k" 档位显示名从模型里取（别把 UI 文案写死进断言）
            for (const auto &v : downloads->qualityOptions()) {
                const QVariantMap m = v.toMap();
                if (m.value(QStringLiteral("id")).toString() == QStringLiteral("128k"))
                    st.wantName = m.value(QStringLiteral("name")).toString();
            }
            QMetaObject::invokeMethod(mainWin, "goPage", Q_ARG(QVariant, QStringLiteral("favorites")));
        });
        QTimer::singleShot(2300, &app, [mainWin, &st]() {
            st.show = lp->property("showBatch").toBool();
            lp->setProperty("batchMode", true);
            st.modeOn = lp->property("batchMode").toBool();
            QMetaObject::invokeMethod(lp, "togglePick", Q_ARG(QVariant, seeded.at(0)));
            QMetaObject::invokeMethod(lp, "togglePick", Q_ARG(QVariant, seeded.at(1)));
            // 音质小下拉：工具条里必须真的长出来了，点它要能开弹窗
            QObject *pill = lp->findChild<QObject*>("batchQualityPill");
            st.pillFound = pill != nullptr;
            if (pill) QMetaObject::invokeMethod(pill, "clicked");
            mainWin->grabWindow().save(QDir::tempPath() + QStringLiteral("/batch_ui_picked.png"));
        });
        QTimer::singleShot(2900, &app, [mainWin, lp, library, &st]() {
            // Popup 内容挂 Overlay 不走 QObject 父子链（老坑）→ 沿可视树找 objectName
            QQuickItem *opt = nullptr;
            if (auto *w = qobject_cast<QQuickWindow*>(mainWin)) {
                std::function<void(QQuickItem*)> rec = [&](QQuickItem *it) {
                    if (!opt && it->objectName() == QStringLiteral("batchQOpt")
                        && it->property("qid").toString() == QStringLiteral("128k"))
                        opt = it;
                    for (QQuickItem *c : it->childItems()) rec(c);
                };
                rec(w->contentItem());
            }
            QObject *click = opt ? opt->findChild<QObject*>("batchQOptClick") : nullptr;
            // QQuickMouseArea::clicked 带 QQuickMouseEvent* 参数——无参 invoke 会报
            // "No such method"（信号匹配是**精确签名**，静默不执行）。传 nullptr 即可。
            QQuickMouseEvent *selfTestNoEvent = nullptr;
            if (click)
                QMetaObject::invokeMethod(click, "clicked",
                                          Q_ARG(QQuickMouseEvent*, selfTestNoEvent));
            QObject *pop = lp->findChild<QObject*>("batchQualityPopup");
            st.popClicked = click != nullptr
                            && lp->property("batchQualityId").toString() == QStringLiteral("128k")
                            && pop && !pop->property("visible").toBool();
            const QStringList sel = lp->property("selected").toStringList();
            st.picked2 = sel.size() == 2;
            QMetaObject::invokeMethod(lp, "selectAllVisible");
            const QStringList sel2 = lp->property("selected").toStringList();
            st.allOn = sel2.size() == library->favorites().size();
            QVariant allPicked; 
            QMetaObject::invokeMethod(lp, "allVisiblePicked", Qt::DirectConnection,
                                      Q_RETURN_ARG(QVariant, allPicked));
            st.allOn = st.allOn && allPicked.toBool();
        });
        QTimer::singleShot(3500, &app, [lp, library, downloads, &st]() {
            st.nameShown = lp->property("batchQualityName").toString() == st.wantName;
            QVariant songs;
            QMetaObject::invokeMethod(lp, "selectedSongs", Qt::DirectConnection,
                                      Q_RETURN_ARG(QVariant, songs));
            const int beforeDl = downloads->items().size();
            // 先试批量下载（只拿前两条，避免真下太多），再批量移除。
            // ⚠ 信号签名现在带第二参数（音质 id），发射必须两参——旧单参 invoke 会静默匹配不上。
            QVariantList two = songs.toList();
            if (two.size() > 2) two = two.mid(0, 2);
            QMetaObject::invokeMethod(lp, "batchDownloadRequested",
                                      Q_ARG(QVariant, two), Q_ARG(QString, QStringLiteral("128k")));
            st.dlAdded = downloads->items().size() >= beforeDl;   // 有音源才会入队；无音源只提示不崩
            // 硬断言（"数据真变了"）：确实新增 2 条，且**每条的 requestedQuality 都是 128k**
            int got128 = 0, gotNew = 0;
            for (const auto &v : downloads->items()) {
                const QVariantMap m = v.toMap();
                const QString nm = m.value(QStringLiteral("song")).toMap()
                                      .value(QStringLiteral("name")).toString();
                if (!nm.startsWith(QStringLiteral("UI批量曲"))) continue;
                ++gotNew;
                if (m.value(QStringLiteral("requestedQuality")).toString() == QStringLiteral("128k"))
                    ++got128;
            }
            st.dlQualityOk = gotNew == 2 && got128 == 2;
            QMetaObject::invokeMethod(lp, "batchRemoveRequested", Q_ARG(QVariant, songs));
        });
        QTimer::singleShot(4300, &app, [lp, library, downloads, mainWin, &st]() {
            st.fav1 = library->favorites().size();
            st.cleared = lp->property("selected").toStringList().isEmpty();
            // 清掉刚才可能入队的下载任务，别留痕
            const QVariantList its = downloads->items();
            for (const auto &v : its) {
                const QVariantMap m = v.toMap();
                if (m.value(QStringLiteral("song")).toMap().value(QStringLiteral("name")).toString()
                        .startsWith(QStringLiteral("UI批量曲"))) {
                    const QString id = m.value(QStringLiteral("id")).toString();
                    downloads->cancelDownload(id);
                    downloads->removeItem(id);
                }
            }
            mainWin->grabWindow().save(QDir::tempPath() + QStringLiteral("/batch_ui_after.png"));
            // 退出批量要把音质选择一起清掉（换一批不该沿用上一批的档位）
            QMetaObject::invokeMethod(lp, "exitBatch");
            st.qidCleared = lp->property("batchQualityId").toString().isEmpty();
            const bool removedAll = st.fav1 == st.fav0 - 6;
            printf("%s 收藏 %d → %d（应减 6）\n", removedAll ? "PASS" : "FAIL", st.fav0, st.fav1);
            struct { const char *name; bool ok; } cases[] = {
                { "收藏页出现「批量」入口", st.show },
                { "进入批量模式", st.modeOn },
                { "点两行 → 勾选 2 首", st.picked2 },
                { "全选覆盖整页并回报 allVisiblePicked", st.allOn },
                { "批量移除后收藏条数 -6", removedAll },
                { "移除后勾选清空", st.cleared },
                { "批量音质小下拉入口出现", st.pillFound },
                { "点开弹窗→真点 128k→选中且弹窗收起", st.popClicked },
                { "pill 文案跟随所选档位", st.nameShown },
                { "批量入队 2 条且音质真的=128k", st.dlQualityOk },
                { "退出批量后音质选择复位默认", st.qidCleared },
            };
            bool pass = true; int n = 0;
            for (const auto &c : cases) {
                printf("[%s] %s\n", c.ok ? "PASS" : "FAIL", c.name);
                pass = pass && c.ok; ++n;
            }
            printf("%s 批量操作交互层（%d 项）｜截图 %s/batch_ui_picked.png、batch_ui_after.png\n",
                   pass ? "[PASS]" : "[FAIL]", n, qPrintable(QDir::tempPath()));
            fflush(stdout);
            finishSelfTest(pass ? 0 : 4);
        });
    }
#endif // MUYUN_SELFTES

    // 洛雪同步 UI 冒烟：开同步服务 → 打开设置·同步分区 → grabWindow 截图 → 退出
    // 用法：MuyunMusic.exe --test-lxsync-gui
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-lxsync-gui"))) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        QTimer::singleShot(1500, &app, [sync, mainWin]() {
            sync->setEnabled(true);
            QObject *ro = mainWin;   // 根对象即 Main.qml 窗口
            if (ro)
                QMetaObject::invokeMethod(ro, "openSettings", Q_ARG(QVariant, QStringLiteral("sync")));
        });
        QTimer::singleShot(4000, &app, [mainWin]() {
            const QString path = QDir::tempPath() + QStringLiteral("/lxsync_gui.png");
            const QImage img = mainWin ? mainWin->grabWindow() : QImage();
            const bool saved = !img.isNull() && img.save(path);
            printf("[shot] %s size=%dx%d\n", saved ? qPrintable(path) : "FAIL",
                   img.width(), img.height());
            fflush(stdout);
            finishSelfTest(saved ? 0 : 4);
        });
    }
#endif // MUYUN_SELFTES

    // 更新提示·交互层自检：真弹窗、真点「不再提醒」、真截图。
    //   断言"视图与数据两端都变"：弹窗文案跟着版本走 → 点不再提醒 → 设置文档真落盘 + 弹窗关闭
    //   → 再自动检查不弹（忽略生效）→ 手动检查/更新版本仍会弹 → 「稍后」不写忽略
    //   → 设置·关于区入口与状态行、自动检查开关双向绑定。
    // 清单用本地文件注入（MUYUN_UPDATE_FEED_FILE），全程不联网。
    // 用法：MuyunMusic.exe --test-update-ui   （务必 MUYUN_STORE_ROOT 隔离）
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-update-ui"))) {
        if (qEnvironmentVariableIsEmpty("MUYUN_STORE_ROOT")) {
            printf("[FAIL] 请先设 MUYUN_STORE_ROOT=<临时目录> 再跑（本自检会写更新设置）\n");
            return finishSelfTest(4);
        }
        setvbuf(stdout, nullptr, _IONBF, 0);
        printf("=== 更新提示 UI 自检 ===\n");

        // ⚠ static：步骤回调都在本 if 块退出后才跑，按引用捕获块内局部量会悬垂
        static QString feedPath;
        static struct UpdUI {
            bool entry = false, statusOk = false, dlgOpen = false, textsOk = false;
            bool ignoreSaved = false, closedAfterIgnore = false, noReopen = false;
            bool newerReopens = false, laterKeepsIgnored = false, switchBound = false;
            QString ignored, statusText, shotDlg, shotSettings;
            QString newer, newest;   // 由"本程序当前版本"推导（见下）
        } u;
        feedPath = QDir::tempPath() + QStringLiteral("/muyun_probe/update_feed_ui.json");
        QDir().mkpath(QFileInfo(feedPath).absolutePath());
        // 夹具版本号从"本程序当前版本"推导，不写死 1.2.0：写死的话等程序升到 1.2.0 时
        // 这条会判成同版本不弹窗，连带后面 4 项一起红（同 --test-update，v1.1.0 时已踩过）。
        u.newer  = bumpLast(UpdateChecker::normalizeVersion(QStringLiteral(MUYUN_VERSION)));
        u.newest = bumpLast(u.newer);
        writeUpdateFeedFile(feedPath, QStringLiteral(
            R"({"version":"%1","notes":"①音效全格式\n②本地时长修复",
                "pageUrl":"https://github.com/x/y/releases/tag/v%1",
                "downloadUrl":"https://github.com/x/y/releases/download/v%1/setup.exe"})")
            .arg(u.newer).toUtf8());
        qputenv("MUYUN_UPDATE_FEED_FILE", feedPath.toLocal8Bit());

        // ⚠ 所有步骤回调都在本 if 块退出之后才跑：块内一律不用"按引用捕获的局部 lambda/局部量"，
        //   需要的东西要么是 main 作用域的指针（mainWin/updater），要么是 static（u/feedPath），
        //   或者是文件级静态 helper（findUiItem/pumpFor/waitCheckIdle/clickMouseAreaObj）——
        //   否则栈帧被 app.exec() 的深调用链覆盖 → 悬垂（本项目真实踩过）。

        using Step = std::pair<int, std::function<void()>>;
        auto steps = std::make_shared<std::vector<Step>>();
        auto idx = std::make_shared<size_t>(0);
        auto runStep = std::make_shared<std::function<void()>>();
        *runStep = [steps, idx, runStep, &app]() {
            if (*idx >= steps->size()) return;
            const Step st = steps->at(*idx);
            QTimer::singleShot(st.first, &app, [st, idx, runStep, steps]() {
                st.second();
                ++(*idx);
                if (*idx < steps->size()) (*runStep)();
            });
        };

        // ---------- 步骤 1：到期自动检查 → 弹窗按真实版本弹出 ----------
        steps->push_back({1500, [mainWin, updater]() {
            updater->clearIgnoredVersion();
            updater->setAutoCheckEnabled(true);
            setLastCheckAgoMs(25LL * 3600 * 1000);
            updater->autoCheck();
            waitCheckIdle(updater, 5000);
        }});

        // ---------- 步骤 2：弹窗文案 + 点「不再提醒」 → 落盘 + 关闭 ----------
        steps->push_back({500, [mainWin, updater]() {
            auto *dlg = mainWin ? mainWin->findChild<QObject *>(QStringLiteral("appUpdateDialog"))
                                : nullptr;
            u.dlgOpen = dlg && dlg->property("visible").toBool();
            QObject *title = findUiItem(mainWin, QStringLiteral("updateTitleText"));
            QObject *notes = findUiItem(mainWin, QStringLiteral("updateNotesText"));
            const QString titleText = title ? title->property("text").toString() : QString();
            const QString notesText = notes ? notes->property("text").toString() : QString();
            u.textsOk = titleText.contains(u.newer)
                        && notesText.contains(QStringLiteral("音效全格式"));
            if (mainWin) {
                u.shotDlg = QDir::tempPath() + QStringLiteral("/update_dialog.png");
                if (!mainWin->grabWindow().save(u.shotDlg)) u.shotDlg.clear();
            }
            printf("[%s] 弹窗打开=%d 标题=\"%s\" 说明含要点=%d 截图=%s\n",
                   (u.dlgOpen && u.textsOk) ? "PASS" : "FAIL", int(u.dlgOpen),
                   u8(titleText), int(u.textsOk),
                   u.shotDlg.isEmpty() ? "失败" : u8(u.shotDlg));

            clickMouseAreaObj(findUiItem(mainWin, QStringLiteral("updateIgnoreBtn")));
            u.ignored = updater->ignoredVersion();
            u.ignoreSaved = (u.ignored == u.newer);
            u.closedAfterIgnore = dlg && !dlg->property("visible").toBool();
            printf("[%s] 点「不再提醒」：存档=%s 弹窗已关=%d\n",
                   (u.ignoreSaved && u.closedAfterIgnore) ? "PASS" : "FAIL",
                   u8(u.ignored), int(u.closedAfterIgnore));
        }});

        // ---------- 步骤 3：忽略生效 → 再自动检查不弹 ----------
        steps->push_back({400, [mainWin, updater]() {
            setLastCheckAgoMs(25LL * 3600 * 1000);
            updater->autoCheck();
            waitCheckIdle(updater, 5000);
            auto *dlg = mainWin ? mainWin->findChild<QObject *>(QStringLiteral("appUpdateDialog"))
                                : nullptr;
            u.noReopen = dlg && !dlg->property("visible").toBool()
                         && updater->status() == QStringLiteral("ignored");
            printf("[%s] 忽略后自动检查：弹窗仍关=%d 状态=\"%s\"\n",
                   u.noReopen ? "PASS" : "FAIL",
                   dlg ? int(!dlg->property("visible").toBool()) : -1,
                   u8(updater->statusText()));
        }});

        // ---------- 步骤 4：出更新版本 → 手动检查仍弹；点「稍后」不写忽略 ----------
        steps->push_back({400, [mainWin, updater]() {
            writeUpdateFeedFile(feedPath, QStringLiteral(
                R"({"version":"%1","notes":"新版本说明","pageUrl":"https://github.com/x/y/releases/tag/v%1"})")
                .arg(u.newest).toUtf8());
            updater->checkForUpdates();
            waitCheckIdle(updater, 5000);
            auto *dlg = mainWin ? mainWin->findChild<QObject *>(QStringLiteral("appUpdateDialog"))
                                : nullptr;
            QObject *title = findUiItem(mainWin, QStringLiteral("updateTitleText"));
            const QString titleText = title ? title->property("text").toString() : QString();
            u.newerReopens = dlg && dlg->property("visible").toBool()
                             && titleText.contains(u.newest);
            printf("[%s] 出现 %s → 手动检查弹窗：可见=%d 标题=\"%s\"\n",
                   u.newerReopens ? "PASS" : "FAIL", u8(u.newest),
                   dlg ? int(dlg->property("visible").toBool()) : -1, u8(titleText));

            clickMouseAreaObj(findUiItem(mainWin, QStringLiteral("updateLaterBtn")));
            u.laterKeepsIgnored = dlg && !dlg->property("visible").toBool()
                                  && updater->ignoredVersion() == u.newer;
            printf("[%s] 点「稍后」：弹窗关闭且不写忽略（仍=%s）\n",
                   u.laterKeepsIgnored ? "PASS" : "FAIL", u8(updater->ignoredVersion()));
        }});

        // ---------- 步骤 5：设置·关于区的入口 / 状态行 / 自动检查开关 ----------
        steps->push_back({400, [mainWin, updater]() {
            QMetaObject::invokeMethod(mainWin, "openSettings",
                                      Q_ARG(QVariant, QVariant(QStringLiteral("about"))));
            pumpFor(600);
            u.entry = findUiItem(mainWin, QStringLiteral("updateCheckBtn")) != nullptr;
            QObject *st = findUiItem(mainWin, QStringLiteral("updateStatusText"));
            u.statusText = st ? st->property("text").toString() : QString();
            u.statusOk = u.statusText.contains(u.newest);
            u.shotSettings = QDir::tempPath() + QStringLiteral("/update_settings.png");
            if (mainWin && !mainWin->grabWindow().save(u.shotSettings)) u.shotSettings.clear();
            printf("[%s] 设置·关于：入口=%d 状态行=\"%s\" 截图=%s\n",
                   (u.entry && u.statusOk) ? "PASS" : "FAIL", int(u.entry),
                   u8(u.statusText),
                   u.shotSettings.isEmpty() ? "失败" : u8(u.shotSettings));

            // 自动检查开关：模型改 → 两态胶囊跟着亮（binding 活；不合成点击编译型控件）
            auto pillSel = [mainWin](const QString &label, bool *seen) {
                bool sel = false, found = false;
                for (QObject *p : findUiItems(mainWin, QStringLiteral("updateAutoSwitch"))) {
                    if (p->property("modelData").toMap().value(QStringLiteral("name")).toString()
                        == label) {
                        sel = p->property("isSel").toBool();
                        found = true;
                    }
                }
                if (seen) *seen = found;
                return sel;
            };
            updater->setAutoCheckEnabled(false);
            pumpFor(150);
            bool onSeen1 = false, offSeen1 = false;
            const bool offSel = pillSel(QStringLiteral("关"), &offSeen1);
            const bool onSel = pillSel(QStringLiteral("开"), &onSeen1);
            updater->setAutoCheckEnabled(true);
            pumpFor(150);
            bool onSeen2 = false, offSeen2 = false;
            const bool onSel2 = pillSel(QStringLiteral("开"), &onSeen2);
            const bool offSel2 = pillSel(QStringLiteral("关"), &offSeen2);
            u.switchBound = offSeen1 && onSeen1 && onSeen2 && offSeen2
                            && offSel && !onSel && onSel2 && !offSel2;
            printf("[%s] 自动检查开关双向绑定：关→(关亮=%d 开亮=%d) 开→(开亮=%d 关亮=%d)\n",
                   u.switchBound ? "PASS" : "FAIL", int(offSel), int(onSel), int(onSel2),
                   int(offSel2));
        }});

        // ---------- 步骤 6：收尾汇总 ----------
        steps->push_back({300, [mainWin, updater]() {
            Q_UNUSED(mainWin)
            Q_UNUSED(updater)
            const bool pass = u.entry && u.statusOk && u.dlgOpen && u.textsOk && u.ignoreSaved
                              && u.closedAfterIgnore && u.noReopen && u.newerReopens
                              && u.laterKeepsIgnored && u.switchBound;
            printf("%s 汇总：弹窗=%d 文案=%d 不再提醒落盘=%d 忽略后不弹=%d 新版再弹=%d "
                   "稍后不忽略=%d 设置入口=%d 开关绑定=%d\n",
                   pass ? "PASS" : "FAIL", int(u.dlgOpen && u.textsOk), int(u.textsOk),
                   int(u.ignoreSaved), int(u.noReopen), int(u.newerReopens),
                   int(u.laterKeepsIgnored), int(u.entry && u.statusOk), int(u.switchBound));
            if (!u.shotDlg.isEmpty()) printf("    截图 %s\n", u8(u.shotDlg));
            if (!u.shotSettings.isEmpty()) printf("    截图 %s\n", u8(u.shotSettings));
            fflush(stdout);
            finishSelfTest(pass ? 0 : 4);
        }});

        (*runStep)();
    }
#endif // MUYUN_SELFTES

    // 音效 UI 自检：入口从"三个点"换成推子图标+文字后，界面必须如实反映生效状态
    //   断言（都是"数据/文案确实变了"，不是"没报错"）：
    //     1) 播放条出现新的音效入口（objectName=fxEntryObj，且文案是"音效"而非空）
    //     2) 开 EQ 播 MP3 → player.effectsLive=true → 面板状态行含"已生效"
    //     3) 换非 MP3 音源 → effectsBypassed=true → 入口文案变"音效未生效"
    //     4) 全关音效 → effectsOn=false（入口不再高亮）
    //   同时 grabWindow 截图，便于人眼复核
    // 用法：MuyunMusic.exe --test-effects-ui [mp3] [非mp3]   （务必 MUYUN_STORE_ROOT 隔离）
#ifdef MUYUN_SELFTES
    if (args.contains(QStringLiteral("--test-effects-ui"))) {
        if (qEnvironmentVariableIsEmpty("MUYUN_STORE_ROOT")) {
            printf("[FAIL] 请先设 MUYUN_STORE_ROOT=<临时目录> 再跑（本自检会写音效设置）\n");
            return finishSelfTest(4);
        }
        setvbuf(stdout, nullptr, _IONBF, 0);
        printf("=== 音效 UI 自检 ===\n");
        const int ei = args.indexOf(QStringLiteral("--test-effects-ui"));
        // ⚠ static：所有步骤回调都在这个 if 块退出之后才被事件循环调用，
        //   按引用捕获块内局部变量会悬垂（实测把路径读成乱码 → 误报"音频文件不存在"）
        static QString mp3, other;
        mp3 = (ei + 1 < args.size() && !args.at(ei + 1).startsWith(QStringLiteral("--")))
                  ? args.at(ei + 1) : QString();
        other = (ei + 2 < args.size() && !args.at(ei + 2).startsWith(QStringLiteral("--")))
                  ? args.at(ei + 2) : QString();
        auto magic = [](const QString &p) {
            QFile f(p);
            if (!f.open(QIODevice::ReadOnly)) return QByteArray();
            const QByteArray h = f.read(4); f.close(); return h;
        };
        const QDir cache(PlayerEngine::audioCacheDir());
        if (mp3.isEmpty() || !magic(mp3).startsWith("ID3")) {
            mp3.clear();
            for (const QFileInfo &fi : cache.entryInfoList(QDir::Files, QDir::Time)) {
                if (fi.size() < 300000) continue;
                if (magic(fi.absoluteFilePath()).startsWith("ID3")) { mp3 = fi.absoluteFilePath(); break; }
            }
        }
        if (other.isEmpty() || !magic(other).startsWith("fLaC")) {
            // 自检要自己校验参数前提：回归脚本会给每个用例都传两个 mp3，
            // 把 mp3 当"非 MP3 素材"必然假失败 → 忽略错参数，自己去缓存里找 flac
            const QString given = other;
            other.clear();
            for (const QFileInfo &fi : cache.entryInfoList(QDir::Files, QDir::Time)) {
                if (fi.size() < 300000) continue;
                const QString p = fi.absoluteFilePath();
                if (p == mp3 || p == given) continue;
                if (magic(p).startsWith("fLaC")) { other = p; break; }
            }
        }
        if (mp3.isEmpty()) { printf("[SKIP] 没有可用 mp3 素材\n"); return finishSelfTest(0); }
        printf("[INFO] mp3=%s\n[INFO] 非mp3=%s\n", qPrintable(mp3),
               qPrintable(other.isEmpty() ? QStringLiteral("(无)") : other));

        static struct UI {
            bool entry = false, live = false, panelOpen = false, fader = false;
            bool spBound = false, resetBtn = false, noScroll = false, modesDistinct = false;
            bool flacLive = false, bypass = false;
            bool revSwitchFound = false, revLiveBefore = false, revOn = false, revOff = false;
            bool onAfterOff = true;
            QString entryText0, entryTextBypass, entryTextFlac, stateLive, infoFlac, stateOff;
            QStringList shots;
        } ui;

        static auto localSong = [](const QString &p, const QString &nm) {
            QVariantMap s;
            s[QStringLiteral("id")] = QStringLiteral("fxui");
            s[QStringLiteral("name")] = nm;
            s[QStringLiteral("artist")] = QStringLiteral("QA");
            s[QStringLiteral("platform")] = QStringLiteral("local");
            s[QStringLiteral("localPath")] = p;
            return s;
        };

        // Popup 的内容挂在 Overlay 上，window->findChildren 走 QObject 父子链找不到
        // （实测 1660 个子对象 0 命中）→ 一律沿可视树递归找
        auto findInUI = [mainWin](const char *name) -> QObject * {
            QObject *hit = nullptr;
            std::function<void(QQuickItem*)> rec = [&](QQuickItem *it) {
                if (!it || hit) return;
                if (it->objectName() == QLatin1String(name)) { hit = it; return; }
                for (QQuickItem *c : it->childItems()) rec(c);
            };
            if (auto *w = qobject_cast<QQuickWindow*>(mainWin)) rec(w->contentItem());
            return hit;
        };
        auto waitABit = [](int ms) {
            const qint64 end = QDateTime::currentMSecsSinceEpoch() + ms;
            while (QDateTime::currentMSecsSinceEpoch() < end)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        };
        auto waitPlaying = [&](int timeoutMs) {
            const qint64 end = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
            while (player->property("isPlaying").toBool() == false
                   && QDateTime::currentMSecsSinceEpoch() < end)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        };

        QObject::connect(player, &PlayerController::playFailed,
                         [](const QString &m) { printf("     [playFailed] %s\n", qPrintable(m)); });

        // 顺序执行器：每步"先等若干毫秒，再执行"，**下一步只在上一步真正做完之后才排队**。
        // 之前用一堆绝对时刻 singleShot + 步骤内部 waitABit 泵事件，导致后面的步骤被重入提前跑，
        // 断言在错误的状态下取值 → 单独跑全绿、进回归就假失败（就是这次的现象）。
        using Step = std::pair<int, std::function<void()>>;
        auto steps = std::make_shared<std::vector<Step>>();
        auto idx = std::make_shared<size_t>(0);
        auto runStep = std::make_shared<std::function<void()>>();

        *runStep = [steps, idx, runStep, &app]() {
            if (*idx >= steps->size()) return;
            const Step st = steps->at(*idx);
            QTimer::singleShot(st.first, &app, [st, idx, runStep, steps]() {
                st.second();
                ++(*idx);
                if (*idx < steps->size()) (*runStep)();
            });
        };

        // ---------- 步骤 1：入口 + 播放模式图标互不相同 ----------
        steps->push_back({1500, [&]() {
            QObject *entry = mainWin ? mainWin->findChild<QObject*>("fxEntryObj") : nullptr;
            ui.entry = entry != nullptr;
            if (auto *t = mainWin->findChild<QObject*>("fxEntryTextObj"))
                ui.entryText0 = t->property("text").toString();
            printf("[%s] 播放条音效入口存在=%d 文案=\"%s\"\n",
                   (ui.entry && ui.entryText0 == QStringLiteral("音效")) ? "PASS" : "FAIL",
                   int(ui.entry), qPrintable(ui.entryText0));

            // 四种播放模式图标必须互不相同（用户反馈：顺序与列表循环长得一样，看不出当前模式）
            QObject *btn = findInUI("playModeBtn");
            QSet<QString> seen; QStringList names;
            if (btn) {
                for (int i = 0; i < 4; ++i) {
                    waitABit(60);
                    const QString n = btn->property("name").toString();
                    names << n; seen.insert(n);
                    const QString shot = QDir::tempPath()
                        + QStringLiteral("/fx_mode_%1_%2.png").arg(i).arg(n);
                    if (mainWin && mainWin->grabWindow().save(shot)) ui.shots << shot;
                    QMetaObject::invokeMethod(player, "cyclePlayMode");
                }
            }
            ui.modesDistinct = (seen.size() == 4);
            printf("[%s] 播放模式图标互不相同：%s（去重后 %d/4）\n",
                   ui.modesDistinct ? "PASS" : "FAIL",
                   qPrintable(names.join(QStringLiteral(" → "))), seen.count());

            QMetaObject::invokeMethod(player, "setEqEnabled", Q_ARG(bool, true));
            QMetaObject::invokeMethod(player, "playSong",
                                      Q_ARG(QVariantMap, localSong(mp3, QStringLiteral("音效UI曲"))),
                                      Q_ARG(QVariantList, QVariantList{ localSong(mp3, QStringLiteral("音效UI曲")) }));
        }});

        // ---------- 步骤 2：等起播 → 开面板 → 状态行 ----------
        steps->push_back({3500, [&]() {
            waitPlaying(3000);
            ui.live = player->property("effectsLive").toBool();
            QMetaObject::invokeMethod(mainWin, "openEffects");
        }});
        steps->push_back({700, [&]() {
            QObject *panel = mainWin ? mainWin->findChild<QObject*>("effectsPanelObj") : nullptr;
            ui.panelOpen = panel && panel->property("visible").toBool();
            ui.stateLive = panel ? panel->property("fxStateText").toString() : QString();
            const QString p = QDir::tempPath() + QStringLiteral("/fx_ui_live.png");
            if (mainWin && mainWin->grabWindow().save(p)) ui.shots << p;
            printf("[%s] effectsLive=%d 面板可见=%d 状态行=\"%s\"\n",
                   (ui.live && ui.panelOpen && ui.stateLive.contains(QStringLiteral("已生效"))) ? "PASS" : "FAIL",
                   int(ui.live), int(ui.panelOpen), qPrintable(ui.stateLive));

            // 推子数据往返：控制器改值 → 自绘推子必须跟着动（防"界面和数据两套皮"）
            QMetaObject::invokeMethod(player, "setEqGain", Q_ARG(int, 2), Q_ARG(double, 6.0));
            waitABit(80);
            double uiVal = -99;
            {
                QObject *band = findInUI("eqBand");
                std::function<void(QQuickItem*)> rec = [&](QQuickItem *it) {
                    if (!it) return;
                    if (it->objectName() == QLatin1String("eqBand")
                        && it->property("idx").toInt() == 2)
                        uiVal = it->property("val").toDouble();
                    for (QQuickItem *c : it->childItems()) rec(c);
                };
                if (auto *w = qobject_cast<QQuickWindow*>(mainWin)) rec(w->contentItem());
                Q_UNUSED(band)
            }
            double gv = 0;
            QMetaObject::invokeMethod(player, "eqGain", Q_RETURN_ARG(double, gv), Q_ARG(int, 2));
            ui.fader = (gv == 6.0 && uiVal == 6.0);
            printf("[%s] 第3段推到 +6dB：控制器=%g 界面推子=%g\n",
                   ui.fader ? "PASS" : "FAIL", gv, uiVal);
            QMetaObject::invokeMethod(player, "setEqGain", Q_ARG(int, 2), Q_ARG(double, 0.0));

            // 环绕滑杆必须显示模型真值。旧版写死 value:50 → 重启看着像"被恢复默认"，
            // 而且一动另一个滑杆就把存档覆盖成 50（显示假 + 数据被毁）
            QMetaObject::invokeMethod(player, "setSpatialParams",
                                      Q_ARG(double, 31.0), Q_ARG(double, 73.0));
            waitABit(80);
            QObject *rs = findInUI("spRadiusSlider");
            QObject *ss = findInUI("spSpeedSlider");
            const double rv = rs ? rs->property("value").toDouble() : -1;
            const double sv = ss ? ss->property("value").toDouble() : -1;
            ui.spBound = qFuzzyCompare(rv, 31.0) && qFuzzyCompare(sv, 73.0);
            printf("[%s] 环绕滑杆绑真值：半径=%g 速度=%g（不是写死的 50）\n",
                   ui.spBound ? "PASS" : "FAIL", rv, sv);

            // 「恢复默认」：参数回位、开关不动
            const bool eqOnBefore = player->property("eqEnabled").toBool();
            QMetaObject::invokeMethod(player, "resetAllEffects");
            waitABit(80);
            const double rv2 = rs ? rs->property("value").toDouble() : -1;
            double gv2 = 0;
            QMetaObject::invokeMethod(player, "eqGain", Q_RETURN_ARG(double, gv2), Q_ARG(int, 0));
            ui.resetBtn = qFuzzyCompare(rv2, 50.0) && qFuzzyCompare(gv2, 0.0)
                          && (player->property("eqEnabled").toBool() == eqOnBefore);
            printf("[%s] 点「恢复默认」：半径回位=%g EQ首段=%g 开关状态未变=%d\n",
                   ui.resetBtn ? "PASS" : "FAIL", rv2, gv2,
                   int(player->property("eqEnabled").toBool()));

            // 拖推子时外层不能跟着滚：验证互斥绑定活着（eqDragging 置位 → interactive 落位）
            {
                QObject *flick = findInUI("panelFlick");
                const bool before = flick ? flick->property("interactive").toBool() : false;
                if (panel) panel->setProperty("eqDragging", true);
                waitABit(60);
                const bool during = flick ? flick->property("interactive").toBool() : true;
                if (panel) panel->setProperty("eqDragging", false);
                waitABit(60);
                const bool after = flick ? flick->property("interactive").toBool() : false;
                ui.noScroll = before && !during && after;
                printf("[%s] 拖推子时面板不跟随滚动：idle可滚=%d 按住时禁滚=%d 松手恢复=%d\n",
                       ui.noScroll ? "PASS" : "FAIL", int(before), int(!during), int(after));
            }

            // 换 FLAC：也应走音效管线（"所有格式都能开音效"的界面证据）
            if (!other.isEmpty()) {
                QMetaObject::invokeMethod(player, "playSong",
                                          Q_ARG(QVariantMap, localSong(other, QStringLiteral("音效UI曲FLAC"))),
                                          Q_ARG(QVariantList, QVariantList{ localSong(other, QStringLiteral("音效UI曲FLAC")) }));
            }
        }});

        // ---------- 步骤 3：FLAC 生效 + 坏文件旁路 ----------
        steps->push_back({2600, [&]() {
            if (!other.isEmpty()) {
                ui.flacLive = player->property("effectsLive").toBool();
                ui.infoFlac = player->property("effectDeviceInfo").toString();
                if (auto *t = mainWin->findChild<QObject*>("fxEntryTextObj"))
                    ui.entryTextFlac = t->property("text").toString();
                const QString p = QDir::tempPath() + QStringLiteral("/fx_ui_flac.png");
                if (mainWin && mainWin->grabWindow().save(p)) ui.shots << p;
                printf("[%s] FLAC：effectsLive=%d 入口文案=\"%s\" 设备行=\"%s\"\n",
                       (ui.flacLive && ui.entryTextFlac == QStringLiteral("音效")) ? "PASS" : "FAIL",
                       int(ui.flacLive), qPrintable(ui.entryTextFlac), qPrintable(ui.infoFlac));
            } else {
                printf("[SKIP] 无第二种格式素材，跳过 FLAC 生效检查\n");
                ui.flacLive = true;
            }

            // 伪音频（有 ID3 头但没有效帧）：解码器都开不了 → 必须旁路并把文案切成"音效未生效"
            const QString bogus = QDir::tempPath() + QStringLiteral("/muyun_fx_ui_bogus.audio");
            QFile bf(bogus);
            if (bf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                QByteArray junk = QByteArrayLiteral("ID3\x03\x00\x00\x00\x00\x00\x00");
                junk.append(QByteArray(256 * 1024, '\x7F'));
                bf.write(junk);
                bf.close();
                QMetaObject::invokeMethod(player, "playSong",
                                          Q_ARG(QVariantMap, localSong(bogus, QStringLiteral("坏文件"))),
                                          Q_ARG(QVariantList, QVariantList{ localSong(bogus, QStringLiteral("坏文件")) }));
            }
        }});
        steps->push_back({3000, [&]() {
            ui.bypass = player->property("effectsBypassed").toBool();
            if (auto *t = mainWin->findChild<QObject*>("fxEntryTextObj"))
                ui.entryTextBypass = t->property("text").toString();
            const QString p = QDir::tempPath() + QStringLiteral("/fx_ui_bypass.png");
            if (mainWin && mainWin->grabWindow().save(p)) ui.shots << p;
            printf("[%s] 坏文件：bypassed=%d 入口文案=\"%s\"（应为\"音效未生效\"）\n",
                   (ui.bypass && ui.entryTextBypass == QStringLiteral("音效未生效")) ? "PASS" : "FAIL",
                   int(ui.bypass), qPrintable(ui.entryTextBypass));
            QFile::remove(QDir::tempPath() + QStringLiteral("/muyun_fx_ui_bogus.audio"));
            QMetaObject::invokeMethod(player, "setEqEnabled", Q_ARG(bool, false));
        }});

        // ---------- 步骤 4：环境混响链路（four-57：用户报"环境混响失效"） ----------
        // 用户纠正得对：上一版借 EQ 唤醒管线，混响/EQ 搅在一起测不出东西。这版只测混响。
        // ⚠ 不模拟点击（三种合成方式实测都点不动编译型 QML Switch，纯测试桩问题，别再浪费时间）：
        //   改验"模型→视图 binding + 混响单独唤醒管线"这条真链路——
        //   写 player.reverbEnabled（= 用户点击后 onToggled 执行的是同一行）→
        //   断言开关跟着亮（binding 活）+ effectsLive（混响单独把管线唤醒）。
        //   视图→模型方向与 EQ/环绕/响度开关是同一套写法（三个都在生产被用户点过），不重复验。
        steps->push_back({900, [&]() {
            QMetaObject::invokeMethod(player, "playSong",
                                      Q_ARG(QVariantMap, localSong(mp3, QStringLiteral("音效UI曲"))),
                                      Q_ARG(QVariantList, QVariantList{ localSong(mp3, QStringLiteral("音效UI曲")) }));
        }});
        steps->push_back({2800, [&]() {
            waitPlaying(3000);
            ui.revLiveBefore = !player->property("effectsLive").toBool();  // 全关 = 不该在音效管线
            QObject *rs = findInUI("revSwitchObj");
            ui.revSwitchFound = rs != nullptr;
            if (rs) {
                QMetaObject::invokeMethod(player, "setReverbEnabled", Q_ARG(bool, true));
                waitPlaying(3000);                     // 混响单独切内核：异步重载，等它起播
                ui.revOn = player->property("reverbEnabled").toBool()
                           && player->property("effectsLive").toBool()
                           && rs->property("checked").toBool();   // 开关跟着亮（binding 未断）
                const QString p = QDir::tempPath() + QStringLiteral("/fx_ui_rev_on.png");
                if (mainWin && mainWin->grabWindow().save(p)) ui.shots << p;
                QMetaObject::invokeMethod(player, "setReverbEnabled", Q_ARG(bool, false));
                waitABit(250);
                ui.revOff = !player->property("reverbEnabled").toBool()
                            && !rs->property("checked").toBool();
            }
            printf("[%s] 环境混响链路：开关=%d 全关live=0→%d 开→(模型&live&开关亮)→%d 关→全复位→%d\n",
                   (ui.revSwitchFound && ui.revLiveBefore && ui.revOn && ui.revOff) ? "PASS" : "FAIL",
                   int(ui.revSwitchFound), int(ui.revLiveBefore), int(ui.revOn), int(ui.revOff));
        }});

        // ---------- 步骤 5：收尾 ----------
        steps->push_back({700, [&]() {
            ui.onAfterOff = player->property("effectsOn").toBool();
            QObject *panel = mainWin ? mainWin->findChild<QObject*>("effectsPanelObj") : nullptr;
            ui.stateOff = panel ? panel->property("fxStateText").toString() : QString();
            const QString p = QDir::tempPath() + QStringLiteral("/fx_ui_off.png");
            if (mainWin && mainWin->grabWindow().save(p)) ui.shots << p;
            printf("[%s] 关闭音效后 effectsOn=%d 状态行=\"%s\"\n",
                   (!ui.onAfterOff && ui.stateOff.contains(QStringLiteral("未开启"))) ? "PASS" : "FAIL",
                   int(ui.onAfterOff), qPrintable(ui.stateOff));

            const bool pass = ui.entry && ui.live && ui.panelOpen && ui.fader && ui.flacLive
                              && ui.spBound && ui.resetBtn && ui.noScroll && ui.modesDistinct
                              && ui.revSwitchFound && ui.revOn && ui.revOff
                              && !ui.onAfterOff
                              && ui.stateOff.contains(QStringLiteral("未开启"))
                              && ui.entryText0 == QStringLiteral("音效")
                              && ui.bypass && ui.entryTextBypass == QStringLiteral("音效未生效");
            printf("[%s] 汇总：入口=%d live=%d 面板开=%d 推子往返=%d 环绕绑真值=%d 恢复默认=%d "
                   "拖推子不滚=%d 模式图标各异=%d FLAC生效=%d 坏文件旁路=%d 混响开关=%d 关闭后不高亮=%d\n",
                   pass ? "PASS" : "FAIL", int(ui.entry), int(ui.live), int(ui.panelOpen),
                   int(ui.fader), int(ui.spBound), int(ui.resetBtn), int(ui.noScroll),
                   int(ui.modesDistinct), int(ui.flacLive), int(ui.bypass),
                   int(ui.revSwitchFound && ui.revOn && ui.revOff), int(!ui.onAfterOff));
            for (const QString &s : ui.shots) printf("    截图 %s\n", qPrintable(s));
            player->stop();
            fflush(stdout);
            finishSelfTest(pass ? 0 : 4);
        }});

        (*runStep)();     // 启动第一步（每步做完才排下一步，避免泵事件造成重入）
    }
#endif // MUYUN_SELFTES

    // 启动后台静默检查更新：延迟 4 秒避开启动高峰（网络在工作线程，失败一律静默）。
    // 自检模式（--test-*）一律不联网，保证回归可复现；--test-update* 自己注入本地清单。
    if (!isSelfTest)
        QTimer::singleShot(4000, updater, &UpdateChecker::autoCheck);

    const int ret = app.exec();

    // ---- 退出收尾（新问题②：播放中托盘退出"未响应"的根因在此）----
    // 旧代码直接 QThreadPool::waitForDone()：worker 里在途的音源解析/歌词/下载请求
    // 若卡在网络，退出就无限干等（每次超时 20s 起、降级链还要串好几档）。
    // 现在先置"正在退出"让在途请求 ≤200ms 内中止，再停播放内核，最后收工。
    Muyun::HttpClient::beginShutdown();
    Muyun::HttpClient::instance()->cancelAll();
    if (player) player->stop();   // 停解码线程/声卡，避免其回调在销毁期访问已析构对象
    // 等待后台任务（音源解析/首页加载）结束，避免退出时的线程警告
    QThreadPool::globalInstance()->waitForDone();
    store->flushAll();
    return ret;
}
