#include "TrayController.h"
#include "PlayerController.h"

#include <QMenu>
#include <QAction>
#include <QApplication>
#include <QIcon>

namespace Muyun {

TrayController::TrayController(PlayerController *player, QObject *parent)
    : QObject(parent), m_player(player)
{
    m_menu = new QMenu();

    // 播放/暂停：文案随播放状态实时同步
    m_playAct = m_menu->addAction(QStringLiteral("播放"));
    updatePlayActionText();
    if (m_player) {
        connect(m_player, &PlayerController::isPlayingChanged,
                this, &TrayController::updatePlayActionText);
        connect(m_playAct, &QAction::triggered,
                m_player, &PlayerController::togglePlay);
    }

    QAction *showAct = m_menu->addAction(QStringLiteral("显示主界面"));
    connect(showAct, &QAction::triggered, this, &TrayController::showRequested);

    m_menu->addSeparator();

    QAction *quitAct = m_menu->addAction(QStringLiteral("退出"));
    // 播放状态均即时持久化（DocumentStore 即时写入），直接退出安全
    connect(quitAct, &QAction::triggered, qApp, &QApplication::quit);

    m_tray = new QSystemTrayIcon(QIcon(QStringLiteral(":/resources/app.svg")), this);
    m_tray->setToolTip(QStringLiteral("暮云音乐"));
    m_tray->setContextMenu(m_menu);
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger ||
                    reason == QSystemTrayIcon::DoubleClick)
                    emit showRequested();
            });
    m_tray->show();
}

TrayController::~TrayController()
{
    m_tray->hide();
    delete m_menu;
}

void TrayController::updatePlayActionText()
{
    if (!m_playAct || !m_player) return;
    m_playAct->setText(m_player->isPlaying() ? QStringLiteral("暂停")
                                             : QStringLiteral("播放"));
}

void TrayController::minimizeToTray()
{
    if (!m_balloonShown) {
        m_tray->showMessage(QStringLiteral("暮云音乐"),
                            QStringLiteral("已最小化到系统托盘，播放继续"),
                            QSystemTrayIcon::Information, 2000);
        m_balloonShown = true;
    }
}

} // namespace Muyun
