#include "LyricParser.h"

#include "core/utils/Format.h"

#include <QRegularExpression>
#include <algorithm>

namespace Muyun {
namespace LyricParser {

namespace {

struct RawLine {
    double time = 0.0;
    QString text;
    QVector<QPair<double, QString>> words;
};

/// 检测文本是否主要是中文（中文字符占字母数字字符比例 > 50%）
bool isMostlyChinese(const QString &text)
{
    int chineseCount = 0;
    int totalCount = 0;
    for (QChar c : text) {
        if (c.isLetterOrNumber()) {
            ++totalCount;
            const uint cp = c.unicode();
            if ((cp >= 0x4E00 && cp <= 0x9FFF)    // CJK 统一汉字
                || (cp >= 0x3400 && cp <= 0x4DBF) // CJK 扩展 A
                || (cp >= 0x3000 && cp <= 0x30FF) // CJK 标点
                || (cp >= 0xFF00 && cp <= 0xFFEF)) // 全角字符
                ++chineseCount;
        }
    }
    return totalCount > 0 && (double)chineseCount / totalCount > 0.5;
}

QVector<RawLine> splitLines(const QString &text, bool parseWords)
{
    QVector<RawLine> out;
    static const QRegularExpression tagRx(QStringLiteral("\\[(\\d{1,3}):(\\d{1,2}(?:[.:]\\d{1,3})?)\\]"));

    const QStringList rawLines = text.split(QLatin1Char('\n'));
    for (const QString &raw : rawLines) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) continue;

        // 收集该行的所有时间标签
        QVector<double> times;
        qsizetype lastEnd = 0;
        auto it = tagRx.globalMatch(line);
        while (it.hasNext()) {
            const auto m = it.next();
            const QString tag = QStringLiteral("[%1:%2]").arg(m.captured(1), m.captured(2));
            const double t = Format::parseLrcTime(tag);
            if (t >= 0) times.append(t);
            lastEnd = m.capturedEnd();
        }
        if (times.isEmpty()) continue;

        QString body = line.mid(lastEnd).trimmed();

        QVector<QPair<double, QString>> words;
        if (parseWords) {
            // 逐字格式：文本<mm:ss.xx>字<mm:ss.xx>字
            static const QRegularExpression wordRx(
                QStringLiteral("<(\\d{1,3}):(\\d{1,2}(?:[.:]\\d{1,3})?)>"));
            qsizetype pos = 0;
            qsizetype wordStart = 0;
            QString pending;
            auto wit = wordRx.globalMatch(body);
            qsizetype prevEnd = 0;
            double prevTime = times.first();
            while (wit.hasNext()) {
                const auto m = wit.next();
                const QString chunk = body.mid(prevEnd, m.capturedStart() - prevEnd);
                if (!chunk.isEmpty()) pending += chunk;
                prevEnd = m.capturedEnd();
                const QString tag = QStringLiteral("[%1:%2]").arg(m.captured(1), m.captured(2));
                const double t = Format::parseLrcTime(tag);
                if (!pending.isEmpty()) {
                    words.append({prevTime, pending});
                    prevTime = t >= 0 ? t : prevTime;
                    pending.clear();
                }
            }
            const QString tail = body.mid(prevEnd);
            if (!tail.isEmpty()) words.append({prevTime, tail});
            // 去掉逐字标记，保留纯文本
            body.remove(wordRx);
            Q_UNUSED(pos)
            Q_UNUSED(wordStart)
        }

        for (double t : times) {
            RawLine rl;
            rl.time = t;
            rl.text = body;
            rl.words = words;
            out.append(rl);
        }
    }

    std::sort(out.begin(), out.end(), [](const RawLine &a, const RawLine &b) {
        return a.time < b.time;
    });
    return out;
}

} // namespace

QString stripMetadata(const QString &lrcText)
{
    static const QRegularExpression metaRx(
        QStringLiteral("^\\[(ti|ar|al|by|offset|kana|re|ve|au|length):.*\\]$"),
        QRegularExpression::CaseInsensitiveOption);
    QStringList kept;
    const QStringList lines = lrcText.split(QLatin1Char('\n'));
    for (const QString &l : lines) {
        if (!metaRx.match(l.trimmed()).hasMatch()) kept.append(l);
    }
    return kept.join(QLatin1Char('\n'));
}

bool hasWordByWord(const QString &lrcText)
{
    static const QRegularExpression wordRx(
        QStringLiteral("<\\d{1,3}:\\d{1,2}([.:]\\d{1,3})?>"));
    return wordRx.match(lrcText).hasMatch();
}

SongLyric parseLrc(const QString &lrcText)
{
    SongLyric lyric;
    if (lrcText.trimmed().isEmpty()) return lyric;

    lyric.rawLrc = lrcText;
    const bool wordMode = hasWordByWord(lrcText);
    const QVector<RawLine> raw = splitLines(stripMetadata(lrcText), wordMode);
    if (raw.isEmpty()) return lyric;

    lyric.hasWordByWord = wordMode;

    for (int i = 0; i < raw.size(); ++i) {
        LyricLine line;
        line.time = raw.at(i).time;
        line.text = raw.at(i).text;
        if (wordMode) {
            for (int w = 0; w < raw.at(i).words.size(); ++w) {
                LyricWord word;
                word.startTime = raw.at(i).words.at(w).first;
                word.endTime = (w + 1 < raw.at(i).words.size())
                                   ? raw.at(i).words.at(w + 1).first
                                   : (i + 1 < raw.size() ? raw.at(i + 1).time
                                                        : word.startTime + 0.5);
                word.text = raw.at(i).words.at(w).second;
                line.words.append(word);
            }
        }
        lyric.lines.append(line);
    }
    // 补齐逐字结束时间：用下一行起始时间兜底
    if (wordMode) {
        for (int i = 0; i < lyric.lines.size(); ++i) {
            const double next = (i + 1 < lyric.lines.size())
                                    ? lyric.lines.at(i + 1).time
                                    : lyric.lines.at(i).time + 3.0;
            for (auto &w : lyric.lines[i].words) {
                if (w.endTime <= w.startTime) w.endTime = next;
            }
        }
    }
    return lyric;
}

void mergeTranslation(SongLyric &lyric, const QString &translationText, bool isRoman)
{
    if (translationText.trimmed().isEmpty() || lyric.lines.isEmpty()) return;
    const QVector<RawLine> parsed = splitLines(stripMetadata(translationText), false);

    // 丢掉空文本行（有些源在译文末尾多一个带时间戳的空行，会让下面的"行数相等"判断失效）
    QVector<RawLine> raw;
    raw.reserve(parsed.size());
    for (const RawLine &r : parsed)
        if (!r.text.trimmed().isEmpty()) raw.append(r);
    if (raw.isEmpty()) return;

    auto slot = [isRoman](LyricLine &l) -> QString & { return isRoman ? l.roman : l.translation; };
    auto markFilled = [isRoman, &lyric]() {
        if (isRoman) lyric.hasRoman = true; else lyric.hasTranslation = true;
    };

    // ① 行数一致 → 直接按序号配对。
    //   在线歌词绝大多数是"同一首歌的两份 LRC"，行数相同但**时间轴常整体偏移 0.5~2s**
    //   （译文按该出现的时间打点）。按序号配最稳。
    if (raw.size() == lyric.lines.size()) {
        for (int i = 0; i < raw.size(); ++i) {
            if (!slot(lyric.lines[i]).isEmpty()) continue;
            slot(lyric.lines[i]) = raw.at(i).text;
            markFilled();
        }
        return;
    }

    // ② 行数不一致 → 顺序单调 + 宽松容差 + **只在没填过的行里找最近**。
    //   旧实现是"全局找最近，若那行已有译文就整条丢弃"，于是会出现：
    //   第 N 条译文抢到了第 N-1 行（差几十毫秒），而 N-1 早被填过 → 这条译文被扔掉，
    //   第 N 行永远拿不到译文 → 表现成"有时只显示原文"（交替出现，最难复现那种）。
    constexpr double kTolSec = 3.0;
    int cursor = 0;
    for (const RawLine &rl : raw) {
        int best = -1;
        double bestDiff = 1e9;
        for (int i = cursor; i < lyric.lines.size(); ++i) {
            // 修复：检查绝对值差（旧代码只检查 lyric.time - rl.time，
            // 当 rl.time > lyric.time 时差值为负，永远不会触发 break，导致扫描越界）
            if (std::fabs(lyric.lines.at(i).time - rl.time) > kTolSec) break;
            if (!slot(lyric.lines[i]).isEmpty()) continue;            // 绝不抢已填的行
            const double diff = std::fabs(lyric.lines.at(i).time - rl.time);
            if (diff < bestDiff) { bestDiff = diff; best = i; }
        }
        if (best < 0 || bestDiff > kTolSec) continue;                 // 宁可留空也不乱配
        slot(lyric.lines[best]) = rl.text;
        markFilled();
        cursor = best + 1;                                            // 顺序单调，禁止交叉抢占
    }
}

/// 检测并合并主 LRC 文件中的翻译行。
/// 场景：有些音源把中英文歌词写在同一个 LRC 文件里，交替出现：
///   [00:01.00] English line
///   [00:01.50] 中文翻译
///   [00:05.00] Another English line
///   [00:05.50] 另一个中文翻译
/// 或者中文在前：
///   [00:01.00] 中文歌词
///   [00:01.50] English translation
/// mergeTranslation 不会处理这种格式（因为 rawTranslation 为空）。
/// 本函数检测相邻行时间接近且语言不同的情况，确保英文作为主歌词（text），中文作为翻译（translation）。
void mergeInlineTranslation(SongLyric &lyric)
{
    if (lyric.lines.size() < 2) return;

    QVector<LyricLine> result;
    result.reserve(lyric.lines.size());

    int i = 0;
    while (i < lyric.lines.size()) {
        LyricLine &line = lyric.lines[i];

        // 已有翻译的行直接保留
        if (!line.translation.isEmpty()) {
            result.append(line);
            ++i;
            continue;
        }

        // 检查下一行是否是翻译行
        if (i + 1 < lyric.lines.size()) {
            const LyricLine &next = lyric.lines[i + 1];
            const double timeDiff = std::fabs(next.time - line.time);

            // 条件：时间接近（< 1s）+ 文本不同 + 语言不同（一行中文一行非中文）
            if (timeDiff < 1.0
                && !next.text.trimmed().isEmpty()
                && next.text != line.text
                && isMostlyChinese(next.text) != isMostlyChinese(line.text)) {

                bool lineChinese = isMostlyChinese(line.text);
                bool nextChinese = isMostlyChinese(next.text);

                LyricLine merged;
                if (lineChinese && !nextChinese) {
                    // 当前行是中文，下一行是英文 → 英文作为主歌词，中文作为翻译
                    merged = next;              // 英文行作为主行
                    merged.translation = line.text; // 中文作为翻译
                } else {
                    // 当前行是英文，下一行是中文 → 英文作为主歌词，中文作为翻译
                    merged = line;              // 英文行作为主行
                    merged.translation = next.text; // 中文作为翻译
                }

                result.append(merged);
                lyric.hasTranslation = true;
                i += 2; // 跳过两行（已合并）
                continue;
            }
        }

        result.append(line);
        ++i;
    }

    lyric.lines = result;
}

int lineIndexAt(const SongLyric &lyric, double timeSec)
{
    if (lyric.lines.isEmpty()) return -1;
    int lo = 0;
    int hi = lyric.lines.size() - 1;
    int result = -1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (lyric.lines.at(mid).time <= timeSec) {
            result = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return result;
}

} // namespace LyricParser
} // namespace Muyun
