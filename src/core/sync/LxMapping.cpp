#include "LxMapping.h"

#include "core/utils/Format.h"

namespace Muyun {
namespace LxMapping {

namespace {

QString jstr(const QJsonValue &v)
{
    if (v.isDouble()) return QString::number(qint64(v.toDouble()));   // 数字 id → 无小数点
    return v.toString();
}

} // namespace

QJsonObject songToMusicInfo(const Song &s)
{
    QJsonObject info;
    const QString source = s.lx.source.isEmpty() ? s.sourceCode() : s.lx.source;
    const QString songmid = !s.lx.songmid.isEmpty() ? s.lx.songmid : s.id;

    QJsonObject meta;
    meta[QStringLiteral("songId")] = songmid;
    meta[QStringLiteral("albumName")] = !s.lx.albumName.isEmpty() ? s.lx.albumName : s.album;
    meta[QStringLiteral("picUrl")] = !s.lx.img.isEmpty() ? s.lx.img : s.cover;

    QString id;
    if (source == QLatin1String("kg")) {
        id = songmid + QLatin1Char('_') + s.lx.hash;   // 洛雪 kg 特殊：songmid_hash
        if (!s.lx.hash.isEmpty()) meta[QStringLiteral("hash")] = s.lx.hash;
    } else {
        id = source + QLatin1Char('_') + songmid;
    }
    if (!s.lx.albumId.isEmpty()) meta[QStringLiteral("albumId")] = s.lx.albumId;
    if (source == QLatin1String("tx")) {
        if (!s.lx.strMediaMid.isEmpty()) meta[QStringLiteral("strMediaMid")] = s.lx.strMediaMid;
        if (!s.lx.albumMid.isEmpty()) meta[QStringLiteral("albumMid")] = s.lx.albumMid;
        if (!s.lx.songId.isEmpty()) meta[QStringLiteral("id")] = s.lx.songId;
    } else if (source == QLatin1String("mg")) {
        if (!s.lx.copyrightId.isEmpty()) meta[QStringLiteral("copyrightId")] = s.lx.copyrightId;
        if (!s.lx.lrcUrl.isEmpty()) meta[QStringLiteral("lrcUrl")] = s.lx.lrcUrl;
        if (!s.lx.mrcUrl.isEmpty()) meta[QStringLiteral("mrcUrl")] = s.lx.mrcUrl;
        if (!s.lx.trcUrl.isEmpty()) meta[QStringLiteral("trcUrl")] = s.lx.trcUrl;
    }

    // 音质列表（洛雪 qualitys/_qualitys；kg 每项带 hash）
    if (!s.lx.types.isEmpty()) {
        QJsonArray qualitys;
        QJsonObject qualitysObj;
        for (const auto &t : s.lx.types) {
            QJsonObject q;
            q[QStringLiteral("type")] = t.type;
            q[QStringLiteral("size")] = t.size.isEmpty() ? QJsonValue::Null : QJsonValue(t.size);
            if (source == QLatin1String("kg") && !t.hash.isEmpty())
                q[QStringLiteral("hash")] = t.hash;
            qualitys.append(q);
            QJsonObject s2;
            s2[QStringLiteral("size")] = q.value(QStringLiteral("size"));
            if (source == QLatin1String("kg") && !t.hash.isEmpty())
                s2[QStringLiteral("hash")] = t.hash;
            qualitysObj[t.type] = s2;
        }
        meta[QStringLiteral("qualitys")] = qualitys;
        meta[QStringLiteral("_qualitys")] = qualitysObj;
    }

    info[QStringLiteral("id")] = id;
    info[QStringLiteral("name")] = s.name;
    info[QStringLiteral("singer")] = s.artist;
    info[QStringLiteral("source")] = source;
    info[QStringLiteral("interval")] = !s.lx.interval.isEmpty()
                                         ? s.lx.interval : Format::duration(s.duration);
    info[QStringLiteral("meta")] = meta;
    return info;
}

double parseInterval(const QString &interval)
{
    const QStringList parts = interval.split(QLatin1Char(':'));
    if (parts.size() == 2) return parts[0].toDouble() * 60 + parts[1].toDouble();
    if (parts.size() == 3) return parts[0].toDouble() * 3600 + parts[1].toDouble() * 60 + parts[2].toDouble();
    return interval.toDouble();
}

bool musicInfoToSong(const QJsonObject &info, Song *out)
{
    const QString source = info.value(QStringLiteral("source")).toString();
    if (source.isEmpty() || source == QLatin1String("local")) return false;
    const QJsonObject meta = info.value(QStringLiteral("meta")).toObject();
    const QString songmid = jstr(meta.value(QStringLiteral("songId")));
    if (songmid.isEmpty()) return false;

    Song s;
    s.id = songmid;
    s.name = info.value(QStringLiteral("name")).toString();
    s.artist = info.value(QStringLiteral("singer")).toString();
    s.platform = platformFromSourceCode(source);
    s.duration = parseInterval(info.value(QStringLiteral("interval")).toString());
    s.hasLx = true;
    s.lx.source = source;
    s.lx.songmid = songmid;
    s.lx.albumName = meta.value(QStringLiteral("albumName")).toString();
    s.lx.img = meta.value(QStringLiteral("picUrl")).toString();
    s.lx.interval = info.value(QStringLiteral("interval")).toString();
    s.lx.albumId = jstr(meta.value(QStringLiteral("albumId")));
    s.lx.hash = meta.value(QStringLiteral("hash")).toString();
    if (source == QLatin1String("tx")) {
        s.lx.strMediaMid = meta.value(QStringLiteral("strMediaMid")).toString();
        s.lx.albumMid = meta.value(QStringLiteral("albumMid")).toString();
        s.lx.songId = jstr(meta.value(QStringLiteral("id")));
    } else if (source == QLatin1String("mg")) {
        s.lx.copyrightId = jstr(meta.value(QStringLiteral("copyrightId")));
        s.lx.lrcUrl = meta.value(QStringLiteral("lrcUrl")).toString();
        s.lx.mrcUrl = meta.value(QStringLiteral("mrcUrl")).toString();
        s.lx.trcUrl = meta.value(QStringLiteral("trcUrl")).toString();
    }
    const QJsonArray qualitys = meta.value(QStringLiteral("qualitys")).toArray();
    for (const QJsonValue &v : qualitys) {
        const QJsonObject q = v.toObject();
        LxSongQualityMeta t;
        t.type = q.value(QStringLiteral("type")).toString();
        t.size = q.value(QStringLiteral("size")).toString();
        t.hash = q.value(QStringLiteral("hash")).toString();
        if (!t.type.isEmpty()) s.lx.types.append(t);
    }
    s.album = s.lx.albumName;
    s.cover = s.lx.img;
    if (out) *out = s;
    return true;
}

} // namespace LxMapping
} // namespace Muyun
