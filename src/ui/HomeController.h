#pragma once

#include "core/Types.h"

#include <QObject>
#include <QVariantList>
#include <QHash>

namespace Muyun {

/**
 * @brief 首页控制器（暴露给 QML）
 *
 * 负责加载「推荐歌单」与「热门排行榜」（含每个榜单的前 3 首预览）。
 *
 * 缓存策略（每平台独立）：
 * - DocumentStore "home-cache" 文档保存 { code → { fetchedAt, recommend, toplists } }；
 * - loadHome() 优先使用 7 天内的缓存，命中直接返回不联网；
 * - refreshHome() 强制联网，成功才覆盖缓存，失败保留旧数据 + 提示；
 * - 无有效缓存时 loadHome 等价于 refreshHome（拉取但不覆盖为空数据）。
 */
class HomeController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QVariantList recommendPlaylists READ recommendPlaylists NOTIFY dataChanged)
    Q_PROPERTY(QVariantList toplists READ toplists NOTIFY dataChanged)
    Q_PROPERTY(QVariantList playlistCategories READ playlistCategories NOTIFY exploreCategoriesChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString platform READ platform NOTIFY platformChanged)
    /// 上次更新的人类可读文本（"5 分钟前" / "2 天前" / ""）
    Q_PROPERTY(QString lastUpdated READ lastUpdated NOTIFY dataChanged)

public:
    explicit HomeController(QObject *parent = nullptr);

    QVariantList recommendPlaylists() const { return m_recommend; }
    QVariantList toplists() const { return m_toplists; }
    bool loading() const { return m_loading; }
    QString platform() const { return m_platform; }
    QString lastUpdated() const { return m_lastUpdated; }

    /// 加载指定平台（音源码 wy/tx/kg/kw/mg）的首页数据；有 <7 天缓存则直接返回不联网
    Q_INVOKABLE void loadHome(const QString &code);
    /// 强制刷新当前平台（联网；成功才覆盖缓存与视图，失败保留旧数据并提示）
    Q_INVOKABLE void refreshHome();
    Q_INVOKABLE void setPlatform(const QString &code);
    /// 平台列表（含显示名）
    Q_INVOKABLE QVariantList platformOptions() const;
    /// 加载某个榜单的完整歌曲列表（autoPlay=true 时前端拿到后直接播放）
    Q_INVOKABLE void loadToplistSongs(const QString &code, const QString &id,
                                      const QString &name, bool autoPlay = false);

    /// 歌单广场：分类标签（异步加载完成后经 exploreCategoriesChanged 通知）
    QVariantList playlistCategories() const { return m_exploreCats; }
    /// 歌单广场：拉取分类（一次即可，缓存复用）
    Q_INVOKABLE void loadExploreCategories(const QString &code);
    /// 歌单广场：按分类加载歌单列表（page 从 1 开始）
    Q_INVOKABLE void loadExplorePlaylists(const QString &code, const QString &cat,
                                          const QString &order, int page);
    /**
     * @brief 歌单广场：**并行预热五个平台的首屏**（默认"全部/hot/第1页"）
     *
     * 只填缓存、不发 exploreReady 信号（不能污染当前正在看的列表）。
     * 用户来切平台时直接命中缓存秒出，不用等一轮网络往返。四-59 新增：
     * 用户反馈"歌单广场五个平台的歌单获取速度过慢"。
     */
    Q_INVOKABLE void warmExplorePlatforms(const QString &cat = QStringLiteral("全部"),
                                          const QString &order = QStringLiteral("hot"));
    /// 打开在线歌单：加载其歌曲列表
    Q_INVOKABLE void loadPlaylistSongs(const QString &code, const QString &id,
                                       const QString &name);

signals:
    void dataChanged();
    void exploreCategoriesChanged();
    void toplistSongsReady(const QVariantList &songs, const QString &name, bool autoPlay);
    void exploreReady(const QVariantList &playlists, const QString &cat,
                      int page, bool hasMore);
    void exploreFailed(const QString &message);
    void playlistSongsReady(const QVariantList &songs, const QString &name,
                            const QString &id);
    void loadingChanged();
    void platformChanged();
    void message(const QString &text);   // 走全局 toast（刷新失败等）

private:
    /// 从缓存尝试填 m_recommend/m_toplists；成功返回 true 并更新 m_lastFetchedAt
    bool tryLoadFromCache(const QString &code);
    /// 保存到缓存（recommend/toplists 至少一项非空才写）
    void saveToCache(const QString &code, const QVariantList &recommend,
                     const QVariantList &toplists);
    /// 触发一次网络拉取（不检查缓存）；onSuccess=true 才覆盖视图与缓存
    void fetchHomeNetwork(const QString &target, bool allowOverwriteWhenEmpty);
    void updateLastUpdated(qint64 fetchedAtMs);
    static QString relativeTime(qint64 ms);

    QVariantList m_recommend;
    QVariantList m_toplists;
    QVariantList m_exploreCats;
    QString m_exploreCatsCode;   ///< 分类缓存所属平台（切平台需重取）

    /// 歌单广场列表缓存（四-59）：key = "平台|分类|排序|页"。
    /// 命中就把上次结果**立刻**发出去（不再闪"加载中"），15 分钟内不再打网络。
    struct ExploreCacheEntry {
        QVariantList list;
        bool hasMore = false;
        qint64 fetchedAt = 0;
    };
    QHash<QString, ExploreCacheEntry> m_exploreCache;
    static QString exploreCacheKey(const QString &code, const QString &cat,
                                   const QString &order, int page);
    /// 把一次拉取结果写进缓存（空结果不写，避免把失败永久缓存下来）
    void storeExploreCache(const QString &key, const QVariantList &list, bool hasMore);
    bool m_loading = false;
    QString m_platform;
    QString m_lastUpdated;
    qint64 m_lastFetchedAt = 0;
    int m_toplistReqId = 0;   // 榜单请求序号（回调时丢弃过期结果）
    int m_exploreReqId = 0;
    int m_plDetailReqId = 0;
};

} // namespace Muyun

