#pragma once

#include <QUrl>
#include <QString>

namespace Muyun {

/// 根据音频 URL 域名推断防盗链 Referer（多数 CDN 校验 Referer，缺失会 403）。
/// 播放与下载共用；返回空串表示无需特殊 referer。
inline QString refererForAudioUrl(const QString &url)
{
    const QString host = QUrl(url).host();
    if (host.contains(QStringLiteral("qqmusic")))
        return QStringLiteral("https://y.qq.com/");
    if (host.contains(QStringLiteral("kuwo")))
        return QStringLiteral("http://www.kuwo.cn/");
    if (host.contains(QStringLiteral("126.net")) || host.contains(QStringLiteral("163.com")))
        return QStringLiteral("https://music.163.com/");
    if (host.contains(QStringLiteral("kugou")))
        return QStringLiteral("https://www.kugou.com/");
    if (host.contains(QStringLiteral("migu")))
        return QStringLiteral("https://music.migu.cn/");
    return QString();
}

} // namespace Muyun
