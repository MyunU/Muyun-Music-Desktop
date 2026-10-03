#include "MusicSdk.h"

#include "core/music/wy/WySource.h"
#include "core/music/tx/TxSource.h"
#include "core/music/kw/KwSource.h"
#include "core/music/kg/KgSource.h"
#include "core/music/mg/MgSource.h"
#include "core/music/lx/LxScriptEngine.h"
#include "core/utils/Format.h"

#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <QCoreApplication>
#include <QDebug>

namespace Muyun {

static MusicSdk *s_instance = nullptr;

MusicSdk::MusicSdk(QObject *parent) : QObject(parent)
{
    registerSources();
    m_lxEngine = new LxScriptEngine();
}

MusicSdk::~MusicSdk()
{
    delete m_lxEngine;
    m_lxEngine = nullptr;
    qDeleteAll(m_sources);
    m_sources.clear();
}

MusicSdk *MusicSdk::instance()
{
    if (!s_instance) s_instance = new MusicSdk(qApp);
    return s_instance;
}

void MusicSdk::registerSources()
{
    // 五个在线平台音源
    registerSource(new WySource());
    registerSource(new TxSource());
    registerSource(new KwSource());
    registerSource(new KgSource());
    registerSource(new MgSource());

    // 默认平台顺序（与原工程一致：小蜗 -> 小枸 -> 小蜜 -> 小芸 -> 小秋）
    m_platformOrder = {QStringLiteral("kw"), QStringLiteral("kg"),
                       QStringLiteral("mg"), QStringLiteral("wy"), QStringLiteral("tx")};
}

void MusicSdk::registerSource(MusicSource *source)
{
    if (!source) return;
    m_sources.insert(source->code(), source);
}

QStringList MusicSdk::sourceCodes() const { return m_sources.keys(); }

QVariantList MusicSdk::sourceInfos() const
{
    QVariantList out;
    for (const auto &code : sourceCodes()) {
        QVariantMap m;
        m[QStringLiteral("code")] = code;
        m[QStringLiteral("name")] = m_sources.value(code)->name();
        m[QStringLiteral("platform")] = platformId(m_sources.value(code)->platform());
        out.append(m);
    }
    return out;
}

MusicSource *MusicSdk::source(const QString &code) const
{
    return m_sources.value(code, nullptr);
}

MusicSource *MusicSdk::source(Platform p) const { return source(platformSourceCode(p)); }

SearchResult MusicSdk::search(const QString &code, const QString &keyword, int page, int limit)
{
    auto *src = source(code);
    if (!src) return {};
    return src->searchSongs(keyword, page, limit);
}

SearchResult MusicSdk::searchAll(const QString &keyword, int page, int limit)
{
    SearchResult merged;
    QVector<QVector<Song>> buckets;

    // 按平台顺序搜索，每平台取指定条数
    const int perPlatform = qMax(1, limit / qMax(1, m_sources.size()));
    for (const auto &code : m_platformOrder) {
        auto *src = source(code);
        if (!src) continue;
        const SearchResult r = src->searchSongs(keyword, page, perPlatform);
        if (!r.songs.isEmpty()) {
            buckets.append(r.songs);
            merged.total += r.total;
            if (r.hasMore) merged.hasMore = true;
        }
    }

    // 交错合并：轮转取各平台结果，保证结果多样性
    qsizetype maxLen = 0;
    for (const auto &b : buckets) maxLen = qMax(maxLen, b.size());
    for (qsizetype i = 0; i < maxLen; ++i) {
        for (const auto &b : buckets) {
            if (i < b.size()) merged.songs.append(b.at(i));
        }
    }
    return merged;
}

QString MusicSdk::resolveUrl(const Song &song, AudioQuality quality, AudioQuality *actualQuality)
{
    // 1) 优先用 LX 自定义音源脚本解析（当已加载脚本且歌曲带 lx 元信息）
    if (hasLxScript() && song.hasLx) {
        const auto chain = qualityFallbackChain(quality);
        for (auto q : chain) {
            const QString url = m_lxEngine->musicUrl(song.lx.source, song.lx.toMap(),
                                                     qualityId(q));
            if (!url.isEmpty()) {
                if (actualQuality) *actualQuality = q;
                return url;
            }
        }
    }

    const QString code = song.lx.source.isEmpty() ? platformSourceCode(song.platform)
                                                  : song.lx.source;
    auto *src = source(code);
    if (!src) return QString();

    // 沿音质降级链逐级尝试
    const auto chain = qualityFallbackChain(quality);
    for (auto q : chain) {
        const QString url = src->getMusicUrl(song, q);
        if (!url.isEmpty()) {
            if (actualQuality) *actualQuality = q;
            return url;
        }
    }

    // 兜底（智能换源）：本音源拿不到链接时，跨平台找同名歌曲借用其它音源的链接
    FindMusicRequest req;
    req.name = song.name;
    req.singer = song.artist;
    req.albumName = song.album;
    req.interval = Format::duration(song.duration);
    req.source = code;

    const QVector<Song> candidates = findMusic(req, 8);
    for (const auto &c : candidates) {
        const QString cCode = c.lx.source.isEmpty() ? platformSourceCode(c.platform)
                                                    : c.lx.source;
        if (cCode == code) continue;
        auto *other = source(cCode);
        if (!other) continue;
        for (auto q : chain) {
            const QString url = other->getMusicUrl(c, q);
            if (!url.isEmpty()) {
                if (actualQuality) *actualQuality = q;
                return url;
            }
        }
    }
    return QString();
}

QString MusicSdk::resolveUrlAtQuality(const Song &song, AudioQuality quality)
{
    // 与 resolveUrl 的区别：只试这一档音质、只用这首歌自带的 source，
    // 不沿降级链、不跨平台兜底。上层（下载器）自己组织尝试顺序。
    if (hasLxScript() && song.hasLx) {
        const QString url = m_lxEngine->musicUrl(song.lx.source, song.lx.toMap(),
                                                 qualityId(quality));
        if (!url.isEmpty()) return url;
        // 脚本没给链接时，仍然允许内置音源用同一首歌的元信息试一次
    }
    const QString code = song.lx.source.isEmpty() ? platformSourceCode(song.platform)
                                                  : song.lx.source;
    auto *src = source(code);
    if (!src) return QString();
    return src->getMusicUrl(song, quality);
}

SongLyric MusicSdk::resolveLyric(const Song &song)
{
    const QString code = song.lx.source.isEmpty() ? platformSourceCode(song.platform)
                                                  : song.lx.source;
    auto *src = source(code);
    if (!src) return {};

    SongLyric lyric = src->getLyric(song);
    if (!lyric.rawLrc.isEmpty()) return lyric;

    // 兜底：本音源取不到歌词（如酷我、咪咕）时，
    // 跨平台找同名同歌手歌曲，从其它音源补齐歌词。
    FindMusicRequest req;
    req.name = song.name;
    req.singer = song.artist;
    req.albumName = song.album;
    req.interval = Format::duration(song.duration);
    req.source = code;

    const QVector<Song> candidates = findMusic(req, 5);
    for (const auto &c : candidates) {
        const QString otherCode = c.lx.source.isEmpty() ? platformSourceCode(c.platform)
                                                        : c.lx.source;
        if (otherCode == code) continue;
        auto *other = source(otherCode);
        if (!other) continue;
        const SongLyric otherLyric = other->getLyric(c);
        if (!otherLyric.rawLrc.isEmpty()) return otherLyric;
    }
    return lyric;
}

// ===========================================================================
// LX 自定义音源脚本
// ===========================================================================

bool MusicSdk::loadLxScript(const QString &scriptPath, QString *error)
{
    if (!m_lxEngine) m_lxEngine = new LxScriptEngine();
    if (scriptPath.isEmpty()) {
        delete m_lxEngine;
        m_lxEngine = new LxScriptEngine();
        return false;
    }
    return m_lxEngine->loadScript(scriptPath, error);
}

bool MusicSdk::hasLxScript() const
{
    return m_lxEngine && m_lxEngine->inited();
}

QString MusicSdk::lxScriptName() const
{
    if (!hasLxScript()) return QString();
    return m_lxEngine->scriptInfo().value(QStringLiteral("name")).toString();
}

QVariantMap MusicSdk::lxUpdateAlert() const
{
    if (!m_lxEngine) return {};
    return m_lxEngine->updateAlert();
}

void MusicSdk::setUpdateAlertCallback(std::function<void(const QVariantMap &)> cb)
{
    if (m_lxEngine) m_lxEngine->setUpdateAlertCallback(std::move(cb));
}

QVector<PlaylistSummary> MusicSdk::getRecommendPlaylists(const QString &code, int limit)
{
    auto *src = source(code);
    return src ? src->getRecommendPlaylists(limit) : QVector<PlaylistSummary>{};
}

QVector<ToplistInfo> MusicSdk::getToplists(const QString &code)
{
    auto *src = source(code);
    return src ? src->getToplists() : QVector<ToplistInfo>{};
}

SearchResult MusicSdk::getToplist(const QString &code, const QString &id, int page, int limit)
{
    auto *src = source(code);
    return src ? src->getToplist(id, page, limit) : SearchResult{};
}

Playlist MusicSdk::getPlaylistDetail(const QString &code, const QString &id)
{
    auto *src = source(code);
    return src ? src->getPlaylistDetail(id) : Playlist{};
}

QVariantList MusicSdk::playlistCategories(const QString &code)
{
    auto *src = source(code);
    return src ? src->playlistCategories() : QVariantList{};
}

QVector<PlaylistSummary> MusicSdk::explorePlaylists(const QString &code, const QString &cat,
                                                    const QString &order, int page, int limit,
                                                    bool *hasMore)
{
    auto *src = source(code);
    return src ? src->explorePlaylists(cat, order, page, limit, hasMore)
               : QVector<PlaylistSummary>{};
}

CommentPage MusicSdk::getComments(const Song &song, int page, int limit, bool hot)
{
    const QString code = song.lx.source.isEmpty() ? platformSourceCode(song.platform)
                                                  : song.lx.source;
    auto *src = source(code);
    return src ? src->getComments(song, page, limit, hot) : CommentPage{};
}

// ---------------------------------------------------------------------------
// findMusic —— 跨平台找歌
// ---------------------------------------------------------------------------

QString MusicSdk::filterStr(const QString &s)
{
    // 与原工程 FILTER_STR_RX 等价：去掉空白与常见标点
    static const QRegularExpression rx(
        QStringLiteral("[\\s'\\.,，&\"、\\(\\)（）`~<>|/\\[\\]!！\\-]"));
    QString out = s;
    out.replace(rx, QString());
    return out.toLower();
}

QString MusicSdk::sortSingers(const QString &singer)
{
    static const QRegularExpression splitRx(QStringLiteral("[、&;;；/,，|]"));
    if (!singer.isEmpty() && singer.contains(splitRx)) {
        QStringList parts = singer.split(splitRx);
        std::sort(parts.begin(), parts.end());
        return parts.join(QStringLiteral("、"));
    }
    return singer;
}

double MusicSdk::parseInterval(const QString &interval)
{
    if (interval.isEmpty()) return 0.0;
    const QStringList parts = interval.split(QLatin1Char(':'));
    double value = 0.0;
    double unit = 1.0;
    for (int i = parts.size() - 1; i >= 0; --i) {
        value += parts.at(i).toDouble() * unit;
        unit *= 60.0;
    }
    return value;
}

namespace {

struct VersionMarker {
    QString key;
    QString pattern;
};

const QVector<VersionMarker> &versionMarkers()
{
    static const QVector<VersionMarker> list = {
        {QStringLiteral("live"), QStringLiteral("(live|演唱会|现场版?)")},
        {QStringLiteral("remix"), QStringLiteral("(remix|混音|remixed)")},
        {QStringLiteral("cover"), QStringLiteral("(翻自|翻唱|cover)")},
        {QStringLiteral("acoustic"), QStringLiteral("(acoustic|原声|unplugged)")},
        {QStringLiteral("instrumental"), QStringLiteral("(instrumental|伴奏|karaoke|纯音乐)")},
        {QStringLiteral("piano"), QStringLiteral("(piano\\s?version|钢琴版)")},
        {QStringLiteral("dj"), QStringLiteral("(dj版|dj\\s?mix)")},
        {QStringLiteral("demo"), QStringLiteral("(demo)")},
        {QStringLiteral("remaster"), QStringLiteral("(remaster(ed)?|重制)")},
    };
    return list;
}

QSet<QString> collectMarkers(const QString &text)
{
    QSet<QString> result;
    if (text.isEmpty()) return result;
    const QString lower = text.toLower();
    for (const auto &marker : versionMarkers()) {
        const QRegularExpression rx(marker.pattern, QRegularExpression::CaseInsensitiveOption);
        if (rx.match(lower).hasMatch()) result.insert(marker.key);
    }
    return result;
}

bool markersCompatible(const QSet<QString> &a, const QSet<QString> &b)
{
    if (a.size() != b.size()) return false;
    for (const auto &key : a) {
        if (!b.contains(key)) return false;
    }
    return true;
}

} // namespace

QVector<Song> MusicSdk::findMusic(const FindMusicRequest &req, int limitPerPlatform)
{
    QVector<Song> result;
    if (req.name.isEmpty()) return result;

    const QString fMusicName = filterStr(req.name);
    const QString fSinger = filterStr(sortSingers(req.singer));
    const double fInterval = parseInterval(req.interval);
    const QSet<QString> originMarkers = collectMarkers(req.name + QLatin1Char(' ') + req.albumName);

    const bool hasIntervalSignal = fInterval > 0;
    const bool hasSingerSignal = !fSinger.isEmpty();
    const bool isShortName = !fMusicName.isEmpty() && fMusicName.length() <= 3;
    // 短名 / 缺时长 / 缺歌手 时强制严格匹配，避免瞎猜
    const bool strictMode = isShortName || !hasIntervalSignal || !hasSingerSignal;

    const QString keyword = (req.name + QLatin1Char(' ') + req.singer).trimmed();

    // 候选池：排除来源平台自身
    QVector<Song> candidates;
    for (const auto &code : m_platformOrder) {
        if (code == req.source) continue;
        auto *src = source(code);
        if (!src) continue;
        const SearchResult r = src->searchSongs(keyword, 1, limitPerPlatform);
        candidates.append(r.songs);
    }

    auto intervalWithinTolerance = [&](const Song &s) {
        if (!hasIntervalSignal) return true;
        const double target = s.duration > 0 ? s.duration : fInterval;
        return std::fabs((target > 0 ? target : fInterval) - fInterval) < 5.0;
    };

    auto nameMatches = [&](const QString &name) {
        const QString fn = filterStr(name);
        if (fn.isEmpty() || fMusicName.isEmpty()) return false;
        return fn == fMusicName || fn.contains(fMusicName) || fMusicName.contains(fn);
    };

    auto singerMatches = [&](const QString &singer) {
        if (!hasSingerSignal) return true;
        const QString fs = filterStr(sortSingers(singer));
        if (fs.isEmpty()) return false;
        return fs == fSinger || fs.contains(fSinger) || fSinger.contains(fs);
    };

    // 分级筛选：等级 0 最严格（全等 + 时长），逐级放宽
    for (int level = 0; level < 3 && result.isEmpty(); ++level) {
        QVector<Song> picked;
        for (const auto &c : candidates) {
            const QString fn = filterStr(c.name);
            const QString fs = filterStr(sortSingers(c.artist));

            bool nameOk = false;
            bool singerOk = false;
            if (level == 0) {
                nameOk = (fn == fMusicName);
                singerOk = hasSingerSignal ? (fs == fSinger) : true;
            } else if (level == 1) {
                nameOk = nameMatches(c.name);
                singerOk = singerMatches(c.artist);
            } else {
                nameOk = nameMatches(c.name);
                singerOk = true; // 放宽歌手
            }
            if (!nameOk || !singerOk) continue;

            const QSet<QString> candidateMarkers =
                collectMarkers(c.name + QLatin1Char(' ') + c.album);
            if (!markersCompatible(originMarkers, candidateMarkers)) continue;

            if (level == 0 && strictMode && !intervalWithinTolerance(c)) continue;
            if (level == 1 && hasIntervalSignal && !intervalWithinTolerance(c)) continue;

            picked.append(c);
        }
        if (!picked.isEmpty()) result = picked;
    }

    return result;
}

} // namespace Muyun
