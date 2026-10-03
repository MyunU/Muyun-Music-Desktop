#pragma once

#include <QObject>
#include <QByteArray>

class QTcpSocket;

namespace Muyun {

/**
 * @brief 极简 WebSocket（RFC6455）帧层，洛雪同步专用。
 *
 * 之所以不用 QWebSocketServer：洛雪协议要求 /hello /id /ah（HTTP）与
 * /socket（WS 升级）同端口，由我们自己的 QTcpServer 统一收包后，
 * 把升级请求连同原始 socket 交给本类接管；且需要主动发文本 "ping"
 * 心跳（QWebSocket 无公开 ping 接口）。测试端（--test-lxsync）用同一
 * 类的 Client 模式，双向复用一套帧编解码。
 *
 * 仅支持：同域 ws://（无 TLS）、文本帧（可分片收）、ping/pong/close 控制帧。
 * 服务端发出帧不掩码、接收必须掩码帧（校验）；客户端相反。
 */
class LxWsConnection : public QObject
{
    Q_OBJECT
public:
    enum Role { Server, Client };

    /// Server 模式接管：rawRequest 为完整的 HTTP 升级请求头（到 \r\n\r\n），
    /// 构造即完成握手回 101 并进入 Open；后续帧自动收发。
    ///
    /// Client 模式（供 --test-lxsync 模拟手机）：rawRequest 留空，socket 需已由
    /// 调用方完成 HTTP 升级握手（发出 GET + 校验 101），leftover 为 101 之后
    /// 已一并收到的字节（可能含首个 RPC 帧），本类只负责帧编解码。
    LxWsConnection(QTcpSocket *socket, Role role, const QByteArray &rawRequest = QByteArray(),
                   const QByteArray &leftover = QByteArray(), QObject *parent = nullptr);

    ~LxWsConnection() override;

    bool isOpen() const { return m_state == Open; }
    /// 收到过任何入站帧（供心跳存活判定）
    qint64 lastActivityMs() const { return m_lastActivity; }

    /// 发送文本帧（自动 gzip>1024 由调用方处理；这里就是原始 payload）
    bool sendText(const QByteArray &utf8Payload);
    bool sendPing();
    /// 主动关闭（发 close 帧后断开）
    void closeConnection(int code = 1000, const QByteArray &reason = QByteArray());
    /// 立即断链（不发 close 帧）
    void abort();

    QString peerAddress() const;

signals:
    void textMessage(const QByteArray &payload);
    void closed(int code);           // 对端/本端关闭，之后对象 deleteLater

private slots:
    void onReadyRead();
    void onDisconnected();

private:
    enum State { Handshaking, Open, Closing, Closed };

    void writeFrame(quint8 opcode, const QByteArray &payload);
    bool consumeFrames();           // 解析 m_inBuf 中完整帧
    static QByteArray makeMaskKey();

    QTcpSocket *m_socket = nullptr;
    Role m_role;
    State m_state = Handshaking;
    QByteArray m_inBuf;             // 未消费原始字节
    QByteArray m_fragBuf;           // 分片聚合
    quint8 m_fragOpcode = 0;
    qint64 m_lastActivity = 0;
    bool m_handshakeSent = false;   // Client 模式：请求已发出
    int m_closeCode = 1000;
};

} // namespace Muyun
