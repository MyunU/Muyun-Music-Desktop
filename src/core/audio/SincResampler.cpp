#include "SincResampler.h"

#include <cmath>
#include <cstring>
#include <algorithm>

namespace Muyun {

void SincResampler::configure(int inRate, int outRate)
{
    if (inRate <= 0 || outRate <= 0) return;
    if (inRate == m_inRate && outRate == m_outRate) return;
    m_inRate = inRate;
    m_outRate = outRate;
    m_step = double(inRate) / double(outRate);
    buildTable();
    reset();
}

void SincResampler::reset()
{
    m_buf.clear();
    m_base = 0;
    m_totalIn = 0;
    m_pos = 0.0;
}

void SincResampler::buildTable()
{
    // 64 相 + 端点，相内线性插值；降采样时截止频率随输出走，防混叠
    const int P = kPhases;
    m_tab.resize(qsizetype(P + 1) * kTaps);
    const double half = kTaps / 2.0;
    const double cutoff = std::min(1.0, double(m_outRate) / double(m_inRate)) * 0.90;

    auto rowFill = [&](int p, double f) {
        float *row = m_tab.data() + qsizetype(p) * kTaps;
        double sum = 0.0;
        for (int j = 0; j < kTaps; ++j) {
            const double x = j - (kTaps / 2.0 - 1.0) - f;
            // Blackman 窗
            double win = 0.0;
            if (std::fabs(x) <= half) {
                const double a = 2.0 * M_PI * x / double(kTaps);
                win = 0.42 + 0.5 * std::cos(a) + 0.08 * std::cos(2.0 * a);
            }
            double s;
            const double xc = x * cutoff;
            if (std::fabs(xc) < 1e-9) s = cutoff;
            else s = std::sin(M_PI * xc) / (M_PI * x);      // sinc(x*cutoff)*cutoff 归一
            row[j] = float(s * win);
            sum += row[j];
        }
        if (std::fabs(sum) > 1e-6) {
            const float g = float(1.0 / sum);
            for (int j = 0; j < kTaps; ++j) row[j] *= g;     // DC 增益归一
        }
    };

    for (int p = 0; p <= P; ++p) rowFill(p, double(p) / double(P));
}

int SincResampler::suggestCap(int inFrames) const
{
    if (m_outRate <= 0 || m_inRate <= 0) return inFrames + kTaps;
    return int(qint64(inFrames) * m_inRate / m_outRate) + kTaps + 8;
}

int SincResampler::feed(const float *in, int inFrames, float *out, int outCap)
{
    if (!in || inFrames <= 0 || !out || outCap <= 0) return 0;
    if (m_inRate <= 0 || m_outRate <= 0) return 0;

    if (identity()) {
        const int n = std::min(inFrames, outCap);
        std::memcpy(out, in, size_t(n) * 2 * sizeof(float));
        return n;
    }

    // 追加输入（Qt6 的 QVector=QList 没有 append(ptr,n)，用 resize+memcpy）
    const qsizetype old = m_buf.size();
    m_buf.resize(old + qsizetype(inFrames) * 2);
    std::memcpy(m_buf.data() + old, in, size_t(inFrames) * 2 * sizeof(float));
    m_totalIn += inFrames;

    const int halfAdv = kTaps / 2 - 1;      // 中心左侧抽头数
    int produced = 0;
    const float *buf = m_buf.constData();   // 循环内不再追加，指针稳定

    while (produced < outCap) {
        const double pos = m_pos;
        const qint64 ipos = qint64(std::floor(pos));
        const double frac = pos - double(ipos);
        const qint64 first = ipos - halfAdv;
        const qint64 last = first + kTaps - 1;
        if (last >= m_totalIn) break;                       // 数据还不够

        // 相位表线性插值
        double f = frac; if (f < 0) f = 0; if (f > 1) f = 1;
        const int p = int(f * double(kPhases));
        const double u = f * double(kPhases) - double(p);
        const float *r0 = m_tab.constData() + qsizetype(p) * kTaps;
        const float *r1 = r0 + kTaps;

        float accL = 0.0f, accR = 0.0f;
        for (int j = 0; j < kTaps; ++j) {
            const qint64 idx = first + j;
            if (idx < 0) continue;                          // 起始边界：按零处理
            const qsizetype off = (idx - m_base) * 2;
            if (off < 0 || off + 1 >= m_buf.size()) continue;
            const float w = r0[j] * float(1.0 - u) + r1[j] * float(u);
            accL += buf[off] * w;
            accR += buf[off + 1] * w;
        }
        out[produced * 2] = accL;
        out[produced * 2 + 1] = accR;
        ++produced;
        m_pos += m_step;
    }

    // 丢弃不再需要的历史（保留到当前输出位置左侧 halfAdv 帧）
    const qint64 need = qint64(std::floor(m_pos)) - halfAdv;
    if (need > m_base) {
        const qint64 drop = need - m_base;
        m_buf.remove(0, qsizetype(drop) * 2);
        m_base = need;
    }
    return produced;
}

void SincResampler::trim() {}

} // namespace Muyun
