#pragma once

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QPointer>
#include <functional>

#include "core/sync/LxSnapshotStore.h"

class QTcpSocket;
class QTimer;

namespace Muyun {

class LibraryController;
class LxSnapshotStore;
struct LxSession;

/**
 * @brief 洛雪音乐（lx-music-mobile / lx-music-desktop 客户端）局域网同步服务端。
 *
 * 与 SyncController 共用同一 TCP 端口：HTTP 路径 /hello /id /ah 与
 * WebSocket 升级 /socket?i=&t= 由本类识别接管；其余仍走暮云自有
 * /api/export /api/import（暮云↔暮云）。
 *
 * 同步范围（用户拍板）：洛雪 loveList ↔ 暮云在线收藏、userList ↔ 自建歌单；
 * defaultList（试听列表）与本地歌曲不参与，但手机端 local 来源条目原样回存
 * （preserved.json），避免覆盖同步误删手机数据。dislike 模块跳过（暮云无该概念）。
 *
 * 首次同步两库都非空时弹模式选择（modeDialogRequested → QML → answerSyncMode）；
 * 之后走快照三方合并，静默收敛。全局同一时刻仅一个同步流程（洛雪 syncingId 语义）。
 */
class LxSyncServer : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString authCode READ authCode NOTIFY authCodeChanged)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(bool modeDialogVisible READ modeDialogVisible NOTIFY modeDialogVisibleChanged)
    Q_PROPERTY(QString modeDeviceName READ modeDeviceName NOTIFY modeDialogVisibleChanged)
    Q_PROPERTY(int onlineCount READ onlineCount NOTIFY devicesChanged)

public:
    enum class HttpRequestResult {
        NotHandled,      ///< 不是洛雪端点，交回 SyncController
        RespondedClosed, ///< 已回文本响应并断开
        Adopted,         ///< 已升级为 WebSocket 会话（socket 所有权移交本类）
    };

    explicit LxSyncServer(LibraryController *library, QObject *parent = nullptr);
    ~LxSyncServer() override;

    QString authCode() const { return m_code; }
    QVariantList devices() const;
    bool modeDialogVisible() const { return m_modeCb != nullptr; }
    QString modeDeviceName() const { return m_modeDevice; }
    int onlineCount() const { return m_sessions.size(); }

    /// 尝试作为洛雪服务端处理一个完整 HTTP 请求头（到 \r\n\r\n，无 body）
    HttpRequestResult handleHttp(QTcpSocket *sock, const QByteArray &head);
    /// 服务关闭：踢掉全部会话
    void closeAllSessions();

    Q_INVOKABLE void removeDevice(const QString &clientId);
    Q_INVOKABLE void regenerateCode();
    /// QML 模式对话框回答（mode ∈ 洛雪 6 种 SyncMode 或 "cancel"）
    Q_INVOKABLE void answerSyncMode(const QString &mode);

signals:
    void authCodeChanged();
    void devicesChanged();
    void modeDialogVisibleChanged();
    /// 需要用户在 6 种同步模式中选择
    void modeDialogRequested(const QString &deviceName);
    /// 状态/结果文本（QML toast / 状态行）
    void syncEvent(const QString &text);

private:
    void adoptSession(QTcpSocket *sock, const QByteArray &head,
                      const LxSnapshotStore::KeyInfo &info);
    void bindSessionRpc(LxSession *s);
    void removeSession(LxSession *s);
    void sessionFail(LxSession *s, const QString &err);
    void startListSync(LxSession *s);
    void finishListSync(LxSession *s);
    void runFirstSync(LxSession *s);
    void runSnapshotMerge(LxSession *s, const QJsonObject &snapshot);
    void broadcastOverwrite(LxSession *exclude, const QJsonObject &data, const QString &key);
    void onListAction(LxSession *s, const QJsonObject &action,
                      std::function<void(const QString &, const QJsonValue &)> done);

    QJsonObject localListData();
    void applyListDataToLibrary(const QJsonObject &data);
    QByteArray canonicalLocal();
    void savePreserved();
    void loadPreserved();
    void requestSyncMode(LxSession *s, std::function<void(const QString &)> done);
    void pushSnapshotToClients();
    void rotateCode();
    void onHeartbeatTick();

    LibraryController *m_library = nullptr;
    LxSnapshotStore *m_store = nullptr;
    QList<LxSession *> m_sessions;
    QString m_syncingClient;                 ///< 全局同步互斥（按 clientId）

    QString m_code;
    QTimer *m_codeTimer = nullptr;           ///< 3 分钟轮换（对齐洛雪桌面端）
    QTimer *m_beatTimer = nullptr;           ///< 30s 心跳
    QTimer *m_pushTimer = nullptr;           ///< 本地库变化防抖
    bool m_applyingLibrary = false;          ///< 环回抑制标志
    QByteArray m_lastPushCanonical;          ///< 环回抑制指纹

    std::function<void(const QString &)> m_modeCb;
    QPointer<LxSession> m_modeSession;
    QString m_modeDevice;
    QTimer *m_modeTimeout = nullptr;         ///< 10 分钟无应答 → cancel

    QJsonArray m_preservedLoveLocal;                  ///< 手机端 local 收藏回存
    QHash<QString, QJsonArray> m_preservedListLocal;  ///< 手机端歌单 local 回存
    QString m_stateFilePath;
};

} // namespace Muyun
