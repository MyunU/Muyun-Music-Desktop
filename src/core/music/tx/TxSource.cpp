#include "TxSource.h"

#include "core/utils/Format.h"
#include "core/utils/Crypto.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QByteArray>
#include <QDateTime>
#include <QRandomGenerator>
#include <QSet>

namespace Muyun {

namespace {
const char *kMusicuUrl = "https://u.y.qq.com/cgi-bin/musicu.fcg";
const char *kDesktopUA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
} // namespace

HttpResponse TxSource::musicuRequest(const QVariantMap &body, const QString &referer)
{
    HttpOptions opt;
    opt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kDesktopUA);
    opt.headers[QStringLiteral("Referer")] = referer;
    opt.headers[QStringLiteral("Content-Type")] = QStringLiteral("application/json");
    opt.timeoutMs = 20000;

    const QByteArray payload =
        QJsonDocument(QJsonObject::fromVariantMap(body)).toJson(QJsonDocument::Compact);
    HttpResponse resp = HttpClient::instance()->post(QString::fromUtf8(kMusicuUrl), payload, opt);
    if (kDiag && qEnvironmentVariableIsSet("MUYUN_DEBUG_HOME"))
        printf("[TX-REQ] %s\n[TX-RESP] %s\n", payload.left(300).constData(),
               resp.body.left(400).constData());
    return resp;
}

// ── 搜索限流熔断 ────────────────────────────────────────────────────────────
// QQ musicu 搜索对未签名请求限流很严（实测约 10 次后开始返回 req.code=2001、
// 歌曲列表恒空，静置数分钟自愈）。联想每敲一个字就查一次，不熔断必然打满。
bool TxSource::searchBlocked() const
{
    QMutexLocker lk(&m_searchMtx);
    return QDateTime::currentMSecsSinceEpoch() < m_searchBlockedUntilMs;
}

void TxSource::markSearchBlocked()
{
    QMutexLocker lk(&m_searchMtx);
    m_searchBlockedUntilMs = QDateTime::currentMSecsSinceEpoch() + kSearchCooldownMs;
}

QString TxSource::qualityPrefix(AudioQuality q)
{
    switch (q) {
    case AudioQuality::K320:      return QStringLiteral("M800");
    case AudioQuality::Flac:      return QStringLiteral("F000");
    case AudioQuality::Flac24Bit: return QStringLiteral("F000");
    case AudioQuality::HiRes:     return QStringLiteral("F000");
    case AudioQuality::Atmos:     return QStringLiteral("F000");
    case AudioQuality::Master:    return QStringLiteral("F000");
    default:                      return QStringLiteral("M500");
    }
}

QString TxSource::resolveSongId(const QString &songmid)
{
    if (songmid.isEmpty()) return QString();

    QVariantMap param;
    param[QStringLiteral("song_type")] = 0;
    param[QStringLiteral("song_mid")] = songmid;

    QVariantMap req;
    req[QStringLiteral("module")] = QStringLiteral("music.pf_song_detail_svr");
    req[QStringLiteral("method")] = QStringLiteral("get_song_detail_yqq");
    req[QStringLiteral("param")] = param;

    QVariantMap comm;
    comm[QStringLiteral("ct")] = QStringLiteral("19");
    comm[QStringLiteral("cv")] = QStringLiteral("1859");
    comm[QStringLiteral("uin")] = QStringLiteral("0");

    QVariantMap body;
    body[QStringLiteral("comm")] = comm;
    body[QStringLiteral("req")] = req;

    const HttpResponse resp = musicuRequest(body);
    if (!resp.ok) return QString();
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return QString();
    const QVariantMap data = doc.object().toVariantMap()
                                 .value(QStringLiteral("req")).toMap()
                                 .value(QStringLiteral("data")).toMap()
                                 .value(QStringLiteral("track_info")).toMap();
    return data.value(QStringLiteral("id")).toString();
}

SearchResult TxSource::searchSongs(const QString &keyword, int page, int limit)
{
    SearchResult result;
    if (limit <= 0) limit = 30;
    // 熔断中：不打网络（QQ 已限流，再打只会延长封禁），直接回空让上层走兜底
    if (searchBlocked()) return result;

    QVariantMap param;
    param[QStringLiteral("query")] = keyword;
    param[QStringLiteral("page_num")] = page;
    param[QStringLiteral("num_per_page")] = limit;
    param[QStringLiteral("search_type")] = 0;
    param[QStringLiteral("grp")] = 1;
    param[QStringLiteral("sin")] = 0;
    param[QStringLiteral("sem")] = 0;

    QVariantMap req;
    req[QStringLiteral("module")] = QStringLiteral("music.search.SearchCgiService");
    req[QStringLiteral("method")] = QStringLiteral("DoSearchForQQMusicDesktop");
    req[QStringLiteral("param")] = param;

    QVariantMap comm;
    comm[QStringLiteral("ct")] = QStringLiteral("19");
    comm[QStringLiteral("cv")] = QStringLiteral("1859");
    comm[QStringLiteral("uin")] = QStringLiteral("0");

    QVariantMap body;
    body[QStringLiteral("comm")] = comm;
    body[QStringLiteral("req")] = req;

    const HttpResponse resp = musicuRequest(body);
    if (!resp.ok) return result;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return result;
    const QVariantMap root = doc.object().toVariantMap();

    // ⚠ req.code=2001 = 被限流拦下（要求登录，feedbackURL 指向登录页），
    //   此时歌曲列表恒空。记下冷却期，后续请求直接短路，避免越打越封。
    if (root.value(QStringLiteral("req")).toMap()
            .value(QStringLiteral("code")).toInt() == 2001) {
        markSearchBlocked();
        return result;
    }

    const QVariantMap reqData = root.value(QStringLiteral("req")).toMap()
                                    .value(QStringLiteral("data")).toMap();
    const QVariantMap bodyData = reqData.value(QStringLiteral("body")).toMap();
    const QVariantList list = bodyData.value(QStringLiteral("song")).toMap()
                                  .value(QStringLiteral("list")).toList();

    for (const auto &item : list) {
        const QVariantMap m = item.toMap();
        const QString songmid = m.value(QStringLiteral("mid")).toString();
        if (songmid.isEmpty()) continue;

        Song s;
        s.id = songmid;
        s.platform = Platform::QQ;
        s.name = m.value(QStringLiteral("name")).toString() +
                 m.value(QStringLiteral("title_extra")).toString();
        s.duration = m.value(QStringLiteral("interval")).toDouble();

        // 歌手
        QStringList artists;
        for (const auto &a : m.value(QStringLiteral("singer")).toList())
            artists.append(a.toMap().value(QStringLiteral("name")).toString());
        s.artist = Format::joinArtists(artists);

        const QVariantMap album = m.value(QStringLiteral("album")).toMap();
        s.album = album.value(QStringLiteral("name")).toString();
        s.albumId = album.value(QStringLiteral("mid")).toString();

        // 封面
        if (!s.albumId.isEmpty() && s.albumId != QStringLiteral("空"))
            s.cover = QStringLiteral("https://y.gtimg.cn/music/photo_new/T002R500x500M000%1.jpg")
                          .arg(s.albumId);

        // LX 元信息
        LxSongMeta lx;
        lx.source = QStringLiteral("tx");
        lx.songmid = songmid;
        lx.songId = m.value(QStringLiteral("id")).toString();
        lx.albumMid = s.albumId;
        lx.strMediaMid = m.value(QStringLiteral("file")).toMap()
                             .value(QStringLiteral("media_mid")).toString();
        lx.interval = Format::duration(s.duration);
        lx.img = s.cover;

        // 可用音质
        const QVariantMap file = m.value(QStringLiteral("file")).toMap();
        auto addType = [&lx](const QString &type, const QVariant &sizeVal) {
            const qint64 size = sizeVal.toLongLong();
            if (size <= 0) return;
            LxSongQualityMeta meta;
            meta.type = type;
            meta.size = Format::fileSize(size);
            lx.types.append(meta);
        };
        addType(QStringLiteral("flac24bit"), file.value(QStringLiteral("size_hires")));
        addType(QStringLiteral("flac"), file.value(QStringLiteral("size_flac")));
        addType(QStringLiteral("320k"), file.value(QStringLiteral("size_320mp3")));
        addType(QStringLiteral("128k"), file.value(QStringLiteral("size_128mp3")));
        std::reverse(lx.types.begin(), lx.types.end());

        if (!lx.types.isEmpty()) {
            bool ok = false;
            s.quality = qualityFromId(lx.types.first().type, &ok);
            s.hasQuality = ok;
        }
        s.lx = lx;
        s.hasLx = true;
        result.songs.append(s);
    }

    result.total = bodyData.value(QStringLiteral("song")).toMap()
                       .value(QStringLiteral("totalnum")).toInt();
    result.hasMore = (page * limit) < result.total;
    return result;
}

QString TxSource::getMusicUrl(const Song &song, AudioQuality quality)
{
    const QString songmid = song.lx.songmid.isEmpty() ? song.id : song.lx.songmid;
    if (songmid.isEmpty()) return QString();

    const QString filename = qualityPrefix(quality) + songmid +
                             (quality == AudioQuality::Flac ||
                              quality == AudioQuality::Flac24Bit ||
                              quality == AudioQuality::HiRes
                                  ? QStringLiteral(".flac") : QStringLiteral(".mp3"));

    QVariantMap param;
    QVariantList mids;   mids.append(songmid);
    QVariantList types;  types.append(0);
    QVariantList files;  files.append(filename);
    param[QStringLiteral("guid")] = QString::number(QRandomGenerator::global()->bounded(1000000000));
    param[QStringLiteral("songmid")] = mids;
    param[QStringLiteral("songtype")] = types;
    param[QStringLiteral("filename")] = files;
    param[QStringLiteral("uin")] = QStringLiteral("0");
    param[QStringLiteral("loginflag")] = 1;
    param[QStringLiteral("platform")] = QStringLiteral("yqq");

    QVariantMap req;
    req[QStringLiteral("module")] = QStringLiteral("vkey.GetVkeyServer");
    req[QStringLiteral("method")] = QStringLiteral("CgiGetVkey");
    req[QStringLiteral("param")] = param;

    QVariantMap comm;
    comm[QStringLiteral("uin")] = QStringLiteral("0");
    comm[QStringLiteral("format")] = QStringLiteral("json");
    comm[QStringLiteral("inCharset")] = QStringLiteral("utf-8");
    comm[QStringLiteral("outCharset")] = QStringLiteral("utf-8");
    comm[QStringLiteral("notice")] = 0;
    comm[QStringLiteral("platform")] = QStringLiteral("h5");
    comm[QStringLiteral("needNewCode")] = 1;
    comm[QStringLiteral("ct")] = 24;
    comm[QStringLiteral("cv")] = 0;

    QVariantMap body;
    body[QStringLiteral("comm")] = comm;
    body[QStringLiteral("req_0")] = req;

    const HttpResponse resp = musicuRequest(body);
    if (!resp.ok) return QString();

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return QString();
    const QVariantMap root = doc.object().toVariantMap();
    const QVariantMap reqData = root.value(QStringLiteral("req_0")).toMap()
                                    .value(QStringLiteral("data")).toMap();

    // 首选：vkey 接口返回的 purl
    QString url;
    const QVariantList sip = reqData.value(QStringLiteral("sip")).toList();
    const QVariantList midInfo = reqData.value(QStringLiteral("midurlinfo")).toList();
    if (!sip.isEmpty() && !midInfo.isEmpty()) {
        const QString purl = midInfo.first().toMap().value(QStringLiteral("purl")).toString();
        if (!purl.isEmpty()) url = sip.first().toString() + purl;
    }

    if (!url.isEmpty()) return url;

    // 回退：未登录时 purl 常为空，按 media_mid 直接拼接 CDN 地址
    const QString mid = song.lx.strMediaMid.isEmpty() ? songmid : song.lx.strMediaMid;
    if (mid.isEmpty()) return QString();
    const bool lossless = (quality == AudioQuality::Flac ||
                           quality == AudioQuality::Flac24Bit ||
                           quality == AudioQuality::HiRes);
    return QStringLiteral("https://isure.stream.qqmusic.qq.com/%1%2.%3")
        .arg(qualityPrefix(quality), mid, lossless ? QStringLiteral("flac")
                                                   : QStringLiteral("mp3"));
}

SongLyric TxSource::getLyric(const Song &song)
{
    SongLyric lyric;
    const QString songmid = song.lx.songmid.isEmpty() ? song.id : song.lx.songmid;
    if (songmid.isEmpty()) return lyric;

    QVariantMap param;
    param[QStringLiteral("songmid")] = songmid;

    QVariantMap req;
    req[QStringLiteral("module")] = QStringLiteral("music.musichallSong.PlayLyricInfo");
    req[QStringLiteral("method")] = QStringLiteral("GetPlayLyricInfo");
    req[QStringLiteral("param")] = param;

    QVariantMap comm;
    comm[QStringLiteral("ct")] = QStringLiteral("19");
    comm[QStringLiteral("cv")] = QStringLiteral("1859");
    comm[QStringLiteral("uin")] = QStringLiteral("0");

    QVariantMap body;
    body[QStringLiteral("comm")] = comm;
    body[QStringLiteral("req")] = req;

    const HttpResponse resp = musicuRequest(body);
    if (!resp.ok) return lyric;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return lyric;
    const QVariantMap data = doc.object().toVariantMap()
                                 .value(QStringLiteral("req")).toMap()
                                 .value(QStringLiteral("data")).toMap();

    // QQ 返回 base64 编码的歌词
    auto decode = [](const QVariant &v) {
        const QString raw = v.toString();
        if (raw.isEmpty()) return QString();
        const QByteArray decoded = QByteArray::fromBase64(raw.toUtf8());
        return QString::fromUtf8(decoded);
    };

    lyric.rawLrc = decode(data.value(QStringLiteral("lyric")));
    lyric.rawTranslation = decode(data.value(QStringLiteral("trans")));
    lyric.hasTranslation = !lyric.rawTranslation.isEmpty();
    return lyric;
}

QVector<PlaylistSummary> TxSource::getRecommendPlaylists(int limit)
{
    QVector<PlaylistSummary> out;
    if (limit <= 0) limit = 6;

    QVariantMap param;
    param[QStringLiteral("id")] = 10000000;   // 默认"全部"标签
    param[QStringLiteral("sin")] = 0;
    param[QStringLiteral("size")] = limit;
    param[QStringLiteral("order")] = QStringLiteral("hot");
    param[QStringLiteral("cur_page")] = 1;

    QVariantMap req;
    req[QStringLiteral("module")] = QStringLiteral("playlist.PlayListPlazaServer");
    req[QStringLiteral("method")] = QStringLiteral("get_playlist_by_tag");
    req[QStringLiteral("param")] = param;

    QVariantMap comm;
    comm[QStringLiteral("ct")] = 20;
    comm[QStringLiteral("cv")] = 1602;

    QVariantMap body;
    body[QStringLiteral("comm")] = comm;
    body[QStringLiteral("playlist")] = req;   // 注意 key 是 playlist 而非 req

    const HttpResponse resp = musicuRequest(body);
    if (!resp.ok) return out;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return out;
    const QVariantList list = doc.object().toVariantMap()
                                  .value(QStringLiteral("playlist")).toMap()
                                  .value(QStringLiteral("data")).toMap()
                                  .value(QStringLiteral("v_playlist")).toList();

    for (const auto &item : list) {
        const QVariantMap m = item.toMap();
        PlaylistSummary p;
        p.id = m.value(QStringLiteral("tid")).toString();
        if (p.id.isEmpty()) p.id = QString::number(m.value(QStringLiteral("tid")).toLongLong());
        p.name = m.value(QStringLiteral("title")).toString();
        p.cover = m.value(QStringLiteral("cover_url_medium")).toString();
        if (p.cover.isEmpty()) p.cover = m.value(QStringLiteral("cover")).toString();
        p.creator = m.value(QStringLiteral("creator_info")).toMap()
                       .value(QStringLiteral("nick")).toString();
        p.playCount = static_cast<qint64>(m.value(QStringLiteral("access_num")).toDouble());
        p.hasPlayCount = p.playCount > 0;
        p.trackCount = m.value(QStringLiteral("song_ids")).toList().size();
        p.platform = Platform::QQ;
        if (!p.id.isEmpty()) out.append(p);
    }
    return out;
}

QVector<ToplistInfo> TxSource::getToplists()
{
    // 2026-09-30 实测失效榜单：MV榜 (topId=201) 详情接口返回 0 首歌曲。
    static const QSet<QString> kDeadIds = { QStringLiteral("201") };

    QVector<ToplistInfo> out;

    QVariantMap req;
    req[QStringLiteral("module")] = QStringLiteral("musicToplist.ToplistInfoServer");
    req[QStringLiteral("method")] = QStringLiteral("GetAll");
    req[QStringLiteral("param")] = QVariantMap();

    QVariantMap comm;
    comm[QStringLiteral("ct")] = QStringLiteral("19");
    comm[QStringLiteral("cv")] = QStringLiteral("1859");
    comm[QStringLiteral("uin")] = QStringLiteral("0");

    QVariantMap body;
    body[QStringLiteral("comm")] = comm;
    body[QStringLiteral("req")] = req;

    const HttpResponse resp = musicuRequest(body);
    if (!resp.ok) return out;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return out;
    const QVariantList groups = doc.object().toVariantMap()
                                    .value(QStringLiteral("req")).toMap()
                                    .value(QStringLiteral("data")).toMap()
                                    .value(QStringLiteral("group")).toList();

    for (const auto &g : groups) {
        const QVariantList toplists = g.toMap().value(QStringLiteral("toplist")).toList();
        for (const auto &t : toplists) {
            const QVariantMap m = t.toMap();
            const QString id = m.value(QStringLiteral("topId")).toString();
            if (kDeadIds.contains(id)) continue;
            ToplistInfo info;
            info.id = id;
            info.name = m.value(QStringLiteral("title")).toString();
            info.description = m.value(QStringLiteral("intro")).toString();
            info.cover = m.value(QStringLiteral("headPicUrl")).toString();
            info.updateTime = m.value(QStringLiteral("updateTime")).toString();
            info.platform = Platform::QQ;
            out.append(info);
        }
    }
    return out;
}

SearchResult TxSource::getToplist(const QString &id, int page, int limit)
{
    SearchResult result;

    QVariantMap param;
    param[QStringLiteral("topId")] = id.toInt();
    param[QStringLiteral("offset")] = (page - 1) * limit;
    param[QStringLiteral("num")] = limit;
    param[QStringLiteral("period")] = QStringLiteral("");

    QVariantMap req;
    req[QStringLiteral("module")] = QStringLiteral("musicToplist.ToplistInfoServer");
    req[QStringLiteral("method")] = QStringLiteral("GetDetail");
    req[QStringLiteral("param")] = param;

    QVariantMap comm;
    comm[QStringLiteral("ct")] = QStringLiteral("19");
    comm[QStringLiteral("cv")] = QStringLiteral("1859");
    comm[QStringLiteral("uin")] = QStringLiteral("0");

    QVariantMap body;
    body[QStringLiteral("comm")] = comm;
    body[QStringLiteral("req")] = req;

    const HttpResponse resp = musicuRequest(body);
    if (!resp.ok) return result;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return result;
    const QVariantMap data = doc.object().toVariantMap()
                                 .value(QStringLiteral("req")).toMap()
                                 .value(QStringLiteral("data")).toMap();

    result.total = data.value(QStringLiteral("totalNum")).toInt();
    for (const auto &item : data.value(QStringLiteral("songInfoList")).toList()) {
        const QVariantMap m = item.toMap();
        Song s;
        s.id = m.value(QStringLiteral("mid")).toString();
        s.platform = Platform::QQ;
        s.name = m.value(QStringLiteral("name")).toString();
        s.duration = m.value(QStringLiteral("interval")).toDouble();
        QStringList artists;
        for (const auto &a : m.value(QStringLiteral("singer")).toList())
            artists.append(a.toMap().value(QStringLiteral("name")).toString());
        s.artist = Format::joinArtists(artists);
        const QVariantMap album = m.value(QStringLiteral("album")).toMap();
        s.album = album.value(QStringLiteral("name")).toString();
        s.albumId = album.value(QStringLiteral("mid")).toString();
        if (!s.albumId.isEmpty())
            s.cover = QStringLiteral("https://y.gtimg.cn/music/photo_new/T002R500x500M000%1.jpg")
                          .arg(s.albumId);
        LxSongMeta lx;
        lx.source = QStringLiteral("tx");
        lx.songmid = s.id;
        // 评论接口需要数字 BizId（lx.songId），榜单详情返回的 id 字段可用；
        // 缺它则 lx.songId 为空 → getComments 回退到 song.id（mid 串）→ 请求必失败。
        lx.songId = m.value(QStringLiteral("id")).toString();
        lx.albumMid = s.albumId;
        lx.strMediaMid = m.value(QStringLiteral("file")).toMap()
                             .value(QStringLiteral("media_mid")).toString();
        s.lx = lx;
        s.hasLx = true;
        result.songs.append(s);
    }
    result.hasMore = (page * limit) < result.total;
    return result;
}

Playlist TxSource::getPlaylistDetail(const QString &id)
{
    Playlist pl;
    pl.id = id;
    pl.platform = Platform::QQ;

    // musicu 的 CgiGetDiss 已被风控(500003)，改用参考工程验证过的
    // fcg_ucc_getcdinfo_byids_cp GET 接口
    const QString url = QStringLiteral(
        "https://c.y.qq.com/qzone/fcg-bin/fcg_ucc_getcdinfo_byids_cp.fcg"
        "?type=1&json=1&utf8=1&onlysong=0&new_format=1&disstid=%1"
        "&loginUin=0&hostUin=0&format=json&inCharset=utf8&outCharset=utf-8"
        "&notice=0&platform=yqq.json&needNewCode=0").arg(id);

    HttpOptions opt;
    opt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kDesktopUA);
    opt.headers[QStringLiteral("Origin")] = QStringLiteral("https://y.qq.com");
    opt.headers[QStringLiteral("Referer")] =
        QStringLiteral("https://y.qq.com/n/yqq/playsquare/%1.html").arg(id);
    opt.timeoutMs = 20000;

    const HttpResponse resp = HttpClient::instance()->get(url, opt);
    if (kDiag && qEnvironmentVariableIsSet("MUYUN_DEBUG_HOME"))
        printf("[TX-PLDETAIL] id=%s body=%.300s\n", qPrintable(id), resp.body.constData());
    if (!resp.ok) return pl;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return pl;
    const QVariantMap root = doc.object().toVariantMap();
    const QVariantList cdlist = root.value(QStringLiteral("cdlist")).toList();
    if (cdlist.isEmpty()) return pl;
    const QVariantMap cd = cdlist.first().toMap();

    pl.name = cd.value(QStringLiteral("dissname")).toString();
    pl.description = cd.value(QStringLiteral("desc")).toString();
    pl.cover = cd.value(QStringLiteral("logo")).toString();
    pl.creator = cd.value(QStringLiteral("nickname")).toString();

    for (const auto &item : cd.value(QStringLiteral("songlist")).toList()) {
        const QVariantMap m = item.toMap();
        Song s;
        s.id = m.value(QStringLiteral("mid")).toString();
        s.platform = Platform::QQ;
        s.name = m.value(QStringLiteral("title")).toString();
        s.duration = m.value(QStringLiteral("interval")).toDouble();
        QStringList artists;
        for (const auto &a : m.value(QStringLiteral("singer")).toList())
            artists.append(a.toMap().value(QStringLiteral("name")).toString());
        s.artist = Format::joinArtists(artists);
        const QVariantMap album = m.value(QStringLiteral("album")).toMap();
        s.album = album.value(QStringLiteral("name")).toString();
        s.albumId = album.value(QStringLiteral("mid")).toString();
        if (!s.albumId.isEmpty())
            s.cover = QStringLiteral("https://y.gtimg.cn/music/photo_new/T002R500x500M000%1.jpg")
                          .arg(s.albumId);

        LxSongMeta lx;
        lx.source = QStringLiteral("tx");
        lx.songmid = s.id;
        // 评论接口需要数字 BizId（lx.songId）；歌单详情返回 id 字段可用。
        lx.songId = m.value(QStringLiteral("id")).toString();
        lx.albumMid = s.albumId;
        const QVariantMap file = m.value(QStringLiteral("file")).toMap();
        lx.strMediaMid = file.value(QStringLiteral("media_mid")).toString();
        auto addType = [&lx](const QString &type, const QVariant &sizeVal) {
            if (sizeVal.toLongLong() <= 0) return;
            LxSongQualityMeta q;
            q.type = type;
            q.size = Format::fileSize(sizeVal.toLongLong());
            lx.types.append(q);
        };
        addType(QStringLiteral("flac24bit"), file.value(QStringLiteral("size_hires")));
        addType(QStringLiteral("flac"), file.value(QStringLiteral("size_flac")));
        addType(QStringLiteral("320k"), file.value(QStringLiteral("size_320mp3")));
        addType(QStringLiteral("128k"), file.value(QStringLiteral("size_128mp3")));
        s.lx = lx;
        s.hasLx = true;
        pl.songs.append(s);
    }
    return pl;
}

// ---------------------------------------------------------------------------
// 歌单广场：分类 + 分类下歌单（musicu.fcg，同推荐歌单族）
// ---------------------------------------------------------------------------

QVariantList TxSource::playlistCategories()
{
    QVariantList out;
    QVariantMap comm;
    comm[QStringLiteral("ct")] = 20;
    comm[QStringLiteral("cv")] = 1602;
    QVariantMap req;
    req[QStringLiteral("module")] = QStringLiteral("playlist.PlaylistAllCategoriesServer");
    req[QStringLiteral("method")] = QStringLiteral("get_all_categories");
    req[QStringLiteral("param")] = QVariantMap{{QStringLiteral("qq"), QString()}};
    QVariantMap body;
    body[QStringLiteral("comm")] = comm;
    body[QStringLiteral("tags")] = req;

    const HttpResponse resp = musicuRequest(body);
    if (!resp.ok) return out;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body);
    if (doc.object().toVariantMap().value(QStringLiteral("code")).toInt() != 0) return out;
    const QVariantList groups = doc.object().toVariantMap()
                                    .value(QStringLiteral("tags")).toMap()
                                    .value(QStringLiteral("data")).toMap()
                                    .value(QStringLiteral("v_group")).toList();
    for (const auto &g : groups) {
        const QVariantMap gm = g.toMap();
        QStringList tags;
        for (const auto &t : gm.value(QStringLiteral("v_item")).toList())
            tags.append(t.toMap().value(QStringLiteral("name")).toString());
        if (tags.isEmpty()) continue;
        QVariantMap entry;
        entry[QStringLiteral("group")] = gm.value(QStringLiteral("group_name")).toString();
        entry[QStringLiteral("tags")] = tags;
        out.append(entry);
    }
    return out;
}

QVector<PlaylistSummary> TxSource::explorePlaylists(const QString &cat, const QString &order,
                                                    int page, int limit, bool *hasMore)
{
    QVector<PlaylistSummary> out;
    if (limit <= 0) limit = 30;
    if (page < 1) page = 1;
    if (hasMore) *hasMore = false;

    // 标签名 → titleid（无共享状态：按需查一次分类拿 id）
    QString titleId;
    if (!cat.isEmpty() && cat != QStringLiteral("全部")) {
        QVariantMap comm;
        comm[QStringLiteral("ct")] = 20;
        comm[QStringLiteral("cv")] = 1602;
        QVariantMap req;
        req[QStringLiteral("module")] = QStringLiteral("playlist.PlaylistAllCategoriesServer");
        req[QStringLiteral("method")] = QStringLiteral("get_all_categories");
        req[QStringLiteral("param")] = QVariantMap{{QStringLiteral("qq"), QString()}};
        QVariantMap body;
        body[QStringLiteral("comm")] = comm;
        body[QStringLiteral("tags")] = req;
        const HttpResponse tr = musicuRequest(body);
        if (tr.ok) {
            const QVariantList groups = QJsonDocument::fromJson(tr.body).object()
                                            .toVariantMap().value(QStringLiteral("tags")).toMap()
                                            .value(QStringLiteral("data")).toMap()
                                            .value(QStringLiteral("v_group")).toList();
            for (const auto &g : groups) {
                bool hit = false;
                for (const auto &t : g.toMap().value(QStringLiteral("v_item")).toList()) {
                    const QVariantMap tm = t.toMap();
                    if (tm.value(QStringLiteral("name")).toString() != cat) continue;
                    titleId = QString::number(tm.value(QStringLiteral("id")).toLongLong());
                    hit = true;
                    break;
                }
                if (hit) break;
            }
        }
    }

    QVariantMap comm;
    comm[QStringLiteral("ct")] = 20;
    comm[QStringLiteral("cv")] = 1602;
    QVariantMap req;
    QVariantMap param;
    QVariantMap body;
    bool catMode = false;

    if (!titleId.isEmpty()) {
        catMode = true;
        const long long tid = titleId.toLongLong();
        param[QStringLiteral("titleid")] = tid;
        param[QStringLiteral("caller")] = QStringLiteral("0");
        param[QStringLiteral("category_id")] = tid;
        param[QStringLiteral("size")] = limit;
        param[QStringLiteral("page")] = page - 1;
        param[QStringLiteral("use_page")] = 1;
        req[QStringLiteral("module")] = QStringLiteral("playlist.PlayListCategoryServer");
        req[QStringLiteral("method")] = QStringLiteral("get_category_content");
    } else {
        param[QStringLiteral("id")] = 10000000;
        param[QStringLiteral("sin")] = limit * (page - 1);
        param[QStringLiteral("size")] = limit;
        param[QStringLiteral("order")] = order == QStringLiteral("new") ? 2 : 5;
        param[QStringLiteral("cur_page")] = page;
        req[QStringLiteral("module")] = QStringLiteral("playlist.PlayListPlazaServer");
        req[QStringLiteral("method")] = QStringLiteral("get_playlist_by_tag");
    }
    req[QStringLiteral("param")] = param;
    body[QStringLiteral("comm")] = comm;
    body[QStringLiteral("playlist")] = req;

    const HttpResponse resp = musicuRequest(body);
    if (kDiag && qEnvironmentVariableIsSet("MUYUN_DEBUG_HOME"))
        printf("[TX-EXPLORE] cat=%s ok=%d body=%.300s\n", qPrintable(cat), resp.ok,
               resp.body.constData());
    if (!resp.ok) return out;
    const QVariantMap pdata = QJsonDocument::fromJson(resp.body).object().toVariantMap()
                                  .value(QStringLiteral("playlist")).toMap()
                                  .value(QStringLiteral("data")).toMap();

    if (catMode) {
        const QVariantMap content = pdata.value(QStringLiteral("content")).toMap();
        for (const auto &item : content.value(QStringLiteral("v_item")).toList()) {
            const QVariantMap basic = item.toMap().value(QStringLiteral("basic")).toMap();
            if (basic.isEmpty()) continue;
            PlaylistSummary p;
            p.id = QString::number(basic.value(QStringLiteral("tid")).toLongLong());
            p.name = basic.value(QStringLiteral("title")).toString();
            const QVariantMap cover = basic.value(QStringLiteral("cover")).toMap();
            p.cover = cover.value(QStringLiteral("medium_url")).toString();
            if (p.cover.isEmpty()) p.cover = cover.value(QStringLiteral("default_url")).toString();
            p.creator = basic.value(QStringLiteral("creator")).toMap()
                            .value(QStringLiteral("nick")).toString();
            p.playCount = static_cast<qint64>(basic.value(QStringLiteral("play_cnt")).toDouble());
            p.hasPlayCount = p.playCount > 0;
            p.platform = Platform::QQ;
            if (!p.id.isEmpty() && !p.name.isEmpty()) out.append(p);
        }
        if (hasMore) {
            const int total = content.value(QStringLiteral("total_cnt")).toInt();
            *hasMore = total > 0 && page * limit < total;
        }
    } else {
        for (const auto &item : pdata.value(QStringLiteral("v_playlist")).toList()) {
            const QVariantMap m = item.toMap();
            PlaylistSummary p;
            p.id = QString::number(m.value(QStringLiteral("tid")).toLongLong());
            p.name = m.value(QStringLiteral("title")).toString();
            p.cover = m.value(QStringLiteral("cover_url_medium")).toString();
            p.creator = m.value(QStringLiteral("creator_info")).toMap()
                            .value(QStringLiteral("nick")).toString();
            p.playCount = static_cast<qint64>(m.value(QStringLiteral("access_num")).toDouble());
            p.hasPlayCount = p.playCount > 0;
            p.platform = Platform::QQ;
            if (!p.id.isEmpty() && !p.name.isEmpty()) out.append(p);
        }
        if (hasMore) {
            const int total = pdata.value(QStringLiteral("total")).toInt();
            *hasMore = total > 0 && page * limit < total;
        }
    }
    return out;
}

CommentPage TxSource::getComments(const Song &song, int page, int limit, bool hot)
{
    // QQ 评论走 musicu.fcg，module=music.globalComment.CommentRead。
    // 热门=GetHotCommentList（带 HotType=1），最新=GetNewCommentList。
    // BizId=数字 songId（lx.songId）；song.id 是 mid 字符串用不了。与搜索一样无需 sign。
    CommentPage out;
    // lx.songId 缺失时（洛雪同步/老收藏常缺）按 songmid 实时解析数字 id——
    // 参照 lx-music 的 tx/comment.js（getSongId 回退 get_song_detail_yqq）。
    // 直接回退到 song.id（mid 串）必失败 → 面板永远"暂无评论"。
    QString songId = song.lx.songId;
    if (songId.isEmpty()) {
        const QString songmid = song.lx.songmid.isEmpty() ? song.id : song.lx.songmid;
        songId = resolveSongId(songmid);
    }
    if (songId.isEmpty()) return out;

    QVariantMap param;
    param[QStringLiteral("BizType")] = 1;            // 1 = 歌曲
    param[QStringLiteral("BizId")] = songId;
    param[QStringLiteral("PageSize")] = limit;
    param[QStringLiteral("PageNum")] = page - 1;     // 0-indexed
    if (hot) param[QStringLiteral("HotType")] = 1;

    QVariantMap req;
    req[QStringLiteral("module")] = QStringLiteral("music.globalComment.CommentRead");
    req[QStringLiteral("method")] = hot ? QStringLiteral("GetHotCommentList")
                                        : QStringLiteral("GetNewCommentList");
    req[QStringLiteral("param")] = param;

    QVariantMap comm;
    comm[QStringLiteral("ct")] = QStringLiteral("24");
    comm[QStringLiteral("cv")] = QStringLiteral("0");
    comm[QStringLiteral("uin")] = QStringLiteral("0");

    QVariantMap body;
    body[QStringLiteral("comm")] = comm;
    body[QStringLiteral("req")] = req;

    const HttpResponse resp = musicuRequest(body);
    if (!resp.ok) return out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return out;
    const QVariantMap root = doc.object().toVariantMap();
    const QVariantMap data = root.value(QStringLiteral("req")).toMap()
                                   .value(QStringLiteral("data")).toMap();
    if (data.isEmpty()) return out;

    // 评论列表在 data.CommentList.Comments[]（CommentList 是对象，内含 Comments 数组）
    const QVariantMap commentList = data.value(QStringLiteral("CommentList")).toMap();
    // 总数在 CommentList.Total（参考 lx-music）；TotalCmNum 仅部分响应存在，作兜底
    out.total = commentList.value(QStringLiteral("Total")).toInt();
    if (out.total <= 0) out.total = data.value(QStringLiteral("TotalCmNum")).toInt();
    const QVariantList list = commentList.value(QStringLiteral("Comments")).toList();
    for (const auto &item : list) {
        const QVariantMap m = item.toMap();
        Comment c;
        c.id = m.value(QStringLiteral("CmId")).toString();
        c.text = m.value(QStringLiteral("Content")).toString();
        c.userName = m.value(QStringLiteral("Nick")).toString();
        c.userAvatar = m.value(QStringLiteral("Avatar")).toString();
        c.likedCount = m.value(QStringLiteral("PraiseNum")).toInt();
        c.replyNum = m.value(QStringLiteral("ReplyCnt")).toInt();
        c.location = m.value(QStringLiteral("Location")).toString();
        const qint64 t = m.value(QStringLiteral("PubTime")).toLongLong();
        if (t > 0) { c.time = t * 1000; c.timeStr = Format::dateTime(c.time); }
        if (!c.text.isEmpty() || !c.userName.isEmpty()) out.comments.append(c);
    }
    out.page = page;
    out.maxPage = out.total > 0 ? (out.total + limit - 1) / limit : 1;
    return out;
}


} // namespace Muyun
