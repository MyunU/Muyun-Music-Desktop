#pragma once

#include "core/Types.h"
#include "core/localmusic/LocalMusicScanner.h"

#include <QObject>
#include <QVariantList>

namespace Muyun {

/**
 * @brief 音乐库控制器（暴露给 QML）
 *
 * 管理本地音乐、收藏、最近播放与自建歌单。
 */
class LibraryController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QVariantList localSongs READ localSongs NOTIFY localSongsChanged)
    Q_PROPERTY(int localCount READ localCount NOTIFY localSongsChanged)
    Q_PROPERTY(QVariantList favorites READ favorites NOTIFY favoritesChanged)
    Q_PROPERTY(QVariantList localFavorites READ localFavorites NOTIFY localFavoritesChanged)
    /// 收藏版本号：每次收藏变化自增。QML 的 isFavorite() 是方法调用，
    /// 必须绑定这个属性才能在收藏变化时重新求值。
    Q_PROPERTY(int favoritesVersion READ favoritesVersion NOTIFY favoritesChanged)
    Q_PROPERTY(QVariantList recent READ recent NOTIFY recentChanged)
    Q_PROPERTY(QVariantList playlists READ playlists NOTIFY playlistsChanged)
    Q_PROPERTY(QStringList folders READ folders NOTIFY foldersChanged)
    Q_PROPERTY(QString lastScannedAt READ lastScannedAt NOTIFY localSongsChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)

public:
    explicit LibraryController(QObject *parent = nullptr);

    QVariantList localSongs() const;
    int localCount() const;
    QVariantList favorites() const;
    QVariantList localFavorites() const;
    QVariantList recent() const;
    QVariantList playlists() const;
    QStringList folders() const { return m_scanner->folders(); }
    QString lastScannedAt() const { return m_scanner->lastScannedAt(); }
    bool scanning() const { return m_scanner->isScanning(); }
    int favoritesVersion() const { return m_favoritesVersion; }

    // ---- 本地音乐 ----
    Q_INVOKABLE void addFolder(const QString &path);
    Q_INVOKABLE void removeFolder(const QString &path);
    Q_INVOKABLE void rescan();

    /// 删除本地歌曲：deleteFile=true 删除磁盘文件；false 仅从列表移除（扫描永久跳过）。
    /// 同步清理收藏/最近播放/歌单中的该曲。返回 false 表示文件删除失败（占用/权限）。
    Q_INVOKABLE bool removeLocalSong(const QVariantMap &song, bool deleteFile);

    /// 播放器实测时长回写（VBR 估算误差自校准）
    void fixLocalDuration(const QString &localPath, int durationSec);

    /// 标签读取优先级（转发给 LocalMusicScanner）
    using TagPriority = LocalMusicScanner::TagPriority;
    void setTagPriority(TagPriority priority);
    TagPriority tagPriority() const;

    // ---- 收藏 ----
    Q_INVOKABLE bool isFavorite(const QVariantMap &song) const;
    Q_INVOKABLE void toggleFavorite(const QVariantMap &song);
    Q_INVOKABLE void removeFavorite(const QString &identityKey);
    /// 歌曲稳定身份 key（QML 批量选择用它当选中项的标识）
    Q_INVOKABLE QString identityOf(const QVariantMap &song) const;
    /// 批量取消收藏：一次保存、一次通知（逐条删会让列表反复重置、滚动乱跳）
    Q_INVOKABLE int removeFavorites(const QStringList &identityKeys);

    // ---- 最近播放 ----
    Q_INVOKABLE void recordPlay(const QVariantMap &song);
    Q_INVOKABLE void clearRecent();

    // ---- 歌单 ----
    Q_INVOKABLE QString createPlaylist(const QString &name, const QString &description = QString());
    Q_INVOKABLE void deletePlaylist(const QString &id);
    Q_INVOKABLE void renamePlaylist(const QString &id, const QString &name);
    Q_INVOKABLE void addToPlaylist(const QString &playlistId, const QVariantMap &song);
    Q_INVOKABLE void removeFromPlaylist(const QString &playlistId, int index);
    /// 批量从歌单移除歌曲（按 identityKey，不按下标——下标会随删除漂移）；一次保存一次通知
    Q_INVOKABLE int removeSongsFromPlaylist(const QString &playlistId, const QStringList &identityKeys);
    Q_INVOKABLE QVariantList playlistSongs(const QString &playlistId) const;

    // ---- 收藏在线歌单（广场/推荐整单收进侧栏「歌单」列表）----
    /// 该在线歌单是否已收藏。platform 传广场给的 platformId 或音源码("wy") 都认，
    /// 内部统一成 platformId 再比（两套串混用会把同一张歌单收藏两遍）。
    Q_INVOKABLE bool isPlaylistCollected(const QString &platform, const QString &sourceId) const;
    /// 已收藏则返回对应的本地歌单 id，未收藏返回空串
    Q_INVOKABLE QString collectedPlaylistId(const QString &platform, const QString &sourceId) const;
    /// 收藏/取消收藏整单（已收藏时再点=取消）。songs 传当前已加载到的歌曲。
    /// 返回 true=已收藏，false=已取消（或参数不合法）
    Q_INVOKABLE bool toggleCollectPlaylist(const QVariantMap &playlist, const QVariantList &songs);

    // ---- 工具 ----
    Q_INVOKABLE QString platformName(const QString &platformId) const;
    Q_INVOKABLE QString formatDuration(double seconds) const;
    Q_INVOKABLE QString formatPlayCount(qint64 count) const;

    // ---- 局域网同步（只同步在线收藏/歌单，本地歌曲不参与）----
    /// 导出在线库：{deviceName, favorites:[songMap], playlists:[{name,description,songs:[...]}]}
    Q_INVOKABLE QVariantMap exportOnlineLibrary() const;
    /// 合并导入对端在线库（按 identityKey 去重），返回新增条目数
    Q_INVOKABLE int importOnlineLibrary(const QVariantMap &data);

    // ---- 洛雪(lx-music)同步适配：由 LxSyncServer 调用 ----
    /// 把当前"在线收藏(loveList)+自建歌单(userList)"序列化为洛雪 ListData。
    /// defaultList 恒空（暮云无试听列表概念），本地歌曲不参与。
    QJsonObject buildLxListData() const;
    /// 用洛雪 ListData 覆盖在线收藏 + 同步自建歌单（缺失即删、多余即增、有则改），
    /// 本地收藏/本地歌曲原样保留。仅在同步合并流程末尾调用。
    void applyLxListData(const QJsonObject &listData);

signals:
    void localSongsChanged();
    void favoritesChanged();
    /// 某本地文件被删除/移除 → 通知播放器把它从播放队列剔除（避免切歌撞空文件）
    void localSongRemoved(const QString &localPath);
    void localFavoritesChanged();
    void recentChanged();
    void playlistsChanged();
    void foldersChanged();
    void scanningChanged();

private:
    void load();
    void save();
    /// 自增版本号并发出收藏变化通知
    void bumpFavorites();

    LocalMusicScanner *m_scanner = nullptr;
    QVector<Song> m_favorites;
    QVector<Song> m_localFavorites;
    QVector<Song> m_recent;
    QVector<Playlist> m_playlists;
    int m_favoritesVersion = 0;
};

} // namespace Muyun
