#pragma once

#include "core/Types.h"


#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QStringList>
#include <QPointer>

namespace Muyun {

class LibraryController;
class SettingsController;
/// 一条 LX 音源脚本的信息
struct LxSourceInfo {
    QString id;
    QString name;
    QString version;
    QString author;
    QString description;
    QString scriptPath;
    bool enabled = true;

    QVariantMap toMap() const;
    static LxSourceInfo fromMap(const QVariantMap &m);
};

/**
 * @brief 设置控制器（本地音源目录 + 在线 LX 音源脚本管理）
 */
class SettingsController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QStringList folders READ folders NOTIFY foldersChanged)
    Q_PROPERTY(int localCount READ localCount NOTIFY localStatsChanged)
    Q_PROPERTY(QString lastScannedAt READ lastScannedAt NOTIFY localStatsChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY localStatsChanged)
    Q_PROPERTY(int tagPriority READ tagPriority WRITE setTagPriority NOTIFY tagPriorityChanged)

    Q_PROPERTY(QVariantList lxSources READ lxSources NOTIFY lxSourcesChanged)
    Q_PROPERTY(qint64 songCacheSize READ songCacheSize NOTIFY songCacheSizeChanged)
    Q_PROPERTY(qint64 otherCacheSize READ otherCacheSize NOTIFY otherCacheSizeChanged)
    Q_PROPERTY(QString activeLxSourceId READ activeLxSourceId NOTIFY lxSourcesChanged)
    Q_PROPERTY(QVariantMap lxUpdateAlert READ lxUpdateAlert NOTIFY lxUpdateAlertChanged)
    /// 是否允许显示更新提醒（用户可在设置里关闭）
    Q_PROPERTY(bool allowUpdateAlert READ allowUpdateAlert WRITE setAllowUpdateAlert NOTIFY allowUpdateAlertChanged)
    /// 关闭窗口行为："ask" 询问 / "minimize" 最小化到托盘 / "exit" 直接退出
    Q_PROPERTY(QString exitAction READ exitAction WRITE setExitAction NOTIFY exitActionChanged)
    /// 全屏播放界面样式："classic" 经典 / "amll" Apple Music / "mineradio" 舞台
    Q_PROPERTY(QString playerStyle READ playerStyle WRITE setPlayerStyle NOTIFY playerStyleChanged)
    /// 下载完成后是否把封面/歌词内嵌进音频文件（默认开）
    Q_PROPERTY(bool embedCover READ embedCover WRITE setEmbedCover NOTIFY embedChanged)
    Q_PROPERTY(bool embedLyrics READ embedLyrics WRITE setEmbedLyrics NOTIFY embedChanged)

public:
    explicit SettingsController(LibraryController *library, QObject *parent = nullptr);

    // ---- 本地音源 ----
    QStringList folders() const;
    int localCount() const;
    QString lastScannedAt() const;
    bool scanning() const;
    int tagPriority() const;
    void setTagPriority(int priority);

    Q_INVOKABLE void addFolder(const QString &path);
    Q_INVOKABLE void removeFolder(const QString &path);
    Q_INVOKABLE void rescan();
    Q_INVOKABLE QString dataPath() const;
    Q_INVOKABLE QString cachePath() const;
    qint64 songCacheSize() const;
    qint64 otherCacheSize() const;
    Q_INVOKABLE void clearSongCache();
    Q_INVOKABLE void clearOtherCache();

    // ---- 在线音源（LX 脚本）----
    QVariantList lxSources() const;
    QString activeLxSourceId() const;

    /// 从本地文件导入脚本
    Q_INVOKABLE bool importLxSourceFile(const QString &filePath);
    /// 从 URL 下载并导入
    Q_INVOKABLE void importLxSourceUrl(const QString &url);
    Q_INVOKABLE void removeLxSource(const QString &id);
    Q_INVOKABLE void setLxSourceEnabled(const QString &id, bool enabled);
    Q_INVOKABLE void setActiveLxSource(const QString &id);

    /// 音源脚本上报的更新推送信息（send('updateAlert')），无更新返回空 Map
    QVariantMap lxUpdateAlert() const { return m_updateAlert; }
    /// 用户已看到更新弹窗后调用，清空待展示的更新信息
    Q_INVOKABLE void dismissLxUpdateAlert() { m_updateAlert.clear(); emit lxUpdateAlertChanged(); }

    /// 主动触发更新检查：重新加载活跃脚本，让脚本有机会 send('updateAlert')
    Q_INVOKABLE void checkLxUpdate();
    /// 打开当前更新提醒里的更新地址（C++ 侧 QDesktopServices，比 QML openUrlExternally 可靠；
    /// 失败会 toast 提示，避免"点了没反应"无从判断）。返回是否成功发起。
    Q_INVOKABLE bool openLxUpdateUrl();

    bool allowUpdateAlert() const { return m_allowUpdateAlert; }
    void setAllowUpdateAlert(bool v);

    /// 通用轻提示（任意 QML 组件可调，经 message 信号 → 顶部 toast）
    Q_INVOKABLE void toast(const QString &text) { emit message(text); }

    // ---- 关闭行为 ----
    QString exitAction() const { return m_exitAction; }
    void setExitAction(const QString &action);

    // ---- 播放界面样式 ----
    QString playerStyle() const { return m_playerStyle; }
    void setPlayerStyle(const QString &style);

    // ---- 下载内嵌 ----
    bool embedCover() const { return m_embedCover; }
    void setEmbedCover(bool v);
    bool embedLyrics() const { return m_embedLyrics; }
    void setEmbedLyrics(bool v);

signals:
    void foldersChanged();
    void localStatsChanged();
    void tagPriorityChanged();
    void lxSourcesChanged();
    void songCacheSizeChanged();
    void otherCacheSizeChanged();
    void message(const QString &text);
    void importFailed(const QString &reason);
    void lxUpdateAlertChanged();
    void allowUpdateAlertChanged();
    void exitActionChanged();
    void playerStyleChanged();
    void embedChanged();

private:
    void loadLxSources();
    void saveLxSources();
    QString sourcesDir() const;
    /// 把当前活跃脚本同步加载到 MusicSdk 的 LX 引擎（内部走异步，见下）
    void syncActiveLxScript();
    /// 真正执行脚本加载（可能在 worker 线程）。返回是否成功 + 错误串。
    static bool doLoadLxScript(const QString &path, QString *err);
    /// 把活跃音源的名称/版本/描述合并进更新提醒 Map（脚本只 send log+updateUrl）
    void mergeActiveSourceMeta(QVariantMap &alert);

    QVector<LxSourceInfo> m_sources;
    QString m_activeId;
    LibraryController *m_library = nullptr;
    QVariantMap m_updateAlert;
    bool m_allowUpdateAlert = true;
    /// 音源脚本正在后台加载：期间禁止再次切换（防并发换引擎 / 连点错乱）。
    bool m_lxLoading = false;
    /// 加载进行中又收到新的目标 → 记下来，本轮完成后补做（避免"点了最后一个却没生效"）
    bool m_lxPendingReload = false;
    QString m_exitAction = QStringLiteral("ask");
    QString m_playerStyle = QStringLiteral("amll");
    bool m_embedCover = true;
    bool m_embedLyrics = true;
    QTimer m_cacheTimer;
    qint64 m_lastSongCacheSize = -1, m_lastOtherCacheSize = -1;
    void clearCacheInternal(const QStringList &dirs, const QString &label);
};

} // namespace Muyun
