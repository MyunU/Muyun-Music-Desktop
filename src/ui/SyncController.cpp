#include "SyncController.h"

#include "ui/LibraryController.h"
#include "ui/LxSyncServer.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QNetworkInterface>
#include <QHostAddress>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace Muyun {

SyncController::SyncController(LibraryController *library, QObject *parent)
    : QObject(parent), m_library(library)
{
    m_nam = new QNetworkAccessManager(this);
    m_lx = new LxSyncServer(library, this);
}

SyncController::~SyncController()
{
    if (m_server) m_server->close();
}

QStringList SyncController::lanAddresses() const
{
    QStringList out;
    const auto ifaces = QNetworkInterface::allInterfaces();
    for (const auto &iface : ifaces) {
        if (!(iface.flags() & QNetworkInterface::IsUp) ||
            !(iface.flags() & QNetworkInterface::CanBroadcast) ||
            (iface.flags() & QNetworkInterface::IsLoopBack))
            continue;
        const auto entries = iface.addressEntries();
        for (const auto &e : entries) {
            const QHostAddress addr = e.ip();
            if (addr.protocol() == QAbstractSocket::IPv4Protocol && !addr.isLoopback())
                out << addr.toString();
        }
    }
    return out;
}

QString SyncController::shareUrl() const
{
    const QStringList ips = lanAddresses();
    if (ips.isEmpty()) return QStringLiteral("http://本机:%1").arg(m_port);
    return QStringLiteral("http://%1:%2").arg(ips.first()).arg(m_port);
}

void SyncController::setEnabled(bool on)
{
    if (on == m_enabled) return;
    m_enabled = on;
    if (on) {
        if (!m_server) m_server = new QTcpServer(this);
        m_server->close();
        connect(m_server, &QTcpServer::newConnection, this, &SyncController::onNewConnection,
                Qt::UniqueConnection);
        if (!m_server->listen(QHostAddress::Any, static_cast<quint16>(m_port))) {
            m_enabled = false;
            emit syncFailed(QStringLiteral("端口 %1 监听失败：%2")
                                .arg(m_port).arg(m_server->errorString()));
        }
    } else if (m_server) {
        if (m_lx) m_lx->closeAllSessions();
        m_server->close();
    }
    emit enabledChanged();
    emit localAddressChanged();
}

void SyncController::setPort(int p)
{
    if (p == m_port || p <= 0 || p > 65535) return;
    m_port = p;
    if (m_enabled) { setEnabled(false); setEnabled(true); }
    emit portChanged();
    emit localAddressChanged();
}

void SyncController::setBusy(bool b)
{
    if (b == m_busy) return;
    m_busy = b;
    emit busyChanged();
}

QByteArray SyncController::buildResponse(int code, const QByteArray &contentType, const QByteArray &body)
{
    const char *status = code == 200 ? "OK" : (code == 404 ? "Not Found" : "Bad Request");
    QByteArray r;
    r += "HTTP/1.1 " + QByteArray::number(code) + " " + status + "\r\n";
    r += "Content-Type: " + contentType + "\r\n";
    r += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    r += "Access-Control-Allow-Origin: *\r\n";
    r += "Connection: close\r\n\r\n";
    r += body;
    return r;
}

void SyncController::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket *sock = m_server->nextPendingConnection();
        m_bufs.insert(sock, QByteArray());
        connect(sock, &QTcpSocket::readyRead, this, &SyncController::onReadyRead);
        connect(sock, &QTcpSocket::disconnected, this, [this, sock]() {
            m_bufs.remove(sock);
            sock->deleteLater();
        });
    }
}

void SyncController::onReadyRead()
{
    auto *sock = qobject_cast<QTcpSocket *>(sender());
    if (!sock) return;
    m_bufs[sock] += sock->readAll();
    const QByteArray &buf = m_bufs[sock];

    const int headerEnd = buf.indexOf("\r\n\r\n");
    if (headerEnd < 0) return;   // 头还没收全
    const QByteArray headers = buf.left(headerEnd);
    int contentLength = 0;
    for (const QByteArray &line : headers.split('\n')) {
        if (line.toLower().startsWith("content-length:"))
            contentLength = line.mid(15).trimmed().toInt();
    }
    const int bodyStart = headerEnd + 4;
    if (buf.size() - bodyStart < contentLength) return;   // body 未收全

    const bool adopted = handleRequest(sock, buf);
    if (!adopted) m_bufs[sock].clear();
    // adopted：socket 所有权已移交 WebSocket 会话
}

bool SyncController::handleRequest(QTcpSocket *sock, const QByteArray &request)
{
    // 洛雪同步端点优先路由（/hello /id /ah /socket）
    if (m_lx) {
        const auto r = m_lx->handleHttp(sock, request);
        if (r == LxSyncServer::HttpRequestResult::Adopted) {
            sock->disconnect(this);          // 摘掉本类的 readyRead/disconnected 处理
            m_bufs.remove(sock);
            return true;
        }
        if (r == LxSyncServer::HttpRequestResult::RespondedClosed)
            return false;                     // 已回复并断开，交回常规清理
    }

    const QByteArray firstLine = request.left(request.indexOf('\r'));
    const QList<QByteArray> parts = firstLine.split(' ');
    if (parts.size() < 2) { sock->write(buildResponse(400, "text/plain", "bad")); sock->disconnectFromHost(); return false; }
    const QByteArray method = parts[0];
    const QByteArray path = parts[1];

    const int bodyStart = request.indexOf("\r\n\r\n");
    const QByteArray body = bodyStart >= 0 ? request.mid(bodyStart + 4) : QByteArray();

    if (method == "GET" && path.startsWith("/api/export")) {
        const QVariantMap data = m_library->exportOnlineLibrary();
        const QByteArray json = QJsonDocument::fromVariant(data).toJson(QJsonDocument::Compact);
        sock->write(buildResponse(200, "application/json; charset=utf-8", json));
    } else if (method == "POST" && path.startsWith("/api/import")) {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            sock->write(buildResponse(400, "application/json", "{\"error\":\"bad json\"}"));
        } else {
            const int added = m_library->importOnlineLibrary(doc.object().toVariantMap());
            sock->write(buildResponse(200, "application/json",
                                      "{\"added\":" + QByteArray::number(added) + "}"));
        }
    } else {
        sock->write(buildResponse(404, "text/plain", "not found"));
    }
    sock->flush();
    sock->disconnectFromHost();
    return false;
}

// ---------------------------------------------------------------------------
// 客户端：pull / push
// ---------------------------------------------------------------------------

void SyncController::pullFrom(const QString &url)
{
    if (m_busy) return;
    setBusy(true);
    QUrl base(url);
    if (!base.isValid() || base.host().isEmpty()) {
        setBusy(false);
        emit syncFailed(QStringLiteral("地址无效"));
        return;
    }
    QUrl u = base; u.setPath("/api/export");
    QNetworkRequest req(u);
    req.setTransferTimeout(8000);
    auto *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        setBusy(false);
        if (reply->error() != QNetworkReply::NoError) {
            emit syncFailed(QStringLiteral("拉取失败：%1").arg(reply->errorString()));
            return;
        }
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            emit syncFailed(QStringLiteral("对端数据无效"));
            return;
        }
        const int added = m_library->importOnlineLibrary(doc.object().toVariantMap());
        const QString dev = doc.object().value(QStringLiteral("deviceName")).toString();
        emit syncFinished(added, QStringLiteral("从「%1」拉取，新增 %2 项").arg(dev).arg(added));
    });
}

void SyncController::pushTo(const QString &url)
{
    if (m_busy) return;
    setBusy(true);
    QUrl base(url);
    if (!base.isValid() || base.host().isEmpty()) {
        setBusy(false);
        emit syncFailed(QStringLiteral("地址无效"));
        return;
    }
    QUrl u = base; u.setPath("/api/import");
    const QByteArray json =
        QJsonDocument::fromVariant(m_library->exportOnlineLibrary()).toJson(QJsonDocument::Compact);
    QNetworkRequest req(u);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setTransferTimeout(8000);
    auto *reply = m_nam->post(req, json);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        setBusy(false);
        if (reply->error() != QNetworkReply::NoError) {
            emit syncFailed(QStringLiteral("推送失败：%1").arg(reply->errorString()));
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const int added = doc.object().value(QStringLiteral("added")).toInt();
        emit syncFinished(added, QStringLiteral("已推送，对端新增 %1 项").arg(added));
    });
}

} // namespace Muyun
