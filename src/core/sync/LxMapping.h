#pragma once

#include <QJsonObject>
#include "core/Types.h"

namespace Muyun {

/**
 * @brief 暮云 Song ↔ 洛雪 MusicInfo（LX.Music.MusicInfo 新版格式）互转。
 * 字段规则逐条对照 lx-music-desktop src/common/utils/tools.ts toNewMusicInfo：
 *  - id = `${source}_${songmid}`；kg 例外 = `${songmid}_${hash}`
 *  - meta.songId=songmid、albumName、picUrl；tx 加 strMediaMid/albumMid/id；
 *    kg 加 hash；mg 加 copyrightId/lrcUrl/mrcUrl/trcUrl；音质放 qualitys/_qualitys
 */
namespace LxMapping {

QJsonObject songToMusicInfo(const Song &s);

/// source=='local' 或无可用 id → 返回 false（out 不写）
bool musicInfoToSong(const QJsonObject &info, Song *out);

/// "m:ss" / "h:mm:ss" / 纯秒数字符串 → 秒
double parseInterval(const QString &interval);

} // namespace LxMapping
} // namespace Muyun
