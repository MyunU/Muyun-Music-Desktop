#pragma once

#include "core/music/MusicSource.h"

namespace Muyun {

/**
 * @brief 酷狗音乐（小枸）音源
 *
 * 对应原工程 src/vendor/lxmusic/renderer/utils/musicSdk/kg。
 * 搜索走 songsearch.kugou.com，播放链接与歌词走 yy/index.php 的 getdata 接口。
 */
class KgSource : public MusicSource
{
public:
    KgSource() = default;

    QString code() const override { return QStringLiteral("kg"); }
    QString name() const override { return QStringLiteral("小枸音乐"); }
    Platform platform() const override { return Platform::Kugou; }

    SearchResult searchSongs(const QString &keyword, int page = 1,
                             int limit = 30) override;
    QString getMusicUrl(const Song &song, AudioQuality quality) override;
    SongLyric getLyric(const Song &song) override;

    QVector<ToplistInfo> getToplists() override;
    SearchResult getToplist(const QString &id, int page = 1, int limit = 100) override;

    /// 推荐歌单：yueku/v9 getSpecial（t=5 推荐）
    QVector<PlaylistSummary> getRecommendPlaylists(int limit = 6) override;
    /// 歌单详情：yueku/v9/special/single/{id}-5-9999.html 内嵌 global.data
    Playlist getPlaylistDetail(const QString &id) override;

    /// 歌单广场：分类（getSpecial?is_smarty=1 的 hotTag/tagids）+ 列表（t=5推荐/6最热/7最新）
    QVariantList playlistCategories() override;
    QVector<PlaylistSummary> explorePlaylists(const QString &cat, const QString &order,
                                              int page, int limit,
                                              bool *hasMore = nullptr) override;

private:
    /// 按音质挑选对应的 hash
    static QString hashForQuality(const Song &song, AudioQuality quality);
    /// 调用 getdata 接口，返回整段 JSON
    static QVariantMap fetchPlayData(const QString &hash);
    static QString machineId();
};

} // namespace Muyun
