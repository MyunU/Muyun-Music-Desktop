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

/// 去除歌词中的元数据标签（ti/ar/al/by/offset 等）
QString stripMetadata(const QString &lrcText);

/// 判断是否为逐字歌词
bool hasWordByWord(const QString &lrcText);

/// 计算某一时刻对应的歌词行索引（二分查找）
int lineIndexAt(const SongLyric &lyric, double timeSec);

} // namespace LyricParser
} // namespace Muyun
