#include "UpdateChecker.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

#include "core/storage/DocumentStore.h"

namespace Muyun {

// ⚠ 开源后改这一处：GitHub 仓库坐标（version.json 放在仓库根目录、默认分支 main）
//    格式就是网页地址 github.com/<这段>：owner/仓库名（不要 https://、不要 .git、不是 SSH 形式）
static const QString kRepoSlug = QStringLiteral("MyunU/Muyun-Music-Desktop");
static const QString kBranch   = QStringLiteral("main");

/// 设置文档（与 exitAction / playerStyle 同一个 general 文档）
static const QString kDoc = QStringLiteral("general");
static const QString kKeyIgnored   = QStringLiteral("updateIgnoredVersion");
static const QString kKeyLastCheck = QStringLiteral("updateLastCheckAt");
static const QString kKeyAuto      = QStringLiteral("updateAutoCheck");

// 2026-10-04 用户拍板（HANDOFF 待办 #17）：节流从"每天最多一次"改成"每次进程启动查一次"。
// kAutoIntervalMs / kKeyLastCheck 的时间戳不再参与节流判定（写入保留，供诊断）。

static QString rawFeedUrl()
{
    return QStringLiteral("https://raw.githubusercontent.com/%1/%2/version.json")
        .arg(kRepoSlug, kBranch);
}

static QString releaseApiUrl()
{
    return QStringLiteral("https://api.github.com/repos/%1/releases/latest").arg(kRepoSlug);
}

// ---------------------------------------------------------------------------
// 纯逻辑
// ---------------------------------------------------------------------------

QString UpdateChecker::normalizeVersion(const QString &v)
{
    QString s = v.trimmed();
    if (s.startsWith(QLatin1Char('v')) || s.startsWith(QLatin1Char('V'))) s = s.mid(1);
    // 丢掉预发布/构建元数据：1.2.3-beta.1 → 1.2.3、1.2.3+win → 1.2.3
    const int cut = s.indexOf(QRegularExpression(QStringLiteral("[-+]")));
    if (cut >= 0) s = s.left(cut);
    s = s.trimmed();
    if (s.isEmpty()) return QString();
    bool anyDigit = false;
    for (const QChar c : s) if (c.isDigit()) anyDigit = true;
    return anyDigit ? s : QString();
}

int UpdateChecker::compareVersion(const QString &a, const QString &b)
{
    auto segments = [](const QString &v) {
        QVector<int> out;
        const QString s = normalizeVersion(v);
        const QStringList parts = s.split(QLatin1Char('.'), Qt::SkipEmptyParts);
        for (const QString &p : parts) {
            int n = 0;
            bool got = false;
            for (const QChar c : p) {
                if (!c.isDigit()) break;
                n = n * 10 + c.digitValue();
                got = true;
            }
            out.append(got ? n : 0);
        }
        return out;
    };
    const QVector<int> x = segments(a);
    const QVector<int> y = segments(b);
    if (x.isEmpty() && y.isEmpty()) return 0;
    const int n = qMax(x.size(), y.size());
    for (int i = 0; i < n; ++i) {
        const int xi = i < x.size() ? x.at(i) : 0;
        const int yi = i < y.size() ? y.at(i) : 0;
        if (xi != yi) return xi > yi ? 1 : -1;
    }
    return 0;
}

bool UpdateChecker::shouldNotify(const QString &latest, const QString &current,
                                 const QString &ignored)
{
    const QString l = normalizeVersion(latest);
    if (l.isEmpty()) return false;
    if (compareVersion(l, current) <= 0) return false;          // 不比本地新
    if (!ignored.isEmpty() && compareVersion(l, ignored) <= 0)  // 已被用户"不再提醒"
        return false;
    return true;
}

/// 从若干候选字段名里取第一个非空字符串
static QString pickString(const QJsonObject &o, const QStringList &keys)
{
    for (const QString &k : keys) {
        const QJsonValue v = o.value(k);
        if (v.isString() && !v.toString().trimmed().isEmpty()) return v.toString().trimmed();
    }
    return QString();
}

bool UpdateChecker::parseFeedJson(const QByteArray &body, UpdateInfo *out, QString *error)
{
    auto fail = [error](const QString &msg) {
        if (error) *error = msg;
        return false;
    };
    if (!out) return fail(QStringLiteral("内部错误：out 为空"));

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject())
        return fail(QStringLiteral("不是合法的 JSON 对象（%1）").arg(perr.errorString()));

    const QJsonObject o = doc.object();
    UpdateInfo info;
    // version.json 的 version ↔ GitHub Releases 的 tag_name
    info.version = normalizeVersion(pickString(o, { QStringLiteral("version"),
                                                    QStringLiteral("tag_name"),
                                                    QStringLiteral("tag"),
                                                    QStringLiteral("latest") }));
    if (info.version.isEmpty()) return fail(QStringLiteral("清单里没有可用的版本号"));

    info.notes    = pickString(o, { QStringLiteral("notes"), QStringLiteral("body"),
                                    QStringLiteral("changelog"), QStringLiteral("releaseNotes"),
                                    QStringLiteral("description") });
    info.pubDate  = pickString(o, { QStringLiteral("pubDate"), QStringLiteral("pub_date"),
                                    QStringLiteral("published_at"), QStringLiteral("date") });
    info.pageUrl  = pickString(o, { QStringLiteral("pageUrl"), QStringLiteral("html_url"),
                                    QStringLiteral("releaseUrl"), QStringLiteral("page_url"),
                                    QStringLiteral("url") });

    // 安装包直链：优先 version.json 的显式字段，其次 GitHub Release 资产里挑安装包
    info.downloadUrl = pickString(o, { QStringLiteral("downloadUrl"),
                                       QStringLiteral("download_url") });
    if (info.downloadUrl.isEmpty()) {
        const QJsonArray assets = o.value(QStringLiteral("assets")).toArray();
        QString firstExe;
        for (const QJsonValue &av : assets) {
            const QJsonObject a = av.toObject();
            const QString url = a.value(QStringLiteral("browser_download_url")).toString();
            const QString name = a.value(QStringLiteral("name")).toString();
            if (url.isEmpty()) continue;
            if (name.contains(QStringLiteral("setup"), Qt::CaseInsensitive)) {
                info.downloadUrl = url;   // 安装器优先
                break;
            }
            if (firstExe.isEmpty() && name.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive))
                firstExe = url;
        }
        if (info.downloadUrl.isEmpty()) info.downloadUrl = firstExe;
    }
    if (info.pageUrl.isEmpty() && !info.downloadUrl.isEmpty()) info.pageUrl = info.downloadUrl;

    *out = info;
    return true;
}

// ---------------------------------------------------------------------------
// UpdateChecker
// ---------------------------------------------------------------------------

QNetworkProxy UpdateChecker::systemProxyFor(const QUrl &url)
{
    // ① 先问 Qt 的系统代理（平台实现，Windows 上读用户 IE/WinINET 设置）
    const QList<QNetworkProxy> proxies =
        QNetworkProxyFactory::systemProxyForQuery(QNetworkProxyQuery(url));
    for (const QNetworkProxy &p : proxies) {
        if (p.type() != QNetworkProxy::NoProxy && p.type() != QNetworkProxy::DefaultProxy
            && !p.hostName().isEmpty())
            return p;
    }
#ifdef Q_OS_WIN
    // ② 兜底：直接读注册表里的用户代理设置（本机实测 WinHTTP 是"直连"、代理只配在 WinINET 里，
    //    而 QNetworkProxyFactory 有可能只认 WinHTTP → 读不到就等于"上不了 GitHub"）
    QSettings ie(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\"
                                "Internet Settings"),
                 QSettings::NativeFormat);
    if (ie.value(QStringLiteral("ProxyEnable"), 0).toInt() == 1) {
        QString server = ie.value(QStringLiteral("ProxyServer")).toString().trimmed();
        // 可能是 "host:port"，也可能形如 "http=host:port;https=host:port;socks=..."
        if (server.contains(QLatin1Char('='))) {
            QString picked;
            const QStringList parts = server.split(QLatin1Char(';'), Qt::SkipEmptyParts);
            for (const QString &part : parts) {
                const QString key = part.section(QLatin1Char('='), 0, 0).trimmed().toLower();
                if (key == QLatin1String("https") || key == QLatin1String("http")) {
                    picked = part.section(QLatin1Char('='), 1).trimmed();
                    if (key == QLatin1String("https")) break;
                }
            }
            server = picked;
        }
        const int colon = server.lastIndexOf(QLatin1Char(':'));
        if (colon > 0) {
            bool ok = false;
            const int port = server.mid(colon + 1).toInt(&ok);
            const QString host = server.left(colon);
            if (ok && port > 0 && !host.isEmpty())
                return QNetworkProxy(QNetworkProxy::HttpProxy, host, static_cast<quint16>(port));
        }
    }
#else
    Q_UNUSED(url)
#endif
    return QNetworkProxy(QNetworkProxy::NoProxy);
}

UpdateChecker::UpdateChecker(QObject *parent) : QObject(parent)
{
    m_current = QStringLiteral(MUYUN_VERSION);
    m_nam = new QNetworkAccessManager(this);
    auto *store = DocumentStore::instance();
    m_ignored  = store->readSync(kDoc, kKeyIgnored, QString()).toString();
    m_autoCheck = store->readSync(kDoc, kKeyAuto, true).toBool();
}

void UpdateChecker::setAutoCheckEnabled(bool on)
{
    if (m_autoCheck == on) return;
    m_autoCheck = on;
    DocumentStore::instance()->write(kDoc, kKeyAuto, on);
    emit autoCheckEnabledChanged();
}

void UpdateChecker::setStatus(const QString &s)
{
    if (m_status == s) return;
    m_status = s;
    emit stateChanged();
}

QString UpdateChecker::statusText() const
{
    const QString ver = QStringLiteral("v") + m_current;
    if (m_status == QStringLiteral("checking")) return QStringLiteral("正在检查更新…");
    if (m_status == QStringLiteral("uptodate")) return QStringLiteral("已是最新版本（%1）").arg(ver);
    if (m_status == QStringLiteral("available"))
        return QStringLiteral("发现新版本 v%1").arg(m_latest.version);
    if (m_status == QStringLiteral("ignored"))
        return QStringLiteral("v%1 已被忽略（不再自动提醒）").arg(m_latest.version);
    if (m_status == QStringLiteral("snoozed"))
        return QStringLiteral("v%1 稍后再提醒（本会话不再自动弹）").arg(m_latest.version);
    if (m_status == QStringLiteral("failed"))
        return QStringLiteral("检查更新失败：%1").arg(m_failReason);
    return QStringLiteral("尚未检查更新");
}

void UpdateChecker::autoCheck()
{
    if (!m_autoCheck) return;
    if (!qEnvironmentVariableIsEmpty("MUYUN_UPDATE_DISABLE")) return;
    // 每次进程启动查一次：同一进程内只查一次（手动检查也计入），防止启动路径被触发多次时连查
    if (m_sessionChecked) return;
    startCheck(false);
}

void UpdateChecker::checkForUpdates()
{
    startCheck(true);
}

void UpdateChecker::startCheck(bool manual)
{
    if (m_checking) return;   // 同一时刻只跑一个
    m_sessionChecked = true;  // 进程内已查过（每次启动最多一次）
    m_checking = true;
    m_manual = manual;
    m_failReason.clear();
    setStatus(QStringLiteral("checking"));
    emit stateChanged();

    // 时间戳保留写入（诊断用），不再参与节流判定
    DocumentStore::instance()->write(kDoc, kKeyLastCheck,
                                     QDateTime::currentMSecsSinceEpoch());

    ++m_generation;

    // 自检/离线调试：把本地文件当清单，仍然走"工作线程读取 → 回主线程落地"的同一条链路
    const QString localFile = qEnvironmentVariable("MUYUN_UPDATE_FEED_FILE");
    if (!localFile.isEmpty()) {
        auto *watcher = new QFutureWatcher<QByteArray>(this);
        QObject::connect(watcher, &QFutureWatcher<QByteArray>::finished, this,
                         [this, watcher, manual]() {
            const QByteArray body = watcher->result();
            watcher->deleteLater();
            if (!m_checking) return;
            m_checking = false;
            if (body.isEmpty()) {
                finishFailed(QStringLiteral("清单文件读不到"));
                return;
            }
            applyFeedBody(body, manual);
        });
        watcher->setFuture(QtConcurrent::run([localFile]() {
            QFile f(localFile);
            if (!f.open(QIODevice::ReadOnly)) return QByteArray();
            return f.readAll();
        }));
        return;
    }

    const QString override = qEnvironmentVariable("MUYUN_UPDATE_FEED");
    const QStringList urls = override.isEmpty()
                                 ? QStringList{ rawFeedUrl(), releaseApiUrl() }
                                 : QStringList{ override };
    // 尝试链：代理 → 直连（同一地址各试一次），再换下一个地址。
    // 用户常把 Clash 开开关关：代理"配了但没开"必须能直连兜底，开了代理而直连被墙则走代理。
    const bool hasProxy = systemProxyFor(QUrl(urls.first())).type() != QNetworkProxy::NoProxy;
    m_attempts.clear();
    for (const QString &u : urls) {
        if (hasProxy) m_attempts.append(Attempt{ u, true });
        m_attempts.append(Attempt{ u, false });
    }
    tryNextAttempt();
}

void UpdateChecker::tryNextAttempt()
{
    if (m_attempts.isEmpty()) {
        m_checking = false;
        finishFailed(QStringLiteral("网络不可用或被拦截"));
        return;
    }
    fetchUrl(m_attempts.takeFirst());
}

void UpdateChecker::fetchUrl(const Attempt &attempt)
{
    const int gen = m_generation;
    const QString url = attempt.url;
    QNetworkRequest req{QUrl(url)};
    req.setRawHeader("Accept", "application/json, text/plain, */*");
    req.setRawHeader("Accept-Language", "zh-CN,zh;q=0.9,en;q=0.8");
    req.setRawHeader("User-Agent",
                     QStringLiteral("MuyunMusic/%1 (+https://github.com/%2)")
                         .arg(m_current, kRepoSlug).toUtf8());
    req.setRawHeader("Cache-Control", "no-cache");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(6000);   // 启动路径上的网络：宁可失败也不拖

    // 只有更新检查这一条按需走系统代理（音乐接口一直直连，别把 CDN 流量送进用户的代理）
    const QNetworkProxy proxy = attempt.useProxy ? systemProxyFor(req.url())
                                                 : QNetworkProxy(QNetworkProxy::NoProxy);
    m_nam->setProxy(proxy);
#ifdef MUYUN_SELFTES
    if (!qEnvironmentVariableIsEmpty("MUYUN_UPDATE_DEBUG"))
        qWarning("[update] url=%s proxy=%d %s:%d", qPrintable(url), int(proxy.type()),
                 qPrintable(proxy.hostName()), int(proxy.port()));
#endif

    QNetworkReply *reply = m_nam->get(req);
    QPointer<UpdateChecker> guard(this);
    QObject::connect(reply, &QNetworkReply::finished, this, [guard, reply, url, gen]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QNetworkReply::NetworkError nerr = reply->error();
        const QString errStr = reply->errorString();
        const bool ok = nerr == QNetworkReply::NoError && status >= 200 && status < 300;
        const QByteArray body = ok ? reply->readAll() : QByteArray();
        reply->deleteLater();

        if (!guard) return;
        UpdateChecker *self = guard.data();
        if (gen != self->m_generation || !self->m_checking) return;   // 过期回调

        if (!ok || body.isEmpty()) {
            QString reason = errStr;
            if (status > 0) {
                reason = QStringLiteral("HTTP %1%2").arg(status).arg(
                    errStr.isEmpty() ? QString() : QStringLiteral("（%1）").arg(errStr));
            }
            self->m_failReason = url.contains(QStringLiteral("api.github.com"))
                                     ? QStringLiteral("网络不可用或被拦截：%1").arg(
                                           reason.isEmpty() ? QStringLiteral("无响应") : reason)
                                     : (reason.isEmpty() ? QStringLiteral("无响应") : reason);
#ifdef MUYUN_SELFTES
            if (!qEnvironmentVariableIsEmpty("MUYUN_UPDATE_DEBUG"))
                qWarning("[update] 失败，换下一条：%s", qPrintable(self->m_failReason));
#endif
            self->tryNextAttempt();   // 代理↔直连 / raw↔API 继续兜底
            return;
        }
        self->applyFeedBody(body, self->m_manual);
    });
}

void UpdateChecker::applyFeedBody(const QByteArray &body, bool manual)
{
    m_checking = false;
    UpdateInfo info;
    QString err;
    if (!parseFeedJson(body, &info, &err)) {
        m_failReason = err;
        setStatus(QStringLiteral("failed"));
        return;
    }
    m_latest = info;

    if (compareVersion(info.version, m_current) <= 0) {
        setStatus(QStringLiteral("uptodate"));
        return;
    }
    // 手动检查是用户主动问的：哪怕他忽略过这个版本，也要给结果
    const QString ignored = manual ? QString() : m_ignored;
    if (!shouldNotify(info.version, m_current, ignored)) {
        setStatus(QStringLiteral("ignored"));
        return;
    }
    // "稍后"（会话级，不落盘）：同版本或更旧不再自动弹；出新版本或手动检查会再弹
    if (!manual && !m_snoozedVersion.isEmpty()
        && compareVersion(info.version, m_snoozedVersion) <= 0) {
        setStatus(QStringLiteral("snoozed"));
        return;
    }
    setStatus(QStringLiteral("available"));
    emit updateFound(info.version, info.notes);
}

void UpdateChecker::snoozeLatestVersion()
{
    if (m_latest.version.isEmpty()) return;
    m_snoozedVersion = m_latest.version;
    if (m_status == QStringLiteral("available")) setStatus(QStringLiteral("snoozed"));
}

void UpdateChecker::finishFailed(const QString &reason)
{
    m_failReason = reason;
    setStatus(QStringLiteral("failed"));
}

void UpdateChecker::ignoreLatestVersion()
{
    if (m_latest.version.isEmpty()) return;
    if (m_ignored == m_latest.version) return;
    m_ignored = m_latest.version;
    DocumentStore::instance()->write(kDoc, kKeyIgnored, m_ignored);
    emit ignoredVersionChanged();
    if (m_status == QStringLiteral("available")) setStatus(QStringLiteral("ignored"));
}

void UpdateChecker::clearIgnoredVersion()
{
    if (m_ignored.isEmpty()) return;
    m_ignored.clear();
    DocumentStore::instance()->write(kDoc, kKeyIgnored, QString());
    emit ignoredVersionChanged();
}

void UpdateChecker::openDownloadPage()
{
    const QString url = !m_latest.downloadUrl.isEmpty() ? m_latest.downloadUrl
                                                        : m_latest.pageUrl;
    if (url.isEmpty()) return;
    QDesktopServices::openUrl(QUrl(url));
}

} // namespace Muyun
