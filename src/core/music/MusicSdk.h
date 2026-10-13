#pragma once

#include "core/Types.h"
#include "core/music/MusicSource.h"

#include <QObject>
#include <QHash>
#include <QMutex>
#include <QReadWriteLock>
#include <atomic>
#include <cstdint>
#include <functional>

namespace Muyun {

class LxScriptEngine;

/**
 * @brief 取源任务取消令牌
 *
 * 语义：一次「为某首歌取音频/歌词/封面」的取源流程绑定一个令牌。
 * 令牌被取消（cancel() 或 bump()）后，MusicSdk 内部所有循环（音质降级链、
 * 跨平台兜底、本平台重搜）都会立刻返回，不再发下一档/下一平台的网络请求。
 *
 * 背景（#39）：切歌/停止/暂停/切后台时，QtConcurrent 里正在跑的
 * sdk->resolveUrl / sdk->findMusic 无法被主线程打断——旧代码要等整个
 * 音质降级链（6 档 × 20s 网络）跑完才回来，新歌曲已经在播，播放条
 * 却显示"加载中"，其他信息都是新歌。用户明确要求「切歌时立即释放，
 * 只有正在播放这首歌时才获取，其他状态立即释放资源」。
 *
 * 用法：调用方创建一个 ResolutionToken（通常是 shared_ptr），把它的
 * 快照快照传入 resolveUrl / findMusic；循环里的每个检查点比较快照与
 * 当前值，一致就继续，不一致就立即返回。调用方在切歌/停止/pause/后台/
 * 失败推进时调用 bump()，让上一轮的 worker 提前退出。
 *
 * ⚠ 单个 MusicSource::getMusicUrl 内的网络请求无法中途打断
 *   （HttpClient::get 是同步阻塞，最长 20s），token 只能让循环尽早退出
 *   ——一次最坏 20s，比原来沿 6 档 + 跨平台 + 重搜全跑完的 2~3 分钟好太多。
 *   这是「不加分外超时强退」的约束（用户明确说不负责）。
 */
class ResolutionToken
{
public:
    ResolutionToken() : m_gen(0) {}
    /// 递增代数：让快照不再匹配，旧 worker 下次检查点立即退出
    void bump() { m_gen.fetch_add(1, std::memory_order_relaxed); }
    /// 当前代数快照（发起 worker 前取，传给 MusicSdk 内部检查点）
    quint64 snapshot() const { return m_gen.load(std::memory_order_relaxed); }
    /// 检查点：token 是否被取消（快照与当前代数不一致）
    bool isStale(quint64 snap) const
    {
        return m_gen.load(std::memory_order_relaxed) != snap;
    }
    /// 便捷检查点：返回 true 表示应退出
    bool check(const ResolutionToken *tok, quint64 snap) const
    {
        if (!tok) return false;   // 无令牌视为永不取消（保持旧行为）
        return tok->isStale(snap);
    }
private:
    std::atomic<quint64> m_gen;
};


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
    /// @param token 可选取消令牌：切歌/停止/pause/后台时上层 bump()，本方法内部
    ///   循环（音质降级链、本平台重搜、跨平台兜底）在检查点发现代数变化立即返回空。
    QString resolveUrl(const Song &song, AudioQuality quality,
                       AudioQuality *actualQuality = nullptr,
                       const ResolutionToken *token = nullptr);
    /// 单一音质解析：不做降级链、不做跨平台兜底，只尝试 (song, quality) 这一组合。
    /// 下载器据此把「先换源、再降音质」的重试序列自己铺开。
    /// @param token 可选取消令牌（同 resolveUrl 语义）
    QString resolveUrlAtQuality(const Song &song, AudioQuality quality,
                                const ResolutionToken *token = nullptr);
    /// 解析歌词
    /// @param token 可选取消令牌：跨平台兜底歌词时逐个候选检查点
    SongLyric resolveLyric(const Song &song, const ResolutionToken *token = nullptr);
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
     * @param token 可选取消令牌：每平台搜索前检查点，取消则返回空列表
     */
    QVector<Song> findMusic(const FindMusicRequest &req, int limitPerPlatform = 25,
                            const ResolutionToken *token = nullptr);

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
    /// 保护 m_lxEngine 指针本身：取源（resolveUrl/lyric/cover）持**读锁**，
    /// loadLxScript 换实例（delete+new）持**写锁**。否则"后台异步切音源正在换引擎"与
    /// "另一 worker 正拿旧引擎跑 musicUrl"并发 → use-after-free。
    /// 注意：QuickJS 内部的 m_mutex 只串行化"同一实例的调用"，不保护实例被替换，故必须再加这一层。
    mutable QReadWriteLock m_lxEngineLock;
    mutable QHash<QString, qint64> m_failedAt;   ///< key → 最近失败时刻（worker 多线程读）
    mutable QMutex m_failMutex;
};

} // namespace Muyun
