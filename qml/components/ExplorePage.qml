import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 歌单广场：分类标签 + 排序 + 歌单网格 + 加载更多
// 状态自管理（不依赖外部 property），内部 Connections 监听 home.exploreReady/exploreFailed
Item {
    id: page
    signal playlistClicked(var pl)
    signal backClicked()          // 返回上级（首页）

    // 当前平台（跟随首页选择的音源）
    property string plat: home.platform
    property string order: "hot"     // hot / new
    property bool loaded: false      // 首次可见才加载（避免开机即发请求）

    // ---------- 内部数据状态 ----------
    property var playlists: []
    property string cat: "全部"
    property int curPage: 1
    property bool hasMore: false
    property bool loading: false

    // 后端信号驱动刷新（列表页数据是快照，必须信号驱动）
    Connections {
        target: home
        function onExploreReady(list, cat, pg, more) {
            page.playlists = pg <= 1 ? list : page.playlists.concat(list)
            page.cat = cat
            page.curPage = pg
            page.hasMore = more
            page.loading = false
        }
        function onExploreFailed(message) {
            page.loading = false
        }
    }

    // 收集所有分类标签（首位"全部"）
    property var allTags: {
        var arr = ["全部"]
        var cats = home.playlistCategories
        for (var i = 0; i < cats.length; i++) {
            var tags = cats[i].tags || []
            for (var j = 0; j < tags.length; j++) arr.push(tags[j])
        }
        return arr
    }

    function reload(c) {
        // 四-59：**不清空旧列表**——命中缓存时第 1 页同步就回来了（看不到空态）；
        // 未命中时宁可短暂留着旧内容 + 顶部"加载中"提示，也比整页闪空体感快得多。
        page.cat = c || "全部"
        page.curPage = 1
        page.loading = true
        home.loadExplorePlaylists(plat, page.cat, order, 1)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 12

        // ---------- 标题 + 排序 ----------
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            // 返回上级（与列表页同款圆形箭头按钮）
            Rectangle {
                id: backBtn
                Layout.preferredWidth: 30
                Layout.preferredHeight: 30
                Layout.alignment: Qt.AlignVCenter
                radius: 15
                color: backMouse.containsMouse ? theme.hoverColor : "transparent"
                border.color: theme.borderColor
                border.width: 1
                Icon {
                    anchors.centerIn: parent
                    name: "chevron-left"
                    iconSize: 16
                    iconColor: theme.textColor
                }
                MouseArea {
                    id: backMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: page.backClicked()
                }
            }

            Text {
                text: "歌单广场"
                color: theme.textColor
                font.pixelSize: 20
                font.bold: true
            }
            Item { Layout.fillWidth: true }
            Repeater {
                model: [{ id: "hot", name: "最热" }, { id: "new", name: "最新" }]
                delegate: Rectangle {
                    required property var modelData
                    height: 28
                    width: orderText.width + 24
                    radius: 14
                    color: page.order === modelData.id ? theme.accentColor : theme.hoverColor
                    Text {
                        id: orderText
                        anchors.centerIn: parent
                        text: modelData.name
                        color: page.order === modelData.id ? "white" : theme.textColor
                        font.pixelSize: 12
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (page.order === modelData.id) return
                            page.order = modelData.id
                            reload(page.cat)
                        }
                    }
                }
            }
        }

        // ---------- 分类标签（横向滚动） ----------
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 32
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AsNeeded
            ScrollBar.vertical.policy: ScrollBar.AlwaysOff
            Row {
                spacing: 8
                Repeater {
                    model: page.allTags
                    delegate: Rectangle {
                        required property var modelData
                        height: 28
                        width: tagText.width + 22
                        radius: 14
                        color: page.cat === modelData ? theme.accentColor : theme.cardColor
                        border.color: page.cat === modelData ? theme.accentColor : theme.borderColor
                        border.width: 1
                        Text {
                            id: tagText
                            anchors.centerIn: parent
                            text: modelData
                            color: page.cat === modelData ? "white" : theme.subTextColor
                            font.pixelSize: 12
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: reload(modelData)
                        }
                    }
                }
            }
        }

        // ---------- 歌单网格 ----------
        GridView {
            id: grid
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            cellWidth: Math.floor(width / 5)
            cellHeight: Math.floor(width / 5) + 46
            boundsBehavior: Flickable.StopAtBounds
            model: page.playlists

            onAtYEndChanged: {
                // 触底自动加载下一页
                if (atYEnd && page.hasMore && !page.loading) {
                    page.loading = true
                    page.curPage += 1
                    home.loadExplorePlaylists(page.plat, page.cat, page.order, page.curPage)
                }
            }

            delegate: Item {
                required property var modelData
                width: grid.cellWidth
                height: grid.cellHeight

                Column {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: 6
                    anchors.rightMargin: 6
                    spacing: 8

                    Rectangle {
                        width: parent.width
                        height: width
                        radius: 10
                        color: theme.cardColor
                        clip: true

                        Image {
                            anchors.fill: parent
                            source: modelData.cover || ""
                            sourceSize: Qt.size(200, 200)
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            visible: status === Image.Ready
                        }
                        Icon {
                            anchors.centerIn: parent
                            name: "play-list"
                            iconSize: 26
                            iconColor: theme.subTextColor
                            visible: !(modelData.cover || "")
                        }

                        Rectangle {
                            anchors.fill: parent
                            color: "#55000000"
                            visible: plMouse.containsMouse
                            Icon {
                                anchors.centerIn: parent
                                name: "play"
                                iconSize: 26
                                iconColor: "#ffffff"
                            }
                        }

                        // 播放量
                        Rectangle {
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 6
                            width: cntText.width + 14
                            height: 18
                            radius: 9
                            color: "#99000000"
                            visible: (modelData.playCount || 0) > 0
                            Text {
                                id: cntText
                                anchors.centerIn: parent
                                text: library.formatPlayCount(modelData.playCount || 0)
                                color: "white"
                                font.pixelSize: 10
                            }
                        }
                    }

                    Text {
                        width: parent.width
                        text: modelData.name || ""
                        color: theme.textColor
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                    }
                }

                MouseArea {
                    id: plMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: page.playlistClicked(modelData)
                }
            }

            // 空态 / 加载态
            Text {
                anchors.centerIn: parent
                text: page.loading ? "加载中…" : "暂无歌单"
                color: theme.subTextColor
                font.pixelSize: 13
                visible: grid.count === 0
            }

            // 顶部加载胶囊（四-59：切分类不再清空旧列表 → 加载要有独立提示，不然像"卡住了"）
            Rectangle {
                anchors.top: parent.top
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.topMargin: 8
                width: pageLoadingLabel.width + 22
                height: 24
                radius: 12
                color: theme.cardColor
                border.color: theme.borderColor
                border.width: 1
                visible: page.loading && grid.count > 0
                Text {
                    id: pageLoadingLabel
                    anchors.centerIn: parent
                    text: "加载中…"
                    color: theme.subTextColor
                    font.pixelSize: 11
                }
            }
        }
    }

    // 首次进入该页才加载（StackLayout 子项启动即实例化，避免开机就发请求）
    onVisibleChanged: {
        if (visible && !loaded) {
            loaded = true
            home.loadExploreCategories(plat)
            reload("全部")
            // 四-59：**并行预热五个平台的首屏**（只填后端缓存、不发信号）——
            // 之后切平台直接命中缓存秒出，不用再等一轮网络（用户反馈"五平台歌单获取过慢"）
            home.warmExplorePlatforms()
        }
    }

    // 平台切换（跟随首页音源）：重载分类 + 重新拉取
    onPlatChanged: {
        if (!loaded) return
        home.loadExploreCategories(plat)
        reload("全部")
    }
}
