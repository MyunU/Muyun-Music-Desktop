#include "LxSyncServer.h"

#include "ui/LibraryController.h"
#include "core/storage/DocumentStore.h"
#include "core/sync/LxUtil.h"
#include "core/sync/LxWsConnection.h"
#include "core/sync/LxMsg2Call.h"
#include "core/sync/LxListMerge.h"
#include "core/utils/Crypto.h"

#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QFile>
#include <QDateTime>

namespace Muyun {

namespace {

constexpr int kMaxDevices = 100;           // 洛雪上限 101，保守拒绝
constexpr int kModeTimeoutMs = 10 * 60 * 1000;
QString groupList() { return QStringLiteral("list"); }

QJsonObject patchListData(QJsonObject d)   // 洛雪 patchListData
{
    if (!d.contains(QStringLiteral("defaultList"))) d[QStringLiteral("defaultList")] = QJsonArray();
    if (!d.contains(QStringLiteral("loveList")))    d[QStringLiteral("loveList")] = QJsonArray();
    if (!d.contains(QStringLiteral("userList")))    d[QStringLiteral("userList")] = QJsonArray();
    return d;
}

void writeTextResponse(QTcpSocket *sock, int code, const QByteArray &body)
{
    QByteArray r;
    r += "HTTP/1.1 " + QByteArray::number(code) + " "
         + (code == 200 ? "OK" : (code == 401 ? "Unauthorized" : "Forbidden")) + "\r\n";
    r += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    r += "Connection: close\r\n\r\n";
    r += body;
    sock->write(r);
    sock->flush();
    sock->disconnectFromHost();
}

} // namespace

struct LxSession : QObject
{
    LxWsConnection *ws = nullptr;
    LxMsg2Call *rpc = nullptr;
    LxSnapshotStore::KeyInfo key;
    bool ready = false;                    ///< list 模块就绪
    qint64 lastPingAt = 0;
};

// ===========================================================================

LxSyncServer::LxSyncServer(LibraryController *library, QObject *parent)
    : QObject(parent), m_library(library)
{
    m_store = new LxSnapshotStore(this);
    m_stateFilePath = DocumentStore::instance()->rootPath() + QStringLiteral("/lx-sync/preserved.json");
    loadPreserved();
    m_code = LxSync::generateAuthCode();
    m_lastPushCanonical = canonicalLocal();   // 起始指纹 = 当前库快照

    m_codeTimer = new QTimer(this);
    m_codeTimer->setInterval(3 * 60 * 1000);
    connect(m_codeTimer, &QTimer::timeout, this, &LxSyncServer::rotateCode);
    m_codeTimer->start();

    m_beatTimer = new QTimer(this);
    m_beatTimer->setInterval(30 * 1000);
    connect(m_beatTimer, &QTimer::timeout, this, &LxSyncServer::onHeartbeatTick);
    m_beatTimer->start();

    m_pushTimer = new QTimer(this);
    m_pushTimer->setSingleShot(true);
    m_pushTimer->setInterval(1200);
    connect(m_pushTimer, &QTimer::timeout, this, [this]() { pushSnapshotToClients(); });

    connect(m_library, &LibraryController::favoritesChanged, m_pushTimer,
            [this]() { if (m_applyingLibrary) return; m_pushTimer->start(); });
    connect(m_library, &LibraryController::playlistsChanged, m_pushTimer,
            [this]() { if (m_applyingLibrary) return; m_pushTimer->start(); });

    m_modeTimeout = new QTimer(this);
    m_modeTimeout->setSingleShot(true);
    m_modeTimeout->setInterval(kModeTimeoutMs);
    connect(m_modeTimeout, &QTimer::timeout, this,
            [this]() { answerSyncMode(QStringLiteral("cancel")); });
}

LxSyncServer::~LxSyncServer() = default;

// ===========================================================================
// HTTP / 升级入口
// ===========================================================================

LxSyncServer::HttpRequestResult LxSyncServer::handleHttp(QTcpSocket *sock, const QByteArray &head)
{
    const QByteArray firstLine = head.left(head.indexOf('\r'));
    const QList<QByteArray> fl = firstLine.split(' ');
    if (fl.size() < 2) return HttpRequestResult::NotHandled;
    const QByteArray target = fl.at(1);
    const int q = target.indexOf('?');
    const QByteArray path = q < 0 ? target : target.left(q);

    if (path != "/hello" && path != "/id" && path != "/ah" && path != "/socket")
        return HttpRequestResult::NotHandled;

    QHash<QByteArray, QByteArray> headers;
    const QList<QByteArray> lines = head.split('\n');
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray &l = lines.at(i);
        const int c = l.indexOf(':');
        if (c <= 0) continue;
        const QByteArray name = l.left(c).toLower().trimmed();
        if (!headers.contains(name)) headers.insert(name, l.mid(c + 1).trimmed());
    }
    QUrlQuery query(QString::fromUtf8(q < 0 ? QByteArray() : target.mid(q + 1)));

    if (path == "/hello") {
        writeTextResponse(sock, 200, QByteArray(LxSync::kHelloMsg));
        return HttpRequestResult::RespondedClosed;
    }
    if (path == "/id") {
        writeTextResponse(sock, 200, QByteArray(LxSync::kIdPrefix) + m_store->serverId().toUtf8());
        return HttpRequestResult::RespondedClosed;
    }
    if (path == "/ah") {
        const QByteArray m = headers.value("m");
        const QByteArray i = headers.value("i");
        QByteArray body;
        if (!m.isEmpty() && !i.isEmpty()) {
            // 老设备密钥鉴权：aes(authMsg + deviceName)
            LxSnapshotStore::KeyInfo info;
            if (m_store->getClientKeyInfo(QString::fromUtf8(i), &info)) {
                const QByteArray text = LxSync::aesDecryptB64Key(m, info.key.toLatin1());
                if (text.startsWith(LxSync::kAuthMsg)) {
                    const QString dev = QString::fromUtf8(text).mid(int(qstrlen(LxSync::kAuthMsg)));
                    if (!dev.isEmpty() && dev != info.deviceName) {
                        info.deviceName = dev;
                        m_store->saveClientKeyInfo(info);
                    }
                    body = LxSync::aesEncryptB64Key(QByteArray(LxSync::kHelloMsg), info.key.toLatin1());
                }
            }
        } else if (!m.isEmpty()) {
            // 口令鉴权：aes(authMsg\n pubkey \n deviceName \n appType)
            const QString key16 = Crypto::md5Hex(m_code.toUtf8()).left(16);
            const QByteArray keyB64 = key16.toLatin1().toBase64();
            const QByteArray text = LxSync::aesDecryptB64Key(m, keyB64);
            if (text.startsWith(LxSync::kAuthMsg)
                && m_store->allClientKeyInfo().size() < kMaxDevices) {
                const QStringList data = QString::fromUtf8(text).split(QLatin1Char('\n'));
                if (data.size() >= 2 && !data.at(1).isEmpty()) {
                    const QString deviceName = (data.size() > 2 && !data.at(2).isEmpty())
                                                 ? data.at(2) : QStringLiteral("Unknown");
                    const bool isMobile = data.size() > 3
                                            && data.at(3) == QLatin1String("lx_music_mobile");
                    auto info = m_store->createClientKeyInfo(deviceName, isMobile);
                    m_store->saveClientKeyInfo(info);
                    QJsonObject resp;
                    resp[QStringLiteral("clientId")] = info.clientId;
                    resp[QStringLiteral("key")] = info.key;
                    resp[QStringLiteral("serverName")] = m_store->serverName();
                    const QString pem = QStringLiteral("-----BEGIN PUBLIC KEY-----\n")
                                        + data.at(1) + QStringLiteral("\n-----END PUBLIC KEY-----");
                    const QByteArray enc = Crypto::rsaOaepEncrypt(
                        QJsonDocument(resp).toJson(QJsonDocument::Compact), pem);
                    if (!enc.isEmpty()) {
                        body = enc.toBase64();
                        emit syncEvent(QStringLiteral("新设备已配对：%1").arg(deviceName));
                    }
                }
            }
        }
        if (body.isEmpty()) {
            writeTextResponse(sock, 401, QByteArray(LxSync::kMsgAuthFailed));
            return HttpRequestResult::RespondedClosed;
        }
        writeTextResponse(sock, 200, body);
        return HttpRequestResult::RespondedClosed;
    }

    // /socket：WebSocket 升级
    if (!headers.value("upgrade").toLower().contains("websocket"))
        return HttpRequestResult::NotHandled;
    const QString clientId = query.queryItemValue(QStringLiteral("i"), QUrl::FullyDecoded);
    const QString t = query.queryItemValue(QStringLiteral("t"), QUrl::FullyDecoded);
    LxSnapshotStore::KeyInfo info;
    bool authOk = false;
    if (m_store->getClientKeyInfo(clientId, &info))
        authOk = (LxSync::aesDecryptB64Key(t.toUtf8(), info.key.toLatin1()) == LxSync::kMsgConnect);
    if (!authOk) {
        writeTextResponse(sock, 401, QByteArray());
        return HttpRequestResult::RespondedClosed;
    }
    info.lastConnectDate = QDateTime::currentMSecsSinceEpoch();
    m_store->saveClientKeyInfo(info);
    adoptSession(sock, head, info);
    return HttpRequestResult::Adopted;
}

void LxSyncServer::adoptSession(QTcpSocket *sock, const QByteArray &head,
                                const LxSnapshotStore::KeyInfo &info)
{
    auto *s = new LxSession;
    s->key = info;
    s->setParent(this);
    s->ws = new LxWsConnection(sock, LxWsConnection::Server, head, QByteArray(), s);
    sock->setParent(s->ws);                      // 生命周期交给帧层

    // 同 clientId 旧连接挤掉（洛雪 checkDuplicateClient）。
    // 注意 closeConnection 会同步触发 closed→removeSession 改 m_sessions，必须先拷贝。
    const auto current = m_sessions;
    for (auto *old : current) {
        if (old == s || old->key.clientId != info.clientId) continue;
        old->ready = false;
        old->ws->closeConnection(LxSync::kCloseNormal);
    }

    s->rpc = new LxMsg2Call(s);
    connect(s->ws, &LxWsConnection::closed, s, [this, s](int) { removeSession(s); });
    bindSessionRpc(s);
    m_sessions.append(s);
    emit devicesChanged();
    qInfo("[lxsync] 设备接入：%s", qUtf8Printable(info.deviceName));

    QJsonObject supported;
    supported[QStringLiteral("list")] = 1;
    supported[QStringLiteral("dislike")] = 1;
    s->rpc->call(QString(), QStringLiteral("getEnabledFeatures"),
                 { QStringLiteral("desktop-app"), supported },
                 [this, s](const QString &err, const QJsonValue &data) {
        if (!m_sessions.contains(s)) return;
        if (!err.isEmpty()) { sessionFail(s, err); return; }
        const QJsonValue f = data.toObject().value(QStringLiteral("list"));
        const bool listOn = f.isObject() || (f.isBool() && f.toBool());
        if (listOn) {
            startListSync(s);
        } else {
            s->rpc->call(QString(), QStringLiteral("finished"), {},
                         [](const QString &, const QJsonValue &) {});
        }
    });
}

void LxSyncServer::bindSessionRpc(LxSession *s)
{
    LxWsConnection *ws = s->ws;
    s->rpc->setSender([ws](const QJsonObject &msg) {
        const QByteArray payload = LxSync::encodeMsg(
            QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
        return ws->sendText(payload);
    });
    LxMsg2Call *rpc = s->rpc;
    connect(ws, &LxWsConnection::textMessage, s, [rpc](const QByteArray &payload) {
        if (payload == "ping") return;           // 心跳回包
        const QByteArray plain = LxSync::decodeMsg(payload);
        if (plain.isEmpty()) return;
        rpc->onMessage(plain);
    });

    QPointer<LxSyncServer> self(this);

    rpc->registerFunction(QStringLiteral("onListSyncAction"),
        [self, s](const QJsonArray &args,
                  std::function<void(const QString &, const QJsonValue &)> done) {
            if (!self) { done(QStringLiteral("closed"), QJsonValue()); return; }
            self->onListAction(s, args.at(0).toObject(), std::move(done));
        });

    rpc->registerFunction(QStringLiteral("onFeatureChanged"),
        [self, s](const QJsonArray &args,
                  std::function<void(const QString &, const QJsonValue &)> done) {
            if (self) {
                const QJsonValue f = args.at(0).toObject().value(QStringLiteral("list"));
                const bool on = f.isObject() || (f.isBool() && f.toBool());
                if (on && !s->ready) self->startListSync(s);
            }
            done(QString(), QJsonValue());
        });

    // 暮云无"不感兴趣"概念，接收后丢弃（也避免对端因未定义方法而断开）
    rpc->registerFunction(QStringLiteral("onDislikeSyncAction"),
        [](const QJsonArray &, std::function<void(const QString &, const QJsonValue &)> done) {
            done(QString(), QJsonValue());
        });
}

void LxSyncServer::removeSession(LxSession *s)
{
    if (!m_sessions.removeAll(s)) { s->deleteLater(); return; }
    if (s->rpc) s->rpc->destroy();
    if (m_syncingClient == s->key.clientId) m_syncingClient.clear();
    if (m_modeSession == s) answerSyncMode(QStringLiteral("cancel"));
    emit devicesChanged();
    emit syncEvent(QStringLiteral("设备已断开：%1").arg(s->key.deviceName));
    s->deleteLater();
}

void LxSyncServer::closeAllSessions()
{
    const auto sessions = m_sessions;
    for (auto *s : sessions) if (s->ws) s->ws->closeConnection(LxSync::kCloseNormal);
}

// ===========================================================================
// 本地库 ↔ listData
// ===========================================================================

QJsonObject LxSyncServer::localListData()
{
    QJsonObject d = m_library->buildLxListData();
    if (!m_preservedLoveLocal.isEmpty()) {
        QJsonArray love = d.value(QStringLiteral("loveList")).toArray();
        for (const QJsonValue &v : m_preservedLoveLocal) love.append(v);
        d[QStringLiteral("loveList")] = love;
    }
    QJsonArray ul = d.value(QStringLiteral("userList")).toArray();
    for (int i = 0; i < ul.size(); ++i) {
        QJsonObject l = ul.at(i).toObject();
        const QJsonArray extra = m_preservedListLocal.value(l.value(QStringLiteral("id")).toString());
        if (extra.isEmpty()) continue;
        QJsonArray musics = l.value(QStringLiteral("list")).toArray();
        for (const QJsonValue &v : extra) musics.append(v);
        l[QStringLiteral("list")] = musics;
        ul.replace(i, l);
    }
    d[QStringLiteral("userList")] = ul;
    return d;
}

void LxSyncServer::applyListDataToLibrary(const QJsonObject &data)
{
    m_applyingLibrary = true;
    m_preservedLoveLocal = QJsonArray();
    for (const QJsonValue &v : data.value(QStringLiteral("loveList")).toArray())
        if (v.toObject().value(QStringLiteral("source")).toString() == QLatin1String("local"))
            m_preservedLoveLocal.append(v);
    QHash<QString, QJsonArray> lists;
    for (const QJsonValue &lv : data.value(QStringLiteral("userList")).toArray()) {
        const QJsonObject l = lv.toObject();
        QJsonArray extra;
        for (const QJsonValue &mv : l.value(QStringLiteral("list")).toArray())
            if (mv.toObject().value(QStringLiteral("source")).toString() == QLatin1String("local"))
                extra.append(mv);
        if (!extra.isEmpty()) lists.insert(l.value(QStringLiteral("id")).toString(), extra);
    }
    m_preservedListLocal = lists;
    m_library->applyLxListData(data);
    m_lastPushCanonical = canonicalLocal();      // 同步刷新指纹，抑制紧随的回环 push
    m_applyingLibrary = false;
    savePreserved();
}

QByteArray LxSyncServer::canonicalLocal()
{
    return LxListMerge::canonicalJson(localListData());
}

void LxSyncServer::savePreserved()
{
    QJsonObject root;
    root[QStringLiteral("love")] = m_preservedLoveLocal;
    QJsonObject lists;
    for (auto it = m_preservedListLocal.constBegin(); it != m_preservedListLocal.constEnd(); ++it)
        lists[it.key()] = it.value();
    root[QStringLiteral("lists")] = lists;
    QFile f(m_stateFilePath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void LxSyncServer::loadPreserved()
{
    QFile f(m_stateFilePath);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    m_preservedLoveLocal = root.value(QStringLiteral("love")).toArray();
    const QJsonObject lists = root.value(QStringLiteral("lists")).toObject();
    m_preservedListLocal.clear();
    for (auto it = lists.begin(); it != lists.end(); ++it)
        m_preservedListLocal.insert(it.key(), it.value().toArray());
}

// ===========================================================================
// 同步流程
// ===========================================================================

void LxSyncServer::sessionFail(LxSession *s, const QString &err)
{
    if (m_syncingClient == s->key.clientId) m_syncingClient.clear();
    if (m_modeSession == s) answerSyncMode(QStringLiteral("cancel"));
    emit syncEvent(QStringLiteral("与「%1」同步失败：%2").arg(s->key.deviceName, err));
    s->ready = false;
    if (s->ws) s->ws->closeConnection(LxSync::kCloseFailed, err.left(100).toUtf8());
}

void LxSyncServer::startListSync(LxSession *s)
{
    if (!m_sessions.contains(s) || s->ready) return;
    if (!m_syncingClient.isEmpty() && m_syncingClient != s->key.clientId) {
        QTimer::singleShot(1000, this, [this, s]() { startListSync(s); });
        return;
    }
    m_syncingClient = s->key.clientId;
    emit syncEvent(QStringLiteral("开始与「%1」同步…").arg(s->key.deviceName));

    const QString devKey = m_store->deviceSnapshotKey(s->key.clientId);
    const QByteArray snap = m_store->getSnapshot(devKey);
    if (!devKey.isEmpty() && !snap.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(snap);
        if (doc.isObject()) { runSnapshotMerge(s, doc.object()); return; }
    }
    runFirstSync(s);
}

void LxSyncServer::finishListSync(LxSession *s)
{
    s->rpc->call(groupList(), QStringLiteral("list_sync_finished"), {},
                 [this, s](const QString &err, const QJsonValue &) {
        if (!m_sessions.contains(s)) return;
        if (!err.isEmpty()) { sessionFail(s, err); return; }
        s->ready = true;
        if (m_syncingClient == s->key.clientId) m_syncingClient.clear();
        emit devicesChanged();
        emit syncEvent(QStringLiteral("与「%1」同步完成").arg(s->key.deviceName));
        s->rpc->call(QString(), QStringLiteral("finished"), {},
                     [](const QString &, const QJsonValue &) {});
    });
}

void LxSyncServer::broadcastOverwrite(LxSession *exclude, const QJsonObject &data, const QString &key)
{
    QJsonObject action;
    action[QStringLiteral("action")] = QStringLiteral("list_data_overwrite");
    action[QStringLiteral("data")] = data;
    const auto sessions = m_sessions;
    for (auto *other : sessions) {
        if (other == exclude || !other->ready) continue;
        other->rpc->call(groupList(), QStringLiteral("onListSyncAction"), { action },
                         [this, other, key](const QString &err, const QJsonValue &) {
            if (!m_sessions.contains(other)) return;
            if (!err.isEmpty()) { other->ready = false; other->ws->closeConnection(LxSync::kCloseFailed); return; }
            m_store->updateDeviceSnapshotKey(other->key.clientId, key);
        });
    }
    m_lastPushCanonical = LxListMerge::canonicalJson(data);   // 刚广播的内容不再重复推
}

void LxSyncServer::runFirstSync(LxSession *s)
{
    const QJsonObject local = localListData();
    s->rpc->call(groupList(), QStringLiteral("list_sync_get_list_data"), {},
                 [this, s, local](const QString &err, const QJsonValue &data) {
        if (!m_sessions.contains(s)) return;
        if (!err.isEmpty()) { sessionFail(s, err); return; }
        const QJsonObject remote = patchListData(data.toObject());
        const bool localEmpty = LxListMerge::isEmpty(local);
        const bool remoteEmpty = LxListMerge::isEmpty(remote);

        if (localEmpty && remoteEmpty) { finishListSync(s); return; }

        if (localEmpty) {
            // 本机空 → 拉远端
            applyListDataToLibrary(remote);
            const QString key = m_store->createSnapshot(canonicalLocal());
            m_store->updateDeviceSnapshotKey(s->key.clientId, key);
            finishListSync(s);
            return;
        }
        if (remoteEmpty) {
            // 手机空 → 推本机
            const QString key = m_store->createSnapshot(canonicalLocal());
            s->rpc->call(groupList(), QStringLiteral("list_sync_set_list_data"), { local },
                         [this, s, key](const QString &e2, const QJsonValue &) {
                if (!m_sessions.contains(s)) return;
                if (!e2.isEmpty()) { sessionFail(s, e2); return; }
                m_store->updateDeviceSnapshotKey(s->key.clientId, key);
                finishListSync(s);
            });
            return;
        }
        // 双端非空 → 用户选模式
        requestSyncMode(s, [this, s, local, remote](const QString &mode) {
            if (!m_sessions.contains(s)) return;
            if (mode.isEmpty() || mode == QLatin1String("cancel")) { sessionFail(s, QStringLiteral("已取消")); return; }
            QJsonObject merged;
            bool updLocal = true, updRemote = true;
            if (mode == QLatin1String("merge_local_remote"))            merged = LxListMerge::mergeData(local, remote);
            else if (mode == QLatin1String("merge_remote_local"))       merged = LxListMerge::mergeData(remote, local);
            else if (mode == QLatin1String("overwrite_local_remote"))   merged = LxListMerge::overwriteData(local, remote);
            else if (mode == QLatin1String("overwrite_remote_local"))   merged = LxListMerge::overwriteData(remote, local);
            else if (mode == QLatin1String("overwrite_local_remote_full")) { merged = local;  updLocal = false; }
            else if (mode == QLatin1String("overwrite_remote_local_full")) { merged = remote; updRemote = false; }
            else { sessionFail(s, QStringLiteral("未知模式 %1").arg(mode)); return; }

            QString key;
            if (updLocal) {
                applyListDataToLibrary(merged);
                key = m_store->createSnapshot(canonicalLocal());
                QJsonObject payload = localListData();
                if (updRemote) {
                    // 同时下发给本机这一路，避免重复广播给自己
                    s->rpc->call(groupList(), QStringLiteral("list_sync_set_list_data"), { payload },
                                 [this, s, key](const QString &e2, const QJsonValue &) {
                        if (!m_sessions.contains(s)) return;
                        if (!e2.isEmpty()) { sessionFail(s, e2); return; }
                        m_store->updateDeviceSnapshotKey(s->key.clientId, key);
                        finishListSync(s);
                    });
                } else {
                    m_store->updateDeviceSnapshotKey(s->key.clientId, key);
                    broadcastOverwrite(s, payload, key);
                    finishListSync(s);
                }
                return;
            }
            // updLocal==false：本机保持不动，仅把 remote 全量下发到本机这一路
            if (key.isEmpty()) key = m_store->createSnapshot(canonicalLocal());
            const QJsonObject payload = localListData();
            s->rpc->call(groupList(), QStringLiteral("list_sync_set_list_data"), { payload },
                         [this, s, key](const QString &e2, const QJsonValue &) {
                if (!m_sessions.contains(s)) return;
                if (!e2.isEmpty()) { sessionFail(s, e2); return; }
                m_store->updateDeviceSnapshotKey(s->key.clientId, key);
                finishListSync(s);
            });
        });
    });
}

void LxSyncServer::runSnapshotMerge(LxSession *s, const QJsonObject &snapshot)
{
    s->rpc->call(groupList(), QStringLiteral("list_sync_get_md5"), {},
                 [this, s, snapshot](const QString &err, const QJsonValue &md5v) {
        if (!m_sessions.contains(s)) return;
        if (!err.isEmpty()) { sessionFail(s, err); return; }
        const QString currentKey = m_store->createSnapshot(canonicalLocal());
        const QString userKey = m_store->deviceSnapshotKey(s->key.clientId);
        if (md5v.toString() == currentKey) {
            if (userKey != currentKey) m_store->updateDeviceSnapshotKey(s->key.clientId, currentKey);
            finishListSync(s);
            return;
        }
        s->rpc->call(groupList(), QStringLiteral("list_sync_get_list_data"), {},
                     [this, s, snapshot](const QString &e2, const QJsonValue &data) {
            if (!m_sessions.contains(s)) return;
            if (!e2.isEmpty()) { sessionFail(s, e2); return; }
            const QJsonObject remote = patchListData(data.toObject());
            const QJsonObject merged = LxListMerge::mergeFromSnapshot(localListData(), remote, snapshot);
            applyListDataToLibrary(merged);
            const QString key = m_store->createSnapshot(canonicalLocal());
            const QJsonObject payload = localListData();
            s->rpc->call(groupList(), QStringLiteral("list_sync_set_list_data"), { payload },
                         [this, s, key, payload](const QString &e3, const QJsonValue &) {
                if (!m_sessions.contains(s)) return;
                if (!e3.isEmpty()) { sessionFail(s, e3); return; }
                m_store->updateDeviceSnapshotKey(s->key.clientId, key);
                broadcastOverwrite(s, payload, key);
                finishListSync(s);
            });
        });
    });
}

void LxSyncServer::onListAction(LxSession *s, const QJsonObject &action,
                                std::function<void(const QString &, const QJsonValue &)> done)
{
    if (!s->ready) { done(QString(), QJsonValue()); return; }
    QJsonObject cur = localListData();
    const bool ok = LxListMerge::applyAction(cur, action);
    if (!ok) {
        done(QStringLiteral("unknown list sync action"), QJsonValue());
        return;
    }
    applyListDataToLibrary(cur);
    const QString key = m_store->createSnapshot(canonicalLocal());
    m_store->updateDeviceSnapshotKey(s->key.clientId, key);
    const QJsonObject payload = localListData();
    broadcastOverwrite(s, payload, key);
    done(QString(), QJsonValue());
}

// ===========================================================================
// 模式选择 / 心跳 / 广播
// ===========================================================================

void LxSyncServer::requestSyncMode(LxSession *s, std::function<void(const QString &)> done)
{
    if (m_modeCb) { done(QStringLiteral("cancel")); return; }
    m_modeCb = std::move(done);
    m_modeSession = s;
    m_modeDevice = s->key.deviceName;
    m_modeTimeout->start();
    emit modeDialogVisibleChanged();
    emit modeDialogRequested(m_modeDevice);
}

void LxSyncServer::answerSyncMode(const QString &mode)
{
    if (!m_modeCb) return;
    m_modeTimeout->stop();
    auto cb = std::exchange(m_modeCb, nullptr);
    m_modeSession = nullptr;
    m_modeDevice.clear();
    emit modeDialogVisibleChanged();
    cb(mode);
}

void LxSyncServer::onHeartbeatTick()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto sessions = m_sessions;
    for (auto *s : sessions) {
        if (!s->ws || !s->ws->isOpen()) continue;
        if (s->lastPingAt > 0 && s->ws->lastActivityMs() < s->lastPingAt) {
            qInfo("[lxsync] 心跳超时：%s", qUtf8Printable(s->key.deviceName));
            s->ws->abort();
            continue;
        }
        s->lastPingAt = now;
        s->ws->sendPing();
        s->ws->sendText("ping");
    }
}

void LxSyncServer::pushSnapshotToClients()
{
    if (m_applyingLibrary) { return; }
    const QByteArray canon = canonicalLocal();
    if (canon == m_lastPushCanonical) return;     // 无实质变化 / 回环抑制
    const QString key = m_store->createSnapshot(canon);
    const QJsonObject data = localListData();
    m_lastPushCanonical = canon;
    const auto sessions = m_sessions;
    for (auto *s : sessions) {
        if (!s->ready) continue;
        QJsonObject action;
        action[QStringLiteral("action")] = QStringLiteral("list_data_overwrite");
        action[QStringLiteral("data")] = data;
        s->rpc->call(groupList(), QStringLiteral("onListSyncAction"), { action },
                     [this, s, key](const QString &err, const QJsonValue &) {
            if (!m_sessions.contains(s)) return;
            if (!err.isEmpty()) { s->ready = false; s->ws->closeConnection(LxSync::kCloseFailed); return; }
            m_store->updateDeviceSnapshotKey(s->key.clientId, key);
        });
    }
}

void LxSyncServer::rotateCode()
{
    m_code = LxSync::generateAuthCode();
    emit authCodeChanged();
}

void LxSyncServer::regenerateCode() { rotateCode(); }

QVariantList LxSyncServer::devices() const
{
    QVariantList out;
    const auto saved = m_store->allClientKeyInfo();
    for (const auto &k : saved) {
        QVariantMap m;
        m[QStringLiteral("clientId")] = k.clientId;
        m[QStringLiteral("deviceName")] = k.deviceName;
        m[QStringLiteral("isMobile")] = k.isMobile;
        m[QStringLiteral("lastConnectDate")] = k.lastConnectDate;
        bool online = false;
        for (auto *s : m_sessions) if (s->key.clientId == k.clientId) { online = true; break; }
        m[QStringLiteral("online")] = online;
        out.append(m);
    }
    return out;
}

void LxSyncServer::removeDevice(const QString &clientId)
{
    const auto sessions = m_sessions;
    for (auto *s : sessions) if (s->key.clientId == clientId) s->ws->closeConnection(LxSync::kCloseNormal);
    m_store->removeClientKeyInfo(clientId);
    emit devicesChanged();
}

} // namespace Muyun
