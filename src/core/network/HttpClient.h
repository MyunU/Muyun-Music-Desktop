#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QVariantMap>
#include <QByteArray>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>

namespace Muyun {

/// HTTP 响应结果
struct HttpResponse {
    int status = 0;
    bool ok = false;
    QByteArray body;
    QString bodyText;
    QVariantMap headers;      // 小写 header 名 -> 值
    QStringList setCookies;
    QString error;

    QString header(const QString &name) const {
        return headers.value(name).toString();
    }
};

/// 请求选项
struct HttpOptions {
    QVariantMap headers;
    QString userAgent;
    QString referer;
    QString cookie;
    int timeoutMs = 20000;
    bool followRedirects = true;
    /// 响应体按文本解码时使用的编码（默认 UTF-8）
    QString charset = QStringLiteral("UTF-8");
};

/// 一条正在进行的下载任务的取消令牌。
/// 主线程调用 requestCancel()，工作线程会在下一次进度回调时 abort 网络回复。
/// 通过 shared_ptr 传递给 downloadFile 与内部 DownloadTask 共享状态。
class DownloadHandle
{
public:
    DownloadHandle() = default;
    void requestCancel() { m_cancelled.store(true); }
    bool isCancelled() const { return m_cancelled.load(); }
private:
    std::atomic<bool> m_cancelled{false};
};

using DownloadHandlePtr = std::shared_ptr<DownloadHandle>;

/**
 * @brief 全局 HTTP 客户端
 *
 * 封装 QNetworkAccessManager，提供同步（阻塞，内部跑事件循环）与
 * 异步（回调，在工作线程执行）两种调用方式。音源解析、网易云接口等
 * 全部通过它发起请求。
 */
class HttpClient : public QObject
{
    Q_OBJECT
public:
    explicit HttpClient(QObject *parent = nullptr);

    static HttpClient *instance();

    /// 同步 GET（调用方应在非 UI 线程调用）
    HttpResponse get(const QString &url, const HttpOptions &opt = {});
    /// 同步 HEAD（只取响应头，如 Content-Length；非 UI 线程调用）
    HttpResponse head(const QString &url, const HttpOptions &opt = {});
    /// 同步 POST
    HttpResponse post(const QString &url, const QByteArray &data,
                      const HttpOptions &opt = {});
    HttpResponse postForm(const QString &url, const QVariantMap &form,
                          const HttpOptions &opt = {});

    /// 异步 GET：在内部工作线程执行，完成后通过回调回到调用线程
    void getAsync(const QString &url, const HttpOptions &opt,
                  std::function<void(HttpResponse)> cb);
    void postAsync(const QString &url, const QByteArray &data,
                   const HttpOptions &opt, std::function<void(HttpResponse)> cb);

    /// 下载文件到磁盘（带进度回调）
    /// 可选传入 cancel：主线程调用 cancel->requestCancel() 即可中止本次下载
    void downloadFile(const QString &url, const QString &savePath,
                      const HttpOptions &opt,
                      std::function<void(qint64, qint64)> progressCb,
                      std::function<void(bool, const QString &)> finishCb,
                      DownloadHandlePtr cancel = nullptr);

    /// 在指定的 manager 上执行一次请求（供异步任务在自有线程中调用）
    HttpResponse execRequest(QNetworkAccessManager *nam, const QString &url,
                             const QByteArray &data, bool isPost,
                             const HttpOptions &opt);
    /// op: 0=get, 1=post, 2=head
    HttpResponse execOp(QNetworkAccessManager *nam, const QString &url,
                        const QByteArray &data, int op,
                        const HttpOptions &opt);

    /// 取消某 URL 的所有进行中请求
    void cancelAll();

    /// 应用退出收尾：置"正在退出"标志。worker 线程里的同步/下载请求
    /// 会在 ≤200ms 内被中止（否则 main 退出时 QThreadPool::waitForDone()
    /// 会干等网络超时，播放中退出表现为"未响应"——见 HANDOFF 待办新问题②）。
    static void beginShutdown();
    static bool shuttingDown();

    /// 设置全局代理（空表示不使用代理）
    void setProxy(const QString &host, int port,
                  const QString &user = QString(),
                  const QString &password = QString());
    void clearProxy();

private:
    QNetworkAccessManager *m_nam = nullptr;
    static std::atomic<bool> s_shuttingDown;
};

/// URL 参数编码（与 JS encodeURIComponent 行为一致）
QString urlEncode(const QString &s);
QString buildQuery(const QVariantMap &params);
/// 表单序列化 application/x-www-form-urlencoded
QByteArray serializeForm(const QVariantMap &form);

} // namespace Muyun
