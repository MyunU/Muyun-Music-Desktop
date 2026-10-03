#pragma once

#include "core/music/MusicSource.h"

namespace Muyun {

/**
 * @brief QQ音乐（小秋）音源
 *
 * 对应原工程 src/vendor/lxmusic/renderer/utils/musicSdk/tx。
 * 通过 u.y.qq.com/cgi-bin/musicu.fcg 统一接口完成搜索、播放链接与歌词解析。
 */
class TxSource : public MusicSource
{
public:
    TxSource() = default;

    QString code() const override { return QStringLiteral("tx"); }
    QString name() const override { return QStringLiteral("小秋音乐"); }
    Platform platform() const override { return Platform::QQ; }

    SearchResult searchSongs(const QString &keyword, int page = 1,
                             int limit = 30) override;
    QString getMusicUrl(const Song &song, AudioQuality quality) override;
    SongLyric getLyric(const Song &song) override;
    QVector<PlaylistSummary> getRecommendPlaylists(int limit = 6) override;
    QVector<ToplistInfo> getToplists() override;
    SearchResult getToplist(const QString &id, int page = 1, int limit = 100) override;
    Playlist getPlaylistDetail(const QString &id) override;

    /// 歌单广场：分类（get_all_categories）+ 分类下歌单（无标签=get_playlist_by_tag，有=按类目内容）
    QVariantList playlistCategories() override;
    QVector<PlaylistSummary> explorePlaylists(const QString &cat, const QString &order,
                                              int page, int limit,
                                              bool *hasMore = nullptr) override;

private:
    HttpResponse musicuRequest(const QVariantMap &body,
                               const QString &referer = QStringLiteral("https://y.qq.com"));
    static QString qualityPrefix(AudioQuality q);
};

} // namespace Muyun
