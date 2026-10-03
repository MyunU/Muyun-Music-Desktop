#include "LxWsConnection.h"

#include <QTcpSocket>
#include <QCryptographicHash>
#include <QDateTime>
#include <QRandomGenerator>
#include <QTimer>

namespace Muyun {

namespace {
constexpr quint8 OP_CONT = 0x0, OP_TEXT = 0x1, OP_BIN = 0x2,
                 OP_CLOSE = 0x8, OP_PING = 0x9, OP_PONG = 0xA;
constexpr int kCloseFailed = 4100;   // LxSync::kCloseFailed（避免拉入 LxUtil 依赖）
constexpr qint64 kMaxFrame = 64LL * 1024 * 1024;   // 64MB 上限（整库快照 gzip 后仍很小）
}

LxWsConnection::LxWsConnection(QTcpSocket *socket, Role role, const QByteArray &rawRequest,
                               const QByteArray &leftover, QObject *parent)
    : QObject(parent), m_socket(socket), m_role(role)
{
    m_lastActivity = QDateTime::currentMSecsSinceEpoch();
    connect(m_socket, &QTcpSocket::readyRead, this, &LxWsConnection::onReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &LxWsConnection::onDisconnected);

    if (role == Server) {
        // 完成 HTTP 升级握手：从请求头取 Sec-WebSocket-Key，算 accept 回 101
        QByteArray key;
        const QList<QByteArray> lines = rawRequest.split('\n');
        for (const QByteArray &line : lines) {
            const QByteArray l = line.trimmed();
            if (l.toLower().startsWith("sec-websocket-key:")) {
                key = l.mid(int(qstrlen("sec-websocket-key:"))).trimmed();
                break;
            }
        }
        if (key.isEmpty()) { m_state = Closed; m_socket->disconnectFromHost(); return; }
        static const QByteArray guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        const QByteArray accept =
            QCryptographicHash::hash(key + guid, QCryptographicHash::Sha1).toBase64();
        QByteArray resp = "HTTP/1.1 101 Switching Protocols\r\n"
                          "Upgrade: websocket\r\n"
                          "Connection: Upgrade\r\n"
                          "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
        m_socket->write(resp);
        m_state = Open;
    } else {
        m_state = Open;   // 客户端模式：握手已由调用方完成
        if (!leftover.isEmpty()) {
            m_inBuf = leftover;
            // 延迟泵送：等调用方接好 textMessage 信号后再消费（构造期不派发）
            QTimer::singleShot(0, this, [this]() { if (m_state == Open) consumeFrames(); });
        }
    }
}

LxWsConnection::~LxWsConnection()
{
    if (m_socket) {
        m_socket->disconnect(this);
        if (m_state != Closed && m_socket->state() != QAbstractSocket::UnconnectedState)
            m_socket->abort();
    }
}

bool LxWsConnection::sendText(const QByteArray &utf8Payload)
{
    if (m_state != Open) return false;
    writeFrame(OP_TEXT, utf8Payload);
    return true;
}

bool LxWsConnection::sendPing()
{
    if (m_state != Open) return false;
    writeFrame(OP_PING, QByteArray());
    return true;
}

void LxWsConnection::closeConnection(int code, const QByteArray &reason)
{
    m_closeCode = code;
    if (m_state == Open) {
        m_state = Closing;
        QByteArray payload;
        payload.append(char((code >> 8) & 0xFF));
        payload.append(char(code & 0xFF));
        payload += reason.left(120);
        writeFrame(OP_CLOSE, payload);
        m_socket->flush();
    }
    m_socket->disconnectFromHost();
    if (m_state != Closed) m_state = Closed;
    emit closed(code);
    deleteLater();
}

void LxWsConnection::abort()
{
    m_state = Closed;
    if (m_socket->state() != QAbstractSocket::UnconnectedState) m_socket->abort();
    emit closed(m_closeCode);
    deleteLater();
}

QString LxWsConnection::peerAddress() const
{
    return m_socket->peerAddress().toString();
}

QByteArray LxWsConnection::makeMaskKey()
{
    QByteArray k(4, '\0');
    auto *r = QRandomGenerator::global();
    for (int i = 0; i < 4; ++i) k[i] = char(r->bounded(256));
    return k;
}

void LxWsConnection::writeFrame(quint8 opcode, const QByteArray &payload)
{
    QByteArray hdr;
    hdr.append(char(0x80 | opcode));                 // FIN + opcode
    const int len = payload.size();
    const bool maskOut = (m_role == Client);         // 客户端→服务端必须掩码
    if (len < 126) {
        hdr.append(char((maskOut ? 0x80 : 0) | len));
    } else if (len <= 0xFFFF) {
        hdr.append(char((maskOut ? 0x80 : 0) | 126));
        hdr.append(char((len >> 8) & 0xFF));
        hdr.append(char(len & 0xFF));
    } else {
        hdr.append(char((maskOut ? 0x80 : 0) | 127));
        for (int s = 56; s >= 0; s -= 8) hdr.append(char((quint64(len) >> s) & 0xFF));
    }
    if (maskOut) {
        const QByteArray key = makeMaskKey();
        hdr += key;
        QByteArray masked(len, '\0');
        for (int i = 0; i < len; ++i) masked[i] = char(payload[i] ^ key[i & 3]);
        m_socket->write(hdr);
        m_socket->write(masked);
    } else {
        m_socket->write(hdr);
        m_socket->write(payload);
    }
}

void LxWsConnection::onReadyRead()
{
    m_inBuf += m_socket->readAll();
    m_lastActivity = QDateTime::currentMSecsSinceEpoch();
    consumeFrames();
}

bool LxWsConnection::consumeFrames()
{
    forever {
        const int n = m_inBuf.size();
        if (n < 2) return true;
        const quint8 b0 = quint8(m_inBuf[0]);
        const quint8 b1 = quint8(m_inBuf[1]);
        const bool fin = b0 & 0x80;
        const quint8 opcode = b0 & 0x0F;
        const bool masked = b1 & 0x80;
        quint64 len = b1 & 0x7F;
        int pos = 2;
        if (len == 126) {
            if (n < pos + 2) return true;
            len = (quint16(quint8(m_inBuf[pos])) << 8) | quint8(m_inBuf[pos + 1]);
            pos += 2;
        } else if (len == 127) {
            if (n < pos + 8) return true;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | quint8(m_inBuf[pos + i]);
            pos += 8;
        }
        if (len > quint64(kMaxFrame)) { closeConnection(kCloseFailed, "too big"); return false; }
        QByteArray mask;
        if (masked) {
            if (n < pos + 4) return true;
            mask = m_inBuf.mid(pos, 4);
            pos += 4;
        }
        const qint64 need = qint64(pos) + qint64(len);
        if (n < need) return true;               // 帧未收全
        QByteArray payload = m_inBuf.mid(pos, int(len));
        if (masked) {
            for (int i = 0; i < payload.size(); ++i) payload[i] = char(payload[i] ^ mask[i & 3]);
        }
        m_inBuf.remove(0, int(need));
        m_lastActivity = QDateTime::currentMSecsSinceEpoch();

        switch (opcode) {
        case OP_CONT:
            m_fragBuf += payload;
            if (fin) { QByteArray full = m_fragBuf; m_fragBuf.clear(); emit textMessage(full); }
            break;
        case OP_TEXT:
            if (fin && m_fragBuf.isEmpty()) {
                emit textMessage(payload);
            } else {
                m_fragOpcode = opcode;
                m_fragBuf += payload;
                if (fin) { QByteArray full = m_fragBuf; m_fragBuf.clear(); emit textMessage(full); }
            }
            break;
        case OP_BIN:
            // 洛雪只用文本；忽略
            break;
        case OP_PING:
            writeFrame(OP_PONG, payload);        // RFC6455：pong 回显 payload
            break;
        case OP_PONG:
            break;
        case OP_CLOSE: {
            int code = m_closeCode;
            if (payload.size() >= 2)
                code = (quint16(quint8(payload[0])) << 8) | quint8(payload[1]);
            if (m_state == Open) { m_state = Closing; writeFrame(OP_CLOSE, payload); m_socket->flush(); }
            m_state = Closed;
            m_socket->disconnectFromHost();
            emit closed(code);
            deleteLater();
            return false;
        }
        default:
            closeConnection(kCloseFailed, "opcode");
            return false;
        }
    }
}

void LxWsConnection::onDisconnected()
{
    if (m_state == Closed) return;
    m_state = Closed;
    emit closed(m_closeCode);
    deleteLater();
}

} // namespace Muyun
