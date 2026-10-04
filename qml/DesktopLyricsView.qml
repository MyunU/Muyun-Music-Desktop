import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts

// 桌面歌词（DesktopLyricsController 用 QQuickView 承载，透明置顶无边框）。
// 悬停工具条：鼠标移到歌词条上，右上角浮出 [置顶][设置][关闭]。
// 设置面板：独立顶层 Window（不受 100px 歌词窗裁剪），浮在歌词条上方。
Item {
    id: root
    width: 760
    height: 110

    property real opacityLevel: 1.0
    property bool locked: false

    readonly property var colorPresets: [
        { name: "玫红", c: "#ff5a5f" }, { name: "橙", c: "#ff9f43" },
        { name: "金黄", c: "#f9ca24" }, { name: "绿", c: "#2ecc71" },
        { name: "青", c: "#00d2d3" }, { name: "蓝", c: "#54a0ff" },
        { name: "紫", c: "#a55eea" }, { name: "白", c: "#f5f6fa" }
    ]
    readonly property var sizePresets: [
        { name: "小", px: 22 }, { name: "中", px: 28 },
        { name: "大", px: 36 }, { name: "特大", px: 46 }
    ]

    // 统一 hover 源：顶层铺满整条、不吃点击(NoButton)的探测层。放最上层可避免"鼠标移到工具条
    // 按钮上→子 MouseArea 抢走 hover→dragArea 失 hover→工具条隐藏"的抖动循环
    readonly property bool barHover: hoverLayer.containsMouse || settingsWin.visible

    Rectangle {
        id: bar
        anchors.fill: parent
        anchors.margins: 6
        radius: 12
        // 置顶态完全透明（只留文字，不显示任何矩形/边框）；编辑态悬停才淡入背景
        color: deskLyrics.pinned ? "transparent"
             : (root.barHover ? "#55000000" : "#14000000")
        border.color: (!deskLyrics.pinned && root.barHover) ? "#33ffffff" : "#00000000"
        border.width: 1

        Column {
            id: lyricCol
            anchors.centerIn: parent
            width: parent.width - 24
            spacing: 4

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: player.currentLyricText
                color: deskLyrics.color
                font.pixelSize: deskLyrics.sizePx
                font.bold: true
                elide: Text.ElideRight
                style: Text.Outline
                styleColor: "#cc000000"
                visible: text.length > 0
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: player.currentLyricTranslation
                color: "#f0f0f0"
                font.pixelSize: Math.max(11, Math.round(deskLyrics.sizePx * 0.55))
                elide: Text.ElideRight
                style: Text.Outline
                styleColor: "#cc000000"
                visible: text.length > 0
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "♪ 暮云音乐 · 桌面歌词"
                color: "#99ffffff"
                font.pixelSize: 15
                visible: (player.currentLyricText || "").length === 0
            }
        }

        // 悬停工具条（编辑态才出现；置顶态点击穿透本就点不到）
        Item {
            id: toolbarBox
            z: 10
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.topMargin: 2
            anchors.rightMargin: 4
            width: toolbarRow.implicitWidth
            height: toolbarRow.implicitHeight
            visible: !deskLyrics.pinned && root.barHover

            Row {
                id: toolbarRow
                spacing: 2
                ToolBtn { text: "置顶"; onClicked: deskLyrics.setPinned(true) }
                ToolBtn { text: "设置"; accent: settingsWin.visible
                    onClicked: settingsWin.visible ? settingsWin.hide() : openSettings() }
                ToolBtn { text: "✕"; danger: true; onClicked: deskLyrics.hide() }
            }
        }

        // 置顶态悬停：只显示「取消置顶」（其余 设置/关闭 不显示，#14）。
        // ⚠ 置顶窗是"点击穿透"的，收不到 Qt 鼠标事件 → 悬停判断走 C++ 全局光标轮询出的
        //   deskLyrics.pinnedHover（不是 barHover）；命中时 C++ 已顺带临时去掉穿透，钮才可点。
        Item {
            id: pinToolbarBox
            z: 10
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.topMargin: 2
            anchors.rightMargin: 4
            width: pinToolbarRow.implicitWidth
            height: pinToolbarRow.implicitHeight
            visible: deskLyrics.pinned && deskLyrics.pinnedHover

            Row {
                id: pinToolbarRow
                spacing: 2
                ToolBtn {
                    objectName: "unpinBtnObj"   // 自检定位用（--test-desklyric-click 走真鼠标点击）
                    text: "取消置顶"; accent: true
                    onClicked: deskLyrics.setPinned(false)
                }
            }
        }

        MouseArea {
            id: dragArea
            anchors.fill: parent
            enabled: !root.locked && !deskLyrics.pinned
            hoverEnabled: true
            cursorShape: Qt.SizeAllCursor
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            property real px
            property real py
            onPressed: (mouse) => { px = mouse.x; py = mouse.y }
            onPositionChanged: (mouse) => {
                if (pressed && !root.locked && !deskLyrics.pinned && root.Window.window) {
                    root.Window.window.x += mouse.x - px
                    root.Window.window.y += mouse.y - py
                    repositionSettings()
                }
            }
            onDoubleClicked: player.togglePlay()
            onClicked: (mouse) => { if (mouse.button === Qt.RightButton) menu.popup() }
        }

        // 顶层统一 hover 探测层：铺满整条、acceptedButtons=NoButton（点击全部透传给下层按钮/拖拽），
        // 只负责"鼠标是否在条上"，从根上消除子控件抢 hover 造成的工具条抖动
        MouseArea {
            id: hoverLayer
            z: 20
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.NoButton
        }
    }

    function openSettings() {
        repositionSettings()
        settingsWin.show()
        settingsWin.raise()
    }
    function repositionSettings() {
        const w = root.Window.window
        if (!w) return
        // 浮在歌词条上方；上方空间不足则放下方
        let ny = w.y - settingsWin.height - 4
        if (ny < 0) ny = w.y + w.height + 4
        settingsWin.x = w.x + (w.width - settingsWin.width) / 2
        settingsWin.y = ny
    }

    // ===== 设置面板：独立顶层 Window（不被 100px 歌词窗裁剪）=====
    Window {
        id: settingsWin
        width: 268
        height: settingsCol.implicitHeight + 24
        flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
        color: "transparent"
        title: "歌词设置"
        // 歌词窗关闭/置顶时，设置窗一并收起
        onVisibleChanged: { }

        Rectangle {
            anchors.fill: parent
            radius: 12
            color: "#f216181e"
            border.color: "#33ffffff"
            border.width: 1

            ColumnLayout {
                id: settingsCol
                anchors.fill: parent
                anchors.margins: 12
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true
                    Text { text: "歌词设置"; color: "#e8ebf0"; font.pixelSize: 13; font.bold: true }
                    Item { Layout.fillWidth: true }
                    Rectangle {
                        width: 22; height: 22; radius: 11
                        color: setCloseMa.containsMouse ? "#33ffffff" : "transparent"
                        Text { anchors.centerIn: parent; text: "✕"; color: "#9aa2af"; font.pixelSize: 12 }
                        MouseArea { id: setCloseMa; anchors.fill: parent; hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: settingsWin.hide() }
                    }
                }

                Text { text: "字体颜色"; color: "#9aa2af"; font.pixelSize: 11 }
                Flow {
                    Layout.fillWidth: true
                    spacing: 8
                    Repeater {
                        model: root.colorPresets
                        delegate: Rectangle {
                            required property var modelData
                            width: 24; height: 24; radius: 12
                            color: modelData.c
                            border.width: deskLyrics.color === modelData.c ? 2 : 1
                            border.color: deskLyrics.color === modelData.c ? "#ffffff" : "#44ffffff"
                            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                onClicked: deskLyrics.color = modelData.c }
                        }
                    }
                }

                Text { text: "字体大小"; color: "#9aa2af"; font.pixelSize: 11 }
                Row {
                    spacing: 8
                    Repeater {
                        model: root.sizePresets
                        delegate: Rectangle {
                            required property var modelData
                            width: sizeLabel.width + 20; height: 26; radius: 13
                            color: deskLyrics.sizePx === modelData.px ? theme.accentColor : "#2a2f3b"
                            border.color: "#22ffffff"; border.width: 1
                            Text { id: sizeLabel; anchors.centerIn: parent
                                text: modelData.name; color: "#e8ebf0"; font.pixelSize: 12 }
                            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                onClicked: deskLyrics.sizePx = modelData.px }
                        }
                    }
                }

                RowLayout {
                    Layout.topMargin: 2
                    Layout.fillWidth: true
                    Text { text: "透明度"; color: "#9aa2af"; font.pixelSize: 11 }
                    Slider {
                        Layout.fillWidth: true
                        from: 0.3; to: 1.0
                        value: root.opacityLevel
                        onMoved: root.opacityLevel = value
                    }
                    Text { text: Math.round(root.opacityLevel * 100) + "%"; color: "#9aa2af"; font.pixelSize: 11 }
                }
            }
        }
    }

    // 右键菜单（备用入口）
    Menu {
        id: menu
        palette.text: theme.textColor
        palette.windowText: theme.textColor
        palette.window: theme.cardColor
        palette.base: theme.cardColor
        palette.highlight: theme.hoverColor
        palette.highlightedText: theme.textColor
        background: Rectangle { color: theme.cardColor; radius: 10; border.color: theme.borderColor }
        MenuItem { height: 32; text: root.locked ? "解锁（可拖动）" : "锁定位置"
            onTriggered: root.locked = !root.locked }
        MenuItem { height: 32; text: "打开设置面板"
            onTriggered: openSettings() }
        MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: theme.borderColor } }
        MenuItem { height: 32; text: "关闭桌面歌词"
            onTriggered: deskLyrics.hide() }
    }

    opacity: root.opacityLevel

    // 歌词窗隐藏时同步收起设置窗
    onVisibleChanged: if (!visible) settingsWin.hide()

    // 工具条按钮内联组件
    component ToolBtn: Rectangle {
        id: btn
        property string text: ""
        property bool danger: false
        property bool accent: false
        width: lbl.implicitWidth + 16
        height: 24
        radius: 12
        color: danger ? (ma.containsMouse ? "#cc3b36" : "#99e04a44")
             : accent ? theme.accentColor
             : (ma.containsMouse ? "#44ffffff" : "#22ffffff")
        Text {
            id: lbl
            anchors.centerIn: parent
            text: btn.text
            color: "white"
            font.pixelSize: 11
            font.family: "Microsoft YaHei UI"
        }
        MouseArea {
            id: ma
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: btn.clicked()
        }
        signal clicked()
    }
}
