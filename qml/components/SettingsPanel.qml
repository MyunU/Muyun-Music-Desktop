import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// 设置面板：本地音源 / 在线音源（无边框极简风）
Popup {
    id: panel
    width: 720
    height: 580
    anchors.centerIn: parent
    modal: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        radius: 14
        color: theme.panelColor
        border.color: theme.borderColor
        border.width: 1
    }

    property string section: "local"   // 分区 id（字符串，防新增分区错位）
    // 分区表：新增分区往数组任意位置加都行——"关于"由 sectionList 强制排最底
    property var sections: [
        { id: "local",  name: "本地音乐" },
        { id: "source", name: "在线音源" },
        { id: "player", name: "播放界面" },
        { id: "sync",   name: "同步" },
        { id: "about",  name: "关于" }
    ]
    readonly property var sectionList:
        sections.filter(function (s) { return s.id !== "about" })
        .concat(sections.filter(function (s) { return s.id === "about" }))
    function sectionName(id) {
        for (var i = 0; i < sections.length; ++i)
            if (sections[i].id === id) return sections[i].name
        return "设置"
    }

    function fmtSize(bytes) {
        if (bytes < 1024) return bytes + " B"
        if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KB"
        if (bytes < 1024 * 1024 * 1024) return (bytes / 1048576).toFixed(1) + " MB"
        return (bytes / 1073741824).toFixed(2) + " GB"
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ================= 左侧分类 =================
        Rectangle {
            Layout.preferredWidth: 150
            Layout.fillHeight: true
            color: theme.bgColor
            radius: 14

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 4

                Text {
                    text: "设置"
                    color: theme.textColor
                    font.pixelSize: 16
                    font.bold: true
                    Layout.bottomMargin: 10
                }

                Repeater {
                    model: panel.sectionList
                    delegate: Rectangle {
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.preferredHeight: 34
                        radius: 8
                        color: panel.section === modelData.id ? theme.activeColor
                                                              : (secHover.containsMouse ? theme.hoverColor : "transparent")
                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.name
                            color: panel.section === modelData.id ? theme.accentColor : theme.textColor
                            font.pixelSize: 13
                        }
                        MouseArea {
                            id: secHover
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: panel.section = modelData.id
                        }
                    }
                }

                Item { Layout.fillHeight: true }
            }
        }

        // ================= 右侧内容 =================
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 20
            spacing: 14

            Text {
                text: panel.sectionName(panel.section)
                color: theme.textColor
                font.pixelSize: 17
                font.bold: true
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: theme.borderColor }

            // ---------- 本地音乐 ----------
            ColumnLayout {
                visible: panel.section === "local"
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 12

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: "音乐目录"
                        color: theme.textColor
                        font.pixelSize: 13
                    }
                    Text {
                        text: settings.scanning ? "扫描中…" : ("共 " + settings.localCount + " 首")
                        color: theme.subTextColor
                        font.pixelSize: 11
                    }
                    Item { Layout.fillWidth: true }

                    Rectangle {
                        width: 74; height: 30; radius: 15
                        color: addHover.containsMouse ? theme.hoverColor : theme.cardColor
                        border.color: theme.borderColor
                        border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: "添加目录"
                            color: theme.textColor
                            font.pixelSize: 12
                        }
                        MouseArea {
                            id: addHover
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: folderPick.open()
                        }
                    }
                    Rectangle {
                        width: 66; height: 30; radius: 15
                        color: rescanHover.containsMouse ? theme.hoverColor : theme.cardColor
                        border.color: theme.borderColor
                        border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: "重新扫描"
                            color: theme.textColor
                            font.pixelSize: 12
                        }
                        MouseArea {
                            id: rescanHover
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: settings.rescan()
                        }
                    }
                }

                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 2
                    model: settings.folders

                    delegate: Rectangle {
                        width: ListView.view.width
                        height: 38
                        radius: 7
                        color: fHover.containsMouse ? theme.hoverColor : "transparent"

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            spacing: 8
                            Icon { name: "folder"; iconSize: 15; iconColor: theme.subTextColor }
                            Text {
                                Layout.fillWidth: true
                                text: modelData
                                color: theme.textColor
                                font.pixelSize: 12
                                elide: Text.ElideMiddle
                            }
                            Item {
                                Layout.preferredWidth: 26
                                Layout.preferredHeight: 38
                                Icon {
                                    id: folderCloseIcon
                                    anchors.centerIn: parent
                                    name: "window-close"
                                    iconSize: 13
                                    iconColor: folderCloseHover.containsMouse ? theme.accentColor : theme.subTextColor
                                }
                                MouseArea {
                                    id: folderCloseHover
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: settings.removeFolder(modelData)
                                }
                            }
                        }
                        MouseArea {
                            id: fHover
                            anchors.fill: parent
                            hoverEnabled: true
                            z: -1   // 必须置底：否则盖住行内 × 按钮导致"无法移除目录"
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        text: "还没有添加音乐目录"
                        color: theme.subTextColor
                        font.pixelSize: 12
                        visible: settings.folders.length === 0
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Text { text: "标签读取优先级"; color: theme.subTextColor; font.pixelSize: 12 }
                    Item { Layout.fillWidth: true }
                    Repeater {
                        model: ["内嵌优先", "外挂优先"]
                        delegate: Rectangle {
                            width: tpText.width + 20
                            height: 26
                            radius: 13
                            color: settings.tagPriority === index ? theme.accentColor : theme.hoverColor
                            Text {
                                id: tpText
                                anchors.centerIn: parent
                                text: modelData
                                color: settings.tagPriority === index ? "white" : theme.textColor
                                font.pixelSize: 11
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: settings.tagPriority = index
                            }
                        }
                    }
                }

                // 下载内嵌（MP3/FLAC）：封面 / 歌词
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Text {
                        text: "下载内嵌（MP3/FLAC）"
                        color: theme.subTextColor
                        font.pixelSize: 12
                    }
                    Item { Layout.fillWidth: true }
                    Repeater {
                        model: [
                            { label: "封面", on: settings.embedCover, toggle: function(v) { settings.embedCover = v } },
                            { label: "歌词", on: settings.embedLyrics, toggle: function(v) { settings.embedLyrics = v } }
                        ]
                        delegate: Rectangle {
                            required property var modelData
                            width: embText.width + 20
                            height: 26
                            radius: 13
                            color: modelData.on ? theme.accentColor : theme.hoverColor
                            Text {
                                id: embText
                                anchors.centerIn: parent
                                text: modelData.label + (modelData.on ? "：开" : "：关")
                                color: modelData.on ? "white" : theme.textColor
                                font.pixelSize: 11
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: modelData.toggle(!modelData.on)
                            }
                        }
                    }
                }

                Text {
                    text: "上次扫描：" + (settings.lastScannedAt || "从未")
                    color: theme.subTextColor
                    font.pixelSize: 11
                }
            }

            // ---------- 在线音源 ----------
            ColumnLayout {
                visible: panel.section === "source"
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 12

                Text {
                    text: "导入 LX 音源脚本后，可解析更多平台的播放链接"
                    color: theme.subTextColor
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Rectangle {
                        width: 88; height: 30; radius: 15
                        color: impHover.containsMouse ? theme.hoverColor : theme.cardColor
                        border.color: theme.borderColor
                        border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: "导入本地脚本"
                            color: theme.textColor
                            font.pixelSize: 12
                        }
                        MouseArea {
                            id: impHover
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: scriptPick.open()
                        }
                    }
                    Rectangle {
                        width: 78; height: 30; radius: 15
                        color: urlHover.containsMouse ? theme.hoverColor : theme.cardColor
                        border.color: theme.borderColor
                        border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: "从 URL 导入"
                            color: theme.textColor
                            font.pixelSize: 12
                        }
                        MouseArea {
                            id: urlHover
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: urlInput.visible = !urlInput.visible
                        }
                    }
                    Item { Layout.fillWidth: true }
                }

                RowLayout {
                    id: urlInput
                    visible: false
                    Layout.fillWidth: true
                    spacing: 8
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 32
                        radius: 16
                        color: theme.bgColor
                        border.color: theme.borderColor
                        border.width: 1
                        TextInput {
                            id: urlField
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            verticalAlignment: Text.AlignVCenter
                            color: theme.textColor
                            font.pixelSize: 12
                            selectByMouse: true
                            clip: true
                            onAccepted: {
                                if (text.trim().length > 0) settings.importLxSourceUrl(text.trim())
                                text = ""
                                urlInput.visible = false
                            }
                        }
                    }
                }

                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 2
                    model: settings.lxSources

                    delegate: Rectangle {
                        width: ListView.view.width
                        height: 58
                        radius: 8
                        color: sHover.containsMouse ? theme.hoverColor : "transparent"
                        border.color: settings.activeLxSourceId === modelData.id
                                      ? theme.accentColor : "transparent"
                        border.width: 1

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            spacing: 10

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.name + (settings.activeLxSourceId === modelData.id
                                          ? "  ·  当前" : "")
                                    color: theme.textColor
                                    font.pixelSize: 13
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: "v" + modelData.version +
                                          (modelData.author ? ("  ·  " + modelData.author) : "")
                                    color: theme.subTextColor
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }
                            }

                            // 启用/停用按钮
                            Rectangle {
                                implicitWidth: 56
                                implicitHeight: 26
                                radius: 5
                                color: "transparent"
                                Text {
                                    anchors.centerIn: parent
                                    text: modelData.enabled ? "已启用" : "已停用"
                                    color: modelData.enabled ? theme.accentColor : theme.subTextColor
                                    font.pixelSize: 11
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: settings.setLxSourceEnabled(modelData.id, !modelData.enabled)
                                }
                            }

                            // 删除按钮（放大点击区域，确保可点）
                            Rectangle {
                                implicitWidth: 28
                                implicitHeight: 28
                                radius: 6
                                color: delHover.containsMouse ? theme.hoverColor : "transparent"
                                Icon {
                                    anchors.centerIn: parent
                                    name: "window-close"
                                    iconSize: 14
                                    iconColor: theme.subTextColor
                                }
                                MouseArea {
                                    id: delHover
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: settings.removeLxSource(modelData.id)
                                }
                            }
                        }

                        // 整行点击设为当前音源（放在最上层但用 acceptedButtons 处理；
                        // 因删除/启停按钮在 RowLayout 内更靠后声明，会优先命中）
                        MouseArea {
                            id: sHover
                            anchors.fill: parent
                            hoverEnabled: true
                            z: -1
                            onClicked: settings.setActiveLxSource(modelData.id)
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        text: "还没有导入音源脚本"
                        color: theme.subTextColor
                        font.pixelSize: 12
                        visible: settings.lxSources.length === 0
                    }
                }
            }

            // ---------- 同步 ----------
                ColumnLayout {
                    visible: panel.section === "sync"
                    Layout.fillWidth: true
                    spacing: 12

                Text { text: "局域网同步"; color: theme.textColor; font.pixelSize: 16; font.bold: true }
                Text {
                    Layout.fillWidth: true
                    text: "在两台登录同一账号思路的设备间，同步【在线收藏 + 自建歌单】。本地音乐与本地收藏不参与同步（文件路径跨设备无意义）。"
                    color: theme.subTextColor; font.pixelSize: 11; wrapMode: Text.Wrap
                }

                // 开关
                RowLayout {
                    Layout.fillWidth: true
                    Text { text: "开启同步服务"; color: theme.textColor; font.pixelSize: 13; Layout.fillWidth: true }
                    Rectangle {
                        width: 40; height: 22; radius: 11
                        color: sync.enabled ? theme.accentColor : theme.hoverColor
                        Rectangle {
                            width: 18; height: 18; radius: 9; color: "white"
                            anchors.verticalCenter: parent.verticalCenter
                            x: sync.enabled ? parent.width - width - 2 : 2
                            Behavior on x { NumberAnimation { duration: 120 } }
                        }
                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -4
                            cursorShape: Qt.PointingHandCursor
                            onClicked: sync.enabled = !sync.enabled
                        }
                    }
                }

                Text {
                    visible: sync.enabled
                    Layout.fillWidth: true
                    text: "本机地址：" + sync.shareUrl + "（另一台设备在下方填此地址即可拉取/推送）"
                    color: theme.subTextColor; font.pixelSize: 11; wrapMode: Text.Wrap
                }

                Rectangle { Layout.fillWidth: true; height: 1; color: theme.borderColor }

                Text { text: "与另一台设备同步"; color: theme.textColor; font.pixelSize: 14; font.bold: true }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Rectangle {
                        Layout.fillWidth: true
                        height: 32; radius: 6; color: theme.cardColor
                        border.color: theme.borderColor; border.width: 1
                        TextInput {
                            id: peerInput
                            anchors.fill: parent
                            anchors.leftMargin: 8; anchors.rightMargin: 8
                            verticalAlignment: TextInput.AlignVCenter
                            clip: true
                            color: theme.textColor
                            font.pixelSize: 12
                            text: ""
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            text: "http://192.168.x.x:8765"
                            color: theme.subTextColor
                            font.pixelSize: 12
                            visible: peerInput.text.length === 0 && !peerInput.activeFocus
                        }
                    }
                }

                RowLayout {
                    spacing: 8
                    Rectangle {
                        width: 96; height: 32; radius: 16
                        color: sync.busy ? theme.hoverColor : theme.accentColor
                        Text { anchors.centerIn: parent; text: sync.busy ? "同步中…" : "拉取到我这"; color: "white"; font.pixelSize: 12 }
                        MouseArea {
                            anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                            enabled: !sync.busy
                            onClicked: {
                                var u = peerInput.text.trim()
                                if (u.length === 0) { syncStatus.text = "请先填对端地址"; return }
                                sync.pullFrom(u)
                            }
                        }
                    }
                    Rectangle {
                        width: 96; height: 32; radius: 16
                        color: sync.busy ? theme.hoverColor : theme.cardColor
                        border.color: theme.borderColor; border.width: 1
                        Text { anchors.centerIn: parent; text: "推送到对端"; color: theme.textColor; font.pixelSize: 12 }
                        MouseArea {
                            anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                            enabled: !sync.busy
                            onClicked: {
                                var u = peerInput.text.trim()
                                if (u.length === 0) { syncStatus.text = "请先填对端地址"; return }
                                sync.pushTo(u)
                            }
                        }
                    }
                }

                Connections {
                    target: sync
                    function onSyncFinished(added, detail) { syncStatus.text = "✓ " + detail }
                    function onSyncFailed(error) { syncStatus.text = "✗ " + error }
                }
                Connections {
                    target: lxsync
                    function onSyncEvent(text) { syncStatus.text = "🔗 " + text }
                }
                Text { id: syncStatus; Layout.fillWidth: true; text: ""; color: theme.subTextColor
                       font.pixelSize: 11; wrapMode: Text.Wrap }

                Rectangle { Layout.fillWidth: true; height: 1; color: theme.borderColor }

                // ---- 洛雪音乐同步兼容 ----
                Text { text: "洛雪音乐（移动版/桌面版）同步"; color: theme.textColor; font.pixelSize: 14; font.bold: true }
                Text {
                    Layout.fillWidth: true
                    text: "开启上方同步服务后，本端口自动兼容洛雪同步协议。在手机「洛雪音乐 → 我的 → 设备同步」填下面的本机地址并输入配对码，即可双向同步在线收藏与歌单（本地歌曲不参与）。配对码每 3 分钟自动更换。"
                    color: theme.subTextColor; font.pixelSize: 11; wrapMode: Text.Wrap
                }
                RowLayout {
                    visible: sync.enabled
                    spacing: 10
                    ColumnLayout {
                        spacing: 2
                        Text { text: "同步地址"; color: theme.subTextColor; font.pixelSize: 11 }
                        Text { text: sync.shareUrl; color: theme.textColor; font.pixelSize: 14; font.bold: true }
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        height: 44; radius: 8; color: theme.cardColor
                        border.color: theme.borderColor; border.width: 1
                        ColumnLayout {
                            anchors.centerIn: parent
                            spacing: 0
                            Text {
                                Layout.alignment: Qt.AlignHCenter
                                text: lxsync.authCode
                                color: theme.accentColor; font.pixelSize: 20; font.bold: true; font.letterSpacing: 4
                            }
                            Text { Layout.alignment: Qt.AlignHCenter; text: "配对码"; color: theme.subTextColor; font.pixelSize: 10 }
                        }
                    }
                    Rectangle {
                        width: 72; height: 40; radius: 8
                        color: lxCodeHover.containsMouse ? theme.hoverColor : theme.cardColor
                        border.color: theme.borderColor; border.width: 1
                        Text { anchors.centerIn: parent; text: "换一个"; color: theme.textColor; font.pixelSize: 12 }
                        MouseArea {
                            id: lxCodeHover
                            anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                            onClicked: lxsync.regenerateCode()
                        }
                    }
                }
                RowLayout {
                    visible: sync.enabled
                    spacing: 8
                    Text {
                        Layout.fillWidth: true
                        text: "已配对设备（在线 " + lxsync.onlineCount + "）"
                        color: theme.textColor; font.pixelSize: 12; font.bold: true
                    }
                }
                Repeater {
                    visible: sync.enabled
                    model: lxsync.devices
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 8
                        Rectangle {
                            width: 8; height: 8; radius: 4
                            color: modelData.online ? "#4caf50" : theme.subTextColor
                        }
                        Text {
                            text: (modelData.isMobile ? "📱 " : "💻 ") + modelData.deviceName + (modelData.online ? "（在线）" : "")
                            color: theme.textColor; font.pixelSize: 12; Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                        Text {
                            text: "删除"
                            color: lxDevHover.containsMouse ? theme.accentColor : theme.subTextColor
                            font.pixelSize: 12
                            MouseArea {
                                id: lxDevHover
                                anchors.fill: parent; anchors.margins: -4
                                hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                onClicked: lxsync.removeDevice(modelData.clientId)
                            }
                        }
                    }
                }
                Text {
                    visible: sync.enabled && lxsync.devices.length === 0
                    Layout.fillWidth: true
                    text: "（还没有设备配对：在手机上输入上方地址与配对码即可）"
                    color: theme.subTextColor; font.pixelSize: 11
                }
                Connections {
                    target: lxsync
                    function onSyncEvent(text) { syncStatus.text = text }
                }
            }

            // ---------- 关于 ----------
            ColumnLayout {
                visible: panel.section === "about"
                Layout.fillWidth: true
                spacing: 10

                Text { text: "暮云音乐"; color: theme.textColor; font.pixelSize: 16; font.bold: true }
                Text { text: "版本 v" + appVersion; color: theme.subTextColor; font.pixelSize: 12 }
                Text { text: "数据目录：" + settings.dataPath(); color: theme.subTextColor; font.pixelSize: 11; wrapMode: Text.Wrap; Layout.fillWidth: true }
                Text { text: "缓存占用：" + panel.fmtSize(settings.cacheSize()); color: theme.subTextColor; font.pixelSize: 12 }

                Rectangle {
                    width: 76; height: 30; radius: 15
                    color: ccHover.containsMouse ? theme.hoverColor : theme.cardColor
                    border.color: theme.borderColor
                    border.width: 1
                    Text { anchors.centerIn: parent; text: "清除缓存"; color: theme.textColor; font.pixelSize: 12 }
                    MouseArea {
                        id: ccHover
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: settings.clearCache()
                    }
                }

                // 关闭窗口行为（询问 / 最小化到托盘 / 直接退出）
                Text { text: "关闭窗口时"; color: theme.textColor; font.pixelSize: 14; font.bold: true
                       Layout.topMargin: 8 }
                Text { text: "音乐软件可最小化到系统托盘后台播放，从托盘图标右键退出。"
                       color: theme.subTextColor; font.pixelSize: 11; wrapMode: Text.Wrap; Layout.fillWidth: true }
                RowLayout {
                    spacing: 8
                    Repeater {
                        model: [
                            { id: "ask",      name: "每次询问" },
                            { id: "minimize", name: "最小化到托盘" },
                            { id: "exit",     name: "直接退出" }
                        ]
                        delegate: Rectangle {
                            id: optItem
                            required property var modelData
                            property bool isSel: settings.exitAction === modelData.id
                            width: optText.width + 28
                            height: 32
                            radius: 16
                            color: isSel ? theme.activeColor
                                         : (optHover.containsMouse ? theme.hoverColor : theme.cardColor)
                            border.color: isSel ? theme.accentColor : theme.borderColor
                            border.width: 1
                            Text {
                                id: optText
                                anchors.centerIn: parent
                                text: parent.modelData.name
                                color: parent.isSel ? theme.accentColor : theme.textColor
                                font.pixelSize: 12
                            }
                            MouseArea {
                                id: optHover
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: settings.exitAction = optItem.modelData.id
                            }
                        }
                    }
                }

                // 版本更新（开源方案 A：纯 GitHub 版本清单，见 core/update/UpdateChecker）
                Text { text: "版本更新"; color: theme.textColor; font.pixelSize: 14; font.bold: true
                       Layout.topMargin: 8 }
                Text { text: "启动时后台静默检查 GitHub 上的版本清单（每天最多一次），检查失败不影响使用。"
                       color: theme.subTextColor; font.pixelSize: 11; wrapMode: Text.Wrap; Layout.fillWidth: true }
                RowLayout {
                    spacing: 10

                    Rectangle {
                        id: checkUpdBtn
                        width: updText.width + 28; height: 32; radius: 16
                        color: updHover.containsMouse ? theme.hoverColor : theme.cardColor
                        border.color: theme.borderColor
                        border.width: 1
                        Text {
                            id: updText
                            anchors.centerIn: parent
                            text: updater.checking ? "检查中…" : "检查更新"
                            color: theme.textColor; font.pixelSize: 12
                        }
                        MouseArea {
                            id: updHover
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            objectName: "updateCheckBtn"
                            onClicked: updater.checkForUpdates()
                        }
                    }

                    Text {
                        objectName: "updateStatusText"
                        Layout.fillWidth: true
                        text: updater.statusText
                        color: updater.updateAvailable ? theme.accentColor : theme.subTextColor
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                }

                // 启动时自动检查（开/关两态胶囊；关掉后仍可手动检查）
                RowLayout {
                    spacing: 8
                    Text { text: "启动时自动检查"; color: theme.subTextColor; font.pixelSize: 12 }
                    Repeater {
                        model: [
                            { id: true,  name: "开" },
                            { id: false, name: "关" }
                        ]
                        delegate: Rectangle {
                            id: autoItem
                            required property var modelData
                            property bool isSel: updater.autoCheckEnabled === modelData.id
                            objectName: "updateAutoSwitch"
                            width: 44; height: 28; radius: 14
                            color: isSel ? theme.activeColor
                                         : (autoHover.containsMouse ? theme.hoverColor : theme.cardColor)
                            border.color: isSel ? theme.accentColor : theme.borderColor
                            border.width: 1
                            Text {
                                anchors.centerIn: parent
                                text: autoItem.modelData.name
                                color: autoItem.isSel ? theme.accentColor : theme.textColor
                                font.pixelSize: 12
                            }
                            MouseArea {
                                id: autoHover
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: updater.autoCheckEnabled = autoItem.modelData.id
                            }
                        }
                    }
                }

                Item { Layout.fillHeight: true }
            }

            // ---------- 播放界面（全屏歌词/播放页三种样式）----------
            ColumnLayout {
                visible: panel.section === "player"
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 12

                Text {
                    text: "点击播放条封面向上展开的播放页样式。也可在播放页顶部随时切换。"
                    color: theme.subTextColor; font.pixelSize: 11; wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 14
                    rowSpacing: 14
                    Repeater {
                        model: [
                            { id: "classic",   name: "经典",
                              desc: "封面、控制和歌词分栏显示", icon: "play-list" },
                            { id: "amll",      name: "Apple Music",
                              desc: "动态背景与逐字歌词", icon: "music" },
                            { id: "mineradio", name: "Mineradio",
                              desc: "封面粒子、舞台歌词与电影镜头", icon: "star" }
                        ]
                        delegate: Rectangle {
                            id: styleCard
                            required property var modelData
                            property bool isSel: settings.playerStyle === modelData.id
                            Layout.fillWidth: true
                            Layout.preferredHeight: 96
                            radius: 12
                            color: isSel ? theme.activeColor
                                         : (scHover.containsMouse ? theme.hoverColor : theme.cardColor)
                            border.color: isSel ? theme.accentColor : theme.borderColor
                            border.width: isSel ? 2 : 1
                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 14
                                spacing: 6
                                Icon {
                                    name: styleCard.modelData.icon
                                    iconSize: 20
                                    iconColor: styleCard.isSel ? theme.accentColor : theme.textColor
                                }
                                Text {
                                    text: styleCard.modelData.name
                                    color: styleCard.isSel ? theme.accentColor : theme.textColor
                                    font.pixelSize: 14; font.bold: true
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: styleCard.modelData.desc
                                    color: theme.subTextColor; font.pixelSize: 11
                                    wrapMode: Text.Wrap
                                }
                            }
                            MouseArea {
                                id: scHover
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: settings.playerStyle = styleCard.modelData.id
                            }
                        }
                    }
                }

                Item { Layout.fillHeight: true }
            }
        }
    }

    FolderDialog {
        id: folderPick
        title: "选择音乐目录"
        onAccepted: {
            var p = selectedFolder.toString()
            if (p.indexOf("file:///") === 0) p = p.substring(8)
            settings.addFolder(decodeURIComponent(p))
        }
    }

    FileDialog {
        id: scriptPick
        title: "选择 LX 音源脚本"
        fileMode: FileDialog.OpenFile
        nameFilters: ["JavaScript 文件 (*.js)", "所有文件 (*)"]
        onAccepted: {
            var p = selectedFile.toString()
            if (p.indexOf("file:///") === 0) p = p.substring(8)
            settings.importLxSourceFile(decodeURIComponent(p))
        }
    }
}
