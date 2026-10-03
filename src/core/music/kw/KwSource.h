#pragma once

#include "core/music/MusicSource.h"

namespace Muyun {

/**
 * @brief 酷我音乐（小蜗）音源
 *
 * 对应原工程 src/vendor/lxmusic/renderer/utils/musicSdk/kw。
 * 搜索走 search.kuwo.cn/r.s，播放链接走 antiserver 转换接口。
 */
class KwSource : public MusicSource
{
public:
    KwSource() = default;

    QString code() const override { return QStringLiteral("kw"); }
    QString name() const override { return QStringLiteral("小蜗音乐"); }
    Platform platform() const override { return Platform::Kuwo; }

    SearchResult searchSongs(const QString &keyword, int page = 1,
                             int limit = 30) override;
    QString getMusicUrl(const Song &song, AudioQuality quality) override;
    SongLyric getLyric(const Song &song) override;

    QVector<ToplistInfo> getToplists() override;
    SearchResult getToplist(const QString &id, int page = 1, int limit = 100) override;
    QVector<PlaylistSummary> getRecommendPlaylists(int limit = 6) override;
    Playlist getPlaylistDetail(const QString &id) override;

    /// 歌单广场：分类（getTagList；标签名→"tagId-type" 在 explorePlaylists 内按需解析，无共享状态）
    QVariantList playlistCategories() override;
    /// 歌单广场：全部→getRcmPlayList（带分页）；具体标签→getTagPlayList（type 10000）
    QVector<PlaylistSummary> explorePlaylists(const QString &cat, const QString &order,
                                              int page, int limit,
                                              bool *hasMore = nullptr) override;

private:
    static QString decodeHtml(const QString &text);
    static QString normalizeCover(const QString &path);
    /// 单引号 JSON -> 标准 JSON
    static QByteArray fixJson(const QString &text);
    /// getRcmPlayList / getTagPlayList 共用的 data.data[] 解析
    static QVector<PlaylistSummary> parseTagList(const QVariantList &list);
};

} // namespace Muyun
