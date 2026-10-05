import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

// 全屏播放/歌词页：三种样式（settings.playerStyle）
//   classic    经典：封面 + 控制 + 歌词分栏
//   amll       Apple Music：动态模糊背景 + 逐字歌词居中
//   mineradio  舞台：电影镜头封面漂移 + 粒子 + 大字幕逐字歌词
// 对外契约：page.visible / page.open() / page.close()（Main.qml 快捷键依赖）
Item {
    id: page
    anchors.fill: parent
    visible: false
    z: 500

    property string style: settings.playerStyle
    property bool showTranslation: true
    property bool showRoman: false
    property bool controlsVisible: true

    property int activeIndex: -1
    property real activeProgress: 0

    // 真·舞台引擎接管中（独立进程渲染）：本页底层 QML 全部卸载省资源
    readonly property bool stageTakeover: page.style === "mineradio"
        && typeof stage !== "undefined" && stage.active

    property bool favState: {
        library.favoritesVersion
        return library.isFavorite(player.currentSong)
    }

    // 关闭完成（动画播完、visible=false）后通知外层，用于重建输入法上下文
    signal closed()

    function recompute() {
        var lines = player.lyricLines
        if (!lines || lines.length === 0) { activeIndex = -1; activeProgress = 0; return }
        var posSec = player.position / 1000.0
        var idx = -1
        for (var i = 0; i < lines.length; i++) {
            if (lines[i].time <= posSec) idx = i
            else break
        }
        activeIndex = idx
        if (idx < 0) { activeProgress = 0; return }
        var cur = lines[idx]
        if (cur.words && cur.words.length > 0) {
            var wStart = cur.words[0].startTime
            var last = cur.words[cur.words.length - 1]
            var wEnd = Math.max(last.endTime, wStart + 0.3)
            activeProgress = Math.min(1, Math.max(0, (posSec - wStart) / (wEnd - wStart)))
        } else {
            var nextTime = (idx + 1 < lines.length) ? lines[idx + 1].time : (player.duration / 1000.0)
            var span = Math.max(0.3, nextTime - cur.time)
            activeProgress = Math.min(1, Math.max(0, (posSec - cur.time) / span))
        }
    }

    function seekLine(i) {
        var lines = player.lyricLines
        if (!lines || i < 0 || i >= lines.length) return
        player.seek((lines[i].time || 0) * 1000)
        recompute()
    }

    Connections {
        target: player
        function onPositionChanged() { page.recompute() }
        function onLyricChanged() { page.recompute() }
        function onDurationChanged() { page.recompute() }
    }
    Connections {
        target: settings
        function onPlayerStyleChanged() { page.style = settings.playerStyle }
    }

    // ===== 背景（共用；舞台接管时连模糊一起关掉）=====
    Image {
        id: coverBg
        anchors.fill: parent
        anchors.margins: -100
        source: (page.stageTakeover || !player.currentSong || !player.currentSong.cover)
                ? "" : player.currentSong.cover
        sourceSize: Qt.size(900, 900)
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        visible: status === Image.Ready
    }
    // amll/mineradio：强模糊；classic：轻模糊
    MultiEffect {
        source: coverBg
        anchors.fill: coverBg
        blurEnabled: true
        blur: page.style === "classic" ? 0.3 : 0.6
        blurMax: page.style === "classic" ? 40 : 64
        brightness: page.style === "classic" ? -0.4 : (theme.dark ? -0.28 : -0.05)
        visible: coverBg.visible
    }
    Rectangle {
        anchors.fill: parent
        color: theme.dark ? "#dd101014" : "#ddf2f2f4"
        visible: !coverBg.visible
    }
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: theme.dark ? "#99000000" : "#88ffffff" }
            GradientStop { position: 0.5; color: "#33000000" }
            GradientStop { position: 1.0; color: theme.dark ? "#bb000000" : "#88ffffff" }
        }
    }

    // ===== 遮罩：点击/移动唤出控件 =====
    MouseArea {
        id: bgMouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: page.controlsVisible = !page.controlsVisible
        onPositionChanged: {
            if (!page.controlsVisible) page.controlsVisible = true
            hideTimer.restart()
        }
        Timer {
            id: hideTimer
            interval: 3200
            onTriggered: if (page.visible) page.controlsVisible = false
        }

        // 三模式（各自一个 Loader，只激活当前样式）
        // 三模式：单个 Loader，sourceComponent 二选一（保证同时只有一个实例）
        Loader {
            // 真·舞台引擎接管时整个卸载底层 lite 近似（省渲染与内存）
            active: !page.stageTakeover
            anchors.fill: parent
            sourceComponent: page.style === "classic" ? classicComp
                             : page.style === "amll" ? amllComp
                             : mineradioComp
        }

        // ===== 顶部控制条（classic/amll）=====
        Rectangle {
            id: topBar
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 56
            color: "transparent"
            visible: page.style !== "mineradio"
            opacity: page.controlsVisible ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 250 } }

            Row {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                spacing: 10
                layoutDirection: Qt.LeftToRight

                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    name: "chevron-down"
                    iconSize: 20
                    tip: "收起 (Esc)"
                    onClicked: page.close()
                }
            }
            // 左侧标题（amll）
            Text {
                anchors.left: topBar.left
                anchors.leftMargin: 56
                anchors.verticalCenter: parent.verticalCenter
                text: "正在播放"
                color: theme.subTextColor
                font.pixelSize: 12
                visible: page.style === "amll"
            }
            // 右侧：翻译开关
            Row {
                anchors.right: topBar.right
                anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10

                Rectangle {
                    width: 52; height: 26; radius: 13
                    color: page.showTranslation ? theme.accentColor : "transparent"
                    border.color: page.showTranslation ? theme.accentColor : theme.borderColor
                    border.width: 1
                    Text { anchors.centerIn: parent; text: "翻译"
                           color: page.showTranslation ? "white" : theme.subTextColor; font.pixelSize: 11 }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                onClicked: page.showTranslation = !page.showTranslation }
                }
            }
        }
    }

    // ==================================================================
    //  classic：封面/控制（左栏）+ 歌词（右栏）
    // ==================================================================
    Component {
        id: classicComp
        Item {
            id: classicRoot
            Rectangle {
                id: leftPanel
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.leftMargin: 24
                anchors.topMargin: 64
                anchors.bottomMargin: 16
                width: (classicRoot.width - 48) * 0.36
                color: "transparent"

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 14

                    Rectangle {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredWidth: Math.min(leftPanel.width - 48, 260)
                        Layout.preferredHeight: Layout.preferredWidth
                        radius: 16
                        color: theme.cardColor
                        clip: true
                        Image {
                            anchors.fill: parent
                            source: (player.currentSong && player.currentSong.cover) ? player.currentSong.cover : ""
                            sourceSize: Qt.size(400, 400)
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            visible: status === Image.Ready
                        }
                        Icon { anchors.centerIn: parent; name: "music"; iconSize: 48
                               iconColor: theme.subTextColor
                               visible: !(player.currentSong && player.currentSong.cover) }
                    }
                    Text {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        text: (player.currentSong && player.currentSong.name) ? player.currentSong.name : "未在播放"
                        color: theme.textColor; font.pixelSize: 18; font.bold: true; elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        text: (player.currentSong && player.currentSong.artist) ? player.currentSong.artist : ""
                        color: theme.subTextColor; font.pixelSize: 12; elide: Text.ElideRight
                    }
                    Item { Layout.fillHeight: true }

                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 2
                        Rectangle {
                            Layout.fillWidth: true; Layout.preferredHeight: 4; radius: 2; color: theme.borderColor
                            property real ratio: player.duration > 0 ? player.position / player.duration : 0
                            Rectangle {
                                anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                                width: parent.width * parent.ratio; radius: 2; color: theme.accentColor
                            }
                            MouseArea { anchors.fill: parent; anchors.margins: -8
                                        onClicked: player.seekRatio(Math.max(0, Math.min(1, mouseX / parent.width))) }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Text { text: player.formatTime(player.position); color: theme.subTextColor; font.pixelSize: 10 }
                            Item { Layout.fillWidth: true }
                            Text { text: player.formatTime(player.duration); color: theme.subTextColor; font.pixelSize: 10 }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 12
                        Item { Layout.fillWidth: true }
                        IconButton { name: "prev"; iconSize: 20; onClicked: player.previous() }
                        Rectangle {
                            Layout.preferredWidth: 46; Layout.preferredHeight: 46; radius: 23; color: theme.accentColor
                            Icon { anchors.centerIn: parent; name: player.isPlaying ? "pause" : "play"
                                   iconSize: 20; iconColor: "white" }
                            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                        onClicked: player.togglePlay() }
                        }
                        IconButton { name: "next"; iconSize: 20; onClicked: player.next() }
                        IconButton { name: page.favState ? "heart-filled" : "heart"
                                     iconColor: page.favState ? "#ff4d4f" : theme.subTextColor
                                     onClicked: library.toggleFavorite(player.currentSong) }
                        Item { Layout.fillWidth: true }
                    }
                }
            }

            Rectangle {
                anchors.left: leftPanel.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.topMargin: 64
                anchors.bottomMargin: 16
                width: 1; color: theme.borderColor; opacity: 0.5
            }

            ListView {
                id: classicList
                anchors.left: leftPanel.right
                anchors.leftMargin: 24
                anchors.right: parent.right
                anchors.rightMargin: 24
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.topMargin: 64
                anchors.bottomMargin: 16
                clip: true
                model: player.lyricLines
                spacing: 16
                boundsBehavior: Flickable.StopAtBounds
                property int lastIdx: -1
                onCountChanged: if (page.activeIndex >= 0) positionViewAtIndex(page.activeIndex, ListView.Center)
                Text {
                    anchors.centerIn: parent
                    text: player.lyricLoading ? "歌词加载中…" : "暂无歌词"
                    color: theme.subTextColor; font.pixelSize: 14
                    visible: parent.count === 0
                }
                Connections {
                    target: page
                    function onActiveIndexChanged() {
                        if (!page.visible || page.style !== "classic") return
                        if (activeIndex === classicList.lastIdx) return
                        classicList.lastIdx = activeIndex
                        classicList.positionViewAtIndex(activeIndex, ListView.Center)
                    }
                }
                delegate: Item {
                    required property var modelData
                    required property int index
                    width: classicList.width
                    height: lyricLine.implicitHeight
                    LyricLine {
                        id: lyricLine
                        anchors.horizontalCenter: parent.horizontalCenter
                        line: modelData
                        active: index === page.activeIndex
                        progress: index === page.activeIndex ? page.activeProgress : 0
                        size: active ? 22 : 17
                        textColor: theme.textColor
                        inactiveColor: theme.textColor
                        inactiveOpacity: 0.45
                        accentColor: theme.accentColor
                        showTranslation: page.showTranslation
                        showRoman: page.showRoman
                        karaoke: false
                        maxW: classicList.width
                        onClicked: page.seekLine(index)
                    }
                }
            }
        }
    }

    // ==================================================================
    //  amll：居中逐字大歌词 + 悬浮旋转唱片 + 底部控制
    // ==================================================================
    Component {
        id: amllComp
        Item {
            ColumnLayout {
                id: amlCol
                anchors.fill: parent
                anchors.topMargin: 56
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 16

                Item {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 110
                    Layout.preferredHeight: 110
                    Rectangle {
                        id: disc
                        anchors.fill: parent; radius: width / 2
                        color: theme.cardColor; clip: true
                        Image {
                            anchors.fill: parent
                            source: (player.currentSong && player.currentSong.cover) ? player.currentSong.cover : ""
                            sourceSize: Qt.size(400, 400)
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true; visible: status === Image.Ready
                        }
                        Icon { anchors.centerIn: parent; name: "music"; iconSize: 38
                               iconColor: theme.subTextColor
                               visible: !(player.currentSong && player.currentSong.cover) }
                        Rectangle {
                            anchors.centerIn: parent; width: 24; height: 24; radius: 12
                            color: theme.dark ? "#1a1a1a" : "#e0e0e0"
                        }
                        RotationAnimator {
                            target: disc; from: 0; to: 360; duration: 24000
                            loops: Animation.Infinite; running: player.isPlaying && page.visible
                        }
                    }
                }

                ListView {
                    id: amllList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: player.lyricLines
                    spacing: 22
                    boundsBehavior: Flickable.StopAtBounds
                    property int lastIdx: -1
                    onCountChanged: if (page.activeIndex >= 0) positionViewAtIndex(page.activeIndex, ListView.Center)
                    Text {
                        anchors.centerIn: parent
                        text: player.lyricLoading ? "歌词加载中…" : "暂无歌词"
                        color: theme.subTextColor; font.pixelSize: 15
                        visible: parent.count === 0
                    }
                    Connections {
                        target: page
                        function onActiveIndexChanged() {
                            if (!page.visible || page.style !== "amll") return
                            if (activeIndex === amllList.lastIdx) return
                            amllList.lastIdx = activeIndex
                            amllList.positionViewAtIndex(activeIndex, ListView.Center)
                        }
                    }
                    delegate: Item {
                        required property var modelData
                        required property int index
                        width: amllList.width
                        height: lyricLine.implicitHeight
                        LyricLine {
                            id: lyricLine
                            anchors.horizontalCenter: parent.horizontalCenter
                            line: modelData
                            active: index === page.activeIndex
                            progress: index === page.activeIndex ? page.activeProgress : 0
                            size: active ? 30 : 22
                        textColor: theme.textColor
                        inactiveColor: theme.textColor
                        inactiveOpacity: 0.35
                        accentColor: theme.accentColor
                        showTranslation: page.showTranslation
                        showRoman: page.showRoman
                        karaoke: false
                        maxW: amllList.width
                        onClicked: page.seekLine(index)
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.bottomMargin: 8
                    spacing: 8
                    RowLayout {
                        Layout.fillWidth: true; spacing: 10
                        Text { text: player.formatTime(player.position); color: theme.subTextColor; font.pixelSize: 11
                               Layout.preferredWidth: 44 }
                        Rectangle {
                            Layout.fillWidth: true; Layout.preferredHeight: 4; radius: 2; color: theme.borderColor
                            property real ratio: player.duration > 0 ? player.position / player.duration : 0
                            Rectangle {
                                anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                                width: parent.width * parent.ratio; radius: 2; color: theme.accentColor
                            }
                            MouseArea { anchors.fill: parent; anchors.margins: -8
                                        onClicked: player.seekRatio(Math.max(0, Math.min(1, mouseX / parent.width))) }
                        }
                        Text { text: player.formatTime(player.duration); color: theme.subTextColor; font.pixelSize: 11
                               Layout.preferredWidth: 44 }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 22
                        Item { Layout.fillWidth: true }
                        IconButton { name: "prev"; iconSize: 24; onClicked: player.previous() }
                        Rectangle {
                            Layout.preferredWidth: 52; Layout.preferredHeight: 52; radius: 26
                            color: theme.dark ? "#2a2a2a" : "#e5e5e5"
                            Icon { anchors.centerIn: parent; name: player.isPlaying ? "pause" : "play"
                                   iconSize: 24; iconColor: theme.textColor }
                            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                        onClicked: player.togglePlay() }
                        }
                        IconButton { name: "next"; iconSize: 24; onClicked: player.next() }
                        Item { Layout.fillWidth: true }
                    }
                }
            }
        }
    }

    // ==================================================================
    //  mineradio：电影镜头漂移封面 + 粒子 + 大字幕（当前/下一行）
    // ==================================================================
    Component {
        id: mineradioComp
        Item {
            id: mrRoot
            anchors.fill: parent
            Item {
                id: cinemaBox
                anchors.centerIn: parent
                width: parent.width; height: parent.height
                clip: true
                Image {
                    id: cinemaCover
                    anchors.centerIn: parent
                    width: parent.width * 1.2; height: parent.height * 1.2
                    source: (player.currentSong && player.currentSong.cover) ? player.currentSong.cover : ""
                    sourceSize: Qt.size(900, 900)
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    visible: status === Image.Ready
                }
                ParallelAnimation {
                    id: cineDrift
                    running: page.visible && player.isPlaying
                    loops: Animation.Infinite
                    NumberAnimation { target: cinemaCover; property: "x"
                                      from: -30; to: 30; duration: 14000; easing.type: Easing.InOutSine }
                    NumberAnimation { target: cinemaCover; property: "y"
                                      from: -20; to: 20; duration: 16000; easing.type: Easing.InOutSine }
                }
                Rectangle { anchors.fill: parent; color: "#b0000000" }
            }

            Canvas {
                anchors.fill: parent
                property real t: 0
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)
                    var n = 44
                    for (var i = 0; i < n; i++) {
                        var seed = i * 12.9898
                        var bx = (Math.sin(seed) * 0.5 + 0.5) * width
                        var speed = 10 + (i % 5) * 6
                        var by = (t * speed) % (height + 40)
                        var r = 1 + (i % 3)
                        var a = 0.12 + 0.18 * Math.sin(t / 20 + i)
                        ctx.beginPath()
                        ctx.fillStyle = Qt.rgba(1, 1, 1, Math.max(0, a))
                        ctx.arc(bx, height - by, r, 0, 2 * Math.PI)
                        ctx.fill()
                    }
                }
                Timer {
                    interval: 90; running: page.visible && player.isPlaying; repeat: true
                    onTriggered: { parent.t += 1; parent.requestPaint() }
                }
            }

            Column {
                anchors.centerIn: parent
                width: parent.width
                spacing: 18
                Repeater {
                    model: {
                        var lines = player.lyricLines || []
                        var out = []
                        for (var k = 0; k <= 1; k++) {
                            var idx = page.activeIndex + k
                            if (idx >= 0 && idx < lines.length) out.push({ i: idx, line: lines[idx] })
                        }
                        return out
                    }
                    delegate: Item {
                        required property var modelData
                        width: parent.width
                        height: lyricLine.implicitHeight
                        property bool isAct: modelData.i === page.activeIndex
                        transform: Rotation {
                            origin.x: parent.width / 2; origin.y: parent.height / 2
                            axis: Qt.vector3d(1, 0, 0)
                            angle: isAct ? 0 : 8
                        }
                        LyricLine {
                            id: lyricLine
                            anchors.horizontalCenter: parent.horizontalCenter
                            line: modelData.line
                            active: isAct
                            progress: isAct ? page.activeProgress : 0
                            size: isAct ? 42 : 26
                            textColor: "white"
                            inactiveColor: "white"
                            inactiveOpacity: 0.4
                            accentColor: theme.accentColor
                            showTranslation: page.showTranslation
                            karaoke: true
                            maxW: parent.width * 0.9
                            onClicked: page.seekLine(modelData.i)
                        }
                    }
                }
            }

            // 顶部：收起（左上角大块命中区）
            Rectangle {
                anchors.top: parent.top
                anchors.left: parent.left
                width: 56; height: 52
                radius: 10
                color: mrCloseHover.containsMouse ? "#22ffffff" : "transparent"
                opacity: page.controlsVisible ? 1 : 0
                visible: opacity > 0
                Behavior on opacity { NumberAnimation { duration: 250 } }
                Icon {
                    anchors.centerIn: parent
                    name: "chevron-down"; iconSize: 22; iconColor: "white"
                }
                MouseArea {
                    id: mrCloseHover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: page.close()
                }
            }

            // 底部：进度条 + 控制
            Rectangle {
                id: mrBar
                z: 100
                width: mrRoot.width - 80
                x: 40
                y: mrRoot.height - 130
                height: 100
                radius: 14
                color: "#55000000"
                border.color: "#22ffffff"; border.width: 1
                opacity: page.controlsVisible ? 1 : 0
                visible: opacity > 0
                Behavior on opacity { NumberAnimation { duration: 250 } }

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        Text { text: player.formatTime(player.position); color: "#e6ffffff"; font.pixelSize: 11
                               Layout.preferredWidth: 44 }
                        Rectangle {
                            id: mrSeek
                            Layout.fillWidth: true
                            Layout.preferredHeight: 4
                            radius: 2
                            color: "#55ffffff"
                            property real ratio: player.duration > 0 ? player.position / player.duration : 0
                            Rectangle {
                                anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                                width: parent.width * mrSeek.ratio; radius: 2; color: theme.accentColor
                            }
                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                x: Math.max(0, Math.min(parent.width - 12, parent.width * mrSeek.ratio - 6))
                                width: 12; height: 12; radius: 6; color: "white"
                            }
                            MouseArea {
                                anchors.fill: parent
                                anchors.topMargin: -12; anchors.bottomMargin: -12
                                cursorShape: Qt.PointingHandCursor
                                onClicked: player.seekRatio(Math.max(0, Math.min(1, mouseX / parent.width)))
                            }
                        }
                        Text { text: player.formatTime(player.duration); color: "#e6ffffff"; font.pixelSize: 11
                               Layout.preferredWidth: 44 }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 22
                        Item { Layout.fillWidth: true }
                        IconButton { name: "prev"; iconSize: 24; iconColor: "white"; onClicked: player.previous() }
                        Rectangle {
                            Layout.preferredWidth: 52; Layout.preferredHeight: 52; radius: 26
                            color: "#33ffffff"; border.color: "#66ffffff"; border.width: 1
                            Icon { anchors.centerIn: parent; name: player.isPlaying ? "pause" : "play"
                                   iconSize: 24; iconColor: "white" }
                            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                        onClicked: player.togglePlay() }
                        }
                        IconButton { name: "next"; iconSize: 24; iconColor: "white"; onClicked: player.next() }
                        Item { Layout.fillWidth: true }
                    }
                }
            }
        }
    }

    // ===== 打开/关闭 =====
    function open() {
        page.visible = true
        page.controlsVisible = true
        page.style = settings.playerStyle
        hideTimer.restart()
        recompute()
        inFade.restart()
        inAnim.restart()
        syncStage()
    }
    function close() { outFade.restart() }

    // ===== 真·舞台引擎（方案C：独立进程）=====
    //  样式为 mineradio 且本页可见 → 拉起/保持 MuyunStage；否则关闭。
    //  stage.unavailable（无壳/启动失败/超时）时静默回退到 lite 近似。
    function syncStage() {
        if (typeof stage === "undefined") return
        if (visible && style === "mineradio" && !stage.unavailable) {
            stage.open()
            stage.setFav(favState)
        } else {
            stage.close()
            // 舞台退出 → 收掉 fx 控制台浮层
            if (typeof stageFx !== "undefined") stageFx.hide()
        }
    }
    onVisibleChanged: {
        syncStage()
        if (!visible) closed()   // 通知外层重建输入法上下文
    }
    onStyleChanged: syncStage()
    onFavStateChanged: if (typeof stage !== "undefined" && stage.active) stage.setFav(favState)

    Connections {
        target: (typeof stage !== "undefined") ? stage : null
        enabled: (typeof stage !== "undefined")
        function onCtrlEvent(action, value) {
            if (action === "back") page.close()
            else if (action === "like") { library.toggleFavorite(player.currentSong); stage.setFav(library.isFavorite(player.currentSong)) }
            else if (action === "queue") { /* 舞台页内已自弹提示，不退出全屏 */ }
            else if (action === "fx") {
                // 舞台 fx-fab → 右侧浮层「视觉控制台」（独立置顶窗，浮在舞台上可实时看效果）
                if (typeof stageFx !== "undefined") stageFx.toggle()
            }
        }
    }

    OpacityAnimator { id: inFade; target: page; from: 0; to: 1; duration: 220 }
    ScaleAnimator  { id: inAnim; target: page; from: 1.02; to: 1; duration: 220; easing.type: Easing.OutCubic }
    OpacityAnimator {
        id: outFade; target: page; from: 1; to: 0; duration: 180
        onFinished: page.visible = false
    }
}
