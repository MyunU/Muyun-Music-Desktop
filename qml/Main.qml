import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs
import QtQuick.Window
import "components"

Window {
    id: root
    // 默认尺寸 = 屏幕 84%×87%（对齐用户红框比例），启动时屏幕居中
    width: Math.round(Screen.width * 0.84)
    height: Math.round(Screen.height * 0.87)
    minimumWidth: 820
    minimumHeight: 560
    x: Math.round((Screen.width - width) / 2)
    y: Math.round((Screen.height - height) / 2)
    visible: true
    title: "暮云音乐"
    color: theme.bgColor

    // 无边框：只保留自绘标题栏，避免出现「系统窗口套一层 UI」的双重壳
    flags: Qt.Window | Qt.FramelessWindowHint

    property string currentPage: "home"
    property var songList: []
    property string listTitle: ""
    property string playContextId: ""   // 当前播放上下文（歌单 id / favorites / local / recent）
    property string listBackTarget: "home"   // 点"返回"时回到哪一页（首页 / 歌单广场）

    // 当前打开的「在线歌单」（广场/推荐点进来的那张），收藏歌单按钮要用它做键；
    // 切到别的页面/自建歌单时必须清空，否则收藏按钮会挂到不相干的列表上。
    property var onlinePl: null
    // 是否已收藏：绑定里必须掺进 library.playlists，Q_INVOKABLE 方法自己不会重算（QML 老坑）
    readonly property bool onlinePlCollected: {
        var _n = library.playlists.length
        return root.onlinePl !== null
               && library.isPlaylistCollected(String(root.onlinePl.platform || home.platform),
                                              String(root.onlinePl.id))
    }

    // 全屏意图位：被别的窗（舞台窗、桌面歌词置顶窗）压住时 visibility 会失真成 Covered，
    // 只认 root.visibility === Window.FullScreen 就会判错 → 表现为"全屏里 F11/ESC 只进不出"。
    // 与 C++ HotkeyManager::m_wantFull 同一套路（都跟着 visibilityChanged 走）。
    property bool fullScreenOn: false
    property bool _wasMinimized: false   // 刚从任务栏恢复（用于补手动最大化的放置信息）
    onVisibilityChanged: {
        if (root.visibility === Window.FullScreen) root.fullScreenOn = true
        else if (root.visibility === Window.Windowed || root.visibility === Window.Maximized
                 || root.visibility === Window.Minimized) root.fullScreenOn = false
        if (root.visibility === Window.Minimized) {
            root._wasMinimized = true
        } else if (root._wasMinimized) {
            root._wasMinimized = false
            // 手动最大化不走 Windows 最大化机制（ShowWindow SW_MAXIMIZE），
            // 最小化→从任务栏恢复时 Windows 按普通尺寸放置、还可能丢掉 WS_MAXIMIZE 位
            // → 表现为"最大化窗口回来变小/系统不认最大化"。恢复时补齐几何 + 样式位。
            if (root.maximized) {
                root.x = 0; root.y = 0
                root.width = Screen.desktopAvailableWidth
                root.height = Screen.desktopAvailableHeight
                if (typeof frameless !== "undefined") frameless.setMaximizedStyle(true)
            }
        }
    }

    // 最大化：不依赖 Qt showMaximized()/visibility()（frameless+原生缩放 hack 下二者会误报，
    // 导致按钮"无效"）。直接手动设置窗口几何到工作区，还原时回到之前尺寸——纯赋值必定生效。
    property bool maximized: false
    // #11：手动最大化同步 WS_MAXIMIZE 样式位（TranslucentTB 等第三方任务栏工具靠
    // IsZoomed() 认"最大化"；我们手动改几何从不带这个位 → 工具认不出）。
    // 全屏(F11)不算最大化：走 showFullScreen，不设该位。
    onMaximizedChanged: {
        if (typeof frameless !== "undefined")
            frameless.setMaximizedStyle(root.maximized)
    }
    property real _rx: 0
    property real _ry: 0
    property real _rw: 0
    property real _rh: 0
    property bool _saved: false
    function toggleMaximize() {
        // 全屏（F11）里点最大化：先退全屏。绝不能把全屏几何当"还原目标"记下来——
        // 那样下一次"还原"还原到的还是这么大，按钮看着就是"按了没反应"（用户实测）。
        if (root.fullScreenOn) {
            root.toggleFullScreen()
            root.maximized = false
            // ⚠ 退全屏后必须立即恢复几何：showNormal() 只会退回"上次可见状态"
            // （可能仍是最大化尺寸）。不清标志、只赋几何也不行——下次点最大化
            // 走 else 分支再赋工作区几何，窗口本来就那么大 → "按了没反应"（用户
            // 实测：全屏→点→无效→再点才还原。此处一并恢复到还原目标/默认尺寸）。
            if (_saved) { root.x = _rx; root.y = _ry; root.width = _rw; root.height = _rh }
            else {
                root.width = Math.round(Screen.width * 0.84)
                root.height = Math.round(Screen.height * 0.87)
                root.x = Math.round((Screen.width - root.width) / 2)
                root.y = Math.round((Screen.height - root.height) / 2)
            }
            return
        }
        if (maximized) {
            // ⚠ 必须先清标志、再赋几何。清标志触发 onMaximizedChanged → setMaximizedStyle(false)
            // 摘掉 WS_MAXIMIZE 位；位还在时 Windows 会直接拒绝改窗口大小（stderr: "Unable to set
            // geometry ... Resulting geometry: <原样>"），几何赋值就被吞掉 → 点「还原」没反应、
            // 窗口钉在工作区大小。v1.1.0 出厂时顺序正好反了（先赋几何 L76、后清标志 L83），
            // --test-maximize 稳定 3/3 复现，2026-10-04 修复（见 HANDOFF 待办 #23）。
            maximized = false
            if (_saved) { root.x = _rx; root.y = _ry; root.width = _rw; root.height = _rh }
            else {      // 没存到还原目标（启动即最大化等）→ 回默认尺寸，别让按钮空转
                root.width = Math.round(Screen.width * 0.84)
                root.height = Math.round(Screen.height * 0.87)
                root.x = Math.round((Screen.width - root.width) / 2)
                root.y = Math.round((Screen.height - root.height) / 2)
            }
        } else {
            // 只有"确实比工作区小"的几何才配当还原目标
            if (root.width < Screen.desktopAvailableWidth - 8 || root.height < Screen.desktopAvailableHeight - 8) {
                _rx = root.x; _ry = root.y; _rw = root.width; _rh = root.height; _saved = true
            }
            root.x = 0
            root.y = 0
            root.width = Screen.desktopAvailableWidth
            root.height = Screen.desktopAvailableHeight
            maximized = true
        }
    }

    // ==================================================================
    // 顶部标题栏（自绘，含窗口控制）
    // ==================================================================
    Rectangle {
        id: titleBar
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 44
        color: theme.panelColor
        z: 10

        // 拖标题栏空白处移动窗口：交给系统级移动，平滑无抖动
        // （手动 root.x += 方式存在事件反馈环路，拖动会抖动/撕裂）
        MouseArea {
            anchors.fill: parent
            onPressed: root.startSystemMove()
        }
        TapHandler {
            onDoubleTapped: root.toggleMaximize()
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 8
            spacing: 10

            Image {
                source: "qrc:/resources/app.svg"
                sourceSize.width: 22
                sourceSize.height: 22
                width: 22
                height: 22
                fillMode: Image.PreserveAspectFit
                smooth: true
            }
            Text {
                text: "暮云音乐"
                color: theme.textColor
                font.pixelSize: 13
                font.bold: true
            }
            Item { Layout.fillWidth: true }

            // 窗口控制（全屏按钮已移除——F11 是音乐软件惯例，且热键已走系统级注册）
            IconButton {
                name: "window-min"
                iconSize: 15
                Layout.preferredWidth: 36
                Layout.preferredHeight: 28
                onClicked: root.showMinimized()
            }
            IconButton {
                name: root.maximized ? "window-restore" : "window-max"   // 已最大化时给"还原"图标，别让人以为按错了
                iconSize: 13
                Layout.preferredWidth: 36
                Layout.preferredHeight: 28
                onClicked: root.toggleMaximize()
            }
            IconButton {
                name: "window-close"
                iconSize: 14
                hoverColor: theme.accentColor
                Layout.preferredWidth: 36
                Layout.preferredHeight: 28
                // 走 onClosing：按用户设置分流（询问/最小化到托盘/直接退出）
                onClicked: root.close()
            }
        }
    }

    // ==================================================================
    // 主体：侧边栏 + 右侧
    // ==================================================================
    RowLayout {
        anchors.top: titleBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        spacing: 0

        // ------------------ 侧边栏 ------------------
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 200
            color: theme.panelColor

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 2

                NavItem { icon: "home"; label: "主页";     active: root.currentPage === "home";           onClicked: goPage("home") }
                NavItem { icon: "music"; label: "本地音乐"; active: root.currentPage === "local";          onClicked: goPage("local") }

                SectionLabel { text: "收藏" }
                NavItem { icon: "heart"; label: "收藏"; count: library.favorites.length; active: root.currentPage === "favorites"; onClicked: goPage("favorites") }
                NavItem { icon: "clock"; label: "最近播放"; active: root.currentPage === "recent";         onClicked: goPage("recent") }

                SectionLabel {
                    text: "歌单"
                    onAddClicked: newPlaylistDialog.open()
                }
                Repeater {
                    model: library.playlists
                    delegate: NavItem {
                        required property var modelData
                        icon: "play-list"
                        label: modelData.name
                        active: root.currentPage === "playlistDetail" && root.playContextId === modelData.id
                        deletable: true
                        onClicked: goPlaylist(modelData.id, modelData.name)
                        onDeleteRequested: {
                            deletePlaylistDialog.pendingId = modelData.id
                            deletePlaylistDialog.pendingName = modelData.name
                            deletePlaylistDialog.open()
                        }
                    }
                }

                Item { Layout.fillHeight: true }
            }
        }

        // ------------------ 右侧 ------------------
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // ========== 顶栏 ==========
            // z:10 让搜索联想/热搜下拉（顶栏子元素、悬浮超出顶栏高度）
            // 绘制在下方内容区之上，否则会被后声明的 StackLayout 内容盖住
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 60
                color: theme.panelColor
                z: 10

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 16
                    spacing: 10

                    // 平台选择
                    ComboBox {
                        id: platformBox
                        Layout.preferredWidth: 96
                        Layout.preferredHeight: 32
                        model: ["全部平台"]
                        function syncFromPlatform() {
                            var opts = searcher.platformOptions()
                            var want = searcher.platform.length === 0 ? "all" : searcher.platform
                            for (var j = 0; j < opts.length; j++)
                                if (opts[j].id === want) { platformBox.currentIndex = j; break }
                        }
                        Component.onCompleted: {
                            var names = ["全部平台"]
                            var opts = searcher.platformOptions()
                            for (var i = 1; i < opts.length; i++) names.push(opts[i].name)
                            platformBox.model = names
                            syncFromPlatform()   // 恢复上次记录的平台
                        }
                        onCurrentTextChanged: {
                            var opts = searcher.platformOptions()
                            for (var i = 0; i < opts.length; i++) {
                                if (opts[i].name === currentText) {
                                    searcher.platform = (opts[i].id === "all") ? "" : opts[i].id
                                    return
                                }
                            }
                        }
                        contentItem: Text {
                            text: platformBox.currentText
                            color: theme.textColor
                            font.pixelSize: 12
                            leftPadding: 10
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                        background: Rectangle {
                            radius: 16
                            color: theme.bgColor
                            border.color: theme.borderColor
                            border.width: 1
                        }
                        indicator: Text {
                            x: platformBox.width - 20
                            y: platformBox.height / 2 - height / 2
                            text: "▾"
                            color: theme.subTextColor
                            font.pixelSize: 10
                        }
                    }

                    // 搜索框
                    Rectangle {
                        id: searchBox
                        Layout.preferredWidth: 300
                        Layout.preferredHeight: 32
                        radius: 16
                        color: theme.bgColor
                        border.color: searchInput.activeFocus ? theme.accentColor : theme.borderColor
                        border.width: 1

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 14
                            anchors.rightMargin: 12
                            spacing: 8
                            Icon { name: "search"; iconSize: 15; iconColor: theme.subTextColor }
                            TextInput {
                                id: searchInput
                                objectName: "searchInputObj"      // 自检用（--test-ime：文本框焦点基线）
                                Layout.fillWidth: true
                                verticalAlignment: Text.AlignVCenter
                                color: theme.textColor
                                font.pixelSize: 12
                                clip: true
                                selectByMouse: true
                                onAccepted: doSearch(text)
                                onTextEdited: {
                                    suggestTimer.restart()
                                    // 清空输入只清联想，**不清搜索结果**（结果属于上一次查询，
                                    // 一键删除输入词不应让整页结果消失）
                                    if (text.length === 0) searcher.suggest("")
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "搜索歌曲、歌手、专辑..."
                                    color: theme.subTextColor
                                    font.pixelSize: 12
                                    // 输入法预编辑期间（拼音未上屏）也要隐藏，避免与预编辑字母重叠
                                    visible: searchInput.text.length === 0
                                             && searchInput.preeditText.length === 0
                                }
                            }
                            // 防抖：停止输入 300ms 后请求联想
                            Timer {
                                id: suggestTimer
                                interval: 300
                                onTriggered: {
                                    if (searchInput.text.length > 0)
                                        searcher.suggest(searchInput.text)
                                }
                            }
                            Icon {
                                name: "window-close"
                                iconSize: 13
                                iconColor: theme.subTextColor
                                visible: searchInput.text.length > 0
                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: { searchInput.text = ""; searcher.suggest("") }
                                }
                            }
                        }

                        // 联想 / 热搜下拉（搜索框子元素，天然贴其正下方；父项不裁剪可悬浮）
                        // 热搜模式左右分栏：热搜歌曲 | 热搜歌手；联想模式单栏
                        Rectangle {
                            id: searchDrop
                            visible: searchInput.activeFocus
                            width: searchInput.text.length > 0 ? 300 : 520
                            height: dropContent.height + 12
                            anchors.top: parent.bottom
                            anchors.topMargin: 6
                            anchors.left: parent.left
                            radius: 10
                            color: theme.cardColor
                            border.color: theme.borderColor
                            border.width: 1
                            z: 999

                            ColumnLayout {
                                id: dropContent
                                width: parent.width - 12
                                x: 6
                                y: 6
                                spacing: 0

                                // ---------- 联想模式（单栏） ----------
                                Text {
                                    visible: searchInput.text.length > 0
                                    text: "搜索联想"
                                    color: theme.subTextColor
                                    font.pixelSize: 11
                                    Layout.leftMargin: 8
                                    Layout.topMargin: 4
                                    Layout.bottomMargin: 4
                                }
                                Repeater {
                                    model: searchInput.text.length > 0 ? searcher.suggestions : []
                                    delegate: Item {
                                        id: sugItem
                                        required property var modelData
                                        Layout.fillWidth: true
                                        height: 34
                                        Rectangle {
                                            anchors.fill: parent
                                            anchors.margins: 2
                                            radius: 5
                                            color: sugHover.containsMouse ? theme.hoverColor : "transparent"
                                        }
                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: 10
                                            anchors.rightMargin: 10
                                            Icon { name: "search"; iconSize: 12; iconColor: theme.subTextColor }
                                            Text {
                                                Layout.fillWidth: true
                                                text: modelData
                                                color: theme.textColor
                                                font.pixelSize: 12
                                                elide: Text.ElideRight
                                            }
                                        }
                                        MouseArea {
                                            id: sugHover
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                searchInput.focus = false
                                                doSearch(modelData)
                                            }
                                        }
                                    }
                                }
                                Text {
                                    visible: searchInput.text.length > 0 && searcher.suggestions.length === 0
                                    text: "无匹配联想"
                                    color: theme.subTextColor
                                    font.pixelSize: 12
                                    Layout.margins: 10
                                }

                                // ---------- 热搜模式（左右分栏） ----------
                                RowLayout {
                                    visible: searchInput.text.length === 0
                                    Layout.fillWidth: true
                                    Layout.topMargin: 6
                                    Layout.bottomMargin: 4
                                    Layout.leftMargin: 8
                                    Layout.rightMargin: 8
                                    spacing: 0

                                    Text {
                                        text: "热搜歌曲"
                                        color: theme.subTextColor
                                        font.pixelSize: 11
                                    }
                                    Item { Layout.fillWidth: true }
                                    Text {
                                        text: "热搜歌手"
                                        color: theme.subTextColor
                                        font.pixelSize: 11
                                    }
                                }
                                RowLayout {
                                    visible: searchInput.text.length === 0
                                    Layout.fillWidth: true
                                    spacing: 8

                                    // 左：热搜歌曲（词 + 歌名·歌手）
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Layout.preferredWidth: 1
                                        Layout.minimumWidth: 200
                                        spacing: 0
                                        Repeater {
                                            model: searcher.hotSongs.slice(0, 10)
                                            delegate: Item {
                                                id: hotSongItem
                                                required property var modelData
                                                required property int index
                                                Layout.fillWidth: true
                                                height: 40
                                                Rectangle {
                                                    anchors.fill: parent
                                                    anchors.margins: 2
                                                    radius: 5
                                                    color: hotSongHover.containsMouse ? theme.hoverColor : "transparent"
                                                }
                                                RowLayout {
                                                    anchors.fill: parent
                                                    anchors.leftMargin: 8
                                                    anchors.rightMargin: 8
                                                    spacing: 8
                                                    Text {
                                                        text: String(hotSongItem.index + 1)
                                                        width: 16
                                                        horizontalAlignment: Text.AlignHCenter
                                                        color: hotSongItem.index < 3 ? theme.accentColor : theme.subTextColor
                                                        font.pixelSize: 12
                                                        font.bold: hotSongItem.index < 3
                                                    }
                                                    ColumnLayout {
                                                        Layout.fillWidth: true
                                                        spacing: 0
                                                        Text {
                                                            Layout.fillWidth: true
                                                            text: hotSongItem.modelData.name
                                                                  || hotSongItem.modelData.word || ""
                                                            color: theme.textColor
                                                            font.pixelSize: 12
                                                            elide: Text.ElideRight
                                                        }
                                                        Text {
                                                            Layout.fillWidth: true
                                                            visible: (hotSongItem.modelData.artist || "") !== ""
                                                            text: hotSongItem.modelData.artist
                                                            color: theme.subTextColor
                                                            font.pixelSize: 10
                                                            elide: Text.ElideRight
                                                        }
                                                    }
                                                }
                                                MouseArea {
                                                    id: hotSongHover
                                                    anchors.fill: parent
                                                    hoverEnabled: true
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        searchInput.focus = false
                                                        doSearch(hotSongItem.modelData.word
                                                                 || hotSongItem.modelData.name)
                                                    }
                                                }
                                            }
                                        }
                                        Text {
                                            visible: searcher.hotSongs.length === 0
                                            text: "加载热搜中..."
                                            color: theme.subTextColor
                                            font.pixelSize: 12
                                            Layout.margins: 10
                                        }
                                    }

                                    // 分隔线
                                    Rectangle {
                                        visible: searchInput.text.length === 0
                                        Layout.preferredWidth: 1
                                        Layout.fillHeight: true
                                        Layout.topMargin: 4
                                        Layout.bottomMargin: 4
                                        color: theme.borderColor
                                    }

                                    // 右：热搜歌手（头像 + 名字）
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Layout.preferredWidth: 1
                                        Layout.minimumWidth: 140
                                        spacing: 0
                                        Repeater {
                                            model: searcher.topArtists.slice(0, 10)
                                            delegate: Item {
                                                id: hotArtistItem
                                                required property var modelData
                                                required property int index
                                                Layout.fillWidth: true
                                                height: 40
                                                Rectangle {
                                                    anchors.fill: parent
                                                    anchors.margins: 2
                                                    radius: 5
                                                    color: hotArtistHover.containsMouse ? theme.hoverColor : "transparent"
                                                }
                                                RowLayout {
                                                    anchors.fill: parent
                                                    anchors.leftMargin: 8
                                                    anchors.rightMargin: 8
                                                    spacing: 8
                                                    // 头像（圆形）
                                                    Rectangle {
                                                        Layout.preferredWidth: 26
                                                        Layout.preferredHeight: 26
                                                        radius: 13
                                                        clip: true
                                                        color: theme.cardColor
                                                        Image {
                                                            anchors.fill: parent
                                                            source: hotArtistItem.modelData.avatar || ""
                                                            sourceSize: Qt.size(80, 80)
                                                            fillMode: Image.PreserveAspectCrop
                                                            asynchronous: true
                                                            visible: status === Image.Ready
                                                        }
                                                        Icon {
                                                            anchors.centerIn: parent
                                                            name: "mic"
                                                            iconSize: 12
                                                            iconColor: theme.subTextColor
                                                            visible: !(hotArtistItem.modelData.avatar || "")
                                                        }
                                                    }
                                                    Text {
                                                        Layout.fillWidth: true
                                                        text: hotArtistItem.modelData.name || ""
                                                        color: theme.textColor
                                                        font.pixelSize: 12
                                                        elide: Text.ElideRight
                                                    }
                                                }
                                                MouseArea {
                                                    id: hotArtistHover
                                                    anchors.fill: parent
                                                    hoverEnabled: true
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        searchInput.focus = false
                                                        doSearch(hotArtistItem.modelData.name)
                                                    }
                                                }
                                            }
                                        }
                                        Text {
                                            visible: searcher.topArtists.length === 0
                                            text: "加载歌手中..."
                                            color: theme.subTextColor
                                            font.pixelSize: 12
                                            Layout.margins: 10
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // 顶栏歌词显示（#13：从播放条搬到搜索框右侧）
                    // ⚠ 只要有歌就常驻可点（用户反馈：没歌词时这块是空的、点不动没法重载歌词）；
                    //   有歌词显当前行，没歌词显「点击加载歌词」占位引导。点击 = 重新获取歌词（音乐不停）。
                    Item {
                        id: topLyricBox
                        Layout.preferredWidth: 360
                        Layout.maximumWidth: 360
                        Layout.preferredHeight: 40
                        visible: player.currentSong.name.length > 0
                        clip: true
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: player.reloadLyric()
                        }
                        ColumnLayout {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.right: parent.right
                            spacing: 1
                            Text {
                                Layout.fillWidth: true
                                text: player.currentLyricText.length > 0
                                      ? player.currentLyricText
                                      : (player.lyricLoading ? "歌词加载中…" : "点击加载歌词")
                                color: player.currentLyricText.length > 0
                                       ? theme.textColor : theme.subTextColor
                                font.pixelSize: 15
                                font.bold: player.currentLyricText.length > 0
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                text: player.currentLyricTranslation
                                color: theme.subTextColor
                                font.pixelSize: 12
                                elide: Text.ElideRight
                                visible: player.currentLyricTranslation.length > 0
                            }
                        }
                    }

                    Item { Layout.fillWidth: true }

                    // 右侧功能图标
                    ToolbarIcon { glyph: "clock"; tip: "最近播放"; onClicked: goPage("recent") }
                    ToolbarIcon { glyph: "download"; tip: "下载管理"; onClicked: downloadDrawer.open() }
                    ToolbarIcon { glyph: "cast"; tip: "音源切换"; onClicked: sourceDrawer.open() }
                    ToolbarIcon {
                        glyph: theme.dark ? "moon" : "sun"
                        tip: "切换主题"
                        onClicked: theme.dark = !theme.dark
                    }
                    ToolbarIcon { glyph: "gear"; tip: "设置"; onClicked: openSettings() }
                }

                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width; height: 1
                    color: theme.borderColor
                }
            }

            // ========== 内容区 ==========
            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: root.currentPage === "home" ? 0
                            : root.currentPage === "explore" ? 2 : 1

                // ---------- 首页 ----------
                HomePage {
                    id: homePage
                    onPlaylistClicked: function(pl) { openOnlinePlaylist(pl, "home") }
                    onToplistClicked: function(pl) {
                        notify("榜单加载中…")
                        home.loadToplistSongs(home.platform, pl.id, pl.name, false)
                    }
                    onPlayToplist: function(pl) {
                        notify("榜单加载中…")
                        home.loadToplistSongs(home.platform, pl.id, pl.name, true)
                    }
                    onMorePlaylistsClicked: goPage("explore")
                }

                // ---------- 列表页 ----------
                ListPage {
                    id: listPage
                    objectName: "listPageObj"     // 自检用（--test-collect-ui 找它断言）
                    title: root.listTitle
                    songs: root.songList
                    contextId: root.playContextId
                    removable: root.currentPage === "playlistDetail"
                    showFilter: root.currentPage !== "search"   // 搜索页已有顶栏搜索框，不再放页内过滤
                    // 只有"从首页/歌单广场点进来的"榜单和在线歌单详情需要返回按钮；
                    // 侧边栏的歌单、收藏、最近播放、本地音乐、搜索结果都能从侧边栏直接切回。
                    showBack: root.currentPage === "toplist"
                             || (root.currentPage === "playlistDetail"
                                 && root.playContextId.startsWith("online-"))
                    showScan: root.currentPage === "local"
                    // 批量操作只对"收藏"和"自建歌单"开放（在线歌单详情/榜单/搜索/本地不参与）
                    showBatch: root.currentPage === "favorites"
                               || (root.currentPage === "playlistDetail"
                                   && root.playContextId.length > 0
                                   && !root.playContextId.startsWith("online-"))
                    removeLabel: root.currentPage === "favorites" ? "取消收藏" : "移出歌单"
                    // 收藏整单：只对"从广场/推荐点进来的在线歌单"开放（自建歌单本来就在列表里）
                    showCollect: root.onlinePl !== null
                                 && root.currentPage === "playlistDetail"
                                 && root.playContextId === "online-" + String(root.onlinePl.id)
                    collected: root.onlinePlCollected
                    onPlayAll: playAllSongs()
                    onBatchDownloadRequested: function (songs, qid) { root.batchDownload(songs, qid) }
                    onBatchRemoveRequested: function (songs) { root.batchRemove(songs) }
                    onCollectRequested: {
                        var pl = root.onlinePl
                        if (!pl) return
                        // 取消收藏不需要歌曲在手；只有"首次收藏"才要求列表已经加载出来
                        if (!root.onlinePlCollected && root.songList.length === 0) {
                            notify("歌曲还没加载完，稍等一下再收藏")
                            return
                        }
                        var got = library.toggleCollectPlaylist(pl, root.songList)
                        notify(got ? ("已收藏歌单「" + (pl.name || "") + "」→ 左侧「歌单」")
                                   : ("已取消收藏「" + (pl.name || "") + "」"))
                    }
                    onBackClicked: goPage(root.listBackTarget)
                    onScanRequested: {
                        library.rescan()
                        settings.toast("正在重新扫描音乐目录…")
                    }
                }

                // ---------- 歌单广场 ----------
                ExplorePage {
                    id: explorePage
                    onPlaylistClicked: function(pl) { openOnlinePlaylist(pl, "explore") }
                    onBackClicked: goPage("home")
                }
            }

            // ========== 底部播放条 ==========
            PlayBar { id: playBar }
        }
    }

    // ==================================================================
    // 弹窗 / 提示
    // ==================================================================
    // 新建歌单（深色无边框，与整体风格一致）
    Popup {
        id: newPlaylistDialog
        width: 380
        height: 210
        anchors.centerIn: parent
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            radius: 14
            color: theme.panelColor
            border.color: theme.borderColor
            border.width: 1
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 22
            spacing: 16

            Text {
                text: "新建歌单"
                color: theme.textColor
                font.pixelSize: 16
                font.bold: true
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 38
                radius: 8
                color: theme.bgColor
                border.color: plName.activeFocus ? theme.accentColor : theme.borderColor
                border.width: 1

                TextInput {
                    id: plName
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    verticalAlignment: Text.AlignVCenter
                    color: theme.textColor
                    font.pixelSize: 13
                    selectByMouse: true
                    clip: true
                    onAccepted: confirmBtn.doCreate()
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "歌单名称"
                        color: theme.subTextColor
                        font.pixelSize: 13
                        visible: plName.text.length === 0
                    }
                }
            }

            Item { Layout.fillHeight: true }

            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 10

                Rectangle {
                    width: 76; height: 34; radius: 17
                    color: cancelHover.containsMouse ? theme.hoverColor : theme.cardColor
                    border.color: theme.borderColor
                    border.width: 1
                    Text {
                        anchors.centerIn: parent
                        text: "取消"
                        color: theme.textColor
                        font.pixelSize: 13
                    }
                    MouseArea {
                        id: cancelHover
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: { newPlaylistDialog.close(); plName.text = "" }
                    }
                }

                Rectangle {
                    id: confirmBtn
                    width: 76; height: 34; radius: 17
                    color: confirmHover.containsMouse ? Qt.lighter(theme.accentColor, 1.1)
                                                      : theme.accentColor
                    function doCreate() {
                        var name = plName.text.trim()
                        if (name.length === 0) {
                            // 空名自动编号：新建歌单、新建歌单1、新建歌单2...
                            var n = 1, base = "新建歌单"
                            var names = []
                            for (var i = 0; i < library.playlists.length; i++)
                                names.push(library.playlists[i].name)
                            name = base
                            while (names.indexOf(name) >= 0) { name = base + n; n++ }
                        }
                        library.createPlaylist(name, "")
                        root.notify("已创建歌单：" + name)
                        plName.text = ""
                        newPlaylistDialog.close()
                    }
                    Text {
                        anchors.centerIn: parent
                        text: "创建"
                        color: "white"
                        font.pixelSize: 13
                    }
                    MouseArea {
                        id: confirmHover
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: confirmBtn.doCreate()
                    }
                }
            }
        }
    }

    // 音效调节面板
    EffectsPanel { id: effectsPanel; objectName: "effectsPanelObj" }

    // 删除歌单确认弹窗
    Popup {
        id: deletePlaylistDialog
        property string pendingId: ""
        property string pendingName: ""
        anchors.centerIn: parent
        modal: true
        dim: true
        width: 360
        padding: 20
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: theme.panelColor
            radius: 12
            border.color: theme.borderColor
        }
        contentItem: ColumnLayout {
            spacing: 14
            Text {
                text: "删除歌单"
                color: theme.textColor
                font.pixelSize: 15
                font.bold: true
            }
            Text {
                text: "确定删除歌单「" + deletePlaylistDialog.pendingName + "」？\n歌单内的歌曲不会被删除，仅移除歌单。"
                color: theme.subTextColor
                font.pixelSize: 12
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Item { Layout.fillWidth: true }
                Rectangle {
                    width: 72; height: 32; radius: 16
                    color: cancelDelHover.containsMouse ? theme.hoverColor : theme.cardColor
                    border.color: theme.borderColor
                    border.width: 1
                    Text { anchors.centerIn: parent; text: "取消"; color: theme.textColor; font.pixelSize: 12 }
                    MouseArea {
                        id: cancelDelHover
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: deletePlaylistDialog.close()
                    }
                }
                Rectangle {
                    width: 72; height: 32; radius: 16
                    color: confirmDelHover.containsMouse ? Qt.lighter(theme.accentColor, 1.15) : theme.accentColor
                    Text { anchors.centerIn: parent; text: "删除"; color: "white"; font.pixelSize: 12 }
                    MouseArea {
                        id: confirmDelHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            library.deletePlaylist(deletePlaylistDialog.pendingId)
                            if (root.playContextId === deletePlaylistDialog.pendingId) {
                                root.currentPage = "home"
                                root.songList = []
                            }
                            deletePlaylistDialog.close()
                            notify("已删除歌单")
                        }
                    }
                }
            }
        }
    }

    // 设置面板（本地音源 / 在线音源 / 关于）
    SettingsPanel { id: settingsPanel; objectName: "settingsPanelObj" }

    Connections {
        target: settings
        function onMessage(text) { toast.show(text) }
        function onImportFailed(reason) { toast.show("导入失败：" + reason) }
        function onLxUpdateAlertChanged() { if (settings.lxUpdateAlert.updateUrl) updateDialog.open() }
    }

    // 应用自身更新提示（开源方案 A：GitHub 清单，见 core/update/UpdateChecker）。
    // 只有后端判定"该提醒"（比本地新、且没被"不再提醒"压住）时才发这个信号 → 弹窗。
    Connections {
        target: updater
        function onUpdateFound(version, notes) { appUpdateDialog.showFor(version, notes) }
    }

    UpdateDialog { id: appUpdateDialog }

    Connections {
        target: downloads
        function onMessage(text) { toast.show(text) }
    }

    Connections {
        target: home
        function onMessage(text) { toast.show(text) }
    }

    // 点击空白处让输入框失焦（同时收起联想/热搜下拉）
    // 注意：root 是 Window（非 Item），forceActiveFocus 对其无效，
    // 必须直接让 searchInput 失焦。
    MouseArea {
        anchors.fill: parent
        z: -100
        onClicked: searchInput.focus = false
    }

    // 关闭窗口：按用户设置（询问 / 最小化到托盘 / 直接退出）
    onClosing: (close) => {
        if (settings.exitAction === "minimize") {
            close.accepted = false
            root.hide()
            tray.minimizeToTray()
        } else if (settings.exitAction === "exit") {
            close.accepted = true
        } else {
            close.accepted = false
            exitDialog.open()
        }
    }

    // 托盘请求恢复主窗口（双击托盘图标 / 菜单）
    Connections {
        target: tray
        function onShowRequested() {
            root.show()
            root.raise()
            root.requestActivate()
        }
    }

    // 洛雪同步：设备接入后的「同步模式选择」弹窗（本机=电脑，对端=手机）
    Popup {
        id: lxModeDialog
        modal: true
        dim: true
        width: 420
        padding: 20
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        closePolicy: Popup.NoAutoClose
        background: Rectangle {
            color: theme.cardColor
            radius: 12
            border.color: theme.borderColor
        }
        contentItem: ColumnLayout {
            spacing: 10
            Text {
                text: "「" + lxsync.modeDeviceName + "」请求同步曲库"
                color: theme.textColor; font.pixelSize: 15; font.bold: true
                Layout.fillWidth: true; wrapMode: Text.Wrap
            }
            Text {
                text: "两台设备的收藏/歌单都有数据，请选择合并方式（仅首次连接会询问，之后按快照自动三方收敛）："
                color: theme.subTextColor; font.pixelSize: 11; wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            Repeater {
                model: [
                    { id: "merge_local_remote",         name: "合并：本机在前、手机在后（推荐）" },
                    { id: "merge_remote_local",         name: "合并：手机在前、本机在后" },
                    { id: "overwrite_local_remote",     name: "以本机为主合并到手机" },
                    { id: "overwrite_remote_local",     name: "以手机为主合并到本机" },
                    { id: "overwrite_local_remote_full", name: "完全覆盖手机（手机=本机）" },
                    { id: "overwrite_remote_local_full", name: "完全覆盖本机（本机=手机）" }
                ]
                delegate: Rectangle {
                    required property var modelData
                    Layout.fillWidth: true
                    height: 34; radius: 8
                    color: lxModeHover.containsMouse ? theme.hoverColor : theme.cardColor
                    border.color: theme.borderColor; border.width: 1
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left; anchors.leftMargin: 10
                        text: modelData.name
                        color: theme.textColor; font.pixelSize: 12
                    }
                    MouseArea {
                        id: lxModeHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { lxsync.answerSyncMode(modelData.id); lxModeDialog.close() }
                    }
                }
            }
            Rectangle {
                Layout.fillWidth: true
                height: 34; radius: 8
                color: lxCancelHover.containsMouse ? theme.hoverColor : "transparent"
                border.color: theme.borderColor; border.width: 1
                Text {
                    anchors.centerIn: parent
                    text: "取消（保持两台不动，断开本次同步）"
                    color: theme.subTextColor; font.pixelSize: 12
                }
                MouseArea {
                    id: lxCancelHover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { lxsync.answerSyncMode("cancel"); lxModeDialog.close() }
                }
            }
        }
        Connections {
            target: lxsync
            function onModeDialogRequested(deviceName) { lxModeDialog.open() }
            // 服务端已解决/取消（超时或设备断开）→ 自动收起
            function onModeDialogVisibleChanged() { if (!lxsync.modeDialogVisible) lxModeDialog.close() }
        }
    }

    // 退出确认弹窗（可记住选择）
    Popup {
        id: exitDialog
        objectName: "exitDialogObj"     // 自检用（--test-esc-ladder：断言关窗意图落到 onClosing 分流）
        property bool rememberChecked: false
        modal: true
        dim: true
        // #19：必须 focus:true——否则弹层不上焦点，CloseOnEscape 永不触发，
        // 用户按 Esc 想关确认框却"没反应"（HANDOFF #19 候选病因③）
        focus: true
        width: 380
        padding: 20
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        closePolicy: Popup.CloseOnEscape
        background: Rectangle {
            color: theme.cardColor
            radius: 12
            border.color: theme.borderColor
        }
        contentItem: ColumnLayout {
            spacing: 12
            Text {
                text: "退出暮云音乐？"
                color: theme.textColor
                font.pixelSize: 15
                font.bold: true
            }
            Text {
                text: "最小化到托盘后播放不会中断，可随时从托盘图标恢复或退出。"
                color: theme.subTextColor
                font.pixelSize: 12
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            // 不再提示勾选
            RowLayout {
                spacing: 8
                Rectangle {
                    width: 16; height: 16; radius: 4
                    color: exitDialog.rememberChecked ? theme.accentColor : "transparent"
                    border.color: exitDialog.rememberChecked ? theme.accentColor : theme.subTextColor
                    border.width: 1
                    Text {
                        anchors.centerIn: parent
                        text: "✓"
                        color: "white"
                        font.pixelSize: 11
                        visible: exitDialog.rememberChecked
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: exitDialog.rememberChecked = !exitDialog.rememberChecked
                    }
                }
                Text { text: "不再提示，下次按此执行"; color: theme.textColor; font.pixelSize: 12 }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Item { Layout.fillWidth: true }
                Rectangle {
                    width: 110; height: 34; radius: 17
                    color: minExitHover.containsMouse ? theme.hoverColor : theme.cardColor
                    border.color: theme.borderColor
                    border.width: 1
                    Text { anchors.centerIn: parent; text: "最小化到托盘"; color: theme.textColor; font.pixelSize: 12 }
                    MouseArea {
                        id: minExitHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (exitDialog.rememberChecked) settings.exitAction = "minimize"
                            exitDialog.close()
                            root.hide()
                            tray.minimizeToTray()
                        }
                    }
                }
                Rectangle {
                    width: 96; height: 34; radius: 17
                    color: quitExitHover.containsMouse ? Qt.lighter(theme.accentColor, 1.15) : theme.accentColor
                    Text { anchors.centerIn: parent; text: "退出程序"; color: "white"; font.pixelSize: 12 }
                    MouseArea {
                        id: quitExitHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (exitDialog.rememberChecked) settings.exitAction = "exit"
                            Qt.quit()
                        }
                    }
                }
            }
        }
    }

    // ==================================================================
    // 播放队列抽屉（右侧滑出）
    // ==================================================================
    SideDrawer {
        id: queueDrawer
        anchors.fill: parent
        title: "播放队列"
        drawerWidth: 360

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // 头部：统计 + 清空
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 40
                color: "transparent"
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 18
                    anchors.rightMargin: 14
                    Text {
                        text: "共 " + player.playlist.length + " 首"
                        color: theme.subTextColor
                        font.pixelSize: 11
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: "清空队列"
                        color: clearQueueHover.containsMouse ? theme.accentColor : theme.subTextColor
                        font.pixelSize: 11
                        visible: player.playlist.length > 0
                        MouseArea {
                            id: clearQueueHover
                            anchors.fill: parent
                            anchors.margins: -6
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: player.clearQueue()
                        }
                    }
                }
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: theme.borderColor }

            // 队列列表
            ListView {
                id: queueList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: player.playlist
                // 打开抽屉时滚到当前播放项
                Component.onCompleted: positionViewAtIndex(Math.max(0, player.currentIndex), ListView.Center)
                onCountChanged: {
                    if (queueDrawer.opened && player.currentIndex >= 0)
                        positionViewAtIndex(player.currentIndex, ListView.Center)
                }

                Text {
                    anchors.centerIn: parent
                    text: "队列为空\n播放歌曲后会出现在这里"
                    color: theme.subTextColor
                    font.pixelSize: 12
                    horizontalAlignment: Text.AlignHCenter
                    visible: queueList.count === 0
                }

                delegate: Rectangle {
                    id: qItem
                    required property var modelData
                    required property int index
                    property bool isCurrent: index === player.currentIndex
                    property bool hovered: false   // 独立维护，避免删除按钮出现即消失的闪烁
                    width: queueList.width
                    height: 52
                    color: qItem.hovered ? theme.hoverColor : "transparent"

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 18
                        anchors.rightMargin: 10
                        spacing: 10

                        // 封面
                        Rectangle {
                            Layout.preferredWidth: 36
                            Layout.preferredHeight: 36
                            radius: 6
                            color: theme.cardColor
                            clip: true
                            Image {
                                anchors.fill: parent
                                source: qItem.modelData.cover || ""
                                sourceSize: Qt.size(360, 360)
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                visible: status === Image.Ready
                            }
                            Icon {
                                anchors.centerIn: parent
                                name: "music"
                                iconSize: 15
                                iconColor: theme.subTextColor
                                visible: !qItem.modelData.cover
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                Layout.fillWidth: true
                                text: qItem.modelData.name || "未知歌曲"
                                color: qItem.isCurrent ? theme.accentColor : theme.textColor
                                font.pixelSize: 12
                                font.bold: qItem.isCurrent
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                text: qItem.modelData.artist || ""
                                color: theme.subTextColor
                                font.pixelSize: 10
                                elide: Text.ElideRight
                            }
                        }

                        // 从队列移除（hover 显示，参照 NavItem 防闪烁模式）
                        Rectangle {
                            id: qDelBtn
                            visible: qItem.hovered
                            Layout.preferredWidth: 24
                            Layout.preferredHeight: 24
                            radius: 12
                            color: qDelHover.containsMouse ? theme.accentColor : theme.hoverColor
                            Icon {
                                anchors.centerIn: parent
                                name: "window-close"
                                iconSize: 11
                                iconColor: qDelHover.containsMouse ? "white" : theme.subTextColor
                            }
                            MouseArea {
                                id: qDelHover
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onEntered: qItem.hovered = true
                                onClicked: player.removeFromQueue(qItem.index)
                            }
                        }
                    }

                    // 行点击（切歌）：必须 z:-1 放最底层，否则盖住删除按钮
                    MouseArea {
                        id: qItemHover
                        anchors.fill: parent
                        z: -1
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton
                        cursorShape: Qt.PointingHandCursor
                        onEntered: qItem.hovered = true
                        onExited: {
                            // 鼠标彻底移出整行才隐藏（移入删除按钮时 qDelHover.onEntered 会保持 true）
                            if (!qDelHover.containsMouse) qItem.hovered = false
                        }
                        onClicked: player.playIndex(qItem.index)
                    }
                }
            }
        }
    }

    // ==================================================================
    // 音源切换抽屉（右侧滑出，替代顶部下拉）
    // ==================================================================
    SideDrawer {
        id: sourceDrawer
        anchors.fill: parent
        title: "选择音源"
        drawerWidth: 320

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            Text {
                Layout.fillWidth: true
                Layout.margins: 18
                text: "音源脚本决定歌曲播放链接的获取方式"
                color: theme.subTextColor
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }

            ListView {
                id: srcList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: settings.lxSources

                Text {
                    anchors.centerIn: parent
                    text: "还没有音源脚本\n请到设置-在线音源导入"
                    color: theme.subTextColor
                    font.pixelSize: 12
                    horizontalAlignment: Text.AlignHCenter
                    visible: srcList.count === 0
                }

                delegate: Rectangle {
                    id: srcRow
                    required property var modelData
                    property bool isCurrent: settings.activeLxSourceId === modelData.id
                    width: srcList.width
                    height: 58
                    color: srcRowHover.containsMouse ? theme.hoverColor : "transparent"

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 18
                        anchors.rightMargin: 14
                        spacing: 10

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                Layout.fillWidth: true
                                text: srcRow.modelData.name
                                color: srcRow.isCurrent ? theme.accentColor : theme.textColor
                                font.pixelSize: 13
                                font.bold: srcRow.isCurrent
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                text: "v" + srcRow.modelData.version + (srcRow.modelData.enabled ? "" : "（已停用）")
                                color: theme.subTextColor
                                font.pixelSize: 10
                                elide: Text.ElideRight
                            }
                        }

                        Text {
                            text: "✓"
                            color: theme.accentColor
                            font.pixelSize: 15
                            visible: srcRow.isCurrent
                        }
                    }

                    MouseArea {
                        id: srcRowHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (settings.lxLoading) { notify("音源加载中，请稍候…"); return }
                            if (!srcRow.isCurrent) {
                                settings.setActiveLxSource(srcRow.modelData.id)
                                notify(settings.lxLoading ? "正在加载音源：" + srcRow.modelData.name
                                                          : "已切换音源：" + srcRow.modelData.name)
                            }
                            sourceDrawer.close()
                        }
                    }
                }
            }
        }
    }

    // 下载管理抽屉
    SideDrawer {
        id: downloadDrawer
        anchors.fill: parent
        title: "下载管理"
        drawerWidth: 420

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // 顶部：目标目录 + 音质选择
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: ddInfo.implicitHeight + 20
                color: "transparent"
                ColumnLayout {
                    id: ddInfo
                    anchors.fill: parent
                    anchors.leftMargin: 18
                    anchors.rightMargin: 14
                    anchors.topMargin: 12
                    anchors.bottomMargin: 8
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Text {
                            text: "保存到："
                            color: theme.subTextColor
                            font.pixelSize: 11
                        }
                        Text {
                            Layout.fillWidth: true
                            text: downloads.downloadPath
                            color: theme.textColor
                            font.pixelSize: 11
                            elide: Text.ElideMiddle
                        }
                        Text {
                            text: "更改"
                            color: ddPathHover.containsMouse ? theme.accentColor : theme.subTextColor
                            font.pixelSize: 11
                            MouseArea {
                                id: ddPathHover
                                anchors.fill: parent
                                anchors.margins: -6
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: ddPathDialog.open()
                            }
                        }
                    }
                    // （音质筛选行已移除：每档音质直接标注在列表歌曲名后面）
                }
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: theme.borderColor }

            // 汇总行
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 34
                color: "transparent"
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 18
                    anchors.rightMargin: 14
                    Text {
                        text: "进行 " + downloads.activeCount + " · 已完成 " + downloads.doneCount
                              + " · 共 " + downloads.items.length + " 项"
                        color: theme.subTextColor
                        font.pixelSize: 11
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: "清空已完成"
                        color: ddClearHover.containsMouse ? theme.accentColor : theme.subTextColor
                        font.pixelSize: 11
                        visible: downloads.doneCount > 0
                        MouseArea {
                            id: ddClearHover
                            anchors.fill: parent
                            anchors.margins: -6
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: downloads.clearCompleted()
                        }
                    }
                }
            }

            // 列表
            ListView {
                id: ddList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: downloads.items

                Text {
                    anchors.centerIn: parent
                    text: "还没有下载任务\n右键歌曲可以「下载」"
                    color: theme.subTextColor
                    font.pixelSize: 12
                    horizontalAlignment: Text.AlignHCenter
                    visible: ddList.count === 0
                }

                delegate: Rectangle {
                    id: ddItem
                    required property var modelData
                    required property int index
                    property int st: modelData.status || 0
                    property bool hovered: false
                    property real prog: (modelData.total && modelData.total > 0)
                                        ? (modelData.received / modelData.total) : 0
                    width: ddList.width
                    height: 62
                    color: ddItem.hovered ? theme.hoverColor : "transparent"

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 18
                        anchors.rightMargin: 10
                        spacing: 10

                        // 封面
                        Rectangle {
                            Layout.preferredWidth: 40
                            Layout.preferredHeight: 40
                            radius: 6
                            color: theme.cardColor
                            clip: true
                            Image {
                                anchors.fill: parent
                                source: (ddItem.modelData.song && ddItem.modelData.song.cover) ? ddItem.modelData.song.cover : ""
                                sourceSize: Qt.size(120, 120)
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                visible: status === Image.Ready
                            }
                            Icon {
                                anchors.centerIn: parent
                                name: "music"
                                iconSize: 16
                                iconColor: theme.subTextColor
                                visible: !(ddItem.modelData.song && ddItem.modelData.song.cover)
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3
                            Text {
                                Layout.fillWidth: true
                                text: {
                                    var nm = (ddItem.modelData.song && ddItem.modelData.song.name)
                                             ? ddItem.modelData.song.name : "未知歌曲"
                                    var q = root.qualityLabel(ddItem.modelData.actualQuality)
                                    return q ? (nm + " · " + q) : nm
                                }
                                color: theme.textColor
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 6
                                Text {
                                    text: {
                                        if (ddItem.st === 0) return "排队中"
                                        if (ddItem.st === 1) return (ddItem.modelData.attemptNote
                                                                      || "解析中…")
                                        if (ddItem.st === 2) return "下载中"
                                        if (ddItem.st === 3) return "已完成"
                                        if (ddItem.st === 4) return "失败：" + (ddItem.modelData.error || "")
                                        return "已取消"
                                    }
                                    color: ddItem.st === 4 ? "#ff6b6b"
                                           : (ddItem.st === 3 ? theme.accentColor : theme.subTextColor)
                                    font.pixelSize: 10
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                Text {
                                    text: (ddItem.st === 2)
                                          ? (downloads.formatBytes(ddItem.modelData.received || 0) + " / " +
                                             (ddItem.modelData.total > 0 ? downloads.formatBytes(ddItem.modelData.total) : "?"))
                                          : (ddItem.st === 3 ? downloads.formatBytes(ddItem.modelData.received || 0) : "")
                                    color: theme.subTextColor
                                    font.pixelSize: 10
                                    visible: text.length > 0
                                }
                            }
                            // 进度条
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 3
                                radius: 1.5
                                color: theme.borderColor
                                visible: ddItem.st === 2 || ddItem.st === 1
                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    width: parent.width * ddItem.prog
                                    radius: 1.5
                                    color: theme.accentColor
                                    Behavior on width { NumberAnimation { duration: 200 } }
                                }
                            }
                        }

                        // 操作按钮（hover 显示）
                        RowLayout {
                            spacing: 4
                            visible: ddItem.hovered
                            Layout.alignment: Qt.AlignVCenter

                            // 打开所在目录
                            Rectangle {
                                width: 24; height: 24; radius: 12
                                color: ddRevealHover.containsMouse ? theme.accentColor : theme.hoverColor
                                visible: ddItem.st === 3
                                Icon {
                                    anchors.centerIn: parent
                                    name: "folder"
                                    iconSize: 12
                                    iconColor: ddRevealHover.containsMouse ? "white" : theme.subTextColor
                                }
                                MouseArea {
                                    id: ddRevealHover
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onEntered: ddItem.hovered = true
                                    onClicked: downloads.revealInFolder(ddItem.modelData.id)
                                }
                            }
                            // 取消 / 重试
                            Rectangle {
                                width: 24; height: 24; radius: 12
                                color: ddActHover.containsMouse ? theme.accentColor : theme.hoverColor
                                visible: ddItem.st === 0 || ddItem.st === 1 || ddItem.st === 2 || ddItem.st === 4 || ddItem.st === 5
                                Icon {
                                    anchors.centerIn: parent
                                    name: (ddItem.st === 4 || ddItem.st === 5) ? "refresh" : "window-close"
                                    iconSize: 11
                                    iconColor: ddActHover.containsMouse ? "white" : theme.subTextColor
                                }
                                MouseArea {
                                    id: ddActHover
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onEntered: ddItem.hovered = true
                                    onClicked: {
                                        if (ddItem.st === 4 || ddItem.st === 5)
                                            downloads.retryDownload(ddItem.modelData.id)
                                        else
                                            downloads.cancelDownload(ddItem.modelData.id)
                                    }
                                }
                            }
                            // 删除记录（保留文件）
                            Rectangle {
                                width: 24; height: 24; radius: 12
                                color: ddDelHover.containsMouse ? theme.accentColor : theme.hoverColor
                                visible: ddItem.st === 3 || ddItem.st === 4 || ddItem.st === 5
                                Icon {
                                    anchors.centerIn: parent
                                    name: "delete"
                                    iconSize: 12
                                    iconColor: ddDelHover.containsMouse ? "white" : theme.subTextColor
                                }
                                MouseArea {
                                    id: ddDelHover
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onEntered: ddItem.hovered = true
                                    onClicked: downloads.removeItem(ddItem.modelData.id)
                                }
                            }
                        }
                    }

                    // 行 hover（不响应点击）：底层，避免遮挡按钮
                    MouseArea {
                        anchors.fill: parent
                        z: -1
                        hoverEnabled: true
                        acceptedButtons: Qt.NoButton
                        onEntered: ddItem.hovered = true
                        onExited: {
                            if (!ddRevealHover.containsMouse && !ddActHover.containsMouse && !ddDelHover.containsMouse)
                                ddItem.hovered = false
                        }
                    }
                }
            }

            // 底部提示
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 30
                color: "transparent"
                Text {
                    anchors.centerIn: parent
                    text: "下载完成后会自动加入本地音乐库"
                    color: theme.subTextColor
                    font.pixelSize: 10
                    visible: downloads.doneCount > 0
                }
            }
        }

        // 下载目录选择对话框（Qt6 用 FolderDialog 选目录）
        FolderDialog {
            id: ddPathDialog
            title: "选择下载目录"
            onAccepted: {
                var s = selectedFolder.toString()
                if (s.indexOf("file:///") === 0) s = s.substring(8)
                else if (s.indexOf("file://") === 0) s = s.substring(7)
                try { s = decodeURIComponent(s) } catch (e) {}
                if (s.length > 0) downloads.downloadPath = s
            }
        }
    }

    // 音源更新推送弹窗
    Popup {
        id: updateDialog
        modal: true
        dim: true
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        width: 480
        padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: theme.cardColor
            radius: 14
            border.color: theme.borderColor
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // ---------------- 头部 ----------------
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 104
                // 底部分隔线
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: theme.borderColor
                }
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 14
                    // 图标徽标
                    Rectangle {
                        Layout.preferredWidth: 44
                        Layout.preferredHeight: 44
                        radius: 12
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: theme.accentColor }
                            GradientStop { position: 1.0; color: Qt.darker(theme.accentColor, 1.35) }
                        }
                        Icon {
                            name: "refresh"
                            anchors.centerIn: parent
                            iconSize: 22
                            iconColor: "#ffffff"
                        }
                    }
                    // 标题 + 描述（右侧留白避免被右上角关闭键压住）
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.rightMargin: 24
                        Layout.alignment: Qt.AlignVCenter
                        spacing: 3
                        Text {
                            text: "LX 音源更新提醒"
                            color: theme.accentColor
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                        }
                        Text {
                            Layout.fillWidth: true
                            text: settings.lxUpdateAlert.name || "LX 音源"
                            color: theme.textColor
                            font.pixelSize: 17
                            font.weight: Font.Bold
                            elide: Text.ElideRight
                        }
                        Text {
                            visible: (settings.lxUpdateAlert.description || "").length > 0
                            Layout.fillWidth: true
                            text: settings.lxUpdateAlert.description || ""
                            color: theme.subTextColor
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                        }
                    }
                }
                // 右上角关闭
                Item {
                    id: closeBtn
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.margins: 10
                    width: 30
                    height: 30
                    Rectangle {
                        anchors.fill: parent
                        radius: 15
                        color: closeHover.containsMouse ? theme.hoverColor : "transparent"
                    }
                    Icon {
                        name: "window-close"
                        anchors.centerIn: parent
                        iconSize: 16
                        iconColor: theme.subTextColor
                    }
                    MouseArea {
                        id: closeHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            updateDialog.close()
                            settings.dismissLxUpdateAlert()
                        }
                    }
                }
            }

            // ---------------- 主体 ----------------
            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                Layout.topMargin: 18
                Layout.bottomMargin: 16
                spacing: 12

                // 版本 / 脚本 信息卡
                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 10
                    rowSpacing: 10

                    // 当前脚本版本
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 62
                        radius: 10
                        color: theme.panelColor
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 10
                            anchors.topMargin: 12
                            spacing: 3
                            Text {
                                text: "当前脚本版本"
                                color: theme.subTextColor
                                font.pixelSize: 11
                            }
                            Text {
                                Layout.fillWidth: true
                                text: settings.lxUpdateAlert.version || "未标注版本"
                                color: theme.textColor
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                    // 音源作者
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 62
                        radius: 10
                        color: theme.panelColor
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 10
                            anchors.topMargin: 12
                            spacing: 3
                            Text {
                                text: "作者"
                                color: theme.subTextColor
                                font.pixelSize: 11
                            }
                            Text {
                                Layout.fillWidth: true
                                text: settings.lxUpdateAlert.author || "未知"
                                color: theme.textColor
                                font.pixelSize: 13
                                elide: Text.ElideMiddle
                            }
                        }
                    }
                }

                // 更新内容
                Rectangle {
                    Layout.fillWidth: true
                    radius: 10
                    color: theme.panelColor
                    border.color: theme.borderColor
                    clip: true
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 6
                        Text {
                            text: "更新内容"
                            color: theme.textColor
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                        }
                        Item {
                            Layout.fillWidth: true
                            // 内容过长时内部滚动，弹窗高度不失控
                            Layout.preferredHeight: Math.min(logText.implicitHeight, 168)
                            Flickable {
                                anchors.fill: parent
                                contentHeight: logText.height
                                boundsBehavior: Flickable.StopAtBounds
                                flickableDirection: Flickable.VerticalFlick
                                Text {
                                    id: logText
                                    width: parent.width
                                    text: settings.lxUpdateAlert.log || ""
                                    color: theme.subTextColor
                                    font.pixelSize: 13
                                    lineHeight: 1.55
                                    wrapMode: Text.Wrap
                                }
                            }
                        }
                    }
                }
            }

            // ---------------- 底部操作栏 ----------------
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 70
                Rectangle {
                    anchors.top: parent.top
                    width: parent.width
                    height: 1
                    color: theme.borderColor
                }
                RowLayout {
                    anchors.centerIn: parent
                    spacing: 10
                    // 关闭（文字按钮）
                    Item {
                        implicitHeight: 34
                        width: alertCloseText.width + 28
                        Text {
                            id: alertCloseText
                            anchors.centerIn: parent
                            text: "关闭"
                            color: alertCloseHover.containsMouse ? theme.textColor : theme.subTextColor
                            font.pixelSize: 13
                        }
                        MouseArea {
                            id: alertCloseHover
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                updateDialog.close()
                                settings.dismissLxUpdateAlert()
                            }
                        }
                    }
                    // 打开更新地址
                    Rectangle {
                        id: goBtn
                        visible: (settings.lxUpdateAlert.updateUrl || "").length > 0
                        radius: 8
                        color: goHover.pressed ? Qt.darker(theme.accentColor, 1.12)
                              : goHover.containsMouse ? Qt.lighter(theme.accentColor, 1.08)
                              : theme.accentColor
                        implicitHeight: 34
                        implicitWidth: goRow.implicitWidth + 28
                        RowLayout {
                            id: goRow
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 14
                            anchors.rightMargin: 14
                            spacing: 6
                            Text {
                                text: "打开更新地址"
                                color: "#ffffff"
                                font.pixelSize: 13
                            }
                            Icon {
                                name: "link"
                                iconSize: 14
                                iconColor: "#ffffff"
                            }
                        }
                        MouseArea {
                            id: goHover
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                settings.openLxUpdateUrl()   // C++ 侧 QDesktopServices，失败会 toast
                                updateDialog.close()
                                settings.dismissLxUpdateAlert()
                            }
                        }
                    }
                }
            }
        }
    }

    FolderDialog {
        id: folderDialog
        title: "选择音乐目录"
        onAccepted: {
            var p = selectedFolder.toString()
            if (p.indexOf("file:///") === 0) p = p.substring(8)
            library.addFolder(decodeURIComponent(p))
        }
    }

    Connections {
        target: library
        function onFoldersChanged() {}
        // 收藏列表实时刷新：toggleFavorite 后 favoritesChanged 发出，
        // 若当前正停留在收藏页则立即重新拉取列表，否则要切页才更新。
        // 重赋数组前显式捕获滚动位置（否则 ListView 模型重置跳顶）。
        function onFavoritesChanged() {
            if (root.currentPage === "favorites") {
                listPage.preserveScroll()
                root.songList = library.favorites
            }
        }
        // 最近播放同理：播放完一首切回最近播放页应立即看到新记录
        function onRecentChanged() {
            if (root.currentPage === "recent") {
                listPage.preserveScroll()
                root.songList = library.recent
            }
        }
    }

    // ==================================================================
    // 逻辑
    // ==================================================================
    // 音质 id → 展示名（下载列表/抽屉共用）
    function qualityLabel(qid) {
        var m = { "master": "Master", "atmos": "Atmos", "hires": "Hi-Res",
                  "flac24bit": "24Bit", "flac": "FLAC", "320k": "320K",
                  "192k": "192K", "128k": "128K", "96k": "96K" }
        return m[qid] || qid || ""
    }

    function goPage(page) {        // 本地音乐：先确保有扫描目录，否则引导到设置里添加
        root.onlinePl = null       // 离开在线歌单，收藏按钮就该消失
        if (page === "local") {
            if (library.folders.length === 0) { openSettings("local"); return }
            root.currentPage = "local"
            root.playContextId = "local"
            root.listTitle = "本地音乐"
            root.songList = library.localSongs
            return
        }
        root.currentPage = page
        var songs = []
        if (page === "favorites") { songs = library.favorites; root.listTitle = "收藏"; root.playContextId = "favorites" }
        else if (page === "recent") { songs = library.recent; root.listTitle = "最近播放"; root.playContextId = "recent" }
        else if (page === "home") { root.listTitle = ""; return }
        root.songList = songs
    }

    // 进入某个歌单（侧边栏直接展开的歌单）
    function goPlaylist(id, name) {
        root.onlinePl = null
        root.currentPage = "playlistDetail"
        root.playContextId = id
        root.listTitle = name
        root.songList = library.playlistSongs(id)
    }

    function doSearch(text) {
        var kw = (text || "").trim()
        if (kw.length === 0) return
        // 收起联想下拉：否则下拉(z:999)悬在结果列表上方吞掉歌曲点击（表现为"点了不切歌"）
        searchInput.focus = false
        root.currentPage = "search"
        root.listTitle = "搜索：" + kw
        searcher.search(kw)
    }

    function playAllSongs() {
        if (root.songList.length > 0)
            player.playAll(root.songList, root.playContextId || root.currentPage, root.listTitle)
    }

    function notify(msg) { toast.show(msg) }

    // 批量下载：qid 空=用下载队列的默认音质（批量工具条那颗小下拉，four-56）；
    // 本地歌、已在队列里的由 C++ 静默跳过（不刷屏弹提示）
    function batchDownload(songs, qid) {
        if (!songs || songs.length === 0) { notify("先勾选要下载的歌曲"); return }
        var n = downloads.addDownloads(songs, qid || "")
        if (n > 0) {
            var qname = ""
            if (qid && qid.length > 0) {
                var opts = downloads.qualityOptions()
                for (var i = 0; i < opts.length; i++)
                    if (opts[i].id === qid) qname = opts[i].name
            }
            notify("已加入下载 " + n + " 首" + (qname ? "（" + qname + "）" : "")
                   + (n < songs.length ? ("（跳过 " + (songs.length - n) + " 首：本地歌或已在下载列表）") : ""))
        }
    }

    // 批量移除：收藏页=取消收藏，歌单详情页=移出歌单。
    // 都走 C++ 批量接口（一次保存一次通知），列表刷新由 favoritesChanged/playlistsChanged 负责（会保滚动）
    function batchRemove(songs) {
        if (!songs || songs.length === 0) { notify("先勾选要移除的歌曲"); return }
        var keys = []
        for (var i = 0; i < songs.length; i++) {
            var k = library.identityOf(songs[i])
            if (k.length > 0) keys.push(k)
        }
        if (keys.length === 0) { notify("所选歌曲无法识别，请重试"); return }
        var n = 0
        if (root.currentPage === "favorites") {
            n = library.removeFavorites(keys)
            notify("已取消收藏 " + n + " 首")
        } else {
            n = library.removeSongsFromPlaylist(root.playContextId, keys)
            notify("已从歌单移除 " + n + " 首")
        }
        listPage.clearSelection()
    }

    function openEffects() { effectsPanel.open() }

    function openQueueDrawer() { queueDrawer.open() }

    function openSettings(section) {
        if (section !== undefined) settingsPanel.section = section
        settingsPanel.open()
    }

    // 打开全屏歌词页前让搜索框失焦：否则搜索框仍持有焦点 → escClaimed=true →
    // ESC 被"让位"给搜索框 → 必须点一下歌词页 ESC 才能退（用户实测）。
    function openLyricsPage() {
        searchInput.focus = false
        lyricsPage.open()
    }
    function closeLyricsPage() { lyricsPage.close() }

    // 榜单歌曲加载完成 -> 切到列表页（autoPlay=true 才自动播放）
    Connections {
        target: home
        function onToplistSongsReady(songs, name, autoPlay) {
            root.onlinePl = null
            root.listTitle = name
            root.songList = songs
            root.playContextId = "toplist-" + name
            root.currentPage = "toplist"
            root.listBackTarget = "home"   // 榜单只能从首页进入
            if (songs.length === 0) { notify("榜单加载失败或为空"); return }
            if (autoPlay) player.playAll(songs, root.playContextId, name)
        }
    }

    // 在线歌单（推荐/广场）歌曲加载完成
    Connections {
        target: home
        function onPlaylistSongsReady(songs, name, id) {
            root.listTitle = name
            root.songList = songs
            root.playContextId = "online-" + id
            root.currentPage = "playlistDetail"
        }
    }

    // 在线歌单打开（推荐歌单/广场歌单共用）
    // from = 来源页，用于"返回"按钮回到哪一页（"home" / "explore"）
    function openOnlinePlaylist(pl, from) {
        if (!pl || !pl.id) { notify("无法打开该歌单"); return }
        // 记住这张在线歌单：广场/推荐给的 map 里 platform 可能缺，补成当前音源，
        // 否则收藏键（平台+源歌单id）会两边对不上，表现为"收藏了却显示未收藏"
        if (!pl.platform) pl.platform = home.platform
        root.onlinePl = pl
        root.listBackTarget = from || "home"
        notify("歌单加载中…")
        home.loadPlaylistSongs(pl.platform || home.platform, String(pl.id), pl.name)
    }

    Connections {
        target: searcher
        function onResultsChanged() {
            if (root.currentPage !== "search") return
            root.songList = searcher.results
        }
        // 行内平台标签点击切换后，顶栏下拉实时跟随更新
        function onPlatformChanged() { platformBox.syncFromPlatform() }
    }

    Connections {
        target: player
        function onPlayFailed(message) { toast.show(message) }
        // 点任何列表行切歌时收起联想/热搜下拉（下拉 z:999 悬在内容区上会吞点击）
        function onCurrentSongChanged() {
            searchInput.focus = false
            // #12：当前曲音质/体积预取（不再等打开弹窗/菜单才拉，播放条与下载弹窗打开即有条目）
            if (typeof downloads !== "undefined" && player.currentSong
                && player.currentSong.platform !== "local" && player.currentSong.name)
                downloads.prefetchSizes([player.currentSong])
        }
    }

    Connections {
        target: library
        function onLocalSongsChanged() { if (root.currentPage === "local") goPage("local") }
    }

    // 歌单变化（移除歌曲/删除歌单）时即时刷新当前列表
    Connections {
        target: library
        function onPlaylistsChanged() {
            if (root.currentPage !== "playlistDetail" || root.playContextId.length === 0) return
            // 在线歌单详情（广场/推荐点进来）的歌曲来自在线，playContextId 是 "online-xxx"，
            // 拿它去查本地歌单必然查空 → 会把正在看的列表清空（收藏这张歌单时就会踩到）。
            if (root.onlinePl && root.playContextId === "online-" + String(root.onlinePl.id)) return
            listPage.preserveScroll()
            root.songList = library.playlistSongs(root.playContextId)
        }
    }

    Item {
        id: toast
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 110
        width: toastBox.width
        height: 38
        opacity: 0
        z: 1000

        property string message: ""

        Rectangle {
            id: toastBox
            width: toastText.width + 36
            height: 38
            radius: 19
            color: "#e6202028"
            border.color: theme.borderColor
            Text {
                id: toastText
                anchors.centerIn: parent
                text: toast.message
                color: "white"
                font.pixelSize: 12
            }
        }

        Timer { id: toastTimer; interval: 2400; onTriggered: toast.opacity = 0 }

        function show(msg) {
            message = msg
            opacity = 1
            toastTimer.restart()
        }

        Behavior on opacity { NumberAnimation { duration: 180 } }
    }

    // 全屏歌词页（覆盖内容，z:500；toast 已提到 z:1000 恒在上）
    LyricsPage {
        id: lyricsPage
        objectName: "lyricsPageObj"
        onClosed: {
            // 全屏歌词页关闭后重建输入法上下文：本页是无输入控件的大 Item，期间
            // Qt 摘掉整窗 IME；仅 reset/commit 不够（焦点项不重发 focusIn，Qt 不重建）。
            // 等价"点窗外再点回"的机制：强制 IME 关联重走 + 焦点往返触发重建。
            if (typeof imeGuard !== "undefined") imeGuard.forceRefresh()
            Qt.inputMethod.reset()
            Qt.inputMethod.commit()
            if (searchInput.activeFocus) {
                searchInput.focus = false
                searchInput.forceActiveFocus()
            }
        }
    }

    // 桌面歌词：由 C++ DesktopLyricsController 承载的独立置顶窗
    function toggleDesktopLyrics() { deskLyrics.toggle() }

    // 全屏切换：F11 走系统级热键（HotkeyManager 在 C++ 直接切换，绕开输入法吞键），
    // Ctrl+Shift+F / 标题栏按钮作备用。判断依据用意图位 fullScreenOn，不用 visibility（会被遮挡骗）
    function toggleFullScreen() {
        if (root.fullScreenOn) { root.fullScreenOn = false; root.showNormal() }
        else { root.fullScreenOn = true; root.showFullScreen() }
    }
    Shortcut {
        sequence: "Ctrl+Shift+F"
        onActivated: root.toggleFullScreen()
    }

    // 快捷键：Ctrl+L 开关歌词页；歌词页打开时 Esc 关闭、Space 播放/暂停
    Shortcut {
        sequence: "Ctrl+L"
        onActivated: lyricsPage.visible ? lyricsPage.close() : root.openLyricsPage()
    }
    Shortcut {
        sequence: "Ctrl+D"
        onActivated: root.toggleDesktopLyrics()
    }
    // Esc 阶梯（用户 2026-10-02 定前三步、2026-10-04 补齐后两步）：
    // ① 舞台沉浸模式 → 退沉浸；② 全屏播放/歌词页开着 → 关页；③ 窗口全屏(F11) → 退全屏；
    // ④ 窗口最大化 → 退出最大化；⑤ 都没有 → 关闭主窗口（走 root.close()，
    //    由 onClosing 按用户设置分流：询问 / 最小化到托盘 / 直接退出，别绕过它）。
    // 键的来源有两条，都汇到这里/页面：主窗有焦点时走下面的 Shortcut；焦点被舞台（独立进程
    // WebView2）叼走时走 C++ 的 Esc 系统热键（escapeRequested → runEscLadder 或 stage.sendEsc）。
    // 舞台开着时阶梯裁判交给页面（stage.sendEsc）——它才知道自己是否真在沉浸，主程序那份
    // 异步回报的 stage.immersive 会过期，早先"ESC 用一次就失灵"就是拿它猜出来的。
    function runEscLadder() {
        if (lyricsPage.visible && typeof stage !== "undefined" && stage.active) {
            if (stage.sendEsc()) return                      // 交页面走阶梯
            if (stage.immersive) stage.setImmersive(false)   // 送不出去才本地收尾
            else lyricsPage.close()
            return
        }
        if (lyricsPage.visible) lyricsPage.close()             // 播放页开着但舞台没开 → 退播放页
        else if (root.fullScreenOn) root.toggleFullScreen()    // 退窗口全屏
        else if (root.maximized) root.toggleMaximize()         // 退最大化（新增）
        else root.close()                                      // 关主窗口（新增）
    }

    // 焦点是否落在某个弹层（Popup / Menu / Drawer）里。
    // Qt 的 QQuickPopup 只在"自己有 active focus"时才用 Esc 关自己，且**没有** ShortcutOverride
    // 保护——窗口级 Shortcut 一旦常开，就会把"关菜单"这一键吃下去顺手关掉主窗（灾难）。
    // 弹层项都挂在窗口的 Overlay 下面，所以从焦点项往上找父级，撞到 Overlay 就算数。
    // 绑定靠 activeFocusItem 变化驱动（它带 NOTIFY），不会算一次就冻住。
    readonly property bool focusInsidePopup: {
        var ov = Overlay.overlay
        var it = root.activeFocusItem
        if (!ov || !it) return false
        while (it) {
            if (it === ov) return true
            it = it.parent
        }
        return false
    }
    // Esc 的"让路"判据：有弹层/输入框在场时，这一键归它们，阶梯不许动。
    // 已知弹层逐个点名（它们不一定抢焦点）+ 通用焦点兜底（委托里的换源菜单等没法点名）。
    // 注意 searchInput.activeFocus：搜索框聚焦时按 Esc 是"让输入框自己处理"（无动作），
    // 不是关窗——这是设计行为，别把"搜索时 Esc 没反应"当 bug 报。
    readonly property bool escClaimed: settingsPanel.visible || effectsPanel.visible
        || queueDrawer.visible || sourceDrawer.visible || downloadDrawer.visible
        || newPlaylistDialog.visible || deletePlaylistDialog.visible
        || lxModeDialog.visible || exitDialog.visible || appUpdateDialog.visible
        || searchInput.activeFocus || root.focusInsidePopup

    Shortcut {
        sequence: "Escape"
        enabled: !root.escClaimed
        onActivated: root.runEscLadder()
    }
    // Esc 系统热键的接管窗口期（HANDOFF #19 修"有时 Esc 关不掉主窗口"）：
    // 旧版只覆盖"播放页/舞台/全屏/最大化"——普通窗口态 + 焦点被自家另一个窗
    // （桌面歌词独立窗、舞台壳等）叼走时，QML Shortcut 收不到键、系统热键又没注册，
    // 两头都断 → Esc 没反应（最符合用户"有时"的观察）。
    // 现在**只要没有弹层/输入框占用就接管**：系统热键与前台归属无关地全局生效，
    // 焦点在自家哪个窗都不影响 Esc 送达 runEscLadder；弹层/输入框在场时让开，
    // 把 Esc 还给它们（点名清单 + Overlay 焦点链兜底）。C++ 侧还会再按
    // "前台是不是我们的窗"复查才真注册（切到别的应用立即让出）。
    readonly property bool escGuardWanted: !root.escClaimed
    onEscGuardWantedChanged: root.syncEscGuard()
    function syncEscGuard() {
        if (typeof hotkey !== "undefined") hotkey.setEscGuard(root.escGuardWanted)
    }
    Shortcut {
        sequence: "Space"
        enabled: lyricsPage.visible
        onActivated: player.togglePlay()
    }

    // ===== 边缘拖拽缩放：纯手动设几何，不走 OS 缩放循环（原生/startSystemResize 都抖）=====
    // 注意：内联组件作用域里 `root` 指组件自身，故用 target 传入窗口（实例处 target: root 才=窗口）
    component ResizeHandle: MouseArea {
        property var target
        property bool le: false
        property bool ri: false
        property bool tp: false
        property bool bt: false
        property real _sx: 0
        property real _sy: 0
        property real _sw: 0
        property real _sh: 0
        property real _gx: 0
        property real _gy: 0
        acceptedButtons: Qt.LeftButton
        cursorShape: ((le && tp) || (ri && bt)) ? Qt.SizeFDiagCursor
                     : ((ri && tp) || (le && bt)) ? Qt.SizeBDiagCursor
                     : ((le || ri) && (tp || bt)) ? Qt.SizeFDiagCursor
                     : (le || ri) ? Qt.SizeHorCursor : Qt.SizeVerCursor
        onPressed: (m) => {
            if (target.maximized) target.maximized = false   // 手动缩放即视为还原态
            _sx = target.x; _sy = target.y; _sw = target.width; _sh = target.height
            const gp = mapToGlobal(m.x, m.y)
            _gx = gp.x; _gy = gp.y
        }
        onPositionChanged: (m) => {
            if (!pressed) return
            const gp = mapToGlobal(m.x, m.y)
            const dx = gp.x - _gx
            const dy = gp.y - _gy
            var nx = _sx, ny = _sy, nw = _sw, nh = _sh
            if (ri) nw = _sw + dx
            if (bt) nh = _sh + dy
            if (le) { nw = _sw - dx; nx = _sx + dx }
            if (tp) { nh = _sh - dy; ny = _sy + dy }
            if (le && nw < target.minimumWidth) { nw = target.minimumWidth; nx = _sx + (_sw - nw) }
            if (tp && nh < target.minimumHeight) { nh = target.minimumHeight; ny = _sy + (_sh - nh) }
            if (nw >= target.minimumWidth) { target.width = nw; target.x = nx }
            if (nh >= target.minimumHeight) { target.height = nh; target.y = ny }
        }
    }
    readonly property int _rz: 6      // 边缘把手厚度
    ResizeHandle {
        z: 1000; target: root; ri: true; width: root._rz
        anchors { right: parent.right; top: parent.top; bottom: parent.bottom }
    }
    ResizeHandle {
        z: 1000; target: root; le: true; width: root._rz
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
    }
    ResizeHandle {
        z: 1000; target: root; bt: true; height: root._rz
        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
    }
    ResizeHandle {
        z: 1000; target: root; tp: true; height: root._rz
        anchors { top: parent.top; left: parent.left; right: parent.right }
    }
    ResizeHandle {
        z: 1001; target: root; le: true; tp: true; width: 12; height: 12
        anchors { left: parent.left; top: parent.top }
    }
    ResizeHandle {
        z: 1001; target: root; ri: true; tp: true; width: 12; height: 12
        anchors { right: parent.right; top: parent.top }
    }
    ResizeHandle {
        z: 1001; target: root; le: true; bt: true; width: 12; height: 12
        anchors { left: parent.left; bottom: parent.bottom }
    }
    ResizeHandle {
        z: 1001; target: root; ri: true; bt: true; width: 12; height: 12
        anchors { right: parent.right; bottom: parent.bottom }
    }

    Component.onCompleted: {
        // 启动时激活到前台，避免被其他软件窗口挡住
        root.raise()
        root.requestActivate()
        root.syncEscGuard()      // Esc 系统热键接管窗口期（播放页/舞台开着时）
        home.loadHome("wy")
        // #12：启动即预取当前曲（重启恢复的歌）的音质/体积
        if (typeof downloads !== "undefined" && player.currentSong
            && player.currentSong.platform !== "local" && player.currentSong.name)
            downloads.prefetchSizes([player.currentSong])
    }
}
