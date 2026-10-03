#pragma once

#include "core/music/MusicSource.h"

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

private:
    static QString makeSign(const QString &keyword, const QString &timestamp);
    static QVariantMap requestResource(const QString &copyrightId);
};

} // namespace Muyun
