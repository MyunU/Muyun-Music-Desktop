import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 应用自身「发现新版本」弹窗（开源方案 A：清单来自 GitHub，见 core/update/UpdateChecker）
// 三个出口：前往下载（打开发布页/安装包直链）、稍后（本次不打扰）、不再提醒（按版本号记忽略，
// 该版本永不再自动弹；远端出现比它更新的版本时会重新提醒）。
Popup {
    id: dlg
    objectName: "appUpdateDialog"
    modal: true
    dim: true
    width: 480
    padding: 0
    anchors.centerIn: parent
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    property string version: ""
    property string notes: ""

    function showFor(v, n) {
        dlg.version = v
        dlg.notes = (n && n.length > 0) ? n : "（该版本没有提供更新说明）"
        dlg.open()
    }

    background: Rectangle {
        color: theme.cardColor
        radius: 14
        border.color: theme.borderColor
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---------------- 头部 ----------------
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 100
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: theme.borderColor
            }
            RowLayout {
                anchors.fill: parent
                anchors.margins: 20
                spacing: 14
                Rectangle {
                    Layout.preferredWidth: 44
                    Layout.preferredHeight: 44
                    radius: 12
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: theme.accentColor }
                        GradientStop { position: 1.0; color: Qt.darker(theme.accentColor, 1.35) }
                    }
                    Icon {
                        name: "download"
                        anchors.centerIn: parent
                        iconSize: 22
                        iconColor: "#ffffff"
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.rightMargin: 24
                    Layout.alignment: Qt.AlignVCenter
                    spacing: 3
                    Text {
                        text: "发现新版本"
                        color: theme.accentColor
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                    }
                    Text {
                        objectName: "updateTitleText"
                        Layout.fillWidth: true
                        text: "暮云音乐 v" + dlg.version
                        color: theme.textColor
                        font.pixelSize: 17
                        font.weight: Font.Bold
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        text: "当前版本 v" + appVersion
                        color: theme.subTextColor
                        font.pixelSize: 12
                    }
                }
            }
            // 右上角关闭（= 稍后，不写忽略）
            Item {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: 10
                width: 30
                height: 30
                Rectangle {
                    anchors.fill: parent
                    radius: 15
                    color: closeHover.containsMouse ? theme.hoverColor : "transparent"
                }
                Icon {
                    name: "window-close"
                    anchors.centerIn: parent
                    iconSize: 16
                    iconColor: theme.subTextColor
                }
                MouseArea {
                    id: closeHover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: dlg.close()
                }
            }
        }

        // ---------------- 更新说明 ----------------
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.rightMargin: 20
            Layout.topMargin: 16
            Layout.bottomMargin: 16
            spacing: 8
            Text {
                text: "更新内容"
                color: theme.textColor
                font.pixelSize: 13
                font.weight: Font.DemiBold
            }
            Item {
                Layout.fillWidth: true
                // 说明很长时内部滚动，弹窗高度不失控
                Layout.preferredHeight: Math.min(notesText.implicitHeight, 168)
                Flickable {
                    anchors.fill: parent
                    contentHeight: notesText.height
                    boundsBehavior: Flickable.StopAtBounds
                    flickableDirection: Flickable.VerticalFlick
                    clip: true
                    Text {
                        id: notesText
                        objectName: "updateNotesText"
                        width: parent.width
                        text: dlg.notes
                        color: theme.subTextColor
                        font.pixelSize: 13
                        lineHeight: 1.55
                        wrapMode: Text.Wrap
                    }
                }
            }
        }

        // ---------------- 底部操作栏 ----------------
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 70
            Rectangle {
                anchors.top: parent.top
                width: parent.width
                height: 1
                color: theme.borderColor
            }
            RowLayout {
                anchors.centerIn: parent
                spacing: 10

                // 不再提醒（按版本号记忽略）
                Item {
                    implicitHeight: 34
                    width: ignoreText.width + 28
                    Text {
                        id: ignoreText
                        anchors.centerIn: parent
                        text: "不再提醒"
                        color: ignoreHover.containsMouse ? theme.textColor : theme.subTextColor
                        font.pixelSize: 13
                    }
                    MouseArea {
                        objectName: "updateIgnoreBtn"
                        id: ignoreHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            updater.ignoreLatestVersion()
                            dlg.close()
                        }
                    }
                }

                // 稍后（下次检查若仍有新版会再提；不写忽略）
                Item {
                    implicitHeight: 34
                    width: laterText.width + 28
                    Text {
                        id: laterText
                        anchors.centerIn: parent
                        text: "稍后"
                        color: laterHover.containsMouse ? theme.textColor : theme.subTextColor
                        font.pixelSize: 13
                    }
                    MouseArea {
                        objectName: "updateLaterBtn"
                        id: laterHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: dlg.close()
                    }
                }

                // 前往下载（优先安装包直链，没有就开发布页）
                Rectangle {
                    id: goBtn
                    radius: 8
                    color: goHover.pressed ? Qt.darker(theme.accentColor, 1.12)
                          : goHover.containsMouse ? Qt.lighter(theme.accentColor, 1.08)
                          : theme.accentColor
                    implicitHeight: 34
                    implicitWidth: goRow.implicitWidth + 28
                    RowLayout {
                        id: goRow
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 14
                        anchors.rightMargin: 14
                        spacing: 6
                        Text {
                            text: "前往下载"
                            color: "#ffffff"
                            font.pixelSize: 13
                        }
                        Icon {
                            name: "link"
                            iconSize: 14
                            iconColor: "#ffffff"
                        }
                    }
                    MouseArea {
                        objectName: "updateDownloadBtn"
                        id: goHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            updater.openDownloadPage()
                            dlg.close()
                        }
                    }
                }
            }
        }
    }
}
