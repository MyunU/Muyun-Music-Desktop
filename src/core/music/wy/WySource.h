#pragma once

#include "core/music/MusicSource.h"

#include <QMutex>

namespace Muyun {

/**
 * @brief 网易云音乐（小芸）音源
 *
 * 对应原工程 src/vendor/lxmusic/renderer/utils/musicSdk/wy。
 * 使用 eapi（AES-ECB + MD5 摘要）调用 interface.music.163.com 接口。
 */
class WySource : public MusicSource
{
public:
    WySource();

    QString code() const override { return QStringLiteral("wy"); }
    QString name() const override { return QStringLiteral("小芸音乐"); }
    Platform platform() const override { return Platform::Netease; }

    SearchResult searchSongs(const QString &keyword, int page = 1,
                             int limit = 30) override;
    QVector<PlaylistSummary> searchPlaylists(const QString &keyword, int page = 1,
                                             int limit = 30) override;
    QVector<AlbumInfo> searchAlbums(const QString &keyword, int page = 1,
                                    int limit = 30) override;
    QStringList searchSuggest(const QString &keyword) override;
    QStringList hotSearchWords() override;
    QVariantList hotSearchSongs() override;
    QVariantList topArtists(int limit = 10) override;

    QString getMusicUrl(const Song &song, AudioQuality quality) override;
    SongLyric getLyric(const Song &song) override;

    QVector<PlaylistSummary> getRecommendPlaylists(int limit = 6) override;
    QVector<ToplistInfo> getToplists() override;
    SearchResult getToplist(const QString &id, int page = 1, int limit = 100) override;
    Playlist getPlaylistDetail(const QString &id) override;
    QVariantList playlistCategories() override;
    QVector<PlaylistSummary> explorePlaylists(const QString &cat, const QString &order,
                                              int page, int limit,
                                              bool *hasMore = nullptr) override;
    CommentPage getComments(const Song &song, int page = 1, int limit = 20,
                            bool hot = true) override;

    /// 设置登录 Cookie（字符串形式，分号分隔）
    void setCookie(const QString &cookie);
    QString cookie() const;

    /// 匿名登录 token（持久化，用于未登录时的接口调用）
    static QString anonymousToken();

private:
    HttpResponse eapiRequest(const QString &path, const QVariantMap &data);
    HttpResponse weapiRequest(const QString &path, const QVariantMap &data);
    /// weapi 的 /api/v3/playlist/detail 无论 n 传多大最多只回前 10 首 tracks，
    /// 但 playlist.trackIds 是全量 id。这里用 /api/v3/song/detail 按 trackIds
    /// 批量补全成完整曲目（含 name/ar/al/dt），供榜单页、歌单详情页使用。
    QVariantList fetchTracksByIds(const QVariantList &trackIds);
    /// 网页兜底：weapi 被风控全量拦死（连续空 body）时，直接抓
    /// https://music.163.com/discover/toplist?id=X 内嵌的 song-list-pre-data
    /// （旧格式：album/artists/duration），解析成 Song 列表。
    QVector<Song> fetchToplistFromWeb(const QString &id, int want);
    void mergeSessionCookies(const QStringList &setCookies) const;
    QVariantMap buildEapiHeader() const;
    QVariantMap processCookies() const;
    static QString serializeCookies(const QVariantMap &cookies);
    static QString eapiParams(const QString &path, const QVariantMap &data);

    QString m_cookie;
    mutable QString m_deviceId;
    mutable QString m_csrf;
    mutable QString m_sessionCookie;   // weapi 预热获得的 NMTID 等
    mutable QMutex m_cookieMtx;        // 首页预览并行拉取时保护上述共享态
};

} // namespace Muyun
