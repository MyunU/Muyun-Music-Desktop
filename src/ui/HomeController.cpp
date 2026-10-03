#include "HomeController.h"

#include "core/music/MusicSdk.h"
#include "core/storage/DocumentStore.h"
#include "core/utils/Format.h"

#include <QtConcurrent>
#include <QFutureWatcher>
#include <QDateTime>
#include <QThreadPool>

namespace Muyun {

namespace {
struct HomeData {
    QVariantList recommend;
    QVariantList toplists;
    qint64 fetchedAt = 0;
};

/// 缓存有效期：7 天
constexpr qint64 kCacheTtlMs = 7LL * 24 * 3600 * 1000;

/// 首页热门排行榜最多显示的榜单卡片数（全部带预览歌）
constexpr int kMaxToplistCards = 12;

/// 歌单广场列表缓存有效期（四-59）：15 分钟内的同参数缓存不再打网络。
/// 用户反馈"歌单广场五个平台的歌单获取速度过慢"——切分类/切平台来回点是最常见的操作，
/// 之前每次都现发请求、期间整页清空显示"加载中…"，体感自然慢。
constexpr qint64 kExploreCacheTtlMs = 15LL * 60 * 1000;

/// 歌单广场缓存条目上限（超过按最旧淘汰，防长时间浏览把内存撑大）
constexpr int kExploreCacheMax = 120;

/// 规范化平台标识：接受音源码(wy/tx/kg/kw/mg) 或 platformId(netease/qq/...)
/// 统一返回音源码；无法识别时返回 fallback
QString normalizeCode(const QString &in, const QString &fallback)
{
    static const QStringList codes = {QStringLiteral("wy"), QStringLiteral("tx"),
                                      QStringLiteral("kg"), QStringLiteral("kw"),
                                      QStringLiteral("mg")};
    if (codes.contains(in)) return in;
    const Platform p = platformFromId(in);
    if (p != Platform::Local) {
        const QString code = platformSourceCode(p);
        if (codes.contains(code)) return code;
    }
    return fallback;
}

/// 从 DocumentStore 取指定平台的缓存条目
QVariantMap readCacheEntry(const QString &code)
{
    QVariantMap doc = DocumentStore::instance()->readAll(QStringLiteral("home-cache"));
    return doc.value(code).toMap();
}

/// 写入指定平台的缓存条目（保留其它平台）
void writeCacheEntry(const QString &code, const QVariantMap &entry)
{
    auto *store = DocumentStore::instance();
    QVariantMap doc = store->readAll(QStringLiteral("home-cache"));
    doc[code] = entry;
    store->writeAll(QStringLiteral("home-cache"), doc);
}

} // namespace

HomeController::HomeController(QObject *parent) : QObject(parent)
{
    m_platform = QStringLiteral("wy");
}

QVariantList HomeController::platformOptions() const
{
    QVariantList out;
    for (const auto &info : MusicSdk::instance()->sourceInfos()) out.append(info);
    return out;
}

void HomeController::setPlatform(const QString &code)
{
    if (m_platform == code) return;
    m_platform = code;
    emit platformChanged();
}

// ---------------------------------------------------------------------------
// 首页缓存
// ---------------------------------------------------------------------------

bool HomeController::tryLoadFromCache(const QString &code)
{
    const QVariantMap entry = readCacheEntry(code);
    if (entry.isEmpty()) return false;
    const qint64 ts = entry.value(QStringLiteral("fetchedAt")).toLongLong();
    if (ts <= 0) return false;
    if (QDateTime::currentMSecsSinceEpoch() - ts > kCacheTtlMs) return false;
    const QVariantList rec = entry.value(QStringLiteral("recommend")).toList();
    QVariantList tops = entry.value(QStringLiteral("toplists")).toList();
    if (tops.size() > kMaxToplistCards)
        tops = tops.mid(0, kMaxToplistCards);   // 兼容旧缓存（曾存全部榜单）
    if (rec.isEmpty() && tops.isEmpty()) return false;

    m_recommend = rec;
    m_toplists = tops;
    m_lastFetchedAt = ts;
    updateLastUpdated(ts);
    emit dataChanged();
    return true;
}

void HomeController::saveToCache(const QString &code, const QVariantList &recommend,
                                 const QVariantList &toplists)
{
    if (recommend.isEmpty() && toplists.isEmpty()) return;   // 空数据不覆盖旧缓存
    QVariantMap entry;
    entry[QStringLiteral("fetchedAt")] = QDateTime::currentMSecsSinceEpoch();
    entry[QStringLiteral("recommend")] = recommend;
    entry[QStringLiteral("toplists")] = toplists;
    writeCacheEntry(code, entry);
    m_lastFetchedAt = entry.value(QStringLiteral("fetchedAt")).toLongLong();
    updateLastUpdated(m_lastFetchedAt);
}

void HomeController::updateLastUpdated(qint64 fetchedAtMs)
{
    m_lastUpdated = fetchedAtMs > 0 ? relativeTime(fetchedAtMs) : QString();
}

QString HomeController::relativeTime(qint64 ms)
{
    const qint64 diff = QDateTime::currentMSecsSinceEpoch() - ms;
    if (diff < 0) return QStringLiteral("刚刚");
    const qint64 sec = diff / 1000;
    if (sec < 60) return QStringLiteral("刚刚");
    if (sec < 3600) return QString::number(sec / 60) + QStringLiteral(" 分钟前");
    if (sec < 86400) return QString::number(sec / 3600) + QStringLiteral(" 小时前");
    return QString::number(sec / 86400) + QStringLiteral(" 天前");
}

void HomeController::loadHome(const QString &code)
{
    const QString target = normalizeCode(code.isEmpty() ? m_platform : code, m_platform);
    setPlatform(target);

    // 1) 先看缓存
    if (tryLoadFromCache(target)) {
        // 缓存新鲜（<7 天）：直接返回，不联网
        return;
    }
    // 2) 缓存缺失/过期：联网拉取。成功后覆盖缓存 + 视图；失败则回落到旧缓存（若有）
    fetchHomeNetwork(target, /*allowOverwriteWhenEmpty=*/false);
}

void HomeController::refreshHome()
{
    // 强制联网；失败时不覆盖视图；成功则更新缓存与视图
    fetchHomeNetwork(m_platform, /*allowOverwriteWhenEmpty=*/false);
}

void HomeController::fetchHomeNetwork(const QString &target, bool allowOverwriteWhenEmpty)
{
    Q_UNUSED(allowOverwriteWhenEmpty);   // 目前语义统一：非空才覆盖；参数保留便于将来差异化
    m_loading = true;
    emit loadingChanged();

    auto *watcher = new QFutureWatcher<HomeData>(this);
    connect(watcher, &QFutureWatcher<HomeData>::finished, this,
            [this, watcher, target]() {
        const HomeData d = watcher->result();
        watcher->deleteLater();
        m_loading = false;
        emit loadingChanged();
        const bool hasData = !d.recommend.isEmpty() || !d.toplists.isEmpty();
        if (m_platform != target) {
            // 用户已切到别的平台：若拉到数据仍然保存缓存（下次进入该平台时可直接读），
            // 但不改视图，避免闪回。
            if (hasData) saveToCache(target, d.recommend, d.toplists);
            return;
        }
        if (hasData) {
            m_recommend = d.recommend;
            m_toplists = d.toplists;
            saveToCache(target, d.recommend, d.toplists);
            emit dataChanged();
        } else {
            // 拉取失败：若有旧缓存（哪怕过期），保留视图并提示；否则空态
            const QVariantMap entry = readCacheEntry(target);
            if (!entry.isEmpty()) {
                const qint64 ts = entry.value(QStringLiteral("fetchedAt")).toLongLong();
                m_recommend = entry.value(QStringLiteral("recommend")).toList();
                m_toplists = entry.value(QStringLiteral("toplists")).toList();
                if (m_toplists.size() > kMaxToplistCards)
                    m_toplists = m_toplists.mid(0, kMaxToplistCards);
                m_lastFetchedAt = ts;
                updateLastUpdated(ts);
                emit dataChanged();
                emit message(QStringLiteral("刷新失败，显示的是 %1 的数据").arg(m_lastUpdated));
            } else {
                m_recommend.clear();
                m_toplists.clear();
                m_lastFetchedAt = 0;
                m_lastUpdated.clear();
                emit dataChanged();
                emit message(QStringLiteral("未能获取到该平台的首页数据"));
            }
        }
    });

    MusicSdk *sdk = MusicSdk::instance();
    watcher->setFuture(QtConcurrent::run([sdk, target]() {
        HomeData d;
        d.fetchedAt = QDateTime::currentMSecsSinceEpoch();

        // 推荐歌单
        for (const auto &p : sdk->getRecommendPlaylists(target, 6))
            d.recommend.append(p.toMap());

        // 热门排行榜：只显示前 12 个榜单卡片（不足 12 有几个显几个），全部带预览歌。
        // 串行拉取（want=3 走 n=10 快速路径，12 个串行很快；曾试并行 blockingMapped
        // 但结果回写不可靠导致预览全空，故回退串行）。
        QVector<ToplistInfo> toplists = sdk->getToplists(target);
        if (toplists.size() > kMaxToplistCards)
            toplists.resize(kMaxToplistCards);
        for (const auto &t : toplists) {
            QVariantMap item = t.toMap();
            const SearchResult r = sdk->getToplist(target, t.id, 1, 3);
            QVariantList songs;
            for (const auto &s : r.songs) {
                songs.append(QVariantMap{
                    {QStringLiteral("name"), s.name},
                    {QStringLiteral("artist"), s.artist},
                });
            }
            item[QStringLiteral("preview")] = songs;
            d.toplists.append(item);
        }
        return d;
    }));
}

void HomeController::loadToplistSongs(const QString &code, const QString &id,
                                      const QString &name, bool autoPlay)
{
    const int reqId = ++m_toplistReqId;
    auto *watcher = new QFutureWatcher<QVariantList>(this);
    connect(watcher, &QFutureWatcher<QVariantList>::finished, this,
            [this, watcher, name, autoPlay, reqId]() {
                const QVariantList songs = watcher->result();
                watcher->deleteLater();
                if (reqId != m_toplistReqId) return;   // 已有更新的请求，丢弃过期结果
                emit toplistSongsReady(songs, name, autoPlay);
            });
    MusicSdk *sdk = MusicSdk::instance();
    watcher->setFuture(QtConcurrent::run([sdk, code, id]() {
        QVariantList out;
        const SearchResult r = sdk->getToplist(code, id, 1, 100);
        for (const auto &s : r.songs) out.append(s.toMap());
        return out;
    }));
}

// ---------------------------------------------------------------------------
// 歌单广场
// ---------------------------------------------------------------------------

void HomeController::loadExploreCategories(const QString &code)
{
    const QString target = code.isEmpty() ? m_platform : code;
    if (!m_exploreCats.isEmpty() && m_exploreCatsCode == target) return;   // 已缓存（按平台）
    auto *watcher = new QFutureWatcher<QVariantList>(this);
    connect(watcher, &QFutureWatcher<QVariantList>::finished, this,
            [this, watcher, target]() {
                const QVariantList cats = watcher->result();
                watcher->deleteLater();
                if (!cats.isEmpty()) {
                    m_exploreCats = cats;
                    m_exploreCatsCode = target;
                    emit exploreCategoriesChanged();
                }
            });
    MusicSdk *sdk = MusicSdk::instance();
    watcher->setFuture(QtConcurrent::run([sdk, target]() {
        return sdk->playlistCategories(target);
    }));
}

QString HomeController::exploreCacheKey(const QString &code, const QString &cat,
                                        const QString &order, int page)
{
    return code + QLatin1Char('|') + cat + QLatin1Char('|') + order
           + QLatin1Char('|') + QString::number(page);
}

void HomeController::storeExploreCache(const QString &key, const QVariantList &list, bool hasMore)
{
    if (list.isEmpty()) return;   // 空结果（多半是失败）不写，别把失败永久缓存下来
    ExploreCacheEntry e;
    e.list = list;
    e.hasMore = hasMore;
    e.fetchedAt = QDateTime::currentMSecsSinceEpoch();
    m_exploreCache.insert(key, e);
    if (m_exploreCache.size() > kExploreCacheMax) {   // 淘汰最旧一条
        QString oldestKey;
        qint64 oldest = -1;
        for (auto it = m_exploreCache.constBegin(); it != m_exploreCache.constEnd(); ++it) {
            if (oldest < 0 || it->fetchedAt < oldest) { oldest = it->fetchedAt; oldestKey = it.key(); }
        }
        if (!oldestKey.isEmpty()) m_exploreCache.remove(oldestKey);
    }
}

void HomeController::loadExplorePlaylists(const QString &code, const QString &cat,
                                          const QString &order, int page)
{
    const QString target = code.isEmpty() ? m_platform : code;
    const QString key = exploreCacheKey(target, cat, order, page);

    // ① 缓存命中 → **立刻**把上次结果发出去（不再闪"加载中"、不再等网络）；
    //    还新鲜就直接返回，过期则继续往下做后台刷新（四-59）
    const auto cached = m_exploreCache.constFind(key);
    if (cached != m_exploreCache.constEnd()) {
        emit exploreReady(cached->list, cat, page, cached->hasMore);
        if (QDateTime::currentMSecsSinceEpoch() - cached->fetchedAt < kExploreCacheTtlMs) return;
    }

    const int reqId = ++m_exploreReqId;
    auto *watcher = new QFutureWatcher<QPair<QVariantList, bool>>(this);
    connect(watcher, &QFutureWatcher<QPair<QVariantList, bool>>::finished, this,
            [this, watcher, cat, page, reqId, key]() {
                const auto result = watcher->result();
                watcher->deleteLater();
                if (reqId != m_exploreReqId) return;
                if (result.first.isEmpty()) {
                    // 已有缓存（上面发过了）就别再报错打断用户
                    if (!m_exploreCache.contains(key))
                        emit exploreFailed(QStringLiteral("歌单加载失败"));
                    return;
                }
                storeExploreCache(key, result.first, result.second);
                emit exploreReady(result.first, cat, page, result.second);
            });
    MusicSdk *sdk = MusicSdk::instance();
    watcher->setFuture(QtConcurrent::run([sdk, target, cat, order, page]() {
        bool hasMore = false;
        QVariantList out;
        const QVector<PlaylistSummary> list =
            sdk->explorePlaylists(target, cat, order, page, 30, &hasMore);
        for (const auto &p : list) out.append(p.toMap());
        return qMakePair(out, hasMore);
    }));
}

void HomeController::warmExplorePlatforms(const QString &cat, const QString &order)
{
    // 并行预热五个平台的首屏：**只写缓存、不发 exploreReady**（正在看的列表不能被覆盖）。
    // 用户切平台时 loadExplorePlaylists 直接命中缓存秒出。
    static const QStringList codes = {QStringLiteral("wy"), QStringLiteral("tx"),
                                      QStringLiteral("kg"), QStringLiteral("kw"),
                                      QStringLiteral("mg")};
    for (const QString &c : codes) {
        const QString key = exploreCacheKey(c, cat, order, 1);
        const auto it = m_exploreCache.constFind(key);
        if (it != m_exploreCache.constEnd()
            && QDateTime::currentMSecsSinceEpoch() - it->fetchedAt < kExploreCacheTtlMs)
            continue;   // 已有新鲜缓存 → 不重复打网络
        MusicSdk *sdk = MusicSdk::instance();
        auto *watcher = new QFutureWatcher<QPair<QVariantList, bool>>(this);
        connect(watcher, &QFutureWatcher<QPair<QVariantList, bool>>::finished, this,
                [this, watcher, key]() {
                    const auto r = watcher->result();
                    watcher->deleteLater();
                    storeExploreCache(key, r.first, r.second);
                });
        watcher->setFuture(QtConcurrent::run([sdk, c, cat, order]() {
            bool hasMore = false;
            QVariantList out;
            const QVector<PlaylistSummary> list =
                sdk->explorePlaylists(c, cat, order, 1, 30, &hasMore);
            for (const auto &p : list) out.append(p.toMap());
            return qMakePair(out, hasMore);
        }));
    }
}

void HomeController::loadPlaylistSongs(const QString &code, const QString &id,
                                       const QString &name)
{
    const int reqId = ++m_plDetailReqId;
    const QString target = normalizeCode(code, m_platform);
    auto *watcher = new QFutureWatcher<QVariantList>(this);
    connect(watcher, &QFutureWatcher<QVariantList>::finished, this,
            [this, watcher, name, id, reqId]() {
                const QVariantList songs = watcher->result();
                watcher->deleteLater();
                if (reqId != m_plDetailReqId) return;
                if (songs.isEmpty()) {
                    emit exploreFailed(QStringLiteral("歌单加载失败或为空"));
                    return;
                }
                emit playlistSongsReady(songs, name, id);
            });
    MusicSdk *sdk = MusicSdk::instance();
    watcher->setFuture(QtConcurrent::run([sdk, target, id]() {
        QVariantList out;
        const Playlist pl = sdk->getPlaylistDetail(target, id);
        for (const auto &s : pl.songs) out.append(s.toMap());
        return out;
    }));
}

} // namespace Muyun

