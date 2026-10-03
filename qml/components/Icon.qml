import QtQuick
import QtQuick.Effects

// 统一的线性图标（用 MultiEffect 着色，跟随主题文字色）
Item {
    id: ico
    property string name: ""
    property color iconColor: theme.textColor
    property int iconSize: 20
    property real opacityLevel: 1.0

    implicitWidth: iconSize
    implicitHeight: iconSize
    opacity: opacityLevel

    Image {
        id: src
        anchors.fill: parent
        source: ico.name.length > 0 ? ("qrc:/icons/" + ico.name + ".svg") : ""
        sourceSize.width: ico.iconSize * 2
        sourceSize.height: ico.iconSize * 2
        fillMode: Image.PreserveAspectFit
        smooth: true
        visible: false
    }

    MultiEffect {
        anchors.fill: src
        source: src
        colorization: 1.0
        colorizationColor: ico.iconColor
        visible: src.status === Image.Ready
    }
}
