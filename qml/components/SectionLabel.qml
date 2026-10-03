import QtQuick
import QtQuick.Layouts

// 侧边栏分组标题（可带新增按钮）
Item {
    id: sec
    property string text: ""
    signal addClicked()

    Layout.fillWidth: true
    Layout.preferredHeight: 30
    Layout.topMargin: 8

    Text {
        anchors.left: parent.left
        anchors.leftMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        text: sec.text
        color: theme.subTextColor
        font.pixelSize: 11
    }

    Icon {
        id: plusIcon
        anchors.right: parent.right
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        name: "plus"
        iconSize: 14
        iconColor: hoverArea.containsMouse ? theme.accentColor : theme.subTextColor
    }

    MouseArea {
        id: hoverArea
        anchors.fill: plusIcon
        anchors.margins: -6
        hoverEnabled: true
        onClicked: sec.addClicked()
    }
}
