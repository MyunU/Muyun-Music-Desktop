import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 歌曲列表行（列表页 / 搜索结果共用）
Rectangle {
    id: row
    property var song
    property int songIndex: 0
    property var contextList: []
    property string contextId: ""
    property string contextName: ""
    property bool showPlatform: true
    property bool removable: false   // 歌单详情页：允许从歌单移除
    // 批量选择模式（收藏/自建歌单页）：行点击=勾选/取消，行内的平台胶囊与喜欢先收起，
    // 否则它们会吃掉本该落在行上的点击（行级 MouseArea 在 z:-1 层）
    property bool batchMode: false
    property bool picked: false
    signal togglePicked()

    // 绑定 favoritesVersion，收藏变化时立即重新求值（否则要切页才刷新）
    property bool favState: {
        library.favoritesVersion
        return library.isFavorite(row.song)
    }

    width: ListView.view ? ListView.view.width : 0
    height: 60
    radius: 8
    color: row.picked ? Qt.alpha(theme.accentColor, 0.14)
                      : (hover.containsMouse ? theme.hoverColor : "transparent")

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 14

        // 序号位：批量模式换成勾选圈（RowLayout 会跳过 invisible 项，不留空位）
        Text {
            Layout.preferredWidth: 26
            visible: !row.batchMode
            text: (row.songIndex + 1)
            color: theme.subTextColor
            font.pixelSize: 12
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        Rectangle {
            Layout.preferredWidth: 20
            Layout.preferredHeight: 20
            Layout.alignment: Qt.AlignVCenter
            visible: row.batchMode
            radius: 10
            color: row.picked ? theme.accentColor : theme.bgColor
            border.color: row.picked ? theme.accentColor : theme.borderColor
            border.width: 1
            Text {
                anchors.centerIn: parent
                visible: row.picked
                text: "✓"
                color: "white"
                font.pixelSize: 13
                font.bold: true
            }
        }

        // 封面
        Item {
            Layout.preferredWidth: 44
            Layout.preferredHeight: 44

            Rectangle {
                anchors.fill: parent
                radius: 6
                color: theme.cardColor
                clip: true

                Image {
                    anchors.fill: parent
                    source: (row.song && row.song.cover) ? row.song.cover : ""
                    sourceSize: Qt.size(120, 120)
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    visible: status === Image.Ready
                }
                Icon {
                    anchors.centerIn: parent
                    name: "music"
                    iconSize: 18
                    iconColor: theme.subTextColor
                    visible: !(row.song && row.song.cover)
                }
            }

            // hover 播放遮罩（批量模式下不提示播放，点击是勾选）
            Rectangle {
                anchors.fill: parent
                radius: 6
                color: "#99000000"
                visible: hover.containsMouse && !row.batchMode
                Icon {
                    anchors.centerIn: parent
                    name: "play"
                    iconSize: 16
                    iconColor: "#ffffff"
                }
            }
        }

        // 歌名 / 歌手
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 3

            Text {
                Layout.fillWidth: true
                text: (row.song && row.song.name) ? row.song.name : "未知歌曲"
                color: theme.textColor
                font.pixelSize: 13
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                text: {
                    if (!row.song) return ""
                    var a = row.song.artist || "未知歌手"
                    var b = row.song.album || ""
                    return b.length > 0 ? (a + " · " + b) : a
                }
                color: theme.subTextColor
                font.pixelSize: 11
                elide: Text.ElideRight
            }
        }

        // 平台标签 = 点击为"这一首歌"选择取源平台（菜单），只改这一首。
        // 改完直接显示所选平台（视作它的平台），不加特殊标记。
        Rectangle {
            id: platTag
            visible: !row.batchMode          // 批量模式收起，避免吃掉本该落在行上的点击
            readonly property string effName: { player.songPlatformVersion;
                                                 return row.song ? player.songPlatformName(row.song) : "" }
            Layout.preferredWidth: 58
            Layout.preferredHeight: 20
            radius: 10
            color: platMouse.containsMouse ? Qt.alpha(theme.accentColor, 0.16) : theme.hoverColor
            Behavior on color { ColorAnimation { duration: 120 } }

            Text {
                anchors.centerIn: parent
                text: platTag.effName
                color: theme.subTextColor
                font.pixelSize: 10
            }
            MouseArea {
                id: platMouse
                anchors.fill: parent
                anchors.margins: -3
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                acceptedButtons: Qt.LeftButton
                onClicked: {
                    if (!row.song || row.song.platform === "local") return
                    platMenu.targetSong = row.song
                    platMenu.popup()
                }
            }
        }

        // 时长
        Text {
            Layout.preferredWidth: 44
            text: library.formatDuration(row.song ? (row.song.duration || 0) : 0)
            color: theme.subTextColor
            font.pixelSize: 12
            horizontalAlignment: Text.AlignRight
        }

        // 收藏
        Item {
            Layout.preferredWidth: 26
            Layout.preferredHeight: 26
            visible: !row.batchMode          // 同上：批量模式只留勾选

            Icon {
                anchors.centerIn: parent
                name: row.favState ? "heart-filled" : "heart"
                iconSize: 16
                iconColor: row.favState ? "#ff4d4f" : theme.subTextColor
            }
            MouseArea {
                anchors.fill: parent
                onClicked: library.toggleFavorite(row.song)
            }
        }
    }

    // 取源平台选择菜单：只作用于当前点击的这一首歌
    Menu {
        id: platMenu
        property var targetSong: null
        width: 160
        padding: 6
        palette.text: theme.textColor
        palette.windowText: theme.textColor
        palette.window: theme.cardColor
        palette.base: theme.cardColor
        palette.highlight: theme.hoverColor
        palette.highlightedText: theme.textColor
        background: Rectangle {
            color: theme.cardColor
            radius: 10
            border.color: theme.borderColor
        }

        MenuItem {
            id: miAuto
            text: "自动（歌曲原平台）"
            height: 34
            background: Rectangle {
                radius: 6; anchors.fill: parent; anchors.margins: 2
                color: miAuto.hovered ? theme.hoverColor : "transparent"
            }
            onTriggered: {
                if (platMenu.targetSong) player.setSongPlatform(platMenu.targetSong, "")
                platMenu.close()
            }
        }
        Repeater {
            model: player.songPlatformOptions()
            MenuItem {
                required property var modelData
                id: miPlat
                text: modelData.name
                      + (platMenu.targetSong && player.songPlatform(platMenu.targetSong) === modelData.id
                         ? "  ✓" : "")
                height: 34
                background: Rectangle {
                    radius: 6; anchors.fill: parent; anchors.margins: 2
                    color: miPlat.hovered ? theme.hoverColor : "transparent"
                }
                onTriggered: {
                    if (platMenu.targetSong) player.setSongPlatform(platMenu.targetSong, modelData.id)
                    platMenu.close()
                }
            }
        }
    }

    // 右键菜单（常见音乐软件样式）
    Menu {
        id: contextMenu
        property var menuSong: null
        width: 180
        padding: 6
        palette.text: theme.textColor
        palette.windowText: theme.textColor
        palette.window: theme.cardColor
        palette.base: theme.cardColor
        palette.highlight: theme.hoverColor
        palette.highlightedText: theme.textColor
        background: Rectangle {
            color: theme.cardColor
            radius: 10
            border.color: theme.borderColor
        }

        MenuItem {
            id: miNext
            text: "下一首播放"
            height: 34
            background: Rectangle {
                radius: 6
                anchors.fill: parent
                anchors.margins: 2
                color: miNext.hovered ? theme.hoverColor : "transparent"
            }
            onTriggered: {
                contextMenu.close()
                player.insertNext(contextMenu.menuSong)
                settings.toast("已添加到下一首播放")
            }
        }
        MenuItem {
            id: miFav
            text: row.favState ? "取消喜欢" : "喜欢"
            height: 34
            background: Rectangle {
                radius: 6
                anchors.fill: parent
                anchors.margins: 2
                color: miFav.hovered ? theme.hoverColor : "transparent"
            }
            onTriggered: {
                contextMenu.close()
                library.toggleFavorite(contextMenu.menuSong)
                settings.toast(row.favState ? "已取消喜欢" : "已添加到喜欢")
            }
        }
        MenuItem {
            id: miDownload
            text: "下载…"
            visible: row.song && row.song.platform !== "local"
            height: visible ? 34 : 0   // Controls Menu 隐藏项仍占布局高度，需显式折叠
            background: Rectangle {
                radius: 6
                anchors.fill: parent
                anchors.margins: 2
                color: miDownload.hovered ? theme.hoverColor : "transparent"
            }
            onTriggered: {
                downloadPicker.pendingSong = contextMenu.menuSong
                contextMenu.close()
                downloadPicker.open()
            }
        }
        MenuItem {
            id: miRemove
            visible: row.removable
            text: "从歌单移除"
            height: visible ? 34 : 0   // 同上：不折叠会在菜单里留大片空白
            background: Rectangle {
                radius: 6
                anchors.fill: parent
                anchors.margins: 2
                color: miRemove.hovered ? theme.hoverColor : "transparent"
            }
            onTriggered: {
                contextMenu.close()
                // 页内搜索过滤后 songIndex 是"视图下标"，须映射回原数组下标（对象引用同一份）
                var realIdx = row.contextList ? row.contextList.indexOf(row.song) : -1
                library.removeFromPlaylist(row.contextId, realIdx >= 0 ? realIdx : row.songIndex)
                settings.toast("已从歌单移除")
            }
        }
        MenuItem {
            id: miLocalDel
            visible: row.song && row.song.platform === "local"
            text: "从本地库删除…"
            height: visible ? 34 : 0   // 同上
            background: Rectangle {
                radius: 6
                anchors.fill: parent
                anchors.margins: 2
                color: miLocalDel.hovered ? theme.hoverColor : "transparent"
            }
            onTriggered: {
                localDeleteAsk.pendingSong = contextMenu.menuSong
                contextMenu.close()
                localDeleteAsk.open()
            }
        }
        MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: theme.borderColor } }
        MenuItem {
            id: miAdd
            text: "添加到歌单"
            height: 34
            background: Rectangle {
                radius: 6
                anchors.fill: parent
                anchors.margins: 2
                color: miAdd.hovered ? theme.hoverColor : "transparent"
            }
            onTriggered: {
                playlistPicker.pendingSong = contextMenu.menuSong
                contextMenu.close()
                playlistPicker.open()
            }
        }
    }

    // 本地歌曲删除确认：删文件 / 仅从列表移除（扫描不再回收，文件保留磁盘）
    Popup {
        id: localDeleteAsk
        property var pendingSong: null
        anchors.centerIn: Overlay.overlay
        modal: true
        dim: true
        width: 340
        padding: 16
        background: Rectangle {
            color: theme.cardColor
            radius: 12
            border.color: theme.borderColor
        }
        contentItem: ColumnLayout {
            spacing: 10
            Text {
                text: "删除本地歌曲"
                color: theme.textColor
                font.pixelSize: 14
                font.bold: true
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: "「" + (localDeleteAsk.pendingSong ? localDeleteAsk.pendingSong.name : "")
                      + "」要如何处理？"
                      + "\n「删除文件」会删掉磁盘上的音频；「仅移除列表」保留文件但不再显示（重新扫描也不会回收）。"
                color: theme.subTextColor
                font.pixelSize: 12
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 4
                spacing: 8
                Item { Layout.fillWidth: true }
                Rectangle {
                    width: delCancelText.implicitWidth + 28
                    height: 32
                    radius: 16
                    color: delCancelMa.containsMouse ? theme.hoverColor : "transparent"
                    border.color: theme.borderColor
                    border.width: 1
                    Text {
                        id: delCancelText
                        anchors.centerIn: parent
                        text: "取消"
                        color: theme.textColor
                        font.pixelSize: 12
                    }
                    MouseArea {
                        id: delCancelMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: localDeleteAsk.close()
                    }
                }
                Rectangle {
                    width: delListText.implicitWidth + 28
                    height: 32
                    radius: 16
                    color: delListMa.containsMouse ? theme.hoverColor : theme.bgColor
                    border.color: theme.borderColor
                    border.width: 1
                    Text {
                        id: delListText
                        anchors.centerIn: parent
                        text: "仅移除列表"
                        color: theme.textColor
                        font.pixelSize: 12
                    }
                    MouseArea {
                        id: delListMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            library.removeLocalSong(localDeleteAsk.pendingSong, false)
                            localDeleteAsk.close()
                            settings.toast("已从列表移除（文件保留在磁盘）")
                        }
                    }
                }
                Rectangle {
                    width: delFileText.implicitWidth + 28
                    height: 32
                    radius: 16
                    color: delFileMa.containsMouse ? "#cc3b36" : "#e04a44"
                    Text {
                        id: delFileText
                        anchors.centerIn: parent
                        text: "删除文件"
                        color: "white"
                        font.pixelSize: 12
                    }
                    MouseArea {
                        id: delFileMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            // 正在播放该曲 → 先停止释放文件句柄，再删
                            var cur = player.currentSong
                            if (cur && cur.localPath && localDeleteAsk.pendingSong
                                && cur.localPath === localDeleteAsk.pendingSong.localPath)
                                player.stop()
                            if (library.removeLocalSong(localDeleteAsk.pendingSong, true))
                                settings.toast("已删除文件")
                            else
                                settings.toast("文件删除失败：正在播放或无权限")
                            localDeleteAsk.close()
                        }
                    }
                }
            }
        }
    }

    // 添加到歌单选择器（独立居中弹窗，避免 Controls 嵌套子菜单的样式坑）
    Popup {
        id: playlistPicker
        property var pendingSong: null
        property bool creating: false
        anchors.centerIn: Overlay.overlay
        modal: true
        dim: true
        width: 300
        padding: 14
        onClosed: creating = false

        function doCreate() {
            var name = newPlInput.text.trim()
            if (name.length === 0) {
                // 空名自动编号：新建歌单、新建歌单1、新建歌单2...
                var n = 1, base = "新建歌单"
                var names = []
                for (var i = 0; i < library.playlists.length; i++)
                    names.push(library.playlists[i].name)
                name = base
                while (names.indexOf(name) >= 0) { name = base + n; n++ }
            }
            var pid = library.createPlaylist(name)
            library.addToPlaylist(pid, playlistPicker.pendingSong)
            playlistPicker.close()
            settings.toast("已创建「" + name + "」并添加歌曲")
        }
        background: Rectangle {
            color: theme.cardColor
            radius: 12
            border.color: theme.borderColor
        }
        contentItem: ColumnLayout {
            spacing: 10
            Text {
                text: "添加到歌单"
                color: theme.textColor
                font.pixelSize: 14
                font.bold: true
            }
            Repeater {
                model: library.playlists
                delegate: Rectangle {
                    id: plRow
                    required property var modelData
                    Layout.fillWidth: true
                    height: 38
                    radius: 8
                    color: plMouse.containsMouse ? theme.hoverColor : theme.bgColor
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        text: plRow.modelData.name
                        color: theme.textColor
                        font.pixelSize: 12
                        elide: Text.ElideRight
                        width: parent.width - 24
                    }
                    MouseArea {
                        id: plMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            library.addToPlaylist(plRow.modelData.id, playlistPicker.pendingSong)
                            playlistPicker.close()
                            settings.toast("已添加到「" + plRow.modelData.name + "」")
                        }
                    }
                }
            }
            Text {
                visible: library.playlists.length === 0
                text: "还没有歌单，点击下方新建"
                color: theme.subTextColor
                font.pixelSize: 11
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: theme.borderColor }

            // 新建歌单（可自定义名称）：点击按钮展开输入框
            Rectangle {
                id: newPlRow
                visible: !playlistPicker.creating
                Layout.fillWidth: true
                height: 38
                radius: 8
                color: newPlMouse.containsMouse ? theme.hoverColor : theme.bgColor
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    text: "新建歌单并添加"
                    color: theme.accentColor
                    font.pixelSize: 12
                }
                MouseArea {
                    id: newPlMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        playlistPicker.creating = true
                        newPlInput.text = ""
                        newPlInput.forceActiveFocus()
                    }
                }
            }

            // 输入歌单名（内联）
            ColumnLayout {
                visible: playlistPicker.creating
                Layout.fillWidth: true
                spacing: 8

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 36
                    radius: 8
                    color: theme.bgColor
                    border.color: newPlInput.activeFocus ? theme.accentColor : theme.borderColor
                    border.width: 1
                    TextInput {
                        id: newPlInput
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        verticalAlignment: Text.AlignVCenter
                        color: theme.textColor
                        font.pixelSize: 12
                        selectByMouse: true
                        clip: true
                        onAccepted: playlistPicker.doCreate()
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "输入歌单名称"
                            color: theme.subTextColor
                            font.pixelSize: 12
                            visible: newPlInput.text.length === 0
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Item { Layout.fillWidth: true }
                    Rectangle {
                        width: 62; height: 30; radius: 15
                        color: cancelCrHover.containsMouse ? theme.hoverColor : theme.cardColor
                        border.color: theme.borderColor
                        border.width: 1
                        Text { anchors.centerIn: parent; text: "取消"; color: theme.textColor; font.pixelSize: 12 }
                        MouseArea {
                            id: cancelCrHover
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: playlistPicker.creating = false
                        }
                    }
                    Rectangle {
                        width: 62; height: 30; radius: 15
                        color: createCrHover.containsMouse ? Qt.lighter(theme.accentColor, 1.15) : theme.accentColor
                        Text { anchors.centerIn: parent; text: "创建"; color: "white"; font.pixelSize: 12 }
                        MouseArea {
                            id: createCrHover
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: playlistPicker.doCreate()
                        }
                    }
                }
            }
        }
    }

    // 下载音质选择弹窗（右键"下载…" → 挑一档 → 加入队列）
    Popup {
        id: downloadPicker
        property var pendingSong: null
        property string pendingKey: ""   // 弹窗这首歌的身份键（songKey 隔离探测结果）
        property string selectedQuality: downloads.downloadQuality
        property var sizes: ({})   // qualityId → bytes（-1 不可得；无键=探测中）
        anchors.centerIn: Overlay.overlay
        modal: true
        dim: true
        width: 340
        padding: 16

        background: Rectangle {
            color: theme.cardColor
            radius: 12
            border.color: theme.borderColor
        }

        Connections {
            target: downloads
            function onQualitySizeReady(songKey, qid, bytes) {
                if (!downloadPicker.visible) return
                // 只认当前弹窗这首歌的探测结果（songKey 隔离，防串歌）
                if (pendingSong && songKey !== downloadPicker.pendingKey) return
                var s = downloadPicker.sizes
                s[qid] = bytes
                downloadPicker.sizes = s
            }
        }

        onAboutToShow: {
            // 每次打开时同步为全局默认（用户可临时改这一首的目标档）
            selectedQuality = downloads.downloadQuality
            sizes = ({})
            if (pendingSong) {
                downloadPicker.pendingKey = pendingSong.platform === "local"
                    ? ("local:" + (pendingSong.localPath || ""))
                    : (((pendingSong.lx && pendingSong.lx.source) ? pendingSong.lx.source : pendingSong.platform) + ":" + pendingSong.id)
                downloads.probeSizes(pendingSong)
            }
        }

        contentItem: ColumnLayout {
            spacing: 12

            Text {
                Layout.fillWidth: true
                text: "下载音质"
                color: theme.textColor
                font.pixelSize: 14
                font.bold: true
            }
            Text {
                Layout.fillWidth: true
                text: (downloadPicker.pendingSong && downloadPicker.pendingSong.name)
                      ? downloadPicker.pendingSong.name + (downloadPicker.pendingSong.artist
                        ? " - " + downloadPicker.pendingSong.artist : "")
                      : ""
                color: theme.subTextColor
                font.pixelSize: 11
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                text: "解析失败会自动换源/降档，直到拿到有效音频"
                color: theme.subTextColor
                font.pixelSize: 10
                wrapMode: Text.Wrap
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 4
                columnSpacing: 8
                rowSpacing: 8
                Repeater {
                    model: downloads.qualityOptions()
                    delegate: Rectangle {
                        id: qPick
                        required property var modelData
                        required property int index
                        property bool isCurrent: downloadPicker.selectedQuality === modelData.id
                        property var sizeVal: downloadPicker.sizes[modelData.id]
                        Layout.fillWidth: true
                        Layout.preferredHeight: 42
                        radius: 8
                        color: qPick.isCurrent ? theme.accentColor
                                               : (qPickHover.containsMouse ? theme.hoverColor : theme.bgColor)
                        border.color: qPick.isCurrent ? theme.accentColor : theme.borderColor
                        border.width: 1
                        Column {
                            anchors.centerIn: parent
                            spacing: 1
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: qPick.modelData.name
                                color: qPick.isCurrent ? "white" : theme.textColor
                                font.pixelSize: 11
                                font.bold: qPick.isCurrent
                            }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: qPick.sizeVal === undefined ? "…"
                                     : (qPick.sizeVal > 0 ? downloads.formatBytes(qPick.sizeVal) : "—")
                                color: qPick.isCurrent ? "#eeffffff" : theme.subTextColor
                                font.pixelSize: 9
                            }
                        }
                        MouseArea {
                            id: qPickHover
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: downloadPicker.selectedQuality = qPick.modelData.id
                        }
                    }
                }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: theme.borderColor }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Text {
                    Layout.fillWidth: true
                    text: "目标目录：" + downloads.downloadPath
                    color: theme.subTextColor
                    font.pixelSize: 10
                    elide: Text.ElideMiddle
                }
                Rectangle {
                    width: 62; height: 30; radius: 15
                    color: dlCancelHover.containsMouse ? theme.hoverColor : theme.cardColor
                    border.color: theme.borderColor
                    border.width: 1
                    Text { anchors.centerIn: parent; text: "取消"; color: theme.textColor; font.pixelSize: 12 }
                    MouseArea {
                        id: dlCancelHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: downloadPicker.close()
                    }
                }
                Rectangle {
                    width: 76; height: 30; radius: 15
                    color: dlGoHover.containsMouse ? Qt.lighter(theme.accentColor, 1.15) : theme.accentColor
                    Text { anchors.centerIn: parent; text: "开始下载"; color: "white"; font.pixelSize: 12 }
                    MouseArea {
                        id: dlGoHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            downloads.addDownload(downloadPicker.pendingSong,
                                                  downloadPicker.selectedQuality)
                            downloadPicker.close()
                        }
                    }
                }
            }
        }
    }

    // 行点击（播放）：放最底层，避免盖住右侧"喜欢"等可交互元素
    MouseArea {
        id: hover
        anchors.fill: parent
        z: -1
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        onClicked: {
            if (row.batchMode) { row.togglePicked(); return }   // 批量模式：点行=勾选/取消
            player.playSong(row.song, row.contextList, row.contextId, row.contextName)
            library.recordPlay(row.song)
        }
    }

    // 右键：独立层放最顶层、只接受右键（层级免疫，左键不受影响）
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        cursorShape: Qt.ArrowCursor
        onClicked: {
            contextMenu.menuSong = row.song
            contextMenu.popup()
        }
    }
}
