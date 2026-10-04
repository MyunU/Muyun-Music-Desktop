#include "DownloadController.h"

#include "core/music/MusicSdk.h"
#include "core/localmusic/TagWriter.h"
#include "core/storage/DocumentStore.h"
#include "core/utils/AudioUrl.h"
#include "core/utils/Format.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QBuffer>
#include <QMetaObject>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>
#include <QUuid>
#include <QtConcurrent>
#include <QFutureWatcher>

namespace Muyun {

// ===========================================================================
// DownloadItem 序列化
// ===========================================================================

QVariantMap DownloadItem::toMap() const
{
    QVariantMap m;
    m[QStringLiteral("id")] = id;
    m[QStringLiteral("song")] = song.toMap();
    m[QStringLiteral("requestedQuality")] = qualityId(requestedQuality);
    m[QStringLiteral("actualQuality")] = qualityId(actualQuality);
    m[QStringLiteral("requestedPlatform")] = requestedPlatform;
    m[QStringLiteral("actualPlatform")] = actualPlatform;
    m[QStringLiteral("originName")] = originName;
    m[QStringLiteral("originArtist")] = originArtist;
    m[QStringLiteral("savePath")] = savePath;
    m[QStringLiteral("status")] = status;
    m[QStringLiteral("received")] = received;
    m[QStringLiteral("total")] = total;
    m[QStringLiteral("addedAt")] = addedAt;
    m[QStringLiteral("finishedAt")] = finishedAt;
    if (!error.isEmpty()) m[QStringLiteral("error")] = error;
    m[QStringLiteral("attemptsTried")] = attemptsTried;
    if (!attemptNote.isEmpty()) m[QStringLiteral("attemptNote")] = attemptNote;
    return m;
}

DownloadItem DownloadItem::fromMap(const QVariantMap &m)
{
    DownloadItem it;
    it.id = m.value(QStringLiteral("id")).toString();
    it.song = Song::fromMap(m.value(QStringLiteral("song")).toMap());
    it.requestedQuality = qualityFromId(m.value(QStringLiteral("requestedQuality")).toString(), nullptr);
    it.actualQuality = qualityFromId(m.value(QStringLiteral("actualQuality")).toString(), nullptr);
    it.requestedPlatform = m.value(QStringLiteral("requestedPlatform")).toString();
    it.actualPlatform = m.value(QStringLiteral("actualPlatform")).toString();
    it.originName = m.value(QStringLiteral("originName")).toString();
    it.originArtist = m.value(QStringLiteral("originArtist")).toString();
    it.savePath = m.value(QStringLiteral("savePath")).toString();
    it.status = m.value(QStringLiteral("status")).toInt();
    it.received = m.value(QStringLiteral("received")).toLongLong();
    it.total = m.value(QStringLiteral("total")).toLongLong();
    it.addedAt = m.value(QStringLiteral("addedAt")).toLongLong();
    it.finishedAt = m.value(QStringLiteral("finishedAt")).toLongLong();
    it.error = m.value(QStringLiteral("error")).toString();
    it.attemptsTried = m.value(QStringLiteral("attemptsTried")).toInt();
    it.attemptNote = m.value(QStringLiteral("attemptNote")).toString();
    return it;
}

// ===========================================================================
// 工具
// ===========================================================================

namespace {

QString makeItemId()
{
    return QString::number(QDateTime::currentMSecsSinceEpoch(), 36)
           + QStringLiteral("_") + QUuid::createUuid().toString(QUuid::Id128).left(8);
}

QString extForQuality(AudioQuality q)
{
    switch (q) {
    case AudioQuality::Flac:
    case AudioQuality::Flac24Bit:
    case AudioQuality::HiRes:
    case AudioQuality::Atmos:
    case AudioQuality::Master:
        return QStringLiteral("flac");
    case AudioQuality::K320:
    case AudioQuality::K128:
    default:
        return QStringLiteral("mp3");
    }
}

QString ensureDir(const QString &dir)
{
    QDir d(dir);
    if (!d.exists()) d.mkpath(QStringLiteral("."));
    return d.absolutePath();
}

QString uniqueBaseName(const QString &dir, const QString &base, const QString &ext)
{
    QString candidate = base + QStringLiteral(".") + ext;
    if (!QFileInfo::exists(dir + QStringLiteral("/") + candidate)) return candidate;
    for (int i = 1; i < 1000; ++i) {
        candidate = base + QStringLiteral(" (") + QString::number(i) + QStringLiteral(").") + ext;
        if (!QFileInfo::exists(dir + QStringLiteral("/") + candidate)) return candidate;
    }
    return base + QStringLiteral("_") + QUuid::createUuid().toString(QUuid::Id128).left(6) + QStringLiteral(".") + ext;
}

QString extFromUrl(const QString &url)
{
    const QString lower = url.toLower();
    static const char *kExts[] = {"flac", "mp3", "m4a", "aac", "wav", "ogg", "wma"};
    for (const char *e : kExts) {
        const QByteArray needle = QByteArray(".") + e;
        int idx = 0;
        while ((idx = lower.indexOf(needle, idx)) != -1) {
            const int end = idx + needle.size();
            const bool boundary = (end >= lower.size()) || (!lower[end].isLetterOrNumber());
            if (boundary) return QString::fromLatin1(e);
            idx = end;
        }
    }
    return QString();
}

} // namespace

// ===========================================================================
// 构造 / 载入 / 保存
// ===========================================================================

DownloadController::DownloadController(QObject *parent)
    : QObject(parent)
{
    m_progressTimer.setInterval(300);
    m_progressTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_progressTimer, &QTimer::timeout, this, &DownloadController::flushProgress);
    load();
}

void DownloadController::load()
{
    auto *store = DocumentStore::instance();
    const QVariantMap doc = store->readAll(QStringLiteral("downloads"));

    QString path = doc.value(QStringLiteral("downloadPath")).toString();
    if (path.isEmpty()) {
        const QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
        path = music.isEmpty() ? QDir::homePath() + QStringLiteral("/Music") : music;
    }
    m_downloadPath = path;

    m_downloadQualityId = doc.value(QStringLiteral("downloadQuality")).toString();
    // 默认最高音质；下载器内部会按「先换源，后降音质」自动降级
    if (m_downloadQualityId.isEmpty()) m_downloadQualityId = QStringLiteral("master");

    m_items.clear();
    const QVariantList arr = doc.value(QStringLiteral("list")).toList();
    for (const auto &v : arr) {
        DownloadItem it = DownloadItem::fromMap(v.toMap());
        if (it.id.isEmpty()) continue;
        if (it.status == DsResolving || it.status == DsDownloading) {
            it.status = DsWaiting;
            it.received = 0;
            it.total = 0;
            if (!it.savePath.isEmpty()) QFile::remove(it.savePath);
        }
        m_items.append(it);
    }
    pump();
}

void DownloadController::save()
{
    QVariantList arr;
    for (const auto &it : m_items) arr.append(it.toMap());
    QVariantMap doc;
    doc[QStringLiteral("downloadPath")] = m_downloadPath;
    doc[QStringLiteral("downloadQuality")] = m_downloadQualityId;
    doc[QStringLiteral("list")] = arr;
    DocumentStore::instance()->writeAll(QStringLiteral("downloads"), doc);
}

// ===========================================================================
// 属性
// ===========================================================================

QVariantList DownloadController::items() const
{
    QVariantList out;
    out.reserve(m_items.size());
    for (const auto &it : m_items) out.append(it.toMap());
    return out;
}

int DownloadController::activeCount() const
{
    int n = 0;
    for (const auto &it : m_items)
        if (it.status == DsWaiting || it.status == DsResolving || it.status == DsDownloading)
            ++n;
    return n;
}

int DownloadController::doneCount() const
{
    int n = 0;
    for (const auto &it : m_items)
        if (it.status == DsCompleted) ++n;
    return n;
}

QString DownloadController::downloadPath() const { return m_downloadPath; }

void DownloadController::setDownloadPath(const QString &path)
{
    const QString p = path.trimmed();
    if (p.isEmpty() || p == m_downloadPath) return;
    m_downloadPath = p;
    save();
    emit downloadPathChanged();
}

QString DownloadController::downloadQuality() const { return m_downloadQualityId; }

void DownloadController::setDownloadQuality(const QString &qualityId)
{
    bool ok = false;
    qualityFromId(qualityId, &ok);
    if (!ok || qualityId == m_downloadQualityId) return;
    m_downloadQualityId = qualityId;
    save();
    emit downloadQualityChanged();
}

QVariantList DownloadController::qualityOptions() const
{
    QVariantList out;
    for (const auto &q : allQualitiesDesc()) {
        QVariantMap m;
        m[QStringLiteral("id")] = qualityId(q);
        m[QStringLiteral("name")] = qualityName(q);
        m[QStringLiteral("desc")] = qualityDesc(q);
        out.append(m);
    }
    return out;
}

QString DownloadController::formatBytes(qint64 bytes) { return Format::fileSize(bytes); }

void DownloadController::probeSizes(const QVariantMap &songMap)
{
    const Song song = Song::fromMap(songMap);
    if (song.id.isEmpty() && song.identityKey().isEmpty()) return;
    const QString key = song.identityKey();
    // #12：缓存命中直接同步发结果（弹窗/菜单打开不再转圈），不打网络
    const auto it = m_sizeCache.constFind(key);
    if (it != m_sizeCache.constEnd()) {
        for (auto i = it.value().constBegin(); i != it.value().constEnd(); ++i)
            emit qualitySizeReady(key, i.key(), i.value());
        return;
    }
    const quint64 token = ++m_probeToken;
    auto *self = this;
    const QList<AudioQuality> quals = allQualitiesDesc();
    QtConcurrent::run([self, song, quals, token, key]() {
        MusicSdk *sdk = MusicSdk::instance();
        for (const AudioQuality q : quals) {
            if (token != self->m_probeToken) return;   // 已被更新的探测作废
            const QString url = sdk->resolveUrlAtQuality(song, q);
            qint64 bytes = -1;
            if (!url.isEmpty()) {
                HttpOptions o;
                o.referer = refererForAudioUrl(url);
                o.timeoutMs = 8000;
                const HttpResponse h = HttpClient::instance()->head(url, o);
                bool okLen = false;
                const qint64 cl = h.header(QStringLiteral("content-length")).toLongLong(&okLen);
                if (h.ok && okLen && cl > 0) bytes = cl;
            }
            const QString qid = qualityId(q);
            QMetaObject::invokeMethod(self, [self, qid, bytes, token, key]() {
                if (token != self->m_probeToken) return;
                if (bytes >= 0) self->m_sizeCache[key].insert(qid, bytes);
                emit self->qualitySizeReady(key, qid, bytes);
            }, Qt::QueuedConnection);
        }
    });
}

void DownloadController::prefetchSizes(const QVariantList &songMaps)
{
    // #12：预取当前曲/可见列表的音质大小。只对"在线歌 + 没缓存过"发请求；
    // 整批共用一个代际：用户此刻打开菜单/弹窗（probeSizes 抬代际）→ 本批自动作废，
    // 不会把别的歌的尺寸灌进正在看的弹窗。每批最多 cap 首，串行问档，失败静默。
    QVector<Song> todo;
    for (const auto &v : songMaps) {
        const Song s = Song::fromMap(v.toMap());
        if (s.id.isEmpty() || s.isLocal()) continue;
        if (m_sizeCache.contains(s.identityKey())) continue;
        todo.append(s);
    }
    if (todo.isEmpty()) return;
    const quint64 token = ++m_probeToken;
    auto *self = this;
    const int cap = qMin(6, todo.size());
    const QList<AudioQuality> quals = allQualitiesDesc();
    QtConcurrent::run([self, todo, quals, token, cap]() {
        MusicSdk *sdk = MusicSdk::instance();
        for (int i = 0; i < cap; ++i) {
            if (token != self->m_probeToken) return;   // 用户开了弹窗/新预取 → 本批作废
            const Song &song = todo.at(i);
            const QString key = song.identityKey();
            for (const AudioQuality q : quals) {
                if (token != self->m_probeToken) return;
                const QString url = sdk->resolveUrlAtQuality(song, q);
                qint64 bytes = -1;
                if (!url.isEmpty()) {
                    HttpOptions o;
                    o.referer = refererForAudioUrl(url);
                    o.timeoutMs = 8000;
                    const HttpResponse h = HttpClient::instance()->head(url, o);
                    bool okLen = false;
                    const qint64 cl = h.header(QStringLiteral("content-length")).toLongLong(&okLen);
                    if (h.ok && okLen && cl > 0) bytes = cl;
                }
                const QString qid = qualityId(q);
                QMetaObject::invokeMethod(self, [self, qid, bytes, token, key]() {
                    if (token != self->m_probeToken) return;
                    if (bytes >= 0) self->m_sizeCache[key].insert(qid, bytes);
                    emit self->qualitySizeReady(key, qid, bytes);
                }, Qt::QueuedConnection);
            }
        }
    });
}

// ===========================================================================
// 队列管理
// ===========================================================================

int DownloadController::indexOf(const QString &id) const
{
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items.at(i).id == id) return i;
    return -1;
}

int DownloadController::addDownloads(const QVariantList &songMaps, const QString &qualityId)
{
    int added = 0, skipped = 0;
    for (const auto &v : songMaps) {
        const QVariantMap m = v.toMap();
        const Song s = Song::fromMap(m);
        const QString key = s.identityKey();
        if ((s.id.isEmpty() && key.isEmpty()) || s.isLocal()) { ++skipped; continue; }
        // 已在队列里 / 已下好且文件还在 → 静默跳过（单首 addDownload 会逐条弹 toast，批量会刷屏）
        bool queued = false;
        for (const auto &it : m_items) {
            if (it.song.identityKey() != key) continue;
            if (it.status == DsWaiting || it.status == DsResolving || it.status == DsDownloading
                || (it.status == DsCompleted && QFile::exists(it.savePath))) { queued = true; break; }
        }
        if (queued) { ++skipped; continue; }
        if (addDownload(m, qualityId)) ++added; else ++skipped;
    }
    if (added == 0 && skipped > 0)
        emit message(QStringLiteral("所选歌曲都无需下载（本地歌或已在下载列表）"));
    return added;
}

bool DownloadController::addDownload(const QVariantMap &songMap, const QString &qualityIdArg)
{
    const Song song = Song::fromMap(songMap);
    if (song.id.isEmpty() && song.identityKey().isEmpty()) {
        emit message(QStringLiteral("歌曲信息无效"));
        return false;
    }
    if (song.isLocal()) {
        emit message(QStringLiteral("本地音乐无需下载"));
        return false;
    }

    const QString key = song.identityKey();
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).song.identityKey() != key) continue;
        const DownloadItem &old = m_items.at(i);
        if (old.status == DsFailed || old.status == DsCancelled) continue;
        // 已完成的记录：文件还在 → 拒绝重复下载；文件被用户删了 → 移除旧记录放行重下
        if (old.status == DsCompleted && !QFile::exists(old.savePath)) {
            m_items.removeAt(i);
            break;
        }
        if (old.status == DsCompleted) {
            emit message(QStringLiteral("歌曲已在下载列表"));
            return false;
        }
        if (old.status == DsWaiting || old.status == DsResolving || old.status == DsDownloading) {
            emit message(QStringLiteral("歌曲已在下载列表"));
            return false;
        }
    }

    QString qid = qualityIdArg;
    if (qid.isEmpty()) qid = m_downloadQualityId;
    bool ok = false;
    AudioQuality q = qualityFromId(qid, &ok);
    if (!ok) q = AudioQuality::Master;

    const QString dir = ensureDir(m_downloadPath);
    const QString ext = extForQuality(q);
    const QString artist = Format::safeFileName(song.artist.isEmpty() ? QStringLiteral("未知歌手") : song.artist);
    const QString title  = Format::safeFileName(song.name.isEmpty()  ? QStringLiteral("未知歌曲") : song.name);
    const QString base   = artist + QStringLiteral(" - ") + title;
    const QString fileName = uniqueBaseName(dir, base, ext);

    DownloadItem it;
    it.id = makeItemId();
    it.song = song;
    it.requestedQuality = q;
    it.actualQuality = q;
    it.requestedPlatform = song.sourceCode();
    it.actualPlatform = song.sourceCode();
    it.originName = song.name;
    it.originArtist = song.artist;
    it.savePath = dir + QStringLiteral("/") + fileName;
    it.status = DsWaiting;
    it.addedAt = QDateTime::currentMSecsSinceEpoch();
    m_items.append(it);
    save();
    emit itemsChanged();
    emit message(QStringLiteral("已加入下载：%1 [%2]").arg(song.name, qualityName(q)));
    pump();
    return true;
}

void DownloadController::cancelDownload(const QString &id)
{
    const int idx = indexOf(id);
    if (idx < 0) return;
    auto &it = m_items[idx];
    if (it.status != DsWaiting && it.status != DsResolving && it.status != DsDownloading) return;

    // 令牌：贯穿整个 ladder，一旦置位，后续任何 resolve/http 回调都应丢弃
    if (auto h = m_handles.value(id, nullptr)) h->requestCancel();
    if (auto h = m_httpHandles.value(id, nullptr)) h->requestCancel();

    it.status = DsCancelled;
    it.finishedAt = QDateTime::currentMSecsSinceEpoch();
    m_handles.remove(id);
    m_httpHandles.remove(id);
    m_ladders.remove(id);
    m_cursor.remove(id);
    save();
    emit itemsChanged();
    pump();
}

void DownloadController::retryDownload(const QString &id)
{
    const int idx = indexOf(id);
    if (idx < 0) return;
    auto &it = m_items[idx];
    if (it.status != DsFailed && it.status != DsCancelled) return;
    QFile::remove(it.savePath);
    it.status = DsWaiting;
    it.received = 0;
    it.total = 0;
    it.error.clear();
    it.attemptsTried = 0;
    it.attemptNote.clear();
    it.finishedAt = 0;
    save();
    emit itemsChanged();
    pump();
}

void DownloadController::removeItem(const QString &id)
{
    const int idx = indexOf(id);
    if (idx < 0) return;
    if (auto h = m_handles.value(id, nullptr)) h->requestCancel();
    if (auto h = m_httpHandles.value(id, nullptr)) h->requestCancel();
    m_handles.remove(id);
    m_httpHandles.remove(id);
    m_ladders.remove(id);
    m_cursor.remove(id);
    m_items.remove(idx);
    save();
    emit itemsChanged();
    pump();
}

void DownloadController::removeAndDeleteFile(const QString &id)
{
    const int idx = indexOf(id);
    if (idx < 0) return;
    const QString path = m_items.at(idx).savePath;
    removeItem(id);
    if (!path.isEmpty() && QFile::exists(path)) QFile::remove(path);
}

void DownloadController::clearCompleted()
{
    for (int i = m_items.size() - 1; i >= 0; --i) {
        if (m_items.at(i).status == DsCompleted) m_items.remove(i);
    }
    save();
    emit itemsChanged();
}

QString DownloadController::itemPath(const QString &id) const
{
    const int idx = indexOf(id);
    return idx < 0 ? QString() : m_items.at(idx).savePath;
}

void DownloadController::revealInFolder(const QString &id) const
{
    const int idx = indexOf(id);
    if (idx < 0) return;
    const QString path = m_items.at(idx).savePath;
#ifdef Q_OS_WIN
    const QString native = QDir::toNativeSeparators(path);
    if (QFile::exists(path)) {
        QProcess::startDetached(QStringLiteral("explorer"),
                                { QStringLiteral("/select,"), native });
    } else {
        QDir d = QFileInfo(path).dir();
        QProcess::startDetached(QStringLiteral("explorer"),
                                { QDir::toNativeSeparators(d.absolutePath()) });
    }
#elif defined(Q_OS_MACOS)
    QProcess::startDetached(QStringLiteral("open"), { QStringLiteral("-R"), path });
#else
    QProcess::startDetached(QStringLiteral("xdg-open"), { QFileInfo(path).absolutePath() });
#endif
}

// ===========================================================================
// 队列驱动 & 重试阶梯
// ===========================================================================

void DownloadController::pump()
{
    int running = m_handles.size();
    if (running >= m_maxConcurrent) return;
    for (auto &it : m_items) {
        if (it.status != DsWaiting) continue;
        if (running >= m_maxConcurrent) break;
        startItem(it.id);
        ++running;
    }
}

void DownloadController::startItem(const QString &id)
{
    const int idx = indexOf(id);
    if (idx < 0) return;
    auto &it = m_items[idx];
    it.status = DsResolving;
    it.error.clear();
    it.attemptsTried = 0;
    it.attemptNote.clear();
    m_cursor[id] = 0;
    m_handles[id] = std::make_shared<DownloadHandle>();

    // 已有真实文件 → 直接完成
    if (QFileInfo::exists(it.savePath) && QFileInfo(it.savePath).size() > 1024) {
        it.status = DsCompleted;
        it.finishedAt = QDateTime::currentMSecsSinceEpoch();
        m_handles.remove(id);
        emit itemsChanged();
        emit rescanRequested();
        save();
        pump();
        return;
    }

    save();
    emit itemsChanged();

    buildLadder(id);
}

/// 在 worker 里做跨平台找歌，构造「音质档位 × 候选歌曲」的尝试序列
void DownloadController::buildLadder(const QString &id)
{
    const int idx = indexOf(id);
    if (idx < 0) return;
    Song original = m_items.at(idx).song;
    AudioQuality startQ = m_items.at(idx).requestedQuality;

    auto *watcher = new QFutureWatcher<QVector<DownloadAttempt>>(this);
    connect(watcher, &QFutureWatcher<QVector<DownloadAttempt>>::finished, this,
            [this, id, original, startQ, watcher]() {
        const QVector<DownloadAttempt> ladder = watcher->result();
        watcher->deleteLater();
        const int i = indexOf(id);
        if (i < 0) return;
        // 期间用户可能已取消：直接丢弃结果，不重建 ladder
        if (m_items[i].status == DsCancelled) return;
        auto handle = m_handles.value(id, nullptr);
        if (handle && handle->isCancelled()) return;
        m_ladders[id] = ladder;
        m_cursor[id] = 0;
        advanceAttempt(id);
    });

    MusicSdk *sdk = MusicSdk::instance();
    watcher->setFuture(QtConcurrent::run([sdk, original, startQ]() {
        QVector<DownloadAttempt> out;
        const auto chain = qualityFallbackChain(startQ);

        // 跨平台找同名歌（一次搜索，多个候选）
        FindMusicRequest req;
        req.name = original.name;
        req.singer = original.artist;
        req.albumName = original.album;
        req.interval = Format::duration(original.duration);
        const QString origCode = original.lx.source.isEmpty()
                                    ? platformSourceCode(original.platform)
                                    : original.lx.source;
        req.source = origCode;
        QVector<Song> crossPlat = sdk->findMusic(req, 6);
        // 剔除与原始同 source 的候选，避免重复
        for (int i = crossPlat.size() - 1; i >= 0; --i) {
            const QString c = crossPlat.at(i).lx.source.isEmpty()
                                  ? platformSourceCode(crossPlat.at(i).platform)
                                  : crossPlat.at(i).lx.source;
            if (c == origCode) crossPlat.removeAt(i);
        }

        // 阶梯：外层音质（从目标降），内层候选歌曲（先原歌、再跨平台）
        for (auto q : chain) {
            out.append({ original, q });
            for (const auto &c : crossPlat) out.append({ c, q });
        }
        return out;
    }));
}

void DownloadController::advanceAttempt(const QString &id)
{
    const int idx = indexOf(id);
    if (idx < 0) return;
    auto &it = m_items[idx];
    // 状态守卫：终态项不再推进
    if (it.status == DsCancelled || it.status == DsCompleted || it.status == DsFailed) return;
    auto handle = m_handles.value(id, nullptr);
    if (handle && handle->isCancelled()) return;

    const QVector<DownloadAttempt> &ladder = m_ladders[id];
    int &cur = m_cursor[id];
    if (cur >= ladder.size()) {
        // 全部尝试失败
        if (it.error.isEmpty()) it.error = QStringLiteral("所有音源/音质尝试均失败");
        finalize(id, DsFailed);
        return;
    }
    it.attemptsTried = cur + 1;
    // 展示当前进度提示：如「正在尝试：320k · 酷我」
    const DownloadAttempt &a = ladder.at(cur);
    const QString code = a.song.lx.source.isEmpty() ? platformSourceCode(a.song.platform) : a.song.lx.source;
    it.attemptNote = QStringLiteral("第 %1 次尝试：%2 · %3")
                        .arg(cur + 1)
                        .arg(qualityName(a.quality), platformName(platformFromSourceCode(code)));
    it.status = DsResolving;
    emit itemsChanged();

    attemptResolve(id);
}

void DownloadController::attemptResolve(const QString &id)
{
    const int idx = indexOf(id);
    if (idx < 0) return;
    const int cur = m_cursor.value(id, 0);
    const QVector<DownloadAttempt> &ladder = m_ladders[id];
    if (cur >= ladder.size()) { advanceAttempt(id); return; }
    const Song s = ladder.at(cur).song;
    const AudioQuality q = ladder.at(cur).quality;

    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this,
            [this, id, s, q, watcher]() {
        const QString url = watcher->result();
        watcher->deleteLater();
        const int i = indexOf(id);
        if (i < 0) return;
        if (m_items[i].status == DsCancelled || m_items[i].status == DsCompleted) return;
        auto handle = m_handles.value(id, nullptr);
        if (handle && handle->isCancelled()) return;

        if (url.isEmpty()) {
            m_cursor[id]++;
            advanceAttempt(id);
            return;
        }
        attemptHttp(id, url, s, q);
    });

    MusicSdk *sdk = MusicSdk::instance();
    watcher->setFuture(QtConcurrent::run([sdk, s, q]() {
        return sdk->resolveUrlAtQuality(s, q);
    }));
}

void DownloadController::attemptHttp(const QString &id, const QString &url,
                                     const Song &s, AudioQuality q)
{
    const int idx = indexOf(id);
    if (idx < 0) return;
    auto &it = m_items[idx];

    it.song = s;                    // 记录当前尝试的歌曲元数据（供展示/后续重下）
    it.actualQuality = q;
    it.actualPlatform = s.sourceCode();
    rebuildSavePath(it, q);         // 音质变了 → 扩展名可能要变

    it.status = DsDownloading;
    it.received = 0;
    it.total = 0;
    it.error.clear();
    emit itemsChanged();

    HttpOptions opt;
    opt.referer = refererForAudioUrl(url);
    opt.timeoutMs = 0;   // DownloadTask 内默认 60s
    auto httpHandle = std::make_shared<DownloadHandle>();
    m_httpHandles[id] = httpHandle;

    auto *self = this;
    HttpClient::instance()->downloadFile(url, it.savePath, opt,
        [self, id](qint64 got, qint64 total) {
            QMetaObject::invokeMethod(self, [self, id, got, total]() {
                const int i = self->indexOf(id);
                if (i < 0) return;
                auto &item = self->m_items[i];
                if (item.status != DsDownloading) return;
                item.received = got;
                if (total > 0) item.total = total;
                self->m_dirtyProgress = true;
                if (!self->m_progressTimer.isActive()) self->m_progressTimer.start();
            }, Qt::QueuedConnection);
        },
        [self, id, url](bool ok, const QString &err) {
            QMetaObject::invokeMethod(self, [self, id, url, ok, err]() {
                const int i = self->indexOf(id);
                if (i < 0) return;
                // 状态守卫：期间已取消/已完成则丢弃 HTTP 回调
                if (self->m_items[i].status == DsCancelled
                    || self->m_items[i].status == DsCompleted
                    || self->m_items[i].status == DsFailed) {
                    self->m_httpHandles.remove(id);
                    return;
                }
                auto handle = self->m_handles.value(id, nullptr);
                if (handle && handle->isCancelled()) {
                    self->m_httpHandles.remove(id);
                    return;
                }
                auto &item = self->m_items[i];

                // 失败：判断是否要继续下一 attempt
                bool contentBad = false;
                QString finalErr = err;
                if (ok) {
                    // HTTP 成功但内容无效（JSON 错误响应 / 文件过小）
                    QFile f(item.savePath);
                    if (f.open(QIODevice::ReadOnly)) {
                        const QByteArray head = f.read(16);
                        const qint64 sz = f.size();
                        f.close();
                        const bool isJson = head.startsWith('{') || head.startsWith('[');
                        if (sz <= 1024 || isJson) {
                            contentBad = true;
                            finalErr = QStringLiteral("音源返回的不是有效音频");
                            QFile::remove(item.savePath);
                        }
                    } else {
                        contentBad = true;
                        finalErr = QStringLiteral("无法读取下载文件");
                    }
                } else {
                    // HTTP 错误 → 清理部分文件
                    QFile::remove(item.savePath);
                }

                self->m_httpHandles.remove(id);

                if (ok && !contentBad) {
                    // 下载完成 → 先按实际结果写提示，再内嵌封面/歌词（worker），最后 finalize
                    item.status = DsCompleted;
                    item.received = QFileInfo(item.savePath).size();
                    item.total = item.received;
                    item.error.clear();
                    self->applyActualNote(item);
                    self->embedTagsThenFinalize(id);
                    return;
                }

                // 失败：记录错误，推进下一 attempt
                item.error = finalErr;
                self->m_cursor[id]++;
                self->advanceAttempt(id);
            }, Qt::QueuedConnection);
        },
        httpHandle);
}

void DownloadController::rebuildSavePath(DownloadItem &it, AudioQuality q)
{
    const QString ext = extForQuality(q);
    const QString currentPath = it.savePath;
    QFileInfo fi(currentPath);
    const QString dir = fi.absolutePath();
    if (dir.isEmpty()) return;
    const QString currentSuffix = QStringLiteral(".") + fi.suffix();
    const QString wantSuffix = QStringLiteral(".") + ext;
    if (currentSuffix.compare(wantSuffix, Qt::CaseInsensitive) == 0) return;

    // 从当前路径推出干净的 base（去掉可能带 (1) 后缀的话保留）
    QString base = fi.completeBaseName();
    it.savePath = dir + QStringLiteral("/") + uniqueBaseName(dir, base, ext);
}

// ---------------------------------------------------------------------------
// 完成收尾：实际结果提示 + 封面/歌词内嵌（设置可关）
// ---------------------------------------------------------------------------

namespace {
QString platformLabel(const QString &code)
{
    if (code == QLatin1String("wy")) return QStringLiteral("网易云音乐");
    if (code == QLatin1String("tx")) return QStringLiteral("QQ音乐");
    if (code == QLatin1String("kg")) return QStringLiteral("酷狗音乐");
    if (code == QLatin1String("kw")) return QStringLiteral("酷我音乐");
    if (code == QLatin1String("mg")) return QStringLiteral("咪咕音乐");
    return code.isEmpty() ? QStringLiteral("未知平台") : code;
}
} // namespace

void DownloadController::applyActualNote(DownloadItem &it) const
{
    const bool qChanged = it.actualQuality != it.requestedQuality;
    const bool pChanged = !it.requestedPlatform.isEmpty() && it.actualPlatform != it.requestedPlatform;
    const bool songChanged = !it.originName.isEmpty() && it.song.name != it.originName;
    if (!qChanged && !pChanged && !songChanged) {
        it.attemptNote.clear();
        return;
    }
    QStringList parts;
    if (qChanged || pChanged)
        parts << QStringLiteral("%1 · %2").arg(qualityName(it.actualQuality),
                                               platformLabel(it.actualPlatform));
    if (songChanged)
        parts << QStringLiteral("换源版本「%1 - %2」").arg(it.song.name, it.song.artist);
    it.attemptNote = QStringLiteral("实际下载：%1").arg(parts.join(QStringLiteral("，")));
}

void DownloadController::embedTagsThenFinalize(const QString &id)
{
    const int idx = indexOf(id);
    if (idx < 0) return;
    const DownloadItem item = m_items[idx];

    auto *store = DocumentStore::instance();
    const bool wantCover  = store->readSync(QStringLiteral("general"),
                                            QStringLiteral("embedCover"), true).toBool();
    const bool wantLyrics = store->readSync(QStringLiteral("general"),
                                            QStringLiteral("embedLyrics"), true).toBool();
    const QString suffix = QFileInfo(item.savePath).suffix().toLower();

    if ((!wantCover && !wantLyrics)
        || (suffix != QLatin1String("mp3") && suffix != QLatin1String("flac"))) {
        finalize(id, DsCompleted);
        return;
    }

    const Song song = item.song;
    const QString savePath = item.savePath;
    auto *w = new QFutureWatcher<void>(this);
    connect(w, &QFutureWatcher<void>::finished, this, [this, id, w]() {
        w->deleteLater();
        const int i = indexOf(id);
        if (i < 0) return;                              // 期间被移除
        if (m_items[i].status != DsCompleted) return;   // 期间被取消
        finalize(id, DsCompleted);
    });
    w->setFuture(QtConcurrent::run([song, savePath, wantCover, wantLyrics]() {
        TagWriter::Payload p;
        p.title = song.name;
        p.artist = song.artist;
        p.album = song.album;

        if (wantLyrics) {
            const SongLyric ly = MusicSdk::instance()->resolveLyric(song);
            p.lyrics = ly.rawLrc;   // 原文 LRC（翻译各播放器规范不一，暂不混入）
        }
        if (wantCover && song.cover.startsWith(QStringLiteral("http"))) {
            HttpOptions o;
            o.referer = refererForAudioUrl(song.cover);   // 按域名带防盗链 referer（kw 等缺了会 403）
            o.timeoutMs = 15000;
            const HttpResponse r = HttpClient::instance()->get(song.cover, o);
            if (r.ok && !r.body.isEmpty()) {
                QImage img = QImage::fromData(r.body);
                if (!img.isNull()) {
                    if (img.width() > 800 || img.height() > 800)
                        img = img.scaled(800, 800, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    QByteArray jpg;
                    QBuffer buf(&jpg);
                    buf.open(QIODevice::WriteOnly);
                    img.save(&buf, "JPEG", 85);
                    p.coverJpeg = jpg;
                }
            }
        }
        if (p.coverJpeg.isEmpty() && p.lyrics.isEmpty()) return;
        TagWriter::write(savePath, p);
    }));
}

void DownloadController::finalize(const QString &id, int newStatus, const QString &err)
{
    const int idx = indexOf(id);
    if (idx < 0) return;
    auto &it = m_items[idx];
    it.status = newStatus;
    if (!err.isEmpty()) it.error = err;
    it.finishedAt = QDateTime::currentMSecsSinceEpoch();

    m_handles.remove(id);
    m_httpHandles.remove(id);
    m_ladders.remove(id);
    m_cursor.remove(id);

    save();
    emit itemsChanged();
    if (newStatus == DsCompleted) emit rescanRequested();
    pump();
}

void DownloadController::flushProgress()
{
    if (!m_dirtyProgress) { m_progressTimer.stop(); return; }
    m_dirtyProgress = false;
    emit itemsChanged();
}

} // namespace Muyun
