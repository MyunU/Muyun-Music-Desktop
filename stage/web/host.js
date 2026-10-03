/* ============================================================
 *  MuyunStage 宿主 glue（自研，MIT；接口语义对齐 Sollin 宿主）
 *
 *  职责：
 *   1. 提供引擎的 DOM/CSS 契约与「假 audio」时钟（播放权威在主程序）
 *   2. 把主程序命令（track/lyrics/clk/fx…）翻译成引擎 API
 *   3. 把页面控制事件（播放条/进度/收藏…）经原生通道回传主程序
 *
 *  音画说明：本进程不出声。引擎拿不到 captureStream（假 audio 无此方法）
 *  → initAudio 静默跳过 → 实时节拍降级；节拍/能量将在阶段2由主程序
 *  离线解析后经 beatmap/analyser 通道注入。当前封面粒子、舞台歌词、
 *  电影镜头时间动画均为时间驱动，不受影响。
 * ============================================================ */
import { createMineradioEngine } from '/mineradio/engine.js'
import { initSpectrum, startAnalyze, resetSpectrum, spectrumState } from '/spectrum.js'

// ---------- 原生通道 ----------
const native = (window.chrome && window.chrome.webview) ? window.chrome.webview : null
function post (obj) { if (native) native.postMessage(obj) }
// JS 异常回传主程序日志（定位引擎内部错误）
window.addEventListener('error', (e) => {
  post({ e: 'log', m: 'JSERR ' + (e.message || '') + ' @' + (e.filename || '') + ':' + (e.lineno || 0) })
})
window.addEventListener('unhandledrejection', (e) => {
  post({ e: 'log', m: 'JSPROMPT-REJECT ' + String((e.reason && e.reason.message) || e.reason) })
})

// 调试：转发引擎节拍相关 console 到主程序（含 beat/tempo/分析/节拍）
;(() => {
  const origLog = console.log.bind(console)
  const origWarn = console.warn.bind(console)
  const want = (a) => { const s = a.map(String).join(' '); return /beat|tempo|分析|节拍|music-tempo|decode|onset/i.test(s) }
  console.log = (...a) => {
    origLog(...a)
    const s = a.map(String).join(' ')
    if (want(a)) post({ e: 'log', m: 'ENG: ' + s.slice(0, 200) })
    // 分析成功：'music-tempo worker: <bpm> bpm, beats: <N> ...'
    const m = /music-tempo worker:\s*([\d.]+)\s*bpm,?\s*beats:\s*(\d+)/i.exec(s)
    if (m) post({ e: 'beat', maps: Number(m[2]), sample: 'bpm=' + m[1] })
  }
  console.warn = (...a) => { origWarn(...a); if (want(a)) post({ e: 'log', m: 'ENGW: ' + a.map(String).join(' ').slice(0, 200) }) }
})()

// ---------- 播放时钟：主程序 clk 消息为权威，tick 间线性外插 ----------
const clock = { tMs: 0, dMs: 0, playing: false, recv: performance.now() }
function nowSec () {
  if (!clock.playing) return clock.tMs / 1000
  return (clock.tMs + (performance.now() - clock.recv)) / 1000
}

// ---------- 频谱注入（A 项）：track.bin 离线 FFT → 假 analyser 查表 ----------
initSpectrum({
  clock: nowSec,
  log: (m) => post({ e: 'log', m: 'SPEC: ' + m }),
})

// ---------- 静默 MediaStream：满足引擎 initAudio 对 captureStream 的要求 ----------
// 没有 analyser 时，引擎主循环的整个节拍块（含 tickBeatMap 离线谱驱动）不会执行。
// 这里给假 audio 一条真实的"静音流"（增益 0 的振荡器 → MediaStreamDestination），
// 引擎据此建立 analyser（读到的恒为 0，实时鼓点自然不触发），
// 但 tickBeatMap() 会按 setTrack 离线分析出的节拍图正常驱动相机 → 镜头跟拍恢复。
let silentCtx = null
let silentStream = null
function getSilentStream () {
  const alive = silentStream && silentStream.getAudioTracks().some((t) => t.readyState === 'live')
  if (alive) return silentStream
  try {
    if (silentCtx) { try { silentCtx.close() } catch (e) {} }
    silentCtx = new (window.AudioContext || window.webkitAudioContext)()
    const dest = silentCtx.createMediaStreamDestination()
    const osc = silentCtx.createOscillator()
    const gain = silentCtx.createGain()
    gain.gain.value = 0
    osc.connect(gain); gain.connect(dest)
    osc.start()
    silentStream = dest.stream
  } catch (e) {
    silentCtx = null; silentStream = null
  }
  return silentStream
}

// ---------- 假 audio 元素（仅提供 currentTime/duration/paused 状态） ----------
const fakeAudio = {
  get currentTime () { return nowSec() },
  set currentTime (v) { clock.tMs = Math.max(0, v) * 1000; clock.recv = performance.now() },
  get duration () { return clock.dMs / 1000 },
  get paused () { return !clock.playing },
  get ended () { return false },
  volume: 1,
  playbackRate: 1,
  currentSrc: '',
  src: '',
  play () { return Promise.resolve() },
  pause () {},
  load () {},
  addEventListener () {},
  removeEventListener () {},
  // 关键：提供静音流，让引擎的 analyser/tickBeatMap 正常走
  captureStream () { return getSilentStream() },
}

// ---------- 歌词转换：Muyun {time,text,words,translation} → Mineradio ----------
// 有译文的行合并为「原文\n译文」双行（引擎 makeLyricMask 支持显式换行），
// 双行时该行去掉逐字 words、全局关 karaoke（避免字索引错位）。
function convertLyrics (lines) {
  const out = []
  let karaokeLines = 0
  let anyTrans = false
  for (const L of (lines || [])) {
    const orig = String(L.text || '').replace(/\s+/g, ' ').trim()
    const tr = String(L.translation || '').replace(/\s+/g, ' ').trim()
    if (!orig && !tr) continue
    let text = orig
    if (tr) { anyTrans = true; text = orig ? orig + '\n' + tr : tr }
    const words = []
    let charCount = 0
    if (!tr && L.words && L.words.length) {
      for (const w of L.words) {
        const wt = String(w.text || '')
        if (!wt) continue
        const s = Number(w.startTime) || 0
        const e = Math.max(s + 0.06, Number(w.endTime) || s + 0.3)
        words.push({ text: wt, c0: charCount, c1: charCount + Array.from(wt.trim()).length || charCount + 1,
          s: Math.round(s * 100) / 100, e: Math.round(e * 100) / 100, d: Math.round((e - s) * 100) / 100 })
        charCount += Array.from(wt).length
      }
    }
    const entry = {
      t: Number(L.time) || 0,
      duration: 0,
      text,
      charCount: Math.max(1, Array.from(text.replace(/\n/g, '')).length),
      source: words.length ? 'yrc-word' : 'lrc',
    }
    if (words.length >= 2) { entry.words = words; karaokeLines++ }
    out.push(entry)
  }
  out.sort((a, b) => a.t - b.t)
  for (let i = 0; i < out.length; i++) {
    const cur = out[i], next = out[i + 1]
    if (next) {
      const gap = Math.max(0.06, next.t - cur.t)
      cur.duration = Math.min(cur.duration || gap, gap)
    } else if (!cur.duration) cur.duration = 6
  }
  const hasKaraoke = !anyTrans && karaokeLines > 0 && karaokeLines >= Math.ceil(out.length * 0.3)
  return { lines: out, hasKaraoke, timingSource: hasKaraoke ? 'yrc-word' : (out.length ? 'lrc' : 'none') }
}

// ---------- SVG 图标（lucide 风格内联） ----------
const ICON = {
  play:  '<svg viewBox="0 0 24 24" fill="currentColor" stroke="none"><polygon points="6 3 20 12 6 21 6 3"/></svg>',
  pause: '<svg viewBox="0 0 24 24" fill="currentColor" stroke="none"><rect x="5" y="4" width="5" height="16" rx="1"/><rect x="14" y="4" width="5" height="16" rx="1"/></svg>',
  repeat: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m17 2 4 4-4 4"/><path d="M3 11v-1a4 4 0 0 1 4-4h14"/><path d="m7 22-4-4 4-4"/><path d="M21 13v1a4 4 0 0 1-4 4H3"/></svg>',
  repeat1: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m17 2 4 4-4 4"/><path d="M3 11v-1a4 4 0 0 1 4-4h14"/><path d="m7 22-4-4 4-4"/><path d="M21 13v1a4 4 0 0 1-4 4H3"/><path d="M11 10h1v4"/></svg>',
  shuffle: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m18 2 4 4-4 4"/><path d="M2 22v-4a4 4 0 0 1 4-4h12"/><path d="m18 14 4 4-4 4"/><path d="M2 2v4a4 4 0 0 0 4 4h2"/><path d="M22 6h-6L4 18"/></svg>',
  volume: '<svg id="volume-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5"/><path d="M15.54 8.46a5 5 0 0 1 0 7.07"/><path d="M19.07 4.93a10 10 0 0 1 0 14.14"/></svg>',
  mute: '<svg id="volume-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5"/><line x1="22" x2="16" y1="9" y2="15"/><line x1="16" x2="22" y1="9" y2="15"/></svg>',
  heart: '<svg class="heart-svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M19 14c1.49-1.46 3-3.21 3-5.5A5.5 5.5 0 0 0 16.5 3c-1.76 0-3 .5-4.5 2-1.5-1.5-2.74-2-4.5-2A5.5 5.5 0 0 0 2 8.5c0 2.3 1.5 4.05 3 5.5l7 7Z"/></svg>',
}
const MODE_ICONS = { sequence: ICON.repeat, loop: ICON.repeat, single: ICON.repeat1, shuffle: ICON.shuffle }

// ---------- DOM ----------
const $ = (id) => document.getElementById(id)
const el = {
  root: $('stage-root'),
  fill: $('progress-fill'), thumb: $('progress-thumb'), bar: $('progress-bar'),
  time: $('time-display'), play: $('play-btn'), mode: $('play-mode-btn'),
  heart: $('heart-btn'), vol: $('volume-control'), volBtn: $('volume-btn'),
  volSlider: $('volume-slider'), volValue: $('volume-value'),
  title: $('control-title'), artist: $('control-artist'), cover: $('control-cover'),
  thumbTitle: $('thumb-title'), thumbArtist: $('thumb-artist'),
}

// ---------- 引擎 ----------
let engine = null
let fxTimer = 0
let immersive = false
let curVol = 1

// ---------- 沉浸空闲自动隐藏：2 秒无操作 → 藏鼠标+底部组件栏；任何操作即唤回 ----------
// （CSS 见 host.css .ui-idle-hidden；只与 immersive-mode 组合生效，不影响普通模式手动显隐）
let idleTimer = 0
function wakeUi () {
  el.root.classList.remove('ui-idle-hidden')
  clearTimeout(idleTimer)
  if (immersive) idleTimer = setTimeout(() => el.root.classList.add('ui-idle-hidden'), 2000)
}
;['pointermove', 'pointerdown', 'wheel', 'keydown'].forEach((t) =>
  document.addEventListener(t, wakeUi, { passive: true }))
// 自检钩子（本地页面、无外部脚本，参照 document.title 信号同款调试面）
window.__muyunUi = {
  wakeUi,
  immersive: () => immersive,
  idleHidden: () => el.root.classList.contains('ui-idle-hidden'),
}

function safeState () {
  if (!engine) return {}
  try {
    const st = engine.getState()
    const f = st.fx
    return {
      preset: f.preset, intensity: f.intensity, cinema: !!f.cinema,
      cinemaShake: f.cinemaShake, depth: f.depth, coverResolution: f.coverResolution,
      point: f.point, speed: f.speed, twist: f.twist, color: f.color,
      scatter: f.scatter, bgFade: f.bgFade,
      aiDepth: !!f.aiDepth, particleLyrics: !!f.particleLyrics,
      bloom: !!f.bloom, bloomStrength: f.bloomStrength, edge: !!f.edge,
      lyricScale: f.lyricScale, lyricGlow: !!f.lyricGlow, lyricGlowStrength: f.lyricGlowStrength,
      lyricGlowBeat: !!f.lyricGlowBeat, lyricGlowParticles: !!f.lyricGlowParticles,
      lyricCameraLock: !!f.lyricCameraLock,
    }
  } catch (e) { return {} }
}
function scheduleFxEcho () {
  clearTimeout(fxTimer)
  fxTimer = setTimeout(() => post({ e: 'fx', state: safeState() }), 250)
}

function initEngine () {
  try {
    engine = createMineradioEngine({
      canvasContainer: $('canvas-container'),
      albumBg: $('album-bg'),
      overlayRoot: el.root,
      audio: fakeAudio,
      assetBase: '/mineradio/',
      toast: (msg) => { showLocalToast(msg); post({ e: 'toast', msg: String(msg == null ? '' : msg) }) },
      onFxChange: () => scheduleFxEcho(),
      onBeatChip: (s) => {
        $('beat-chip').classList.toggle('show', !!(s && s.visible))
        $('beat-text').textContent = (s && s.text) || '分析节奏…'
      },
      onImmersiveChange: (on) => {
        immersive = !!on
        el.root.classList.toggle('immersive-mode', immersive)
        wakeUi()   // 进入沉浸：武装 2s 空闲隐藏；退出：清类解除
        post({ e: 'immersive', on: immersive })   // 状态回报主程序（ESC 阶梯要用）
      },
    })
    post({ e: 'engineReady' })
    document.title = 'ENGINE-OK'
  } catch (err) {
    document.title = 'ENGINE-FAIL'
    post({ e: 'fatal', msg: String((err && err.stack) || err) })
  }
}

// ---------- 工具 ----------
function fmt (sec) {
  if (!isFinite(sec) || sec < 0) sec = 0
  const t = Math.floor(sec)
  return Math.floor(t / 60) + ':' + String(t % 60).padStart(2, '0')
}
function setPlayIcon (playing) { el.play.innerHTML = playing ? ICON.pause : ICON.play }
function setModeIcon (id) { el.mode.innerHTML = MODE_ICONS[id] || ICON.repeat }
function setVolIcon (v) { el.volBtn.innerHTML = v <= 0.001 ? ICON.mute : ICON.volume }

// ---------- rAF：进度条/时间 ----------
let dragRatio = null   // 拖拽预览
function tickUI () {
  requestAnimationFrame(tickUI)
  const durSec = clock.dMs / 1000
  const cur = dragRatio != null ? dragRatio * durSec : Math.min(nowSec(), durSec || nowSec())
  const ratio = durSec > 0 ? (dragRatio != null ? dragRatio : cur / durSec) : 0
  el.fill.style.width = (ratio * 100).toFixed(2) + '%'
  el.thumb.style.left = (ratio * 100).toFixed(2) + '%'
  el.time.textContent = fmt(cur) + ' / ' + fmt(durSec)
}

// ---------- 进度条交互 ----------
function ratioFromEvent (ev) {
  const r = el.bar.getBoundingClientRect()
  return r.width > 0 ? Math.min(1, Math.max(0, (ev.clientX - r.left) / r.width)) : 0
}
let dragging = false
el.bar.addEventListener('pointerdown', (ev) => {
  dragging = true
  dragRatio = ratioFromEvent(ev)
  if (engine) { engine.markRenderInteraction('progress-seek'); engine.emitProgressDragParticles(ev.clientX, ev.clientY) }
  el.bar.setPointerCapture(ev.pointerId)
})
el.bar.addEventListener('pointermove', (ev) => {
  if (!dragging) return
  dragRatio = ratioFromEvent(ev)
  if (engine) engine.emitProgressDragParticles(ev.clientX, ev.clientY)
})
function endDrag (ev) {
  if (!dragging) return
  dragging = false
  const ratio = dragRatio
  dragRatio = null
  if (ratio != null && clock.dMs > 0) post({ e: 'ctrl', a: 'seek', v: Math.round(ratio * clock.dMs) })
}
el.bar.addEventListener('pointerup', endDrag)
el.bar.addEventListener('pointercancel', endDrag)

// ---------- 控制事件 ----------
el.play.addEventListener('click', () => post({ e: 'ctrl', a: 'play' }))
$('prev-btn').addEventListener('click', () => post({ e: 'ctrl', a: 'prev' }))
$('next-btn').addEventListener('click', () => post({ e: 'ctrl', a: 'next' }))
el.mode.addEventListener('click', () => post({ e: 'ctrl', a: 'mode' }))
el.heart.addEventListener('click', () => post({ e: 'ctrl', a: 'like' }))
$('close-btn').addEventListener('click', () => post({ e: 'ctrl', a: 'back' }))
$('lyrics-toggle-btn').addEventListener('click', () => engine && engine.toggleLyrics())
$('immersive-btn').addEventListener('click', () => engine && engine.setImmersive(!immersive))

// ---------- 键盘转发：舞台（独立进程）点击控件后 WebView2 会抢走激活，
// 主窗的 QML Shortcut 收不到键 → 页面把 Esc/Space 经事件管道回传（close-btn 的
// 提示「返回 (Esc)」由此兑现；Space 语义与歌词页一致：播放/暂停）。
// ESC 阶梯（页面是唯一裁判，主程序不再拿自己那份 immersive 标志猜）：
//   沉浸中 → 只退沉浸；否则 → 回传 back 关歌词页。
// 主窗有焦点时主程序发 cmd{c:'key',k:'esc'} 进来，跑的也是同一个 escLadder()。 ----------
function isImmersiveNow () {
  // 以 DOM 类为准，不信缓存标志：引擎早期回报 immersive 时事件管道可能还没接通，
  // 那条回报会丢 → 页面以为自己还在沉浸、主程序以为不是，于是第一次 ESC 变成
  // "白按一次退沉浸"（用户症状：ESC 用过一次后就大部分时间无效）。
  return el.root.classList.contains('immersive-mode')
}
function escLadder () {
  if (isImmersiveNow()) {
    immersive = true
    if (engine) engine.setImmersive(false)
    else {
      immersive = false
      el.root.classList.remove('immersive-mode', 'ui-idle-hidden')
    }
    post({ e: 'immersive', on: false })   // 顺手把主程序那份标志拉回一致
    return
  }
  if (immersive) immersive = false        // 缓存与视觉不一致时一并纠正
  post({ e: 'ctrl', a: 'back' })
}
document.addEventListener('keydown', (ev) => {
  const t = ev.target
  const inField = !!(t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.isContentEditable))
  if (ev.key === 'Escape') {
    ev.preventDefault()
    // 焦点在输入框（fx 滑杆等）时，Esc 先交还给页面控件再走阶梯——
    // 早先是直接 return，结果"碰过滑杆后 ESC 大部分时间失灵"
    if (inField && t.blur) t.blur()
    escLadder()
    return
  }
  if (inField) return                       // Space 之类仍要放行给输入框
  if (ev.key === ' ' || ev.code === 'Space') { ev.preventDefault(); post({ e: 'ctrl', a: 'play' }) }
})
$('fx-fab').addEventListener('click', () => {
  // 主程序 QML 侧「视觉控制台」浮层面板（toggle 语义在主程序）
  post({ e: 'ctrl', a: 'fx' })
})

// ---------- 页面内轻提示（主窗被舞台盖住，toast 必须画在本页） ----------
let toastEl = null
let toastTimer = 0
function showLocalToast (msg) {
  if (!toastEl) {
    toastEl = document.createElement('div')
    toastEl.style.cssText = 'position:fixed;left:50%;bottom:120px;transform:translateX(-50%);' +
      'padding:9px 18px;border-radius:20px;font:13px/1.4 "Microsoft YaHei UI",sans-serif;' +
      'color:#f2f4f8;background:rgba(24,26,32,.86);border:1px solid rgba(255,255,255,.14);' +
      'backdrop-filter:blur(10px);z-index:99;opacity:0;transition:opacity .2s;pointer-events:none'
    document.body.appendChild(toastEl)
  }
  toastEl.textContent = String(msg || '')
  toastEl.style.opacity = '1'
  clearTimeout(toastTimer)
  toastTimer = setTimeout(() => { if (toastEl) toastEl.style.opacity = '0' }, 2200)
}

el.volBtn.addEventListener('click', (ev) => {
  ev.stopPropagation()
  el.vol.classList.toggle('open')
})
document.addEventListener('click', (ev) => {
  if (!el.vol.contains(ev.target)) el.vol.classList.remove('open')
})
el.volSlider.addEventListener('input', () => {
  curVol = Number(el.volSlider.value)
  el.volValue.textContent = Math.round(curVol * 100) + '%'
  setVolIcon(curVol)
  post({ e: 'ctrl', a: 'vol', v: curVol })
})
el.vol.addEventListener('wheel', (ev) => {
  ev.preventDefault()
  const v = Math.min(1, Math.max(0, curVol + (ev.deltaY < 0 ? 0.02 : -0.02)))
  el.volSlider.value = String(v)
  el.volSlider.dispatchEvent(new Event('input'))
}, { passive: false })

function applyVolume (v) {
  curVol = Math.min(1, Math.max(0, Number(v)))
  el.volSlider.value = String(curVol)
  el.volValue.textContent = Math.round(curVol * 100) + '%'
  setVolIcon(curVol)
}

// ---------- 歌词应用（译文已并入 convertLyrics 的「原文\n译文」双行） ----------
let rawLyricLines = []
let convLyrics = []
function applyLyrics () {
  if (!engine) return
  const r = convertLyrics(rawLyricLines)
  convLyrics = r.lines
  try {
    engine.setLyrics(r.lines, { hasKaraoke: r.hasKaraoke, timingSource: r.timingSource })
  } catch (e) { post({ e: 'log', m: 'setLyrics failed: ' + String(e) }) }
}

// ---------- 主程序命令入口 ----------
// 原生壳用 PostWebMessageAsJson 发送，页面经 message 事件收到已解析对象（UTF-8 无损）。
function handleCmd (msg) {
  if (!msg || typeof msg !== 'object') return
  if (msg.c !== 'clk') post({ e: 'log', m: 'recv cmd=' + msg.c })
  switch (msg.c) {
    case 'track': {
      // 封面相对路径（/covers/xx.jpg）→ 绝对 URL：引擎 loadCoverFromUrl 只接受
      // ^(https?|data|blob|file): 前缀，相对路径会被判无效 → 粒子退回渐变幕
      let coverUrl = String(msg.cover || '')
      if (coverUrl.startsWith('/')) coverUrl = location.origin + coverUrl
      const t = {
        id: String(msg.id || ''), name: String(msg.name || ''),
        artist: String(msg.artist || ''), album: String(msg.album || ''),
        cover: coverUrl, url: String(msg.url || ''), platform: String(msg.platform || ''),
      }
      el.title.textContent = t.name || '未在播放'
      el.artist.textContent = t.artist || '—'
      el.thumbTitle.textContent = t.name || ''
      el.thumbArtist.textContent = t.artist || ''
      if (t.cover) {
        el.cover.style.backgroundImage = 'url("' + t.cover + '")'
        el.cover.classList.remove('cover-empty')
      } else {
        el.cover.style.backgroundImage = ''
        el.cover.classList.add('cover-empty')
      }
      try { engine && engine.setTrack(t) } catch (e) {
        post({ e: 'log', m: 'setTrack threw: ' + String(e) })
      }
      // 频谱注入：带 url（track.bin 已挂载）→ 离线分析；无 url → 关闭注入
      if (t.url) startAnalyze(t.url, t.id)
      else resetSpectrum()
      // 封面加载诊断（同源 /covers/*，粒子云靠它取色）
      if (t.cover) {
        const probe = new Image()
        probe.onload = () => post({ e: 'log', m: 'cover OK ' + probe.naturalWidth + 'x' + probe.naturalHeight })
        probe.onerror = () => post({ e: 'log', m: 'cover ERR ' + t.cover })
        probe.src = t.cover
      }
      break
    }
    case 'fav':
      el.heart.classList.toggle('liked', !!msg.on)
      el.heart.querySelector('svg').setAttribute('fill', msg.on ? 'currentColor' : 'none')
      break
    case 'immersive':
      // 主程序驱动的沉浸切换（保留：fx 面板等仍可直接调；ESC 阶梯请用下面的 key 指令）
      if (engine) engine.setImmersive(!!msg.on)
      break
    case 'key':
      // 主窗有焦点时主程序把 Esc 送进来，阶梯判断仍由页面做（页面才知道自己是否真在沉浸）
      if (msg.k === 'esc') escLadder()
      else if (msg.k === 'space') post({ e: 'ctrl', a: 'play' })
      break
    case 'lyrics': {
      rawLyricLines = Array.isArray(msg.lines) ? msg.lines : []
      applyLyrics()
      break
    }
    case 'clk': {
      const tMs = Number(msg.t) || 0, dMs = Number(msg.d) || 0, p = !!msg.p
      const expected = nowSec() * 1000
      clock.recv = performance.now()
      clock.tMs = tMs
      clock.dMs = dMs
      const wasPlaying = clock.playing
      clock.playing = p
      if (Math.abs(tMs - expected) > 800 && engine) engine.notifySeek()
      if (wasPlaying !== p) {
        setPlayIcon(p)
        try { engine && engine.setPlaying(p) } catch (e) {}
      }
      break
    }
    case 'pmode': setModeIcon(String(msg.id || 'sequence')); break
    case 'vol': applyVolume(msg.v); break
    case 'domprobe': {
      // 调试：枚举视口分数矩形内可见元素（定位"黑块"真身）
      const hits = []
      const fx = msg.fx != null ? msg.fx : 0.55, fy = msg.fy != null ? msg.fy : 0.7
      const fw = msg.fw != null ? msg.fw : 0.45, fh = msg.fh != null ? msg.fh : 0.3
      const x0 = innerWidth * fx, y0 = innerHeight * fy
      const x1 = innerWidth * (fx + fw), y1 = innerHeight * (fy + fh)
      // fx-fab 专项体检（fx 面板入口调试）
      try {
        const fab = document.getElementById('fx-fab')
        if (fab) {
          const r = fab.getBoundingClientRect()
          const cs = getComputedStyle(fab)
          const top = document.elementFromPoint(r.left + r.width / 2, r.top + r.height / 2)
          post({ e: 'log', m: 'FABPROBE rect=' + Math.round(r.left) + ',' + Math.round(r.top) +
            ' ' + Math.round(r.width) + 'x' + Math.round(r.height) +
            ' vis=' + cs.visibility + ' disp=' + cs.display + ' op=' + cs.opacity +
            ' pe=' + cs.pointerEvents + ' z=' + cs.zIndex +
            ' topEl=' + (top ? (top.id || top.className || top.tagName) : 'null') +
            ' vw=' + innerWidth + ' vh=' + innerHeight + ' dpr=' + devicePixelRatio })
        } else post({ e: 'log', m: 'FABPROBE no #fx-fab' })
      } catch (e) { post({ e: 'log', m: 'FABPROBE fail ' + String(e) }) }
      document.querySelectorAll('*').forEach((e) => {
        const r = e.getBoundingClientRect()
        if (r.right > x0 && r.left < x1 && r.bottom > y0 && r.top < y1 && r.width > 16 && r.height > 16) {
          const cs = getComputedStyle(e)
          if (parseFloat(cs.opacity) > 0.25 && cs.display !== 'none' && cs.visibility !== 'hidden') {
            hits.push((e.id || String(e.className).slice(0, 18)) + ' ' + Math.round(r.width) + 'x' + Math.round(r.height) +
              '@' + Math.round(r.left) + ',' + Math.round(r.top) + ' bg=' + cs.backgroundColor +
              ' bf=' + (cs.backdropFilter !== 'none' ? String(cs.backdropFilter).slice(0, 20) : '-') +
              ' z=' + cs.zIndex)
          }
        }
      })
      const bar = document.getElementById('bottom-bar')
      post({ e: 'log', m: 'DOMPROBE vw=' + innerWidth + ' vh=' + innerHeight +
        ' rootcls=' + el.root.className +
        ' bar=' + (bar ? Math.round(bar.getBoundingClientRect().width) + 'x' + Math.round(bar.getBoundingClientRect().height) + ' cls=' + bar.className : '-') +
        ' hits[' + hits.slice(0, 12).join(' ;; ') + ']' })
      break
    }
    case 'beatprobe': {
      // 回报离线节拍缓存（引擎分析完音频后写入 localStorage）
      let n = 0, sample = ''
      try {
        const raw = localStorage.getItem('mineradio-local-beatmaps-v1')
        if (raw) { const o = JSON.parse(raw); const ks = Object.keys(o); n = ks.length
          if (n) { const e0 = o[ks[0]]; const mr = e0 && e0.mr; sample = ks[0].slice(0,20) + ':' + (mr && mr.cameraBeats ? mr.cameraBeats.length : 0) } }
      } catch (e) {}
      const dbg = window.__mineradioDebug ? window.__mineradioDebug() : {}
      const spec = spectrumState()
      // 当前舞台歌词行（⏎ 标记双行换行，验证译文双行）：宿主按时间自行定位转换行
      let lyrText = ''
      const tSec = nowSec()
      for (const cl of convLyrics) { if (cl.t <= tSec + 0.05) lyrText = cl.text; else break }
      post({ e: 'beat', maps: n, sample:
        'audioReady=' + dbg.audioReady + ' analyser=' + dbg.analyserSet +
        ' camZ=' + dbg.cameraZ + ' uB=' + dbg.uBass + ' uE=' + dbg.uEnergy +
        ' spec=' + (spec.ready ? 'ready' : (spec.analyzing ? 'analyzing' : 'off')) + '/' + spec.frames +
        ' lyr=' + lyrText.replace(/\n/g, '⏎').slice(0, 60) })
      break
    }
    case 'theme': break
    case 'bounds': try { engine && engine.syncViewport() } catch (e) {} break
    case 'fx': {
      if (!engine) break
      try {
        if (msg.op === 'set') engine.setFxValue(String(msg.k), Number(msg.v))
        else if (msg.op === 'toggle') engine.toggleFx(String(msg.k))
        else if (msg.op === 'preset') engine.setPreset(Number(msg.i) | 0)
        else if (msg.op === 'reset') engine.resetFx()
        else if (msg.op === 'get') post({ e: 'fx', state: safeState() })
      } catch (e) { post({ e: 'log', m: 'fx op failed: ' + String(e) }) }
      break
    }
    default: break
  }
}

// ---------- 启动 ----------
setPlayIcon(false)
setModeIcon('sequence')
setVolIcon(1)
// 原生 → 页面：结构化消息事件（主通道，PostWebMessageAsJson）
if (native && native.addEventListener) {
  native.addEventListener('message', (ev) => {
    let d = ev && ev.data
    if (typeof d === 'string') { try { d = JSON.parse(d) } catch (e) { return } }
    handleCmd(d)
  })
}
// 兼容入口（若有消息走 ExecuteScript 传字符串/对象）
window.muyunOnNativeMessage = (obj) => {
  try { handleCmd(typeof obj === 'string' ? JSON.parse(obj) : obj) } catch (e) {}
}
post({ e: 'ready' })
initEngine()
tickUI()
