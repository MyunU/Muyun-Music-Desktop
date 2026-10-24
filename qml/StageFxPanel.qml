import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import QtQuick.Dialogs
import "components"

// 舞台 fx 视觉控制台面板（StageFxController 的 QQuickView 承载内容）
// 经 StageBridge::fx* 通道透传 mineradio 引擎（setFxValue/toggleFx/setPreset/resetFx），
// 引擎回显 state 驱动控件值；面板浮在舞台窗之上，边调参边看效果。
// 样式对齐 EffectsPanel：Flow 预设 chips / 自绘 Slider / 胶囊开关。
Item {
    id: root
    // QQuickView 承载：root 必须有显式尺寸（同 DesktopLyricsView 模式），
    // 否则 SizeRootObjectToView 下 root 保持 0x0，布局全部溢出
    // #5 BUG5: 360 → 440, 之前太窄 —— 用户报"导入图片按钮和其他 UI 重叠, 滚放排列右边被吞"
    width: 440
    height: 860

    // ---- 状态（引擎回显，{k: value}）----
    property var fxState: ({})
    // #5 滚放排序拖拽状态 (第 5 版 2026-10-23, 全局坐标方案):
    //   - rowDragMa.onPressed 记 draggingIdx/dragToIdx, onPositionChanged 调
    //     _updateRollerDragTarget() (QCursor.pos 屏幕坐标逐行比 y), onReleased 调
    //     _commitRollerDrag() (splice + applyRollerConfig + 清状态).
    //   - 越出当前行后 rowDragMa 收不到事件, 由 root 层 dragFallbackMa 兜底
    //     (enabled=rollerDragging, z=100, 释放/移动都调对应函数).
    //   - 拖拽期间 rollerDragging=true → 标题栏 startSystemMove 禁用 +
    //     rollerSeqFlick.interactive=false (不抢 flick 手势); 无 DropArea → wheel 正常.
    property bool rollerDragging: false
    property int draggingIdx: -1
    property int dragToIdx: -1
    // 拖拽源行屏幕坐标 (onPressed 时记), 给 dragFallbackMa 兜底用
    property real dragSrcTop: 0
    property real dragSrcHeight: 0

    function fxNum(k, d) {
        var s = root.fxState
        if (!s || typeof s !== "object") return d
        var v = s[k]
        return (typeof v === "number" && isFinite(v)) ? v : d
    }
    function fxBool(k, d) {
        var s = root.fxState
        if (!s || typeof s !== "object") return d
        var v = s[k]
        return (v === undefined || v === null) ? d : !!v
    }

    // 预设（短中文名，index 对应引擎 presetMeta；顺序照抄 presetDisplayOrder）
    readonly property var presets: [
        { i: 0, name: "封面" }, { i: 6, name: "安魂" }, { i: 5, name: "星河" },
        { i: 4, name: "唱片" }, { i: 2, name: "星球" }, { i: 1, name: "滚筒" },
        { i: 3, name: "虚空" }
    ]

    // #5 滚放: 可用预设候选（排除虚空 3 和安魂 6，因为它们无粒子切换特效）
    readonly property var rollerCandidates: [
        { i: 0, name: "封面" }, { i: 5, name: "星河" }, { i: 4, name: "唱片" },
        { i: 2, name: "星球" }, { i: 1, name: "滚筒" }
    ]

    // 滚放当前配置（从 fxState 读；变更时调用 engine 更新）
    function rollerEnabled() {
        var r = root.fxState["roller"]
        return r ? !!r["enabled"] : false
    }
    function rollerSeq() {
        var r = root.fxState["roller"]
        return (r && Array.isArray(r["seq"]) && r["seq"].length > 0) ? r["seq"].slice() : [0, 5, 4, 2, 1]
    }
    function rollerInterval() {
        var r = root.fxState["roller"]
        var v = r ? r["intervalSec"] : 8
        return (typeof v === "number" && isFinite(v) && v >= 2 && v <= 120) ? v : 8
    }
    function applyRollerConfig(enabled, seq, intervalSec) {
        var obj = ({ enabled: enabled, seq: seq, intervalSec: intervalSec })
        if (typeof stage !== "undefined") stage.rollerSetConfig(obj)
    }
    // 在 seq 中上移/下移某个预设索引
    function moveRollerPreset(presetIdx, dir) {
        var seq = root.rollerSeq()
        var pos = seq.indexOf(presetIdx)
        if (pos < 0) return
        var np = pos + dir
        if (np < 0 || np >= seq.length) return
        var tmp = seq[pos]; seq[pos] = seq[np]; seq[np] = tmp
        root.applyRollerConfig(root.rollerEnabled(), seq, root.rollerInterval())
    }
    function removeFromRoller(presetIdx) {
        var seq = root.rollerSeq()
        var pos = seq.indexOf(presetIdx)
        if (pos < 0) return
        if (seq.length <= 1) return  // 至少 1 项
        seq.splice(pos, 1)
        root.applyRollerConfig(root.rollerEnabled(), seq, root.rollerInterval())
    }
    function addToRoller(presetIdx) {
        var seq = root.rollerSeq()
        if (seq.indexOf(presetIdx) >= 0) return
        seq.push(presetIdx)
        root.applyRollerConfig(root.rollerEnabled(), seq, root.rollerInterval())
    }
    // 预设索引 → 中文名 (从 presets 表查; 未知索引显示 "?")
    function presetName(idx) {
        for (var i = 0; i < root.presets.length; i++) {
            if (root.presets[i].i === idx) return root.presets[i].name
        }
        return "?"
    }

    // ---- #5 滚放拖拽排序 (第 5 版 2026-10-23, 全局坐标方案) ----
    // ⚠ 实现要点 (四轮血泪总结, 新会话必读):
    //   1. 不用 QML 事件里的 mouse.x/mouse.y —— 是 MouseArea 局部坐标, 被 clamp 在
    //      [0, width/height], 越出本行后上方向永远卡 0 (第 1 版根因).
    //   2. 不用 QQuickView 的 grabMouse —— 只对 QWindow 有效, QQuickView 是独立 view
    //      不是 QWindow (第 2 版根因: 事件仍被 Flickable/窗口 frame 抢走).
    //   3. 不用 QML 原生 Drag/Drop API —— 在 QQuickView 里 Drag 会话与 DropArea 事件
    //      通道不兼容, 且 DropArea{anchors.fill:parent} 吞 wheel 导致"滚轮下滚无效"
    //      (Flickable/窗口 frame 抢事件的典型症状).
    //   4. 本版方案: 纯 QML 全局坐标 (第 6 版 2026-10-24): 调用方 MouseArea.mapToGlobal(0,0).y
    //      + mouse.y 得屏幕 Y (无 QCursor, 纯 QML), 遍历 rollerSeqCol 子项取
    //      child.mapToGlobal(0,0).y 作为每行屏幕顶部, 与全局 Y 比命中哪行.
    //      上下方向都不 clamp. 越出本行后 rowDragMa 收不到 positionChanged,
    //      由根层 dragFallbackMa (enabled=rollerDragging, z=100) 兜底
    //      positionChanged/onReleased, 同样调 _updateRollerDragTarget(), 避免状态卡死.
    // 入参 globalY = 调用方 MouseArea.mapToGlobal(0,0).y + mouse.y (纯 QML 可用, 无 QCursor).
    // 第 7 版 (2026-10-24): stride 方案 —— 调用方传本行 mapToGlobal(0,0).y + height + seqPos,
    // 函数算 offset = round((globalY - myTop) / stride), targetIdx = seqPos + offset.
    // 纯 QML 可用 (无 QCursor, 无 children 遍历).
    function _updateRollerDragTarget(globalY, myTop, myHeight, mySeqPos, seqLen) {
        if (!root.rollerDragging) return
        var stride = myHeight + 4  // spacing: 4
        var offset = Math.round((globalY - myTop) / stride)
        var targetIdx = mySeqPos + offset
        if (targetIdx < 0) targetIdx = 0
        if (targetIdx > seqLen - 1) targetIdx = seqLen - 1
        if (targetIdx !== root.dragToIdx) {
            root.dragToIdx = targetIdx
        }
    }
    // 提交 splice + 清理拖拽状态
    function _commitRollerDrag() {
        var fromIdx = root.draggingIdx
        var toIdx = root.dragToIdx
        if (fromIdx >= 0 && toIdx >= 0 && fromIdx !== toIdx) {
            var seq = root.rollerSeq()
            if (fromIdx < seq.length && toIdx < seq.length) {
                var v = seq.splice(fromIdx, 1)[0]
                seq.splice(toIdx, 0, v)
                root.applyRollerConfig(root.rollerEnabled(), seq, root.rollerInterval())
            }
        }
        root.rollerDragging = false
        root.draggingIdx = -1
        root.dragToIdx = -1
        root.dragSrcTop = 0
        root.dragSrcHeight = 0
    }

    // ---- 外观：深色玻璃（舞台是暗色视觉主体，面板不随浅色主题变白）----
    readonly property color accent: theme.accentColor
    readonly property color panelText: "#e8ebf0"
    readonly property color subText: "#9aa2af"
    readonly property color chipBg: "#2a2f3b"
    readonly property color grooveBg: "#3a4150"
    readonly property font uiFont: Qt.font({ family: "Microsoft YaHei UI" })

    Rectangle {
        anchors.fill: parent
        radius: 14
        color: "#e814161c"
        border.color: "#33ffffff"
        border.width: 1
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 10

        // ===== 标题栏（拖动区 + 关闭）=====
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 30
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: "视觉控制台"
                color: root.panelText
                font.pixelSize: 15
                font.bold: true
                font.family: "Microsoft YaHei UI"
            }
            MouseArea {
                hoverEnabled: true
                anchors.fill: parent
                cursorShape: Qt.DragMoveCursor
                // #5 BUG1: 滚放排序拖拽期间, 标题栏不再响应拖动, 防止用户按下
                // 标题栏区时"整个界面跟着拖" —— 只有 rollerDragging=false 时才能拖窗口
                enabled: !root.rollerDragging
                onPressed: {
                    if (Window.window) Window.window.startSystemMove()
                }
            }
            Rectangle {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 26; height: 26; radius: 13
                color: closeMa.containsMouse ? "#33ffffff" : "transparent"
                Text {
                    anchors.centerIn: parent
                    text: "✕"; color: root.subText
                    font.pixelSize: 13
                }
                MouseArea {
                    id: closeMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: if (typeof stageFx !== "undefined") stageFx.hide()
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: "#22ffffff"
        }

        // ===== 视觉预设 =====
        Text {
            text: "视觉预设"
            color: root.subText
            font.pixelSize: 11
            font.family: "Microsoft YaHei UI"
        }
        Flow {
            Layout.fillWidth: true
            spacing: 6
            Repeater {
                model: root.presets
                delegate: Rectangle {
                    required property var modelData
                    readonly property bool selected: root.fxNum("preset", 0) === modelData.i
                    width: presetLabel.implicitWidth + 22
                    height: 26
                    radius: 13
                    color: selected ? root.accent : root.chipBg
                    border.color: selected ? "#55ffffff" : "#1fffffff"
                    border.width: 1
                    Text {
                        id: presetLabel
                        anchors.centerIn: parent
                        text: modelData.name
                        color: selected ? "#ffffff" : "#c8ccd4"
                        font.pixelSize: 12
                        font.family: "Microsoft YaHei UI"
                    }
                    MouseArea {
                        hoverEnabled: true
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: if (typeof stage !== "undefined") stage.fxPreset(modelData.i)
                    }
                }
            }
        }

        // ===== #5 滚放 =====
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: "#22ffffff"
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Text {
                text: "滚放"
                color: root.panelText
                font.pixelSize: 13
                font.bold: true
                font.family: "Microsoft YaHei UI"
            }
            Text {
                text: root.rollerEnabled() ? "按序列定时切换" : "按序列自动轮换"
                color: root.rollerEnabled() ? root.accent : root.subText
                font.pixelSize: 10
                font.family: "Microsoft YaHei UI"
            }
            Item { Layout.fillWidth: true }
            Text {
                text: root.rollerEnabled() ? "已启用" : "已关闭"
                color: root.rollerEnabled() ? root.accent : root.subText
                font.pixelSize: 10
                font.family: "Microsoft YaHei UI"
            }
        }
        // 滚放开关行（无"排序"按钮——直接拖拽⋮⋮手柄排序）
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                radius: 6
                color: rollerToggleMa.containsMouse ? "#14ffffff" : "transparent"
                border.color: root.rollerEnabled() ? "#44ffffff" : "#1fffffff"
                border.width: 1
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: "启用滚放"
                    color: root.panelText
                    font.pixelSize: 12
                    font.family: "Microsoft YaHei UI"
                }
                // 开关
                Rectangle {
                    anchors.right: parent.right
                    anchors.rightMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    width: 36; height: 18; radius: 9
                    color: root.rollerEnabled() ? root.accent : root.grooveBg
                    Rectangle {
                        x: root.rollerEnabled() ? parent.width - width - 2 : 2
                        anchors.verticalCenter: parent.verticalCenter
                        width: 14; height: 14; radius: 7
                        color: "white"
                        Behavior on x { NumberAnimation { duration: 120 } }
                    }
                }
                MouseArea {
                    id: rollerToggleMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        var next = !root.rollerEnabled()
                        var seq = root.rollerSeq()
                        if (next && seq.length === 0) seq = [0, 5, 4, 2, 1]
                        root.applyRollerConfig(next, seq, root.rollerInterval())
                    }
                }
            }
            Text {
                text: "按住任意行拖动 · 右侧 ✕ 删除"
                color: root.rollerEnabled() ? root.subText : "#666666"
                font.pixelSize: 10
                font.family: "Microsoft YaHei UI"
            }
        }
        // 序列预览 + 停留秒数（用 Loader 做条件挂载，因为 QML 组件内不允许裸 if）
        Loader {
            Layout.fillWidth: true
            Layout.preferredHeight: 130
            active: root.rollerEnabled()
            sourceComponent: Flickable {
                id: rollerSeqFlick      // #5 BUG2: 拖拽手柄时临时禁用滚动, 避免 Flickable 抢占鼠标事件
                Layout.fillWidth: true
                Layout.preferredHeight: 130
                clip: true
                interactive: root.rollerDragging !== true
                contentHeight: rollerSeqCol.implicitHeight
                contentWidth: width
                boundsBehavior: Flickable.StopAtBounds
                ColumnLayout {
                    id: rollerSeqCol
                    width: parent.width
                    spacing: 4
                    // 停留秒数行
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Text {
                            text: "停留"
                            color: root.panelText
                            font.pixelSize: 11
                            font.family: "Microsoft YaHei UI"
                            Layout.preferredWidth: 42
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 4
                            Repeater {
                                model: [4, 6, 8, 10, 15, 20]
                                delegate: Rectangle {
                                    required property int modelData
                                    readonly property bool sel: root.rollerInterval() === modelData
                                    Layout.preferredWidth: 32
                                    Layout.preferredHeight: 22
                                    radius: 11
                                    color: sel ? root.accent : root.chipBg
                                    border.color: sel ? "#55ffffff" : "#1fffffff"
                                    border.width: 1
                                    Text {
                                        anchors.centerIn: parent
                                        text: modelData + "s"
                                        color: sel ? "#ffffff" : "#c8ccd4"
                                        font.pixelSize: 10
                                        font.family: "Consolas"
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.applyRollerConfig(root.rollerEnabled(), root.rollerSeq(), modelData)
                                    }
                                }
                            }
                        }
                    }
                    // 序列项 —— 整行拖拽排序 (第 5 版 2026-10-23, 见 root 级注释) + 右侧 × 删除
                    Repeater {
                        model: root.rollerSeq()
                        delegate: Item {
                            id: seqDelegate
                            objectName: "rollerSeqItem"
                            required property int modelData
                            readonly property int seqPos: root.rollerSeq().indexOf(modelData)
                            Layout.fillWidth: true
                            Layout.preferredHeight: 26
                            // 被拖拽的行 opacity 降 (root.draggingIdx 指向自己时)
                            readonly property bool isDragSource: root.draggingIdx === seqPos
                            opacity: isDragSource ? 0.4 : 1
                            Behavior on opacity { NumberAnimation { duration: 120 } }

                            // 拖拽排序: 整行 MouseArea, 按下启动拖拽 / 移动实时算目标行 / 释放提交 splice.
                            // 坐标换算在 root 级 _updateRollerDragTarget() 里完成 (全局坐标 QCursor.pos).
                            Rectangle {
                                id: seqItem
                                anchors.fill: parent
                                radius: 5
                                color: seqDelegate.isDragSource ? "#22ffffff"
                                       : (root.dragToIdx === seqDelegate.seqPos && !seqDelegate.isDragSource ? "#1a4488ff" : "#10ffffff")
                                border.color: seqDelegate.isDragSource ? "#33ffffff"
                                       : (root.dragToIdx === seqDelegate.seqPos && !seqDelegate.isDragSource ? "#5588ff" : "#1fffffff")
                                border.width: 1
                                Behavior on color { ColorAnimation { duration: 120 } }

                                MouseArea {
                                    id: rowDragMa
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.SizeAllCursor
                                    property bool dragging: false
                                    // 按下: 记录源索引, 启动拖拽
                                    onPressed: function(mouse) {
                                        rowDragMa.dragging = true
                                        root.draggingIdx = seqDelegate.seqPos
                                        root.dragToIdx = seqDelegate.seqPos
                                        root.rollerDragging = true
                                        // 记录源行屏幕顶Y和高度, 给 dragFallbackMa 兜底用
                                        root.dragSrcTop = seqDelegate.mapToGlobal(0, 0).y
                                        root.dragSrcHeight = seqDelegate.height
                                    }
                                    // 移动: 实时算目标行 (传本行全局顶Y+高度+seqPos, 纯 QML)
                                    onPositionChanged: function(mouse) {
                                        if (!rowDragMa.dragging) return
                                        var gY = rowDragMa.mapToGlobal(0, 0).y + mouse.y
                                        root._updateRollerDragTarget(gY, seqDelegate.mapToGlobal(0, 0).y, seqDelegate.height, seqDelegate.seqPos, root.rollerSeq().length)
                                    }
                                    // 释放: 提交 splice + 清理
                                    onReleased: function(mouse) {
                                        if (!rowDragMa.dragging) return
                                        rowDragMa.dragging = false
                                        root._commitRollerDrag()
                                    }
                                }
                            }

                            // 序号
                            Text {
                                anchors.left: parent.left
                                anchors.leftMargin: 6
                                anchors.verticalCenter: parent.verticalCenter
                                text: (seqDelegate.seqPos + 1) + "."
                                color: root.subText
                                font.pixelSize: 10
                                font.family: "Consolas"
                                font.weight: Font.Bold
                            }
                            // 预设名
                            Text {
                                id: nameTxt
                                anchors.left: parent.left
                                anchors.leftMargin: 22
                                anchors.right: delBtn.left
                                anchors.rightMargin: 4
                                anchors.verticalCenter: parent.verticalCenter
                                elide: Text.ElideRight
                                text: root.presetName(modelData)
                                color: root.panelText
                                font.pixelSize: 11
                                font.family: "Microsoft YaHei UI"
                            }
                            // × 删除按钮 (显式加在右侧, 不再"找不到")
                            Rectangle {
                                id: delBtn
                                width: 22; height: 20; radius: 3
                                color: delMa.containsMouse ? "#44ff5555" : "transparent"
                                anchors.right: parent.right
                                anchors.rightMargin: 4
                                anchors.verticalCenter: parent.verticalCenter
                                Text {
                                    anchors.centerIn: parent
                                    text: "✕"; color: "#ff8888"; font.pixelSize: 11
                                }
                            }
                            MouseArea {
                                id: delMa
                                anchors.fill: delBtn
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.removeFromRoller(modelData)
                            }

                        }
                    }
                    // 候选添加
                    Item { Layout.preferredHeight: 2 }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Text {
                            text: "+ 加入:"
                            color: root.subText
                            font.pixelSize: 10
                            font.family: "Microsoft YaHei UI"
                        }
                        Repeater {
                            model: root.rollerCandidates
                            delegate: Rectangle {
                                required property var modelData
                                readonly property bool inSeq: root.rollerSeq().indexOf(modelData.i) >= 0
                                width: 64
                                Layout.preferredWidth: 64
                                Layout.preferredHeight: 22
                                radius: 11
                                color: inSeq ? "transparent" : "#0effffff"
                                border.color: inSeq ? "#11ffffff" : "#22ffffff"
                                border.width: 1
                                opacity: inSeq ? 0.4 : 1
                                Text {
                                    anchors.centerIn: parent
                                    text: "+" + modelData.name
                                    color: "#c8ccd4"
                                    font.pixelSize: 10
                                    font.family: "Microsoft YaHei UI"
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    hoverEnabled: !inSeq
                                    cursorShape: inSeq ? Qt.ArrowCursor : Qt.PointingHandCursor
                                    onClicked: root.addToRoller(modelData.i)
                                }
                            }
                        }
                    }
                    // 重置序列按钮 (独占一行, 不与导入按钮挤在同一 RowLayout)
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 22
                            radius: 11
                            color: resetRollerMa.containsMouse ? "#33ffffff" : "transparent"
                            border.color: "#22ffffff"
                            border.width: 1
                            Text {
                                anchors.centerIn: parent
                                text: "重置序列 (封面→星河→唱片→星球→滚筒)"
                                color: root.subText
                                font.pixelSize: 10
                                font.family: "Microsoft YaHei UI"
                            }
                            MouseArea {
                                id: resetRollerMa
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.applyRollerConfig(root.rollerEnabled(), [0, 5, 4, 2, 1], 8)
                            }
                        }
                    }
                    // #5 P2/多图 图片导入：把本地图片解析到粒子上（当作封面纹理解析，预设0封面/唱片中心取样）
                    //   单张 = 当前预设立即换粒子图；多张 = 按滚放序列槽位循环匹配 (特效多于图片时回到第一张)
                    // #5 BUG5: 之前把导入按钮和 hint 硬塞在同一个 28 高的 Rectangle, 又被外层的
                    // "重置序列" RowLayout 挤压, 导致按钮文字和 hint 重叠、视觉上挤到看不见.
                    // 现在: 按钮独占一行 36 高, 文字整行居中; hint 单独一行居左, 不再互相挤压.
                    // FileDialog 从内层 RowLayout 移出来挂到 root (下面), 之前嵌在 RowLayout
                    // 里导致 .open() 静默失败, 用户"看不到弹窗".
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 36
                        radius: 6
                        color: importImgMa.containsMouse ? "#1a4488ff" : "#0f4488ff"
                        border.color: "#334488ff"
                        border.width: 1
                        Text {
                            anchors.fill: parent
                            anchors.margins: 8
                            verticalAlignment: Text.AlignVCenter
                            horizontalAlignment: Text.AlignHCenter
                            text: "🖼  导入本地图片 → 粒子"
                            color: "#ccddff"
                            font.pixelSize: 12
                            font.weight: Font.Medium
                            font.family: "Microsoft YaHei UI"
                        }
                        MouseArea {
                            id: importImgMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                // #5 始终 OpenFiles: 单张走 onAccepted 单张路径, 多张走批量.
                                //   原 rollerEnabled() 动态切 OpenFile → 滚放关闭时无法多选.
                                importImagePick.open()
                            }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: (root.rollerEnabled()
                                ? "可多选 · 按序列循环匹配"
                                : "选择一张图片 · 滚放开启时可多选")
                        color: root.subText
                        font.pixelSize: 9
                        font.family: "Microsoft YaHei UI"
                        opacity: 0.75
                    }
                }
            }
        }

        // #5 修复 (2026-10-24): "清理导入图片" 专用按钮 —— 导入图默认持久化
        //   (重启不再自动清除), 清除是用户主动动作。放 Loader 之外: 滚放关闭时
        //   导入图仍铺在粒子上 (applyCoverDataUrl), 也要能清。清理后引擎强制恢复歌曲封面。
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 30
            radius: 6
            color: (clearImgMa.containsMouse && root.fxNum('importedImageCount', 0) > 0)
                   ? "#5a2733ff" : "#332026ff"
            border.color: "#662233ff"
            border.width: 1
            Text {
                anchors.fill: parent
                anchors.margins: 8
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
                text: "🧹  清理导入图片 (" + root.fxNum('importedImageCount', 0) + ") · 恢复歌曲封面"
                color: root.fxNum('importedImageCount', 0) > 0 ? "#ffc9d4" : "#665566"
                font.pixelSize: 11
                font.family: "Microsoft YaHei UI"
            }
            MouseArea {
                id: clearImgMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                enabled: root.fxNum('importedImageCount', 0) > 0
                onClicked: stage.clearImportedImages()
            }
        }

        // ===== 参数区（可滚动）=====
        Flickable {
            id: flick
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            contentHeight: paramsCol.implicitHeight
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            ColumnLayout {
                id: paramsCol
                width: flick.width
                spacing: 8

                Text {
                    text: "粒子"
                    color: root.subText
                    font.pixelSize: 11
                    font.family: "Microsoft YaHei UI"
                }
                Repeater {
                    model: [
                        { k: "intensity",       label: "律动强度",   mn: 0.2,  mx: 1.6,  df: 0.85 },
                        { k: "depth",           label: "空间深度",   mn: 0.2,  mx: 1.8,  df: 1.0  },
                        { k: "point",           label: "粒子大小",   mn: 0.5,  mx: 2.2,  df: 1.0  },
                        { k: "speed",           label: "流动速度",   mn: 0.2,  mx: 2.5,  df: 1.0  },
                        { k: "twist",           label: "扭曲",       mn: 0.0,  mx: 0.6,  df: 0.0  },
                        { k: "color",           label: "色彩张力",   mn: 0.5,  mx: 2.0,  df: 1.1  },
                        { k: "scatter",         label: "粒子发散",   mn: 0.0,  mx: 0.5,  df: 0.0  },
                        { k: "bgFade",          label: "背景淡出",   mn: 0.0,  mx: 1.2,  df: 0.2  },
                        { k: "coverResolution", label: "粒子密度",   mn: 0.75, mx: 1.55, df: 1.55 }
                    ]
                    delegate: SliderRow {
                        required property var modelData
                        kKey: modelData.k
                        label: modelData.label
                        mn: modelData.mn
                        mx: modelData.mx
                        dflt: modelData.df
                    }
                }

                Text {
                    text: "镜头与光效"
                    color: root.subText
                    font.pixelSize: 11
                    font.family: "Microsoft YaHei UI"
                }
                Repeater {
                    model: [
                        { k: "cinema",  label: "电影镜头",    df: true  },
                        { k: "bloom",   label: "溢光",        df: false },
                        { k: "edge",    label: "轮廓高亮",    df: false },
                        { k: "aiDepth", label: "AI 立体增强", df: false }
                    ]
                    delegate: SwitchRow {
                        required property var modelData
                        kKey: modelData.k
                        label: modelData.label
                        dflt: modelData.df
                    }
                }
                Repeater {
                    model: [
                        { k: "cinemaShake",   label: "电影震感", mn: 0.0, mx: 1.6, df: 0.5  },
                        { k: "bloomStrength", label: "溢光强度", mn: 0.0, mx: 1.6, df: 0.62 }
                    ]
                    delegate: SliderRow {
                        required property var modelData
                        kKey: modelData.k
                        label: modelData.label
                        mn: modelData.mn
                        mx: modelData.mx
                        dflt: modelData.df
                    }
                }

                Text {
                    text: "歌词"
                    color: root.subText
                    font.pixelSize: 11
                    font.family: "Microsoft YaHei UI"
                }
                Repeater {
                    model: [
                        { k: "particleLyrics",     label: "粒子歌词",   df: true  },
                        { k: "lyricGlow",          label: "歌词溢光",   df: true  },
                        { k: "lyricGlowBeat",      label: "溢光随鼓点", df: true  },
                        { k: "lyricGlowParticles", label: "歌词光粒",   df: false },
                        { k: "lyricCameraLock",    label: "歌词绑镜头", df: false }
                    ]
                    delegate: SwitchRow {
                        required property var modelData
                        kKey: modelData.k
                        label: modelData.label
                        dflt: modelData.df
                    }
                }
                Repeater {
                    model: [
                        { k: "lyricScale",        label: "歌词大小", mn: 0.35, mx: 1.65, df: 1.0  },
                        { k: "lyricGlowStrength", label: "溢光亮度", mn: 0.0,  mx: 0.85, df: 0.28 }
                    ]
                    delegate: SliderRow {
                        required property var modelData
                        kKey: modelData.k
                        label: modelData.label
                        mn: modelData.mn
                        mx: modelData.mx
                        dflt: modelData.df
                    }
                }
            }
        }

        // ===== 底部 =====
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: "#22ffffff"
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Rectangle {
                Layout.preferredWidth: 76
                Layout.preferredHeight: 26
                radius: 13
                color: resetMa.containsMouse ? "#33ffffff" : "transparent"
                border.color: "#44ffffff"
                border.width: 1
                Text {
                    anchors.centerIn: parent
                    text: "恢复默认"
                    color: root.panelText
                    font.pixelSize: 11
                    font.family: "Microsoft YaHei UI"
                }
                MouseArea {
                    id: resetMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: if (typeof stage !== "undefined") stage.fxReset()
                }
            }
            Item { Layout.fillWidth: true }
            Text {
                text: "调参即时生效 · 自动保存"
                color: root.subText
                font.pixelSize: 10
                font.family: "Microsoft YaHei UI"
            }
        }
    }

    // 引擎回显 → 刷新控件（回显经 250ms 防抖；拖拽中的滑块不覆盖）
    Connections {
        target: (typeof stage !== "undefined") ? stage : null
        enabled: (typeof stage !== "undefined")
        function onFxStateReceived(state) {
            root.fxState = state
        }
    }

    // #5 BUG5: FileDialog 必须挂在 root 层级, 不能嵌在 RowLayout/Flickable 等容器里.
    // 之前把 importImagePick 塞在 "重置序列" 的 RowLayout 内部 —— QML 的 QtQuick.Dialogs
    // 需要 FileDialog 有稳定锚点, 塞在 Repeater delegate 或行容器里会导致 .open() 静默
    // 失败 (不弹窗, 无任何反馈). 用户"点导入按钮没反应"就是这个.
    // #5 BUG2: 加载时主动 requestFxState() —— QML 端启动时 stage 可能还没推回显,
    // 主动拉一次能确保滚放/预设状态在开面板瞬间就是最新的, 不依赖 stage 那边的时序.
    Component.onCompleted: {
        if (typeof stage !== "undefined") stage.requestFxState()
    }

    // 图片导入 FileDialog (root 层级)
    FileDialog {
        id: importImagePick
        title: "选择要解析到粒子上的图片（可多选，滚放按序列匹配）"
        // #5 多图根因 (2026-10-24 第二轮): 原 L621-622 按 rollerEnabled() 动态切 OpenFile,
        //   滚放关闭时无法多选; 现统一 OpenFiles (1张走单张路径, N张走批量, onAccepted 内分发).
        fileMode: FileDialog.OpenFiles
        nameFilters: ["图片文件 (*.jpg *.jpeg *.png *.webp *.bmp *.gif)", "所有文件 (*)"]
        onAccepted: {
            // 多选 → 批量导入按槽位循环匹配；单选 → 立即铺到当前粒子
            var files = importImagePick.selectedFiles
            // #5 多图根因实锤 (2026-10-24): FileDialog.selectedFiles 是 QML list<url>
            //   (QVariantList), 不是 JS Array → Array.isArray() 恒 false → 旧代码永远
            //   走 else 单张分支, 用户多选 N 张也只导入第 1 张 (日志: QML selectedFiles n=-1).
            //   改用 files.length 判断 (QVariantList 有 length, 可索引访问 files[i]).
            // #5 诊断: QML console.log 在 QQuickView 里不落盘, 改走 C++ 回报
            if (typeof stage !== "undefined") {
                stage.importParticleImagesLog((files && files.length) ? files.length : 0,
                                              (files && files.length) ? String(files[0]) : "(none)")
            }
            if (files && files.length > 0) {
                var paths = []
                for (var i = 0; i < files.length; i++) {
                    var p = String(files[i])
                    // "file:///" 是 8 个字符，substring(7) 会留下前导 "/" 导致 QImage::load 失败（图片导入不更新的根因）
                    if (p.indexOf("file:///") === 0) p = p.substring(8)
                    paths.push(decodeURIComponent(p))
                }
                if (typeof stage !== "undefined") {
                    if (paths.length === 1) stage.importParticleImage(paths[0])
                    else stage.importParticleImages(paths)
                }
            } else {
                var p2 = String(importImagePick.selectedFile)
                if (p2.indexOf("file:///") === 0) p2 = p2.substring(8)
                if (typeof stage !== "undefined") stage.importParticleImage(decodeURIComponent(p2))
            }
        }
    }

    // #5 滚放拖拽排序兜底 (2026-10-23 第 5 版): 拖拽进行中, 鼠标越出当前行后在面板
    //   内其它位置 (标题栏/参数区/空白处) 释放, rowDragMa 收不到 onReleased (QQuickView
    //   里 grabMouse 不生效) → 状态卡死 (rollerDragging 一直 true, 标题栏/Flickable 全禁).
    //   兜底 MouseArea 挂在 root 层, 仅 rollerDragging=true 时启用, 覆盖整个面板,
    //   z 高于内容但低于 FileDialog; 释放时调 _commitRollerDrag() 兜底清理.
    //   不影响正常操作: 平时 enabled=false 不拦截任何事件.
    MouseArea {
        id: dragFallbackMa
        anchors.fill: parent
        z: 100
        enabled: root.rollerDragging
        propagateComposedEvents: false
        cursorShape: Qt.SizeAllCursor
        onReleased: {
            root._commitRollerDrag()
        }
        // 移动: 鼠标越出本行后 rowDragMa 收不到事件, 兜底刷新目标行
        onPositionChanged: function(mouse) {
            var gY = dragFallbackMa.mapToGlobal(0, 0).y + mouse.y
            root._updateRollerDragTarget(gY, root.dragSrcTop, root.dragSrcHeight, root.draggingIdx, root.rollerSeq().length)
        }
    }


    // ===== 行内组件 =====
    component SliderRow: RowLayout {
        id: srow
        property string kKey: ""
        property string label: ""
        property real mn: 0
        property real mx: 1
        property real dflt: 0
        spacing: 6
        Layout.fillWidth: true
        Layout.preferredHeight: 30
        Text {
            text: srow.label
            color: root.panelText
            font.pixelSize: 12
            font.family: "Microsoft YaHei UI"
            Layout.preferredWidth: 74
            Layout.alignment: Qt.AlignVCenter
        }
        MuyunSlider {
            id: sl
            Layout.fillWidth: true
            implicitWidth: 100
            fillColor: root.accent
            trackColor: root.grooveBg
            from: srow.mn
            to: srow.mx
            // 回显只在未拖拽时写回，防止拖动中被 echo 打断
            Binding {
                target: sl
                property: "value"
                value: root.fxNum(srow.kKey, srow.dflt)
                when: !sl.pressed
                restoreMode: Binding.RestoreBindingOrValue
            }
            onMoved: if (typeof stage !== "undefined") stage.fxSet(srow.kKey, value)
        }
        Text {
            text: sl.value.toFixed(2)
            color: root.subText
            font.pixelSize: 11
            font.family: "Consolas"
            Layout.preferredWidth: 36
            horizontalAlignment: Text.AlignRight
            Layout.alignment: Qt.AlignVCenter
        }
    }

    // 开关行：整行点击发 fxToggle；状态是引擎回显的纯镜像（不本地翻转）
    component SwitchRow: Rectangle {
        id: swrow
        property string kKey: ""
        property string label: ""
        property bool dflt: false
        readonly property bool on: root.fxBool(kKey, dflt)
        Layout.fillWidth: true
        implicitHeight: 28
        color: swMa.containsMouse ? "#14ffffff" : "transparent"
        radius: 6
        Text {
            anchors.left: parent.left
            anchors.leftMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            text: swrow.label
            color: root.panelText
            font.pixelSize: 12
            font.family: "Microsoft YaHei UI"
        }
        Rectangle {
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            implicitWidth: 40; implicitHeight: 22
            radius: 11
            color: swrow.on ? root.accent : root.grooveBg
            Rectangle {
                x: swrow.on ? parent.width - width - 3 : 3
                anchors.verticalCenter: parent.verticalCenter
                width: 16; height: 16; radius: 8
                color: "white"
                Behavior on x { NumberAnimation { duration: 120 } }
            }
        }
        MouseArea {
            id: swMa
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: if (typeof stage !== "undefined") stage.fxToggle(swrow.kKey)
        }
    }
}
