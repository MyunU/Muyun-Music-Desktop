#include "KgSource.h"

#include "core/utils/Format.h"
#include "core/utils/Crypto.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <algorithm>

namespace Muyun {

namespace {
const char *kKgUa =
    "Mozilla/5.0 (iPhone; CPU iPhone OS 16_0 like Mac OS X) AppleWebKit/605.1.15 "
    "(KHTML, like Gecko) Version/16.0 Mobile/15E148 Safari/604.1";
} // namespace

QString KgSource::machineId()
{
    static QString mid;
    if (mid.isEmpty()) mid = Crypto::randomHex(16);
    return mid;
}

SearchResult KgSource::searchSongs(const QString &keyword, int page, int limit)
{
    SearchResult result;
    if (limit <= 0) limit = 30;

    const QString url =
        QStringLiteral("https://songsearch.kugou.com/song_search_v2?keyword=%1"
                       "&page=%2&pagesize=%3&userid=0&clientver=&platform=WebFilter"
                       "&filter=2&iscorrection=1&privilege_filter=0&area_code=1")
            .arg(QString::fromUtf8(QUrl::toPercentEncoding(keyword)))
            .arg(page)
            .arg(limit);

    HttpOptions opt;
    opt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kKgUa);
    opt.headers[QStringLiteral("Referer")] = QStringLiteral("https://www.kugou.com/");
    opt.timeoutMs = 20000;

    const HttpResponse resp = HttpClient::instance()->get(url, opt);
    if (!resp.ok) return result;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return result;
    const QVariantMap root = doc.object().toVariantMap();
    const QVariantMap data = root.value(QStringLiteral("data")).toMap();
    const QVariantList list = data.value(QStringLiteral("lists")).toList();

    result.total = data.value(QStringLiteral("total")).toInt();

    for (const auto &item : list) {
        const QVariantMap m = item.toMap();
        const QString audioId = m.value(QStringLiteral("Audioid")).toString();
        if (audioId.isEmpty()) continue;

        Song s;
        s.id = audioId;
        s.platform = Platform::Kugou;
        // 歌名/歌手常带 HTML 实体
        s.name = m.value(QStringLiteral("SongName")).toString();
        s.name.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
        s.name.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
        s.name.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
        s.name.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
        s.name.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
        s.name.replace(QStringLiteral("<em>"), QString());
        s.name.replace(QStringLiteral("</em>"), QString());

        QStringList artists;
        for (const auto &a : m.value(QStringLiteral("Singers")).toList()) {
            QString name = a.toMap().value(QStringLiteral("name")).toString();
            name.replace(QStringLiteral("<em>"), QString());
            name.replace(QStringLiteral("</em>"), QString());
            artists.append(name);
        }
        s.artist = Format::joinArtists(artists);

        s.album = m.value(QStringLiteral("AlbumName")).toString();
        s.albumId = m.value(QStringLiteral("AlbumID")).toString();
        s.duration = m.value(QStringLiteral("Duration")).toDouble();
        // 专辑封面：AlbumImage 带 {size} 占位符，替换为 240
        QString cover = m.value(QStringLiteral("AlbumImage")).toString();
        cover.replace(QStringLiteral("{size}"), QStringLiteral("240"));
        s.cover = cover;

        LxSongMeta lx;
        lx.source = QStringLiteral("kg");
        lx.songmid = audioId;
        lx.albumId = s.albumId;
        lx.interval = Format::duration(s.duration);
        lx.img = s.cover;

        auto addType = [&lx](const QString &type, const QVariant &sizeVal,
                             const QVariant &hashVal) {
            const qint64 size = sizeVal.toLongLong();
            if (size <= 0) return;
            LxSongQualityMeta q;
            q.type = type;
            q.size = Format::fileSize(size);
            q.hash = hashVal.toString();
            lx.types.append(q);
        };
        addType(QStringLiteral("flac24bit"), m.value(QStringLiteral("ResFileSize")),
                m.value(QStringLiteral("ResFileHash")));
        addType(QStringLiteral("flac"), m.value(QStringLiteral("SQFileSize")),
                m.value(QStringLiteral("SQFileHash")));
        addType(QStringLiteral("320k"), m.value(QStringLiteral("HQFileSize")),
                m.value(QStringLiteral("HQFileHash")));
        addType(QStringLiteral("128k"), m.value(QStringLiteral("FileSize")),
                m.value(QStringLiteral("FileHash")));
        std::reverse(lx.types.begin(), lx.types.end());

        // 默认 hash 用于兜底取链接
        lx.hash = m.value(QStringLiteral("FileHash")).toString();

        if (!lx.types.isEmpty()) {
            bool ok = false;
            s.quality = qualityFromId(lx.types.first().type, &ok);
            s.hasQuality = ok;
        }
        s.lx = lx;
        s.hasLx = true;
        result.songs.append(s);
    }

    result.hasMore = (page * limit) < result.total;
    return result;
}

QVariantMap KgSource::fetchPlayData(const QString &hash)
{
    if (hash.isEmpty()) return {};

    const QString mid = machineId();
    const QString url =
        QStringLiteral("https://wwwapi.kugou.com/yy/index.php?r=play/getdata"
                       "&hash=%1&dfid=-&mid=%2&platid=4").arg(hash, mid);

    HttpOptions opt;
    opt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kKgUa);
    opt.headers[QStringLiteral("Cookie")] =
        QStringLiteral("kg_mid=%1").arg(mid);
    opt.headers[QStringLiteral("Referer")] = QStringLiteral("https://www.kugou.com/");
    opt.timeoutMs = 20000;

    const HttpResponse resp = HttpClient::instance()->get(url, opt);
    if (!resp.ok) return {};

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return {};
    return doc.object().toVariantMap().value(QStringLiteral("data")).toMap();
}

QString KgSource::hashForQuality(const Song &song, AudioQuality quality)
{
    // 从已有音质列表里按目标音质找对应 hash，找不到就降级
    const auto chain = qualityFallbackChain(quality);
    for (auto q : chain) {
        const QString typeId = qualityId(q);
        for (const auto &t : song.lx.types) {
            if (t.type == typeId && !t.hash.isEmpty()) return t.hash;
        }
    }
    if (!song.lx.hash.isEmpty()) return song.lx.hash;
    if (!song.lx.types.isEmpty()) return song.lx.types.last().hash;
    return QString();
}

QString KgSource::getMusicUrl(const Song &song, AudioQuality quality)
{
    const QString hash = hashForQuality(song, quality);
    if (hash.isEmpty()) return QString();

    const QVariantMap data = fetchPlayData(hash);
    QString url = data.value(QStringLiteral("play_url")).toString();
    if (url.isEmpty()) return QString();
    return url.replace(QStringLiteral("\\/"), QStringLiteral("/"));
}

SongLyric KgSource::getLyric(const Song &song)
{
    SongLyric lyric;
    const QString hash = hashForQuality(song, AudioQuality::K320);
    if (hash.isEmpty()) return lyric;

    const QVariantMap data = fetchPlayData(hash);
    lyric.rawLrc = data.value(QStringLiteral("lyrics")).toString();
    return lyric;
}

// ---------------------------------------------------------------------------
// 榜单（参考工程硬编码榜单表 + v3/rank/song 详情）
// ---------------------------------------------------------------------------

QVector<ToplistInfo> KgSource::getToplists()
{
    struct Board { const char *id; const char *name; };
    static const Board boards[] = {
        {"8888", "TOP500"}, {"6666", "飙升榜"}, {"23784", "网络红歌榜"},
        {"21101", "分享榜"}, {"33164", "纯音乐榜"}, {"33161", "古风榜"},
        {"33165", "粤语金曲榜"}, {"33166", "欧美金曲榜"}, {"33160", "电音榜"},
        {"24971", "DJ热歌榜"}, {"31308", "内地榜"}, {"52144", "抖音热歌榜"},
        {"52767", "快手热歌榜"}, {"44412", "说唱先锋榜"}, {"51341", "民谣榜"},
        {"31310", "欧美榜"}, {"31311", "韩国榜"}, {"31312", "日本榜"},
    };
    QVector<ToplistInfo> out;
    for (const auto &b : boards) {
        ToplistInfo t;
        t.id = QString::fromLatin1(b.id);
        t.name = QString::fromUtf8(b.name);
        t.platform = Platform::Kugou;
        out.append(t);
    }
    return out;
}

SearchResult KgSource::getToplist(const QString &id, int page, int limit)
{
    SearchResult result;
    if (limit <= 0) limit = 100;
    const QString url =
        QStringLiteral("http://mobilecdnbj.kugou.com/api/v3/rank/song?version=9108"
                       "&ranktype=1&plat=0&pagesize=%1&area_code=1&page=%2"
                       "&rankid=%3&with_res_tag=0&show_portrait_mv=1")
            .arg(limit).arg(page).arg(id);

    HttpOptions opt;
    opt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kKgUa);
    opt.headers[QStringLiteral("Referer")] = QStringLiteral("https://www.kugou.com/");
    opt.timeoutMs = 20000;

    const HttpResponse resp = HttpClient::instance()->get(url, opt);
    if (kDiag && qEnvironmentVariableIsSet("MUYUN_DEBUG_HOME"))
        printf("[KG-TOPLIST] id=%s ok=%d body=%.300s\n", qPrintable(id), resp.ok,
               resp.body.constData());
    if (!resp.ok) return result;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return result;
    const QVariantMap root = doc.object().toVariantMap();
    if (root.value(QStringLiteral("errcode")).toInt() != 0) return result;
    const QVariantMap data = root.value(QStringLiteral("data")).toMap();
    result.total = data.value(QStringLiteral("total")).toInt();
    const QVariantList list = data.value(QStringLiteral("info")).toList();

    auto decode = [](QString s) {
        s.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
        s.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
        s.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
        s.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
        s.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
        return s;
    };

    for (const auto &item : list) {
        const QVariantMap m = item.toMap();
        Song s;
        s.platform = Platform::Kugou;
        s.id = m.value(QStringLiteral("audio_id")).toString();
        s.name = decode(m.value(QStringLiteral("songname")).toString());
        QStringList artists;
        for (const auto &a : m.value(QStringLiteral("authors")).toList())
            artists.append(a.toMap().value(QStringLiteral("author_name")).toString());
        s.artist = Format::joinArtists(artists);
        s.album = decode(m.value(QStringLiteral("remark")).toString());
        s.albumId = m.value(QStringLiteral("album_id")).toString();
        s.duration = m.value(QStringLiteral("duration")).toDouble();
        // 专辑封面：album_sizable_cover 含 {size} 占位符，替换为 240
        QString cover = m.value(QStringLiteral("album_sizable_cover")).toString();
        cover.replace(QStringLiteral("{size}"), QStringLiteral("240"));
        s.cover = cover;

        LxSongMeta lx;
        lx.source = QStringLiteral("kg");
        lx.songmid = s.id;
        lx.albumId = s.albumId;
        lx.interval = Format::duration(s.duration);
        auto addType = [&lx](const QString &type, const QVariant &sizeVal,
                             const QVariant &hashVal) {
            const qint64 size = sizeVal.toLongLong();
            if (size <= 0) return;
            LxSongQualityMeta q;
            q.type = type;
            q.size = Format::fileSize(size);
            q.hash = hashVal.toString();
            lx.types.append(q);
        };
        addType(QStringLiteral("flac"), m.value(QStringLiteral("sqfilesize")),
                m.value(QStringLiteral("sqhash")));
        addType(QStringLiteral("320k"), m.value(QStringLiteral("320filesize")),
                m.value(QStringLiteral("320hash")));
        addType(QStringLiteral("128k"), m.value(QStringLiteral("filesize")),
                m.value(QStringLiteral("hash")));
        lx.hash = m.value(QStringLiteral("hash")).toString();
        s.lx = lx;
        s.hasLx = true;
        result.songs.append(s);
    }
    result.hasMore = (page * limit) < result.total;
    return result;
}

// ---------------------------------------------------------------------------
// 推荐歌单 + 歌单详情
//
// 参考工程原本用 everydayrec.service.kugou.com/guess_special_recommend 取推荐，
// 该接口现已失效（返回 {"status":0,"error_code":200101}）；改成 v9 的 getSpecial：
//   推荐列表  .../yueku/v9/special/getSpecial?is_ajax=1&cdn=cdn&t=5
//   歌单详情  .../yueku/v9/special/single/{id}-5-9999.html
// 详情是 HTML 页面，歌曲数组以 `global.data = [...]` 内嵌，字段齐全
// （songname/singername/audio_id/hash/filesize/union_cover），无需任何签名接口。
// ---------------------------------------------------------------------------

namespace {

/// 去 HTML 实体（酷狗把歌名/歌手里的 & < > " ' 都转义了，还夹着 <em> 高亮）
static QString kgDecode(QString s)
{
    s.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    s.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    s.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    s.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    s.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    s.replace(QStringLiteral("<em>"), QString());
    s.replace(QStringLiteral("</em>"), QString());
    return s.trimmed();
}

/// 图片 URL：{size} 占位符替换为 240
static QString kgCover(QString url)
{
    url.replace(QStringLiteral("{size}"), QStringLiteral("240"));
    return url.trimmed();
}

/// 播放量：接口返回 "429496.7万" / "3.2亿" / "123"
static qint64 kgParseCount(const QString &s)
{
    bool ok = false;
    double v = s.trimmed().toDouble(&ok);
    if (!ok) return 0;
    if (s.contains(QChar(0x4EBF)))      // 万
        v *= 10000.0;
    else if (s.contains(QChar(0x4EBB))) // 亿
        v *= 100000000.0;
    return static_cast<qint64>(v);
}

// ---- 极简 JSON 解析 ------------------------------------------------------
// `global.data = [...]` 不是独立 JSON 文档，QJsonDocument 解不了；而数组里
// 的字符串值本身就可能含分号，所以「正则匹配到第一个 ];」也会误切。
// 这里用「字符串感知的花括号配对」定位数组边界，再用手写递归下降解析器解析。

static bool kgReadQuoted(const QString &s, int &i, QString &out)
{
    if (i >= s.size() || s.at(i) != QLatin1Char('"')) return false;
    ++i;
    QString buf;
    while (i < s.size()) {
        const QChar c = s.at(i++);
        if (c == QLatin1Char('"')) { out = buf; return true; }
        if (c != QLatin1Char('\\')) { buf.append(c); continue; }
        if (i >= s.size()) return false;
        const QChar e = s.at(i++);
        switch (e.unicode()) {
        case '\"': buf.append(QLatin1Char('"')); break;
        case '\\': buf.append(QLatin1Char('\\')); break;
        case '/':  buf.append(QLatin1Char('/'));  break;
        case 'b':  buf.append(QChar(0x08)); break;
        case 'f':  buf.append(QChar(0x0C)); break;
        case 'n':  buf.append(QLatin1Char('\n')); break;
        case 'r':  buf.append(QLatin1Char('\r')); break;
        case 't':  buf.append(QLatin1Char('\t')); break;
        case 'u': {
            if (i + 4 > s.size()) return false;
            bool uok = false;
            const uint cp = s.mid(i, 4).toUInt(&uok, 16);
            if (!uok) return false;
            buf.append(QChar(cp));
            i += 4;
            break;
        }
        default:
            buf.append(e);
            break;
        }
    }
    return false;
}

static void kgSkipWs(const QString &s, int &i)
{
    while (i < s.size() && s.at(i).isSpace()) ++i;
}

static bool kgParseValue(const QString &s, int &i, QVariant &out)
{
    kgSkipWs(s, i);
    if (i >= s.size()) return false;
    const QChar c = s.at(i);

    if (c == QLatin1Char('"')) {
        QString v;
        if (!kgReadQuoted(s, i, v)) return false;
        out = v;                       // 必须回写给 out，否则整个字符串值丢失
        return true;
    }
    if (c == QLatin1Char('[')) {
        ++i;
        kgSkipWs(s, i);
        QVariantList arr;
        if (i < s.size() && s.at(i) == QLatin1Char(']')) { ++i; out = arr; return true; }
        for (;;) {
            QVariant v;
            if (!kgParseValue(s, i, v)) return false;
            arr.append(v);
            kgSkipWs(s, i);
            if (i >= s.size()) return false;
            if (s.at(i) == QLatin1Char(',')) { ++i; continue; }
            if (s.at(i) == QLatin1Char(']')) { ++i; out = arr; return true; }
            return false;
        }
    }
    if (c == QLatin1Char('{')) {
        ++i;
        kgSkipWs(s, i);
        QVariantMap obj;
        if (i < s.size() && s.at(i) == QLatin1Char('}')) { ++i; out = obj; return true; }
        for (;;) {
            QString key;
            if (!kgReadQuoted(s, i, key)) return false;
            kgSkipWs(s, i);
            if (i >= s.size() || s.at(i) != QLatin1Char(':')) return false;
            ++i;
            QVariant v;
            if (!kgParseValue(s, i, v)) return false;
            obj.insert(key, v);
            kgSkipWs(s, i);
            if (i >= s.size()) return false;
            if (s.at(i) == QLatin1Char(',')) { ++i; continue; }
            if (s.at(i) == QLatin1Char('}')) { ++i; out = obj; return true; }
            return false;
        }
    }
    if (c == QLatin1Char('t')) {
        if (s.mid(i, 4) == QStringLiteral("true")) { i += 4; out = true;  return true; }
        return false;
    }
    if (c == QLatin1Char('f')) {
        if (s.mid(i, 5) == QStringLiteral("false")) { i += 5; out = false; return true; }
        return false;
    }
    if (c == QLatin1Char('n')) {
        if (s.mid(i, 4) == QStringLiteral("null")) { i += 4; out = QVariant(); return true; }
        return false;
    }
    // 数字
    int start = i;
    if (c == QLatin1Char('-')) ++i;
    while (i < s.size()) {
        const QChar d = s.at(i);
        if (d.isDigit() || d == QLatin1Char('.') || d == QLatin1Char('e')
            || d == QLatin1Char('E') || d == QLatin1Char('+') || d == QLatin1Char('-'))
            ++i;
        else break;
    }
    if (i == start) return false;
    bool ok = false;
    out = s.mid(start, i - start).toDouble(&ok);
    return ok;
}

/// 从 HTML 里取出 `global.data = [...]` 的数组文本
static QString kgExtractGlobalData(const QString &html)
{
    const int tag = html.indexOf(QStringLiteral("global.data"));
    if (tag < 0) return QString();
    const int bracket = html.indexOf(QLatin1Char('['), tag);
    if (bracket < 0) return QString();
    const QString src = html.mid(bracket);

    int depth = 0;
    bool inStr = false;
    for (int i = 0; i < src.size(); ++i) {
        const QChar c = src.at(i);
        if (inStr) {
            if (c == QLatin1Char('\\')) { ++i; continue; }   // 跳过转义对
            if (c == QLatin1Char('"')) inStr = false;
            continue;                                         // 字符串内的 [ ] 不计
        }
        if (c == QLatin1Char('"')) { inStr = true; continue; }
        if (c == QLatin1Char('[')) { ++depth; continue; }
        if (c == QLatin1Char(']')) {
            --depth;
            if (depth == 0) return src.left(i + 1);
        }
    }
    return QString();
}

} // namespace

QVector<PlaylistSummary> KgSource::getRecommendPlaylists(int limit)
{
    QVector<PlaylistSummary> out;
    if (limit <= 0) limit = 6;

    // t: 5 推荐 / 6 最热 / 7 最新 / 3 热藏 / 8 飙升
    const QString url = QStringLiteral(
        "http://www2.kugou.kugou.com/yueku/v9/special/getSpecial?is_ajax=1&cdn=cdn&t=5&p=1");

    HttpOptions opt;
    opt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kKgUa);
    opt.headers[QStringLiteral("Referer")] = QStringLiteral("https://www.kugou.com/");
    opt.timeoutMs = 20000;

    const HttpResponse resp = HttpClient::instance()->get(url, opt);
    if (kDiag && qEnvironmentVariableIsSet("MUYUN_DEBUG_HOME"))
        printf("[KG-RCM] ok=%d body=%.300s\n", resp.ok, resp.body.constData());
    if (!resp.ok) return out;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return out;
    const QVariantMap root = doc.object().toVariantMap();
    if (root.value(QStringLiteral("status")).toInt() != 1) return out;

    const QVariantList list = root.value(QStringLiteral("special_db")).toList();
    QSet<QString> used;
    int added = 0;
    for (const auto &item : list) {
        if (added >= limit) break;
        const QVariantMap m = item.toMap();
        PlaylistSummary p;
        const QString sid = m.value(QStringLiteral("specialid")).toString().trimmed();
        if (sid.isEmpty() || used.contains(sid)) continue;
        p.id = sid;
        p.name = kgDecode(m.value(QStringLiteral("specialname")).toString());
        p.creator = kgDecode(m.value(QStringLiteral("nickname")).toString());
        p.cover = kgCover(m.value(QStringLiteral("img")).toString());
        p.trackCount = m.value(QStringLiteral("song_count")).toInt();
        p.playCount = kgParseCount(m.value(QStringLiteral("total_play_count")).toString());
        p.hasPlayCount = p.playCount > 0;
        p.platform = Platform::Kugou;
        if (p.name.isEmpty()) continue;
        used.insert(sid);
        out.append(p);
        ++added;
    }
    return out;
}

// ---------------------------------------------------------------------------
// 歌单广场：分类（getSpecial?is_smarty=1）+ 列表（t=5推荐/6最热/7最新，c=tagid）
// ---------------------------------------------------------------------------

namespace {
/// 分类信息：{status:1, data:{hotTag:{k:{special_id,special_name}}, tagids:{组:{data:[{id,name,pname}]}}}}
/// 该接口可能带 JSONP 外壳 → 截取首尾花括号再解析
QVariantMap fetchKgTagInfo()
{
    const QString url = QStringLiteral(
        "http://www2.kugou.kugou.com/yueku/v9/special/getSpecial?is_smarty=1&");
    HttpOptions opt;
    opt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kKgUa);
    opt.headers[QStringLiteral("Referer")] = QStringLiteral("https://www.kugou.com/");
    opt.timeoutMs = 20000;
    const HttpResponse resp = HttpClient::instance()->get(url, opt);
    if (!resp.ok) return {};
    const int a = resp.bodyText.indexOf(QLatin1Char('{'));
    const int b = resp.bodyText.lastIndexOf(QLatin1Char('}'));
    if (a < 0 || b <= a) return {};
    const QJsonDocument doc =
        QJsonDocument::fromJson(resp.bodyText.mid(a, b - a + 1).toUtf8());
    const QVariantMap root = doc.object().toVariantMap();
    if (root.value(QStringLiteral("status")).toInt() != 1) return {};
    return root.value(QStringLiteral("data")).toMap();
}
} // namespace

QVariantList KgSource::playlistCategories()
{
    QVariantList out;
    const QVariantMap data = fetchKgTagInfo();
    if (data.isEmpty()) return out;

    QStringList hot;
    const QVariantMap hotTag = data.value(QStringLiteral("hotTag")).toMap();
    for (auto it = hotTag.constBegin(); it != hotTag.constEnd(); ++it) {
        const QString n = kgDecode(it.value().toMap()
                                      .value(QStringLiteral("special_name")).toString());
        if (!n.isEmpty()) hot.append(n);
    }
    if (!hot.isEmpty()) {
        QVariantMap e;
        e[QStringLiteral("group")] = QStringLiteral("热门");
        e[QStringLiteral("tags")] = hot;
        out.append(e);
    }
    const QVariantMap tagids = data.value(QStringLiteral("tagids")).toMap();
    for (auto it = tagids.constBegin(); it != tagids.constEnd(); ++it) {
        QStringList tags;
        QString gname;
        const QVariantList list = it.value().toMap()
                                      .value(QStringLiteral("data")).toList();
        for (const auto &tv : list) {
            const QVariantMap tm = tv.toMap();
            const QString name = kgDecode(tm.value(QStringLiteral("name")).toString());
            if (!name.isEmpty() && !tags.contains(name)) tags.append(name);
            if (gname.isEmpty())
                gname = kgDecode(tm.value(QStringLiteral("pname")).toString());
        }
        if (tags.isEmpty()) continue;
        QVariantMap e;
        e[QStringLiteral("group")] = gname.isEmpty() ? it.key() : gname;
        e[QStringLiteral("tags")] = tags;
        out.append(e);
    }
    return out;
}

QVector<PlaylistSummary> KgSource::explorePlaylists(const QString &cat, const QString &order,
                                                    int page, int limit, bool *hasMore)
{
    QVector<PlaylistSummary> out;
    if (limit <= 0) limit = 30;
    if (page < 1) page = 1;
    if (hasMore) *hasMore = false;

    QString t = order == QStringLiteral("new") ? QStringLiteral("7") : QStringLiteral("6");
    QString c;
    if (cat.isEmpty() || cat == QStringLiteral("全部")) {
        t = QStringLiteral("5");   // 全部 → 推荐
    } else {
        const QVariantMap data = fetchKgTagInfo();
        const QVariantMap hotTag = data.value(QStringLiteral("hotTag")).toMap();
        for (auto it = hotTag.constBegin(); it != hotTag.constEnd(); ++it) {
            const QVariantMap hm = it.value().toMap();
            if (kgDecode(hm.value(QStringLiteral("special_name")).toString()) == cat) {
                c = QString::number(hm.value(QStringLiteral("special_id")).toLongLong());
                break;
            }
        }
        if (c.isEmpty()) {
            const QVariantMap tagids = data.value(QStringLiteral("tagids")).toMap();
            for (auto it = tagids.constBegin(); it != tagids.constEnd() && c.isEmpty(); ++it) {
                const QVariantList list = it.value().toMap()
                                              .value(QStringLiteral("data")).toList();
                for (const auto &tv : list) {
                    const QVariantMap tm = tv.toMap();
                    if (kgDecode(tm.value(QStringLiteral("name")).toString()) == cat) {
                        c = QString::number(tm.value(QStringLiteral("id")).toLongLong());
                        break;
                    }
                }
            }
        }
    }

    const QString url = QStringLiteral(
        "http://www2.kugou.kugou.com/yueku/v9/special/getSpecial?is_ajax=1&cdn=cdn"
        "&t=%1&c=%2&p=%3").arg(t, c, QString::number(page));
    HttpOptions opt;
    opt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kKgUa);
    opt.headers[QStringLiteral("Referer")] = QStringLiteral("https://www.kugou.com/");
    opt.timeoutMs = 20000;
    const HttpResponse resp = HttpClient::instance()->get(url, opt);
    if (kDiag && qEnvironmentVariableIsSet("MUYUN_DEBUG_HOME"))
        printf("[KG-EXPLORE] cat=%s ok=%d body=%.200s\n", qPrintable(cat), resp.ok,
               resp.body.constData());
    if (!resp.ok) return out;
    const QVariantMap root = QJsonDocument::fromJson(resp.body).object().toVariantMap();
    if (root.value(QStringLiteral("status")).toInt() != 1) return out;

    QSet<QString> used;
    for (const auto &item : root.value(QStringLiteral("special_db")).toList()) {
        const QVariantMap m = item.toMap();
        PlaylistSummary p;
        const QString sid = m.value(QStringLiteral("specialid")).toString().trimmed();
        if (sid.isEmpty() || used.contains(sid)) continue;
        p.id = sid;
        p.name = kgDecode(m.value(QStringLiteral("specialname")).toString());
        p.creator = kgDecode(m.value(QStringLiteral("nickname")).toString());
        p.cover = kgCover(m.value(QStringLiteral("img")).toString());
        p.trackCount = m.value(QStringLiteral("song_count")).toInt();
        p.playCount = kgParseCount(m.value(QStringLiteral("total_play_count")).toString());
        p.hasPlayCount = p.playCount > 0;
        p.platform = Platform::Kugou;
        if (p.name.isEmpty()) continue;
        used.insert(sid);
        out.append(p);
    }
    if (hasMore) *hasMore = out.size() >= limit;
    return out;
}

Playlist KgSource::getPlaylistDetail(const QString &id)
{
    Playlist pl;
    pl.id = id;
    pl.platform = Platform::Kugou;

    // 只接受纯数字 specialid（接口按路径拼接，防注入）
    QString pid = id;
    if (pid.startsWith(QStringLiteral("id_"))) pid.remove(0, 3);
    if (pid.isEmpty()) return pl;
    for (const QChar &c : pid) {
        if (!c.isDigit()) return pl;
    }

    const QString url = QStringLiteral(
        "http://www2.kugou.kugou.com/yueku/v9/special/single/%1-5-9999.html").arg(pid);

    HttpOptions opt;
    opt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kKgUa);
    opt.headers[QStringLiteral("Referer")] = QStringLiteral(
        "https://www.kugou.com/yy/special/single/%1.html").arg(pid);
    opt.timeoutMs = 20000;

    const HttpResponse resp = HttpClient::instance()->get(url, opt);
    if (kDiag && qEnvironmentVariableIsSet("MUYUN_DEBUG_HOME"))
        printf("[KG-PLDETAIL] pid=%s ok=%d bytes=%d\n", qPrintable(pid), resp.ok,
               static_cast<int>(resp.body.size()));
    if (!resp.ok) return pl;

    const QString json = kgExtractGlobalData(resp.bodyText);
    if (json.isEmpty()) return pl;

    QVariant arrVal;
    int i = 0;
    if (!kgParseValue(json, i, arrVal)) return pl;
    const QVariantList list = arrVal.toList();

    for (const auto &item : list) {
        const QVariantMap m = item.toMap();
        const QString audioId = m.value(QStringLiteral("audio_id")).toString();
        if (audioId.isEmpty()) continue;

        Song s;
        s.platform = Platform::Kugou;
        s.id = audioId;
        s.name = kgDecode(m.value(QStringLiteral("songname")).toString());
        if (s.name.isEmpty()) continue;
        s.artist = kgDecode(m.value(QStringLiteral("singername")).toString());
        s.album = kgDecode(m.value(QStringLiteral("album_name")).toString());
        s.albumId = m.value(QStringLiteral("album_id")).toString();
        s.duration = m.value(QStringLiteral("duration")).toDouble() / 1000.0;
        // 封面在 trans_param.union_cover，带 {size} 占位符
        s.cover = kgCover(m.value(QStringLiteral("trans_param")).toMap()
                             .value(QStringLiteral("union_cover")).toString());

        LxSongMeta lx;
        lx.source = QStringLiteral("kg");
        lx.songmid = audioId;
        lx.albumId = s.albumId;
        lx.interval = Format::duration(s.duration);
        lx.img = s.cover;

        auto addType = [&lx](const QString &type, const QVariant &sizeVal,
                             const QVariant &hashVal) {
            const qint64 size = sizeVal.toLongLong();
            if (size <= 0) return;
            LxSongQualityMeta q;
            q.type = type;
            q.size = Format::fileSize(size);
            q.hash = hashVal.toString();
            lx.types.append(q);
        };
        // v9 字段无 _sq/_high，flac 对应 _flac，24bit 对应 ape
        addType(QStringLiteral("flac24bit"), m.value(QStringLiteral("filesize_ape")),
                m.value(QStringLiteral("hash_ape")));
        addType(QStringLiteral("flac"), m.value(QStringLiteral("filesize_flac")),
                m.value(QStringLiteral("hash_flac")));
        addType(QStringLiteral("320k"), m.value(QStringLiteral("filesize_320")),
                m.value(QStringLiteral("hash_320")));
        addType(QStringLiteral("128k"), m.value(QStringLiteral("filesize")),
                m.value(QStringLiteral("hash")));
        std::reverse(lx.types.begin(), lx.types.end());

        // 默认 hash 用于兜底取链接
        lx.hash = m.value(QStringLiteral("hash")).toString();

        if (!lx.types.isEmpty()) {
            bool ok = false;
            s.quality = qualityFromId(lx.types.first().type, &ok);
            s.hasQuality = ok;
        }
        s.lx = lx;
        s.hasLx = true;
        pl.songs.append(s);
    }
    return pl;
}

} // namespace Muyun
