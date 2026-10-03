#include "Format.h"

#include <QRegularExpression>
#include <cmath>

namespace Muyun {
namespace Format {

QString duration(double seconds)
{
    if (seconds < 0) seconds = 0;
    const int total = static_cast<int>(std::floor(seconds + 0.5));
    const int h = total / 3600;
    const int m = (total % 3600) / 60;
    const int s = total % 60;
    if (h > 0)
        return QString::asprintf("%d:%02d:%02d", h, m, s);
    return QString::asprintf("%02d:%02d", m, s);
}

QString durationShort(double seconds)
{
    if (seconds < 0) seconds = 0;
    const int total = static_cast<int>(std::floor(seconds + 0.5));
    const int h = total / 3600;
    const int m = (total % 3600) / 60;
    const int s = total % 60;
    if (h > 0)
        return QString::asprintf("%d:%02d:%02d", h, m, s);
    return QString::asprintf("%d:%02d", m, s);
}

QString fileSize(qint64 bytes)
{
    if (bytes < 0) bytes = 0;
    constexpr double KB = 1024.0;
    constexpr double MB = KB * 1024.0;
    constexpr double GB = MB * 1024.0;
    const double b = static_cast<double>(bytes);
    if (b >= GB) return QString::number(b / GB, 'f', 2) + QStringLiteral(" GB");
    if (b >= MB) return QString::number(b / MB, 'f', 1) + QStringLiteral(" MB");
    if (b >= KB) return QString::number(b / KB, 'f', 0) + QStringLiteral(" KB");
    return QString::number(bytes) + QStringLiteral(" B");
}

QString playCount(qint64 count)
{
    if (count <= 0) return QStringLiteral("0");
    if (count >= 100000000)
        return QString::number(count / 100000000.0, 'f', 1) + QStringLiteral("亿");
    if (count >= 10000)
        return QString::number(count / 10000.0, 'f', 1) + QStringLiteral("万");
    return QString::number(count);
}

QString dateTime(qint64 ms)
{
    if (ms <= 0) return QString();
    return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString date(qint64 ms)
{
    if (ms <= 0) return QString();
    return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("yyyy-MM-dd"));
}

QString relativeTime(qint64 ms)
{
    if (ms <= 0) return QString();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 diff = now - ms;
    if (diff < 0) return dateTime(ms);

    constexpr qint64 MIN = 60000;
    constexpr qint64 HOUR = 60 * MIN;
    constexpr qint64 DAY = 24 * HOUR;

    if (diff < MIN) return QStringLiteral("刚刚");
    if (diff < HOUR) return QString::number(diff / MIN) + QStringLiteral("分钟前");
    if (diff < DAY) return QString::number(diff / HOUR) + QStringLiteral("小时前");
    if (diff < 2 * DAY) return QStringLiteral("昨天 ") +
                               QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("HH:mm"));
    if (diff < 30 * DAY) return QString::number(diff / DAY) + QStringLiteral("天前");
    return date(ms);
}

QString joinArtists(const QStringList &artists)
{
    return artists.join(QStringLiteral("、"));
}

QString safeFileName(const QString &name)
{
    static const QRegularExpression invalid(QStringLiteral("[\\\\/:*?\"<>|]"));
    QString out = name;
    out.replace(invalid, QStringLiteral("_"));
    out.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return out.trimmed();
}

double parseLrcTime(const QString &tag)
{
    // 支持 [mm:ss.xx] 与 [mm:ss:xx]
    static const QRegularExpression rx(
        QStringLiteral("^\\[(\\d{1,3}):(\\d{1,2})(?:[.:](\\d{1,3}))?\\]$"));
    const auto m = rx.match(tag.trimmed());
    if (!m.hasMatch()) return -1.0;
    const int minutes = m.captured(1).toInt();
    const int seconds = m.captured(2).toInt();
    double frac = 0.0;
    if (m.captured(3).length() > 0) {
        const QString fracStr = m.captured(3);
        const double digits = std::pow(10.0, fracStr.length());
        frac = fracStr.toInt() / digits;
    }
    return minutes * 60.0 + seconds + frac;
}

} // namespace Format
} // namespace Muyun
