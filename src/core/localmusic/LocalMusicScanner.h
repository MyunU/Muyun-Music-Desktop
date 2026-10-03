#pragma once

#include "core/Types.h"
#include "core/localmusic/TagReader.h"

#include <QObject>
#include <QStringList>
#include <QFutureWatcher>

namespace Muyun {

/**
 * @brief 本地音乐库管理
 *
 * 负责维护扫描目录、扫描音频文件、读取标签并生成 Song 列表。
 * 扫描在后台线程执行，通过信号回报进度与结果。
 */
class LocalMusicScanner : public QObject
{
    Q_OBJECT
public:
    explicit LocalMusicScanner(QObject *parent = nullptr);

    /// 已添加的扫描目录
    QStringList folders() const;
    void addFolder(const QString &path);
    void removeFolder(const QString &path);

    /// "仅从列表移除"的忽略清单：扫描时跳过这些路径（文件保留在磁盘）
    void ignorePath(const QString &path);
    void clearIgnoredUnder(const QString &folder);

    /// 从内存列表移除某路径歌曲（配合删除文件/仅移除）
    void removeSongByPath(const QString &path);

    /// 播放器实测时长回写某路径歌曲（返回是否有变更）
    bool updateDurationByPath(const QString &path, int durationSec);

    /// 四-59：时长解析修复（不再盲信 Xing/Info 头 + OGG 读 granule）后，存档里已存的**错误时长**
    /// 不会自己变对（列表直接读存档）→ 首次启动做一次性重扫迁移。返回 true 表示还没迁移过。
    bool needsMediaDurationFix() const;
    /// 落迁移标记（先落标记再扫描：扫描异常也不会每次启动都重扫）
    void markMediaDurationFixed();

    /// 当前扫描到的歌曲
    QVector<Song> songs() const;
    int songCount() const;

    /// 异步扫描全部目录
    void scanAsync();
    /// 扫描单个目录（在后台线程执行）
    void scanFolderAsync(const QString &path);
    /// 同步扫描（用于测试与首次加载）
    QVector<Song> scanFolder(const QString &path);

    /// 上次扫描时间
    QString lastScannedAt() const;
    bool isScanning() const;

    /// 标签读取优先级
    enum class TagPriority { Embedded, External };
    void setTagPriority(TagPriority priority);
    TagPriority tagPriority() const;

    /// 读取单曲完整信息（含标签，用于标签编辑器）
    bool readSongDetail(const QString &path, LocalTags &tags, AudioInfo &info) const;

    /// 从磁盘加载/保存歌曲列表
    void loadFromDisk();
    void saveToDisk();

signals:
    void scanStarted();
    void scanProgress(int found, int scanned);
    void scanFinished(const QVector<Song> &songs);
    void folderAdded(const QString &path);
    void folderRemoved(const QString &path);

private:
    void mergeSongs(const QVector<Song> &newSongs);
    static QVector<Song> collectSongs(const QString &folder, const QStringList &ignored);

    QStringList m_folders;
    QStringList m_ignored;   // 仅从列表移除的文件路径（扫描跳过）
    QVector<Song> m_songs;
    QString m_lastScannedAt;
    bool m_scanning = false;
    bool m_scanPending = false;   // 扫描中又收到扫描请求 → 本轮结束补扫一次
    TagPriority m_tagPriority = TagPriority::Embedded;
    /// 四-59 时长迁移版本（存档字段 mediaFixVersion；0=旧档未迁移）
    int m_mediaFixVersion = 0;
};

} // namespace Muyun
