#pragma once

#include <QObject>
#include <QSystemTrayIcon>

class QMenu;
class QAction;

namespace Muyun {

class PlayerController;

/**
 * @brief 系统托盘（音乐软件后台运行标配）
 *
 * - 托盘图标 + 右键菜单（播放/暂停 / 显示主界面 / 退出）
 * - 双击托盘图标恢复主窗口
 * - "退出"直接结束应用（播放状态均即时持久化，无需额外保存）
 */
class TrayController : public QObject
{
    Q_OBJECT
public:
    explicit TrayController(PlayerController *player, QObject *parent = nullptr);
    ~TrayController() override;

    /// 最小化到托盘（主窗口 hide 后调用，首次会弹气泡提示）
    Q_INVOKABLE void minimizeToTray();

signals:
    /// 用户请求恢复主窗口（双击托盘 / 菜单"显示主界面"）
    void showRequested();

private:
    void updatePlayActionText();

    PlayerController *m_player = nullptr;
    QSystemTrayIcon *m_tray = nullptr;
    QMenu *m_menu = nullptr;
    QAction *m_playAct = nullptr;
    bool m_balloonShown = false;
};

} // namespace Muyun
