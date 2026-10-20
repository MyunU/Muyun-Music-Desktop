#pragma once

#include "core/Types.h"

#include <QString>
#include <QStringList>

namespace Muyun {
namespace LyricParser {

/**
 * @brief 解析 LRC 歌词文本
 *
 * 支持：
 *  - 普通 LRC：[mm:ss.xx]文本
 *  - 增强 LRC（逐字）：[mm:ss.xx]文本<mm:ss.xx>字<mm:ss.xx>字...
 *  - 多时间标签：[00:10.00][00:15.00]文本
 */
SongLyric parseLrc(const QString &lrcText);

/// 把翻译/罗马音歌词合并进主歌词（按时间对齐）
void mergeTranslation(SongLyric &lyric, const QString &translationText, bool isRoman = false);

/// 检测并合并主 LRC 文件中的翻译行（原文与译文交替出现时，译文挂到前一行原文上）
/// @param song 可选：用歌名/歌手的书写系统辅助判断"原文是哪种语言"（日文歌歌手名必含假名）
void mergeInlineTranslation(SongLyric &lyric, const Song *song = nullptr);

/// 剥掉开头"歌名 - 歌手"行（不是歌词，且会污染内联合并）
void dropLeadingTitleLine(SongLyric &lyric, const Song &song);

/// 去除歌词中的元数据标签（ti/ar/al/by/offset 等）
QString stripMetadata(const QString &lrcText);

/// 判断是否为逐字歌词
bool hasWordByWord(const QString &lrcText);

/// 计算某一时刻对应的歌词行索引（二分查找）
int lineIndexAt(const SongLyric &lyric, double timeSec);

} // namespace LyricParser
} // namespace Muyun
