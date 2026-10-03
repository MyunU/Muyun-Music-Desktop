import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 音效调节面板：均衡器 / 混响 / 环绕 / 响度
Popup {
    id: panel
    width: 580
    height: 640
    anchors.centerIn: parent
    modal: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        radius: 14
        color: theme.panelColor
        border.color: theme.borderColor
        border.width: 1
    }

    property var eqPresets: ["默认", "流行", "舞曲", "摇滚", "古典", "人声", "电子"]
    property var reverbPresets: ["小房间", "大厅", "教堂", "金属板"]

    // 生效状态文案：让用户一眼知道"音效到底有没有在起作用"
    // （自四-51 起所有常见格式都能生效，只有真的解不了码才旁路，措辞相应改掉"不是 MP3"）
    readonly property string fxStateText: {
        if (!player.effectsOn) return "未开启：打开下面任一开关即实时生效"
        if (player.effectsLive) return "音效已生效（实时处理，调参数立刻能听到）"
        if (player.effectsBypassed) return "已开启，但当前音源无法解码，音效未生效"
        return "正在准备音效管线…"
    }
    readonly property color fxStateColor: player.effectsLive ? theme.accentColor
                                          : (player.effectsBypassed ? "#e0a34a" : theme.subTextColor)

    // 有推子正在被拖 → 外层 Flickable 让位（见下面 interactive 绑定）
    property bool eqDragging: false

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 14

        // ---------- 标题 ----------
        RowLayout {
            Layout.fillWidth: true
            Icon { name: "effects"; iconSize: 18; iconColor: theme.accentColor }
            Text {
                text: "音效设置"
                color: theme.textColor
                font.pixelSize: 18
                font.bold: true
            }
            Item { Layout.fillWidth: true }
            // 手动恢复默认：只归零参数，不动四个开关
            // （配置一律记住用户习惯，程序绝不自己恢复默认——这是本轮明确改掉的旧行为）
            Rectangle {
                Layout.alignment: Qt.AlignVCenter
                width: rstRow.implicitWidth + 16
                height: 26
                radius: 13
                color: rstMa.containsMouse ? theme.hoverColor : "transparent"
                border.width: 1
                border.color: theme.borderColor
                Row {
                    id: rstRow
                    anchors.centerIn: parent
                    spacing: 4
                    Icon {
                        name: "refresh"
                        iconSize: 13
                        anchors.verticalCenter: parent.verticalCenter
                        iconColor: rstMa.containsMouse ? theme.textColor : theme.subTextColor
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "恢复默认"
                        color: rstMa.containsMouse ? theme.textColor : theme.subTextColor
                        font.pixelSize: 12
                    }
                }
                MouseArea {
                    id: rstMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: player.resetAllEffects()
                }
                ToolTip.visible: rstMa.containsMouse
                ToolTip.delay: 500
                ToolTip.text: "把均衡器曲线 / 混响 / 环绕 / 响度目标恢复默认值（不改变各音效开关的开关状态）"
            }
            IconButton {
                name: "window-close"
                iconSize: 15
                onClicked: panel.close()
            }
        }

        // ---------- 生效状态行 ----------
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Rectangle {
                Layout.alignment: Qt.AlignVCenter
                width: 7; height: 7; radius: 4
                color: panel.fxStateColor
            }
            Text {
                text: panel.fxStateText
                color: panel.fxStateColor
                font.pixelSize: 11
            }
            Item { Layout.fillWidth: true }
            Text {
                visible: player.effectsLive && player.effectDeviceInfo.length > 0
                text: player.effectDeviceInfo
                color: theme.subTextColor
                font.pixelSize: 10
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: theme.borderColor }

        Flickable {
            id: panelFlick
            objectName: "panelFlick"        // 自检读 interactive，验证"拖推子时不滚动"的互斥
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentHeight: col.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            // ⚠ 拖动均衡器推子时必须让 Flickable 撒手：推子是竖向拖的，
            //   外层也是竖向滚动，不互斥就会"调 EQ 的同时整面板上下滚"（用户反馈）
            interactive: !panel.eqDragging

            ColumnLayout {
                id: col
                width: parent.width
                spacing: 16

                // ================= 均衡器 =================
                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: "均衡器"
                        color: theme.textColor
                        font.pixelSize: 14
                        font.bold: true
                    }
                    Item { Layout.fillWidth: true }
                    Switch {
                        id: eqSwitch
                        objectName: "eqSwitchObj"
                        checked: player.eqEnabled
                        onToggled: player.eqEnabled = checked
                        indicator: Rectangle {
                            implicitWidth: 40; implicitHeight: 22
                            radius: 11
                            color: eqSwitch.checked ? theme.accentColor : theme.borderColor
                            Rectangle {
                                x: eqSwitch.checked ? parent.width - width - 3 : 3
                                anchors.verticalCenter: parent.verticalCenter
                                width: 16; height: 16; radius: 8
                                color: "white"
                            }
                        }
                    }
                }

                // 预设按钮
                Flow {
                    Layout.fillWidth: true
                    spacing: 8
                    Repeater {
                        model: panel.eqPresets
                        delegate: Rectangle {
                            width: lbl.width + 22
                            height: 28
                            radius: 14
                            color: player.eqPreset === index ? theme.accentColor : theme.hoverColor
                            Text {
                                id: lbl
                                anchors.centerIn: parent
                                text: modelData
                                color: player.eqPreset === index ? "white" : theme.textColor
                                font.pixelSize: 12
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: {
                                    player.setEqPreset(index)
                                    player.eqEnabled = true
                                }
                            }
                        }
                    }
                }

                // 10 段滑杆
                // ⚠ 两处坑（截图实测出来的）：
                //   1) 外层是 Flickable，contentHeight 走 implicit 高度链，
                //      只写 Layout.preferredHeight 撑不开 → 同时给 minimumHeight
                //   2) delegate 的 ColumnLayout 与竖向 Slider 都必须 Layout.fillHeight，
                //      否则滑杆按 implicit 高度（~28px）排版，手柄掉到轨道外面，等于没法拖
                RowLayout {
                    Layout.fillWidth: true
                    Layout.minimumHeight: 170
                    Layout.preferredHeight: 170
                    spacing: 4
                    Repeater {
                        model: 10
                        delegate: ColumnLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            spacing: 6

                            Text {
                                Layout.alignment: Qt.AlignHCenter
                                text: (player.eqGain(index) > 0 ? "+" : "") + player.eqGain(index).toFixed(0)
                                color: theme.subTextColor
                                font.pixelSize: 10
                            }

                            // 自绘竖向推子。
                            // 不用 Controls.Slider(orientation: Vertical)：它的
                            // availableHeight/topPadding 在竖向模式下算错，轨道被压成一小块、
                            // 手柄掉到轨道外（截图实测），用户根本拖不动。
                            Item {
                                id: band
                                objectName: "eqBand"      // 自检按名字找推子核对数据往返
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.minimumHeight: 96
                                property int idx: index
                                property real val: player.eqGain(index)

                                // 值 → y（+12 在顶，-12 在底；上下各留半个把手高度）
                                function yOf(v) {
                                    const usable = Math.max(1, height - 12)
                                    return 6 + (1 - (v + 12) / 24) * usable
                                }
                                function vOf(y) {
                                    const usable = Math.max(1, height - 12)
                                    return 12 - ((y - 6) / usable) * 24
                                }
                                Connections {
                                    target: player
                                    function onEffectsChanged() {
                                        const v = player.eqGain(band.idx)
                                        if (Math.abs(band.val - v) > 0.01) band.val = v
                                    }
                                }

                                // 轨道
                                Rectangle {
                                    x: band.width / 2 - 2; y: 6
                                    width: 4; height: Math.max(0, band.height - 12); radius: 2
                                    color: theme.borderColor
                                }
                                // 0dB 中线（推子必须有参照，否则看不出是提升还是衰减）
                                Rectangle {
                                    x: band.width / 2 - 7
                                    y: band.height / 2 - 0.5
                                    width: 14; height: 1
                                    color: theme.hoverColor
                                }
                                // 从 0dB 到把手的填充段
                                Rectangle {
                                    x: band.width / 2 - 2
                                    width: 4; radius: 2
                                    color: Qt.rgba(theme.accentColor.r, theme.accentColor.g,
                                                   theme.accentColor.b, 0.55)
                                    y: band.val >= 0 ? band.yOf(band.val) + 5 : band.height / 2
                                    height: Math.abs(band.yOf(band.val) + 5 - band.height / 2)
                                }
                                // 把手
                                Rectangle {
                                    id: bandKnob
                                    width: 18; height: 10; radius: 5
                                    x: band.width / 2 - width / 2
                                    y: band.yOf(band.val)
                                    color: Math.abs(band.val) < 0.01
                                            ? theme.subTextColor : theme.accentColor
                                    border.width: 1
                                    border.color: theme.panelColor
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    // 按住即接管滚动：拖动推子时外层 Flickable 不再跟着滚
                                    onPressed: { panel.eqDragging = true; apply(mouse.y) }
                                    onReleased: panel.eqDragging = false
                                    onExited: if (pressed) panel.eqDragging = false
                                    onCanceled: panel.eqDragging = false
                                    onPositionChanged: if (containsPress) apply(mouse.y)
                                    function apply(y) {
                                        var v = Math.round(band.vOf(y))
                                        v = Math.max(-12, Math.min(12, v))
                                        if (Math.abs(v - band.val) > 0.01) {
                                            band.val = v
                                            player.setEqGain(band.idx, v)
                                        }
                                    }
                                }
                            }

                            Text {
                                Layout.alignment: Qt.AlignHCenter
                                text: player.eqBandLabels()[index]
                                color: theme.subTextColor
                                font.pixelSize: 9
                            }
                        }
                    }
                }

                Text {
                    text: "重置"
                    color: theme.accentColor
                    font.pixelSize: 12
                    MouseArea {
                        anchors.fill: parent
                        onClicked: player.resetEq()
                    }
                }

                Rectangle { Layout.fillWidth: true; height: 1; color: theme.borderColor }

                // ================= 混响 =================
                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: "环境混响"
                        color: theme.textColor
                        font.pixelSize: 14
                        font.bold: true
                    }
                    Item { Layout.fillWidth: true }
                    Switch {
                        id: revSwitch
                        objectName: "revSwitchObj"
                        checked: player.reverbEnabled
                        onToggled: player.reverbEnabled = checked
                        indicator: Rectangle {
                            implicitWidth: 40; implicitHeight: 22
                            radius: 11
                            color: revSwitch.checked ? theme.accentColor : theme.borderColor
                            Rectangle {
                                x: revSwitch.checked ? parent.width - width - 3 : 3
                                anchors.verticalCenter: parent.verticalCenter
                                width: 16; height: 16; radius: 8
                                color: "white"
                            }
                        }
                    }
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: 8
                    Repeater {
                        model: panel.reverbPresets
                        delegate: Rectangle {
                            width: rlbl.width + 22
                            height: 28
                            radius: 14
                            color: player.reverbPreset === index ? theme.accentColor : theme.hoverColor
                            Text {
                                id: rlbl
                                anchors.centerIn: parent
                                text: modelData
                                color: player.reverbPreset === index ? "white" : theme.textColor
                                font.pixelSize: 12
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: {
                                    player.setReverbPreset(index)
                                    player.reverbEnabled = true
                                }
                            }
                        }
                    }
                }

                Rectangle { Layout.fillWidth: true; height: 1; color: theme.borderColor }

                // ================= 3D 环绕 =================
                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: "3D 环绕"
                        color: theme.textColor
                        font.pixelSize: 14
                        font.bold: true
                    }
                    Item { Layout.fillWidth: true }
                    Switch {
                        id: spSwitch
                        checked: player.spatialEnabled
                        onToggled: player.spatialEnabled = checked
                        indicator: Rectangle {
                            implicitWidth: 40; implicitHeight: 22
                            radius: 11
                            color: spSwitch.checked ? theme.accentColor : theme.borderColor
                            Rectangle {
                                x: spSwitch.checked ? parent.width - width - 3 : 3
                                anchors.verticalCenter: parent.verticalCenter
                                width: 16; height: 16; radius: 8
                                color: "white"
                            }
                        }
                    }
                }

                // 环绕参数说明：深度＝左右摆多大（0＝不摆、不改变左右平衡），
                // 摆动速度＝人声从左耳移到右耳有多快（默认约 5 秒一个来回）
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12
                    Text { text: "环绕深度"; color: theme.subTextColor; font.pixelSize: 12 }
                    Slider {
                        id: radiusSlider
                        objectName: "spRadiusSlider"     // 自检要读它，确认显示的是模型真值而不是写死的 50
                        Layout.fillWidth: true
                        from: 0; to: 100
                        // ⚠ 必须绑模型真值。以前写死 value: 50：
                        //   ① 重启后永远显示 50（看着像"配置被恢复默认"）
                        //   ② 一动速度滑杆就把 setSpatialParams(50, …) 写回去，
                        //      真的把用户存的半径覆盖成默认 —— 显示假 + 数据被毁，双重坑
                        value: player.spatialRadius
                        onMoved: player.setSpatialParams(value, speedSlider.value)
                        Connections {
                            target: player
                            function onEffectsChanged() {
                                // 拖动会打断 value 绑定，外部改动（恢复默认）要显式回位
                                const v = player.spatialRadius
                                if (!radiusSlider.pressed && Math.abs(radiusSlider.value - v) > 0.01)
                                    radiusSlider.value = v
                            }
                        }
                        background: Rectangle {
                            x: radiusSlider.leftPadding
                            y: radiusSlider.topPadding + radiusSlider.availableHeight / 2 - height / 2
                            width: radiusSlider.availableWidth
                            height: 3; radius: 2
                            color: theme.borderColor
                            Rectangle {
                                width: radiusSlider.visualPosition * parent.width
                                height: parent.height; radius: 2
                                color: theme.accentColor
                            }
                        }
                        handle: Rectangle {
                            x: radiusSlider.leftPadding + radiusSlider.visualPosition * (radiusSlider.availableWidth - width)
                            y: radiusSlider.topPadding + radiusSlider.availableHeight / 2 - height / 2
                            width: 12; height: 12; radius: 6
                            color: "white"
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12
                    Text { text: "摆动速度"; color: theme.subTextColor; font.pixelSize: 12 }
                    Slider {
                        id: speedSlider
                        objectName: "spSpeedSlider"
                        Layout.fillWidth: true
                        from: 1; to: 100
                        value: player.spatialSpeed          // 同上：绑真值，不写死
                        onMoved: player.setSpatialParams(radiusSlider.value, value)
                        Connections {
                            target: player
                            function onEffectsChanged() {
                                const v = player.spatialSpeed
                                if (!speedSlider.pressed && Math.abs(speedSlider.value - v) > 0.01)
                                    speedSlider.value = v
                            }
                        }
                        background: Rectangle {
                            x: speedSlider.leftPadding
                            y: speedSlider.topPadding + speedSlider.availableHeight / 2 - height / 2
                            width: speedSlider.availableWidth
                            height: 3; radius: 2
                            color: theme.borderColor
                            Rectangle {
                                width: speedSlider.visualPosition * parent.width
                                height: parent.height; radius: 2
                                color: theme.accentColor
                            }
                        }
                        handle: Rectangle {
                            x: speedSlider.leftPadding + speedSlider.visualPosition * (speedSlider.availableWidth - width)
                            y: speedSlider.topPadding + speedSlider.availableHeight / 2 - height / 2
                            width: 12; height: 12; radius: 6
                            color: "white"
                        }
                    }
                }

                Rectangle { Layout.fillWidth: true; height: 1; color: theme.borderColor }

                // ================= 响度均衡 =================
                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: "响度均衡"
                        color: theme.textColor
                        font.pixelSize: 14
                        font.bold: true
                    }
                    Item { Layout.fillWidth: true }
                    Switch {
                        id: ldSwitch
                        checked: player.loudnessEnabled
                        onToggled: player.loudnessEnabled = checked
                        indicator: Rectangle {
                            implicitWidth: 40; implicitHeight: 22
                            radius: 11
                            color: ldSwitch.checked ? theme.accentColor : theme.borderColor
                            Rectangle {
                                x: ldSwitch.checked ? parent.width - width - 3 : 3
                                anchors.verticalCenter: parent.verticalCenter
                                width: 16; height: 16; radius: 8
                                color: "white"
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12
                    Text { text: "目标响度"; color: theme.subTextColor; font.pixelSize: 12 }
                    Slider {
                        id: ldSlider
                        Layout.fillWidth: true
                        from: -24; to: -8
                        stepSize: 1
                        value: player.loudnessTarget
                        onMoved: player.setLoudnessTarget(value)
                        Connections {
                            target: player
                            function onEffectsChanged() {
                                const v = player.loudnessTarget
                                if (!ldSlider.pressed && Math.abs(ldSlider.value - v) > 0.01)
                                    ldSlider.value = v
                            }
                        }
                        background: Rectangle {
                            x: ldSlider.leftPadding
                            y: ldSlider.topPadding + ldSlider.availableHeight / 2 - height / 2
                            width: ldSlider.availableWidth
                            height: 3; radius: 2
                            color: theme.borderColor
                            Rectangle {
                                width: ldSlider.visualPosition * parent.width
                                height: parent.height; radius: 2
                                color: theme.accentColor
                            }
                        }
                        handle: Rectangle {
                            x: ldSlider.leftPadding + ldSlider.visualPosition * (ldSlider.availableWidth - width)
                            y: ldSlider.topPadding + ldSlider.availableHeight / 2 - height / 2
                            width: 12; height: 12; radius: 6
                            color: "white"
                        }
                    }
                    Text {
                        text: player.loudnessTarget.toFixed(0) + " dB"
                        color: theme.subTextColor
                        font.pixelSize: 11
                        Layout.preferredWidth: 46
                    }
                }
            }
        }
    }
}
