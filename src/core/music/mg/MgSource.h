#pragma once

#include "core/music/MusicSource.h"

#include <QHash>
#include <QMutex>

namespace Muyun {

/**
 * @brief 咪咕音乐（小蜜）音源
 *
 * 对应原工程 src/vendor/lxmusic/renderer/utils/musicSdk/mg。
 * 搜索走 jadeite.migu.cn（带签名），播放链接走 resourceinfo.do。
 */
class MgSource : public MusicSource
{
public:
    MgSource() = default;

    QString code() const override { return QStringLiteral("mg"); }
    QString name() const override { return QStringLiteral("小蜜音乐"); }
    Platform platform() const override { return Platform::Migu; }

    SearchResult searchSongs(const QString &keyword, int page = 1,
                             int limit = 30) override;
    QString getMusicUrl(const Song &song, AudioQuality quality) override;
    SongLyric getLyric(const Song &song) override;

    QVector<ToplistInfo> getToplists() override;
    SearchResult getToplist(const QString &id, int page = 1, int limit = 100) override;
    QVector<PlaylistSummary> getRecommendPlaylists(int limit = 6) override;
    Playlist getPlaylistDetail(const QString &id) override;

    /// 歌单广场：分类（musiclistplaza-taglist）+ 分类下歌单（listbytag；"全部"→广场推荐模板分页）
    QVariantList playlistCategories() override;
    QVector<PlaylistSummary> explorePlaylists(const QString &cat, const QString &order,
                                              int page, int limit,
                                              bool *hasMore = nullptr) override;

    /// 歌曲评论：app.c.nf.migu.cn/MIGUM3.0/user/comment/stack/v1.0，
    /// resourceId=歌曲 songId（lx.songId）。热门=queryType=2，最新=queryType=1（commentId 游标翻页）。参考 lx-music。
    CommentPage getComments(const Song &song, int page = 1, int limit = 20,
                            bool hot = true) override;

private:
    static QString makeSign(const QString &keyword, const QString &timestamp);
    static QVariantMap requestResource(const QString &copyrightId);
    /// 最新评论游标缓存：key = "<songId>@<page>" → 该页最后一条评论的 commentId。
    /// 下一页用前一页末条 id 当游标（与 lx-music 的 lastCommentIds 一致）。
    QHash<QString, QString> m_latestCursor;
    QMutex m_cursorMtx;
};

} // namespace Muyun
