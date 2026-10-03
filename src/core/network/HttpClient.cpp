#include "HttpClient.h"

#include <QEventLoop>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QNetworkRequest>
#include <QNetworkProxy>
#include <QNetworkCookie>
#include <QThreadPool>
#include <QThread>
#include <QRunnable>
#include <QFile>
#include <QDir>
#include <QStringDecoder>
#include <QCoreApplication>
#include <QDebug>

namespace Muyun {

// ---------------------------------------------------------------------------
// 工具函数
// ---------------------------------------------------------------------------

QString urlEncode(const QString &s)
{
    return QString::fromUtf8(QUrl::toPercentEncoding(s));
}

QString buildQuery(const QVariantMap &params)
{
    QUrlQuery q;
    for (auto it = params.constBegin(); it != params.constEnd(); ++it) {
        if (it.value().userType() == QMetaType::QStringList) {
            for (const auto &v : it.value().toStringList())
                q.addQueryItem(it.key(), v);
        } else {
            q.addQueryItem(it.key(), it.value().toString());
        }
    }
    return q.toString(QUrl::FullyEncoded);
}

QByteArray serializeForm(const QVariantMap &form)
{
    QUrlQuery q;
    for (auto it = form.constBegin(); it != form.constEnd(); ++it)
        q.addQueryItem(it.key(), it.value().toString());
    return q.toString(QUrl::FullyEncoded).toUtf8();
}

// ---------------------------------------------------------------------------
// HttpClient
// ---------------------------------------------------------------------------

static HttpClient *s_instance = nullptr;

HttpClient::HttpClient(QObject *parent) : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
}

HttpClient *HttpClient::instance()
{
    if (!s_instance) s_instance = new HttpClient(qApp);
    return s_instance;
}

static void applyOptions(QNetworkRequest &req, const HttpOptions &opt)
{
    if (opt.followRedirects) {
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    } else {
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    }
    req.setRawHeader("Accept", "*/*");
    req.setRawHeader("Accept-Language", "zh-CN,zh;q=0.9,en;q=0.8");
    // 默认模拟浏览器 UA：多数音乐 CDN（qq/kuwo/163 等）会校验 UA 防盗链，
    // 不设置会导致播放/下载返回 403。原工程 lxmusic 也默认带浏览器 UA。
    if (!opt.userAgent.isEmpty())
        req.setRawHeader("User-Agent", opt.userAgent.toUtf8());
    else
        req.setRawHeader("User-Agent",
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
            "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36");
    if (!opt.referer.isEmpty())
        req.setRawHeader("Referer", opt.referer.toUtf8());
    if (!opt.cookie.isEmpty())
        req.setRawHeader("Cookie", opt.cookie.toUtf8());
    for (auto it = opt.headers.constBegin(); it != opt.headers.constEnd(); ++it)
        req.setRawHeader(it.key().toUtf8(), it.value().toString().toUtf8());
}

HttpResponse HttpClient::execRequest(QNetworkAccessManager *nam, const QString &url,
                                     const QByteArray &data, bool isPost,
                                     const HttpOptions &opt)
{
    return execOp(nam, url, data, isPost ? 1 : 0, opt);
}

// op: 0=get, 1=post, 2=head
HttpResponse HttpClient::execOp(QNetworkAccessManager *nam, const QString &url,
                                const QByteArray &data, int op,
                                const HttpOptions &opt)
{
    HttpResponse resp;
    QNetworkRequest req;
    req.setUrl(QUrl(url));
    applyOptions(req, opt);

    QNetworkReply *reply = (op == 1) ? nam->post(req, data)
                       : (op == 2) ? nam->head(req)
                       : nam->get(req);
    if (!reply) {
        resp.error = QStringLiteral("创建请求失败");
        return resp;
    }

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    bool timedOut = false;

    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, reply, [&]() { timedOut = true; });
    timer.start(opt.timeoutMs);
    loop.exec();

    if (timedOut) {
        reply->abort();
        reply->deleteLater();
        resp.error = QStringLiteral("请求超时");
        return resp;
    }
    timer.stop();

    resp.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    for (const auto &h : reply->rawHeaderList())
        resp.headers.insert(QString::fromUtf8(h).toLower(),
                            QString::fromUtf8(reply->rawHeader(h)));

    const QByteArray setCookie = reply->rawHeader("Set-Cookie");
    if (!setCookie.isEmpty())
        resp.setCookies.append(QString::fromUtf8(setCookie));
    for (const auto &c : reply->header(QNetworkRequest::SetCookieHeader)
                             .value<QList<QNetworkCookie>>()) {
        resp.setCookies.append(QString::fromUtf8(c.toRawForm()));
    }

    resp.body = reply->readAll();
    const auto err = reply->error();
    resp.error = err == QNetworkReply::NoError ? QString() : reply->errorString();
    reply->deleteLater();

    resp.ok = (err == QNetworkReply::NoError)
              && resp.status >= 200 && resp.status < 400;

    // 文本解码：优先按指定 charset，失败回退 UTF-8
    QStringDecoder decoder(opt.charset.toUtf8().constData());
    if (decoder.isValid())
        resp.bodyText = decoder.decode(resp.body);
    else
        resp.bodyText = QString::fromUtf8(resp.body);

    return resp;
}

HttpResponse HttpClient::get(const QString &url, const HttpOptions &opt)
{
    // 音源解析等工作线程会调用同步接口：跨线程时必须用局部 manager，
    // 否则 QNetworkReply 会以主线程对象为父，触发 Qt 线程警告甚至崩溃。
    if (QThread::currentThread() == thread())
        return execRequest(m_nam, url, QByteArray(), false, opt);

    QNetworkAccessManager local;
    return execRequest(&local, url, QByteArray(), false, opt);
}

HttpResponse HttpClient::post(const QString &url, const QByteArray &data,
                              const HttpOptions &opt)
{
    if (QThread::currentThread() == thread())
        return execRequest(m_nam, url, data, true, opt);

    QNetworkAccessManager local;
    return execRequest(&local, url, data, true, opt);
}

HttpResponse HttpClient::head(const QString &url, const HttpOptions &opt)
{
    if (QThread::currentThread() == thread())
        return execOp(m_nam, url, QByteArray(), 2, opt);
    QNetworkAccessManager local;
    return execOp(&local, url, QByteArray(), 2, opt);
}

HttpResponse HttpClient::postForm(const QString &url, const QVariantMap &form,
                                  const HttpOptions &opt)
{
    HttpOptions o = opt;
    o.headers[QStringLiteral("Content-Type")] =
        QStringLiteral("application/x-www-form-urlencoded");
    return post(url, serializeForm(form), o);
}

namespace {

class AsyncRequestTask : public QRunnable
{
public:
    QString url;
    QByteArray data;
    bool isPost;
    HttpOptions opt;
    std::function<void(HttpResponse)> cb;

    void run() override
    {
        QNetworkAccessManager nam;
        HttpResponse resp = HttpClient::instance()->execRequest(&nam, url, data,
                                                               isPost, opt);
        if (cb) cb(resp);
    }
};

class DownloadTask : public QRunnable
{
public:
    QString url;
    QString savePath;
    HttpOptions opt;
    std::function<void(qint64, qint64)> progressCb;
    std::function<void(bool, const QString &)> finishCb;
    DownloadHandlePtr cancel;

    void run() override
    {
        // 入队后可能已被取消（如快速移除），立即返回
        if (cancel && cancel->isCancelled()) {
            QFile::remove(savePath);
            if (finishCb) finishCb(false, QStringLiteral("已取消"));
            return;
        }

        QNetworkAccessManager nam;
        QNetworkRequest req;
        req.setUrl(QUrl(url));
        applyOptions(req, opt);
        QNetworkReply *reply = nam.get(req);
        if (!reply) {
            if (finishCb) finishCb(false, QStringLiteral("创建请求失败"));
            return;
        }

        QFile out(savePath);
        if (!out.open(QIODevice::WriteOnly)) {
            reply->abort();
            reply->deleteLater();
            if (finishCb) finishCb(false, QStringLiteral("无法写入文件"));
            return;
        }

        QEventLoop loop;
        bool finished = false;
        QObject::connect(reply, &QNetworkReply::finished, &loop, [&]() {
            finished = true;
            loop.quit();
        });
        QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&]() {
            out.write(reply->readAll());
        });
        QObject::connect(reply, &QNetworkReply::downloadProgress, &loop,
                         [&](qint64 got, qint64 total) {
                             if (cancel && cancel->isCancelled()) reply->abort();
                             if (progressCb) progressCb(got, total);
                         });
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, reply, &QNetworkReply::abort);
        timer.start(opt.timeoutMs > 0 ? opt.timeoutMs : 60000);

        if (!finished) loop.exec();
        out.write(reply->readAll());
        out.close();

        const bool userCancelled = cancel && cancel->isCancelled();
        if (userCancelled) {
            reply->deleteLater();
            QFile::remove(savePath);
            if (finishCb) finishCb(false, QStringLiteral("已取消"));
            return;
        }

        const bool ok = reply->error() == QNetworkReply::NoError;
        const QString err = ok ? QString() : reply->errorString();
        reply->deleteLater();
        if (finishCb) finishCb(ok, err);
    }
};

} // namespace

void HttpClient::getAsync(const QString &url, const HttpOptions &opt,
                          std::function<void(HttpResponse)> cb)
{
    auto *task = new AsyncRequestTask();
    task->url = url;
    task->isPost = false;
    task->opt = opt;
    task->cb = cb;
    task->setAutoDelete(true);
    QThreadPool::globalInstance()->start(task);
}

void HttpClient::postAsync(const QString &url, const QByteArray &data,
                           const HttpOptions &opt,
                           std::function<void(HttpResponse)> cb)
{
    auto *task = new AsyncRequestTask();
    task->url = url;
    task->data = data;
    task->isPost = true;
    task->opt = opt;
    task->cb = cb;
    task->setAutoDelete(true);
    QThreadPool::globalInstance()->start(task);
}

void HttpClient::downloadFile(const QString &url, const QString &savePath,
                              const HttpOptions &opt,
                              std::function<void(qint64, qint64)> progressCb,
                              std::function<void(bool, const QString &)> finishCb,
                              DownloadHandlePtr cancel)
{
    auto *task = new DownloadTask();
    task->url = url;
    task->savePath = savePath;
    task->opt = opt;
    task->progressCb = progressCb;
    task->finishCb = finishCb;
    task->cancel = std::move(cancel);
    task->setAutoDelete(true);
    QThreadPool::globalInstance()->start(task);
}

void HttpClient::cancelAll()
{
    // QNetworkAccessManager 无法枚举活动请求，这里通过断开所有连接实现
    m_nam->deleteLater();
    m_nam = new QNetworkAccessManager(this);
}

void HttpClient::setProxy(const QString &host, int port, const QString &user,
                          const QString &password)
{
    QNetworkProxy proxy;
    proxy.setType(QNetworkProxy::HttpProxy);
    proxy.setHostName(host);
    proxy.setPort(static_cast<quint16>(port));
    if (!user.isEmpty()) proxy.setUser(user);
    if (!password.isEmpty()) proxy.setPassword(password);
    m_nam->setProxy(proxy);
}

void HttpClient::clearProxy()
{
    m_nam->setProxy(QNetworkProxy::NoProxy);
}

} // namespace Muyun
