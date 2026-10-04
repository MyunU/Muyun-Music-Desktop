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

    m_embedCover  = DocumentStore::instance()->readSync(
        QStringLiteral("general"), QStringLiteral("embedCover"), true).toBool();
    m_embedLyrics = DocumentStore::instance()->readSync(
        QStringLiteral("general"), QStringLiteral("embedLyrics"), true).toBool();
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

/// 应用真正的磁盘缓存位置（"cache" 目录 + 播放临时音频 + 舞台封面缓存）
QStringList cacheDirs()
{
    return {
        DocumentStore::instance()->cacheDir(),
        QDir::tempPath() + QStringLiteral("/muyun-audio"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/stage/web/covers"),
    };
}
} // namespace

qint64 SettingsController::cacheSize() const
{
    // 真实磁盘缓存：播放临时音频（大头）、舞台封面缓存、~/.muyun/cache。
    // 注意：正在播放的临时文件删不掉（Windows 文件锁），clearCache 会自动跳过。
    qint64 total = 0;
    for (const auto &d : cacheDirs())
        total += dirSize(d);
    return total;
}

void SettingsController::clearCache()
{
    int removed = 0;
    for (const auto &path : cacheDirs()) {
        QDir d(path);
        if (!d.exists()) continue;
        for (const auto &fi : d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
            const bool ok = fi.isDir() ? QDir(fi.absoluteFilePath()).removeRecursively()
                                       : QFile::remove(fi.absoluteFilePath());
            if (ok) ++removed;
        }
    }
    emit message(QStringLiteral("缓存已清除（%1 项，正在使用的文件已跳过）").arg(removed));
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
void SettingsController::syncActiveLxScript()
{
    // 找到活跃且启用的脚本
    QString activePath;
    for (const auto &s : m_sources) {
        if (s.id == m_activeId && s.enabled) {
            activePath = s.scriptPath;
            break;
        }
    }

    QString err;
    if (activePath.isEmpty()) {
        MusicSdk::instance()->loadLxScript(QString(), &err);
        m_updateAlert.clear();
        return;
    }
    if (!MusicSdk::instance()->loadLxScript(activePath, &err)) {
        qWarning() << "[Settings] LX 脚本加载失败:" << err;
        emit message(QStringLiteral("音源脚本加载失败：%1").arg(err));
        m_updateAlert.clear();
        return;
    }

    // 检查脚本是否上报了更新推送
    if (!m_allowUpdateAlert) return;
    m_updateAlert = MusicSdk::instance()->lxUpdateAlert();
    mergeActiveSourceMeta(m_updateAlert);
    if (!m_updateAlert.isEmpty())
        emit lxUpdateAlertChanged();
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
}

void SettingsController::checkLxUpdate()
{
    // 主动触发更新检查：重新加载活跃脚本，让脚本有机会 send('updateAlert')
    syncActiveLxScript();
    if (m_updateAlert.isEmpty()) {
        emit message(QStringLiteral("当前音源已是最新版本"));
    }
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
