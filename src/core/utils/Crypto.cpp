#include "Crypto.h"

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QDateTime>

#include <algorithm>

namespace Muyun {
namespace Crypto {

// ===========================================================================
// AES-128
// ===========================================================================

namespace {

const quint8 SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

const quint8 INV_SBOX[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

const quint8 RCON[11] = { 0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36 };

inline quint8 xtime(quint8 x) { return static_cast<quint8>((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00)); }

inline quint8 mulGalois(quint8 a, quint8 b)
{
    quint8 r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

/// 轮密钥扩展：生成 44 个 word（11 组 × 4）
void expandKey(const QByteArray &key, quint8 roundKey[44][4])
{
    quint8 tmp[4];
    for (int i = 0; i < 16; ++i)
        roundKey[i / 4][i % 4] = static_cast<quint8>(key[i]);

    for (int i = 4; i < 44; ++i) {
        tmp[0] = roundKey[i - 1][0];
        tmp[1] = roundKey[i - 1][1];
        tmp[2] = roundKey[i - 1][2];
        tmp[3] = roundKey[i - 1][3];

        if (i % 4 == 0) {
            // RotWord
            const quint8 t = tmp[0];
            tmp[0] = tmp[1]; tmp[1] = tmp[2]; tmp[2] = tmp[3]; tmp[3] = t;
            // SubWord
            for (int j = 0; j < 4; ++j) tmp[j] = SBOX[tmp[j]];
            tmp[0] ^= RCON[i / 4];
        }
        for (int j = 0; j < 4; ++j)
            roundKey[i][j] = roundKey[i - 4][j] ^ tmp[j];
    }
}

void addRoundKey(quint8 state[16], const quint8 roundKey[44][4], int round)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            state[c * 4 + r] ^= roundKey[round * 4 + c][r];
}

void subBytes(quint8 state[16])
{
    for (int i = 0; i < 16; ++i) state[i] = SBOX[state[i]];
}

void invSubBytes(quint8 state[16])
{
    for (int i = 0; i < 16; ++i) state[i] = INV_SBOX[state[i]];
}

/// state 按列优先存放：state[c*4+r]，即 state[r][c] = state[c*4+r]
/// ShiftRows：第 r 行循环左移 r 个字节 -> new[r][c] = old[r][(c+r)%4]
void shiftRows(quint8 state[16])
{
    quint8 t[16];
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            t[c * 4 + r] = state[((c + r) % 4) * 4 + r];
    memcpy(state, t, 16);
}

void invShiftRows(quint8 state[16])
{
    // 逆 ShiftRows：第 r 行循环右移 r 字节 -> new[r][c] = old[r][(c-r+4)%4]
    // （正变换是 new[r][c]=old[r][(c+r)%4]，二者复合须为恒等）
    quint8 t[16];
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            t[c * 4 + r] = state[((c + 4 - r) % 4) * 4 + r];
    memcpy(state, t, 16);
}

void mixColumns(quint8 state[16])
{
    for (int c = 0; c < 4; ++c) {
        const quint8 a0 = state[c * 4 + 0];
        const quint8 a1 = state[c * 4 + 1];
        const quint8 a2 = state[c * 4 + 2];
        const quint8 a3 = state[c * 4 + 3];
        state[c * 4 + 0] = mulGalois(a0, 2) ^ mulGalois(a1, 3) ^ a2 ^ a3;
        state[c * 4 + 1] = a0 ^ mulGalois(a1, 2) ^ mulGalois(a2, 3) ^ a3;
        state[c * 4 + 2] = a0 ^ a1 ^ mulGalois(a2, 2) ^ mulGalois(a3, 3);
        state[c * 4 + 3] = mulGalois(a0, 3) ^ a1 ^ a2 ^ mulGalois(a3, 2);
    }
}

void invMixColumns(quint8 state[16])
{
    for (int c = 0; c < 4; ++c) {
        const quint8 a0 = state[c * 4 + 0];
        const quint8 a1 = state[c * 4 + 1];
        const quint8 a2 = state[c * 4 + 2];
        const quint8 a3 = state[c * 4 + 3];
        state[c * 4 + 0] = mulGalois(a0, 14) ^ mulGalois(a1, 11) ^ mulGalois(a2, 13) ^ mulGalois(a3, 9);
        state[c * 4 + 1] = mulGalois(a0, 9) ^ mulGalois(a1, 14) ^ mulGalois(a2, 11) ^ mulGalois(a3, 13);
        state[c * 4 + 2] = mulGalois(a0, 13) ^ mulGalois(a1, 9) ^ mulGalois(a2, 14) ^ mulGalois(a3, 11);
        state[c * 4 + 3] = mulGalois(a0, 11) ^ mulGalois(a1, 13) ^ mulGalois(a2, 9) ^ mulGalois(a3, 14);
    }
}

/// 加密单个 16 字节块（state 输入输出均为 16 字节）
void encryptBlock(const quint8 in[16], quint8 out[16], const quint8 rk[44][4])
{
    quint8 state[16];
    memcpy(state, in, 16);
    addRoundKey(state, rk, 0);
    for (int round = 1; round < 10; ++round) {
        subBytes(state);
        shiftRows(state);
        mixColumns(state);
        addRoundKey(state, rk, round);
    }
    subBytes(state);
    shiftRows(state);
    addRoundKey(state, rk, 10);
    memcpy(out, state, 16);
}

void decryptBlock(const quint8 in[16], quint8 out[16], const quint8 rk[44][4])
{
    quint8 state[16];
    memcpy(state, in, 16);
    addRoundKey(state, rk, 10);
    for (int round = 9; round >= 1; --round) {
        invShiftRows(state);
        invSubBytes(state);
        addRoundKey(state, rk, round);
        invMixColumns(state);
    }
    invShiftRows(state);
    invSubBytes(state);
    addRoundKey(state, rk, 0);
    memcpy(out, state, 16);
}

QByteArray pkcs7Pad(const QByteArray &data)
{
    const int padLen = 16 - (data.size() % 16);
    QByteArray padded = data;
    padded.append(QByteArray(padLen, static_cast<char>(padLen)));
    return padded;
}

QByteArray pkcs7Unpad(const QByteArray &data)
{
    if (data.isEmpty() || (data.size() % 16) != 0) return data;
    const int padLen = static_cast<quint8>(data[data.size() - 1]);
    if (padLen < 1 || padLen > 16 || padLen > data.size()) return data;
    return data.left(data.size() - padLen);
}

QByteArray normalizeKey(const QByteArray &key)
{
    QByteArray k = key;
    if (k.size() > 16) k = k.left(16);
    while (k.size() < 16) k.append('\0');
    return k;
}

QByteArray normalizeIv(const QByteArray &iv)
{
    QByteArray v = iv;
    if (v.size() > 16) v = v.left(16);
    while (v.size() < 16) v.append('\0');
    return v;
}

} // namespace

QByteArray aes128EcbEncrypt(const QByteArray &plain, const QByteArray &key)
{
    if (key.size() < 16) return QByteArray();
    quint8 rk[44][4];
    expandKey(normalizeKey(key), rk);

    const QByteArray data = pkcs7Pad(plain);
    QByteArray out;
    quint8 block[16], enc[16];
    for (int i = 0; i < data.size(); i += 16) {
        memcpy(block, data.constData() + i, 16);
        encryptBlock(block, enc, rk);
        out.append(reinterpret_cast<const char *>(enc), 16);
    }
    return out;
}

/// 无填充 ECB（LX 音源脚本协议里 aes-128-ecb 就是 NoPadding，见官方 user-api-preload.js）。
/// 数据不是 16 字节整数倍时返回空（与 Node createCipheriv 无自动填充时的报错语义一致）。
QByteArray aes128EcbEncryptNoPad(const QByteArray &plain, const QByteArray &key)
{
    if (key.size() < 16 || (plain.size() % 16) != 0) return QByteArray();
    quint8 rk[44][4];
    expandKey(normalizeKey(key), rk);
    QByteArray out;
    quint8 block[16], enc[16];
    for (int i = 0; i < plain.size(); i += 16) {
        memcpy(block, plain.constData() + i, 16);
        encryptBlock(block, enc, rk);
        out.append(reinterpret_cast<const char *>(enc), 16);
    }
    return out;
}

/// 无填充 ECB 解密（不做 pkcs7 去填充）
QByteArray aes128EcbDecryptNoPad(const QByteArray &cipher, const QByteArray &key)
{
    if (key.size() < 16 || cipher.isEmpty() || (cipher.size() % 16) != 0)
        return QByteArray();
    quint8 rk[44][4];
    expandKey(normalizeKey(key), rk);
    QByteArray out;
    quint8 block[16], dec[16];
    for (int i = 0; i < cipher.size(); i += 16) {
        memcpy(block, cipher.constData() + i, 16);
        decryptBlock(block, dec, rk);
        out.append(reinterpret_cast<const char *>(dec), 16);
    }
    return out;
}

QByteArray aes128CbcEncrypt(const QByteArray &plain, const QByteArray &key,
                            const QByteArray &iv)
{
    if (key.size() < 16) return QByteArray();
    quint8 rk[44][4];
    expandKey(normalizeKey(key), rk);
    const QByteArray ivBytes = normalizeIv(iv);

    const QByteArray data = pkcs7Pad(plain);
    QByteArray out;
    quint8 block[16], enc[16];
    quint8 prev[16];
    memcpy(prev, ivBytes.constData(), 16);

    for (int i = 0; i < data.size(); i += 16) {
        memcpy(block, data.constData() + i, 16);
        for (int j = 0; j < 16; ++j) block[j] ^= prev[j];
        encryptBlock(block, enc, rk);
        out.append(reinterpret_cast<const char *>(enc), 16);
        memcpy(prev, enc, 16);
    }
    return out;
}

QByteArray aes128EcbDecrypt(const QByteArray &cipher, const QByteArray &key)
{
    if (key.size() < 16 || cipher.isEmpty() || (cipher.size() % 16) != 0)
        return QByteArray();
    quint8 rk[44][4];
    expandKey(normalizeKey(key), rk);

    QByteArray out;
    quint8 block[16], dec[16];
    for (int i = 0; i < cipher.size(); i += 16) {
        memcpy(block, cipher.constData() + i, 16);
        decryptBlock(block, dec, rk);
        out.append(reinterpret_cast<const char *>(dec), 16);
    }
    return pkcs7Unpad(out);
}

QByteArray aes128CbcDecrypt(const QByteArray &cipher, const QByteArray &key,
                            const QByteArray &iv)
{
    if (key.size() < 16 || cipher.isEmpty() || (cipher.size() % 16) != 0)
        return QByteArray();
    quint8 rk[44][4];
    expandKey(normalizeKey(key), rk);
    QByteArray out;
    quint8 block[16], dec[16];
    quint8 prev[16];
    memcpy(prev, normalizeIv(iv).constData(), 16);

    for (int i = 0; i < cipher.size(); i += 16) {
        memcpy(block, cipher.constData() + i, 16);
        decryptBlock(block, dec, rk);
        for (int j = 0; j < 16; ++j) dec[j] ^= prev[j];
        out.append(reinterpret_cast<const char *>(dec), 16);
        memcpy(prev, block, 16);
    }
    return pkcs7Unpad(out);
}

// ===========================================================================
// 摘要
// ===========================================================================

QByteArray md5Raw(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Md5);
}

QString md5Hex(const QByteArray &data)
{
    return QString::fromUtf8(md5Raw(data).toHex());
}

QString sha256Hex(const QByteArray &data)
{
    return QString::fromUtf8(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

// ===========================================================================
// 大数运算（用于 textbook RSA）
// ===========================================================================

namespace {

/// 大端字节数组 -> 小端 32 位字数组
QVector<quint32> fromBigEndian(const QByteArray &bytes)
{
    QVector<quint32> words;
    const int n = bytes.size();
    // 从低位开始，每 4 字节组成一个 word
    for (int i = n; i > 0; i -= 4) {
        quint32 w = 0;
        for (int j = std::max(0, i - 4); j < i; ++j) {
            w = (w << 8) | static_cast<quint32>(static_cast<quint8>(bytes[j]));
        }
        words.append(w);
    }
    while (!words.isEmpty() && words.last() == 0) words.removeLast();
    return words;
}

/// 小端字数组 -> 固定长度的大端字节数组
QByteArray toBigEndian(const QVector<quint32> &words, int byteLen)
{
    QByteArray out(byteLen, '\0');
    for (int i = 0; i < words.size() && i * 4 < byteLen; ++i) {
        const quint32 w = words[i];
        for (int b = 0; b < 4; ++b) {
            const int pos = byteLen - 1 - (i * 4 + b);
            if (pos >= 0)
                out[pos] = static_cast<char>((w >> (b * 8)) & 0xFF);
        }
    }
    return out;
}

int bitLength(const QVector<quint32> &a)
{
    if (a.isEmpty()) return 0;
    const quint32 top = a.last();
    int bits = a.size() * 32;
    quint32 mask = 0x80000000u;
    while (mask && !(top & mask)) { --bits; mask >>= 1; }
    return bits;
}

bool getBit(const QVector<quint32> &a, int index)
{
    const int wordIdx = index / 32;
    if (wordIdx >= a.size()) return false;
    return (a[wordIdx] >> (index % 32)) & 1u;
}

int compare(const QVector<quint32> &a, const QVector<quint32> &b)
{
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    for (int i = a.size() - 1; i >= 0; --i) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

void trim(QVector<quint32> &a)
{
    while (!a.isEmpty() && a.last() == 0) a.removeLast();
}

QVector<quint32> add(const QVector<quint32> &a, const QVector<quint32> &b)
{
    QVector<quint32> r;
    const int n = std::max(a.size(), b.size());
    quint64 carry = 0;
    for (int i = 0; i < n; ++i) {
        const quint64 av = i < a.size() ? a[i] : 0u;
        const quint64 bv = i < b.size() ? b[i] : 0u;
        const quint64 sum = av + bv + carry;
        r.append(static_cast<quint32>(sum & 0xFFFFFFFFu));
        carry = sum >> 32;
    }
    if (carry) r.append(static_cast<quint32>(carry));
    return r;
}

/// a - b，要求 a >= b
QVector<quint32> sub(const QVector<quint32> &a, const QVector<quint32> &b)
{
    QVector<quint32> r = a;
    qint64 borrow = 0;
    for (int i = 0; i < r.size(); ++i) {
        const qint64 av = static_cast<qint64>(r[i]);
        const qint64 bv = i < b.size() ? static_cast<qint64>(b[i]) : 0;
        qint64 diff = av - bv - borrow;
        if (diff < 0) { diff += 0x100000000LL; borrow = 1; }
        else borrow = 0;
        r[i] = static_cast<quint32>(diff);
    }
    trim(r);
    return r;
}

/// (a << 1) mod m，要求 a < m
QVector<quint32> shlMod(const QVector<quint32> &a, const QVector<quint32> &m)
{
    QVector<quint32> r = a;
    quint32 carry = 0;
    for (int i = 0; i < r.size(); ++i) {
        const quint64 v = (static_cast<quint64>(r[i]) << 1) | carry;
        r[i] = static_cast<quint32>(v & 0xFFFFFFFFu);
        carry = static_cast<quint32>(v >> 32);
    }
    if (carry) r.append(carry);
    trim(r);
    if (compare(r, m) >= 0) r = sub(r, m);
    return r;
}

/// (a + b) mod m，要求 a < m 且 b < m
QVector<quint32> addMod(const QVector<quint32> &a, const QVector<quint32> &b,
                        const QVector<quint32> &m)
{
    QVector<quint32> r = add(a, b);
    if (compare(r, m) >= 0) r = sub(r, m);
    return r;
}

/// (a * b) mod m，使用移位累加（double-and-add）
QVector<quint32> mulMod(const QVector<quint32> &a, const QVector<quint32> &b,
                        const QVector<quint32> &m)
{
    QVector<quint32> r;
    const int bits = bitLength(a);
    for (int i = bits - 1; i >= 0; --i) {
        r = shlMod(r, m);
        if (getBit(a, i)) r = addMod(r, b, m);
    }
    return r;
}

/// base^exp mod m（平方-乘）
QVector<quint32> modPow(const QVector<quint32> &base, const QVector<quint32> &exp,
                        const QVector<quint32> &m)
{
    QVector<quint32> result;
    result.append(1u);
    QVector<quint32> b = base;
    const int bits = bitLength(exp);
    for (int i = 0; i < bits; ++i) {
        if (getBit(exp, i)) result = mulMod(result, b, m);
        b = mulMod(b, b, m);
    }
    return result;
}

/// 极简 DER 读取器：返回 (tag, 内容起始偏移, 内容长度)
struct DerNode {
    quint8 tag = 0;
    int start = 0;   // 内容起始
    int length = 0;
};

bool readDer(const QByteArray &der, int &pos, DerNode &node)
{
    if (pos >= der.size()) return false;
    node.tag = static_cast<quint8>(der[pos++]);
    if (pos >= der.size()) return false;
    quint8 first = static_cast<quint8>(der[pos++]);
    if (first & 0x80) {
        const int lenBytes = first & 0x7F;
        if (lenBytes == 0 || lenBytes > 4) return false;
        node.length = 0;
        for (int i = 0; i < lenBytes; ++i) {
            if (pos >= der.size()) return false;
            node.length = (node.length << 8) | static_cast<quint8>(der[pos++]);
        }
    } else {
        node.length = first;
    }
    node.start = pos;
    pos += node.length;
    return pos <= der.size();
}

} // namespace

QString rsaNoPaddingEncryptHex(const QByteArray &data, const QString &pem)
{
    // --- 提取 PEM 主体并 base64 解码 ---
    QString body = pem;
    body.remove(QStringLiteral("-----BEGIN PUBLIC KEY-----"));
    body.remove(QStringLiteral("-----END PUBLIC KEY-----"));
    body.remove(QStringLiteral("-----BEGIN RSA PUBLIC KEY-----"));
    body.remove(QStringLiteral("-----END RSA PUBLIC KEY-----"));
    body = body.simplified();
    body.remove(QLatin1Char(' '));

    const QByteArray der = QByteArray::fromBase64(body.toLatin1());
    if (der.isEmpty()) return QString();

    // --- 解析 DER 取模数 n 与指数 e ---
    QVector<quint32> n, e;
    int pos = 0;
    DerNode outer;
    if (!readDer(der, pos, outer)) return QString(); // SEQUENCE

    // 尝试逐层下钻：SEQUENCE { SEQUENCE(algid), BITSTRING { SEQUENCE { INTEGER n, INTEGER e } } }
    // 用递归搜索：找到第一个内含两个 INTEGER 的 SEQUENCE
    std::function<bool(const QByteArray &, int, int)> findModulus;
    findModulus = [&](const QByteArray &buf, int start, int end) -> bool {
        int p = start;
        while (p < end) {
            DerNode node;
            const int before = p;
            if (!readDer(buf, p, node)) return false;
            if (node.start + node.length > end) return false;
            if (node.tag == 0x02) { // INTEGER
                QByteArray intBytes = buf.mid(node.start, node.length);
                if (!intBytes.isEmpty() && static_cast<quint8>(intBytes[0]) == 0x00)
                    intBytes.remove(0, 1); // 去掉正数的前导零
                if (n.isEmpty()) n = fromBigEndian(intBytes);
                else if (e.isEmpty()) { e = fromBigEndian(intBytes); return true; }
                continue;
            }
            if (node.tag == 0x30 || node.tag == 0x03) {
                int innerStart = node.start;
                if (node.tag == 0x03) innerStart += 1; // BIT STRING 跳过 unused-bits 字节
                if (findModulus(buf, innerStart, node.start + node.length)) return true;
            }
            if (p <= before) break;
        }
        return !n.isEmpty() && !e.isEmpty();
    };

    if (!findModulus(der, outer.start, outer.start + outer.length))
        return QString();
    if (n.isEmpty() || e.isEmpty()) return QString();

    // --- m = 明文对应的整数（大端），要求 m < n ---
    QVector<quint32> m = fromBigEndian(data);
    if (compare(m, n) >= 0) return QString();

    // --- c = m^e mod n ---
    const QVector<quint32> c = modPow(m, e, n);
    const QByteArray cBytes = toBigEndian(c, (bitLength(n) + 7) / 8);
    return QString::fromUtf8(cBytes.toHex());
}

// ---------------------------------------------------------------------------
// RSA-OAEP(SHA-1)——洛雪同步鉴权回包用（Node RSA_PKCS1_OAEP_PADDING 默认 SHA-1）
// ---------------------------------------------------------------------------

namespace {

/// 递归收集 DER 中所有 INTEGER 的字节（大端、可能含前导零）
void collectIntegers(const QByteArray &der, int start, int end, QList<QByteArray> &out, int limit)
{
    int p = start;
    while (p < end && out.size() < limit) {
        DerNode node;
        const int before = p;
        if (!readDer(der, p, node)) return;
        if (node.start + node.length > end) return;
        if (node.tag == 0x02) {
            out.append(der.mid(node.start, node.length));
        } else if (node.tag == 0x30 || node.tag == 0x03 || node.tag == 0x04) {
            int innerStart = node.start;
            if (node.tag == 0x03) innerStart += 1; // BIT STRING: 跳过 unused-bits
            collectIntegers(der, innerStart, node.start + node.length, out, limit);
        }
        if (p <= before) return;
    }
}

QByteArray pemBody(const QString &pem, const QStringList &markers)
{
    QString body = pem;
    for (const QString &m : markers) body.remove(m);
    body = body.simplified();
    body.remove(QLatin1Char(' '));
    return QByteArray::fromBase64(body.toLatin1());
}

/// MGF1(SHA-1)
QByteArray mgf1Sha1(const QByteArray &seed, int length)
{
    QByteArray out;
    quint32 counter = 0;
    while (out.size() < length) {
        char cb[4] = {
            char((counter >> 24) & 0xFF), char((counter >> 16) & 0xFF),
            char((counter >> 8) & 0xFF), char(counter & 0xFF)
        };
        out += QCryptographicHash::hash(seed + QByteArray(cb, 4), QCryptographicHash::Sha1);
        ++counter;
    }
    return out.left(length);
}

QByteArray xorBytes(const QByteArray &a, const QByteArray &b)
{
    QByteArray r(a.size(), '\0');
    for (int i = 0; i < r.size(); ++i) r[i] = char(a[i] ^ b[i]);
    return r;
}

QByteArray dropLeadingZero(QByteArray b)
{
    if (!b.isEmpty() && static_cast<quint8>(b[0]) == 0) b.remove(0, 1);
    return b;
}

} // namespace

QByteArray rsaOaepEncrypt(const QByteArray &data, const QString &pem) {
    const QByteArray der = pemBody(pem, {
        QStringLiteral("-----BEGIN PUBLIC KEY-----"),
        QStringLiteral("-----END PUBLIC KEY-----"),
        QStringLiteral("-----BEGIN RSA PUBLIC KEY-----"),
        QStringLiteral("-----END RSA PUBLIC KEY-----"),
    });
    if (der.isEmpty()) return {};

    QList<QByteArray> ints;
    DerNode outer;
    int pos = 0;
    if (!readDer(der, pos, outer)) return {};
    collectIntegers(der, outer.start, outer.start + outer.length, ints, 2);
    if (ints.size() < 2) return {};

    const QVector<quint32> n = fromBigEndian(dropLeadingZero(ints[0]));
    const QVector<quint32> e = fromBigEndian(dropLeadingZero(ints[1]));
    const int k = (bitLength(n) + 7) / 8;
    const int hLen = 20; // SHA-1
    if (data.size() > k - 2 * hLen - 2) return {};

    // EM = 0x00 || maskedSeed || maskedDB
    const QByteArray lHash = QCryptographicHash::hash(QByteArray(), QCryptographicHash::Sha1);
    QByteArray db = lHash;
    db.append(QByteArray(k - 2 * hLen - 2 - data.size(), '\0')); // PS
    db.append(char(0x01));
    db.append(data);
    Q_ASSERT(db.size() == k - hLen - 1);

    QByteArray seed(hLen, '\0');
    auto *rng = QRandomGenerator::global();
    for (int i = 0; i < hLen; ++i) seed[i] = char(rng->bounded(256));

    const QByteArray maskedDB = xorBytes(db, mgf1Sha1(seed, k - hLen - 1));
    const QByteArray maskedSeed = xorBytes(seed, mgf1Sha1(maskedDB, hLen));
    QByteArray em;
    em.append(char(0));
    em += maskedSeed + maskedDB;
    Q_ASSERT(em.size() == k);

    const QVector<quint32> m = fromBigEndian(em);
    if (compare(m, n) >= 0) return {};
    return toBigEndian(modPow(m, e, n), k);
}

QByteArray rsaOaepDecrypt(const QByteArray &cipher, const QString &pem) {
    const QByteArray der = pemBody(pem, {
        QStringLiteral("-----BEGIN RSA PRIVATE KEY-----"),
        QStringLiteral("-----END RSA PRIVATE KEY-----"),
        QStringLiteral("-----BEGIN PRIVATE KEY-----"),
        QStringLiteral("-----END PRIVATE KEY-----"),
    });
    if (der.isEmpty()) return {};

    // PKCS#1 RSAPrivateKey: SEQUENCE { version, n, e, d, p, q, ... }
    QList<QByteArray> ints;
    DerNode outer;
    int pos = 0;
    if (!readDer(der, pos, outer)) return {};
    collectIntegers(der, outer.start, outer.start + outer.length, ints, 64);
    QVector<quint32> n, d;
    for (const QByteArray &b : ints) {
        const QByteArray t = dropLeadingZero(b);
        if (t.size() >= 128) { if (n.isEmpty()) n = fromBigEndian(t); else if (d.isEmpty()) d = fromBigEndian(t); }
    }
    if (n.isEmpty() || d.isEmpty()) return {};
    const int k = (bitLength(n) + 7) / 8;
    const int hLen = 20;
    if (cipher.size() != k || k < 2 * hLen + 2) return {};

    const QVector<quint32> c = fromBigEndian(cipher);
    if (compare(c, n) >= 0) return {};
    const QByteArray em = toBigEndian(modPow(c, d, n), k);

    const QByteArray maskedSeed = em.mid(1, hLen);
    const QByteArray maskedDB = em.mid(1 + hLen);
    const QByteArray seed = xorBytes(maskedSeed, mgf1Sha1(maskedDB, hLen));
    const QByteArray db = xorBytes(maskedDB, mgf1Sha1(seed, k - hLen - 1));
    const QByteArray lHash = QCryptographicHash::hash(QByteArray(), QCryptographicHash::Sha1);
    if (db.left(hLen) != lHash || em.at(0) != '\0') return {};
    int i = hLen;
    while (i < db.size() && db.at(i) == '\0') ++i;
    if (i >= db.size() || static_cast<quint8>(db.at(i)) != 0x01) return {};
    return db.mid(i + 1);
}

// ===========================================================================
// Base64 / 随机
// ===========================================================================

QByteArray base64Encode(const QByteArray &data) { return data.toBase64(); }

QByteArray base64Decode(const QByteArray &data)
{
    return QByteArray::fromBase64(data);
}

QString base64EncodeStr(const QByteArray &data)
{
    return QString::fromUtf8(data.toBase64());
}

QString randomBase62Key(int length)
{
    static const QString chars =
        QStringLiteral("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
    QString out;
    auto *rng = QRandomGenerator::global();
    for (int i = 0; i < length; ++i)
        out.append(chars.at(static_cast<int>(rng->bounded(62))));
    return out;
}

QString randomHex(int bytes)
{
    QByteArray out;
    auto *rng = QRandomGenerator::global();
    for (int i = 0; i < bytes; ++i)
        out.append(static_cast<char>(rng->bounded(256)));
    return QString::fromUtf8(out.toHex());
}

} // namespace Crypto
} // namespace Muyun
