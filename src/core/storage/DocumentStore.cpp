#include "DocumentStore.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QProcess>
#include <QDebug>

namespace Muyun {

static DocumentStore *s_instance = nullptr;

DocumentStore::DocumentStore(QObject *parent) : QObject(parent)
{
    // 数据根目录：优先 ~/.muyun；MUYUN_STORE_ROOT 可覆盖（自检/多实例隔离用）
    const QByteArray rootOverride = qgetenv("MUYUN_STORE_ROOT");
    const QString home = rootOverride.isEmpty()
                             ? QDir::homePath() + QStringLiteral("/.muyun")
                             : QString::fromLocal8Bit(rootOverride);
    m_rootPath = home;
    m_cacheDir = m_rootPath + QStringLiteral("/cache");

    QDir dir;
    dir.mkpath(m_rootPath + QStringLiteral("/store"));
    dir.mkpath(m_cacheDir);
}

DocumentStore::~DocumentStore()
{
    flushAll();
}

DocumentStore *DocumentStore::instance()
{
    if (!s_instance) s_instance = new DocumentStore(qApp);
    return s_instance;
}

QString DocumentStore::rootPath() const { return m_rootPath; }
QString DocumentStore::storeDir() const { return m_rootPath + QStringLiteral("/store"); }
QString DocumentStore::cacheDir() const
{
    return m_customCacheDir.isEmpty() ? m_cacheDir : m_customCacheDir;
}

void DocumentStore::setCustomCacheDir(const QString &dir)
{
    m_customCacheDir = dir;
    if (!dir.isEmpty()) {
        QDir d;
        d.mkpath(dir);
    }
}

QString DocumentStore::customCacheDir() const { return m_customCacheDir; }

QString DocumentStore::docPath(const QString &docName) const
{
    return storeDir() + QStringLiteral("/") + docName + QStringLiteral(".json");
}

void DocumentStore::ensureLoaded(const QString &docName)
{
    QWriteLocker locker(&m_lock);
    auto it = m_docs.find(docName);
    if (it == m_docs.end()) {
        DocEntry entry;
        m_docs.insert(docName, entry);
        it = m_docs.find(docName);
    }
    if (it->loaded) return;

    QFile f(docPath(docName));
    if (f.open(QIODevice::ReadOnly)) {
        const QByteArray raw = f.readAll();
        f.close();
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
        if (err.error == QJsonParseError::NoError && doc.isObject())
            it->data = doc.object().toVariantMap();
    }
    it->loaded = true;
}

QVariant DocumentStore::read(const QString &docName, const QString &key,
                             const QVariant &defaultValue)
{
    return readSync(docName, key, defaultValue);
}

QVariant DocumentStore::readSync(const QString &docName, const QString &key,
                                 const QVariant &defaultValue)
{
    ensureLoaded(docName);
    QReadLocker locker(&m_lock);
    const auto it = m_docs.constFind(docName);
    if (it == m_docs.constEnd()) return defaultValue;
    return it->data.value(key, defaultValue);
}

QVariantMap DocumentStore::readAll(const QString &docName)
{
    ensureLoaded(docName);
    QReadLocker locker(&m_lock);
    return m_docs.value(docName).data;
}

void DocumentStore::write(const QString &docName, const QString &key, const QVariant &value)
{
    ensureLoaded(docName);
    {
        QWriteLocker locker(&m_lock);
        auto &entry = m_docs[docName];
        entry.data.insert(key, value);
        entry.dirty = true;
    }
    // player 文档写盘更慢，其余 250ms
    scheduleFlush(docName, docName == QStringLiteral("player") ? 400 : 250);
}

void DocumentStore::writeSync(const QString &docName, const QString &key, const QVariant &value)
{
    write(docName, key, value);
    flush(docName);
}

void DocumentStore::writeAll(const QString &docName, const QVariantMap &data)
{
    ensureLoaded(docName);
    {
        QWriteLocker locker(&m_lock);
        auto &entry = m_docs[docName];
        entry.data = data;
        entry.dirty = true;
    }
    scheduleFlush(docName, 250);
}

void DocumentStore::remove(const QString &docName, const QString &key)
{
    ensureLoaded(docName);
    {
        QWriteLocker locker(&m_lock);
        auto &entry = m_docs[docName];
        entry.data.remove(key);
        entry.dirty = true;
    }
    scheduleFlush(docName, 250);
}

void DocumentStore::scheduleFlush(const QString &docName, int delayMs)
{
    // 定时器必须在创建线程（主线程）中投递
    QMetaObject::invokeMethod(this, [this, docName, delayMs]() {
        QWriteLocker locker(&m_lock);
        auto it = m_docs.find(docName);
        if (it == m_docs.end()) return;
        if (!it->timer) {
            it->timer = new QTimer(this);
            it->timer->setSingleShot(true);
            const QString name = docName;
            connect(it->timer, &QTimer::timeout, this, [this, name]() { flush(name); });
        }
        it->timer->start(delayMs);
    }, Qt::QueuedConnection);
}

void DocumentStore::flush(const QString &docName)
{
    if (docName.isEmpty()) { flushAll(); return; }

    QVariantMap snapshot;
    {
        QWriteLocker locker(&m_lock);
        auto it = m_docs.find(docName);
        if (it == m_docs.end() || !it->dirty) return;
        snapshot = it->data;
        it->dirty = false;
        if (it->timer) it->timer->stop();
    }

    QJsonObject obj = QJsonObject::fromVariantMap(snapshot);
    QJsonDocument doc(obj);
    QFile f(docPath(docName));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "DocumentStore: 无法写入文档" << docName;
        return;
    }
    f.write(doc.toJson(QJsonDocument::Compact));
    f.close();
}

void DocumentStore::flushAll()
{
    QStringList names;
    {
        QReadLocker locker(&m_lock);
        names = m_docs.keys();
    }
    for (const auto &n : names) flush(n);
}

void DocumentStore::openRootPath() const
{
    const QString path = QDir::toNativeSeparators(m_rootPath);
#if defined(Q_OS_WIN)
    QProcess::startDetached(QStringLiteral("explorer"), QStringList() << path);
#elif defined(Q_OS_MAC)
    QProcess::startDetached(QStringLiteral("open"), QStringList() << path);
#else
    QProcess::startDetached(QStringLiteral("xdg-open"), QStringList() << path);
#endif
}

} // namespace Muyun
