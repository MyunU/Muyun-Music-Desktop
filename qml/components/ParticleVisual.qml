import QtQuick

// ===========================================================================
//  #29 粒子视觉组件：封面 ↔ 滚筒 逐粒子 morph（全屏歌词页 mineradio 样式用）
// ---------------------------------------------------------------------------
//  设计（成本可控的 GPU 方案）：
//   - 屏幕按 uCell 划分成网格，每个网格格点 = 一个粒子（片元 O(1) 距离场，
//     不做逐粒子遍历，粒子数 ≈ 宽*高/uCell²，1 个片元只算自己格点的粒子）
//   - 封面形态：粒子平铺成封面点阵，颜色 = 采样封面图对应像素（封面由粒子构成）
//   - 滚筒形态：粒子按"漏斗环带"分布（半径随行渐变 + 透视近大远小），绕中心自转
//   - 逐粒子 morph：每粒子带随机相位，morphT 错峰插值（种子靠前的先变），
//     位置与颜色同步过渡 → 肉眼可见"粒子逐个从封面流进滚筒"
//   - 镜头缓动：整体旋转/缩放/漂移由导演状态机驱动（切歌时 punch、滚筒时呼吸）
//   - 导演状态机：打开/切歌 → 封面展示 → 缓动进滚筒；暂停降速、播放恢复
//   shader：qml/shaders/particle_morph.frag（GLSL 440）→ qsb.exe 编译成
//   particle_morph.frag.qsb 打进 qrc（Qt 6 ShaderEffect 只认 .qsb，不认字符串）
//  对外契约：
//   coverSource : 封面 Image（直接当 sampler2D 采样原图，不应用 fillMode）
//   playing     : 播放中（false 时自转与呼吸大幅降速）
//   songTag     : 切歌标记（内容变化即触发"回封面→进滚筒"导演流程）
//   active      : 页面可见（false 时停表省电）
// ===========================================================================
Item {
    id: root

    property var coverSource: null        // 封面 Image（sampler2D 采样原图）
    property bool playing: false
    property bool active: false
    property bool dirEnabled: true          // 导演状态机开关（自检可关掉，完全手动控制 morphT）
    property var songTag: null            // 切歌标记（内容变化即视为切歌）
    property real cellPx: 22              // 粒子格点间距（像素）；越小粒子越密
    property color fallbackColor: "#f2f4ff" // 无封面时的粒子底色

    // 镜头参数（导演状态机输出，也可由外部微调）
    property real camRot: 0.0
    property real camZoom: 1.0
    property real camDriftX: 0.0
    property real camDriftY: 0.0
    property real morphT: 0.0             // 0=封面 1=滚筒（导演状态机驱动）

    // 供外部读取的当前形态名（调试/自检）
    readonly property string phaseName: morphT > 0.5 ? "tunnel" : "cover"
    // 供外部读取的 shader 编译状态（自检断言用：1=Compiled 2=Error 0=未编译）
    readonly property int shaderStatus: fx.status

    readonly property bool hasCover: coverSource !== null
        && coverSource.status === Image.Ready
    // 封面原图像素尺寸（fit 映射用；Image 加载完成后 implicit* 即图片实际尺寸）
    readonly property real coverW: hasCover ? Math.max(1, coverSource.implicitWidth) : 1
    readonly property real coverH: hasCover ? Math.max(1, coverSource.implicitHeight) : 1

    ShaderEffect {
        id: fx
        anchors.fill: parent
        visible: root.active

        // ---- 与 shader uniform block 同名的自定义属性（自动映射） ----
        property real uTime: 0
        property real uMorphT: root.morphT
        property real uCell: root.cellPx
        property real uCamRot: root.camRot
        property real uCamZoom: root.camZoom
        property real uDriftX: root.camDriftX
        property real uDriftY: root.camDriftY
        property real uHasCover: root.hasCover ? 1.0 : 0.0
        // Qt 6 ShaderEffect 属性映射：QSize→vec2（无 QVector2D），uSize 必须 size 类型
        property size uSize: Qt.size(fx.width, fx.height)
        property size uCoverSize: Qt.size(root.coverW, root.coverH)
        property vector3d uFallback: Qt.vector3d(root.fallbackColor.r, root.fallbackColor.g, root.fallbackColor.b)
        property var cover: root.coverSource   // Image → sampler2D（binding 1）

        fragmentShader: "qrc:/qml/shaders/particle_morph.frag.qsb"

        // 编译失败时把日志吐到 stderr，便于冒烟扫错
        onStatusChanged: if (status === ShaderEffect.Error) console.warn("[ParticleVisual] shader error: " + log)
    }

    // 时间驱动：只在页面可见时走表（播放中正常速度，暂停时降到 1/4 模拟慢镜头）
    Timer {
        interval: 33
        repeat: true
        running: root.active
        onTriggered: {
            var dt = 0.033
            if (!root.playing) dt *= 0.25
            fx.uTime += dt
        }
    }

    // =======================================================================
    //  导演状态机：切歌/打开 → 封面定格展示 → 缓动进滚筒；暂停降速/播放恢复
    // =======================================================================
    property var lastSongTag: null

    function stopSettle() { }

    function _dirOpen() {
        // 打开页面：封面展示 → 2.4s 后缓动进滚筒
        dirSeq.running = false
        dirSeq.restart()
    }

    function _dirSong() {
        // 切歌：快速 punch 回封面（新封面由粒子构成）→ 展示 → 进滚筒
        punchAnim.restart()
        dirSeq.running = false
        dirSeq.restart()
    }

    SequentialAnimation {
        id: dirSeq
        running: false
        // 回封面（若已在滚筒）
        NumberAnimation { target: root; property: "morphT"; to: 0.0; duration: 650; easing.type: Easing.InOutCubic }
        // 封面定格展示
        PauseAnimation { duration: 2400 }
        // 缓动进滚筒
        NumberAnimation { target: root; property: "morphT"; to: 1.0; duration: 2200; easing.type: Easing.InOutCubic }
    }

    // 切歌 punch：镜头快速推近 + 微旋，模拟"粒子重聚成新封面"
    ParallelAnimation {
        id: punchAnim
        running: false
        NumberAnimation { target: root; property: "camZoom"; from: 1.14; to: 1.0; duration: 900; easing.type: Easing.OutCubic }
        NumberAnimation { target: root; property: "camRot"; from: 0.05; to: 0.0; duration: 900; easing.type: Easing.OutCubic }
        NumberAnimation { target: root; property: "camDriftX"; from: 0.02; to: 0.0; duration: 900; easing.type: Easing.OutCubic }
        NumberAnimation { target: root; property: "camDriftY"; from: -0.015; to: 0.0; duration: 900; easing.type: Easing.OutCubic }
    }

    // 滚筒漫游：镜头轻微呼吸（自旋角度 + 缩放脉动），只在滚筒态且播放中
    Timer {
        id: roamT
        interval: 90
        repeat: true
        running: root.active && root.morphT > 0.5 && root.playing
        onTriggered: {
            var t = fx.uTime
            // 缓慢自旋（叠加在形态自转上）与缩放呼吸；速度受播放状态影响
            root.camRot = 0.18 * Math.sin(t * 0.22)
            root.camZoom = 1.0 + 0.05 * Math.sin(t * 0.55)
        }
    }

    // 事件接线
    onActiveChanged: {
        if (active && dirEnabled) _dirOpen()
    }
    onSongTagChanged: {
        if (active && dirEnabled && lastSongTag !== null && lastSongTag !== songTag) _dirSong()
        lastSongTag = songTag
    }
    Component.onCompleted: { lastSongTag = songTag }
}
