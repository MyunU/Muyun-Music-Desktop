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

/// 行文本的书写系统分类（用于内联翻译行合并：区分原文/译文的脚本，识别非中文歌词）
enum class Script { None = 0, Latin, Chinese, Japanese, Korean, Other };

/// 统计一行文本的主要书写系统：
///  - 含假名（平/片假名）→ 日文（汉字+假名混排判日文，旧实现把假名当汉字导致日文歌
///    原文与中文译文"都是中文" → 不合并 → 译文被识别成两行，用户 2026-10-20 报）
///  - 含谚文 → 韩文
///  - 含汉字 → 中文
///  - 含其它字母（拉丁/西里尔/希腊…）→ 拉丁/其它
/// 标点（CJK 标点 0x3000-0x303F、全角 0xFF00-0xFFEF）不算任何脚本，避免"の/サ"被算成汉字
Script classifyScript(const QString &text)
{
    int han = 0, kana = 0, hangul = 0, latin = 0, other = 0;
    for (QChar c : text) {
        if (!c.isLetter()) continue;
        const uint cp = c.unicode();
        if ((cp >= 0x4E00 && cp <= 0x9FFF)      // CJK 统一汉字（中日共用字形，日文靠假名区分）
            || (cp >= 0x3400 && cp <= 0x4DBF)   // CJK 扩展 A
            || (cp >= 0xF900 && cp <= 0xFAFF)) { // CJK 兼容汉字
            ++han; continue;
        }
        if ((cp >= 0x3040 && cp <= 0x309F)      // 平假名
            || (cp >= 0x30A0 && cp <= 0x30FF)   // 片假名
            || (cp >= 0x31F0 && cp <= 0x31FF)) { // 片假名语音扩展
            ++kana; continue;
        }
        if (cp >= 0xAC00 && cp <= 0xD7A3) { ++hangul; continue; }  // 谚文音节
        if ((cp >= 0x0041 && cp <= 0x005A) || (cp >= 0x0061 && cp <= 0x007A)
            || (cp >= 0x00C0 && cp <= 0x024F)   // 拉丁扩展
            || (cp >= 0x0370 && cp <= 0x03FF)   // 希腊
            || (cp >= 0x0400 && cp <= 0x04FF)   // 西里尔
            || (cp >= 0x1E00 && cp <= 0x1EFF)
            || (cp >= 0xFF21 && cp <= 0xFF3A)   // 全角 Ａ-Ｚ
            || (cp >= 0xFF41 && cp <= 0xFF5A)) { // 全角 ａ-ｚ
            ++latin; continue;
        }
        if (c.isLetter()) ++other;
    }
    if (kana > 0) return Script::Japanese;
    if (hangul > 0) return Script::Korean;
    if (han > 0) return Script::Chinese;
    if (latin > 0) return Script::Latin;
    if (other > 0) return Script::Other;   // 其余字母文字（泰/阿/天城文…）单独一类，不与拉丁混
    return Script::None;
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

    // 必须用**稳定排序**：同时间戳的行（酷我式内联翻译里"译文 + 下一行原文"就是同时间戳）
    // 要保持文件里的先后顺序，否则 std::sort 会把译文和下一句原文调换 →
    // 译文被当成后一句的译文，整首错位（用户 2026-10-20 报"日文译文与正文显示反了"）
    std::stable_sort(out.begin(), out.end(), [](const RawLine &a, const RawLine &b) {
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
    // 时间标签（可多个），用来取出时间轴之后的正文
    static const QRegularExpression timeTagRx(
        QStringLiteral("^(?:\\[\\d{1,3}:\\d{1,2}(?:[.:]\\d{1,3})?\\])+"));
    // 时间轴后的"标签 : 值"元数据行——酷我/酷狗/网易的日文·中文歌极常见：
    //   [00:00.000] 作词 : 米津玄師 ｜ [00:00.636] 制作人 : 米津玄師 ｜ [00:00.00] 詞：米津玄師
    // 它们不是歌词。留着会被 mergeInlineTranslation 当成"中文原文"去并相邻的日文行
    // → 日文行拿到元数据当译文、真译文错位挂到元数据行上（用户 2026-10-20 报"日文译文与正文显示反了"）。
    static const QRegularExpression metaLabelRx(QStringLiteral(
        "^\\s*(?:作词|作詞|作曲|编曲|編曲|制作人|製作人|监制|監製|混音|混音工程|母带|母帶|录音|錄音|录音助理|錄音助理|录音工程|錄音工程|吉他|贝斯|貝斯|键盘|鍵盤|鼓手|鼓|和声|和聲|合声|合聲|合声编写|合聲編寫|合唱|演唱|歌手|翻唱|原唱|配音|独白|念白|出品|发行|發行|"
        "企划|企劃|统筹|統籌|策划|策劃|词|詞|曲|编|編|唱|歌|Produced by|Written by|Writer|Composer|Lyricist|Arranged by|Arranger|Guitar by|Bass by|Keyboard by|Synthesizer|Drums by|Background Vocals by|Vocal by|Chorus by|Mixed by|Mastered by|Recorded by|Rap by|Strings by|Piano by|Chorus|Engineer|Engineers|Recording Engineer|Recording Engineers|Mix Engineer|Vocal & Rap Arrangement|Digital Editor|OP|SP)\\s*[:：]"));
    QStringList kept;
    const QStringList lines = lrcText.split(QLatin1Char('\n'));
    for (const QString &l : lines) {
        const QString t = l.trimmed();
        if (metaRx.match(t).hasMatch()) continue;                       // [ti:...] 式
        const QString body = t.mid(timeTagRx.match(t).capturedLength()).trimmed();
        if (metaLabelRx.match(body).hasMatch()) continue;               // 时间轴后的标签行
        kept.append(l);
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
/// 场景：有些音源把原文和译文写在同一个 LRC 文件里。实测两种排法（酷我/咪咕/QQ）：
///   A) 原文在前，译文紧随、常与**下一行原文**同时间戳：
///        [00:01.54]夢ならば
///        [00:02.88]如果只是一场梦            ← 夢ならば 的译文
///        [00:02.88]どれほどよかったでしょう
///   B) 译文排在原文之前：
///        [00:02.88]如果只是一场梦
///        [00:02.88]夢ならば
/// mergeTranslation 不处理这种格式（rawTranslation 为空）。
/// 旧实现按"相邻两行"合并、且只认"中文↔非中文"，遇到 A 会把译文挂到**后一行**原文上
/// （整首错位），遇到 B 或日文假名误判（假名被当汉字）会变成"译文当正文"
/// （用户 2026-10-20 报"日文译文与正文显示反了"）。
/// 现改为：先定全曲主导脚本（= 原文语言），再把每个"非主导脚本行"当译文，
/// 挂到**它前面最近的原文行**上（A/B 都成立：译文总在原文之后；前面实在没有原文才挂到后一行）。
void mergeInlineTranslation(SongLyric &lyric, const Song *song)
{
    if (lyric.lines.size() < 2) return;

    // 每行脚本 + 全曲主导脚本统计（空文本行不计）
    QVector<Script> scripts(lyric.lines.size());
    int counts[6] = {0, 0, 0, 0, 0, 0};   // 下标 = Script 枚举值
    for (int i = 0; i < lyric.lines.size(); ++i) {
        scripts[i] = classifyScript(lyric.lines[i].text);
        if (scripts[i] != Script::None) ++counts[int(scripts[i])];
    }
    // 主导脚本 = 原文语言。判据按可靠性从强到弱：
    //   ① 强信号：歌词里只要出现假名 → 原文必是日文；出现谚文 → 原文必是韩文。
    //     中文译文永不含假名/谚文，故"有假名"就能一锤定音。日文歌里难免有纯汉字行
    //     （如"心臓"），classifyScript 会把它误算成中文，使 counts[中文] > counts[日文]，
    //     旧计数判据据此把中文当原文 → 整首反位（用户 2026-10-20 报"日文译文和正文显示反了"）。
    //     强信号绕过计数，只要全曲有一行带假名即锁定日文。
    //   ② 无假名/谚文（中文↔拉丁 等无 CJK 独有特征的场景）：走计数 + 并列结构判据。
    //     并列时结构判据"最早时间戳组最后一行"对同时间戳排版是非对称的（译文在前=对、
    //     原文在前=错），但中文↔拉丁内联同时间戳极少见，且歌名/歌手可辅助，留作次选。
    Script dominant = Script::None;
    const bool hasKana = counts[int(Script::Japanese)] > 0;
    const bool hasHangul = counts[int(Script::Korean)] > 0;
    if (hasKana && hasHangul) {
        // 极罕见的三语（日+韩）：取行数多者为原文
        dominant = counts[int(Script::Japanese)] >= counts[int(Script::Korean)]
                       ? Script::Japanese : Script::Korean;
    } else if (hasKana) {
        dominant = Script::Japanese;
    } else if (hasHangul) {
        dominant = Script::Korean;
    } else {
        Script hint = Script::None;
        if (song) hint = classifyScript(song->name + QLatin1Char(' ') + song->artist);
        int best = 0;
        bool tie = false;
        for (int k = 1; k <= int(Script::Other); ++k) {
            if (counts[k] > best) { best = counts[k]; dominant = Script(k); tie = false; }
            else if (counts[k] == best && counts[k] > 0) { tie = true; }
        }
        if (tie) {
            if ((hint == Script::Japanese || hint == Script::Korean) && counts[int(hint)] == best) {
                dominant = hint;
            } else {
                // 结构判据：最早时间戳那一组里**最后一行**是原文。
                int last = 0;
                const double t0 = lyric.lines.at(0).time;
                for (int i = 1; i < lyric.lines.size(); ++i) {
                    if (qFuzzyCompare(lyric.lines.at(i).time, t0)) last = i; else break;
                }
                dominant = scripts.at(last);
            }
        }
    }
    if (dominant == Script::None) return;   // 全是空行/无字母文字，没什么可并

    // 先决定每条译文行挂到哪个原文行（原文行下标），再统一应用。
    // 译文候选必须"贴"在原文行上，否则不算——否则间奏里的拉丁文拟声歌词
    // （网易中文歌常见的 "Re So So Si Do Si La"）会被当成译文挂到前一句上。
    //   ① 与相邻原文行同时间戳（酷我式：译文插在下一行原文之前；或原文后紧跟译文）
    //   ② 前一行是原文且相差 ≤ 1.0s（原文后紧跟译文、时间戳略有出入）
    auto attachedToOriginal = [&](int i) -> bool {
        for (int j = i - 1; j >= 0; --j) {
            if (scripts.at(j) != dominant) continue;
            if (qFuzzyCompare(lyric.lines.at(j).time, lyric.lines.at(i).time)) return true;
            if (lyric.lines.at(i).time - lyric.lines.at(j).time <= 1.0) return true;
            break;
        }
        bool hasFollowingOriginal = false;
        for (int j = i + 1; j < lyric.lines.size(); ++j) {
            if (scripts.at(j) != dominant) continue;
            if (qFuzzyCompare(lyric.lines.at(j).time, lyric.lines.at(i).time)) return true;
            hasFollowingOriginal = true;
            break;
        }
        // ③ 后面已经没有原文了 → 结尾那句的译文常单独占一行（歌词最后一句译文时间戳离得远），照挂
        return !hasFollowingOriginal;
    };

    QVector<int> attachTo(lyric.lines.size(), -1);
    QVector<bool> filled(lyric.lines.size(), false);   // 一条原文只收一条译文
    for (int i = 0; i < lyric.lines.size(); ++i) {
        if (scripts.at(i) == dominant || scripts.at(i) == Script::None) continue;   // 原文行/空行
        if (!attachedToOriginal(i)) continue;                                      // 没贴在原文上 → 不当译文
        int target = -1;
        for (int j = i - 1; j >= 0; --j)                    // ① 前面最近的"还没译文的"原文行
            if (scripts.at(j) == dominant && !filled.at(j)) { target = j; break; }
        if (target < 0)
            for (int j = i + 1; j < lyric.lines.size(); ++j)   // ② 前面没有 → 后面最近的原文行
                if (scripts.at(j) == dominant && !filled.at(j)) { target = j; break; }
        if (target >= 0) { attachTo[i] = target; filled[target] = true; }
    }

    // 应用：先把译文写进目标原文行，再压缩掉译文行（顺序保持不变）
    QVector<bool> isTranslation(lyric.lines.size(), false);
    for (int i = 0; i < lyric.lines.size(); ++i) {
        const int t = attachTo.at(i);
        if (t < 0) continue;
        isTranslation[i] = true;
        lyric.lines[t].translation = lyric.lines.at(i).text;
        lyric.hasTranslation = true;
    }
    QVector<LyricLine> result;
    result.reserve(lyric.lines.size());
    for (int i = 0; i < lyric.lines.size(); ++i)
        if (!isTranslation.at(i)) result.append(lyric.lines.at(i));

    lyric.lines = result;
}

/// 剥掉歌词开头"歌名 - 歌手"行（酷我/咪咕/QQ 歌词常见：[00:00.00]Lemon - 米津玄師）。
/// 它不是歌词，且时间通常紧贴首句；不剥的话会被 mergeInlineTranslation 当成"译文"
/// 挂到首句原文上（用户 2026-10-20 报"日文译文与正文显示反了"）。
void dropLeadingTitleLine(SongLyric &lyric, const Song &song)
{
    if (lyric.lines.isEmpty()) return;
    const QString t = lyric.lines.at(0).text.trimmed();
    if (t.isEmpty()) return;
    // 标题行特征①：含 " - "（"歌名 - 歌手"），与歌名是否匹配无关
    if (t.contains(QStringLiteral(" - "))) {
        lyric.lines.removeFirst();
        return;
    }
    // 特征②：正好等于歌名（歌名常带后缀 "봄날 (Spring Day)"，取括号前的主名再比对）
    QString nm = song.name.trimmed();
    if (nm.isEmpty()) return;
    int paren = nm.indexOf(QLatin1Char('('));
    const int parenFull = nm.indexOf(QChar(0xFF08));
    if (parenFull >= 0 && (paren < 0 || parenFull < paren)) paren = parenFull;
    if (paren > 0) nm = nm.left(paren).trimmed();
    if (!nm.isEmpty() && (t == nm || t.startsWith(nm + QStringLiteral(" - "))))
        lyric.lines.removeFirst();
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
