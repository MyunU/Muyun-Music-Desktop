#pragma once

#include <QObject>
#include <QVariantMap>
#include <QHash>
#include <QTimer>
#include <QReadWriteLock>

namespace Muyun {

/**
 * @brief 应用数据目录与 JSON 文档存储
 *
 * 对应原工程的 persistentStorage：数据落在 ~/.muyun/store/<name>.json，
 * 采用「内存缓存 + 防抖写盘 + 退出强制刷盘」策略，避免频繁 IO。
 */
class DocumentStore : public QObject
{
    Q_OBJECT
public:
    explicit DocumentStore(QObject *parent = nullptr);
    ~DocumentStore() override;

    static DocumentStore *instance();

    /// 数据根目录（~/.muyun）
    QString rootPath() const;
    /// 文档目录（~/.muyun/store）
    QString storeDir() const;
    /// 缓存目录
    QString cacheDir() const;
    /// 设置缓存目录（可自定义）
    void setCustomCacheDir(const QString &dir);
    QString customCacheDir() const;

    /// 读取文档（异步加载 + 内存缓存）
    QVariant read(const QString &docName, const QString &key,
                  const QVariant &defaultValue = QVariant());
    /// 同步读取（首次会从磁盘加载）
    QVariant readSync(const QString &docName, const QString &key,
                      const QVariant &defaultValue = QVariant());
    /// 读取整个文档
    QVariantMap readAll(const QString &docName);

    /// 写入（防抖落盘）
    void write(const QString &docName, const QString &key, const QVariant &value);
    void writeSync(const QString &docName, const QString &key, const QVariant &value);
    void writeAll(const QString &docName, const QVariantMap &data);

    /// 删除键
    void remove(const QString &docName, const QString &key);

    /// 立即把指定文档刷到磁盘（全部则传空）
    void flush(const QString &docName = QString());
    void flushAll();

    /// 打开数据目录（系统文件管理器）
    void openRootPath() const;

private:
    struct DocEntry {
        QVariantMap data;
        bool loaded = false;
        bool dirty = false;
        QTimer *timer = nullptr;
    };

    void ensureLoaded(const QString &docName);
    void scheduleFlush(const QString &docName, int delayMs);
    QString docPath(const QString &docName) const;

    mutable QReadWriteLock m_lock;
    QHash<QString, DocEntry> m_docs;
    QString m_rootPath;
    QString m_cacheDir;
    QString m_customCacheDir;
};

} // namespace Muyun
