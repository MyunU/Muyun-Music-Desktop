#include "WySource.h"

#include "core/utils/Crypto.h"
#include "core/utils/Format.h"
#include "core/storage/DocumentStore.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <algorithm>
#include <QUrlQuery>
#include <QDebug>
#include <QHash>
#include <QSet>
#include <QThread>
#include <QRandomGenerator>
#include <QRegularExpression>

namespace Muyun {

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------
namespace {
const char *kEapiBase = "https://interface.music.163.com/eapi/";
const char *kEapiKey = "e82ckenh8dichen8";
const char *kWeapiPresetKey = "0CoJUm6Qyw8W8jud";
const char *kWeapiIv = "0102030405060708";
const char *kWeapiPublicKey =
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDgtQn2JZ34ZC28NWYpAUd98iZ37BUrX/"
    "aKzmFbt7clFSs6sXqHauqKWqdtLkF2KexO40H1YTX8z2lSgBBOAxLsvaklV8k4cBFK9snQ"
    "XE9/DDaFt6Rr7iVZMldczhC0JNgTz+SHXT6CBHuX3e9SdB1Ua44oncaTWz7OBGLbCiK45w"
    "IDAQAB";
const char *kDefaultUA =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/60.0.3112.90 Safari/537.36";

/// 音质 -> 网易云 bitrate
int qualityToBr(AudioQuality q)
{
    switch (q) {
    case AudioQuality::K128:      return 128000;
    case AudioQuality::K320:      return 320000;
    case AudioQuality::Flac:      return 999000;
    case AudioQuality::Flac24Bit: return 999000;
    case AudioQuality::HiRes:     return 999000;
    case AudioQuality::Atmos:     return 999000;
    case AudioQuality::Master:    return 999000;
    }
    return 320000;
}
} // namespace

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------

WySource::WySource()
{
    m_deviceId = Crypto::randomHex(16);
    m_csrf = QString();
}

QString WySource::anonymousToken()
{
    auto *store = DocumentStore::instance();
    QString token = store->readSync(QStringLiteral("misc"),
                                    QStringLiteral("netease_anonymous_token")).toString();
    if (token.isEmpty()) {
        token = Crypto::randomHex(32);
        store->write(QStringLiteral("misc"), QStringLiteral("netease_anonymous_token"), token);
    }
    return token;
}

void WySource::setCookie(const QString &cookie)
{
    QMutexLocker lock(&m_cookieMtx);
    m_cookie = cookie;
    // 提取 __csrf
    const auto parts = cookie.split(QLatin1Char(';'));
    for (const auto &p : parts) {
        const int eq = p.indexOf(QLatin1Char('='));
        if (eq < 0) continue;
        const QString k = p.left(eq).trimmed();
        const QString v = p.mid(eq + 1).trimmed();
        if (k == QStringLiteral("__csrf")) m_csrf = v;
    }
}

QString WySource::cookie() const { return m_cookie; }

// ---------------------------------------------------------------------------
// eapi
// ---------------------------------------------------------------------------

QString WySource::eapiParams(const QString &path, const QVariantMap &data)
{
    const QByteArray text =
        QJsonDocument(QJsonObject::fromVariantMap(data)).toJson(QJsonDocument::Compact);
    const QString message = QStringLiteral("nobody%1use%2md5forencrypt")
                                .arg(path, QString::fromUtf8(text));
    const QString digest = Crypto::md5Hex(message.toUtf8());
    const QString payload = QStringLiteral("%1-36cd479b6b5-%2-36cd479b6b5-%3")
                                .arg(path, QString::fromUtf8(text), digest);
    const QByteArray enc = Crypto::aes128EcbEncrypt(payload.toUtf8(), kEapiKey);
    return QString::fromUtf8(enc.toHex()).toUpper();
}

QVariantMap WySource::buildEapiHeader() const
{
    QVariantMap header;
    header[QStringLiteral("osver")] =
        QStringLiteral("Microsoft-Windows-10-Professional-build-19045-64bit");
    header[QStringLiteral("deviceId")] = m_deviceId;
    header[QStringLiteral("os")] = QStringLiteral("pc");
    header[QStringLiteral("appver")] = QStringLiteral("3.1.17.204416");
    header[QStringLiteral("versioncode")] = QStringLiteral("140");
    header[QStringLiteral("mobilename")] = QString();
    header[QStringLiteral("buildver")] =
        QString::number(QDateTime::currentMSecsSinceEpoch() / 1000);
    header[QStringLiteral("resolution")] = QStringLiteral("1920x1080");
    header[QStringLiteral("__csrf")] = m_csrf;
    header[QStringLiteral("channel")] = QStringLiteral("netease");
    header[QStringLiteral("requestId")] =
        QStringLiteral("%1_%2")
            .arg(QDateTime::currentMSecsSinceEpoch())
            .arg(Crypto::randomHex(2));
    return header;
}

QVariantMap WySource::processCookies() const
{
    QMutexLocker lock(&m_cookieMtx);   // 首页预览并行拉取会并发读写 cookie
    QVariantMap c;
    c[QStringLiteral("__remember_me")] = QStringLiteral("true");
    c[QStringLiteral("ntes_kaola_ad")] = QStringLiteral("1");
    c[QStringLiteral("osver")] =
        QStringLiteral("Microsoft-Windows-10-Professional-build-19045-64bit");
    c[QStringLiteral("deviceId")] = m_deviceId;
    c[QStringLiteral("os")] = QStringLiteral("pc");
    c[QStringLiteral("channel")] = QStringLiteral("netease");
    c[QStringLiteral("appver")] = QStringLiteral("3.1.17.204416");
    c[QStringLiteral("WEVNSM")] = QStringLiteral("1.0.0");

    // 合并用户 cookie
    const auto parts = m_cookie.split(QLatin1Char(';'));
    for (const auto &p : parts) {
        const int eq = p.indexOf(QLatin1Char('='));
        if (eq < 0) continue;
        c[p.left(eq).trimmed()] = p.mid(eq + 1).trimmed();
    }
    if (!c.contains(QStringLiteral("MUSIC_U")))
        c[QStringLiteral("MUSIC_A")] = anonymousToken();
    // 合并 weapi 预热 cookie（NMTID 等风控 cookie，缺了会被拦截返回空 body）
    for (const auto &kv : m_sessionCookie.split(QLatin1Char(';'))) {
        const QString item = kv.trimmed();
        const int eq = item.indexOf(QLatin1Char('='));
        if (eq > 0) c[item.left(eq)] = item.mid(eq + 1);
    }
    return c;
}

QString WySource::serializeCookies(const QVariantMap &cookies)
{
    QStringList parts;
    for (auto it = cookies.constBegin(); it != cookies.constEnd(); ++it) {
        if (it.value().toString().isEmpty()) continue;
        parts.append(QStringLiteral("%1=%2")
                         .arg(QString::fromUtf8(QUrl::toPercentEncoding(it.key())),
                              QString::fromUtf8(QUrl::toPercentEncoding(it.value().toString()))));
    }
    return parts.join(QStringLiteral("; "));
}

HttpResponse WySource::eapiRequest(const QString &path, const QVariantMap &data)
{
    QVariantMap payload = data;
    payload[QStringLiteral("header")] = buildEapiHeader();

    const QString params = eapiParams(path, payload);

    HttpOptions opt;
    opt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kDefaultUA);
    opt.headers[QStringLiteral("origin")] = QStringLiteral("https://music.163.com");
    opt.headers[QStringLiteral("referer")] = QStringLiteral("https://music.163.com");
    opt.headers[QStringLiteral("Cookie")] = serializeCookies(processCookies());
    opt.headers[QStringLiteral("Content-Type")] =
        QStringLiteral("application/x-www-form-urlencoded;charset=UTF-8");
    opt.timeoutMs = 20000;

    QVariantMap form;
    form[QStringLiteral("params")] = params;

    // 请求路径 = /eapi/<去掉 /api 前缀后的路径>，加密仍使用完整 path
    const QString url = QString::fromUtf8(kEapiBase) + path.mid(5);
    return HttpClient::instance()->postForm(url, form, opt);
}

// ---------------------------------------------------------------------------
// weapi（分类/歌单广场等无 eapi 变体的接口）
//   params    = AES-CBC( AES-CBC(json, presetKey, iv), secretKey, iv )  均 base64
//   encSecKey = RSA-noPadding( reverse(secretKey), publicKey )           hex
// ---------------------------------------------------------------------------

/// weapi 风控 cookie（NMTID）获取：网易首页是 SPA，纯 GET 不下发；
/// 真正的机制是首个 weapi POST 响应头会 Set-Cookie NMTID（即便 body 为空），
/// 捕获后带入后续请求即可通过。合并逻辑在 weapiRequest 内完成。
void WySource::mergeSessionCookies(const QStringList &setCookies) const
{
    QMutexLocker lock(&m_cookieMtx);
    QVariantMap cur;
    for (const auto &kv : m_sessionCookie.split(QLatin1Char(';'))) {
        const QString item = kv.trimmed();
        const int eq = item.indexOf(QLatin1Char('='));
        if (eq > 0) cur[item.left(eq)] = item.mid(eq + 1);
    }
    bool changed = false;
    for (const auto &sc : setCookies) {
        const QString kv = sc.section(QLatin1Char(';'), 0, 0).trimmed();
        const int eq = kv.indexOf(QLatin1Char('='));
        if (eq > 0 && !kv.mid(eq + 1).isEmpty()) {
            cur[kv.left(eq)] = kv.mid(eq + 1);
            changed = true;
        }
    }
    if (changed) {
        QStringList parts;
        for (auto it = cur.constBegin(); it != cur.constEnd(); ++it)
            parts.append(QStringLiteral("%1=%2").arg(it.key(), it.value().toString()));
        m_sessionCookie = parts.join(QStringLiteral("; "));
    }
}

HttpResponse WySource::weapiRequest(const QString &path, const QVariantMap &input)
{
    QVariantMap data = input;
    QString csrf;
    { QMutexLocker lock(&m_cookieMtx); csrf = m_csrf; }   // 局部锁，避免与 processCookies 嵌套
    if (!csrf.isEmpty())
        data[QStringLiteral("csrf_token")] = csrf;

    const QByteArray text =
        QJsonDocument(QJsonObject::fromVariantMap(data)).toJson(QJsonDocument::Compact);

    const QString secretKey = Crypto::randomBase62Key(16);
    // 与 JS 一致：第一段密文先转 base64 字符串，再以该字符串为明文做第二段加密
    const QByteArray stage1 = Crypto::aes128CbcEncrypt(
        text, QByteArray(kWeapiPresetKey), QByteArray(kWeapiIv));
    const QString params = QString::fromLatin1(
        Crypto::base64Encode(Crypto::aes128CbcEncrypt(
            Crypto::base64Encode(stage1), secretKey.toUtf8(), QByteArray(kWeapiIv))));

    QString reversedKey;
    for (int i = secretKey.size() - 1; i >= 0; --i)
        reversedKey.append(secretKey.at(i));
    const QString encSecKey =
        Crypto::rsaNoPaddingEncryptHex(reversedKey.toUtf8(),
                                       QString::fromLatin1(kWeapiPublicKey));
    if (kDiag && qEnvironmentVariableIsSet("MUYUN_DEBUG_HOT"))
        printf("[WEAPI] paramsLen=%d encSecKeyLen=%d\n", params.size(), encSecKey.size());

    HttpOptions opt;
    opt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kDefaultUA);
    opt.headers[QStringLiteral("referer")] = QStringLiteral("https://music.163.com/");
    opt.headers[QStringLiteral("Cookie")] = serializeCookies(processCookies());
    opt.headers[QStringLiteral("Content-Type")] =
        QStringLiteral("application/x-www-form-urlencoded;charset=UTF-8");
    opt.timeoutMs = 20000;

    QVariantMap form;
    form[QStringLiteral("params")] = params;
    form[QStringLiteral("encSecKey")] = encSecKey;

    // /api/xxx -> /weapi/xxx
    const QString url = QStringLiteral("https://music.163.com/weapi/") + path.mid(5);
    HttpResponse resp = HttpClient::instance()->postForm(url, form, opt);
    // 捕获风控 cookie（NMTID 等），供后续请求/重试带上
    mergeSessionCookies(resp.setCookies);
    return resp;
}

// ---------------------------------------------------------------------------
// 搜索
// ---------------------------------------------------------------------------

SearchResult WySource::searchSongs(const QString &keyword, int page, int limit)
{
    SearchResult result;
    if (limit <= 0) limit = 30;

    QVariantMap data;
    data[QStringLiteral("keyword")] = keyword;
    data[QStringLiteral("needCorrect")] = QStringLiteral("1");
    data[QStringLiteral("channel")] = QStringLiteral("typing");
    data[QStringLiteral("offset")] = QString::number(limit * (page - 1));
    data[QStringLiteral("scene")] = QStringLiteral("normal");
    data[QStringLiteral("total")] = page == 1 ? QStringLiteral("true") : QStringLiteral("false");
    data[QStringLiteral("limit")] = QString::number(limit);

    const HttpResponse resp = eapiRequest(QStringLiteral("/api/search/song/list/page"), data);
    if (!resp.ok || resp.body.isEmpty()) {
        printf("WY search: request failed. status=%d err=%s bodylen=%d\n",
               resp.status, qPrintable(resp.error), resp.body.size());
        for (auto it = resp.headers.constBegin(); it != resp.headers.constEnd(); ++it)
            printf("  hdr: %s = %s\n", qPrintable(it.key()), qPrintable(it.value().toString()));
        fflush(stdout);
        return result;
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return result;

    const QVariantMap root = doc.object().toVariantMap();
    if (root.value(QStringLiteral("code")).toInt() != 200) {
        printf("WY search: api code=%d body=%s\n",
               root.value(QStringLiteral("code")).toInt(),
               qPrintable(resp.bodyText.left(400)));
        fflush(stdout);
        return result;
    }

    const QVariantMap dataObj = root.value(QStringLiteral("data")).toMap();
    result.total = dataObj.value(QStringLiteral("totalCount")).toInt();
    const QVariantList resources =
        dataObj.value(QStringLiteral("resources")).toList();

    for (const auto &item : resources) {
        const QVariantMap simple =
            item.toMap().value(QStringLiteral("baseInfo")).toMap()
                .value(QStringLiteral("simpleSongData")).toMap();
        if (simple.isEmpty()) continue;

        Song s;
        s.id = simple.value(QStringLiteral("id")).toString();
        s.name = simple.value(QStringLiteral("name")).toString();
        s.platform = Platform::Netease;

        // 歌手
        QStringList artists;
        const auto ar = simple.value(QStringLiteral("ar")).toList();
        for (const auto &a : ar) artists.append(a.toMap().value(QStringLiteral("name")).toString());
        s.artist = Format::joinArtists(artists);

        const QVariantMap al = simple.value(QStringLiteral("al")).toMap();
        s.album = al.value(QStringLiteral("name")).toString();
        s.albumId = al.value(QStringLiteral("id")).toString();
        s.cover = al.value(QStringLiteral("picUrl")).toString();
        s.duration = simple.value(QStringLiteral("dt")).toDouble() / 1000.0;

        // 可用音质
        LxSongMeta lx;
        lx.source = QStringLiteral("wy");
        lx.songmid = s.id;
        lx.albumId = s.albumId;
        lx.img = s.cover;
        lx.interval = Format::duration(s.duration);

        const QVariantMap privilege = simple.value(QStringLiteral("privilege")).toMap();
        const int maxbr = privilege.value(QStringLiteral("maxbr")).toInt();
        const QString maxLevel = privilege.value(QStringLiteral("maxBrLevel")).toString();

        auto addType = [&lx](const QString &type, const QVariantMap &qualityObj) {
            if (qualityObj.isEmpty()) return;
            LxSongQualityMeta meta;
            meta.type = type;
            meta.size = Format::fileSize(qualityObj.value(QStringLiteral("size")).toLongLong());
            lx.types.append(meta);
        };

        if (maxLevel == QStringLiteral("hires"))
            addType(QStringLiteral("flac24bit"), simple.value(QStringLiteral("hr")).toMap());
        if (maxbr >= 999000)
            addType(QStringLiteral("flac"), simple.value(QStringLiteral("sq")).toMap());
        if (maxbr >= 320000)
            addType(QStringLiteral("320k"), simple.value(QStringLiteral("h")).toMap());
        if (maxbr >= 128000)
            addType(QStringLiteral("128k"), simple.value(QStringLiteral("l")).toMap());

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

    result.hasMore = (limit * page) < result.total;
    return result;
}

QStringList WySource::searchSuggest(const QString &keyword)
{
    QStringList out;
    // 网易云 suggest 类接口（web/keyword）返回的是歌单/关键词，不适合歌曲自动补全；
    // 直接复用已验证的搜索接口取若干首歌名+歌手做联想，再按热度 pop 降序排。
    QVariantMap data;
    data[QStringLiteral("keyword")] = keyword;
    data[QStringLiteral("needCorrect")] = QStringLiteral("1");
    data[QStringLiteral("channel")] = QStringLiteral("typing");
    data[QStringLiteral("offset")] = QStringLiteral("0");
    data[QStringLiteral("scene")] = QStringLiteral("normal");
    data[QStringLiteral("total")] = QStringLiteral("true");   // 与 searchMusic 完全一致：
    data[QStringLiteral("limit")] = QStringLiteral("30");     // total=false 时 typing 通道结果集会变（无原唱）
    const HttpResponse resp = eapiRequest(QStringLiteral("/api/search/song/list/page"), data);
    if (!resp.ok) return out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return out;
    const QVariantMap root = doc.object().toVariantMap();
    if (root.value(QStringLiteral("code")).toInt() != 200) return out;
    const QVariantList resources = root.value(QStringLiteral("data")).toMap()
                                      .value(QStringLiteral("resources")).toList();
    // 搜索接口返回顺序不代表热度（如"枫"首位是 Live 翻唱）→ 打分排序：
    // 歌名完全等于关键词 ≫ pop 热度（翻唱/Live 变体名带后缀，自然沉底）
    const QString kw = keyword.trimmed();
    struct Cand { QString term; double score; };
    QList<Cand> cands;
    QSet<QString> seen;
    for (const auto &item : resources) {
        const QVariantMap simple = item.toMap().value(QStringLiteral("baseInfo")).toMap()
                                       .value(QStringLiteral("simpleSongData")).toMap();
        if (simple.isEmpty()) continue;
        const QString name = simple.value(QStringLiteral("name")).toString();
        if (name.isEmpty()) continue;
        QString artist;
        const QVariantList ar = simple.value(QStringLiteral("ar")).toList();
        if (!ar.isEmpty()) artist = ar.first().toMap().value(QStringLiteral("name")).toString();
        const QString term = artist.isEmpty() ? name : (name + QLatin1Char(' ') + artist);
        if (seen.contains(term)) continue;
        seen.insert(term);
        double score = simple.value(QStringLiteral("pop")).toDouble();   // 0~100
        if (name.compare(kw, Qt::CaseInsensitive) == 0)
            score += 100000;                       // 完全同名（周杰伦的"枫"）压倒性优先
        else if (name.startsWith(kw))
            score += 1000;                         // "枫 xxx" 前缀次优
        // 翻唱/Live 变体带 originSongSimpleData（指向原唱）→ 压到原唱之后
        const QVariantMap origin = simple.value(QStringLiteral("originSongSimpleData")).toMap();
        if (!origin.isEmpty())
            score -= 50000;
        cands.append({ term, score });
    }
    std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) {
        return a.score > b.score;
    });
    for (const Cand &c : std::as_const(cands)) {
        out.append(c.term);
        if (out.size() >= 12) break;
    }
    return out;
}

QStringList WySource::hotSearchWords()
{
    QStringList out;
    for (const auto &item : hotSearchSongs())
        out.append(item.toMap().value(QStringLiteral("word")).toString());
    return out;
}

QVariantList WySource::hotSearchSongs()
{
    // 返回结构：{code:200, result:{hots:[{first:"热搜歌曲名", iconType:1}, ...]}}
    // 热搜词条本身就是歌名；iconType=1 表示歌曲
    QVariantList out;
    QVariantMap hotData;
    hotData[QStringLiteral("type")] = 1;   // 缺 type 参数会返回400"参数错误"
    const HttpResponse resp = eapiRequest(QStringLiteral("/api/search/hot"), hotData);
    if (!resp.ok) return out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return out;
    const QVariantMap root = doc.object().toVariantMap();
    if (root.value(QStringLiteral("code")).toInt() != 200) return out;
    const QVariantList hots = root.value(QStringLiteral("result")).toMap()
                                 .value(QStringLiteral("hots")).toList();
    for (const auto &item : hots) {
        const QVariantMap m = item.toMap();
        const QString word = m.value(QStringLiteral("first")).toString();
        if (word.isEmpty()) continue;
        QVariantMap entry;
        entry[QStringLiteral("word")] = word;
        entry[QStringLiteral("name")] = word;    // 热搜词条即歌名
        entry[QStringLiteral("artist")] = QString();
        out.append(entry);
        if (out.size() >= 30) break;
    }
    return out;
}

QVariantList WySource::topArtists(int limit)
{
    QVariantList out;
    QVariantMap data;
    data[QStringLiteral("limit")] = limit;
    data[QStringLiteral("offset")] = 0;
    const HttpResponse resp = eapiRequest(QStringLiteral("/api/artist/top"), data);
    if (!resp.ok) return out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return out;
    const QVariantMap root = doc.object().toVariantMap();
    if (root.value(QStringLiteral("code")).toInt() != 200) return out;
    // 兼容两种返回结构：artists / data.artists / list
    QVariantList list = root.value(QStringLiteral("artists")).toList();
    if (list.isEmpty()) list = root.value(QStringLiteral("data")).toMap().value(QStringLiteral("artists")).toList();
    if (list.isEmpty()) list = root.value(QStringLiteral("list")).toList();
    for (const auto &item : list) {
        const QVariantMap m = item.toMap();
        QVariantMap entry;
        entry[QStringLiteral("id")] = m.value(QStringLiteral("id")).toString();
        entry[QStringLiteral("name")] = m.value(QStringLiteral("name")).toString();
        QString avatar = m.value(QStringLiteral("picUrl")).toString();
        if (avatar.isEmpty()) avatar = m.value(QStringLiteral("img1v1Url")).toString();
        if (avatar.isEmpty()) avatar = m.value(QStringLiteral("cover")).toString();
        entry[QStringLiteral("avatar")] = avatar;
        if (!entry.value(QStringLiteral("name")).toString().isEmpty())
            out.append(entry);
        if (out.size() >= limit) break;
    }
    return out;
}

QVector<PlaylistSummary> WySource::searchPlaylists(const QString &keyword, int page, int limit)
{
    QVector<PlaylistSummary> out;
    QVariantMap data;
    data[QStringLiteral("keyword")] = keyword;
    data[QStringLiteral("offset")] = QString::number(limit * (page - 1));
    data[QStringLiteral("limit")] = QString::number(limit);
    data[QStringLiteral("total")] = page == 1 ? QStringLiteral("true") : QStringLiteral("false");
    data[QStringLiteral("scene")] = QStringLiteral("normal");

    const HttpResponse resp = eapiRequest(QStringLiteral("/api/search/playlist/page"), data);
    if (!resp.ok) return out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return out;
    const QVariantMap root = doc.object().toVariantMap();
    if (root.value(QStringLiteral("code")).toInt() != 200) return out;

    const QVariantList list =
        root.value(QStringLiteral("data")).toMap().value(QStringLiteral("resources")).toList();
    for (const auto &item : list) {
        const QVariantMap base =
            item.toMap().value(QStringLiteral("baseInfo")).toMap();
        PlaylistSummary p;
        p.id = base.value(QStringLiteral("id")).toString();
        p.name = base.value(QStringLiteral("name")).toString();
        p.cover = base.value(QStringLiteral("coverUrl")).toString();
        p.creator = base.value(QStringLiteral("creator")).toMap()
                        .value(QStringLiteral("nickname")).toString();
        p.platform = Platform::Netease;
        out.append(p);
    }
    return out;
}

QVector<AlbumInfo> WySource::searchAlbums(const QString &keyword, int page, int limit)
{
    QVector<AlbumInfo> out;
    QVariantMap data;
    data[QStringLiteral("keyword")] = keyword;
    data[QStringLiteral("offset")] = QString::number(limit * (page - 1));
    data[QStringLiteral("limit")] = QString::number(limit);
    data[QStringLiteral("total")] = page == 1 ? QStringLiteral("true") : QStringLiteral("false");
    data[QStringLiteral("scene")] = QStringLiteral("normal");

    const HttpResponse resp = eapiRequest(QStringLiteral("/api/search/album/page"), data);
    if (!resp.ok) return out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return out;
    const QVariantMap root = doc.object().toVariantMap();
    if (root.value(QStringLiteral("code")).toInt() != 200) return out;

    const QVariantList list =
        root.value(QStringLiteral("data")).toMap().value(QStringLiteral("resources")).toList();
    for (const auto &item : list) {
        const QVariantMap base =
            item.toMap().value(QStringLiteral("baseInfo")).toMap();
        AlbumInfo a;
        a.id = base.value(QStringLiteral("id")).toString();
        a.name = base.value(QStringLiteral("name")).toString();
        a.cover = base.value(QStringLiteral("picUrl")).toString();
        a.artist = base.value(QStringLiteral("artist")).toMap()
                       .value(QStringLiteral("name")).toString();
        a.platform = Platform::Netease;
        out.append(a);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 播放链接
// ---------------------------------------------------------------------------

QString WySource::getMusicUrl(const Song &song, AudioQuality quality)
{
    const QString songId = song.lx.songmid.isEmpty() ? song.id : song.lx.songmid;
    if (songId.isEmpty()) return QString();

    QVariantMap data;
    QVariantList ids;
    ids.append(songId);
    data[QStringLiteral("ids")] = ids;
    data[QStringLiteral("br")] = QString::number(qualityToBr(quality));

    const HttpResponse resp = eapiRequest(QStringLiteral("/api/song/enhance/player/url"), data);
    if (!resp.ok) return QString();

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return QString();
    const QVariantMap root = doc.object().toVariantMap();
    const QVariantList list = root.value(QStringLiteral("data")).toList();
    if (list.isEmpty()) return QString();
    return list.first().toMap().value(QStringLiteral("url")).toString();
}

// ---------------------------------------------------------------------------
// 歌词
// ---------------------------------------------------------------------------

SongLyric WySource::getLyric(const Song &song)
{
    SongLyric lyric;
    const QString songId = song.lx.songmid.isEmpty() ? song.id : song.lx.songmid;
    if (songId.isEmpty()) return lyric;

    QVariantMap data;
    data[QStringLiteral("id")] = songId;
    data[QStringLiteral("cp")] = false;
    data[QStringLiteral("tv")] = -1;
    data[QStringLiteral("lv")] = -1;
    data[QStringLiteral("rv")] = -1;
    data[QStringLiteral("kv")] = -1;

    const HttpResponse resp = eapiRequest(QStringLiteral("/api/song/lyric"), data);
    if (!resp.ok) return lyric;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return lyric;
    const QVariantMap root = doc.object().toVariantMap();

    lyric.rawLrc = root.value(QStringLiteral("lrc")).toMap().value(QStringLiteral("lyric")).toString();
    lyric.rawTranslation =
        root.value(QStringLiteral("tlyric")).toMap().value(QStringLiteral("lyric")).toString();
    lyric.rawRoman =
        root.value(QStringLiteral("romalrc")).toMap().value(QStringLiteral("lyric")).toString();
    lyric.hasTranslation = !lyric.rawTranslation.isEmpty();
    lyric.hasRoman = !lyric.rawRoman.isEmpty();
    return lyric;
}

// ---------------------------------------------------------------------------
// 榜单 / 歌单 / 评论
// ---------------------------------------------------------------------------

QVector<PlaylistSummary> WySource::getRecommendPlaylists(int limit)
{
    QVector<PlaylistSummary> out;
    QVariantMap data;
    data[QStringLiteral("limit")] = QString::number(limit);
    data[QStringLiteral("total")] = QStringLiteral("true");
    data[QStringLiteral("n")] = QString::number(1000);

    const QString path = QStringLiteral("/api/personalized/playlist");
    const HttpResponse resp = eapiRequest(path, data);
    if (!resp.ok) return out;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return out;
    const QVariantMap root = doc.object().toVariantMap();

    for (const auto &item : root.value(QStringLiteral("result")).toList()) {
        const QVariantMap m = item.toMap();
        PlaylistSummary p;
        p.id = m.value(QStringLiteral("id")).toString();
        p.name = m.value(QStringLiteral("name")).toString();
        p.cover = m.value(QStringLiteral("picUrl")).toString();
        p.playCount = m.value(QStringLiteral("playCount")).toLongLong();
        p.hasPlayCount = p.playCount > 0;
        p.platform = Platform::Netease;
        out.append(p);
    }
    return out;
}

QVector<ToplistInfo> WySource::getToplists()
{
    // 真正已下线的榜单 id。注意这份名单必须很小——
    //
    // 2026-09-30 教训：这里曾经有 33 个 id，是被 eapi 风控误杀后当作"死榜"逐条
    // 拉黑的。实际上那 33 个里有 30 个是正常有数据的，只是 eapi 的
    // /api/v3/playlist/detail 已被全量拦死返回空 body（见 getToplist 头部注释）。
    // 改用 weapi 后这些榜单全部恢复。用 weapi 逐个实测后，真正返回 0 首的
    // 只剩下面 3 个。后续若再出现空白卡片，先怀疑接口/风控，不要急着往这里加 id。
    static const QSet<QString> kDeadIds = {
        QStringLiteral("8246775932"),  // 实时热度榜
        QStringLiteral("8537588450"),  // 喜力?星电音派对潮音榜
        QStringLiteral("8661209031"),  // 乐夏榜
    };

    QVector<ToplistInfo> out;
    const HttpResponse resp = eapiRequest(QStringLiteral("/api/toplist"), QVariantMap());
    if (!resp.ok) return out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return out;
    const QVariantList list = doc.object().toVariantMap().value(QStringLiteral("list")).toList();
    for (const auto &item : list) {
        const QVariantMap m = item.toMap();
        const QString id = m.value(QStringLiteral("id")).toString();
        if (kDeadIds.contains(id)) continue;
        ToplistInfo t;
        t.id = id;
        t.name = m.value(QStringLiteral("name")).toString();
        t.description = m.value(QStringLiteral("description")).toString();
        t.cover = m.value(QStringLiteral("coverImgUrl")).toString();
        t.updateTime = Format::date(m.value(QStringLiteral("updateTime")).toLongLong());
        t.platform = Platform::Netease;
        out.append(t);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 榜单 / 歌单详情
//
// 2026-09-30 curl 实测结论：网易 **eapi** 的 /api/v3/playlist/detail 已被风控
// 全量拦死——对任意 id（飙升榜、新歌榜、热门用户歌单都一样）都返回空 body，
// 和 id 是否有效无关。之前把这类空 body 误判成"榜单已下线"，据此维护了一份
// 33 个 id 的黑名单，其中 30 个其实有数据。
//
// 正确做法（参考工程 lx-music 的 leaderboard.js / musicDetail.js 同款）：
//   1. 榜单 / 歌单详情走 **weapi**，且必须带 n（实测不带 n 只回 10 首）
//   2. weapi 无论 n 传多大最多只回前 10 首 tracks，但 playlist.trackIds 是全量
//      → 用 weapi /api/v3/song/detail 按 trackIds 批量补全成完整曲目
// ---------------------------------------------------------------------------

namespace {
/// 把 weapi 返回的 track 对象解析成 Song（榜单详情 / 歌单详情共用）。
/// 同时填充 lx 元信息——播放完全依赖用户导入的 LX 脚本，缺了出不了声音。
Song toWySong(const QVariantMap &m)
{
    Song s;
    s.id = m.value(QStringLiteral("id")).toString();
    s.name = m.value(QStringLiteral("name")).toString();
    QStringList artists;
    for (const auto &a : m.value(QStringLiteral("ar")).toList())
        artists.append(a.toMap().value(QStringLiteral("name")).toString());
    s.artist = Format::joinArtists(artists);
    const QVariantMap al = m.value(QStringLiteral("al")).toMap();
    s.album = al.value(QStringLiteral("name")).toString();
    s.albumId = al.value(QStringLiteral("id")).toString();
    s.cover = al.value(QStringLiteral("picUrl")).toString();
    s.duration = m.value(QStringLiteral("dt")).toDouble() / 1000.0;
    s.platform = Platform::Netease;
    LxSongMeta lx;
    lx.source = QStringLiteral("wy");
    lx.songmid = s.id;
    lx.albumId = s.albumId;
    lx.img = s.cover;
    s.lx = lx;
    s.hasLx = true;
    return s;
}
} // namespace

QVariantList WySource::fetchTracksByIds(const QVariantList &trackIds)
{
    QVariantList out;
    if (trackIds.isEmpty()) return out;

    QJsonArray cArr;
    QStringList idStrs;
    for (const auto &t : trackIds) {
        const QString sid = t.toMap().value(QStringLiteral("id")).toString();
        if (sid.isEmpty()) continue;
        QJsonObject o;
        o[QStringLiteral("id")] = sid.toLongLong();
        cArr.append(o);
        idStrs.append(sid);
    }
    if (cArr.isEmpty()) return out;

    QVariantMap sd;
    sd[QStringLiteral("c")] =
        QString::fromUtf8(QJsonDocument(cArr).toJson(QJsonDocument::Compact));
    // 参考工程同时传 c 与 ids 两种形式，实测两者都要带上
    sd[QStringLiteral("ids")] =
        QStringLiteral("[") + idStrs.join(QStringLiteral(",")) + QStringLiteral("]");

    const HttpResponse resp = weapiRequest(QStringLiteral("/api/v3/song/detail"), sd);
    if (!resp.ok || resp.body.isEmpty()) return out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return out;
    return doc.object().toVariantMap().value(QStringLiteral("songs")).toList();
}

// 网页兜底：weapi 被风控全量拦死时，直接抓 music.163.com/discover/toplist?id=X 页面里
// 内嵌的 <textarea id="song-list-pre-data"> JSON（旧格式：album/artists/duration）。
// 这是浏览器打开榜单页看到的数据，风控宽松得多。
QVector<Song> WySource::fetchToplistFromWeb(const QString &id, int want)
{
    QVector<Song> out;
    HttpOptions opt;
    opt.headers[QStringLiteral("User-Agent")] = QString::fromUtf8(kDefaultUA);
    opt.headers[QStringLiteral("Referer")] = QStringLiteral("https://music.163.com/");
    opt.timeoutMs = 15000;
    const HttpResponse resp = HttpClient::instance()->get(
        QStringLiteral("https://music.163.com/discover/toplist?id=") + id, opt);
    if (!resp.ok || resp.body.isEmpty()) return out;

    static const QRegularExpression re(
        QStringLiteral("song-list-pre-data[^>]*>(.*?)</textarea>"),
        QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch m = re.match(QString::fromUtf8(resp.body));
    if (!m.hasMatch()) return out;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(m.captured(1).toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) return out;

    for (const auto &v : doc.array().toVariantList()) {
        const QVariantMap o = v.toMap();
        Song s;
        s.id = QString::number(o.value(QStringLiteral("id")).toLongLong());
        s.name = o.value(QStringLiteral("name")).toString();
        QStringList artists;
        for (const auto &a : o.value(QStringLiteral("artists")).toList())
            artists.append(a.toMap().value(QStringLiteral("name")).toString());
        if (artists.isEmpty()) {   // 个别榜只有单数 artist 字段
            const QString one = o.value(QStringLiteral("artist")).toMap()
                                    .value(QStringLiteral("name")).toString();
            if (!one.isEmpty()) artists.append(one);
        }
        s.artist = Format::joinArtists(artists);
        const QVariantMap al = o.value(QStringLiteral("album")).toMap();
        s.album = al.value(QStringLiteral("name")).toString();
        s.albumId = QString::number(al.value(QStringLiteral("id")).toLongLong());
        s.cover = al.value(QStringLiteral("picUrl")).toString();
        s.duration = o.value(QStringLiteral("duration")).toDouble() / 1000.0;
        s.platform = Platform::Netease;
        LxSongMeta lx;
        lx.source = QStringLiteral("wy");
        lx.songmid = s.id;
        lx.albumId = s.albumId;
        lx.img = s.cover;
        s.lx = lx;
        s.hasLx = true;
        out.append(s);
        if (want > 0 && out.size() >= want) break;
    }
    return out;
}

SearchResult WySource::getToplist(const QString &id, int page, int limit)
{
    SearchResult result;
    const int want = limit > 0 ? limit : 100;

    QVariantMap req;
    req[QStringLiteral("id")] = id;
    // 小请求（首页预览 3 首）不带全量 n=100000：载荷小、风控触发率低
    req[QStringLiteral("n")] = want <= 10 ? 10 : 100000;   // 实测：不带 n 只回 10 首，带了回 100 首
    req[QStringLiteral("p")] = 1;

    // weapi 偶发空 body（逐请求随机风控，与 id 无关）。
    // 详情页（want 大）多重试 5 次；首页预览（want≤10）只试 2 次即转网页兜底
    // （网页兜底稳定且快，避免首页冷启动被重试退避拖慢）。
    const int maxAttempts = want <= 10 ? 2 : 5;
    static const int kDelaysMs[5] = {0, 400, 700, 1100, 1600};
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        if (attempt > 0) {
            const int jitter = QRandomGenerator::global()->bounded(201) - 100;
            QThread::msleep(kDelaysMs[attempt] + jitter);
        }
        const HttpResponse resp = weapiRequest(QStringLiteral("/api/v3/playlist/detail"), req);
        if (kDiag && qEnvironmentVariableIsSet("MUYUN_DEBUG_HOME"))
            printf("[WY-TOPLIST] id=%s attempt=%d ok=%d bodyLen=%d\n",
                   qPrintable(id), attempt, resp.ok, resp.body.size());
        if (!resp.ok || resp.body.isEmpty()) continue;
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
        if (err.error != QJsonParseError::NoError) continue;
        const QVariantMap pl = doc.object().toVariantMap()
                                   .value(QStringLiteral("playlist")).toMap();
        QVariantList tracks = pl.value(QStringLiteral("tracks")).toList();

        // tracks 不够用时（weapi 最多回 10 首，部分榜甚至不回），用 trackIds 批量补
        // ——只补到刚好 want 首，预览 3 首就不拉 100 首
        if (tracks.size() < want) {
            QVariantList trackIds = pl.value(QStringLiteral("trackIds")).toList();
            if (trackIds.size() > want) trackIds = trackIds.mid(0, want);
            const QVariantList filled = fetchTracksByIds(trackIds);
            if (filled.size() > tracks.size()) tracks = filled;
        }
        if (tracks.isEmpty()) continue;   // 空榜 → 重试

        for (const auto &item : tracks)
            result.songs.append(toWySong(item.toMap()));
        break;
    }
    // weapi 多次仍空 → 网页兜底（抓 discover/toplist 内嵌 JSON）
    if (result.songs.isEmpty()) {
        const QVector<Song> web = fetchToplistFromWeb(id, want);
        if (kDiag && qEnvironmentVariableIsSet("MUYUN_DEBUG_HOME"))
            printf("[WY-TOPLIST] id=%s WEB-FALLBACK songs=%d\n", qPrintable(id), web.size());
        for (const auto &s : web)
            result.songs.append(s);
    }
    if (result.songs.size() > want)
        result.songs = result.songs.mid(0, want);
    result.total = result.songs.size();
    Q_UNUSED(page)
    return result;
}

Playlist WySource::getPlaylistDetail(const QString &id)
{
    Playlist pl;

    QVariantMap req;
    req[QStringLiteral("id")] = id;
    req[QStringLiteral("n")] = 100000;
    req[QStringLiteral("p")] = 1;

    // eapi 该端点已被风控全量拦死，走 weapi；风控抖动时重试 3 次
    HttpResponse resp;
    for (int attempt = 0; attempt < 3; ++attempt) {
        resp = weapiRequest(QStringLiteral("/api/v3/playlist/detail"), req);
        if (resp.ok && !resp.body.isEmpty()) break;
        if (attempt < 2) QThread::msleep(500);
    }
    if (!resp.ok || resp.body.isEmpty()) return pl;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return pl;
    const QVariantMap data = doc.object().toVariantMap()
                                 .value(QStringLiteral("playlist")).toMap();

    pl.id = data.value(QStringLiteral("id")).toString();
    pl.name = data.value(QStringLiteral("name")).toString();
    pl.description = data.value(QStringLiteral("description")).toString();
    pl.cover = data.value(QStringLiteral("coverImgUrl")).toString();
    pl.creator = data.value(QStringLiteral("creator")).toMap()
                     .value(QStringLiteral("nickname")).toString();
    pl.platform = Platform::Netease;

    QVariantList tracks = data.value(QStringLiteral("tracks")).toList();
    // weapi 最多只回 10 首，用全量 trackIds 补成完整歌单
    if (tracks.size() <= 10) {
        const QVariantList filled =
            fetchTracksByIds(data.value(QStringLiteral("trackIds")).toList());
        if (filled.size() > tracks.size()) tracks = filled;
    }
    for (const auto &item : tracks)
        pl.songs.append(toWySong(item.toMap()));
    return pl;
}

QVariantList WySource::playlistCategories()
{
    // eapi /api/playlist/catalogue -> { categories:{1:"语种",...},
    //                                    sub:[{name,category},...] }
    QVariantList out;
    const HttpResponse resp = eapiRequest(QStringLiteral("/api/playlist/catalogue"), QVariantMap());
    if (!resp.ok) return out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return out;
    const QVariantMap root = doc.object().toVariantMap();
    if (root.value(QStringLiteral("code")).toInt() != 200) return out;

    const QVariantMap categories = root.value(QStringLiteral("categories")).toMap();
    const QVariantList sub = root.value(QStringLiteral("sub")).toList();
    QHash<QString, QVariantList> byGroup;   // group name -> tags
    QStringList groupOrder;
    for (const auto &item : sub) {
        const QVariantMap m = item.toMap();
        const QString group = categories.value(m.value(QStringLiteral("category")).toString())
                                 .toString();
        const QString tag = m.value(QStringLiteral("name")).toString();
        if (group.isEmpty() || tag.isEmpty()) continue;
        if (!byGroup.contains(group)) { byGroup[group] = {}; groupOrder.append(group); }
        byGroup[group].append(tag);
    }
    for (const auto &g : groupOrder) {
        QVariantMap entry;
        entry[QStringLiteral("group")] = g;
        entry[QStringLiteral("tags")] = byGroup.value(g);
        out.append(entry);
    }
    return out;
}

QVector<PlaylistSummary> WySource::explorePlaylists(const QString &cat,
                                                    const QString &order,
                                                    int page, int limit,
                                                    bool *hasMore)
{
    QVector<PlaylistSummary> out;
    QVariantMap data;
    data[QStringLiteral("cat")] = cat.isEmpty() ? QStringLiteral("全部") : cat;
    data[QStringLiteral("order")] = order;      // hot / new
    data[QStringLiteral("limit")] = limit;
    data[QStringLiteral("offset")] = limit * (page - 1);
    data[QStringLiteral("total")] = true;
    // 2026-10 实测：weapi /api/playlist/list 已被网易下线（恒空 body，探针对照），
    // 同路径 **eapi** 变体返回完整数据 → eapi 优先，weapi 仅作历史兜底。
    HttpResponse resp;
    for (int attempt = 0; attempt < 3 && resp.body.isEmpty(); ++attempt) {
        resp = eapiRequest(QStringLiteral("/api/playlist/list"), data);
        if (resp.ok && !resp.body.isEmpty()) break;
        QThread::msleep(150 + QRandomGenerator::global()->bounded(250) * (attempt + 1));
    }
    if (resp.body.isEmpty()) {
        for (int attempt = 0; attempt < 2 && resp.body.isEmpty(); ++attempt) {
            resp = weapiRequest(QStringLiteral("/api/playlist/list"), data);
            if (resp.ok && !resp.body.isEmpty()) break;
            QThread::msleep(150 + QRandomGenerator::global()->bounded(200));
        }
    }
    if (kDiag && qEnvironmentVariableIsSet("MUYUN_DEBUG_HOT"))
        printf("[EXPLORE] ok=%d status=%d err=%s body=%s\n", resp.ok, resp.status,
               qPrintable(resp.error), resp.body.left(600).constData());
    if (hasMore) *hasMore = false;
    if (!resp.ok) return out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return out;
    const QVariantMap root = doc.object().toVariantMap();
    if (root.contains(QStringLiteral("code"))
        && root.value(QStringLiteral("code")).toInt() != 200) return out;

    // 返回结构 {code, playlists:[...], total, more}
    if (hasMore) {
        if (root.contains(QStringLiteral("more")))
            *hasMore = root.value(QStringLiteral("more")).toBool();
        else
            *hasMore = page * limit < root.value(QStringLiteral("total")).toInt();
    }

    for (const auto &item : root.value(QStringLiteral("playlists")).toList()) {
        const QVariantMap m = item.toMap();
        PlaylistSummary p;
        p.id = m.value(QStringLiteral("id")).toString();
        p.name = m.value(QStringLiteral("name")).toString();
        p.cover = m.value(QStringLiteral("coverImgUrl")).toString();
        p.creator = m.value(QStringLiteral("creator")).toMap()
                       .value(QStringLiteral("nickname")).toString();
        p.trackCount = m.value(QStringLiteral("trackCount")).toInt();
        p.playCount = static_cast<qint64>(m.value(QStringLiteral("playCount")).toDouble());
        p.hasPlayCount = true;
        p.platform = Platform::Netease;
        if (!p.id.isEmpty()) out.append(p);
    }
    return out;
}

CommentPage WySource::getComments(const Song &song, int page, int limit, bool hot)
{
    CommentPage out;
    const QString songId = song.lx.songmid.isEmpty() ? song.id : song.lx.songmid;
    if (songId.isEmpty()) return out;

    const QString path = QStringLiteral("/api/v1/resource/comments/R_SO_4_%1").arg(songId);
    QVariantMap data;
    data[QStringLiteral("rid")] = songId;
    data[QStringLiteral("offset")] = QString::number((page - 1) * limit);
    data[QStringLiteral("limit")] = QString::number(limit);
    if (hot) data[QStringLiteral("sortType")] = 1;

    const HttpResponse resp = eapiRequest(path, data);
    if (!resp.ok) return out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &err);
    if (err.error != QJsonParseError::NoError) return out;
    const QVariantMap root = doc.object().toVariantMap();

    out.total = root.value(QStringLiteral("total")).toInt();
    const QVariantList list = hot ? root.value(QStringLiteral("hotComments")).toList()
                                  : root.value(QStringLiteral("comments")).toList();
    for (const auto &item : list) {
        const QVariantMap m = item.toMap();
        Comment c;
        c.id = m.value(QStringLiteral("commentId")).toString();
        c.text = m.value(QStringLiteral("content")).toString();
        c.time = m.value(QStringLiteral("time")).toLongLong();
        c.timeStr = Format::dateTime(c.time);
        c.likedCount = m.value(QStringLiteral("likedCount")).toInt();
        c.liked = m.value(QStringLiteral("liked")).toBool();
        const QVariantMap u = m.value(QStringLiteral("user")).toMap();
        c.userName = u.value(QStringLiteral("nickname")).toString();
        c.userAvatar = u.value(QStringLiteral("avatarUrl")).toString();
        c.userId = u.value(QStringLiteral("userId")).toString();
        out.comments.append(c);
    }
    return out;
}

} // namespace Muyun
