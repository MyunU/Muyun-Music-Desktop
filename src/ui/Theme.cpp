#include "Theme.h"

namespace Muyun {

Theme::Theme(QObject *parent) : QObject(parent) {}

QColor Theme::bgColor() const
{
    return m_dark ? QColor("#0f1115") : QColor("#f6f7f9");
}

QColor Theme::panelColor() const
{
    return m_dark ? QColor("#161920") : QColor("#ffffff");
}

QColor Theme::cardColor() const
{
    return m_dark ? QColor("#1e222b") : QColor("#ffffff");
}

QColor Theme::textColor() const
{
    return m_dark ? QColor("#e8ebf2") : QColor("#1a1d24");
}

QColor Theme::subTextColor() const
{
    return m_dark ? QColor("#8b93a7") : QColor("#5f6672");
}

QColor Theme::accentColor() const
{
    return QColor("#ec4141");
}

QColor Theme::borderColor() const
{
    return m_dark ? QColor("#262b36") : QColor("#e3e6eb");
}

QColor Theme::hoverColor() const
{
    return m_dark ? QColor("#232833") : QColor("#eef0f3");
}

QColor Theme::activeColor() const
{
    return m_dark ? QColor("#2b3241") : QColor("#e6e9ef");
}

QColor Theme::hoverCardColor() const
{
    return m_dark ? QColor("#262c38") : QColor("#f2f4f7");
}

void Theme::setDark(bool dark)
{
    if (m_dark == dark) return;
    m_dark = dark;
    emit changed();
}

QColor Theme::platformColor(const QString &platformId) const
{
    if (platformId == QStringLiteral("netease")) return QColor("#c5393b");
    if (platformId == QStringLiteral("qq"))      return QColor("#2c9e4b");
    if (platformId == QStringLiteral("kugou"))   return QColor("#2b7fd4");
    if (platformId == QStringLiteral("kuwo"))    return QColor("#e0a000");
    if (platformId == QStringLiteral("migu"))    return QColor("#8a5cd6");
    if (platformId == QStringLiteral("local"))   return QColor("#5b6472");
    return QColor("#5b6472");
}

} // namespace Muyun
