#pragma once

#include <QObject>
#include <QColor>

namespace Muyun {

/**
 * @brief 界面主题（暴露给 QML）
 *
 * 提供配色与平台标签色，并支持深色/浅色切换。
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
    Q_PROPERTY(bool dark READ dark WRITE setDark NOTIFY changed)

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
    bool dark() const { return m_dark; }
    void setDark(bool dark);

    /// 平台标签底色（按平台区分）
    Q_INVOKABLE QColor platformColor(const QString &platformId) const;

signals:
    void changed();

private:
    bool m_dark = true;
};

} // namespace Muyun
