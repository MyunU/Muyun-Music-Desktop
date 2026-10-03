#pragma once

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QHash>
#include <QVector>
#include <functional>

class QTimer;

namespace Muyun {

/**
 * @brief message2call v0.1.3 协议移植（洛雪同步的 RPC 帧层）。
 *
 * 线上格式（JSON 文本，可能外层 gzip "cg_"）：
 *   请求   {name: "方法名__随机数", path: ["方法名"], data: [参数...]}
 *   应答   {name: "同一事件名", error: null|string, data: 结果}
 * 语义照抄 JS 实现：
 *   - 收到带 path → 查本地注册函数执行 → 回 {error:null,data}（无 data 则省略键）
 *     未知方法回 {error:"xxx is not defined"}，异常回 {error:msg}
 *   - 收到无 path → 匹配挂起调用并回调
 *   - group（如 "list"/"dislike"）串行：前一调用未回时后入队；
 *     **任一失败会级联 reject 队列中全部等待者**（洛雪取消同步依赖此行为）
 *   - 出站调用 120s 超时回 "timeout"
 */
class LxMsg2Call : public QObject
{
    Q_OBJECT
public:
    /// error 非空 = 失败。done 可异步任意次延迟调用一次。
    using Handler = std::function<void(const QJsonArray &args,
                                       std::function<void(const QString &error,
                                                          const QJsonValue &data)>)>;
    using Sender = std::function<bool(const QJsonObject &msg)>;

    explicit LxMsg2Call(QObject *parent = nullptr);
    ~LxMsg2Call() override;

    void setSender(Sender s) { m_sender = std::move(s); }
    void registerFunction(const QString &name, Handler h) { m_funcs.insert(name, std::move(h)); }

    /// 入站一条（已解码/解压的 JSON 文本）
    void onMessage(const QByteArray &utf8Json);

    /// 出站调用。group 空 = 不排队直发。cb 在应答/超时/destroy/级联失败时恰好触发一次。
    void call(const QString &group, const QString &name, const QJsonArray &args,
              std::function<void(const QString &error, const QJsonValue &data)> cb);

    void destroy();     // 所有挂起调用回 "destroy"

private:
    struct Pending {
        QString group;
        std::function<void(const QString &, const QJsonValue &)> cb;
        QTimer *timer = nullptr;
        bool settled = false;
    };
    struct Group {
        bool handling = false;
        QList<std::pair<std::function<void()>, std::function<void(const QString &)>>> waiters;
    };

    QString newEventName(const QString &method);
    void sendResponse(const QString &eventName, const QString &error, const QJsonValue &data);
    void settle(const QString &eventName, const QString &error, const QJsonValue &data);
    void startCall(const QString &group, const QString &name, const QJsonArray &args,
                   std::function<void(const QString &, const QJsonValue &)> cb);
    void pumpGroup(const QString &group, const QString &error);   // error 非空=级联拒绝
    void invokeLocal(const QString &eventName, const QString &method, const QJsonArray &args);

    Sender m_sender;
    QHash<QString, Handler> m_funcs;
    QHash<QString, Pending> m_pending;
    QHash<QString, Group> m_groups;
    quint64 m_eventSeq = 0;
    bool m_destroyed = false;
};

} // namespace Muyun
