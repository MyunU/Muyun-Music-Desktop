import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

// 舞台 fx 视觉控制台面板（StageFxController 的 QQuickView 承载内容）
// 经 StageBridge::fx* 通道透传 mineradio 引擎（setFxValue/toggleFx/setPreset/resetFx），
// 引擎回显 state 驱动控件值；面板浮在舞台窗之上，边调参边看效果。
// 样式对齐 EffectsPanel：Flow 预设 chips / 自绘 Slider / 胶囊开关。
Item {
    id: root
    // QQuickView 承载：root 必须有显式尺寸（同 DesktopLyricsView 模式），
    // 否则 SizeRootObjectToView 下 root 保持 0x0，布局全部溢出
    width: 340
    height: 720

    // ---- 状态（引擎回显，{k: value}）----
    property var fxState: ({})

    function fxNum(k, d) {
        var s = root.fxState
        if (!s || typeof s !== "object") return d
        var v = s[k]
        return (typeof v === "number" && isFinite(v)) ? v : d
    }
    function fxBool(k, d) {
        var s = root.fxState
        if (!s || typeof s !== "object") return d
        var v = s[k]
        return (v === undefined || v === null) ? d : !!v
    }

    // 预设（短中文名，index 对应引擎 presetMeta；顺序照抄 presetDisplayOrder）
    readonly property var presets: [
        { i: 0, name: "封面" }, { i: 6, name: "安魂" }, { i: 5, name: "星河" },
        { i: 4, name: "唱片" }, { i: 2, name: "星球" }, { i: 1, name: "滚筒" },
        { i: 3, name: "虚空" }
    ]

    // ---- 外观：深色玻璃（舞台是暗色视觉主体，面板不随浅色主题变白）----
    readonly property color accent: theme.accentColor
    readonly property color panelText: "#e8ebf0"
    readonly property color subText: "#9aa2af"
    readonly property color chipBg: "#2a2f3b"
    readonly property color grooveBg: "#3a4150"
    readonly property font uiFont: Qt.font({ family: "Microsoft YaHei UI" })

    Rectangle {
        anchors.fill: parent
        radius: 14
        color: "#e814161c"
        border.color: "#33ffffff"
        border.width: 1
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 10

        // ===== 标题栏（拖动区 + 关闭）=====
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 30
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: "视觉控制台"
                color: root.panelText
                font.pixelSize: 15
                font.bold: true
                font.family: "Microsoft YaHei UI"
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.DragMoveCursor
                onPressed: {
                    if (Window.window) Window.window.startSystemMove()
                }
            }
            Rectangle {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 26; height: 26; radius: 13
                color: closeMa.containsMouse ? "#33ffffff" : "transparent"
                Text {
                    anchors.centerIn: parent
                    text: "✕"; color: root.subText
                    font.pixelSize: 13
                }
                MouseArea {
                    id: closeMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: if (typeof stageFx !== "undefined") stageFx.hide()
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: "#22ffffff"
        }

        // ===== 视觉预设 =====
        Text {
            text: "视觉预设"
            color: root.subText
            font.pixelSize: 11
            font.family: "Microsoft YaHei UI"
        }
        Flow {
            Layout.fillWidth: true
            spacing: 6
            Repeater {
                model: root.presets
                delegate: Rectangle {
                    required property var modelData
                    readonly property bool selected: root.fxNum("preset", 0) === modelData.i
                    width: presetLabel.implicitWidth + 22
                    height: 26
                    radius: 13
                    color: selected ? root.accent : root.chipBg
                    border.color: selected ? "#55ffffff" : "#1fffffff"
                    border.width: 1
                    Text {
                        id: presetLabel
                        anchors.centerIn: parent
                        text: modelData.name
                        color: selected ? "#ffffff" : "#c8ccd4"
                        font.pixelSize: 12
                        font.family: "Microsoft YaHei UI"
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: if (typeof stage !== "undefined") stage.fxPreset(modelData.i)
                    }
                }
            }
        }

        // ===== 参数区（可滚动）=====
        Flickable {
            id: flick
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            contentHeight: paramsCol.implicitHeight
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            ColumnLayout {
                id: paramsCol
                width: flick.width
                spacing: 8

                Text {
                    text: "粒子"
                    color: root.subText
                    font.pixelSize: 11
                    font.family: "Microsoft YaHei UI"
                }
                Repeater {
                    model: [
                        { k: "intensity",       label: "律动强度",   mn: 0.2,  mx: 1.6,  df: 0.85 },
                        { k: "depth",           label: "空间深度",   mn: 0.2,  mx: 1.8,  df: 1.0  },
                        { k: "point",           label: "粒子大小",   mn: 0.5,  mx: 2.2,  df: 1.0  },
                        { k: "speed",           label: "流动速度",   mn: 0.2,  mx: 2.5,  df: 1.0  },
                        { k: "twist",           label: "扭曲",       mn: 0.0,  mx: 0.6,  df: 0.0  },
                        { k: "color",           label: "色彩张力",   mn: 0.5,  mx: 2.0,  df: 1.1  },
                        { k: "scatter",         label: "粒子发散",   mn: 0.0,  mx: 0.5,  df: 0.0  },
                        { k: "bgFade",          label: "背景淡出",   mn: 0.0,  mx: 1.2,  df: 0.2  },
                        { k: "coverResolution", label: "粒子密度",   mn: 0.75, mx: 1.55, df: 1.55 }
                    ]
                    delegate: SliderRow {
                        required property var modelData
                        kKey: modelData.k
                        label: modelData.label
                        mn: modelData.mn
                        mx: modelData.mx
                        dflt: modelData.df
                    }
                }

                Text {
                    text: "镜头与光效"
                    color: root.subText
                    font.pixelSize: 11
                    font.family: "Microsoft YaHei UI"
                }
                Repeater {
                    model: [
                        { k: "cinema",  label: "电影镜头",    df: true  },
                        { k: "bloom",   label: "溢光",        df: false },
                        { k: "edge",    label: "轮廓高亮",    df: false },
                        { k: "aiDepth", label: "AI 立体增强", df: false }
                    ]
                    delegate: SwitchRow {
                        required property var modelData
                        kKey: modelData.k
                        label: modelData.label
                        dflt: modelData.df
                    }
                }
                Repeater {
                    model: [
                        { k: "cinemaShake",   label: "电影震感", mn: 0.0, mx: 1.6, df: 0.5  },
                        { k: "bloomStrength", label: "溢光强度", mn: 0.0, mx: 1.6, df: 0.62 }
                    ]
                    delegate: SliderRow {
                        required property var modelData
                        kKey: modelData.k
                        label: modelData.label
                        mn: modelData.mn
                        mx: modelData.mx
                        dflt: modelData.df
                    }
                }

                Text {
                    text: "歌词"
                    color: root.subText
                    font.pixelSize: 11
                    font.family: "Microsoft YaHei UI"
                }
                Repeater {
                    model: [
                        { k: "particleLyrics",     label: "粒子歌词",   df: true  },
                        { k: "lyricGlow",          label: "歌词溢光",   df: true  },
                        { k: "lyricGlowBeat",      label: "溢光随鼓点", df: true  },
                        { k: "lyricGlowParticles", label: "歌词光粒",   df: false },
                        { k: "lyricCameraLock",    label: "歌词绑镜头", df: false }
                    ]
                    delegate: SwitchRow {
                        required property var modelData
                        kKey: modelData.k
                        label: modelData.label
                        dflt: modelData.df
                    }
                }
                Repeater {
                    model: [
                        { k: "lyricScale",        label: "歌词大小", mn: 0.35, mx: 1.65, df: 1.0  },
                        { k: "lyricGlowStrength", label: "溢光亮度", mn: 0.0,  mx: 0.85, df: 0.28 }
                    ]
                    delegate: SliderRow {
                        required property var modelData
                        kKey: modelData.k
                        label: modelData.label
                        mn: modelData.mn
                        mx: modelData.mx
                        dflt: modelData.df
                    }
                }
            }
        }

        // ===== 底部 =====
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: "#22ffffff"
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Rectangle {
                Layout.preferredWidth: 76
                Layout.preferredHeight: 26
                radius: 13
                color: resetMa.containsMouse ? "#33ffffff" : "transparent"
                border.color: "#44ffffff"
                border.width: 1
                Text {
                    anchors.centerIn: parent
                    text: "恢复默认"
                    color: root.panelText
                    font.pixelSize: 11
                    font.family: "Microsoft YaHei UI"
                }
                MouseArea {
                    id: resetMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: if (typeof stage !== "undefined") stage.fxReset()
                }
            }
            Item { Layout.fillWidth: true }
            Text {
                text: "调参即时生效 · 自动保存"
                color: root.subText
                font.pixelSize: 10
                font.family: "Microsoft YaHei UI"
            }
        }
    }

    // 引擎回显 → 刷新控件（回显经 250ms 防抖；拖拽中的滑块不覆盖）
    Connections {
        target: (typeof stage !== "undefined") ? stage : null
        enabled: (typeof stage !== "undefined")
        function onFxStateReceived(state) {
            root.fxState = state
        }
    }

    // ===== 行内组件 =====
    component SliderRow: RowLayout {
        id: srow
        property string kKey: ""
        property string label: ""
        property real mn: 0
        property real mx: 1
        property real dflt: 0
        spacing: 6
        Layout.fillWidth: true
        Layout.preferredHeight: 30
        Text {
            text: srow.label
            color: root.panelText
            font.pixelSize: 12
            font.family: "Microsoft YaHei UI"
            Layout.preferredWidth: 74
            Layout.alignment: Qt.AlignVCenter
        }
        Slider {
            id: sl
            Layout.fillWidth: true
            implicitWidth: 100
            from: srow.mn
            to: srow.mx
            // 回显只在未拖拽时写回，防止拖动中被 echo 打断
            Binding {
                target: sl
                property: "value"
                value: root.fxNum(srow.kKey, srow.dflt)
                when: !sl.pressed
                restoreMode: Binding.RestoreBindingOrValue
            }
            onMoved: if (typeof stage !== "undefined") stage.fxSet(srow.kKey, value)
            background: Rectangle {
                x: sl.leftPadding
                y: sl.topPadding + sl.availableHeight / 2 - height / 2
                width: sl.availableWidth
                height: 3; radius: 2
                color: root.grooveBg
                Rectangle {
                    width: sl.visualPosition * parent.width
                    height: parent.height; radius: 2
                    color: root.accent
                }
            }
            handle: Rectangle {
                x: sl.leftPadding + sl.visualPosition * (sl.availableWidth - width)
                y: sl.topPadding + sl.availableHeight / 2 - height / 2
                implicitWidth: 12; implicitHeight: 12; radius: 6
                color: "white"
            }
        }
        Text {
            text: sl.value.toFixed(2)
            color: root.subText
            font.pixelSize: 11
            font.family: "Consolas"
            Layout.preferredWidth: 36
            horizontalAlignment: Text.AlignRight
            Layout.alignment: Qt.AlignVCenter
        }
    }

    // 开关行：整行点击发 fxToggle；状态是引擎回显的纯镜像（不本地翻转）
    component SwitchRow: Rectangle {
        id: swrow
        property string kKey: ""
        property string label: ""
        property bool dflt: false
        readonly property bool on: root.fxBool(kKey, dflt)
        Layout.fillWidth: true
        implicitHeight: 28
        color: swMa.containsMouse ? "#14ffffff" : "transparent"
        radius: 6
        Text {
            anchors.left: parent.left
            anchors.leftMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            text: swrow.label
            color: root.panelText
            font.pixelSize: 12
            font.family: "Microsoft YaHei UI"
        }
        Rectangle {
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            implicitWidth: 40; implicitHeight: 22
            radius: 11
            color: swrow.on ? root.accent : root.grooveBg
            Rectangle {
                x: swrow.on ? parent.width - width - 3 : 3
                anchors.verticalCenter: parent.verticalCenter
                width: 16; height: 16; radius: 8
                color: "white"
                Behavior on x { NumberAnimation { duration: 120 } }
            }
        }
        MouseArea {
            id: swMa
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: if (typeof stage !== "undefined") stage.fxToggle(swrow.kKey)
        }
    }
}
