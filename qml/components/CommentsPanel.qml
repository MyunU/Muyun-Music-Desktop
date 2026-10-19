import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 歌曲评论面板（右侧抽屉）：热门/最新切换 + 分页 + 评论列表。
// 由 PlayBar / LyricsPage 的评论按钮经 root.openComments() 打开。
// 取数走 player.loadComments(page, hot)（QtConcurrent 异步），结果经
// player.commentsReady 信号回来；切歌/翻页/切热门后旧结果按 songKey+hot 比对丢弃。
SideDrawer {
    id: panel
    title: "评论"
    drawerWidth: 420
    // 从全屏歌词页（z:500）内打开，须盖在它之上，故提到 600
    drawerZ: 600

    // 当前展示评论的歌曲身份 key（与 commentsReady 带回的 songKey 比对，丢弃旧结果）
    property string songKey: ""
    property bool hot: true            // true=热门 false=最新
    property int page: 1
    property int total: 0
    property int maxPage: 1
    property var comments: []
    property bool loading: false
    property bool isLocal: player.currentSong ? player.currentSong.platform === "local" : false

    onOpenedChanged: if (opened) reload()

    function reload() {
        panel.songKey = player.currentSongKey()
        panel.page = 1
        panel.comments = []
        panel.loading = true
        player.loadComments(1, panel.hot)
    }

    function switchTab(h) {
        if (panel.hot === h) return
        panel.hot = h
        panel.page = 1
        panel.comments = []
        panel.loading = true
        player.loadComments(1, h)
    }

    function gotoPage(p) {
        if (p < 1) return
        if (panel.maxPage > 0 && p > panel.maxPage) return
        if (p === panel.page) return
        panel.page = p
        panel.comments = []
        panel.loading = true
        player.loadComments(p, panel.hot)
    }

    Connections {
        target: player
        function onCommentsReady(songKey, comments, total, pg, maxPage, h) {
            // 切歌/切热/翻页后旧请求结果按 songKey+hot 比对丢弃
            if (songKey !== panel.songKey) return
            if (h !== panel.hot) return
            panel.loading = false
            panel.page = pg
            panel.total = total
            panel.maxPage = maxPage
            panel.comments = comments
            list.positionViewAtBeginning()
        }
    }

    // 切歌时面板若开着 → 自动切到新歌的评论
    Connections {
        target: player
        function onCurrentSongChanged() {
            if (panel.opened) reload()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---------- 子标题栏：热门/最新切换 + 歌名 ----------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 44
            color: "transparent"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                spacing: 8

                // 热门 / 最新 双胶囊
                Repeater {
                    model: [
                        { h: true,  label: "热门" },
                        { h: false, label: "最新" }
                    ]
                    delegate: Rectangle {
                        required property var modelData
                        Layout.preferredWidth: tabText.implicitWidth + 22
                        Layout.preferredHeight: 26
                        radius: 13
                        color: panel.hot === modelData.h
                               ? Qt.rgba(theme.accentColor.r, theme.accentColor.g,
                                         theme.accentColor.b, 0.16)
                               : "transparent"
                        border.width: panel.hot === modelData.h ? 1 : 0
                        border.color: theme.accentColor
                        Text {
                            id: tabText
                            anchors.centerIn: parent
                            text: modelData.label
                            color: panel.hot === modelData.h ? theme.accentColor : theme.subTextColor
                            font.pixelSize: 12
                            font.bold: panel.hot === modelData.h
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: panel.switchTab(modelData.h)
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                Text {
                    Layout.maximumWidth: 180
                    text: player.currentSong ? player.currentSong.name : ""
                    color: theme.subTextColor
                    font.pixelSize: 11
                    elide: Text.ElideRight
                    visible: text.length > 0
                }
            }

            // 覆盖层：吃掉标题栏区的滚轮（空白/歌名处），点击穿透到下层 tab 胶囊
            MouseArea {
                anchors.fill: parent
                propagateComposedEvents: true
                onWheel: wheel.accepted = true
                onPressed: mouse.accepted = false
                onReleased: mouse.accepted = false
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: theme.borderColor }

        // ---------- 评论列表（包裹层：列表 + 空态覆盖） ----------
        // 用 MouseArea 作包裹层：列表能滚时 Flickable 自己吃 wheel；列表空/到边界
        // 不吃时，wheel 向上传到本 MouseArea 被吃掉，不穿透到下层歌单。
        // （ListView/Flickable 没有 onWheel 信号，只能在父级 MouseArea 拦截。）
        MouseArea {
            Layout.fillWidth: true
            Layout.fillHeight: true
            onWheel: wheel.accepted = true

            ListView {
                id: list
                anchors.fill: parent
                clip: true
                model: panel.comments
                spacing: 0

                delegate: Item {
                    id: row
                    required property var modelData
                    width: list.width
                    height: contentCol.implicitHeight + 16

                    Rectangle {
                        anchors.fill: parent
                        color: rowMouse.containsMouse ? theme.hoverColor : "transparent"
                    }

                    RowLayout {
                        id: contentCol
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: 14
                        anchors.rightMargin: 14
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 10

                        // 头像
                        Rectangle {
                            Layout.preferredWidth: 34
                            Layout.preferredHeight: 34
                            radius: 17
                            color: theme.cardColor
                            clip: true
                            Image {
                                anchors.fill: parent
                                source: row.modelData.userAvatar || ""
                                sourceSize: Qt.size(68, 68)
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                visible: status === Image.Ready
                            }
                            Icon {
                                anchors.centerIn: parent
                                visible: !row.modelData.userAvatar
                                name: "music"
                                iconSize: 16
                                iconColor: theme.subTextColor
                            }
                        }

                        // 文本列
                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignTop
                            spacing: 3

                            // 用户名 + 时间 + 点赞
                            RowLayout {
                                spacing: 6
                                Text {
                                    text: row.modelData.userName || "未知用户"
                                    color: theme.accentColor
                                    font.pixelSize: 12
                                    font.bold: true
                                    elide: Text.ElideRight
                                    Layout.maximumWidth: 160
                                }
                                Item { Layout.fillWidth: true }
                                Text {
                                    text: row.modelData.timeStr || ""
                                    color: theme.subTextColor
                                    font.pixelSize: 10
                                }
                                Text {
                                    visible: row.modelData.likedCount > 0
                                    text: "♡ " + row.modelData.likedCount
                                    color: theme.subTextColor
                                    font.pixelSize: 10
                                }
                            }

                            // 被引用的回复（楼中楼）
                            Rectangle {
                                visible: (row.modelData.replyText || "").length > 0
                                Layout.fillWidth: true
                                Layout.preferredHeight: replyRow.implicitHeight + 8
                                radius: 4
                                color: Qt.rgba(theme.subTextColor.r, theme.subTextColor.g,
                                               theme.subTextColor.b, 0.08)
                                Text {
                                    id: replyRow
                                    anchors.centerIn: parent
                                    width: parent.width - 12
                                    text: "回复 " + (row.modelData.replyUser || "") + "：" +
                                          (row.modelData.replyText || "")
                                    color: theme.subTextColor
                                    font.pixelSize: 11
                                    wrapMode: Text.Wrap
                                    elide: Text.ElideRight
                                    maximumLineCount: 2
                                }
                            }

                            // 正文
                            Text {
                                Layout.fillWidth: true
                                text: row.modelData.text || ""
                                color: theme.textColor
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }
                    }

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        propagateComposedEvents: true
                    }
                }
            }

            // 空态 / 加载态覆盖层（列表无内容时显示）
            Item {
                anchors.fill: parent
                visible: panel.loading || list.count === 0
                ColumnLayout {
                    anchors.centerIn: parent
                    spacing: 10
                    Item {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredWidth: 22
                        Layout.preferredHeight: 22
                        visible: panel.loading
                        RotationAnimation on rotation {
                            from: 0; to: 360; duration: 900; loops: Animation.Infinite
                            running: panel.loading
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
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: panel.loading ? "加载中…"
                              : (panel.isLocal ? "本地歌曲无评论" : "暂无评论")
                        color: theme.subTextColor
                        font.pixelSize: 12
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: theme.borderColor }

        // ---------- 底部分页 ----------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 46
            color: "transparent"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                spacing: 8

                Text {
                    text: panel.total > 0
                          ? ("共 " + panel.total + " 条 · 第 " + panel.page + "/" + Math.max(1, panel.maxPage) + " 页")
                          : ""
                    color: theme.subTextColor
                    font.pixelSize: 11
                    visible: !panel.loading && panel.total > 0
                }
                Item { Layout.fillWidth: true }

                // 上一页
                Rectangle {
                    Layout.preferredWidth: prevTxt.implicitWidth + 22
                    Layout.preferredHeight: 26
                    radius: 13
                    color: prevHover.containsMouse ? theme.hoverColor : "transparent"
                    border.width: 1
                    border.color: theme.borderColor
                    enabled: !panel.loading && panel.page > 1
                    opacity: enabled ? 1.0 : 0.4
                    Text {
                        id: prevTxt
                        anchors.centerIn: parent
                        text: "上一页"
                        color: theme.subTextColor
                        font.pixelSize: 12
                    }
                    MouseArea {
                        id: prevHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: panel.gotoPage(panel.page - 1)
                    }
                }

                // 下一页
                Rectangle {
                    Layout.preferredWidth: nextTxt.implicitWidth + 22
                    Layout.preferredHeight: 26
                    radius: 13
                    color: nextHover.containsMouse ? theme.hoverColor : "transparent"
                    border.width: 1
                    border.color: theme.borderColor
                    enabled: !panel.loading && panel.maxPage > 0 && panel.page < panel.maxPage
                    opacity: enabled ? 1.0 : 0.4
                    Text {
                        id: nextTxt
                        anchors.centerIn: parent
                        text: "下一页"
                        color: theme.subTextColor
                        font.pixelSize: 12
                    }
                    MouseArea {
                        id: nextHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: panel.gotoPage(panel.page + 1)
                    }
                }
            }

            // 覆盖层：吃掉分页栏区的滚轮（空白/页码处），点击穿透到下层翻页按钮
            MouseArea {
                anchors.fill: parent
                propagateComposedEvents: true
                onWheel: wheel.accepted = true
                onPressed: mouse.accepted = false
                onReleased: mouse.accepted = false
            }
        }
    }
}
