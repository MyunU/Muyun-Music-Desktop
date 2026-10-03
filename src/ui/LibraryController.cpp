#include "LibraryController.h"

#include "core/storage/DocumentStore.h"
#include "core/utils/Format.h"
#include "core/sync/LxMapping.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QUuid>
#include <QFile>
#include <QThread>
#include <QSysInfo>
#include <QSet>

namespace Muyun {

LibraryController::LibraryController(QObject *parent) : QObject(parent)
{
    m_scanner = new LocalMusicScanner(this);
    connect(m_scanner, &LocalMusicScanner::scanStarted, this,
            [this]() { emit scanningChanged(); });
    connect(m_scanner, &LocalMusicScanner::scanFinished, this,
            [this]() {
                emit scanningChanged();
                emit localSongsChanged();
            });
    connect(m_scanner, &LocalMusicScanner::folderAdded, this,
            [this]() { emit foldersChanged(); });
    connect(m_scanner, &LocalMusicScanner::folderRemoved, this,
            [this]() { emit foldersChanged(); });
    load();

    // 四-59 一次性迁移：时长解析修复（帧头遍历 + OGG granule）之前入库的时长是错的，
    // 而列表**直接读存档**、不会自己变对（旧行为：只有播放/手动"重新扫描"才纠正）。
    // 首次启动静默重扫一遍纠正；标记先落盘——万一扫描出问题也不会每次启动都重扫。
    if (m_scanner->needsMediaDurationFix()) {
        m_scanner->markMediaDurationFixed();
        m_scanner->scanAsync();
    }
}

// ---------------------------------------------------------------------------
// 读取
// ---------------------------------------------------------------------------

QVariantList LibraryController::localSongs() const
{
    QVariantList out;
    for (const auto &s : m_scanner->songs()) out.append(s.toMap());
    return out;
}

int LibraryController::localCount() const { return m_scanner->songCount(); }

QVariantList LibraryController::favorites() const
{
    QVariantList out;
    for (const auto &s : m_favorites) out.append(s.toMap());
    return out;
}

QVariantList LibraryController::localFavorites() const
{
    // 收藏已合并为单一列表，此属性保留兼容（与 favorites 同源）
    QVariantList out;
    for (const auto &s : m_favorites) out.append(s.toMap());
    return out;
}

QVariantList LibraryController::recent() const
{
    QVariantList out;
    for (const auto &s : m_recent) out.append(s.toMap());
    return out;
}

QVariantList LibraryController::playlists() const
{
    QVariantList out;
    for (const auto &p : m_playlists) out.append(p.toMap());
    return out;
}

// ---------------------------------------------------------------------------
// 本地音乐
// ---------------------------------------------------------------------------

void LibraryController::addFolder(const QString &path)
{
    if (path.isEmpty()) return;
    m_scanner->addFolder(path);
    m_scanner->scanFolderAsync(path);
}

void LibraryController::removeFolder(const QString &path)
{
    m_scanner->removeFolder(path);
    emit localSongsChanged();
}

void LibraryController::rescan() { m_scanner->scanAsync(); }

void LibraryController::fixLocalDuration(const QString &localPath, int durationSec)
{
    if (localPath.isEmpty() || durationSec <= 0) return;
    bool any = m_scanner->updateDurationByPath(localPath, durationSec);
    const auto fix = [&](QVector<Song> &list) {
        for (auto &s : list) {
            if (s.localPath == localPath && s.duration != durationSec) {
                s.duration = durationSec; any = true;
            }
        }
    };
    fix(m_favorites);
    fix(m_recent);
    for (auto &pl : m_playlists) fix(pl.songs);
    if (any) {
        save();
        emit localSongsChanged();
        emit favoritesChanged();
        emit recentChanged();
        emit playlistsChanged();
    }
}

bool LibraryController::removeLocalSong(const QVariantMap &song, bool deleteFile)
{
    const QString path = song.value(QStringLiteral("localPath")).toString();
    if (path.isEmpty()) return false;

    if (deleteFile) {
        // 正在播放时播放器句柄可能未立即释放 → 重试几次（调用方已先 player.stop()）
        bool removed = !QFile::exists(path);
        for (int i = 0; i < 6 && !removed; ++i) {
            if (QFile::remove(path)) { removed = true; break; }
            QThread::msleep(120);
        }
        if (!removed) return false;   // 仍占用 → 交给调用方提示
    } else {
        m_scanner->ignorePath(path);
    }
    m_scanner->removeSongByPath(path);

    // 文件已删/已隐藏 → 收藏、最近播放、歌单里的该曲一并清理
    const QString key = Song::fromMap(song).identityKey();
    bool libChanged = false;
    for (int i = 0; i < m_favorites.size(); ++i) {
        if (m_favorites.at(i).identityKey() == key) {
            m_favorites.removeAt(i); libChanged = true; bumpFavorites(); break;
        }
    }
    for (int i = 0; i < m_recent.size(); ++i) {
        if (m_recent.at(i).identityKey() == key) {
            m_recent.removeAt(i); libChanged = true; emit recentChanged(); break;
        }
    }
    for (auto &pl : m_playlists) {
        for (int i = 0; i < pl.songs.size(); ++i) {
            if (pl.songs.at(i).identityKey() == key) {
                pl.songs.removeAt(i); libChanged = true; emit playlistsChanged(); break;
            }
        }
    }
    if (libChanged) save();
    emit localSongRemoved(path);
    emit localSongsChanged();
    return true;
}

void LibraryController::setTagPriority(TagPriority priority)
{
    m_scanner->setTagPriority(priority);
}

LibraryController::TagPriority LibraryController::tagPriority() const
{
    return m_scanner->tagPriority();
}

// ---------------------------------------------------------------------------
// 收藏
// ---------------------------------------------------------------------------

QString LibraryController::identityOf(const QVariantMap &song) const
{
    return Song::fromMap(song).identityKey();
}

bool LibraryController::isFavorite(const QVariantMap &song) const
{
    // 收藏已合并为单一列表（在线/本地歌曲共用），不再按来源分库
    const Song s = Song::fromMap(song);
    const QString key = s.identityKey();
    for (const auto &item : m_favorites) {
        if (item.identityKey() == key) return true;
    }
    return false;
}

void LibraryController::toggleFavorite(const QVariantMap &song)
{
    const Song s = Song::fromMap(song);
    const QString key = s.identityKey();
    auto &list = m_favorites;

    for (int i = 0; i < list.size(); ++i) {
        if (list.at(i).identityKey() == key) {
            list.removeAt(i);
            save();
            bumpFavorites();
            return;
        }
    }
    list.prepend(s);
    while (list.size() > 2000) list.removeLast();
    save();
    bumpFavorites();
}

void LibraryController::removeFavorite(const QString &identityKey)
{
    bool changed = false;
    for (int i = 0; i < m_favorites.size(); ++i) {
        if (m_favorites.at(i).identityKey() == identityKey) {
            m_favorites.removeAt(i); changed = true; break;
        }
    }
    if (changed) {
        save();
        bumpFavorites();
    }
}

int LibraryController::removeFavorites(const QStringList &identityKeys)
{
    if (identityKeys.isEmpty()) return 0;
    const QSet<QString> want(identityKeys.begin(), identityKeys.end());
    const int before = m_favorites.size();
    for (int i = m_favorites.size() - 1; i >= 0; --i) {
        if (want.contains(m_favorites.at(i).identityKey())) m_favorites.removeAt(i);
    }
    const int removed = before - m_favorites.size();
    if (removed > 0) {          // 一次保存一次通知：逐条删会反复重置列表
        save();
        bumpFavorites();
    }
    return removed;
}

// ---------------------------------------------------------------------------
// 最近播放
// ---------------------------------------------------------------------------

void LibraryController::recordPlay(const QVariantMap &song)
{
    const Song s = Song::fromMap(song);
    const QString key = s.identityKey();
    for (int i = 0; i < m_recent.size(); ++i) {
        if (m_recent.at(i).identityKey() == key) {
            m_recent.removeAt(i);
            break;
        }
    }
    m_recent.prepend(s);
    while (m_recent.size() > 500) m_recent.removeLast();
    save();
    emit recentChanged();
}

void LibraryController::clearRecent()
{
    m_recent.clear();
    save();
    emit recentChanged();
}

// ---------------------------------------------------------------------------
// 歌单
// ---------------------------------------------------------------------------

QString LibraryController::createPlaylist(const QString &name, const QString &description)
{
    if (name.trimmed().isEmpty()) return QString();
    Playlist p;
    p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    p.name = name.trimmed();
    p.description = description;
    p.createdAt = Format::dateTime(QDateTime::currentMSecsSinceEpoch());
    p.updatedAt = p.createdAt;
    m_playlists.append(p);
    save();
    emit playlistsChanged();
    return p.id;
}

void LibraryController::deletePlaylist(const QString &id)
{
    for (int i = 0; i < m_playlists.size(); ++i) {
        if (m_playlists.at(i).id == id) {
            m_playlists.removeAt(i);
            save();
            emit playlistsChanged();
            return;
        }
    }
}

void LibraryController::renamePlaylist(const QString &id, const QString &name)
{
    for (auto &p : m_playlists) {
        if (p.id == id) {
            p.name = name.trimmed();
            p.updatedAt = Format::dateTime(QDateTime::currentMSecsSinceEpoch());
            save();
            emit playlistsChanged();
            return;
        }
    }
}

void LibraryController::addToPlaylist(const QString &playlistId, const QVariantMap &song)
{
    const Song s = Song::fromMap(song);
    for (auto &p : m_playlists) {
        if (p.id != playlistId) continue;
        for (const auto &exist : p.songs) {
            if (exist.identityKey() == s.identityKey()) return; // 去重
        }
        p.songs.append(s);
        p.updatedAt = Format::dateTime(QDateTime::currentMSecsSinceEpoch());
        save();
        emit playlistsChanged();
        return;
    }
}

void LibraryController::removeFromPlaylist(const QString &playlistId, int index)
{
    for (auto &p : m_playlists) {
        if (p.id != playlistId) continue;
        if (index < 0 || index >= p.songs.size()) return;
        p.songs.removeAt(index);
        p.updatedAt = Format::dateTime(QDateTime::currentMSecsSinceEpoch());
        save();
        emit playlistsChanged();
        return;
    }
}

int LibraryController::removeSongsFromPlaylist(const QString &playlistId, const QStringList &identityKeys)
{
    if (playlistId.isEmpty() || identityKeys.isEmpty()) return 0;
    const QSet<QString> want(identityKeys.begin(), identityKeys.end());
    for (auto &p : m_playlists) {
        if (p.id != playlistId) continue;
        const int before = p.songs.size();
        for (int i = p.songs.size() - 1; i >= 0; --i) {
            if (want.contains(p.songs.at(i).identityKey())) p.songs.removeAt(i);
        }
        const int removed = before - p.songs.size();
        if (removed > 0) {      // 一次保存一次通知
            p.updatedAt = Format::dateTime(QDateTime::currentMSecsSinceEpoch());
            save();
            emit playlistsChanged();
        }
        return removed;
    }
    return 0;
}

QVariantList LibraryController::playlistSongs(const QString &playlistId) const
{
    QVariantList out;
    for (const auto &p : m_playlists) {
        if (p.id != playlistId) continue;
        for (const auto &s : p.songs) out.append(s.toMap());
        break;
    }
    return out;
}

// ---------------------------------------------------------------------------
// 收藏在线歌单（广场/推荐整单收进侧栏「歌单」列表）
// ---------------------------------------------------------------------------

namespace {
/// 平台串归一：广场/推荐给的是 platformId("netease")，home.platform 给的是音源码("wy")。
/// 收藏键必须统一，否则同一张歌单会被认成两张（HANDOFF 点名的"两套串"坑）。
Platform platformLoose(const QString &s)
{
    Platform p = platformFromId(s);
    if (p == Platform::Local) p = platformFromSourceCode(s);
    return p == Platform::Local ? Platform::Netease : p;
}
} // namespace

QString LibraryController::collectedPlaylistId(const QString &platform, const QString &sourceId) const
{
    if (sourceId.isEmpty()) return QString();
    const QString pid = platformId(platformLoose(platform));
    for (const auto &p : m_playlists) {
        if (p.isOnlineImported && p.sourceId == sourceId && platformId(p.platform) == pid)
            return p.id;
    }
    return QString();
}

bool LibraryController::isPlaylistCollected(const QString &platform, const QString &sourceId) const
{
    return !collectedPlaylistId(platform, sourceId).isEmpty();
}

bool LibraryController::toggleCollectPlaylist(const QVariantMap &playlist, const QVariantList &songs)
{
    const QString sid = playlist.value(QStringLiteral("id")).toString();
    if (sid.isEmpty()) return false;
    const QString platform = playlist.value(QStringLiteral("platform")).toString();

    // 已经收藏过 → 这一下是「取消收藏」
    const QString existing = collectedPlaylistId(platform, sid);
    if (!existing.isEmpty()) {
        deletePlaylist(existing);
        return false;
    }

    Playlist p;
    p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    p.name = playlist.value(QStringLiteral("name")).toString().trimmed();
    if (p.name.isEmpty()) p.name = QStringLiteral("在线歌单");
    p.description = playlist.value(QStringLiteral("description")).toString();
    p.cover = playlist.value(QStringLiteral("cover")).toString();
    p.creator = playlist.value(QStringLiteral("creator")).toString();
    if (p.creator.isEmpty()) p.creator = playlist.value(QStringLiteral("author")).toString();
    if (p.creator.isEmpty()) p.creator = playlist.value(QStringLiteral("nickname")).toString();
    p.platform = platformLoose(platform);
    p.isOnlineImported = true;
    p.sourceId = sid;
    p.externalType = QStringLiteral("playlist");
    const QString now = Format::dateTime(QDateTime::currentMSecsSinceEpoch());
    p.createdAt = now;
    p.updatedAt = now;
    p.importedAt = now;

    QSet<QString> seen;
    for (const auto &v : songs) {
        const Song s = Song::fromMap(v.toMap());
        const QString key = s.identityKey();
        if (key.isEmpty() || seen.contains(key)) continue;
        seen.insert(key);
        p.songs.append(s);
    }

    m_playlists.append(p);
    save();
    emit playlistsChanged();
    return true;
}

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------

void LibraryController::bumpFavorites()
{
    ++m_favoritesVersion;
    emit favoritesChanged();
}

QString LibraryController::platformName(const QString &platformId) const
{
    return Muyun::platformName(Muyun::platformFromId(platformId));
}

QString LibraryController::formatDuration(double seconds) const
{
    return Format::duration(seconds);
}

QString LibraryController::formatPlayCount(qint64 count) const
{
    return Format::playCount(count);
}

// ---------------------------------------------------------------------------
// 局域网同步：在线库导出/导入
// ---------------------------------------------------------------------------

QVariantMap LibraryController::exportOnlineLibrary() const
{
    QVariantMap out;
    out[QStringLiteral("deviceName")] = QSysInfo::machineHostName();
    out[QStringLiteral("exportedAt")] = QDateTime::currentMSecsSinceEpoch();

    QVariantList favs;
    for (const auto &s : m_favorites)
        if (!s.isLocal()) favs.append(s.toMap());
    out[QStringLiteral("favorites")] = favs;

    QVariantList pls;
    for (const auto &p : m_playlists) {
        QVariantMap pm;
        pm[QStringLiteral("name")] = p.name;
        pm[QStringLiteral("description")] = p.description;
        QVariantList ps;
        for (const auto &s : p.songs)
            if (!s.isLocal()) ps.append(s.toMap());
        pm[QStringLiteral("songs")] = ps;
        pls.append(pm);
    }
    out[QStringLiteral("playlists")] = pls;
    return out;
}

int LibraryController::importOnlineLibrary(const QVariantMap &data)
{
    int added = 0;

    // 收藏：按 identityKey 去重后追加
    QSet<QString> favKeys;
    for (const auto &s : m_favorites) favKeys.insert(s.identityKey());
    const QVariantList favs = data.value(QStringLiteral("favorites")).toList();
    for (const auto &v : favs) {
        const QVariantMap m = v.toMap();
        if (m.isEmpty()) continue;
        const Song s = Song::fromMap(m);
        if (s.isLocal() || s.id.isEmpty()) continue;
        if (favKeys.contains(s.identityKey())) continue;
        favKeys.insert(s.identityKey());
        m_favorites.append(s);
        ++added;
    }

    // 歌单：同名合并（缺失歌曲追加），无同名则新建
    for (const auto &v : data.value(QStringLiteral("playlists")).toList()) {
        const QVariantMap pm = v.toMap();
        const QString pname = pm.value(QStringLiteral("name")).toString();
        if (pname.isEmpty()) continue;
        const QVariantList ps = pm.value(QStringLiteral("songs")).toList();

        Playlist *target = nullptr;
        for (auto &p : m_playlists) if (p.name == pname) { target = &p; break; }
        if (!target) {
            Playlist np;
            np.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            np.name = pname;
            np.description = pm.value(QStringLiteral("description")).toString();
            m_playlists.append(np);
            target = &m_playlists.last();
        }
        QSet<QString> exist;
        for (const auto &s : target->songs) exist.insert(s.identityKey());
        for (const auto &sv : ps) {
            const Song s = Song::fromMap(sv.toMap());
            if (s.isLocal() || s.id.isEmpty()) continue;
            if (exist.contains(s.identityKey())) continue;
            exist.insert(s.identityKey());
            target->songs.append(s);
            ++added;
        }
    }

    if (added > 0) {
        save();
        emit favoritesChanged();
        emit playlistsChanged();
        bumpFavorites();
    }
    return added;
}

// ---------------------------------------------------------------------------
// 洛雪同步适配：build / apply ListData
// ---------------------------------------------------------------------------

QJsonObject LibraryController::buildLxListData() const
{
    QJsonArray love;
    for (const auto &s : m_favorites)
        if (!s.isLocal()) love.append(LxMapping::songToMusicInfo(s));

    QJsonArray userList;
    for (const auto &p : m_playlists) {
        QJsonArray musics;
        for (const auto &s : p.songs)
            if (!s.isLocal()) musics.append(LxMapping::songToMusicInfo(s));
        QJsonObject o;
        o[QStringLiteral("id")] = p.id;
        o[QStringLiteral("name")] = p.name;
        o[QStringLiteral("source")] = QString();
        o[QStringLiteral("sourceListId")] = QString();
        o[QStringLiteral("locationUpdateTime")] = 0;
        o[QStringLiteral("list")] = musics;
        userList.append(o);
    }

    QJsonObject data;
    data[QStringLiteral("defaultList")] = QJsonArray();
    data[QStringLiteral("loveList")] = love;
    data[QStringLiteral("userList")] = userList;
    return data;
}

void LibraryController::applyLxListData(const QJsonObject &listData)
{
    // 1) 在线收藏：loveList 覆盖非本地部分，本地收藏原样保留
    QJsonArray love = listData.value(QStringLiteral("loveList")).toArray();
    QVector<Song> newOnline;
    for (const QJsonValue &v : love) {
        Song s;
        bool conv = LxMapping::musicInfoToSong(v.toObject(), &s);
        if (conv && !s.isLocal())
            newOnline.append(s);
    }
    QVector<Song> merged;
    for (const auto &s : m_favorites) if (s.isLocal()) merged.append(s);   // 本地在前
    QSet<QString> seen;
    for (const auto &s : merged) seen.insert(s.identityKey());
    for (const auto &s : newOnline)
        if (!seen.contains(s.identityKey())) { merged.append(s); seen.insert(s.identityKey()); }
    m_favorites = merged;

    // 2) 自建歌单：按 id 对齐（存在则改、缺失则删、新增则建），保留各歌单里的本地歌曲
    QJsonArray userList = listData.value(QStringLiteral("userList")).toArray();
    QVector<Playlist> result;
    for (const QJsonValue &lv : userList) {
        const QJsonObject lo = lv.toObject();
        const QString lid = lo.value(QStringLiteral("id")).toString();
        const QString lname = lo.value(QStringLiteral("name")).toString();

        Playlist np;
        np.id = lid.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : lid;
        np.name = lname;
        Playlist *old = nullptr;
        for (auto &p : m_playlists) if (p.id == np.id) { old = &p; break; }
        if (old) {
            np.description = old->description;
            np.createdAt = old->createdAt;
            for (const auto &s : old->songs)
                if (s.isLocal()) np.songs.append(s);   // 保留该歌单原有的本地歌曲
        }
        QSet<QString> onlineSeen;
        for (const QJsonValue &mv : lo.value(QStringLiteral("list")).toArray()) {
            Song s;
            if (!LxMapping::musicInfoToSong(mv.toObject(), &s) || s.isLocal()) continue;
            if (onlineSeen.contains(s.identityKey())) continue;
            onlineSeen.insert(s.identityKey());
            np.songs.append(s);
        }
        result.append(np);
    }
    m_playlists = result;

    save();
    emit favoritesChanged();
    emit playlistsChanged();
    bumpFavorites();
}

// ---------------------------------------------------------------------------
// 持久化
// ---------------------------------------------------------------------------

void LibraryController::load()
{
    auto *store = DocumentStore::instance();
    const QVariantMap doc = store->readAll(QStringLiteral("user"));

    auto readSongs = [](const QVariant &v) {
        QVector<Song> out;
        for (const auto &item : v.toList()) out.append(Song::fromMap(item.toMap()));
        return out;
    };

    m_favorites = readSongs(doc.value(QStringLiteral("favorites")));
    // 收藏合并迁移：旧版本"本地收藏"并入统一收藏（按 identityKey 去重）
    {
        const QVector<Song> legacyLocal = readSongs(doc.value(QStringLiteral("localFavorites")));
        for (const auto &s : legacyLocal) {
            bool dup = false;
            for (const auto &exist : m_favorites) {
                if (exist.identityKey() == s.identityKey()) { dup = true; break; }
            }
            if (!dup) m_favorites.prepend(s);
        }
        if (!legacyLocal.isEmpty()) {
            // 迁移后立即保存，清掉旧 localFavorites 键
            save();
        }
    }
    m_recent = readSongs(doc.value(QStringLiteral("recentlyPlayed")));

    m_playlists.clear();
    for (const auto &item : doc.value(QStringLiteral("playlists")).toList())
        m_playlists.append(Playlist::fromMap(item.toMap()));
}

void LibraryController::save()
{
    auto *store = DocumentStore::instance();
    QVariantMap doc = store->readAll(QStringLiteral("user"));

    auto writeSongs = [](const QVector<Song> &songs) {
        QVariantList out;
        for (const auto &s : songs) out.append(s.toMap());
        return QVariant(out);
    };

    doc[QStringLiteral("favorites")] = writeSongs(m_favorites);
    doc[QStringLiteral("localFavorites")] = QVariantList(); // 收藏已合并，写空清除旧键
    doc[QStringLiteral("recentlyPlayed")] = writeSongs(m_recent);

    QVariantList plArr;
    for (const auto &p : m_playlists) plArr.append(p.toMap());
    doc[QStringLiteral("playlists")] = plArr;

    store->writeAll(QStringLiteral("user"), doc);
}

} // namespace Muyun
