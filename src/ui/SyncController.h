#pragma once

#include <QObject>
#include <QPointer>
#include <QHash>

class QTcpServer;
class QTcpSocket;
class QNetworkAccessManager;

namespace Muyun {

class LibraryController;
class LxSyncServer;

/**
 * @brief 局域网同步：把"在线收藏 + 自建歌单"在多台设备间互相同步
 *
 * 规则（按用户要求）：**只同步在线来源的收藏与歌单，本地音乐/本地收藏不参与**
 * （本地文件路径在别的设备上无意义）。
 *
 * 实现：内置一个极简 HTTP/1.1 服务（QTcpServer），暴露
 *   GET  /api/export  → 在线库 JSON
 *   POST /api/import  → 合并对端在线库（按 identityKey 去重）
 * 两端互相 pull/push 即可完成双向同步。
 *
 * 安全：仅监听、仅局域网手工会用；无鉴权（个人工具场景）。可在设置里开关。
 */
class SyncController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(int port READ port WRITE setPort NOTIFY portChanged)
    Q_PROPERTY(QString shareUrl READ shareUrl NOTIFY localAddressChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    explicit SyncController(LibraryController *library, QObject *parent = nullptr);
    ~SyncController() override;

    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);
    int port() const { return m_port; }
    void setPort(int p);
    QString shareUrl() const;
    bool busy() const { return m_busy; }

    /// 从对端拉取并合并（url 形如 http://192.168.1.5:8765）
    Q_INVOKABLE void pullFrom(const QString &url);
    /// 把本机在线库推送到对端
    Q_INVOKABLE void pushTo(const QString &url);
    /// 本机局域网 IP 列表（供展示/复制）
    Q_INVOKABLE QStringList lanAddresses() const;

    /// 洛雪兼容服务端（与暮云自有 /api 共端口；QML 绑定为 lxsync 上下文属性）
    LxSyncServer *lxServer() const { return m_lx; }

signals:
    void enabledChanged();
    void portChanged();
    void localAddressChanged();
    void busyChanged();
    void syncFinished(int added, const QString &detail);
    void syncFailed(const QString &error);

private slots:
    void onNewConnection();
    void onReadyRead();

private:
    /// 处理一个完整 HTTP 请求；返回 true = 已作为洛雪 WebSocket 移交，本类不再管该 socket
    bool handleRequest(QTcpSocket *sock, const QByteArray &request);
    void setBusy(bool b);
    QByteArray buildResponse(int code, const QByteArray &contentType, const QByteArray &body);

    LibraryController *m_library = nullptr;
    LxSyncServer *m_lx = nullptr;
    QTcpServer *m_server = nullptr;
    QNetworkAccessManager *m_nam = nullptr;
    QHash<QTcpSocket *, QByteArray> m_bufs;
    bool m_enabled = false;
    bool m_busy = false;
    int m_port = 8765;
};

} // namespace Muyun
