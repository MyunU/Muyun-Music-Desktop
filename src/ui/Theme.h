#pragma once

#include <QObject>
#include <QColor>

namespace Muyun {

/**
 * @brief 界面主题（暴露给 QML）
 *
 * 提供配色与平台标签色，并支持深色/浅色切换。
 *
 * v1.4.0 起多了「毛玻璃」模式（glass）：窗口背后由 DWM 亚克力做高斯模糊，
 * 因此所有"面"的颜色都要带 alpha，模糊才能透上来。QML 里的容器一律用
 * windowColor / bgColor / panelColor / cardColor 这几档，不要在 QML 里写死
 * rgba —— 开关玻璃时才能整体跟着切。
 */
class Theme : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QColor bgColor READ bgColor NOTIFY changed)
    Q_PROPERTY(QColor panelColor READ panelColor NOTIFY changed)
    Q_PROPERTY(QColor cardColor READ cardColor NOTIFY changed)
    Q_PROPERTY(QColor textColor READ textColor NOTIFY changed)
    Q_PROPERTY(QColor subTextColor READ subTextColor NOTIFY changed)
    Q_PROPERTY(QColor accentColor READ accentColor NOTIFY changed)
    Q_PROPERTY(QColor borderColor READ borderColor NOTIFY changed)
    Q_PROPERTY(QColor hoverColor READ hoverColor NOTIFY changed)
    Q_PROPERTY(QColor activeColor READ activeColor NOTIFY changed)
    Q_PROPERTY(QColor hoverCardColor READ hoverCardColor NOTIFY changed)
    /// 窗口底色：玻璃开 = 全透明（模糊层直接透上来），关 = 不透明 bgColor
    Q_PROPERTY(QColor windowColor READ windowColor NOTIFY changed)
    /// 窗口底渐变的中段主色（玻璃模式唯一那层"厚"底；page 们不要再用 bgColor 叠它）
    Q_PROPERTY(QColor glassBaseColor READ glassBaseColor NOTIFY changed)
    /// 主程序窗口那层厚底的不透明度（0..1，来自设置里的滑块）
    Q_PROPERTY(double mainAlpha READ mainAlpha WRITE setMainAlpha NOTIFY changed)
    /// 设置界面那层厚底的不透明度（0..1，来自设置里的滑块）
    Q_PROPERTY(double settingsAlpha READ settingsAlpha WRITE setSettingsAlpha NOTIFY changed)
    /// 设置面板自己的厚底：要比主窗更实（背后还有主页内容，太透字就糊了）
    Q_PROPERTY(QColor settingsBaseColor READ settingsBaseColor NOTIFY changed)
    /// 设置面板关掉毛玻璃时的实底（不透明，非玻璃模式那档 panelColor）
    Q_PROPERTY(QColor panelSolidColor READ panelSolidColor NOTIFY changed)
    /// 窗口底渐变的顶部提亮色（玻璃模式的"光"）
    /// ⚠ 提亮必须靠 **色彩变亮** 实现，alpha 要和 glassBaseColor 同档：
    /// 早期这里返回「白色 alpha 10」，窗口最上沿就只有 4% 不透明度，
    /// 桌面/后窗直接透上来一圈"白框"（v1.4.0 现场缺陷，别再改回去）
    Q_PROPERTY(QColor glassSheen READ glassSheen NOTIFY changed)
    /// 窗口底渐变的底部压暗色（让模糊背景有层次，不是一片糊）
    Q_PROPERTY(QColor bgDeepColor READ bgDeepColor NOTIFY changed)
    Q_PROPERTY(bool dark READ dark WRITE setDark NOTIFY changed)
    /// 毛玻璃模式（由设置里的开关驱动；关掉后所有颜色回不透明）
    Q_PROPERTY(bool glass READ glass WRITE setGlass NOTIFY changed)

public:
    explicit Theme(QObject *parent = nullptr);

    QColor bgColor() const;
    QColor panelColor() const;
    QColor cardColor() const;
    QColor textColor() const;
    QColor subTextColor() const;
    QColor accentColor() const;
    QColor borderColor() const;
    QColor hoverColor() const;
    QColor activeColor() const;
    QColor hoverCardColor() const;
    QColor windowColor() const;
    QColor glassBaseColor() const;
    QColor glassSheen() const;
    QColor bgDeepColor() const;
    QColor settingsBaseColor() const;
    QColor panelSolidColor() const;
    double mainAlpha() const { return m_mainAlpha; }
    void setMainAlpha(double a);
    double settingsAlpha() const { return m_settingsAlpha; }
    void setSettingsAlpha(double a);
    bool dark() const { return m_dark; }
    void setDark(bool dark);
    bool glass() const { return m_glass; }
    void setGlass(bool on);

    /// 平台标签底色（按平台区分）
    Q_INVOKABLE QColor platformColor(const QString &platformId) const;

signals:
    void changed();

private:
    bool m_dark = true;
    bool m_glass = false;
    /// 主窗厚底 / 设置面板厚底的不透明度（0..1）
    double m_mainAlpha = 0.62;
    double m_settingsAlpha = 0.90;
};

} // namespace Muyun
