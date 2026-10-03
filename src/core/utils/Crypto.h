#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

namespace Muyun {
namespace Crypto {

/// AES-128 ECB 加密，PKCS7 填充，输出原始字节
QByteArray aes128EcbEncrypt(const QByteArray &plain, const QByteArray &key);

/// AES-128 CBC 加密，PKCS7 填充，输出原始字节
QByteArray aes128CbcEncrypt(const QByteArray &plain, const QByteArray &key,
                            const QByteArray &iv);

/// AES-128 ECB 解密（用于 QRC/KRC 等歌词解密），PKCS7 去填充
QByteArray aes128EcbDecrypt(const QByteArray &cipher, const QByteArray &key);

/// AES-128 CBC 解密
QByteArray aes128CbcDecrypt(const QByteArray &cipher, const QByteArray &key,
                            const QByteArray &iv);

/// MD5（32 位小写十六进制）
QString md5Hex(const QByteArray &data);
QByteArray md5Raw(const QByteArray &data);

/// SHA-256 十六进制（用于更新校验等）
QString sha256Hex(const QByteArray &data);

/**
 * @brief RSA 公钥加密（无填充 / textbook RSA）
 *
 * 网易云 weapi 使用：将随机密钥反转后，用固定公钥做 RSA_NO_PADDING 加密。
 * @param data  待加密数据（长度需 <= 模长）
 * @param pem    PEM 格式公钥（可含或不含首尾标记行）
 * @return 加密结果的小写十六进制字符串；失败返回空
 */
QString rsaNoPaddingEncryptHex(const QByteArray &data, const QString &pem);

/**
 * @brief RSA-OAEP(SHA-1, 空 label) 公钥加密，对应 Node crypto 的
 *        publicEncrypt({padding: RSA_PKCS1_OAEP_PADDING})（其默认 hash 即 SHA-1）。
 *        洛雪局域网同步鉴权回包用。
 * @param data  明文（长度需 <= k-66，k=模数字节数）
 * @param pem   SPKI/PKCS#1 公钥 PEM（可含或不含首尾标记行；换行随意）
 * @return 原始密文字节（k 长度）；失败返回空
 */
QByteArray rsaOaepEncrypt(const QByteArray &data, const QString &pem);

/**
 * @brief RSA-OAEP(SHA-1, 空 label) 私钥解密（洛雪同步测试端模拟手机解回包用）。
 * @param cipher 原始密文
 * @param pem    PKCS#1 RSAPrivateKey PEM（BEGIN RSA PRIVATE KEY）
 * @return 明文；失败返回空
 */
QByteArray rsaOaepDecrypt(const QByteArray &cipher, const QString &pem);

/// Base64 编码 / 解码
QByteArray base64Encode(const QByteArray &data);
QByteArray base64Decode(const QByteArray &data);
QString base64EncodeStr(const QByteArray &data);

/// 生成 16 位随机字符串（BASE62 字符集），与 JS randomKey 行为一致
QString randomBase62Key(int length = 16);

/// 生成指定字节数的随机十六进制串
QString randomHex(int bytes);

} // namespace Crypto
} // namespace Muyun
