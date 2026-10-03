import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 图标按钮（悬停圆形高亮 + 提示）
Item {
    id: btn
    property string name: ""
    property color iconColor: theme.subTextColor
    property color hoverColor: theme.textColor
    property int iconSize: 19
    property string tip: ""
    signal clicked()

    // 非 Layout 容器（如 Row）里也要有尺寸：width/height 默认取 implicit*
    implicitWidth: 32
    implicitHeight: 32
    Layout.preferredWidth: 32
    Layout.preferredHeight: 32

    Rectangle {
        anchors.fill: parent
        radius: 16
        color: mouse.containsMouse ? theme.hoverColor : "transparent"
    }

    Icon {
        anchors.centerIn: parent
        name: btn.name
        iconSize: btn.iconSize
        iconColor: mouse.containsMouse ? btn.hoverColor : btn.iconColor
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: btn.clicked()
    }

    ToolTip.visible: mouse.containsMouse && btn.tip.length > 0
    ToolTip.text: btn.tip
    ToolTip.delay: 600
}
