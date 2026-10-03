#pragma once

#include <QString>
#include <QVariantMap>
#include <QMutex>
#include <functional>

extern "C" {
#include "quickjs.h"
}

namespace Muyun {

/**
 * @brief LX 自定义音源脚本引擎（QuickJS 桥接）
 *
 * 为 LX 音乐自定义音源脚本提供运行环境，实现 lx-music API 的核心子集：
 *   globalThis.lx = { EVENT_NAMES, on, send, request, utils, env, version, currentScriptInfo }
 *
 * - 脚本通过 lx.on(lx.EVENT_NAMES.request, handler) 注册请求处理器；
 * - handler 收到 { source, action, info }，返回 Promise（播放链接字符串或一个含 url 的对象）；
 * - lx.request(url, options) 返回 Promise<{statusCode, body, headers}>，桥接到 HttpClient；
 * - lx.utils.crypto 提供 md5 / aesEn / aesDe / rsaEncrypt / base64。
 *
 * C++ 侧调用 musicUrl() 时：触发 handler -> 驱动 QuickJS 微任务直到 Promise settle。
 * 全部运行在调用线程，HTTP 使用局部 QNetworkAccessManager，跨线程安全。
 */
class LxScriptEngine
{
public:
    LxScriptEngine();
    ~LxScriptEngine();

    /// 加载并执行音源脚本（执行脚本本体，等待 send('inited')）
    bool loadScript(const QString &path, QString *error = nullptr);

    bool inited() const { return m_inited; }
    QString initError() const { return m_initError; }

    /// 请求播放链接；quality 形如 128k / 320k / flac / flac24bit
    QString musicUrl(const QString &source, const QVariantMap &musicInfo,
                     const QString &quality, QString *error = nullptr);

    /// 脚本元信息（send('inited') 时填充）
    QVariantMap scriptInfo() const { return m_scriptInfo; }

    /// 脚本上报的更新信息（send('updateAlert') 时填充），无更新返回空 Map
    QVariantMap updateAlert() const { return m_updateInfo; }

    /// 更新提醒回调：脚本 send('updateAlert') 时触发（异步，可能晚于 loadScript 返回）
    using UpdateAlertCallback = std::function<void(const QVariantMap &)>;
    void setUpdateAlertCallback(UpdateAlertCallback cb) { m_updateAlertCallback = cb; }

private:
    // 宿主 API（静态 C 函数，通过 JS_GetContextOpaque 取回实例）
    static JSValue jsRequest(JSContext *ctx, JSValueConst self,
                             int argc, JSValueConst *argv);
    static JSValue jsOn(JSContext *ctx, JSValueConst self,
                        int argc, JSValueConst *argv);
    static JSValue jsSend(JSContext *ctx, JSValueConst self,
                          int argc, JSValueConst *argv);
    static JSValue jsMd5(JSContext *ctx, JSValueConst self,
                         int argc, JSValueConst *argv);
    static JSValue jsAesEn(JSContext *ctx, JSValueConst self,
                           int argc, JSValueConst *argv);
    static JSValue jsAesDe(JSContext *ctx, JSValueConst self,
                           int argc, JSValueConst *argv);
    static JSValue jsRsaEncrypt(JSContext *ctx, JSValueConst self,
                                int argc, JSValueConst *argv);
    static JSValue jsB64Enc(JSContext *ctx, JSValueConst self,
                            int argc, JSValueConst *argv);
    static JSValue jsB64Dec(JSContext *ctx, JSValueConst self,
                            int argc, JSValueConst *argv);
    static JSValue jsBufFrom(JSContext *ctx, JSValueConst self,
                             int argc, JSValueConst *argv);
    static JSValue jsBufToString(JSContext *ctx, JSValueConst self,
                                 int argc, JSValueConst *argv);
    static JSValue jsPrint(JSContext *ctx, JSValueConst self,
                           int argc, JSValueConst *argv);

    void registerApi();
    void runPendingJobs(int maxMs);
    bool pumpUntilDone(int maxMs);

    /// 从 JSContext 取回引擎实例（存于 context opaque，避免静态全局）
    static LxScriptEngine *selfOf(JSContext *ctx);

    JSRuntime *m_rt = nullptr;
    JSContext *m_ctx = nullptr;
    JSValue m_handler = JS_UNDEFINED;
    bool m_inited = false;
    QString m_initError;
    QVariantMap m_scriptInfo;
    QVariantMap m_updateInfo;   ///< send('updateAlert') 上报的更新信息
    UpdateAlertCallback m_updateAlertCallback;  ///< send('updateAlert') 时触发
    QString m_lastResult;     ///< Promise settle 后的结果（字符串或 url）
    bool m_done = false;      ///< 结果是否已就绪

    /// QuickJS 非线程安全，加锁保证 loadScript/musicUrl 互斥
    mutable QMutex m_mutex;
};

} // namespace Muyun
