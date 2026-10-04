#pragma once

#include "core/Types.h"
#include "core/music/MusicSource.h"

#include <QObject>
#include <QHash>
#include <QMutex>
#include <functional>

namespace Muyun {

class LxScriptEngine;

/// 跨平台找歌请求（换源时用）
struct FindMusicRequest {
    QString name;
    QString singer;
    QString albumName;
    QString interval;   // "03:45" 形式
    QString source;     // 需要排除的源（原平台音源码）
};

/**
 * @brief 音源聚合器
 *
 * 对应原工程 src/vendor/lxmusic/renderer/utils/musicSdk/index.js，
 * 汇总 wy/tx/kg/kw/mg 五个平台，并提供跨平台找歌（换源候选匹配）。
 */
class MusicSdk : public QObject
{
    Q_OBJECT
public:
    explicit MusicSdk(QObject *parent = nullptr);
    ~MusicSdk() override;

    static MusicSdk *instance();

    /// 全部音源码（wy/tx/kg/kw/mg）
    QStringList sourceCodes() const;
    /// 音源信息（对外展示）
    QVariantList sourceInfos() const;

    MusicSource *source(const QString &code) const;
    MusicSource *source(Platform p) const;

    /// 单平台搜索
    SearchResult search(const QString &code, const QString &keyword,
                        int page = 1, int limit = 30);
    /// 全平台聚合搜索（每平台取 limit 条后交错合并）
    SearchResult searchAll(const QString &keyword, int page = 1, int limit = 30);

    /// 解析播放链接（自动按降级链回退）
    QString resolveUrl(const Song &song, AudioQuality quality,
                       AudioQuality *actualQuality = nullptr);
    /// 单一音质解析：不做降级链、不做跨平台兜底，只尝试 (song, quality) 这一组合。
    /// 下载器据此把「先换源、再降音质」的重试序列自己铺开。
    QString resolveUrlAtQuality(const Song &song, AudioQuality quality);
    /// 解析歌词
    SongLyric resolveLyric(const Song &song);
    /// 解析封面：本地有 cover 直接用；为空且带 LX 元信息时问脚本（local 源 pic action）
    QString resolveCover(const Song &song);

    /// 获取推荐歌单（首页）
    QVector<PlaylistSummary> getRecommendPlaylists(const QString &code, int limit = 6);

    /// 获取榜单
    QVector<ToplistInfo> getToplists(const QString &code);
    SearchResult getToplist(const QString &code, const QString &id, int page = 1,
                            int limit = 100);
    /// 歌单详情
    Playlist getPlaylistDetail(const QString &code, const QString &id);
    /// 歌单广场：分类 + 分类下歌单
    QVariantList playlistCategories(const QString &code);
    QVector<PlaylistSummary> explorePlaylists(const QString &code, const QString &cat,
                                              const QString &order, int page, int limit,
                                              bool *hasMore = nullptr);
    /// 评论
    CommentPage getComments(const Song &song, int page = 1, int limit = 20, bool hot = true);

    /**
     * @brief 跨平台找歌
     *
     * 给定歌曲信息，在其它平台搜索并筛选出可信的替换候选。
     * 筛选规则与原工程一致：过滤字符串、版本标记对齐、时长容差 5 秒、
     * 短名（<=3 字符）走严格匹配。
     */
    QVector<Song> findMusic(const FindMusicRequest &req, int limitPerPlatform = 25);

    /// 设置智能换源时的平台顺序（默认 kw,kg,mg,wy,tx）
    void setPlatformOrder(const QStringList &order);
    QStringList platformOrder() const { return m_platformOrder; }

    /**
     * @brief 加载并启用 LX 自定义音源脚本
     *
     * 脚本通过 lx.on(lx.EVENT_NAMES.request, handler) 注册请求处理器，
     * handler 收到 { source, action, info } 返回播放链接。
     * 加载成功后，resolveUrl 会优先用脚本解析（当歌曲带 lx 元信息时）。
     * @param scriptPath 脚本文件路径；传空字符串表示卸载脚本
     * @param error      失败原因（可选）
     * @return 是否加载成功
     */
    bool loadLxScript(const QString &scriptPath, QString *error = nullptr);
    bool hasLxScript() const;
    /// 当前脚本名（未加载返回空）
    QString lxScriptName() const;
    /// 脚本上报的更新推送信息（send('updateAlert')），无更新返回空 Map
    QVariantMap lxUpdateAlert() const;
    /// 注册更新提醒回调（转发到 LxScriptEngine，脚本异步 send 时触发）
    void setUpdateAlertCallback(std::function<void(const QVariantMap &)> cb);

private:
    void registerSources();
    /// 注册一个音源实现（所有权归 MusicSdk）
    void registerSource(MusicSource *source);
    static QString filterStr(const QString &s);
    static QString sortSingers(const QString &singer);
    static double parseInterval(const QString &interval);

    // #10 性能：音源解析失败记忆——同一源同一档 60 秒内不再重复打网络
    // （源挂了时，旧实现沿降级链每档真请求一次，切歌/重播都要重吃一遍）
    bool recentlyFailed(const QString &key) const;
    void rememberFailure(const QString &key);
    static constexpr qint64 kFailRememberMs = 60LL * 1000;

    QHash<QString, MusicSource *> m_sources;
    QStringList m_platformOrder;
    LxScriptEngine *m_lxEngine = nullptr;
    mutable QHash<QString, qint64> m_failedAt;   ///< key → 最近失败时刻（worker 多线程读）
    mutable QMutex m_failMutex;
};

} // namespace Muyun
