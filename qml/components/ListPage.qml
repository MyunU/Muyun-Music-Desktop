import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 通用歌曲列表页
Item {
    id: page
    property string title: ""
    property var songs: []
    property string contextId: ""
    property bool removable: false   // 是否"歌单详情"页（行内歌曲可移除）
    property bool showBack: false    // 是否显示"返回上级"按钮（榜单/在线歌单详情）
    property bool showScan: false    // 本地音乐页：显示"扫描"按钮
    property bool showCollect: false // 在线歌单详情（广场/推荐）：显示"收藏歌单"按钮
    property bool collected: false   // 该在线歌单是否已收藏（再点=取消收藏）
    signal playAll()
    signal collectRequested()
    signal backClicked()
    signal scanRequested()

    // ===== 批量选择（收藏页 / 自建歌单详情页）=====
    property bool showBatch: false            // 这一页允许批量操作吗
    property bool batchMode: false
    property var selected: []                 // 存 identityKey（下标会随删除漂移，不能用下标）
    property string removeLabel: "移出歌单"    // 收藏页会传"取消收藏"
    signal batchDownloadRequested(var songs, string qualityId)
    signal batchRemoveRequested(var songs)

    // 批量下载音质（four-56 待办#1）：空串=跟随设置里的默认音质；只作用于本批，不改全局设置。
    property string batchQualityId: ""
    readonly property string batchQualityName: {
        if (page.batchQualityId === "") return "默认"
        var opts = downloads.qualityOptions()
        for (var i = 0; i < opts.length; i++)
            if (opts[i].id === page.batchQualityId) return opts[i].name
        return "默认"
    }

    function keyOf(s) { return s ? library.identityOf(s) : "" }
    function togglePick(s) {
        var k = page.keyOf(s)
        if (k.length === 0) return
        var arr = page.selected.slice()
        var i = arr.indexOf(k)
        if (i >= 0) arr.splice(i, 1); else arr.push(k)
        page.selected = arr
    }
    function selectAllVisible() {
        var arr = page.selected.slice()
        for (var i = 0; i < page.shownSongs.length; i++) {
            var k = page.keyOf(page.shownSongs[i])
            if (k.length > 0 && arr.indexOf(k) < 0) arr.push(k)
        }
        page.selected = arr
    }
    function allVisiblePicked() {
        if (page.shownSongs.length === 0) return false
        for (var i = 0; i < page.shownSongs.length; i++)
            if (page.selected.indexOf(page.keyOf(page.shownSongs[i])) < 0) return false
        return true
    }
    function selectedSongs() {
        return page.songs.filter(function (s) {
            return s && page.selected.indexOf(page.keyOf(s)) >= 0
        })
    }
    function clearSelection() { page.selected = [] }
    function exitBatch() { page.batchMode = false; page.selected = []; page.batchQualityId = "" }

    // 页内搜索：按 歌名/歌手/专辑 过滤（大小写不敏感）；搜索结果页不需要（showFilter=false）
    property bool showFilter: true
    property string filterText: ""
    readonly property var shownSongs: {
        var t = filterText.trim().toLowerCase()
        if (t.length === 0) return page.songs
        return page.songs.filter(function (s) {
            if (!s) return false
            return String(s.name || "").toLowerCase().indexOf(t) >= 0
                || String(s.artist || "").toLowerCase().indexOf(t) >= 0
                || String(s.album || "").toLowerCase().indexOf(t) >= 0
        })
    }
    // 换页清过滤 + 退出批量（换了列表还留着上一张歌单的勾选就成事故了）
    onContextIdChanged: {
        page.filterText = ""
        if (typeof pageFilterInput !== "undefined") pageFilterInput.text = ""
        page.exitBatch()
    }
    onFilterTextChanged: list.contentY = 0

    // 供 Main.qml 在重赋 songs 数组前调用（实现在 ListView 内，这里做根级转发——
    // 没有这层转发的话 listPage.preserveScroll() 抛 TypeError 中断整个刷新 handler）
    function preserveScroll() { list.preserveScroll() }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            // 返回上级（榜单、在线歌单详情进入后显示）
            Rectangle {
                id: backBtn
                visible: page.showBack
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
                text: page.title.length > 0 ? page.title : "列表"
                color: theme.textColor
                font.pixelSize: 20
                font.bold: true
            }
            Text {
                text: page.songs.length > 0
                      ? (page.filterText.length > 0
                         ? ("匹配 " + page.shownSongs.length + " / " + page.songs.length + " 首")
                         : ("共 " + page.songs.length + " 首"))
                      : ""
                color: theme.subTextColor
                font.pixelSize: 12
            }

            // 页内搜索框
            Rectangle {
                visible: page.showFilter && page.songs.length > 0
                Layout.preferredWidth: 170
                Layout.preferredHeight: 30
                Layout.alignment: Qt.AlignVCenter
                radius: 15
                color: theme.bgColor
                border.color: page.filterText.length > 0 ? theme.accentColor : theme.borderColor
                border.width: 1
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 8
                    spacing: 6
                    Icon { name: "search"; iconSize: 12; iconColor: theme.subTextColor }
                    TextInput {
                        id: pageFilterInput
                        Layout.fillWidth: true
                        verticalAlignment: TextInput.AlignVCenter
                        color: theme.textColor
                        font.pixelSize: 12
                        selectByMouse: true
                        clip: true
                        onTextChanged: page.filterText = text
                        // TextInput 无 placeholderText（Controls 属性），用占位 Text（坑：勿赋 placeholderText）
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "搜索歌曲"
                            color: theme.subTextColor
                            font.pixelSize: 12
                            visible: pageFilterInput.text.length === 0 && !pageFilterInput.activeFocus
                        }
                    }
                    Item {
                        Layout.preferredWidth: 16
                        Layout.preferredHeight: 16
                        visible: page.filterText.length > 0
                        Text {
                            anchors.centerIn: parent
                            text: "✕"
                            color: filterClearHover.containsMouse ? theme.textColor : theme.subTextColor
                            font.pixelSize: 11
                        }
                        MouseArea {
                            id: filterClearHover
                            anchors.fill: parent
                            anchors.margins: -4
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: pageFilterInput.text = ""
                        }
                    }
                }
            }
            Item { Layout.fillWidth: true }

            // ---------- 批量工具条（进入批量后顶掉常规按钮） ----------
            RowLayout {
                visible: page.batchMode
                spacing: 8
                Text {
                    Layout.alignment: Qt.AlignVCenter
                    text: page.selected.length > 0 ? ("已选 " + page.selected.length + " 首")
                                                   : "点行勾选"
                    color: page.selected.length > 0 ? theme.textColor : theme.subTextColor
                    font.pixelSize: 12
                }
                Pill {
                    label: page.allVisiblePicked() ? "取消全选" : "全选"
                    onClicked: {
                        if (page.allVisiblePicked()) page.clearSelection()
                        else page.selectAllVisible()
                    }
                }
                Pill {
                    id: batchDownloadPill
                    objectName: "batchDownloadPill"
                    label: "下载"
                    iconName: "download"
                    accent: true
                    enabled: page.selected.length > 0
                    onClicked: page.batchDownloadRequested(page.selectedSongs(), page.batchQualityId)
                }
                // 音质小下拉（four-56 待办#1）：批量逐首弹窗不现实，这里"一颗选择器管一批"
                Pill {
                    id: batchQualityPill
                    objectName: "batchQualityPill"
                    label: "音质 · " + page.batchQualityName
                    onClicked: batchQualityPopup.open()
                }
                Pill {
                    label: page.removeLabel
                    iconName: "delete"
                    danger: true
                    enabled: page.selected.length > 0
                    onClicked: page.batchRemoveRequested(page.selectedSongs())
                }
                Pill {
                    label: "完成"
                    onClicked: page.exitBatch()
                }
            }

            // 批量入口（只在收藏页/自建歌单页出现）
            Pill {
                visible: page.showBatch && !page.batchMode
                label: "批量"
                onClicked: page.batchMode = true
            }

            // 扫描目录（本地音乐页）
            Rectangle {
                visible: page.showScan && !page.batchMode
                width: scanRow.implicitWidth + 28
                height: 32; radius: 16
                color: scanMouse.containsMouse ? theme.hoverColor : theme.cardColor
                border.color: theme.borderColor
                border.width: 1
                RowLayout {
                    id: scanRow
                    anchors.centerIn: parent
                    spacing: 6
                    Icon { name: "refresh"; iconSize: 13; iconColor: theme.textColor }
                    Text {
                        text: settings.scanning ? "扫描中…" : "扫描"
                        color: theme.textColor
                        font.pixelSize: 12
                    }
                }
                MouseArea {
                    id: scanMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: page.scanRequested()
                }
            }

            // 收藏整单（广场/推荐点进来的在线歌单）：再点一次=取消收藏
            Rectangle {
                visible: page.showCollect && !page.batchMode
                width: 96; height: 32; radius: 16
                color: collectMouse.containsMouse ? theme.hoverColor : theme.cardColor
                border.color: page.collected ? theme.accentColor : theme.borderColor
                border.width: 1
                Behavior on border.color { ColorAnimation { duration: 120 } }
                RowLayout {
                    anchors.centerIn: parent
                    spacing: 6
                    Icon {
                        name: page.collected ? "heart-filled" : "heart"
                        iconSize: 13
                        iconColor: page.collected ? "#ff4d4f" : theme.textColor
                    }
                    Text {
                        text: page.collected ? "已收藏" : "收藏歌单"
                        color: page.collected ? theme.subTextColor : theme.textColor
                        font.pixelSize: 12
                    }
                }
                MouseArea {
                    id: collectMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: page.collectRequested()
                }
            }

            Rectangle {
                visible: page.songs.length > 0 && !page.batchMode
                width: 92; height: 32; radius: 16
                color: playMouse.containsMouse ? Qt.lighter(theme.accentColor, 1.1) : theme.accentColor
                RowLayout {
                    anchors.centerIn: parent
                    spacing: 6
                    Icon { name: "play"; iconSize: 13; iconColor: "#ffffff" }
                    Text { text: "播放全部"; color: "white"; font.pixelSize: 12 }
                }
                MouseArea {
                    id: playMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: page.playAll()
                }
            }
        }

        ListView {
            id: list
            objectName: "listPageView"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            model: page.shownSongs

            // 取消喜欢/移除歌曲会整体重赋 songs 数组 → ListView 在 model setter 内部就把
            // contentY 重置（onModelChanged 里再抓已太晚——上一版 keepY 自动跟踪因此失效）。
            // 正确做法：Main.qml 在重赋 songList **之前**调 preserveScroll() 显式捕获，
            // onModelChanged 里 callLater + 150ms 双段恢复（等委托异步建完）。
            property int keepIndex: -1
            property real keepOffset: 0
            property int pendIdx: -1
            property real pendOff: 0
            function preserveScroll() {
                var idx = list.indexAt(list.width / 2, list.contentY + 1)
                if (idx < 0) { keepIndex = -1; return }
                var it = list.itemAt(list.width / 2, list.contentY + 1)
                keepIndex = idx
                keepOffset = it ? (list.contentY - it.y) : 0
            }
            function doRestore() {
                if (pendIdx < 0) return
                var idx = Math.max(0, Math.min(pendIdx, Math.max(0, list.count - 1)))
                list.positionViewAtIndex(idx, ListView.Beginning)
                list.contentY += pendOff
            }
            onModelChanged: {
                if (keepIndex < 0) return
                pendIdx = keepIndex
                pendOff = keepOffset
                keepIndex = -1
                Qt.callLater(doRestore)
                restoreTimer.restart()
            }
            Timer {
                id: restoreTimer
                interval: 150
                onTriggered: { list.doRestore(); list.pendIdx = -1 }
            }

            delegate: SongDelegate {
                song: modelData
                songIndex: index
                contextList: page.songs
                contextId: page.contextId.length > 0 ? page.contextId : page.title
                contextName: page.title
                removable: page.removable
                batchMode: page.batchMode
                picked: page.selected.indexOf(page.keyOf(modelData)) >= 0
                onTogglePicked: page.togglePick(modelData)
            }

            Text {
                anchors.centerIn: parent
                text: page.filterText.length > 0 ? "无匹配歌曲" : "暂无内容"
                color: theme.subTextColor
                font.pixelSize: 13
                visible: list.count === 0
            }
        }
    }

    // 批量下载音质选择弹窗（four-56）：样式对齐 SongDelegate 的下载音质弹窗，但批量不逐首探测大小
    // （一首一个样没意义），只给"默认（跟随设置）+ 各档位"的列表。
    Popup {
        id: batchQualityPopup
        objectName: "batchQualityPopup"
        anchors.centerIn: Overlay.overlay
        modal: true
        dim: true
        width: 320
        padding: 16

        background: Rectangle {
            color: theme.cardColor
            radius: 12
            border.color: theme.borderColor
        }

        contentItem: ColumnLayout {
            spacing: 12
            Text {
                Layout.fillWidth: true
                text: "本批下载音质"
                color: theme.textColor
                font.pixelSize: 14
                font.bold: true
            }
            Text {
                Layout.fillWidth: true
                text: "只作用于这次批量加入的歌曲，不改设置里的默认音质"
                color: theme.subTextColor
                font.pixelSize: 10
                wrapMode: Text.Wrap
            }
            Repeater {
                model: [{ id: "", name: "默认（跟随设置）" }].concat(downloads.qualityOptions())
                delegate: Rectangle {
                    id: qOpt
                    objectName: "batchQOpt"
                    property string qid: modelData.id
                    required property var modelData
                    required property int index
                    property bool isCurrent: page.batchQualityId === qOpt.modelData.id
                    Layout.fillWidth: true
                    Layout.preferredHeight: 34
                    radius: 8
                    color: qOpt.isCurrent ? theme.accentColor
                                           : (qOptHover.containsMouse ? theme.hoverColor : theme.bgColor)
                    border.color: qOpt.isCurrent ? theme.accentColor : theme.borderColor
                    border.width: 1
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 8
                        Text {
                            Layout.fillWidth: true
                            text: qOpt.modelData.name
                            color: qOpt.isCurrent ? "white" : theme.textColor
                            font.pixelSize: 12
                            font.bold: qOpt.isCurrent
                            elide: Text.ElideRight
                        }
                        Text {
                            visible: String(qOpt.modelData.desc || "").length > 0
                            text: qOpt.modelData.desc || ""
                            color: qOpt.isCurrent ? "#eeffffff" : theme.subTextColor
                            font.pixelSize: 10
                        }
                    }
                    MouseArea {
                        id: qOptHover
                        objectName: "batchQOptClick"
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            page.batchQualityId = qOpt.modelData.id   // 绑定模型属性，pill 文案自动跟
                            batchQualityPopup.close()
                        }
                    }
                }
            }
        }
    }

    // 批量工具条的小圆角按钮（页内复用，样式对齐"播放全部/收藏歌单"那颗胶囊）
    component Pill: Rectangle {
        id: pill
        property string label: ""
        property string iconName: ""
        property bool accent: false
        property bool danger: false
        signal clicked()
        Layout.alignment: Qt.AlignVCenter
        width: pillRow.implicitWidth + 26
        height: 32
        radius: 16
        opacity: enabled ? 1.0 : 0.45
        // danger（移出/取消收藏）用描边红字：和实心的「下载」拉开，别两颗红蛋挤在一起
        color: pill.danger ? (pillHover.containsMouse ? theme.hoverColor : theme.cardColor)
             : pill.accent ? (pillHover.containsMouse ? Qt.lighter(theme.accentColor, 1.1) : theme.accentColor)
             : (pillHover.containsMouse ? theme.hoverColor : theme.cardColor)
        border.color: pill.danger ? "#c0392b"
                      : (pill.accent ? "transparent" : theme.borderColor)
        border.width: pill.danger ? 1 : 1
        Behavior on color { ColorAnimation { duration: 120 } }

        RowLayout {
            id: pillRow
            anchors.centerIn: parent
            spacing: 6
            Icon {
                name: pill.iconName
                iconSize: 13
                visible: pill.iconName.length > 0
                iconColor: pill.danger ? "#e05a4f"
                          : pill.accent ? "#ffffff" : theme.textColor
            }
            Text {
                text: pill.label
                color: pill.danger ? "#e05a4f" : pill.accent ? "#ffffff" : theme.textColor
                font.pixelSize: 12
            }
        }
        MouseArea {
            id: pillHover
            anchors.fill: parent
            hoverEnabled: true
            enabled: pill.enabled
            cursorShape: Qt.PointingHandCursor
            onClicked: pill.clicked()
        }
    }
}
