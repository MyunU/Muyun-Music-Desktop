#pragma once

#include <QString>
#include <QDateTime>

namespace Muyun {
namespace Format {

/// 秒 -> "03:45"（超过 1 小时为 "1:02:03"）
QString duration(double seconds);
/// 秒 -> "3:45"（不补前导零）
QString durationShort(double seconds);

/// 字节 -> "3.5 MB"
QString fileSize(qint64 bytes);

/// 播放量 -> "1.2万" / "3.4亿"
QString playCount(qint64 count);

/// 毫秒时间戳 -> "2024-01-01 12:00"
QString dateTime(qint64 ms);
/// 毫秒时间戳 -> "2024-01-01"
QString date(qint64 ms);
/// 毫秒时间戳 -> "3分钟前" / "昨天 12:00"
QString relativeTime(qint64 ms);

/// 拼接歌手列表
QString joinArtists(const QStringList &artists);

/// 去除文件名中的非法字符
QString safeFileName(const QString &name);

/// 把 "[00:12.34]" 形式的时间标签解析为秒，失败返回 -1
double parseLrcTime(const QString &tag);

} // namespace Format
} // namespace Muyun
