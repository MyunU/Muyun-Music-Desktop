#include "LxSnapshotStore.h"

#include "core/storage/DocumentStore.h"
#include "LxUtil.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSysInfo>
#include <QCryptographicHash>
#include <algorithm>

namespace Muyun {

namespace {
constexpr int kMaxSnapshot = 10;

QByteArray md5Hex(const QByteArray &d)
{
    return QCryptographicHash::hash(d, QCryptographicHash::Md5).toHex();
}

bool writeJsonFile(const QString &path, const QJsonObject &obj)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    return true;
}

QJsonObject readJsonFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QJsonDocument d = QJsonDocument::fromJson(f.readAll());
    return d.isObject() ? d.object() : QJsonObject();
}
}

QJsonObject LxSnapshotStore::KeyInfo::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("clientId")] = clientId;
    o[QStringLiteral("key")] = key;
    o[QStringLiteral("deviceName")] = deviceName;
    o[QStringLiteral("isMobile")] = isMobile;
    o[QStringLiteral("lastConnectDate")] = double(lastConnectDate);
    return o;
}

LxSnapshotStore::KeyInfo LxSnapshotStore::KeyInfo::fromJson(const QJsonObject &o)
{
    KeyInfo k;
    k.clientId = o.value(QStringLiteral("clientId")).toString();
    k.key = o.value(QStringLiteral("key")).toString();
    k.deviceName = o.value(QStringLiteral("deviceName")).toString();
    k.isMobile = o.value(QStringLiteral("isMobile")).toBool();
    k.lastConnectDate = qint64(o.value(QStringLiteral("lastConnectDate")).toDouble());
    return k;
}

LxSnapshotStore::LxSnapshotStore(QObject *parent) : QObject(parent)
{
    m_dir = DocumentStore::instance()->rootPath() + QStringLiteral("/lx-sync");
    QDir().mkpath(m_dir + QStringLiteral("/list/snapshot"));

    const QJsonObject si = readJsonFile(m_dir + QStringLiteral("/serverInfo.json"));
    m_serverId = si.value(QStringLiteral("serverId")).toString();
    if (m_serverId.isEmpty()) {
        m_serverId = LxSync::randomB64(16);
        QJsonObject o;
        o[QStringLiteral("serverId")] = m_serverId;
        o[QStringLiteral("version")] = 2;
        writeJsonFile(m_dir + QStringLiteral("/serverInfo.json"), o);
    }

    loadDevices();
    loadSnapshotInfo();
}

QString LxSnapshotStore::serverId()
{
    return m_serverId;
}

QString LxSnapshotStore::serverName() const
{
    return QSysInfo::machineHostName();
}

// ---------------------------------------------------------------------------
// 设备密钥
// ---------------------------------------------------------------------------

LxSnapshotStore::KeyInfo LxSnapshotStore::createClientKeyInfo(const QString &deviceName, bool isMobile)
{
    KeyInfo k;
    k.clientId = LxSync::randomB64(16);
    k.key = LxSync::randomB64(16);
    k.deviceName = deviceName;
    k.isMobile = isMobile;
    k.lastConnectDate = 0;
    return k;
}

bool LxSnapshotStore::getClientKeyInfo(const QString &clientId, KeyInfo *out) const
{
    auto it = m_devices.constFind(clientId);
    if (it == m_devices.constEnd()) return false;
    if (out) *out = it.value();
    return true;
}

void LxSnapshotStore::saveClientKeyInfo(const KeyInfo &info)
{
    m_devices.insert(info.clientId, info);
    loadDevicesSave();
}

void LxSnapshotStore::removeClientKeyInfo(const QString &clientId)
{
    m_devices.remove(clientId);
    // 清其快照引用
    QJsonObject clients = m_snapshotInfo.value(QStringLiteral("clients")).toObject();
    clients.remove(clientId);
    m_snapshotInfo[QStringLiteral("clients")] = clients;
    rebuildClientSnapshotKeys();
    saveSnapshotInfo();
    loadDevicesSave();
}

QList<LxSnapshotStore::KeyInfo> LxSnapshotStore::allClientKeyInfo() const
{
    QList<KeyInfo> out = m_devices.values();
    std::sort(out.begin(), out.end(), [](const KeyInfo &a, const KeyInfo &b) {
        return a.lastConnectDate > b.lastConnectDate;
    });
    return out;
}

// ---------------------------------------------------------------------------
// 快照链
// ---------------------------------------------------------------------------

QString LxSnapshotStore::createSnapshot(const QByteArray &canonicalData)
{
    const QString md5 = QString::fromLatin1(md5Hex(canonicalData));
    const QString latest = m_snapshotInfo.value(QStringLiteral("latest")).toString();
    if (latest == md5) return md5;

    QJsonArray list = m_snapshotInfo.value(QStringLiteral("list")).toArray();
    int existing = -1;
    for (int i = 0; i < list.size(); ++i) if (list.at(i).toString() == md5) { existing = i; break; }
    if (existing >= 0) {
        list.removeAt(existing);
    } else {
        QFile f(m_dir + QStringLiteral("/list/snapshot/snapshot_") + md5);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return md5;
        f.write(canonicalData);
    }
    if (!latest.isEmpty()) list.prepend(latest);
    m_snapshotInfo[QStringLiteral("latest")] = md5;
    m_snapshotInfo[QStringLiteral("time")] = double(QDateTime::currentMSecsSinceEpoch());
    m_snapshotInfo[QStringLiteral("list")] = list;
    saveSnapshotInfo();
    clearOldSnapshots();
    return md5;
}

QByteArray LxSnapshotStore::getSnapshot(const QString &key) const
{
    if (key.isEmpty()) return {};
    QFile f(m_dir + QStringLiteral("/list/snapshot/snapshot_") + key);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

QString LxSnapshotStore::deviceSnapshotKey(const QString &clientId) const
{
    const QJsonObject clients = m_snapshotInfo.value(QStringLiteral("clients")).toObject();
    return clients.value(clientId).toObject().value(QStringLiteral("snapshotKey")).toString();
}

void LxSnapshotStore::updateDeviceSnapshotKey(const QString &clientId, const QString &key)
{
    QJsonObject clients = m_snapshotInfo.value(QStringLiteral("clients")).toObject();
    QJsonObject c = clients.value(clientId).toObject();
    c[QStringLiteral("snapshotKey")] = key;
    c[QStringLiteral("lastSyncDate")] = double(QDateTime::currentMSecsSinceEpoch());
    clients[clientId] = c;
    m_snapshotInfo[QStringLiteral("clients")] = clients;
    rebuildClientSnapshotKeys();
    saveSnapshotInfo();
}

// ---------------------------------------------------------------------------
// 内部
// ---------------------------------------------------------------------------

void LxSnapshotStore::loadDevices()
{
    m_clientSnapshotKeys.clear();
    const QJsonObject root = readJsonFile(m_dir + QStringLiteral("/devices.json"));
    const QJsonObject clients = root.value(QStringLiteral("clients")).toObject();
    m_devices.clear();
    for (auto it = clients.begin(); it != clients.end(); ++it)
        m_devices.insert(it.key(), KeyInfo::fromJson(it.value().toObject()));
}

void LxSnapshotStore::loadDevicesSave()
{
    QJsonObject clients;
    for (auto it = m_devices.constBegin(); it != m_devices.constEnd(); ++it)
        clients[it.key()] = it.value().toJson();
    QJsonObject root;
    root[QStringLiteral("userName")] = QStringLiteral("default");
    root[QStringLiteral("clients")] = clients;
    writeJsonFile(m_dir + QStringLiteral("/devices.json"), root);
}

void LxSnapshotStore::loadSnapshotInfo()
{
    QJsonObject info = readJsonFile(m_dir + QStringLiteral("/list/snapshotInfo.json"));
    if (info.isEmpty()) {
        info[QStringLiteral("latest")] = QString();
        info[QStringLiteral("time")] = 0;
        info[QStringLiteral("list")] = QJsonArray();
        info[QStringLiteral("clients")] = QJsonObject();
    }
    m_snapshotInfo = info;
    rebuildClientSnapshotKeys();
}

void LxSnapshotStore::saveSnapshotInfo()
{
    writeJsonFile(m_dir + QStringLiteral("/list/snapshotInfo.json"), m_snapshotInfo);
}

void LxSnapshotStore::rebuildClientSnapshotKeys()
{
    m_clientSnapshotKeys.clear();
    const QJsonObject clients = m_snapshotInfo.value(QStringLiteral("clients")).toObject();
    for (auto it = clients.begin(); it != clients.end(); ++it) {
        const QString k = it.value().toObject().value(QStringLiteral("snapshotKey")).toString();
        if (!k.isEmpty()) m_clientSnapshotKeys.insert(k);
    }
}

void LxSnapshotStore::clearOldSnapshots()
{
    QJsonArray list = m_snapshotInfo.value(QStringLiteral("list")).toArray();
    QList<QString> removable;
    for (const QJsonValue &v : list) {
        const QString k = v.toString();
        if (!m_clientSnapshotKeys.contains(k)) removable.append(k);
    }
    bool changed = false;
    while (removable.size() > kMaxSnapshot) {
        const QString k = removable.takeLast();
        QFile::remove(m_dir + QStringLiteral("/list/snapshot/snapshot_") + k);
        int idx = -1;
        for (int i = 0; i < list.size(); ++i) if (list.at(i).toString() == k) { idx = i; break; }
        if (idx >= 0) { list.removeAt(idx); changed = true; }
    }
    if (changed) {
        m_snapshotInfo[QStringLiteral("list")] = list;
        saveSnapshotInfo();
    }
}

} // namespace Muyun
