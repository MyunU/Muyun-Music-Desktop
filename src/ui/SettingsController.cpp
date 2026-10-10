#include "SettingsController.h"

#include "ui/LibraryController.h"
#include "core/storage/DocumentStore.h"
#include "core/network/HttpClient.h"
#include "core/utils/Crypto.h"
#include "core/music/MusicSdk.h"
#include "core/music/lx/LxScriptEngine.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUuid>
#include <QUrl>
#include <QDesktopServices>
#include <QtConcurrent>
#include <QFutureWatcher>

namespace Muyun {

// ===========================================================================
// LxSourceInfo
// ===========================================================================

QVariantMap LxSourceInfo::toMap() const
{
    QVariantMap m;
    m[QStringLiteral("id")] = id;
    m[QStringLiteral("name")] = name;
    m[QStringLiteral("version")] = version;
    m[QStringLiteral("author")] = author;
    m[QStringLiteral("description")] = description;
    m[QStringLiteral("scriptPath")] = scriptPath;
    m[QStringLiteral("enabled")] = enabled;
    return m;
}

LxSourceInfo LxSourceInfo::fromMap(const QVariantMap &m)
{
    LxSourceInfo s;
    s.id = m.value(QStringLiteral("id")).toString();
    s.name = m.value(QStringLiteral("name")).toString();
    s.version = m.value(QStringLiteral("version")).toString();
    s.author = m.value(QStringLiteral("author")).toString();
    s.description = m.value(QStringLiteral("description")).toString();
    s.scriptPath = m.value(QStringLiteral("scriptPath")).toString();
    s.enabled = m.value(QStringLiteral("enabled"), true).toBool();
    return s;
}

// ===========================================================================
// 构造
// ===========================================================================

SettingsController::SettingsController(LibraryController *library, QObject *parent)
    : QObject(parent), m_library(library)
{
    if (m_library) {
        connect(m_library, &LibraryController::foldersChanged, this, &SettingsController::foldersChanged);
        connect(m_library, &LibraryController::localSongsChanged, this, &SettingsController::localStatsChanged);
    }

    // 注册 LX 脚本更新提醒回调：脚本可能异步 send('updateAlert')（晚于 loadScript 返回），
    // 通过回调立即感知，而非等下次 syncActiveLxScript 轮询。
    MusicSdk::instance()->setUpdateAlertCallback([this](const QVariantMap &info) {
        QMetaObject::invokeMethod(this, [this, info]() {
            if (!m_allowUpdateAlert) return;
            m_updateAlert = info;
            mergeActiveSourceMeta(m_updateAlert);
            emit lxUpdateAlertChanged();
        }, Qt::QueuedConnection);
    });

    loadLxSources();
    // 关闭窗口行为（ask/minimize/exit），持久化
    const QString exit = DocumentStore::instance()->readSync(
        QStringLiteral("general"), QStringLiteral("exitAction"), QStringLiteral("ask")).toString();
    m_exitAction = (exit == QStringLiteral("minimize") || exit == QStringLiteral("exit"))
                       ? exit : QStringLiteral("ask");

    const QString style = DocumentStore::instance()->readSync(
        QStringLiteral("general"), QStringLiteral("playerStyle"), QStringLiteral("amll")).toString();
    m_playerStyle = (style == QStringLiteral("classic") || style == QStringLiteral("mineradio"))
                        ? style : QStringLiteral("amll");

    m_lyricTranslation = DocumentStore::instance()->readSync(
        QStringLiteral("general"), QStringLiteral("lyricTranslation"), true).toBool();
    m_lyricRoman = DocumentStore::instance()->readSync(
        QStringLiteral("general"), QStringLiteral("lyricRoman"), false).toBool();

    m_embedCover  = DocumentStore::instance()->readSync(
        QStringLiteral("general"), QStringLiteral("embedCover"), true).toBool();
    m_embedLyrics = DocumentStore::instance()->readSync(
        QStringLiteral("general"), QStringLiteral("embedLyrics"), true).toBool();

    // 缓存大小定期刷：新缓存（下载音频 / 封面）加了要马上更新显示，
    // 别等下次进设置页才刷。只在值变了才 emit，避免无谓的 QML 重求值。
    m_cacheTimer.setInterval(3000);
    connect(&m_cacheTimer, &QTimer::timeout, this, [this]() {
        const qint64 s = songCacheSize(), o = otherCacheSize();
        bool ch = false;
        if (s != m_lastSongCacheSize) { m_lastSongCacheSize = s; ch = true; }
        if (o != m_lastOtherCacheSize) { m_lastOtherCacheSize = o; ch = true; }
        if (ch) { emit songCacheSizeChanged(); emit otherCacheSizeChanged(); }
    });
    m_cacheTimer.start();
}

void SettingsController::setEmbedCover(bool v)
{
    if (m_embedCover == v) return;
    m_embedCover = v;
    DocumentStore::instance()->write(QStringLiteral("general"),
                                     QStringLiteral("embedCover"), v);
    emit embedChanged();
}

void SettingsController::setEmbedLyrics(bool v)
{
    if (m_embedLyrics == v) return;
    m_embedLyrics = v;
    DocumentStore::instance()->write(QStringLiteral("general"),
                                     QStringLiteral("embedLyrics"), v);
    emit embedChanged();
}

void SettingsController::setPlayerStyle(const QString &style)
{
    const QString s = (style == QStringLiteral("classic") ||
                       style == QStringLiteral("mineradio"))
                          ? style
                          : QStringLiteral("amll");
    if (m_playerStyle == s) return;
    m_playerStyle = s;
    DocumentStore::instance()->write(QStringLiteral("general"),
                                     QStringLiteral("playerStyle"), s);
    emit playerStyleChanged();
}

void SettingsController::setLyricTranslation(bool v)
{
    if (m_lyricTranslation == v) return;
    m_lyricTranslation = v;
    DocumentStore::instance()->write(QStringLiteral("general"),
                                      QStringLiteral("lyricTranslation"), v);
    emit lyricSettingsChanged();
}

void SettingsController::setLyricRoman(bool v)
{
    if (m_lyricRoman == v) return;
    m_lyricRoman = v;
    DocumentStore::instance()->write(QStringLiteral("general"),
                                      QStringLiteral("lyricRoman"), v);
    emit lyricSettingsChanged();
}

// ===========================================================================
// 本地音源
// ===========================================================================

QStringList SettingsController::folders() const
{
    return m_library ? m_library->folders() : QStringList{};
}

int SettingsController::localCount() const
{
    return m_library ? m_library->localCount() : 0;
}

QString SettingsController::lastScannedAt() const
{
    return m_library ? m_library->lastScannedAt() : QString{};
}

bool SettingsController::scanning() const
{
    return m_library ? m_library->scanning() : false;
}

int SettingsController::tagPriority() const
{
    if (!m_library) return 0;
    return static_cast<int>(m_library->tagPriority());
}

void SettingsController::setTagPriority(int priority)
{
    if (!m_library) return;
    m_library->setTagPriority(static_cast<LibraryController::TagPriority>(priority));
    emit tagPriorityChanged();
}

void SettingsController::setExitAction(const QString &action)
{
    const QString a = (action == QStringLiteral("minimize") ||
                       action == QStringLiteral("exit"))
                          ? action
                          : QStringLiteral("ask");
    if (m_exitAction == a) return;
    m_exitAction = a;
    DocumentStore::instance()->write(QStringLiteral("general"),
                                     QStringLiteral("exitAction"), a);
    emit exitActionChanged();
}

void SettingsController::addFolder(const QString &path)
{
    if (path.isEmpty() || !m_library) return;
    m_library->addFolder(path);
}

void SettingsController::removeFolder(const QString &path)
{
    if (!m_library) return;
    m_library->removeFolder(path);
}

void SettingsController::rescan()
{
    if (!m_library) return;
    m_library->rescan();
}

QString SettingsController::dataPath() const
{
    return DocumentStore::instance()->rootPath();
}

QString SettingsController::cachePath() const
{
    return DocumentStore::instance()->cacheDir();
}

namespace {
/// 递归统计目录大小（限深防符号链接环）
qint64 dirSize(const QString &path, int depth = 4)
{
    QDir dir(path);
    if (!dir.exists() || depth <= 0) return 0;
    qint64 total = 0;
    for (const auto &fi : dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (fi.isDir()) total += dirSize(fi.absoluteFilePath(), depth - 1);
        else total += fi.size();
    }
    return total;
}

/// 递归统计"真能删掉"的大小：每个文件试开 RW，打不开（被占用）就不算。
/// clearCache 的 QFile::remove 同样会失败跳过 → 两边口径一致。
qint64 deletableSize(const QString &path, int depth = 4)
{
    QDir dir(path);
    if (!dir.exists() || depth <= 0) return 0;
    qint64 total = 0;
    for (const auto &fi : dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (fi.isDir()) { total += deletableSize(fi.absoluteFilePath(), depth - 1); continue; }
        QFile f(fi.absoluteFilePath());
        if (f.open(QIODevice::ReadWrite)) { f.close(); total += fi.size(); }
    }
    return total;
}

/// 字节 → 人读大小（"缓存占用" / "已清除" 两处共用）
QString friendlySize(qint64 bytes)
{
    if (bytes <= 0) return QStringLiteral("0 B");
    const char *u[] = {"B", "KB", "MB", "GB"};
    int i = 0;
    double v = bytes;
    while (v >= 1024 && i < 3) { v /= 1024; ++i; }
    return (i == 0 ? QStringLiteral("%1 B").arg(qint64(v))
                   : QStringLiteral("%1 %2").arg(v, 0, 'f', 1).arg(QLatin1String(u[i])));
}
QStringList songCacheDirs()
{
    // 歌曲音频缓存（大头）：在线歌下载到 %TEMP%/muyun-audio/
    return { QDir::tempPath() + QStringLiteral("/muyun-audio") };
}

QStringList otherCacheDirs()
{
    // 非歌曲缓存：封面（~/.muyun/cache + 舞台 web/covers）
    return {
        DocumentStore::instance()->cacheDir(),
        QCoreApplication::applicationDirPath() + QStringLiteral("/stage/web/covers"),
    };
}
} // namespace

qint64 SettingsController::songCacheSize() const
{
    // 只统计"真能删掉"的：每个文件试开 RW，打不开（被占用）就不算。
    // QFile::remove 同样需要写权限 → 两边口径一致。
    qint64 total = 0;
    for (const auto &d : songCacheDirs())
        total += deletableSize(d);
    return total;
}

qint64 SettingsController::otherCacheSize() const
{
    qint64 total = 0;
    for (const auto &d : otherCacheDirs())
        total += deletableSize(d);
    return total;
}

void SettingsController::clearSongCache()
{
    clearCacheInternal(songCacheDirs(), QStringLiteral("歌曲缓存"));
}

void SettingsController::clearOtherCache()
{
    clearCacheInternal(otherCacheDirs(), QStringLiteral("缓存"));
}

void SettingsController::clearCacheInternal(const QStringList &dirs, const QString &label)
{
    int removed = 0;
    qint64 freed = 0;
    for (const auto &path : dirs) {
        QDir d(path);
        if (!d.exists()) continue;
        for (const auto &fi : d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
            const bool ok = fi.isDir() ? QDir(fi.absoluteFilePath()).removeRecursively()
                                       : QFile::remove(fi.absoluteFilePath());
            if (ok) { ++removed; freed += fi.size(); }
        }
    }
    emit message(QStringLiteral("%1已清除（%2 项 / %3，正在使用的文件已跳过）")
                     .arg(label).arg(removed).arg(friendlySize(freed)));
    emit songCacheSizeChanged();
    emit otherCacheSizeChanged();
}

// ===========================================================================
// 在线音源（LX 脚本）
// ===========================================================================

QString SettingsController::sourcesDir() const
{
    return DocumentStore::instance()->rootPath() + QStringLiteral("/lx-sources");
}

QVariantList SettingsController::lxSources() const
{
    QVariantList out;
    for (const auto &s : m_sources) out.append(s.toMap());
    return out;
}

QString SettingsController::activeLxSourceId() const { return m_activeId; }

/// 把当前活跃脚本同步加载到 MusicSdk 的 LX 引擎
bool SettingsController::doLoadLxScript(const QString &path, QString *err)
{
    // 可能在 worker 线程执行：只碰 MusicSdk 的引擎加载（内部已用读写锁护住实例指针）。
    return MusicSdk::instance()->loadLxScript(path, err);
}

void SettingsController::syncActiveLxScript()
{
    // 找到活跃且启用的脚本路径
    QString activePath;
    for (const auto &s : m_sources) {
        if (s.id == m_activeId && s.enabled) {
            activePath = s.scriptPath;
            break;
        }
    }

    // ⚠ 主线程同步加载：QuickJS 必须在主线程创建（worker 线程加载 → 跨线程调用
    //   musicUrl 返回空 → 播放失败兜底酷我，用户实测确认）。加载期间 m_lxLoading=true
    //   供 QML 禁用切换/显示"加载中"；loadScript 内等待脚本异步逻辑时会泵事件循环
    //   （processEvents），UI 不会完全冻结。
    if (m_lxLoading) { m_lxPendingReload = true; return; }
    m_lxLoading = true;
    emit lxLoadingChanged();

    if (activePath.isEmpty()) {
        doLoadLxScript(QString(), nullptr);
        m_updateAlert.clear();
        m_lxLoading = false;
        emit lxLoadingChanged();
        emit lxSourcesChanged();
        return;
    }

    QString err;
    if (!doLoadLxScript(activePath, &err)) {
        qWarning() << "[Settings] LX 脚本加载失败:" << err;
        emit message(QStringLiteral("音源脚本加载失败：%1").arg(err));
        m_updateAlert.clear();
        emit lxUpdateAlertChanged();
    } else if (m_allowUpdateAlert) {
        m_updateAlert = MusicSdk::instance()->lxUpdateAlert();
        mergeActiveSourceMeta(m_updateAlert);
        if (!m_updateAlert.isEmpty()) emit lxUpdateAlertChanged();
    }
    m_lxLoading = false;
    emit lxLoadingChanged();
    emit lxSourcesChanged();   // 让抽屉里"当前音源"勾选/状态刷新

    // 加载期间用户又切了一次 → 用最新目标补做一轮
    if (m_lxPendingReload) {
        m_lxPendingReload = false;
        syncActiveLxScript();
    }
}

/// 把活跃音源的名称/版本/描述合并进更新提醒 Map。
/// 脚本侧 send('updateAlert', {log, updateUrl}) 只带推送内容，
/// 展示层还需要「哪个音源」的上下文，这里从本地登记信息补全。
void SettingsController::mergeActiveSourceMeta(QVariantMap &alert)
{
    if (alert.isEmpty()) return;
    for (const auto &s : m_sources) {
        if (s.id != m_activeId) continue;
        if (alert.value(QStringLiteral("name")).toString().isEmpty())
            alert[QStringLiteral("name")] = s.name;
        if (alert.value(QStringLiteral("version")).toString().isEmpty())
            alert[QStringLiteral("version")] = s.version;
        if (alert.value(QStringLiteral("description")).toString().isEmpty())
            alert[QStringLiteral("description")] = s.description;
        if (alert.value(QStringLiteral("author")).toString().isEmpty())
            alert[QStringLiteral("author")] = s.author;
        if (alert.value(QStringLiteral("scriptName")).toString().isEmpty())
            alert[QStringLiteral("scriptName")] = QFileInfo(s.scriptPath).fileName();
        break;
    }

    // ⚠ 更新地址归一化（修"点打开更新地址没反应"）：各家 LX 脚本 send('updateAlert') 的
    //   链接字段名不统一（updateUrl / url / link / downloadUrl），且有的值是裸域名
    //   （github.com/x/y）没有协议 → Qt.openUrlExternally 拿到空串或非 URL 直接无响应。
    QString u = alert.value(QStringLiteral("updateUrl")).toString();
    if (u.isEmpty()) u = alert.value(QStringLiteral("url")).toString();
    if (u.isEmpty()) u = alert.value(QStringLiteral("link")).toString();
    if (u.isEmpty()) u = alert.value(QStringLiteral("downloadUrl")).toString();
    u = u.trimmed();
    if (!u.isEmpty() && !u.contains(QLatin1Char(':')))     // 裸域名 → 补 https://
        u = QStringLiteral("https://") + u;
    alert[QStringLiteral("updateUrl")] = u;                 // 回写规范字段供 QML 直接读
}

void SettingsController::checkLxUpdate()
{
    // 主动触发更新检查：重新加载活跃脚本，让脚本有机会 send('updateAlert')
    syncActiveLxScript();
    if (m_updateAlert.isEmpty()) {
        emit message(QStringLiteral("当前音源已是最新版本"));
    }
}

bool SettingsController::openLxUpdateUrl()
{
    const QString u = m_updateAlert.value(QStringLiteral("updateUrl")).toString().trimmed();
    if (u.isEmpty()) { emit message(QStringLiteral("没有可打开的更新地址")); return false; }
    QUrl url(u);
    if (!url.isValid() || url.scheme().isEmpty()) {
        // 仍无协议（如纯中文/畸形串）→ 补 https 再试一次
        url = QUrl(QStringLiteral("https://") + u);
    }
    const bool ok = QDesktopServices::openUrl(url);
    if (!ok) emit message(QStringLiteral("无法打开更新地址：%1").arg(u));
    return ok;
}

void SettingsController::setAllowUpdateAlert(bool v)
{
    if (m_allowUpdateAlert == v) return;
    m_allowUpdateAlert = v;
    // 关闭时清空待展示的更新信息
    if (!v) {
        m_updateAlert.clear();
        emit lxUpdateAlertChanged();
    }
    emit allowUpdateAlertChanged();
}

void SettingsController::loadLxSources()
{
    auto *store = DocumentStore::instance();
    // 顺手清理历史遗留的临时导入文件（#10：旧版失败分支不删，_tmp_*.js 越积越多；
    // 现在导入已改走 %TEMP%，这里清一次用户机器上已有的残留）
    const QDir srcDir(sourcesDir());
    for (const auto &fi : srcDir.entryInfoList({QStringLiteral("_tmp_*.js")}, QDir::Files))
        QFile::remove(fi.absoluteFilePath());
    const QVariantMap doc = store->readAll(QStringLiteral("lx-sources"));
    m_sources.clear();
    for (const auto &item : doc.value(QStringLiteral("list")).toList()) {
        const LxSourceInfo s = LxSourceInfo::fromMap(item.toMap());
        if (s.id.isEmpty()) continue;
        m_sources.append(s);
    }
    m_activeId = doc.value(QStringLiteral("activeId")).toString();
    syncActiveLxScript();
}

void SettingsController::saveLxSources()
{
    auto *store = DocumentStore::instance();
    QVariantList arr;
    for (const auto &s : m_sources) arr.append(s.toMap());
    QVariantMap doc;
    doc[QStringLiteral("list")] = arr;
    doc[QStringLiteral("activeId")] = m_activeId;
    store->writeAll(QStringLiteral("lx-sources"), doc);
    emit lxSourcesChanged();
}

namespace {
/// 从脚本内容里提取 LX 音源头部元信息
bool parseScriptMeta(const QString &content, LxSourceInfo &info)
{
    // 形式一：@name / @version / @author / @description
    static const QRegularExpression rx(
        QStringLiteral("@(name|version|author|description)\\s+([^\\r\\n*]+)"),
        QRegularExpression::CaseInsensitiveOption);
    auto it = rx.globalMatch(content.left(4000));
    int found = 0;
    while (it.hasNext()) {
        const auto m = it.next();
        const QString key = m.captured(1).toLower();
        const QString val = m.captured(2).trimmed();
        if (key == QStringLiteral("name")) { info.name = val; ++found; }
        else if (key == QStringLiteral("version")) { info.version = val; ++found; }
        else if (key == QStringLiteral("author")) info.author = val;
        else if (key == QStringLiteral("description")) info.description = val;
    }

    // 形式二：sourceInfo / scriptInfo 对象里的 name/version
    if (info.name.isEmpty()) {
        static const QRegularExpression rx2(
            QStringLiteral("(?:sourceInfo|scriptInfo)[\\s\\S]{0,400}?name\\s*[:=]\\s*['\"]([^'\"]+)['\"]"));
        const auto m2 = rx2.match(content.left(6000));
        if (m2.hasMatch()) { info.name = m2.captured(1); ++found; }
    }
    if (info.name.isEmpty())
        info.name = QStringLiteral("未命名音源");
    if (info.version.isEmpty())
        info.version = QStringLiteral("1.0.0");
    return found > 0;
}
} // namespace

bool SettingsController::importLxSourceFile(const QString &filePath)
{
    const QString src = filePath.startsWith(QStringLiteral("file:///")) ? filePath.mid(8) : filePath;
    QFile f(src);
    if (!f.open(QIODevice::ReadOnly)) {
        emit importFailed(QStringLiteral("无法读取文件"));
        return false;
    }
    const QString content = QString::fromUtf8(f.readAll());
    f.close();

    // 有效性校验：**只认新版 globalThis.lx 协议**（挡掉 404 页/无效 JS）。
    // 旧格式（脚本侧 registerSource / userApi 宿主环境）我们没有实现过 shim，
    // 放进去只会在用户机器上"导入成功、播放全挂"，所以这里直接说清楚。
    const QString trimmed = content.trimmed();
    if (trimmed.isEmpty() || !LxScriptEngine::looksLikeLxScript(content)) {
        const bool looksLegacy =
            content.contains(QStringLiteral("registerSource"))
            || content.contains(QStringLiteral("userApi"));
        emit importFailed(looksLegacy
            ? QStringLiteral("暂不支持旧版音源脚本（registerSource / userApi），"
                             "请导入新版 globalThis.lx 格式的脚本")
            : QStringLiteral("不是有效的音源脚本（未识别到新版 globalThis.lx 特征）"));
        return false;
    }

    LxSourceInfo info;
    parseScriptMeta(content, info);
    info.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    info.scriptPath = sourcesDir() + QStringLiteral("/") + info.id + QStringLiteral(".js");

    QDir().mkpath(sourcesDir());
    QFile out(info.scriptPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit importFailed(QStringLiteral("无法写入音源目录"));
        return false;
    }
    out.write(content.toUtf8());
    out.close();

    const QString prevActive = m_activeId;
    m_sources.append(info);
    if (m_activeId.isEmpty()) m_activeId = info.id;
    saveLxSources();

    // 若新脚本会成为活跃音源 → 先验证 QuickJS 能加载，失败则回滚（避免"假导入成功"）
    if (m_activeId == info.id) {
        QString err;
        if (!MusicSdk::instance()->loadLxScript(info.scriptPath, &err)) {
            m_sources.removeLast();
            m_activeId = prevActive;
            QFile::remove(info.scriptPath);
            saveLxSources();
            syncActiveLxScript();   // 恢复原音源
            emit importFailed(QStringLiteral("音源脚本加载失败：%1").arg(err));
            return false;
        }
    }

    syncActiveLxScript();
    emit message(QStringLiteral("已导入音源：%1").arg(info.name));
    return true;
}

void SettingsController::importLxSourceUrl(const QString &url)
{
    // 临时文件写到系统临时目录而不是音源目录（#10：旧版失败分支不删，音源目录积了一堆
    // _tmp_*.js 垃圾；且"取目录里第一个 js"的自检可能挑中空文件报误导性失败）
    const QString savePath = QDir::tempPath() + QStringLiteral("/muyun_lx_import_") +
                             Crypto::randomHex(4) + QStringLiteral(".js");
    auto *self = this;
    HttpClient::instance()->downloadFile(url, savePath, HttpOptions(), nullptr,
        [self, savePath](bool ok, const QString &err) {
            QMetaObject::invokeMethod(self, [self, savePath, ok, err]() {
                if (!ok) {
                    QFile::remove(savePath);   // 下载失败也删（旧版漏了这行）
                    emit self->importFailed(err.isEmpty() ? QStringLiteral("下载失败") : err);
                    return;
                }
                self->importLxSourceFile(savePath);
                QFile::remove(savePath);
            }, Qt::QueuedConnection);
        });
}

void SettingsController::removeLxSource(const QString &id)
{
    for (int i = 0; i < m_sources.size(); ++i) {
        if (m_sources.at(i).id != id) continue;
        QFile::remove(m_sources.at(i).scriptPath);
        m_sources.removeAt(i);
        if (m_activeId == id) m_activeId = m_sources.isEmpty() ? QString() : m_sources.first().id;
        saveLxSources();
        syncActiveLxScript();
        return;
    }
}

void SettingsController::setLxSourceEnabled(const QString &id, bool enabled)
{
    for (auto &s : m_sources) {
        if (s.id != id) continue;
        s.enabled = enabled;
        saveLxSources();
        syncActiveLxScript();
        return;
    }
}

void SettingsController::setActiveLxSource(const QString &id)
{
    if (m_activeId == id) return;
    m_activeId = id;
    saveLxSources();
    syncActiveLxScript();
}

} // namespace Muyun
