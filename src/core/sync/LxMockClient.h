#pragma once

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <functional>

class QTcpSocket;

namespace Muyun {

class LxWsConnection;
class LxMsg2Call;

/**
 * @brief 洛雪移动客户端模拟器（仅供 --test-lxsync 端到端自检）。
 *
 * 完整复刻手机端流程：/hello → /id → /ah(口令鉴权，RSA-OAEP 解回包) →
 * /socket WebSocket 升级 → message2call 双向 RPC → list 快照同步。
 * 与真实 app 的差异仅在：RSA 密钥对用固定 fixture、同步模式由电脑侧
 * 弹窗决定（与本 mock 无关）。
 */
class LxMockClient : public QObject
{
    Q_OBJECT
public:
    explicit LxMockClient(QObject *parent = nullptr);
    ~LxMockClient() override;

    /// 手机侧库（listData 结构 JSON）
    void setPhoneLibrary(const QJsonObject &listData);
    QJsonObject phoneLibrary() const { return m_listData; }

    /// 启动：port + 认证码（口令鉴权路径）；每阶段完成 emit done(pass, detail)
    void start(int port, const QString &authCode);
    /// 重连（复用已配对 clientId/key，走 keyAuth + 快照合并路径）
    void reconnect(int port);
    /// 主动断开当前 WS（重连测试用）
    void disconnectNow();

    /// 手机侧模拟用户改库：把 musicInfo 加进 loveList 并推 onListSyncAction 给电脑
    void phoneAddFavorite(const QJsonObject &musicInfo);

    // 流程观测（测试断言用）
    int setListDataCalls() const { return m_setListDataCalls; }
    int getListDataCalls() const { return m_getListDataCalls; }
    int finishedCalls() const { return m_finishedCalls; }
    QString clientId() const { return m_clientId; }
    QString serverName() const { return m_serverName; }

    /// 测试用固定 RSA 私钥（PKCS#1 base64）与 SPKI 公钥（裸 base64）
    static QByteArray fixturePrivateB64();
    static QByteArray fixturePublicSpkiB64();

signals:
    void done(bool pass, const QString &detail);
    void logLine(const QString &text);

private:
    void httpGet(int port, const QString &path, const QHash<QByteArray, QByteArray> &headers,
                 std::function<void(int code, const QByteArray &body)> cb);
    void openSocket(int port);
    void registerRpcFunctions();
    void phaseFinished(bool pass, const QString &detail);
    static QJsonObject patchLocal(QJsonObject d);

    QTcpSocket *m_sock = nullptr;
    LxWsConnection *m_ws = nullptr;
    LxMsg2Call *m_rpc = nullptr;

    QString m_clientId, m_key;      // 鉴权所得
    QJsonObject m_listData;         // 手机侧库
    QString m_serverName;

    int m_setListDataCalls = 0;     // 电脑推给手机的次数
    int m_getListDataCalls = 0;     // 电脑向手机拉库的次数
    int m_finishedCalls = 0;        // 电脑发 finished 的次数
};

} // namespace Muyun
