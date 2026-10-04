#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <QMutex>
#include <functional>

extern "C" {
#include "quickjs.h"
}

namespace Muyun {

/**
 * @brief LX 自定义音源脚本引擎（QuickJS 桥接）
 *
 * 按 LX Music 官方协议（桌面版文档 + 移动版 user-api-preload.js 逐条对照）实现：
 *   globalThis.lx = { EVENT_NAMES, on, send, request, utils, env, version, currentScriptInfo }
 *
 * - `lx.on(lx.EVENT_NAMES.request, handler)`：handler 收到 { source, action, info }，
 *   必须返回 Promise（播放链接字符串）；
 * - `lx.send(lx.EVENT_NAMES.inited, { sources })`：声明支持的源/音质/动作（按协议过滤后入库）；
 *   `lx.send(lx.EVENT_NAMES.updateAlert, { log, updateUrl })`：源更新提示，每次运行只能调一次；
 *   on/send 都返回 Promise（与官方一致，脚本会 .then/.catch）；
 * - `lx.request(url, options, cb)`：options 支持 method/headers/body/form/formData/timeout/binary，
 *   cb 入参 (err, resp, body)，resp={statusCode,statusMessage,headers,body}；**返回取消函数**；
 * - `lx.utils.buffer.from/bufToString`、`crypto.md5/aesEncrypt/rsaEncrypt/randomBytes`、
 *   `zlib.inflate/deflate`（Promise<Buffer>）；
 * - `lx.currentScriptInfo` 填脚本头部注释解析出的真值（含 rawScript）；
 * - 宿主 API：`setTimeout` / `clearTimeout`（QuickJS 沙箱里由本引擎的定时器泵驱动）。
 *
 * 兼容性取舍：`lx.env` 固定 `desktop`（桌面版），所以 Buffer 语义按 Node Buffer 来
 * （既支持 `buf.toString('base64')`，也满足移动版的 `bufToString(buf,'base64')` /
 * `ArrayBuffer.isView`）；`crypto.md5` 按桌面版对 UTF-8 字节求哈希。
 *
 * C++ 侧调用 musicUrl() 时：触发 handler -> 驱动 QuickJS 微任务/定时器直到 Promise settle。
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

    /// 请求播放链接；quality 形如 128k / 320k / flac / flac24bit（协议音质）
    QString musicUrl(const QString &source, const QVariantMap &musicInfo,
                     const QString &quality, QString *error = nullptr);

    /// 请求歌词（local 源 lyric action，协议 info = 歌曲信息对象；返回 rawLrc 文本或空）
    QString lyric(const QString &source, const QVariantMap &musicInfo,
                  QString *error = nullptr);
    /// 请求封面图（local 源 pic action，返回图片 URL 或空）
    QString pic(const QString &source, const QVariantMap &musicInfo,
                QString *error = nullptr);

    /// 脚本元信息（send('inited') 时填充；sources 已按协议过滤）
    QVariantMap scriptInfo() const { return m_scriptInfo; }

    /// 脚本为某个源声明的音质（协议值）；没声明/没这个源返回空
    QStringList declaredQualitys(const QString &source) const;

    /// 应用音质 id → 协议音质 id（master/atmos/hires 归到 flac24bit，其余同名）
    static QString protocolQualityId(const QString &appQualityId);

    /// 脚本头部注释解析出的信息（currentScriptInfo 的真值来源）
    static QVariantMap parseScriptMeta(const QString &code);

    /**
     * @brief 是不是**新版**（globalThis.lx）音源脚本
     *
     * 只认新协议：EVENT_NAMES / globalThis.lx + inited。旧格式（脚本侧 registerSource、
     * userApi 宿主环境）我们从来没实现过——旧脚本执行即 ReferenceError，与其让它
     * 报一堆看不懂的错，不如导入时就明说"仅支持新版格式"。
     */
    static bool looksLikeLxScript(const QString &code);

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
    /// aesEncrypt(buffer, mode, key, iv) —— 参数顺序按官方协议
    static JSValue jsAesEncrypt(JSContext *ctx, JSValueConst self,
                                int argc, JSValueConst *argv);
    static JSValue jsAesDecrypt(JSContext *ctx, JSValueConst self,
                                int argc, JSValueConst *argv);
    static JSValue jsRandomBytes(JSContext *ctx, JSValueConst self,
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
    static JSValue jsZlibInflate(JSContext *ctx, JSValueConst self,
                                 int argc, JSValueConst *argv);
    static JSValue jsZlibDeflate(JSContext *ctx, JSValueConst self,
                                 int argc, JSValueConst *argv);
    static JSValue jsSetTimeout(JSContext *ctx, JSValueConst self,
                                int argc, JSValueConst *argv);
    static JSValue jsClearTimeout(JSContext *ctx, JSValueConst self,
                                  int argc, JSValueConst *argv);
    static JSValue jsPrint(JSContext *ctx, JSValueConst self,
                           int argc, JSValueConst *argv);

    /// 未处理的 Promise 拒绝上报。QuickJS 不设 tracker 就**静默吞掉**拒绝——
    /// 脚本在 .then/.catch 里抛错时，宿主只会看到"初始化超时"，最难查的一类坑（四-61 踩过）。
    static void onPromiseRejection(JSContext *ctx, JSValueConst promise, JSValueConst reason,
                                   int is_handled, void *opaque);

    void registerApi();
    void runPendingJobs(int maxMs);
    bool pumpUntilDone(int maxMs);
    /// 触发到点的 setTimeout（在微任务泵里调用；回调本体也跑在调用线程）
    void runDueTimers();
    void clearTimers();

    /// 从 JSContext 取回引擎实例（存于 context opaque，避免静态全局）
    static LxScriptEngine *selfOf(JSContext *ctx);

    /// 通用 action 请求：构造 { source, action, info } → 触发 handler → 驱动微任务/
    /// 定时器直到 Promise settle → 返回字符串结果（lyric 的 {lrc} 对象也会被解出来）
    QString requestString(const QString &source, const QString &action,
                          const QVariantMap &infoMap, QString *error);

    JSRuntime *m_rt = nullptr;
    JSContext *m_ctx = nullptr;
    JSValue m_handler = JS_UNDEFINED;
    bool m_inited = false;
    QString m_initError;
    QVariantMap m_scriptInfo;
    QVariantMap m_updateInfo;   ///< send('updateAlert') 上报的更新信息
    bool m_updateAlertSent = false;  ///< 协议：updateAlert 每次运行只能调一次
    UpdateAlertCallback m_updateAlertCallback;  ///< send('updateAlert') 时触发
    QString m_lastResult;     ///< Promise settle 后的结果（字符串或 url）
    bool m_done = false;      ///< 结果是否已就绪

    /// setTimeout 登记表：JS 侧回调存在全局 __lx_timers[id]（GC 根），C++ 只记到期时刻
    struct Timer { int id; qint64 dueMs; };
    QVector<Timer> m_timers;
    int m_nextTimerId = 1;

    /// QuickJS 非线程安全，加锁保证 loadScript/musicUrl 互斥
    mutable QMutex m_mutex;
};

} // namespace Muyun
