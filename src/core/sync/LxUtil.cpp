#include "LxUtil.h"

#include "core/utils/Crypto.h"

#include <QByteArray>
#include <QRandomGenerator>

#include <zlib.h>

namespace Muyun {
namespace LxSync {

QByteArray gzipCompress(const QByteArray &plain)
{
    z_stream zs{};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK)
        return {};
    QByteArray out;
    out.resize(int(deflateBound(&zs, uLong(plain.size())) + 64));
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(plain.constData()));
    zs.avail_in = uInt(plain.size());
    zs.next_out = reinterpret_cast<Bytef *>(out.data());
    zs.avail_out = uInt(out.size());
    const int ret = deflate(&zs, Z_FINISH);
    const int written = out.size() - int(zs.avail_out);
    deflateEnd(&zs);
    if (ret != Z_STREAM_END) return {};
    out.resize(written);
    return out;
}

QByteArray gzipUncompress(const QByteArray &zipped)
{
    z_stream zs{};
    if (inflateInit2(&zs, 15 + 32) != Z_OK)   // 15+32：自动识别 gzip/zlib
        return {};
    QByteArray out;
    out.resize(qMax(4096, zipped.size() * 8));
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(zipped.constData()));
    zs.avail_in = uInt(zipped.size());
    zs.next_out = reinterpret_cast<Bytef *>(out.data());
    zs.avail_out = uInt(out.size());
    int ret = Z_OK;
    forever {
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK && ret != Z_BUF_ERROR) { inflateEnd(&zs); return {}; }
        if (zs.avail_out > 0) {
            if (ret == Z_BUF_ERROR && zs.avail_in == 0) {   // 输入耗尽但未结束
                inflateEnd(&zs);
                return {};
            }
            continue;
        }
        const int used = out.size();
        out.resize(used * 2);
        zs.next_out = reinterpret_cast<Bytef *>(out.data() + used);
        zs.avail_out = uInt(out.size() - used);
    }
    const int produced = int(zs.total_out);
    inflateEnd(&zs);
    out.resize(produced);
    return out;
}

QByteArray encodeMsg(const QString &text)
{
    if (text.size() <= 1024) return text.toUtf8();
    const QByteArray gz = gzipCompress(text.toUtf8());
    if (gz.isEmpty()) return text.toUtf8();
    return QByteArray("cg_") + gz.toBase64();
}

QByteArray decodeMsg(const QByteArray &frame)
{
    if (!frame.startsWith("cg_")) return frame;
    const QByteArray raw = QByteArray::fromBase64(frame.mid(3));
    return gzipUncompress(raw);   // 失败为空，调用方判错
}

QByteArray aesEncryptB64Key(const QByteArray &plain, const QByteArray &keyB64)
{
    return Crypto::aes128EcbEncrypt(plain, QByteArray::fromBase64(keyB64)).toBase64();
}

QByteArray aesDecryptB64Key(const QByteArray &cipherB64, const QByteArray &keyB64)
{
    return Crypto::aes128EcbDecrypt(QByteArray::fromBase64(cipherB64),
                                    QByteArray::fromBase64(keyB64));
}

QString generateAuthCode()
{
    // 洛雪：Math.random().toString().substring(2, 8) → 6 位数字（可含前导零）
    auto *rng = QRandomGenerator::global();
    return QString::number(rng->bounded(1000000), 10).rightJustified(6, QLatin1Char('0'));
}

QString randomB64(int bytes)
{
    QByteArray raw(bytes, '\0');
    auto *rng = QRandomGenerator::global();
    for (int i = 0; i < bytes; ++i) raw[i] = char(rng->bounded(256));
    return QString::fromLatin1(raw.toBase64());
}

} // namespace LxSync
} // namespace Muyun
