#include "Theme.h"

namespace Muyun {

namespace {
/// 给基色套一个 alpha（玻璃模式下所有"面"都半透明，模糊才透得上来）
QColor veil(const QColor &base, double alpha)
{
    QColor c = base;
    c.setAlphaF(alpha);
    return c;
}
} // namespace

Theme::Theme(QObject *parent) : QObject(parent) {}

QColor Theme::bgColor() const
{
    // 深色底 #0f1115 / 浅色底 #f6f7f9
    // 玻璃模式：**完全透明**。厚底由窗口那层 glassBaseColor 统一提供；页面若再套 62%
    // 的 bgColor 会和它叠成 0.85，多层嵌套后彻底不透光（本机实测：模糊全被压死）。
    return m_glass ? veil(m_dark ? QColor("#0f1115") : QColor("#f6f7f9"), 0.0)
                   : (m_dark ? QColor("#0f1115") : QColor("#f6f7f9"));
}

QColor Theme::panelColor() const
{
    // 标题栏/侧栏/顶栏/播放条这类"外框面"：只给薄薄一层染色，别糊住模糊
    return m_glass ? veil(m_dark ? QColor("#161920") : QColor("#ffffff"), 0.14)
                   : (m_dark ? QColor("#161920") : QColor("#ffffff"));
}

QColor Theme::cardColor() const
{
    // 卡片/列表行/面板：薄染色。多层嵌套（page → 分组 → 行）叠起来也就 0.4 上下，
    // 还能透出模糊；给到 0.5 的话三层就 0.87，视觉上和纯色没区别了。
    return m_glass ? veil(m_dark ? QColor("#1e222b") : QColor("#ffffff"), 0.24)
                   : (m_dark ? QColor("#1e222b") : QColor("#ffffff"));
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
    // 玻璃模式换成"白色描边"（半透明面之间用亮度分隔，比深色描边更像玻璃）
    if (m_glass) return veil(QColor("#ffffff"), m_dark ? 0.09 : 0.55);
    return m_dark ? QColor("#262b36") : QColor("#e3e6eb");
}

QColor Theme::hoverColor() const
{
    // 悬停染色一律"薄"：它要压在面板/卡片之上，厚了就见不着模糊了
    return m_glass ? veil(m_dark ? QColor("#ffffff") : QColor("#000000"), m_dark ? 0.07 : 0.05)
                   : (m_dark ? QColor("#232833") : QColor("#eef0f3"));
}

QColor Theme::activeColor() const
{
    // 选中态用"白上加白"（深色）/ 深色淡压（浅色），跟卡片叠出来是微微发亮的底
    return m_glass ? veil(m_dark ? QColor("#ffffff") : QColor("#000000"), m_dark ? 0.12 : 0.07)
                   : (m_dark ? QColor("#2b3241") : QColor("#e6e9ef"));
}

QColor Theme::hoverCardColor() const
{
    return m_glass ? veil(m_dark ? QColor("#ffffff") : QColor("#000000"), m_dark ? 0.06 : 0.04)
                   : (m_dark ? QColor("#262c38") : QColor("#f2f4f7"));
}

QColor Theme::windowColor() const
{
    // 玻璃模式：窗口本身全透明，模糊层 + QML 渐变叠加出观感
    return m_glass ? QColor(Qt::transparent) : bgColor();
}

QColor Theme::glassBaseColor() const
{
    if (!m_glass) return bgColor();
    // 整窗唯一那层"厚"底：alpha 由设置里的「主程序窗口 · 透明度」滑块驱动（0.62 默认）。
    // 想更透玻璃就往下调（0.5 会更糊更透，低于 0.45 文字就开始难读）。
    return veil(m_dark ? QColor("#0f1115") : QColor("#fafbfd"), m_mainAlpha);
}

QColor Theme::settingsBaseColor() const
{
    // 设置面板的厚底：面板背后还压着主页内容，必须比主窗更实，否则字和列表糊成一片。
    // 默认 0.90；滑块让用户自己往"透"的方向调。
    if (!m_glass) return panelColor();
    return veil(m_dark ? QColor("#0f1115") : QColor("#fafbfd"), m_settingsAlpha);
}

QColor Theme::panelSolidColor() const
{
    // 设置面板把「毛玻璃」关掉时用的实底：不透明，就是非玻璃模式那档 panelColor。
    // 玻璃模式下 panelColor 只有 0.14 的薄染色，拿它当面板底会透成一片（看不清字）。
    return m_dark ? QColor("#161920") : QColor("#ffffff");
}

void Theme::setMainAlpha(double a)
{
    a = qBound(0.0, a, 1.0);
    if (qFuzzyCompare(m_mainAlpha, a)) return;
    m_mainAlpha = a;
    emit changed();
}

void Theme::setSettingsAlpha(double a)
{
    a = qBound(0.0, a, 1.0);
    if (qFuzzyCompare(m_settingsAlpha, a)) return;
    m_settingsAlpha = a;
    emit changed();
}

QColor Theme::glassSheen() const
{
    if (!m_glass) return bgColor();
    // 顶部那层微光：**只把颜色提亮，alpha 必须和整窗厚底同档**（都是 mainAlpha）。
    // 曾经这里返回「白色 alpha 10」——窗口最上沿就只剩 4% 不透明度，
    // 桌面/后窗从这一条透上来，窗口顶上就出现一圈"白框"（v1.4.0 现场缺陷）。
    return veil(m_dark ? QColor("#1b2029") : QColor("#ffffff"), m_mainAlpha);
}

QColor Theme::bgDeepColor() const
{
    if (!m_glass) return bgColor();
    // 底部压暗：让模糊背景有"上亮下暗"的纵深。同理，只压颜色不压 alpha，
    // 否则窗口下沿也会变成一条半透明的边（同白框一样的问题）。
    return veil(m_dark ? QColor("#05070b") : QColor("#eef1f6"), m_mainAlpha);
}

void Theme::setDark(bool dark)
{
    if (m_dark == dark) return;
    m_dark = dark;
    emit changed();
}

void Theme::setGlass(bool on)
{
    if (m_glass == on) return;
    m_glass = on;
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
