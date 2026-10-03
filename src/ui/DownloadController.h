#pragma once

#include "core/Types.h"
#include "core/network/HttpClient.h"

#include <QObject>
#include <QVariantList>
#include <QVector>
#include <QHash>
#include <QTimer>
#include <memory>

namespace Muyun {

/// 下载项状态
enum DownloadStatus {
    DsWaiting     = 0,   ///< 排队中（尚未开始）
    DsResolving   = 1,   ///< 正在解析/尝试某个候选（含重试切换）
    DsDownloading = 2,   ///< 正在下载
    DsCompleted   = 3,   ///< 已完成
    DsFailed      = 4,   ///< 失败（所有尝试都失败）
    DsCancelled   = 5,   ///< 用户取消
};

/// 一次下载尝试：用哪首歌（原始或跨平台候选）+ 哪档音质
struct DownloadAttempt {
    Song song;
    AudioQuality quality = AudioQuality::K320;
};

/// 一条下载任务（含元数据、进度、状态、重试序列）
struct DownloadItem {
    QString id;                                   ///< 唯一 id
    Song    song;                                 ///< 实际下载到的歌（换源后会变）
    AudioQuality requestedQuality = AudioQuality::Master;
    AudioQuality actualQuality    = AudioQuality::K320;
    QString requestedPlatform;                    ///< 用户点下载时的平台（wy/tx/...）
    QString actualPlatform;                       ///< 成功 attempt 实际走的平台
    QString originName;                           ///< 用户原始歌曲名（换源后对比展示）
    QString originArtist;
    QString savePath;                             ///< 目标文件路径
    int     status    = DsWaiting;
    qint64  received  = 0;
    qint64  total     = 0;
    qint64  addedAt   = 0;
    qint64  finishedAt = 0;
    QString error;
    int     attemptsTried = 0;                    ///< 已尝试过的候选数（展示用）
    QString attemptNote;                          ///< 如「已换源 3 次，音质降到 320k」

    QVariantMap toMap() const;
    static DownloadItem fromMap(const QVariantMap &m);
};

/**
 * @brief 下载管理器（暴露给 QML）
 *
 * 核心策略：**先换源，后降音质**
 * - 用户指定目标音质 Q；
 * - 尝试序列：对每个音质档位 q（从 Q 沿降级链向下），依次尝试
 *   [原歌曲, 跨平台候选 1..N]，每次调 `MusicSdk::resolveUrlAtQuality`
 *   拿到候选 URL 后立刻走 HTTP 下载；
 * - 只要有一次 HTTP 成功即完成；HTTP 失败或 URL 解析为空 → 推进到下一个尝试；
 * - 全部尝试失败 → 标失败（保留最后一次错误）。
 *
 * 其它：
 * - 下载目录默认 `QStandardPaths::MusicLocation`；
 * - 状态持久化到 DocumentStore "downloads"；重启后中断项自动重新排队；
 * - 完成后触发 `rescanRequested()` → 本地库重扫。
 */
class DownloadController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged)
    Q_PROPERTY(int activeCount READ activeCount NOTIFY itemsChanged)
    Q_PROPERTY(int doneCount READ doneCount NOTIFY itemsChanged)
    Q_PROPERTY(QString downloadPath READ downloadPath WRITE setDownloadPath NOTIFY downloadPathChanged)
    Q_PROPERTY(QString downloadQuality READ downloadQuality WRITE setDownloadQuality NOTIFY downloadQualityChanged)

public:
    explicit DownloadController(QObject *parent = nullptr);

    QVariantList items() const;
    int activeCount() const;
    int doneCount() const;
    QString downloadPath() const;
    void setDownloadPath(const QString &path);
    QString downloadQuality() const;
    void setDownloadQuality(const QString &qualityId);

    /// 加入下载队列；qualityId 为空则用默认下载音质（默认 master）。
    Q_INVOKABLE bool addDownload(const QVariantMap &songMap, const QString &qualityId = QString());
    /// 批量入队（歌单/收藏的"批量下载"）：本地歌、已在队列/已下好的自动跳过且**不弹提示**，
    /// 返回真正加入的条数。
    Q_INVOKABLE int addDownloads(const QVariantList &songMaps, const QString &qualityId = QString());
    Q_INVOKABLE void cancelDownload(const QString &id);
    Q_INVOKABLE void retryDownload(const QString &id);
    Q_INVOKABLE void removeItem(const QString &id);
    Q_INVOKABLE void removeAndDeleteFile(const QString &id);
    Q_INVOKABLE void clearCompleted();
    Q_INVOKABLE QString itemPath(const QString &id) const;
    Q_INVOKABLE void revealInFolder(const QString &id) const;

    Q_INVOKABLE QVariantList qualityOptions() const;
    Q_INVOKABLE static QString formatBytes(qint64 bytes);

    /// 探测各音质文件大小（解析URL + HEAD Content-Length），逐个经 qualitySizeReady 回报。
    /// 再次调用会作废上一次未完成的探测（换歌/重开弹窗）。bytes=-1 表示该档不可得。
    Q_INVOKABLE void probeSizes(const QVariantMap &songMap);

signals:
    void itemsChanged();
    void downloadPathChanged();
    void downloadQualityChanged();
    void rescanRequested();
    void message(const QString &text);
    void qualitySizeReady(const QString &qualityId, qint64 bytes);

private slots:
    void flushProgress();

private:
    void load();
    void save();
    void pump();
    void startItem(const QString &id);
    /// 异步构建尝试序列（在 worker 里 findMusic），完成后回到 main 调 advanceAttempt
    void buildLadder(const QString &id);
    /// 推进到下一个尝试；若已跑完则标失败
    void advanceAttempt(const QString &id);
    /// 对给定 attempt 解析 URL（worker）→ 有 URL 就走 HTTP；无 URL 则下一 attempt
    void attemptResolve(const QString &id);
    /// 拿到 URL 后启动 HTTP 下载
    void attemptHttp(const QString &id, const QString &url, const Song &s, AudioQuality q);
    /// 完成后：worker 内嵌封面/歌词（设置可关），再 finalize
    void embedTagsThenFinalize(const QString &id);
    /// 降级/换源时写"实际：音质 · 平台"提示
    void applyActualNote(DownloadItem &it) const;
    /// 完成收尾
    void finalize(const QString &id, int newStatus, const QString &err = QString());

    int  indexOf(const QString &id) const;
    QString uniqueSavePath(const QString &dir, const QString &baseName, const QString &ext) const;
    /// 根据目标音质重算 savePath（用于换音质后调整扩展名）
    void rebuildSavePath(DownloadItem &it, AudioQuality q);

    QVector<DownloadItem> m_items;
    /// 每项的取消令牌（贯穿整个 attempt 序列）
    QHash<QString, DownloadHandlePtr> m_handles;
    /// 每项的重试序列（内存态，不持久化；重启会重建）
    QHash<QString, QVector<DownloadAttempt>> m_ladders;
    /// 每项当前尝试下标（内存态）
    QHash<QString, int> m_cursor;
    /// 每项的 HTTP 层取消句柄（每次 attempt 一个）
    QHash<QString, DownloadHandlePtr> m_httpHandles;

    QTimer m_progressTimer;
    bool   m_dirtyProgress = false;
    QString m_downloadPath;
    QString m_downloadQualityId;
    int     m_maxConcurrent = 2;
    quint64 m_probeToken = 0;   // 音质大小探测代际（换歌/重发作废旧探测）
};

} // namespace Muyun
