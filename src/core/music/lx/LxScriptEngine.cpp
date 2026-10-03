#include "LxScriptEngine.h"

#include "core/utils/Crypto.h"
#include "core/network/HttpClient.h"

#include <QFile>
#include <QNetworkAccessManager>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QDebug>

extern "C" {
#include "quickjs.h"
}

namespace Muyun {

LxScriptEngine *LxScriptEngine::selfOf(JSContext *ctx)
{
    return static_cast<LxScriptEngine *>(JS_GetContextOpaque(ctx));
}

namespace {

// ---------------------------------------------------------------------------
// JS <-> QVariant 转换
// ---------------------------------------------------------------------------

QVariant jsToVariant(JSContext *ctx, JSValueConst v)
{
    if (JS_IsBool(v)) return JS_ToBool(ctx, v) != 0;
    if (JS_IsNumber(v)) { double d = 0; JS_ToFloat64(ctx, &d, v); return d; }
    if (JS_IsString(v)) {
        size_t len = 0;
        const char *s = JS_ToCStringLen(ctx, &len, v);
        QString out = QString::fromUtf8(s, static_cast<int>(len));
        JS_FreeCString(ctx, s);
        return out;
    }
    if (JS_IsArray(ctx, v)) {
        QVariantList list;
        JSValue lenVal = JS_GetPropertyStr(ctx, v, "length");
        int32_t n = 0; JS_ToInt32(ctx, &n, lenVal);
        JS_FreeValue(ctx, lenVal);
        for (int32_t i = 0; i < n; ++i)
            list.append(jsToVariant(ctx, JS_GetPropertyUint32(ctx, v, static_cast<uint32_t>(i))));
        return list;
    }
    if (JS_IsObject(v)) {
        QVariantMap out;
        JSPropertyEnum *props = nullptr;
        uint32_t n = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &n, v,
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0)
            return out;
        for (uint32_t i = 0; i < n; ++i) {
            const char *key = JS_AtomToCString(ctx, props[i].atom);
            out[QString::fromUtf8(key)] = jsToVariant(ctx, JS_GetProperty(ctx, v, props[i].atom));
            JS_FreeCString(ctx, key);
            JS_FreeAtom(ctx, props[i].atom);
        }
        js_free(ctx, props);
        return out;
    }
    return QVariant();
}

QVariantMap jsObjToVariantSafe(JSContext *ctx, JSValueConst v)
{
    if (!JS_IsObject(v)) return QVariantMap();
    return jsToVariant(ctx, v).toMap();
}

JSValue variantToJs(JSContext *ctx, const QVariant &var)
{
    switch (var.typeId()) {
    case QMetaType::Bool:   return JS_NewBool(ctx, var.toBool());
    case QMetaType::Int:    return JS_NewInt32(ctx, var.toInt());
    case QMetaType::Double: return JS_NewFloat64(ctx, var.toDouble());
    case QMetaType::QStringList:
    case QMetaType::QVariantList: {
        JSValue arr = JS_NewArray(ctx);
        const QVariantList list = var.toList();
        for (int i = 0; i < list.size(); ++i)
            JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), variantToJs(ctx, list.at(i)));
        return arr;
    }
    case QMetaType::QVariantMap: {
        JSValue obj = JS_NewObject(ctx);
        const QVariantMap m = var.toMap();
        for (auto it = m.constBegin(); it != m.constEnd(); ++it) {
            const QByteArray key = it.key().toUtf8();
            JS_SetPropertyStr(ctx, obj, key.constData(), variantToJs(ctx, it.value()));
        }
        return obj;
    }
    default: {
        const QByteArray b = var.toString().toUtf8();
        return JS_NewStringLen(ctx, b.constData(), static_cast<size_t>(b.size()));
    }
    }
}

} // namespace

// ===========================================================================
// 构造 / 析构
// ===========================================================================

LxScriptEngine::LxScriptEngine()
{
    m_rt = JS_NewRuntime();
    JS_SetMaxStackSize(m_rt, 4 * 1024 * 1024);
    m_ctx = JS_NewContext(m_rt);
    JS_SetContextOpaque(m_ctx, this);
    registerApi();
}

LxScriptEngine::~LxScriptEngine()
{
    if (m_ctx) {
        JS_SetContextOpaque(m_ctx, nullptr);
        JS_FreeValue(m_ctx, m_handler);
        JS_FreeContext(m_ctx);
    }
    if (m_rt) JS_FreeRuntime(m_rt);
}

// ===========================================================================
// 注册 lx API
// ===========================================================================

void LxScriptEngine::registerApi()
{
    JSValue global = JS_GetGlobalObject(m_ctx);

    JSValue lx = JS_NewObject(m_ctx);

    // EVENT_NAMES
    JSValue names = JS_NewObject(m_ctx);
    JS_SetPropertyStr(m_ctx, names, "request",     JS_NewString(m_ctx, "request"));
    JS_SetPropertyStr(m_ctx, names, "inited",       JS_NewString(m_ctx, "inited"));
    JS_SetPropertyStr(m_ctx, names, "updateAlert",  JS_NewString(m_ctx, "updateAlert"));
    JS_SetPropertyStr(m_ctx, lx, "EVENT_NAMES", names);

    // 基础方法
    JS_SetPropertyStr(m_ctx, lx, "on",   JS_NewCFunction(m_ctx, jsOn,   "on",   2));
    JS_SetPropertyStr(m_ctx, lx, "send", JS_NewCFunction(m_ctx, jsSend, "send", 2));
    JS_SetPropertyStr(m_ctx, lx, "request", JS_NewCFunction(m_ctx, jsRequest, "request", 2));

    // utils.crypto
    JSValue utils  = JS_NewObject(m_ctx);
    JSValue crypto = JS_NewObject(m_ctx);
    JS_SetPropertyStr(m_ctx, crypto, "md5",         JS_NewCFunction(m_ctx, jsMd5,         "md5",         1));
    JS_SetPropertyStr(m_ctx, crypto, "aesEn",       JS_NewCFunction(m_ctx, jsAesEn,       "aesEn",       4));
    JS_SetPropertyStr(m_ctx, crypto, "aesDe",       JS_NewCFunction(m_ctx, jsAesDe,       "aesDe",       4));
    JS_SetPropertyStr(m_ctx, crypto, "rsaEncrypt",  JS_NewCFunction(m_ctx, jsRsaEncrypt,  "rsaEncrypt",  2));
    JS_SetPropertyStr(m_ctx, crypto, "base64Encode",JS_NewCFunction(m_ctx, jsB64Enc,      "base64Encode", 1));
    JS_SetPropertyStr(m_ctx, crypto, "base64Decode",JS_NewCFunction(m_ctx, jsB64Dec,      "base64Decode", 1));
    JS_SetPropertyStr(m_ctx, utils, "crypto", crypto);

    // utils.buffer：base64/utf8 字符串转换（LX 脚本签名常用）
    JSValue buffer = JS_NewObject(m_ctx);
    JS_SetPropertyStr(m_ctx, buffer, "from",       JS_NewCFunction(m_ctx, jsBufFrom,       "from",       2));
    JS_SetPropertyStr(m_ctx, buffer, "bufToString",JS_NewCFunction(m_ctx, jsBufToString,   "bufToString", 2));
    JS_SetPropertyStr(m_ctx, buffer, "toBase64",   JS_NewCFunction(m_ctx, jsB64Enc,        "toBase64",    1));
    JS_SetPropertyStr(m_ctx, buffer, "fromBase64", JS_NewCFunction(m_ctx, jsB64Dec,        "fromBase64",  1));
    JS_SetPropertyStr(m_ctx, utils, "buffer", buffer);
    JS_SetPropertyStr(m_ctx, lx, "utils", utils);

    // 脚本调试输出通道（console.* 的底层实现）
    JS_SetPropertyStr(m_ctx, global, "__lx_print",
        JS_NewCFunction(m_ctx, jsPrint, "__lx_print", 1));

    // env / version / script info
    JS_SetPropertyStr(m_ctx, lx, "env",     JS_NewString(m_ctx, "desktop"));
    JS_SetPropertyStr(m_ctx, lx, "version", JS_NewString(m_ctx, "2.0.0"));
    JS_SetPropertyStr(m_ctx, lx, "currentScriptInfo", JS_NewObject(m_ctx));

    JS_SetPropertyStr(m_ctx, global, "lx", lx);

    // -----------------------------------------------------------------------
    // 全局辅助：Buffer / atob / btoa（依赖上面的 crypto 绑定）
    // -----------------------------------------------------------------------
    static const char *shim =
        "globalThis.atob = function(s){ return globalThis.lx.utils.crypto.base64Decode(s); };"
        "globalThis.btoa = function(s){ return globalThis.lx.utils.crypto.base64Encode(s); };"
        "globalThis.Buffer = {"
        "  from: function(data, enc){"
        "    return {"
        "      toString: function(e2){"
        "        if (e2 === 'base64') return globalThis.btoa(data);"
        "        if (e2 === 'hex') {"
        "          var o=''; for (var i=0;i<data.length;i++) o += data.charCodeAt(i).toString(16).padStart(2,'0');"
        "          return o;"
        "        }"
        "        return data;"
        "      },"
        "      toBase64: function(){ return globalThis.btoa(data); }"
        "    };"
        "  },"
        "  fromBase64: function(b64){ return globalThis.atob(b64); }"
        "};"
        // console：脚本大量使用 console.log，缺失会抛 ReferenceError 中断初始化
        "globalThis.console = (function(){"
        "  function fmt(a){"
        "    var out='';"
        "    for (var i=0;i<a.length;i++){"
        "      var v=a[i];"
        "      try {"
        "        if (typeof v === 'object' && v !== null) out += JSON.stringify(v);"
        "        else out += String(v);"
        "      } catch(e) { out += '[unprintable]'; }"
        "      if (i < a.length-1) out += ' ';"
        "    }"
        "    return out;"
        "  }"
        "  function emit(){ try { globalThis.__lx_print(fmt(arguments)); } catch(e) {} }"
        "  return { log: emit, info: emit, warn: emit, error: emit, debug: emit };"
        "})();";
    JSValue r = JS_Eval(m_ctx, shim, strlen(shim), "<lx-shim>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue exc = JS_GetException(m_ctx);
        size_t l = 0; const char *msg = JS_ToCStringLen(m_ctx, &l, exc);
        qWarning() << "[LxScriptEngine] shim 注入失败:" << (msg ? QString::fromUtf8(msg, (int)l) : QString());
        if (msg) JS_FreeCString(m_ctx, msg);
        JS_FreeValue(m_ctx, exc);
    }
    JS_FreeValue(m_ctx, r);

    JS_FreeValue(m_ctx, global);
}

// ===========================================================================
// 宿主函数实现
// ===========================================================================

JSValue LxScriptEngine::jsOn(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    LxScriptEngine *self = selfOf(ctx);
    if (argc < 2 || !self) return JS_UNDEFINED;
    size_t len = 0;
    const char *name = JS_ToCStringLen(ctx, &len, argv[0]);
    const QString ev = QString::fromUtf8(name, static_cast<int>(len));
    JS_FreeCString(ctx, name);

    if (ev == QStringLiteral("request")) {
        JS_FreeValue(self->m_ctx, self->m_handler);
        self->m_handler = JS_DupValue(self->m_ctx, argv[1]);
        // 关键：把 handler 挂到全局对象做 GC 根。
        // QuickJS 的保守 GC 扫不到 C++ 类成员变量，若仅存裸 JSValue，
        // 脚本执行完 GC 会回收该函数对象，导致后续 JS_Call 崩溃。
        JSValue g = JS_GetGlobalObject(self->m_ctx);
        JS_SetPropertyStr(self->m_ctx, g, "__lx_handler", JS_DupValue(self->m_ctx, argv[1]));
        JS_FreeValue(self->m_ctx, g);
    }
    return JS_UNDEFINED;
}

JSValue LxScriptEngine::jsSend(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    LxScriptEngine *self = selfOf(ctx);
    if (argc < 2 || !self) return JS_UNDEFINED;
    size_t len = 0;
    const char *name = JS_ToCStringLen(ctx, &len, argv[0]);
    const QString ev = QString::fromUtf8(name, static_cast<int>(len));
    JS_FreeCString(ctx, name);

    if (ev == QStringLiteral("inited")) {
        const QVariantMap info = jsObjToVariantSafe(self->m_ctx, argv[1]);
        self->m_scriptInfo = info;
        const QVariant status = info.value(QStringLiteral("status"));
        self->m_inited = status.isValid() ? status.toBool() : true;
    } else if (ev == QStringLiteral("updateAlert")) {
        // 脚本上报的更新推送：{ log, updateUrl } 或 { log, updateUrl } 变体
        self->m_updateInfo = jsObjToVariantSafe(self->m_ctx, argv[1]);
        // 异步通知：脚本可能在 loadScript 返回后才 send('updateAlert')，
        // 需回调让上层（SettingsController）立即感知并弹窗
        if (self->m_updateAlertCallback) {
            self->m_updateAlertCallback(self->m_updateInfo);
        }
    }
    return JS_UNDEFINED;
}

JSValue LxScriptEngine::jsRequest(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1 || !selfOf(ctx)) return JS_ThrowTypeError(ctx, "request(url, options, cb?)");

    // URL
    size_t len = 0;
    const char *urlS = JS_ToCStringLen(ctx, &len, argv[0]);
    const QString url = QString::fromUtf8(urlS, static_cast<int>(len));
    JS_FreeCString(ctx, urlS);

    // options
    QVariantMap options;
    QString method = QStringLiteral("GET");
    QByteArray body;
    QVariantMap form;
    if (argc > 1 && JS_IsObject(argv[1])) {
        options = jsObjToVariantSafe(ctx, argv[1]);
        method = options.value(QStringLiteral("method"), QStringLiteral("get"))
                     .toString().toUpper();
        const QVariant b = options.value(QStringLiteral("body"));
        if (b.typeId() == QMetaType::QByteArray)            body = b.toByteArray();
        else if (b.canConvert<QString>())                   body = b.toString().toUtf8();
        form = options.value(QStringLiteral("formData")).toMap();
        if (form.isEmpty()) form = options.value(QStringLiteral("form")).toMap();
    }

    HttpOptions opt;
    const QVariantMap headers = options.value(QStringLiteral("headers")).toMap();
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
        opt.headers[it.key()] = it.value().toString();
    opt.timeoutMs = options.value(QStringLiteral("timeout"), 20000).toInt();

    const bool isPost = (method == QStringLiteral("POST") || method == QStringLiteral("PUT"));
    QByteArray postBody = body;
    if (isPost && postBody.isEmpty() && !form.isEmpty())
        postBody = serializeForm(form);

    // 在调用线程用局部 QNAM 同步执行（自带阻塞事件循环，跨线程安全）
    QNetworkAccessManager nam;
    const HttpResponse resp =
        HttpClient::instance()->execRequest(&nam, url, postBody, isPost, opt);

    // 响应对象
    JSValue respObj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, respObj, "statusCode", JS_NewInt32(ctx, resp.status));
    JS_SetPropertyStr(ctx, respObj, "body",
        JS_NewStringLen(ctx, resp.body.constData(), static_cast<size_t>(resp.body.size())));
    JSValue hdrs = JS_NewObject(ctx);
    for (auto it = resp.headers.constBegin(); it != resp.headers.constEnd(); ++it) {
        const QByteArray k = it.key().toUtf8();
        const QByteArray v = it.value().toString().toUtf8();
        JS_SetPropertyStr(ctx, hdrs, k.constData(),
            JS_NewStringLen(ctx, v.constData(), static_cast<size_t>(v.size())));
    }
    JS_SetPropertyStr(ctx, respObj, "headers", hdrs);

    // 回调式：request(url, options, cb(err, resp))，返回 undefined
    if (argc > 2 && JS_IsFunction(ctx, argv[2])) {
        JSValue errVal = resp.ok ? JS_NULL
                                 : JS_NewString(ctx, resp.error.toUtf8().constData());
        JSValueConst cbArgs[2] = { errVal, respObj };
        JSValue ret = JS_Call(ctx, argv[2], JS_UNDEFINED, 2, cbArgs);
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, errVal);
        JS_FreeValue(ctx, respObj);
        return JS_UNDEFINED;
    }

    // Promise 式：返回已 resolve 的 Promise
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) { JS_FreeValue(ctx, respObj); return promise; }
    JSValueConst rargs[1] = { respObj };
    JSValue callR = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, rargs);
    JS_FreeValue(ctx, callR);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    JS_FreeValue(ctx, respObj);
    return promise;
}

JSValue LxScriptEngine::jsMd5(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_UNDEFINED;
    size_t len = 0;
    const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
    const QString hex = Crypto::md5Hex(QByteArray(s, static_cast<int>(len)));
    JS_FreeCString(ctx, s);
    const QByteArray b = hex.toUtf8();
    return JS_NewStringLen(ctx, b.constData(), static_cast<size_t>(b.size()));
}

JSValue LxScriptEngine::jsAesEn(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 4) return JS_UNDEFINED;
    auto toStr = [&](JSValueConst v) -> QString {
        size_t l = 0; const char *s = JS_ToCStringLen(ctx, &l, v);
        QString out = QString::fromUtf8(s, static_cast<int>(l));
        JS_FreeCString(ctx, s);
        return out;
    };
    const QByteArray data = toStr(argv[0]).toUtf8();
    const QByteArray key  = toStr(argv[2]).toUtf8();
    const QByteArray iv   = toStr(argv[3]).toUtf8();
    const QString mode = argc > 4 ? toStr(argv[4]) : QStringLiteral("aes-128-cbc");

    QByteArray out;
    if (mode.contains(QStringLiteral("ecb"), Qt::CaseInsensitive))
        out = Crypto::aes128EcbEncrypt(data, key);
    else
        out = Crypto::aes128CbcEncrypt(data, key, iv);
    const QByteArray b64 = out.toBase64();
    return JS_NewStringLen(ctx, b64.constData(), static_cast<size_t>(b64.size()));
}

JSValue LxScriptEngine::jsAesDe(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 4) return JS_UNDEFINED;
    auto toStr = [&](JSValueConst v) -> QString {
        size_t l = 0; const char *s = JS_ToCStringLen(ctx, &l, v);
        QString out = QString::fromUtf8(s, static_cast<int>(l));
        JS_FreeCString(ctx, s);
        return out;
    };
    const QByteArray data = QByteArray::fromBase64(toStr(argv[0]).toUtf8());
    const QByteArray key  = toStr(argv[2]).toUtf8();
    const QByteArray iv   = toStr(argv[3]).toUtf8();
    const QString mode = argc > 4 ? toStr(argv[4]) : QStringLiteral("aes-128-cbc");

    QByteArray out;
    if (mode.contains(QStringLiteral("ecb"), Qt::CaseInsensitive))
        out = Crypto::aes128EcbDecrypt(data, key);
    else
        out = Crypto::aes128CbcDecrypt(data, key, iv);
    return JS_NewStringLen(ctx, out.constData(), static_cast<size_t>(out.size()));
}

JSValue LxScriptEngine::jsRsaEncrypt(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    size_t l1 = 0; const char *s1 = JS_ToCStringLen(ctx, &l1, argv[0]);
    size_t l2 = 0; const char *s2 = JS_ToCStringLen(ctx, &l2, argv[1]);
    const QString data = QString::fromUtf8(s1, static_cast<int>(l1));
    const QString key  = QString::fromUtf8(s2, static_cast<int>(l2));
    JS_FreeCString(ctx, s1);
    JS_FreeCString(ctx, s2);
    const QString hex = Crypto::rsaNoPaddingEncryptHex(data.toUtf8(), key);
    const QByteArray b = hex.toUtf8();
    return JS_NewStringLen(ctx, b.constData(), static_cast<size_t>(b.size()));
}

JSValue LxScriptEngine::jsB64Enc(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_UNDEFINED;
    size_t l = 0; const char *s = JS_ToCStringLen(ctx, &l, argv[0]);
    const QByteArray out = Crypto::base64Encode(QByteArray(s, static_cast<int>(l)));
    JS_FreeCString(ctx, s);
    return JS_NewStringLen(ctx, out.constData(), static_cast<size_t>(out.size()));
}

JSValue LxScriptEngine::jsB64Dec(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_UNDEFINED;
    size_t l = 0; const char *s = JS_ToCStringLen(ctx, &l, argv[0]);
    const QByteArray out = Crypto::base64Decode(QByteArray(s, static_cast<int>(l)));
    JS_FreeCString(ctx, s);
    return JS_NewStringLen(ctx, out.constData(), static_cast<size_t>(out.size()));
}

JSValue LxScriptEngine::jsPrint(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    // console.* 的底层输出（stderr，便于脚本诊断）
    if (argc < 1) return JS_UNDEFINED;
    size_t l = 0; const char *s = JS_ToCStringLen(ctx, &l, argv[0]);
    if (s) {
        fprintf(stderr, "[lx-script] %s\n", s);
        fflush(stderr);
        JS_FreeCString(ctx, s);
    }
    return JS_UNDEFINED;
}

JSValue LxScriptEngine::jsBufFrom(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    // buffer.from(str, encoding) -> 返回一个带 toString/toBase64 的对象
    if (argc < 1) return JS_UNDEFINED;
    size_t l = 0; const char *s = JS_ToCStringLen(ctx, &l, argv[0]);
    const QString data = QString::fromUtf8(s, static_cast<int>(l));
    JS_FreeCString(ctx, s);

    QString enc = QStringLiteral("utf8");
    if (argc > 1) {
        size_t el = 0; const char *es = JS_ToCStringLen(ctx, &el, argv[1]);
        enc = QString::fromUtf8(es, static_cast<int>(el));
        JS_FreeCString(ctx, es);
    }

    const QByteArray b64 = Crypto::base64Encode(data.toUtf8());
    const QString b64Str = QString::fromLatin1(b64);

    // 构造 { toString(enc), toBase64() } 对象
    JSValue obj = JS_NewObject(ctx);
    JSValue jsData = JS_NewStringLen(ctx, data.toUtf8().constData(),
                                     static_cast<size_t>(data.toUtf8().size()));
    JSValue jsB64 = JS_NewStringLen(ctx, b64Str.toUtf8().constData(),
                                    static_cast<size_t>(b64Str.toUtf8().size()));

    static const char *shim =
        "(function(d, b){ return {"
        "  toString: function(e){ return e === 'base64' ? b : d; },"
        "  toBase64: function(){ return b; },"
        "  raw: d, base64: b"
        "}; })";
    JSValue fn = JS_Eval(ctx, shim, strlen(shim), "<buf>", JS_EVAL_TYPE_GLOBAL);
    JSValueConst args[2] = { jsData, jsB64 };
    JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, 2, args);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, jsData);
    JS_FreeValue(ctx, jsB64);
    JS_FreeValue(ctx, obj);
    return ret;
}

JSValue LxScriptEngine::jsBufToString(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    // buffer.bufToString(buf, encoding) -> 字符串
    if (argc < 1) return JS_UNDEFINED;
    size_t l = 0; const char *s = JS_ToCStringLen(ctx, &l, argv[0]);
    QString data = QString::fromUtf8(s, static_cast<int>(l));
    JS_FreeCString(ctx, s);

    QString enc = QStringLiteral("utf8");
    if (argc > 1) {
        size_t el = 0; const char *es = JS_ToCStringLen(ctx, &el, argv[1]);
        enc = QString::fromUtf8(es, static_cast<int>(el));
        JS_FreeCString(ctx, es);
    }

    // 若传入的是 buffer.from 产生的对象，取其 raw
    if (JS_IsObject(argv[0])) {
        JSValue raw = JS_GetPropertyStr(ctx, argv[0], "raw");
        if (JS_IsString(raw)) {
            size_t rl = 0; const char *rs = JS_ToCStringLen(ctx, &rl, raw);
            data = QString::fromUtf8(rs, static_cast<int>(rl));
            JS_FreeCString(ctx, rs);
        }
        JS_FreeValue(ctx, raw);
    }

    if (enc == QStringLiteral("base64")) {
        const QByteArray b64 = Crypto::base64Encode(data.toUtf8());
        return JS_NewStringLen(ctx, b64.constData(), static_cast<size_t>(b64.size()));
    }
    const QByteArray b = data.toUtf8();
    return JS_NewStringLen(ctx, b.constData(), static_cast<size_t>(b.size()));
}

// ===========================================================================
// 微任务驱动
// ===========================================================================

void LxScriptEngine::runPendingJobs(int maxMs)
{
    QElapsedTimer timer;
    timer.start();
    while (JS_IsJobPending(m_rt)) {
        int r = JS_ExecutePendingJob(m_rt, &m_ctx);
        if (r < 0) {
            // 任务抛错：必须取出异常以清空 context 的 pending exception 状态，
            // 否则后续所有 JS API（含 JS_NewObject）都会失败返回 exception。
            JSValue exc = JS_GetException(m_ctx);
            size_t l = 0; const char *msg = JS_ToCStringLen(m_ctx, &l, exc);
            if (msg) {
                m_initError = QString::fromUtf8(msg, static_cast<int>(l));
                JS_FreeCString(m_ctx, msg);
            }
            JS_FreeValue(m_ctx, exc);
            break;
        }
        if (timer.elapsed() > maxMs) break;
    }
}

/// 驱动 QuickJS 微任务 + Qt 事件，直到 m_done 置位或超时
bool LxScriptEngine::pumpUntilDone(int maxMs)
{
    QElapsedTimer timer;
    timer.start();
    m_done = false;
    while (!m_done && timer.elapsed() < maxMs) {
        if (JS_IsJobPending(m_rt)) {
            int r = JS_ExecutePendingJob(m_rt, &m_ctx);
            if (r < 0) {
                // 清空 pending exception，避免污染后续调用
                JSValue exc = JS_GetException(m_ctx);
                JS_FreeValue(m_ctx, exc);
                break;
            }
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return m_done;
}

// ===========================================================================
// 脚本加载
// ===========================================================================

bool LxScriptEngine::loadScript(const QString &path, QString *error)
{
    QMutexLocker locker(&m_mutex);

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法读取脚本文件");
        return false;
    }
    const QByteArray code = f.readAll();
    f.close();

    m_inited = false;
    m_initError.clear();
    m_scriptInfo.clear();
    m_updateInfo.clear();
    m_handler = JS_UNDEFINED;

    // 关键：完全重建 JS 运行环境（与 lx-music 官方行为一致——每个脚本一个全新隔离环境）。
    // QuickJS 的 JS_EVAL_TYPE_GLOBAL 会把顶层 const/let 保留在全局词法环境，
    // 复用 context 二次加载同一脚本会报 "SyntaxError: redeclaration of 'xxx'"。
    if (m_ctx) {
        JS_FreeContext(m_ctx);
        m_ctx = nullptr;
    }
    if (m_rt) {
        JS_FreeRuntime(m_rt);
        m_rt = nullptr;
    }
    m_rt = JS_NewRuntime();
    JS_SetMaxStackSize(m_rt, 4 * 1024 * 1024);
    m_ctx = JS_NewContext(m_rt);
    JS_SetContextOpaque(m_ctx, this);
    registerApi();

    const QByteArray pathUtf8 = path.toUtf8();
    JSValue ret = JS_Eval(m_ctx, code.constData(), static_cast<size_t>(code.size()),
                          pathUtf8.constData(), JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(ret)) {
        JSValue exc = JS_GetException(m_ctx);
        size_t l = 0;
        const char *msg = JS_ToCStringLen(m_ctx, &l, exc);
        m_initError = msg ? QString::fromUtf8(msg, static_cast<int>(l))
                          : QStringLiteral("脚本执行异常");
        if (msg) JS_FreeCString(m_ctx, msg);
        JS_FreeValue(m_ctx, exc);
        if (error) *error = m_initError;
        return false;
    }
    JS_FreeValue(m_ctx, ret);

    // 执行同步初始化产生的微任务，等待 send('inited')。
    // 部分脚本的 inited 依赖异步 HTTP 回调（先 request 再 send），需循环驱动微任务直到就绪。
    // 注意：m_inited 已在 JS_Eval 前重置，脚本顶层同步 send(inited) 会在这里已被置 true，
    // 不要再重置，否则同步脚本也会被误判为超时。
    {
        QElapsedTimer timer;
        timer.start();
        while (!m_inited && timer.elapsed() < 8000) {
            if (JS_IsJobPending(m_rt)) {
                int r = JS_ExecutePendingJob(m_rt, &m_ctx);
                if (r < 0) {
                    JSValue exc = JS_GetException(m_ctx);
                    JS_FreeValue(m_ctx, exc);
                    break;
                }
            } else {
                // 无待执行微任务时短暂让出，避免忙等（脚本的 request 是同步的，这里主要防死循环）
                break;
            }
        }

        // m_inited 为 true 后，继续驱动微任务，让 checkUpdate() 等异步逻辑有机会完成。
        // 脚本可能在 send('inited') 后调用 checkUpdate()，该函数发 HTTP 请求，
        // Promise 尚未 resolve 时退出循环会导致 send('updateAlert') 永远不会被调用。
        if (m_inited) {
            while (JS_IsJobPending(m_rt) && timer.elapsed() < 10000) {
                int r = JS_ExecutePendingJob(m_rt, &m_ctx);
                if (r < 0) {
                    JSValue exc = JS_GetException(m_ctx);
                    JS_FreeValue(m_ctx, exc);
                    break;
                }
            }
        }
    }

    if (!m_inited) {
        m_initError = QStringLiteral("脚本初始化超时（未调用 send('inited')）");
        if (error) *error = m_initError;
        return false;
    }
    if (m_scriptInfo.value(QStringLiteral("name")).toString().isEmpty())
        m_scriptInfo[QStringLiteral("name")] = QStringLiteral("LX 音源");
    return true;
}

// ===========================================================================
// 请求播放链接
// ===========================================================================

QString LxScriptEngine::musicUrl(const QString &source, const QVariantMap &musicInfo,
                                 const QString &quality, QString *error)
{
    QMutexLocker locker(&m_mutex);

    if (!m_inited || JS_IsUndefined(m_handler)) {
        if (error) *error = QStringLiteral("脚本未就绪");
        return QString();
    }

    // 构造入参（与真实 LX 一致：单个对象 { source, action, info }）
    // 注意：JS_SetPropertyStr 是「接管所有权」语义，传入的值由属性持有，
    // 不能再 JS_FreeValue，否则双重释放。
    const QByteArray sourceUtf8 = source.toUtf8();
    const QByteArray qualityUtf8 = quality.toUtf8();
    JSValue data = JS_NewObject(m_ctx);
    JS_SetPropertyStr(m_ctx, data, "source",
        JS_NewStringLen(m_ctx, sourceUtf8.constData(), static_cast<size_t>(sourceUtf8.size())));
    JS_SetPropertyStr(m_ctx, data, "action", JS_NewStringLen(m_ctx, "musicUrl", 8));
    JSValue info = JS_NewObject(m_ctx);
    JS_SetPropertyStr(m_ctx, info, "musicInfo", variantToJs(m_ctx, musicInfo));
    JS_SetPropertyStr(m_ctx, info, "type",
        JS_NewStringLen(m_ctx, qualityUtf8.constData(), static_cast<size_t>(qualityUtf8.size())));
    JS_SetPropertyStr(m_ctx, data, "info", info);

    JSValueConst args[1] = { data };
    JSValue result = JS_Call(m_ctx, m_handler, JS_UNDEFINED, 1, args);

    QString out;
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(m_ctx);
        size_t l = 0;
        const char *msg = JS_ToCStringLen(m_ctx, &l, exc);
        const QString msgS = msg ? QString::fromUtf8(msg, static_cast<int>(l))
                                 : QStringLiteral("调用失败");
        if (msg) JS_FreeCString(m_ctx, msg);
        JS_FreeValue(m_ctx, exc);
        if (error) *error = msgS;
    } else if (!JS_IsObject(result)) {
        // 非 Promise：直接取结果
        out = jsToVariant(m_ctx, result).toString();
        JS_FreeValue(m_ctx, result);
    } else {
        // Promise 路径：挂 .then，settle 后写入 m_lastResult / m_done
        // fail 时用 'ERR:' 前缀透传错误信息
        static const char *waitCode =
            "(function(p, cb){"
            "  function ok(v){"
            "    if (typeof v === 'string') cb(v);"
            "    else if (v && typeof v === 'object' && typeof v.url === 'string') cb(v.url);"
            "    else cb(v == null ? '' : String(v));"
            "  }"
            "  function fail(e){ cb('ERR:' + (e && e.message ? e.message : String(e))); }"
            "  p.then(ok, fail);"
            "})";
        JSValue fn = JS_Eval(m_ctx, waitCode, strlen(waitCode), "<wait>", JS_EVAL_TYPE_GLOBAL);
        JSValue cb = JS_NewCFunction(m_ctx, [](JSContext *c, JSValueConst, int argc,
                                               JSValueConst *argv) -> JSValue {
            LxScriptEngine *self = selfOf(c);
            if (!self || argc < 1) return JS_UNDEFINED;
            size_t l = 0; const char *s = JS_ToCStringLen(c, &l, argv[0]);
            self->m_lastResult = QString::fromUtf8(s, static_cast<int>(l));
            if (s) JS_FreeCString(c, s);
            self->m_done = true;
            return JS_UNDEFINED;
        }, "__lx_waitcb", 1);

        m_lastResult.clear();
        JSValueConst wargs[2] = { result, cb };
        JSValue wr = JS_Call(m_ctx, fn, JS_UNDEFINED, 2, wargs);
        JS_FreeValue(m_ctx, wr);
        JS_FreeValue(m_ctx, fn);
        JS_FreeValue(m_ctx, cb);
        JS_FreeValue(m_ctx, result);

        // 驱动微任务直到 settle（默认 20s 超时）
        if (!pumpUntilDone(20000)) {
            if (error) *error = QStringLiteral("脚本执行超时（Promise 未 settle）");
        } else if (m_lastResult.startsWith(QStringLiteral("ERR:"))) {
            if (error) *error = m_lastResult.mid(4);
        } else {
            out = m_lastResult;
        }
    }

    // data 已通过 JS_Call 参数传递，其所有权由 QuickJS 的调用栈管理；
    // 这里释放本地引用（不释放其内部属性，属性由 data 对象统一管理）。
    JS_FreeValue(m_ctx, data);

    if (out.isEmpty() && error && error->isEmpty())
        *error = QStringLiteral("脚本未返回播放链接");
    return out;
}

} // namespace Muyun
