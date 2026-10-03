#pragma once

#include "core/Types.h"
#include "core/network/HttpClient.h"

#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

namespace Muyun {

/**
 * @brief 在线音源接口（对应原工程 lxmusic musicSdk 的各平台 facade）
 *
 * 所有方法都是同步阻塞的，调用方需在工作线程执行，避免卡住 UI。
 */
class MusicSource
{
public:
    virtual ~MusicSource() = default;

    /// 音源码：wy / tx / kg / kw / mg
    virtual QString code() const = 0;
    /// 平台显示名
    virtual QString name() const = 0;
    virtual Platform platform() const = 0;

    /// 搜索歌曲
    virtual SearchResult searchSongs(const QString &keyword, int page = 1,
                                     int limit = 30) = 0;
    /// 搜索歌单
    virtual QVector<PlaylistSummary> searchPlaylists(const QString &keyword,
                                                    int page = 1, int limit = 30)
    { Q_UNUSED(keyword) Q_UNUSED(page) Q_UNUSED(limit) return {}; }
    /// 搜索专辑
    virtual QVector<AlbumInfo> searchAlbums(const QString &keyword,
                                            int page = 1, int limit = 30)
    { Q_UNUSED(keyword) Q_UNUSED(page) Q_UNUSED(limit) return {}; }

    /// 搜索联想词（输入时实时提示）
    virtual QStringList searchSuggest(const QString &keyword)
    { Q_UNUSED(keyword) return {}; }

    /// 热门搜索词（搜索页热搜榜）
    virtual QStringList hotSearchWords()
    { return {}; }

    /// 热搜歌曲：[{word, name, artist}, ...]（word 为可直接搜索的关键词）
    virtual QVariantList hotSearchSongs()
    { return {}; }

    /// 热搜歌手：[{id, name, avatar}, ...]
    virtual QVariantList topArtists(int limit = 10)
    { Q_UNUSED(limit) return {}; }

    /// 获取播放链接（按指定音质，失败返回空）
    virtual QString getMusicUrl(const Song &song, AudioQuality quality) = 0;

    /// 获取歌词
    virtual SongLyric getLyric(const Song &song) = 0;

    /// 获取推荐歌单（首页用）
    virtual QVector<PlaylistSummary> getRecommendPlaylists(int limit = 6)
    { Q_UNUSED(limit) return {}; }

    /// 获取榜单列表
    virtual QVector<ToplistInfo> getToplists() { return {}; }
    /// 获取榜单歌曲
    virtual SearchResult getToplist(const QString &id, int page = 1, int limit = 100)
    { Q_UNUSED(id) Q_UNUSED(page) Q_UNUSED(limit) return {}; }
    /// 获取歌单详情
    virtual Playlist getPlaylistDetail(const QString &id) { Q_UNUSED(id) return {}; }

    /// 歌单广场：分类（[{group, tags:[..]}, ...]）
    virtual QVariantList playlistCategories() { return {}; }
    /// 歌单广场：分类下歌单（order: hot/new；hasMore 输出是否有下一页）
    virtual QVector<PlaylistSummary> explorePlaylists(const QString &cat,
                                                      const QString &order,
                                                      int page, int limit,
                                                      bool *hasMore = nullptr)
    { Q_UNUSED(cat) Q_UNUSED(order) Q_UNUSED(page) Q_UNUSED(limit)
      if (hasMore) *hasMore = false; return {}; }
    /// 获取评论
    virtual CommentPage getComments(const Song &song, int page = 1, int limit = 20,
                                    bool hot = true)
    { Q_UNUSED(song) Q_UNUSED(page) Q_UNUSED(limit) Q_UNUSED(hot) return {}; }

    /// 歌曲详情（补全 lx 元信息）
    virtual bool fillSongDetail(Song &song) { Q_UNUSED(song) return false; }
};

/// 结果回调（在工作线程触发，调用方自行切回 UI 线程）
template <typename T>
using SourceCallback = std::function<void(T)>;

} // namespace Muyun
