#include "LxScriptEngine.h"

#include "core/utils/Crypto.h"
#include "core/network/HttpClient.h"

#include <QFile>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QCoreApplication>
#include <QThread>
#include <QDateTime>
#include <QRandomGenerator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QDebug>

#include <zlib.h>

#include <vector>

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
    // ⚠ 数字必须**逐个元类型**接：Qt 6 的 QJsonValue::toVariant() 对整数给的是
    //   LongLong/Int，只写 `case Double` 会掉进 default 的 toString()，
    //   于是 JSON 里的 `{"code":200}` 到脚本里变成字符串 `"200"` →
    //   脚本 `r.body.code !== 200` 直接判失败（真实脚本 2e0bafbe 就这么"初始化失败"的，
    //   musicInfo.duration 等同理，是个影响所有音源的坑）。
    case QMetaType::Char:
    case QMetaType::Short:
    case QMetaType::Int:
    case QMetaType::Long:
    case QMetaType::LongLong:
    case QMetaType::UChar:
    case QMetaType::UShort:
    case QMetaType::UInt:
    case QMetaType::ULong:
    case QMetaType::ULongLong:
        return JS_NewFloat64(ctx, var.toDouble());
    case QMetaType::Float:
    case QMetaType::Double:
        return JS_NewFloat64(ctx, var.toDouble());
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

// ---------------------------------------------------------------------------
// 二进制（Buffer）桥接
//
// 官方两版的 Buffer 语义不一致：桌面版是真 Node Buffer（有 toString(enc)），
// 移动版是 Uint8Array（只能喂 utils.buffer.bufToString）。我们是 desktop 环境，
// 所以造一个**既是真 Uint8Array、又带 Node 风格 toString/toBase64** 的对象：
// 两代脚本的写法都能跑。字节本体以 base64 挂在 __b64 上（C++ 取回时解码，
// 绝不走 JS 字符串 → 二进制会被 UTF-8 编码弄坏）。
// ---------------------------------------------------------------------------

/// 每个 char code = 一个字节的 JS 字符串（二进制安全）
QByteArray latin1AsUtf8(const QByteArray &bytes)
{
    return QString::fromLatin1(bytes).toUtf8();
}

/// 二进制字节 → 文本（format: utf8/utf-8 | hex | base64 | binary/latin1）
QByteArray encodeBytes(const QByteArray &bytes, const QString &format)
{
    const QString f = format.trimmed().toLower();
    if (f == QLatin1String("hex")) return bytes.toHex();
    if (f == QLatin1String("base64")) return bytes.toBase64();
    if (f == QLatin1String("binary") || f == QLatin1String("latin1")
        || f == QLatin1String("latin-1")) return latin1AsUtf8(bytes);
    return bytes;   // utf8 / 其它：QByteArray 就是 UTF-8 字节
}

/// JS 字符串参数 → QString
QString jsStr(JSContext *ctx, JSValueConst v)
{
    size_t l = 0;
    const char *s = JS_ToCStringLen(ctx, &l, v);
    const QString out = s ? QString::fromUtf8(s, static_cast<int>(l)) : QString();
    if (s) JS_FreeCString(ctx, s);
    return out;
}

/// buffer.toString(enc) 的底层实现：argv[0]=base64 字节，argv[1]=编码
JSValue jsBufBytesToString(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_NewString(ctx, "");
    const QByteArray bytes = QByteArray::fromBase64(jsStr(ctx, argv[0]).toLatin1());
    const QString fmt = (argc > 1) ? jsStr(ctx, argv[1]) : QStringLiteral("utf8");
    const QByteArray out = encodeBytes(bytes, fmt);
    return JS_NewStringLen(ctx, out.constData(), static_cast<size_t>(out.size()));
}

/// 造 Buffer 对象（Uint8Array + Node 风格方法）
const char *kBufFactory =
    "(function (raw, b64, toStr) {"
    "  var n = raw.length;"
    "  var a = new Uint8Array(n);"
    "  for (var i = 0; i < n; i++) a[i] = raw.charCodeAt(i) & 0xff;"
    "  a.__b64 = b64;"
    "  a.__isLxBuf = true;"
    "  a.toString = function (enc) { return toStr(b64, enc == null ? 'utf8' : String(enc)); };"
    "  a.toBase64 = function () { return b64; };"
    "  return a;"
    "})";

JSValue makeBuffer(JSContext *ctx, const QByteArray &bytes)
{
    JSValue factory = JS_Eval(ctx, kBufFactory, strlen(kBufFactory), "<lx-buf>",
                              JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(factory)) {
        JSValue exc = JS_GetException(ctx);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, factory);
        return JS_UNDEFINED;
    }
    const QByteArray raw = latin1AsUtf8(bytes);
    JSValue rawVal = JS_NewStringLen(ctx, raw.constData(), static_cast<size_t>(raw.size()));
    const QByteArray b64 = bytes.toBase64();
    JSValue b64Val = JS_NewStringLen(ctx, b64.constData(), static_cast<size_t>(b64.size()));
    JSValue toStr = JS_NewCFunction(ctx, jsBufBytesToString, "__lx_bufstr", 2);
    JSValueConst args[3] = { rawVal, b64Val, toStr };
    JSValue buf = JS_Call(ctx, factory, JS_UNDEFINED, 3, args);
    if (JS_IsException(buf)) {
        JSValue exc = JS_GetException(ctx);
        JS_FreeValue(ctx, exc);
        buf = JS_UNDEFINED;
    }
    JS_FreeValue(ctx, factory);
    JS_FreeValue(ctx, rawVal);
    JS_FreeValue(ctx, b64Val);
    JS_FreeValue(ctx, toStr);
    return buf;
}

/// JS 值 → 原始字节：字符串按 UTF-8 字节；Buffer/类数组按字节逐个取
/// （官方 utils_str2b64 / Uint8Array 都是这个语义）
QByteArray bytesFromJs(JSContext *ctx, JSValueConst v)
{
    if (JS_IsString(v)) {
        size_t l = 0;
        const char *s = JS_ToCStringLen(ctx, &l, v);
        QByteArray out(s ? s : "", s ? static_cast<int>(l) : 0);
        if (s) JS_FreeCString(ctx, s);
        return out;
    }
    if (!JS_IsObject(v)) return QByteArray();

    JSValue b64 = JS_GetPropertyStr(ctx, v, "__b64");
    if (JS_IsString(b64)) {
        const QByteArray out = QByteArray::fromBase64(jsStr(ctx, b64).toLatin1());
        JS_FreeValue(ctx, b64);
        return out;
    }
    JS_FreeValue(ctx, b64);

    JSValue lenV = JS_GetPropertyStr(ctx, v, "length");
    int64_t n = 0;
    const bool okLen = (JS_ToInt64(ctx, &n, lenV) == 0);
    JS_FreeValue(ctx, lenV);
    if (!okLen || n <= 0 || n > (8 << 20)) return QByteArray();

    QByteArray out(static_cast<int>(n), '\0');
    for (int64_t i = 0; i < n; ++i) {
        JSValue item = JS_GetPropertyUint32(ctx, v, static_cast<uint32_t>(i));
        int32_t b = 0;
        JS_ToInt32(ctx, &b, item);
        JS_FreeValue(ctx, item);
        out[static_cast<int>(i)] = static_cast<char>(b & 0xff);
    }
    return out;
}

/// buffer.from(input, encoding)：字符串按 encoding 解释，数组/Buffer 直接取字节
QByteArray bytesFromBufferInput(JSContext *ctx, JSValueConst v, const QString &enc)
{
    if (JS_IsString(v)) {
        size_t l = 0;
        const char *s = JS_ToCStringLen(ctx, &l, v);
        const QByteArray raw(s ? s : "", s ? static_cast<int>(l) : 0);
        if (s) JS_FreeCString(ctx, s);
        const QString e = enc.trimmed().toLower();
        if (e == QLatin1String("base64")) return QByteArray::fromBase64(raw);
        if (e == QLatin1String("hex")) return QByteArray::fromHex(raw);
        // binary/latin1：JS 串里每个 code unit 就是一个字节
        if (e == QLatin1String("binary") || e == QLatin1String("latin1")
            || e == QLatin1String("latin-1"))
            return QString::fromUtf8(raw).toLatin1();
        return raw;   // utf8
    }
    return bytesFromJs(ctx, v);
}

/// 已 resolve 的 Promise
JSValue resolvedPromise(JSContext *ctx, JSValueConst value)
{
    JSValue funcs[2];
    JSValue p = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(p)) { JS_FreeValue(ctx, p); return JS_UNDEFINED; }
    JSValue r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, &value);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return p;
}

JSValue resolvedPromiseString(JSContext *ctx, const QString &s)
{
    const QByteArray b = s.toUtf8();
    JSValue v = JS_NewStringLen(ctx, b.constData(), static_cast<size_t>(b.size()));
    JSValue p = resolvedPromise(ctx, v);
    JS_FreeValue(ctx, v);
    return p;
}

/// 已 reject 的 Promise（用 Error 对象，脚本 catch 到的 e.message 与官方一致）
JSValue rejectedPromise(JSContext *ctx, const QString &msg)
{
    JSValue funcs[2];
    JSValue p = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(p)) { JS_FreeValue(ctx, p); return JS_UNDEFINED; }
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, msg.toUtf8().constData()));
    JSValue r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, &err);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, err);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return p;
}

// ---------------------------------------------------------------------------
// zlib（协议：inflate/deflate 都是 zlib 格式，与 Node zlib 默认一致）
// ---------------------------------------------------------------------------

bool zlibInflateBytes(const QByteArray &in, QByteArray *out, QString *err)
{
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) {                       // windowBits=15 → zlib 头
        if (err) *err = QStringLiteral("zlib inflate 初始化失败");
        return false;
    }
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.constData()));
    zs.avail_in = uInt(in.size());
    out->clear();
    QByteArray buf(64 * 1024, '\0');
    int ret = Z_OK;
    while (ret != Z_STREAM_END) {
        // 官方坑：每轮进 inflate 前必须重摆 next_out/avail_out
        zs.next_out = reinterpret_cast<Bytef *>(buf.data());
        zs.avail_out = uInt(buf.size());
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
            inflateEnd(&zs);
            if (err) *err = QStringLiteral("zlib inflate 失败(%1)").arg(ret);
            return false;
        }
        out->append(buf.constData(), int(buf.size()) - int(zs.avail_out));
        if (ret == Z_BUF_ERROR && zs.avail_out != 0) {     // 输入耗尽但流没结束
            inflateEnd(&zs);
            if (err) *err = QStringLiteral("zlib inflate 数据不完整");
            return false;
        }
        if (out->size() > (64 << 20)) {
            inflateEnd(&zs);
            if (err) *err = QStringLiteral("zlib inflate 结果过大");
            return false;
        }
    }
    inflateEnd(&zs);
    return true;
}

bool zlibDeflateBytes(const QByteArray &in, QByteArray *out, QString *err)
{
    z_stream zs{};
    if (deflateInit(&zs, Z_DEFAULT_COMPRESSION) != Z_OK) {
        if (err) *err = QStringLiteral("zlib deflate 初始化失败");
        return false;
    }
    QByteArray buf(static_cast<int>(deflateBound(&zs, uLong(in.size()))) + 64, '\0');
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.constData()));
    zs.avail_in = uInt(in.size());
    zs.next_out = reinterpret_cast<Bytef *>(buf.data());
    zs.avail_out = uInt(buf.size());
    const int ret = deflate(&zs, Z_FINISH);
    const int written = int(buf.size()) - int(zs.avail_out);
    deflateEnd(&zs);
    if (ret != Z_STREAM_END) {
        if (err) *err = QStringLiteral("zlib deflate 失败(%1)").arg(ret);
        return false;
    }
    buf.resize(written);
    *out = buf;
    return true;
}

// ---------------------------------------------------------------------------
// 协议字段过滤（与官方 user-api-preload.js 的 handleInit 一致）
// ---------------------------------------------------------------------------

const QStringList &protocolSources()
{
    static const QStringList kAll = { QStringLiteral("kw"), QStringLiteral("kg"),
                                      QStringLiteral("tx"), QStringLiteral("wy"),
                                      QStringLiteral("mg"), QStringLiteral("local") };
    return kAll;
}

const QStringList &protocolQualitys()
{
    static const QStringList kAll = { QStringLiteral("128k"), QStringLiteral("320k"),
                                      QStringLiteral("flac"), QStringLiteral("flac24bit") };
    return kAll;
}

QStringList protocolActions(const QString &source)
{
    if (source == QLatin1String("local"))
        return { QStringLiteral("musicUrl"), QStringLiteral("lyric"), QStringLiteral("pic") };
    return { QStringLiteral("musicUrl") };
}

/// 把脚本 send('inited') 里的 sources 按协议收敛：只认 kw/kg/tx/wy/mg/local、
/// type 必须是 music、actions/qualitys 只保留协议允许的值（与官方一致）
QVariantMap filterSourceInfo(const QVariantMap &raw)
{
    QVariantMap out;
    for (const QString &src : protocolSources()) {
        const QVariantMap user = raw.value(src).toMap();
        if (user.isEmpty()) continue;
        if (user.value(QStringLiteral("type")).toString() != QLatin1String("music")) continue;

        QStringList declaredActions;
        for (const QVariant &a : user.value(QStringLiteral("actions")).toList())
            declaredActions.append(a.toString());
        QStringList actions;
        for (const QString &a : protocolActions(src))
            if (declaredActions.contains(a)) actions.append(a);

        QStringList declaredQualitys;
        for (const QVariant &q : user.value(QStringLiteral("qualitys")).toList())
            declaredQualitys.append(q.toString());
        QStringList qualitys;
        for (const QString &q : protocolQualitys())
            if (declaredQualitys.contains(q)) qualitys.append(q);

        QVariantMap one;
        one[QStringLiteral("type")] = QStringLiteral("music");
        one[QStringLiteral("actions")] = actions;
        one[QStringLiteral("qualitys")] = qualitys;
        const QString name = user.value(QStringLiteral("name")).toString();
        if (!name.isEmpty()) one[QStringLiteral("name")] = name;
        out[src] = one;
    }
    return out;
}

// ---------------------------------------------------------------------------
// 脚本头部注释（@name/@description/@version/@author/@homepage）
// ---------------------------------------------------------------------------

QString stripCommentPrefix(QString line)
{
    line = line.trimmed();
    while (!line.isEmpty()
           && (line.startsWith(QLatin1Char('*')) || line.startsWith(QLatin1Char('/'))
               || line.startsWith(QLatin1Char('\t')))) {
        if (line.startsWith(QLatin1Char('*')) || line.startsWith(QLatin1Char('/')))
            line = line.mid(1).trimmed();
        else
            line = line.mid(1);
    }
    return line;
}

/// currentScriptInfo 工厂（避免手拼 JSON 字符串被引号/换行咬到）
const char *kScriptInfoFactory =
    "(function (i) { return {"
    "  name: i.name || '', description: i.description || '',"
    "  version: i.version || '', author: i.author || '',"
    "  homepage: i.homepage || '', rawScript: i.rawScript || ''"
    "}; })";

void applyCurrentScriptInfo(JSContext *ctx, const QVariantMap &meta)
{
    JSValue factory = JS_Eval(ctx, kScriptInfoFactory, strlen(kScriptInfoFactory),
                              "<lx-info>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(factory)) {
        JSValue exc = JS_GetException(ctx);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, factory);
        return;
    }
    JSValue arg = variantToJs(ctx, meta);
    JSValue info = JS_Call(ctx, factory, JS_UNDEFINED, 1, &arg);
    if (JS_IsException(info)) {
        JSValue exc = JS_GetException(ctx);
        JS_FreeValue(ctx, exc);
        info = JS_UNDEFINED;
    }
    JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, factory);

    JSValue g = JS_GetGlobalObject(ctx);
    JSValue lx = JS_GetPropertyStr(ctx, g, "lx");
    if (JS_IsObject(lx) && !JS_IsUndefined(info))
        JS_SetPropertyStr(ctx, lx, "currentScriptInfo",
                          JS_DupValue(ctx, info));   // SetPropertyStr 接管所有权
    JS_FreeValue(ctx, info);
    JS_FreeValue(ctx, lx);
    JS_FreeValue(ctx, g);
}

} // namespace

// ===========================================================================
// 构造 / 析构
// ===========================================================================

LxScriptEngine::LxScriptEngine()
{
    m_rt = JS_NewRuntime();
    JS_SetMaxStackSize(m_rt, 4 * 1024 * 1024);
    JS_SetHostPromiseRejectionTracker(m_rt, onPromiseRejection, nullptr);
    m_ctx = JS_NewContext(m_rt);
    JS_SetContextOpaque(m_ctx, this);
    registerApi();
}

LxScriptEngine::~LxScriptEngine()
{
    clearTimers();
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
    JS_SetPropertyStr(m_ctx, lx, "request", JS_NewCFunction(m_ctx, jsRequest, "request", 3));

    // utils.crypto —— 名字与签名严格按官方协议（aesEncrypt(buffer, mode, key, iv)）；
    // 旧的 aesEn/aesDe 作为别名保留（我们自己早期版本用过，脚本可能照抄过）
    JSValue utils  = JS_NewObject(m_ctx);
    JSValue crypto = JS_NewObject(m_ctx);
    JS_SetPropertyStr(m_ctx, crypto, "md5",         JS_NewCFunction(m_ctx, jsMd5,         "md5",         1));
    JS_SetPropertyStr(m_ctx, crypto, "aesEncrypt",  JS_NewCFunction(m_ctx, jsAesEncrypt,  "aesEncrypt",  4));
    JS_SetPropertyStr(m_ctx, crypto, "aesDecrypt",  JS_NewCFunction(m_ctx, jsAesDecrypt,  "aesDecrypt",  4));
    JS_SetPropertyStr(m_ctx, crypto, "aesEn",       JS_NewCFunction(m_ctx, jsAesEncrypt,  "aesEn",       4));
    JS_SetPropertyStr(m_ctx, crypto, "aesDe",       JS_NewCFunction(m_ctx, jsAesDecrypt,  "aesDe",       4));
    JS_SetPropertyStr(m_ctx, crypto, "randomBytes", JS_NewCFunction(m_ctx, jsRandomBytes, "randomBytes", 1));
    JS_SetPropertyStr(m_ctx, crypto, "rsaEncrypt",  JS_NewCFunction(m_ctx, jsRsaEncrypt,  "rsaEncrypt",  2));
    JS_SetPropertyStr(m_ctx, crypto, "base64Encode",JS_NewCFunction(m_ctx, jsB64Enc,      "base64Encode", 1));
    JS_SetPropertyStr(m_ctx, crypto, "base64Decode",JS_NewCFunction(m_ctx, jsB64Dec,      "base64Decode", 1));
    JS_SetPropertyStr(m_ctx, utils, "crypto", crypto);

    // utils.buffer：base64/hex/utf8 转换（协议里脚本靠它拿到可读文本）
    JSValue buffer = JS_NewObject(m_ctx);
    JS_SetPropertyStr(m_ctx, buffer, "from",       JS_NewCFunction(m_ctx, jsBufFrom,       "from",       2));
    JS_SetPropertyStr(m_ctx, buffer, "bufToString",JS_NewCFunction(m_ctx, jsBufToString,   "bufToString", 2));
    JS_SetPropertyStr(m_ctx, buffer, "toBase64",   JS_NewCFunction(m_ctx, jsB64Enc,        "toBase64",    1));
    JS_SetPropertyStr(m_ctx, buffer, "fromBase64", JS_NewCFunction(m_ctx, jsB64Dec,        "fromBase64",  1));
    JS_SetPropertyStr(m_ctx, utils, "buffer", buffer);

    // utils.zlib：桌面版有（移动版没有），返回 Promise<Buffer>，与 Node zlib 同一格式
    JSValue zlib = JS_NewObject(m_ctx);
    JS_SetPropertyStr(m_ctx, zlib, "inflate", JS_NewCFunction(m_ctx, jsZlibInflate, "inflate", 1));
    JS_SetPropertyStr(m_ctx, zlib, "deflate", JS_NewCFunction(m_ctx, jsZlibDeflate, "deflate", 1));
    JS_SetPropertyStr(m_ctx, utils, "zlib", zlib);

    JS_SetPropertyStr(m_ctx, lx, "utils", utils);

    // 脚本调试输出通道（console.* 的底层实现）
    JS_SetPropertyStr(m_ctx, global, "__lx_print",
        JS_NewCFunction(m_ctx, jsPrint, "__lx_print", 1));

    // 宿主定时器：回调本体存在全局 __lx_timers（GC 根），C++ 只记到期时刻
    JS_SetPropertyStr(m_ctx, global, "__lx_timers", JS_NewObject(m_ctx));
    JS_SetPropertyStr(m_ctx, global, "setTimeout",
        JS_NewCFunction(m_ctx, jsSetTimeout, "setTimeout", 2));
    JS_SetPropertyStr(m_ctx, global, "clearTimeout",
        JS_NewCFunction(m_ctx, jsClearTimeout, "clearTimeout", 1));

    // env / version / script info
    JS_SetPropertyStr(m_ctx, lx, "env",     JS_NewString(m_ctx, "desktop"));
    JS_SetPropertyStr(m_ctx, lx, "version", JS_NewString(m_ctx, "2.0.0"));
    JS_SetPropertyStr(m_ctx, lx, "currentScriptInfo", JS_NewObject(m_ctx));

    JS_SetPropertyStr(m_ctx, global, "lx", lx);

    // -----------------------------------------------------------------------
    // 全局辅助：Buffer / atob / btoa / console（依赖上面的绑定）
    // -----------------------------------------------------------------------
    static const char *shim =
        "globalThis.atob = function(s){ return globalThis.lx.utils.crypto.base64Decode(s); };"
        "globalThis.btoa = function(s){ return globalThis.lx.utils.crypto.base64Encode(s); };"
        // Node 风格 Buffer（桌面版脚本大量直接用全局 Buffer）
        "globalThis.Buffer = {"
        "  from: function(data, enc){ return globalThis.lx.utils.buffer.from(data, enc); },"
        "  alloc: function(n, fill){"
        "    var a = []; for (var i = 0; i < n; i++) a.push(fill === undefined ? 0 : fill);"
        "    return globalThis.lx.utils.buffer.from(a);"
        "  },"
        "  isBuffer: function(b){"
        "    return !!(b && (b.__isLxBuf === true"
        "      || (typeof ArrayBuffer !== 'undefined' && ArrayBuffer.isView && ArrayBuffer.isView(b))));"
        "  },"
        "  concat: function(list){"
        "    var out = [];"
        "    for (var i = 0; i < list.length; i++) {"
        "      var b = list[i];"
        "      for (var j = 0; j < b.length; j++) out.push(b[j] & 0xff);"
        "    }"
        "    return globalThis.lx.utils.buffer.from(out);"
        "  },"
        "  fromBase64: function(b64){ return globalThis.lx.utils.buffer.from(b64, 'base64'); }"
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
    if (argc < 2 || !self) return JS_ThrowTypeError(ctx, "lx.on(eventName, handler)");
    const QString ev = jsStr(ctx, argv[0]);

    if (ev != QStringLiteral("request"))
        return rejectedPromise(ctx, QStringLiteral("The event is not supported: ") + ev);

    if (!JS_IsFunction(ctx, argv[1]))
        return rejectedPromise(ctx, QStringLiteral("handler required a function"));

    JS_FreeValue(self->m_ctx, self->m_handler);
    self->m_handler = JS_DupValue(self->m_ctx, argv[1]);
    // 关键：把 handler 挂到全局对象做 GC 根。
    // QuickJS 的保守 GC 扫不到 C++ 类成员变量，若仅存裸 JSValue，
    // 脚本执行完 GC 会回收该函数对象，导致后续 JS_Call 崩溃。
    JSValue g = JS_GetGlobalObject(self->m_ctx);
    JS_SetPropertyStr(self->m_ctx, g, "__lx_handler", JS_DupValue(self->m_ctx, argv[1]));
    JS_FreeValue(self->m_ctx, g);
    return resolvedPromiseString(ctx, QStringLiteral("ok"));
}

JSValue LxScriptEngine::jsSend(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    LxScriptEngine *self = selfOf(ctx);
    if (argc < 2 || !self) return JS_ThrowTypeError(ctx, "lx.send(eventName, data)");
    const QString ev = jsStr(ctx, argv[0]);

    if (ev == QStringLiteral("inited")) {
        if (self->m_inited)
            return rejectedPromise(ctx, QStringLiteral("Script is inited"));
        QVariantMap info = jsObjToVariantSafe(self->m_ctx, argv[1]);
        const QVariant status = info.value(QStringLiteral("status"));
        if (status.isValid() && !status.toBool())
            return rejectedPromise(ctx, QStringLiteral("Missing required parameter init info"));
        // 按协议收敛 sources（与官方 handleInit 一致；脚本一个有效源都没声明时不报错，只是空表）
        info[QStringLiteral("sources")] =
            filterSourceInfo(info.value(QStringLiteral("sources")).toMap());
        self->m_scriptInfo = info;
        self->m_inited = true;
        return resolvedPromiseString(ctx, QStringLiteral("ok"));
    }
    if (ev == QStringLiteral("updateAlert")) {
        // 与官方一致：每次运行脚本只能调一次
        if (self->m_updateAlertSent)
            return rejectedPromise(ctx, QStringLiteral("The update alert can only be called once."));
        self->m_updateAlertSent = true;
        self->m_updateInfo = jsObjToVariantSafe(self->m_ctx, argv[1]);
        if (self->m_updateAlertCallback)
            self->m_updateAlertCallback(self->m_updateInfo);
        return resolvedPromiseString(ctx, QStringLiteral("ok"));
    }
    return rejectedPromise(ctx, QStringLiteral("The event is not supported: ") + ev);
}

JSValue LxScriptEngine::jsRequest(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1 || !selfOf(ctx)) return JS_ThrowTypeError(ctx, "request(url, options, cb?)");

    const QString url = jsStr(ctx, argv[0]);

    // options（协议字段：method / headers / body / form / formData / timeout / binary）
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
    // 与官方一致：timeout 有效才用，上限 60s（官方 Math.min(timeout, 60_000)）
    int timeout = options.value(QStringLiteral("timeout"), 30000).toInt();
    if (timeout <= 0) timeout = 30000;
    opt.timeoutMs = qMin(timeout, 60000);

    const bool isPost = (method == QStringLiteral("POST") || method == QStringLiteral("PUT")
                         || method == QStringLiteral("PATCH"));
    QByteArray postBody = body;
    if (isPost && postBody.isEmpty() && !form.isEmpty())
        postBody = serializeForm(form);

    // 在调用线程用局部 QNAM 同步执行（自带阻塞事件循环，跨线程安全）
    QNetworkAccessManager nam;
    const HttpResponse resp =
        HttpClient::instance()->execRequest(&nam, url, postBody, isPost, opt);

    // 响应对象：{ statusCode, statusMessage, headers, body }
    //
    // body 的形态必须照官方来（桌面版走 axios）：
    //   · `binary: true`      → Buffer（真实脚本里有 `isBuffer(body)` 分支）
    //   · Content-Type 是 JSON → **已解析的对象**（真实脚本里 `resp.body.code` 这种写法很常见，
    //                             我们以前一律给字符串，这类脚本就直接初始化失败）
    //   · 其余                → 字符串（脚本自己 JSON.parse）
    // 两种写法在真实脚本里都有人用，所以只能按 Content-Type 分，不能一刀切。
    JSValue bodyVal = JS_UNDEFINED;
    if (options.value(QStringLiteral("binary")).toBool()) {
        bodyVal = makeBuffer(ctx, resp.body);
    } else {
        const QString ct = resp.headers.value(QStringLiteral("content-type")).toString().toLower();
        if (ct.contains(QLatin1String("json"))) {
            QJsonParseError pe{};
            const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &pe);
            if (pe.error == QJsonParseError::NoError && !doc.isNull())
                bodyVal = variantToJs(ctx, doc.toVariant());
        }
        if (JS_IsUndefined(bodyVal))
            bodyVal = JS_NewStringLen(ctx, resp.body.constData(),
                                      static_cast<size_t>(resp.body.size()));
    }

    JSValue respObj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, respObj, "statusCode", JS_NewInt32(ctx, resp.status));
    JS_SetPropertyStr(ctx, respObj, "statusMessage", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, respObj, "body", JS_DupValue(ctx, bodyVal));   // resp.body 与第三参同源
    JSValue hdrs = JS_NewObject(ctx);
    for (auto it = resp.headers.constBegin(); it != resp.headers.constEnd(); ++it) {
        const QByteArray k = it.key().toUtf8();
        const QByteArray v = it.value().toString().toUtf8();
        JS_SetPropertyStr(ctx, hdrs, k.constData(),
            JS_NewStringLen(ctx, v.constData(), static_cast<size_t>(v.size())));
    }
    JS_SetPropertyStr(ctx, respObj, "headers", hdrs);

    // 回调式：request(url, options, cb(err, resp, body))，返回取消函数
    if (argc > 2 && JS_IsFunction(ctx, argv[2])) {
        JSValue errVal = resp.ok ? JS_NULL
                                 : JS_NewString(ctx, resp.error.toUtf8().constData());
        JSValueConst cbArgs[3] = { errVal, respObj, bodyVal };
        JSValue ret = JS_Call(ctx, argv[2], JS_UNDEFINED, 3, cbArgs);
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, errVal);
        JS_FreeValue(ctx, respObj);
        JS_FreeValue(ctx, bodyVal);
        // 本实现是同步请求（返回前已完成），取消函数按协议给一个空操作，
        // 脚本 `const cancel = request(...)` / `typeof cancel === 'function'` 都不会踩空
        return JS_NewCFunction(ctx, [](JSContext *, JSValueConst, int, JSValueConst *) -> JSValue {
            return JS_UNDEFINED;
        }, "cancel", 0);
    }

    // Promise 式（非官方扩展）：返回已 resolve 的 Promise
    JSValue promise = resolvedPromise(ctx, respObj);
    JS_FreeValue(ctx, respObj);
    JS_FreeValue(ctx, bodyVal);
    return promise;
}

JSValue LxScriptEngine::jsMd5(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "md5(str)");
    // 桌面版语义：对字符串的 UTF-8 字节求 MD5（与 Node createHash('md5').update(str) 一致）
    size_t len = 0;
    const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
    const QString hex = Crypto::md5Hex(QByteArray(s, static_cast<int>(len)));
    JS_FreeCString(ctx, s);
    const QByteArray b = hex.toUtf8();
    return JS_NewStringLen(ctx, b.constData(), static_cast<size_t>(b.size()));
}

JSValue LxScriptEngine::jsAesEncrypt(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    // 官方签名：aesEncrypt(buffer, mode, key, iv)
    if (argc < 3) return JS_ThrowTypeError(ctx, "aesEncrypt(buffer, mode, key, iv)");
    const QString mode = jsStr(ctx, argv[1]).trimmed().toLower();
    const QByteArray data = bytesFromJs(ctx, argv[0]);
    const QByteArray key  = bytesFromJs(ctx, argv[2]);
    const QByteArray iv   = (argc > 3) ? bytesFromJs(ctx, argv[3]) : QByteArray();

    QByteArray out;
    if (mode.contains(QLatin1String("ecb"))) {
        // 协议里 aes-128-ecb 是 NoPadding（官方 AES_MODE.ECB_128_NoPadding）
        if ((data.size() % 16) != 0)
            return JS_ThrowTypeError(ctx, "aes-128-ecb 输入必须是 16 字节整数倍（NoPadding）");
        out = Crypto::aes128EcbEncryptNoPad(data, key);
    } else if (mode.contains(QLatin1String("cbc"))) {
        out = Crypto::aes128CbcEncrypt(data, key, iv);   // PKCS7
    } else {
        return JS_ThrowTypeError(ctx, "unsupported aes mode: %s", mode.toUtf8().constData());
    }
    if (out.isEmpty() && !data.isEmpty())
        return JS_ThrowTypeError(ctx, "aes encrypt failed (key 至少 16 字节)");
    return makeBuffer(ctx, out);
}

JSValue LxScriptEngine::jsAesDecrypt(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    // 非官方扩展（我们自己的别名 aesDe 用它）：与 aesEncrypt 对称
    if (argc < 3) return JS_ThrowTypeError(ctx, "aesDecrypt(buffer, mode, key, iv)");
    const QString mode = jsStr(ctx, argv[1]).trimmed().toLower();
    const QByteArray data = bytesFromJs(ctx, argv[0]);
    const QByteArray key  = bytesFromJs(ctx, argv[2]);
    const QByteArray iv   = (argc > 3) ? bytesFromJs(ctx, argv[3]) : QByteArray();

    QByteArray out;
    if (mode.contains(QLatin1String("ecb")))
        out = Crypto::aes128EcbDecryptNoPad(data, key);
    else
        out = Crypto::aes128CbcDecrypt(data, key, iv);
    return makeBuffer(ctx, out);
}

JSValue LxScriptEngine::jsRandomBytes(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "randomBytes(size)");
    int32_t n = 0;
    JS_ToInt32(ctx, &n, argv[0]);
    if (n < 0) n = 0;
    if (n > (1 << 20)) n = 1 << 20;
    QByteArray bytes(static_cast<int>(n), '\0');
    auto *gen = QRandomGenerator::system();
    for (int i = 0; i + 4 <= n; i += 4) {
        const quint32 v = gen->generate();
        bytes[i + 0] = char(v & 0xff);
        bytes[i + 1] = char((v >> 8) & 0xff);
        bytes[i + 2] = char((v >> 16) & 0xff);
        bytes[i + 3] = char((v >> 24) & 0xff);
    }
    for (int i = (n / 4) * 4; i < n; ++i)
        bytes[i] = char(gen->bounded(256));
    return makeBuffer(ctx, bytes);
}

JSValue LxScriptEngine::jsRsaEncrypt(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    // 官方签名 rsaEncrypt(buffer, key)：返回二进制（脚本再 bufToString(...,'base64')）
    if (argc < 2) return JS_ThrowTypeError(ctx, "rsaEncrypt(buffer, key)");
    const QByteArray data = bytesFromJs(ctx, argv[0]);
    QString key = jsStr(ctx, argv[1]);
    if (key.trimmed().isEmpty()) return JS_ThrowTypeError(ctx, "Invalid RSA key");

    // 官方会先把 PEM 标记行去掉（公私钥两种都认）
    key.remove(QStringLiteral("-----BEGIN PUBLIC KEY-----"));
    key.remove(QStringLiteral("-----END PUBLIC KEY-----"));
    key.remove(QStringLiteral("-----BEGIN PRIVATE KEY-----"));
    key.remove(QStringLiteral("-----END PRIVATE KEY-----"));
    key = key.simplified();

    const QString hex = Crypto::rsaNoPaddingEncryptHex(data, key);
    if (hex.isEmpty()) return JS_ThrowTypeError(ctx, "rsa encrypt failed");
    return makeBuffer(ctx, QByteArray::fromHex(hex.toLatin1()));
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

/// 未处理的 Promise 拒绝：官方宿主（Electron / RN）都会把它打出来，QuickJS 不设 tracker
/// 就是**静默吞掉**——脚本在 .then/.catch 里抛错时宿主只看到"初始化超时"（四-61 真踩过）。
void LxScriptEngine::onPromiseRejection(JSContext *ctx, JSValueConst, JSValueConst reason,
                                        int is_handled, void *)
{
    if (is_handled || !ctx) return;
    size_t l = 0;
    const char *msg = JS_ToCStringLen(ctx, &l, reason);
    const QString text = msg ? QString::fromUtf8(msg, static_cast<int>(l)) : QStringLiteral("(未知)");
    if (msg) JS_FreeCString(ctx, msg);
    fprintf(stderr, "[lx-script] Uncaught (in promise) %s\n", text.toUtf8().constData());
    fflush(stderr);
    auto *self = static_cast<LxScriptEngine *>(JS_GetContextOpaque(ctx));
    // 只在"还没初始化成功"时留痕：脚本自己的报错比"初始化超时"有用得多
    if (self && !self->m_inited && self->m_initError.isEmpty())
        self->m_initError = text;
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
    // buffer.from(str|array|buffer, encoding) -> Buffer（Uint8Array + Node 风格方法）
    if (argc < 1) return JS_ThrowTypeError(ctx, "buffer.from(input, encoding)");
    const QString enc = (argc > 1) ? jsStr(ctx, argv[1]) : QStringLiteral("utf8");
    return makeBuffer(ctx, bytesFromBufferInput(ctx, argv[0], enc));
}

JSValue LxScriptEngine::jsBufToString(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    // buffer.bufToString(buf, format) -> 字符串
    if (argc < 1) return JS_ThrowTypeError(ctx, "bufToString(buffer, format)");
    const QByteArray bytes = bytesFromBufferInput(ctx, argv[0], QString());
    const QString fmt = (argc > 1) ? jsStr(ctx, argv[1]) : QStringLiteral("utf8");
    const QByteArray out = encodeBytes(bytes, fmt);
    return JS_NewStringLen(ctx, out.constData(), static_cast<size_t>(out.size()));
}

JSValue LxScriptEngine::jsZlibInflate(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1) return rejectedPromise(ctx, QStringLiteral("inflate(buffer)"));
    QByteArray out; QString err;
    if (!zlibInflateBytes(bytesFromJs(ctx, argv[0]), &out, &err))
        return rejectedPromise(ctx, err);
    JSValue buf = makeBuffer(ctx, out);
    JSValue p = resolvedPromise(ctx, buf);
    JS_FreeValue(ctx, buf);
    return p;
}

JSValue LxScriptEngine::jsZlibDeflate(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1) return rejectedPromise(ctx, QStringLiteral("deflate(buffer)"));
    QByteArray out; QString err;
    if (!zlibDeflateBytes(bytesFromJs(ctx, argv[0]), &out, &err))
        return rejectedPromise(ctx, err);
    JSValue buf = makeBuffer(ctx, out);
    JSValue p = resolvedPromise(ctx, buf);
    JS_FreeValue(ctx, buf);
    return p;
}

JSValue LxScriptEngine::jsSetTimeout(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    LxScriptEngine *self = selfOf(ctx);
    if (!self || argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "setTimeout(callback, timeout)");

    double ms = 0;
    if (argc > 1) JS_ToFloat64(ctx, &ms, argv[1]);
    if (!(ms >= 0)) ms = 0;                    // NaN / 负数 → 立即
    if (ms > 3600000) ms = 3600000;

    const int id = self->m_nextTimerId++;

    JSValue args = JS_NewArray(ctx);
    for (int i = 2; i < argc; ++i)
        JS_SetPropertyUint32(ctx, args, static_cast<uint32_t>(i - 2), JS_DupValue(ctx, argv[i]));
    JSValue rec = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, rec, "fn", JS_DupValue(ctx, argv[0]));
    JS_SetPropertyStr(ctx, rec, "args", args);          // 接管 args 所有权

    JSValue g = JS_GetGlobalObject(ctx);
    JSValue timers = JS_GetPropertyStr(ctx, g, "__lx_timers");
    if (JS_IsObject(timers))
        JS_SetPropertyUint32(ctx, timers, static_cast<uint32_t>(id), rec);   // 接管 rec
    else
        JS_FreeValue(ctx, rec);
    JS_FreeValue(ctx, timers);
    JS_FreeValue(ctx, g);

    self->m_timers.append({ id, QDateTime::currentMSecsSinceEpoch() + static_cast<qint64>(ms) });
    return JS_NewInt32(ctx, id);
}

JSValue LxScriptEngine::jsClearTimeout(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    LxScriptEngine *self = selfOf(ctx);
    if (!self || argc < 1) return JS_UNDEFINED;
    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    for (int i = 0; i < self->m_timers.size(); ++i) {
        if (self->m_timers.at(i).id == id) { self->m_timers.removeAt(i); break; }
    }
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue timers = JS_GetPropertyStr(ctx, g, "__lx_timers");
    if (JS_IsObject(timers))
        JS_SetPropertyUint32(ctx, timers, static_cast<uint32_t>(id), JS_UNDEFINED);
    JS_FreeValue(ctx, timers);
    JS_FreeValue(ctx, g);
    return JS_UNDEFINED;
}

// ===========================================================================
// 微任务 / 定时器驱动
// ===========================================================================

void LxScriptEngine::clearTimers()
{
    m_timers.clear();
}

/// 触发所有到点的 setTimeout（回调里可能再排新的定时器，故先把到点的摘出来再执行）
void LxScriptEngine::runDueTimers()
{
    if (m_timers.isEmpty() || !m_ctx) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QVector<Timer> due;
    for (int i = m_timers.size() - 1; i >= 0; --i) {
        if (m_timers.at(i).dueMs <= now) {
            due.append(m_timers.at(i));
            m_timers.removeAt(i);
        }
    }
    if (due.isEmpty()) return;

    JSValue g = JS_GetGlobalObject(m_ctx);
    JSValue timers = JS_GetPropertyStr(m_ctx, g, "__lx_timers");
    for (const Timer &t : due) {
        JSValue rec = JS_IsObject(timers)
            ? JS_GetPropertyUint32(m_ctx, timers, static_cast<uint32_t>(t.id)) : JS_UNDEFINED;
        if (JS_IsObject(rec)) {
            JSValue fn = JS_GetPropertyStr(m_ctx, rec, "fn");
            JSValue argsV = JS_GetPropertyStr(m_ctx, rec, "args");
            std::vector<JSValue> args;
            if (JS_IsArray(m_ctx, argsV)) {
                JSValue lenV = JS_GetPropertyStr(m_ctx, argsV, "length");
                int32_t n = 0; JS_ToInt32(m_ctx, &n, lenV);
                JS_FreeValue(m_ctx, lenV);
                for (int32_t i = 0; i < n; ++i)
                    args.push_back(JS_GetPropertyUint32(m_ctx, argsV, static_cast<uint32_t>(i)));
            }
            if (JS_IsFunction(m_ctx, fn)) {
                JSValue r = JS_Call(m_ctx, fn, JS_UNDEFINED, static_cast<int>(args.size()),
                                    args.empty() ? nullptr : args.data());
                if (JS_IsException(r)) {
                    JSValue exc = JS_GetException(m_ctx);
                    JS_FreeValue(m_ctx, exc);
                }
                JS_FreeValue(m_ctx, r);
            }
            for (JSValue &a : args) JS_FreeValue(m_ctx, a);
            JS_FreeValue(m_ctx, argsV);
            JS_FreeValue(m_ctx, fn);
        }
        JS_FreeValue(m_ctx, rec);
        if (JS_IsObject(timers))
            JS_SetPropertyUint32(m_ctx, timers, static_cast<uint32_t>(t.id), JS_UNDEFINED);
    }
    JS_FreeValue(m_ctx, timers);
    JS_FreeValue(m_ctx, g);
}

void LxScriptEngine::runPendingJobs(int maxMs)
{
    QElapsedTimer timer;
    timer.start();
    while (JS_IsJobPending(m_rt) || !m_timers.isEmpty()) {
        runDueTimers();
        if (JS_IsJobPending(m_rt)) {
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
        } else if (m_timers.isEmpty()) {
            break;
        } else {
            // ⚠ 同 pumpUntilDone：不在非 GUI 线程泵 Qt 事件循环。QuickJS 的 setTimeout/微任务
            //   由 runDueTimers + JS_ExecutePendingJob 推进，短睡让定时器到期即可，不需要 processEvents。
            QThread::msleep(1);
        }
        if (timer.elapsed() > maxMs) break;
    }
}

/// 驱动 QuickJS 微任务/定时器 + Qt 事件，直到 m_done 置位或超时
bool LxScriptEngine::pumpUntilDone(int maxMs)
{
    QElapsedTimer timer;
    timer.start();
    m_done = false;
    while (!m_done && timer.elapsed() < maxMs) {
        runDueTimers();
        if (JS_IsJobPending(m_rt)) {
            int r = JS_ExecutePendingJob(m_rt, &m_ctx);
            if (r < 0) {
                // 清空 pending exception，避免污染后续调用
                JSValue exc = JS_GetException(m_ctx);
                JS_FreeValue(m_ctx, exc);
                break;
            }
        }
        // ⚠ 绝不能在非 GUI 线程调 QCoreApplication::processEvents()！
        //   本函数跑在 QtConcurrent worker 线程（MusicSdk::resolveUrl → musicUrl → requestString）。
        //   脚本的 request() 是同步阻塞（局部 QNAM + execRequest 自带阻塞循环），Promise 的 settle
        //   全在本线程内闭合，根本不依赖 Qt 事件循环推进。以前这里 processEvents 会把「主线程投递到
        //   本线程队列」的事件（含另一条并发取源的 queued 回调、UI 事件）在 worker 线程里乱执行 →
        //   与 m_mutex 形成嵌套等待 → 「加载歌曲时切音源大概率卡死未响应」（用户报）。
        //   改成纯 QuickJS job/timer 驱动 + 短睡；无待办且没 settle 就交给超时兜底。
        if (!JS_IsJobPending(m_rt) && m_timers.isEmpty()) break;
        QThread::msleep(1);
    }
    return m_done;
}

// ===========================================================================
// 脚本加载
// ===========================================================================

QVariantMap LxScriptEngine::parseScriptMeta(const QString &code)
{
    QVariantMap meta;
    QString text = code;
    if (text.startsWith(QChar(0xFEFF))) text.remove(0, 1);   // 去 BOM

    const int start = text.indexOf(QStringLiteral("/**"));
    if (start < 0) return meta;
    const int end = text.indexOf(QStringLiteral("*/"), start);
    const QString block = text.mid(start, (end < 0 ? text.size() : end + 2) - start);

    const QStringList lines = block.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = stripCommentPrefix(raw);
        if (!line.startsWith(QLatin1Char('@'))) continue;
        int sp = -1;
        for (int i = 1; i < line.size(); ++i) {
            if (line.at(i).isSpace()) { sp = i; break; }
        }
        if (sp < 0) continue;
        const QString key = line.mid(1, sp - 1).toLower();
        const QString value = line.mid(sp).trimmed();
        if (key == QLatin1String("name"))        meta[QStringLiteral("name")] = value;
        else if (key == QLatin1String("description")) meta[QStringLiteral("description")] = value;
        else if (key == QLatin1String("version"))     meta[QStringLiteral("version")] = value;
        else if (key == QLatin1String("author"))      meta[QStringLiteral("author")] = value;
        else if (key == QLatin1String("homepage"))    meta[QStringLiteral("homepage")] = value;
    }
    return meta;
}

bool LxScriptEngine::looksLikeLxScript(const QString &code)
{
    if (code.trimmed().isEmpty()) return false;
    // 新协议三特征，命中任一即认为是新版脚本
    if (code.contains(QStringLiteral("EVENT_NAMES"))) return true;
    if (code.contains(QStringLiteral("globalThis.lx"))) return true;
    return code.contains(QStringLiteral("lx.on")) && code.contains(QStringLiteral("inited"));
}

QStringList LxScriptEngine::declaredQualitys(const QString &source) const
{
    QStringList out;
    const QVariantMap sources = m_scriptInfo.value(QStringLiteral("sources")).toMap();
    const QVariantMap one = sources.value(source).toMap();
    for (const QVariant &q : one.value(QStringLiteral("qualitys")).toList())
        out.append(q.toString());
    return out;
}

QString LxScriptEngine::protocolQualityId(const QString &appQualityId)
{
    // 协议只有四档：128k / 320k / flac / flac24bit。
    // 我们多出来的三档（hires/atmos/master）都超出 16bit flac，统一按 flac24bit 问脚本。
    const QString id = appQualityId.trimmed().toLower();
    if (id == QLatin1String("hires") || id == QLatin1String("atmos")
        || id == QLatin1String("master"))
        return QStringLiteral("flac24bit");
    return id;
}

bool LxScriptEngine::loadScript(const QString &path, QString *error)
{
    QMutexLocker locker(&m_mutex);

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法读取脚本文件");
        return false;
    }
    QByteArray code = f.readAll();
    f.close();
    // 带 BOM 的脚本文件不少见（记事本另存就会加），QuickJS 不认 → 先剥掉
    if (code.startsWith("\xEF\xBB\xBF")) code.remove(0, 3);

    m_inited = false;
    m_initError.clear();
    m_scriptInfo.clear();
    m_updateInfo.clear();
    m_updateAlertSent = false;
    m_handler = JS_UNDEFINED;
    clearTimers();

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
    JS_SetHostPromiseRejectionTracker(m_rt, onPromiseRejection, nullptr);
    m_ctx = JS_NewContext(m_rt);
    JS_SetContextOpaque(m_ctx, this);
    registerApi();

    // currentScriptInfo：官方语义是"导入时在头部解析到的"真值，必须能读到
    QVariantMap meta = parseScriptMeta(QString::fromUtf8(code));
    meta[QStringLiteral("rawScript")] = QString::fromUtf8(code);
    applyCurrentScriptInfo(m_ctx, meta);

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

    // 执行同步初始化产生的微任务/定时器，等待 send('inited')。
    // 部分脚本的 inited 依赖异步 HTTP 回调或 setTimeout（先 request/延时 再 send），
    // 需循环驱动直到就绪；既没有待办微任务、也没有待触发的定时器才可能提前退出。
    {
        QElapsedTimer timer;
        timer.start();
        while (!m_inited && timer.elapsed() < 8000) {
            runDueTimers();
            if (JS_IsJobPending(m_rt)) {
                int r = JS_ExecutePendingJob(m_rt, &m_ctx);
                if (r < 0) {
                    // 脚本自己抛了错（比如"初始化阶段请求失败就 x('脚本初始化失败')"）：
                    // 留下这条信息，否则用户只看到"初始化超时"，一头雾水
                    JSValue exc = JS_GetException(m_ctx);
                    size_t l = 0; const char *msg = JS_ToCStringLen(m_ctx, &l, exc);
                    if (msg && *msg && m_initError.isEmpty())
                        m_initError = QString::fromUtf8(msg, static_cast<int>(l));
                    if (msg) JS_FreeCString(m_ctx, msg);
                    JS_FreeValue(m_ctx, exc);
                    break;
                }
            } else if (!m_timers.isEmpty()) {
                // ⚠ 关键：GUI 线程（主线程）加载时泵 Qt 事件循环，避免"脚本加载卡死 UI"；
                //   同时保证 QuickJS 实例在**主线程**创建（1.1.5 在 worker 线程加载导致
                //   跨线程调用 musicUrl 返回空 → 播放失败兜底酷我，用户实测确认）。
                //   非 GUI 线程才 msleep（worker 线程泵事件循环是 UB，见 pumpUntilDone 注释）。
                if (QThread::currentThread() == QCoreApplication::instance()->thread())
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                else
                    QThread::msleep(1);
            } else {
                break;   // 彻底没事可做
            }
        }

        // m_inited 为 true 后，继续驱动微任务/定时器，让 checkUpdate() 等异步逻辑完成。
        // 脚本可能在 send('inited') 后调用 checkUpdate()，该函数发 HTTP 请求，
        // Promise 尚未 resolve 时退出循环会导致 send('updateAlert') 永远不会被调用。
        if (m_inited) {
            // 更新提醒不是播放关键路径：只等 2s（脚本 checkUpdate 网络慢时主线程少被占用）
            while ((JS_IsJobPending(m_rt) || !m_timers.isEmpty()) && timer.elapsed() < 2000) {
                runDueTimers();
                if (JS_IsJobPending(m_rt)) {
                    int r = JS_ExecutePendingJob(m_rt, &m_ctx);
                    if (r < 0) {
                        JSValue exc = JS_GetException(m_ctx);
                        JS_FreeValue(m_ctx, exc);
                        break;
                    }
                } else if (!m_timers.isEmpty()) {
                    // 同前：GUI 线程泵事件循环（不冻结 UI），非 GUI 线程 msleep
                    if (QThread::currentThread() == QCoreApplication::instance()->thread())
                        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                    else
                        QThread::msleep(1);
                } else {
                    break;
                }
            }
        }
    }

    if (!m_inited) {
        if (m_initError.isEmpty())
            m_initError = QStringLiteral("脚本初始化超时（未调用 send('inited')）");
        if (error) *error = m_initError;
        return false;
    }
    if (m_scriptInfo.value(QStringLiteral("name")).toString().isEmpty()) {
        // inited 里没给 name 时用头部注释的 @name，再退到默认名
        const QString metaName = meta.value(QStringLiteral("name")).toString();
        m_scriptInfo[QStringLiteral("name")] =
            metaName.isEmpty() ? QStringLiteral("LX 音源") : metaName;
    }
    // 头部注释的其余字段也补进 scriptInfo（UI 想显示作者/版本时能直接用）
    for (const QString &key : { QStringLiteral("description"), QStringLiteral("version"),
                                QStringLiteral("author"), QStringLiteral("homepage") }) {
        if (m_scriptInfo.value(key).toString().isEmpty())
            m_scriptInfo[key] = meta.value(key);
    }
    return true;
}

// ===========================================================================
// 请求脚本动作（musicUrl / lyric / pic，协议统一入口 requestString）
// ===========================================================================

QString LxScriptEngine::requestString(const QString &source, const QString &action,
                                      const QVariantMap &infoMap, QString *error)
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
    const QByteArray actionUtf8 = action.toUtf8();
    JSValue data = JS_NewObject(m_ctx);
    JS_SetPropertyStr(m_ctx, data, "source",
        JS_NewStringLen(m_ctx, sourceUtf8.constData(), static_cast<size_t>(sourceUtf8.size())));
    JS_SetPropertyStr(m_ctx, data, "action",
        JS_NewStringLen(m_ctx, actionUtf8.constData(), static_cast<size_t>(actionUtf8.size())));
    JS_SetPropertyStr(m_ctx, data, "info", variantToJs(m_ctx, infoMap));

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
        // fail 时用 'ERR:' 前缀透传错误信息；lyric 的 { lrc } 对象一并解出
        static const char *waitCode =
            "(function(p, cb){"
            "  function ok(v){"
            "    if (typeof v === 'string') cb(v);"
            "    else if (v && typeof v === 'object' && typeof v.url === 'string') cb(v.url);"
            "    else if (v && typeof v === 'object' && typeof v.lrc === 'string') cb(v.lrc);"
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

        // 驱动微任务/定时器直到 settle（默认 20s 超时）
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
        *error = QStringLiteral("脚本未返回结果");
    return out;
}

QString LxScriptEngine::musicUrl(const QString &source, const QVariantMap &musicInfo,
                                 const QString &quality, QString *error)
{
    // 协议：musicUrl 的 info = { musicInfo, type }
    QVariantMap info;
    info[QStringLiteral("musicInfo")] = musicInfo;
    info[QStringLiteral("type")] = quality;
    return requestString(source, QStringLiteral("musicUrl"), info, error);
}

QString LxScriptEngine::lyric(const QString &source, const QVariantMap &musicInfo,
                              QString *error)
{
    // 协议：lyric 的 info = 歌曲信息对象；脚本返回 rawLrc 文本或 { lrc }
    return requestString(source, QStringLiteral("lyric"), musicInfo, error);
}

QString LxScriptEngine::pic(const QString &source, const QVariantMap &musicInfo,
                            QString *error)
{
    // 协议：pic 的 info = 歌曲信息对象；脚本返回封面图 URL
    return requestString(source, QStringLiteral("pic"), musicInfo, error);
}

} // namespace Muyun
