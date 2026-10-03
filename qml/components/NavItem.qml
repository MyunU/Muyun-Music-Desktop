import QtQuick
import QtQuick.Layouts

// 侧边栏导航项
Rectangle {
    id: item
    property string icon: ""
    property string label: ""
    property bool active: false
    property int count: -1
    property bool deletable: false   // 显示 hover 删除按钮（歌单用）
    property bool hovered: false     // 整项 hover 状态（独立维护，避免按钮出现即消失的闪烁）
    signal clicked()
    signal deleteRequested()

    Layout.fillWidth: true
    Layout.preferredHeight: 38
    radius: 8
    color: item.active ? theme.activeColor
                       : (mouse.containsMouse ? theme.hoverColor : "transparent")

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 10
        spacing: 10

        Icon {
            name: item.icon
            iconSize: 17
            iconColor: item.active ? theme.accentColor : theme.subTextColor
        }
        Text {
            Layout.fillWidth: true
            text: item.label
            color: item.active ? theme.accentColor : theme.textColor
            font.pixelSize: 13
            elide: Text.ElideRight
        }
        Text {
            visible: item.count > 0
            text: item.count
            color: theme.subTextColor
            font.pixelSize: 11
        }
    }

    Rectangle {
        visible: item.active
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: 3
        height: 18
        radius: 2
        color: theme.accentColor
    }

    // hover 删除按钮（歌单）
    Rectangle {
        id: delBtn
        visible: item.deletable && item.hovered
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        width: 22
        height: 22
        radius: 11
        z: 2
        color: delHover.containsMouse ? theme.accentColor : theme.hoverColor
        Icon {
            anchors.centerIn: parent
            name: "window-close"
            iconSize: 11
            iconColor: delHover.containsMouse ? "white" : theme.subTextColor
        }
        MouseArea {
            id: delHover
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onEntered: item.hovered = true
            onClicked: item.deleteRequested()
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        onEntered: item.hovered = true
        onExited: {
            // 鼠标彻底移出整项才隐藏（移入删除按钮时 delHover.onEntered 会保持 true）
            if (!delHover.containsMouse) item.hovered = false
        }
        onClicked: item.clicked()
    }
}
