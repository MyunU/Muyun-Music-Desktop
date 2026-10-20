#pragma once

#include "core/music/MusicSource.h"

#include <QMutex>

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

    /// 歌曲评论（musicu.fcg `music.comment.getcomment`/`GetComment`，bizid=数字 songId）
    CommentPage getComments(const Song &song, int page = 1, int limit = 20,
                            bool hot = true) override;

private:
    HttpResponse musicuRequest(const QVariantMap &body,
                               const QString &referer = QStringLiteral("https://y.qq.com"));
    static QString qualityPrefix(AudioQuality q);
    /// 按 songmid（mid 串）调 musicu `music.pf_song_detail_svr/get_song_detail_yqq`
    /// 解析数字 songId（评论接口 BizId 需要数字 id；lx.songId 缺失时用，参考 lx-music）。
    QString resolveSongId(const QString &songmid);

    // ── 搜索限流熔断 ─────────────────────────────────────────────────────────
    // QQ musicu 搜索对**未签名**请求限流很严：短时间内多次请求后开始返回
    // req.code=2001（歌曲列表恒空，feedbackURL 指向登录页），实测约 10 次即触发，
    // 静置数分钟后自行恢复。搜索联想每敲一个字就查一次 → 极易打满 → 联想整体空白
    // （用户报"搜索联想失效"）。命中后熔断 N 分钟不再发请求，避免越打越封。
    static constexpr qint64 kSearchCooldownMs = 5LL * 60 * 1000;
    bool searchBlocked() const;
    void markSearchBlocked();
    mutable QMutex m_searchMtx;
    mutable qint64 m_searchBlockedUntilMs = 0;   ///< 熔断截止时刻（epoch ms）
};

} // namespace Muyun
