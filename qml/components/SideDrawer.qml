import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 通用右侧抽屉（侧滑面板）：遮罩 + 右侧滑入 + 标题栏 + 自定义内容
// 用法：SideDrawer { id: xxx; title: "播放队列"; drawerWidth: 340; ... 内容 ... }
Item {
    id: drawer
    anchors.fill: parent
    z: 300

    property bool opened: false
    property string title: ""
    property int drawerWidth: 340
    /// 内容区（默认子项都进这里）
    default property alias drawerContent: contentHolder.data

    signal closed()

    // 未打开且动画结束时不参与命中测试，避免遮住下层
    visible: opened || slideAnim.running || fadeAnim.running

    function open() { opened = true }
    function close() { opened = false; closed() }
    function toggle() { opened ? close() : open() }

    onOpenedChanged: {
        if (opened) {
            fadeAnim.to = 1; fadeAnim.start()
            slideAnim.to = drawer.width - drawerWidth; slideAnim.start()
        } else {
            fadeAnim.to = 0; fadeAnim.start()
            slideAnim.to = drawer.width; slideAnim.start()
        }
    }

    Component.onCompleted: panel.x = drawer.width   // 初始藏在右边缘外

    PropertyAnimation {
        id: fadeAnim
        target: mask
        property: "opacity"
        duration: 200
        to: 0
    }

    PropertyAnimation {
        id: slideAnim
        target: panel
        property: "x"
        duration: 220
        easing.type: Easing.OutCubic
        to: drawer.width
    }

    // 半透明遮罩（点击关闭）
    Rectangle {
        id: mask
        anchors.fill: parent
        color: "#66000000"
        opacity: 0
        MouseArea {
            anchors.fill: parent
            onClicked: drawer.close()
        }
    }

    // 面板本体（右侧滑入）
    Rectangle {
        id: panel
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: drawerWidth
        x: drawer.width          // 初始在屏幕外（纯 x 定位，不用 anchors.right 以免冲突）
        color: theme.panelColor
        border.color: theme.borderColor
        border.width: 1

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // 标题栏
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 52
                color: "transparent"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 18
                    anchors.rightMargin: 10
                    spacing: 8

                    Text {
                        text: drawer.title
                        color: theme.textColor
                        font.pixelSize: 15
                        font.bold: true
                    }
                    Item { Layout.fillWidth: true }
                    IconButton {
                        name: "window-close"
                        iconSize: 13
                        Layout.preferredWidth: 30
                        Layout.preferredHeight: 30
                        tip: "关闭"
                        onClicked: drawer.close()
                    }
                }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: theme.borderColor }

            // 内容区
            Item {
                id: contentHolder
                Layout.fillWidth: true
                Layout.fillHeight: true
            }
        }
    }
}
