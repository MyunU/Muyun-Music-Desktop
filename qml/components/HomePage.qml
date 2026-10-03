import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 首页：平台标签 + 推荐歌单 + 热门排行榜
Item {
    id: page
    signal playlistClicked(var pl)
    signal toplistClicked(var pl)
    signal playToplist(var pl)
    signal morePlaylistsClicked()

    property var platforms: [
        { code: "wy", name: "小芸音乐" },
        { code: "tx", name: "小秋音乐" },
        { code: "kg", name: "小枸音乐" },
        { code: "kw", name: "小蜗音乐" },
        { code: "mg", name: "小蜜音乐" }
    ]

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: contentCol.height + 48
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: contentCol
            width: flick.width - 48
            x: 24
            y: 20
            spacing: 18

            // ---------- 平台标签 + 刷新 ----------
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Repeater {
                    model: page.platforms
                    delegate: Rectangle {
                        height: 30
                        width: labelText.width + 26
                        radius: 15
                        color: home.platform === modelData.code ? theme.accentColor
                                                                : theme.hoverColor

                        Text {
                            id: labelText
                            anchors.centerIn: parent
                            text: modelData.name
                            color: home.platform === modelData.code ? "white" : theme.textColor
                            font.pixelSize: 12
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: home.loadHome(modelData.code)
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                Rectangle {
                    width: 78; height: 30; radius: 15
                    color: refreshMouse.containsMouse ? theme.hoverColor : "transparent"
                    border.color: theme.borderColor
                    border.width: 1
                    RowLayout {
                        anchors.centerIn: parent
                        spacing: 5
                        Icon {
                            name: "refresh"
                            iconSize: 14
                            iconColor: theme.subTextColor
                            SequentialAnimation on rotation {
                                loops: Animation.Infinite
                                running: home.loading
                                NumberAnimation { from: 0; to: 360; duration: 900; easing.type: Easing.InOutSine }
                            }
                        }
                        Text { text: "刷新"; color: theme.subTextColor; font.pixelSize: 12 }
                    }
                    MouseArea {
                        id: refreshMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: home.refreshHome()
                    }
                }
            }

            // ---------- 推荐歌单（无数据的平台整块隐藏） ----------
            RowLayout {
                Layout.fillWidth: true
                visible: home.recommendPlaylists.length > 0
                Text {
                    text: "推荐歌单"
                    color: theme.textColor
                    font.pixelSize: 17
                    font.bold: true
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: "共 " + home.recommendPlaylists.length + " 个"
                    color: theme.subTextColor
                    font.pixelSize: 11
                }
            }

            GridView {
                id: plGrid
                visible: home.recommendPlaylists.length > 0
                Layout.fillWidth: true
                Layout.preferredHeight: 200
                cellWidth: Math.floor(width / 6)
                cellHeight: 200
                clip: true
                interactive: false
                model: home.recommendPlaylists

                delegate: Item {
                    width: plGrid.cellWidth
                    height: plGrid.cellHeight

                    Column {
                        anchors.left: parent.left
                        anchors.leftMargin: 6
                        spacing: 8

                        Rectangle {
                            width: plGrid.cellWidth - 24
                            height: width
                            radius: 10
                            color: theme.cardColor
                            clip: true

                            Image {
                                anchors.fill: parent
                                source: modelData.cover || ""
                                sourceSize: Qt.size(360, 360)
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                visible: status === Image.Ready
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
                                Text {
                                    id: cntText
                                    anchors.centerIn: parent
                                    text: modelData.playCount
                                          ? library.formatPlayCount(modelData.playCount) : ""
                                    color: "white"
                                    font.pixelSize: 10
                                }
                            }
                        }

                        Text {
                            width: plGrid.cellWidth - 24
                            text: modelData.name
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
                        onClicked: page.playlistClicked(modelData)
                    }
                }
            }

            // ---------- 热门排行榜 ----------
            RowLayout {
                Layout.fillWidth: true
                Text {
                    text: "热门排行榜"
                    color: theme.textColor
                    font.pixelSize: 17
                    font.bold: true
                }
                Item { Layout.fillWidth: true }
                // 歌单广场入口：胶囊按钮（纯文字太隐蔽，用户发现不了；样式对齐顶栏平台选择框）
                Rectangle {
                    Layout.alignment: Qt.AlignVCenter
                    height: 28; radius: 14
                    width: plPillText.implicitWidth + 26
                    color: morePlMouse.containsMouse ? theme.hoverColor : theme.bgColor
                    border.color: morePlMouse.containsMouse ? theme.accentColor : theme.borderColor
                    border.width: 1
                    Behavior on border.color { ColorAnimation { duration: 120 } }
                    Text {
                        id: plPillText
                        anchors.centerIn: parent
                        text: "歌单广场（瞧一瞧看一看啦~死鬼）"
                        color: morePlMouse.containsMouse ? theme.accentColor : theme.textColor
                        font.pixelSize: 12
                    }
                    MouseArea {
                        id: morePlMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: page.morePlaylistsClicked()
                    }
                }
                Text {
                    text: home.loading ? "加载中…" : (home.lastUpdated.length > 0
                                                      ? "上次更新：" + home.lastUpdated
                                                      : "")
                    color: theme.subTextColor
                    font.pixelSize: 11
                }
            }

            GridView {
                id: topGrid
                Layout.fillWidth: true
                Layout.preferredHeight: Math.ceil(home.toplists.length / 2) * 170
                cellWidth: Math.floor(width / 2)
                cellHeight: 168
                clip: true
                interactive: false
                model: home.toplists

                delegate: Item {
                    id: cardCell
                    width: topGrid.cellWidth
                    height: topGrid.cellHeight

                    // 固定像素几何，避免长文字 elide 塌陷成 0（参考 MEMORY 已记录踩坑）。
                    // 布局：左侧 84×84 封面，标题在封面正上方，右侧 3 行预览（榜单名 + 3 首歌）。
                    // 相比上一版：删掉右侧独占 34×34 的播放按钮（省 58px），
                    // 把 4 行预览压成 3 行，把每行的"艺名列 72"独立列删除，
                    // 歌名/艺人合并到同一列，让歌名列从 ~100px 扩到 ~210px+。
                    property int padX: 16
                    property int padY: 14
                    property int coverW: 84
                    property real colX: padX + coverW + 14
                    property real colW: Math.max(180, width - 12 - colX - 12)

                    Rectangle {
                        id: card
                        anchors.fill: parent
                        anchors.margins: 6
                        radius: 12
                        color: topMouse.containsMouse ? theme.hoverCardColor : theme.cardColor
                        border.color: theme.borderColor
                        border.width: 1
                        clip: true

                        // ---- 封面（84×84，居中于卡片垂直方向）----
                        Rectangle {
                            id: coverBox
                            x: cardCell.padX
                            y: (parent.height - height) / 2
                            width: cardCell.coverW
                            height: cardCell.coverW
                            radius: 10
                            clip: true
                            gradient: Gradient {
                                GradientStop { position: 0.0; color: theme.platformColor(modelData.platform) }
                                GradientStop { position: 1.0; color: Qt.darker(theme.platformColor(modelData.platform), 1.6) }
                            }

                            Image {
                                id: topCover
                                anchors.fill: parent
                                source: modelData.cover || ""
                                sourceSize: Qt.size(320, 320)
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                visible: status === Image.Ready
                            }

                            Text {
                                anchors.centerIn: parent
                                width: coverBox.width - 18
                                height: coverBox.height - 18
                                text: modelData.name
                                color: "white"
                                font.pixelSize: 13
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                wrapMode: Text.Wrap
                                elide: Text.ElideRight
                                visible: topCover.status !== Image.Ready
                            }
                        }

                        // ---- 榜单名（覆盖在封面正上方，与封面左对齐）----
                        Text {
                            id: topName
                            x: cardCell.padX
                            y: coverBox.y - 24
                            width: cardCell.coverW
                            text: modelData.name
                            color: theme.textColor
                            font.pixelSize: 13
                            font.bold: true
                            elide: Text.ElideRight
                            horizontalAlignment: Text.AlignLeft
                            verticalAlignment: Text.AlignBottom
                        }

                        // ---- 前 3 首预览（与封面在垂直方向同高）----
                        Column {
                            x: cardCell.colX
                            y: coverBox.y
                            width: cardCell.colW
                            height: coverBox.height
                            spacing: 5

                            Repeater {
                                model: {
                                    var p = modelData.preview || []
                                    return p.length > 3 ? p.slice(0, 3) : p
                                }
                                delegate: Row {
                                    id: songRow
                                    width: parent.width
                                    spacing: 8
                                    height: 28
                                    Layout.fillWidth: true

                                    Text {
                                        width: 22
                                        text: (index + 1) + "."
                                        color: theme.accentColor
                                        font.pixelSize: 12
                                        font.bold: true
                                        verticalAlignment: Text.AlignVCenter
                                        elide: Text.ElideRight
                                    }
                                    Column {
                                        width: Math.max(80, parent.width - 22 - 8)
                                        height: parent.height
                                        spacing: 1

                                        Text {
                                            width: parent.width
                                            text: modelData.name
                                            color: theme.textColor
                                            font.pixelSize: 12
                                            verticalAlignment: Text.AlignVCenter
                                            elide: Text.ElideRight
                                        }
                                        Text {
                                            width: parent.width
                                            text: modelData.artist
                                            color: theme.subTextColor
                                            font.pixelSize: 10
                                            verticalAlignment: Text.AlignVCenter
                                            elide: Text.ElideRight
                                        }
                                    }
                                }
                            }
                        }
                    }

                    MouseArea {
                        id: topMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton
                        onClicked: page.toplistClicked(modelData)
                    }
                }
            }

            Item { Layout.preferredHeight: 24 }
        }
    }
}
