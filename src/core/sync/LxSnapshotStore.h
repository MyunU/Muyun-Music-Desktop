#pragma once

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QSet>

namespace Muyun {

/**
 * @brief 洛雪同步服务端持久化：serverId、设备密钥表、list 快照链。
 *
 * 目录 ~/.muyun/lx-sync/（跟随 DocumentStore 根，受 MUYUN_STORE_ROOT 隔离）：
 *   serverInfo.json           {serverId, version:2}
 *   devices.json              {userName:"default", clients:{clientId:{...keyInfo}}}
 *   list/snapshotInfo.json    {latest,time,list:[md5],clients:{id:{snapshotKey,lastSyncDate}}}
 *   list/snapshot/snapshot_<md5>   快照原文（canonical listData JSON）
 * 语义对齐 lx-music-desktop server 的 user/data.ts + list/snapshotDataManage.ts，
 * 快照数上限 10（设备引用中的快照不删）。
 */
class LxSnapshotStore : public QObject
{
    Q_OBJECT
public:
    struct KeyInfo {
        QString clientId;
        QString key;         // base64(16B) AES key
        QString deviceName;
        bool isMobile = false;
        qint64 lastConnectDate = 0;
        QJsonObject toJson() const;
        static KeyInfo fromJson(const QJsonObject &o);
    };

    explicit LxSnapshotStore(QObject *parent = nullptr);

    /// 服务端标识（随机 16 字节 base64，持久化；/id 端点返回）
    QString serverId();
    /// serverName（鉴权回包），用机器名
    QString serverName() const;

    // ---- 设备密钥 ----
    KeyInfo createClientKeyInfo(const QString &deviceName, bool isMobile);
    bool getClientKeyInfo(const QString &clientId, KeyInfo *out) const;
    void saveClientKeyInfo(const KeyInfo &info);      // 也用于更新 deviceName/lastConnectDate
    void removeClientKeyInfo(const QString &clientId); // 连带清其快照引用
    QList<KeyInfo> allClientKeyInfo() const;           // 按最近连接排序

    // ---- 快照链 ----
    /// 记录快照（内容寻址），返回 md5 键；与 latest 相同则复用
    QString createSnapshot(const QByteArray &canonicalData);
    /// 取快照原文；无则返回空
    QByteArray getSnapshot(const QString &key) const;
    QString currentKey() const { return m_snapshotInfo.value(QStringLiteral("latest")).toString(); }

    QString deviceSnapshotKey(const QString &clientId) const;
    void updateDeviceSnapshotKey(const QString &clientId, const QString &key);

private:
    void loadDevices();
    void loadDevicesSave();
    void loadSnapshotInfo();
    void saveSnapshotInfo();
    void rebuildClientSnapshotKeys();
    void clearOldSnapshots();

    QString m_dir;                 // ~/.muyun/lx-sync
    QString m_serverId;
    QHash<QString, KeyInfo> m_devices;   // clientId -> keyInfo
    QJsonObject m_snapshotInfo;    // latest/time/list/clients
    QSet<QString> m_clientSnapshotKeys;
};

} // namespace Muyun
