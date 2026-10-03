import QtQuick

// 单行歌词渲染：整行 + 逐字扫描（左→右裁切 accent 覆盖层）+ 可选翻译/罗马音。
// 三种全屏模式 + 桌面歌词共用。
Item {
    id: root

    property var line: ({})
    property bool active: false
    property real progress: 0            // 0~1，仅对 active 行有效
    property int size: 20
    property color textColor: "#ffffff"
    property color inactiveColor: "#ffffff"
    property real inactiveOpacity: 0.4
    property color accentColor: "#ff4d4f"
    property bool showTranslation: false
    property bool showRoman: false
    property bool karaoke: false
    property real maxW: 1e9

    signal clicked()

    // 以主行文字宽度为内容宽（居中/裁切都基于 mainText）
    implicitWidth: Math.min(maxW, Math.max(mainText.implicitWidth,
                        (showTranslation && line.translation) ? Math.min(maxW, transText.implicitWidth) : 0))
    implicitHeight: col.height

    width: implicitWidth
    height: implicitHeight

    Column {
        id: col
        x: 0
        y: 0
        width: root.width
        spacing: 5

        // ---- 主行 ----
        Item {
            id: mainHolder
            width: root.width
            height: mainText.height

            Text {
                id: mainText
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                text: root.line && root.line.text ? root.line.text : ""
                color: root.active ? root.textColor
                                   : Qt.alpha(root.inactiveColor, root.inactiveOpacity)
                font.pixelSize: root.size
                font.bold: root.active
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                Behavior on font.pixelSize { NumberAnimation { duration: 200 } }
                Behavior on color { ColorAnimation { duration: 250 } }
            }

            // 逐字卡拉OK：与主行完全同几何，仅把已唱部分染 accent，clip 从左边按 progress 揭示
            Item {
                visible: !!(root.karaoke && root.active
                         && root.line && root.line.words && root.line.words.length > 0)
                anchors.left: mainText.left
                anchors.top: mainText.top
                width: mainText.width * Math.max(0, Math.min(1, root.progress))
                height: mainText.height
                clip: true
                Text {
                    x: 0; y: 0
                    width: mainText.width
                    text: mainText.text
                    color: root.accentColor
                    font.pixelSize: mainText.font.pixelSize
                    font.bold: mainText.font.bold
                    horizontalAlignment: mainText.horizontalAlignment
                    wrapMode: mainText.wrapMode
                }
                Behavior on width { NumberAnimation { duration: 100 } }
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: root.clicked()
            }
        }

        // ---- 翻译 ----
        Text {
            id: transText
            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.min(root.maxW, root.width)
            visible: root.showTranslation
                     && root.line && (root.line.translation || "").length > 0
            text: root.line ? (root.line.translation || "") : ""
            color: Qt.alpha(root.inactiveColor, root.active ? 0.9 : 0.3)
            font.pixelSize: Math.max(12, root.size * 0.6)
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }
        // ---- 罗马音 ----
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.min(root.maxW, root.width)
            visible: root.showRoman
                     && root.line && (root.line.roman || "").length > 0
            text: root.line ? (root.line.roman || "") : ""
            color: Qt.alpha(root.inactiveColor, root.active ? 0.7 : 0.25)
            font.pixelSize: Math.max(11, root.size * 0.48)
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }
    }
}
