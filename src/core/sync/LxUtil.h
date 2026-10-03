#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace Muyun {

/**
 * @brief 洛雪音乐（lx-music-desktop / lx-music-mobile）局域网同步协议的
 *        常量与编码工具。协议细节逆向自 lx-music-desktop 源码
 *        src/common/constants_sync.ts、src/main/modules/sync/server/*。
 *
 * 要点：
 *  - 传输 = 单端口 HTTP(/hello /id /ah) + WebSocket(/socket?i=&t=)
 *  - 消息体不加密；>1024 字符时 gzip+base64 并加前缀 "cg_"
 *  - 鉴权 = AES-128-ECB(key=md5(口令)前16字符的 base64) 换 clientId+随机 key，
 *    回包用客户端上传的 RSA 公钥做 OAEP(SHA-1) 加密；
 *    WS 升级时 t = AES(key, "lx-music connect") 作简单凭证
 */
namespace LxSync {

// ---- 协议常量（与洛雪 SYNC_CODE 一致）----
inline constexpr const char *kHelloMsg     = "Hello~::^-^::~v4~";
inline constexpr const char *kIdPrefix     = "OjppZDo6";
inline constexpr const char *kAuthMsg      = "lx-music auth::";
inline constexpr const char *kMsgAuthFailed = "Auth failed";
inline constexpr const char *kMsgBlockedIp = "Blocked IP";
inline constexpr const char *kMsgConnect   = "lx-music connect";

enum CloseCode { kCloseNormal = 1000, kCloseFailed = 4100 };

/// gzip（RFC1952）压缩/解压；失败返回空
QByteArray gzipCompress(const QByteArray &plain);
QByteArray gzipUncompress(const QByteArray &zipped);

/// 出站封包：>1024 字符则 "cg_" + base64(gzip(utf8))
QByteArray encodeMsg(const QString &text);
/// 入站解包：识别 "cg_" 前缀解压；失败返回空
QByteArray decodeMsg(const QByteArray &frame);

/// AES-128-ECB（key 为 base64 的 16 字节），对应洛雪 aesEncrypt/aesDecrypt
QByteArray aesEncryptB64Key(const QByteArray &plain, const QByteArray &keyB64);
QByteArray aesDecryptB64Key(const QByteArray &cipherB64, const QByteArray &keyB64);

/// 鉴权口令：6 位数字（洛雪 Math.random().toString().substring(2,8) 同构），3 分钟轮换
QString generateAuthCode();

/// 随机 base64 id（洛雪 randomBytes(n).toString('base64') 同构）
QString randomB64(int bytes);

} // namespace LxSync
} // namespace Muyun
