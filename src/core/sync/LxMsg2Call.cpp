#include "LxMsg2Call.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QTimer>

namespace Muyun {

LxMsg2Call::LxMsg2Call(QObject *parent) : QObject(parent) {}

LxMsg2Call::~LxMsg2Call() { destroy(); }

QString LxMsg2Call::newEventName(const QString &method)
{
    return QStringLiteral("%1__%2").arg(method).arg(++m_eventSeq);
}

void LxMsg2Call::settle(const QString &eventName, const QString &error, const QJsonValue &data)
{
    auto it = m_pending.find(eventName);
    if (it == m_pending.end()) return;
    Pending p = it.value();
    m_pending.erase(it);
    if (p.timer) { p.timer->stop(); p.timer->deleteLater(); }
    if (p.cb) p.cb(error, data);
    if (!p.group.isEmpty()) pumpGroup(p.group, error);
}

void LxMsg2Call::startCall(const QString &group, const QString &name, const QJsonArray &args,
                           std::function<void(const QString &, const QJsonValue &)> cb)
{
    const QString eventName = newEventName(name);
    Pending p;
    p.group = group;
    p.cb = std::move(cb);
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, eventName]() {
        settle(eventName, QStringLiteral("timeout"), QJsonValue());
    });
    p.timer = timer;
    timer->start(120 * 1000);
    m_pending.insert(eventName, p);

    QJsonObject msg;
    msg[QStringLiteral("name")] = eventName;
    QJsonArray path; path.append(name);
    msg[QStringLiteral("path")] = path;
    msg[QStringLiteral("data")] = args;
    if (m_sender) m_sender(msg);
}

void LxMsg2Call::pumpGroup(const QString &group, const QString &error)
{
    QTimer::singleShot(0, this, [this, group, error]() {
        auto it = m_groups.find(group);
        if (it == m_groups.end()) return;
        Group &g = it.value();
        g.handling = false;
        if (g.waiters.isEmpty()) return;
        auto waiter = g.waiters.takeFirst();
        if (error.isEmpty()) {
            waiter.first();                       // start
        } else {
            waiter.second(error);                 // reject（其内部再级联 pumpGroup）
        }
    });
}

void LxMsg2Call::call(const QString &group, const QString &name, const QJsonArray &args,
                      std::function<void(const QString &, const QJsonValue &)> cb)
{
    if (m_destroyed) { if (cb) cb(QStringLiteral("destroyed"), QJsonValue()); return; }
    if (group.isEmpty()) { startCall(group, name, args, std::move(cb)); return; }

    Group &g = m_groups[group];
    if (g.handling) {
        std::function<void()> start = [this, group, name, args, cb]() {
            m_groups[group].handling = true;
            startCall(group, name, args, cb);
        };
        std::function<void(const QString &)> reject = [this, group, cb](const QString &err) {
            if (cb) cb(err, QJsonValue());
            pumpGroup(group, err);               // 级联拒绝后续排队者
        };
        g.waiters.append({ std::move(start), std::move(reject) });
    } else {
        g.handling = true;
        startCall(group, name, args, std::move(cb));
    }
}

void LxMsg2Call::sendResponse(const QString &eventName, const QString &error, const QJsonValue &data)
{
    QJsonObject msg;
    msg[QStringLiteral("name")] = eventName;
    if (error.isEmpty()) {
        msg[QStringLiteral("error")] = QJsonValue::Null;
        if (!data.isUndefined()) msg[QStringLiteral("data")] = data;
    } else {
        msg[QStringLiteral("error")] = error;
    }
    if (m_sender) m_sender(msg);
}

void LxMsg2Call::invokeLocal(const QString &eventName, const QString &method, const QJsonArray &args)
{
    auto it = m_funcs.find(method);
    if (it == m_funcs.end()) {
        sendResponse(eventName, QStringLiteral("%1 is not defined").arg(method), QJsonValue());
        return;
    }
    Handler h = it.value();
    h(args, [this, eventName](const QString &err, const QJsonValue &data) {
        sendResponse(eventName, err, data);
    });
}

void LxMsg2Call::onMessage(const QByteArray &utf8Json)
{
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(utf8Json, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) return;
    const QJsonObject obj = doc.object();
    const QString name = obj.value(QStringLiteral("name")).toString();
    if (name.isEmpty()) return;

    const QJsonValue pathVal = obj.value(QStringLiteral("path"));
    if (pathVal.isArray() && !pathVal.toArray().isEmpty()) {
        const QJsonArray path = pathVal.toArray();
        const QString method = path.at(path.size() - 1).toString();
        const QJsonArray args = obj.value(QStringLiteral("data")).toArray();
        invokeLocal(name, method, args);
    } else {
        // 应答：匹配挂起调用
        if (!m_pending.contains(name)) return;
        const QJsonValue errVal = obj.value(QStringLiteral("error"));
        const QString error = errVal.isNull() ? QString() : errVal.toString();
        settle(name, error, obj.value(QStringLiteral("data")));
    }
}

void LxMsg2Call::destroy()
{
    if (m_destroyed) return;
    m_destroyed = true;
    const QStringList names = m_pending.keys();
    for (const QString &n : names) settle(n, QStringLiteral("destroy"), QJsonValue());
}

} // namespace Muyun
