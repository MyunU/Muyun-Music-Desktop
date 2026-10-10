import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 底部播放条
Rectangle {
    id: bar
    Layout.fillWidth: true
    Layout.preferredHeight: 76
    color: theme.panelColor

    Rectangle { width: parent.width; height: 1; color: theme.borderColor }

    // 绑定 favoritesVersion，收藏变化立即刷新
    property bool favState: {
        library.favoritesVersion
        return library.isFavorite(player.currentSong)
    }

    // 从当前歌曲的 LX 元信息里取指定音质的文件大小（无则空串）；
    // lx.types 没带大小的平台，回退到 downloads.probeSizes 的 HEAD 探测结果。
    // ⚠ 缓存按"歌曲身份"隔离（songKey = platform:id / local:path），换曲即空——
    //   否则上一首探测到的大小会残留显示成新曲的（用户反馈：切歌未加载完显示上一首大小）。
    property string curSongKey: ""
    property var probedSizes: ({})   // songKey → { qualityId → bytes }
    function songKeyOf(song) {
        if (!song || !song.name) return ""
        if (song.platform === "local") return "local:" + (song.localPath || "")
        var src = (song.lx && song.lx.source) ? song.lx.source : song.platform
        return src + ":" + song.id
    }
    Connections {
        target: downloads
        function onQualitySizeReady(songKey, qid, bytes) {
            if (bytes <= 0) return
            if (songKey !== bar.curSongKey) return   // 已切歌，旧探测结果丢弃
            var outer = bar.probedSizes
            var s = outer[songKey] || {}
            s[qid] = bytes
            outer[songKey] = s
            bar.probedSizes = outer
        }
    }
    Connections {
        target: player
        function onCurrentSongChanged() {
            bar.curSongKey = bar.songKeyOf(player.currentSong)
        }
    }
    Component.onCompleted: bar.curSongKey = bar.songKeyOf(player.currentSong)
    function qualitySize(qid) {
        var lx = player.currentSong.lx
        if (lx && lx.types) {
            for (var i = 0; i < lx.types.length; i++)
                if (lx.types[i].type === qid) return lx.types[i].size
        }
        var bySong = bar.probedSizes[bar.curSongKey]
        var p = bySong ? bySong[qid] : undefined
        return p ? downloads.formatBytes(p) : ""
    }

    // ---------- 顶部进度条 ----------
    Rectangle {
        id: progressTrack
        property bool dragging: false
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: (hoverArea.containsMouse || progressTrack.dragging) ? 4 : 2
        color: theme.borderColor
        visible: player.duration > 0
        Behavior on height { NumberAnimation { duration: 120 } }

        Rectangle {
            width: player.duration > 0 ? parent.width * (player.position / player.duration) : 0
            height: parent.height
            color: theme.accentColor
        }

        Rectangle {
            x: player.duration > 0 ? parent.width * (player.position / player.duration) - width / 2 : 0
            anchors.verticalCenter: parent.verticalCenter
            width: 10; height: 10; radius: 5
            color: "white"
            visible: hoverArea.containsMouse || progressTrack.dragging
        }

        MouseArea {
            id: hoverArea
            anchors.fill: parent
            anchors.topMargin: -6
            anchors.bottomMargin: -6
            hoverEnabled: true
            onPressed: progressTrack.dragging = true
            onReleased: progressTrack.dragging = false
            onClicked: player.seekRatio(Math.max(0, Math.min(1, mouseX / progressTrack.width)))
            onPositionChanged: {
                if (progressTrack.dragging)
                    player.seekRatio(Math.max(0, Math.min(1, mouseX / progressTrack.width)))
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 18
        anchors.rightMargin: 18
        anchors.topMargin: 4
        spacing: 16

        // ================= 左侧：歌曲信息 =================
        // 按内容自适应宽度（歌名最长 170px 截断）——歌词紧贴其后显示，不留大段空白
        RowLayout {
            spacing: 12

            Rectangle {
                id: barCover
                Layout.preferredWidth: 46
                Layout.preferredHeight: 46
                radius: 8
                color: theme.cardColor
                clip: true

                Image {
                    anchors.fill: parent
                    source: player.currentSong.cover || ""
                    sourceSize: Qt.size(64, 64)
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    visible: status === Image.Ready
                }
                Icon {
                    anchors.centerIn: parent
                    visible: !player.currentSong.cover
                    name: "music"
                    iconSize: 20
                    iconColor: theme.subTextColor
                }
                // hover 提示：点击展开全屏播放页
                Rectangle {
                    anchors.fill: parent
                    radius: 8
                    color: "#66000000"
                    visible: coverHover.containsMouse && player.currentSong.name
                    Icon {
                        anchors.centerIn: parent
                        name: "window-max"
                        iconSize: 18
                        iconColor: "white"
                    }
                }
                MouseArea {
                    id: coverHover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.openLyricsPage()
                }
            }

            ColumnLayout {
                spacing: 2

                Text {
                    // #13：歌词位腾空后歌曲信息加宽
                    Layout.maximumWidth: 220
                    text: player.currentSong.name || "未在播放"
                    color: theme.textColor
                    font.pixelSize: 13
                    elide: Text.ElideRight
                }
                Text {
                    Layout.maximumWidth: 220
                    text: player.currentSong.artist || "搜索歌曲开始播放"
                    color: theme.subTextColor
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
                Rectangle {
                    id: qualityTag
                    width: qText.width + 12
                    height: 15
                    radius: 3
                    color: qHover.containsMouse ? theme.accentColor : theme.hoverColor
                    visible: player.currentSong.name
                    Text {
                        id: qText
                        anchors.centerIn: parent
                        text: player.currentQualityLabel
                        color: qHover.containsMouse ? "white" : theme.subTextColor
                        font.pixelSize: 9
                    }
                    MouseArea {
                        id: qHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: qualityMenu.open()
                    }

                    // 音质选择菜单（对齐 lx-music 官方 7 档）
                    Popup {
                        id: qualityMenu
                        y: -height - 8
                        x: (parent.width - width) / 2
                        width: 230
                        padding: 6
                        // 打开时探测各音质文件大小（lx.types 已带的会即时显示，缺的靠 HEAD 补）
                        onAboutToShow: {
                            if (player.currentSong && player.currentSong.name
                                && player.currentSong.platform !== "local")
                                downloads.probeSizes(player.currentSong)
                        }
                        background: Rectangle {
                            color: theme.cardColor
                            radius: 10
                            border.color: theme.borderColor
                        }
                        ColumnLayout {
                            width: parent.width
                            spacing: 0
                            Repeater {
                                model: [
                                    { id: "master",    title: "Master", sub: "母带音质" },
                                    { id: "atmos",     title: "Atmos",  sub: "空间音频" },
                                    { id: "hires",     title: "Hi-Res", sub: "高解析度无损" },
                                    { id: "flac24bit", title: "24Bit",  sub: "24bit FLAC" },
                                    { id: "flac",      title: "FLAC",   sub: "FLAC" },
                                    { id: "320k",      title: "320K",   sub: "320kbps" },
                                    { id: "128k",      title: "128K",   sub: "128kbps" }
                                ]
                                delegate: Item {
                                    id: qItem
                                    required property var modelData
                                    property bool isCurrent: player.quality === modelData.id
                                    Layout.fillWidth: true
                                    height: 44
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: 2
                                        radius: 6
                                        color: itemHover.containsMouse
                                               ? theme.hoverColor : "transparent"
                                    }
                                    ColumnLayout {
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.left: parent.left
                                        anchors.leftMargin: 12
                                        spacing: 1
                                        Text {
                                            text: modelData.title
                                            color: qItem.isCurrent
                                                   ? theme.accentColor
                                                   : (itemHover.containsMouse ? theme.textColor : theme.textColor)
                                            font.pixelSize: 13
                                            font.bold: qItem.isCurrent
                                        }
                                        Text {
                                            text: {
                                                var s = modelData.sub
                                                var size = bar.qualitySize(modelData.id)
                                                return size ? (s + " · " + size) : s
                                            }
                                            color: theme.subTextColor
                                            font.pixelSize: 10
                                        }
                                    }
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.right: parent.right
                                        anchors.rightMargin: 14
                                        text: "✓"
                                        color: theme.accentColor
                                        font.pixelSize: 14
                                        visible: qItem.isCurrent
                                    }
                                    MouseArea {
                                        id: itemHover
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            player.quality = modelData.id
                                            qualityMenu.close()
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                // 当前音质文件大小（用户反馈：直接写大小，别显示"实测 320k"那种码率字样）。
                // ⚠ 切歌时上一首的探测结果已按 songKey 隔离清空 → 新曲未就绪显「加载中」，
                //   绝不串成上一首的大小；本地歌不探测（无此信息）→ 隐藏。
                Text {
                    property bool isLocal: player.currentSong.platform === "local"
                    property string sizeText: {
                        bar.curSongKey   // 触发换曲重算
                        var q = player.quality
                        return bar.qualitySize(q).replace(" ", "")
                    }
                    visible: player.currentSong.name.length > 0 && !isLocal
                    text: sizeText.length > 0 ? sizeText : "加载中"
                    color: theme.subTextColor
                    font.pixelSize: 9
                }
            }
        }

        // ================= 实时歌词（#13：已搬到顶栏搜索框右侧，此处腾空） =================
        // （无内容：按钮组占位保留，避免右侧区延伸到控制组正下方）

        // 按钮组占位：控制组是居中浮层，布局需要为其左半预留宽度，
        // 否则右侧区会延伸到按钮组正下方（与按钮重叠）
        Item {
            Layout.preferredWidth: controlsRow.implicitWidth / 2 + 10
        }

        // ================= 右侧：音量等 =================
        // fillWidth + 前置 filler：内容真右对齐；滑条可压缩，窄窗口不溢出
        RowLayout {
            Layout.fillWidth: true
            spacing: 14

            Item { Layout.fillWidth: true }

            Text {
                text: player.duration > 0
                      ? (player.formatTime(player.position) + " / " + player.formatTime(player.duration))
                      : "--:-- / --:--"
                color: theme.subTextColor
                font.pixelSize: 10
                visible: bar.width > 820
            }

            RowLayout {
                spacing: 6
                IconButton {
                    name: player.muted ? "volume-mute" : "volume"
                    iconSize: 18
                    tip: "静音"
                    onClicked: player.muted = !player.muted
                }
                Slider {
                    id: volSlider
                    Layout.fillWidth: true
                    // #13：音量条加长（原来 84px 太短不好拖）；腾出的宽度主要给音量，
                    // 其余给歌曲信息（歌名/歌手宽度也在左侧同步加宽）
                    Layout.maximumWidth: 150
                    Layout.minimumWidth: 60
                    from: 0; to: 1
                    value: player.volume
                    onMoved: player.volume = value
                    background: Rectangle {
                        x: volSlider.leftPadding
                        y: volSlider.topPadding + volSlider.availableHeight / 2 - height / 2
                        width: volSlider.availableWidth
                        height: 3
                        radius: 2
                        color: theme.borderColor
                        Rectangle {
                            width: volSlider.visualPosition * parent.width
                            height: parent.height
                            radius: 2
                            color: theme.textColor
                        }
                    }
                    handle: Rectangle {
                        x: volSlider.leftPadding + volSlider.visualPosition * (volSlider.availableWidth - width)
                        y: volSlider.topPadding + volSlider.availableHeight / 2 - height / 2
                        width: 11; height: 11; radius: 6
                        color: "white"
                    }
                }
            }

            // 音效入口：推子图标 + 文字。
            // 以前这里是一颗"三个点"，没人知道点下去是音效面板（用户反馈），
            // 换成"图标+音效"文字，并且开启时整颗胶囊高亮，一眼看出状态。
            Rectangle {
                id: fxEntry
                objectName: "fxEntryObj"
                Layout.alignment: Qt.AlignVCenter
                implicitWidth: fxEntryRow.implicitWidth + 18
                implicitHeight: 26
                radius: 13
                color: fxEntryMa.containsMouse
                        ? theme.hoverColor
                        : (player.effectsOn
                           ? Qt.rgba(theme.accentColor.r, theme.accentColor.g,
                                     theme.accentColor.b, 0.14)
                           : "transparent")
                border.width: player.effectsOn ? 1 : 0
                border.color: theme.accentColor

                Row {
                    id: fxEntryRow
                    anchors.centerIn: parent
                    spacing: 5
                    Icon {
                        name: "effects"
                        iconSize: 15
                        anchors.verticalCenter: parent.verticalCenter
                        iconColor: player.effectsBypassed ? "#e0a34a"
                                     : (player.effectsOn ? theme.accentColor : theme.subTextColor)
                    }
                    Text {
                        id: fxEntryText
                        objectName: "fxEntryTextObj"
                        anchors.verticalCenter: parent.verticalCenter
                        visible: bar.width > 880
                        text: player.effectsBypassed ? "音效未生效" : "音效"
                        color: player.effectsBypassed ? "#e0a34a"
                                     : (player.effectsOn ? theme.accentColor : theme.subTextColor)
                        font.pixelSize: 12
                    }
                }

                MouseArea {
                    id: fxEntryMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.openEffects()
                }
                ToolTip.visible: fxEntryMa.containsMouse
                ToolTip.delay: 500
                ToolTip.text: player.effectsBypassed
                              ? "已开启音效，但当前音源无法解码，音效未生效"
                              : (player.effectsOn ? "音效（已生效，点按调节）" : "音效设置")
            }
        }
    }
    // ================= 中间：控制（锚定播放条正中，永不漂移） =================
    // 隐式宽度（不 fillWidth）：按钮组宽度恒定，不随两侧自适应区漂移
    RowLayout {
        id: controlsRow
        anchors.centerIn: parent
        Layout.alignment: Qt.AlignVCenter
        spacing: 8

        // 喜欢
        IconButton {
            name: bar.favState ? "heart-filled" : "heart"
            iconColor: bar.favState ? "#ff4d4f" : theme.subTextColor
            hoverColor: bar.favState ? "#ff7875" : theme.textColor
            tip: "喜欢"
            onClicked: if (player.currentSong.name) library.toggleFavorite(player.currentSong)
        }

        // 播放模式
        IconButton {
            id: playModeBtn
            objectName: "playModeBtn"       // 自检读它的 name，断言四种模式图标互不相同
            name: {
                switch (player.playMode) {
                case "loop": return "repeat"
                case "single": return "repeat-one"
                case "shuffle": return "shuffle"
                // 顺序播放以前也返回 "repeat"，和列表循环长得一模一样，
                // 用户完全看不出当前是哪种模式 → 换成"列表+向下箭头"的专属图标
                default: return "order"
                }
            }
            // 非顺序（有循环/随机语义）一律高亮，顺序保持常态色，一眼分得清
            iconColor: player.playMode === "sequence" ? theme.subTextColor : theme.accentColor
            tip: "播放模式：" + player.playModeName
            onClicked: player.cyclePlayMode()
        }

        // 上一首
        IconButton {
            name: "prev"
            iconSize: 22
            iconColor: theme.textColor
            tip: "上一首"
            onClicked: player.previous()
        }

        // 播放 / 暂停（深色圆底 + 白色图标，参照原版）
        Rectangle {
            width: 40; height: 40; radius: 20
            color: playHover.containsMouse ? theme.activeColor : theme.cardColor
            border.color: theme.borderColor
            border.width: 1

            Icon {
                anchors.centerIn: parent
                name: player.isPlaying ? "pause" : "play"
                iconSize: 18
                iconColor: theme.textColor
                visible: !player.isLoading
            }
            // 加载转圈（自定义弧线，深色底上清晰可见）
            Item {
                anchors.centerIn: parent
                width: 20; height: 20
                visible: player.isLoading
                RotationAnimation on rotation {
                    from: 0; to: 360; duration: 900; loops: Animation.Infinite
                }
                Canvas {
                    anchors.fill: parent
                    onPaint: {
                        var ctx = getContext("2d")
                        ctx.reset()
                        ctx.strokeStyle = theme.accentColor
                        ctx.lineWidth = 2.5
                        ctx.lineCap = "round"
                        ctx.beginPath()
                        ctx.arc(width / 2, height / 2, 8, 0, Math.PI * 1.25)
                        ctx.stroke()
                    }
                }
            }
            MouseArea {
                id: playHover
                anchors.fill: parent
                hoverEnabled: true
                onClicked: player.togglePlay()
            }
        }

        // 下一首
        IconButton {
            name: "next"
            iconSize: 22
            iconColor: theme.textColor
            tip: "下一首"
            onClicked: player.next()
        }

        // 队列
        IconButton {
            name: "queue"
            tip: "播放队列"
            onClicked: root.openQueueDrawer()
        }

        // 歌词
        IconButton {
            name: "mic"
            tip: "桌面歌词"
            onClicked: root.toggleDesktopLyrics()
        }
    }
}
