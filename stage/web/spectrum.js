/* ============================================================
 *  MuyunStage 频谱注入模块（自研，MIT）
 *
 *  背景：本进程不出声，假 audio 走静音流 → 引擎 analyser 读数恒 0，
 *  封面粒子的频段律动（uBass/uMid/uTreble/uEnergy）与实时鼓点引擎失效。
 *
 *  方案（备忘录 A 项）：webview 内 fetch track.bin → decodeAudioData
 *  → OfflineAudioContext 重采样到引擎 AudioContext 同采样率的 mono PCM
 *  → 分帧 FFT（2048 Hann，hop 20ms，bin=SR/2048 与引擎一致）→ 1024-bin
 *  dB 频谱帧表 + Int16 时域 PCM。
 *  再 monkey-patch AnalyserNode.prototype 的 getByteFrequencyData /
 *  getByteTimeDomainData：注入开启且时长表匹配时，按播放时钟查表填充；
 *  否则原样透传（静音流/其他 analyser 不受影响）。引擎零改动。
 * ============================================================ */

const SP = {
  trackId: '',
  ready: false,
  frames: null,       // Uint8Array(nFrames * 1024) dB 频谱帧表
  nFrames: 0,
  hopSec: 0.02,
  pcm: null,          // Int16Array mono（与 frames 同 SR）
  sr: 44100,
  bins: 1024,
  fftSize: 2048,
  analyzing: false,
}

let clockSec = () => 0
let logFn = () => {}

export function initSpectrum ({ clock, log }) {
  clockSec = clock || clockSec
  logFn = log || logFn
}

export function spectrumState () {
  return { ready: SP.ready, analyzing: SP.analyzing, frames: SP.nFrames, trackId: SP.trackId }
}

export function resetSpectrum () {
  SP.ready = false
  SP.analyzing = false
  SP.frames = null
  SP.nFrames = 0
  SP.pcm = null
  SP.trackId = ''
}

// ---------- FFT（iterative radix-2，2048 点，预计算位反转与旋转因子） ----------
function makeFFT (n) {
  const rev = new Uint32Array(n)
  let bits = 0
  while ((1 << bits) < n) bits++
  for (let i = 0; i < n; i++) {
    let r = 0
    for (let b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b)
    rev[i] = r
  }
  const cosT = new Float32Array(n / 2)
  const sinT = new Float32Array(n / 2)
  for (let i = 0; i < n / 2; i++) {
    cosT[i] = Math.cos(-2 * Math.PI * i / n)
    sinT[i] = Math.sin(-2 * Math.PI * i / n)
  }
  return function fft (re, im) {
    for (let i = 0; i < n; i++) {
      const j = rev[i]
      if (j > i) {
        let t = re[i]; re[i] = re[j]; re[j] = t
        t = im[i]; im[i] = im[j]; im[j] = t
      }
    }
    for (let size = 2; size <= n; size *= 2) {
      const half = size / 2
      const step = n / size
      for (let i = 0; i < n; i += size) {
        for (let j = i, k = 0; j < i + half; j++, k += step) {
          const l = j + half
          const tre = re[l] * cosT[k] - im[l] * sinT[k]
          const tim = re[l] * sinT[k] + im[l] * cosT[k]
          re[l] = re[j] - tre
          im[l] = im[j] - tim
          re[j] += tre
          im[j] += tim
        }
      }
    }
  }
}

// ---------- 分析：url → 帧表 + PCM ----------
let analysisToken = 0

export function startAnalyze (url, trackId) {
  const token = ++analysisToken
  resetSpectrumKeepToken()
  SP.trackId = String(trackId || '')
  SP.analyzing = true
  analyze(url, token, trackId)
}

function resetSpectrumKeepToken () {
  SP.ready = false
  SP.frames = null
  SP.nFrames = 0
  SP.pcm = null
}

async function analyze (url, token, trackId) {
  try {
    const resp = await fetch(url)
    if (!resp.ok) throw new Error('fetch ' + resp.status)
    const buf = await resp.arrayBuffer()
    if (token !== analysisToken) return
    const probe = new (window.AudioContext || window.webkitAudioContext)()
    const decoded = await probe.decodeAudioData(buf.slice(0))
    // 与引擎 analyser 同源采样率（同一默认设备），保证 bin→Hz 映射一致
    const sr = probe.sampleRate
    probe.close()
    if (token !== analysisToken) return

    // 重采样 + 下混 mono @sr
    const frames = Math.max(1, Math.ceil(decoded.duration * sr))
    const oac = new OfflineAudioContext(1, frames, sr)
    const node = oac.createBufferSource()
    node.buffer = decoded
    node.connect(oac.destination)
    node.start()
    const rendered = await oac.startRendering()
    if (token !== analysisToken) return
    const mono = rendered.getChannelData(0)

    // Int16 时域表（供 getByteTimeDomainData）
    const pcm = new Int16Array(mono.length)
    for (let i = 0; i < mono.length; i++) {
      const v = Math.max(-1, Math.min(1, mono[i]))
      pcm[i] = v < 0 ? v * 32768 : v * 32767
    }

    // 分帧 FFT → 幅度谱帧表（先存 Float32 幅度，全部帧完成后按全局峰值映射 dB）
    const N = SP.fftSize
    const hop = Math.max(1, Math.round(SP.hopSec * sr))
    const nFrames = Math.max(1, Math.floor((mono.length - N) / hop) + 1)
    const bins = N / 2
    const mags = new Float32Array(nFrames * bins)
    const fft = makeFFT(N)
    const win = new Float32Array(N)
    for (let i = 0; i < N; i++) win[i] = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / N)
    const re = new Float32Array(N)
    const im = new Float32Array(N)

    let maxMag = 1e-7
    let f = 0
    const CHUNK = 24   // 每批帧数（让出主线程，避免卡渲染）

    function batch () {
      if (token !== analysisToken) return
      const end = Math.min(f + CHUNK, nFrames)
      for (; f < end; f++) {
        const off = f * hop
        for (let i = 0; i < N; i++) {
          re[i] = (off + i < mono.length ? mono[off + i] : 0) * win[i]
          im[i] = 0
        }
        fft(re, im)
        const base = f * bins
        for (let b = 0; b < bins; b++) {
          const m = Math.sqrt(re[b] * re[b] + im[b] * im[b])
          mags[base + b] = m
          if (m > maxMag) maxMag = m
        }
      }
      if (f < nFrames) {
        setTimeout(batch, 0)
      } else {
        finish(mags, maxMag, pcm, sr, nFrames, bins, token, trackId)
      }
    }
    setTimeout(batch, 0)
  } catch (e) {
    if (token === analysisToken) {
      SP.analyzing = false
      logFn('spectrum analyze failed: ' + String((e && e.message) || e))
    }
  }
}

function finish (mags, maxMag, pcm, sr, nFrames, bins, token, trackId) {
  if (token !== analysisToken) return
  // dB 映射（仿 WebAudio AnalyserNode：-100..-30 dB → 0..255）
  const out = new Uint8Array(nFrames * bins)
  for (let i = 0; i < mags.length; i++) {
    const norm = mags[i] / maxMag
    const db = norm > 0 ? 20 * Math.log10(norm) : -120
    let v = (db + 100) / 70
    v = v < 0 ? 0 : (v > 1 ? 1 : v)
    out[i] = (v * 255) | 0
  }
  SP.frames = out
  SP.pcm = pcm
  SP.sr = sr
  SP.nFrames = nFrames
  SP.bins = bins
  SP.ready = true
  SP.analyzing = false
  logFn('spectrum ready: ' + nFrames + ' frames @' + sr + 'Hz')
}

// ---------- 查表填充 ----------
function fillFrequency (arr) {
  const t = clockSec()
  const fi = t / SP.hopSec
  const i0 = Math.floor(fi)
  if (i0 < 0 || i0 >= SP.nFrames) { arr.fill(0); return }
  const frac = fi - i0
  const i1 = Math.min(SP.nFrames - 1, i0 + 1)
  const bins = Math.min(arr.length, SP.bins)
  const a = SP.frames
  const b0 = i0 * SP.bins
  const b1 = i1 * SP.bins
  for (let b = 0; b < bins; b++) {
    arr[b] = a[b0 + b] + (a[b1 + b] - a[b0 + b]) * frac
  }
}

function fillTimeDomain (arr) {
  const t = clockSec()
  const n = arr.length
  const center = Math.round(t * SP.sr)
  const start = center - (n >> 1)
  const pcm = SP.pcm
  for (let i = 0; i < n; i++) {
    const idx = start + i
    const v = (idx >= 0 && idx < pcm.length) ? pcm[idx] / 32768 : 0
    arr[i] = Math.max(0, Math.min(255, 128 + Math.round(v * 127)))
  }
}

// ---------- AnalyserNode 原型 monkey-patch（引擎零改动） ----------
const origGetFreq = AnalyserNode.prototype.getByteFrequencyData
const origGetTime = AnalyserNode.prototype.getByteTimeDomainData

AnalyserNode.prototype.getByteFrequencyData = function (array) {
  if (SP.ready && SP.frames && array && array.length === SP.bins) {
    fillFrequency(array)
    return
  }
  origGetFreq.call(this, array)
}

AnalyserNode.prototype.getByteTimeDomainData = function (array) {
  if (SP.ready && SP.pcm && array && array.length === SP.fftSize) {
    fillTimeDomain(array)
    return
  }
  origGetTime.call(this, array)
}
