import QtQuick
import QtQuick.Controls

// #31-⑨ 统一滑块组件：自绘背景 + 圆手柄，参数化填充色/手柄尺寸/轨道高度。
// 取代 EffectsPanel / PlayBar 里各自重复的 background/handle 内联定义。
Slider {
    id: sl
    property color fillColor: theme.accentColor   // 已填充段颜色
    property color trackColor: theme.borderColor  // 未填充轨道颜色（深色面板可覆盖）
    property color handleColor: "white"
    property int handleSize: 12
    property int trackHeight: 3

    background: Rectangle {
        x: sl.leftPadding
        y: sl.topPadding + sl.availableHeight / 2 - height / 2
        width: sl.availableWidth
        height: sl.trackHeight
        radius: sl.trackHeight / 2
        color: sl.trackColor
        Rectangle {
            width: sl.visualPosition * parent.width
            height: parent.height
            radius: parent.radius
            color: sl.fillColor
        }
    }
    handle: Rectangle {
        x: sl.leftPadding + sl.visualPosition * (sl.availableWidth - width)
        y: sl.topPadding + sl.availableHeight / 2 - height / 2
        width: sl.handleSize
        height: sl.handleSize
        radius: sl.handleSize / 2
        color: sl.handleColor
    }
}
