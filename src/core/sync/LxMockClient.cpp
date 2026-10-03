#include "LxMockClient.h"

#include "LxUtil.h"
#include "LxWsConnection.h"
#include "LxMsg2Call.h"
#include "LxListMerge.h"
#include "core/utils/Crypto.h"

#include <QTcpSocket>
#include <QUrl>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSharedPointer>

namespace Muyun {

// 固定测试密钥（仅 --test-lxsync 用；私钥泄露无害，非生产凭据）
QByteArray LxMockClient::fixturePrivateB64()
{
    return QByteArrayLiteral("MIIEoQIBAAKCAQBUm4V5n54wtI8GPzUn7w98y7FmahVeS+8AUBwKaRWgJwcsm+g5n0nCthn1JiNp7Ces6h+Tu9XaO2Wh58sMp64zQpPAuYKUFTui+RVgSTmxJ5M5hIoVSxWCTm3o8U4MwTut+A9fVwDWheFUXvhYYCHW4uxcQzlTKh5p5I56fLRWzLzcTKUFIRiwt3crVmx2T+qlUxY2ewtrjODx9smcYFhiSDjPx+iKkanAUlFo+K7K5sCt4VWZI3avTu55Tmjt7YqwiwAlstgNPoW/X8lRsg4I0UiGk0bsbP6fgPbHIQYOtzSzkX3nJmHYtS3kIpdpEtH+3MlHiI5sAQRSl2UoWZN/AgMBAAECggEAK9HsvIROMx2hIfQkvM0VMWZyN//jl1Yur/PNwhqRB8SO86zABvAYDM+x/AVHvDmChw0vU52qcf/ncIaT5LcIAyaNTfkGx1mD7LGjP55NUhKULusVdVfRCQ9D36zy5qpCFaVvmMp/9Zubi84wQ91uSVR4ZISwsQEUhXXJ0Wi7ImUIhYqTntW6+/tElw2Pxfj07A3eN3QQMEMINJcM0XxZk17fScKR7YXMhRXFGEIR3J4n58ukYXlmkC+3ux1RnvtbeDK9H4D90VHz+Le7RCOgoFIlDuUsTWj/eJYo2KBvtBvF+JRdfTpPdJWhtwXCS1dvdYybjJLsZcZuRNQwGW9EqQKBgQCi0n+qz0CkhHP9zEctf69IsAKD7XN2veOfnMNu9uj+gn3nR0ZhKelR1Q/0iDdkONuI1A7hpIhZ5xFzIFQ5ILBYlyaAeX1cW214WABD1pUA53lhL5t5mHO9StKi7WwS9Wsu8hDlHuK7p8E6HdniXlg/1FhdURbOhR5k78O3ZUFoXQKBgQCFBoqyUvLhYlDKAEMm1RMstZnc8v7KGKauChCRKfDvo45aq1+8N21fLOFmmXVwn1zq1O5CWbs/tHxDNK1QAvYV9o/RX7tKuq4lQReGasc3iDYRMLaCTYPwfZlyp154S4Ikx/5WYpVx9plyAEGXt7cl+6KmSLWiyMdVyeiCrVr9iwKBgFPr8Tel+SiSyIIw7yLpY/wwFCvRkKjMOO2EzEp+YxsDQUeauhDMnGC8U6vzJbG3iJfezQjKWCkWay0OpbqIObx/s62WcOHQkfzsRr6wdD5yEGLiOd2TkVwom6a0fvbKngMla0XdBHo6feKpjisZ+5xyo1T9vaaWgFMALb1yKCNVAoGAciHjvA4pLzEwSbAh7V5hXjnNcmHWM0UlNSuTfUjz3FeC2s4wjIctwrv6BBJkTaWdjin05l6gpo7PEsZOcNiplLyANk6hngM/SN1pNr3EgSPP573YyUGCk0Lf9hwdlK6MUcx5rlpVCHtgugv4Oxi3/dHBglPd63XCeRuL7xqXd/MCgYBEVMBbwdSZXX8YpPIK0FFmYdwCq0iqZ8iCkkSyquaPS4TLFjkN4uyxYdsy6AYVnUqMe/e0n0e/PSMR+DZxjWEORxdebl1BLNUPH1aOuPghnBUJheFWee3XAw5JpRa+IvP5pBoIhC+OBw6XiRjdKf0CPIFIYcr5pOpTGqqKUgcV8g==");
}

QByteArray LxMockClient::fixturePublicSpkiB64()
{
    return QByteArrayLiteral("MIIBITANBgkqhkiG9w0BAQEFAAOCAQ4AMIIBCQKCAQBUm4V5n54wtI8GPzUn7w98y7FmahVeS+8AUBwKaRWgJwcsm+g5n0nCthn1JiNp7Ces6h+Tu9XaO2Wh58sMp64zQpPAuYKUFTui+RVgSTmxJ5M5hIoVSxWCTm3o8U4MwTut+A9fVwDWheFUXvhYYCHW4uxcQzlTKh5p5I56fLRWzLzcTKUFIRiwt3crVmx2T+qlUxY2ewtrjODx9smcYFhiSDjPx+iKkanAUlFo+K7K5sCt4VWZI3avTu55Tmjt7YqwiwAlstgNPoW/X8lRsg4I0UiGk0bsbP6fgPbHIQYOtzSzkX3nJmHYtS3kIpdpEtH+3MlHiI5sAQRSl2UoWZN/AgMBAAE=");
}

namespace {

QString randomWsKey()   // WS Sec-WebSocket-Key：16 字节随机 base64
{
    QByteArray raw(16, '\0');
    auto *r = QRandomGenerator::global();
    for (int i = 0; i < 16; ++i) raw[i] = char(r->bounded(256));
    return QString::fromLatin1(raw.toBase64());
}

} // namespace

LxMockClient::LxMockClient(QObject *parent) : QObject(parent) {}

LxMockClient::~LxMockClient()
{
    if (m_rpc) m_rpc->destroy();
}

void LxMockClient::setPhoneLibrary(const QJsonObject &listData)
{
    m_listData = listData;
}

QJsonObject LxMockClient::patchLocal(QJsonObject d)
{
    if (!d.contains(QStringLiteral("defaultList"))) d[QStringLiteral("defaultList")] = QJsonArray();
    if (!d.contains(QStringLiteral("loveList")))    d[QStringLiteral("loveList")] = QJsonArray();
    if (!d.contains(QStringLiteral("userList")))    d[QStringLiteral("userList")] = QJsonArray();
    return d;
}

// ---------------------------------------------------------------------------
// 极简 HTTP GET（Connection: close，读完全部后回调）
// ---------------------------------------------------------------------------
void LxMockClient::httpGet(int port, const QString &path,
                           const QHash<QByteArray, QByteArray> &headers,
                           std::function<void(int code, const QByteArray &body)> cb)
{
    auto *sock = new QTcpSocket(this);
    QObject::connect(sock, &QTcpSocket::connected, sock, [sock, path, headers]() {
        QByteArray head = "GET " + path.toUtf8() + " HTTP/1.1\r\n"
                          "Host: 127.0.0.1\r\n";
        for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
            head += it.key() + ": " + it.value() + "\r\n";
        head += "Connection: close\r\n\r\n";
        sock->write(head);
    });
    auto buf = QSharedPointer<QByteArray>::create();
    auto done = QSharedPointer<bool>::create(false);
    QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, buf]() { *buf += sock->readAll(); });
    QObject::connect(sock, &QTcpSocket::disconnected, sock, [sock, buf, cb, done]() {
        if (*done) return;
        *done = true;
        int code = 0;
        if (buf->startsWith("HTTP/1.1 ")) code = buf->mid(9, 3).toInt();
        const int he = buf->indexOf("\r\n\r\n");
        const QByteArray body = he >= 0 ? buf->mid(he + 4) : QByteArray();
        cb(code, body);
        sock->deleteLater();
    });
    sock->connectToHost(QStringLiteral("127.0.0.1"), quint16(port));
}

// ---------------------------------------------------------------------------
// 首连：/hello → /id → /ah(口令) → WS
// ---------------------------------------------------------------------------
void LxMockClient::start(int port, const QString &authCode)
{
    httpGet(port, QStringLiteral("/hello"), {}, [this, port, authCode](int code, const QByteArray &body) {
        if (code != 200 || QString::fromUtf8(body) != QLatin1String(LxSync::kHelloMsg)) {
            phaseFinished(false, QStringLiteral("hello 失败 %1").arg(code));
            return;
        }
        emit logLine(QStringLiteral("hello ✓"));
        httpGet(port, QStringLiteral("/id"), {}, [this, port, authCode](int c2, const QByteArray &b2) {
            const QString idBody = QString::fromUtf8(b2);
            if (c2 != 200 || !idBody.startsWith(QLatin1String(LxSync::kIdPrefix))) {
                phaseFinished(false, QStringLiteral("id 失败"));
                return;
            }
            emit logLine(QStringLiteral("serverId ✓ (%1...)")
                             .arg(idBody.mid(int(qstrlen(LxSync::kIdPrefix))).left(8)));

            // 口令鉴权：aes( authMsg \n SPKI \n deviceName \n lx_music_mobile )
            const QString key16 = Crypto::md5Hex(authCode.toUtf8()).left(16);
            const QByteArray keyB64 = key16.toLatin1().toBase64();
            const QString msg = QString::fromLatin1(LxSync::kAuthMsg)
                                + QChar('\n') + QString::fromLatin1(fixturePublicSpkiB64())
                                + QChar('\n') + QStringLiteral("MuyunMockPhone")
                                + QChar('\n') + QStringLiteral("lx_music_mobile");
            const QByteArray m = LxSync::aesEncryptB64Key(msg.toUtf8(), keyB64);
            QHash<QByteArray, QByteArray> hdr;
            hdr.insert("m", m);
            httpGet(port, QStringLiteral("/ah"), hdr, [this, port](int c3, const QByteArray &b3) {
                if (c3 != 200) {
                    phaseFinished(false, QStringLiteral("鉴权失败 %1 %2")
                                             .arg(c3).arg(QString::fromUtf8(b3.left(30))));
                    return;
                }
                const QString privPem = QStringLiteral("-----BEGIN RSA PRIVATE KEY-----\n")
                        + QString::fromLatin1(fixturePrivateB64())
                        + QStringLiteral("\n-----END RSA PRIVATE KEY-----");
                const QByteArray plain = Crypto::rsaOaepDecrypt(
                    QByteArray::fromBase64(b3.trimmed()), privPem);
                const QJsonObject info = QJsonDocument::fromJson(plain).object();
                m_clientId = info.value(QStringLiteral("clientId")).toString();
                m_key = info.value(QStringLiteral("key")).toString();
                m_serverName = info.value(QStringLiteral("serverName")).toString();
                if (m_clientId.isEmpty() || m_key.isEmpty()) {
                    phaseFinished(false, QStringLiteral("RSA 回包解析失败"));
                    return;
                }
                emit logLine(QStringLiteral("auth(RSA-OAEP) ✓ serverName=%1").arg(m_serverName));
                openSocket(port);
            });
        });
    });
}

// ---------------------------------------------------------------------------
// 重连：/ah(i+m) keyAuth → WS
// ---------------------------------------------------------------------------
void LxMockClient::reconnect(int port)
{
    const QByteArray mm = LxSync::aesEncryptB64Key(
        QByteArray(LxSync::kAuthMsg) + QByteArrayLiteral("MuyunMockPhone"), m_key.toLatin1());
    QHash<QByteArray, QByteArray> hdr;
    hdr.insert("i", m_clientId.toLatin1());
    hdr.insert("m", mm);
    httpGet(port, QStringLiteral("/ah"), hdr, [this, port](int c, const QByteArray &b) {
        const QByteArray text = LxSync::aesDecryptB64Key(b.trimmed(), m_key.toLatin1());
        if (c != 200 || text != QByteArray(LxSync::kHelloMsg)) {
            phaseFinished(false, QStringLiteral("keyAuth 失败 %1").arg(c));
            return;
        }
        emit logLine(QStringLiteral("keyAuth ✓"));
        openSocket(port);
    });
}

// ---------------------------------------------------------------------------
// WS 升级 + msg2call 客户端
// ---------------------------------------------------------------------------
void LxMockClient::openSocket(int port)
{
    const QByteArray t = LxSync::aesEncryptB64Key(QByteArray(LxSync::kMsgConnect), m_key.toLatin1());
    const QString path = QStringLiteral("/socket?i=%1&t=%2")
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(m_clientId)),
             QString::fromUtf8(QUrl::toPercentEncoding(t)));
    const QString wsKey = randomWsKey();

    auto *sock = new QTcpSocket(this);
    m_sock = sock;
    QObject::connect(sock, &QTcpSocket::connected, sock, [sock, path, wsKey]() {
        const QByteArray head = "GET " + path.toUtf8() + " HTTP/1.1\r\n"
                                "Host: 127.0.0.1\r\n"
                                "Upgrade: websocket\r\n"
                                "Connection: Upgrade\r\n"
                                "Sec-WebSocket-Key: " + wsKey.toUtf8() + "\r\n"
                                "Sec-WebSocket-Version: 13\r\n\r\n";
        sock->write(head);
    });

    auto state = QSharedPointer<QByteArray>::create();
    auto handled = QSharedPointer<bool>::create(false);
    QObject::connect(sock, &QTcpSocket::readyRead, this,
                     [this, sock, state, handled]() {
        if (*handled) return;                    // 101 之后帧由 LxWsConnection 自己处理
        *state += sock->readAll();
        const int eoh = state->indexOf("\r\n\r\n");
        if (eoh < 0) return;                     // 101 头未到齐
        *handled = true;
        const QByteArray respHead = state->left(eoh);
        if (!respHead.startsWith("HTTP/1.1 101")) {
            phaseFinished(false, QStringLiteral("WS 升级失败：%1")
                                     .arg(QString::fromUtf8(respHead.left(40))));
            return;
        }
        const QByteArray leftover = state->mid(eoh + 4);
        emit logLine(QStringLiteral("websocket ✓"));

        m_ws = new LxWsConnection(sock, LxWsConnection::Client, QByteArray(), leftover, this);
        m_rpc = new LxMsg2Call(this);
        LxWsConnection *ws = m_ws;
        LxMsg2Call *rpc = m_rpc;
        rpc->setSender([ws](const QJsonObject &msg) {
            const QByteArray payload = LxSync::encodeMsg(
                QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
            return ws->sendText(payload);
        });
        QObject::connect(ws, &LxWsConnection::textMessage, this, [ws, rpc](const QByteArray &payload) {
            if (payload == "ping") { ws->sendText("ping"); return; }   // 心跳文本回声
            const QByteArray plain = LxSync::decodeMsg(payload);
            if (!plain.isEmpty()) rpc->onMessage(plain);
        });
        registerRpcFunctions();
        emit logLine(QStringLiteral("mock 就绪，等待服务端发起同步…"));
    });
    QObject::connect(sock, &QAbstractSocket::errorOccurred, this, [this, handled]() {
        if (!*handled) phaseFinished(false, QStringLiteral("WS 连接失败"));
    });
    sock->connectToHost(QStringLiteral("127.0.0.1"), quint16(port));
}

void LxMockClient::registerRpcFunctions()
{
    // 手机端（=服务端 RPC 目标）：真实 mobile client 的全套 list handler
    m_rpc->registerFunction(QStringLiteral("getEnabledFeatures"),
        [](const QJsonArray &,
           std::function<void(const QString &, const QJsonValue &)> done) {
            QJsonObject en;
            en[QStringLiteral("list")] = QJsonObject{{QStringLiteral("skipSnapshot"), false}};
            done(QString(), en);
        });
    m_rpc->registerFunction(QStringLiteral("list_sync_get_list_data"),
        [this](const QJsonArray &,
               std::function<void(const QString &, const QJsonValue &)> done) {
            ++m_getListDataCalls;
            emit logLine(QStringLiteral("← list_sync_get_list_data"));
            done(QString(), m_listData);
        });
    m_rpc->registerFunction(QStringLiteral("list_sync_get_md5"),
        [this](const QJsonArray &,
               std::function<void(const QString &, const QJsonValue &)> done) {
            done(QString(), Crypto::md5Hex(LxListMerge::canonicalJson(m_listData)));
        });
    m_rpc->registerFunction(QStringLiteral("list_sync_set_list_data"),
        [this](const QJsonArray &args,
               std::function<void(const QString &, const QJsonValue &)> done) {
            ++m_setListDataCalls;
            m_listData = patchLocal(args.at(0).toObject());
            emit logLine(QStringLiteral("← list_sync_set_list_data（电脑下发 %1 条收藏）")
                             .arg(m_listData.value(QStringLiteral("loveList")).toArray().size()));
            done(QString(), QJsonValue());
        });
    m_rpc->registerFunction(QStringLiteral("list_sync_finished"),
        [this](const QJsonArray &,
               std::function<void(const QString &, const QJsonValue &)> done) {
            emit logLine(QStringLiteral("← list_sync_finished"));
            done(QString(), QJsonValue());
        });
    m_rpc->registerFunction(QStringLiteral("finished"),
        [this](const QJsonArray &,
               std::function<void(const QString &, const QJsonValue &)> done) {
            ++m_finishedCalls;
            emit logLine(QStringLiteral("← finished（握手全链路完成）"));
            done(QString(), QJsonValue());
            phaseFinished(true, QStringLiteral("mock 同步完成"));
        });
    m_rpc->registerFunction(QStringLiteral("list_sync_get_sync_mode"),
        [](const QJsonArray &,
           std::function<void(const QString &, const QJsonValue &)> done) {
            // 本架构模式选择发生在电脑端弹窗；真实手机端才用这个回口
            done(QString(), QStringLiteral("merge_remote_local"));
        });
    m_rpc->registerFunction(QStringLiteral("onListSyncAction"),
        [this](const QJsonArray &args,
               std::function<void(const QString &, const QJsonValue &)> done) {
            const QJsonObject action = args.at(0).toObject();
            const QString type = action.value(QStringLiteral("action")).toString();
            if (type == QLatin1String("list_data_overwrite"))
                m_listData = patchLocal(action.value(QStringLiteral("data")).toObject());
            emit logLine(QStringLiteral("← onListSyncAction(%1)").arg(type));
            done(QString(), QJsonValue());
        });
}

void LxMockClient::phoneAddFavorite(const QJsonObject &musicInfo)
{
    QJsonArray love = m_listData.value(QStringLiteral("loveList")).toArray();
    love.append(musicInfo);
    m_listData[QStringLiteral("loveList")] = love;
    if (!m_rpc) return;
    QJsonObject data;
    data[QStringLiteral("id")] = QStringLiteral("love");
    data[QStringLiteral("musicInfos")] = QJsonArray{ musicInfo };
    data[QStringLiteral("addMusicLocationType")] = QStringLiteral("bottom");
    QJsonObject action;
    action[QStringLiteral("action")] = QStringLiteral("list_music_add");
    action[QStringLiteral("data")] = data;
    m_rpc->call(QStringLiteral("list"), QStringLiteral("onListSyncAction"), { action },
                [](const QString &, const QJsonValue &) {});
}

void LxMockClient::phaseFinished(bool pass, const QString &detail)
{
    emit done(pass, detail);
}

void LxMockClient::disconnectNow()
{
    if (m_ws) { m_ws->closeConnection(1000); return; }
    if (m_sock && m_sock->state() != QAbstractSocket::UnconnectedState) m_sock->abort();
}

} // namespace Muyun
