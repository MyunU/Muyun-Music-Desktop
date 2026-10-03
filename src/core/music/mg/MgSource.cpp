#include "MgSource.h"

#include "core/utils/Format.h"
#include "core/utils/Crypto.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <algorithm>
#include <QDateTime>
#include <QRegularExpression>

namespace Muyun {

namespace {
const char *kDeviceId = "963B7AA0D21511ED807EE5846EC87D20";
const char *kSignMd5  = "6cdc72a439cef99a3418d2a78aa28c73";
const char *kTail     = "yyapp2d16148780a1dcc7408e06336b98cfd50";
const char *kMgUa =
    "Mozilla/5.0 (Linux; U; Android 11.0.0; zh-cn; MI 11 Build/OPR1.170623.032) "
    "AppleWebKit/534.30 (KHTML, like Gecko) Version/4.0 Mobile Safari/534.30";
const char *kSearchSwitch =
    "%7B%22song%22%3A1%2C%22album%22%3A0%2C%22singer%22%3A0%2C%22tagSong%22%3A1%2C"
    "%22mvSong%22%3A0%2C%22bestShow%22%3A1%2C%22songlist%22%3A0%2C%22lyricSong%22%3A0%7D";

/// 咪咕封面常为相对路径 /data/oss/...，需补全 CDN 域名，否则被 Qt 当 qrc 解析
QString normalizeMgCover(const QString &u)
{
    if (u.startsWith(QLatin1Char('/')))
        return QStringLiteral("https://d.musicapp.migu.cn") + u;
    return u;
}
} // namespace

QString MgSource::makeSign(const QString &keyword, const QString &timestamp)
{
    // 用拼接而非 QString::arg：searchSwitch/关键词中的 %xx 会被 arg 误认为占位符
    const QString raw = keyword + QString::fromUtf8(kSignMd5) +
                        QString::fromUtf8(kTail) + QString::fromUtf8(kDeviceId) +
                        timestamp;
    return Crypto::md5Hex(raw.toUtf8());
}

SearchResult MgSource::searchSongs(const QString &keyword, int page, int limit)
{
    SearchResult result;
    if (limit <= 0) limit = 20;

    const QString time = QString::number(QDateTime::currentMSecsSinceEpoch());
    // 注意：必须用拼接。若用 QString::arg，searchSwitch 内的 "%3A" 等
    // 会被误判为占位符 %3，导致 URL 被关键词内容污染（咪咕会返回 code 700）。
    const QString url =
        QStringLiteral("https://jadeite.migu.cn/music_search/v3/search/searchAll"
                       "?isCorrect=0&isCopyright=1&searchSwitch=")
        + QString::fromUtf8(kSearchSwitch)
        + QStringLiteral("&pageSize=") + QString::number(limit)
        + QStringLiteral("&text=") + QString::fromUtf8(QUrl::toPercentEncoding(keyword))
        + QStringLiteral("&pageNo=") + QString::number(page)
        + QStringLiteral("&sort=0&sid=USS");

    HttpOptions opt;
    opt.headers[QStringLiteral("uiVersion")] = QStringLiteral("A_music_3.6.1");
    opt.headers[QStringLiteral("deviceId")] = QString::fromUtf8(kDeviceId);
    opt.headers[QStringLiteral("timestamp")] = time;
    opt.headers[QStringLiteral("sign")] = makeSign(keyword, time);
    opt.headers[QStringLiteral("channel")] = QStringLiteral("0146921");
    opt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kMgUa);
    opt.timeoutMs = 20000;

    const HttpResponse resp = HttpClient::instance()->get(url, opt);
    if (!resp.ok) return result;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return result;
    const QVariantMap root = doc.object().toVariantMap();

    // songResultData 位于响应根层级，结果为分组数组的数组（resultList: [[ {...} ]]）
    QVariantMap data = root.value(QStringLiteral("songResultData")).toMap();
    if (data.isEmpty()) {
        data = root.value(QStringLiteral("data")).toMap()
                   .value(QStringLiteral("songResultData")).toMap();
    }
    const QVariantList groups = data.value(QStringLiteral("resultList")).toList();
    const QVariantList flat = groups.isEmpty()
        ? data.value(QStringLiteral("result")).toList() : QVariantList();

    auto handleItem = [&result](const QVariantMap &m) {
        const QString songId = m.value(QStringLiteral("songId")).toString();
        const QString copyrightId = m.value(QStringLiteral("copyrightId")).toString();
        if (songId.isEmpty()) return;

        Song s;
        s.id = songId;
        s.platform = Platform::Migu;
        s.name = m.value(QStringLiteral("songName")).toString();
        s.album = m.value(QStringLiteral("albumName")).toString();
        s.albumId = m.value(QStringLiteral("albumId")).toString();

        QStringList artists;
        for (const auto &a : m.value(QStringLiteral("singerList")).toList())
            artists.append(a.toMap().value(QStringLiteral("name")).toString());
        s.artist = Format::joinArtists(artists);

        QString cover = m.value(QStringLiteral("img1")).toString();
        if (cover.isEmpty()) cover = m.value(QStringLiteral("img2")).toString();
        if (cover.isEmpty()) cover = m.value(QStringLiteral("img3")).toString();
        s.cover = normalizeMgCover(cover);
        s.duration = m.value(QStringLiteral("duration")).toDouble();
        if (s.duration <= 0)
            s.duration = m.value(QStringLiteral("length")).toDouble();

        LxSongMeta lx;
        lx.source = QStringLiteral("mg");
        lx.songmid = songId;
        lx.songId = songId;
        lx.copyrightId = copyrightId;
        lx.albumId = s.albumId;
        lx.interval = Format::duration(s.duration);
        lx.img = s.cover;

        // 音质：PQ=128k, HQ=320k, SQ=flac, ZQ24=flac24bit
        for (const auto &f : m.value(QStringLiteral("audioFormats")).toList()) {
            const QVariantMap fm = f.toMap();
            const QString ft = fm.value(QStringLiteral("formatType")).toString();
            LxSongQualityMeta q;
            if (ft == QStringLiteral("ZQ24")) q.type = QStringLiteral("flac24bit");
            else if (ft == QStringLiteral("SQ")) q.type = QStringLiteral("flac");
            else if (ft == QStringLiteral("HQ")) q.type = QStringLiteral("320k");
            else if (ft == QStringLiteral("PQ")) q.type = QStringLiteral("128k");
            else continue;
            const qint64 size = fm.value(QStringLiteral("asize")).toLongLong();
            q.size = Format::fileSize(size > 0 ? size
                                               : fm.value(QStringLiteral("isize")).toLongLong());
            lx.types.append(q);
        }
        std::reverse(lx.types.begin(), lx.types.end());

        if (!lx.types.isEmpty()) {
            bool ok = false;
            s.quality = qualityFromId(lx.types.first().type, &ok);
            s.hasQuality = ok;
        }
        s.lx = lx;
        s.hasLx = true;
        result.songs.append(s);
    };

    for (const auto &g : groups) {
        if (g.canConvert<QVariantList>()) {
            for (const auto &item : g.toList()) handleItem(item.toMap());
        } else if (g.canConvert<QVariantMap>()) {
            handleItem(g.toMap());
        }
    }
    for (const auto &item : flat) handleItem(item.toMap());

    result.total = data.value(QStringLiteral("totalCount")).toInt();
    if (result.total <= 0) result.total = result.songs.size();
    result.hasMore = (page * limit) < result.total;
    return result;
}

QVariantMap MgSource::requestResource(const QString &copyrightId)
{
    if (copyrightId.isEmpty()) return {};

    const QString url = QStringLiteral(
        "https://c.musicapp.migu.cn/MIGUM2.0/v1.0/content/resourceinfo.do?resourceType=2");

    HttpOptions opt;
    opt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kMgUa);
    opt.headers[QStringLiteral("channel")] = QStringLiteral("0146921");
    opt.headers[QStringLiteral("deviceId")] = QString::fromUtf8(kDeviceId);
    opt.headers[QStringLiteral("uiVersion")] = QStringLiteral("A_music_3.6.1");
    opt.timeoutMs = 20000;

    QVariantMap form;
    form[QStringLiteral("resourceId")] = copyrightId;

    const HttpResponse resp = HttpClient::instance()->postForm(url, form, opt);
    if (!resp.ok) return {};

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return {};
    return doc.object().toVariantMap();
}

QString MgSource::getMusicUrl(const Song &song, AudioQuality quality)
{
    const QString copyrightId = song.lx.copyrightId.isEmpty()
                                    ? song.id : song.lx.copyrightId;
    if (copyrightId.isEmpty()) return QString();

    const QVariantMap root = requestResource(copyrightId);
    const QVariantList resources = root.value(QStringLiteral("resource")).toList();
    if (resources.isEmpty()) return QString();

    const QVariantMap res = resources.first().toMap();
    // 各音质候选：优先匹配目标音质
    const QString wantType = qualityId(quality);
    QString url;

    auto pick = [&url](const QVariant &v) {
        const QString u = v.toMap().value(QStringLiteral("url")).toString();
        if (!u.isEmpty()) url = u;
    };

    const QVariantList formats = res.value(QStringLiteral("newRateFormats")).toList();
    for (const auto &f : formats) {
        const QVariantMap fm = f.toMap();
        const QString ft = fm.value(QStringLiteral("formatType")).toString();
        if ((wantType == QStringLiteral("flac") && ft == QStringLiteral("SQ")) ||
            (wantType == QStringLiteral("flac24bit") && ft == QStringLiteral("ZQ24")) ||
            (wantType == QStringLiteral("320k") && ft == QStringLiteral("HQ")) ||
            (wantType == QStringLiteral("128k") && ft == QStringLiteral("PQ"))) {
            pick(fm);
            if (!url.isEmpty()) break;
        }
    }
    if (url.isEmpty()) {
        // 降级：取任一可用音质
        for (const auto &f : formats) { pick(f); if (!url.isEmpty()) break; }
    }
    if (url.isEmpty())
        url = res.value(QStringLiteral("url")).toString();

    return url;
}

SongLyric MgSource::getLyric(const Song &song)
{
    // 咪咕歌词为加密的 mrc 格式，此处返回空，
    // 由 MusicSdk::resolveLyric 的跨平台兜底机制从其它音源补齐。
    Q_UNUSED(song)
    return {};
}

// ---------------------------------------------------------------------------
// 榜单（参考工程硬编码榜单表 + querycontentbyId 详情）
// ---------------------------------------------------------------------------

QVector<ToplistInfo> MgSource::getToplists()
{
    // 咪咕 2026-09 实测：老榜单 id（19190036/23189813/15140034/2360xxxx/23603926 等）
    // 全部返回 code=302001（栏目下线）。用参考工程 2026 版新 id 表，全部实测 code=000000。
    struct Board { const char *id; const char *name; };
    static const Board boards[] = {
        {"27553319", "尖叫新歌榜"}, {"27186466", "尖叫热歌榜"}, {"27553408", "尖叫原创榜"},
        {"75959118", "音乐风向榜"}, {"76557036", "彩铃分贝榜"}, {"76557745", "会员臻爱榜"},
        {"23189800", "港台榜"},     {"23189399", "内地榜"},     {"83176390", "国风金曲榜"},
    };
    QVector<ToplistInfo> out;
    for (const auto &b : boards) {
        ToplistInfo t;
        t.id = QString::fromLatin1(b.id);
        t.name = QString::fromUtf8(b.name);
        t.platform = Platform::Migu;
        out.append(t);
    }
    return out;
}

SearchResult MgSource::getToplist(const QString &id, int page, int limit)
{
    Q_UNUSED(page)
    SearchResult result;

    const QString url = QStringLiteral(
        "https://app.c.nf.migu.cn/MIGUM2.0/v1.0/content/querycontentbyId.do"
        "?columnId=%1&needAll=0").arg(id);

    HttpOptions opt;
    opt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kMgUa);
    opt.headers[QStringLiteral("channel")] = QStringLiteral("0146921");
    opt.headers[QStringLiteral("Referer")] = QStringLiteral("https://app.c.nf.migu.cn/");
    opt.timeoutMs = 20000;

    const HttpResponse resp = HttpClient::instance()->get(url, opt);
    if (kDiag && qEnvironmentVariableIsSet("MUYUN_DEBUG_HOME"))
        printf("[MG-TOPLIST] id=%s ok=%d body=%.300s\n", qPrintable(id), resp.ok,
               resp.body.constData());
    if (!resp.ok) return result;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return result;
    const QVariantMap root = doc.object().toVariantMap();
    if (root.value(QStringLiteral("code")).toString() != QStringLiteral("000000"))
        return result;
    const QVariantList contents = root.value(QStringLiteral("columnInfo")).toMap()
                                     .value(QStringLiteral("contents")).toList();

    for (const auto &c : contents) {
        const QVariantMap m = c.toMap().value(QStringLiteral("objectInfo")).toMap();
        const QString songId = m.value(QStringLiteral("songId")).toString();
        if (songId.isEmpty()) continue;

        Song s;
        s.id = songId;
        s.platform = Platform::Migu;
        s.name = m.value(QStringLiteral("songName")).toString();
        s.album = m.value(QStringLiteral("album")).toString();
        s.albumId = m.value(QStringLiteral("albumId")).toString();
        QStringList artists;
        for (const auto &a : m.value(QStringLiteral("artists")).toList())
            artists.append(a.toMap().value(QStringLiteral("name")).toString());
        s.artist = Format::joinArtists(artists);
        const QVariantList imgs = m.value(QStringLiteral("albumImgs")).toList();
        if (!imgs.isEmpty()) s.cover = normalizeMgCover(imgs.first().toMap().value(QStringLiteral("img")).toString());
        // length 形如 "00:04:30" 或秒数
        const QString lenStr = m.value(QStringLiteral("length")).toString();
        static const QRegularExpression re(QStringLiteral("(\\d+):(\\d+)$"));
        const auto mm = re.match(lenStr);
        if (mm.hasMatch())
            s.duration = mm.captured(1).toInt() * 60 + mm.captured(2).toInt();

        LxSongMeta lx;
        lx.source = QStringLiteral("mg");
        lx.songmid = songId;
        lx.songId = songId;
        lx.copyrightId = m.value(QStringLiteral("copyrightId")).toString();
        lx.albumId = s.albumId;
        lx.interval = Format::duration(s.duration);
        lx.img = s.cover;
        s.lx = lx;
        s.hasLx = true;
        result.songs.append(s);
    }
    result.total = result.songs.size();
    Q_UNUSED(limit)
    return result;
}

// ---------------------------------------------------------------------------
// 推荐歌单 / 歌单详情
// ---------------------------------------------------------------------------

namespace {
/// 递归收集 resType==2021 的歌单项
void collectMgPlaylists(const QVariantList &contents, QVariantList &out)
{
    for (const auto &c : contents) {
        const QVariantMap m = c.toMap();
        if (m.contains(QStringLiteral("contents"))) {
            collectMgPlaylists(m.value(QStringLiteral("contents")).toList(), out);
            continue;
        }
        if (m.value(QStringLiteral("resType")).toString() != QStringLiteral("2021")) continue;
        const QString id = m.value(QStringLiteral("resId")).toString();
        const QString name = m.value(QStringLiteral("txt")).toString();
        if (id.isEmpty() || name.isEmpty()) continue;
        QVariantMap entry;
        entry[QStringLiteral("id")] = id;
        entry[QStringLiteral("name")] = name;
        entry[QStringLiteral("img")] = m.value(QStringLiteral("img")).toString();
        entry[QStringLiteral("desc")] = m.value(QStringLiteral("txt2")).toString();
        out.append(entry);
    }
}
} // namespace

QVector<PlaylistSummary> MgSource::getRecommendPlaylists(int limit)
{
    QVector<PlaylistSummary> out;
    if (limit <= 0) limit = 6;

    const QString url = QStringLiteral(
        "https://app.c.nf.migu.cn/pc/bmw/page-data/playlist-square-recommend/v1.0"
        "?templateVersion=2&pageNo=1");

    HttpOptions opt;
    opt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kMgUa);
    opt.headers[QStringLiteral("channel")] = QStringLiteral("0146921");
    opt.headers[QStringLiteral("Referer")] = QStringLiteral("https://app.c.nf.migu.cn/");
    opt.timeoutMs = 20000;

    const HttpResponse resp = HttpClient::instance()->get(url, opt);
    if (kDiag && qEnvironmentVariableIsSet("MUYUN_DEBUG_HOME"))
        printf("[MG-RCM] ok=%d body=%.300s\n", resp.ok, resp.body.constData());
    if (!resp.ok) return out;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return out;
    const QVariantMap root = doc.object().toVariantMap();
    if (root.value(QStringLiteral("code")).toString() != QStringLiteral("000000"))
        return out;

    QVariantList found;
    collectMgPlaylists(root.value(QStringLiteral("data")).toMap()
                          .value(QStringLiteral("contents")).toList(), found);

    for (const auto &item : found) {
        const QVariantMap m = item.toMap();
        PlaylistSummary p;
        p.id = m.value(QStringLiteral("id")).toString();
        p.name = m.value(QStringLiteral("name")).toString();
        p.cover = normalizeMgCover(m.value(QStringLiteral("img")).toString());
        p.platform = Platform::Migu;
        if (!p.id.isEmpty()) out.append(p);
        if (out.size() >= limit) break;
    }
    return out;
}

// ---------------------------------------------------------------------------
// 歌单广场：分类 + 分类下歌单
// ---------------------------------------------------------------------------

namespace {
HttpOptions mgPcOptions()
{
    HttpOptions opt;
    opt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kMgUa);
    opt.headers[QStringLiteral("channel")] = QStringLiteral("0146921");
    opt.headers[QStringLiteral("Referer")] = QStringLiteral("https://app.c.nf.migu.cn/");
    opt.timeoutMs = 20000;
    return opt;
}

/// taglist 模板：[{header:{title}, content:[{texts:[name,tagId]}]}]，首组为热门
QVariantList fetchMgTagList(QString *errText = nullptr)
{
    QVariantList out;
    const QString url = QStringLiteral(
        "https://app.c.nf.migu.cn/pc/v1.0/template/musiclistplaza-taglist/release");
    const HttpResponse resp = HttpClient::instance()->get(url, mgPcOptions());
    if (errText) *errText = resp.bodyText;
    if (!resp.ok) return out;
    const QVariantMap root = QJsonDocument::fromJson(resp.body).object().toVariantMap();
    if (root.value(QStringLiteral("code")).toString() != QStringLiteral("000000")) return out;
    const QVariantList groups = root.value(QStringLiteral("data")).toList();
    bool first = true;
    for (const auto &g : groups) {
        const QVariantMap gm = g.toMap();
        QStringList tags;
        for (const auto &t : gm.value(QStringLiteral("content")).toList()) {
            const QStringList texts = t.toMap().value(QStringLiteral("texts")).toStringList();
            if (texts.size() >= 2 && !texts.at(0).isEmpty()) tags.append(texts.at(0));
        }
        if (tags.isEmpty()) continue;
        const QString group = first ? QStringLiteral("热门")
                                    : gm.value(QStringLiteral("header")).toMap()
                                          .value(QStringLiteral("title")).toString();
        first = false;
        QVariantMap entry;
        entry[QStringLiteral("group")] = group.isEmpty() ? QStringLiteral("分类") : group;
        entry[QStringLiteral("tags")] = tags;
        out.append(entry);
    }
    return out;
}
} // namespace

QVariantList MgSource::playlistCategories()
{
    return fetchMgTagList();
}

QVector<PlaylistSummary> MgSource::explorePlaylists(const QString &cat, const QString &order,
                                                    int page, int limit, bool *hasMore)
{
    QVector<PlaylistSummary> out;
    Q_UNUSED(order)   // 咪咕广场模板无排序参数
    if (limit <= 0) limit = 30;
    if (page < 1) page = 1;
    if (hasMore) *hasMore = false;

    QString url;
    bool byTag = false;
    if (!cat.isEmpty() && cat != QStringLiteral("全部")) {
        // 标签名 → tagId（按需查分类，无共享状态）
        QString tagId;
        const QVariantList tr = QJsonDocument::fromJson(
            HttpClient::instance()->get(QStringLiteral(
                "https://app.c.nf.migu.cn/pc/v1.0/template/musiclistplaza-taglist/release"),
                mgPcOptions()).body).object().toVariantMap()
            .value(QStringLiteral("data")).toList();
        for (const auto &g : tr) {
            for (const auto &t : g.toMap().value(QStringLiteral("content")).toList()) {
                const QStringList texts = t.toMap().value(QStringLiteral("texts")).toStringList();
                if (texts.size() >= 2 && texts.at(0) == cat) { tagId = texts.at(1); break; }
            }
            if (!tagId.isEmpty()) break;
        }
        if (!tagId.isEmpty()) {
            byTag = true;
            url = QStringLiteral(
                      "https://app.c.nf.migu.cn/pc/v1.0/template/musiclistplaza-listbytag/release"
                      "?pageNumber=%1&templateVersion=2&tagId=%2")
                      .arg(page).arg(tagId);
        }
    }
    if (url.isEmpty()) {
        url = QStringLiteral(
                  "https://app.c.nf.migu.cn/pc/bmw/page-data/playlist-square-recommend/v1.0"
                  "?templateVersion=2&pageNo=%1").arg(page);
    }

    const HttpResponse resp = HttpClient::instance()->get(url, mgPcOptions());
    if (kDiag && qEnvironmentVariableIsSet("MUYUN_DEBUG_HOME"))
        printf("[MG-EXPLORE] cat=%s ok=%d body=%.200s\n", qPrintable(cat), resp.ok,
               resp.body.constData());
    if (!resp.ok) return out;
    const QVariantMap root = QJsonDocument::fromJson(resp.body).object().toVariantMap();
    if (root.value(QStringLiteral("code")).toString() != QStringLiteral("000000")) return out;
    const QVariantMap data = root.value(QStringLiteral("data")).toMap();

    if (byTag) {
        // data.contentItemList[1].itemList[]：{title, imageUrl, logEvent.contentId, barList[0].title}
        const QVariantList items = data.value(QStringLiteral("contentItemList")).toList()
                                       .value(1).toMap()
                                       .value(QStringLiteral("itemList")).toList();
        for (const auto &item : items) {
            const QVariantMap m = item.toMap();
            PlaylistSummary p;
            p.id = m.value(QStringLiteral("logEvent")).toMap()
                       .value(QStringLiteral("contentId")).toString();
            p.name = m.value(QStringLiteral("title")).toString();
            p.cover = normalizeMgCover(m.value(QStringLiteral("imageUrl")).toString());
            p.platform = Platform::Migu;
            if (!p.id.isEmpty() && !p.name.isEmpty()) out.append(p);
        }
        if (hasMore) *hasMore = !items.isEmpty();   // 咪咕不返回 total，有货就让继续翻
    } else {
        QVariantList found;
        collectMgPlaylists(data.value(QStringLiteral("contents")).toList(), found);
        for (const auto &item : found) {
            if (out.size() >= limit) break;   // 模板接口无视 limit，一次回 100+ 条，本地截断
            const QVariantMap m = item.toMap();
            PlaylistSummary p;
            p.id = m.value(QStringLiteral("id")).toString();
            p.name = m.value(QStringLiteral("name")).toString();
            p.cover = normalizeMgCover(m.value(QStringLiteral("img")).toString());
            p.platform = Platform::Migu;
            if (!p.id.isEmpty()) out.append(p);
        }
        if (hasMore) *hasMore = found.size() > limit;
    }
    return out;
}

Playlist MgSource::getPlaylistDetail(const QString &id)
{
    Playlist pl;
    pl.id = id;
    pl.platform = Platform::Migu;
    if (id.isEmpty()) return pl;

    const QString url = QStringLiteral(
        "https://app.c.nf.migu.cn/MIGUM3.0/resource/playlist/song/v2.0"
        "?pageNo=1&pageSize=100&playlistId=%1").arg(id);

    HttpOptions opt;
    opt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kMgUa);
    opt.headers[QStringLiteral("channel")] = QStringLiteral("0146921");
    opt.headers[QStringLiteral("Referer")] = QStringLiteral("https://m.music.migu.cn/");
    opt.timeoutMs = 20000;

    const HttpResponse resp = HttpClient::instance()->get(url, opt);
    if (kDiag && qEnvironmentVariableIsSet("MUYUN_DEBUG_HOME"))
        printf("[MG-PLDETAIL] id=%s ok=%d body=%.300s\n", qPrintable(id), resp.ok,
               resp.body.constData());
    if (!resp.ok) return pl;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return pl;
    const QVariantMap root = doc.object().toVariantMap();
    if (root.value(QStringLiteral("code")).toString() != QStringLiteral("000000"))
        return pl;
    const QVariantMap data = root.value(QStringLiteral("data")).toMap();

    static const QRegularExpression re(QStringLiteral("(\\d+):(\\d+)$"));
    for (const auto &item : data.value(QStringLiteral("songList")).toList()) {
        const QVariantMap m = item.toMap();
        const QString songId = m.value(QStringLiteral("songId")).toString();
        if (songId.isEmpty()) continue;

        Song s;
        s.id = songId;
        s.platform = Platform::Migu;
        s.name = m.value(QStringLiteral("songName")).toString();
        s.album = m.value(QStringLiteral("albumName")).toString();
        if (s.album.isEmpty()) s.album = m.value(QStringLiteral("album")).toString();
        s.albumId = m.value(QStringLiteral("albumId")).toString();
        QStringList artists;
        for (const auto &a : m.value(QStringLiteral("artists")).toList())
            artists.append(a.toMap().value(QStringLiteral("name")).toString());
        if (artists.isEmpty())
            for (const auto &a : m.value(QStringLiteral("singerList")).toList())
                artists.append(a.toMap().value(QStringLiteral("name")).toString());
        s.artist = Format::joinArtists(artists);
        QString cover = m.value(QStringLiteral("img3")).toString();
        if (cover.isEmpty()) cover = m.value(QStringLiteral("img2")).toString();
        if (cover.isEmpty()) cover = m.value(QStringLiteral("img1")).toString();
        if (cover.isEmpty()) {
            const QVariantList imgs = m.value(QStringLiteral("albumImgs")).toList();
            if (!imgs.isEmpty()) cover = imgs.first().toMap().value(QStringLiteral("img")).toString();
        }
        s.cover = normalizeMgCover(cover);
        const auto mm = re.match(m.value(QStringLiteral("length")).toString());
        if (mm.hasMatch())
            s.duration = mm.captured(1).toInt() * 60 + mm.captured(2).toInt();

        LxSongMeta lx;
        lx.source = QStringLiteral("mg");
        lx.songmid = songId;
        lx.songId = songId;
        lx.copyrightId = m.value(QStringLiteral("copyrightId")).toString();
        lx.albumId = s.albumId;
        lx.interval = Format::duration(s.duration);
        lx.img = s.cover;
        s.lx = lx;
        s.hasLx = true;
        pl.songs.append(s);
    }
    return pl;
}

} // namespace Muyun
