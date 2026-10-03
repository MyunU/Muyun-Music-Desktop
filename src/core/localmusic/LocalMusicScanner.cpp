#include "LocalMusicScanner.h"

#include "core/storage/DocumentStore.h"
#include "core/utils/Format.h"

#include <QDirIterator>
#include <QFileInfo>
#include <QDateTime>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSet>

namespace Muyun {

LocalMusicScanner::LocalMusicScanner(QObject *parent) : QObject(parent)
{
    loadFromDisk();
}

QStringList LocalMusicScanner::folders() const { return m_folders; }
int LocalMusicScanner::songCount() const { return m_songs.size(); }
QVector<Song> LocalMusicScanner::songs() const { return m_songs; }
QString LocalMusicScanner::lastScannedAt() const { return m_lastScannedAt; }
bool LocalMusicScanner::isScanning() const { return m_scanning; }

void LocalMusicScanner::setTagPriority(TagPriority priority) { m_tagPriority = priority; }
LocalMusicScanner::TagPriority LocalMusicScanner::tagPriority() const { return m_tagPriority; }

void LocalMusicScanner::addFolder(const QString &path)
{
    if (path.isEmpty() || m_folders.contains(path)) return;
    m_folders.append(path);
    saveToDisk();
    emit folderAdded(path);
}

void LocalMusicScanner::removeFolder(const QString &path)
{
    if (!m_folders.contains(path)) return;
    m_folders.removeAll(path);
    clearIgnoredUnder(path);
    // 移除该目录下的歌曲
    QVector<Song> kept;
    for (const auto &s : m_songs) {
        if (s.localFolder != path) kept.append(s);
    }
    m_songs = kept;
    saveToDisk();
    emit folderRemoved(path);
}

void LocalMusicScanner::ignorePath(const QString &path)
{
    if (path.isEmpty() || m_ignored.contains(path)) return;
    m_ignored.append(path);
    saveToDisk();
}

void LocalMusicScanner::clearIgnoredUnder(const QString &folder)
{
    const QString prefix = folder + QLatin1Char('/');
    QStringList kept;
    for (const auto &p : m_ignored) {
        if (!p.startsWith(prefix) && p != folder) kept.append(p);
    }
    m_ignored = kept;
}

void LocalMusicScanner::removeSongByPath(const QString &path)
{
    QVector<Song> kept;
    for (const auto &s : m_songs) {
        if (s.localPath != path) kept.append(s);
    }
    m_songs = kept;
    saveToDisk();
}

bool LocalMusicScanner::updateDurationByPath(const QString &path, int durationSec)
{
    bool changed = false;
    for (auto &s : m_songs) {
        if (s.localPath == path && s.duration != durationSec) {
            s.duration = durationSec;
            changed = true;
        }
    }
    if (changed) saveToDisk();
    return changed;
}

bool LocalMusicScanner::readSongDetail(const QString &path, LocalTags &tags,
                                       AudioInfo &info) const
{
    TagReader::read(path, tags, info);
    return info.valid;
}

QVector<Song> LocalMusicScanner::collectSongs(const QString &folder, const QStringList &ignored)
{
    QVector<Song> out;
    if (folder.isEmpty()) return out;

    const QStringList nameFilters = TagReader::supportedExtensions();
    QDirIterator it(folder, nameFilters, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (ignored.contains(path)) continue;   // "仅从列表移除"过的文件不再回收
        LocalTags tags;
        AudioInfo info;
        TagReader::read(path, tags, info);
        if (!info.valid) continue;

        const QFileInfo fi(path);
        Song s;
        s.platform = Platform::Local;
        s.localPath = path;
        s.localFolder = folder;
        s.localFileSize = fi.size();
        s.localModifiedAt = Format::dateTime(fi.lastModified().toMSecsSinceEpoch());
        s.duration = info.durationSec;
        s.id = QString::number(qHash(path), 16);

        // 优先内嵌标签，缺失时回退文件名
        s.name = tags.title.isEmpty() ? fi.completeBaseName() : tags.title;
        s.artist = tags.artist;
        s.album = tags.album;
        if (s.artist.isEmpty() || s.name == fi.completeBaseName()) {
            QString t, a;
            TagReader::parseFileName(fi.completeBaseName(), t, a);
            if (s.artist.isEmpty()) s.artist = a;
            if (!t.isEmpty() && s.name == fi.completeBaseName()) s.name = t;
        }
        s.localTrackNo = tags.trackNo;
        s.localDiscNo = tags.discNo;

        // 同目录外挂封面
        const QString dirPath = fi.absolutePath();
        const QString base = fi.completeBaseName();
        static const QStringList coverExt = {QStringLiteral("jpg"),
                                             QStringLiteral("png"),
                                             QStringLiteral("webp"),
                                             QStringLiteral("jpeg")};
        for (const auto &ext : coverExt) {
            const QString candidate = dirPath + QLatin1Char('/') + base +
                                      QLatin1Char('.') + ext;
            if (QFileInfo::exists(candidate)) {
                s.cover = QUrl::fromLocalFile(candidate).toString();
                break;
            }
        }
        // 无外挂封面 → 导出内嵌封面到缓存目录（Song.cover 是 URL 字符串，供 Image 加载）
        if (s.cover.isEmpty() && tags.hasCover && !tags.cover.isNull()) {
            const QString cacheDir = DocumentStore::instance()->cacheDir()
                                     + QStringLiteral("/covers");
            QDir().mkpath(cacheDir);
            const QString fn = cacheDir + QStringLiteral("/")
                               + QString::number(qHash(path), 16) + QStringLiteral(".jpg");
            if (!QFileInfo::exists(fn))
                tags.cover.save(fn, "JPG", 85);
            s.cover = QUrl::fromLocalFile(fn).toString();
        }
        out.append(s);
    }
    return out;
}

QVector<Song> LocalMusicScanner::scanFolder(const QString &path)
{
    return collectSongs(path, m_ignored);
}

void LocalMusicScanner::mergeSongs(const QVector<Song> &newSongs)
{
    QHash<QString, int> indexByPath;
    for (int i = 0; i < m_songs.size(); ++i)
        indexByPath.insert(m_songs.at(i).localPath, i);

    for (const auto &s : newSongs) {
        auto it = indexByPath.find(s.localPath);
        if (it != indexByPath.end()) m_songs[it.value()] = s;
        else m_songs.append(s);
    }
}

void LocalMusicScanner::scanAsync()
{
    if (m_scanning) { m_scanPending = true; return; }   // 忙 → 记待扫，本轮结束自动补扫
    m_scanning = true;
    emit scanStarted();

    const QStringList folders = m_folders;
    const QStringList ignored = m_ignored;
    auto *watcher = new QFutureWatcher<QVector<Song>>(this);
    connect(watcher, &QFutureWatcher<QVector<Song>>::finished, this,
            [this, watcher, folders]() {
                const QVector<Song> result = watcher->result();
                // 全量扫描结果权威替换：只保留本次扫描目录内的歌曲（磁盘已删的自动消失）
                QSet<QString> folderSet(folders.begin(), folders.end());
                QVector<Song> merged;
                QHash<QString, int> byPath;
                for (const auto &s : result) {
                    if (!folderSet.contains(s.localFolder)) continue;
                    byPath.insert(s.localPath, merged.size());
                    merged.append(s);
                }
                // 保留不在本次扫描目录内的历史歌曲（理论上 folders 覆盖全部，防御性保留）
                for (const auto &s : m_songs)
                    if (!folderSet.contains(s.localFolder) && !byPath.contains(s.localPath))
                        merged.append(s);
                m_songs = merged;
                m_lastScannedAt =
                    Format::dateTime(QDateTime::currentMSecsSinceEpoch());
                m_scanning = false;
                saveToDisk();
                emit scanFinished(m_songs);
                watcher->deleteLater();
                if (m_scanPending) { m_scanPending = false; scanAsync(); }
            });

    auto future = QtConcurrent::run([folders, ignored]() {
        QVector<Song> all;
        for (const auto &f : folders) all.append(collectSongs(f, ignored));
        return all;
    });
    watcher->setFuture(future);
}

void LocalMusicScanner::scanFolderAsync(const QString &path)
{
    if (m_scanning) return;
    m_scanning = true;
    emit scanStarted();

    auto *watcher = new QFutureWatcher<QVector<Song>>(this);
    connect(watcher, &QFutureWatcher<QVector<Song>>::finished, this,
            [this, watcher]() {
                mergeSongs(watcher->result());
                m_lastScannedAt =
                    Format::dateTime(QDateTime::currentMSecsSinceEpoch());
                m_scanning = false;
                saveToDisk();
                emit scanFinished(m_songs);
                watcher->deleteLater();
            });
    watcher->setFuture(QtConcurrent::run([path, ignored = m_ignored]() { return collectSongs(path, ignored); }));
}

void LocalMusicScanner::loadFromDisk()
{
    auto *store = DocumentStore::instance();
    const QVariantMap doc = store->readAll(QStringLiteral("local-music"));
    m_folders = doc.value(QStringLiteral("folders")).toStringList();
    m_ignored = doc.value(QStringLiteral("ignored")).toStringList();
    m_lastScannedAt = doc.value(QStringLiteral("lastScannedAt")).toString();
    m_mediaFixVersion = doc.value(QStringLiteral("mediaFixVersion"), 0).toInt();
    const auto arr = doc.value(QStringLiteral("songs")).toList();
    for (const auto &item : arr) m_songs.append(Song::fromMap(item.toMap()));
}

void LocalMusicScanner::saveToDisk()
{
    auto *store = DocumentStore::instance();
    QVariantList arr;
    for (const auto &s : m_songs) arr.append(s.toMap());
    QVariantMap doc;
    doc[QStringLiteral("folders")] = m_folders;
    doc[QStringLiteral("ignored")] = m_ignored;
    doc[QStringLiteral("lastScannedAt")] = m_lastScannedAt;
    doc[QStringLiteral("mediaFixVersion")] = m_mediaFixVersion;
    doc[QStringLiteral("songs")] = arr;
    store->writeAll(QStringLiteral("local-music"), doc);
}

// 四-59：本轮修复后的"时长解析版本"（1 = 帧头遍历 + OGG granule）。以后若再修时长算法就 +1。
static constexpr int kMediaFixVersion = 1;

bool LocalMusicScanner::needsMediaDurationFix() const
{
    return m_mediaFixVersion < kMediaFixVersion && !m_folders.isEmpty();
}

void LocalMusicScanner::markMediaDurationFixed()
{
    if (m_mediaFixVersion >= kMediaFixVersion) return;
    m_mediaFixVersion = kMediaFixVersion;
    saveToDisk();
}

} // namespace Muyun
