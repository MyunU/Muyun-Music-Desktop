#include "KwSource.h"

#include "core/utils/Format.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QTextDocumentFragment>
#include <QUrl>
#include <algorithm>

namespace Muyun {

namespace {
const char *kSearchUa =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";

/// 把"单引号 JS 字面量"里的**结构性单引号**换成双引号（酷我老接口格式）。
/// 旧实现用一条带**变长后顾断言**的正则（`(?<=([:,]\s*))` 等）做这件事——
/// PCRE2 不支持可变长后顾 → QRegularExpression 构造失败 → replace() 静默不执行，
/// 只留一行 "invalid QRegularExpression" 警告刷日志（四-59 修）。
/// 现在手写扫描替代正则：只在"结构位置"（前一个非空白字符是 { , : [）开串，
/// 串内遇到 `'` 即收串；`\` 转义与双引号串原样保留，避免把正文里的撇号当定界符。
QString kwNormalizeJsLiteral(const QString &src)
{
    QString out;
    out.reserve(src.size());
    bool inSingle = false, inDouble = false;
    for (int i = 0; i < src.size(); ++i) {
        const QChar c = src.at(i);
        if (c == QLatin1Char('\\') && i + 1 < src.size()) {   // 转义序列整段带走
            out.append(c);
            out.append(src.at(++i));
            continue;
        }
        if (inSingle) {
            if (c == QLatin1Char('\'')) { out.append(QLatin1Char('"')); inSingle = false; }
            else out.append(c);
            continue;
        }
        if (inDouble) {
            if (c == QLatin1Char('"')) inDouble = false;
            out.append(c);
            continue;
        }
        if (c == QLatin1Char('"')) { inDouble = true; out.append(c); continue; }
        if (c == QLatin1Char('\'')) {
            QChar prev;
            for (int k = out.size() - 1; k >= 0; --k)
                if (!out.at(k).isSpace()) { prev = out.at(k); break; }
            const bool structural = prev.isNull() || prev == QLatin1Char('{')
                                    || prev == QLatin1Char(',') || prev == QLatin1Char(':')
                                    || prev == QLatin1Char('[');
            if (structural) { out.append(QLatin1Char('"')); inSingle = true; }
            else out.append(c);                               // 不像定界符 → 原样留（正文撇号）
            continue;
        }
        out.append(c);
    }
    return out;
}

/// 解析酷我响应根对象：**先按标准 JSON 解**（现行接口就是标准 JSON，实测确认）；
/// 失败才按单引号字面量规范化后重试（兼容老接口）。解析不出来返回空 map。
QVariantMap kwParseRoot(const QString &body)
{
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(body.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        doc = QJsonDocument::fromJson(kwNormalizeJsLiteral(body).toUtf8(), &err);
    }
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return QVariantMap();
    return doc.object().toVariantMap();
}
} // namespace

QString KwSource::decodeHtml(const QString &text)
{
    if (text.isEmpty()) return text;
    if (!text.contains(QLatin1Char('&'))) return text;
    // 用富文本解析器做 HTML 实体解码
    return QTextDocumentFragment::fromHtml(text).toPlainText();
}

QString KwSource::normalizeCover(const QString &path)
{
    if (path.isEmpty()) return QString();
    if (path.startsWith(QStringLiteral("http"), Qt::CaseInsensitive)) return path;
    QString p = path;
    while (p.startsWith(QLatin1Char('/'))) p.remove(0, 1);
    return QStringLiteral("https://img1.kuwo.cn/star/albumcover/") + p;
}

SearchResult KwSource::searchSongs(const QString &keyword, int page, int limit)
{
    SearchResult result;
    if (limit <= 0) limit = 30;

    const QString url =
        QStringLiteral("http://search.kuwo.cn/r.s?client=kt&all=%1&pn=%2&rn=%3"
                       "&uid=794762570&ver=kwplayer_ar_9.2.2.1&vipver=1"
                       "&show_copyright_off=1&newver=1&ft=music&cluster=0"
                       "&strategy=2012&encoding=utf8&rformat=json&vermerge=1"
                       "&mobi=1&issubtitle=1")
            .arg(QString::fromUtf8(QUrl::toPercentEncoding(keyword)))
            .arg(page - 1)
            .arg(limit);

    HttpOptions opt;
    opt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kSearchUa);
    opt.headers[QStringLiteral("Referer")] = QStringLiteral("http://www.kuwo.cn/");
    opt.timeoutMs = 20000;

    const HttpResponse resp = HttpClient::instance()->get(url, opt);
    if (!resp.ok) return result;

    // 酷我返回的按标准 JSON 解，失败才走单引号字面量兜底（四-59：删掉那条 PCRE2 不支持的
    // 变长后顾正则——它构造失败后 replace 静默不执行，只往日志刷警告）
    const QVariantMap root = kwParseRoot(resp.bodyText);
    if (root.isEmpty()) return result;

    result.total = root.value(QStringLiteral("TOTAL")).toString().toInt();
    const QVariantList list = root.value(QStringLiteral("abslist")).toList();

    // 音质信息：level:xxx,bitrate:4000,format:flac,size:30.5MB
    static const QRegularExpression metaRx(
        QStringLiteral("level:(\\w+),bitrate:(\\d+),format:(\\w+),size:([\\w.]+)"));

    for (const auto &item : list) {
        const QVariantMap m = item.toMap();
        const QString rid = m.value(QStringLiteral("MUSICRID")).toString()
                                .remove(QStringLiteral("MUSIC_"));
        if (rid.isEmpty()) continue;

        Song s;
        s.id = rid;
        s.platform = Platform::Kuwo;
        s.name = decodeHtml(m.value(QStringLiteral("SONGNAME")).toString());
        // 酷我歌手用 & 分隔
        s.artist = decodeHtml(m.value(QStringLiteral("ARTIST")).toString())
                       .replace(QLatin1Char('&'), QStringLiteral("、"));
        s.album = decodeHtml(m.value(QStringLiteral("ALBUM")).toString());
        s.albumId = decodeHtml(m.value(QStringLiteral("ALBUMID")).toString());
        s.duration = m.value(QStringLiteral("DURATION")).toString().toDouble();

        const QString pic = m.value(QStringLiteral("web_albumpic_short")).toString();
        s.cover = normalizeCover(pic);

        LxSongMeta lx;
        lx.source = QStringLiteral("kw");
        lx.songmid = rid;
        lx.albumId = s.albumId;
        lx.interval = Format::duration(s.duration);
        lx.img = s.cover;

        // 解析 N_MINFO 中的可用音质
        const QString minfo = m.value(QStringLiteral("N_MINFO")).toString();
        if (!minfo.isEmpty()) {
            const QStringList parts = minfo.split(QLatin1Char(';'));
            for (const auto &part : parts) {
                const auto match = metaRx.match(part);
                if (!match.hasMatch()) continue;
                const QString bitrate = match.captured(2);
                LxSongQualityMeta q;
                q.size = match.captured(4);
                if (bitrate == QStringLiteral("4000")) q.type = QStringLiteral("flac24bit");
                else if (bitrate == QStringLiteral("2000")) q.type = QStringLiteral("flac");
                else if (bitrate == QStringLiteral("320")) q.type = QStringLiteral("320k");
                else if (bitrate == QStringLiteral("128")) q.type = QStringLiteral("128k");
                else continue;
                lx.types.append(q);
            }
            std::reverse(lx.types.begin(), lx.types.end());
        }

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

QString KwSource::getMusicUrl(const Song &song, AudioQuality quality)
{
    const QString rid = song.lx.songmid.isEmpty() ? song.id : song.lx.songmid;
    if (rid.isEmpty()) return QString();

    const bool lossless = (quality == AudioQuality::Flac ||
                           quality == AudioQuality::Flac24Bit ||
                           quality == AudioQuality::HiRes);
    const QString format = lossless ? QStringLiteral("flac") : QStringLiteral("mp3");

    const QString url =
        QStringLiteral("http://antiserver.kuwo.cn/anti.s?type=convert_url&rid=MUSIC_%1"
                       "&format=%2&response=url").arg(rid, format);

    HttpOptions opt;
    opt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kSearchUa);
    opt.headers[QStringLiteral("Referer")] = QStringLiteral("http://www.kuwo.cn/");
    opt.timeoutMs = 20000;

    const HttpResponse resp = HttpClient::instance()->get(url, opt);
    if (!resp.ok) return QString();

    const QString text = resp.bodyText.trimmed();
    // 正常返回形如 http://... 的裸链接
    if (text.startsWith(QStringLiteral("http"))) return text;

    // 部分情况返回 JSON
    if (text.startsWith(QLatin1Char('{'))) {
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
        if (err.error == QJsonParseError::NoError) {
            const QVariantMap m = doc.object().toVariantMap();
            const QString u = m.value(QStringLiteral("url")).toString();
            if (!u.isEmpty()) return u;
        }
    }
    return QString();
}

SongLyric KwSource::getLyric(const Song &song)
{
    SongLyric lyric;
    const QString rid = song.lx.songmid.isEmpty() ? song.id : song.lx.songmid;
    if (rid.isEmpty()) return lyric;

    const QString url =
        QStringLiteral("http://m.kuwo.cn/newh5/singles/songinfoandlrc?musicId=%1").arg(rid);

    HttpOptions opt;
    opt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kSearchUa);
    opt.timeoutMs = 20000;

    const HttpResponse resp = HttpClient::instance()->get(url, opt);
    if (!resp.ok) return lyric;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return lyric;

    const QVariantList lrcList = doc.object().toVariantMap()
                                     .value(QStringLiteral("data")).toMap()
                                     .value(QStringLiteral("lrclist")).toList();

    QStringList lines;
    for (const auto &item : lrcList) {
        const QVariantMap m = item.toMap();
        const double t = m.value(QStringLiteral("time")).toString().toDouble();
        const QString text = m.value(QStringLiteral("lineLyric")).toString();
        if (text.trimmed().isEmpty()) continue;
        const int minutes = static_cast<int>(t) / 60;
        const double seconds = t - minutes * 60;
        lines.append(QStringLiteral("[%1:%2]%3")
                         .arg(minutes, 2, 10, QLatin1Char('0'))
                         .arg(seconds, 5, 'f', 2, QLatin1Char('0'))
                         .arg(text));
    }
    lyric.rawLrc = lines.join(QLatin1Char('\n'));
    return lyric;
}

// ---------------------------------------------------------------------------
// 榜单（参考工程硬编码榜单表 + kbangserver 明文接口）
// ---------------------------------------------------------------------------

QVector<ToplistInfo> KwSource::getToplists()
{
    struct Board { const char *id; const char *name; };
    static const Board boards[] = {
        {"93", "飙升榜"}, {"17", "新歌榜"}, {"16", "热歌榜"},
        {"158", "抖音热歌榜"}, {"284", "热评榜"}, {"290", "ACG新歌榜"},
        {"255", "KTV点唱榜"}, {"278", "古风音乐榜"}, {"242", "电音榜"},
        {"187", "流行趋势榜"}, {"26", "经典怀旧榜"}, {"104", "华语榜"},
        {"182", "粤语榜"}, {"22", "欧美榜"}, {"184", "韩语榜"}, {"183", "日语榜"},
    };
    QVector<ToplistInfo> out;
    for (const auto &b : boards) {
        ToplistInfo t;
        t.id = QString::fromLatin1(b.id);
        t.name = QString::fromUtf8(b.name);
        t.platform = Platform::Kuwo;
        out.append(t);
    }
    return out;
}

SearchResult KwSource::getToplist(const QString &id, int page, int limit)
{
    SearchResult result;
    if (limit <= 0) limit = 100;

    const QString url =
        QStringLiteral("http://kbangserver.kuwo.cn/ksong.s?from=pc&fmt=json"
                       "&pn=%1&rn=%2&type=bang&data=content&id=%3"
                       "&show_copyright_off=0&pcmp4=1&isbang=1")
            .arg(page - 1).arg(limit).arg(id);

    HttpOptions opt;
    opt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kSearchUa);
    opt.headers[QStringLiteral("Referer")] = QStringLiteral("http://www.kuwo.cn/");
    opt.timeoutMs = 20000;

    const HttpResponse resp = HttpClient::instance()->get(url, opt);
    if (kDiag && qEnvironmentVariableIsSet("MUYUN_DEBUG_HOME"))
        printf("[KW-TOPLIST] id=%s ok=%d body=%.300s\n", qPrintable(id), resp.ok,
               resp.body.constData());
    if (!resp.ok) return result;

    // 同上：标准 JSON 优先，老格式由 kwParseRoot 兜底（四-59）
    const QVariantMap root = kwParseRoot(resp.bodyText);
    if (root.isEmpty()) return result;
    result.total = root.value(QStringLiteral("total")).toString().toInt();
    const QVariantList list = root.value(QStringLiteral("musiclist")).toList();

    static const QRegularExpression metaRx(
        QStringLiteral("level:(\\w+),bitrate:(\\d+),format:(\\w+),size:([\\w.]+)"));

    for (const auto &item : list) {
        const QVariantMap m = item.toMap();
        const QString rid = m.value(QStringLiteral("id")).toString();
        if (rid.isEmpty()) continue;

        Song s;
        s.id = rid;
        s.platform = Platform::Kuwo;
        s.name = decodeHtml(m.value(QStringLiteral("name")).toString());
        s.artist = decodeHtml(m.value(QStringLiteral("artist")).toString())
                       .replace(QLatin1Char('&'), QStringLiteral("、"));
        s.album = decodeHtml(m.value(QStringLiteral("album")).toString());
        s.albumId = m.value(QStringLiteral("albumid")).toString();
        s.duration = m.value(QStringLiteral("duration")).toString().toDouble();
        s.cover = normalizeCover(m.value(QStringLiteral("pic")).toString());

        LxSongMeta lx;
        lx.source = QStringLiteral("kw");
        lx.songmid = rid;
        lx.albumId = s.albumId;
        lx.interval = Format::duration(s.duration);
        lx.img = s.cover;

        const QString minfo = m.value(QStringLiteral("n_minfo")).toString();
        if (!minfo.isEmpty()) {
            for (const auto &part : minfo.split(QLatin1Char(';'))) {
                const auto match = metaRx.match(part);
                if (!match.hasMatch()) continue;
                const QString bitrate = match.captured(2);
                LxSongQualityMeta q;
                q.size = match.captured(4);
                if (bitrate == QStringLiteral("4000")) q.type = QStringLiteral("flac24bit");
                else if (bitrate == QStringLiteral("2000")) q.type = QStringLiteral("flac");
                else if (bitrate == QStringLiteral("320")) q.type = QStringLiteral("320k");
                else if (bitrate == QStringLiteral("128")) q.type = QStringLiteral("128k");
                else continue;
                lx.types.append(q);
            }
            std::reverse(lx.types.begin(), lx.types.end());
        }

        s.lx = lx;
        s.hasLx = true;
        result.songs.append(s);
    }
    if (result.total <= 0) result.total = result.songs.size();
    result.hasMore = (page * limit) < result.total;
    return result;
}

// ---------------------------------------------------------------------------
// 推荐歌单 / 歌单详情（wapi 推荐 + nplserver 详情，均为明文接口）
// ---------------------------------------------------------------------------

QByteArray KwSource::fixJson(const QString &text)
{
    // 四-59：原来是"带变长后顾断言的正则 replace"，PCRE2 直接构造失败（静默无效 + 刷警告）。
    // 现在手写规范化，语义不变（结构性单引号 → 双引号）。
    return kwNormalizeJsLiteral(text).toUtf8();
}

QVector<PlaylistSummary> KwSource::getRecommendPlaylists(int limit)
{
    QVector<PlaylistSummary> out;
    if (limit <= 0) limit = 6;

    const QString url = QStringLiteral(
        "http://wapi.kuwo.cn/api/pc/classify/playlist/getRcmPlayList"
        "?loginUid=0&loginSid=0&appUid=76039576&pn=1&rn=%1&order=hot").arg(limit);

    HttpOptions opt;
    opt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kSearchUa);
    opt.headers[QStringLiteral("Referer")] = QStringLiteral("http://www.kuwo.cn/");
    opt.timeoutMs = 20000;

    const HttpResponse resp = HttpClient::instance()->get(url, opt);
    if (kDiag && qEnvironmentVariableIsSet("MUYUN_DEBUG_HOME"))
        printf("[KW-RCM] ok=%d body=%.300s\n", resp.ok, resp.body.constData());
    if (!resp.ok) return out;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return out;
    const QVariantMap root = doc.object().toVariantMap();
    // 结构：{code:200, data:{data:[...]}}
    QVariantList list = root.value(QStringLiteral("data")).toMap()
                           .value(QStringLiteral("data")).toList();
    if (list.isEmpty()) list = root.value(QStringLiteral("data")).toList();

    for (const auto &item : list) {
        const QVariantMap m = item.toMap();
        PlaylistSummary p;
        const QString digest = m.value(QStringLiteral("digest")).toString();
        const QString rawId = m.value(QStringLiteral("id")).toString();
        p.id = digest.isEmpty() ? rawId
                                : QStringLiteral("digest-%1__%2").arg(digest, rawId);
        p.name = decodeHtml(m.value(QStringLiteral("name")).toString());
        p.cover = normalizeCover(m.value(QStringLiteral("img")).toString());
        p.creator = decodeHtml(m.value(QStringLiteral("uname")).toString());
        p.trackCount = m.value(QStringLiteral("total")).toInt();
        p.playCount = static_cast<qint64>(m.value(QStringLiteral("listencnt")).toDouble());
        p.hasPlayCount = p.playCount > 0;
        p.platform = Platform::Kuwo;
        if (!p.id.isEmpty() && !p.name.isEmpty()) out.append(p);
    }
    return out;
}

Playlist KwSource::getPlaylistDetail(const QString &id)
{
    Playlist pl;
    pl.id = id;
    pl.platform = Platform::Kuwo;

    // 解析推荐列表给的 id：可能是 "digest-8__123" 或纯数字
    QString pid = id;
    if (pid.startsWith(QStringLiteral("digest-"))) {
        const int sep = pid.indexOf(QStringLiteral("__"));
        if (sep >= 0) pid = pid.mid(sep + 2);
    }
    if (pid.isEmpty()) return pl;

    const QString url = QStringLiteral(
        "http://nplserver.kuwo.cn/pl.svc?op=getlistinfo&pid=%1&pn=0&rn=200"
        "&encode=utf8&keyset=pl2012&identity=kuwo&pcmp4=1&vipver=MUSIC_9.0.5.0_W1&newver=1").arg(pid);

    HttpOptions opt;
    opt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kSearchUa);
    opt.headers[QStringLiteral("Referer")] = QStringLiteral("http://www.kuwo.cn/");
    opt.timeoutMs = 20000;

    const HttpResponse resp = HttpClient::instance()->get(url, opt);
    if (kDiag && qEnvironmentVariableIsSet("MUYUN_DEBUG_HOME"))
        printf("[KW-PLDETAIL] pid=%s ok=%d body=%.200s\n", qPrintable(pid), resp.ok,
               resp.body.constData());
    if (!resp.ok) return pl;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(fixJson(resp.bodyText), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return pl;
    const QVariantMap root = doc.object().toVariantMap();
    if (root.value(QStringLiteral("result")).toString() != QStringLiteral("ok")) return pl;

    pl.name = decodeHtml(root.value(QStringLiteral("title")).toString());
    pl.description = decodeHtml(root.value(QStringLiteral("info")).toString());
    pl.cover = normalizeCover(root.value(QStringLiteral("pic")).toString());
    pl.creator = decodeHtml(root.value(QStringLiteral("uname")).toString());

    static const QRegularExpression metaRx(
        QStringLiteral("level:(\\w+),bitrate:(\\d+),format:(\\w+),size:([\\w.]+)"));
    for (const auto &item : root.value(QStringLiteral("musiclist")).toList()) {
        const QVariantMap m = item.toMap();
        const QString rid = m.value(QStringLiteral("id")).toString();
        if (rid.isEmpty()) continue;

        Song s;
        s.id = rid;
        s.platform = Platform::Kuwo;
        s.name = decodeHtml(m.value(QStringLiteral("name")).toString());
        s.artist = decodeHtml(m.value(QStringLiteral("artist")).toString())
                       .replace(QLatin1Char('&'), QStringLiteral("、"));
        s.album = decodeHtml(m.value(QStringLiteral("album")).toString());
        s.albumId = m.value(QStringLiteral("albumid")).toString();
        s.duration = m.value(QStringLiteral("duration")).toString().toDouble();
        s.cover = normalizeCover(m.value(QStringLiteral("pic")).toString());

        LxSongMeta lx;
        lx.source = QStringLiteral("kw");
        lx.songmid = rid;
        lx.albumId = s.albumId;
        lx.interval = Format::duration(s.duration);
        lx.img = s.cover;
        const QString minfo = m.value(QStringLiteral("N_MINFO")).toString();
        if (!minfo.isEmpty()) {
            for (const auto &part : minfo.split(QLatin1Char(';'))) {
                const auto match = metaRx.match(part);
                if (!match.hasMatch()) continue;
                const QString bitrate = match.captured(2);
                LxSongQualityMeta q;
                q.size = match.captured(4);
                if (bitrate == QStringLiteral("4000")) q.type = QStringLiteral("flac24bit");
                else if (bitrate == QStringLiteral("2000")) q.type = QStringLiteral("flac");
                else if (bitrate == QStringLiteral("320")) q.type = QStringLiteral("320k");
                else if (bitrate == QStringLiteral("128")) q.type = QStringLiteral("128k");
                else continue;
                lx.types.append(q);
            }
            std::reverse(lx.types.begin(), lx.types.end());
        }
        s.lx = lx;
        s.hasLx = true;
        pl.songs.append(s);
    }
    return pl;
}

// ---------------------------------------------------------------------------
// 歌单广场：分类 + 分类下歌单（wapi 明文接口，同推荐歌单族）
// ---------------------------------------------------------------------------

QVector<PlaylistSummary> KwSource::parseTagList(const QVariantList &list)
{
    QVector<PlaylistSummary> out;
    for (const auto &item : list) {
        const QVariantMap m = item.toMap();
        PlaylistSummary p;
        const QString digest = m.value(QStringLiteral("digest")).toString();
        const QString rawId = m.value(QStringLiteral("id")).toString();
        p.id = digest.isEmpty() ? rawId
                                : QStringLiteral("digest-%1__%2").arg(digest, rawId);
        p.name = decodeHtml(m.value(QStringLiteral("name")).toString());
        p.cover = normalizeCover(m.value(QStringLiteral("img")).toString());
        p.creator = decodeHtml(m.value(QStringLiteral("uname")).toString());
        p.trackCount = m.value(QStringLiteral("total")).toInt();
        p.playCount = static_cast<qint64>(m.value(QStringLiteral("listencnt")).toDouble());
        p.hasPlayCount = p.playCount > 0;
        p.platform = Platform::Kuwo;
        if (!p.id.isEmpty() && !p.name.isEmpty()) out.append(p);
    }
    return out;
}

QVariantList KwSource::playlistCategories()
{
    QVariantList out;
    const QString url = QStringLiteral(
        "http://wapi.kuwo.cn/api/pc/classify/playlist/getTagList"
        "?cmd=rcm_keyword_playlist&user=0&prod=kwplayer_pc_9.0.5.0&vipver=9.0.5.0"
        "&source=kwplayer_pc_9.0.5.0&loginUid=0&loginSid=0&appUid=76039576");
    HttpOptions opt;
    opt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kSearchUa);
    opt.headers[QStringLiteral("Referer")] = QStringLiteral("http://www.kuwo.cn/");
    opt.timeoutMs = 20000;
    const HttpResponse resp = HttpClient::instance()->get(url, opt);
    if (!resp.ok) return out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return out;
    const QVariantMap root = doc.object().toVariantMap();
    if (root.value(QStringLiteral("code")).toInt() != 200) return out;
    // data:[{id,name,data:[{id,name,digest}]}]；digest 即列表接口分派类型（10000=标签歌单）
    for (const auto &g : root.value(QStringLiteral("data")).toList()) {
        const QVariantMap gm = g.toMap();
        QStringList tags;
        for (const auto &t : gm.value(QStringLiteral("data")).toList())
            tags.append(t.toMap().value(QStringLiteral("name")).toString());
        if (tags.isEmpty()) continue;
        QVariantMap entry;
        entry[QStringLiteral("group")] = gm.value(QStringLiteral("name")).toString();
        entry[QStringLiteral("tags")] = tags;
        out.append(entry);
    }
    return out;
}

QVector<PlaylistSummary> KwSource::explorePlaylists(const QString &cat, const QString &order,
                                                    int page, int limit, bool *hasMore)
{
    QVector<PlaylistSummary> out;
    if (limit <= 0) limit = 30;
    if (page < 1) page = 1;
    if (hasMore) *hasMore = false;

    const QString sort = order == QStringLiteral("new") ? QStringLiteral("new")
                                                        : QStringLiteral("hot");
    QString url;
    bool tagMode = false;

    // 指定标签：先查 getTagList 解析 "tagId-digest"（无共享状态，线程安全）
    if (!cat.isEmpty() && cat != QStringLiteral("全部")) {
        const QString tagUrl = QStringLiteral(
            "http://wapi.kuwo.cn/api/pc/classify/playlist/getTagList"
            "?cmd=rcm_keyword_playlist&user=0&prod=kwplayer_pc_9.0.5.0&vipver=9.0.5.0"
            "&source=kwplayer_pc_9.0.5.0&loginUid=0&loginSid=0&appUid=76039576");
        HttpOptions topt;
        topt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kSearchUa);
        topt.headers[QStringLiteral("Referer")] = QStringLiteral("http://www.kuwo.cn/");
        topt.timeoutMs = 20000;
        const HttpResponse tr = HttpClient::instance()->get(tagUrl, topt);
        if (tr.ok) {
            const QJsonDocument td = QJsonDocument::fromJson(tr.body);
            for (const auto &g : td.object().toVariantMap().value(QStringLiteral("data")).toList()) {
                for (const auto &t : g.toMap().value(QStringLiteral("data")).toList()) {
                    const QVariantMap tm = t.toMap();
                    if (tm.value(QStringLiteral("name")).toString() != cat) continue;
                    const QString digest = tm.value(QStringLiteral("digest")).toString();
                    const QString tid = tm.value(QStringLiteral("id")).toString();
                    if (digest == QStringLiteral("10000") && !tid.isEmpty()) {
                        url = QStringLiteral(
                                  "http://wapi.kuwo.cn/api/pc/classify/playlist/getTagPlayList"
                                  "?loginUid=0&loginSid=0&appUid=76039576&pn=%1&id=%2&rn=%3")
                                  .arg(page).arg(tid).arg(limit);
                        tagMode = true;
                    }
                    break;   // digest=43 等其它类型走 er.s 字符串化字典，暂不支持→回退推荐
                }
                if (tagMode) break;
            }
        }
    }
    if (url.isEmpty()) {
        url = QStringLiteral(
                  "http://wapi.kuwo.cn/api/pc/classify/playlist/getRcmPlayList"
                  "?loginUid=0&loginSid=0&appUid=76039576&pn=%1&rn=%2&order=%3")
                  .arg(page).arg(limit).arg(sort);
    }

    HttpOptions opt;
    opt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kSearchUa);
    opt.headers[QStringLiteral("Referer")] = QStringLiteral("http://www.kuwo.cn/");
    opt.timeoutMs = 20000;
    const HttpResponse resp = HttpClient::instance()->get(url, opt);
    if (kDiag && qEnvironmentVariableIsSet("MUYUN_DEBUG_HOME"))
        printf("[KW-EXPLORE] cat=%s ok=%d body=%.200s\n", qPrintable(cat), resp.ok,
               resp.body.constData());
    if (!resp.ok) return out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return out;
    const QVariantMap root = doc.object().toVariantMap();
    if (root.value(QStringLiteral("code")).toInt() != 200) return out;
    const QVariantMap data = root.value(QStringLiteral("data")).toMap();
    out = parseTagList(data.value(QStringLiteral("data")).toList());
    const int total = data.value(QStringLiteral("total")).toInt();
    if (hasMore && total > 0) *hasMore = page * limit < total;
    return out;
}

} // namespace Muyun
