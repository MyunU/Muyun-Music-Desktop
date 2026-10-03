import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 顶栏圆形图标按钮
Item {
    id: btn
    property string glyph: ""
    property string tip: ""
    signal clicked()

    Layout.preferredWidth: 34
    Layout.preferredHeight: 34

    Rectangle {
        anchors.fill: parent
        radius: 17
        color: mouse.containsMouse ? theme.hoverColor : "transparent"
    }

    Icon {
        anchors.centerIn: parent
        name: btn.glyph
        iconSize: 18
        iconColor: mouse.containsMouse ? theme.textColor : theme.subTextColor
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: btn.clicked()
    }

    ToolTip.visible: mouse.containsMouse && btn.tip.length > 0
    ToolTip.text: btn.tip
    ToolTip.delay: 500
}
