#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QNetworkProxy>
#include <QUrl>
#include <QVector>

class QNetworkAccessManager;   // Qt 类：必须在 namespace 外前向声明（否则变成 Muyun::QNetworkAccessManager）

namespace Muyun {

/// 远程版本清单的解析结果（version.json 与 GitHub Releases API 两种格式共用）
struct UpdateInfo {
    QString version;      ///< 规范化后的版本号（已去掉 v 前缀），如 "1.1.0"
    QString notes;        ///< 更新说明（可含换行）
    QString pageUrl;      ///< 发布页（Releases 页面）
    QString downloadUrl;  ///< 安装包直链（可为空，为空时退化成打开发布页）
    QString pubDate;      ///< 发布时间（可选）

    bool valid() const { return !version.isEmpty(); }
};

/**
 * @brief 应用自身更新提示（开源方案 A：纯 GitHub，不建服务器、不做多源回退）
 *
 * 数据流：启动后台静默拉一次清单 → semver 比对 MUYUN_VERSION → 比本地新就发 updateFound
 * 让 QML 弹窗。所有失败一律静默（绝不阻塞启动、绝不打断播放），只在设置页显示状态文字。
 *
 * 清单地址固定在 GitHub 仓库（见 .cpp 顶部 kRepoSlug，开源后改这一处即可）：
 *   首选 https://raw.githubusercontent.com/<slug>/main/version.json（快、无 API 限额）
 *   兜底 https://api.github.com/repos/<slug>/releases/latest（解析 tag_name/body/html_url/assets）
 *
 * 两条自检友好设计（离线可复现，不依赖网络）：
 *   - 纯逻辑都是 static：compareVersion / shouldNotify / parseFeedJson 可直接断言；
 *   - 环境变量 MUYUN_UPDATE_FEED_FILE=<本地清单路径> 走"本地文件当清单"，
 *     且仍然走"工作线程读取 → 回主线程落地"的同一条链路。
 */
class UpdateChecker : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    /// idle / checking / uptodate / available / ignored / failed
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    /// 设置页直接显示的人话状态
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(bool checking READ checking NOTIFY stateChanged)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY stateChanged)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY stateChanged)
    Q_PROPERTY(QString releaseNotes READ releaseNotes NOTIFY stateChanged)
    Q_PROPERTY(QString pageUrl READ pageUrl NOTIFY stateChanged)
    Q_PROPERTY(QString downloadUrl READ downloadUrl NOTIFY stateChanged)
    /// 用户"不再提醒"过的版本（该版本永不再自动弹窗）
    Q_PROPERTY(QString ignoredVersion READ ignoredVersion NOTIFY ignoredVersionChanged)
    /// 启动时自动检查（默认开，可关；关掉后仍可手动检查）
    Q_PROPERTY(bool autoCheckEnabled READ autoCheckEnabled WRITE setAutoCheckEnabled
                   NOTIFY autoCheckEnabledChanged)

public:
    explicit UpdateChecker(QObject *parent = nullptr);

    // ---- 纯逻辑（static，自检直接调；不碰网络/磁盘）----
    /// 版本号规范化："v1.2.3" / " 1.2.3+build" / "1.2.3-beta.1" → "1.2.3"；无数字则返回空
    static QString normalizeVersion(const QString &v);
    /// semver 数值段比较：a>b 返回 >0，a==b 返回 0，a<b 返回 <0（段数不等时短的补 0）
    static int compareVersion(const QString &a, const QString &b);
    /// 是否该提醒：远端比本地新，且比"已忽略版本"新（已忽略的那个版本永不再提）
    static bool shouldNotify(const QString &latest, const QString &current,
                             const QString &ignored);
    /// 解析清单：同时认 version.json 与 GitHub Releases API 的字段名
    static bool parseFeedJson(const QByteArray &body, UpdateInfo *out,
                              QString *error = nullptr);
    /// 为某条 URL 解析"系统代理"（WinINET/IE 设置；Qt 默认不读它）。
    /// 国内用户常靠 Clash/v2ray 上 GitHub —— 不解析就永远是"检查更新失败"。
    static QNetworkProxy systemProxyFor(const QUrl &url);

    // ---- 状态 ----
    QString currentVersion() const { return m_current; }
    QString status() const { return m_status; }
    QString statusText() const;
    bool checking() const { return m_checking; }
    bool updateAvailable() const { return m_status == QStringLiteral("available"); }
    QString latestVersion() const { return m_latest.version; }
    QString releaseNotes() const { return m_latest.notes; }
    QString pageUrl() const { return m_latest.pageUrl; }
    QString downloadUrl() const { return m_latest.downloadUrl; }
    QString ignoredVersion() const { return m_ignored; }

    bool autoCheckEnabled() const { return m_autoCheck; }
    Q_INVOKABLE void setAutoCheckEnabled(bool on);

    // ---- 动作 ----
    /// 启动时调用：每次进程启动查一次（不再有"每天最多一次"节流），
    /// 受 autoCheckEnabled 开关约束；同一进程内只查一次；失败静默
    Q_INVOKABLE void autoCheck();
    /// 手动检查（无视节流；发现新版一定弹窗，哪怕用户曾忽略过该版本——是用户主动问的）
    Q_INVOKABLE void checkForUpdates();
    /// 打开下载页（优先安装包直链，没有就开发布页）
    Q_INVOKABLE void openDownloadPage();
    /// 不再提醒当前远端版本（按版本号记忽略，写进设置文档）
    Q_INVOKABLE void ignoreLatestVersion();
    /// 清掉"不再提醒"记录（设置页恢复提醒用）
    Q_INVOKABLE void clearIgnoredVersion();
    /// 弹窗"稍后"：本会话不再自动提醒当前版本（不落盘；下次启动/出新版本/手动检查会再弹）
    Q_INVOKABLE void snoozeLatestVersion();

    /// 拿到一段清单文本后的统一落地路径（网络/本地文件/自检共用）
    void applyFeedBody(const QByteArray &body, bool manual);

signals:
    void stateChanged();
    void ignoredVersionChanged();
    void autoCheckEnabledChanged();
    /// 发现新版本（QML 据此弹窗）
    void updateFound(const QString &version, const QString &notes);

private:
    /// 一次拉取尝试：清单地址 + 是否走系统代理
    struct Attempt {
        QString url;
        bool useProxy = false;
    };
    void startCheck(bool manual);
    void fetchUrl(const Attempt &attempt);
    /// 当前尝试失败 → 换下一条（代理↔直连、raw↔Releases API 的兜底链条）
    void tryNextAttempt();
    void finishFailed(const QString &reason);
    void setStatus(const QString &s);

    QVector<Attempt> m_attempts;
    bool m_manual = false;
    QString m_current;
    QString m_status = QStringLiteral("idle");
    QString m_failReason;
    QString m_ignored;
    QString m_snoozedVersion;   ///< "稍后"的版本：本会话内不再自动弹（不落盘）
    UpdateInfo m_latest;
    bool m_checking = false;
    bool m_sessionChecked = false;   ///< 本次进程已查过（每次启动只查一次）
    bool m_autoCheck = true;
    /// 自带 NAM（不复用全局 HttpClient）：只给"更新检查"这一条按系统代理走
    QNetworkAccessManager *m_nam = nullptr;
    /// 同一时刻只允许一个在途检查
    int m_generation = 0;
};

} // namespace Muyun
