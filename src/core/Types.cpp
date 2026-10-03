#include "Types.h"

namespace Muyun {

// ---------------------------------------------------------------------------
// 平台
// ---------------------------------------------------------------------------

QString platformId(Platform p)
{
    switch (p) {
    case Platform::Netease: return QStringLiteral("netease");
    case Platform::QQ:      return QStringLiteral("qq");
    case Platform::Kugou:   return QStringLiteral("kugou");
    case Platform::Kuwo:    return QStringLiteral("kuwo");
    case Platform::Migu:    return QStringLiteral("migu");
    case Platform::Local:   return QStringLiteral("local");
    }
    return QStringLiteral("netease");
}

QString platformName(Platform p)
{
    switch (p) {
    case Platform::Netease: return QStringLiteral("小芸音乐");
    case Platform::QQ:      return QStringLiteral("小秋音乐");
    case Platform::Kugou:   return QStringLiteral("小枸音乐");
    case Platform::Kuwo:    return QStringLiteral("小蜗音乐");
    case Platform::Migu:    return QStringLiteral("小蜜音乐");
    case Platform::Local:   return QStringLiteral("本地音乐");
    }
    return QStringLiteral("未知平台");
}

QString platformSourceCode(Platform p)
{
    switch (p) {
    case Platform::Netease: return QStringLiteral("wy");
    case Platform::QQ:      return QStringLiteral("tx");
    case Platform::Kugou:   return QStringLiteral("kg");
    case Platform::Kuwo:    return QStringLiteral("kw");
    case Platform::Migu:    return QStringLiteral("mg");
    case Platform::Local:   return QString();
    }
    return QString();
}

Platform platformFromId(const QString &id)
{
    if (id == QStringLiteral("netease")) return Platform::Netease;
    if (id == QStringLiteral("qq"))      return Platform::QQ;
    if (id == QStringLiteral("kugou"))   return Platform::Kugou;
    if (id == QStringLiteral("kuwo"))    return Platform::Kuwo;
    if (id == QStringLiteral("migu"))    return Platform::Migu;
    return Platform::Local;
}

Platform platformFromSourceCode(const QString &code)
{
    if (code == QStringLiteral("wy")) return Platform::Netease;
    if (code == QStringLiteral("tx")) return Platform::QQ;
    if (code == QStringLiteral("kg")) return Platform::Kugou;
    if (code == QStringLiteral("kw")) return Platform::Kuwo;
    if (code == QStringLiteral("mg")) return Platform::Migu;
    return Platform::Local;
}

// ---------------------------------------------------------------------------
// 音质
// ---------------------------------------------------------------------------

QString qualityId(AudioQuality q)
{
    switch (q) {
    case AudioQuality::K128:      return QStringLiteral("128k");
    case AudioQuality::K320:      return QStringLiteral("320k");
    case AudioQuality::Flac:      return QStringLiteral("flac");
    case AudioQuality::Flac24Bit: return QStringLiteral("flac24bit");
    case AudioQuality::HiRes:     return QStringLiteral("hires");
    case AudioQuality::Atmos:     return QStringLiteral("atmos");
    case AudioQuality::Master:    return QStringLiteral("master");
    }
    return QStringLiteral("128k");
}

QString qualityName(AudioQuality q)
{
    switch (q) {
    case AudioQuality::K128:      return QStringLiteral("128K");
    case AudioQuality::K320:      return QStringLiteral("320K");
    case AudioQuality::Flac:      return QStringLiteral("FLAC");
    case AudioQuality::Flac24Bit: return QStringLiteral("24Bit");
    case AudioQuality::HiRes:     return QStringLiteral("Hi-Res");
    case AudioQuality::Atmos:     return QStringLiteral("Atmos");
    case AudioQuality::Master:    return QStringLiteral("Master");
    }
    return QStringLiteral("128K");
}

QString qualityDesc(AudioQuality q)
{
    switch (q) {
    case AudioQuality::K128:      return QStringLiteral("标准 128kbps");
    case AudioQuality::K320:      return QStringLiteral("高品质 320kbps");
    case AudioQuality::Flac:      return QStringLiteral("无损 FLAC");
    case AudioQuality::Flac24Bit: return QStringLiteral("24bit FLAC");
    case AudioQuality::HiRes:     return QStringLiteral("高解析度无损");
    case AudioQuality::Atmos:     return QStringLiteral("空间音频");
    case AudioQuality::Master:    return QStringLiteral("母带音质");
    }
    return QString();
}

AudioQuality qualityFromId(const QString &id, bool *ok)
{
    if (ok) *ok = true;
    if (id == QStringLiteral("128k"))      return AudioQuality::K128;
    if (id == QStringLiteral("320k"))      return AudioQuality::K320;
    if (id == QStringLiteral("flac"))      return AudioQuality::Flac;
    if (id == QStringLiteral("flac24bit")) return AudioQuality::Flac24Bit;
    if (id == QStringLiteral("hires"))     return AudioQuality::HiRes;
    if (id == QStringLiteral("atmos"))     return AudioQuality::Atmos;
    if (id == QStringLiteral("master"))    return AudioQuality::Master;
    if (ok) *ok = false;
    return AudioQuality::K128;
}

QVector<AudioQuality> allQualitiesAsc()
{
    return {
        AudioQuality::K128, AudioQuality::K320, AudioQuality::Flac,
        AudioQuality::Flac24Bit, AudioQuality::HiRes, AudioQuality::Atmos,
        AudioQuality::Master
    };
}

QVector<AudioQuality> allQualitiesDesc()
{
    return {
        AudioQuality::Master, AudioQuality::Atmos, AudioQuality::HiRes,
        AudioQuality::Flac24Bit, AudioQuality::Flac, AudioQuality::K320,
        AudioQuality::K128
    };
}

QVector<AudioQuality> qualityFallbackChain(AudioQuality q)
{
    const auto desc = allQualitiesDesc();
    QVector<AudioQuality> chain;
    bool started = false;
    for (auto item : desc) {
        if (!started && item == q) started = true;
        if (started) chain.append(item);
    }
    if (!started) chain = desc; // 未知音质则回退全链
    return chain;
}

// ---------------------------------------------------------------------------
// 播放模式
// ---------------------------------------------------------------------------

QString playModeId(PlayMode m)
{
    switch (m) {
    case PlayMode::Sequence: return QStringLiteral("sequence");
    case PlayMode::Loop:     return QStringLiteral("loop");
    case PlayMode::Single:   return QStringLiteral("single");
    case PlayMode::Shuffle:  return QStringLiteral("shuffle");
    }
    return QStringLiteral("sequence");
}

QString playModeName(PlayMode m)
{
    switch (m) {
    case PlayMode::Sequence: return QStringLiteral("顺序播放");
    case PlayMode::Loop:     return QStringLiteral("列表循环");
    case PlayMode::Single:   return QStringLiteral("单曲循环");
    case PlayMode::Shuffle:  return QStringLiteral("随机播放");
    }
    return QStringLiteral("顺序播放");
}

PlayMode playModeFromId(const QString &id)
{
    if (id == QStringLiteral("loop"))    return PlayMode::Loop;
    if (id == QStringLiteral("single"))  return PlayMode::Single;
    if (id == QStringLiteral("shuffle")) return PlayMode::Shuffle;
    return PlayMode::Sequence;
}

// ---------------------------------------------------------------------------
// LxSongQualityMeta
// ---------------------------------------------------------------------------

QVariantMap LxSongQualityMeta::toMap() const
{
    QVariantMap m;
    m[QStringLiteral("type")] = type;
    m[QStringLiteral("size")] = size;
    m[QStringLiteral("hash")] = hash;
    return m;
}

LxSongQualityMeta LxSongQualityMeta::fromMap(const QVariantMap &m)
{
    LxSongQualityMeta v;
    v.type = m.value(QStringLiteral("type")).toString();
    v.size = m.value(QStringLiteral("size")).toString();
    v.hash = m.value(QStringLiteral("hash")).toString();
    return v;
}

// ---------------------------------------------------------------------------
// LxSongMeta
// ---------------------------------------------------------------------------

QVariantMap LxSongMeta::toMap() const
{
    QVariantMap m;
    auto put = [&m](const QString &k, const QString &v) {
        if (!v.isEmpty()) m[k] = v;
    };
    put(QStringLiteral("source"), source);
    put(QStringLiteral("songmid"), songmid);
    put(QStringLiteral("songId"), songId);
    put(QStringLiteral("albumId"), albumId);
    put(QStringLiteral("albumMid"), albumMid);
    put(QStringLiteral("strMediaMid"), strMediaMid);
    put(QStringLiteral("hash"), hash);
    put(QStringLiteral("copyrightId"), copyrightId);
    put(QStringLiteral("lrcUrl"), lrcUrl);
    put(QStringLiteral("mrcUrl"), mrcUrl);
    put(QStringLiteral("trcUrl"), trcUrl);
    put(QStringLiteral("interval"), interval);
    put(QStringLiteral("albumName"), albumName);
    put(QStringLiteral("img"), img);
    if (!types.isEmpty()) {
        QVariantList arr;
        for (const auto &t : types) arr.append(t.toMap());
        m[QStringLiteral("types")] = arr;
    }
    return m;
}

LxSongMeta LxSongMeta::fromMap(const QVariantMap &m)
{
    LxSongMeta v;
    auto get = [&m](const QString &k) { return m.value(k).toString(); };
    v.source      = get(QStringLiteral("source"));
    v.songmid     = get(QStringLiteral("songmid"));
    v.songId      = get(QStringLiteral("songId"));
    v.albumId     = get(QStringLiteral("albumId"));
    v.albumMid    = get(QStringLiteral("albumMid"));
    v.strMediaMid = get(QStringLiteral("strMediaMid"));
    v.hash        = get(QStringLiteral("hash"));
    v.copyrightId = get(QStringLiteral("copyrightId"));
    v.lrcUrl      = get(QStringLiteral("lrcUrl"));
    v.mrcUrl      = get(QStringLiteral("mrcUrl"));
    v.trcUrl      = get(QStringLiteral("trcUrl"));
    v.interval    = get(QStringLiteral("interval"));
    v.albumName   = get(QStringLiteral("albumName"));
    v.img         = get(QStringLiteral("img"));
    const auto arr = m.value(QStringLiteral("types")).toList();
    for (const auto &item : arr)
        v.types.append(LxSongQualityMeta::fromMap(item.toMap()));
    return v;
}

// ---------------------------------------------------------------------------
// ReplayGain
// ---------------------------------------------------------------------------

QVariantMap ReplayGain::toMap() const
{
    QVariantMap m;
    m[QStringLiteral("trackGainDb")] = trackGainDb;
    m[QStringLiteral("trackPeak")] = trackPeak;
    m[QStringLiteral("albumGainDb")] = albumGainDb;
    m[QStringLiteral("albumPeak")] = albumPeak;
    m[QStringLiteral("hasTrackGain")] = hasTrackGain;
    return m;
}

ReplayGain ReplayGain::fromMap(const QVariantMap &m)
{
    ReplayGain v;
    v.trackGainDb = m.value(QStringLiteral("trackGainDb"), 0.0).toDouble();
    v.trackPeak = m.value(QStringLiteral("trackPeak"), 0.0).toDouble();
    v.albumGainDb = m.value(QStringLiteral("albumGainDb"), 0.0).toDouble();
    v.albumPeak = m.value(QStringLiteral("albumPeak"), 0.0).toDouble();
    v.hasTrackGain = m.value(QStringLiteral("hasTrackGain"), false).toBool();
    return v;
}

// ---------------------------------------------------------------------------
// Song
// ---------------------------------------------------------------------------

QVariantMap Song::toMap() const
{
    QVariantMap m;
    m[QStringLiteral("id")] = id;
    m[QStringLiteral("name")] = name;
    m[QStringLiteral("artist")] = artist;
    m[QStringLiteral("album")] = album;
    if (!albumId.isEmpty()) m[QStringLiteral("albumId")] = albumId;
    if (!cover.isEmpty()) m[QStringLiteral("cover")] = cover;
    m[QStringLiteral("duration")] = duration;
    m[QStringLiteral("platform")] = platformId(platform);
    if (hasQuality) m[QStringLiteral("quality")] = qualityId(quality);
    if (hasLx) m[QStringLiteral("lx")] = lx.toMap();
    if (isLocal()) {
        m[QStringLiteral("localPath")] = localPath;
        m[QStringLiteral("localFolder")] = localFolder;
        m[QStringLiteral("localFileSize")] = localFileSize;
        m[QStringLiteral("localModifiedAt")] = localModifiedAt;
        m[QStringLiteral("localTrackNo")] = localTrackNo;
        m[QStringLiteral("localDiscNo")] = localDiscNo;
        m[QStringLiteral("replayGain")] = replayGain.toMap();
    }
    return m;
}

Song Song::fromMap(const QVariantMap &m)
{
    Song s;
    s.id = m.value(QStringLiteral("id")).toString();
    s.name = m.value(QStringLiteral("name")).toString();
    s.artist = m.value(QStringLiteral("artist")).toString();
    s.album = m.value(QStringLiteral("album")).toString();
    s.albumId = m.value(QStringLiteral("albumId")).toString();
    s.cover = m.value(QStringLiteral("cover")).toString();
    s.duration = m.value(QStringLiteral("duration"), 0.0).toDouble();
    s.platform = platformFromId(m.value(QStringLiteral("platform")).toString());
    if (m.contains(QStringLiteral("quality"))) {
        bool ok = false;
        s.quality = qualityFromId(m.value(QStringLiteral("quality")).toString(), &ok);
        s.hasQuality = ok;
    }
    if (m.contains(QStringLiteral("lx")))
        s.lx = LxSongMeta::fromMap(m.value(QStringLiteral("lx")).toMap());
    s.hasLx = m.contains(QStringLiteral("lx"));

    if (s.isLocal()) {
        s.localPath = m.value(QStringLiteral("localPath")).toString();
        s.localFolder = m.value(QStringLiteral("localFolder")).toString();
        s.localFileSize = m.value(QStringLiteral("localFileSize"), 0).toLongLong();
        s.localModifiedAt = m.value(QStringLiteral("localModifiedAt")).toString();
        s.localTrackNo = m.value(QStringLiteral("localTrackNo"), 0).toInt();
        s.localDiscNo = m.value(QStringLiteral("localDiscNo"), 0).toInt();
        s.replayGain = ReplayGain::fromMap(m.value(QStringLiteral("replayGain")).toMap());
    }
    return s;
}

QString Song::identityKey() const
{
    if (isLocal()) return QStringLiteral("local:") + localPath;
    const QString src = lx.source.isEmpty() ? platformSourceCode(platform) : lx.source;
    return src + QStringLiteral(":") + id;
}

// ---------------------------------------------------------------------------
// 歌词
// ---------------------------------------------------------------------------

QVariantMap LyricWord::toMap() const
{
    QVariantMap m;
    m[QStringLiteral("startTime")] = startTime;
    m[QStringLiteral("endTime")] = endTime;
    m[QStringLiteral("text")] = text;
    return m;
}

QVariantMap LyricLine::toMap() const
{
    QVariantMap m;
    m[QStringLiteral("time")] = time;
    m[QStringLiteral("text")] = text;
    if (!translation.isEmpty()) m[QStringLiteral("translation")] = translation;
    if (!roman.isEmpty()) m[QStringLiteral("roman")] = roman;
    if (!words.isEmpty()) {
        QVariantList arr;
        for (const auto &w : words) arr.append(w.toMap());
        m[QStringLiteral("words")] = arr;
    }
    return m;
}

QVariantList SongLyric::toList() const
{
    QVariantList arr;
    for (const auto &l : lines) arr.append(l.toMap());
    return arr;
}

// ---------------------------------------------------------------------------
// Playlist
// ---------------------------------------------------------------------------

QVariantMap Playlist::toMap() const
{
    QVariantMap m;
    m[QStringLiteral("id")] = id;
    m[QStringLiteral("name")] = name;
    m[QStringLiteral("description")] = description;
    if (!cover.isEmpty()) m[QStringLiteral("cover")] = cover;
    m[QStringLiteral("creator")] = creator;
    m[QStringLiteral("createdAt")] = createdAt;
    m[QStringLiteral("updatedAt")] = updatedAt;
    m[QStringLiteral("isPublic")] = isPublic;
    m[QStringLiteral("platform")] = platformId(platform);
    m[QStringLiteral("isOnlineImported")] = isOnlineImported;
    if (isOnlineImported) {
        m[QStringLiteral("sourceId")] = sourceId;
        m[QStringLiteral("externalType")] = externalType;
        m[QStringLiteral("autoUpdate")] = autoUpdate;
        m[QStringLiteral("importedAt")] = importedAt;
        m[QStringLiteral("lastSyncedAt")] = lastSyncedAt;
        m[QStringLiteral("lastSyncError")] = lastSyncError;
    }
    QVariantList songArr;
    for (const auto &s : songs) songArr.append(s.toMap());
    m[QStringLiteral("songs")] = songArr;
    return m;
}

Playlist Playlist::fromMap(const QVariantMap &m)
{
    Playlist p;
    p.id = m.value(QStringLiteral("id")).toString();
    p.name = m.value(QStringLiteral("name")).toString();
    p.description = m.value(QStringLiteral("description")).toString();
    p.cover = m.value(QStringLiteral("cover")).toString();
    p.creator = m.value(QStringLiteral("creator")).toString();
    p.createdAt = m.value(QStringLiteral("createdAt")).toString();
    p.updatedAt = m.value(QStringLiteral("updatedAt")).toString();
    p.isPublic = m.value(QStringLiteral("isPublic"), false).toBool();
    p.platform = platformFromId(m.value(QStringLiteral("platform")).toString());
    p.isOnlineImported = m.value(QStringLiteral("isOnlineImported"), false).toBool();
    if (p.isOnlineImported) {
        p.sourceId = m.value(QStringLiteral("sourceId")).toString();
        p.externalType = m.value(QStringLiteral("externalType")).toString();
        p.autoUpdate = m.value(QStringLiteral("autoUpdate"), false).toBool();
        p.importedAt = m.value(QStringLiteral("importedAt")).toString();
        p.lastSyncedAt = m.value(QStringLiteral("lastSyncedAt")).toString();
        p.lastSyncError = m.value(QStringLiteral("lastSyncError")).toString();
    }
    const auto arr = m.value(QStringLiteral("songs")).toList();
    for (const auto &item : arr) p.songs.append(Song::fromMap(item.toMap()));
    return p;
}

// ---------------------------------------------------------------------------
// PlaylistSummary
// ---------------------------------------------------------------------------

QVariantMap PlaylistSummary::toMap() const
{
    QVariantMap m;
    m[QStringLiteral("id")] = id;
    m[QStringLiteral("name")] = name;
    m[QStringLiteral("creator")] = creator;
    m[QStringLiteral("cover")] = cover;
    m[QStringLiteral("trackCount")] = trackCount;
    if (hasPlayCount) m[QStringLiteral("playCount")] = playCount;
    m[QStringLiteral("platform")] = platformId(platform);
    return m;
}

PlaylistSummary PlaylistSummary::fromMap(const QVariantMap &m)
{
    PlaylistSummary p;
    p.id = m.value(QStringLiteral("id")).toString();
    p.name = m.value(QStringLiteral("name")).toString();
    p.creator = m.value(QStringLiteral("creator")).toString();
    p.cover = m.value(QStringLiteral("cover")).toString();
    p.trackCount = m.value(QStringLiteral("trackCount"), 0).toInt();
    p.hasPlayCount = m.contains(QStringLiteral("playCount"));
    p.playCount = m.value(QStringLiteral("playCount"), 0).toLongLong();
    p.platform = platformFromId(m.value(QStringLiteral("platform")).toString());
    return p;
}

// ---------------------------------------------------------------------------
// AlbumInfo
// ---------------------------------------------------------------------------

QVariantMap AlbumInfo::toMap() const
{
    QVariantMap m;
    m[QStringLiteral("id")] = id;
    m[QStringLiteral("name")] = name;
    m[QStringLiteral("artist")] = artist;
    m[QStringLiteral("artistId")] = artistId;
    m[QStringLiteral("cover")] = cover;
    m[QStringLiteral("releaseDate")] = releaseDate;
    m[QStringLiteral("description")] = description;
    m[QStringLiteral("company")] = company;
    m[QStringLiteral("platform")] = platformId(platform);
    QVariantList arr;
    for (const auto &s : songs) arr.append(s.toMap());
    m[QStringLiteral("songs")] = arr;
    return m;
}

AlbumInfo AlbumInfo::fromMap(const QVariantMap &m)
{
    AlbumInfo a;
    a.id = m.value(QStringLiteral("id")).toString();
    a.name = m.value(QStringLiteral("name")).toString();
    a.artist = m.value(QStringLiteral("artist")).toString();
    a.artistId = m.value(QStringLiteral("artistId")).toString();
    a.cover = m.value(QStringLiteral("cover")).toString();
    a.releaseDate = m.value(QStringLiteral("releaseDate")).toString();
    a.description = m.value(QStringLiteral("description")).toString();
    a.company = m.value(QStringLiteral("company")).toString();
    a.platform = platformFromId(m.value(QStringLiteral("platform")).toString());
    const auto arr = m.value(QStringLiteral("songs")).toList();
    for (const auto &item : arr) a.songs.append(Song::fromMap(item.toMap()));
    return a;
}

// ---------------------------------------------------------------------------
// ArtistInfo
// ---------------------------------------------------------------------------

QVariantMap ArtistInfo::toMap() const
{
    QVariantMap m;
    m[QStringLiteral("id")] = id;
    m[QStringLiteral("name")] = name;
    m[QStringLiteral("avatar")] = avatar;
    m[QStringLiteral("alias")] = alias;
    m[QStringLiteral("identities")] = identities;
    m[QStringLiteral("briefDesc")] = briefDesc;
    m[QStringLiteral("platform")] = platformId(platform);
    return m;
}

// ---------------------------------------------------------------------------
// ToplistInfo
// ---------------------------------------------------------------------------

QVariantMap ToplistInfo::toMap() const
{
    QVariantMap m;
    m[QStringLiteral("id")] = id;
    m[QStringLiteral("name")] = name;
    m[QStringLiteral("description")] = description;
    m[QStringLiteral("cover")] = cover;
    m[QStringLiteral("updateTime")] = updateTime;
    m[QStringLiteral("platform")] = platformId(platform);
    return m;
}

// ---------------------------------------------------------------------------
// Comment
// ---------------------------------------------------------------------------

QVariantMap Comment::toMap() const
{
    QVariantMap m;
    m[QStringLiteral("id")] = id;
    m[QStringLiteral("text")] = text;
    m[QStringLiteral("time")] = time;
    m[QStringLiteral("timeStr")] = timeStr;
    m[QStringLiteral("location")] = location;
    m[QStringLiteral("images")] = images;
    m[QStringLiteral("likedCount")] = likedCount;
    m[QStringLiteral("liked")] = liked;
    m[QStringLiteral("replyNum")] = replyNum;
    m[QStringLiteral("userName")] = userName;
    m[QStringLiteral("userAvatar")] = userAvatar;
    m[QStringLiteral("userId")] = userId;
    m[QStringLiteral("replyText")] = replyText;
    m[QStringLiteral("replyUser")] = replyUser;
    return m;
}

} // namespace Muyun
