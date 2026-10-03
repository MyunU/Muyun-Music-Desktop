/*!
 * Mineradio 播放界面视觉引擎（移植版）
 *
 * 代码提取自 Mineradio 项目 public/index.html（GPL-3.0）：
 *   https://github.com/XxHuberrr/Mineradio
 * 以工厂函数形式封装，宿主（Sollin）通过 host 适配器提供 DOM 挂载点、
 * 音频元素、歌曲/歌词数据与 UI 回调。视觉子系统（封面粒子、舞台歌词、
 * 电影镜头、涟漪、浮空粒子、骷髅预设、离线节拍解析）保持原实现。
 *
 * 本文件按 GPL-3.0 授权分发。
 */
import * as THREE from 'three'
import { gsap } from 'gsap'
import MusicTempo from 'music-tempo'

const MUSIC_TEMPO_MODULE = MusicTempo

// vendored 代码里大量以 window.gsap / window.MusicTempo / window.THREE 引用全局，
// 这里把模块实例挂到 window（不覆盖宿主已有值）。
if (typeof window !== 'undefined') {
  if (!window.gsap) window.gsap = gsap
  if (!window.THREE) window.THREE = THREE
  if (!window.MusicTempo) window.MusicTempo = MusicTempo
}

/**
 * @param {{
 *   canvasContainer: HTMLElement,
 *   albumBg: HTMLElement,
 *   overlayRoot: HTMLElement,
 *   audio: HTMLAudioElement | null,
 *   toast?: (msg: string) => void,
 *   onFxChange?: () => void,
 *   onBeatChip?: (state: { visible: boolean, text: string }) => void,
 *   onImmersiveChange?: (on: boolean) => void,
 * }} host
 */
export function createMineradioEngine(host) {
'use strict';

if (!host || !host.canvasContainer || !host.albumBg) {
  throw new Error('[mineradio] host.canvasContainer and host.albumBg are required');
}

var __disposed = false;
var __listeners = [];
function __on(target, type, fn, opts) {
  target.addEventListener(type, fn, opts);
  __listeners.push([target, type, fn, opts]);
}
var __timers = [];
function __interval(fn, ms) {
  var id = setInterval(fn, ms);
  __timers.push(id);
  return id;
}
// 宿主 DOM 查询：优先在播放界面覆盖层内解析 id，避免与宿主应用冲突。
var __doc = document;
function byId(id) {
  if (host.overlayRoot) {
    var scoped = host.overlayRoot.querySelector('#' + id);
    if (scoped) return scoped;
  }
  return __doc.getElementById(id);
}
function __qsa(selector) {
  var root = host.overlayRoot || __doc;
  return root.querySelectorAll(selector);
}
function __cssRoot() {
  return host.overlayRoot || __doc.documentElement;
}
function __stateRoot() {
  return host.overlayRoot || __doc.body;
}
function __assetUrl(name) {
  var base = host.assetBase != null ? host.assetBase : '/mineradio/';
  try { return new URL(base + name, window.location.href).href; } catch (e) { return base + name; }
}

// ============================================================
//  Global State（Mineradio 原始全局状态的视觉子集）
// ============================================================
var audio = host.audio || null, audioCtx = null, source = null, analyser = null, beatAnalyser = null, gainNode = null, audioReady = false;
var FFT_SIZE = 2048;
var frequencyData = new Uint8Array(FFT_SIZE / 2);
var timeDomainData = new Uint8Array(FFT_SIZE);
var BEAT_FFT_SIZE = 2048;
var beatFrequencyData = new Uint8Array(BEAT_FFT_SIZE / 2);
var beatTimeDomainData = new Uint8Array(BEAT_FFT_SIZE);
var bass = 0, mid = 0, treble = 0, audioEnergy = 0, beatPulse = 0, prevEnergy = 0;
var lyricSunEnergy = 0, lyricSunTarget = 0, lyricSunHold = 0, lyricSunAvg = 0, lyricSunPeak = 0.55;
var smoothBass = 0, smoothMid = 0, smoothTreb = 0, smoothEnergy = 0;
var bassPeak = 0.12, midPeak = 0.10, treblePeak = 0.08, energyPeak = 0.10;
var beatOnsetFlag = false;        // beat 上升沿瞬时标志,每帧消费一次
var lastStrongDrop = 0;           // 用于 burst 预设的强 drop 时刻

var lyricsLines = [], lyricsVisible = false, lyricsHasNativeKaraoke = false, lyricsTimingSource = 'none';
var playQueue = [], currentIdx = -1, playing = false;
var volumeTween = null, trackSwitchToken = 0;

var LYRIC_LAYOUT_STORE_KEY = 'mineradio-lyric-layout-v1';
var VISUAL_PRESET_SCHEMA = 'skull-preset-v2';
var FREE_CAMERA_STORE_KEY = 'mineradio-free-camera-v1';
var CONTROLS_AUTO_HIDE_STORE_KEY = 'mineradio-controls-auto-hide-v1';
var LOCAL_BEATMAP_STORE_KEY = 'mineradio-local-beatmaps-v1';
var LOCAL_BEAT_PREF_STORE_KEY = 'mineradio-local-beatmap-prefs-v1';
var LOCAL_BEAT_COMBOS = ['', 'downbeat', 'push', 'drop', 'rebound', 'accent'];

var currentLocalSong = null;
var localBeatAnalysis = { song:null, audioUrl:'', mode:'mr', active:false, token:0 };
var queueViewTab = 'queue', playMode = 'loop', miniQueueOpen = false;
var desktopFullscreenActive = false;

var coverProcessToken = 0, aiDepthPipeline = null, aiDepthReady = false, aiDepthBusy = false, aiDepthFailUntil = 0;
var coverDepthCache = Object.create(null), coverDepthCacheKeys = [];
var aiDepthLastRunAt = 0, aiDepthMinGapMs = 18000;

// v7.2: 离线节拍预解析
var beatMapCache = {};       // { songId: { kicks: [t1, t2, ...], duration: ... } }
var currentBeatMap = null;   // 当前播放的歌的 beatMap
var beatMapNextIdx = 0;      // 下一个待触发的 kick index
var beatMapBusy = false;     // 正在分析中
var beatMapToken = 0;        // 取消旧分析
var beatAnalysisTimer = null;
var beatAnalysisStartedAt = 0;
var beatPrefetchTimer = null;
var beatPrefetchBusy = false;
var beatPrefetchToken = 0;
var beatPrefetchLastKey = '';
var BEAT_PREFETCH_LIMIT = 2;
var beatDiskCacheStatus = { checked:false, enabled:false, mode:'unknown', reason:'' };
var beatDiskCacheNoticeLogged = false;
var djBeatMapCache = {};
var currentDjBeatMap = null;
var djBeatMapNextIdx = 0;
var djBeatPulseNextIdx = 0;
var djBeatMapBusy = false;
var djBeatMapToken = 0;
var djBeatAnalysisTimer = null;
var beatAnalysisConfig = {
  delayMs: 1600,
  minPlaybackSec: 1.2,
  idleTimeout: 1400,
  skipMusicTempoWhilePlaying: false
};
var beatCam = {
  nextIdx: 0,
  events: [],
  punch: 0,
  lookahead: 0.075,
  lastTriggerAt: -10,
  lastRealtimeAt: -10,
  minInterval: 0.500,
  fallbackMinInterval: 0.320,
  realtimeMinInterval: 0.460,
  realtimeMergeWindow: 0.135,
  attack: 0.028,
  hold: 0.030,
  release: 0.185,
  thetaKick: 0,
  phiKick: 0,
  radiusKick: 0,
  rollKick: 0,
  prevAudioTime: -1,
  stats: { map: 0, live: 0, merged: 0, liveBlocked: 0 }
};
var liveCamAvg = 0, liveCamPeak = 0.28, liveCamLastRaw = 0;
var cinemaDynamics = { avg: 0, lowAvg: 0, peak: 0.30, scale: 0.82 };
var cinemaTrackProfile = {
  scale: 1.0,
  target: 1.0,
  nameHint: 1.0,
  frames: 0,
  energyAvg: 0,
  lowAvg: 0,
  vocalAvg: 0,
  melodyAvg: 0,
  punchPeak: 0.10,
  density: 0
};
var rtBeat = {
  subFast: 0, subSlow: 0, lowFast: 0, lowSlow: 0,
  bodyFast: 0, bodySlow: 0, vocalFast: 0, vocalSlow: 0, snapFast: 0, snapSlow: 0,
  prevSub: 0, prevLow: 0, prevBody: 0, prevVocal: 0, prevSnap: 0, prevRms: 0,
  onsetAvg: 0.012, onsetPeak: 0.060,
  subPeak: 0.14, lowPeak: 0.18, bodyPeak: 0.16, vocalPeak: 0.16, snapPeak: 0.14,
  lastHitAt: -10,
  tempoGap: 0,
  tempoConfidence: 0,
  beatCount: 0,
  primedFrames: 0,
  warmupUntil: 0,
  pulse: 0,
  score: 0,
  stats: { hits: 0, blocked: 0, assisted: 0, strong: 0, rejected: 0 }
};
var djMode = {
  active: false,
  songKey: '',
  startedAt: 0,
  lastNoticeAt: -100000,
  tempoGap: 0,
  tempoConfidence: 0,
  sectionEnergy: 0,
  sectionLow: 0,
  sectionChange: 0,
  visualPulse: 0,
  lastBeatAt: -10
};

// fx 状态: 预设 + 主滑块 + 开关 + 三态
var fxDefaults = {
  preset: 0,            // 0=emily cover, 1=tunnel, 2=orbit, 3=void, 4=vinyl, 5=wallpaper, 6=skull
  intensity: 0.85,
  cinemaShake: 0.5,
  depth: 1.0,
  coverResolution: 1.55,
  point: 1.0, speed: 1.0, twist: 0.0, color: 1.10, scatter: 0.0, bgFade: 0.20,
  bloomStrength: 0.62,
  lyricGlowStrength: 0.28,
  lyricScale: 1.0,
  lyricOffsetX: 0,
  lyricOffsetY: 0,
  lyricOffsetZ: 0,
  lyricTiltX: 0,
  lyricTiltY: 0,
  lyricColorMode: 'auto',
  lyricColor: '#a9b8c8',
  lyricHighlightMode: 'auto',
  lyricHighlightColor: '#fac900',
  lyricGlowLinked: true,
  lyricGlowColor: '#008aff',
  lyricFont: 'hei',
  lyricLetterSpacing: 0,
  lyricLineHeight: 1.0,
  lyricWeight: 900,
  visualTintMode: 'auto',
  visualTintColor: '#9db8cf',
  uiAccentColor: '#ffffff',
  homeAccentColor: '#ffffff',
  homeIconColor: '#ffffff',
  visualIconColor: '#ffffff',
  backgroundColorMode: 'cover',
  backgroundColor: '#000000',
  backgroundOpacity: 1,
  controlGlassChromaticOffset: 90,
  backgroundColorCustom: false,
  backgroundImage: '',
  backgroundMedia: null,
  desktopLyrics: false,
  desktopLyricsSize: 1.0,
  desktopLyricsOpacity: 0.92,
  desktopLyricsY: 0.76,
  desktopLyricsClickThrough: false,
  desktopLyricsCinema: true,
  desktopLyricsHighlight: false,
  desktopLyricsFps: 60,
  wallpaperMode: false,
  wallpaperOpacity: 1,
  floatLayer: false, cinema: true, edge: false, aiDepth: false, bloom: false, lyricGlow: true,
  lyricGlowBeat: true,
  lyricGlowParticles: false,
  lyricCameraLock: false,
  particleLyrics: true,    // v7.2: 粒子歌词
  backCover: false,        // 旧的封面背面粒子层关闭；浮空粒子层会跟随封面翻转
  shelf: 'off',
  shelfCameraMode: 'static',
  shelfPresence: 'always',
  shelfShowPodcasts: false,
  shelfMergeCollections: false,
  shelfSize: 1,
  shelfOffsetX: 0,
  shelfOffsetY: 0,
  shelfOffsetZ: 0,
  shelfAngleY: -15,
  shelfAngleYManual: false,
  shelfOpacity: 1,
  shelfBgOpacity: 0.90,
  shelfAccentColor: '#ffffff',
  performanceBackground: 'auto',
  performanceQuality: 'high',
  liveBackgroundKeep: false,
  cam: 'off',
};
var PACKAGED_DEFAULT_USER_FX_ARCHIVE_NAME = '默认测试';
var PACKAGED_DEFAULT_USER_FX_ARCHIVE_EXPORTED_AT = 1782276031784;
var PACKAGED_DEFAULT_USER_FX_ARCHIVE_SAVED_AT = 1782273019045;
var PACKAGED_DEFAULT_FX_SNAPSHOT = Object.freeze({
  visualPresetSchema: VISUAL_PRESET_SCHEMA,
  preset: 0,
  intensity: 0.85,
  cinemaShake: 0.5,
  depth: 1,
  coverResolution: 1.55,
  point: 1,
  speed: 1,
  twist: 0,
  color: 1.1,
  scatter: 0,
  bgFade: 0.2,
  bloomStrength: 0.62,
  lyricGlowStrength: 0.28,
  lyricScale: 1,
  lyricOffsetX: 0,
  lyricOffsetY: 0,
  lyricOffsetZ: 0,
  lyricTiltX: 0,
  lyricTiltY: 0,
  lyricCameraLock: false,
  lyricColorMode: 'auto',
  lyricColor: '#a9b8c8',
  lyricHighlightMode: 'auto',
  lyricHighlightColor: '#fac900',
  lyricGlowLinked: true,
  lyricGlowColor: '#008aff',
  lyricFont: 'hei',
  lyricLetterSpacing: 0,
  lyricLineHeight: 1,
  lyricWeight: 900,
  visualTintMode: 'auto',
  visualTintColor: '#9db8cf',
  uiAccentColor: '#ffffff',
  homeAccentColor: '#ffffff',
  homeIconColor: '#ffffff',
  visualIconColor: '#ffffff',
  backgroundColorMode: 'cover',
  backgroundColor: '#000000',
  backgroundOpacity: 1,
  controlGlassChromaticOffset: 90,
  backgroundColorCustom: false,
  floatLayer: false,
  cinema: true,
  edge: false,
  aiDepth: false,
  bloom: false,
  lyricGlow: true,
  lyricGlowBeat: true,
  lyricGlowParticles: false,
  desktopLyrics: false,
  desktopLyricsSize: 1,
  desktopLyricsOpacity: 0.92,
  desktopLyricsY: 0.76,
  desktopLyricsClickThrough: false,
  desktopLyricsCinema: true,
  desktopLyricsHighlight: false,
  desktopLyricsFps: 60,
  performanceBackground: 'auto',
  performanceQuality: 'high',
  liveBackgroundKeep: false,
  particleLyrics: true,
  backCover: false,
  shelf: 'off',
  shelfCameraMode: 'static',
  shelfPresence: 'always',
  shelfShowPodcasts: false,
  shelfMergeCollections: false,
  shelfSize: 1,
  shelfOffsetX: 0,
  shelfOffsetY: 0,
  shelfOffsetZ: 0,
  shelfAngleY: -15,
  shelfAngleYManual: false,
  shelfOpacity: 1,
  shelfBgOpacity: 0.9,
  shelfAccentColor: '#ffffff',
  cam: 'off'
});
var DEVELOPMENT_LOCKED_FX = {
  wallpaperMode: true
};
function isDevelopmentLockedFx(key) {
  return !!DEVELOPMENT_LOCKED_FX[key];
}
function normalizeDevelopmentLockedFxState() {
  if (!fx) return;
  fx.wallpaperMode = false;
}

// ---------- 小工具（原散落于宿主应用各处） ----------
function escHtml(s){ var d = document.createElement('div'); d.textContent = s; return d.innerHTML; }
function readBooleanPreference(key, fallback) {
  try {
    var raw = localStorage.getItem(key);
    if (raw == null) return !!fallback;
    return raw === '1';
  } catch (e) {
    return !!fallback;
  }
}
function saveBooleanPreference(key, on) {
  try { localStorage.setItem(key, on ? '1' : '0'); } catch (e) {}
}
function readHotkeySettings() { return {}; }
function pulseObjectValue(target, key, amount, duration) {
  if (!target) return;
  target[key] = Math.max(target[key] || 0, amount || 1);
  if (window.gsap) {
    window.gsap.killTweensOf(target, key);
    var vars = { duration: duration || 0.42, ease: 'power3.out' };
    vars[key] = 0;
    window.gsap.to(target, vars);
  } else {
    setTimeout(function(){ if (target) target[key] = 0; }, (duration || 0.42) * 1000);
  }
}

// ---------- 歌曲模型适配（Sollin Song → Mineradio song） ----------
function isInlineCoverSrc(url) {
  return /^(?:data|blob|file):/i.test(String(url || ''));
}
// Sollin Electron 关闭 webSecurity，封面可直接跨域加载，无需 Mineradio 的 /api/cover 代理。
function coverProxySrc(url) { return url || ''; }
function coverUrlWithSize(url, size) {
  if (!url || isInlineCoverSrc(url) || !/^https?:\/\//i.test(url)) return url || '';
  if (!size) return url;
  var param = 'param=' + size + 'y' + size;
  if (/[?&]param=\d+y\d+/i.test(url)) return url.replace(/([?&])param=\d+y\d+/i, '$1' + param);
  return url + (url.indexOf('?') >= 0 ? '&' : '?') + param;
}
function getCustomCoverForSong() { return ''; }
function hydrateCustomCover(song) { return song; }
function songCoverSrc(song, size) {
  return song && song.cover ? coverUrlWithSize(song.cover, size) : '';
}
function cssImageUrl(url) {
  return String(url || '').replace(/\\/g, '\\\\').replace(/"/g, '%22');
}
function songProviderKey(song) {
  if (song && (song.provider === 'qq' || song.source === 'qq' || song.type === 'qq')) return 'qq';
  return 'netease';
}
function currentCoverSong() {
  if (currentIdx >= 0 && playQueue[currentIdx]) return playQueue[currentIdx];
  return currentLocalSong || null;
}
function currentLyricSong() { return currentCoverSong(); }
function currentLyricFallbackText() {
  var song = currentLyricSong() || {};
  var title = String(song.name || '').trim();
  var artist = String(song.artist || '').trim();
  if (!title) return '';
  return artist ? title + ' - ' + artist : title;
}
var playbackQuality = 'exhigh';
var qqPlaybackQualityCeiling = '';
function normalizePlaybackQuality(value) { return value || 'exhigh'; }
function hasProviderSvip() { return false; }
var loginStatus = { loggedIn: false, vipType: 0, vipLevel: 'none', isVip: false, isSvip: false, vipLabel: '无VIP' };

// ---------- 鼠标 / 摄像头视差 ----------
var pointerParallax = { x:0, y:0 };
var pointerTarget = { x:0, y:0 };
var headParallax = { x:0, y:0, active:false };
var headNeutral = null;

var desktopRuntimeState = {
  desktop: false,
  minimized: false,
  visible: true,
  focused: true,
  fullscreen: false
};
var renderPowerState = { mode: '', width: 0, height: 0, pixelRatio: 0 };
var backgroundCacheTrimTimer = 0;
var runtimePerfState = {
  lastCacheTrimAt: 0,
  cacheTrimCount: 0,
  lastCacheTrimReason: '',
  lastHeapSampleAt: 0,
  heapMB: 0,
  cacheCounts: {}
};

// ---------- 面板/歌单架/杂项桩（对应未移植的宿主子系统） ----------
var shelfManager = null;
var shelfPinnedOpen = false;
var shelfVisibility = { mode: 'off', visible: false };
var shelfHoverCue = { active: false };
var shelfLayoutProfile = { compact: false };
var controlsAutoHide = false;
var controlsHovering = false;
var controlsHideTimer = null;
var controlsHandleDimTimer = null;
var controlsLastMoveAt = 0;
var controlsShelfSuppressUntil = 0;
var cursorHideTimer = null;
var CURSOR_HIDE_DELAY = 2500;
var fxPanelPinned = false;
var immersiveMode = false;
var diyPlayerMode = true;
var startupVisualPreviewActive = false;
var playbackVisualPreset = 0;
var coverCropState = null, coverCropBound = false;
var peekTimers = {};
var PEEK_HIDE_DELAY = 170;
var localBeatMapCache = {};
var localBeatMapPrefs = {};
var playlistCoverCache = {};
var playlistPanelDetailState = null;
var USER_CAPSULE_AUTO_HIDE_STORE_KEY = 'mineradio-user-capsule-auto-hide-v1';
var PLAYLIST_PANEL_PIN_STORE_KEY = 'mineradio-playlist-panel-pinned-v1';
var FX_FAB_AUTO_HIDE_STORE_KEY = 'mineradio-fx-fab-auto-hide-v1';

// ▼▼▼ [mineradio:dj-helpers] source lines 3131,3193 ▼▼▼
function isPodcastSong(song) {
  return !!(song && song.type === 'podcast');
}

function djSongKey(song) {
  if (!song) return '';
  if (song.localKey) return 'local:' + song.localKey;
  return 'podcast:' + (song.programId || song.id || song.name || '');
}

function resetDjModeMeter() {
  djMode.tempoGap = 0;
  djMode.tempoConfidence = 0;
  djMode.sectionEnergy = 0;
  djMode.sectionLow = 0;
  djMode.sectionChange = 0;
  djMode.visualPulse = 0;
  djMode.lastBeatAt = -10;
}

function resetDjBeatMapState() {
  currentDjBeatMap = null;
  djBeatMapNextIdx = 0;
  djBeatPulseNextIdx = 0;
}

function cancelDjBeatAnalysisTimer() {
  if (djBeatAnalysisTimer) {
    clearTimeout(djBeatAnalysisTimer);
    djBeatAnalysisTimer = null;
  }
}

function setDjModeActive(active, song) {
  active = !!active;
  var key = active ? djSongKey(song) : '';
  var changed = djMode.active !== active || djMode.songKey !== key;
  djMode.active = active;
  djMode.songKey = key;
  if (changed) {
    djMode.startedAt = performance.now();
    resetDjModeMeter();
  }
  if (active) {
    currentBeatMap = null;
    beatMapNextIdx = 0;
    cancelBeatAnalysisTimer();
    hideBeatChip();
  } else {
    djBeatMapToken++;
    cancelDjBeatAnalysisTimer();
    resetDjBeatMapState();
  }
}

function maybeAnnounceDjMode() {
  if (!djMode.active) return;
  var now = performance.now();
  if (now - djMode.lastNoticeAt > 8000) {
    djMode.lastNoticeAt = now;
    showToast('DJ Mode · 离线锁拍');
  }
}

// ▼▼▼ [mineradio:fx-snapshot-perf] source lines 3352,3714 ▼▼▼
function clonePackagedDefaultFxSnapshot() {
  return Object.assign({}, PACKAGED_DEFAULT_FX_SNAPSHOT);
}
function packagedDefaultLyricLayoutRaw() {
  return Object.assign({ desktopLyricsSchema: 'desktop-lyrics-v3' }, clonePackagedDefaultFxSnapshot());
}
var DEVELOPMENT_LOCKED_FX = {
  wallpaperMode: true
};
function isDevelopmentLockedFx(key) {
  return !!DEVELOPMENT_LOCKED_FX[key];
}
function normalizeDevelopmentLockedFxState() {
  if (!fx) return;
  fx.wallpaperMode = false;
}
function readSavedPlaybackVisualPreset() {
  try {
    var raw = JSON.parse(localStorage.getItem(LYRIC_LAYOUT_STORE_KEY) || '{}') || {};
    if (!Object.prototype.hasOwnProperty.call(raw, 'preset')) return fxDefaults.preset;
    var savedPreset = clampRange(Number(raw.preset) || 0, 0, 6);
    if (savedPreset === 3 && raw.visualPresetSchema !== VISUAL_PRESET_SCHEMA) savedPreset = 5;
    return savedPreset;
  } catch (e) {
    return fxDefaults.preset;
  }
}
var playbackVisualPreset = readSavedPlaybackVisualPreset();
var startupVisualPreviewActive = false;
var fx = Object.assign({}, fxDefaults, readSavedLyricLayout());
normalizeDevelopmentLockedFxState();
var presetTransition = { active:false, start:-10, duration:0.92, from:0, to:0 };
var controlsAutoHide = readBooleanPreference(CONTROLS_AUTO_HIDE_STORE_KEY, false);
var controlsHovering = false;
var controlsHideTimer = null;
var controlsHandleDimTimer = null;
var controlsLastMoveAt = 0;
var controlsShelfSuppressUntil = 0;
var cursorHideTimer = null;
var CURSOR_HIDE_DELAY = 2500;
var fxPanelPinned = false;
var playlistPanelPinned = readBooleanPreference(PLAYLIST_PANEL_PIN_STORE_KEY, false);
var userCapsuleAutoHide = readBooleanPreference(USER_CAPSULE_AUTO_HIDE_STORE_KEY, false);
var fxFabAutoHide = readBooleanPreference(FX_FAB_AUTO_HIDE_STORE_KEY, false);
var fxFabAutoHideRevealArmed = true;
var hotkeySettings = readHotkeySettings();
var immersiveMode = false;
var immersiveState = {
  shelfMode: null,
  shelfPinnedOpen: false,
  lyrics: true,
  controlsAutoHide: true,
  bottomVisible: false
};

// 鼠标 / 摄像头视差
var pointerParallax = { x:0, y:0 };
var pointerTarget = { x:0, y:0 };
var headParallax = { x:0, y:0, active:false };
var headNeutral = null;

function pulseObjectValue(target, key, amount, duration) {
  if (!target) return;
  target[key] = Math.max(target[key] || 0, amount || 1);
  if (window.gsap) {
    window.gsap.killTweensOf(target, key);
    var vars = { duration: duration || 0.42, ease: 'power3.out' };
    vars[key] = 0;
    window.gsap.to(target, vars);
  } else {
    setTimeout(function(){ if (target) target[key] = 0; }, (duration || 0.42) * 1000);
  }
}

var desktopRuntimeState = {
  desktop: !!window.desktopWindow,
  minimized: false,
  visible: true,
  focused: true,
  fullscreen: false
};
var renderPowerState = { mode: '', width: 0, height: 0, pixelRatio: 0 };
var backgroundCacheTrimTimer = 0;
var runtimePerfState = {
  lastCacheTrimAt: 0,
  cacheTrimCount: 0,
  lastCacheTrimReason: '',
  lastHeapSampleAt: 0,
  heapMB: 0,
  cacheCounts: {}
};
function isDeepBackgroundMode() {
  if (isLiveBackgroundKeepMode()) return false;
  return !!(document.hidden || desktopRuntimeState.minimized || desktopRuntimeState.visible === false);
}
function currentPerformanceBackgroundMode() {
  return normalizePerformanceBackgroundMode(fx && fx.performanceBackground, fx && fx.liveBackgroundKeep === true);
}
function isLiveBackgroundKeepMode() {
  return currentPerformanceBackgroundMode() === 'keep';
}
function isBackgroundReleaseMode() {
  return currentPerformanceBackgroundMode() === 'release';
}
function isHiddenForBackgroundOptimization() {
  return !!(document.hidden && !isLiveBackgroundKeepMode());
}
function isVisibleBackgroundMode() {
  return false;
}
function updateRenderPowerClasses() {
  __stateRoot().classList.toggle('render-deep-sleep', isDeepBackgroundMode());
  __stateRoot().classList.toggle('render-background-eco', isVisibleBackgroundMode());
}
function safeObjectKeys(obj) {
  try { return obj ? Object.keys(obj) : []; } catch (e) { return []; }
}
function markProtectedKey(map, key) {
  if (key) map[String(key)] = true;
}
function collectProtectedCoverUrls() {
  var keep = Object.create(null);
  function mark(url) { if (url) keep[String(url)] = true; }
  try {
    var song = (typeof currentCoverSong === 'function') ? currentCoverSong() : (playQueue && currentIdx >= 0 ? playQueue[currentIdx] : null);
    if (song) {
      mark(song.cover);
      if (typeof songCoverSrc === 'function') {
        mark(songCoverSrc(song, 60));
        mark(songCoverSrc(song, 360));
        mark(songCoverSrc(song, 400));
      }
    }
    if (typeof currentCoverSource !== 'undefined' && currentCoverSource && currentCoverSource.src) mark(currentCoverSource.src);
    if (typeof playlistPanelDetailState !== 'undefined' && playlistPanelDetailState && playlistPanelDetailState.playlist) {
      var cover = playlistPanelDetailState.playlist.cover;
      mark(cover);
      if (typeof coverUrlWithSize === 'function') {
        mark(coverUrlWithSize(cover, 88));
        mark(coverUrlWithSize(cover, 96));
      }
    }
    if (shelfManager && shelfManager.getCards) {
      shelfManager.getCards().forEach(function(card){
        if (card && card.item) mark(card.item.cover);
      });
    }
  } catch (e) {}
  return keep;
}
function collectProtectedBeatMapKeys() {
  var keep = Object.create(null);
  try {
    if (typeof beatMapSongKey === 'function' && playQueue && playQueue.length) {
      var start = Math.max(0, currentIdx - 5);
      var end = Math.min(playQueue.length - 1, currentIdx + 5);
      for (var i = start; i <= end; i++) markProtectedKey(keep, beatMapSongKey(playQueue[i]));
    }
    if (typeof beatPrefetchLastKey !== 'undefined') markProtectedKey(keep, beatPrefetchLastKey);
    if (typeof djMode !== 'undefined' && djMode && djMode.songKey) markProtectedKey(keep, djMode.songKey);
    if (typeof localBeatAnalysis !== 'undefined' && localBeatAnalysis && localBeatAnalysis.song && typeof beatMapSongKey === 'function') {
      markProtectedKey(keep, beatMapSongKey(localBeatAnalysis.song));
    }
  } catch (e) {}
  return keep;
}
function collectProtectedCoverDepthIds() {
  var keep = Object.create(null);
  try {
    if (typeof coverDepthCacheId !== 'function') return keep;
    var candidates = [];
    if (typeof currentCoverSource !== 'undefined' && currentCoverSource && currentCoverSource.src) candidates.push(currentCoverSource.src);
    var song = (typeof currentCoverSong === 'function') ? currentCoverSong() : null;
    if (song && typeof songCoverSrc === 'function') {
      candidates.push(songCoverSrc(song, 360));
      candidates.push(songCoverSrc(song, 400));
    }
    var texImg = (typeof coverTex !== 'undefined' && coverTex && coverTex.image) ? coverTex.image : null;
    var w = texImg && texImg.width ? texImg.width : 0;
    var h = texImg && texImg.height ? texImg.height : 0;
    candidates.forEach(function(src){
      if (src) markProtectedKey(keep, coverDepthCacheId(src + '|tex=' + w + 'x' + h));
    });
  } catch (e) {}
  return keep;
}
function trimObjectCache(cache, keep, protectedKeys, skipRecord) {
  var keys = safeObjectKeys(cache);
  if (!cache || keys.length <= keep) return 0;
  var drop = keys.length - keep;
  var dropped = 0;
  for (var i = 0; i < keys.length && drop > 0; i++) {
    var key = keys[i];
    if (protectedKeys && protectedKeys[key]) continue;
    var rec = cache[key];
    if (skipRecord && skipRecord(rec, key)) continue;
    delete cache[key];
    drop--;
    dropped++;
  }
  return dropped;
}
function trimCoverDepthCache(keep, protectedKeys) {
  if (!coverDepthCache || !coverDepthCacheKeys) return 0;
  var keys = coverDepthCacheKeys.filter(function(key){ return !!coverDepthCache[key]; });
  if (keys.length <= keep) {
    coverDepthCacheKeys = keys;
    return 0;
  }
  var keepSet = Object.create(null);
  var count = 0;
  for (var i = keys.length - 1; i >= 0 && count < keep; i--) {
    keepSet[keys[i]] = true;
    count++;
  }
  Object.keys(protectedKeys || {}).forEach(function(key){ keepSet[key] = true; });
  var dropped = 0;
  keys.forEach(function(key){
    if (keepSet[key]) return;
    delete coverDepthCache[key];
    dropped++;
  });
  coverDepthCacheKeys = keys.filter(function(key){ return !!coverDepthCache[key]; });
  return dropped;
}
function collectRuntimePerfSnapshot(now) {
  now = now || performance.now();
  runtimePerfState.cacheCounts = {
    playlistCovers: safeObjectKeys(playlistCoverCache).length,
    coverDepth: coverDepthCacheKeys ? coverDepthCacheKeys.length : 0,
    beatMaps: safeObjectKeys(beatMapCache).length,
    djBeatMaps: safeObjectKeys(djBeatMapCache).length
  };
  if (performance && performance.memory && now - runtimePerfState.lastHeapSampleAt > 12000) {
    runtimePerfState.lastHeapSampleAt = now;
    runtimePerfState.heapMB = Math.round((performance.memory.usedJSHeapSize || 0) / 1048576);
  }
  return {
    render: (typeof renderPerfState !== 'undefined') ? {
      mode: renderPerfState.mode,
      fps: renderPerfState.fps,
      skipped: renderPerfState.skipped,
      longFrames: renderPerfState.longFrames
    } : null,
    runtime: runtimePerfState,
    renderer: (typeof renderer !== 'undefined' && renderer && renderer.info) ? {
      geometries: renderer.info.memory && renderer.info.memory.geometries,
      textures: renderer.info.memory && renderer.info.memory.textures,
      calls: renderer.info.render && renderer.info.render.calls,
      triangles: renderer.info.render && renderer.info.render.triangles
    } : null,
    viewport: (typeof renderer !== 'undefined' && renderer && renderer.domElement) ? {
      width: innerWidth,
      height: innerHeight,
      devicePixelRatio: window.devicePixelRatio || 1,
      renderPixelRatio: renderer.getPixelRatio ? Number(renderer.getPixelRatio().toFixed(3)) : 0,
      canvasWidth: renderer.domElement.width || 0,
      canvasHeight: renderer.domElement.height || 0,
      renderPixels: (renderer.domElement.width || 0) * (renderer.domElement.height || 0),
      targetFps: (typeof getAdaptiveRenderFps === 'function') ? getAdaptiveRenderFps() : 0,
      interactionBoost: (typeof isRenderInteractionActive === 'function') ? isRenderInteractionActive() : false,
      interactionReason: (typeof renderInteractionReason !== 'undefined') ? renderInteractionReason : ''
    } : null,
    deepSleep: isDeepBackgroundMode()
  };
}
window.__mineradioPerfSnapshot = collectRuntimePerfSnapshot;
function trimRuntimeCaches(reason, aggressive) {
  var protectedCovers = collectProtectedCoverUrls();
  var protectedBeats = collectProtectedBeatMapKeys();
  var dropped = 0;
  dropped += trimObjectCache(playlistCoverCache, aggressive ? 72 : 180, protectedCovers, function(rec){
    return rec && rec.loading;
  });
  dropped += trimCoverDepthCache(aggressive ? 4 : 10, collectProtectedCoverDepthIds());
  dropped += trimObjectCache(beatMapCache, aggressive ? 12 : 36, protectedBeats);
  dropped += trimObjectCache(djBeatMapCache, aggressive ? 4 : 12, protectedBeats);
  if (aggressive && typeof renderer !== 'undefined' && renderer && renderer.renderLists && renderer.renderLists.dispose) {
    try { renderer.renderLists.dispose(); } catch (e) {}
  }
  runtimePerfState.lastCacheTrimAt = performance.now();
  runtimePerfState.cacheTrimCount += 1;
  runtimePerfState.lastCacheTrimReason = reason || (aggressive ? 'deep' : 'active');
  collectRuntimePerfSnapshot(runtimePerfState.lastCacheTrimAt);
  return dropped;
}
function trimVisualCachesForBackground() {
  if (!isDeepBackgroundMode()) return;
  trimRuntimeCaches('deep-background', true);
}
function scheduleBackgroundCacheTrim() {
  if (!isDeepBackgroundMode()) return;
  if (backgroundCacheTrimTimer) clearTimeout(backgroundCacheTrimTimer);
  backgroundCacheTrimTimer = setTimeout(function(){
    backgroundCacheTrimTimer = 0;
    trimVisualCachesForBackground();
  }, 900);
}
function maybeTrimRuntimeCaches(now) {
  now = now || performance.now();
  var deep = isDeepBackgroundMode();
  var gap = deep ? (isBackgroundReleaseMode() ? 3600 : 7000) : 45000;
  if (!deep && now < 30000) return;
  if (now - runtimePerfState.lastCacheTrimAt < gap) return;
  trimRuntimeCaches(deep ? (isBackgroundReleaseMode() ? 'release-frame' : 'deep-frame') : 'active-frame', deep);
}
function applyRendererPowerMode() {
  if (typeof renderer === 'undefined' || !renderer) return;
  var deep = isDeepBackgroundMode();
  var width = deep ? 4 : Math.max(1, innerWidth);
  var height = deep ? 4 : Math.max(1, innerHeight);
  var pixelRatio = getRenderPixelRatio();
  var mode = deep ? 'sleep' : 'active';
  if (renderPowerState.mode === mode && renderPowerState.width === width && renderPowerState.height === height && Math.abs(renderPowerState.pixelRatio - pixelRatio) < 0.001) return;
  renderPowerState = { mode: mode, width: width, height: height, pixelRatio: pixelRatio };
  renderer.setPixelRatio(pixelRatio);
  renderer.setSize(width, height, false);
  if (typeof uniforms !== 'undefined' && uniforms && uniforms.uPixel) uniforms.uPixel.value = renderer.getPixelRatio();
  if (deep) {
    if (renderer.renderLists && renderer.renderLists.dispose) renderer.renderLists.dispose();
    scheduleBackgroundCacheTrim();
  }
}
function updateDesktopRuntimeState(state) {
  state = state || {};
  var wasFullscreen = desktopRuntimeState.fullscreen;
  var wasDeep = isDeepBackgroundMode();
  desktopRuntimeState.desktop = !!window.desktopWindow;
  desktopRuntimeState.minimized = !!state.isMinimized;
  desktopRuntimeState.visible = state.isVisible !== false;
  desktopRuntimeState.focused = state.isFocused !== false;
  desktopRuntimeState.fullscreen = !!(state.isFullScreen || state.isNativeFullScreen || state.isHtmlFullScreen || state.isWindowFullScreen);
  updateRenderPowerClasses();
  applyRendererPowerMode();
  if (fx && (fx.desktopLyrics || fx.wallpaperMode)) setTimeout(syncDesktopOverlayState, 0);
  if (wasDeep && !isDeepBackgroundMode()) recoverVisualsAfterBackground('desktop-runtime-state');
  if (desktopRuntimeState.fullscreen !== wasFullscreen) scheduleMainRendererViewportRefresh('desktop-runtime-state');
}
function installRenderPowerHooks() {
  updateRenderPowerClasses();
  __on(document, 'visibilitychange', function(){
    updateRenderPowerClasses();
    applyRendererPowerMode();
    if (!isDeepBackgroundMode()) recoverVisualsAfterBackground('visibilitychange');
  });
  __on(window, 'focus', function(){
    desktopRuntimeState.focused = true;
    updateRenderPowerClasses();
    applyRendererPowerMode();
    if (!isDeepBackgroundMode()) recoverVisualsAfterBackground('focus');
  });
  __on(window, 'blur', function(){
    desktopRuntimeState.focused = false;
    updateRenderPowerClasses();
    applyRendererPowerMode();
  });
  if (window.desktopWindow && typeof window.desktopWindow.onStateChange === 'function') {
    window.desktopWindow.onStateChange(updateDesktopRuntimeState);
    if (typeof window.desktopWindow.getState === 'function') {
      window.desktopWindow.getState().then(updateDesktopRuntimeState).catch(function(){});
    }
  }
}

// ▼▼▼ [mineradio:scene] source lines 3716,3783 ▼▼▼
// ============================================================
//  Three.js 场景
// ============================================================
var scene = new THREE.Scene();
scene.background = null;
var camera = new THREE.PerspectiveCamera(45, innerWidth / innerHeight, 0.1, 100);
var RENDER_DPR_CAP = 1.35;
var RENDER_PIXEL_BUDGET = 5200000;
var RENDER_MIN_DPR = 0.72;
// 0 = display vsync. Keep visible playback high-refresh capable instead of capping 120Hz+ screens to 60/72.
var RENDER_VISIBLE_VSYNC = true;
var RENDER_ACTIVE_FPS = 0;
var RENDER_LARGE_FPS = 0;
var RENDER_HUGE_FPS = 0;
var RENDER_INTERACTION_FPS = 0;
var RENDER_INTERACTION_LARGE_FPS = 0;
var RENDER_INTERACTION_HUGE_FPS = 0;
var RENDER_INTERACTION_HOLD_MS = 900;
var renderInteractionBoostUntil = 0;
var renderInteractionReason = '';
function renderQualityProfile() {
  var quality = normalizePerformanceQuality(fx && fx.performanceQuality);
  if (quality === 'eco') return { cap: 0.95, min: 0.56, budget: 2400000 };
  if (quality === 'balanced') return { cap: 1.12, min: 0.66, budget: 3800000 };
  if (quality === 'ultra') return { cap: 1.75, min: 0.85, budget: 7800000 };
  return { cap: RENDER_DPR_CAP, min: RENDER_MIN_DPR, budget: RENDER_PIXEL_BUDGET };
}
function getRenderPixelRatio() {
  var device = window.devicePixelRatio || 1;
  if (isDeepBackgroundMode()) return Math.min(device, 0.30);
  var cssPixels = Math.max(1, innerWidth * innerHeight);
  var quality = renderQualityProfile();
  var budgetCap = Math.sqrt(quality.budget / cssPixels);
  var cap = Math.min(quality.cap, budgetCap);
  return Math.max(quality.min, Math.min(device, cap));
}
function getRenderPixelLoad() {
  var ratio = getRenderPixelRatio();
  return Math.max(1, innerWidth * innerHeight) * ratio * ratio;
}
function markRenderInteraction(reason, holdMs) {
  if (isDeepBackgroundMode()) return;
  var now = performance.now();
  renderInteractionBoostUntil = Math.max(renderInteractionBoostUntil, now + (holdMs || RENDER_INTERACTION_HOLD_MS));
  renderInteractionReason = reason || renderInteractionReason || 'interaction';
  if (typeof renderPerfState !== 'undefined' && renderPerfState) renderPerfState.lastRenderAt = 0;
}
function isRenderInteractionActive(now) {
  return (now || performance.now()) < renderInteractionBoostUntil;
}
function getRenderLoadTier() {
  var cssPixels = Math.max(1, innerWidth * innerHeight);
  var renderPixels = (typeof getRenderPixelLoad === 'function') ? getRenderPixelLoad() : cssPixels;
  if (cssPixels >= 7200000 || renderPixels >= 5000000) return 2;
  if (cssPixels >= 3200000 || renderPixels >= 3600000) return 1;
  return 0;
}
var renderer = new THREE.WebGLRenderer({ antialias: false, alpha: true, powerPreference: 'high-performance' });
renderer.setClearColor(0x000000, 0);
renderer.setPixelRatio(getRenderPixelRatio());
renderer.setSize(innerWidth, innerHeight);
renderer.domElement.style.background = 'transparent';
renderer.domElement.style.display = 'block';
renderer.domElement.style.width = '100%';
renderer.domElement.style.height = '100%';
renderer.domElement.tabIndex = 0;
byId('canvas-container').appendChild(renderer.domElement);


// ▼▼▼ [mineradio:camera] source lines 3784,5494 ▼▼▼
// ============================================================
//  相机系统 v7.1 — 分离 user offset / cinema offset
//   - userOrbit: 用户拖拽的目标 (永久保留, 不会被电影模式覆盖)
//   - cinemaOffset: 电影模式的微偏移 (始终叠加, 即使用户在拖)
//   - 最终 theta = userOrbit.theta + cinemaOffset.theta
//   - 回正按钮 / 双击屏幕: 让 userOrbit 缓慢归零
// ============================================================
var orbit = {
  userTheta: 0.0, userPhi: 0.08, userRadius: 6.6,
  cineTheta: 0.0, cinePhi: 0.0, cineRadius: 0.0,
  theta: 0.0, phi: 0.08, radius: 6.6,
  minPhi: -Math.PI*0.45, maxPhi: Math.PI*0.45,
  minRadius: 2.4, maxRadius: 14.0,
  baselineTheta: 0.0, baselinePhi: 0.08, baselineRadius: 6.6,
  rotating: false, last:{x:0,y:0},
  recentering: false,
  centerLocked: false,
  // v8: 镜头跟拍 (hover shelf / queue 时)
  lookAt: new THREE.Vector3(0,0,0),
  focus: {
    active: false,
    type: null,        // 'shelf-side' | 'shelf-stage' | 'queue'
    theta: 0.0, phi: 0.08, radius: 6.6,
    lookAt: new THREE.Vector3(0,0,0),
  },
  glowFollowX: 0,
  glowFollowY: 0,
  glowFollowRoll: 0,
  beatGlow: 0,
};
var ZERO_VEC = new THREE.Vector3(0,0,0);
var BASE_FOV = 45;
var camPunch = 0;
var cinemaT = 0;
function defaultFreeCameraState() {
  return {
    active: false,
    locked: false,
    position: new THREE.Vector3(0, 0, 6.6),
    yaw: 0,
    pitch: 0,
    roll: 0,
    fov: BASE_FOV,
    velocity: new THREE.Vector3(),
    keys: {},
    resetTween: null
  };
}
function readFreeCameraState() {
  var state = defaultFreeCameraState();
  try {
    var raw = JSON.parse(localStorage.getItem(FREE_CAMERA_STORE_KEY) || '{}') || {};
    if (raw.position) {
      state.position.set(
        clampRange(Number(raw.position.x) || 0, -80, 80),
        clampRange(Number(raw.position.y) || 0, -80, 80),
        clampRange(Number(raw.position.z) || 6.6, -80, 80)
      );
    }
    state.yaw = clampRange(Number(raw.yaw) || 0, -Math.PI * 8, Math.PI * 8);
    state.pitch = clampRange(Number(raw.pitch) || 0, -Math.PI * 0.49, Math.PI * 0.49);
    state.roll = clampRange(Number(raw.roll) || 0, -Math.PI, Math.PI);
    state.fov = clampRange(Number(raw.fov) || BASE_FOV, 26, 72);
    state.locked = !!(raw.locked || raw.active);
    state.active = false;
  } catch (e) {}
  return state;
}
var freeCamera = readFreeCameraState();
var FREE_CAMERA_MOVE = new THREE.Vector3();
var FREE_CAMERA_TARGET_VEL = new THREE.Vector3();
var FREE_CAMERA_SHAKE_DIR = new THREE.Vector3();
var FREE_CAMERA_EULER = new THREE.Euler(0, 0, 0, 'YXZ');
var FREE_CAMERA_RESET_MAT = new THREE.Matrix4();
var FREE_CAMERA_RESET_QUAT = new THREE.Quaternion();
var FREE_CAMERA_UP = new THREE.Vector3(0, 1, 0);
var freeCameraPointer = { seen: false, x: 0, y: 0 };
var freeCameraDeferredSaveTimer = 0;
function saveFreeCameraState() {
  if (!freeCamera) return;
  try {
    localStorage.setItem(FREE_CAMERA_STORE_KEY, JSON.stringify({
      locked: !!freeCamera.locked,
      active: !!freeCamera.active,
      position: { x: freeCamera.position.x, y: freeCamera.position.y, z: freeCamera.position.z },
      yaw: freeCamera.yaw,
      pitch: freeCamera.pitch,
      roll: freeCamera.roll,
      fov: freeCamera.fov
    }));
  } catch (e) {}
}
function scheduleFreeCameraStateSave(delay) {
  if (freeCameraDeferredSaveTimer) return;
  freeCameraDeferredSaveTimer = setTimeout(function(){
    freeCameraDeferredSaveTimer = 0;
    saveFreeCameraState();
  }, delay || 720);
}
function easeOutCubic01(t) {
  t = clamp01(t);
  return 1 - Math.pow(1 - t, 3);
}
function shortestAngleDelta(from, to) {
  return Math.atan2(Math.sin(to - from), Math.cos(to - from));
}
function getDefaultFreeCameraResetPose() {
  var pose = {
    position: new THREE.Vector3(0, 0, 6.6),
    yaw: 0,
    pitch: 0,
    roll: 0,
    fov: BASE_FOV
  };
  if (typeof SKULL_PRESET_INDEX !== 'undefined' && fx && fx.preset === SKULL_PRESET_INDEX && typeof setSkullCameraTargetVectors === 'function') {
    var look = new THREE.Vector3();
    var shelfComposition = typeof isSkullShelfCompositionActive === 'function' && isSkullShelfCompositionActive();
    setSkullCameraTargetVectors(pose.position, look, innerHeight > innerWidth * 1.08, shelfComposition, 0);
    FREE_CAMERA_RESET_MAT.lookAt(pose.position, look, FREE_CAMERA_UP);
    FREE_CAMERA_RESET_QUAT.setFromRotationMatrix(FREE_CAMERA_RESET_MAT);
    FREE_CAMERA_EULER.setFromQuaternion(FREE_CAMERA_RESET_QUAT, 'YXZ');
    pose.pitch = FREE_CAMERA_EULER.x;
    pose.yaw = FREE_CAMERA_EULER.y;
    pose.roll = FREE_CAMERA_EULER.z;
  }
  return pose;
}
function captureFreeCameraFromCurrent() {
  if (!freeCamera) freeCamera = defaultFreeCameraState();
  camera.updateMatrixWorld(true);
  freeCamera.position.copy(camera.position);
  FREE_CAMERA_EULER.setFromQuaternion(camera.quaternion, 'YXZ');
  freeCamera.pitch = FREE_CAMERA_EULER.x;
  freeCamera.yaw = FREE_CAMERA_EULER.y;
  freeCamera.roll = FREE_CAMERA_EULER.z;
  freeCamera.fov = clampRange(camera.fov || BASE_FOV, 26, 72);
}
function applyFreeCameraToCamera() {
  if (!freeCamera || !(freeCamera.active || freeCamera.locked)) return false;
  var cameraShake = clampRange(Number(fx.cinemaShake) || 0, 0, 1.8);
  camera.position.copy(freeCamera.position);
  camera.rotation.order = 'YXZ';
  camera.rotation.set(
    freeCamera.pitch + beatCam.phiKick * cameraShake * 0.45,
    freeCamera.yaw + beatCam.thetaKick * cameraShake * 0.45,
    freeCamera.roll + beatCam.rollKick * cameraShake
  );
  if (cameraShake > 0 && Math.abs(beatCam.radiusKick) > 0.0001) {
    FREE_CAMERA_SHAKE_DIR.set(0, 0, -1).applyEuler(camera.rotation);
    camera.position.addScaledVector(FREE_CAMERA_SHAKE_DIR, beatCam.radiusKick * cameraShake * 0.52);
  }
  var cameraPunch = Math.max(camPunch * 0.55, beatCam.punch * 0.54 + beatCam.radiusKick * 0.16) * cameraShake;
  var targetFov = clampRange(freeCamera.fov || BASE_FOV, 26, 72) - cameraPunch * 1.75;
  camera.fov += (targetFov - camera.fov) * (targetFov < camera.fov ? 0.24 : 0.12);
  camera.updateProjectionMatrix();
  camPunch *= 0.86;
  return true;
}
function updateFreeCameraHint() {
  var el = byId('free-camera-hint');
  if (el) el.classList.toggle('show', !!(freeCamera && freeCamera.active));
}
function resetFreeCameraToDefault() {
  if (!freeCamera) return;
  if (freeCameraDeferredSaveTimer) {
    clearTimeout(freeCameraDeferredSaveTimer);
    freeCameraDeferredSaveTimer = 0;
  }
  var fromPos = freeCamera.position ? freeCamera.position.clone() : new THREE.Vector3(0, 0, 6.6);
  var resetPose = getDefaultFreeCameraResetPose();
  freeCamera.resetTween = {
    start: performance.now(),
    duration: 620,
    from: {
      position: fromPos,
      yaw: Number(freeCamera.yaw) || 0,
      pitch: Number(freeCamera.pitch) || 0,
      roll: Number(freeCamera.roll) || 0,
      fov: Number(freeCamera.fov) || BASE_FOV
    },
    to: {
      position: resetPose.position,
      yaw: resetPose.yaw,
      pitch: resetPose.pitch,
      roll: resetPose.roll,
      fov: resetPose.fov
    }
  };
  freeCamera.active = false;
  freeCamera.locked = true;
  freeCamera.keys = {};
  if (freeCamera.velocity) freeCamera.velocity.set(0, 0, 0);
  try { if (document.pointerLockElement === renderer.domElement) document.exitPointerLock(); } catch (e) {}
  updateFreeCameraHint();
  showToast('自由镜头正在平滑回正');
}
function toggleFreeCamera() {
  if (!freeCamera) freeCamera = defaultFreeCameraState();
  if (freeCamera.active) {
    freeCamera.active = false;
    freeCamera.locked = true;
    freeCamera.keys = {};
    if (freeCamera.velocity) freeCamera.velocity.set(0, 0, 0);
    try { if (document.pointerLockElement === renderer.domElement) document.exitPointerLock(); } catch (e) {}
    saveFreeCameraState();
    updateFreeCameraHint();
    showToast('自由镜头已固定');
    return;
  }
  captureFreeCameraFromCurrent();
  freeCamera.active = true;
  freeCamera.locked = true;
  freeCamera.resetTween = null;
  freeCamera.keys = {};
  freeCameraPointer.seen = false;
  if (!freeCamera.velocity) freeCamera.velocity = new THREE.Vector3();
  try { renderer.domElement.focus && renderer.domElement.focus({ preventScroll: true }); } catch (e) {
    try { renderer.domElement.focus && renderer.domElement.focus(); } catch (ignore) {}
  }
  saveFreeCameraState();
  updateFreeCameraHint();
  try {
    var lockResult = renderer.domElement.requestPointerLock && renderer.domElement.requestPointerLock();
    if (lockResult && lockResult.catch) lockResult.catch(function(){ freeCameraPointer.seen = false; });
  } catch (e) {
    freeCameraPointer.seen = false;
  }
  showToast('自由镜头: WASD 移动 · 鼠标转向 · K 回正');
}
function updateFreeCamera(dt) {
  if (!freeCamera) return;
  if (freeCamera.resetTween) {
    var tw = freeCamera.resetTween;
    var t = easeOutCubic01((performance.now() - tw.start) / Math.max(1, tw.duration || 620));
    freeCamera.position.copy(tw.from.position).lerp(tw.to.position, t);
    freeCamera.yaw = tw.from.yaw + shortestAngleDelta(tw.from.yaw, tw.to.yaw) * t;
    freeCamera.pitch = tw.from.pitch + (tw.to.pitch - tw.from.pitch) * t;
    freeCamera.roll = tw.from.roll + shortestAngleDelta(tw.from.roll, tw.to.roll) * t;
    freeCamera.fov = tw.from.fov + (tw.to.fov - tw.from.fov) * t;
    if (t >= 0.999) {
      freeCamera.position.copy(tw.to.position);
      freeCamera.yaw = tw.to.yaw;
      freeCamera.pitch = tw.to.pitch;
      freeCamera.roll = tw.to.roll;
      freeCamera.fov = tw.to.fov;
      freeCamera.resetTween = null;
      freeCamera.active = false;
      freeCamera.locked = false;
      saveFreeCameraState();
      updateFreeCameraHint();
      recenterCamera();
      showToast('自由镜头已回正');
    }
    return;
  }
  if (!freeCamera.active) return;
  var keys = freeCamera.keys || {};
  FREE_CAMERA_MOVE.set(0, 0, 0);
  if (keys.KeyW) FREE_CAMERA_MOVE.z -= 1;
  if (keys.KeyS) FREE_CAMERA_MOVE.z += 1;
  if (keys.KeyA) FREE_CAMERA_MOVE.x -= 1;
  if (keys.KeyD) FREE_CAMERA_MOVE.x += 1;
  if (keys.Space) FREE_CAMERA_MOVE.y += 1;
  if (keys.ControlLeft || keys.ControlRight) FREE_CAMERA_MOVE.y -= 1;
  if (!freeCamera.velocity) freeCamera.velocity = new THREE.Vector3();
  var targetVel = FREE_CAMERA_TARGET_VEL.set(0, 0, 0);
  if (FREE_CAMERA_MOVE.lengthSq() > 0) {
    FREE_CAMERA_MOVE.normalize();
    FREE_CAMERA_EULER.set(freeCamera.pitch, freeCamera.yaw, 0, 'YXZ');
    FREE_CAMERA_MOVE.applyEuler(FREE_CAMERA_EULER);
    var speed = (keys.ShiftLeft || keys.ShiftRight ? 6.2 : 2.35);
    targetVel.copy(FREE_CAMERA_MOVE).multiplyScalar(speed);
  }
  var ease = targetVel.lengthSq() > 0 ? 8.2 : 13.5;
  freeCamera.velocity.lerp(targetVel, clampRange(ease * Math.max(0.001, dt || 1 / 60), 0, 1));
  if (freeCamera.velocity.lengthSq() < 0.0004) freeCamera.velocity.set(0, 0, 0);
  freeCamera.position.addScaledVector(freeCamera.velocity, Math.max(0.001, dt || 1 / 60));
  var rollDir = (keys.KeyQ ? 1 : 0) - (keys.KeyE ? 1 : 0);
  if (rollDir) freeCamera.roll = clampRange(freeCamera.roll + rollDir * dt * 0.9, -Math.PI, Math.PI);
  scheduleFreeCameraStateSave(720);
}
function flushPersistentVisualState() {
  try { saveLyricLayout(); } catch (e) {}
  try { saveFreeCameraState(); } catch (e) {}
}
__on(window, 'beforeunload', flushPersistentVisualState);
__on(window, 'pagehide', flushPersistentVisualState);

function resetBeatCameraSync(t) {
  beatCam.nextIdx = 0;
  beatCam.events.length = 0;
  beatCam.punch = 0;
  beatCam.lastTriggerAt = -10;
  beatCam.lastRealtimeAt = -10;
  beatCam.thetaKick = 0;
  beatCam.phiKick = 0;
  beatCam.radiusKick = 0;
  beatCam.rollKick = 0;
  beatCam.prevAudioTime = isFinite(t) ? t : -1;
  camPunch = 0;
  beatCam.stats.map = 0;
  beatCam.stats.live = 0;
  beatCam.stats.merged = 0;
  beatCam.stats.liveBlocked = 0;
  liveCamAvg = 0;
  liveCamPeak = 0.28;
  liveCamLastRaw = 0;
  resetRealtimeBeatEngine();
}

function syncBeatCameraToTime(t) {
  resetBeatCameraSync(t);
  if (!currentBeatMap) return;
  alignBeatCameraCursorToTime(t);
}

function alignBeatCameraCursorToTime(t) {
  if (!currentBeatMap) return;
  var beats = currentBeatMap.cameraBeats || currentBeatMap.beats || currentBeatMap.kicks || [];
  beatCam.nextIdx = 0;
  while (beatCam.nextIdx < beats.length) {
    var bt = typeof beats[beatCam.nextIdx] === 'number' ? beats[beatCam.nextIdx] : beats[beatCam.nextIdx].time;
    if (bt >= t + beatCam.lookahead) break;
    beatCam.nextIdx++;
  }
}

function easeBeatCamera(x) {
  x = Math.max(0, Math.min(1, x));
  return x * x * (3 - 2 * x);
}

function updateCinemaDynamics(rawEnergy, rawLow) {
  var e = clamp01(rawEnergy || 0);
  var l = clamp01(rawLow || 0);
  var isDj = djMode.active;
  var composite = clamp01(e * (isDj ? 0.52 : 0.62) + l * (isDj ? 0.48 : 0.38));
  if (isDj) {
    var prevEnergy = djMode.sectionEnergy || 0;
    var prevLow = djMode.sectionLow || 0;
    djMode.sectionEnergy += (e - djMode.sectionEnergy) * (e > djMode.sectionEnergy ? 0.030 : 0.010);
    djMode.sectionLow += (l - djMode.sectionLow) * (l > djMode.sectionLow ? 0.036 : 0.012);
    var change = Math.abs(e - prevEnergy) * 0.46 + Math.abs(l - prevLow) * 0.62;
    djMode.sectionChange += (change - djMode.sectionChange) * (change > djMode.sectionChange ? 0.055 : 0.018);
    djMode.visualPulse *= Math.pow(0.30, 1 / 60);
  }
  cinemaDynamics.avg += (composite - cinemaDynamics.avg) * (composite > cinemaDynamics.avg ? (isDj ? 0.018 : 0.010) : (isDj ? 0.006 : 0.004));
  cinemaDynamics.lowAvg += (l - cinemaDynamics.lowAvg) * (l > cinemaDynamics.lowAvg ? (isDj ? 0.022 : 0.012) : (isDj ? 0.007 : 0.005));
  cinemaDynamics.peak = Math.max(isDj ? 0.36 : 0.30, cinemaDynamics.peak * (isDj ? 0.9980 : 0.9988), composite);
  var floor = Math.max(0.10, cinemaDynamics.avg * 0.82);
  var span = Math.max(0.18, cinemaDynamics.peak - floor);
  var lift = clamp01((composite - floor) / span);
  lift = lift * lift * (3 - 2 * lift);
  var target = isDj
    ? 0.50 + lift * 0.66 + clamp01((l - cinemaDynamics.lowAvg) / 0.30) * 0.18 + clamp01(djMode.sectionChange * 2.4) * 0.08
    : 0.42 + lift * 0.56 + clamp01((l - cinemaDynamics.lowAvg) / 0.36) * 0.12;
  if (cinemaDynamics.avg < 0.18 && l < 0.32) target *= isDj ? 0.88 : 0.78;
  if (e > 0.48 && l > 0.46) target = Math.max(target, isDj ? 1.02 : 0.92);
  target = clampRange(target, isDj ? 0.42 : 0.34, isDj ? 1.24 : 1.08);
  cinemaDynamics.scale += (target - cinemaDynamics.scale) * (target > cinemaDynamics.scale ? (isDj ? 0.070 : 0.045) : (isDj ? 0.030 : 0.022));
}

function cameraDynamicsScale(extra) {
  var isDj = djMode.active;
  var djBoost = isDj ? (1.06 + clamp01(djMode.sectionLow) * 0.16 + clamp01(rtBeat.tempoConfidence) * 0.08) : 1;
  return clampRange((cinemaDynamics.scale || 0.82) * (cinemaTrackProfile.scale || 1) * (extra == null ? 1 : extra) * djBoost, isDj ? 0.24 : 0.18, isDj ? 1.42 : 1.18);
}

function cinemaTrackNameHint(song) {
  var label = ((song && song.name) || '') + ' ' + ((song && song.artist) || '');
  label = label.toLowerCase().replace(/\s+/g, '');
  if (/after17/.test(label)) return 0.46;
  if (/joey/.test(label)) return 1.08;
  return 1.0;
}

function cinemaAnalysisProfileForSong(song) {
  var title = String((song && (song.name || song.title)) || '').toLowerCase().replace(/\s+/g, '');
  var artist = String((song && song.artist) || '').toLowerCase().replace(/\s+/g, '');
  var label = title + ' ' + artist;
  if (/日落大道|sunsetboulevard/.test(label)) {
    return {
      id: 'sunset-boulevard-soft-groove',
      softGroove: true,
      phaseScan: true,
      localRefine: true,
      sparseCamera: true,
      introPattern: true
    };
  }
  return { id: 'default', softGroove: false, phaseScan: false, localRefine: false, sparseCamera: false, introPattern: false };
}

function resetCinemaTrackProfile(song) {
  var isDj = isPodcastSong(song);
  cinemaTrackProfile.scale = isDj ? 1.08 : 1.0;
  cinemaTrackProfile.target = isDj ? 1.10 : 1.0;
  cinemaTrackProfile.nameHint = isDj ? 1.12 : cinemaTrackNameHint(song);
  cinemaTrackProfile.frames = 0;
  cinemaTrackProfile.energyAvg = 0;
  cinemaTrackProfile.lowAvg = 0;
  cinemaTrackProfile.vocalAvg = 0;
  cinemaTrackProfile.melodyAvg = 0;
  cinemaTrackProfile.punchPeak = 0.10;
  cinemaTrackProfile.density = 0;
}

function updateCinemaTrackProfile(sample) {
  if (!sample) return;
  var p = cinemaTrackProfile;
  p.frames++;
  function follow(cur, next, k) { return cur + (next - cur) * k; }
  var early = p.frames < 360;
  var k = early ? 0.020 : 0.006;
  p.energyAvg = follow(p.energyAvg, clamp01(sample.energy), k);
  p.lowAvg = follow(p.lowAvg, clamp01(sample.low), k);
  p.vocalAvg = follow(p.vocalAvg, clamp01(sample.vocal), k * 0.8);
  p.melodyAvg = follow(p.melodyAvg, clamp01(sample.melody), k * 0.8);
  var punchRaw = clamp01((sample.lowOnset || 0) * 2.4 + (sample.energyOnset || 0) * 1.5 + sample.low * 0.16);
  p.punchPeak = Math.max(0.10, p.punchPeak * 0.9975, punchRaw);
  var lowDrive = clamp01((p.lowAvg - 0.20) / 0.42);
  var loudDrive = clamp01((p.energyAvg - 0.18) / 0.40);
  var punchDrive = clamp01((p.punchPeak - 0.13) / 0.36);
  var vocalSoft = clamp01((p.vocalAvg * 0.72 + p.melodyAvg * 0.42 - p.lowAvg * 0.34 - 0.08) / 0.42);
  var quietSoft = clamp01((0.24 - p.energyAvg) / 0.18);
  var target = djMode.active
    ? 0.72 + lowDrive * 0.34 + loudDrive * 0.18 + punchDrive * 0.42 - vocalSoft * 0.12 - quietSoft * 0.06
    : 0.54 + lowDrive * 0.28 + loudDrive * 0.22 + punchDrive * 0.34 - vocalSoft * 0.34 - quietSoft * 0.18;
  if (p.density) target += clamp01((p.density - 0.55) / 1.6) * 0.14;
  target *= p.nameHint || 1;
  target = clampRange(target, djMode.active ? 0.68 : 0.28, djMode.active ? 1.26 : 1.12);
  p.target = target;
  p.scale += (target - p.scale) * (target > p.scale ? (djMode.active ? 0.045 : 0.030) : (djMode.active ? 0.030 : 0.045));
}

function applyCinemaProfileFromBeatMap(map) {
  if (!map || !map.duration) return;
  var events = (map.cameraBeats || map.beats || []).filter(function(b){ return b && typeof b !== 'number' && b.camera !== false; });
  if (!events.length) return;
  var sumImpact = 0, sumLow = 0, primary = 0;
  events.forEach(function(b){
    sumImpact += Math.max(b.impact || 0, b.strength || 0);
    sumLow += b.low || 0;
    if (b.primary !== false) primary++;
  });
  var avgImpact = sumImpact / events.length;
  var avgLow = sumLow / events.length;
  var density = events.length / Math.max(20, map.duration);
  cinemaTrackProfile.density = density;
  var target = 0.44 + clamp01((avgImpact - 0.20) / 0.55) * 0.38 + clamp01((avgLow - 0.24) / 0.48) * 0.18 + clamp01((density - 0.45) / 1.65) * 0.20 + clamp01(primary / Math.max(1, events.length)) * 0.08;
  target *= cinemaTrackProfile.nameHint || 1;
  target = clampRange(target, 0.28, 1.12);
  cinemaTrackProfile.target = target;
  cinemaTrackProfile.scale += (target - cinemaTrackProfile.scale) * (target < cinemaTrackProfile.scale ? 0.55 : 0.22);
}

function resetRealtimeBeatEngine() {
  rtBeat.subFast = rtBeat.subSlow = rtBeat.lowFast = rtBeat.lowSlow = 0;
  rtBeat.bodyFast = rtBeat.bodySlow = rtBeat.vocalFast = rtBeat.vocalSlow = rtBeat.snapFast = rtBeat.snapSlow = 0;
  rtBeat.prevSub = rtBeat.prevLow = rtBeat.prevBody = rtBeat.prevVocal = rtBeat.prevSnap = rtBeat.prevRms = 0;
  rtBeat.onsetAvg = 0.012;
  rtBeat.onsetPeak = 0.060;
  rtBeat.subPeak = 0.14;
  rtBeat.lowPeak = 0.18;
  rtBeat.bodyPeak = 0.16;
  rtBeat.vocalPeak = 0.16;
  rtBeat.snapPeak = 0.14;
  rtBeat.lastHitAt = -10;
  rtBeat.tempoGap = 0;
  rtBeat.tempoConfidence = 0;
  rtBeat.beatCount = 0;
  rtBeat.primedFrames = 0;
  rtBeat.warmupUntil = (audio && isFinite(audio.currentTime) ? audio.currentTime : 0) + (djMode.active ? 0.34 : 1.15);
  rtBeat.pulse = 0;
  rtBeat.score = 0;
  rtBeat.stats.hits = 0;
  rtBeat.stats.blocked = 0;
  rtBeat.stats.assisted = 0;
  rtBeat.stats.strong = 0;
  rtBeat.stats.rejected = 0;
}

function resetAudioVisualState() {
  bass = 0;
  mid = 0;
  treble = 0;
  audioEnergy = 0;
  beatPulse = 0;
  prevEnergy = 0;
  smoothBass = 0;
  smoothMid = 0;
  smoothTreb = 0;
  smoothEnergy = 0;
  bassPeak = 0.12;
  midPeak = 0.10;
  treblePeak = 0.08;
  energyPeak = 0.10;
  scheduledBeatPulse = 0;
  scheduledBeatFlag = false;
  beatOnsetFlag = false;
  cinemaDynamics.avg = 0;
  cinemaDynamics.lowAvg = 0;
  cinemaDynamics.peak = 0.30;
  cinemaDynamics.scale = 0.82;
  if (djMode.active) resetDjModeMeter();
}

function beatEventTime(ev) {
  return typeof ev === 'number' ? ev : (ev && isFinite(ev.time) ? ev.time : Infinity);
}

function yieldToPaint() {
  return new Promise(function(resolve) {
    if (isHiddenForBackgroundOptimization() || typeof requestAnimationFrame !== 'function') {
      setTimeout(resolve, 0);
    } else {
      requestAnimationFrame(function(){ setTimeout(resolve, 0); });
    }
  });
}

function yieldToIdle(timeout) {
  return new Promise(function(resolve) {
    if (isHiddenForBackgroundOptimization()) {
      setTimeout(resolve, Math.min(timeout || 80, 80));
      return;
    }
    if (typeof requestIdleCallback === 'function') {
      requestIdleCallback(function(){ resolve(); }, { timeout: timeout || 1200 });
    } else {
      setTimeout(resolve, timeout ? Math.min(timeout, 600) : 160);
    }
  });
}

function scheduleAnalysisTask(fn, timeout) {
  if (typeof fn !== 'function') return;
  if (isHiddenForBackgroundOptimization()) {
    setTimeout(fn, 0);
    return;
  }
  if (window.requestIdleCallback) {
    requestIdleCallback(fn, { timeout: timeout || 900 });
  } else {
    setTimeout(fn, Math.min(timeout || 420, 420));
  }
}

function scheduleVisualApply(fn, delay, timeout) {
  if (typeof fn !== 'function') return;
  setTimeout(function(){
    if (isHiddenForBackgroundOptimization() || typeof requestAnimationFrame !== 'function') {
      fn();
      return;
    }
    var run = function(){ requestAnimationFrame(fn); };
    if (window.requestIdleCallback) requestIdleCallback(run, { timeout: timeout || 360 });
    else run();
  }, delay || 0);
}

function scheduleUiWarmTask(fn, timeout) {
  if (typeof fn !== 'function') return;
  var run = function(){ requestAnimationFrame(fn); };
  if (isHiddenForBackgroundOptimization() || typeof requestAnimationFrame !== 'function') {
    setTimeout(fn, 0);
  } else if (window.requestIdleCallback) {
    requestIdleCallback(run, { timeout: timeout || 220 });
  } else {
    requestAnimationFrame(fn);
  }
}

function cancelBeatAnalysisTimer() {
  if (beatAnalysisTimer) {
    clearTimeout(beatAnalysisTimer);
    beatAnalysisTimer = null;
  }
}

function cancelBeatPrefetchTimer() {
  if (beatPrefetchTimer) {
    clearTimeout(beatPrefetchTimer);
    beatPrefetchTimer = null;
  }
}

function beatAnalysisYieldMs(options, currentMs, prefetchMs) {
  options = options || {};
  if (options.prefetch) return prefetchMs == null ? 620 : prefetchMs;
  if (options.background) return currentMs == null ? 120 : currentMs;
  return Math.min(currentMs == null ? 120 : currentMs, 160);
}

function beatBandRms(data, sampleRate, fftSize, hz0, hz1) {
  var binHz = sampleRate / fftSize;
  var a = Math.max(1, Math.floor(hz0 / binHz));
  var b = Math.min(data.length - 1, Math.ceil(hz1 / binHz));
  var sum = 0, count = 0;
  for (var i = a; i <= b; i++) {
    var v = data[i] / 255;
    sum += v * v;
    count++;
  }
  return count ? Math.sqrt(sum / count) : 0;
}

function processRealtimeBeatEngine(dt) {
  if (!beatAnalyser || !audioCtx || !audio || audio.paused) return null;
  dt = Math.max(0.001, Math.min(0.080, dt || 0.016));
  var dj = djMode.active;
  beatAnalyser.getByteFrequencyData(beatFrequencyData);
  beatAnalyser.getByteTimeDomainData(beatTimeDomainData);
  var sr = audioCtx.sampleRate || 44100;
  var sub = beatBandRms(beatFrequencyData, sr, beatAnalyser.fftSize, 38, 74);
  var kick = beatBandRms(beatFrequencyData, sr, beatAnalyser.fftSize, 52, 165);
  var body = beatBandRms(beatFrequencyData, sr, beatAnalyser.fftSize, 165, 420);
  var vocal = beatBandRms(beatFrequencyData, sr, beatAnalyser.fftSize, 420, 2600);
  var snap = beatBandRms(beatFrequencyData, sr, beatAnalyser.fftSize, 1800, 9200);
  var low = Math.min(1, kick * 0.86 + sub * 0.42);
  var rms = 0;
  for (var i = 0; i < beatTimeDomainData.length; i++) {
    var tv = (beatTimeDomainData[i] - 128) / 128;
    rms += tv * tv;
  }
  rms = Math.sqrt(rms / beatTimeDomainData.length);

  function follow(cur, next, upTau, downTau) {
    var tau = next > cur ? upTau : downTau;
    return cur + (next - cur) * (1 - Math.exp(-dt / Math.max(0.001, tau)));
  }
  var fastMul = dj ? 0.86 : 1;
  var downMul = dj ? 0.94 : 1;
  var slowMul = dj ? 1.06 : 1;
  rtBeat.subFast = follow(rtBeat.subFast, sub, 0.018 * fastMul, 0.064 * downMul);
  rtBeat.subSlow = follow(rtBeat.subSlow, sub, 0.320 * slowMul, 0.520 * slowMul);
  rtBeat.lowFast = follow(rtBeat.lowFast, low, 0.016 * fastMul, 0.070 * downMul);
  rtBeat.lowSlow = follow(rtBeat.lowSlow, low, 0.300 * slowMul, 0.540 * slowMul);
  rtBeat.bodyFast = follow(rtBeat.bodyFast, body, 0.020 * fastMul, 0.082 * downMul);
  rtBeat.bodySlow = follow(rtBeat.bodySlow, body, 0.360 * slowMul, 0.600 * slowMul);
  rtBeat.vocalFast = follow(rtBeat.vocalFast, vocal, 0.026 * fastMul, 0.090 * downMul);
  rtBeat.vocalSlow = follow(rtBeat.vocalSlow, vocal, 0.340 * slowMul, 0.580 * slowMul);
  rtBeat.snapFast = follow(rtBeat.snapFast, snap, 0.012 * fastMul, 0.060 * downMul);
  rtBeat.snapSlow = follow(rtBeat.snapSlow, snap, 0.300 * slowMul, 0.520 * slowMul);

  var peakDecay = dj ? 0.988 : 0.990;
  rtBeat.subPeak = Math.max(rtBeat.subPeak * Math.pow(peakDecay, dt * 60), sub, 0.045);
  rtBeat.lowPeak = Math.max(rtBeat.lowPeak * Math.pow(dj ? 0.987 : 0.989, dt * 60), low, 0.060);
  rtBeat.bodyPeak = Math.max(rtBeat.bodyPeak * Math.pow(peakDecay, dt * 60), body, 0.040);
  rtBeat.vocalPeak = Math.max(rtBeat.vocalPeak * Math.pow(peakDecay, dt * 60), vocal, 0.040);
  rtBeat.snapPeak = Math.max(rtBeat.snapPeak * Math.pow(peakDecay, dt * 60), snap, 0.035);

  var subFlux = Math.max(0, sub - rtBeat.prevSub);
  var lowFlux = Math.max(0, low - rtBeat.prevLow);
  var bodyFlux = Math.max(0, body - rtBeat.prevBody);
  var vocalFlux = Math.max(0, vocal - rtBeat.prevVocal);
  var snapFlux = Math.max(0, snap - rtBeat.prevSnap);
  var rmsFlux = Math.max(0, rms - rtBeat.prevRms);
  var subRise = Math.max(0, rtBeat.subFast - rtBeat.subSlow);
  var lowRise = Math.max(0, rtBeat.lowFast - rtBeat.lowSlow);
  var bodyRise = Math.max(0, rtBeat.bodyFast - rtBeat.bodySlow);
  var vocalRise = Math.max(0, rtBeat.vocalFast - rtBeat.vocalSlow);
  var snapRise = Math.max(0, rtBeat.snapFast - rtBeat.snapSlow);
  var drumOnset = subRise * 0.88 + subFlux * 0.66 + lowRise * 1.62 + lowFlux * 1.34;
  var musicalOnset = bodyRise * 0.34 + bodyFlux * 0.24 + vocalRise * 0.52 + vocalFlux * 0.36 + snapRise * 0.08 + snapFlux * 0.06 + rmsFlux * 0.20;
  var onset = dj ? drumOnset * 1.05 + musicalOnset * 0.07 : drumOnset + musicalOnset * 0.16;

  var avgTau = onset > rtBeat.onsetAvg ? (dj ? 0.88 : 1.10) : (dj ? 0.30 : 0.34);
  rtBeat.onsetAvg = follow(rtBeat.onsetAvg, onset, avgTau, avgTau);
  rtBeat.onsetPeak = Math.max(rtBeat.onsetPeak * Math.pow(dj ? 0.986 : 0.988, dt * 60), onset, 0.032);
  var floor = rtBeat.onsetAvg * (dj ? 0.88 : 0.84);
  var score = clamp01((onset - floor) / Math.max(dj ? 0.013 : 0.014, rtBeat.onsetPeak - floor));
  var subNorm = clamp01(sub / Math.max(0.045, rtBeat.subPeak * (dj ? 0.72 : 0.70)));
  var lowNorm = clamp01(low / Math.max(0.060, rtBeat.lowPeak * (dj ? 0.74 : 0.72)));
  var bodyNorm = clamp01(body / Math.max(0.045, rtBeat.bodyPeak * (dj ? 0.74 : 0.72)));
  var vocalNorm = clamp01(vocal / Math.max(0.045, rtBeat.vocalPeak * 0.72));
  var snapNorm = clamp01(snap / Math.max(0.040, rtBeat.snapPeak * (dj ? 0.78 : 0.72)));
  var nowT = audio.currentTime || 0;
  rtBeat.primedFrames++;
  var warmingUp = nowT < rtBeat.warmupUntil || rtBeat.primedFrames < (dj ? 8 : 18);
  var gapFromLast = nowT - rtBeat.lastHitAt;
  var expectedGap = rtBeat.tempoGap > 0 ? rtBeat.tempoGap : 0;
  var phaseErr = expectedGap > 0 ? Math.abs(gapFromLast - expectedGap) : 99;
  var phaseWindow = expectedGap > 0 ? Math.max(dj ? 0.055 : 0.055, Math.min(dj ? 0.105 : 0.105, expectedGap * (dj ? 0.16 : 0.16))) : 0;
  var tempoDue = expectedGap > 0 && gapFromLast > expectedGap - phaseWindow && gapFromLast < expectedGap + phaseWindow;
  var lowPresence = Math.max(lowNorm, subNorm * 0.74);
  var lowAttack = lowRise + lowFlux * 0.72 + subRise * 0.58 + subFlux * 0.40;
  var lowDominance = low / Math.max(0.001, vocal * 0.84 + body * 0.36 + snap * 0.10);
  var lowFluxDominance = (lowFlux + subFlux * 0.58) / Math.max(0.001, vocalFlux * 0.72 + bodyFlux * 0.42 + snapFlux * 0.16);
  var voiceMask = dj
    ? (vocalNorm > 0.62 && lowDominance < 0.92 && lowFluxDominance < 1.06 && subNorm < 0.54)
    : (vocalNorm > 0.58 && lowDominance < 0.86 && lowFluxDominance < 1.10);
  var drumGate = lowPresence > (dj ? 0.42 : 0.38) && lowAttack > Math.max(dj ? 0.015 : 0.014, rtBeat.onsetAvg * (dj ? 0.38 : 0.34)) && !voiceMask;
  drumGate = drumGate && (lowDominance > (dj ? 0.86 : 0.72) || lowFluxDominance > (dj ? 1.14 : 1.02) || subNorm > (dj ? 0.62 : 0.56));
  var strongTransient = drumGate && score > (dj ? 0.55 : 0.54) && drumOnset > rtBeat.onsetAvg * (dj ? 0.92 : 0.84);
  var kickTransient = drumGate && score > (dj ? 0.43 : 0.40) && lowAttack > Math.max(dj ? 0.020 : 0.018, rtBeat.onsetAvg * (dj ? 0.54 : 0.46));
  var tempoAssist = tempoDue && rtBeat.tempoConfidence > (dj ? 0.40 : 0.42) && drumGate && lowPresence > (dj ? 0.48 : 0) && score > (dj ? 0.30 : 0.22) && lowAttack > Math.max(0.016, rtBeat.onsetAvg * (dj ? 0.44 : 0.34));
  var candidateHit = strongTransient || kickTransient || tempoAssist;
  if (warmingUp) candidateHit = false;
  var hasTempoLock = expectedGap >= (dj ? 0.32 : 0.42) && expectedGap <= (dj ? 0.92 : 0.88) && rtBeat.tempoConfidence > (dj ? 0.36 : 0.38);
  var lockedWindow = hasTempoLock ? Math.max(dj ? 0.062 : 0.070, Math.min(dj ? 0.118 : 0.110, expectedGap * (dj ? 0.17 : 0.16))) : 0;
  var gapRaw = nowT - rtBeat.lastHitAt;
  var rhythmAccept = false;
  if (candidateHit) {
    if (rtBeat.lastHitAt < 0) {
      rhythmAccept = strongTransient && score > (dj ? 0.58 : 0.62) && lowPresence > (dj ? 0.50 : 0.48);
    } else if (hasTempoLock) {
      var oneBeatErr = Math.abs(gapRaw - expectedGap);
      var twoBeatErr = Math.abs(gapRaw - expectedGap * 2);
      rhythmAccept = oneBeatErr <= lockedWindow && (kickTransient || strongTransient);
      rhythmAccept = rhythmAccept || (twoBeatErr <= lockedWindow * 1.35 && strongTransient && score > (dj ? 0.54 : 0.58));
      rhythmAccept = rhythmAccept || (gapRaw > expectedGap * 1.55 && strongTransient && lowPresence > (dj ? 0.50 : 0.44));
      if (dj) {
        rhythmAccept = rhythmAccept || (gapRaw > expectedGap * 1.24 && strongTransient && score > 0.56 && lowDominance > 0.92);
      }
    } else {
      rhythmAccept = gapRaw >= (dj ? 0.340 : beatCam.realtimeMinInterval) && strongTransient && score > (dj ? 0.56 : 0.58) && lowPresence > (dj ? 0.50 : 0.44);
    }
  }
  var hit = candidateHit && rhythmAccept;
  if (!hit && (candidateHit || score > 0.42 || vocalNorm > 0.62 || bodyNorm > 0.54)) rtBeat.stats.rejected++;
  var minGap = hasTempoLock ? Math.max(dj ? 0.315 : 0.400, Math.min(dj ? 0.500 : 0.540, expectedGap * (dj ? 0.64 : 0.72))) : (dj ? 0.340 : beatCam.realtimeMinInterval);
  if (hit && gapRaw < minGap) {
    rtBeat.stats.blocked++;
    hit = false;
  }

  rtBeat.prevSub = sub;
  rtBeat.prevLow = low;
  rtBeat.prevBody = body;
  rtBeat.prevVocal = vocal;
  rtBeat.prevSnap = snap;
  rtBeat.prevRms = rms;
  rtBeat.score = score;
  rtBeat.pulse *= Math.pow(dj ? 0.24 : 0.18, dt);
  rtBeat.tempoConfidence *= Math.pow(dj ? 0.992 : 0.996, dt * 60);

  if (!hit) {
    if (dj) {
      djMode.tempoGap = rtBeat.tempoGap;
      djMode.tempoConfidence = rtBeat.tempoConfidence;
    }
    return { hit: false, score: score, low: lowNorm, body: bodyNorm, vocal: vocalNorm, snap: snapNorm, tempoConfidence: rtBeat.tempoConfidence };
  }

  var gapShift = 0;
  if (rtBeat.lastHitAt > 0) {
    var gap = nowT - rtBeat.lastHitAt;
    while (gap > (dj ? 0.96 : 0.88)) gap *= 0.5;
    while (gap < (dj ? 0.32 : 0.42)) gap *= 2.0;
    if (gap >= (dj ? 0.32 : 0.42) && gap <= (dj ? 0.96 : 0.88)) {
      gapShift = rtBeat.tempoGap ? Math.abs(gap - rtBeat.tempoGap) / Math.max(0.001, rtBeat.tempoGap) : 0;
      var tempoEase = hasTempoLock ? (dj ? 0.12 : 0.10) : (dj ? 0.24 : 0.22);
      if (dj && gapShift > 0.16 && strongTransient && lowDominance > 0.95) tempoEase = Math.min(0.36, tempoEase + gapShift * 0.45);
      rtBeat.tempoGap = rtBeat.tempoGap ? rtBeat.tempoGap * (1 - tempoEase) + gap * tempoEase : gap;
      rtBeat.tempoConfidence = Math.min(1, rtBeat.tempoConfidence + (tempoAssist ? (dj ? 0.04 : 0.04) : (dj ? 0.16 : 0.18)));
    }
  }
  rtBeat.lastHitAt = nowT;
  rtBeat.beatCount++;
  rtBeat.stats.hits++;
  if (tempoAssist) rtBeat.stats.assisted++;
  if (strongTransient || kickTransient) rtBeat.stats.strong++;
  var strength = dj
    ? clamp01(0.18 + score * 0.38 + lowPresence * 0.34 + Math.min(1.35, lowDominance) * 0.08 + rmsFlux * 0.72)
    : clamp01(0.24 + score * 0.36 + lowPresence * 0.34 + Math.min(1.25, lowDominance) * 0.07 + rmsFlux * 0.95);
  if (tempoAssist) strength = Math.max(strength, (dj ? 0.46 : 0.48) + rtBeat.tempoConfidence * (dj ? 0.10 : 0.10) + lowPresence * (dj ? 0.14 : 0.14));
  var comboSlot = (rtBeat.beatCount - 1) % 4;
  var combo = comboSlot === 0 ? 'downbeat' : (comboSlot === 1 ? 'push' : (comboSlot === 2 ? 'drop' : 'rebound'));
  if (strength > 0.84 && comboSlot !== 0) combo = 'accent';
  if (dj && strength > 0.78 && snapNorm > 0.56 && comboSlot !== 0) combo = 'accent';
  if (dj && gapShift > 0.14 && strongTransient && lowPresence > 0.52) combo = 'downbeat';
  rtBeat.pulse = Math.max(rtBeat.pulse, strength);
  if (dj) {
    djMode.tempoGap = rtBeat.tempoGap;
    djMode.tempoConfidence = rtBeat.tempoConfidence;
    djMode.sectionChange = Math.max(djMode.sectionChange, Math.min(1, gapShift * 1.4));
    djMode.visualPulse = Math.max(djMode.visualPulse, strength);
    djMode.lastBeatAt = nowT;
  }
  return {
    hit: true,
    time: dj ? Math.max(0, nowT - 0.026) : nowT,
    strength: strength,
    confidence: dj ? clamp01(score * 0.58 + lowPresence * 0.30 + rtBeat.tempoConfidence * 0.12) : clamp01(score * 0.62 + lowPresence * 0.26 + rtBeat.tempoConfidence * 0.12),
    low: Math.max(0.05, lowPresence),
    body: Math.max(0.02, bodyNorm * (dj ? 0.50 : 0.62)),
    snap: Math.max(0.02, snapNorm * (dj ? 0.86 : 1)),
    mass: dj ? clamp01(lowPresence * 0.84 + bodyNorm * 0.10) : clamp01(lowPresence * 0.76 + bodyNorm * 0.20),
    sharpness: dj ? clamp01(snapNorm * 0.58 + bodyNorm * 0.10) : clamp01(snapNorm * 0.70 + bodyNorm * 0.12),
    tempoAssist: tempoAssist,
    tempoGap: rtBeat.tempoGap,
    combo: combo,
    score: score,
    lowDominance: lowDominance,
    dj: dj
  };
}

function mergeRealtimeBeatCamera(time, amp, tone) {
  var best = null;
  var bestDist = beatCam.realtimeMergeWindow;
  for (var i = 0; i < beatCam.events.length; i++) {
    var dist = Math.abs((beatCam.events[i].hit || 0) - time);
    if (dist < bestDist) {
      best = beatCam.events[i];
      bestDist = dist;
    }
  }
  if (!best) return false;
  var nowT = audio ? audio.currentTime : uniforms.uTime.value;
  best.hit = time;
  best.start = nowT - (best.attack || beatCam.attack) * 0.42;
  var mergeMaxAmp = ((tone && tone.dj) || djMode.active) ? 0.62 : 0.62;
  best.amp = Math.min(mergeMaxAmp, Math.max(best.amp || 0, amp));
  if (tone) {
    best.zoomAmp = Math.max(best.zoomAmp || 0, tone.zoomAmp);
    best.thetaAmp = Math.max(best.thetaAmp || 0, tone.thetaAmp);
    best.phiAmp = Math.max(best.phiAmp || 0, tone.phiAmp);
    best.rollAmp = Math.max(best.rollAmp || 0, tone.rollAmp || 0);
    best.low = Math.max(best.low || 0, tone.low);
    best.body = Math.max(best.body || 0, tone.body);
    best.snap = Math.max(best.snap || 0, tone.snap);
    best.mode = tone.mode || best.mode;
    best.dj = !!tone.dj || !!best.dj;
  }
  best.source = 'hybrid';
  beatCam.stats.merged++;
  return true;
}

function scheduleBeatCamera(beat, source) {
  if (!fx.cinema) return;
  var time = typeof beat === 'number' ? beat : beat.time;
  if (!isFinite(time)) return;
  var strength = typeof beat === 'number' ? 0.72 : Math.max(0, Math.min(1, beat.strength || 0.72));
  var confidence = typeof beat === 'number' ? 0.72 : Math.max(0, Math.min(1, beat.confidence || 0.72));
  var isPrimary = typeof beat === 'number' ? true : beat.primary !== false;
  var visualImpact = typeof beat === 'number' ? strength : Math.max(0, Math.min(1, beat.impact == null ? strength : beat.impact));
  var isDjMapSource = source === 'djmap';
  var isMapSource = source === 'map' || !source;
  var isLiveSource = source === 'live' || source === 'fallback';
  var livePreview = !!(isLiveSource && beat && beat.preview);
  var dj = djMode.active && (isLiveSource || isDjMapSource || (beat && beat.dj));
  if (isMapSource && !isPrimary) return;
  if (isMapSource && visualImpact < 0.18 && strength < 0.56) return;
  if (isMapSource && confidence < 0.30 && strength < 0.68) return;
  var trackScale = cinemaTrackProfile.scale || 1;
  if (trackScale < 0.58 && isMapSource && strength < 0.72 && visualImpact < 0.46) return;
  if (trackScale < 0.50 && isLiveSource && strength < (dj ? 0.58 : 0.84) && visualImpact < (dj ? 0.42 : 0.56)) return;
  var lowTone = typeof beat === 'number' ? 0.62 : Math.max(0, beat.low == null ? 0.62 : beat.low);
  var bodyTone = typeof beat === 'number' ? 0.22 : Math.max(0, beat.body == null ? 0.22 : beat.body);
  var snapTone = typeof beat === 'number' ? 0.16 : Math.max(0, beat.snap == null ? 0.16 : beat.snap);
  var rawLowTone = lowTone;
  var rawBodyTone = bodyTone;
  var rawSnapTone = snapTone;
  var toneSum = Math.max(0.001, lowTone + bodyTone + snapTone);
  lowTone /= toneSum;
  bodyTone /= toneSum;
  snapTone /= toneSum;
  var sharpness = typeof beat === 'number' ? snapTone : Math.max(0, Math.min(1, beat.sharpness == null ? snapTone : beat.sharpness));
  var mass = typeof beat === 'number' ? lowTone : Math.max(0, Math.min(1, beat.mass == null ? (lowTone * 0.72 + bodyTone * 0.36 + strength * 0.20) : beat.mass));
  var nowT = audio ? audio.currentTime : uniforms.uTime.value;
  var mode = 'deep';
  if (dj) {
    if (rawSnapTone > 0.58 && rawSnapTone > rawLowTone * 0.86 && rawSnapTone > rawBodyTone * 1.08) mode = 'snap';
    else if (rawBodyTone > 0.36 && rawBodyTone > rawLowTone * 0.56) mode = 'body';
  } else {
    if (snapTone > 0.42 && snapTone > lowTone * 1.18 && snapTone > bodyTone * 1.08) mode = 'snap';
    else if (bodyTone > 0.46 && bodyTone > lowTone * 1.12) mode = 'body';
  }
  var amp;
  if (dj) {
    var lowDrive = clamp01((rawLowTone - 0.42) / 0.54);
    var bodyDrive = clamp01((rawBodyTone - 0.24) / 0.58);
    var snapDrive = clamp01((rawSnapTone - 0.30) / 0.60);
    if (mode === 'deep') amp = 0.16 + strength * 0.20 + lowDrive * 0.25 + confidence * 0.05;
    else if (mode === 'body') amp = 0.12 + strength * 0.15 + bodyDrive * 0.18 + lowDrive * 0.06;
    else amp = 0.08 + strength * 0.11 + snapDrive * 0.13;
  } else {
    amp = Math.max(0.18, Math.min(0.72, 0.15 + strength * 0.34 + confidence * 0.06 + mass * 0.13 + snapTone * 0.04));
  }
  if (isMapSource) amp *= 0.68 + visualImpact * 0.46;
  if (!isPrimary) amp *= 0.62;
  if (source === 'fallback') amp *= 0.74;
  if (source === 'live') amp *= dj ? 0.62 : (livePreview ? 0.78 : 0.92);
  if (mode === 'deep' && !dj) amp = Math.min(0.62, amp * 1.12);
  var dynScale = cameraDynamicsScale(0.92 + visualImpact * 0.12 + mass * 0.08);
  amp *= dj ? clampRange(dynScale, 0.72, 1.16) : dynScale;
  var attack = dj
    ? (mode === 'snap' ? 0.010 : (mode === 'body' ? 0.015 : 0.017))
    : Math.max(0.014, Math.min(0.038, beatCam.attack * (1.18 - sharpness * 0.55)));
  var hold = dj
    ? (mode === 'deep' ? 0.038 + lowTone * 0.014 : (mode === 'body' ? 0.026 : 0.014))
    : Math.max(0.014, Math.min(0.052, beatCam.hold * (0.62 + lowTone * 0.55 + bodyTone * 0.25)));
  var release = dj
    ? (mode === 'deep' ? 0.178 + mass * 0.040 : (mode === 'body' ? 0.140 : 0.104))
    : Math.max(0.110, Math.min(0.255, beatCam.release * (0.76 + mass * 0.56 + bodyTone * 0.18 - sharpness * 0.18)));
  var idx = typeof beat === 'number' ? Math.floor(time * 2.7) : (beat.index || Math.floor(time * 2.7));
  var combo = typeof beat === 'number' ? null : beat.combo;
  if (!combo) {
    var comboSlot = Math.abs(idx) % 4;
    combo = comboSlot === 0 ? 'downbeat' : (comboSlot === 1 ? 'push' : (comboSlot === 2 ? 'drop' : 'rebound'));
  }
  var zoomAmp = 0.070 + mass * 0.190 + (mode === 'deep' ? 0.095 : 0.018) + strength * 0.045;
  var thetaAmp = 0.00035;
  var phiAmp = 0.002 + (mode === 'body' ? 0.012 : (mode === 'snap' ? 0.005 : 0.002));
  var rollAmp = mode === 'snap' ? (0.003 + snapTone * 0.004) : 0.0008;
  zoomAmp *= 0.76 + dynScale * 0.28;
  phiAmp *= 0.82 + dynScale * 0.20;
  rollAmp *= 0.78 + dynScale * 0.24;
  if (dj) {
    var lowDrive2 = clamp01((rawLowTone - 0.42) / 0.54);
    var bodyDrive2 = clamp01((rawBodyTone - 0.24) / 0.58);
    var snapDrive2 = clamp01((rawSnapTone - 0.30) / 0.60);
    if (mode === 'deep') {
      zoomAmp = 0.115 + lowDrive2 * 0.170 + strength * 0.036;
      phiAmp = 0.0016 + bodyDrive2 * 0.0022;
      thetaAmp = 0.0006 + bodyDrive2 * 0.0012;
      rollAmp = 0.0006 + snapDrive2 * 0.0016;
    } else if (mode === 'body') {
      zoomAmp = 0.052 + lowDrive2 * 0.052;
      phiAmp = 0.0075 + bodyDrive2 * 0.018;
      thetaAmp = 0.0018 + bodyDrive2 * 0.0046;
      rollAmp = 0.0014 + snapDrive2 * 0.0022;
    } else {
      zoomAmp = 0.026 + lowDrive2 * 0.024;
      phiAmp = 0.0024 + bodyDrive2 * 0.0040;
      thetaAmp = 0.0009 + snapDrive2 * 0.0018;
      rollAmp = 0.0048 + snapDrive2 * 0.0095;
    }
    if (combo === 'downbeat') {
      amp *= 1.12;
      zoomAmp *= mode === 'deep' ? 1.28 : 1.06;
      phiAmp *= 0.76;
    } else if (combo === 'push') {
      amp *= mode === 'deep' ? 0.76 : 0.68;
      zoomAmp *= 0.62;
      thetaAmp *= 1.15;
    } else if (combo === 'drop') {
      amp *= 0.82;
      zoomAmp *= 0.50;
      phiAmp *= 1.38;
    } else if (combo === 'rebound') {
      amp *= 0.62;
      zoomAmp *= 0.40;
      phiAmp *= 0.70;
    } else if (combo === 'accent') {
      amp *= mode === 'snap' ? 0.78 : 0.94;
      zoomAmp *= mode === 'snap' ? 0.42 : 0.78;
      rollAmp *= 1.58;
    }
    if (isDjMapSource) {
      var offlineContrast = Math.pow(clamp01((visualImpact - 0.16) / 0.72), 1.06);
      var offlineDrive = 0.72 + offlineContrast * 0.94 + Math.pow(strength, 1.22) * 0.14;
      var sectionLowGate = clamp01(((djMode.sectionLow || 0) - 0.030) / 0.32);
      var sectionEnergyGate = clamp01(((djMode.sectionEnergy || 0) - 0.045) / 0.40);
      var liveSectionGate = Math.max(sectionLowGate * 0.58 + sectionEnergyGate * 0.34, visualImpact * 0.82);
      var weakSectionScale = 0.54 + Math.pow(clamp01(liveSectionGate), 0.78) * 0.46;
      var comboDrive = combo === 'downbeat'
        ? 0.96 + offlineContrast * 0.38
        : (combo === 'drop'
          ? 0.80 + offlineContrast * 0.26
          : (combo === 'accent'
            ? 0.74 + offlineContrast * 0.30
            : (combo === 'push' ? 0.68 + offlineContrast * 0.16 : 0.52 + offlineContrast * 0.12)));
      if (mode === 'deep') {
        amp *= offlineDrive * comboDrive * 1.38;
        zoomAmp *= 1.14 + offlineContrast * 0.68 + lowDrive2 * 0.20;
        phiAmp *= 0.72 + offlineContrast * 0.22;
        thetaAmp *= 0.72 + offlineContrast * 0.20;
        release *= 0.98 + offlineContrast * 0.20;
      } else if (mode === 'body') {
        amp *= offlineDrive * comboDrive * 1.24;
        zoomAmp *= 0.90 + offlineContrast * 0.32;
        phiAmp *= 1.00 + offlineContrast * 0.42 + bodyDrive2 * 0.18;
        thetaAmp *= 0.98 + offlineContrast * 0.36 + bodyDrive2 * 0.14;
        release *= 0.96 + offlineContrast * 0.12;
      } else {
        amp *= offlineDrive * comboDrive * 0.94;
        zoomAmp *= 0.52 + offlineContrast * 0.24;
        phiAmp *= 0.84 + offlineContrast * 0.28;
        thetaAmp *= 0.86 + offlineContrast * 0.30;
        rollAmp *= 1.02 + offlineContrast * 0.76 + snapDrive2 * 0.22;
        attack *= 0.92;
        release *= 0.78 + offlineContrast * 0.14;
      }
      if (combo === 'downbeat') {
        zoomAmp *= mode === 'deep' ? (1.04 + offlineContrast * 0.18) : (0.96 + offlineContrast * 0.12);
      } else if (combo === 'drop') {
        phiAmp *= 0.96 + offlineContrast * 0.28;
      } else if (combo === 'accent') {
        rollAmp *= 1.02 + offlineContrast * 0.34;
        zoomAmp *= 0.72 + offlineContrast * 0.20;
      }
      var peakTame = Math.pow(clamp01((visualImpact - 0.76) / 0.24), 1.35);
      if (peakTame > 0) {
        var downbeatTame = combo === 'downbeat' ? 1.0 : 0.58;
        amp *= 1 - peakTame * (0.070 + downbeatTame * 0.050);
        zoomAmp *= 1 - peakTame * (0.060 + downbeatTame * 0.050);
        phiAmp *= 1 - peakTame * 0.035;
        release *= 1 - peakTame * 0.045;
      }
      if (visualImpact < 0.12 && liveSectionGate < 0.18) {
        var softScale = Math.min(1, weakSectionScale * (0.72 + visualImpact * 1.10));
        amp *= softScale;
        zoomAmp *= 0.58 + softScale * 0.34;
        phiAmp *= 0.62 + softScale * 0.30;
        thetaAmp *= 0.62 + softScale * 0.28;
        rollAmp *= 0.66 + softScale * 0.24;
        release *= 0.86 + softScale * 0.16;
      }
    }
  } else if (combo === 'downbeat') {
    amp *= 1.10;
    zoomAmp *= 1.18;
    phiAmp *= 0.72;
  } else if (combo === 'push') {
    amp *= 0.84;
    zoomAmp *= 0.88;
    phiAmp *= 0.62;
  } else if (combo === 'drop') {
    amp *= 0.96;
    zoomAmp *= 0.72;
    phiAmp *= 1.22;
  } else if (combo === 'rebound') {
    amp *= 0.74;
    zoomAmp *= 0.62;
    phiAmp *= 0.78;
  } else if (combo === 'accent') {
    amp *= 1.14;
    zoomAmp *= 1.08;
    rollAmp *= 1.35;
  }
  if (livePreview && !dj) {
    var previewTone = clamp01(visualImpact * 0.54 + rawLowTone * 0.22 + confidence * 0.18 + strength * 0.06);
    amp *= 0.72 + previewTone * 0.16;
    zoomAmp *= 0.62 + previewTone * 0.18;
    phiAmp *= 0.70 + previewTone * 0.12;
    thetaAmp *= 0.70 + previewTone * 0.12;
    rollAmp *= 0.54 + previewTone * 0.16;
    release *= 1.08 + previewTone * 0.08;
  }
  if (dj && isDjMapSource && amp > 0.74) amp = 0.74 + (amp - 0.74) * 0.56;
  if (dj && isDjMapSource && zoomAmp > 0.30) zoomAmp = 0.30 + (zoomAmp - 0.30) * 0.52;
  amp = Math.max(dj ? (isDjMapSource ? 0.018 : 0.040) : 0.08, Math.min(dj ? (isDjMapSource ? 0.92 : 0.34) : 0.68, amp));
  if (isLiveSource) {
    var liveMinInterval = dj ? Math.max(0.315, Math.min(0.500, rtBeat.tempoGap ? rtBeat.tempoGap * 0.62 : 0.360)) : beatCam.realtimeMinInterval;
    if (time - beatCam.lastRealtimeAt < liveMinInterval && strength < (dj ? 0.74 : 0.78)) {
      beatCam.stats.liveBlocked++;
      return;
    }
    beatCam.lastRealtimeAt = time;
    if (mergeRealtimeBeatCamera(time, amp, {
      zoomAmp: zoomAmp, thetaAmp: thetaAmp, phiAmp: phiAmp, rollAmp: rollAmp, mode: mode,
      low: lowTone, body: bodyTone, snap: snapTone, dj: dj
    })) {
      beatCam.lastTriggerAt = Math.max(beatCam.lastTriggerAt, time);
      return;
    }
    for (var ei = beatCam.events.length - 1; ei >= 0; ei--) {
      var pending = beatCam.events[ei];
      if (pending.source === 'map' && pending.hit > time && pending.hit - time < beatCam.realtimeMergeWindow) {
        beatCam.events.splice(ei, 1);
      }
    }
  }
  if (isDjMapSource) {
    var djGap = time - beatCam.lastTriggerAt;
    var djMinGap = Math.max(0.255, Math.min(0.470, (beat && beat.step ? beat.step * 0.52 : 0.320)));
    if (djGap < djMinGap && strength < 0.86) return;
    beatCam.lastTriggerAt = time;
    beatCam.stats.map++;
  } else if (!isLiveSource) {
    var gap = time - beatCam.lastTriggerAt;
    var minGap = beatCam.minInterval;
    if (isMapSource && isPrimary) minGap *= 0.82;
    if (gap < minGap && strength < 0.88) return;
    beatCam.lastTriggerAt = time;
    beatCam.stats.map++;
  } else {
    beatCam.lastTriggerAt = Math.max(beatCam.lastTriggerAt, time);
    beatCam.stats.live++;
  }
  beatCam.events.push({
    start: isLiveSource ? nowT - attack * 0.42 : nowT + (time - nowT) - attack,
    hit: time,
    amp: amp,
    attack: attack,
    hold: hold,
    release: release,
    zoomAmp: zoomAmp,
    thetaAmp: thetaAmp,
    phiAmp: phiAmp,
    rollAmp: rollAmp,
    mode: mode,
    combo: combo,
    phase: idx * 2.399963 + (snapTone - lowTone) * 1.4,
    low: lowTone,
    body: bodyTone,
    snap: snapTone,
    mass: mass,
    source: source || 'map',
    dj: dj
  });
  var maxEvents = djMode.active ? 12 : 8;
  if (beatCam.events.length > maxEvents) beatCam.events.splice(0, beatCam.events.length - maxEvents);
}

function updateBeatCamera(dt) {
  var t = audio ? audio.currentTime : uniforms.uTime.value;
  if (!audio || audio.paused) {
    beatCam.punch *= Math.pow(0.08, dt);
    beatCam.thetaKick *= Math.pow(0.05, dt);
    beatCam.phiKick *= Math.pow(0.05, dt);
    beatCam.radiusKick *= Math.pow(0.05, dt);
    beatCam.rollKick *= Math.pow(0.05, dt);
    beatCam.events.length = 0;
    beatCam.prevAudioTime = t;
    return;
  }
  if (beatCam.prevAudioTime >= 0 && Math.abs(t - beatCam.prevAudioTime) > 0.55) {
    if (djMode.active) syncPodcastDjMapCursor(t, false);
    else syncBeatCameraToTime(t);
  }
  beatCam.prevAudioTime = t;

  var punch = 0;
  var thetaKick = 0;
  var phiKick = 0;
  var radiusKick = 0;
  var rollKick = 0;
  var leadEvent = null;
  var leadPunch = 0;
  var leadVal = 0;
  for (var i = beatCam.events.length - 1; i >= 0; i--) {
    var ev = beatCam.events[i];
    var attack = ev.attack || beatCam.attack;
    var hold = ev.hold || beatCam.hold;
    var release = ev.release || beatCam.release;
    var local = t - ev.start;
    var val = 0;
    if (local < 0) {
      val = 0;
    } else if (local < attack) {
      val = easeBeatCamera(local / attack);
    } else if (local < attack + hold) {
      val = 1;
    } else if (local < attack + hold + release) {
      var r = (local - attack - hold) / release;
      val = 1 - easeBeatCamera(r);
    } else {
      beatCam.events.splice(i, 1);
      continue;
    }
    var evPunch = val * ev.amp;
    punch = Math.max(punch, evPunch);
    if (evPunch > leadPunch) {
      leadEvent = ev;
      leadPunch = evPunch;
      leadVal = val;
    }
  }
  if (leadEvent) {
    var sign = Math.sin(leadEvent.phase) >= 0 ? 1 : -1;
    var snapFlick = 1.0 - Math.min(1, Math.max(0, leadVal - 0.25) / 0.75);
    var combo = leadEvent.combo || 'downbeat';
    if (combo === 'downbeat') {
      radiusKick = leadPunch * leadEvent.zoomAmp;
      phiKick = -leadPunch * 0.0032;
    } else if (combo === 'push') {
      radiusKick = leadPunch * leadEvent.zoomAmp * 0.72;
      phiKick = -leadPunch * 0.0014;
    } else if (combo === 'drop') {
      radiusKick = leadPunch * leadEvent.zoomAmp * 0.46;
      phiKick = leadPunch * leadEvent.phiAmp * 0.92;
    } else if (combo === 'rebound') {
      radiusKick = leadPunch * leadEvent.zoomAmp * 0.30;
      phiKick = -leadPunch * leadEvent.phiAmp * 0.22;
    } else if (combo === 'accent') {
      radiusKick = leadPunch * leadEvent.zoomAmp * 0.90;
      phiKick = -leadPunch * 0.0022;
      rollKick = sign * leadPunch * (leadEvent.rollAmp || 0) * (0.45 + snapFlick * 0.30);
    } else if (leadEvent.mode === 'deep') {
      radiusKick = leadPunch * leadEvent.zoomAmp;
      phiKick = -leadPunch * 0.003;
    }
    if (leadEvent.dj) {
      var djSide = sign * leadPunch * (leadEvent.thetaAmp || 0.0012) * (0.70 + (leadEvent.body || 0) * 0.65 + (leadEvent.snap || 0) * 0.35);
      thetaKick += djSide;
      if (leadEvent.mode === 'snap' || combo === 'accent') {
        rollKick += sign * leadPunch * (leadEvent.rollAmp || 0.003) * (0.52 + snapFlick * 0.34);
      }
      if (combo === 'downbeat') radiusKick *= 1.06;
      else if (combo === 'drop') phiKick *= 1.18;
      punch = Math.min(0.90, punch * (1.04 + (leadEvent.mass || 0) * 0.10));
    }
  }
  var djEase = djMode.active;
  beatCam.punch += (punch - beatCam.punch) * (punch > beatCam.punch ? (djEase ? 0.82 : 0.72) : (djEase ? 0.44 : 0.38));
  beatCam.thetaKick += (thetaKick - beatCam.thetaKick) * (Math.abs(thetaKick) > Math.abs(beatCam.thetaKick) ? (djEase ? 0.80 : 0.70) : (djEase ? 0.42 : 0.36));
  beatCam.phiKick += (phiKick - beatCam.phiKick) * (Math.abs(phiKick) > Math.abs(beatCam.phiKick) ? (djEase ? 0.80 : 0.70) : (djEase ? 0.42 : 0.36));
  beatCam.radiusKick += (radiusKick - beatCam.radiusKick) * (radiusKick > beatCam.radiusKick ? (djEase ? 0.82 : 0.72) : (djEase ? 0.40 : 0.34));
  beatCam.rollKick += (rollKick - beatCam.rollKick) * (Math.abs(rollKick) > Math.abs(beatCam.rollKick) ? (djEase ? 0.82 : 0.72) : (djEase ? 0.44 : 0.38));
}

function unlockCenteredView() {
  orbit.centerLocked = false;
}

function clearCenteredViewOffsets() {
  pointerTarget.x = 0;
  pointerTarget.y = 0;
  pointerParallax.x = 0;
  pointerParallax.y = 0;
  mouseWorld.set(-999, -999, 0);
  mouseActive = false;
  headParallax.x = 0;
  headParallax.y = 0;
  headParallax.active = false;
  headNeutral = null;
  if (typeof gestureRotation !== 'undefined') {
    gestureRotation.x = 0;
    gestureRotation.y = 0;
  }
  if (typeof particleSpin !== 'undefined') {
    particleSpin.vx = 0;
    particleSpin.vy = 0;
  }
  if (typeof pinchState !== 'undefined') pinchState.active = false;
  if (typeof particlePointerSpin !== 'undefined') particlePointerSpin.active = false;
  if (typeof resetParticleRotationTarget === 'function') resetParticleRotationTarget(false);
  if (typeof uniforms !== 'undefined' && uniforms.uHandActive) {
    uniforms.uHandActive.value = 0;
    uniforms.uHandXY.value.set(-999, -999);
    if (uniforms.uGestureGrip) uniforms.uGestureGrip.value = 0;
  }
}

function updateCamera() {
  if (applyFreeCameraToCamera()) return;
  if (orbit.recentering) {
    orbit.userTheta  += (orbit.baselineTheta - orbit.userTheta)  * 0.04;
    orbit.userPhi    += (orbit.baselinePhi   - orbit.userPhi)    * 0.04;
    orbit.userRadius += (orbit.baselineRadius- orbit.userRadius) * 0.04;
    if (Math.abs(orbit.userTheta - orbit.baselineTheta) < 0.005 &&
        Math.abs(orbit.userPhi - orbit.baselinePhi) < 0.005 &&
        Math.abs(orbit.userRadius - orbit.baselineRadius) < 0.05) {
      orbit.userTheta = orbit.baselineTheta;
      orbit.userPhi   = orbit.baselinePhi;
      orbit.userRadius= orbit.baselineRadius;
      orbit.recentering = false;
    }
  }

  // v8: focus 优先, 否则用 user + cine 复合姿态
  var fa = orbit.focus.active;
  var targetTheta, targetPhi, targetRadius, tLookAt;
  if (fa) {
    targetTheta = orbit.focus.theta;
    targetPhi   = orbit.focus.phi;
    targetRadius = orbit.focus.radius;
    tLookAt = orbit.focus.lookAt;
  } else if (orbit.centerLocked) {
    targetTheta = orbit.baselineTheta + orbit.cineTheta;
    targetPhi = Math.max(orbit.minPhi, Math.min(orbit.maxPhi, orbit.baselinePhi + orbit.cinePhi));
    targetRadius = Math.max(orbit.minRadius, Math.min(orbit.maxRadius, orbit.baselineRadius + orbit.cineRadius));
    tLookAt = ZERO_VEC;
  } else {
    targetTheta = orbit.userTheta + orbit.cineTheta;
    targetPhi   = Math.max(orbit.minPhi, Math.min(orbit.maxPhi, orbit.userPhi + orbit.cinePhi));
    targetRadius= Math.max(orbit.minRadius, Math.min(orbit.maxRadius, orbit.userRadius + orbit.cineRadius));
    tLookAt = ZERO_VEC;
  }
  // 丝滑变速: 线性 lerp 自然给出 "快→慢" 缓出曲线
  var focusEase = fa ? 0.16 : 0.10;
  var radiusEase = fa ? 0.12 : 0.07;
  if (beatCam.punch > 0.01) {
    focusEase = Math.max(focusEase, 0.12 + beatCam.punch * 0.12);
    radiusEase = Math.max(radiusEase, 0.09 + beatCam.punch * 0.12);
  }
  orbit.theta  += (targetTheta  - orbit.theta)  * focusEase;
  orbit.phi    += (targetPhi    - orbit.phi)    * focusEase;
  orbit.radius += (targetRadius - orbit.radius) * radiusEase;
  orbit.lookAt.x += (tLookAt.x - orbit.lookAt.x) * focusEase;
  orbit.lookAt.y += (tLookAt.y - orbit.lookAt.y) * focusEase;
  orbit.lookAt.z += (tLookAt.z - orbit.lookAt.z) * focusEase;

  var cy = Math.cos(orbit.phi), sy = Math.sin(orbit.phi);
  var ct = Math.cos(orbit.theta), st = Math.sin(orbit.theta);
  camera.position.set(
    orbit.lookAt.x + orbit.radius * cy * st,
    orbit.lookAt.y + orbit.radius * sy,
    orbit.lookAt.z + orbit.radius * cy * ct
  );
  camera.lookAt(orbit.lookAt);
  var cameraShake = clampRange(Number(fx.cinemaShake) || 0, 0, 1.8);
  camera.rotation.z += beatCam.rollKick * cameraShake;

  var cameraPunch = Math.max(camPunch * 0.55, beatCam.punch * 0.54 + beatCam.radiusKick * 0.16) * cameraShake;
  var targetFOV = BASE_FOV - cameraPunch * (djMode.active ? 2.62 : 2.35);
  var fovEase = targetFOV < camera.fov ? 0.24 : 0.12;
  camera.fov += (targetFOV - camera.fov) * fovEase;
  camera.updateProjectionMatrix();
  camPunch *= 0.86;
}

// 焦点跟拍 (hover 0.5s 后镜头移到目标)
var focusHover = { wantType: null, pendingTimer: null, exitTimer: null };
function shouldUseWallpaperSafeShelfCamera() {
  return !!(fx && Number(fx.preset) === 5);
}
function shouldUseSkullSafeShelfCamera() {
  return !!(fx && Number(fx.preset) === SKULL_PRESET_INDEX);
}
function shouldUseWallpaperLyricCameraLock() {
  return !!(fx && Number(fx.preset) === 5 && fx.lyricCameraLock);
}
function requestStageLyricCameraSnap(frames) {
  if (typeof stageLyrics === 'undefined' || !stageLyrics) return;
  stageLyrics.snapCameraLockFrames = Math.max(stageLyrics.snapCameraLockFrames || 0, frames || 8);
}
function shouldDimWallpaperForShelf() {
  if (!shouldUseWallpaperSafeShelfCamera()) return false;
  if (!shelfManager || !shelfManager.getMode || shelfManager.getMode() !== 'side') return false;
  if (shelfPinnedOpen) return true;
  return !!(shelfManager.hasOpenContent && shelfManager.hasOpenContent());
}
function shouldOffsetLyricsForShelfDetail() {
  if (!shelfManager || !shelfManager.getMode || shelfManager.getMode() !== 'side') return false;
  return !!(shelfManager.hasOpenContent && shelfManager.hasOpenContent());
}
function shouldAvoidStageLyricsForShelf() {
  if (!shelfManager || !shelfManager.getMode || shelfManager.getMode() !== 'side') return false;
  if (shelfAlwaysVisible()) return true;
  if (shelfPinnedOpen) return true;
  if (shelfManager.hasOpenContent && shelfManager.hasOpenContent()) return true;
  return !!(shelfVisibility > 0.24 || (shelfHoverCue && shelfHoverCue.value > 0.28));
}
function activateFocusZone(type) {
  unlockCenteredView();
  orbit.focus.active = true;
  orbit.focus.type = type;
  var shelfProfile = shelfLayoutProfile();
  if (type === 'shelf-side') {
    if (shouldUseWallpaperSafeShelfCamera()) {
      orbit.focus.theta  = shelfProfile.portrait ? 0.18 : 0.24;
      orbit.focus.phi    = shelfProfile.portrait ? 0.00 : 0.02;
      orbit.focus.radius = shelfProfile.portrait ? 5.74 : 5.32;
      orbit.focus.lookAt.set(shelfProfile.portrait ? 1.04 : 2.24, -0.08, 0.78);
      camPunch = Math.max(camPunch, 0.28);
      requestStageLyricCameraSnap(10);
    } else {
      // 侧栏 (右): 近一点、侧一点，让歌单架打开时有明确的镜头推近。
      orbit.focus.theta  = shelfProfile.portrait ? 0.24 : 0.42;
      orbit.focus.phi    = shelfProfile.portrait ? -0.06 : -0.12;
      orbit.focus.radius = shelfProfile.portrait ? 5.28 : 4.20;
      orbit.focus.lookAt.set(shelfProfile.portrait ? 1.08 : 2.32, shelfProfile.portrait ? -0.18 : -0.10, 0.72);
      camPunch = Math.max(camPunch, 0.82);
    }
  } else if (type === 'shelf-detail') {
    if (shouldUseWallpaperSafeShelfCamera()) {
      orbit.focus.theta  = shelfProfile.portrait ? 0.16 : 0.26;
      orbit.focus.phi    = shelfProfile.portrait ? -0.02 : 0.02;
      orbit.focus.radius = shelfProfile.portrait ? 5.88 : 5.18;
      orbit.focus.lookAt.set(shelfProfile.portrait ? 0.72 : 2.28, shelfProfile.portrait ? -0.36 : -0.32, 0.84);
      camPunch = Math.max(camPunch, 0.30);
      requestStageLyricCameraSnap(10);
    } else {
      orbit.focus.theta  = shelfProfile.portrait ? 0.16 : 0.34;
      orbit.focus.phi    = shelfProfile.portrait ? -0.03 : -0.06;
      orbit.focus.radius = shelfProfile.portrait ? 5.90 : 4.86;
      orbit.focus.lookAt.set(shelfProfile.portrait ? 0.62 : 1.74, shelfProfile.portrait ? -0.08 : 0.02, 0.82);
      camPunch = Math.max(camPunch, 0.38);
    }
  } else if (type === 'shelf-stage') {
    // 舞台: 居中仰拍
    orbit.focus.theta  = 0.0;
    orbit.focus.phi    = shelfProfile.portrait ? -0.24 : -0.32;
    orbit.focus.radius = shelfProfile.portrait ? 4.8 : 3.8;
    orbit.focus.lookAt.set(0, shelfProfile.portrait ? -1.86 : -1.7, 0.8);
  } else if (type === 'queue') {
    // 队列在左侧 HTML 面板, 相机微微左移 + 抬升
    orbit.focus.theta  = 0.40;
    orbit.focus.phi    = 0.05;
    orbit.focus.radius = 5.8;
    orbit.focus.lookAt.set(-1.2, 0, 0);
  }
}
function setFocusZone(type, immediate) {
  if (type && !shouldUseShelfDynamicCamera(type)) {
    if (/^shelf-/.test(String(orbit.focus.type || ''))) orbit.focus.active = false;
    type = null;
  }
  if (focusHover.wantType === type) return;
  focusHover.wantType = type;
  if (focusHover.pendingTimer) { clearTimeout(focusHover.pendingTimer); focusHover.pendingTimer = null; }
  if (focusHover.exitTimer) { clearTimeout(focusHover.exitTimer); focusHover.exitTimer = null; }
  if (!type) {
    // 立刻退出 focus, 让相机回主姿态 (但插值是平滑的)
    var exitDelay = orbit.focus.type === 'queue' ? PEEK_HIDE_DELAY : 120;
    focusHover.exitTimer = setTimeout(function(){
      focusHover.exitTimer = null;
      if (!focusHover.wantType) orbit.focus.active = false;
    }, exitDelay);
    return;
  }
  if (immediate) {
    activateFocusZone(type);
    return;
  }
  // 延迟 500ms 激活
  focusHover.pendingTimer = setTimeout(function(){
    focusHover.pendingTimer = null;
    if (focusHover.wantType !== type) return;
    activateFocusZone(type);
  }, 260);
}

// 电影镜头 v8: 振幅大幅减小, 节拍 punch 加冷却 + 强度门槛
//   - cineTheta/Phi 是非常缓慢的低频漂移, 不再让人 motion sick
//   - punch zoom 只在 真·强主拍 触发, 至少间隔 0.45s, 振幅 ×0.5
var lastCamPunchAt = -10;
var CAM_PUNCH_MIN_INTERVAL = 0.45;     // 秒
var CAM_PUNCH_BEAT_THRESHOLD = 0.55;   // 必须够强才触发
function updateCinema(dt) {
  cinemaT += dt;
  updateBeatCamera(dt);
  if (!fx.cinema) {
    orbit.cineTheta  *= 0.95;
    orbit.cinePhi    *= 0.95;
    orbit.cineRadius *= 0.95;
    return;
  }
  var damp = orbit.rotating ? 0.25 : 1.0;
  // v8: 振幅减半, 周期更长 (更优雅)
  var dj = djMode.active;
  var shake = clampRange(Number(fx.cinemaShake) || 0, 0, 1.8);
  var beatDamp = (orbit.focus.active ? (dj ? 0.66 : 0.55) : (dj ? 1.12 : 1.0)) * shake;
  var idleDamp = damp * (dj ? 0.72 : 1.0) * shake;
  orbit.cineTheta  = Math.sin(cinemaT * 0.08) * 0.012 * idleDamp + beatCam.thetaKick * beatDamp;
  orbit.cinePhi    = Math.sin(cinemaT * 0.06 + 1.0) * 0.010 * idleDamp + beatCam.phiKick * beatDamp;
  orbit.cineRadius = Math.sin(cinemaT * 0.04 + 2.0) * 0.080 * idleDamp - beatCam.radiusKick * beatDamp * (dj ? 1.22 : 1.18);
}
updateCamera();

function recenterCamera() {
  orbit.centerLocked = true;
  orbit.recentering = true;
  clearCenteredViewOffsets();
  if (typeof skullWheelZoomTarget !== 'undefined') {
    skullWheelZoomTarget = 0;
    if (!(fx && fx.preset === SKULL_PRESET_INDEX)) skullWheelZoom = 0;
  }
  // 同时解除任何镜头跟拍
  if (focusHover) {
    focusHover.wantType = null;
    if (focusHover.pendingTimer) { clearTimeout(focusHover.pendingTimer); focusHover.pendingTimer = null; }
    if (focusHover.exitTimer) { clearTimeout(focusHover.exitTimer); focusHover.exitTimer = null; }
  }
  orbit.focus.active = false;
  if (fx && fx.preset === SKULL_PRESET_INDEX) {
    resetSkullPresetView(false, { smooth:true, keepLyricLock:true });
  } else {
    resetSkullPresetView(true);
  }
  if (!(fx && fx.preset === SKULL_PRESET_INDEX) && ((fx && fx.lyricCameraLock) || shouldUseWallpaperLyricCameraLock())) requestStageLyricCameraSnap(14);
  showToast('视角回正');
}

function hasActivePlaybackControls() {
  return !!(playing || (audio && !audio.paused) || (Array.isArray(playQueue) && currentIdx >= 0 && playQueue[currentIdx]));
}

function setControlsHidden(hidden) {
  var bar = byId('bottom-bar');
  if (!bar) return;
  if (hidden && (controlsHovering || miniQueueOpen)) hidden = false;
  bar.classList.toggle('soft-hidden', !!hidden && controlsAutoHide && bar.classList.contains('visible'));
  bar.style.pointerEvents = '';
  updateControlsChromeState();
}

function isBottomControlsSuppressedForShelf() {
  var shelfContentOpen = false;
  try {
    shelfContentOpen = !!(typeof shelfManager !== 'undefined' && shelfManager && shelfManager.hasOpenContent && shelfManager.hasOpenContent());
  } catch (e) {}
  return !!(shelfPinnedOpen || shelfContentOpen || (controlsShelfSuppressUntil && performance.now() < controlsShelfSuppressUntil));
}

function suppressBottomControlsForShelf(duration) {
  controlsShelfSuppressUntil = performance.now() + (duration == null ? 900 : duration);
  controlsHovering = false;
  if (controlsHideTimer) {
    clearTimeout(controlsHideTimer);
    controlsHideTimer = null;
  }
  __stateRoot().classList.remove('controls-handle-awake');
  if (miniQueueOpen) closeMiniQueue();
  var bar = byId('bottom-bar');
  if (bar) {
    bar.classList.remove('visible', 'soft-hidden');
    bar.style.pointerEvents = '';
  }
  updateControlsChromeState();
}

function scheduleControlsHide(delay) {
  if (controlsHideTimer) clearTimeout(controlsHideTimer);
  if (!controlsAutoHide) return;
  controlsHideTimer = setTimeout(function(){
    controlsHideTimer = null;
    if (!controlsHovering) setControlsHidden(true);
  }, delay == null ? 480 : delay);
}

function revealBottomControls(delay) {
  if (__stateRoot().classList.contains('home-controls-locked')) return;
  var bar = byId('bottom-bar');
  if (isBottomControlsSuppressedForShelf()) return;
  if (bar) bar.classList.add('visible');
  wakeBottomHandle();
  setControlsHidden(false);
  if (controlsAutoHide) scheduleControlsHide(delay == null ? 520 : delay);
}

function updateControlsChromeState() {
  var bar = byId('bottom-bar');
  var handle = byId('bottom-handle');
  var active = !!(bar && bar.classList.contains('visible') && !bar.classList.contains('soft-hidden'));
  __stateRoot().classList.toggle('controls-visible', active);
  if (handle) handle.classList.toggle('active', active);
}

function wakeBottomHandle(duration) {
  __stateRoot().classList.add('controls-handle-awake');
  if (controlsHandleDimTimer) clearTimeout(controlsHandleDimTimer);
  controlsHandleDimTimer = setTimeout(function(){
    controlsHandleDimTimer = null;
    __stateRoot().classList.remove('controls-handle-awake');
  }, duration == null ? 2000 : duration);
}

function forcePlaybackControlsInteractive() {
  if (!hasActivePlaybackControls()) return;
  try {
    __stateRoot().classList.remove('home-controls-locked');
    var bar = byId('bottom-bar');
    if (bar) {
      bar.style.pointerEvents = '';
      if (!controlsAutoHide) {
        bar.classList.add('visible');
        bar.classList.remove('soft-hidden');
      }
    }
    ['play-btn', 'prev-btn', 'next-btn', 'mini-queue-btn', 'heart-btn', 'play-mode-btn', 'collect-btn'].forEach(function(id){
      var btn = byId(id);
      if (!btn) return;
      btn.disabled = false;
      btn.classList.remove('busy');
    });
    updateControlsChromeState();
    if (bar && bar.classList.contains('visible') && controlsAutoHide && !controlsHovering) scheduleControlsHide(220);
  } catch (e) {
    console.warn('[PlaybackControlsRestore]', e);
  }
}

function toggleBottomControlsFromHandle() {
  var bar = byId('bottom-bar');
  if (!bar || __stateRoot().classList.contains('home-controls-locked')) return;
  if (isBottomControlsSuppressedForShelf()) return;
  revealBottomControls(900);
}

function updateControlsAutoHideFromPointer(x, y) {
  if (__stateRoot().classList.contains('home-controls-locked')) return;
  if (isBottomControlsSuppressedForShelf()) return;
  var bar = byId('bottom-bar');
  if (!bar || !bar.classList.contains('visible')) return;
  if (!controlsAutoHide) { setControlsHidden(false); return; }
  if (diyPlayerMode) {
    var fxPanel = byId('fx-panel');
    var fxFab = byId('fx-fab');
    var fr = fxPanel ? fxPanel.getBoundingClientRect() : null;
    var br = fxFab ? fxFab.getBoundingClientRect() : null;
    var overFxPanel = fxPanel && (fxPanel.classList.contains('peek') || fxPanel.classList.contains('show')) && fr && x >= fr.left - 18 && x <= fr.right + 18 && y >= fr.top - 18 && y <= fr.bottom + 18;
    var overFxFab = br && x >= br.left - 18 && x <= br.right + 18 && y >= br.top - 18 && y <= br.bottom + 18;
    if (overFxPanel || overFxFab) {
      scheduleControlsHide(80);
      return;
    }
  }
  controlsLastMoveAt = performance.now();
  var rect = bar.getBoundingClientRect();
  var handle = byId('bottom-handle');
  var hr = handle ? handle.getBoundingClientRect() : null;
  var overHandle = hr && x >= hr.left - 18 && x <= hr.right + 18 && y >= hr.top - 12 && y <= hr.bottom + 14;
  var overBar = x >= rect.left - 18 && x <= rect.right + 18 && y >= rect.top - 18 && y <= rect.bottom + 14;
  var mini = byId('mini-queue-popover');
  var miniRect = mini ? mini.getBoundingClientRect() : null;
  var overMini = miniQueueOpen && miniRect && x >= miniRect.left - 16 && x <= miniRect.right + 16 && y >= miniRect.top - 16 && y <= miniRect.bottom + 16;
  if (overHandle) wakeBottomHandle();
  if (overBar || overMini || overHandle) revealBottomControls(overHandle ? 900 : 520);
  else scheduleControlsHide(70);
}

function toggleControlsAutoHide() {
  controlsAutoHide = !controlsAutoHide;
  saveBooleanPreference(CONTROLS_AUTO_HIDE_STORE_KEY, controlsAutoHide);
  var btn = byId('controls-hide-btn');
  if (btn) btn.classList.toggle('active', controlsAutoHide);
  setControlsHidden(false);
  if (controlsAutoHide) {
    scheduleControlsHide(520);
    showToast('控制条自动隐藏已开启');
  } else {
    if (controlsHideTimer) { clearTimeout(controlsHideTimer); controlsHideTimer = null; }
    showToast('控制条保持显示');
  }
}

function applyControlsAutoHidePreference() {
  var btn = byId('controls-hide-btn');
  if (btn) btn.classList.toggle('active', !!controlsAutoHide);
  if (!controlsAutoHide && controlsHideTimer) {
    clearTimeout(controlsHideTimer);
    controlsHideTimer = null;
  }
  setControlsHidden(false);
}

(function initControlsAutoHide() {
  var bar = byId('bottom-bar');
  var handle = byId('bottom-handle');
  if (!bar) return;
  function enterControls(){
    controlsHovering = true;
    wakeBottomHandle();
    setControlsHidden(false);
    if (controlsHideTimer) { clearTimeout(controlsHideTimer); controlsHideTimer = null; }
  }
  function leaveControls(){
    controlsHovering = false;
    scheduleControlsHide(70);
    wakeBottomHandle(900);
  }
  bar.addEventListener('mouseenter', enterControls);
  bar.addEventListener('mouseleave', leaveControls);
  if (handle) {
    handle.addEventListener('mouseenter', function(){
      controlsHovering = true;
      revealBottomControls(900);
    });
    handle.addEventListener('mouseleave', leaveControls);
    handle.addEventListener('click', function(e){ e.preventDefault(); e.stopPropagation(); toggleBottomControlsFromHandle(); });
  }
  updateControlsChromeState();
})();

function isCursorAutoHideMode() {
  return !document.hidden;
}

function clearCursorAutoHideTimer() {
  if (cursorHideTimer) {
    clearTimeout(cursorHideTimer);
    cursorHideTimer = null;
  }
}

function setCursorHidden(hidden) {
  __stateRoot().classList.toggle('cursor-hidden', !!hidden && isCursorAutoHideMode());
}

function scheduleCursorHide(delay) {
  clearCursorAutoHideTimer();
  if (!isCursorAutoHideMode()) {
    setCursorHidden(false);
    return;
  }
  cursorHideTimer = setTimeout(function(){
    cursorHideTimer = null;
    setCursorHidden(true);
  }, delay == null ? CURSOR_HIDE_DELAY : delay);
}

function revealCursorForActivity() {
  if (!isCursorAutoHideMode()) {
    clearCursorAutoHideTimer();
    setCursorHidden(false);
    return;
  }
  setCursorHidden(false);
  scheduleCursorHide(CURSOR_HIDE_DELAY);
}

function syncCursorAutoHideMode() {
  if (isCursorAutoHideMode()) revealCursorForActivity();
  else {
    clearCursorAutoHideTimer();
    setCursorHidden(false);
  }
}

['mousemove', 'pointermove', 'mousedown', 'wheel', 'touchstart'].forEach(function(type){
  __on(window, type, revealCursorForActivity, { passive:true, capture:true });
});
syncCursorAutoHideMode();


// ▼▼▼ [mineradio:pointer] source lines 5495,5676 ▼▼▼
// ============================================================
//  指针 / 拖拽控制
//   v7.1: 用 userOrbit 替代 targetOrbit; 加 drag 距离判断
// ============================================================
var mouseWorld = new THREE.Vector3(-999, -999, 0);
var mouseActive = false;
var mouseDownAt = { x:0, y:0, t:0, hadDrag:false };
var particlePointerSpin = { active:false, lastX:0, lastY:0, lastT:0 };
var particlePointerRay = new THREE.Raycaster();
var particlePointerNdc = new THREE.Vector2();
var particlePointerPlane = new THREE.Plane();
var particlePointerPlanePoint = new THREE.Vector3();
var particlePointerPlaneNormal = new THREE.Vector3();
var particlePointerWorldHit = new THREE.Vector3();
var particlePointerLocalHit = new THREE.Vector3();
var particlePointerQuat = new THREE.Quaternion();
var particlePointerFrame = { dirty:false, ndcX:0, ndcY:0 };
var CLICK_THRESHOLD = 6;  // 像素, 拖动 > 6px 视为 drag
var UI_HIT_SELECTOR = '#search-area,#top-right,#fullscreen-diy-zone,#fx-panel,#fx-fab,#fx-fab-hide-btn,#playlist-panel,#bottom-bar,#thumb-wrap,#empty-home,#visual-guide,#trial-banner,#source-fallback-notice,.modal-mask,#toast,#ai-depth-chip,#beat-chip,#drop-overlay';

function isPointerOverUi(e) {
  if (!e) return false;
  var el = document.elementFromPoint(e.clientX, e.clientY);
  return !!(el && el.closest && el.closest(UI_HIT_SELECTOR));
}

function particleLocalPointFromNdc(ndcX, ndcY, out) {
  particlePointerNdc.set(ndcX, ndcY);
  particlePointerRay.setFromCamera(particlePointerNdc, camera);
  if (particles) {
    particles.updateMatrixWorld(true);
    particles.getWorldPosition(particlePointerPlanePoint);
    particles.getWorldQuaternion(particlePointerQuat);
    particlePointerPlaneNormal.set(0, 0, 1).applyQuaternion(particlePointerQuat).normalize();
    if (Math.abs(particlePointerPlaneNormal.dot(particlePointerRay.ray.direction)) < 0.16) return false;
    particlePointerPlane.setFromNormalAndCoplanarPoint(particlePointerPlaneNormal, particlePointerPlanePoint);
    if (particlePointerRay.ray.intersectPlane(particlePointerPlane, particlePointerWorldHit)) {
      out.copy(particlePointerWorldHit);
      particles.worldToLocal(out);
      return isFinite(out.x) && isFinite(out.y) && Math.abs(out.x) < 8.5 && Math.abs(out.y) < 8.5;
    }
  }
  particlePointerPlaneNormal.set(0, 0, 1);
  particlePointerPlane.set(particlePointerPlaneNormal, 0);
  if (particlePointerRay.ray.intersectPlane(particlePointerPlane, particlePointerWorldHit)) {
    out.copy(particlePointerWorldHit);
    return isFinite(out.x) && isFinite(out.y) && Math.abs(out.x) < 8.5 && Math.abs(out.y) < 8.5;
  }
  return false;
}

function queueParticlePointerFrame(clientX, clientY) {
  var mx = (clientX / innerWidth) * 2 - 1;
  var my = -(clientY / innerHeight) * 2 + 1;
  pointerTarget.x = mx; pointerTarget.y = my;
  particlePointerFrame.ndcX = mx;
  particlePointerFrame.ndcY = my;
  particlePointerFrame.dirty = true;
}

function updateParticlePointerFrame() {
  if (!particlePointerFrame.dirty) return;
  particlePointerFrame.dirty = false;
  if (particleLocalPointFromNdc(particlePointerFrame.ndcX, particlePointerFrame.ndcY, particlePointerLocalHit)) {
    mouseWorld.x = particlePointerLocalHit.x;
    mouseWorld.y = particlePointerLocalHit.y;
    mouseActive = true;
  } else {
    mouseWorld.set(-999, -999, 0);
    mouseActive = false;
  }
}

function beginParticlePointerDrag(e) {
  if (e.button === 2) return;
  if (isPointerOverUi(e)) return;
  markRenderInteraction('canvas-drag', 1200);
  idleGuidePointerDown(e);
  orbit.rotating = true; orbit.last.x = e.clientX; orbit.last.y = e.clientY;
  particlePointerSpin.active = true;
  particlePointerSpin.lastX = e.clientX;
  particlePointerSpin.lastY = e.clientY;
  particlePointerSpin.lastT = performance.now();
  if (typeof particleSpin !== 'undefined') particleSpin.vx = particleSpin.vy = 0;
  mouseDownAt.x = e.clientX; mouseDownAt.y = e.clientY;
  mouseDownAt.t = performance.now(); mouseDownAt.hadDrag = false;
}
renderer.domElement.addEventListener('mousedown', function(e){
  beginParticlePointerDrag(e);
});
__on(window, 'mousedown', function(e){
  if (!(fx && fx.preset === SKULL_PRESET_INDEX)) return;
  if (orbit.rotating || e.target === renderer.domElement) return;
  beginParticlePointerDrag(e);
}, true);
__on(window, 'mousemove', function(e){
  updateControlsAutoHideFromPointer(e.clientX, e.clientY);
  idleGuidePointerMove(e);
  if (freeCamera && freeCamera.active) {
    markRenderInteraction('free-camera', 900);
    var mdx = e.movementX || 0;
    var mdy = e.movementY || 0;
    if ((!mdx && !mdy) && freeCameraPointer.seen) {
      mdx = e.clientX - freeCameraPointer.x;
      mdy = e.clientY - freeCameraPointer.y;
    }
    freeCameraPointer.x = e.clientX;
    freeCameraPointer.y = e.clientY;
    freeCameraPointer.seen = true;
    freeCamera.yaw -= mdx * 0.00125;
    freeCamera.pitch = clampRange(freeCamera.pitch - mdy * 0.00125, -Math.PI * 0.49, Math.PI * 0.49);
    return;
  }
  if (isPointerOverUi(e) && !orbit.rotating) { mouseActive = false; return; }
  if (orbit.rotating) {
    markRenderInteraction('canvas-drag', 900);
    unlockCenteredView();
    var dx = e.clientX - orbit.last.x, dy = e.clientY - orbit.last.y;
    if (particlePointerSpin.active) {
      var nowSpin = performance.now();
      var spinDt = Math.max(1 / 120, Math.min(0.08, (nowSpin - particlePointerSpin.lastT) / 1000 || 1 / 60));
      applyParticleSpinDrag(dx, dy, spinDt);
      particlePointerSpin.lastX = e.clientX;
      particlePointerSpin.lastY = e.clientY;
      particlePointerSpin.lastT = nowSpin;
    }
    orbit.last.x = e.clientX; orbit.last.y = e.clientY;
    // drag 距离判断
    var totalDx = e.clientX - mouseDownAt.x, totalDy = e.clientY - mouseDownAt.y;
    if (Math.sqrt(totalDx*totalDx + totalDy*totalDy) > CLICK_THRESHOLD) mouseDownAt.hadDrag = true;
    if (orbit.recentering) orbit.recentering = false;
  }
  queueParticlePointerFrame(e.clientX, e.clientY);
});
__on(window, 'mouseup', function(){
  orbit.rotating = false;
  particlePointerSpin.active = false;
  idleGuidePointerUp();
});
renderer.domElement.addEventListener('mouseleave', function(){
  particlePointerFrame.dirty = false;
  mouseWorld.set(-999, -999, 0);
  mouseActive = false;
  idleGuidePointerLeave();
});
renderer.domElement.addEventListener('wheel', function(e){
  if (isPointerOverUi(e)) return;
  e.preventDefault();
  markRenderInteraction('canvas-wheel', 900);
  if (freeCamera && freeCamera.active) {
    freeCamera.fov = clampRange((freeCamera.fov || BASE_FOV) + e.deltaY * 0.018, 26, 72);
    saveFreeCameraState();
    return;
  }
  if (fx && fx.preset === SKULL_PRESET_INDEX && typeof skullWheelZoomTarget !== 'undefined') {
    skullWheelZoomTarget = clampRange(skullWheelZoomTarget + e.deltaY * 0.00155, -0.95, 1.28);
    return;
  }
  idleGuideWheel(e);
  unlockCenteredView();
  orbit.userRadius = Math.max(orbit.minRadius, Math.min(orbit.maxRadius, orbit.userRadius + e.deltaY * 0.005));
  if (orbit.recentering) orbit.recentering = false;
}, { passive:false });

// 双击屏幕回正 — 不命中卡片时
renderer.domElement.addEventListener('dblclick', function(e){
  if (isPointerOverUi(e)) return;
  if (freeCamera && freeCamera.locked) {
    resetFreeCameraToDefault();
    resetSkullPresetView(false, { smooth:true, keepLyricLock:true });
    return;
  }
  if (shelfManager && shelfManager.getMode() !== 'off') {
    var mx = (e.clientX / innerWidth) * 2 - 1;
    var my = -(e.clientY / innerHeight) * 2 + 1;
    var rc = new THREE.Raycaster();
    rc.setFromCamera(new THREE.Vector2(mx, my), camera);
    if (shelfManager.raycastCards(rc)) return;
  }
  recenterCamera();
});


// ▼▼▼ [mineradio:dot-texture] source lines 5677,5695 ▼▼▼
// ============================================================
//  粒子点纹理 (干净圆点, 无 glow)
// ============================================================
function makeDotTexture() {
  var cv = document.createElement('canvas'); cv.width = cv.height = 64;
  var ctx = cv.getContext('2d');
  var g = ctx.createRadialGradient(32, 32, 0, 32, 32, 31);
  g.addColorStop(0.00, 'rgba(255,255,255,0.96)');
  g.addColorStop(0.42, 'rgba(255,255,255,0.78)');
  g.addColorStop(0.72, 'rgba(255,255,255,0.22)');
  g.addColorStop(1.00, 'rgba(255,255,255,0)');
  ctx.fillStyle = g;
  ctx.fillRect(0, 0, 64, 64);
  var tex = new THREE.CanvasTexture(cv);
  tex.minFilter = THREE.LinearFilter; tex.magFilter = THREE.LinearFilter;
  return tex;
}
var dotTexture = makeDotTexture();


// ▼▼▼ [mineradio:main-particles] source lines 5696,6400 ▼▼▼
// ============================================================
//  主粒子系统
//   - 5 个 preset, 每个预设走完全不同的 pos 计算
//   - 共享: 封面色采样, 鼠标交互, 粒子大小限制
// ============================================================
var PLANE_SIZE = 4.8;
var RIPPLE_MAX = 12;

var GRID_X = coverParticleGridForResolution(fx.coverResolution), GRID_Y = GRID_X;
var PCOUNT = GRID_X * GRID_Y;
var positions = null, uvs = null, aRand = null;
var coverResolutionReloadTimer = null;
var currentCoverSource = null;
var coverPickerCanvas = null;

function buildCoverParticleGeometry(grid) {
  grid = coverParticleGridForResolution(grid / 118);
  var count = grid * grid;
  var nextGeo = new THREE.BufferGeometry();
  var nextPositions = new Float32Array(count * 3);
  var nextUvs = new Float32Array(count * 2);
  var nextRand = new Float32Array(count);
  var texelStep = 1 / grid;
  for (var i = 0; i < count; i++) {
    var gx = i % grid, gy = Math.floor(i / grid);
    var u = (gx + 0.5) * texelStep, v = (gy + 0.5) * texelStep;
    var px = gx / (grid - 1), py = gy / (grid - 1);
    nextPositions[i*3]   = (px - 0.5) * PLANE_SIZE;
    nextPositions[i*3+1] = (py - 0.5) * PLANE_SIZE;
    nextPositions[i*3+2] = 0;
    nextUvs[i*2]   = u;
    nextUvs[i*2+1] = v;
    nextRand[i]   = Math.random();
  }
  nextGeo.setAttribute('position', new THREE.BufferAttribute(nextPositions, 3));
  nextGeo.setAttribute('aUv',      new THREE.BufferAttribute(nextUvs, 2));
  nextGeo.setAttribute('aRand',    new THREE.BufferAttribute(nextRand, 1));
  nextGeo.userData.grid = grid;
  nextGeo.userData.count = count;
  positions = nextPositions;
  uvs = nextUvs;
  aRand = nextRand;
  return nextGeo;
}

var geo = buildCoverParticleGeometry(GRID_X);

function applyCoverParticleResolution(value, opts) {
  opts = opts || {};
  fx.coverResolution = normalizeCoverResolution(value);
  var grid = coverParticleGridForResolution(fx.coverResolution);
  if (grid === GRID_X && geo && geo.userData && geo.userData.grid === grid) return;
  var oldGeo = geo;
  var nextGeo = buildCoverParticleGeometry(grid);
  geo = nextGeo;
  GRID_X = GRID_Y = grid;
  PCOUNT = grid * grid;
  if (particles) particles.geometry = nextGeo;
  if (bloomParticles) bloomParticles.geometry = nextGeo;
  if (oldGeo && oldGeo !== nextGeo) oldGeo.dispose();
  uniforms.uBurstAmt.value = Math.max(uniforms.uBurstAmt.value, 0.18);
  if (opts.reload !== false) scheduleCoverResolutionReload();
}

function scheduleCoverResolutionReload() {
  if (!currentCoverSource || !currentCoverSource.src) return;
  if (coverResolutionReloadTimer) clearTimeout(coverResolutionReloadTimer);
  coverResolutionReloadTimer = setTimeout(function(){
    coverResolutionReloadTimer = null;
    if (!currentCoverSource || !currentCoverSource.src) return;
    if (currentCoverSource.kind === 'url') {
      loadCoverFromUrl(currentCoverSource.src, { trackToken: trackSwitchToken, fromResolutionChange: true });
    } else if (currentCoverSource.kind === 'data') {
      applyCoverDataUrl(currentCoverSource.src, { trackToken: trackSwitchToken, fromResolutionChange: true });
    }
  }, 260);
}

// 涟漪数据纹理 (1×N, RGBA: x, y, age, str)
var rippleData = new Float32Array(RIPPLE_MAX * 4);
var rippleTex  = new THREE.DataTexture(rippleData, 1, RIPPLE_MAX, THREE.RGBAFormat, THREE.FloatType);
rippleTex.magFilter = THREE.NearestFilter; rippleTex.minFilter = THREE.NearestFilter;
var ripples = [];
for (var ri = 0; ri < RIPPLE_MAX; ri++) ripples.push({ x:0, y:0, age:-10, str:0 });

// 封面纹理 + 边缘/深度纹理
var coverTex = new THREE.Texture();
coverTex.minFilter = THREE.LinearFilter; coverTex.magFilter = THREE.LinearFilter;
coverTex.wrapS = THREE.ClampToEdgeWrapping; coverTex.wrapT = THREE.ClampToEdgeWrapping;

var coverEdgeTex = new THREE.Texture();  // R=depth, G=edge, B=fg-mask, A=lum
coverEdgeTex.minFilter = THREE.LinearFilter; coverEdgeTex.magFilter = THREE.LinearFilter;

// 初始 1×1 像素
(function(){
  var c = document.createElement('canvas'); c.width = c.height = 4;
  var x = c.getContext('2d'); x.fillStyle = '#1c1c28'; x.fillRect(0,0,4,4);
  coverTex.image = c; coverTex.needsUpdate = true;
  var d = document.createElement('canvas'); d.width = d.height = 4;
  var dx = d.getContext('2d'); dx.fillStyle = 'rgba(128,0,0,255)'; dx.fillRect(0,0,4,4);
  coverEdgeTex.image = d; coverEdgeTex.needsUpdate = true;
})();

// 前一首封面纹理 (用于切歌渐变)
var prevCoverTex = new THREE.Texture();
prevCoverTex.minFilter = THREE.LinearFilter; prevCoverTex.magFilter = THREE.LinearFilter;
(function(){
  var c = document.createElement('canvas'); c.width = c.height = 4;
  var x = c.getContext('2d'); x.fillStyle = '#1c1c28'; x.fillRect(0,0,4,4);
  prevCoverTex.image = c; prevCoverTex.needsUpdate = true;
})();

var uniforms = {
  uTime:       { value: 0 },
  uBass:       { value: 0 },
  uMid:        { value: 0 },
  uTreble:     { value: 0 },
  uBeat:       { value: 0 },
  uEnergy:     { value: 0 },
  uBurstAmt:   { value: 0 },          // 通用预设切换脉冲 0..1
  uVinylSpin:  { value: 0 },
  uPreset:     { value: 0 },
  uIntensity:  { value: 0.85 },
  uDepth:      { value: 1.0 },
  uPointScale: { value: 1.0 },
  uSpeed:      { value: 1.0 },
  uTwist:      { value: 0 },
  uColorBoost: { value: 1.1 },
  uScatter:    { value: 0 },
  uCoverRes:   { value: 1.0 },
  uBgFade:     { value: 0.20 },
  uBloomStrength:{ value: 0.62 },
  uBloomSize:  { value: 2.65 },
  uTintColor:  { value: new THREE.Color('#9db8cf') },
  uTintStrength:{ value: 0 },
  uCoverTex:   { value: coverTex },
  uPrevCoverTex:{ value: prevCoverTex },
  uColorMixT:  { value: 1.0 },        // 0=显示旧封面 → 1=显示新封面
  uEdgeTex:    { value: coverEdgeTex },
  uRippleTex:  { value: rippleTex },
  uRippleCount:{ value: 0 },
  uDotTex:     { value: dotTexture },
  uHasCover:   { value: 0 },
  uHasDepth:   { value: 0 },
  uEdgeEnabled:{ value: 1 },
  uAiBoost:    { value: 0 },          // AI 深度增益, 当 AI 接管时升至 1
  uMouseXY:    { value: new THREE.Vector2(-999, -999) },
  uMouseActive:{ value: 0 },
  uHandXY:     { value: new THREE.Vector2(-999, -999) },
  uHandActive: { value: 0 },
  uGestureGrip:{ value: 0 },
  uPixel:      { value: renderer.getPixelRatio() },
  uAlpha:      { value: 0 },          // 整体粒子透明度 (启动 fade-in)
  uParticleDim:{ value: 1 },          // 覆盖层打开时只压低粒子背景, 不影响 3D 卡片
  uFloatAlpha: { value: 0 },          // 空场/浮空粒子透明度
  uLoading:    { value: 0 },          // 加载动画混合度 0..1 (1 = 完全聚成圆环)
};
installRenderPowerHooks();
applyRendererPowerMode();

// ----- 顶点 Shader -----
//   v7.1: 律动幅度 ×2.5, Tunnel 自旋, 虚空预设, 切歌颜色渐变
var vs = `
precision highp float;
uniform float uTime, uBass, uMid, uTreble, uBeat, uEnergy, uBurstAmt;
uniform float uPreset, uIntensity, uDepth, uPointScale, uSpeed, uTwist;
uniform float uVinylSpin;
uniform float uColorBoost, uScatter, uCoverRes, uBgFade;
uniform float uHasCover, uHasDepth, uEdgeEnabled, uAiBoost;
uniform float uMouseActive, uPixel, uColorMixT, uLoading;
uniform sampler2D uCoverTex, uPrevCoverTex, uEdgeTex, uRippleTex;
uniform int uRippleCount;
uniform vec2 uMouseXY, uHandXY;
uniform float uHandActive, uGestureGrip;
uniform vec3 uTintColor;
uniform float uTintStrength;
attribute vec2 aUv;
attribute float aRand;
varying vec3 vColor;
varying float vBright, vRipple, vEdgeBoost, vAlpha, vSourceLum;

#define PI 3.14159265359

vec3 mod289(vec3 x){return x-floor(x*(1.0/289.0))*289.0;}
vec4 mod289v(vec4 x){return x-floor(x*(1.0/289.0))*289.0;}
vec4 perm(vec4 x){return mod289v(((x*34.0)+1.0)*x);}
float snoise(vec3 v){
  const vec2 C=vec2(1.0/6.0,1.0/3.0);
  const vec4 D=vec4(0.0,0.5,1.0,2.0);
  vec3 i=floor(v+dot(v,C.yyy));
  vec3 x0=v-i+dot(i,C.xxx);
  vec3 g=step(x0.yzx,x0.xyz); vec3 l=1.0-g;
  vec3 i1=min(g.xyz,l.zxy); vec3 i2=max(g.xyz,l.zxy);
  vec3 x1=x0-i1+C.xxx;
  vec3 x2=x0-i2+C.yyy;
  vec3 x3=x0-D.yyy;
  i=mod289(i);
  vec4 p=perm(perm(perm(i.z+vec4(0.0,i1.z,i2.z,1.0))+i.y+vec4(0.0,i1.y,i2.y,1.0))+i.x+vec4(0.0,i1.x,i2.x,1.0));
  float n_=0.142857142857;
  vec3 ns=n_*D.wyz-D.xzx;
  vec4 j=p-49.0*floor(p*ns.z*ns.z);
  vec4 x_=floor(j*ns.z); vec4 y_=floor(j-7.0*x_);
  vec4 x=x_*ns.x+ns.yyyy; vec4 y=y_*ns.x+ns.yyyy;
  vec4 h=1.0-abs(x)-abs(y);
  vec4 b0=vec4(x.xy,y.xy); vec4 b1=vec4(x.zw,y.zw);
  vec4 s0=floor(b0)*2.0+1.0; vec4 s1=floor(b1)*2.0+1.0;
  vec4 sh=-step(h,vec4(0.0));
  vec4 a0=b0.xzyw+s0.xzyw*sh.xxyy; vec4 a1=b1.xzyw+s1.xzyw*sh.zzww;
  vec3 p0=vec3(a0.xy,h.x); vec3 p1=vec3(a0.zw,h.y); vec3 p2=vec3(a1.xy,h.z); vec3 p3=vec3(a1.zw,h.w);
  vec4 norm=inversesqrt(vec4(dot(p0,p0),dot(p1,p1),dot(p2,p2),dot(p3,p3)));
  p0*=norm.x; p1*=norm.y; p2*=norm.z; p3*=norm.w;
  vec4 m=max(0.6-vec4(dot(x0,x0),dot(x1,x1),dot(x2,x2),dot(x3,x3)),0.0);
  m=m*m;
  return 42.0*dot(m*m,vec4(dot(p0,x0),dot(p1,x1),dot(p2,x2),dot(p3,x3)));
}

float hash11(float p) {
  return fract(sin(p * 127.1) * 43758.5453123);
}

vec2 safeCoverUv(vec2 uv) {
  return clamp(uv, vec2(0.0012), vec2(0.9988));
}

vec3 sampleNewCoverColor(vec2 uv) {
  return texture2D(uCoverTex, safeCoverUv(uv)).rgb;
}

vec3 samplePrevCoverColor(vec2 uv) {
  return texture2D(uPrevCoverTex, safeCoverUv(uv)).rgb;
}

vec4 sampleEdgeColor(vec2 uv) {
  return texture2D(uEdgeTex, safeCoverUv(uv));
}

float rippleSumAt(vec2 p, out float maxAmp) {
  float sum = 0.0; maxAmp = 0.0;
  for (int ri = 0; ri < 12; ri++) {
    if (ri >= uRippleCount) break;
    float vCoord = (float(ri) + 0.5) / 12.0;
    vec4 rd = texture2D(uRippleTex, vec2(0.5, vCoord));
    float age = rd.z; float str = rd.w;
    if (str < 0.005 || age < 0.0 || age > 2.0) continue;
    float dx = p.x - rd.x, dy = p.y - rd.y;
    float dist = sqrt(dx*dx + dy*dy);
    float lifeN = age / 2.0;
    float fadeIn  = smoothstep(0.0, 0.06, age);
    float fadeOut = 1.0 - smoothstep(0.7, 1.0, lifeN);
    float env = fadeIn * fadeOut;
    // v7.1: 把幅度放大 — 中心凸起更高更宽
    float bulgeW = 0.55 + age * 0.80;
    float bulge  = exp(-dist*dist / (2.0 * bulgeW * bulgeW)) * (1.0 - smoothstep(0.0, 0.55, lifeN));
    float waveR  = age * 2.10;
    float ringW  = 0.40 + age * 0.22;
    float ring   = exp(-pow((dist - waveR) / ringW, 2.0));
    // v7.1: 提升整体幅度 ×2
    float local  = (bulge * 2.4 + ring * 1.30) * env * str;
    sum += local;
    maxAmp = max(maxAmp, abs(local));
  }
  return sum;
}

void main(){
  float t = uTime * uSpeed;
  vec3 pos;
  vec2 sampleUv = safeCoverUv(aUv);
  // 切歌颜色渐变: 在新旧封面间 mix
  vec3 newCol = sampleNewCoverColor(sampleUv);
  vec3 prevCol = samplePrevCoverColor(sampleUv);
  vec3 coverColor = mix(prevCol, newCol, clamp(uColorMixT, 0.0, 1.0));
  vec4 edge = sampleEdgeColor(sampleUv);
  float depthVal = edge.r;
  float edgeVal  = edge.g;
  float fgMask   = edge.b;
  float lumVal   = edge.a;
  float maxRippleAmp = 0.0;
  float rippleZ = 0.0;

  vec3 defaultColor = mix(
    vec3(0.36, 0.28, 0.72),
    mix(vec3(0.85, 0.55, 0.95), vec3(0.45, 0.78, 0.95), aUv.x),
    aUv.y
  );
  vColor = mix(defaultColor, coverColor, uHasCover);
  vAlpha = 1.0;

  // 律动强度的真实倍数 (放大 intensity 滑块的影响)
  float K = uIntensity * 1.6;   // 滑块 1.0 → K=1.6, 滑块 1.6 → K=2.56

  // ====================================================
  //  Preset 0: SILK — 丝绸 (xy 平面, z 涟漪)
  //  v7.1: 全部位移 ×2.5
  // ====================================================
  if (uPreset < 0.5) {
    pos = position;
    rippleZ = rippleSumAt(pos.xy, maxRippleAmp);

    float midN = snoise(vec3(pos.x*1.4, pos.y*1.4, t*0.55)) * 0.6
               + snoise(vec3(pos.x*2.8+5.0, pos.y*2.8-3.0, t*0.85)) * 0.4;
    float midMask = 0.55 + 0.45 * snoise(vec3(pos.x*0.4, pos.y*0.4, t*0.18));
    float midDisp = midN * uMid * 0.55 * midMask * K;       // 0.20 → 0.55

    float trebleJ = snoise(vec3(pos.x*6.5, pos.y*6.5, t*3.5 + aRand*4.0)) * uTreble * 0.18 * K;  // 0.06→0.18
    float bassBreath = snoise(vec3(pos.x*0.35, pos.y*0.35, t*0.4)) * uBass * 0.42 * K;          // 0.14→0.42

    // AI 深度: 显著强化 (0.85 → 1.4)
    float depthZ = (depthVal - 0.5) * uAiBoost * uDepth * 1.40 * uHasDepth;

    pos.z = rippleZ * 1.30 + midDisp + trebleJ + bassBreath + depthZ;
  }

  // ====================================================
  //  Preset 1: TUNNEL — 隧道 + 自旋
  // ====================================================
  else if (uPreset < 1.5) {
    // v7.1: 整体自旋 — 整管缓慢绕 Z 轴
    float spin = t * 0.12;
    float angle = aUv.x * 2.0 * PI + spin;
    float flow = aUv.y - t * 0.08 * (1.0 + uBass * 0.55);
    flow = fract(flow);
    float zPos = (flow - 0.5) * 9.0;
    float baseR = 2.0 - uBass * 0.28 * K;                  // bass 收缩更明显
    float ripG  = sin(angle * 5.0 + zPos * 1.4 + t * 2.2) * 0.10 * (uMid + uTreble) * K;   // 0.04→0.10
    float r = baseR + ripG;
    pos.x = cos(angle) * r;
    pos.y = sin(angle) * r;
    pos.z = zPos;

    sampleUv = vec2(aUv.x, flow);
    sampleUv = safeCoverUv(sampleUv);
    newCol = sampleNewCoverColor(sampleUv);
    prevCol = samplePrevCoverColor(sampleUv);
    coverColor = mix(prevCol, newCol, clamp(uColorMixT, 0.0, 1.0));
    vColor = mix(defaultColor, coverColor, uHasCover);

    float depthFade = smoothstep(-4.5, 4.5, zPos);
    vColor *= 0.4 + depthFade * 0.7;
  }

  // ====================================================
  //  Preset 2: ORBIT — 星球 (保留自转)
  //  v7.1: 律动幅度加大
  // ====================================================
  else if (uPreset < 2.5) {
    float theta = aUv.x * 2.0 * PI;
    float phi   = (aUv.y - 0.5) * PI;
    float baseR = 2.2;
    float trebFlare = snoise(vec3(theta * 1.5, phi * 1.5, t * 0.7)) * uTreble * 0.85 * K;   // 0.40→0.85
    float bassExpand = uBass * 0.35 * K;                                                      // 0.18→0.35
    float r = baseR * (1.0 + bassExpand) + trebFlare;

    pos.x = r * cos(phi) * cos(theta);
    pos.y = r * sin(phi);
    pos.z = r * cos(phi) * sin(theta);

    float yaw = t * 0.18;
    float cy = cos(yaw), sy = sin(yaw);
    pos.xz = mat2(cy, -sy, sy, cy) * pos.xz;
  }

  // ====================================================
  //  Preset 3: VOID — 虚空 (无粒子, 适合自定义背景)
  // ====================================================
  else if (uPreset < 3.5) {
    pos = vec3((aUv.x - 0.5) * 0.01, (aUv.y - 0.5) * 0.01, -90.0);
    vAlpha = 0.0;
    vColor = vec3(0.0);
    maxRippleAmp = 0.0;
  }

  // ====================================================
  //  Preset 4: VINYL RECORD
  //  A real record layout: circular album cover in the center, black vinyl
  //  grooves outside, and a complete white particle rim.
  // ====================================================
  else if (uPreset < 4.5) {
    float bassDrive = smoothstep(0.08, 0.78, uBass + uBeat * 0.82);
    float highDrive = smoothstep(0.05, 0.46, uTreble);
    float hiResGuard = smoothstep(1.08, 1.55, uCoverRes);
    float edgeGuard = mix(1.0, 0.38, hiResGuard);
    float depthGuard = mix(1.0, 0.44, hiResGuard);
    float grooveGuard = mix(1.0, 0.48, hiResGuard);
    float beatGuard = mix(1.0, 0.36, hiResGuard);

    vec2 p = (aUv - 0.5) * 5.12;
    float spin = uVinylSpin;
    float cs = cos(spin), sn = sin(spin);
    vec2 rp = mat2(cs, -sn, sn, cs) * p;
    float d = length(p);
    float angle0 = atan(p.y, p.x);
    float recordR = 2.46;
    float coverR = 1.18;
    float recordAlpha = 1.0 - smoothstep(recordR - 0.02, recordR + 0.05, d);
    float coverMask = 1.0 - smoothstep(coverR - 0.012, coverR + 0.018, d);
    float border = exp(-pow((d - coverR) / 0.064, 2.0)) * edgeGuard;
    float outerRim = exp(-pow((d - (recordR - 0.050)) / 0.055, 2.0)) * edgeGuard;
    float vinylN = clamp((d - coverR) / max(0.001, recordR - coverR), 0.0, 1.0);

    pos = vec3(rp * (1.0 + bassDrive * 0.012 * beatGuard + uBeat * 0.026 * beatGuard), 0.0);
    vAlpha = recordAlpha;

    if (coverMask > 0.02) {
      vec2 coverUv = p / (coverR * 2.0) + 0.5;
      newCol = sampleNewCoverColor(coverUv);
      prevCol = samplePrevCoverColor(coverUv);
      coverColor = mix(prevCol, newCol, clamp(uColorMixT, 0.0, 1.0));
      if (hiResGuard > 0.001) {
        vec2 sx = vec2(0.0026, 0.0);
        vec2 sy = vec2(0.0, 0.0026);
        vec3 softNew = (sampleNewCoverColor(coverUv + sx) + sampleNewCoverColor(coverUv - sx) + sampleNewCoverColor(coverUv + sy) + sampleNewCoverColor(coverUv - sy)) * 0.25;
        vec3 softPrev = (samplePrevCoverColor(coverUv + sx) + samplePrevCoverColor(coverUv - sx) + samplePrevCoverColor(coverUv + sy) + samplePrevCoverColor(coverUv - sy)) * 0.25;
        coverColor = mix(coverColor, mix(softPrev, softNew, clamp(uColorMixT, 0.0, 1.0)), hiResGuard * 0.42);
      }
      vColor = mix(defaultColor, coverColor, uHasCover);
      float coverShade = 1.02 + 0.10 * (1.0 - smoothstep(0.0, coverR, d));
      vColor *= coverShade;
      vColor = mix(vColor, vec3(1.0), border * 0.54);
      pos.z = 0.040 + border * 0.026 * depthGuard + uBeat * 0.018 * beatGuard;
      maxRippleAmp = max(maxRippleAmp, border * 0.30 + bassDrive * 0.075 * beatGuard + uBeat * 0.075 * beatGuard);
    } else {
      float groove = 0.5 + 0.5 * sin((d - coverR) * mix(98.0, 58.0, hiResGuard));
      float fineGroove = 0.5 + 0.5 * sin((d - coverR) * mix(170.0, 92.0, hiResGuard) + aRand * 3.0);
      float tick = smoothstep(0.82, 0.995, hash11(floor((angle0 + PI) * 38.0) + floor(d * 72.0) * 2.1));
      vec3 vinyl = vec3(0.052, 0.054, 0.058) + vec3(0.052 * grooveGuard) * groove + vec3(0.026 * grooveGuard) * fineGroove;
      vinyl = mix(vinyl, coverColor * 0.32, 0.18 * (1.0 - vinylN));
      float whiteRing = max(border * 0.92, outerRim * 0.26);
      vColor = mix(vinyl, vec3(0.92, 0.94, 0.94), whiteRing);
      vColor = mix(vColor, vec3(1.0), tick * highDrive * (0.06 + border * 0.12) * grooveGuard);
      pos.z = groove * 0.010 * grooveGuard + border * 0.024 * depthGuard + bassDrive * vinylN * 0.016 * K * beatGuard + tick * highDrive * 0.010 * grooveGuard;
      maxRippleAmp = max(maxRippleAmp, border * 0.32 + outerRim * 0.12 + bassDrive * vinylN * 0.11 * beatGuard + tick * highDrive * 0.10 * grooveGuard + uBeat * vinylN * 0.08 * beatGuard);
    }
  }

  // ====================================================
  //  Preset 5: WALLPAPER PULSE
  //  Layered music-particle wallpaper: aurora ribbons, depth sparks,
  //  and cover-colored audio flow.
  // ====================================================
  else {
    float bassGlow = smoothstep(0.07, 0.78, uBass) * 0.34 + uBeat * 0.014;
    float midGlow = smoothstep(0.07, 0.62, uMid) * 0.42;
    float highGlow = smoothstep(0.04, 0.46, uTreble) * 0.46;
    float lane = aUv.y;
    float transition = clamp(uBurstAmt, 0.0, 1.0);

    if (lane < 0.80) {
      float laneWarp = snoise(vec3(aUv.x * 0.42, lane * 1.7, t * 0.026)) * 0.11 + (hash11(aRand * 73.1) - 0.5) * 0.045;
      float warpedLane = clamp(lane + laneWarp, 0.0, 0.80);
      float bandCoord = warpedLane / 0.80 * 5.65 + snoise(vec3(aUv.x * 0.82, lane * 2.25, t * 0.032)) * 0.62;
      float band = floor(bandCoord);
      float local = fract(bandCoord + hash11(band * 9.13 + aRand * 2.4) * 0.18);
      float bandN = clamp((band + 0.5) / 5.65, 0.0, 1.0);
      float seed = hash11(band * 19.17 + aRand * 31.0);
      float flow = fract(aUv.x + t * (0.0034 + bandN * 0.0038 + seed * 0.0022) + seed * 0.53);
      float arc = (flow - 0.5) * PI * (1.35 + bandN * 0.72 + seed * 0.24);
      float armCurve = sin(arc + bandN * 2.2 + seed * 5.3);
      float spiralRadius = 9.2 + bandN * 11.8 + seed * 6.0 + local * 2.9;
      float x = cos(arc * 0.72 + bandN * 0.92 + seed * 1.3) * spiralRadius + (flow - 0.5) * (13.5 + bandN * 9.5);
      float ribbonPhase = flow * PI * 2.0 * (0.55 + bandN * 0.24 + seed * 0.10) + t * (0.010 + bandN * 0.007) + seed * 5.7;
      float broadWave = sin(ribbonPhase) * 0.92;
      float fineWave = sin(ribbonPhase * (1.36 + seed * 0.62) - t * 0.044 + seed * 5.0) * 0.045;
      float yBase = (bandN - 0.5) * 13.2 + armCurve * (2.3 + bandN * 1.6) + (seed - 0.5) * 1.85 + snoise(vec3(bandN * 2.0, flow * 0.62, seed)) * 0.92;
      float ridgeCenter = 0.43 + (seed - 0.5) * 0.18;
      float ridge = exp(-pow((local - ridgeCenter) / (0.25 + seed * 0.04), 2.0));
      float softMask = smoothstep(0.010, 0.12, lane) * (1.0 - smoothstep(0.72, 0.81, lane));
      float ribbonNoise = snoise(vec3(flow * 1.18 + seed, bandN * 2.0, t * 0.018)) * 0.74;
      float zLayer = mix(-23.5, 15.5, bandN) + (seed - 0.5) * 6.0;

      pos.x = x + ribbonNoise * 1.40 + sin(t * 0.012 + seed * 8.0) * 0.22;
      pos.y = yBase + broadWave + fineWave + (local - 0.5) * (0.58 + ridge * 0.14);
      pos.z = zLayer + broadWave * 1.35 + ribbonNoise * 1.85;

      float pulseLine = 0.5 + 0.5 * sin(ribbonPhase * (1.7 + seed * 0.9) - t * 0.32 + seed * 6.0);
      vec3 aurora = mix(vec3(0.52, 0.86, 1.0), vec3(0.70, 0.58, 1.0), bandN);
      aurora = mix(aurora, vec3(0.96, 0.98, 0.92), bassGlow * 0.05);
      vAlpha = (0.18 + ridge * 0.78 + pulseLine * highGlow * 0.035 + bassGlow * 0.025) * softMask * (0.96 + transition * 0.02);
      vColor = mix(coverColor, aurora, 0.62 + ridge * 0.22) * (0.76 + ridge * 0.86 + pulseLine * highGlow * 0.05 + bassGlow * 0.04);
      maxRippleAmp = max(maxRippleAmp, ridge * (0.12 + midGlow * 0.05) + pulseLine * highGlow * 0.045 + bassGlow * 0.030);
    } else {
      float q = (lane - 0.80) / 0.20;
      float seed = hash11(aRand * 917.0 + floor(q * 130.0));
      float depth = mix(-32.0, 18.0, seed);
      float drift = fract(aUv.x + t * (0.0014 + seed * 0.0048) + seed * 0.63);
      float cluster = snoise(vec3(seed * 2.0, q * 3.2, t * 0.007));
      float x = (drift - 0.5) * (45.0 + seed * 22.0) + cluster * 3.4;
      float y = (hash11(aRand * 331.0 + seed * 5.0) - 0.5) * 22.0 + sin(t * (0.018 + seed * 0.028) + seed * 7.0) * 0.86;
      float z = depth + sin(t * (0.020 + seed * 0.032) + aRand * 8.0) * 1.05;
      float twinkle = pow(0.5 + 0.5 * sin(t * (0.24 + seed * 0.42) + aRand * 17.0), 5.0);
      float dust = smoothstep(0.22, 0.98, hash11(aRand * 661.0 + floor(q * 160.0)));

      pos = vec3(x, y, z);
      vAlpha = dust * (0.16 + twinkle * 0.46 + highGlow * 0.025 + bassGlow * 0.018) * (1.0 - q * 0.06);
      vColor = mix(coverColor, vec3(0.92, 0.97, 1.0), 0.62 + twinkle * 0.14) * (0.72 + twinkle * 0.62 + bassGlow * 0.025);
      maxRippleAmp = max(maxRippleAmp, twinkle * highGlow * 0.055 + dust * bassGlow * 0.030);
    }

    if (transition > 0.001) {
      float bloom = smoothstep(0.0, 1.0, transition);
      vec2 burstVec = pos.xy + vec2(hash11(aRand * 31.0) - 0.5, hash11(aRand * 47.0) - 0.5) * 0.75;
      vec2 burstDir = burstVec / max(length(burstVec), 0.001);
      pos.xy += burstDir * bloom * 0.026;
      pos.xy += vec2(snoise(vec3(aRand, t * 0.014, 1.0)), snoise(vec3(aRand, t * 0.014, 5.0))) * bloom * 0.06;
      pos.xy *= 1.0 + bloom * 0.014;
      pos.z += (hash11(aRand * 123.0) - 0.5) * bloom * 0.18;
      vAlpha *= 0.86 + bloom * 0.22;
      maxRippleAmp = max(maxRippleAmp, bloom * 0.10);
    }
  }

  // ====================================================
  //  鼠标交互 (仅 SILK)
  // ====================================================
  if (uMouseActive > 0.5 && uPreset < 0.5) {
    float mdx = pos.x - uMouseXY.x;
    float mdy = pos.y - uMouseXY.y;
    float md = sqrt(mdx*mdx + mdy*mdy);
    if (md < 1.0) {
      float push = (1.0 - md) * (1.0 - md);
      pos.z += push * 0.55;
    }
  }

  // ====================================================
  //  v8 手势遮挡 — uHandActive 是 0..1 平滑过渡, 大半径推开
  // ====================================================
  if (uHandActive > 0.01) {
    float hdx = pos.x - uHandXY.x;
    float hdy = pos.y - uHandXY.y;
    float hd = sqrt(hdx*hdx + hdy*hdy);
    float rad = 1.55;
    if (hd < rad) {
      float push = (rad - hd) / rad;
      push = push * push * uHandActive;
      pos.z += push * 1.10;
      vec2 outDir = vec2(hdx, hdy) / max(0.001, hd);
      pos.xy += outDir * push * 0.28;
    }
  }
  if (uGestureGrip > 0.001) {
    float grip = clamp(uGestureGrip, 0.0, 1.0);
    float gripWave = 0.5 + 0.5 * sin(uTime * 2.2 + aRand * 6.2831);
    pos.xy *= mix(1.0, 0.66 + gripWave * 0.035, grip);
    pos.z += grip * (0.18 + uBass * 0.22 + gripWave * 0.10);
  }

  // ====================================================
  //  通用: 离散感 / 扭曲
  // ====================================================
  if (uScatter > 0.001) {
    vec2 jdir = vec2(cos(aRand * 6.2831), sin(aRand * 6.2831));
    pos.xy += jdir * uScatter * (0.05 + uTreble * 0.10);
  }
  if (uTwist > 0.001 && uPreset < 0.5) {
    float ta = uTwist * pos.z * 0.6;
    float cs = cos(ta), sn = sin(ta);
    pos.xy = mat2(cs, -sn, sn, cs) * pos.xy;
  }

  // 颜色
  float vinylHiResGuard = smoothstep(1.08, 1.55, uCoverRes) * step(3.5, uPreset) * (1.0 - step(4.5, uPreset));
  float edgeBoost = uEdgeEnabled * edgeVal * mix(1.0, 0.42, vinylHiResGuard);
  vSourceLum = dot(max(vColor, vec3(0.0)), vec3(0.299, 0.587, 0.114));
  float blackParticleGuard = 1.0 - smoothstep(0.025, 0.115, vSourceLum);
  vEdgeBoost = edgeBoost * (uPreset > 3.5 ? 0.22 : 1.0) * (1.0 - blackParticleGuard);
  vColor = pow(max(vColor, vec3(0.0)), vec3(1.0 / max(0.35, uColorBoost)));
  float edgeColorMix = edgeBoost * (uPreset > 3.5 ? 0.20 : 0.50) * (1.0 - blackParticleGuard);
  vColor = mix(vColor, vColor + vec3(0.20), edgeColorMix);
  float tintLum = max(max(vColor.r, vColor.g), vColor.b);
  vec3 tintedColor = uTintColor * max(0.24, tintLum * 1.12);
  vColor = mix(vColor, tintedColor, clamp(uTintStrength, 0.0, 1.0) * (1.0 - blackParticleGuard));

  vBright = 0.82 + maxRippleAmp * 0.55 + uBass * 0.10 + edgeBoost * 0.30 + uEnergy * 0.05 + uBurstAmt * 0.40;
  if (uPreset > 4.5) {
    vBright = 0.94 + maxRippleAmp * 0.34 + uBass * 0.020 + uEnergy * 0.026 + uBurstAmt * 0.025;
  } else if (uPreset > 3.5) {
    vBright = 0.94 + maxRippleAmp * 0.64 + uBass * 0.08 + edgeBoost * 0.12 + uEnergy * 0.05 + uBeat * 0.16 + uBurstAmt * 0.16;
  }
  vRipple = clamp(maxRippleAmp * 1.5, 0.0, 1.0);

  if (uHasDepth > 0.5 && uPreset < 0.5) {
    float bgMul = mix(1.0, 0.55, uBgFade * (1.0 - fgMask));
    vBright *= bgMul;
  }
  vBright += uGestureGrip * 0.22;
  float loadingMistSize = 1.0;

  // 加载形态: 雾状微尘流，避免廉价旋转圆环
  if (uLoading > 0.001) {
    float mistSeed = hash11(aRand * 931.7);
    float mistLayer = floor(mistSeed * 4.0);
    float layerN = (mistLayer + 0.5) / 4.0;
    float mistAngle = aRand * 6.2831 + uTime * (0.16 + mistSeed * 0.18) + snoise(vec3(aRand * 2.1, uTime * 0.24, 2.0)) * 1.85;
    float mistR = mix(1.35, 3.15, sqrt(hash11(aRand * 127.3))) * (1.0 + sin(uTime * 0.42 + aRand * 7.0) * 0.13);
    vec2 mistCurl = vec2(
      snoise(vec3(aRand * 4.1, uTime * 0.32, 3.0)),
      snoise(vec3(aRand * 4.7, uTime * 0.30, 8.0))
    );
    float mistBreath = 0.5 + 0.5 * sin(uTime * (0.82 + mistSeed * 0.55) + aRand * 17.0);
    float mistRibbon = sin(mistAngle * (1.35 + layerN * 0.55) + uTime * 0.34 + mistSeed * 4.0);
    float glowPick = smoothstep(0.88, 0.997, hash11(aRand * 1501.0 + mistLayer * 17.0));
    float dustPick = 0.34 + glowPick * 0.66;
    vec3 mistPos = vec3(
      cos(mistAngle) * mistR * (1.24 + mistCurl.x * 0.16) + mistCurl.x * 0.72,
      sin(mistAngle * 0.82 + mistRibbon * 0.25) * mistR * (0.56 + layerN * 0.10) + mistCurl.y * 0.62,
      (layerN - 0.5) * 4.85 + mistCurl.x * 0.56 + mistBreath * 0.36 + mistRibbon * 0.24
    );
    vec3 mistCol = mix(vec3(0.62, 0.86, 0.84), vec3(0.36, 0.46, 0.78), mistSeed);
    mistCol = mix(mistCol, vec3(0.94, 1.0, 0.97), glowPick * (0.45 + mistBreath * 0.35));
    vColor = mix(vColor, mistCol, uLoading * 0.78);
    vBright = mix(vBright, 0.20 + mistBreath * 0.18 + abs(mistCurl.x) * 0.06 + glowPick * (0.72 + abs(mistRibbon) * 0.24), uLoading);
    vAlpha = mix(vAlpha, 0.08 + mistBreath * 0.11 + dustPick * 0.11 + glowPick * 0.30, uLoading);
    pos = mix(pos, mistPos, uLoading);
    loadingMistSize = 1.26 + mistBreath * 0.24 + abs(mistRibbon) * 0.14 + glowPick * 0.78;
  }

  vec4 mvPos = modelViewMatrix * vec4(pos, 1.0);
  float depthSize = 36.0 / max(0.5, -mvPos.z);
  float audioBoost = 1.0 + maxRippleAmp * 0.7 + edgeBoost * 0.55 + uBeat * 0.30 + uBurstAmt * 0.5;
  float sz = clamp(depthSize * audioBoost, 1.05, 4.95);
  if (uPreset > 4.5) {
    float flowDrive = uBass * 0.070 + uMid * 0.046 + uTreble * 0.060 + uBurstAmt * 0.090 + uBeat * 0.055;
    sz = clamp(depthSize * (1.05 + flowDrive), 1.00, 5.45);
  } else if (uPreset > 3.5) {
    float ringDrive = uBass * 0.30 + uMid * 0.18 + uTreble * 0.22 + uBeat * 0.30;
    sz = clamp(depthSize * (0.90 + ringDrive * 0.62), 1.05, 3.90);
  }
  // 加载态下粒子稍大
  sz = mix(sz, sz * loadingMistSize, uLoading);
  gl_PointSize = sz * uPixel * uPointScale;
  gl_Position = projectionMatrix * mvPos;
}
`;

// ----- 片元 Shader -----
var fs = `
precision highp float;
uniform sampler2D uDotTex;
uniform float uAlpha, uPreset, uParticleDim;
varying vec3 vColor;
varying float vBright, vRipple, vEdgeBoost, vAlpha, vSourceLum;

void main(){
  vec4 tex = texture2D(uDotTex, gl_PointCoord);
  if (tex.a < 0.02) discard;
  vec3 col = vColor * vBright;
  col = mix(col, col * 1.3 + vec3(0.05), vEdgeBoost * 0.35);
  col = mix(col, col * 1.2, vRipple * 0.4);
  float keepBlack = 1.0 - smoothstep(0.025, 0.115, vSourceLum);
  float nonBlack = 1.0 - keepBlack;
  float dotDist = length(gl_PointCoord - vec2(0.5)) * 2.0;
  float readableRim = smoothstep(0.44, 0.94, dotDist) * (1.0 - smoothstep(0.94, 1.08, dotDist)) * tex.a;
  float outLum = dot(col, vec3(0.299, 0.587, 0.114));
  float lightParticle = smoothstep(0.50, 0.82, outLum) * nonBlack;
  float darkParticle = (1.0 - smoothstep(0.20, 0.50, outLum)) * nonBlack;
  col = mix(col, vec3(0.0), readableRim * lightParticle * 0.38);
  col = mix(col, vec3(1.0), readableRim * darkParticle * 0.20);
  col = clamp(col, vec3(0.0), vec3(1.6));
  gl_FragColor = vec4(col, tex.a * uAlpha * uParticleDim * vAlpha);
}
`;

var material = new THREE.ShaderMaterial({
  uniforms: uniforms, vertexShader: vs, fragmentShader: fs,
  transparent: true, depthWrite: false, blending: THREE.NormalBlending,
});

var bloomVs = vs
  .replace('uniform float uMouseActive, uPixel, uColorMixT, uLoading;', 'uniform float uMouseActive, uPixel, uColorMixT, uLoading, uBloomSize;')
  .replace('gl_PointSize = sz * uPixel * uPointScale;', 'gl_PointSize = sz * uPixel * uPointScale * uBloomSize;');
var bloomFs = `
precision highp float;
uniform sampler2D uDotTex;
uniform float uAlpha, uBloomStrength, uPreset, uParticleDim;
varying vec3 vColor;
varying float vBright, vRipple, vEdgeBoost, vAlpha, vSourceLum;

void main(){
  vec4 tex = texture2D(uDotTex, gl_PointCoord);
  if (tex.a < 0.01) discard;
  float soft = tex.a * tex.a;
  vec3 col = vColor * (0.55 + vBright * 0.62);
  col = mix(col, col + vec3(0.22, 0.18, 0.10), vEdgeBoost * 0.35);
  col = clamp(col, vec3(0.0), vec3(1.8));
  float pulse = 1.0 + vRipple * 0.65;
  float keepBlack = 1.0 - smoothstep(0.025, 0.115, vSourceLum);
  float bloomKeep = 1.0 - keepBlack * 0.92;
  gl_FragColor = vec4(col, soft * uAlpha * uBloomStrength * uParticleDim * pulse * 0.55 * vAlpha * bloomKeep);
}
`;
var bloomMaterial = new THREE.ShaderMaterial({
  uniforms: uniforms, vertexShader: bloomVs, fragmentShader: bloomFs,
  transparent: true, depthWrite: false, depthTest: false, blending: THREE.AdditiveBlending,
});
var bloomParticles = new THREE.Points(geo, bloomMaterial);
bloomParticles.frustumCulled = false;
bloomParticles.renderOrder = 0;
scene.add(bloomParticles);
var particles = new THREE.Points(geo, material);
particles.frustumCulled = false;
particles.renderOrder = 1;
scene.add(particles);
console.log('v7 shell loaded, JS pending');


// ▼▼▼ [mineradio:float-layer] source lines 6401,6520 ▼▼▼
// ============================================================
//  浮空粒子层 (独立 Points)
//   v7.1: 速度大幅放慢, 改用 sin/cos 长周期漂移 (优雅而非乱飞)
// ============================================================
var FLOAT_COUNT = 1300;
var floatGroup = null;
var floatPositionsArr = null, floatBaseArr = null, floatPhaseArr = null, floatColorArr = null;

function createFloatLayer() {
  fx.floatLayer = false;
  uniforms.uFloatAlpha.value = 0;
  if (floatGroup) destroyFloatLayer();
  return;
  if (floatGroup) return;
  var fgeo = new THREE.BufferGeometry();
  floatPositionsArr = new Float32Array(FLOAT_COUNT * 3);
  floatBaseArr      = new Float32Array(FLOAT_COUNT * 3);  // 基准位置
  floatPhaseArr     = new Float32Array(FLOAT_COUNT * 3);  // 每粒子相位 (0..2π)
  floatColorArr     = new Float32Array(FLOAT_COUNT * 3);
  var floatRandArr  = new Float32Array(FLOAT_COUNT);
  var floatAmpArr   = new Float32Array(FLOAT_COUNT);      // 漂移幅度 (0.15-0.45)
  for (var i = 0; i < FLOAT_COUNT; i++) {
    var halo = i < FLOAT_COUNT * 0.76;
    var bx, by, bz;
    if (halo) {
      var a = Math.random() * Math.PI * 2;
      var r = 0.62 + Math.pow(Math.random(), 0.72) * 2.75;
      var lane = (Math.random() - 0.5) * 0.62;
      bx = Math.cos(a) * r;
      by = Math.sin(a) * r * 0.54 + lane;
      bz = (Math.random() - 0.5) * 2.4 - 0.25;
    } else {
      bx = (Math.random() - 0.5) * 8.4;
      by = (Math.random() - 0.5) * 5.8;
      bz = (Math.random() - 0.5) * 5.6;
    }
    floatBaseArr[i*3]   = bx; floatBaseArr[i*3+1] = by; floatBaseArr[i*3+2] = bz;
    floatPositionsArr[i*3]   = bx;
    floatPositionsArr[i*3+1] = by;
    floatPositionsArr[i*3+2] = bz;
    floatPhaseArr[i*3]   = Math.random() * Math.PI * 2;
    floatPhaseArr[i*3+1] = Math.random() * Math.PI * 2;
    floatPhaseArr[i*3+2] = Math.random() * Math.PI * 2;
    floatAmpArr[i] = 0.15 + Math.random() * 0.35;
    var white = 0.88 + Math.random() * 0.12;
    floatColorArr[i*3]   = white;
    floatColorArr[i*3+1] = white;
    floatColorArr[i*3+2] = white;
    floatRandArr[i] = Math.random();
  }
  fgeo.setAttribute('position', new THREE.BufferAttribute(floatPositionsArr, 3));
  fgeo.setAttribute('aColor',   new THREE.BufferAttribute(floatColorArr, 3));
  fgeo.setAttribute('aRand',    new THREE.BufferAttribute(floatRandArr, 1));

  // 把 amp + phase 存到 attribute 让 shader 端做漂移 (避免 JS 每帧改 buffer)
  fgeo.setAttribute('aAmp',     new THREE.BufferAttribute(floatAmpArr, 1));
  fgeo.setAttribute('aPhase',   new THREE.BufferAttribute(floatPhaseArr, 3));

  var fvs = `
    precision highp float;
    uniform float uTime, uBass, uPixel, uFloatAlpha;
    attribute vec3 aColor;
    attribute vec3 aPhase;
    attribute float aRand, aAmp;
    varying vec3 vC;
    varying float vA;
    void main(){
      vec3 pos = position;
      float orbit = uTime * (0.030 + aRand * 0.034);
      float cs = cos(orbit), sn = sin(orbit);
      pos.xy = mat2(cs, -sn, sn, cs) * pos.xy;
      float breathe = 1.0 + sin(uTime * 0.34 + aPhase.x) * 0.045;
      pos.xy *= breathe;
      pos.x += sin(uTime * (0.18 + aRand * 0.05) + aPhase.x) * aAmp * 0.34;
      pos.y += cos(uTime * (0.15 + aRand * 0.06) + aPhase.y) * aAmp * 0.30;
      pos.z += sin(uTime * (0.11 + aRand * 0.04) + aPhase.z) * aAmp * 0.68 + uBass * 0.10 * sin(aRand * 12.0);
      vC = aColor;
      vec4 mvPos = modelViewMatrix * vec4(pos, 1.0);
      float dist = -mvPos.z;
      float twinkle = 0.62 + 0.38 * sin(uTime * (0.42 + aRand * 0.34) + aPhase.z);
      vA = clamp(0.22 + (5.0 - dist) * 0.10, 0.055, 0.58) * twinkle;
      float sz = clamp(40.0 / max(0.5, dist), 1.3, 4.1);
      gl_PointSize = sz * uPixel;
      gl_Position = projectionMatrix * mvPos;
    }
  `;
  var ffs = `
    precision highp float;
    uniform sampler2D uDotTex;
    uniform float uFloatAlpha;
    varying vec3 vC;
    varying float vA;
    void main(){
      vec4 tex = texture2D(uDotTex, gl_PointCoord);
      if (tex.a < 0.02) discard;
      gl_FragColor = vec4(vC, tex.a * vA * uFloatAlpha);
    }
  `;
  var fmat = new THREE.ShaderMaterial({
    uniforms: {
      uTime: uniforms.uTime,
      uBass: uniforms.uBass,
      uPixel: uniforms.uPixel,
      uDotTex: uniforms.uDotTex,
      uFloatAlpha: uniforms.uFloatAlpha,
    },
    vertexShader: fvs, fragmentShader: ffs,
    transparent:true, depthWrite:false, blending: THREE.AdditiveBlending,
  });
  floatGroup = new THREE.Points(fgeo, fmat);
  floatGroup.frustumCulled = false;
  scene.add(floatGroup);
}
function destroyFloatLayer() {
  if (!floatGroup) return;
  scene.remove(floatGroup);
  floatGroup.geometry.dispose(); floatGroup.material.dispose();
  floatGroup = null;
}


// ▼▼▼ [mineradio:skull] source lines 6521,7058 ▼▼▼
// ============================================================
//  安魂 — 3D 粒子建模层
// ============================================================
var SKULL_PRESET_INDEX = 6;
var SKULL_MODEL_BASE_ROTATION_X = -0.26;
var SKULL_MODEL_BASE_ROTATION_Y = 0.00;
var SKULL_MODEL_SCALE = 2.34;
var SKULL_MODEL_BASE_POSITION = { x: 0, y: 0.22, z: 0.10 };
var skullAmpPulse = 0;
var skullBeatFlash = 0;
var skullJawOpen = 0;
var skullCameraBlend = 0;
var skullWheelZoom = 0;
var skullWheelZoomTarget = 0;
var skullCameraTargetPos = new THREE.Vector3();
var skullCameraTargetLook = new THREE.Vector3();
var skullCameraBasePos = new THREE.Vector3();
var skullCameraBaseLook = new THREE.Vector3();
var skullCameraShelfPos = new THREE.Vector3();
var skullCameraShelfLook = new THREE.Vector3();
var skullCameraMixedLook = new THREE.Vector3();
var skullShelfCameraMix = 0;
var skullLyricMouthLocal = new THREE.Vector3(0.025, -0.72, 0.62);
var skullLyricMouthTarget = new THREE.Vector3();
var skullLyricMouthForward = new THREE.Vector3();
var skullLyricMouthQuat = new THREE.Quaternion();
var skullLyricReadableQuat = new THREE.Quaternion();
var skullParticleGroup = null;
var skullParticleOpacity = 0;
var skullParticleAsset = { data: null, promise: null, failed: false };
var skullBaseColors = {
  boneA: new THREE.Color('#b8ae98'),
  boneB: new THREE.Color('#fff4d8'),
  shadow: new THREE.Color('#100d0d'),
  light: new THREE.Color('#ffe3a0'),
  neutralBoneA: new THREE.Color('#9fb7c8'),
  neutralBoneB: new THREE.Color('#eef9ff'),
  neutralShadow: new THREE.Color('#070b12'),
  neutralLight: new THREE.Color('#d6f3ff')
};
var skullTintScratch = {
  tint: new THREE.Color(),
  soft: new THREE.Color(),
  bright: new THREE.Color(),
  dark: new THREE.Color(),
  boneA: new THREE.Color(),
  boneB: new THREE.Color(),
  shadow: new THREE.Color(),
  light: new THREE.Color()
};

function effectiveSkullVisualTint() {
  var pal = stageLyrics && (stageLyrics.coverPalette || stageLyrics.palette) || {};
  var custom = fx && fx.visualTintMode === 'custom';
  var color = custom
    ? fx.visualTintColor
    : (pal.secondary || pal.primary || fx.visualTintColor || fxDefaults.visualTintColor || '#9db8cf');
  color = normalizeHexColor(color || '#9db8cf', '#9db8cf');
  var strength = custom ? 0.98 : (pal && (pal.secondary || pal.primary) ? 0.30 : 0.14);
  return { color: color, strength: strength, custom: custom };
}

function syncSkullParticleColors() {
  if (!skullParticleGroup || !skullParticleGroup.material || !skullParticleGroup.material.uniforms) return;
  var u = skullParticleGroup.material.uniforms;
  var tint = effectiveSkullVisualTint();
  var custom = !!tint.custom;
  var strength = clampRange(Number(tint.strength) || 0, 0, custom ? 0.99 : 0.78);
  skullTintScratch.tint.set(tint.color);
  skullTintScratch.soft.copy(skullTintScratch.tint).lerp(new THREE.Color('#e8f5ff'), custom ? 0.05 : 0.28);
  skullTintScratch.bright.copy(skullTintScratch.tint).lerp(new THREE.Color(custom ? '#f6fbff' : '#fff7d6'), custom ? 0.14 : 0.46);
  skullTintScratch.dark.copy(skullTintScratch.tint).lerp(new THREE.Color('#05070c'), custom ? 0.74 : 0.72);
  skullTintScratch.boneA.copy(custom ? skullBaseColors.neutralBoneA : skullBaseColors.boneA).lerp(skullTintScratch.soft, strength * (custom ? 0.99 : 0.64));
  skullTintScratch.boneB.copy(custom ? skullBaseColors.neutralBoneB : skullBaseColors.boneB).lerp(skullTintScratch.bright, strength * (custom ? 0.94 : 0.46));
  skullTintScratch.shadow.copy(custom ? skullBaseColors.neutralShadow : skullBaseColors.shadow).lerp(skullTintScratch.dark, strength * (custom ? 0.72 : 0.42));
  skullTintScratch.light.copy(custom ? skullBaseColors.neutralLight : skullBaseColors.light).lerp(skullTintScratch.bright, strength * (custom ? 0.98 : 0.76));
  if (u.uColorA) u.uColorA.value.copy(skullTintScratch.boneA);
  if (u.uColorB) u.uColorB.value.copy(skullTintScratch.boneB);
  if (u.uShadow) u.uShadow.value.copy(skullTintScratch.shadow);
  if (u.uLight) u.uLight.value.copy(skullTintScratch.light);
}

function buildSkullParticleGeometryFromAsset(points) {
  var count = Math.floor((points && points.length || 0) / 5);
  var geo = new THREE.BufferGeometry();
  var positions = new Float32Array(count * 3);
  var seeds = new Float32Array(count);
  var kinds = new Float32Array(count);
  for (var i = 0; i < count; i++) {
    positions[i * 3] = points[i * 5];
    positions[i * 3 + 1] = points[i * 5 + 1];
    positions[i * 3 + 2] = points[i * 5 + 2];
    kinds[i] = points[i * 5 + 3];
    seeds[i] = points[i * 5 + 4];
  }
  geo.setAttribute('position', new THREE.BufferAttribute(positions, 3));
  geo.setAttribute('seed', new THREE.BufferAttribute(seeds, 1));
  geo.setAttribute('kind', new THREE.BufferAttribute(kinds, 1));
  return geo;
}

function loadSkullParticleAsset() {
  if (skullParticleAsset.data || skullParticleAsset.promise || skullParticleAsset.failed) return skullParticleAsset.promise || Promise.resolve(skullParticleAsset.data);
  if (typeof fetch !== 'function') {
    skullParticleAsset.failed = true;
    return Promise.resolve(null);
  }
  skullParticleAsset.promise = fetch(__assetUrl('skull-decimation-points.bin') + '?v=regular-surface-teeth-soften-20260621', { cache: 'reload' })
    .then(function(res){
      if (!res.ok) throw new Error('skull asset ' + res.status);
      return res.arrayBuffer();
    })
    .then(function(buf){
      if (!buf || buf.byteLength < 20 || buf.byteLength % 20 !== 0) throw new Error('invalid skull asset');
      skullParticleAsset.data = new Float32Array(buf);
      skullParticleAsset.promise = null;
      return skullParticleAsset.data;
    })
    .catch(function(err){
      console.warn('skull particle asset load failed:', err);
      skullParticleAsset.failed = true;
      skullParticleAsset.promise = null;
      return null;
    });
  return skullParticleAsset.promise;
}

function skullPushPoint(pos, seed, kind, x, y, z, k) {
  pos.push(x, y, z);
  seed.push(Math.random() * 1000);
  kind.push(k == null ? 0 : k);
}
function skullPushCurve(pos, seed, kind, count, fn, k, jitter) {
  jitter = jitter == null ? 0.012 : jitter;
  for (var i = 0; i < count; i++) {
    var t = count > 1 ? i / (count - 1) : 0;
    var p = fn(t);
    skullPushPoint(pos, seed, kind, p.x + (Math.random() - 0.5) * jitter, p.y + (Math.random() - 0.5) * jitter, p.z + (Math.random() - 0.5) * jitter, k);
  }
}
function createSkullParticleLayer() {
  if (skullParticleGroup) return skullParticleGroup;
  var asset = skullParticleAsset.data;
  if (!asset) return null;
  var pos = [];
  var seed = [];
  var kind = [];
  if (!asset) {

  function rotate2(x, y, a) {
    var c = Math.cos(a), s = Math.sin(a);
    return { x:x * c - y * s, y:x * s + y * c };
  }
  function eyeCut(x, y, z, side) {
    if (z < 0.16) return false;
    var p = rotate2(x - side * 0.38, y - 0.02, side * 0.10);
    var almond = Math.pow(Math.abs(p.x) / 0.34, 1.70) + Math.pow(Math.abs(p.y) / 0.215, 1.34);
    var slantGate = p.y < 0.22 - Math.abs(p.x) * 0.12 && p.y > -0.24 + Math.abs(p.x) * 0.10;
    return almond < 1.0 && slantGate;
  }
  function noseCut(x, y, z) {
    if (z < 0.20 || y > -0.12 || y < -0.62) return false;
    var t = clampRange((-0.12 - y) / 0.50, 0, 1);
    var half = 0.050 + t * 0.185;
    return Math.abs(x) < half && z > 0.38 + t * 0.18;
  }
  function mouthGap(x, y, z) {
    return z > 0.18 && y < -0.66 && y > -1.03 && Math.abs(x) < 0.30;
  }
  function addEllipsoidSurface(count, cx, cy, cz, rx, ry, rz, yMin, yMax, k, frontBias) {
    var made = 0, guard = 0;
    while (made < count && guard < count * 8) {
      guard++;
      var theta = frontBias
        ? (-Math.PI * 0.07 + Math.random() * Math.PI * 1.14)
        : (Math.random() * Math.PI * 2);
      var phi = Math.acos(1 - Math.random() * 2);
      var sx = Math.sin(phi) * Math.cos(theta);
      var sy = Math.cos(phi);
      var sz = Math.sin(phi) * Math.sin(theta);
      var x = cx + sx * rx * (0.96 + Math.max(0, -sy) * 0.12);
      var y = cy + sy * ry;
      var z = cz + sz * rz;
      if (y < yMin || y > yMax) continue;
      if (eyeCut(x, y, z, -1) || eyeCut(x, y, z, 1) || noseCut(x, y, z) || mouthGap(x, y, z)) continue;
      var cheekCarve = z > 0.18 && y < -0.18 && y > -0.66 && Math.abs(x) > 0.26 && Math.abs(x) < 0.58 && Math.random() < 0.36;
      if (cheekCarve) continue;
      skullPushPoint(pos, seed, kind, x, y, z, k + Math.random() * 0.08);
      made++;
    }
  }

  addEllipsoidSurface(3150, 0, 0.46, 0.00, 0.93, 0.88, 0.58, -0.16, 1.35, 0.055, true);
  addEllipsoidSurface(2100, 0, -0.34, 0.10, 0.70, 0.66, 0.46, -0.95, 0.14, 0.10, true);
  for (var j = 0; j < 1450; j++) {
    var a = Math.random() * Math.PI * 2;
    var v = Math.random();
    var y = -1.16 + v * 0.48;
    var taper = clampRange((y + 1.16) / 0.48, 0, 1);
    var rx = 0.32 + taper * 0.31;
    var rz = 0.22 + taper * 0.18;
    var x = Math.cos(a) * rx;
    var z = 0.22 + Math.sin(a) * rz;
    if (mouthGap(x, y, z)) continue;
    if (y > -0.94 && Math.abs(x) < 0.22 && z > 0.18) continue;
    skullPushPoint(pos, seed, kind, x, y, z, 0.15 + Math.random() * 0.10);
  }

  [-1, 1].forEach(function(side){
    var cx = side * 0.38;
    skullPushCurve(pos, seed, kind, 520, function(t){
      var a = t * Math.PI * 2;
      var px = Math.cos(a) * (0.345 + Math.sin(a * 2.0) * 0.012);
      var py = Math.sin(a) * (0.205 + Math.cos(a * 2.0) * 0.010);
      var r = rotate2(px, py, -side * 0.10);
      return {
        x: cx + r.x,
        y: 0.02 + r.y - Math.max(0, Math.cos(a)) * 0.018,
        z: 0.72 + Math.sin(a * 2.0) * 0.030
      };
    }, 0.96, 0.010);
    skullPushCurve(pos, seed, kind, 330, function(t){
      var x = side * (0.13 + t * 0.58);
      var y = 0.245 - t * 0.085 + Math.sin(t * Math.PI) * 0.055;
      return { x:x, y:y, z:0.66 + Math.sin(t * Math.PI) * 0.055 };
    }, 0.98, 0.010);
    skullPushCurve(pos, seed, kind, 300, function(t){
      return {
        x: side * (0.30 + t * 0.47),
        y: -0.18 - t * 0.25 + Math.sin(t * Math.PI) * 0.070,
        z: 0.69 - t * 0.095
      };
    }, 0.84, 0.012);
    skullPushCurve(pos, seed, kind, 330, function(t){
      return {
        x: side * (0.62 - t * 0.20),
        y: -0.28 - t * 0.55 + Math.sin(t * Math.PI) * 0.065,
        z: 0.50 + Math.sin(t * Math.PI) * 0.070
      };
    }, 0.72, 0.014);
  });

  skullPushCurve(pos, seed, kind, 360, function(t){
    var x = -0.72 + t * 1.44;
    return { x:x, y:0.235 - Math.abs(x) * 0.055 + Math.sin(t * Math.PI) * 0.035, z:0.62 + Math.sin(t * Math.PI) * 0.040 };
  }, 0.86, 0.012);
  [-1, 1].forEach(function(side){
    skullPushCurve(pos, seed, kind, 260, function(t){
      return { x:side * (0.035 + t * 0.205), y:-0.15 - t * 0.43, z:0.79 - t * 0.035 };
    }, 0.98, 0.007);
  });
  skullPushCurve(pos, seed, kind, 240, function(t){
    var x = -0.25 + t * 0.50;
    return { x:x, y:-0.62 + Math.sin(t * Math.PI) * 0.030, z:0.70 };
  }, 0.86, 0.008);
  skullPushCurve(pos, seed, kind, 420, function(t){
    var a = Math.PI + t * Math.PI;
    return { x: Math.cos(a) * 0.50, y: -0.98 + Math.sin(a) * 0.205, z: 0.46 + Math.sin(t * Math.PI) * 0.075 };
  }, 0.82, 0.014);
  skullPushCurve(pos, seed, kind, 360, function(t){
    var x = -0.39 + t * 0.78;
    return { x:x, y:-0.70 + Math.sin(t * Math.PI) * 0.018, z:0.73 };
  }, 0.96, 0.006);
  skullPushCurve(pos, seed, kind, 320, function(t){
    var x = -0.36 + t * 0.72;
    return { x:x, y:-1.005 - Math.sin(t * Math.PI) * 0.018, z:0.70 };
  }, 0.78, 0.008);
  for (var tooth = -4; tooth <= 4; tooth++) {
    var tx = tooth * 0.082;
    var height = tooth === 0 ? 0.30 : (0.25 + (4 - Math.abs(tooth)) * 0.012);
    skullPushCurve(pos, seed, kind, 58, function(t){
      return { x: tx + Math.sin(t * Math.PI) * 0.006, y: -0.715 - t * height, z: 0.735 - t * 0.020 };
    }, 0.94, 0.004);
  }
  skullPushCurve(pos, seed, kind, 520, function(t){
    var a = Math.PI * 0.12 + t * Math.PI * 0.76;
    return { x: Math.cos(a) * 0.98, y: 0.42 + Math.sin(a) * 0.92, z: 0.48 + Math.sin(t * Math.PI) * 0.10 };
  }, 0.70, 0.012);
  skullPushCurve(pos, seed, kind, 360, function(t){
    var a = t * Math.PI * 2;
    return { x: Math.cos(a) * 0.52, y: -1.19 + Math.sin(a) * 0.082, z: 0.24 + Math.sin(a * 2.0) * 0.028 };
  }, 0.72, 0.010);
  }

  var geo = asset ? buildSkullParticleGeometryFromAsset(asset) : new THREE.BufferGeometry();
  if (!asset) {
    geo.setAttribute('position', new THREE.BufferAttribute(new Float32Array(pos), 3));
    geo.setAttribute('seed', new THREE.BufferAttribute(new Float32Array(seed), 1));
    geo.setAttribute('kind', new THREE.BufferAttribute(new Float32Array(kind), 1));
  }
  var mat = new THREE.ShaderMaterial({
    uniforms: {
      uMap: { value: dotTexture },
      uTime: uniforms.uTime,
      uPixel: uniforms.uPixel,
      uBass: uniforms.uBass,
      uMid: uniforms.uMid,
      uTreble: uniforms.uTreble,
      uBeat: uniforms.uBeat,
      uJawOpen: { value: 0 },
      uSkullFlash: { value: 0 },
      uPointScale: uniforms.uPointScale,
      uBloomStrength: uniforms.uBloomStrength,
      uColorBoost: uniforms.uColorBoost,
      uOpacity: { value: 0 },
      uColorA: { value: new THREE.Color('#b8ae98') },
      uColorB: { value: new THREE.Color('#fff4d8') },
      uShadow: { value: new THREE.Color('#100d0d') },
      uLight: { value: new THREE.Color('#ffe3a0') }
    },
    vertexShader: [
      'precision highp float;',
      'attribute float seed,kind;',
      'uniform float uTime,uPixel,uPointScale,uBloomStrength,uColorBoost;',
      'uniform float uBass,uMid,uTreble,uBeat,uJawOpen,uSkullFlash;',
      'varying float vKind,vLight,vRim,vAmp,vDensity,vFlash;',
      'void main(){',
      '  vec3 pos = position;',
      '  float jawGroup = step(1.0, kind);',
      '  float boneKind = fract(kind);',
      '  vKind = boneKind;',
      '  vec3 n = normalize(vec3(position.x * 0.82, position.y * 0.68, position.z * 1.22 + 0.16));',
      '  float toothBand = smoothstep(0.48, 0.70, position.z) * (1.0 - smoothstep(0.27, 0.48, abs(position.x))) * (1.0 - smoothstep(0.18, 0.46, abs(position.y + 0.72)));',
      '  float toothNoise = fract(sin(seed * 21.731 + floor((position.x + 0.52) * 21.0) * 5.137) * 43758.5453);',
      '  pos.y += toothBand * (toothNoise - 0.5) * 0.020;',
      '  pos.z += toothBand * (fract(sin(seed * 17.923 + position.y * 31.0) * 24634.6345) - 0.5) * 0.012;',
      '  float jawSidePull = jawGroup * smoothstep(-0.42, -1.06, position.y) * smoothstep(0.24, 0.62, abs(position.x)) * (1.0 - smoothstep(0.78, 1.04, abs(position.x))) * smoothstep(0.16, 0.70, position.z);',
      '  pos.x *= 1.0 - jawSidePull * 0.10;',
      '  float fallbackJaw = smoothstep(-0.48, -0.90, position.y) * smoothstep(0.08, 0.52, position.z) * (1.0 - smoothstep(0.62, 0.96, abs(position.x)));',
      '  float jawMask = jawGroup;',
      '  float jawSideAnchor = smoothstep(0.36, 0.66, abs(position.x)) * (1.0 - smoothstep(0.78, 0.98, abs(position.x))) * smoothstep(-0.34, -0.74, position.y) * (1.0 - smoothstep(0.62, 0.86, position.z));',
      '  float jawMotion = jawMask * (1.0 - jawSideAnchor * 0.32);',
      '  vec2 jawHinge = vec2(-0.45, 0.18);',
      '  float jawAngle = uJawOpen * 0.52 * jawMotion;',
      '  float jc = cos(jawAngle);',
      '  float js = sin(jawAngle);',
      '  vec2 jr = pos.yz - jawHinge;',
      '  vec2 openedJaw = vec2(jr.x * jc - jr.y * js, jr.x * js + jr.y * jc) + jawHinge;',
      '  pos.yz = mix(pos.yz, openedJaw, jawMotion);',
      '  float jawDrop = jawMotion * smoothstep(-0.32, -0.88, position.y) * (0.58 + smoothstep(0.18, 0.62, abs(position.x)) * 0.04);',
      '  float openDrive = clamp(uJawOpen, 0.0, 1.25);',
      '  pos.y -= jawDrop * (0.038 + openDrive * 0.100);',
      '  pos.z += jawDrop * (0.003 + openDrive * 0.014);',
      '  float ampDrive = smoothstep(0.20, 0.82, uBass * 0.44 + uMid * 0.22 + uBeat * 0.72);',
      '  float ampPhase = 0.50 + 0.50 * sin(uTime * (1.05 + uMid * 0.30) + seed * 6.2831);',
      '  vFlash = clamp(uSkullFlash * (0.68 + ampPhase * 0.32), 0.0, 1.0);',
      '  vAmp = clamp(ampDrive * 0.045 + vFlash * 0.92 + uTreble * 0.012, 0.0, 1.0);',
      '  vec4 mv = modelViewMatrix * vec4(pos, 1.0);',
      '  float dist = max(0.55, -mv.z);',
      '  vec3 vn = normalize(normalMatrix * n);',
      '  vec3 keyDir = normalize(vec3(-0.48, 0.64, 0.60));',
      '  vec3 lowDir = normalize(vec3(-0.10, -0.78, 0.34));',
      '  vec3 fillDir = normalize(vec3(0.36, -0.04, 0.64));',
      '  vec3 rimDir = normalize(vec3(0.88, 0.18, -0.44));',
      '  float key = pow(max(dot(vn, keyDir), 0.0), 1.18);',
      '  float low = pow(max(dot(vn, lowDir), 0.0), 1.34) * 0.10;',
      '  float fill = max(dot(vn, fillDir), 0.0) * 0.055;',
      '  float gothicShadow = smoothstep(-0.10, 0.36, dot(vn, normalize(vec3(0.44, -0.06, -0.58))));',
      '  float dentalLift = smoothstep(0.48, 0.72, position.z) * (1.0 - smoothstep(0.30, 0.54, abs(position.x))) * (1.0 - smoothstep(0.18, 0.48, abs(position.y + 0.70))) * (0.62 + toothNoise * 0.20);',
      '  vRim = pow(max(dot(vn, rimDir), 0.0), 2.50) * (0.24 + uBloomStrength * 0.08 + vFlash * 0.62);',
      '  float dust = fract(sin(seed * 13.871 + position.x * 19.7 + position.y * 7.1) * 43758.5453);',
      '  vDensity = clamp(0.30 + key * 0.70 + vRim * 0.24 - gothicShadow * 0.24 + dust * 0.025 + vFlash * 0.08, 0.16, 1.20);',
      '  vLight = clamp(0.115 + key * 1.02 + low + fill + dentalLift * 0.20 + boneKind * 0.070 + vAmp * 0.56 - gothicShadow * 0.08, 0.035, 1.72);',
      '  float scaleCtl = clamp(uPointScale, 0.48, 2.35);',
      '  float size = (0.035 + boneKind * 0.026) * (0.84 + vDensity * 0.22 + vLight * 0.13 + uBloomStrength * 0.030 + vFlash * 0.18);',
      '  gl_PointSize = clamp(size * uPixel * scaleCtl * 128.0 / dist, 0.95, 7.60);',
      '  gl_Position = projectionMatrix * mv;',
      '}'
    ].join('\n'),
    fragmentShader: [
      'precision highp float;',
      'uniform sampler2D uMap;',
      'uniform vec3 uColorA,uColorB,uShadow,uLight;',
      'uniform float uOpacity,uBloomStrength,uColorBoost;',
      'varying float vKind,vLight,vRim,vAmp,vDensity,vFlash;',
      'void main(){',
      '  vec4 tex = texture2D(uMap, gl_PointCoord);',
      '  if(tex.a < 0.070) discard;',
      '  float contrast = clamp(uColorBoost, 0.50, 2.00);',
      '  float lit = clamp(pow(vLight, mix(1.18, 0.74, (contrast - 0.50) / 1.50)), 0.0, 1.28);',
      '  vec3 bone = mix(uColorA, uColorB, clamp((vKind - 0.34) * 2.0 + lit * 0.18, 0.0, 1.0));',
      '  vec3 col = mix(uShadow, bone, clamp(lit, 0.0, 1.0));',
      '  col = mix(col, uLight, clamp(vRim * (0.14 + uBloomStrength * 0.035 + vFlash * 0.40), 0.0, 0.54));',
      '  col = mix(col, uLight, clamp(vAmp * (0.09 + uBloomStrength * 0.025) + vFlash * 0.56, 0.0, 0.68));',
      '  float alpha = tex.a * uOpacity * clamp(0.20 + lit * 0.44 + vDensity * 0.40 + vRim * 0.10 + vFlash * 0.46, 0.12, 1.56);',
      '  gl_FragColor = vec4(col, alpha);',
      '}'
    ].join('\n'),
    transparent: true,
    depthWrite: false,
    depthTest: true,
    blending: THREE.NormalBlending
  });
  skullParticleGroup = new THREE.Points(geo, mat);
  skullParticleGroup.frustumCulled = false;
  skullParticleGroup.visible = false;
  skullParticleGroup.userData.source = asset ? 'asset' : 'fallback';
  skullParticleGroup.position.set(SKULL_MODEL_BASE_POSITION.x, SKULL_MODEL_BASE_POSITION.y, SKULL_MODEL_BASE_POSITION.z);
  skullParticleGroup.scale.setScalar(SKULL_MODEL_SCALE);
  skullParticleGroup.rotation.x = SKULL_MODEL_BASE_ROTATION_X;
  skullParticleGroup.rotation.y = SKULL_MODEL_BASE_ROTATION_Y;
  skullParticleGroup.renderOrder = 32;
  syncSkullParticleColors();
  scene.add(skullParticleGroup);
  return skullParticleGroup;
}
function isSkullShelfCompositionActive() {
  if (!(fx && fx.preset === SKULL_PRESET_INDEX)) return false;
  if (!shelfManager || !shelfManager.getMode || shelfManager.getMode() !== 'side') return false;
  if (shelfPinnedOpen || shelfVisibility > 0.18) return true;
  return !!(shelfManager.hasOpenContent && shelfManager.hasOpenContent());
}
function clearSkullPresetResidue() {
  skullParticleOpacity = 0;
  skullAmpPulse = 0;
  skullBeatFlash = 0;
  skullJawOpen = 0;
  skullCameraBlend = 0;
  if (!skullParticleGroup) return;
  skullParticleGroup.visible = false;
  if (skullParticleGroup.material && skullParticleGroup.material.uniforms) {
    if (skullParticleGroup.material.uniforms.uOpacity) skullParticleGroup.material.uniforms.uOpacity.value = 0;
    if (skullParticleGroup.material.uniforms.uJawOpen) skullParticleGroup.material.uniforms.uJawOpen.value = 0;
    if (skullParticleGroup.material.uniforms.uSkullFlash) skullParticleGroup.material.uniforms.uSkullFlash.value = 0;
  }
}
function resetSkullPresetView(immediate, opts) {
  opts = opts || {};
  if (!(fx && fx.preset === SKULL_PRESET_INDEX)) return;
  skullWheelZoomTarget = 0;
  if (!opts.smooth) skullWheelZoom = 0;
  skullCameraBlend = Math.max(skullCameraBlend, 1);
  if (!opts.keepLyricLock && typeof stageLyrics !== 'undefined' && stageLyrics && stageLyrics.group && stageLyrics.group.userData) stageLyrics.group.userData.skullMouthLocked = false;
  if (!opts.keepLyricLock && typeof requestStageLyricCameraSnap === 'function') requestStageLyricCameraSnap(10);
  if (!immediate || !skullParticleGroup) return;
  var shelfComposition = isSkullShelfCompositionActive();
  skullShelfCameraMix = shelfComposition ? 1 : 0;
  skullParticleGroup.position.set(shelfComposition ? -1.18 : SKULL_MODEL_BASE_POSITION.x, shelfComposition ? 0.32 : SKULL_MODEL_BASE_POSITION.y, SKULL_MODEL_BASE_POSITION.z);
  skullParticleGroup.scale.setScalar(shelfComposition ? 3.02 : SKULL_MODEL_SCALE);
  skullParticleGroup.rotation.set(SKULL_MODEL_BASE_ROTATION_X, SKULL_MODEL_BASE_ROTATION_Y, 0);
  skullParticleGroup.updateMatrixWorld(true);
  if (camera && typeof setSkullCameraTargetVectors === 'function') {
    var portrait = innerHeight > innerWidth * 1.08;
    setSkullCameraTargetVectors(skullCameraTargetPos, skullCameraTargetLook, portrait, shelfComposition, 0);
    camera.position.copy(skullCameraTargetPos);
    skullCameraMixedLook.copy(skullCameraTargetLook);
    camera.lookAt(skullCameraMixedLook);
    camera.updateProjectionMatrix();
  }
}
function skullBreathOffset(t, shelfComposition) {
  var strength = shelfComposition ? 0.70 : 1.0;
  return {
    x: strength * (Math.sin(t * 0.33 + 1.7) * 0.028 + Math.sin(t * 0.61 + 0.4) * 0.010),
    y: strength * (Math.sin(t * 0.38 + 0.2) * 0.036 + Math.sin(t * 0.83 + 2.1) * 0.012),
    z: strength * (Math.sin(t * 0.24 + 2.6) * 0.026)
  };
}
function setSkullCameraTargetVectors(pos, look, portrait, shelfComposition, zoom) {
  zoom = Number(zoom) || 0;
  if (shelfComposition) {
    pos.set(portrait ? -0.06 : 0.00, portrait ? -2.36 : -2.50, (portrait ? 4.88 : 4.96) + zoom * 0.78);
    look.set(portrait ? -0.04 : 0.00, portrait ? -0.26 : -0.20, 0.03);
    return;
  }
  pos.set(0.00, portrait ? -2.38 : -2.52, (portrait ? 4.92 : 4.98) + zoom);
  look.set(0.00, portrait ? -0.28 : -0.20, 0.02);
}
function applySkullCameraPose(dt) {
  if (freeCamera && (freeCamera.active || freeCamera.locked || freeCamera.resetTween)) return;
  var active = fx && fx.preset === SKULL_PRESET_INDEX;
  skullCameraBlend += ((active ? 1 : 0) - skullCameraBlend) * Math.min(1, dt * (active ? 4.8 : 7.2));
  if (skullCameraBlend < 0.002) return;
  skullWheelZoom += (skullWheelZoomTarget - skullWheelZoom) * Math.min(1, dt * 8.0);
  var portrait = innerHeight > innerWidth * 1.08;
  var shelfComposition = isSkullShelfCompositionActive();
  var shelfMixTarget = shelfComposition ? 1 : 0;
  skullShelfCameraMix += (shelfMixTarget - skullShelfCameraMix) * Math.min(1, dt * (shelfMixTarget > skullShelfCameraMix ? 4.6 : 5.8));
  if (Math.abs(skullShelfCameraMix - shelfMixTarget) < 0.002) skullShelfCameraMix = shelfMixTarget;
  setSkullCameraTargetVectors(skullCameraBasePos, skullCameraBaseLook, portrait, false, skullWheelZoom);
  setSkullCameraTargetVectors(skullCameraShelfPos, skullCameraShelfLook, portrait, true, skullWheelZoom);
  skullCameraTargetPos.copy(skullCameraBasePos).lerp(skullCameraShelfPos, skullShelfCameraMix);
  skullCameraTargetLook.copy(skullCameraBaseLook).lerp(skullCameraShelfLook, skullShelfCameraMix);
  camera.position.lerp(skullCameraTargetPos, skullCameraBlend);
  skullCameraMixedLook.set(orbit.lookAt.x, orbit.lookAt.y, orbit.lookAt.z).lerp(skullCameraTargetLook, skullCameraBlend);
  camera.lookAt(skullCameraMixedLook);
  camera.updateProjectionMatrix();
}
function updateSkullParticleLayer(dt) {
  var active = fx && fx.preset === SKULL_PRESET_INDEX;
  if (active && !skullParticleAsset.data && !skullParticleAsset.failed) {
    loadSkullParticleAsset();
    return;
  }
  if (active && !skullParticleAsset.data) return;
  if (active) createSkullParticleLayer();
  if (!skullParticleGroup) return;
  var target = active ? 1 : 0;
  skullParticleOpacity += (target - skullParticleOpacity) * Math.min(1, dt * (active ? 3.2 : 2.4));
  if (skullParticleOpacity < 0.006 && !active) {
    skullParticleGroup.visible = false;
    return;
  }
  skullParticleGroup.visible = true;
  skullParticleGroup.material.uniforms.uOpacity.value = skullParticleOpacity * clampRange(0.78 + (fx.intensity || 0.85) * 0.18, 0.56, 1.0);
  var beatTransient = clampRange(Math.max(0, beatPulse - 0.16) / 0.84, 0, 1.35);
  var flashTarget = clampRange(Math.pow(beatTransient, 1.34) * 1.08 + Math.max(0, bass - 0.60) * 0.18 * beatTransient, 0, 1);
  skullBeatFlash += (flashTarget - skullBeatFlash) * Math.min(1, dt * (flashTarget > skullBeatFlash ? 24.0 : 6.2));
  if (skullParticleGroup.material.uniforms.uSkullFlash) skullParticleGroup.material.uniforms.uSkullFlash.value = skullBeatFlash;
  var jawTarget = clampRange(0.60 + (0.5 + 0.5 * Math.sin(uniforms.uTime.value * 0.50)) * 0.050 + bass * 0.060 + skullBeatFlash * 0.090, 0.52, 0.88);
  skullJawOpen += (jawTarget - skullJawOpen) * Math.min(1, dt * (jawTarget > skullJawOpen ? 7.8 : 3.4));
  if (skullParticleGroup.material.uniforms.uJawOpen) skullParticleGroup.material.uniforms.uJawOpen.value = skullJawOpen;
  var shelfComposition = isSkullShelfCompositionActive();
  var shelfMix = clampRange(skullShelfCameraMix || (shelfComposition ? 1 : 0), 0, 1);
  var drift = skullBreathOffset(uniforms.uTime.value, shelfComposition);
  var ampTarget = clampRange(bass * 0.006 + mid * 0.004 + skullBeatFlash * 0.070, 0, 0.090);
  skullAmpPulse += (ampTarget - skullAmpPulse) * Math.min(1, dt * (ampTarget > skullAmpPulse ? 11.0 : 4.0));
  var shelfScale = 3.02;
  var targetScale = (SKULL_MODEL_SCALE + (shelfScale - SKULL_MODEL_SCALE) * shelfMix) * (1 + skullAmpPulse) * clampRange(1 - skullWheelZoom * 0.055, 0.92, 1.08);
  var shelfX = -1.18;
  var shelfY = 0.32;
  var targetX = (SKULL_MODEL_BASE_POSITION.x + (shelfX - SKULL_MODEL_BASE_POSITION.x) * shelfMix) + drift.x;
  var targetY = (SKULL_MODEL_BASE_POSITION.y + (shelfY - SKULL_MODEL_BASE_POSITION.y) * shelfMix) + drift.y;
  var targetZ = SKULL_MODEL_BASE_POSITION.z + drift.z;
  skullParticleGroup.position.x += (targetX - skullParticleGroup.position.x) * Math.min(1, dt * 4.2);
  skullParticleGroup.position.y += (targetY - skullParticleGroup.position.y) * Math.min(1, dt * 4.8);
  skullParticleGroup.position.z += (targetZ - skullParticleGroup.position.z) * Math.min(1, dt * 4.2);
  skullParticleGroup.scale.x += (targetScale - skullParticleGroup.scale.x) * Math.min(1, dt * 4.6);
  skullParticleGroup.scale.y = skullParticleGroup.scale.x;
  skullParticleGroup.scale.z = skullParticleGroup.scale.x;
  var targetRotY = SKULL_MODEL_BASE_ROTATION_Y + (orbit.centerLocked ? 0 : (headParallax.active ? headParallax.x * 0.5 : 0) + gestureRotation.y);
  var targetRotX = SKULL_MODEL_BASE_ROTATION_X + (orbit.centerLocked ? 0 : (headParallax.active ? -headParallax.y * 0.35 : 0) + gestureRotation.x);
  var rotEase = Math.min(1, dt * 7.4);
  skullParticleGroup.rotation.y += (targetRotY - skullParticleGroup.rotation.y) * rotEase;
  skullParticleGroup.rotation.x += (targetRotX - skullParticleGroup.rotation.x) * rotEase;
  skullParticleGroup.rotation.z += (0 - skullParticleGroup.rotation.z) * Math.min(1, dt * 6.0);
}


// ▼▼▼ [mineradio:back-cover] source lines 7059,7200 ▼▼▼
// ============================================================
//  封面背面粒子层 (v7.2)
//   - 独立 Points, 放在 z=-1.5 (主封面平面背面)
//   - 颜色取自封面镜像 UV
//   - 慢呼吸 + 小幅 noise 漂移
//   - 跟主粒子同步旋转 (在主循环里赋值)
//   - 视角转到背面才能看到 — 不需要手动控制 visible
// ============================================================
var BACK_COVER_COUNT = 3000;
var backCoverGroup = null;
var backCoverColorArr = null;

function createBackCoverLayer() {
  if (backCoverGroup) return;
  var bg = new THREE.BufferGeometry();
  var bp = new Float32Array(BACK_COVER_COUNT * 3);
  var bc = new Float32Array(BACK_COVER_COUNT * 3);
  var br = new Float32Array(BACK_COVER_COUNT);
  var bu = new Float32Array(BACK_COVER_COUNT * 2);  // 镜像 UV 用于采样封面
  for (var i = 0; i < BACK_COVER_COUNT; i++) {
    var u = Math.random();
    var v = Math.random();
    // 在 PLANE_SIZE 范围内分布
    bp[i*3]   = (u - 0.5) * PLANE_SIZE;
    bp[i*3+1] = (v - 0.5) * PLANE_SIZE;
    bp[i*3+2] = -1.5 - Math.random() * 0.4;  // 在主平面后方
    bu[i*2]   = 1.0 - u;  // 镜像 X
    bu[i*2+1] = v;
    br[i] = Math.random();
    bc[i*3] = 0.7; bc[i*3+1] = 0.6; bc[i*3+2] = 0.8;  // 占位
  }
  bg.setAttribute('position', new THREE.BufferAttribute(bp, 3));
  bg.setAttribute('aColor',   new THREE.BufferAttribute(bc, 3));
  bg.setAttribute('aRand',    new THREE.BufferAttribute(br, 1));
  bg.setAttribute('aUv',      new THREE.BufferAttribute(bu, 2));

  var vs = `
    precision highp float;
    uniform float uTime, uBass, uPixel, uAlpha;
    attribute vec3 aColor;
    attribute vec2 aUv;
    attribute float aRand;
    varying vec3 vC;
    varying float vA;
    void main(){
      vec3 pos = position;
      // 缓慢呼吸
      pos.x += sin(uTime * 0.20 + aRand * 8.0) * 0.20;
      pos.y += cos(uTime * 0.18 + aRand * 6.0) * 0.22;
      pos.z += sin(uTime * 0.12 + aRand * 5.0) * 0.18 + uBass * 0.12 * sin(aRand * 11.0);
      vC = aColor;
      vec4 mvPos = modelViewMatrix * vec4(pos, 1.0);
      float dist = -mvPos.z;
      vA = clamp(0.30 + 0.4 * sin(uTime * 0.6 + aRand * 5.0), 0.10, 0.65);
      float sz = clamp(46.0 / max(0.5, dist), 1.4, 4.5);
      gl_PointSize = sz * uPixel;
      gl_Position = projectionMatrix * mvPos;
    }
  `;
  var fs = `
    precision highp float;
    uniform sampler2D uDotTex;
    uniform float uAlpha;
    varying vec3 vC;
    varying float vA;
    void main(){
      vec4 tex = texture2D(uDotTex, gl_PointCoord);
      if (tex.a < 0.02) discard;
      gl_FragColor = vec4(vC, tex.a * vA * uAlpha);
    }
  `;
  var mat = new THREE.ShaderMaterial({
    uniforms: {
      uTime: uniforms.uTime,
      uBass: uniforms.uBass,
      uPixel: uniforms.uPixel,
      uDotTex: uniforms.uDotTex,
      uAlpha: uniforms.uAlpha,
    },
    vertexShader: vs, fragmentShader: fs,
    transparent:true, depthWrite:false, blending: THREE.NormalBlending,
  });
  backCoverGroup = new THREE.Points(bg, mat);
  backCoverGroup.frustumCulled = false;
  backCoverColorArr = bc;
  scene.add(backCoverGroup);
}

function destroyBackCoverLayer() {
  if (!backCoverGroup) return;
  scene.remove(backCoverGroup);
  backCoverGroup.geometry.dispose(); backCoverGroup.material.dispose();
  backCoverGroup = null; backCoverColorArr = null;
}

function refreshBackCoverColorsFromCanvas(coverCanvas) {
  if (!backCoverGroup || !coverCanvas || !backCoverColorArr) return;
  var ctx = coverCanvas.getContext('2d');
  var img = ctx.getImageData(0, 0, coverCanvas.width, coverCanvas.height).data;
  var w = coverCanvas.width, h = coverCanvas.height;
  var attr = backCoverGroup.geometry.attributes;
  var uvA = attr.aUv.array;
  for (var i = 0; i < BACK_COVER_COUNT; i++) {
    var u = uvA[i*2], v = uvA[i*2+1];
    var sx = Math.floor(u * w);
    var sy = Math.floor(v * h);
    var di = (sy * w + sx) * 4;
    backCoverColorArr[i*3]   = img[di]   / 255 * 0.85;
    backCoverColorArr[i*3+1] = img[di+1] / 255 * 0.85;
    backCoverColorArr[i*3+2] = img[di+2] / 255 * 0.85;
  }
  attr.aColor.needsUpdate = true;
}
function updateFloatLayer(dt) {
  // 漂移已在 shader 中完成, JS 不需要每帧改 buffer
}
function refreshFloatColorsFromCover(coverCanvas) {
  if (!floatGroup || !coverCanvas) return;
  var ctx = coverCanvas.getContext('2d');
  var img = ctx.getImageData(0, 0, coverCanvas.width, coverCanvas.height).data;
  var w = coverCanvas.width, h = coverCanvas.height;
  for (var i = 0; i < FLOAT_COUNT; i++) {
    var sx = Math.floor(Math.random() * w);
    var sy = Math.floor(Math.random() * h);
    var di = (sy * w + sx) * 4;
    floatColorArr[i*3]   = img[di]   / 255 * 0.95;
    floatColorArr[i*3+1] = img[di+1] / 255 * 0.95;
    floatColorArr[i*3+2] = img[di+2] / 255 * 0.95;
  }
  floatGroup.geometry.attributes.aColor.needsUpdate = true;
}
function resetFloatColorsToIdle() {
  if (!floatGroup || !floatColorArr) return;
  for (var i = 0; i < FLOAT_COUNT; i++) {
    var white = 0.88 + (i % 17) / 17 * 0.12;
    floatColorArr[i*3] = white;
    floatColorArr[i*3+1] = white;
    floatColorArr[i*3+2] = white;
  }
  floatGroup.geometry.attributes.aColor.needsUpdate = true;
}


// ▼▼▼ [mineradio:stage-lyrics] source lines 7201,9359 ▼▼▼
// ============================================================
//  舞台歌词系统 v9 — Three.js 文字平面, 跟随专辑粒子 3D 运动
// ============================================================
var stageLyrics = {
  group: null,
  current: null,
  outgoing: [],
  currentIdx: -1,
  currentText: '',
  highBloom: 0,
  beatGlow: 0,
  glowFollowX: 0,
  glowFollowY: 0,
  glowFollowRoll: 0,
  palette: {
    primary: '#d6f8ff',
    secondary: '#9cffdf',
    highlight: '#eef7ff',
    shadow: 'rgba(2,8,12,0.42)',
    glow: 'rgba(143,233,255,0.34)',
  },
  coverPalette: {
    primary: '#d6f8ff',
    secondary: '#9cffdf',
    highlight: '#eef7ff',
    shadow: 'rgba(2,8,12,0.42)',
    glow: 'rgba(143,233,255,0.34)',
  },
  starRiver: null,
  starRiverWidth: 4.2,
  starRiverHeight: 0.58,
  lockFitScale: 1,
  snapCameraLockFrames: 0,
};
var lyricSunColor = new THREE.Color(0xffe6a4);
var lyricSunHotColor = new THREE.Color(0xfff4cc);
var lyricCameraDir = new THREE.Vector3();
var lyricCameraRight = new THREE.Vector3();
var lyricCameraUp = new THREE.Vector3();
var lyricCameraTarget = new THREE.Vector3();
var lyricLayoutBase = new THREE.Vector3();
var lyricLayoutTarget = new THREE.Vector3();
var lyricCoverWorldPos = new THREE.Vector3();
var lyricCoverWorldQuat = new THREE.Quaternion();
var lyricBaseEuler = new THREE.Euler(0, 0, 0, 'YXZ');
var lyricTiltEuler = new THREE.Euler(0, 0, 0, 'YXZ');
var lyricBaseQuat = new THREE.Quaternion();
var lyricTiltQuat = new THREE.Quaternion();
var lyricTargetQuat = new THREE.Quaternion();
var LYRIC_CAMERA_LOCK_MAX_SCALE = 0.80;
function setStageLyricViewBasisFromCameraOrQuaternion(fallbackQuat) {
  if (fallbackQuat) {
    lyricCameraDir.set(0, 0, 1).applyQuaternion(fallbackQuat);
    lyricCameraRight.set(1, 0, 0).applyQuaternion(fallbackQuat);
    lyricCameraUp.set(0, 1, 0).applyQuaternion(fallbackQuat);
  } else if (camera) {
    camera.getWorldDirection(lyricCameraDir);
    lyricCameraRight.set(1, 0, 0).applyQuaternion(camera.quaternion);
    lyricCameraUp.set(0, 1, 0).applyQuaternion(camera.quaternion);
  } else {
    lyricCameraDir.set(0, 0, 1);
    lyricCameraRight.set(1, 0, 0);
    lyricCameraUp.set(0, 1, 0);
  }
  lyricCameraDir.normalize();
  lyricCameraRight.normalize();
  lyricCameraUp.normalize();
}
function applyStageLyricLayoutOffset(target, x, y, z) {
  return target
    .addScaledVector(lyricCameraRight, x || 0)
    .addScaledVector(lyricCameraUp, y || 0)
    .addScaledVector(lyricCameraDir, z || 0);
}
function stageLyricTargetQuaternion(baseQuat, tiltX, tiltY) {
  lyricTiltEuler.set((tiltX || 0) * Math.PI / 180, (tiltY || 0) * Math.PI / 180, 0, 'YXZ');
  lyricTiltQuat.setFromEuler(lyricTiltEuler);
  return lyricTargetQuat.copy(baseQuat || lyricBaseQuat).multiply(lyricTiltQuat);
}
function getStageLyricLockBounds() {
  var maxW = 0, maxH = 0;
  function take(mesh) {
    if (!mesh || !mesh.userData || !mesh.userData.lyric) return;
    var d = mesh.userData.lyric;
    var meshScale = Math.max(mesh.scale && isFinite(mesh.scale.x) ? mesh.scale.x : 1, mesh.scale && isFinite(mesh.scale.y) ? mesh.scale.y : 1);
    maxW = Math.max(maxW, (d.textWorldW || d.worldW || 6.1) * meshScale);
    maxH = Math.max(maxH, (d.textWorldH || d.worldH || 1.0) * meshScale);
  }
  take(stageLyrics.current);
  for (var i = 0; i < stageLyrics.outgoing.length; i++) take(stageLyrics.outgoing[i]);
  return { w: maxW || 5.4, h: maxH || 0.78 };
}
function lyricCameraLockFit(layoutScale, layoutX, layoutY, distance) {
  if (!camera || !camera.isPerspectiveCamera) return 1;
  layoutScale = Math.max(0.1, layoutScale || 1);
  var fov = (camera.fov || 45) * Math.PI / 180;
  var dist = Math.max(1.4, distance || 4.85);
  var visibleH = 2 * Math.tan(fov * 0.5) * dist;
  var visibleW = visibleH * (camera.aspect || (innerWidth / Math.max(1, innerHeight)) || 1.78);
  var bounds = getStageLyricLockBounds();
  var skullSafe = !!(fx && fx.preset === SKULL_PRESET_INDEX);
  var safeW = Math.max(visibleW * (skullSafe ? 0.36 : 0.42), visibleW * (skullSafe ? 0.70 : 0.84) - Math.abs(layoutX || 0) * (skullSafe ? 1.36 : 1.22));
  var safeH = Math.max(visibleH * (skullSafe ? 0.16 : 0.18), visibleH * (skullSafe ? 0.34 : 0.44) - Math.abs(layoutY || 0) * (skullSafe ? 0.98 : 0.82));
  var scaledW = Math.max(0.01, bounds.w * layoutScale);
  var scaledH = Math.max(0.01, bounds.h * layoutScale);
  var viewportFit = Math.min(1, safeW / scaledW, safeH / scaledH);
  var lockScaleCap = Math.min(1, (skullSafe ? 0.94 : LYRIC_CAMERA_LOCK_MAX_SCALE) / layoutScale);
  return clampRange(Math.min(viewportFit, lockScaleCap), skullSafe ? 0.36 : 0.42, 1);
}
// 兼容旧变量名以便其它代码不破坏
var lyricsParticles = null;
var lyricsGeo = null;

// 三个 attribute: 源位置(随机扩散态), 目标位置(组成字), color, brightness
var lyricsAttrTargetA = null;
var lyricsAttrTargetB = null;
var lyricsAttrSeed = null;

function createLyricsParticles() {
  if (stageLyrics.group) {
    ensureLyricStarRiver();
    return;
  }
  stageLyrics.group = new THREE.Group();
  stageLyrics.group.renderOrder = 38;
  scene.add(stageLyrics.group);
  ensureLyricStarRiver();
}

function ensureLyricStarRiver() {
  if (!stageLyrics.group || stageLyrics.starRiver) return stageLyrics.starRiver;
  var count = 420;
  var geo = new THREE.BufferGeometry();
  var seeds = new Float32Array(count);
  var lanes = new Float32Array(count);
  var depths = new Float32Array(count);
  for (var i = 0; i < count; i++) {
    seeds[i] = Math.random() * 1000;
    lanes[i] = Math.random();
    depths[i] = Math.random();
  }
  geo.setAttribute('seed', new THREE.BufferAttribute(seeds, 1));
  geo.setAttribute('lane', new THREE.BufferAttribute(lanes, 1));
  geo.setAttribute('depthSeed', new THREE.BufferAttribute(depths, 1));
  var mat = new THREE.ShaderMaterial({
    uniforms: {
      uMap: { value: dotTexture },
      uTime: uniforms.uTime,
      uPixel: uniforms.uPixel,
      uBass: uniforms.uBass,
      uBeat: uniforms.uBeat,
      uWidth: { value: stageLyrics.starRiverWidth || 4.2 },
      uHeight: { value: stageLyrics.starRiverHeight || 0.58 },
      uOpacity: { value: 0 },
      uColorA: { value: lyricThreeColor(stageLyrics.palette.secondary, '#9cffdf', 0.42) },
      uColorB: { value: lyricThreeColor(stageLyrics.palette.highlight, '#fff7d2', 0.44) }
    },
    vertexShader: [
      'precision highp float;',
      'attribute float seed,lane,depthSeed;',
      'uniform float uTime,uPixel,uBass,uBeat,uWidth,uHeight;',
      'varying float vSeed,vLane,vGlow;',
      'float hash(float n){return fract(sin(n)*43758.5453123);}',
      'void main(){',
      '  float laneBand = floor(lane * 5.0);',
      '  float laneLocal = fract(lane * 5.0);',
      '  float speed = 0.030 + hash(seed * 1.71) * 0.055 + laneBand * 0.005;',
      '  float flow = fract(hash(seed * 2.13) + uTime * speed);',
      '  float x = (flow - 0.5) * uWidth * (1.08 + hash(seed * 5.1) * 0.18);',
      '  float curve = sin(flow * 6.2831853 * (0.92 + hash(seed * 4.0) * 0.46) + seed * 0.071 + uTime * 0.34);',
      '  float breath = sin(uTime * (0.42 + hash(seed * 6.9) * 0.42) + seed * 0.093);',
      '  float y = (laneBand - 2.0) * uHeight * 0.135 + curve * uHeight * (0.20 + hash(seed * 9.0) * 0.18) + (laneLocal - 0.5) * uHeight * 0.16 + breath * uHeight * 0.10;',
      '  float z = -0.08 + (depthSeed - 0.5) * 0.44 + sin(uTime * (0.18 + hash(seed) * 0.24) + seed) * 0.08;',
      '  vec3 pos = vec3(x, y, z);',
      '  float edge = smoothstep(0.0, 0.18, flow) * (1.0 - smoothstep(0.82, 1.0, flow));',
      '  vSeed = seed;',
      '  vLane = lane;',
      '  vGlow = edge * (0.62 + 0.38 * sin(uTime * (0.9 + hash(seed * 8.0) * 0.7) + seed));',
      '  vec4 mv = modelViewMatrix * vec4(pos, 1.0);',
      '  float dist = max(0.45, -mv.z);',
      '  float size = (0.030 + hash(seed * 12.0) * 0.040 + vGlow * 0.024 + uBeat * 0.010) * (1.0 + uBass * 0.18);',
      '  gl_PointSize = clamp(size * uPixel * 120.0 / dist, 1.0, 7.2);',
      '  gl_Position = projectionMatrix * mv;',
      '}'
    ].join('\n'),
    fragmentShader: [
      'precision highp float;',
      'uniform sampler2D uMap;',
      'uniform vec3 uColorA,uColorB;',
      'uniform float uOpacity,uTime,uBeat;',
      'varying float vSeed,vLane,vGlow;',
      'void main(){',
      '  vec4 tex = texture2D(uMap, gl_PointCoord);',
      '  if(tex.a < 0.02) discard;',
      '  float tw = pow(0.5 + 0.5 * sin(uTime * (0.55 + fract(vSeed) * 0.35) + vSeed), 4.0);',
      '  vec3 col = mix(uColorA, uColorB, smoothstep(0.12, 0.92, vLane) * 0.45 + tw * 0.42 + vGlow * 0.26);',
      '  float alpha = tex.a * uOpacity * (0.20 + vGlow * 0.78 + tw * 0.32 + uBeat * 0.10);',
      '  gl_FragColor = vec4(col * (0.82 + vGlow * 0.72 + tw * 0.32), alpha);',
      '}'
    ].join('\n'),
    transparent: true,
    depthWrite: false,
    depthTest: false,
    blending: THREE.AdditiveBlending
  });
  var points = new THREE.Points(geo, mat);
  points.renderOrder = 45;
  points.frustumCulled = false;
  points.position.set(0, 0.20, 1.53);
  stageLyrics.group.add(points);
  stageLyrics.starRiver = points;
  return points;
}

function updateLyricStarRiver(dt) {
  var river = ensureLyricStarRiver();
  if (!river || !river.material || !river.material.uniforms) return;
  if (fx && fx.preset === SKULL_PRESET_INDEX) {
    river.visible = false;
    if (river.material.uniforms.uOpacity) river.material.uniforms.uOpacity.value = 0;
    return;
  }
  var u = river.material.uniforms;
  var data = stageLyrics.current && stageLyrics.current.userData ? stageLyrics.current.userData.lyric : null;
  var targetW = data ? clampRange((data.textWorldW || data.worldW || 4.2) * 1.12 + 0.80, 2.25, 7.20) : 3.4;
  var targetH = data ? clampRange((data.textWorldH || data.worldH || 0.58) * 1.85 + 0.18, 0.52, 1.35) : 0.58;
  stageLyrics.starRiverWidth += (targetW - stageLyrics.starRiverWidth) * Math.min(1, dt * 5.2);
  stageLyrics.starRiverHeight += (targetH - stageLyrics.starRiverHeight) * Math.min(1, dt * 4.6);
  u.uWidth.value = stageLyrics.starRiverWidth;
  u.uHeight.value = stageLyrics.starRiverHeight;
  var lyricGlowStrength = fx.lyricGlow ? Math.min(0.85, Math.max(0, fx.lyricGlowStrength)) : 0;
  var targetOpacity = (stageLyrics.current && fx.lyricGlowParticles)
    ? clampRange(0.22 + lyricGlowStrength * 0.58 + stageLyrics.highBloom * 0.16 + stageLyrics.beatGlow * 0.12, 0.16, 0.86)
    : 0;
  u.uOpacity.value += (targetOpacity - u.uOpacity.value) * (targetOpacity > u.uOpacity.value ? 0.10 : 0.055);
  u.uColorA.value.copy(lyricThreeColor(stageLyrics.palette.secondary || stageLyrics.palette.primary, '#9cffdf', 0.42));
  u.uColorB.value.copy(lyricThreeColor(stageLyrics.palette.highlight || stageLyrics.palette.primary, '#fff7d2', 0.46));
  river.visible = u.uOpacity.value > 0.01 || !!stageLyrics.current;
  var t = uniforms.uTime.value;
  river.position.y += ((0.18 + Math.sin(t * 0.44) * 0.035 + Math.sin(t * 0.91 + 1.7) * 0.018) - river.position.y) * 0.08;
  river.position.z += ((1.54 + Math.cos(t * 0.31) * 0.060) - river.position.z) * 0.08;
  river.rotation.z = Math.sin(t * 0.22) * 0.012;
}

function disposeLyricMesh(mesh) {
  if (!mesh) return;
  if (mesh.parent) mesh.parent.remove(mesh);
  mesh.traverse(function(obj){
    if (obj.material) {
      if (Array.isArray(obj.material)) {
        obj.material.forEach(function(m){ if (m.map) m.map.dispose(); m.dispose(); });
      } else {
        if (obj.material.map) obj.material.map.dispose();
        if (obj.material.uniforms && obj.material.uniforms.uMap && obj.material.uniforms.uMap.value) obj.material.uniforms.uMap.value.dispose();
        obj.material.dispose();
      }
    }
    if (obj.geometry) obj.geometry.dispose();
  });
}

function clamp01(v) { return Math.max(0, Math.min(1, v)); }
function rgbToHsl(r, g, b) {
  r /= 255; g /= 255; b /= 255;
  var max = Math.max(r, g, b), min = Math.min(r, g, b);
  var h = 0, s = 0, l = (max + min) / 2;
  if (max !== min) {
    var d = max - min;
    s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
    if (max === r) h = (g - b) / d + (g < b ? 6 : 0);
    else if (max === g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h /= 6;
  }
  return { h:h, s:s, l:l };
}
function hslToRgb(h, s, l) {
  function hue2rgb(p, q, t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1/6) return p + (q - p) * 6 * t;
    if (t < 1/2) return q;
    if (t < 2/3) return p + (q - p) * (2/3 - t) * 6;
    return p;
  }
  var r, g, b;
  if (s === 0) r = g = b = l;
  else {
    var q = l < 0.5 ? l * (1 + s) : l + s - l * s;
    var p = 2 * l - q;
    r = hue2rgb(p, q, h + 1/3);
    g = hue2rgb(p, q, h);
    b = hue2rgb(p, q, h - 1/3);
  }
  return { r:Math.round(r * 255), g:Math.round(g * 255), b:Math.round(b * 255) };
}
function rgbCss(c, a) {
  if (a == null) return 'rgb(' + c.r + ',' + c.g + ',' + c.b + ')';
  return 'rgba(' + c.r + ',' + c.g + ',' + c.b + ',' + a + ')';
}
function clampRange(v, min, max) { return Math.max(min, Math.min(max, v)); }
function normalizeCoverResolution(v) {
  return clampRange(Number(v) || 1, 0.75, 1.55);
}
function normalizePerformanceBackgroundMode(v, liveKeepFallback) {
  var value = String(v || '');
  if (value === 'keep' || liveKeepFallback === true) return 'keep';
  if (value === 'release') return 'release';
  return 'auto';
}
function normalizePerformanceQuality(v) {
  var value = String(v || '');
  return /^(eco|balanced|high|ultra)$/.test(value) ? value : fxDefaults.performanceQuality;
}
function coverParticleGridForResolution(v) {
  var grid = Math.round(118 * normalizeCoverResolution(v));
  grid = Math.max(88, Math.min(183, grid));
  return grid % 2 ? grid : grid + 1;
}
function coverParticleCountLabel(v) {
  var grid = coverParticleGridForResolution(v);
  return grid + 'x' + grid;
}
function coverTextureSizeForResolution(v) {
  v = normalizeCoverResolution(v);
  if (v >= 1.32) return 512;
  if (v >= 1.10) return 384;
  return 256;
}
function readSavedLyricLayout() {
  try {
    var savedLayoutRaw = localStorage.getItem(LYRIC_LAYOUT_STORE_KEY);
    var raw = savedLayoutRaw ? (JSON.parse(savedLayoutRaw) || {}) : packagedDefaultLyricLayoutRaw();
    var savedPreset = clampRange(Number(raw.preset) || 0, 0, 6);
    if (savedPreset === 3 && raw.visualPresetSchema !== VISUAL_PRESET_SCHEMA) {
      savedPreset = 5;
    }
    var savedBgColor = normalizeHexColor(raw.backgroundColor || '#000000', '#000000');
    var savedBgOpacity = clampRange(raw.backgroundOpacity == null ? fxDefaults.backgroundOpacity : Number(raw.backgroundOpacity), 0, 1);
    var savedGlassOffset = clampRange(raw.controlGlassChromaticOffset == null ? fxDefaults.controlGlassChromaticOffset : Number(raw.controlGlassChromaticOffset), 0, 140);
    var savedBgMode = /^(cover|custom)$/.test(String(raw.backgroundColorMode || '')) ? String(raw.backgroundColorMode) : '';
    var savedBgCustom = savedBgMode
      ? savedBgMode === 'custom'
      : (raw.backgroundColorCustom === true || (raw.backgroundColorCustom !== false && savedBgColor !== '#000000') || savedBgOpacity < 1);
    var desktopLyricsSchemaReady = raw.desktopLyricsSchema === 'desktop-lyrics-v3';
    var savedShelfCameraMode = normalizeShelfCameraMode(raw.shelfCameraMode || fxDefaults.shelfCameraMode);
    var savedShelfAngleManual = raw.shelfAngleYManual === true;
    var savedShelfAngle = savedShelfAngleManual
      ? clampRange(raw.shelfAngleY == null ? shelfDefaultAngleForCameraMode(savedShelfCameraMode) : Number(raw.shelfAngleY), -30, 30)
      : shelfDefaultAngleForCameraMode(savedShelfCameraMode);
    return {
      preset: savedPreset,
      intensity: clampRange(Number(raw.intensity) || fxDefaults.intensity, 0.2, 1.6),
      cinemaShake: clampRange(Number(raw.cinemaShake) || fxDefaults.cinemaShake, 0, 1.8),
      depth: clampRange(Number(raw.depth) || fxDefaults.depth, 0.2, 1.8),
      point: clampRange(Number(raw.point) || fxDefaults.point, 0.5, 2.2),
      speed: clampRange(Number(raw.speed) || fxDefaults.speed, 0.2, 2.5),
      twist: clampRange(Number(raw.twist) || fxDefaults.twist, 0, 0.6),
      color: clampRange(Number(raw.color) || fxDefaults.color, 0.5, 2.0),
      scatter: clampRange(Number(raw.scatter) || fxDefaults.scatter, 0, 0.5),
      bgFade: clampRange(Number(raw.bgFade) || fxDefaults.bgFade, 0, 1.2),
      bloomStrength: clampRange(Number(raw.bloomStrength) || fxDefaults.bloomStrength, 0, 1.6),
      lyricGlowStrength: clampRange(Number(raw.lyricGlowStrength) || fxDefaults.lyricGlowStrength, 0, 0.85),
      lyricScale: clampRange(Number(raw.lyricScale) || 1, 0.35, 1.65),
      lyricOffsetX: clampRange(Number(raw.lyricOffsetX) || 0, -2.0, 2.0),
      lyricOffsetY: clampRange(Number(raw.lyricOffsetY) || 0, -1.2, 1.35),
      lyricOffsetZ: clampRange(Number(raw.lyricOffsetZ) || 0, -1.6, 1.6),
      lyricTiltX: clampRange(Number(raw.lyricTiltX) || 0, -42, 42),
      lyricTiltY: clampRange(Number(raw.lyricTiltY) || 0, -42, 42),
      lyricCameraLock: !!raw.lyricCameraLock,
      lyricColorMode: raw.lyricColorMode === 'custom' ? 'custom' : 'auto',
      lyricColor: normalizeHexColor(raw.lyricColor || '#a9b8c8'),
      lyricHighlightMode: raw.lyricHighlightMode === 'custom' ? 'custom' : 'auto',
      lyricHighlightColor: normalizeHexColor(raw.lyricHighlightColor || '#fff0b8'),
      lyricGlowLinked: raw.lyricGlowLinked !== false,
      lyricGlowColor: normalizeHexColor(raw.lyricGlowColor || '#9db8cf'),
      lyricFont: normalizeLyricFontKey(raw.lyricFont),
      lyricLetterSpacing: clampRange(Number(raw.lyricLetterSpacing) || 0, -0.04, 0.18),
      lyricLineHeight: clampRange(Number(raw.lyricLineHeight) || 1, 0.86, 1.35),
      lyricWeight: clampRange(Number(raw.lyricWeight) || 900, 500, 900),
      lyricGlow: raw.lyricGlow !== false,
      lyricGlowBeat: raw.lyricGlowBeat !== false,
      lyricGlowParticles: !!raw.lyricGlowParticles,
      cinema: raw.cinema !== false,
      bloom: raw.bloom === true,
      edge: raw.edge === true,
      visualTintMode: raw.visualTintMode === 'custom' ? 'custom' : 'auto',
      visualTintColor: normalizeHexColor(raw.visualTintColor || '#9db8cf'),
      uiAccentColor: normalizeHexColor(raw.uiAccentColor || '#00f5d4', '#00f5d4'),
      homeAccentColor: normalizeHexColor(raw.homeAccentColor || '#00f5d4'),
      homeIconColor: normalizeHexColor(raw.homeIconColor || fxDefaults.homeIconColor || '#f4d28a', '#f4d28a'),
      visualIconColor: normalizeHexColor(raw.visualIconColor || fxDefaults.visualIconColor || '#7fd8ff', '#7fd8ff'),
      backgroundColorMode: savedBgCustom ? 'custom' : 'cover',
      backgroundColor: savedBgColor,
      backgroundOpacity: savedBgOpacity,
      controlGlassChromaticOffset: savedGlassOffset,
      backgroundColorCustom: savedBgCustom,
      backgroundImage: normalizeCustomBackgroundImage(raw.backgroundImage),
      backgroundMedia: normalizeCustomBackgroundMedia(raw.backgroundMedia || raw.backgroundImage),
      desktopLyrics: raw.desktopLyrics === true,
      desktopLyricsSize: clampRange(Number(raw.desktopLyricsSize) || fxDefaults.desktopLyricsSize, 0.72, 1.55),
      desktopLyricsOpacity: clampRange(raw.desktopLyricsOpacity == null ? fxDefaults.desktopLyricsOpacity : Number(raw.desktopLyricsOpacity), 0.28, 1),
      desktopLyricsY: clampRange(raw.desktopLyricsY == null ? fxDefaults.desktopLyricsY : Number(raw.desktopLyricsY), 0.08, 0.92),
      desktopLyricsClickThrough: desktopLyricsSchemaReady ? raw.desktopLyricsClickThrough === true : fxDefaults.desktopLyricsClickThrough,
      desktopLyricsCinema: desktopLyricsSchemaReady ? raw.desktopLyricsCinema !== false : fxDefaults.desktopLyricsCinema,
      desktopLyricsHighlight: desktopLyricsSchemaReady ? raw.desktopLyricsHighlight === true : fxDefaults.desktopLyricsHighlight,
      desktopLyricsFps: desktopLyricsSchemaReady ? normalizeDesktopLyricsFps(raw.desktopLyricsFps) : fxDefaults.desktopLyricsFps,
      performanceBackground: normalizePerformanceBackgroundMode(raw.performanceBackground, raw.liveBackgroundKeep === true),
      performanceQuality: normalizePerformanceQuality(raw.performanceQuality),
      liveBackgroundKeep: normalizePerformanceBackgroundMode(raw.performanceBackground, raw.liveBackgroundKeep === true) === 'keep',
      wallpaperMode: false,
      wallpaperOpacity: clampRange(raw.wallpaperOpacity == null ? fxDefaults.wallpaperOpacity : Number(raw.wallpaperOpacity), 0.35, 1),
      coverResolution: normalizeCoverResolution(raw.coverResolution),
      shelf: /^(off|side|stage)$/.test(String(raw.shelf || '')) ? raw.shelf : fxDefaults.shelf,
      shelfCameraMode: savedShelfCameraMode,
      shelfPresence: normalizeShelfPresence(raw.shelfPresence || fxDefaults.shelfPresence),
      shelfShowPodcasts: raw.shelfShowPodcasts !== false,
      shelfMergeCollections: raw.shelfMergeCollections === true,
      shelfSize: clampRange(raw.shelfSize == null ? fxDefaults.shelfSize : Number(raw.shelfSize), 0.65, 1.45),
      shelfOffsetX: clampRange(raw.shelfOffsetX == null ? fxDefaults.shelfOffsetX : Number(raw.shelfOffsetX), -1.2, 1.2),
      shelfOffsetY: clampRange(raw.shelfOffsetY == null ? fxDefaults.shelfOffsetY : Number(raw.shelfOffsetY), -0.9, 0.9),
      shelfOffsetZ: clampRange(raw.shelfOffsetZ == null ? fxDefaults.shelfOffsetZ : Number(raw.shelfOffsetZ), -0.9, 0.9),
      shelfAngleY: savedShelfAngle,
      shelfAngleYManual: savedShelfAngleManual,
      shelfOpacity: clampRange(raw.shelfOpacity == null ? fxDefaults.shelfOpacity : Number(raw.shelfOpacity), 0.25, 1),
      shelfBgOpacity: clampRange(raw.shelfBgOpacity == null ? fxDefaults.shelfBgOpacity : Number(raw.shelfBgOpacity), 0.25, 0.98),
      shelfAccentColor: normalizeHexColor(raw.shelfAccentColor || fxDefaults.shelfAccentColor, fxDefaults.shelfAccentColor),
      cam: /^(off|gesture)$/.test(String(raw.cam || '')) ? raw.cam : fxDefaults.cam
    };
  } catch (e) {
    return {};
  }
}
function saveLyricLayout() {
  try {
    var presetForSave = startupVisualPreviewActive && !playing && currentIdx < 0
      ? playbackVisualPreset
      : clampRange(Number(fx.preset) || 0, 0, presetMeta.length - 1);
    localStorage.setItem(LYRIC_LAYOUT_STORE_KEY, JSON.stringify({
      visualPresetSchema: VISUAL_PRESET_SCHEMA,
      desktopLyricsSchema: 'desktop-lyrics-v3',
      preset: presetForSave,
      intensity: clampRange(Number(fx.intensity) || fxDefaults.intensity, 0.2, 1.6),
      cinemaShake: clampRange(Number(fx.cinemaShake) || fxDefaults.cinemaShake, 0, 1.8),
      depth: clampRange(Number(fx.depth) || fxDefaults.depth, 0.2, 1.8),
      point: clampRange(Number(fx.point) || fxDefaults.point, 0.5, 2.2),
      speed: clampRange(Number(fx.speed) || fxDefaults.speed, 0.2, 2.5),
      twist: clampRange(Number(fx.twist) || fxDefaults.twist, 0, 0.6),
      color: clampRange(Number(fx.color) || fxDefaults.color, 0.5, 2.0),
      scatter: clampRange(Number(fx.scatter) || fxDefaults.scatter, 0, 0.5),
      bgFade: clampRange(Number(fx.bgFade) || fxDefaults.bgFade, 0, 1.2),
      bloomStrength: clampRange(Number(fx.bloomStrength) || fxDefaults.bloomStrength, 0, 1.6),
      lyricGlowStrength: clampRange(Number(fx.lyricGlowStrength) || fxDefaults.lyricGlowStrength, 0, 0.85),
      lyricScale: clampRange(Number(fx.lyricScale) || 1, 0.35, 1.65),
      lyricOffsetX: clampRange(Number(fx.lyricOffsetX) || 0, -2.0, 2.0),
      lyricOffsetY: clampRange(Number(fx.lyricOffsetY) || 0, -1.2, 1.35),
      lyricOffsetZ: clampRange(Number(fx.lyricOffsetZ) || 0, -1.6, 1.6),
      lyricTiltX: clampRange(Number(fx.lyricTiltX) || 0, -42, 42),
      lyricTiltY: clampRange(Number(fx.lyricTiltY) || 0, -42, 42),
      lyricCameraLock: !!fx.lyricCameraLock,
      lyricColorMode: fx.lyricColorMode === 'custom' ? 'custom' : 'auto',
      lyricColor: normalizeHexColor(fx.lyricColor || '#a9b8c8'),
      lyricHighlightMode: fx.lyricHighlightMode === 'custom' ? 'custom' : 'auto',
      lyricHighlightColor: normalizeHexColor(fx.lyricHighlightColor || '#fff0b8'),
      lyricGlowLinked: fx.lyricGlowLinked !== false,
      lyricGlowColor: normalizeHexColor(fx.lyricGlowColor || '#9db8cf'),
      lyricFont: normalizeLyricFontKey(fx.lyricFont),
      lyricLetterSpacing: clampRange(Number(fx.lyricLetterSpacing) || 0, -0.04, 0.18),
      lyricLineHeight: clampRange(Number(fx.lyricLineHeight) || 1, 0.86, 1.35),
      lyricWeight: clampRange(Number(fx.lyricWeight) || 900, 500, 900),
      lyricGlow: !!fx.lyricGlow,
      lyricGlowBeat: !!fx.lyricGlowBeat,
      lyricGlowParticles: !!fx.lyricGlowParticles,
      cinema: !!fx.cinema,
      bloom: !!fx.bloom,
      edge: !!fx.edge,
      visualTintMode: fx.visualTintMode === 'custom' ? 'custom' : 'auto',
      visualTintColor: normalizeHexColor(fx.visualTintColor || '#9db8cf'),
      uiAccentColor: normalizeHexColor(fx.uiAccentColor || '#00f5d4', '#00f5d4'),
      homeAccentColor: normalizeHexColor(fx.homeAccentColor || '#00f5d4'),
      homeIconColor: normalizeHexColor(fx.homeIconColor || '#f4d28a', '#f4d28a'),
      visualIconColor: normalizeHexColor(fx.visualIconColor || '#7fd8ff', '#7fd8ff'),
      backgroundColorMode: fx.backgroundColorMode === 'custom' || fx.backgroundColorCustom ? 'custom' : 'cover',
      backgroundColor: normalizeHexColor(fx.backgroundColor || '#000000', '#000000'),
      backgroundOpacity: clampRange(fx.backgroundOpacity == null ? fxDefaults.backgroundOpacity : Number(fx.backgroundOpacity), 0, 1),
      controlGlassChromaticOffset: clampRange(fx.controlGlassChromaticOffset == null ? fxDefaults.controlGlassChromaticOffset : Number(fx.controlGlassChromaticOffset), 0, 140),
      backgroundColorCustom: fx.backgroundColorMode === 'custom' || !!fx.backgroundColorCustom,
      backgroundImage: normalizeCustomBackgroundImage(fx.backgroundImage),
      backgroundMedia: normalizeCustomBackgroundMedia(fx.backgroundMedia || fx.backgroundImage),
      desktopLyrics: !!fx.desktopLyrics,
      desktopLyricsSize: clampRange(Number(fx.desktopLyricsSize) || fxDefaults.desktopLyricsSize, 0.72, 1.55),
      desktopLyricsOpacity: clampRange(fx.desktopLyricsOpacity == null ? fxDefaults.desktopLyricsOpacity : Number(fx.desktopLyricsOpacity), 0.28, 1),
      desktopLyricsY: clampRange(fx.desktopLyricsY == null ? fxDefaults.desktopLyricsY : Number(fx.desktopLyricsY), 0.08, 0.92),
      desktopLyricsClickThrough: fx.desktopLyricsClickThrough === true,
      desktopLyricsCinema: fx.desktopLyricsCinema !== false,
      desktopLyricsHighlight: fx.desktopLyricsHighlight === true,
      desktopLyricsFps: normalizeDesktopLyricsFps(fx.desktopLyricsFps),
      performanceBackground: normalizePerformanceBackgroundMode(fx.performanceBackground, fx.liveBackgroundKeep === true),
      performanceQuality: normalizePerformanceQuality(fx.performanceQuality),
      liveBackgroundKeep: normalizePerformanceBackgroundMode(fx.performanceBackground, fx.liveBackgroundKeep === true) === 'keep',
      wallpaperMode: false,
      wallpaperOpacity: clampRange(fx.wallpaperOpacity == null ? fxDefaults.wallpaperOpacity : Number(fx.wallpaperOpacity), 0.35, 1),
      coverResolution: normalizeCoverResolution(fx.coverResolution),
      shelf: /^(off|side|stage)$/.test(String(fx.shelf || '')) ? fx.shelf : fxDefaults.shelf,
      shelfCameraMode: normalizeShelfCameraMode(fx.shelfCameraMode || fxDefaults.shelfCameraMode),
      shelfPresence: normalizeShelfPresence(fx.shelfPresence || fxDefaults.shelfPresence),
      shelfShowPodcasts: fx.shelfShowPodcasts !== false,
      shelfMergeCollections: fx.shelfMergeCollections === true,
      shelfSize: clampRange(fx.shelfSize == null ? fxDefaults.shelfSize : Number(fx.shelfSize), 0.65, 1.45),
      shelfOffsetX: clampRange(fx.shelfOffsetX == null ? fxDefaults.shelfOffsetX : Number(fx.shelfOffsetX), -1.2, 1.2),
      shelfOffsetY: clampRange(fx.shelfOffsetY == null ? fxDefaults.shelfOffsetY : Number(fx.shelfOffsetY), -0.9, 0.9),
      shelfOffsetZ: clampRange(fx.shelfOffsetZ == null ? fxDefaults.shelfOffsetZ : Number(fx.shelfOffsetZ), -0.9, 0.9),
      shelfAngleY: clampRange(fx.shelfAngleY == null ? fxDefaults.shelfAngleY : Number(fx.shelfAngleY), -30, 30),
      shelfAngleYManual: fx.shelfAngleYManual === true,
      shelfOpacity: clampRange(fx.shelfOpacity == null ? fxDefaults.shelfOpacity : Number(fx.shelfOpacity), 0.25, 1),
      shelfBgOpacity: clampRange(fx.shelfBgOpacity == null ? fxDefaults.shelfBgOpacity : Number(fx.shelfBgOpacity), 0.25, 0.98),
      shelfAccentColor: normalizeHexColor(fx.shelfAccentColor || fxDefaults.shelfAccentColor, fxDefaults.shelfAccentColor),
      cam: /^(off|gesture)$/.test(String(fx.cam || '')) ? fx.cam : fxDefaults.cam
    }));
  } catch (e) {}
}
function normalizeHexColor(value, fallback) {
  var hex = String(value || '').trim();
  if (/^#[0-9a-f]{3}$/i.test(hex)) {
    hex = '#' + hex.charAt(1) + hex.charAt(1) + hex.charAt(2) + hex.charAt(2) + hex.charAt(3) + hex.charAt(3);
  }
  fallback = /^#[0-9a-f]{6}$/i.test(String(fallback || '')) ? String(fallback).toLowerCase() : '#a9b8c8';
  return /^#[0-9a-f]{6}$/i.test(hex) ? hex.toLowerCase() : fallback;
}
function normalizeDesktopLyricsFps(value) {
  var n = Number(value);
  if (!isFinite(n) || n <= 0) return 0;
  if (n <= 26) return 24;
  if (n <= 45) return 30;
  if (n <= 90) return 60;
  return 120;
}
function normalizeShelfCameraMode(value) {
  return String(value || '') === 'static' ? 'static' : 'dynamic';
}
function shelfDefaultAngleForCameraMode(mode) {
  return normalizeShelfCameraMode(mode) === 'static' ? -15 : 0;
}
function applyShelfCameraDefaultAngle(force) {
  if (!fx) return;
  fx.shelfCameraMode = normalizeShelfCameraMode(fx.shelfCameraMode || fxDefaults.shelfCameraMode);
  if (force || fx.shelfAngleYManual !== true) {
    fx.shelfAngleYManual = false;
    fx.shelfAngleY = shelfDefaultAngleForCameraMode(fx.shelfCameraMode);
  } else {
    fx.shelfAngleY = Math.round(clampRange(Number(fx.shelfAngleY) || 0, -30, 30));
  }
}
function normalizeShelfPresence(value) {
  return String(value || '') === 'always' ? 'always' : 'auto';
}
function normalizedShelfNumber(key, fallback, min, max) {
  var value = fx && fx[key] != null ? Number(fx[key]) : fallback;
  if (!isFinite(value)) value = fallback;
  return clampRange(value, min, max);
}
function shelfSettings() {
  var angleDeg = fx && fx.shelfAngleYManual === true
    ? normalizedShelfNumber('shelfAngleY', shelfDefaultAngleForCameraMode(fx.shelfCameraMode), -30, 30)
    : shelfDefaultAngleForCameraMode(fx && fx.shelfCameraMode);
  return {
    size: normalizedShelfNumber('shelfSize', fxDefaults.shelfSize, 0.65, 1.45),
    x: normalizedShelfNumber('shelfOffsetX', fxDefaults.shelfOffsetX, -1.2, 1.2),
    y: normalizedShelfNumber('shelfOffsetY', fxDefaults.shelfOffsetY, -0.9, 0.9),
    z: normalizedShelfNumber('shelfOffsetZ', fxDefaults.shelfOffsetZ, -0.9, 0.9),
    angle: angleDeg * Math.PI / 180,
    opacity: normalizedShelfNumber('shelfOpacity', fxDefaults.shelfOpacity, 0.25, 1),
    bgOpacity: normalizedShelfNumber('shelfBgOpacity', fxDefaults.shelfBgOpacity, 0.25, 0.98),
    accent: normalizeHexColor((fx && fx.shelfAccentColor) || fxDefaults.shelfAccentColor, fxDefaults.shelfAccentColor)
  };
}
function shelfAlwaysVisible() {
  return !!(fx && normalizeShelfPresence(fx.shelfPresence) === 'always');
}
function shouldUseShelfDynamicCamera(type) {
  if (!/^shelf-/.test(String(type || ''))) return true;
  return !(fx && normalizeShelfCameraMode(fx.shelfCameraMode) === 'static');
}
function shelfAccentHex() {
  return normalizeHexColor((fx && fx.shelfAccentColor) || fxDefaults.shelfAccentColor, fxDefaults.shelfAccentColor);
}
function shelfAccentRgba(alpha, fallback) {
  var rgb = hexToRgb(shelfAccentHex());
  if (!rgb) return fallback || 'rgba(244,210,138,' + alpha + ')';
  return 'rgba(' + rgb.r + ',' + rgb.g + ',' + rgb.b + ',' + alpha + ')';
}
function rgbToHexColor(r, g, b) {
  function part(v) {
    return Math.max(0, Math.min(255, Math.round(v || 0))).toString(16).padStart(2, '0');
  }
  return '#' + part(r) + part(g) + part(b);
}
function normalizeLyricFontKey(value) {
  value = String(value || 'sans');
  return /^(sans|hei|song|bold-song|stone-song|kai-song|serif-en|gothic|editorial|humanist|round|mono|display)$/.test(value) ? value : 'sans';
}
function lyricFontStackForKey(key) {
  key = normalizeLyricFontKey(key);
  if (key === 'hei') return '"Noto Sans SC","Microsoft YaHei",SimHei,"PingFang SC",sans-serif';
  if (key === 'song') return '"Noto Serif SC","Source Han Serif SC",SimSun,"Songti SC",serif';
  if (key === 'bold-song') return '"Source Han Serif SC Heavy","Source Han Serif SC","Noto Serif SC Black","Noto Serif SC","STZhongsong","SimSun",serif';
  if (key === 'stone-song') return '"FZYaSongS-B-GB","FZCuSong-B09S","Source Han Serif SC Heavy","Noto Serif SC Black","STZhongsong","SimSun",serif';
  if (key === 'kai-song') return '"Kaiti SC","STKaiti","KaiTi","Source Han Serif SC","Noto Serif SC",serif';
  if (key === 'serif-en') return 'Georgia,"Times New Roman","Noto Serif SC","Source Han Serif SC",serif';
  if (key === 'gothic') return '"UnifrakturCook","UnifrakturMaguntia","Old English Text MT","Blackletter","Cinzel Decorative","Noto Serif SC",serif';
  if (key === 'editorial') return '"Didot","Bodoni 72","Libre Baskerville",Georgia,"Noto Serif SC",serif';
  if (key === 'humanist') return '"Avenir Next","Segoe UI","Inter","Noto Sans SC","PingFang SC",sans-serif';
  if (key === 'round') return '"HarmonyOS Sans SC","Microsoft YaHei UI","PingFang SC","Noto Sans SC",sans-serif';
  if (key === 'mono') return '"JetBrains Mono",Consolas,"Noto Sans SC","Microsoft YaHei",monospace';
  if (key === 'display') return '"Alibaba PuHuiTi","Noto Sans SC","PingFang SC","Microsoft YaHei",sans-serif';
  return 'Inter,"Noto Sans SC","PingFang SC","Microsoft YaHei",Arial,sans-serif';
}
function lyricFontWeightValue() {
  if (normalizeLyricFontKey(fx && fx.lyricFont) === 'stone-song') return 900;
  return Math.round(clampRange(Number(fx && fx.lyricWeight) || 900, 500, 900) / 50) * 50;
}
function lyricFontCss(fontSize) {
  return lyricFontWeightValue() + ' ' + fontSize + 'px ' + lyricFontStackForKey(fx && fx.lyricFont);
}
function lyricLetterSpacingPx(fontSize) {
  return clampRange(Number(fx && fx.lyricLetterSpacing) || 0, -0.04, 0.18) * Math.max(1, fontSize || 1);
}
function lyricLineHeightFactor() {
  return clampRange(Number(fx && fx.lyricLineHeight) || 1, 0.86, 1.35);
}
function measureTextWithLetterSpacing(ctx, text, spacing) {
  text = String(text || '');
  spacing = Number(spacing) || 0;
  if (!spacing || text.length < 2) return ctx.measureText(text).width;
  var chars = Array.from(text);
  var w = 0;
  for (var i = 0; i < chars.length; i++) {
    w += ctx.measureText(chars[i]).width;
    if (i < chars.length - 1) w += spacing;
  }
  return Math.max(1, w);
}
function lyricMeasureText(ctx, text, fontSize) {
  return measureTextWithLetterSpacing(ctx, text, lyricLetterSpacingPx(fontSize));
}
function drawTextWithLetterSpacing(ctx, text, x, y, spacing, stroke) {
  text = String(text || '');
  spacing = Number(spacing) || 0;
  if (!spacing || text.length < 2) {
    if (stroke) ctx.strokeText(text, x, y);
    else ctx.fillText(text, x, y);
    return;
  }
  var chars = Array.from(text);
  var align = ctx.textAlign || 'left';
  var width = measureTextWithLetterSpacing(ctx, text, spacing);
  var start = x;
  if (align === 'center') start = x - width / 2;
  else if (align === 'right' || align === 'end') start = x - width;
  ctx.textAlign = 'left';
  var cursor = start;
  for (var i = 0; i < chars.length; i++) {
    if (stroke) ctx.strokeText(chars[i], cursor, y);
    else ctx.fillText(chars[i], cursor, y);
    cursor += ctx.measureText(chars[i]).width + (i < chars.length - 1 ? spacing : 0);
  }
  ctx.textAlign = align;
}
function lyricFillText(ctx, text, x, y, fontSize) {
  drawTextWithLetterSpacing(ctx, text, x, y, lyricLetterSpacingPx(fontSize), false);
}
function lyricStrokeText(ctx, text, x, y, fontSize) {
  drawTextWithLetterSpacing(ctx, text, x, y, lyricLetterSpacingPx(fontSize), true);
}
function applyStonePrintTexture(ctx, W, H, fontSize) {
  if (normalizeLyricFontKey(fx && fx.lyricFont) !== 'stone-song') return;
  var size = clampRange(fontSize || 128, 42, 180);
  var bandTop = H * 0.10;
  var bandH = H * 0.80;
  ctx.save();
  ctx.globalCompositeOperation = 'destination-out';

  var noiseW = 300, noiseH = 110;
  var noise = document.createElement('canvas');
  noise.width = noiseW; noise.height = noiseH;
  var nctx = noise.getContext('2d');
  var img = nctx.createImageData(noiseW, noiseH);
  for (var p = 0; p < noiseW * noiseH; p++) {
    var x0 = p % noiseW;
    var y0 = Math.floor(p / noiseW);
    var vein = Math.sin(x0 * 0.19 + y0 * 0.043) * 0.10 + Math.sin(y0 * 0.31) * 0.06;
    var r = Math.random() + vein;
    var a = 0;
    if (r > 0.82) a = 78 + Math.random() * 92;
    else if (r > 0.62) a = 22 + Math.random() * 54;
    else if (r > 0.48) a = 4 + Math.random() * 24;
    img.data[p * 4] = 255;
    img.data[p * 4 + 1] = 255;
    img.data[p * 4 + 2] = 255;
    img.data[p * 4 + 3] = a;
  }
  nctx.putImageData(img, 0, 0);
  ctx.imageSmoothingEnabled = false;
  ctx.globalAlpha = 0.34;
  ctx.drawImage(noise, 0, bandTop, W, bandH);

  var chips = Math.round(size * 7.2);
  for (var i = 0; i < chips; i++) {
    var x = Math.random() * W;
    var y = bandTop + Math.random() * bandH;
    var w = 0.7 + Math.random() * (size * 0.052);
    var h = 0.45 + Math.random() * (size * 0.026);
    ctx.globalAlpha = 0.16 + Math.random() * 0.36;
    ctx.save();
    ctx.translate(x, y);
    ctx.rotate((Math.random() - 0.5) * 0.38);
    ctx.fillRect(-w / 2, -h / 2, w, h);
    ctx.restore();
  }

  ctx.lineCap = 'round';
  for (var s = 0; s < 44; s++) {
    var sx = Math.random() * W;
    var sy = bandTop + Math.random() * bandH;
    ctx.globalAlpha = 0.09 + Math.random() * 0.16;
    ctx.lineWidth = 0.45 + Math.random() * 1.2;
    ctx.beginPath();
    ctx.moveTo(sx, sy);
    ctx.lineTo(sx + 10 + Math.random() * 86, sy + (Math.random() - 0.5) * 4.8);
    ctx.stroke();
  }

  for (var c = 0; c < 26; c++) {
    var cx = Math.random() * W;
    var cy = bandTop + Math.random() * bandH;
    var radius = 1.8 + Math.random() * (size * 0.060);
    ctx.globalAlpha = 0.08 + Math.random() * 0.18;
    ctx.beginPath();
    ctx.ellipse(cx, cy, radius * (0.7 + Math.random() * 1.4), radius * (0.25 + Math.random() * 0.55), Math.random() * Math.PI, 0, Math.PI * 2);
    ctx.fill();
  }
  ctx.restore();
}
function hexToRgb(hex) {
  hex = normalizeHexColor(hex).slice(1);
  return {
    r: parseInt(hex.slice(0, 2), 16),
    g: parseInt(hex.slice(2, 4), 16),
    b: parseInt(hex.slice(4, 6), 16)
  };
}
function normalizeCustomBackgroundImage(value) {
  var src = String(value || '').trim();
  if (!src) return '';
  if (/^data:image\/(png|jpe?g|webp);base64,/i.test(src)) return src;
  if (/^https?:\/\//i.test(src)) return src;
  return '';
}
function normalizeCustomBackgroundMedia(value) {
  if (!value) return null;
  if (typeof value === 'string') {
    var img = normalizeCustomBackgroundImage(value);
    if (img) return { type: 'image', src: img };
    if (/^data:video\/(mp4|webm|quicktime);base64,/i.test(value) || /^https?:\/\//i.test(value)) return { type: 'video', src: String(value) };
    return null;
  }
  if (typeof value !== 'object') return null;
  var type = value.type === 'video' ? 'video' : (value.type === 'image' ? 'image' : '');
  if (type === 'image') {
    var imageSrc = normalizeCustomBackgroundImage(value.src || value.url || '');
    return imageSrc ? { type: 'image', src: imageSrc } : null;
  }
  if (type === 'video') {
    var src = String(value.src || '').trim();
    var id = String(value.id || '').trim();
    if (!id && !/^data:video\/(mp4|webm|quicktime);base64,/i.test(src) && !/^https?:\/\//i.test(src)) return null;
    return {
      type: 'video',
      id: id,
      src: src,
      name: String(value.name || '').slice(0, 120),
      mime: String(value.mime || '').slice(0, 80),
      size: Math.max(0, Number(value.size) || 0)
    };
  }
  return null;
}
function customBackgroundMediaLabel(media) {
  media = normalizeCustomBackgroundMedia(media);
  if (!media) return '未设置';
  return media.type === 'video' ? '视频已设置' : '图片已设置';
}
var CUSTOM_BG_DB_NAME = 'mineradio-custom-background-v1';
var CUSTOM_BG_STORE = 'media';
var customBgObjectUrl = '';
var customBgApplyToken = 0;
function openCustomBackgroundDb() {
  return new Promise(function(resolve, reject){
    if (!window.indexedDB) { reject(new Error('indexedDB unavailable')); return; }
    var req = indexedDB.open(CUSTOM_BG_DB_NAME, 1);
    req.onupgradeneeded = function(){
      var db = req.result;
      if (!db.objectStoreNames.contains(CUSTOM_BG_STORE)) db.createObjectStore(CUSTOM_BG_STORE, { keyPath: 'id' });
    };
    req.onsuccess = function(){ resolve(req.result); };
    req.onerror = function(){ reject(req.error || new Error('indexedDB open failed')); };
  });
}
async function putCustomBackgroundBlob(id, blob, meta) {
  var db = await openCustomBackgroundDb();
  return new Promise(function(resolve, reject){
    var tx = db.transaction(CUSTOM_BG_STORE, 'readwrite');
    tx.objectStore(CUSTOM_BG_STORE).put(Object.assign({ id: id, blob: blob, savedAt: Date.now() }, meta || {}));
    tx.oncomplete = function(){ db.close(); resolve(); };
    tx.onerror = function(){ db.close(); reject(tx.error || new Error('indexedDB put failed')); };
  });
}
async function getCustomBackgroundBlob(id) {
  var db = await openCustomBackgroundDb();
  return new Promise(function(resolve, reject){
    var tx = db.transaction(CUSTOM_BG_STORE, 'readonly');
    var req = tx.objectStore(CUSTOM_BG_STORE).get(id);
    req.onsuccess = function(){ resolve(req.result && req.result.blob ? req.result.blob : null); };
    req.onerror = function(){ reject(req.error || new Error('indexedDB get failed')); };
    tx.oncomplete = function(){ db.close(); };
  });
}
var colorLabState = { picker: null, id: '', h: 0, s: 1, v: 1, dragging: false };
var COLOR_LAB_PRESETS = [
  { name: '极黑', color: '#000000' },
  { name: '极白', color: '#ffffff' },
  { name: '克莱因蓝', color: '#002fa7' },
  { name: '法拉利红', color: '#f00000' },
  { name: '香槟金', color: '#c8a96a' },
  { name: '孔雀绿', color: '#006b5b' },
  { name: '午夜紫', color: '#2b164f' },
  { name: '银雾', color: '#d9dde2' }
];
function rgbToHsv(r, g, b) {
  r /= 255; g /= 255; b /= 255;
  var max = Math.max(r, g, b), min = Math.min(r, g, b);
  var d = max - min, h = 0;
  if (d) {
    if (max === r) h = ((g - b) / d + (g < b ? 6 : 0)) / 6;
    else if (max === g) h = ((b - r) / d + 2) / 6;
    else h = ((r - g) / d + 4) / 6;
  }
  return { h: h, s: max === 0 ? 0 : d / max, v: max };
}
function hsvToHex(h, s, v) {
  h = ((h % 1) + 1) % 1; s = clampRange(s, 0, 1); v = clampRange(v, 0, 1);
  var i = Math.floor(h * 6), f = h * 6 - i;
  var p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
  var r, g, b;
  switch (i % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
  return rgbToHexColor(r * 255, g * 255, b * 255);
}
function applyColorLabValue(hex, silent) {
  hex = normalizeHexColor(hex || '#000000', '#000000');
  var id = colorLabState.id;
  if (id === 'ui-accent-picker') setUiAccentColor(hex, true);
  else if (id === 'visual-tint-picker') setVisualTintCustom(hex, true);
  else if (id === 'home-accent-picker') setHomeAccentColor(hex, true);
  else if (id === 'home-icon-picker') setHomeIconColor(hex, true);
  else if (id === 'visual-icon-picker') setVisualIconColor(hex, true);
  else if (id === 'bg-color-picker') setCustomBackgroundColor(hex, true, true);
  else if (id === 'shelf-accent-picker') setShelfAccentColor(hex, true);
  else if (id === 'lyric-color-picker') setLyricColorCustom(hex, true);
  else if (id === 'lyric-highlight-picker') setLyricHighlightCustom(hex, true);
  else if (id === 'lyric-glow-picker') setLyricGlowCustom(hex, true);
  if (!silent) showToast('颜色: ' + hex.toUpperCase());
}
function syncColorLabUi(hex) {
  hex = normalizeHexColor(hex || '#000000', '#000000');
  var rgb = hexToRgb(hex);
  var hsv = rgbToHsv(rgb.r, rgb.g, rgb.b);
  colorLabState.h = hsv.h; colorLabState.s = hsv.s; colorLabState.v = hsv.v;
  var pop = byId('color-lab-pop');
  var sv = byId('color-lab-sv');
  var cursor = byId('color-lab-cursor');
  var hue = byId('color-lab-hue');
  var hexInput = byId('color-lab-hex');
  var preview = byId('color-lab-preview');
  var hueHex = hsvToHex(colorLabState.h, 1, 1);
  if (pop) {
    pop.style.setProperty('--lab-color', hex);
    pop.style.setProperty('--lab-hue', hueHex);
  }
  if (sv) sv.style.setProperty('--lab-hue', hueHex);
  if (cursor) { cursor.style.left = (colorLabState.s * 100).toFixed(2) + '%'; cursor.style.top = ((1 - colorLabState.v) * 100).toFixed(2) + '%'; }
  if (hue) hue.value = Math.round(colorLabState.h * 360);
  if (hexInput) hexInput.value = hex.toUpperCase();
  if (preview) preview.style.setProperty('--lab-color', hex);
}
function closeColorLab() {
  var pop = byId('color-lab-pop');
  if (pop) pop.classList.remove('show');
  colorLabState.picker = null;
  colorLabState.id = '';
}
function placeFxFloatingPanel(pop, anchor, opts) {
  if (!pop || !anchor || !anchor.getBoundingClientRect) return;
  opts = opts || {};
  var gap = opts.gap == null ? 12 : opts.gap;
  var pad = opts.pad == null ? 14 : opts.pad;
  var rect = anchor.getBoundingClientRect();
  var vw = Math.max(320, window.innerWidth || document.documentElement.clientWidth || 320);
  var vh = Math.max(320, window.innerHeight || document.documentElement.clientHeight || 320);
  var pw = Math.min(pop.offsetWidth || pop.getBoundingClientRect().width || 330, vw - pad * 2);
  var ph = Math.min(pop.offsetHeight || pop.getBoundingClientRect().height || 260, vh - pad * 2);
  var left;
  var top;
  if (vw < 760) {
    left = Math.max(pad, Math.min(vw - pw - pad, rect.left + rect.width / 2 - pw / 2));
    top = rect.bottom + gap;
    if (top + ph > vh - pad) top = Math.max(pad, rect.top - ph - gap);
  } else {
    var roomRight = vw - rect.right - pad;
    var roomLeft = rect.left - pad;
    if (roomRight >= pw + gap || roomRight >= roomLeft) left = rect.right + gap;
    else left = rect.left - pw - gap;
    left = Math.max(pad, Math.min(vw - pw - pad, left));
    top = rect.top + rect.height / 2 - ph / 2;
    top = Math.max(pad, Math.min(vh - ph - pad, top));
  }
  pop.style.left = Math.round(left) + 'px';
  pop.style.top = Math.round(top) + 'px';
  pop.style.transform = 'none';
}
function openColorLabForPicker(picker) {
  var pop = byId('color-lab-pop');
  if (!picker || !pop) return;
  if (pop.classList.contains('show') && colorLabState.picker === picker) {
    closeColorLab();
    return;
  }
  colorLabState.picker = picker;
  colorLabState.id = picker.id || '';
  var label = picker.closest('.lyric-color-row');
  var title = byId('color-lab-title');
  if (title) title.textContent = label ? (label.textContent || 'Color').replace(/#[0-9a-f]{6}/ig, '').trim().slice(0, 24) : 'Color';
  syncColorLabUi(picker.value || '#000000');
  var presets = byId('color-lab-presets');
  if (presets) {
    presets.innerHTML = COLOR_LAB_PRESETS.map(function(p){
      return '<button type="button" title="' + escHtml(p.name) + '" style="--c:' + p.color + '" data-color="' + p.color + '"></button>';
    }).join('');
  }
  pop.classList.add('show');
  placeFxFloatingPanel(pop, label || picker, { gap: 12, pad: 14 });
}
function updateColorLabFromSv(e) {
  var sv = byId('color-lab-sv');
  if (!sv) return;
  var rect = sv.getBoundingClientRect();
  colorLabState.s = clampRange((e.clientX - rect.left) / Math.max(1, rect.width), 0, 1);
  colorLabState.v = 1 - clampRange((e.clientY - rect.top) / Math.max(1, rect.height), 0, 1);
  var hex = hsvToHex(colorLabState.h, colorLabState.s, colorLabState.v);
  syncColorLabUi(hex);
  applyColorLabValue(hex, true);
}
function bindColorLabPicker(picker) {
  if (!picker || picker._colorLabBound) return;
  picker._colorLabBound = true;
  picker.setAttribute('aria-haspopup', 'dialog');
  picker.setAttribute('data-color-lab-picker', '1');
  function openFromPickerEvent(e) {
    if (e) {
      e.preventDefault();
      e.stopPropagation();
    }
    picker._colorLabOpenedAt = Date.now();
    openColorLabForPicker(picker);
  }
  picker.addEventListener('pointerdown', openFromPickerEvent);
  picker.addEventListener('mousedown', function(e){ e.preventDefault(); e.stopPropagation(); });
  picker.addEventListener('click', function(e){
    e.preventDefault();
    e.stopPropagation();
    if (Date.now() - (picker._colorLabOpenedAt || 0) < 260) return;
    openColorLabForPicker(picker);
  });
  picker.addEventListener('keydown', function(e){
    if (e.key === 'Enter' || e.key === ' ') openFromPickerEvent(e);
  });
}
function liftFxFloatingPopups() {
  ['cover-color-pop', 'color-lab-pop', 'cover-color-loupe'].forEach(function(id){
    var el = byId(id);
    if (el && el.parentElement !== document.body) document.body.appendChild(el);
  });
}
function bindColorLabRows() {
  __qsa('.lyric-color-row').forEach(function(row){
    if (!row || row._colorLabRowBound || row.classList.contains('linked')) return;
    var picker = row.querySelector('.lyric-color-picker');
    if (!picker) return;
    row._colorLabRowBound = true;
    row.addEventListener('pointerdown', function(e){
      if (!e || !e.target) return;
      if (e.target.closest('button,.fx-mini-btn,input[type="range"],select,textarea')) return;
      e.preventDefault();
      e.stopPropagation();
      picker._colorLabOpenedAt = Date.now();
      openColorLabForPicker(picker);
    });
  });
}
function repositionFxFloatingPanels() {
  var colorPop = byId('color-lab-pop');
  if (colorPop && colorPop.classList.contains('show') && colorLabState.picker) {
    placeFxFloatingPanel(colorPop, colorLabState.picker.closest('.lyric-color-row') || colorLabState.picker, { gap: 12, pad: 14 });
  }
  var coverPop = byId('cover-color-pop');
  if (coverPop && coverPop.classList.contains('show')) {
    placeFxFloatingPanel(coverPop, byId('visual-tint-auto-btn') || byId('visual-tint-picker') || coverPop, { gap: 12, pad: 14 });
  }
}
__on(window, 'resize', function(){
  if (window.requestAnimationFrame) requestAnimationFrame(repositionFxFloatingPanels);
  else repositionFxFloatingPanels();
});
function uiAccentHex(fallback) {
  return normalizeHexColor((fx && fx.uiAccentColor) || fallback || '#00f5d4', fallback || '#00f5d4');
}
function uiAccentRgba(alpha, fallback) {
  var c = hexToRgb(uiAccentHex(fallback));
  return 'rgba(' + c.r + ',' + c.g + ',' + c.b + ',' + (alpha == null ? 1 : alpha) + ')';
}
function readableInkForHex(hex) {
  var c = hexToRgb(hex || '#00f5d4');
  var lum = (c.r * 0.299 + c.g * 0.587 + c.b * 0.114) / 255;
  return lum > 0.54 ? '#06100f' : '#f8fbff';
}
function lyricPaletteFromHex(hex) {
  var c = hexToRgb(hex);
  var hsl = rgbToHsl(c.r, c.g, c.b);
  var neutral = hsl.s < 0.035;
  var s = neutral ? 0 : clampRange(hsl.s * 1.08, 0.14, 0.92);
  var l = hsl.l;
  if (l < 0.11) l = 0.15 + l * 1.18;
  else if (l < 0.28) l = 0.21 + (l - 0.11) * 1.18;
  else l = clampRange(l, 0.30, 0.82);
  l = clampRange(l, 0.14, 0.84);
  var primary = hslToRgb(hsl.h, s, l);
  var secondary = hslToRgb((hsl.h + 0.055) % 1, neutral ? 0 : clampRange(s * 0.88, 0.12, 0.78), clampRange(l + (l < 0.38 ? 0.10 : -0.08), 0.18, 0.76));
  var highlight = hslToRgb((hsl.h + 0.018) % 1, neutral ? 0 : clampRange(s * 0.72, 0.10, 0.70), clampRange(l + 0.22, 0.38, 0.92));
  var darkText = l < 0.40;
  return {
    primary: rgbCss(primary),
    secondary: rgbCss(secondary),
    highlight: rgbCss(highlight),
    shadow: darkText ? 'rgba(0,6,10,0.46)' : 'rgba(248,253,255,0.34)',
    glow: rgbCss(primary, 0.26),
  };
}
function silverBlueLyricPalette() {
  return {
    primary: '#d8f1ff',
    secondary: '#9db8cf',
    highlight: '#eef7ff',
    shadow: 'rgba(0,7,12,0.48)',
    glow: 'rgba(138,190,255,0.26)',
  };
}
function setLyricSparkOpacity(data, value) {
  if (!data || !data.sparkMat) return;
  value = clampRange(Number(value) || 0, 0, 1);
  if (data.sparkMat.uniforms && data.sparkMat.uniforms.uOpacity) data.sparkMat.uniforms.uOpacity.value = value;
  else data.sparkMat.opacity = value;
}
function getLyricSparkOpacity(data) {
  if (!data || !data.sparkMat) return 0;
  if (data.sparkMat.uniforms && data.sparkMat.uniforms.uOpacity) return Number(data.sparkMat.uniforms.uOpacity.value) || 0;
  return Number(data.sparkMat.opacity) || 0;
}
function setLyricSparkSize(data, value) {
  if (!data || !data.sparkMat) return;
  value = Math.max(0.002, Number(value) || 0.035);
  if (data.sparkMat.uniforms && data.sparkMat.uniforms.uSize) data.sparkMat.uniforms.uSize.value = value;
  else data.sparkMat.size = value;
}
function getLyricSparkSize(data) {
  if (!data || !data.sparkMat) return 0.035;
  if (data.sparkMat.uniforms && data.sparkMat.uniforms.uSize) return Number(data.sparkMat.uniforms.uSize.value) || 0.035;
  return Number(data.sparkMat.size) || 0.035;
}
function setLyricSparkColor(data, color) {
  if (!data || !data.sparkMat) return;
  if (data.sparkMat.uniforms && data.sparkMat.uniforms.uColor) data.sparkMat.uniforms.uColor.value.copy(color);
  else if (data.sparkMat.color) data.sparkMat.color.copy(color);
}
function applyLyricPaletteToMesh(mesh) {
  if (!mesh || !mesh.userData || !mesh.userData.lyric) return;
  var pal = stageLyrics.palette || {};
  var data = mesh.userData.lyric;
  if (data.textMat && data.textMat.uniforms) {
    var u = data.textMat.uniforms;
    if (u.uBaseColor) u.uBaseColor.value.copy(lyricThreeColor(pal.primary, '#d6f8ff', 0.38));
    if (u.uHiColor) u.uHiColor.value.copy(lyricThreeColor(pal.highlight || pal.primary, '#fff0b8', 0.48));
    if (u.uGlowColor) u.uGlowColor.value.copy(lyricThreeColor(pal.glowColor || pal.secondary || pal.primary, '#9cffdf', 0.36));
    if (u.uSolarColor) u.uSolarColor.value.copy(lyricThreeColor(pal.highlight || pal.secondary || pal.primary, '#fff0b8', 0.50));
    if (u.uSolar && !isFinite(u.uSolar.value)) u.uSolar.value = 0;
    if (u.uOpacity && !isFinite(u.uOpacity.value)) u.uOpacity.value = 0;
    data.textMat.needsUpdate = true;
  }
  if (data.glowMat) data.glowMat.color.copy(lyricThreeColor(pal.glowColor || pal.secondary || pal.primary, '#9cffdf', 0.36));
  if (data.sparkMat) setLyricSparkColor(data, lyricThreeColor(pal.highlight || pal.secondary || pal.primary, '#fff0b8', 0.46));
  if (data.sunMat) data.sunMat.color.copy(lyricThreeColor(pal.highlight || pal.secondary || pal.primary, '#fff0b8', 0.50));
}
function effectiveLyricPalette(pal) {
  var src = pal || stageLyrics.coverPalette || stageLyrics.palette || {};
  var out = {
    primary: src.primary || '#d6f8ff',
    secondary: src.secondary || '#9cffdf',
    highlight: src.highlight || '#eef7ff',
    shadow: src.shadow || 'rgba(2,8,12,0.42)',
    glow: src.glow || 'rgba(143,233,255,0.34)'
  };
  if (fx.lyricHighlightMode === 'custom') {
    var hi = lyricPaletteFromHex(fx.lyricHighlightColor);
    out.highlight = hi.primary;
    if (fx.lyricGlowLinked !== false) {
      out.glowColor = hi.secondary || hi.primary;
      out.glow = hi.glow || out.glow;
    }
  }
  if (fx.lyricGlowLinked === false) {
    var glowPal = lyricPaletteFromHex(fx.lyricGlowColor || '#9db8cf');
    out.glowColor = glowPal.primary;
    out.glow = glowPal.glow || out.glow;
  }
  if (!out.glowColor) out.glowColor = out.secondary;
  return out;
}
function setStageLyricPalette(pal) {
  stageLyrics.palette = effectiveLyricPalette(pal);
  lyricSunColor.copy(lyricThreeColor(stageLyrics.palette.glowColor || stageLyrics.palette.secondary || stageLyrics.palette.primary, '#ffe6a4', 0.44));
  lyricSunHotColor.copy(lyricThreeColor(stageLyrics.palette.highlight || stageLyrics.palette.primary, '#fff4cc', 0.54));
  applyLyricPaletteToMesh(stageLyrics.current);
  stageLyrics.outgoing.forEach(applyLyricPaletteToMesh);
  syncSkullParticleColors();
}
function lyricTextPaletteFromHsl(hsl, avgL, chroma) {
  if (avgL < 0.16 || chroma < 0.08) {
    return silverBlueLyricPalette();
  }
  var hue = hsl.h;
  if (avgL < 0.30 && (hue < 0.06 || hue > 0.86 || (hue > 0.75 && hue < 0.86))) return silverBlueLyricPalette();
  if (avgL > 0.82 && chroma < 0.12) {
    return {
      primary: '#064b5b',
      secondary: '#168c88',
      highlight: '#315f68',
      shadow: 'rgba(255,255,255,0.48)',
      glow: 'rgba(143,233,255,0.14)',
    };
  }
  var lightText = avgL < 0.52;
  var s = Math.max(0.42, Math.min(0.78, hsl.s + 0.16));
  var c1 = hslToRgb(hsl.h, s, lightText ? 0.74 : 0.34);
  var c2 = hslToRgb((hsl.h + 0.08) % 1, Math.max(0.36, s - 0.10), lightText ? 0.62 : 0.46);
  return {
    primary: rgbCss(c1),
    secondary: rgbCss(c2),
    highlight: rgbCss(hslToRgb((hsl.h + 0.03) % 1, Math.max(0.28, s - 0.18), lightText ? 0.86 : 0.58)),
    shadow: lightText ? 'rgba(0,6,10,0.44)' : 'rgba(248,253,255,0.40)',
    glow: rgbCss(c1, lightText ? 0.24 : 0.14),
  };
}
function updateLyricPaletteFromCover(coverCanvas) {
  if (!coverCanvas) return;
  try {
    var ctx = coverCanvas.getContext('2d');
    var img = ctx.getImageData(0, 0, coverCanvas.width, coverCanvas.height).data;
    var w = coverCanvas.width, h = coverCanvas.height;
    var sumR = 0, sumG = 0, sumB = 0, count = 0;
    var best = { score:-1, r:143, g:233, b:255 };
    for (var y = 0; y < h; y += 8) {
      for (var x = 0; x < w; x += 8) {
        var di = (y * w + x) * 4;
        var r = img[di], g = img[di+1], b = img[di+2], a = img[di+3] / 255;
        if (a < 0.5) continue;
        var lum = (r * 0.299 + g * 0.587 + b * 0.114) / 255;
        var maxC = Math.max(r, g, b), minC = Math.min(r, g, b);
        var chroma = (maxC - minC) / 255;
        var edgePenalty = Math.abs(lum - 0.5);
        var score = chroma * 1.6 + (0.5 - edgePenalty) * 0.45;
        sumR += r; sumG += g; sumB += b; count++;
        if (lum > 0.08 && lum < 0.92 && score > best.score) best = { score:score, r:r, g:g, b:b };
      }
    }
    if (!count) return;
    var avgL = (sumR / count * 0.299 + sumG / count * 0.587 + sumB / count * 0.114) / 255;
    var hsl = rgbToHsl(best.r, best.g, best.b);
    stageLyrics.coverPalette = lyricTextPaletteFromHsl(hsl, avgL, Math.max(0, best.score));
    if (fx.lyricColorMode !== 'custom') setStageLyricPalette(stageLyrics.coverPalette);
  } catch (e) {}
}

function wrapLyricText(ctx, text, maxWidth, maxLines, fontSize) {
  text = String(text || '').trim();
  var useWords = /\s/.test(text) && /[A-Za-z0-9]/.test(text);
  var units = useWords ? text.split(/(\s+)/).filter(Boolean) : text.split('');
  var lines = [], line = '';
  for (var i = 0; i < units.length; i++) {
    var test = line + units[i];
    if (lyricMeasureText(ctx, test, fontSize) > maxWidth && line) {
      lines.push(line.trim());
      line = units[i].trimStart ? units[i].trimStart() : units[i].replace(/^\s+/, '');
      if (lines.length >= maxLines) {
        var rest = units.slice(i).join('').trim();
        if (rest) lines[lines.length - 1] = lines[lines.length - 1].replace(/[.。,…，、\s]*$/, '') + '...';
        return lines;
      }
    } else {
      line = test;
    }
  }
  if (line && lines.length < maxLines) lines.push(line.trim());
  return lines.length ? lines : [''];
}

function cssColorToThreeColor(css, fallback) {
  var c = new THREE.Color(fallback || '#d6f8ff');
  var value = String(css || fallback || '#d6f8ff').trim();
  try {
    if (/^#[0-9a-f]{3}$/i.test(value) || /^#[0-9a-f]{6}$/i.test(value)) {
      c.set(normalizeHexColor(value));
      return c;
    }
    var m = value.match(/^rgba?\(\s*([.\d]+)\s*,\s*([.\d]+)\s*,\s*([.\d]+)/i);
    if (m) {
      c.setRGB(
        Math.max(0, Math.min(255, parseFloat(m[1]))) / 255,
        Math.max(0, Math.min(255, parseFloat(m[2]))) / 255,
        Math.max(0, Math.min(255, parseFloat(m[3]))) / 255
      );
      return c;
    }
    c.setStyle(value);
  } catch (e) {
    try { c.set(normalizeHexColor(fallback || '#d6f8ff')); } catch (e2) {}
  }
  return c;
}
function lyricThreeColor(css, fallback, minLum) {
  var c = cssColorToThreeColor(css, fallback || '#d6f8ff');
  var lum = c.r * 0.299 + c.g * 0.587 + c.b * 0.114;
  var floor = minLum == null ? 0.34 : minLum;
  if (lum < floor) {
    var lift = floor - lum;
    c.r = Math.min(1, c.r + lift);
    c.g = Math.min(1, c.g + lift);
    c.b = Math.min(1, c.b + lift);
  }
  return c;
}

var STAGE_LYRIC_MAX_LINES = 1;

function makeLyricMask(text) {
  var canvas = document.createElement('canvas');
  var W = 2048, H = 384;
  canvas.width = W; canvas.height = H;
  var ctx = canvas.getContext('2d');
  var maxWidth = W - 190;
  var maxLines = STAGE_LYRIC_MAX_LINES;
  var fontSize = 128;
  // [muyun:explicit-newline] 显式 \n 分行（暮云宿主传「原文\n译文」双行歌词）：
  // 不参与自动换行逻辑，仅按换行切段；字号缩圈按各行最宽测量（下方循环天然支持）
  var rawStr = String(text || '');
  var explicitLines = null;
  if (rawStr.indexOf('\n') >= 0) {
    explicitLines = rawStr.split('\n')
      .map(function (s) { return s.replace(/\s+/g, ' ').trim(); })
      .filter(function (s) { return s.length > 0; })
      .slice(0, 3);
    if (explicitLines.length > 1) maxLines = explicitLines.length;
    else explicitLines = null;
  }
  text = rawStr.replace(/\s+/g, ' ').trim();
  var lines = explicitLines || [text];
  var widest = 1;
  for (; fontSize >= 42; fontSize -= 4) {
    ctx.font = lyricFontCss(fontSize);
    lines = explicitLines ? explicitLines : (maxLines > 1 && lyricMeasureText(ctx, text, fontSize) > maxWidth ? wrapLyricText(ctx, text, maxWidth, maxLines, fontSize) : [text]);
    widest = 1;
    for (var li = 0; li < lines.length; li++) widest = Math.max(widest, lyricMeasureText(ctx, lines[li], fontSize));
    if (widest <= maxWidth) break;
  }
  ctx.font = lyricFontCss(fontSize);
  if (!lines.length) lines = [''];
  widest = 1;
  for (var mi = 0; mi < lines.length; mi++) widest = Math.max(widest, lyricMeasureText(ctx, lines[mi], fontSize));
  var width = Math.min(maxWidth, widest);
  var fitScaleX = maxLines <= 1 && widest > maxWidth ? Math.max(0.68, maxWidth / widest) : 1;
  if (fitScaleX < 1) width = Math.min(maxWidth, widest * fitScaleX);
  var lineHeight = fontSize * (lines.length > 1 ? 1.02 : 1.0) * lyricLineHeightFactor();
  var blockH = fontSize + (lines.length - 1) * lineHeight;
  var x = W / 2, y0 = H / 2 - blockH / 2 + fontSize * 0.82;
  ctx.clearRect(0, 0, W, H);
  ctx.textAlign = 'center';
  ctx.textBaseline = 'alphabetic';
  ctx.fillStyle = '#fff';
  for (var di = 0; di < lines.length; di++) {
    if (fitScaleX < 1) {
      ctx.save();
      ctx.translate(x, 0);
      ctx.scale(fitScaleX, 1);
      lyricFillText(ctx, lines[di], 0, y0 + di * lineHeight, fontSize);
      ctx.restore();
    } else {
      lyricFillText(ctx, lines[di], x, y0 + di * lineHeight, fontSize);
    }
  }
  applyStonePrintTexture(ctx, W, H, fontSize);
  var tex = new THREE.CanvasTexture(canvas);
  tex.minFilter = THREE.LinearFilter;
  tex.magFilter = THREE.LinearFilter;
  tex.generateMipmaps = false;
  tex.anisotropy = Math.min(8, renderer.capabilities.getMaxAnisotropy ? renderer.capabilities.getMaxAnisotropy() : 1);
  return { texture:tex, width:W, height:H, textWidth:width, textHeight:blockH, fontSize:fontSize, lineHeight:lineHeight, lineCount:lines.length, lines:lines, fitScaleX:fitScaleX, textMin:(W / 2 - width / 2) / W, textMax:(W / 2 + width / 2) / W };
}

function makeLyricReadabilityTexture(mask) {
  var canvas = document.createElement('canvas');
  var W = mask && mask.width || 2048;
  var H = mask && mask.height || 384;
  var fontSize = mask && mask.fontSize || 128;
  var lines = mask && Array.isArray(mask.lines) && mask.lines.length ? mask.lines : [''];
  var lineHeight = mask && mask.lineHeight || fontSize * lyricLineHeightFactor();
  var fitScaleX = mask && mask.fitScaleX || 1;
  canvas.width = W; canvas.height = H;
  var ctx = canvas.getContext('2d');
  ctx.clearRect(0, 0, W, H);
  ctx.font = lyricFontCss(fontSize);
  ctx.textAlign = 'center';
  ctx.textBaseline = 'alphabetic';
  ctx.lineJoin = 'round';
  ctx.lineCap = 'round';
  ctx.miterLimit = 2;
  var blockH = fontSize + (lines.length - 1) * lineHeight;
  var y0 = H / 2 - blockH / 2 + fontSize * 0.82;
  function strokeLines(dx, dy) {
    for (var i = 0; i < lines.length; i++) {
      var y = y0 + i * lineHeight + (dy || 0);
      if (fitScaleX < 1) {
        ctx.save();
        ctx.translate(W / 2 + (dx || 0), 0);
        ctx.scale(fitScaleX, 1);
        lyricStrokeText(ctx, lines[i], 0, y, fontSize);
        ctx.restore();
      } else {
        lyricStrokeText(ctx, lines[i], W / 2 + (dx || 0), y, fontSize);
      }
    }
  }

  // Black/white readability layer: text-shaped only, no rectangular backing.
  ctx.save();
  ctx.filter = 'blur(14px)';
  ctx.globalAlpha = 0.18;
  ctx.lineWidth = Math.max(18, fontSize * 0.16);
  ctx.strokeStyle = 'rgba(0,0,0,1)';
  strokeLines(0, fontSize * 0.018);
  ctx.restore();

  ctx.save();
  ctx.filter = 'blur(5px)';
  ctx.globalAlpha = 0.32;
  ctx.lineWidth = Math.max(9, fontSize * 0.075);
  ctx.strokeStyle = 'rgba(0,0,0,1)';
  strokeLines(0, fontSize * 0.012);
  ctx.restore();

  ctx.save();
  ctx.filter = 'blur(4px)';
  ctx.globalAlpha = 0.15;
  ctx.lineWidth = Math.max(9, fontSize * 0.070);
  ctx.strokeStyle = 'rgba(255,255,255,1)';
  strokeLines(0, 0);
  ctx.restore();

  ctx.save();
  ctx.filter = 'blur(1.2px)';
  ctx.globalAlpha = 0.26;
  ctx.lineWidth = Math.max(3.2, fontSize * 0.030);
  ctx.strokeStyle = 'rgba(255,255,255,1)';
  strokeLines(0, 0);
  ctx.restore();

  var tex = new THREE.CanvasTexture(canvas);
  tex.minFilter = THREE.LinearFilter;
  tex.magFilter = THREE.LinearFilter;
  tex.generateMipmaps = false;
  tex.anisotropy = Math.min(8, renderer.capabilities.getMaxAnisotropy ? renderer.capabilities.getMaxAnisotropy() : 1);
  return tex;
}

function makeLyricGlowTexture(text, fontSize, textWidth, lines, lineHeight, fitScaleX) {
  text = String(text || '').replace(/\s+/g, ' ').trim();
  var drawLines = Array.isArray(lines) && lines.length ? lines : [text];
  var canvas = document.createElement('canvas');
  var measureCanvas = document.createElement('canvas');
  var measureCtx = measureCanvas.getContext('2d');
  measureCtx.font = lyricFontCss(fontSize);
  fitScaleX = fitScaleX || 1;
  var measuredWidth = Math.max(1, textWidth || lyricMeasureText(measureCtx, text, fontSize) * fitScaleX);
  for (var li = 0; li < drawLines.length; li++) measuredWidth = Math.max(measuredWidth, lyricMeasureText(measureCtx, drawLines[li], fontSize) * fitScaleX);
  var padX = Math.max(160, fontSize * 1.45);
  var padY = Math.max(86, fontSize * 0.78);
  var lh = lineHeight || fontSize * 1.04;
  var blockH = fontSize + (drawLines.length - 1) * lh;
  var W = Math.ceil(measuredWidth + padX * 2);
  var H = Math.ceil(blockH + padY * 2);
  canvas.width = W; canvas.height = H;
  var ctx = canvas.getContext('2d');
  ctx.clearRect(0, 0, W, H);
  ctx.textAlign = 'center';
  ctx.textBaseline = 'alphabetic';
  ctx.font = lyricFontCss(fontSize);
  var y0 = H / 2 - blockH / 2 + fontSize * 0.82;
  function drawGlowText(dx, dy) {
    for (var i = 0; i < drawLines.length; i++) {
      var y = y0 + i * lh + (dy || 0);
      if (fitScaleX < 1) {
        ctx.save();
        ctx.translate(W / 2 + (dx || 0), 0);
        ctx.scale(fitScaleX, 1);
        if (ctx.lineWidth > 0) lyricStrokeText(ctx, drawLines[i], 0, y, fontSize);
        lyricFillText(ctx, drawLines[i], 0, y, fontSize);
        ctx.restore();
      } else {
        if (ctx.lineWidth > 0) lyricStrokeText(ctx, drawLines[i], W / 2 + (dx || 0), y, fontSize);
        lyricFillText(ctx, drawLines[i], W / 2 + (dx || 0), y, fontSize);
      }
    }
  }
  ctx.save();
  ctx.filter = 'blur(14px)';
  ctx.globalAlpha = 0.46;
  ctx.fillStyle = '#fff';
  ctx.lineWidth = Math.max(10, fontSize * 0.10);
  ctx.strokeStyle = '#fff';
  drawGlowText(0, 0);
  ctx.restore();
  ctx.save();
  ctx.filter = 'blur(34px)';
  ctx.globalAlpha = 0.34;
  ctx.fillStyle = '#fff';
  ctx.lineWidth = Math.max(18, fontSize * 0.18);
  ctx.strokeStyle = '#fff';
  drawGlowText(0, 0);
  ctx.restore();
  ctx.save();
  ctx.filter = 'blur(78px)';
  ctx.globalAlpha = 0.22;
  ctx.fillStyle = '#fff';
  ctx.lineWidth = Math.max(28, fontSize * 0.26);
  ctx.strokeStyle = '#fff';
  drawGlowText(0, 0);
  ctx.restore();
  ctx.save();
  ctx.filter = 'blur(116px)';
  ctx.globalAlpha = 0.13;
  ctx.fillStyle = '#fff';
  ctx.lineWidth = Math.max(42, fontSize * 0.40);
  ctx.strokeStyle = '#fff';
  drawGlowText(0, 0);
  ctx.restore();
  ctx.save();
  ctx.globalCompositeOperation = 'lighter';
  ctx.filter = 'blur(8px)';
  ctx.globalAlpha = 0.26;
  ctx.fillStyle = '#fff';
  for (var ri = 0; ri < 8; ri++) {
    var ang = ri / 8 * Math.PI * 2;
    drawGlowText(Math.cos(ang) * 7, Math.sin(ang) * 4);
  }
  ctx.restore();
  ctx.save();
  ctx.globalCompositeOperation = 'destination-in';
  var xMask = ctx.createLinearGradient(0, 0, W, 0);
  xMask.addColorStop(0.00, 'rgba(255,255,255,0)');
  xMask.addColorStop(0.10, 'rgba(255,255,255,1)');
  xMask.addColorStop(0.90, 'rgba(255,255,255,1)');
  xMask.addColorStop(1.00, 'rgba(255,255,255,0)');
  ctx.fillStyle = xMask;
  ctx.fillRect(0, 0, W, H);
  var yMask = ctx.createLinearGradient(0, 0, 0, H);
  yMask.addColorStop(0.00, 'rgba(255,255,255,0)');
  yMask.addColorStop(0.16, 'rgba(255,255,255,1)');
  yMask.addColorStop(0.84, 'rgba(255,255,255,1)');
  yMask.addColorStop(1.00, 'rgba(255,255,255,0)');
  ctx.fillStyle = yMask;
  ctx.fillRect(0, 0, W, H);
  ctx.restore();
  var tex = new THREE.CanvasTexture(canvas);
  tex.minFilter = THREE.LinearFilter;
  tex.magFilter = THREE.LinearFilter;
  tex.generateMipmaps = false;
  tex.userData = { width:W, height:H, textWidth:measuredWidth };
  return tex;
}

var lyricSunBloomTexture = null;
function getLyricSunBloomTexture() {
  if (lyricSunBloomTexture) return lyricSunBloomTexture;
  var canvas = document.createElement('canvas');
  canvas.width = 1024; canvas.height = 512;
  var ctx = canvas.getContext('2d');
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  var cx = canvas.width * 0.50, cy = canvas.height * 0.50;
  ctx.save();
  ctx.translate(cx, cy);
  ctx.scale(2.05, 1);
  var radial = ctx.createRadialGradient(0, 0, 0, 0, 0, canvas.height * 0.43);
  radial.addColorStop(0.00, 'rgba(255,246,186,0.92)');
  radial.addColorStop(0.18, 'rgba(255,219,126,0.44)');
  radial.addColorStop(0.46, 'rgba(255,186,82,0.15)');
  radial.addColorStop(1.00, 'rgba(255,186,82,0)');
  ctx.fillStyle = radial;
  ctx.fillRect(-canvas.width, -canvas.height, canvas.width * 2, canvas.height * 2);
  ctx.restore();
  ctx.save();
  ctx.globalCompositeOperation = 'lighter';
  ctx.filter = 'blur(34px)';
  ctx.fillStyle = 'rgba(255,235,168,0.18)';
  ctx.beginPath();
  ctx.ellipse(cx, cy, canvas.width * 0.33, canvas.height * 0.14, -0.06, 0, Math.PI * 2);
  ctx.fill();
  ctx.filter = 'blur(58px)';
  ctx.fillStyle = 'rgba(255,214,122,0.11)';
  ctx.beginPath();
  ctx.ellipse(cx, cy, canvas.width * 0.45, canvas.height * 0.19, -0.05, 0, Math.PI * 2);
  ctx.fill();
  ctx.filter = 'blur(18px)';
  var core = ctx.createRadialGradient(cx, cy, 0, cx, cy, canvas.width * 0.16);
  core.addColorStop(0.00, 'rgba(255,252,220,0.38)');
  core.addColorStop(0.34, 'rgba(255,230,158,0.20)');
  core.addColorStop(1.00, 'rgba(255,210,116,0)');
  ctx.fillStyle = core;
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  ctx.restore();
  ctx.save();
  ctx.globalCompositeOperation = 'destination-in';
  var xMask = ctx.createLinearGradient(0, 0, canvas.width, 0);
  xMask.addColorStop(0.00, 'rgba(255,255,255,0)');
  xMask.addColorStop(0.11, 'rgba(255,255,255,1)');
  xMask.addColorStop(0.89, 'rgba(255,255,255,1)');
  xMask.addColorStop(1.00, 'rgba(255,255,255,0)');
  ctx.fillStyle = xMask;
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  var yMask = ctx.createLinearGradient(0, 0, 0, canvas.height);
  yMask.addColorStop(0.00, 'rgba(255,255,255,0)');
  yMask.addColorStop(0.18, 'rgba(255,255,255,1)');
  yMask.addColorStop(0.82, 'rgba(255,255,255,1)');
  yMask.addColorStop(1.00, 'rgba(255,255,255,0)');
  ctx.fillStyle = yMask;
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  ctx.restore();
  lyricSunBloomTexture = new THREE.CanvasTexture(canvas);
  lyricSunBloomTexture.minFilter = THREE.LinearFilter;
  lyricSunBloomTexture.magFilter = THREE.LinearFilter;
  lyricSunBloomTexture.generateMipmaps = false;
  return lyricSunBloomTexture;
}

function makeLyricShaderMaterial(mask, pal) {
  return new THREE.ShaderMaterial({
    uniforms: {
      uMap: { value: mask.texture },
      uProgress: { value: 0 },
      uTextMin: { value: mask.textMin },
      uTextMax: { value: mask.textMax },
      uOpacity: { value: 0 },
      uBaseColor: { value: lyricThreeColor(pal.primary, '#d6f8ff', 0.38) },
      uHiColor: { value: lyricThreeColor(pal.highlight || pal.primary, '#fff0b8', 0.48) },
      uGlowColor: { value: lyricThreeColor(pal.glowColor || pal.secondary, '#9cffdf', 0.36) },
      uSolarColor: { value: lyricThreeColor(pal.highlight || pal.secondary || pal.primary, '#fff0b8', 0.50) },
      uFeather: { value: lyricsHasNativeKaraoke ? 0.030 : 0.055 },
      uSolar: { value: 0 },
    },
    vertexShader: 'varying vec2 vUv; void main(){ vUv=uv; gl_Position=projectionMatrix*modelViewMatrix*vec4(position,1.0); }',
    fragmentShader: [
      'precision highp float;',
      'uniform sampler2D uMap;',
      'uniform float uProgress,uTextMin,uTextMax,uOpacity,uFeather,uSolar;',
      'uniform vec3 uBaseColor,uHiColor,uGlowColor,uSolarColor;',
      'varying vec2 vUv;',
      'void main(){',
      '  vec2 uv = gl_FrontFacing ? vUv : vec2(1.0 - vUv.x, vUv.y);',
      '  float mask = texture2D(uMap, uv).a;',
      '  if(mask < 0.01) discard;',
      '  float denom = max(0.001, uTextMax - uTextMin);',
      '  float p = clamp((uv.x - uTextMin) / denom, 0.0, 1.0);',
      '  float filled = 1.0 - smoothstep(uProgress, uProgress + uFeather, p);',
      '  float edge = 1.0 - smoothstep(0.0, uFeather * 2.8, abs(p - uProgress));',
      '  vec3 color = mix(uBaseColor, uHiColor, filled * 0.88);',
      '  color += uGlowColor * edge * 0.14;',
      '  vec3 solar = uSolarColor;',
      '  color = mix(color, color + solar * 0.34, uSolar * (0.25 + filled * 0.45));',
      '  color += solar * edge * uSolar * 0.22;',
      '  float lum = dot(color, vec3(0.299, 0.587, 0.114));',
      '  color += vec3(max(0.0, 0.30 - lum));',
      '  gl_FragColor = vec4(color, mask * uOpacity);',
      '}',
    ].join('\n'),
    transparent:true, depthWrite:false, depthTest:false, side:THREE.DoubleSide,
  });
}

function buildLyricMesh(text) {
  // [muyun:explicit-newline] 保留原始 \n 交给 makeLyricMask 显式分行（译文双行）
  var rawText = String(text || '');
  text = rawText.replace(/\s+/g, ' ').trim();
  var mask = makeLyricMask(rawText);
  var pal = stageLyrics.palette;
  var worldW = 6.10;
  var worldH = worldW * (mask.height / mask.width);
  var geo = new THREE.PlaneGeometry(worldW, worldH, 1, 1);
  var textWorldW = worldW * (mask.textWidth / mask.width);
  var textWorldH = worldH * ((mask.textHeight || mask.fontSize) / mask.height);
  var group = new THREE.Group();
  group.renderOrder = 42;
  group.position.set((Math.random() - 0.5) * 0.08, 0.20, 1.46);
  group.scale.setScalar(0.96);
  group.userData.age = 0;
  group.userData.state = 'in';
  group.userData.lastLyricProgress = -1;
  group.userData.floatSeed = Math.random() * 100;

  var sunMat = new THREE.MeshBasicMaterial({
    map:getLyricSunBloomTexture(), transparent:true, opacity:0,
    depthWrite:false, depthTest:false, side:THREE.DoubleSide,
    blending:THREE.AdditiveBlending, color:lyricThreeColor(pal.highlight || pal.secondary || pal.primary, '#ffe7a6', 0.50)
  });
  var sunWorldW = Math.max(textWorldW + worldH * 1.10, textWorldW * 1.18);
  sunWorldW = Math.min(worldW * 1.16, Math.max(worldH * 1.35, sunWorldW));
  var sunWorldH = Math.max(worldH * 1.02, Math.min(worldH * 1.54, worldH + textWorldW * 0.070));
  var sun = new THREE.Mesh(new THREE.PlaneGeometry(sunWorldW, sunWorldH, 1, 1), sunMat);
  sun.renderOrder = 40;
  sun.position.set(0, 0.02, -0.030);
  sun.scale.set(0.78, 0.58, 1);
  group.add(sun);

  var glowTex = makeLyricGlowTexture(text, mask.fontSize, mask.textWidth, mask.lines, mask.lineHeight, mask.fitScaleX);
  var glowMat = new THREE.MeshBasicMaterial({
    map: glowTex, transparent:true, opacity:0, depthWrite:false, depthTest:false,
    side:THREE.DoubleSide, blending:THREE.AdditiveBlending, color:lyricThreeColor(pal.secondary, '#9cffdf', 0.36)
  });
  var glowMeta = glowTex.userData || {};
  var glowWorldW = textWorldW * ((glowMeta.width || mask.width) / Math.max(1, glowMeta.textWidth || mask.textWidth));
  glowWorldW = Math.min(worldW * 1.10, Math.max(textWorldW + worldH * 0.38, glowWorldW));
  var glowWorldH = worldH * ((glowMeta.height || mask.height) / mask.height);
  glowWorldH = Math.min(worldH * 1.42, Math.max(worldH * 0.92, glowWorldH));
  var glow = new THREE.Mesh(new THREE.PlaneGeometry(glowWorldW, glowWorldH, 1, 1), glowMat);
  glow.renderOrder = 41;
  glow.scale.set(1.0, 1.06, 1);
  group.add(glow);

  var readabilityTex = makeLyricReadabilityTexture(mask);
  var readabilityMat = new THREE.MeshBasicMaterial({
    map: readabilityTex, transparent:true, opacity:0, depthWrite:false, depthTest:false,
    side:THREE.DoubleSide
  });
  var readability = new THREE.Mesh(new THREE.PlaneGeometry(worldW, worldH, 1, 1), readabilityMat);
  readability.renderOrder = 42;
  readability.position.set(0, 0, -0.012);
  group.add(readability);

  var textMat = makeLyricShaderMaterial(mask, pal);
  var textMesh = new THREE.Mesh(geo, textMat);
  textMesh.renderOrder = 43;
  group.add(textMesh);

  var sparkCount = 132;
  var pgeo = new THREE.BufferGeometry();
  var ppos = new Float32Array(sparkCount * 3);
  var pseed = new Float32Array(sparkCount);
  for (var i = 0; i < sparkCount; i++) {
    var angle = Math.random() * Math.PI * 2;
    var ring = 0.78 + Math.pow(Math.random(), 1.45) * 0.58;
    var rx = textWorldW * (0.50 + Math.random() * 0.22) + 0.10;
    var ry = worldH * (0.42 + Math.random() * 0.22) + 0.08;
    ppos[i*3] = Math.cos(angle) * rx * ring + (Math.random() - 0.5) * textWorldW * 0.12;
    ppos[i*3+1] = Math.sin(angle) * ry * ring + (Math.random() - 0.5) * worldH * 0.14;
    ppos[i*3+2] = (Math.random() - 0.5) * 0.24;
    pseed[i] = Math.random() * 1000;
  }
  pgeo.setAttribute('position', new THREE.BufferAttribute(ppos, 3));
  pgeo.setAttribute('seed', new THREE.BufferAttribute(pseed, 1));
  var pmat = new THREE.ShaderMaterial({
    uniforms: {
      uMap: { value: dotTexture },
      uSize: { value: 0.052 },
      uOpacity: { value: 0 },
      uColor: { value: lyricThreeColor(pal.highlight || pal.secondary || pal.primary, '#fff7d2', 0.30) },
      uPixel: uniforms.uPixel
    },
    vertexShader: [
      'attribute float seed;',
      'uniform float uSize;',
      'uniform float uPixel;',
      'varying float vSeed;',
      'void main(){',
      '  vSeed = seed;',
      '  vec4 mv = modelViewMatrix * vec4(position, 1.0);',
      '  float jitter = 0.58 + fract(sin(seed * 19.17) * 43758.5453) * 1.18;',
      '  float depth = clamp(2.2 / max(0.35, -mv.z), 0.54, 1.55);',
      '  gl_PointSize = uSize * jitter * depth * uPixel * 120.0;',
      '  gl_Position = projectionMatrix * mv;',
      '}'
    ].join('\n'),
    fragmentShader: [
      'precision highp float;',
      'uniform sampler2D uMap;',
      'uniform vec3 uColor;',
      'uniform float uOpacity;',
      'varying float vSeed;',
      'void main(){',
      '  vec4 tex = texture2D(uMap, gl_PointCoord);',
      '  float twinkle = 0.72 + fract(sin(vSeed * 7.31) * 91.7) * 0.28;',
      '  gl_FragColor = vec4(uColor * twinkle, tex.a * uOpacity);',
      '}'
    ].join('\n'),
    transparent:true, depthWrite:false, depthTest:false, blending:THREE.AdditiveBlending
  });
  var sparks = new THREE.Points(pgeo, pmat);
  sparks.renderOrder = 44;
  sparks.visible = !!fx.lyricGlowParticles;
  group.add(sparks);

  group.userData.lyric = {
    mask:mask, textMesh:textMesh, readability:readability, glow:glow, sparks:sparks, sun:sun,
    textMat:textMat, readabilityMat:readabilityMat, glowMat:glowMat, sparkMat:pmat, sunMat:sunMat,
    basePositions:ppos.slice ? ppos.slice(0) : new Float32Array(ppos),
    textWorldW:textWorldW, textWorldH:textWorldH, worldW:worldW, worldH:worldH
  };
  updateLyricMeshProgress(group, 0);
  return group;
}

function updateLyricMeshProgress(mesh, progress) {
  if (!mesh || !mesh.userData || !mesh.userData.lyric) return;
  progress = Math.max(0, Math.min(1, progress || 0));
  var d = mesh.userData.lyric;
  d.textMat.uniforms.uProgress.value = progress;
  mesh.userData.lastLyricProgress = progress;
}

function showStageLine(text, redrawOnly) {
  createLyricsParticles();
  if (!stageLyrics.group) return;
  if (!text) { clearStageLyrics(); return; }
  if (redrawOnly && stageLyrics.current) {
    disposeLyricMesh(stageLyrics.current);
    stageLyrics.current = null;
  } else if (stageLyrics.current) {
    stageLyrics.current.userData.state = 'out';
    stageLyrics.current.userData.age = 0;
    stageLyrics.outgoing.push(stageLyrics.current);
  }
  stageLyrics.currentText = text;
  var mesh = buildLyricMesh(text);
  stageLyrics.group.add(mesh);
  stageLyrics.current = mesh;
}

function refreshCurrentLyricStyle() {
  if (!stageLyrics || !stageLyrics.currentText || !stageLyrics.current) return;
  var progress = stageLyrics.current.userData ? (stageLyrics.current.userData.lastLyricProgress || 0) : 0;
  showStageLine(stageLyrics.currentText, true);
  updateLyricMeshProgress(stageLyrics.current, progress);
  if (stageLyrics.current && stageLyrics.current.userData) stageLyrics.current.userData.age = 0.48;
}

function clearStageLyrics() {
  disposeLyricMesh(stageLyrics.current);
  stageLyrics.current = null;
  stageLyrics.currentIdx = -1;
  stageLyrics.currentText = '';
  while (stageLyrics.outgoing.length) disposeLyricMesh(stageLyrics.outgoing.pop());
}

function updateStageLyrics3D(dt) {
  if (!stageLyrics.group) return;
  if (!fx.particleLyrics && !stageLyrics.current && (!stageLyrics.outgoing || !stageLyrics.outgoing.length)) return;
  if (!isFinite(stageLyrics.highBloom)) stageLyrics.highBloom = 0;
  if (!isFinite(stageLyrics.beatGlow)) stageLyrics.beatGlow = 0;
  if (!isFinite(stageLyrics.glowFollowX)) stageLyrics.glowFollowX = 0;
  if (!isFinite(stageLyrics.glowFollowY)) stageLyrics.glowFollowY = 0;
  if (!isFinite(stageLyrics.glowFollowRoll)) stageLyrics.glowFollowRoll = 0;
  var t = uniforms.uTime.value;
  var lyricGlowStrength = fx.lyricGlow ? Math.min(0.85, Math.max(0, fx.lyricGlowStrength)) : 0;
  var glowDrive = Math.min(1.7, Math.max(0, lyricGlowStrength / 0.50));
  var glowBreath = lyricGlowStrength > 0 ? (0.5 + 0.5 * Math.sin(t * 1.05)) : 0;
  var musicBloom = Math.max(lyricSunEnergy, beatPulse * 0.10);
  var beatGlowRaw = fx.lyricGlowBeat && lyricGlowStrength > 0
    ? Math.max(beatPulse * 1.22, beatCam.punch * 0.86 + beatCam.radiusKick * 1.85)
    : 0;
  stageLyrics.beatGlow += (beatGlowRaw - stageLyrics.beatGlow) * (beatGlowRaw > stageLyrics.beatGlow ? 0.32 : 0.10);
  if (!isFinite(stageLyrics.beatGlow)) stageLyrics.beatGlow = 0;
  var skullLyricPreset = !!(fx && fx.preset === SKULL_PRESET_INDEX);
  var solarBloom = lyricGlowStrength > 0 ? (0.18 + glowBreath * 0.16 + musicBloom * 0.90 + stageLyrics.beatGlow * 1.18 + Math.sin(t * 0.37 + 1.2) * 0.035) * glowDrive : 0;
  if (skullLyricPreset && lyricGlowStrength > 0) {
    solarBloom = (0.035 + glowBreath * 0.030 + musicBloom * 0.11 + Math.pow(Math.max(0, stageLyrics.beatGlow), 1.26) * 1.45 + Math.pow(Math.max(0, skullBeatFlash || 0), 1.08) * 1.18) * glowDrive;
  }
  solarBloom = Math.max(0, Math.min(1.45, solarBloom));
  stageLyrics.highBloom += (solarBloom - stageLyrics.highBloom) * (solarBloom > stageLyrics.highBloom ? (skullLyricPreset ? 0.22 : 0.075) : (skullLyricPreset ? 0.070 : 0.050));
  if (!isFinite(stageLyrics.highBloom)) stageLyrics.highBloom = 0;
  updateLyricStarRiver(dt);
  var followDrive = fx.lyricGlowBeat && lyricGlowStrength > 0 ? Math.min(1.35, stageLyrics.beatGlow) : 0;
  var followXTarget = followDrive * (beatCam.thetaKick * 34 + beatCam.rollKick * 8);
  var followYTarget = followDrive * (beatCam.phiKick * 42 - beatCam.radiusKick * 0.48);
  var followRollTarget = followDrive * (beatCam.rollKick * 22 + beatCam.thetaKick * 10);
  stageLyrics.glowFollowX += (followXTarget - stageLyrics.glowFollowX) * 0.26;
  stageLyrics.glowFollowY += (followYTarget - stageLyrics.glowFollowY) * 0.24;
  stageLyrics.glowFollowRoll += (followRollTarget - stageLyrics.glowFollowRoll) * 0.22;
  stageLyrics.glowFollowX *= 0.92;
  stageLyrics.glowFollowY *= 0.92;
  stageLyrics.glowFollowRoll *= 0.90;
  var layoutScale = clampRange(Number(fx.lyricScale) || 1, 0.35, 1.65);
  var layoutX = clampRange(Number(fx.lyricOffsetX) || 0, -2.0, 2.0);
  var layoutY = clampRange(Number(fx.lyricOffsetY) || 0, -1.2, 1.35);
  var layoutZ = clampRange(Number(fx.lyricOffsetZ) || 0, -1.6, 1.6);
  var layoutTiltX = clampRange(Number(fx.lyricTiltX) || 0, -42, 42);
  var layoutTiltY = clampRange(Number(fx.lyricTiltY) || 0, -42, 42);
  var skullMouthLyrics = !!(camera && fx && fx.preset === SKULL_PRESET_INDEX && skullParticleGroup && skullParticleGroup.visible);
  var shelfDetailOpen = !!(shelfManager && shelfManager.hasOpenContent && shelfManager.hasOpenContent());
  var skullShelfDetailOpen = !!(fx && fx.preset === SKULL_PRESET_INDEX && shelfDetailOpen);
  var normalShelfDetailOpen = !!(shelfDetailOpen && !skullShelfDetailOpen);
  stageLyrics.group.renderOrder = shelfDetailOpen ? 24 : 38;
  var shelfDetailLyricProfile = shelfDetailOpen ? {
    opacity: skullShelfDetailOpen ? 0.30 : 0.38,
    readability: skullShelfDetailOpen ? 0.20 : 0.26,
    bloom: skullShelfDetailOpen ? 0.20 : 0.24,
    glowCap: skullShelfDetailOpen ? 0.050 : 0.070,
    outgoing: skullShelfDetailOpen ? 0.34 : 0.42,
    easeDown: 0.34
  } : {
    opacity: 0.96,
    readability: 0.86,
    bloom: 1,
    glowCap: 1.0,
    outgoing: 1,
    easeDown: 0.16
  };
  var shelfLyricAvoid = shouldAvoidStageLyricsForShelf();
  var wallpaperLyricLock = shouldUseWallpaperLyricCameraLock();
  var wallpaperShelfLyrics = wallpaperLyricLock && shouldDimWallpaperForShelf();
  if (wallpaperLyricLock) {
    layoutScale *= wallpaperShelfLyrics ? 0.60 : 0.84;
    layoutX = clampRange(layoutX + (wallpaperShelfLyrics ? -1.34 : 0), -2.0, 2.0);
    layoutY = clampRange(layoutY + (wallpaperShelfLyrics ? -0.04 : 0.08), -1.2, 1.35);
    layoutZ = clampRange(layoutZ + (wallpaperShelfLyrics ? 1.02 : 1.15), -1.6, 1.6);
  } else if (!skullMouthLyrics && shelfLyricAvoid && fx.lyricCameraLock) {
    layoutScale *= 0.72;
    layoutX = clampRange(layoutX - 1.36, -2.0, 2.0);
    layoutY = clampRange(layoutY + 0.06, -1.2, 1.35);
    layoutZ = clampRange(layoutZ + 0.72, -1.6, 1.6);
  } else if (!skullMouthLyrics && shouldOffsetLyricsForShelfDetail()) {
    layoutScale *= normalShelfDetailOpen ? 0.56 : 0.70;
    layoutX = clampRange(layoutX - (normalShelfDetailOpen ? 1.78 : 1.58), -2.0, 2.0);
    layoutY = clampRange(layoutY + (normalShelfDetailOpen ? 0.18 : 0.08), -1.2, 1.35);
    layoutZ = clampRange(layoutZ + 0.84, -1.6, 1.6);
  }
  if (skullMouthLyrics) {
    layoutScale *= skullShelfDetailOpen ? 0.52 : (shelfLyricAvoid ? 0.58 : 0.66);
    if (shelfLyricAvoid && !skullShelfDetailOpen) {
      layoutX = clampRange(layoutX - 0.36, -2.0, 2.0);
      layoutY = clampRange(layoutY + 0.02, -1.2, 1.35);
      layoutZ = clampRange(layoutZ + 0.18, -1.6, 1.6);
    }
  }
  var lockBaseDistance = wallpaperShelfLyrics ? 5.58 : 4.85;
  var lockDistance = lockBaseDistance + layoutZ;
  var cameraLockedLyrics = (fx.lyricCameraLock || wallpaperLyricLock) && camera;
  var skullLyricEdgeGuard = !!(fx && fx.preset === SKULL_PRESET_INDEX && (orbit.centerLocked || orbit.recentering));
  var lockFit = (cameraLockedLyrics || skullLyricEdgeGuard || skullMouthLyrics) ? lyricCameraLockFit(layoutScale, layoutX, layoutY, skullMouthLyrics ? Math.max(2.2, 4.4 + layoutZ) : lockDistance) : 1;
  if (skullMouthLyrics) lockFit = Math.min(lockFit, 1.12);
  if (!isFinite(stageLyrics.lockFitScale)) stageLyrics.lockFitScale = 1;
  stageLyrics.lockFitScale += (lockFit - stageLyrics.lockFitScale) * (lockFit < stageLyrics.lockFitScale ? 0.18 : 0.10);
  stageLyrics.group.scale.setScalar(layoutScale * stageLyrics.lockFitScale);
  if (skullMouthLyrics) {
    stageLyrics.snapCameraLockFrames = 0;
    skullParticleGroup.updateMatrixWorld(true);
    skullLyricMouthTarget.copy(skullLyricMouthLocal).applyMatrix4(skullParticleGroup.matrixWorld);
    skullParticleGroup.getWorldQuaternion(skullLyricMouthQuat);
    skullLyricMouthForward.set(0, 0, 1).applyQuaternion(skullLyricMouthQuat);
    skullLyricMouthTarget.addScaledVector(skullLyricMouthForward, 0.020);
    skullLyricReadableQuat.copy(skullLyricMouthQuat);
    setStageLyricViewBasisFromCameraOrQuaternion(skullLyricMouthQuat);
    lyricLayoutTarget.copy(skullLyricMouthTarget);
    applyStageLyricLayoutOffset(lyricLayoutTarget, layoutX, layoutY, layoutZ);
    stageLyricTargetQuaternion(skullLyricReadableQuat, layoutTiltX, layoutTiltY);
    stageLyrics.group.userData = stageLyrics.group.userData || {};
    if (!stageLyrics.group.userData.skullMouthLocked) {
      stageLyrics.group.position.copy(lyricLayoutTarget);
      stageLyrics.group.quaternion.copy(lyricTargetQuat);
      stageLyrics.group.userData.skullMouthLocked = true;
    } else {
      stageLyrics.group.position.lerp(lyricLayoutTarget, 0.26);
      stageLyrics.group.quaternion.slerp(lyricTargetQuat, 0.30);
    }
  } else if (cameraLockedLyrics) {
    if (stageLyrics.group.userData) stageLyrics.group.userData.skullMouthLocked = false;
    setStageLyricViewBasisFromCameraOrQuaternion(null);
    lyricLayoutBase.copy(camera.position).addScaledVector(lyricCameraDir, lockBaseDistance);
    lyricCameraTarget.copy(lyricLayoutBase);
    applyStageLyricLayoutOffset(lyricCameraTarget, layoutX, layoutY, layoutZ);
    stageLyricTargetQuaternion(camera.quaternion, layoutTiltX, layoutTiltY);
    if (stageLyrics.snapCameraLockFrames > 0) {
      stageLyrics.group.position.copy(lyricCameraTarget);
      stageLyrics.group.quaternion.copy(lyricTargetQuat);
      stageLyrics.snapCameraLockFrames -= 1;
    } else {
      var lockPosEase = wallpaperLyricLock ? (wallpaperShelfLyrics ? 0.42 : 0.34) : 0.24;
      var lockQuatEase = wallpaperLyricLock ? (wallpaperShelfLyrics ? 0.44 : 0.36) : 0.22;
      stageLyrics.group.position.lerp(lyricCameraTarget, lockPosEase);
      stageLyrics.group.quaternion.slerp(lyricTargetQuat, lockQuatEase);
    }
  } else {
    if (stageLyrics.group.userData) stageLyrics.group.userData.skullMouthLocked = false;
    stageLyrics.snapCameraLockFrames = 0;
    if (particles) {
      particles.updateMatrixWorld(true);
      particles.getWorldPosition(lyricCoverWorldPos);
      particles.getWorldQuaternion(lyricCoverWorldQuat);
    } else {
      lyricCoverWorldPos.set(0, 0, 0);
      lyricCoverWorldQuat.identity();
    }
    setStageLyricViewBasisFromCameraOrQuaternion(lyricCoverWorldQuat);
    lyricLayoutBase.copy(lyricCoverWorldPos);
    lyricLayoutTarget.copy(lyricLayoutBase);
    applyStageLyricLayoutOffset(lyricLayoutTarget, layoutX, layoutY, layoutZ);
    stageLyrics.group.position.copy(lyricLayoutTarget);
    stageLyricTargetQuaternion(lyricCoverWorldQuat, layoutTiltX, layoutTiltY);
    stageLyrics.group.quaternion.copy(lyricTargetQuat);
  }
  function tickMesh(mesh, isCurrent) {
    if (!mesh) return false;
    mesh.userData.age += dt;
    var a = Math.min(1, mesh.userData.age / (isCurrent ? 0.52 : 0.38));
    a = a * a * (3 - 2 * a);
    var data = mesh.userData.lyric || {};
    var followMix = isCurrent ? 1.0 : 0.64;
    var glowX = stageLyrics.glowFollowX * followMix;
    var glowY = stageLyrics.glowFollowY * followMix;
    var glowRoll = stageLyrics.glowFollowRoll * followMix;
    if (data.glow) {
      data.glow.position.set(glowX * 0.14, glowY * 0.12, -0.006);
      data.glow.rotation.z = glowRoll * 0.30;
    }
    if (data.sun) {
      data.sun.position.set(glowX * 0.42, 0.02 + glowY * 0.34, -0.035);
      data.sun.rotation.z = glowRoll * 0.36;
    }
    if (data.sparks) {
      data.sparks.position.set(glowX * 0.24, glowY * 0.22, 0.010);
      data.sparks.rotation.z = glowRoll * 0.22;
    }
    var opacity = 0;
    if (isCurrent) {
      var shelfDetailLyricDim = shelfDetailLyricProfile.bloom;
      var lyricOpacityTarget = shelfDetailLyricProfile.opacity;
      var currentOpacity = data.textMat ? data.textMat.uniforms.uOpacity.value : 0;
      var opacityEase = shelfDetailOpen && currentOpacity > lyricOpacityTarget ? shelfDetailLyricProfile.easeDown : 0.16;
      opacity = clampRange(currentOpacity + (lyricOpacityTarget - currentOpacity) * opacityEase, 0, 1);
      if (data.textMat) data.textMat.uniforms.uOpacity.value = opacity;
      if (data.readabilityMat) {
        var readabilityTarget = opacity * shelfDetailLyricProfile.readability;
        var readabilityEase = shelfDetailOpen && data.readabilityMat.opacity > readabilityTarget ? 0.28 : 0.16;
        data.readabilityMat.opacity += (readabilityTarget - data.readabilityMat.opacity) * readabilityEase;
      }
      if (data.textMat && data.textMat.uniforms.uSolar) {
        var solarTarget = stageLyrics.highBloom * shelfDetailLyricDim;
        var solarEase = shelfDetailOpen && data.textMat.uniforms.uSolar.value > solarTarget ? 0.26 : 0.12;
        data.textMat.uniforms.uSolar.value += (solarTarget - data.textMat.uniforms.uSolar.value) * solarEase;
      }
      var solar = stageLyrics.highBloom * shelfDetailLyricDim;
      var warmth = Math.max(0, Math.min(1, solar * 1.10));
      if (data.glowMat) {
        var glowTarget = lyricGlowStrength > 0 ? Math.min(shelfDetailLyricProfile.glowCap, (0.075 + solar * 0.34 + stageLyrics.beatGlow * 0.16 * shelfDetailLyricDim) * Math.min(3.0, glowDrive)) : 0;
        data.glowMat.opacity += (glowTarget - data.glowMat.opacity) * (glowTarget > data.glowMat.opacity ? 0.095 : (shelfDetailOpen ? 0.20 : 0.055));
        data.glowMat.color.copy(lyricThreeColor(stageLyrics.palette.glowColor || stageLyrics.palette.secondary, '#9cffdf', 0.36)).lerp(lyricSunHotColor, warmth);
      }
      if (data.sparkMat) {
        var sparkTarget = lyricGlowStrength > 0 && fx.lyricGlowParticles && !shelfDetailOpen ? Math.min(0.42, (0.10 + solar * 0.14 + stageLyrics.beatGlow * 0.10) * Math.min(1.6, glowDrive)) : 0;
        var sparkOpacity = getLyricSparkOpacity(data);
        sparkOpacity += (sparkTarget - sparkOpacity) * (sparkTarget > sparkOpacity ? 0.13 : (shelfDetailOpen ? 0.22 : 0.075));
        setLyricSparkOpacity(data, sparkOpacity);
        var sparkSizeTarget = fx.lyricGlowParticles && !shelfDetailOpen ? (0.050 + solar * 0.016 + stageLyrics.beatGlow * 0.026 + bass * 0.008) : 0.035;
        setLyricSparkSize(data, getLyricSparkSize(data) + (sparkSizeTarget - getLyricSparkSize(data)) * 0.12);
        var sparkColor = lyricSunHotColor.clone().lerp(lyricSunColor, 0.22 + solar * 0.18);
        setLyricSparkColor(data, sparkColor);
      }
      var seed = mesh.userData.floatSeed || 0;
      if (data.sunMat) {
        var sunTarget = lyricGlowStrength > 0 && !shelfDetailOpen ? Math.min(0.88, (Math.pow(Math.min(1.35, solar), 1.08) * 0.28 + stageLyrics.beatGlow * 0.20) * Math.min(2.4, glowDrive)) : 0;
        data.sunMat.opacity += (sunTarget - data.sunMat.opacity) * (shelfDetailOpen ? 0.18 : 0.055);
        data.sunMat.color.copy(lyricSunColor).lerp(lyricSunHotColor, solar * 0.55);
      }
      if (data.sun) {
        var sunPulse = solar;
        var beatScale = fx.lyricGlowBeat ? stageLyrics.beatGlow * 0.24 : 0;
        data.sun.scale.set(0.82 + sunPulse * 0.36 + beatScale + Math.sin(t * 1.6) * sunPulse * 0.018, 0.60 + sunPulse * 0.34 + beatScale * 0.72 + Math.cos(t * 1.25) * sunPulse * 0.020, 1);
        data.sun.rotation.z += Math.sin(t * 0.32 + seed) * 0.010 * sunPulse;
      }
      var breathe = Math.sin(t * 0.92 + seed) * 0.050 + Math.sin(t * 0.41 + seed * 0.7) * 0.028;
      if (skullMouthLyrics) {
        var mouthMeshY = -0.070 + Math.sin(t * 0.50 + seed) * 0.018 + Math.sin(t * 1.12 + seed) * 0.006;
        var mouthMeshZ = 0.018 + Math.cos(t * 0.46 + seed) * 0.007;
        var mouthMeshScale = 1.08 + a * 0.040 + breathe * 0.12 + bass * 0.024 + beatPulse * 0.014;
        if (!mesh.userData.skullMouthMeshLocked) {
          mesh.position.set(0, mouthMeshY, mouthMeshZ);
          mesh.userData.skullMouthMeshLocked = true;
        } else {
          mesh.position.x += (0 - mesh.position.x) * 0.18;
          mesh.position.y += (mouthMeshY - mesh.position.y) * 0.16;
          mesh.position.z += (mouthMeshZ - mesh.position.z) * 0.18;
        }
        mesh.scale.setScalar(mouthMeshScale);
        mesh.rotation.z = Math.sin(t * 0.30 + seed) * 0.010;
      } else {
        mesh.userData.skullMouthMeshLocked = false;
        mesh.scale.setScalar(0.96 + a * 0.055 + breathe + bass * 0.038 + beatPulse * 0.014);
        mesh.position.y += ((0.18 + Math.sin(t * 0.55 + seed) * 0.055 + Math.sin(t * 1.35 + seed) * 0.014) - mesh.position.y) * 0.075;
        mesh.position.z += ((1.48 + Math.cos(t * 0.48 + seed) * 0.080) - mesh.position.z) * 0.080;
        mesh.rotation.z = Math.sin(t * 0.34 + seed) * 0.018;
      }
      if (data.sparks && data.sparkMat) data.sparks.visible = fx.lyricGlowParticles || getLyricSparkOpacity(data) > 0.015;
      if (data.sparks && data.basePositions) {
        var pos = data.sparks.geometry.attributes.position;
        var arr = pos.array, base = data.basePositions;
        data.sparks.rotation.z += ((fx.lyricGlowParticles ? 0.0009 : 0.00025) + stageLyrics.beatGlow * 0.0007) * (dt * 60);
        data.sparks.rotation.x = Math.sin(t * 0.12 + seed) * 0.012;
        for (var si = 0; si < arr.length / 3; si++) {
          var s = si * 12.989 + seed;
          var particleBeat = fx.lyricGlowParticles ? stageLyrics.beatGlow : 0;
          var dustBreath = fx.lyricGlowParticles ? (0.62 + 0.38 * Math.sin(t * (0.32 + (si % 7) * 0.025) + s)) : 0.18;
          var drift = fx.lyricGlowParticles ? 1 : 0.30;
          arr[si*3] = base[si*3] + Math.sin(t * (0.18 + (si % 5) * 0.025) + s) * (0.045 + bass * 0.030 + particleBeat * 0.052) * drift + Math.cos(t * 0.11 + s) * 0.018 * dustBreath;
          arr[si*3+1] = base[si*3+1] + Math.cos(t * (0.16 + (si % 6) * 0.024) + s) * (0.042 + mid * 0.026 + particleBeat * 0.046) * drift + Math.sin(t * 0.13 + s) * 0.016 * dustBreath;
          arr[si*3+2] = base[si*3+2] + Math.sin(t * (0.24 + (si % 4) * 0.035) + s) * (0.036 + particleBeat * 0.028) * drift;
        }
        pos.needsUpdate = true;
      }
      return true;
    }
    opacity = (1 - a) * 0.72 * shelfDetailLyricProfile.outgoing;
    if (data.textMat) data.textMat.uniforms.uOpacity.value = opacity;
    if (data.readabilityMat) data.readabilityMat.opacity = opacity * (shelfDetailOpen ? shelfDetailLyricProfile.readability : 0.58);
    if (data.textMat && data.textMat.uniforms.uSolar) data.textMat.uniforms.uSolar.value *= shelfDetailOpen ? 0.72 : 0.86;
    if (data.glowMat) data.glowMat.opacity = lyricGlowStrength > 0 ? (shelfDetailOpen ? Math.min(shelfDetailLyricProfile.glowCap * 0.40, opacity * 0.05 * lyricGlowStrength) : opacity * 0.08 * lyricGlowStrength) : 0;
    if (data.sparkMat) {
      var outgoingSpark = lyricGlowStrength > 0 && fx.lyricGlowParticles && !shelfDetailOpen ? Math.max(opacity * 0.24 * lyricGlowStrength, (1 - a) * 0.18 * lyricGlowStrength) : 0;
      setLyricSparkOpacity(data, outgoingSpark);
      setLyricSparkSize(data, 0.046 + (1 - a) * 0.020);
    }
    if (data.sunMat) data.sunMat.opacity = lyricGlowStrength > 0 && !shelfDetailOpen ? opacity * 0.08 * lyricGlowStrength : 0;
    mesh.position.z -= dt * 0.26;
    mesh.position.y += dt * 0.08;
    mesh.scale.setScalar(0.98 - a * 0.06);
    return a < 1;
  }
  tickMesh(stageLyrics.current, true);
  for (var i = stageLyrics.outgoing.length - 1; i >= 0; i--) {
    if (!tickMesh(stageLyrics.outgoing[i], false)) {
      disposeLyricMesh(stageLyrics.outgoing[i]);
      stageLyrics.outgoing.splice(i, 1);
    }
  }
}

function getLyricLineProgress(line, nextLine, now) {
  if (!line) return 0;
  now += line.words && line.words.length ? 0.030 : 0.020;
  if (line.words && line.words.length && line.charCount > 0) {
    var lastP = 0;
    for (var i = 0; i < line.words.length; i++) {
      var w = line.words[i];
      var ws = w.t;
      var we = w.t + Math.max(0.08, w.d || 0.24);
      if (now < ws) return lastP;
      var local = now >= we ? 1 : (now - ws) / Math.max(0.08, we - ws);
      local = Math.max(0, Math.min(1, local));
      var p = (w.c0 + (w.c1 - w.c0) * local) / line.charCount;
      lastP = Math.max(lastP, p);
      if (now < we) return lastP;
    }
    return 1;
  }
  var nextT = nextLine && nextLine.t > line.t ? nextLine.t : Math.min((audio && audio.duration) || now + 4, line.t + (line.duration || 4.8));
  var span = Math.max(0.75, nextT - line.t);
  var prog = Math.max(0, Math.min(1, (now - line.t) / span));
  return prog * prog * (3 - 2 * prog);
}

function tickLyricsParticles() {
  if (!fx.particleLyrics) {
    if (stageLyrics.current || stageLyrics.currentText || (stageLyrics.outgoing && stageLyrics.outgoing.length)) clearStageLyrics();
    return;
  }
  if (!playing || !audio || !lyricsLines.length) {
    if (stageLyrics.current) {
      stageLyrics.current.userData.state = 'out';
      stageLyrics.current.userData.age = 0;
      stageLyrics.outgoing.push(stageLyrics.current);
      stageLyrics.current = null;
      stageLyrics.currentIdx = -1;
      stageLyrics.currentText = '';
    }
    return;
  }
  var t = audio.currentTime;
  var newIdx = -1;
  for (var i = 0; i < lyricsLines.length; i++) {
    if (lyricsLines[i].t <= t + 0.05) newIdx = i; else break;
  }
  if (newIdx < 0) {
    var introText = currentLyricFallbackText();
    if (!introText) {
      clearStageLyrics();
      return;
    }
    if (stageLyrics.currentIdx !== -2 || stageLyrics.currentText !== introText) {
      stageLyrics.currentIdx = -2;
      showStageLine(introText);
    }
    if (stageLyrics.current) {
      var firstLine = lyricsLines[0];
      var introEnd = firstLine && firstLine.t > 0 ? firstLine.t : Math.min((audio && audio.duration) || 4.8, 4.8);
      var introLine = { t:0, text:introText, duration:Math.max(0.8, introEnd), charCount:Math.max(1, introText.length), fallback:true };
      updateLyricMeshProgress(stageLyrics.current, getLyricLineProgress(introLine, null, t));
    }
    return;
  }
  if (newIdx !== stageLyrics.currentIdx) {
    stageLyrics.currentIdx = newIdx;
    showStageLine(lyricsLines[newIdx].text || '');
  }
  if (stageLyrics.current) {
    var curLine = lyricsLines[newIdx] || { t:t };
    var nextLine = lyricsLines[newIdx + 1];
    var progress = getLyricLineProgress(curLine, nextLine, t);
    updateLyricMeshProgress(stageLyrics.current, progress);
  }
}

function disposeLyricsParticles() {
  clearStageLyrics();
  if (stageLyrics.starRiver) {
    if (stageLyrics.starRiver.parent) stageLyrics.starRiver.parent.remove(stageLyrics.starRiver);
    if (stageLyrics.starRiver.geometry) stageLyrics.starRiver.geometry.dispose();
    if (stageLyrics.starRiver.material) stageLyrics.starRiver.material.dispose();
    stageLyrics.starRiver = null;
  }
  if (stageLyrics.group) {
    scene.remove(stageLyrics.group);
    stageLyrics.group = null;
  }
}


// ▼▼▼ [mineradio:ripples] source lines 9360,9421 ▼▼▼
// ============================================================
//  涟漪触发系统 — 3×3 九宫格 + bass 上升沿
// ============================================================
var rippleIdx = 0;
var lastRippleAt = 0;
var lastBassRising = false;
var BASS_THRESHOLD = 0.30;
var RIPPLE_COOLDOWN = 0.32;

var regions = [];
for (var ry = 0; ry < 3; ry++) for (var rx = 0; rx < 3; rx++) {
  regions.push({
    x: (rx / 2 - 0.5) * PLANE_SIZE * 0.72,
    y: (ry / 2 - 0.5) * PLANE_SIZE * 0.72,
  });
}

function triggerRipple(x, y, strength) {
  var r = ripples[rippleIdx];
  r.x = x; r.y = y; r.age = 0; r.str = strength;
  rippleIdx = (rippleIdx + 1) % RIPPLE_MAX;
}

function updateRipples(dt) {
  var isBassHit = bass > BASS_THRESHOLD && !lastBassRising;
  lastBassRising = bass > BASS_THRESHOLD * 0.75;
  var now = uniforms.uTime.value;
  if (isBassHit && (now - lastRippleAt) > RIPPLE_COOLDOWN) {
    lastRippleAt = now;
    var count = 2 + (Math.random() < 0.5 ? 0 : 1);
    var used = {};
    for (var k = 0; k < count; k++) {
      var idx, tries = 0;
      do { idx = Math.floor(Math.random() * 9); tries++; } while (used[idx] && tries < 12);
      used[idx] = true;
      var reg = regions[idx];
      var jx = reg.x + (Math.random() - 0.5) * 0.7;
      var jy = reg.y + (Math.random() - 0.5) * 0.7;
      var str = 0.65 + bass * 1.4 + Math.random() * 0.25;
      triggerRipple(jx, jy, str);
    }
  }

  for (var i = 0; i < RIPPLE_MAX; i++) {
    var r = ripples[i];
    if (r.str > 0.005) {
      r.age += dt;
      if (r.age > 2.0) { r.str = 0; r.age = -10; }
    }
    var off = i * 4;
    rippleData[off]   = r.x;
    rippleData[off+1] = r.y;
    rippleData[off+2] = r.age;
    rippleData[off+3] = r.str;
  }
  rippleTex.needsUpdate = true;

  var active = 0;
  for (var i = 0; i < RIPPLE_MAX; i++) if (ripples[i].str > 0.005) active++;
  uniforms.uRippleCount.value = active;
}


// ▼▼▼ [mineradio:cover-processing] source lines 9422,9947 ▼▼▼
// ============================================================
//  封面 + 边缘 + 启发式深度 处理 (CPU 端)
//   生成 256×256 RGBA 纹理: R=depth G=edge B=fg-mask A=lum
// ============================================================
function coverDepthCacheId(raw) {
  var str = String(raw || '');
  if (!str) return '';
  var h = 2166136261;
  for (var i = 0; i < str.length; i++) {
    h ^= str.charCodeAt(i);
    h += (h << 1) + (h << 4) + (h << 7) + (h << 8) + (h << 24);
  }
  return str.length + ':' + (h >>> 0).toString(36);
}
function getCoverDepthCache(raw) {
  var id = coverDepthCacheId(raw);
  if (!id || !coverDepthCache[id]) return null;
  coverDepthCache[id].at = Date.now();
  var idx = coverDepthCacheKeys.indexOf(id);
  if (idx >= 0) {
    coverDepthCacheKeys.splice(idx, 1);
    coverDepthCacheKeys.push(id);
  } else coverDepthCacheKeys.push(id);
  return coverDepthCache[id];
}
function setCoverDepthCache(raw, canvas, aiEnhanced) {
  var id = coverDepthCacheId(raw);
  if (!id || !canvas) return;
  var idx = coverDepthCacheKeys.indexOf(id);
  if (idx >= 0) coverDepthCacheKeys.splice(idx, 1);
  coverDepthCacheKeys.push(id);
  coverDepthCache[id] = { canvas: canvas, ai: !!aiEnhanced, at: Date.now() };
  while (coverDepthCacheKeys.length > 18) {
    var drop = coverDepthCacheKeys.shift();
    delete coverDepthCache[drop];
  }
}

function buildEdgeAndDepth(srcCanvas) {
  var W = 256, H = 256, N = W * H;
  var normalized = document.createElement('canvas');
  normalized.width = W;
  normalized.height = H;
  var sctx = normalized.getContext('2d');
  sctx.drawImage(srcCanvas, 0, 0, W, H);
  var src = sctx.getImageData(0, 0, W, H).data;
  var lum = new Float32Array(N), blur = new Float32Array(N), tmp = new Float32Array(N);
  // 1) Luminance
  for (var i = 0; i < N; i++) {
    var di = i * 4;
    lum[i] = (src[di] * 0.299 + src[di+1] * 0.587 + src[di+2] * 0.114) / 255;
  }
  // 2) Box blur 2 次 (深度基础)
  function blurH(s, d, r) {
    for (var y = 0; y < H; y++) {
      var sum = 0;
      for (var x = -r; x <= r; x++) sum += s[y * W + Math.max(0, Math.min(W-1, x))];
      for (var x = 0; x < W; x++) {
        d[y * W + x] = sum / (2*r + 1);
        var xR = Math.min(W-1, x + r + 1), xL = Math.max(0, x - r);
        sum += s[y * W + xR] - s[y * W + xL];
      }
    }
  }
  function blurV(s, d, r) {
    for (var x = 0; x < W; x++) {
      var sum = 0;
      for (var y = -r; y <= r; y++) sum += s[Math.max(0, Math.min(H-1, y)) * W + x];
      for (var y = 0; y < H; y++) {
        d[y * W + x] = sum / (2*r + 1);
        var yD = Math.min(H-1, y + r + 1), yU = Math.max(0, y - r);
        sum += s[yD * W + x] - s[yU * W + x];
      }
    }
  }
  blurH(lum, tmp, 4); blurV(tmp, blur, 4);

  // 3) Sobel 边缘 (在 blur 上做 - 减少噪声)
  var edge = new Float32Array(N);
  for (var y = 1; y < H-1; y++) for (var x = 1; x < W-1; x++) {
    var gx = -blur[(y-1)*W + (x-1)] - 2*blur[y*W + (x-1)] - blur[(y+1)*W + (x-1)]
            + blur[(y-1)*W + (x+1)] + 2*blur[y*W + (x+1)] + blur[(y+1)*W + (x+1)];
    var gy = -blur[(y-1)*W + (x-1)] - 2*blur[(y-1)*W + x] - blur[(y-1)*W + (x+1)]
            + blur[(y+1)*W + (x-1)] + 2*blur[(y+1)*W + x] + blur[(y+1)*W + (x+1)];
    edge[y*W + x] = Math.min(1.0, Math.sqrt(gx*gx + gy*gy) * 1.4);
  }
  // 4) 启发式深度:亮度 + 中心 mask + 边缘累积
  var depth = new Float32Array(N);
  for (var y = 0; y < H; y++) for (var x = 0; x < W; x++) {
    var i = y*W + x;
    var cx = (x / (W-1) - 0.5) * 2.0;
    var cy = (y / (H-1) - 0.5) * 2.0;
    var rr = Math.sqrt(cx*cx + cy*cy);
    var centerBias = 1.0 - Math.min(1, rr * 0.75);
    var bright = blur[i];
    depth[i] = Math.min(1.0, bright * 0.45 + centerBias * 0.55);
  }
  // 5) fg-mask: 中心 + 高对比区
  var fg = new Float32Array(N);
  for (var i = 0; i < N; i++) {
    var d = depth[i];
    var e = edge[i];
    fg[i] = Math.min(1.0, d * 0.6 + e * 0.5);
  }

  // 输出 256×256 RGBA
  var out = document.createElement('canvas'); out.width = W; out.height = H;
  var octx = out.getContext('2d'), imgOut = octx.createImageData(W, H);
  for (var i = 0; i < N; i++) {
    var di = i * 4;
    imgOut.data[di]   = Math.round(depth[i] * 255);
    imgOut.data[di+1] = Math.round(edge[i] * 255);
    imgOut.data[di+2] = Math.round(fg[i] * 255);
    imgOut.data[di+3] = Math.round(lum[i] * 255);
  }
  octx.putImageData(imgOut, 0, 0);
  return out;
}

// AI 深度估计 (Xenova/depth-anything-small) - 异步加载, 失败回退
async function ensureAIDepthPipeline() {
  if (aiDepthReady && aiDepthPipeline) return aiDepthPipeline;
  if (aiDepthBusy) return null;
  aiDepthBusy = true;
  try {
    showAIDepthChip('加载 AI 深度模型 (首次需下载 50MB)…');
    var mod = await import('https://cdn.jsdelivr.net/npm/@xenova/transformers@2.17.2');
    mod.env.allowLocalModels = false;
    if (mod.env.backends && mod.env.backends.onnx && mod.env.backends.onnx.wasm) mod.env.backends.onnx.wasm.numThreads = 1;
    aiDepthPipeline = await mod.pipeline('depth-estimation', 'Xenova/depth-anything-small-hf');
    aiDepthReady = true;
    return aiDepthPipeline;
  } catch (e) {
    console.warn('AI depth pipeline failed:', e);
    return null;
  } finally {
    aiDepthBusy = false;
  }
}

function makeAIDepthInputCanvas(srcCanvas) {
  if (!srcCanvas) return srcCanvas;
  var size = 160;
  var cv = document.createElement('canvas');
  cv.width = cv.height = size;
  var ctx = cv.getContext('2d');
  try {
    ctx.drawImage(srcCanvas, 0, 0, size, size);
    return cv;
  } catch (e) {
    return srcCanvas;
  }
}

async function estimateAIDepth(srcCanvas, token) {
  if (!fx.aiDepth) return null;
  if (performance.now() < aiDepthFailUntil) return null;
  showAIDepthChip('后台增强封面深度…');
  try {
    var pipe = await ensureAIDepthPipeline();
    if (!pipe) { hideAIDepthChip(); return null; }
    if (token !== coverProcessToken) { hideAIDepthChip(); return null; }
    var inputCanvas = makeAIDepthInputCanvas(srcCanvas);
    var input = inputCanvas;
    try {
      if (inputCanvas && inputCanvas.toDataURL) input = inputCanvas.toDataURL('image/jpeg', 0.82);
    } catch (e) {
      input = inputCanvas;
    }
    var result = await pipe(input);
    if (token !== coverProcessToken) { hideAIDepthChip(); return null; }
    var raw = result && (result.depth || result.predicted_depth || result);
    var rawCv = raw && raw.toCanvas ? await raw.toCanvas() : raw;
    hideAIDepthChip();
    return rawCv;
  } catch (e) {
    console.warn('AI depth estimation failed:', e);
    aiDepthFailUntil = performance.now() + 120000;
    hideAIDepthChip();
    return null;
  }
}

function mergeAIDepthIntoEdgeTexture(heuristicCanvas, aiCanvas) {
  // 把 AI 深度 (灰度) 写入 R 通道, 保留启发式的 G/B/A
  var W = heuristicCanvas.width || 256, H = heuristicCanvas.height || 256;
  var hctx = heuristicCanvas.getContext('2d');
  var hImg = hctx.getImageData(0, 0, W, H);

  var aiTmp = document.createElement('canvas'); aiTmp.width = W; aiTmp.height = H;
  var actx = aiTmp.getContext('2d');
  actx.drawImage(aiCanvas, 0, 0, W, H);
  var aData = actx.getImageData(0, 0, W, H).data;

  // 归一化 AI 深度
  var aiVals = new Float32Array(W * H), minV = 1, maxV = 0;
  for (var i = 0; i < aiVals.length; i++) {
    var di = i * 4;
    var v = (aData[di] * 0.299 + aData[di+1] * 0.587 + aData[di+2] * 0.114) / 255;
    aiVals[i] = v; if (v < minV) minV = v; if (v > maxV) maxV = v;
  }
  var range = Math.max(0.001, maxV - minV);
  // 判断是否反相 (中心应该比边缘深, 表示前景在中)
  var centerSum = 0, centerCount = 0, edgeSum = 0, edgeCount = 0;
  for (var y = 0; y < H; y++) for (var x = 0; x < W; x++) {
    var i = y * W + x;
    var cx = x / (W-1) - 0.5, cy = y / (H-1) - 0.5;
    var rr = Math.sqrt(cx*cx + cy*cy);
    if (rr < 0.22) { centerSum += aiVals[i]; centerCount++; }
    else if (rr > 0.46) { edgeSum += aiVals[i]; edgeCount++; }
  }
  var invert = (centerSum / Math.max(1, centerCount)) < (edgeSum / Math.max(1, edgeCount));

  for (var i = 0; i < aiVals.length; i++) {
    var n = (aiVals[i] - minV) / range;
    if (invert) n = 1.0 - n;
    hImg.data[i*4] = Math.round(n * 255);
  }
  hctx.putImageData(hImg, 0, 0);
  return heuristicCanvas;
}

function queueAIDepthForCover(srcCanvas, edgeCanvas, token, opts, cacheSeed, force) {
  opts = opts || {};
  if (!fx.aiDepth || !srcCanvas || !edgeCanvas) return;
  if (!force && isHiddenForBackgroundOptimization()) return;
  if (performance.now() < aiDepthFailUntil || aiDepthBusy) return;
  var now = performance.now();
  if (!force && now - aiDepthLastRunAt < aiDepthMinGapMs) return;
  aiDepthLastRunAt = now;
  scheduleVisualApply(async function(){
    if (!fx.aiDepth || token !== coverProcessToken || !coverApplyStillCurrent(opts)) return;
    await yieldToIdle(force ? 900 : 2600);
    if (!fx.aiDepth || token !== coverProcessToken || !coverApplyStillCurrent(opts)) return;
    var aiCanvas = await estimateAIDepth(srcCanvas, token);
    if (!aiCanvas || token !== coverProcessToken || !coverApplyStillCurrent(opts)) return;
    mergeAIDepthIntoEdgeTexture(edgeCanvas, aiCanvas);
    coverEdgeTex.image = edgeCanvas;
    coverEdgeTex.needsUpdate = true;
    setCoverDepthState(1, 1.0, 360);
    setCoverDepthCache(cacheSeed, edgeCanvas, true);
    showToast('AI 深度已后台增强');
  }, force ? 240 : 1800, force ? 1200 : 3000);
}

function queueAIDepthForCurrentCover(force) {
  if (!coverTex || !coverTex.image || !coverEdgeTex || !coverEdgeTex.image) return;
  if (!uniforms.uHasCover.value || !uniforms.uHasDepth.value) return;
  queueAIDepthForCover(coverTex.image, coverEdgeTex.image, coverProcessToken, {}, '', !!force);
}

// 颜色渐变 tween (切歌时旧封面→新封面)
var colorMixTween = null;
function startColorMixTween(durationMs) {
  if (colorMixTween) cancelAnimationFrame(colorMixTween.raf);
  durationMs = Math.max(1, durationMs || 1);
  var start = performance.now();
  uniforms.uColorMixT.value = 0;
  function step(now) {
    var t = Math.min(1, (now - start) / durationMs);
    t = visualEase(t);
    uniforms.uColorMixT.value = t;
    if (t < 1) colorMixTween = { raf: requestAnimationFrame(step) };
    else colorMixTween = null;
  }
  colorMixTween = { raf: requestAnimationFrame(step) };
}

// 粒子整体透明度 tween (启动 fade-in)
var alphaTween = null;
var floatAlphaTween = null;
var IDLE_PARTICLE_ALPHA = 0;
function tweenParticleAlpha(from, to, durationMs) {
  if (alphaTween) cancelAnimationFrame(alphaTween.raf);
  from = clampRange(Number(from) || 0, 0, 1);
  to = clampRange(Number(to) || 0, 0, 1);
  var start = performance.now();
  function step(now) {
    var t = Math.min(1, (now - start) / durationMs);
    t = t * t * (3 - 2 * t);
    uniforms.uAlpha.value = clampRange(from + (to - from) * t, 0, 1);
    if (t < 1) alphaTween = { raf: requestAnimationFrame(step) };
    else alphaTween = null;
  }
  alphaTween = { raf: requestAnimationFrame(step) };
}
function tweenFloatAlpha(from, to, durationMs) {
  if (floatAlphaTween) cancelAnimationFrame(floatAlphaTween.raf);
  var start = performance.now();
  function step(now) {
    var t = Math.min(1, (now - start) / durationMs);
    t = t * t * (3 - 2 * t);
    uniforms.uFloatAlpha.value = from + (to - from) * t;
    if (t < 1) floatAlphaTween = { raf: requestAnimationFrame(step) };
    else floatAlphaTween = null;
  }
  floatAlphaTween = { raf: requestAnimationFrame(step) };
}
function revealIdleParticles(target, durationMs) {
  if (!uniforms || !uniforms.uFloatAlpha) return;
  if (floatAlphaTween) { cancelAnimationFrame(floatAlphaTween.raf); floatAlphaTween = null; }
  uniforms.uFloatAlpha.value = 0;
  if (floatGroup) destroyFloatLayer();
  return;
  var next = typeof target === 'number' ? target : IDLE_PARTICLE_ALPHA;
  var from = uniforms.uFloatAlpha.value || 0;
  if (from >= next - 0.01) return;
  tweenFloatAlpha(from, next, durationMs || 1800);
}

// 加载形态 tween (uLoading 0..1)
var loadingTween = null;
var loadingShownAt = 0;
var loadingHideTimer = null;
var coverDepthTween = null;
function visualEase(t) {
  t = Math.max(0, Math.min(1, t));
  return t * t * (3 - 2 * t);
}
function tweenLoading(to, durationMs, onComplete) {
  if (loadingTween) cancelAnimationFrame(loadingTween.raf);
  durationMs = Math.max(1, durationMs || 1);
  if (isHiddenForBackgroundOptimization() || isDeepBackgroundMode()) {
    uniforms.uLoading.value = to;
    loadingTween = null;
    if (onComplete) onComplete();
    return;
  }
  var start = performance.now();
  var from = uniforms.uLoading.value;
  function step(now) {
    var t = Math.min(1, (now - start) / durationMs);
    var eased = visualEase(t);
    uniforms.uLoading.value = from + (to - from) * eased;
    if (t < 1) loadingTween = { raf: requestAnimationFrame(step) };
    else {
      uniforms.uLoading.value = to;
      loadingTween = null;
      if (onComplete) onComplete();
    }
  }
  loadingTween = { raf: requestAnimationFrame(step) };
}
function showLoading() {
  loadingShownAt = performance.now();
  if (loadingHideTimer) {
    clearTimeout(loadingHideTimer);
    loadingHideTimer = null;
  }
  var current = uniforms.uLoading.value || 0;
  tweenLoading(Math.max(current, 0.56), current > 0.04 ? 86 : 118);
}
function hideLoading() {
  if (loadingHideTimer) clearTimeout(loadingHideTimer);
  if (isHiddenForBackgroundOptimization() || isDeepBackgroundMode()) {
    forceLoadingSettled('background-hide');
    return;
  }
  var elapsed = loadingShownAt ? performance.now() - loadingShownAt : 999;
  var wait = Math.max(0, 72 - elapsed);
  loadingHideTimer = setTimeout(function(){
    loadingHideTimer = null;
    var current = uniforms.uLoading.value || 0;
    if (current <= 0.015 || isHiddenForBackgroundOptimization() || isDeepBackgroundMode()) {
      if (loadingTween) {
        cancelAnimationFrame(loadingTween.raf);
        loadingTween = null;
      }
      uniforms.uLoading.value = 0;
      return;
    }
    tweenLoading(0, current > 0.38 ? 126 : 96);
  }, wait);
}
function forceLoadingSettled(reason) {
  if (loadingHideTimer) {
    clearTimeout(loadingHideTimer);
    loadingHideTimer = null;
  }
  if (loadingTween) {
    cancelAnimationFrame(loadingTween.raf);
    loadingTween = null;
  }
  uniforms.uLoading.value = 0;
  loadingShownAt = 0;
  if (reason && window.__mineradioDebugLoading) console.log('[LoadingSettled]', reason);
}
function recoverVisualsAfterBackground(reason) {
  applyRendererPowerMode();
  if (typeof scheduleMainRendererViewportRefresh === 'function') scheduleMainRendererViewportRefresh(reason || 'restore');
  if (audio && audio.src && !audio.paused && ((uniforms.uLoading.value || 0) > 0.015 || loadingTween || loadingHideTimer)) {
    forceLoadingSettled(reason || 'restore');
  }
  if (typeof markRenderInteraction === 'function') markRenderInteraction('restore', 1100);
}

function setCoverDepthState(depthTo, aiTo, durationMs) {
  depthTo = Math.max(0, Math.min(1, Number(depthTo) || 0));
  aiTo = Math.max(0, Math.min(1, Number(aiTo) || 0));
  if (coverDepthTween) {
    cancelAnimationFrame(coverDepthTween.raf);
    coverDepthTween = null;
  }
  durationMs = Math.max(1, durationMs || 1);
  var depthFrom = uniforms.uHasDepth.value || 0;
  var aiFrom = uniforms.uAiBoost.value || 0;
  if (durationMs <= 1 || (Math.abs(depthFrom - depthTo) < 0.001 && Math.abs(aiFrom - aiTo) < 0.001)) {
    uniforms.uHasDepth.value = depthTo;
    uniforms.uAiBoost.value = aiTo;
    return;
  }
  var start = performance.now();
  function step(now) {
    var t = Math.min(1, (now - start) / durationMs);
    var eased = visualEase(t);
    uniforms.uHasDepth.value = depthFrom + (depthTo - depthFrom) * eased;
    uniforms.uAiBoost.value = aiFrom + (aiTo - aiFrom) * eased;
    if (t < 1) coverDepthTween = { raf: requestAnimationFrame(step) };
    else {
      uniforms.uHasDepth.value = depthTo;
      uniforms.uAiBoost.value = aiTo;
      coverDepthTween = null;
    }
  }
  coverDepthTween = { raf: requestAnimationFrame(step) };
}

function coverApplyStillCurrent(opts) {
  opts = opts || {};
  return !opts.trackToken || opts.trackToken === trackSwitchToken;
}

function setControlCoverSrc(src) {
  var cover = byId('control-cover');
  if (!cover) return;
  if (!src) {
    cover.style.backgroundImage = '';
    cover.classList.add('cover-empty');
    return;
  }
  cover.style.backgroundImage = 'url("' + String(src).replace(/"/g, '\\"') + '")';
  cover.classList.remove('cover-empty');
}

function updateControlTrackInfo(song) {
  song = song || {};
  var title = byId('control-title');
  var artist = byId('control-artist');
  if (title) title.textContent = song.name || '';
  if (artist) artist.textContent = song.artist || '';
}

function applyCoverCanvas(cv, thumbSrc, opts) {
  opts = opts || {};
  if (!cv || !coverApplyStillCurrent(opts)) return;
  var token = ++coverProcessToken;
  if (opts.coverSource && opts.coverSourceKind) {
    currentCoverSource = { kind: opts.coverSourceKind, src: opts.coverSource };
  }
  var cacheSeed = (opts.coverKey || thumbSrc || '') + '|tex=' + (cv.width || 0) + 'x' + (cv.height || 0);
  var cachedDepth = getCoverDepthCache(cacheSeed);
  // 切歌颜色渐变: 把当前 coverTex 当作 prevCoverTex
  if (uniforms.uHasCover.value > 0.5 && coverTex.image) {
    var prevW = coverTex.image.width || 256;
    var prevH = coverTex.image.height || 256;
    var prevScale = Math.min(1, 256 / Math.max(prevW, prevH, 1));
    var prevCv = document.createElement('canvas');
    prevCv.width = Math.max(1, Math.round(prevW * prevScale));
    prevCv.height = Math.max(1, Math.round(prevH * prevScale));
    try {
      prevCv.getContext('2d').drawImage(coverTex.image, 0, 0, prevCv.width, prevCv.height);
      prevCoverTex.image = prevCv;
      prevCoverTex.needsUpdate = true;
    } catch (e) {}
  }
  coverTex.image = cv; coverTex.needsUpdate = true;
  coverPickerCanvas = cv;
  uniforms.uHasCover.value = 1;
  if (cachedDepth && cachedDepth.canvas) {
    coverEdgeTex.image = cachedDepth.canvas;
    coverEdgeTex.needsUpdate = true;
    setCoverDepthState(1, cachedDepth.ai ? 1.0 : 0.55, opts.deferHeavy ? 180 : 120);
  } else {
    setCoverDepthState(opts.deferHeavy ? (uniforms.uHasDepth.value > 0.5 ? 0.22 : 0) : 0, opts.deferHeavy ? 0.20 : 0, opts.deferHeavy ? 120 : 1);
  }

  if (thumbSrc) {
    byId('thumb-cover').src = thumbSrc;
    setControlCoverSrc(thumbSrc);
  }
  if (shelfManager) shelfManager.onCoverChange(thumbSrc);

  // 启动颜色渐变 (1.4 秒)
  var colorMixMs = opts.colorMixDuration || (fx.preset === 0 ? 520 : 1400);
  startColorMixTween(opts.fromResolutionChange ? (fx.preset === 0 ? 300 : 520) : colorMixMs);

  function refreshCoverDependentColors() {
    if (token !== coverProcessToken || !coverApplyStillCurrent(opts)) return;
    if (floatGroup) refreshFloatColorsFromCover(cv);
    if (backCoverGroup) refreshBackCoverColorsFromCanvas(cv);
    updateLyricPaletteFromCover(cv);
  }

  function runHeavyCoverWork() {
    if (token !== coverProcessToken || !coverApplyStillCurrent(opts)) return;
    if (opts.deferHeavy && typeof isRenderInteractionActive === 'function' && isRenderInteractionActive()) {
      scheduleVisualApply(runHeavyCoverWork, 420, heavyTimeout || 1800);
      return;
    }
    var edgeCv = buildEdgeAndDepth(cv);
    if (token !== coverProcessToken || !coverApplyStillCurrent(opts)) return;
    setCoverDepthCache(cacheSeed, edgeCv, false);
    coverEdgeTex.image = edgeCv; coverEdgeTex.needsUpdate = true;
    setCoverDepthState(1, 0.55, opts.deferHeavy ? 260 : 180);
    refreshCoverDependentColors();

    queueAIDepthForCover(cv, edgeCv, token, opts, cacheSeed, false);
  }
  if (cachedDepth && cachedDepth.canvas) {
    scheduleVisualApply(refreshCoverDependentColors, opts.deferHeavy ? 260 : 90, opts.deferHeavy ? 1200 : 700);
    if (!cachedDepth.ai) queueAIDepthForCover(cv, cachedDepth.canvas, token, opts, cacheSeed, false);
    return;
  }
  var heavyDelay = opts.deferHeavy ? (opts.delay || 620) : (opts.delay || 120);
  var heavyTimeout = opts.deferHeavy ? (opts.timeout || 1800) : (opts.timeout || 900);
  scheduleVisualApply(runHeavyCoverWork, heavyDelay, heavyTimeout);
}


// ▼▼▼ [mineradio:beat-analysis] source lines 9948,12754 ▼▼▼
// ============================================================
//  离线节拍预解析 (v7.2)
//    流程: fetch 完整音频 → OfflineAudioContext.decodeAudioData
//          → 低通滤波 (只保留 60-150Hz, 即 kick 频段)
//          → 短时能量曲线 → 自适应阈值检测峰值
//          → 输出 kick 时间戳数组 (单位: 秒)
//    优点: 完全规避人声干扰; 预先准备好节奏表
//    缺点: 每首歌首次要 1-3 秒
// ============================================================
function medianGap(times, minGap, maxGap) {
  if (!times || times.length < 2) return 0;
  var gaps = [];
  for (var i = 1; i < times.length; i++) {
    var gap = times[i] - times[i - 1];
    if (gap >= minGap && gap <= maxGap) gaps.push(gap);
  }
  gaps.sort(function(a,b){ return a - b; });
  return gaps.length ? gaps[Math.floor(gaps.length * 0.5)] : 0;
}

function normalizeMusicTempoBeats(times, duration) {
  if (!times || !times.length) return [];
  var sorted = times
    .filter(function(t){ return isFinite(t) && t >= 0.05 && (!duration || t < duration - 0.05); })
    .sort(function(a,b){ return a - b; });
  if (sorted.length < 4) return sorted;
  var gap = medianGap(sorted, 0.20, 1.20);
  var minMainGap = gap && gap < 0.42 ? Math.min(0.44, gap * 1.65) : 0.36;
  var out = [];
  var last = -10;
  for (var i = 0; i < sorted.length; i++) {
    if (sorted[i] - last >= minMainGap) {
      out.push(sorted[i]);
      last = sorted[i];
    }
  }
  return out;
}

function estimateTempoPhaseOffset(tempoBeats, beatCandidates, step, duration) {
  if (!tempoBeats || tempoBeats.length < 8 || !beatCandidates || beatCandidates.length < 4 || !step) return 0;
  var maxOffset = Math.min(0.26, Math.max(0.12, step * 0.58));
  var binSize = 0.025;
  var bins = {};
  var samples = [];
  var totalWeight = 0;
  var ti = 0;
  for (var i = 0; i < beatCandidates.length; i++) {
    var b = beatCandidates[i];
    if (!b || !isFinite(b.time)) continue;
    if (duration && (b.time < 1.0 || b.time > duration - 0.5)) continue;
    var strength = Math.max(0, Math.min(1, b.strength || 0));
    if (!b.camera && strength < 0.54) continue;
    if (b.low != null && b.low < 0.18 && strength < 0.66) continue;
    while (ti < tempoBeats.length - 1 && Math.abs(tempoBeats[ti + 1] - b.time) <= Math.abs(tempoBeats[ti] - b.time)) ti++;
    var base = tempoBeats[ti];
    var offset = b.time - base;
    if (!isFinite(offset) || Math.abs(offset) > maxOffset) continue;
    var weight = 0.20 + strength * strength * 1.35;
    if (b.primary) weight *= 1.35;
    if (b.camera) weight *= 1.18;
    if (b.mass != null) weight *= 0.82 + Math.max(0, Math.min(1, b.mass)) * 0.42;
    if (Math.abs(offset) < 0.025) weight *= 0.72;
    var key = Math.round(offset / binSize);
    bins[key] = (bins[key] || 0) + weight;
    samples.push({ offset: offset, weight: weight, key: key });
    totalWeight += weight;
  }
  if (samples.length < 4 || totalWeight <= 0) return 0;
  var bestKey = null;
  var bestWeight = 0;
  Object.keys(bins).forEach(function(k){
    var key = parseInt(k, 10);
    var w = (bins[key] || 0) + (bins[key - 1] || 0) * 0.72 + (bins[key + 1] || 0) * 0.72;
    if (w > bestWeight) {
      bestWeight = w;
      bestKey = key;
    }
  });
  if (bestKey == null || bestWeight < totalWeight * 0.26) return 0;
  var sum = 0;
  var wsum = 0;
  for (var si = 0; si < samples.length; si++) {
    var s = samples[si];
    if (Math.abs(s.key - bestKey) <= 1) {
      sum += s.offset * s.weight;
      wsum += s.weight;
    }
  }
  if (wsum <= 0) return 0;
  var offsetOut = sum / wsum;
  return Math.abs(offsetOut) >= 0.045 ? Math.max(-maxOffset, Math.min(maxOffset, offsetOut)) : 0;
}

var musicTempoLoadPromise = null;
function ensureMusicTempo() {
  if (window.MusicTempo) return Promise.resolve(window.MusicTempo);
  if (musicTempoLoadPromise) return musicTempoLoadPromise;
  musicTempoLoadPromise = fetch('/vendor/music-tempo.min.js')
    .then(function(resp){
      if (!resp.ok) throw new Error('music-tempo load failed: ' + resp.status);
      return resp.text();
    })
    .then(function(code){
      (0, eval)(code);
      return window.MusicTempo || null;
    })
    .catch(function(err){
      console.warn('music-tempo dynamic load failed:', err);
      return null;
    });
  return musicTempoLoadPromise;
}

var musicTempoWorkerUrl = null;
function getMusicTempoWorkerUrl() {
  if (musicTempoWorkerUrl) return musicTempoWorkerUrl;
  var code = [
    'self.onmessage=function(e){',
    'var d=e.data||{};',
    'try{',
    'importScripts(d.scriptUrl||"/vendor/music-tempo.min.js");',
    'var C=self.MusicTempo||(typeof MusicTempo!=="undefined"?MusicTempo:null);',
    'if(!C)throw new Error("MusicTempo unavailable");',
    'var mono=new Float32Array(d.mono);',
    'var mt=new C(mono,{bufferSize:2048,hopSize:Math.max(128,Math.round(d.sampleRate*0.010)),timeStep:0.010,minBeatInterval:0.36,maxBeatInterval:0.95,expiryTime:8});',
    'self.postMessage({ok:true,tempo:mt.tempo||0,beats:mt.beats||[]});',
    '}catch(err){self.postMessage({ok:false,error:(err&&err.message)||String(err)});}',
    '};'
  ].join('');
  musicTempoWorkerUrl = URL.createObjectURL(new Blob([code], { type: 'application/javascript' }));
  return musicTempoWorkerUrl;
}

async function analyzeMusicTempoInWorker(buffer, token) {
  if (typeof Worker === 'undefined' || typeof Blob === 'undefined' || typeof URL === 'undefined') return null;
  try {
    showBeatChip('后台锁定电影主拍…');
    await yieldToIdle(isHiddenForBackgroundOptimization() ? 20 : 180);
    if (token !== beatMapToken) return null;
    var channels = buffer.numberOfChannels;
    var len = buffer.length;
    var mono = new Float32Array(len);
    var chDataList = [];
    for (var ch = 0; ch < channels; ch++) chDataList.push(buffer.getChannelData(ch));
    var chScale = 1 / Math.max(1, channels);
    var monoChunk = Math.max(4096, Math.floor(buffer.sampleRate * 0.70));
    for (var monoStart = 0; monoStart < len; monoStart += monoChunk) {
      var monoEnd = Math.min(len, monoStart + monoChunk);
      for (var mi = monoStart; mi < monoEnd; mi++) {
        var sum = 0;
        for (var ci = 0; ci < channels; ci++) sum += chDataList[ci][mi] * chScale;
        mono[mi] = sum;
      }
      if ((monoStart / monoChunk) % 2 === 1) {
        await yieldToIdle(isHiddenForBackgroundOptimization() ? 10 : 60);
        if (token !== beatMapToken) return null;
      }
    }
    var worker = new Worker(getMusicTempoWorkerUrl());
    return await new Promise(function(resolve) {
      var done = false;
      var timer = setTimeout(function(){
        if (done) return;
        done = true;
        worker.terminate();
        resolve(null);
      }, 16000);
      worker.onmessage = function(ev) {
        if (done) return;
        done = true;
        clearTimeout(timer);
        worker.terminate();
        var data = ev.data || {};
        if (!data.ok) {
          console.warn('music-tempo worker failed:', data.error);
          resolve(null);
          return;
        }
        resolve(data);
      };
      worker.onerror = function(err) {
        if (done) return;
        done = true;
        clearTimeout(timer);
        worker.terminate();
        console.warn('music-tempo worker error:', err && err.message ? err.message : err);
        resolve(null);
      };
      worker.postMessage({
        mono: mono.buffer,
        sampleRate: buffer.sampleRate,
        scriptUrl: __assetUrl('music-tempo.min.js')
      }, [mono.buffer]);
    });
  } catch (err) {
    console.warn('music-tempo worker setup failed:', err);
    return null;
  }
}

function scheduleBeatAnalysis(songId, audioUrl, token, song) {
  if (!songId || !audioUrl) return;
  if (djMode.active) {
    cancelBeatAnalysisTimer();
    beatAnalysisStartedAt = 0;
    hideBeatChip();
    return;
  }
  cancelBeatAnalysisTimer();
  beatAnalysisStartedAt = 0;
  hideBeatChip();
  beatAnalysisTimer = setTimeout(function waitForQuietStart(){
    beatAnalysisTimer = null;
    if (token !== beatMapToken || !audio || audio.paused) return;
    var current = audio.currentTime || 0;
    if (current < beatAnalysisConfig.minPlaybackSec) {
      beatAnalysisTimer = setTimeout(waitForQuietStart, Math.max(500, (beatAnalysisConfig.minPlaybackSec - current) * 1000));
      return;
    }
    var startAnalysis = async function(){
      if (token !== beatMapToken || !audio || audio.paused || beatMapCache[songId]) return;
      var diskMap = await readBeatDiskCache(songId);
      if (diskMap) {
        applyBeatMapCacheForCurrent(songId, diskMap, token, 'D盘节拍缓存命中:');
        return;
      }
      if (token !== beatMapToken || !audio || audio.paused || beatMapCache[songId]) return;
      if (beatMapBusy) {
        beatAnalysisTimer = setTimeout(function(){
          beatAnalysisTimer = null;
          scheduleAnalysisTask(startAnalysis, 260);
        }, 420);
        return;
      }
      beatAnalysisStartedAt = performance.now();
      analyzeAudioBeats(audioUrl, null, token, {
        skipMusicTempo: beatAnalysisConfig.skipMusicTempoWhilePlaying && !audio.paused,
        background: true,
        song: song || null
      }).then(function(map){
        if (token !== beatMapToken || !map) return;
        smoothBeatMapHandoff(songId, map, token, song || null);
      }).catch(function(err){
        console.warn('scheduled beat analysis failed:', err);
        hideBeatChip();
      });
    };
    scheduleAnalysisTask(startAnalysis, beatAnalysisConfig.idleTimeout);
  }, beatAnalysisConfig.delayMs);
}

function beatMapSongKey(song) {
  if (!song) return '';
  if (song.type === 'local' && song.localKey) return 'local:' + song.localKey;
  if (songProviderKey(song) === 'qq') return 'qq:' + (song.mid || song.songmid || song.id || (song.name + '|' + song.artist));
  if (song.id != null && song.id !== '') return 'song:' + song.id;
  return '';
}

function localBeatDiskKey(localKey, mode) {
  if (!localKey) return '';
  return 'local:' + localKey + ':' + (mode === 'dj' ? 'dj' : 'mr');
}

function updateBeatDiskCacheStatus(data) {
  if (!data) return;
  beatDiskCacheStatus.checked = true;
  beatDiskCacheStatus.enabled = !!data.enabled || data.mode === 'disk';
  beatDiskCacheStatus.mode = data.mode || (beatDiskCacheStatus.enabled ? 'disk' : 'memory-only');
  beatDiskCacheStatus.reason = data.reason || '';
  if (!beatDiskCacheStatus.enabled && !beatDiskCacheNoticeLogged) {
    beatDiskCacheNoticeLogged = true;
    console.log('节拍磁盘缓存不可用，已降级为本次运行内存缓存:', beatDiskCacheStatus.reason || 'unknown');
  }
}

async function ensureBeatDiskCacheStatus() {
  if (beatDiskCacheStatus.checked) return beatDiskCacheStatus;
  try {
    updateBeatDiskCacheStatus(await apiJson('/api/beatmap/cache/status?t=' + Date.now()));
  } catch (e) {
    updateBeatDiskCacheStatus({ enabled:false, mode:'memory-only', reason:'STATUS_FAILED' });
  }
  return beatDiskCacheStatus;
}

async function readBeatDiskCache(key) {
  if (!key || beatMapCache[key]) return beatMapCache[key] || null;
  var st = await ensureBeatDiskCacheStatus();
  if (!st.enabled) return null;
  try {
    var r = await apiJson('/api/beatmap/cache?key=' + encodeURIComponent(key) + '&t=' + Date.now());
    if (r && r.enabled === false) updateBeatDiskCacheStatus(r);
    if (!r || !r.hit || !r.map) return null;
    var map = unpackLocalBeatMap(r.map);
    if (!map) return null;
    beatMapCache[key] = map;
    return map;
  } catch (e) {
    console.warn('beat disk cache read failed:', e);
    return null;
  }
}

async function writeBeatDiskCache(key, map, song, mode) {
  if (!key || !map) return false;
  var st = await ensureBeatDiskCacheStatus();
  if (!st.enabled) return false;
  try {
    var packed = packLocalBeatMap(map);
    if (!packed) return false;
    var r = await apiJson('/api/beatmap/cache', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        key: key,
        mode: mode || 'mr',
        provider: songProviderKey(song),
        title: song && song.name,
        artist: song && song.artist,
        map: packed
      })
    });
    if (r && r.enabled === false) updateBeatDiskCacheStatus(r);
    return !!(r && r.ok);
  } catch (e) {
    console.warn('beat disk cache write failed:', e);
    return false;
  }
}

function isBeatPrefetchCandidate(song) {
  if (!song || isPodcastSong(song) || song.type === 'local' || song.localUrl) return false;
  return !!beatMapSongKey(song);
}

function findNextBeatPrefetchIndex(fromIdx, seen) {
  if (!playQueue.length) return -1;
  seen = seen || {};
  var total = playQueue.length;
  for (var step = 1; step < total; step++) {
    var idx = (fromIdx + step + total) % total;
    if (idx === currentIdx) continue;
    var song = playQueue[idx];
    if (!isBeatPrefetchCandidate(song)) continue;
    var key = beatMapSongKey(song);
    if (!key || beatMapCache[key] || seen[key]) continue;
    return idx;
  }
  return -1;
}

function normalizeBeatPrefetchState(state) {
  state = state || {};
  return {
    keys: Object.assign({}, state.keys || state),
    count: Math.max(0, Number(state.count) || 0)
  };
}

async function fetchBeatPrefetchAudioUrl(song) {
  if (!song) return null;
  var isQQ = songProviderKey(song) === 'qq';
  var requestedQuality = normalizePlaybackQuality(playbackQuality);
  if (!isQQ && requestedQuality === 'jymaster' && !hasProviderSvip('netease', loginStatus)) requestedQuality = 'hires';
  if (isQQ && qqPlaybackQualityCeiling && (requestedQuality === 'jymaster' || requestedQuality === 'hires' || requestedQuality === 'lossless')) requestedQuality = qqPlaybackQualityCeiling;
  var qualityParam = '&quality=' + encodeURIComponent(requestedQuality);
  var data = isQQ
    ? await apiJson('/api/qq/song/url?mid=' + encodeURIComponent(song.mid || song.songmid || song.id || '') + '&mediaMid=' + encodeURIComponent(song.mediaMid || song.media_mid || '') + qualityParam)
    : await apiJson('/api/song/url?id=' + encodeURIComponent(song.id) + qualityParam);
  if (!data || !data.url || data.trial) return null;
  return '/api/audio?url=' + encodeURIComponent(data.url);
}

function scheduleQueueBeatPrefetch(fromIdx, delayMs, state) {
  cancelBeatPrefetchTimer();
  if (!playQueue.length || beatPrefetchBusy || localBeatAnalysis.active) return;
  var prefetchState = normalizeBeatPrefetchState(state);
  if (prefetchState.count >= BEAT_PREFETCH_LIMIT) return;
  var token = beatMapToken;
  var seq = ++beatPrefetchToken;
  var startIdx = isFinite(fromIdx) ? fromIdx : currentIdx;
  var waitMs = delayMs == null ? 1800 : delayMs;
  if (typeof isRenderInteractionActive === 'function' && isRenderInteractionActive()) waitMs = Math.max(waitMs, 2200);
  beatPrefetchTimer = setTimeout(function(){
    beatPrefetchTimer = null;
    runQueueBeatPrefetch(startIdx, token, seq, prefetchState);
  }, waitMs);
}

async function runQueueBeatPrefetch(fromIdx, token, seq, state) {
  if (token !== beatMapToken || seq !== beatPrefetchToken || beatPrefetchBusy || !playQueue.length) return;
  if (audio && audio.paused) return;
  state = normalizeBeatPrefetchState(state);
  if (state.count >= BEAT_PREFETCH_LIMIT) return;
  var idx = findNextBeatPrefetchIndex(fromIdx, state.keys);
  if (idx < 0) return;
  var song = hydrateCustomCover(playQueue[idx]);
  var key = beatMapSongKey(song);
  if (!key) return;
  state.keys[key] = true;
  state.count++;
  beatPrefetchBusy = true;
  beatPrefetchLastKey = key;
  try {
    if (token !== beatMapToken || seq !== beatPrefetchToken) return;
    var diskMap = await readBeatDiskCache(key);
    if (diskMap) {
      console.log('队列节奏D盘缓存命中:', song.name || key, diskMap.visualBeatCount || 0);
      return;
    }
    var audioUrl = await fetchBeatPrefetchAudioUrl(song);
    if (token !== beatMapToken || seq !== beatPrefetchToken || !audioUrl || beatMapCache[key]) return;
    while (typeof isRenderInteractionActive === 'function' && isRenderInteractionActive() && token === beatMapToken && seq === beatPrefetchToken) {
      await yieldToIdle(isHiddenForBackgroundOptimization() ? 30 : 320);
    }
    if (token !== beatMapToken || seq !== beatPrefetchToken || beatMapCache[key]) return;
    while (beatMapBusy && token === beatMapToken && seq === beatPrefetchToken) {
      await yieldToIdle(isHiddenForBackgroundOptimization() ? 30 : 240);
    }
    if (token !== beatMapToken || seq !== beatPrefetchToken || beatMapCache[key]) return;
    var map = await analyzeAudioBeats(audioUrl, null, token, {
      background: true,
      prefetch: true,
      song: song
    });
    if (token !== beatMapToken || seq !== beatPrefetchToken || !map) return;
    beatMapCache[key] = map;
    writeBeatDiskCache(key, map, song, 'mr');
    console.log('队列节奏预热完成:', song.name || key, map.visualBeatCount || 0);
  } catch (err) {
    console.warn('queue beat prefetch failed:', err && err.message ? err.message : err);
  } finally {
    beatPrefetchBusy = false;
    if (state.count < BEAT_PREFETCH_LIMIT && token === beatMapToken && seq === beatPrefetchToken && playQueue.length && !(audio && audio.paused)) {
      scheduleQueueBeatPrefetch(idx, 1600, state);
    }
  }
}

async function analyzeAudioBeats(audioUrl, durationSec, token, options) {
  options = options || {};
  var analysisProfile = cinemaAnalysisProfileForSong(options.song);
  var softGrooveAnalysis = !!(analysisProfile && analysisProfile.softGroove);
  try {
    beatMapBusy = true;
    if (options.prefetch) showBeatChip('预热下一首节奏…');
    else if (options.background) showBeatChip('后台缓冲节奏…');
    await yieldToIdle(beatAnalysisYieldMs(options, 140, 760));
    if (token !== beatMapToken) { hideBeatChip(); beatMapBusy = false; return null; }
    showBeatChip('正在分析节奏…');
    var resp = await fetch(audioUrl);
    if (token !== beatMapToken) { hideBeatChip(); return null; }
    var ab = await resp.arrayBuffer();
    if (token !== beatMapToken) { hideBeatChip(); return null; }

    // 用临时 AudioContext 解码 (我们不能复用 audioCtx 因为它可能 closed)
    var TmpCtx = window.OfflineAudioContext || window.webkitOfflineAudioContext;
    if (!TmpCtx) { hideBeatChip(); return null; }
    var DecodeCtx = window.AudioContext || window.webkitAudioContext;
    var dc = new DecodeCtx();
    var buffer = await new Promise(function(resolve, reject){
      dc.decodeAudioData(ab.slice(0), resolve, reject);
    }).catch(function(e){ console.warn('decode failed:', e); return null; });
    dc.close && dc.close();
    if (!buffer) { hideBeatChip(); return null; }
    if (token !== beatMapToken) { hideBeatChip(); return null; }

    var musicTempoBeats = [];
    var musicTempoGridStep = 0;
    var musicTempoTask = options.skipMusicTempo ? Promise.resolve(null) : analyzeMusicTempoInWorker(buffer, token);

    // 用 OfflineAudioContext 分离低频重鼓 / 中频鼓身 / 高频敲击感.
    var sr = buffer.sampleRate;
    async function renderBand(hpFreq, lpFreq) {
      var off = new TmpCtx(1, buffer.length, sr);
      var src = off.createBufferSource(); src.buffer = buffer;
      var node = src;
      if (hpFreq) {
        var hp = off.createBiquadFilter();
        hp.type = 'highpass';
        hp.frequency.value = Math.min(hpFreq, sr * 0.45);
        hp.Q.value = 0.85;
        node.connect(hp);
        node = hp;
      }
      if (lpFreq) {
        var lp = off.createBiquadFilter();
        lp.type = 'lowpass';
        lp.frequency.value = Math.min(lpFreq, sr * 0.45);
        lp.Q.value = 0.9;
        node.connect(lp);
        node = lp;
      }
      node.connect(off.destination);
      src.start(0);
      var renderedBand = await off.startRendering();
      if (token !== beatMapToken) return null;
      await yieldToIdle(beatAnalysisYieldMs(options, 110, 620));
      return renderedBand.getChannelData(0);
    }
    var bands = [];
    bands.push(await renderBand(38, 155));
    if (token !== beatMapToken || !bands[0]) { hideBeatChip(); return null; }
    bands.push(await renderBand(130, 420));
    if (token !== beatMapToken || !bands[1]) { hideBeatChip(); return null; }
    bands.push(await renderBand(420, 2600));
    if (token !== beatMapToken || !bands[2]) { hideBeatChip(); return null; }
    bands.push(await renderBand(1800, 9000));
    if (token !== beatMapToken) { hideBeatChip(); return null; }
    var lowPcm = bands[0];
    var bodyPcm = bands[1];
    var vocalPcm = bands[2];
    var snapPcm = bands[3];

    // 帧化能量 (10ms 窗口)
    var winSize = Math.floor(sr * 0.010);
    async function makeFrameEnergy(pcm) {
      var frames = Math.floor(pcm.length / winSize);
      var out = new Float32Array(frames);
      for (var f = 0; f < frames; f++) {
        var s = 0;
        var off2 = f * winSize;
        for (var i = 0; i < winSize; i++) {
          var v = pcm[off2 + i];
          s += v * v;
        }
        out[f] = Math.sqrt(s / winSize);
        if (f > 0 && f % 520 === 0) {
          await yieldToPaint();
          if (token !== beatMapToken) return null;
        }
      }
      return out;
    }
    var frameBands = [];
    frameBands.push(await makeFrameEnergy(lowPcm));
    await yieldToIdle(beatAnalysisYieldMs(options, 90, 520));
    frameBands.push(await makeFrameEnergy(bodyPcm));
    await yieldToIdle(beatAnalysisYieldMs(options, 90, 520));
    frameBands.push(await makeFrameEnergy(vocalPcm));
    await yieldToIdle(beatAnalysisYieldMs(options, 90, 520));
    frameBands.push(await makeFrameEnergy(snapPcm));
    if (token !== beatMapToken || !frameBands[0] || !frameBands[1] || !frameBands[2] || !frameBands[3]) { hideBeatChip(); return null; }
    var energy = frameBands[0];
    var bodyEnergy = frameBands[1];
    var vocalEnergy = frameBands[2];
    var snapEnergy = frameBands[3];
    var nFrames = Math.min(energy.length, bodyEnergy.length, vocalEnergy.length, snapEnergy.length);
    function percentile(arr, p) {
      var copy = Array.prototype.slice.call(arr).sort(function(a,b){ return a-b; });
      return copy.length ? copy[Math.floor(copy.length * p)] : 0.001;
    }
    function bandAt(arr, f) {
      var a = arr[Math.max(0, f - 1)] || 0;
      var b = arr[f] || 0;
      var c = arr[Math.min(nFrames - 1, f + 1)] || 0;
      return (a + b * 2 + c) * 0.25;
    }
    var lowRef = Math.max(0.0008, percentile(energy, 0.86));
    var bodyRef = Math.max(0.0008, percentile(bodyEnergy, 0.86));
    var vocalRef = Math.max(0.0008, percentile(vocalEnergy, 0.86));
    var snapRef = Math.max(0.0008, percentile(snapEnergy, 0.86));

    // 计算 onset (能量正向差分), 然后取峰
    function makeOnset(arr) {
      var out = new Float32Array(nFrames);
      for (var oi = 1; oi < nFrames; oi++) {
        out[oi] = Math.max(0, arr[oi] - arr[oi - 1]);
      }
      return out;
    }
    var onset = makeOnset(energy);
    var bodyOnset = makeOnset(bodyEnergy);
    var vocalOnset = makeOnset(vocalEnergy);
    var snapOnset = makeOnset(snapEnergy);
    var lowOnsetRef = Math.max(0.00025, percentile(onset, 0.88));
    var bodyOnsetRef = Math.max(0.00025, percentile(bodyOnset, 0.88));
    var vocalOnsetRef = Math.max(0.00025, percentile(vocalOnset, 0.88));
    var snapOnsetRef = Math.max(0.00025, percentile(snapOnset, 0.88));

    function softGrooveFrameScore(frame) {
      var sf = Math.max(0, Math.min(nFrames - 1, Math.round(frame)));
      var lowTone = Math.min(2.2, bandAt(energy, sf) / lowRef);
      var bodyTone = Math.min(2.2, bandAt(bodyEnergy, sf) / bodyRef);
      var vocalTone = Math.min(2.2, bandAt(vocalEnergy, sf) / vocalRef);
      var snapTone = Math.min(2.2, bandAt(snapEnergy, sf) / snapRef);
      var lowRise = Math.min(2.6, (onset[sf] || 0) / lowOnsetRef);
      var bodyRise = Math.min(2.6, (bodyOnset[sf] || 0) / bodyOnsetRef);
      var vocalRise = Math.min(2.6, (vocalOnset[sf] || 0) / vocalOnsetRef);
      var snapRise = Math.min(2.6, (snapOnset[sf] || 0) / snapOnsetRef);
      var drumRise = lowRise * 0.52 + bodyRise * 0.42 + snapRise * 0.08;
      var drumTone = lowTone * 0.24 + bodyTone * 0.22 + snapTone * 0.05;
      var vocalLeak = Math.max(0, vocalRise + vocalTone * 0.30 - (lowRise + bodyRise) * 0.54 - 0.18);
      return Math.max(0, drumRise + drumTone - vocalLeak * 0.18);
    }

    function bestSoftGrooveFrameNear(time, radiusSec) {
      var center = Math.max(0, Math.min(nFrames - 1, Math.round(time / 0.010)));
      var radius = Math.max(1, Math.round(Math.max(0.010, radiusSec || 0.040) / 0.010));
      var base = softGrooveFrameScore(center);
      var bestFrame = center;
      var bestScore = base;
      for (var sf = Math.max(0, center - radius); sf <= Math.min(nFrames - 1, center + radius); sf++) {
        var dist = Math.abs(sf - center) / Math.max(1, radius);
        var score = softGrooveFrameScore(sf) * (1 - dist * 0.16);
        if (score > bestScore) {
          bestScore = score;
          bestFrame = sf;
        }
      }
      return { frame: bestFrame, time: bestFrame * 0.010, score: bestScore, base: base };
    }

    function scoreSoftGrooveTempoOffset(times, offset, step) {
      if (!times || !times.length) return 0;
      var total = 0;
      var weightTotal = 0;
      var localRadius = Math.min(0.026, Math.max(0.014, (step || 0.55) * 0.045));
      var stride = times.length > 720 ? 2 : 1;
      for (var si = 0; si < times.length; si += stride) {
        var t = times[si] + offset;
        if (!isFinite(t) || t < 1.0 || t > buffer.duration - 0.40) continue;
        var slot = si % 4;
        var slotWeight = slot === 0 ? 1.22 : (slot === 2 ? 1.06 : 0.88);
        var point = bestSoftGrooveFrameNear(t, localRadius);
        total += point.score * slotWeight;
        weightTotal += slotWeight;
      }
      return weightTotal > 0 ? total / weightTotal : 0;
    }

    function estimateSoftGrooveTempoOffset(times, step) {
      if (!softGrooveAnalysis || !times || times.length < 8 || !step) return 0;
      var maxOffset = Math.min(0.20, Math.max(0.075, step * 0.32));
      var baseScore = scoreSoftGrooveTempoOffset(times, 0, step);
      var bestOffset = 0;
      var bestScore = baseScore;
      for (var off = -maxOffset; off <= maxOffset + 0.0001; off += 0.010) {
        var score = scoreSoftGrooveTempoOffset(times, off, step);
        if (score > bestScore) {
          bestScore = score;
          bestOffset = off;
        }
      }
      if (Math.abs(bestOffset) < 0.014) return 0;
      return bestScore > baseScore * 1.055 ? Math.max(-maxOffset, Math.min(maxOffset, bestOffset)) : 0;
    }

    function refineSoftGrooveBeatTime(time, step) {
      if (!softGrooveAnalysis || !analysisProfile.localRefine) return { time: time, score: 0, base: 0 };
      var radius = Math.min(0.058, Math.max(0.024, (step || 0.55) * 0.095));
      var point = bestSoftGrooveFrameNear(time, radius);
      if (Math.abs(point.time - time) < 0.011) return { time: time, score: point.score, base: point.base };
      if (point.score < point.base * 1.045) return { time: time, score: point.score, base: point.base };
      return { time: point.time, score: point.score, base: point.base };
    }

    function thinSoftGrooveCameraBeats(events, step, duration) {
      if (!analysisProfile.sparseCamera || !events || events.length < 6) return events || [];
      step = Math.max(0.001, step || medianGap(events.map(function(b){ return b.time; }), 0.30, 1.20) || 0.82);
      function moodScore(b) {
        if (!b) return 0;
        return (b.grooveEvidence || 0) * 0.56 + (b.impact || 0) * 0.34 + (b.strength || 0) * 0.18 + (b.low || 0) * 0.10 + (b.body || 0) * 0.08;
      }
      function eventPercentile(rows, p) {
        var vals = rows.map(function(row){ return row.score; }).sort(function(a,b){ return a-b; });
        return vals.length ? vals[Math.min(vals.length - 1, Math.floor(vals.length * p))] : 0;
      }
      function medianNumber(vals) {
        vals = vals.filter(function(v){ return isFinite(v); }).sort(function(a,b){ return a-b; });
        return vals.length ? vals[Math.floor(vals.length * 0.5)] : 0;
      }
      function cloneSparseBeat(b, score, accent, tag) {
        var out = Object.assign({}, b);
        out.primary = true;
        out.camera = true;
        out.pulse = true;
        out.sparse = true;
        out.tone = tag || 'sunset-groove';
        out.impact = clampRange((out.impact || out.strength || 0.30) * (accent ? 0.76 : 0.66) + score * 0.07, 0.18, accent ? 0.58 : 0.50);
        out.strength = clampRange((out.strength || 0.34) * (accent ? 0.76 : 0.68) + score * 0.055, 0.30, accent ? 0.64 : 0.56);
        out.mass = clampRange((out.mass || 0.48) * 0.78, 0.28, 0.60);
        out.sharpness = clampRange((out.sharpness || 0.10) * 0.66, 0.05, 0.32);
        out._sparseScore = score;
        return out;
      }
      function findBestEventNear(time, radius) {
        var best = null;
        var bestScore = -1;
        radius = radius || 0.20;
        for (var i = 0; i < events.length; i++) {
          var b = events[i];
          if (!b || !isFinite(b.time)) continue;
          var dist = Math.abs(b.time - time);
          if (dist > radius) continue;
          var score = moodScore(b) * (1 - dist / radius * 0.18);
          if (score > bestScore) {
            best = b;
            bestScore = score;
          }
        }
        return best ? { beat: best, score: Math.max(0, bestScore) } : null;
      }
      function buildBeatFromFrame(time, score, tag) {
        var f = Math.max(0, Math.min(nFrames - 1, Math.round(time / 0.010)));
        var lowTone = Math.min(2.0, bandAt(energy, f) / lowRef);
        var bodyTone = Math.min(2.0, bandAt(bodyEnergy, f) / bodyRef);
        var snapTone = Math.min(2.0, bandAt(snapEnergy, f) / snapRef);
        var toneTotal = Math.max(0.001, lowTone + bodyTone * 0.72 + snapTone * 0.58);
        var lowMix = lowTone / toneTotal;
        var bodyMix = (bodyTone * 0.72) / toneTotal;
        var snapMix = (snapTone * 0.58) / toneTotal;
        return {
          time: time,
          strength: clampRange(0.30 + score * 0.055, 0.30, 0.52),
          confidence: clampRange(0.46 + score * 0.08, 0.46, 0.66),
          primary: true,
          camera: true,
          pulse: true,
          sparse: true,
          tone: tag || 'sunset-pattern',
          impact: clampRange(0.18 + score * 0.060, 0.18, 0.48),
          low: Math.max(0.22, Math.min(0.74, lowMix)),
          body: bodyMix,
          snap: snapMix,
          mass: Math.max(0.30, Math.min(0.58, lowMix * 0.58 + bodyMix * 0.20)),
          sharpness: Math.max(0.05, Math.min(0.28, snapMix * 0.72))
        };
      }
      function learnIntroPattern() {
        if (!analysisProfile.introPattern) return null;
        var introEnd = Math.min(duration || 34, 34);
        var rows = events.filter(function(b){ return b && isFinite(b.time) && b.time >= 1.2 && b.time <= introEnd; })
          .map(function(b){ return { beat: b, score: moodScore(b) }; });
        if (rows.length < 6) return null;
        var scoreFloor = Math.max(0.34, eventPercentile(rows, 0.58));
        var hits = [];
        var minIntroGap = 1.08;
        rows.forEach(function(row){
          if (row.score < scoreFloor && !(row.beat && (row.beat.low || 0) > 0.42 && row.score > scoreFloor * 0.78)) return;
          var last = hits[hits.length - 1];
          if (last && row.beat.time - last.beat.time < minIntroGap) {
            if (row.score > last.score) hits[hits.length - 1] = row;
          } else {
            hits.push(row);
          }
        });
        if (hits.length < 5) return null;
        var gaps = [];
        for (var hi = 1; hi < hits.length; hi++) {
          var gap = hits[hi].beat.time - hits[hi - 1].beat.time;
          if (gap >= 1.18 && gap <= 2.45) gaps.push(gap);
        }
        if (gaps.length < 4) return null;
        var firstGaps = gaps.slice(0, Math.min(8, gaps.length));
        var evenGaps = [];
        var oddGaps = [];
        for (var gi = 0; gi < firstGaps.length; gi++) {
          (gi % 2 === 0 ? evenGaps : oddGaps).push(firstGaps[gi]);
        }
        var evenGap = medianNumber(evenGaps);
        var oddGap = medianNumber(oddGaps);
        var patternGaps;
        if (evenGap && oddGap && Math.abs(evenGap - oddGap) > 0.16) {
          patternGaps = [evenGap, oddGap].map(function(v){ return clampRange(v, 1.30, 2.22); });
        } else {
          patternGaps = [clampRange(medianNumber(firstGaps), 1.42, 2.12)];
        }
        var refScore = Math.max(0.35, eventPercentile(hits, 0.50));
        return {
          anchor: hits[0].beat.time,
          gaps: patternGaps,
          refScore: refScore,
          introHitCount: hits.length,
          introTimes: hits.slice(0, 10).map(function(row){ return row.beat.time; })
        };
      }
      function buildIntroPatternBeats() {
        var pattern = learnIntroPattern();
        if (!pattern) return null;
        var selected = [];
        var t = pattern.anchor;
        var gi = 0;
        var avgGap = pattern.gaps.reduce(function(a,b){ return a + b; }, 0) / Math.max(1, pattern.gaps.length);
        var refineRadius = Math.min(0.22, Math.max(0.14, avgGap * 0.10));
        var findRadius = Math.min(0.26, Math.max(0.18, avgGap * 0.13));
        while (t < (duration || 0) - 0.55) {
          var point = bestSoftGrooveFrameNear(t, refineRadius);
          var refinedTime = Math.abs(point.time - t) <= refineRadius ? point.time : t;
          var match = findBestEventNear(refinedTime, findRadius) || findBestEventNear(t, findRadius);
          var score = match ? match.score : Math.max(0.26, (point.score || 0) / Math.max(1.0, pattern.refScore * 2.2));
          var accent = (gi % pattern.gaps.length) === 0;
          var beat = match ? cloneSparseBeat(match.beat, score, accent, 'sunset-intro-pattern') : buildBeatFromFrame(refinedTime, score, 'sunset-intro-pattern');
          beat.time = refinedTime;
          beat.index = gi;
          beat.combo = accent ? 'downbeat' : 'rebound';
          beat.introPattern = true;
          selected.push(beat);
          t += pattern.gaps[gi % pattern.gaps.length];
          gi++;
          if (gi > 800) break;
        }
        for (var si = 0; si < selected.length; si++) delete selected[si]._sparseScore;
        console.log('soft-groove intro pattern camera:', selected.length, 'gaps:', pattern.gaps.map(function(v){ return v.toFixed(2); }).join('/'), 'anchor:', pattern.anchor.toFixed(2), 'introHits:', pattern.introHitCount);
        return selected.length >= 8 ? selected : null;
      }
      var introPatternBeats = buildIntroPatternBeats();
      if (introPatternBeats && introPatternBeats.length >= 8) return introPatternBeats;

      var railStep = step;
      while (railStep < 1.35) railStep *= 2;
      railStep = clampRange(railStep, 1.42, 2.12);
      var railMultiple = Math.max(1, Math.round(railStep / step));
      if (railMultiple < 2 && step < 1.20) railMultiple = 2;
      var phaseScores = new Array(railMultiple);
      for (var pi = 0; pi < phaseScores.length; pi++) phaseScores[pi] = 0;
      for (var ei = 0; ei < events.length; ei++) {
        var ev = events[ei];
        if (!ev || !isFinite(ev.time)) continue;
        if (ev.time < 1.0 || (duration && ev.time > duration - 0.65)) continue;
        var phase = Math.abs((ev.index == null ? ei : ev.index) % railMultiple);
        var earlyWeight = ev.time < 70 ? 1.18 : (ev.time < 205 ? 1.0 : 0.94);
        phaseScores[phase] += moodScore(ev) * earlyWeight;
      }
      var bestPhase = 0;
      for (var ps = 1; ps < phaseScores.length; ps++) {
        if (phaseScores[ps] > phaseScores[bestPhase]) bestPhase = ps;
      }
      var selected = [];
      var minGap = Math.max(1.12, railStep * 0.68);
      function pushSparse(b, score, accent) {
        if (!b || score < 0.28) return;
        var copy = cloneSparseBeat(b, score, accent, 'sunset-groove');
        copy.combo = selected.length % 2 === 0 ? 'downbeat' : 'rebound';
        var last = selected[selected.length - 1];
        if (last && copy.time - last.time < minGap) {
          if (score > (last._sparseScore || 0) + 0.05) selected[selected.length - 1] = copy;
          return;
        }
        selected.push(copy);
      }
      for (var si = 0; si < events.length; si++) {
        var b = events[si];
        if (!b || !isFinite(b.time)) continue;
        var idx = b.index == null ? si : b.index;
        var score = moodScore(b);
        var onRail = Math.abs(idx % railMultiple) === bestPhase;
        if (onRail) {
          pushSparse(b, score, false);
        } else if (score >= 0.82 && (!selected.length || b.time - selected[selected.length - 1].time >= minGap * 1.18)) {
          pushSparse(b, score, true);
        }
      }
      for (var ci = 0; ci < selected.length; ci++) {
        delete selected[ci]._sparseScore;
      }
      var minExpected = duration ? Math.max(16, Math.floor(duration / 3.2)) : 16;
      if (selected.length < minExpected) {
        var fallback = events.filter(function(b){ return b && b.camera !== false && b.pulse !== false; });
        selected = [];
        for (var fi = 0; fi < fallback.length; fi++) pushSparse(fallback[fi], moodScore(fallback[fi]), false);
        for (var di = 0; di < selected.length; di++) delete selected[di]._sparseScore;
      }
      console.log('soft-groove sparse camera:', selected.length, 'of', events.length, 'railStep:', railStep.toFixed(2), 'phase:', bestPhase + '/' + railMultiple);
      return selected.length >= 4 ? selected : events.filter(function(b){ return b && b.camera !== false; });
    }

    // 自适应阈值: 滑动均值 + 标准差, 输出带强度的 beat 事件.
    var winN = 50;  // 0.5 秒
    var candidates = [];
    var lastKickFrame = -winN;
    var minIntervalFrames = 12;  // 120ms, 粒子可响应较密集的低频瞬态.
    for (var f = winN; f < nFrames - 5; f++) {
      var sum = 0, sqSum = 0;
      for (var k = f - winN; k < f; k++) { sum += onset[k]; sqSum += onset[k] * onset[k]; }
      var mean = sum / winN;
      var std = Math.sqrt(Math.max(0, sqSum / winN - mean * mean));
      var thresh = mean + std * 2.35 + 0.0045;
      if (onset[f] > thresh && onset[f] > onset[f-1] && onset[f] >= onset[f+1]) {
        if (f - lastKickFrame >= minIntervalFrames) {
          var localScore = (onset[f] - thresh) / Math.max(0.006, std + mean * 0.35);
          candidates.push({
            frame: f,
            time: f * 0.010,
            raw: onset[f],
            score: localScore,
            lowTone: Math.min(2.0, bandAt(energy, f) / lowRef),
            bodyTone: Math.min(2.0, bandAt(bodyEnergy, f) / bodyRef),
            vocalTone: Math.min(2.0, bandAt(vocalEnergy, f) / vocalRef),
            snapTone: Math.min(2.0, bandAt(snapEnergy, f) / snapRef)
          });
          lastKickFrame = f;
        }
      }
      if (f > winN && f % 900 === 0) {
        await yieldToPaint();
        if (token !== beatMapToken) { hideBeatChip(); return null; }
      }
    }

    var scores = candidates.map(function(b){ return b.score; }).sort(function(a,b){ return a-b; });
    var p75 = scores.length ? scores[Math.floor(scores.length * 0.75)] : 1;
    var p92 = scores.length ? scores[Math.floor(scores.length * 0.92)] : Math.max(1, p75);
    var strongTimes = [];
    var beats = candidates.map(function(b, i){
      var strength = Math.max(0.18, Math.min(1, (b.score - p75 * 0.36) / Math.max(0.001, p92 - p75 * 0.36)));
      var lowDominance = b.lowTone / Math.max(0.001, b.vocalTone * 0.84 + b.bodyTone * 0.36 + b.snapTone * 0.10);
      var toneTotal = Math.max(0.001, b.lowTone + b.bodyTone * 0.72 + b.snapTone * 0.58);
      var lowMix = b.lowTone / toneTotal;
      var bodyMix = (b.bodyTone * 0.72) / toneTotal;
      var snapMix = (b.snapTone * 0.58) / toneTotal;
      var drumLike = b.lowTone > 0.38 && (lowMix > 0.42 || lowDominance > 0.72);
      if (strength > 0.55 && drumLike) strongTimes.push(b.time);
      var sharpness = Math.max(0.08, Math.min(1, snapMix * 1.55 + strength * 0.10));
      var mass = Math.max(0.25, Math.min(1, lowMix * 0.72 + bodyMix * 0.36 + strength * 0.20));
      var tone = snapMix > 0.34 && b.snapTone > 0.55 ? 'snap' : (bodyMix > 0.36 && b.bodyTone > 0.55 ? 'body' : (lowMix > 0.55 ? 'deep' : 'mixed'));
      return {
        time: b.time,
        strength: strength,
        confidence: Math.max(0.22, Math.min(1, b.score / Math.max(0.001, p92))),
        primary: drumLike && strength >= 0.50,
        camera: drumLike && strength >= 0.42,
        tone: tone,
        low: lowMix,
        body: bodyMix,
        snap: snapMix,
        mass: mass,
        sharpness: sharpness,
        index: i
      };
    });

    var gaps = [];
    for (var gi = 1; gi < strongTimes.length; gi++) {
      var gap = strongTimes[gi] - strongTimes[gi - 1];
      if (gap >= 0.26 && gap <= 0.86) gaps.push(gap);
    }
    gaps.sort(function(a,b){ return a-b; });
    var gridStep = gaps.length ? gaps[Math.floor(gaps.length * 0.5)] : 0;
    var cameraBeats = beats.filter(function(b){ return b.camera; });
    if (gridStep > 0) {
      for (var bi = 0; bi < beats.length; bi++) {
        var prevGap = bi > 0 ? beats[bi].time - beats[bi - 1].time : gridStep;
        var nextGap = bi < beats.length - 1 ? beats[bi + 1].time - beats[bi].time : gridStep;
        var gridLike = Math.abs(prevGap - gridStep) < gridStep * 0.32 || Math.abs(nextGap - gridStep) < gridStep * 0.32;
        beats[bi].primary = beats[bi].camera && beats[bi].strength >= (gridLike ? 0.42 : 0.58);
      }
      if (gridStep >= 0.38 && gridStep <= 0.88 && strongTimes.length >= 4) {
        var anchor = strongTimes[0];
        while (anchor - gridStep > 0.20) anchor -= gridStep;
        var gridBeats = [];
        var windowSec = Math.min(0.18, gridStep * 0.30);
        for (var gt = anchor; gt < buffer.duration - 0.05; gt += gridStep) {
          var best = null;
          var bestDist = windowSec;
          for (var ci = 0; ci < beats.length; ci++) {
            var dist = Math.abs(beats[ci].time - gt);
            if (dist < bestDist) {
              best = beats[ci];
              bestDist = dist;
            }
          }
          if (best && best.camera) {
            best.primary = true;
            best.strength = Math.max(best.strength, 0.54);
            best.confidence = Math.max(best.confidence, 0.58);
            gridBeats.push(best);
          } else {
            var gf = Math.max(0, Math.min(nFrames - 1, Math.round(gt / 0.010)));
            var lowTone = Math.min(2.0, bandAt(energy, gf) / lowRef);
            var bodyTone = Math.min(2.0, bandAt(bodyEnergy, gf) / bodyRef);
            var vocalTone = Math.min(2.0, bandAt(vocalEnergy, gf) / vocalRef);
            var snapTone = Math.min(2.0, bandAt(snapEnergy, gf) / snapRef);
            var lowDominance = lowTone / Math.max(0.001, vocalTone * 0.84 + bodyTone * 0.36 + snapTone * 0.10);
            var toneTotal = Math.max(0.001, lowTone + bodyTone * 0.72 + snapTone * 0.58);
            var lowMix = lowTone / toneTotal;
            var bodyMix = (bodyTone * 0.72) / toneTotal;
            var snapMix = (snapTone * 0.58) / toneTotal;
            if (lowTone <= 0.38 || (lowMix <= 0.42 && lowDominance <= 0.72)) continue;
            gridBeats.push({
              time: gt,
              strength: 0.53,
              confidence: 0.60,
              primary: true,
              ghost: true,
              tone: 'grid',
              low: lowMix,
              body: bodyMix,
              snap: snapMix,
              mass: Math.max(0.35, Math.min(0.82, lowMix * 0.72 + bodyMix * 0.36 + 0.16)),
              sharpness: Math.max(0.08, Math.min(0.65, snapMix * 1.25)),
              index: gridBeats.length
            });
          }
        }
        cameraBeats = gridBeats;
      }
    }

    var musicTempoResult = await musicTempoTask;
    if (token !== beatMapToken) { hideBeatChip(); return null; }
    if (musicTempoResult && musicTempoResult.beats && musicTempoResult.beats.length) {
      musicTempoBeats = normalizeMusicTempoBeats(musicTempoResult.beats || [], buffer.duration);
      musicTempoGridStep = medianGap(musicTempoBeats, 0.36, 1.00);
      console.log('music-tempo worker:', musicTempoResult.tempo, 'bpm, beats:', musicTempoBeats.length, 'step:', musicTempoGridStep);
    }

    if (musicTempoBeats.length >= 4) {
      var musicTempoPhaseOffset = estimateTempoPhaseOffset(musicTempoBeats, beats, musicTempoGridStep || gridStep, buffer.duration);
      if (musicTempoPhaseOffset) {
        musicTempoBeats = musicTempoBeats.map(function(t){ return t + musicTempoPhaseOffset; })
          .filter(function(t){ return isFinite(t) && t >= 0.05 && t < buffer.duration - 0.05; });
        console.log('music-tempo phase correction:', musicTempoPhaseOffset.toFixed(3), 's');
      }
      if (analysisProfile.phaseScan) {
        var softGroovePhaseOffset = estimateSoftGrooveTempoOffset(musicTempoBeats, musicTempoGridStep || gridStep);
        if (softGroovePhaseOffset) {
          musicTempoBeats = musicTempoBeats.map(function(t){ return t + softGroovePhaseOffset; })
            .filter(function(t){ return isFinite(t) && t >= 0.05 && t < buffer.duration - 0.05; });
          console.log('soft-groove phase correction:', softGroovePhaseOffset.toFixed(3), 's');
        }
      }
      var tempoCameraBeats = [];
      var tempoWindow = Math.min(0.16, Math.max(0.095, (musicTempoGridStep || 0.60) * 0.24));
      var tempoMetrics = [];
      for (var ti = 0; ti < musicTempoBeats.length; ti++) {
        var mtTime = musicTempoBeats[ti];
        var refinedPoint = refineSoftGrooveBeatTime(mtTime, musicTempoGridStep || gridStep);
        var metricTime = refinedPoint.time;
        var nearest = null;
        var nearestDist = tempoWindow;
        for (var nb = 0; nb < beats.length; nb++) {
          var nd = Math.abs(beats[nb].time - metricTime);
          if (nd < nearestDist) {
            nearest = beats[nb];
            nearestDist = nd;
          }
        }
        var mf = Math.max(0, Math.min(nFrames - 1, Math.round(metricTime / 0.010)));
        var mtLowTone = Math.min(2.0, bandAt(energy, mf) / lowRef);
        var mtBodyTone = Math.min(2.0, bandAt(bodyEnergy, mf) / bodyRef);
        var mtVocalTone = Math.min(2.0, bandAt(vocalEnergy, mf) / vocalRef);
        var mtSnapTone = Math.min(2.0, bandAt(snapEnergy, mf) / snapRef);
        var mtLowRise = Math.min(2.5, (onset[mf] || 0) / lowOnsetRef);
        var mtBodyRise = Math.min(2.5, (bodyOnset[mf] || 0) / bodyOnsetRef);
        var mtVocalRise = Math.min(2.5, (vocalOnset[mf] || 0) / vocalOnsetRef);
        var mtSnapRise = Math.min(2.5, (snapOnset[mf] || 0) / snapOnsetRef);
        var mtLowDominance = mtLowTone / Math.max(0.001, mtVocalTone * 0.84 + mtBodyTone * 0.36 + mtSnapTone * 0.10);
        var mtToneTotal = Math.max(0.001, mtLowTone + mtBodyTone * 0.72 + mtSnapTone * 0.58);
        var mtLowMix = mtLowTone / mtToneTotal;
        var mtBodyMix = (mtBodyTone * 0.72) / mtToneTotal;
        var mtSnapMix = (mtSnapTone * 0.58) / mtToneTotal;
        var mtPower = mtLowTone * 0.44 + mtBodyTone * 0.16 + mtSnapTone * 0.08 + Math.min(1.8, mtLowDominance) * 0.16 + (nearest ? nearest.strength * 0.46 : 0);
        if (softGrooveAnalysis) {
          var vocalLeak = Math.max(0, mtVocalRise + mtVocalTone * 0.22 - (mtLowRise + mtBodyRise) * 0.50 - 0.14);
          mtPower = mtLowTone * 0.26 + mtBodyTone * 0.24 + mtLowRise * 0.34 + mtBodyRise * 0.32 + mtSnapRise * 0.06 + Math.min(1.7, mtLowDominance) * 0.10 + (nearest ? nearest.strength * 0.30 : 0) - vocalLeak * 0.16;
        }
        tempoMetrics.push({
          time: metricTime,
          gridTime: mtTime,
          nearest: nearest,
          lowTone: mtLowTone,
          bodyTone: mtBodyTone,
          snapTone: mtSnapTone,
          lowRise: mtLowRise,
          bodyRise: mtBodyRise,
          snapRise: mtSnapRise,
          lowDominance: mtLowDominance,
          lowMix: mtLowMix,
          bodyMix: mtBodyMix,
          snapMix: mtSnapMix,
          power: mtPower,
          softScore: refinedPoint.score || 0,
          index: ti
        });
      }
      var tempoPowers = tempoMetrics.map(function(m){ return m.power; });
      var tempoLowTones = tempoMetrics.map(function(m){ return m.lowTone; });
      var tempoBodyTones = tempoMetrics.map(function(m){ return m.bodyTone; });
      var tempoSnapTones = tempoMetrics.map(function(m){ return m.snapTone; });
      var tempoLowRises = tempoMetrics.map(function(m){ return m.lowRise || 0; });
      var tempoBodyRises = tempoMetrics.map(function(m){ return m.bodyRise || 0; });
      var tempoSnapRises = tempoMetrics.map(function(m){ return m.snapRise || 0; });
      var powerFloor = Math.max(0.001, percentile(tempoPowers, 0.25));
      var powerCeil = Math.max(powerFloor + 0.001, percentile(tempoPowers, 0.90));
      var lowFloor = Math.max(0.001, percentile(tempoLowTones, 0.25));
      var lowCeil = Math.max(lowFloor + 0.001, percentile(tempoLowTones, 0.88));
      var bodyFloor = Math.max(0.001, percentile(tempoBodyTones, 0.25));
      var bodyCeil = Math.max(bodyFloor + 0.001, percentile(tempoBodyTones, 0.90));
      var snapFloor = Math.max(0.001, percentile(tempoSnapTones, 0.25));
      var snapCeil = Math.max(snapFloor + 0.001, percentile(tempoSnapTones, 0.90));
      var lowRiseFloor = Math.max(0.001, percentile(tempoLowRises, 0.25));
      var lowRiseCeil = Math.max(lowRiseFloor + 0.001, percentile(tempoLowRises, 0.90));
      var bodyRiseFloor = Math.max(0.001, percentile(tempoBodyRises, 0.25));
      var bodyRiseCeil = Math.max(bodyRiseFloor + 0.001, percentile(tempoBodyRises, 0.90));
      var snapRiseFloor = Math.max(0.001, percentile(tempoSnapRises, 0.25));
      var snapRiseCeil = Math.max(snapRiseFloor + 0.001, percentile(tempoSnapRises, 0.90));
      for (var tm = 0; tm < tempoMetrics.length; tm++) {
        var m = tempoMetrics[tm];
        var mtSlot = m.index % 4;
        var powerRel = clamp01((m.power - powerFloor) / (powerCeil - powerFloor));
        var lowRel = clamp01((m.lowTone - lowFloor) / (lowCeil - lowFloor));
        var bodyRel = clamp01((m.bodyTone - bodyFloor) / (bodyCeil - bodyFloor));
        var snapRel = clamp01((m.snapTone - snapFloor) / (snapCeil - snapFloor));
        var lowRiseRel = clamp01(((m.lowRise || 0) - lowRiseFloor) / (lowRiseCeil - lowRiseFloor));
        var bodyRiseRel = clamp01(((m.bodyRise || 0) - bodyRiseFloor) / (bodyRiseCeil - bodyRiseFloor));
        var snapRiseRel = clamp01(((m.snapRise || 0) - snapRiseFloor) / (snapRiseCeil - snapRiseFloor));
        var mtImpact = clamp01(powerRel * 0.50 + lowRel * 0.24 + bodyRel * 0.18 + snapRel * 0.08);
        if (m.nearest) mtImpact = Math.max(mtImpact, Math.min(1, m.nearest.strength * 0.58 + (m.nearest.primary ? 0.08 : 0)));
        if (softGrooveAnalysis) {
          mtImpact = clamp01(powerRel * 0.34 + lowRel * 0.18 + bodyRel * 0.18 + lowRiseRel * 0.24 + bodyRiseRel * 0.24 + snapRiseRel * 0.04);
          if (m.nearest) mtImpact = Math.max(mtImpact, Math.min(0.72, m.nearest.strength * 0.42 + (m.nearest.primary ? 0.06 : 0)));
        }
        var activeCamera = mtImpact >= 0.20 || (mtSlot === 0 && mtImpact >= 0.15 && (lowRel > 0.20 || bodyRel > 0.26));
        var activePulse = mtImpact >= 0.24 || (mtSlot === 0 && mtImpact >= 0.18);
        var grooveEvidence = lowRiseRel * 0.52 + bodyRiseRel * 0.48 + lowRel * 0.20 + bodyRel * 0.18;
        if (softGrooveAnalysis) {
          activeCamera = mtImpact >= 0.19 || (mtSlot === 0 && mtImpact >= 0.135 && grooveEvidence >= 0.32);
          activePulse = mtImpact >= 0.23 || (mtSlot === 0 && mtImpact >= 0.165 && grooveEvidence >= 0.28);
        }
        var downbeatLift = activeCamera ? (mtSlot === 0 ? 0.14 : (mtSlot === 2 ? 0.06 : 0)) : 0;
        var mtStrength = 0.26 + powerRel * 0.23 + lowRel * 0.10 + bodyRel * 0.08 + snapRel * 0.04 + downbeatLift;
        if (m.nearest) mtStrength = Math.max(mtStrength, 0.42 + m.nearest.strength * 0.28);
        if (mtSlot === 0 && activeCamera) mtStrength = Math.max(mtStrength, 0.54 + mtImpact * 0.16);
        if (!activeCamera) mtStrength = Math.min(mtStrength, 0.36);
        if (softGrooveAnalysis) {
          mtStrength = 0.24 + powerRel * 0.18 + lowRel * 0.08 + bodyRel * 0.08 + lowRiseRel * 0.13 + bodyRiseRel * 0.12 + downbeatLift * 0.90;
          if (m.nearest) mtStrength = Math.max(mtStrength, 0.36 + m.nearest.strength * 0.22);
          if (mtSlot === 0 && activeCamera) mtStrength = Math.max(mtStrength, 0.50 + mtImpact * 0.15);
          if (mtSlot === 2 && activeCamera) mtStrength = Math.max(mtStrength, 0.43 + mtImpact * 0.10);
          if (!activeCamera) mtStrength = Math.min(mtStrength, 0.34);
          mtStrength = Math.max(0.28, Math.min(0.76, mtStrength));
        } else {
          mtStrength = Math.max(0.30, Math.min(0.82, mtStrength));
        }
        var lowForCamera = Math.max(0.22, Math.min(0.78, m.lowMix * 0.82 + lowRel * 0.18));
        tempoCameraBeats.push({
          time: m.time,
          strength: mtStrength,
          confidence: m.nearest ? Math.max(0.60, m.nearest.confidence || 0) : Math.max(0.52, 0.48 + powerRel * 0.28),
          primary: activeCamera,
          camera: activeCamera,
          pulse: activePulse,
          impact: mtImpact,
          tone: 'music-tempo',
          grooveEvidence: grooveEvidence,
          low: lowForCamera,
          body: m.bodyMix,
          snap: m.snapMix,
          mass: Math.max(0.35, Math.min(0.86, lowForCamera * 0.68 + m.bodyMix * 0.24 + mtStrength * 0.16)),
          sharpness: Math.max(0.08, Math.min(0.65, m.snapMix * 1.18)),
          combo: mtSlot === 0 ? 'downbeat' : (mtSlot === 1 ? 'push' : (mtSlot === 2 ? 'drop' : 'rebound')),
          index: m.index
        });
      }
      if (tempoCameraBeats.length >= 4) {
        if (analysisProfile.sparseCamera) {
          tempoCameraBeats = thinSoftGrooveCameraBeats(tempoCameraBeats, musicTempoGridStep || gridStep, buffer.duration);
        }
        cameraBeats = tempoCameraBeats;
        gridStep = musicTempoGridStep || gridStep;
      }
    }

    var kicks = beats.map(function(b){ return b.time; });
    var visualBeatCount = 0;
    var pulseBeats = cameraBeats.filter(function(b){
      if (typeof b === 'number') {
        visualBeatCount++;
        return true;
      }
      var active = b.primary !== false && b.camera !== false && b.pulse !== false;
      if (active) visualBeatCount++;
      return active && (b.strength >= 0.38 || (b.impact || 0) >= 0.20);
    }).map(function(b){
      if (typeof b === 'number') return { time: b, strength: 0.42, impact: 0.42 };
      return {
        time: b.time,
        strength: b.strength,
        impact: b.impact == null ? b.strength : b.impact,
        combo: b.combo,
        low: b.low,
        body: b.body,
        snap: b.snap
      };
    });
    await yieldToPaint();
    if (token !== beatMapToken) { hideBeatChip(); return null; }
    if (options.prefetch) hideBeatChip();
    else showBeatChip('节奏缓冲中…');
    return { kicks: kicks, beats: beats, pulseBeats: pulseBeats, cameraBeats: cameraBeats, gridStep: gridStep, tempoSource: musicTempoBeats.length >= 4 ? 'music-tempo' : 'local', analysisProfile: analysisProfile.id || 'default', duration: buffer.duration, visualBeatCount: visualBeatCount, analyzedAt: Date.now() };
  } catch (e) {
    console.warn('beat analysis failed:', e);
    hideBeatChip();
    return null;
  } finally {
    beatMapBusy = false;
  }
}

function schedulePodcastDjAnalysis(songKey, audioUrl, token, durationSec) {
  cancelDjBeatAnalysisTimer();
  if (!songKey || !audioUrl) return;
  djBeatAnalysisTimer = setTimeout(function waitForDjStart(){
    djBeatAnalysisTimer = null;
    if (token !== djBeatMapToken || !djMode.active || djMode.songKey !== songKey || djBeatMapCache[songKey]) return;
    var startAnalysis = function(){
      if (token !== djBeatMapToken || !djMode.active || djMode.songKey !== songKey || djBeatMapCache[songKey]) return;
      if (djBeatMapBusy) {
        djBeatAnalysisTimer = setTimeout(waitForDjStart, 900);
        return;
      }
      if (/^https?:\/\//i.test(audioUrl || '') && (durationSec <= 0 || durationSec > 3300)) {
        analyzePodcastDjIntroBeats(audioUrl, token, durationSec).then(function(map){
          if (token !== djBeatMapToken || !map) return;
          smoothPodcastDjIntroHandoff(songKey, map, token);
        }).catch(function(err){
          console.warn('podcast DJ intro beat analysis failed:', err);
        });
      }
      analyzePodcastDjBeats(audioUrl, token, durationSec).then(function(map){
        if (token !== djBeatMapToken || !map) return;
        smoothPodcastDjMapHandoff(songKey, map, token);
      }).catch(function(err){
        console.warn('podcast DJ beat analysis failed:', err);
        hideBeatChip();
      });
    };
    scheduleAnalysisTask(startAnalysis, 900);
  }, 900);
}

async function analyzePodcastDjIntroBeats(audioUrl, token, durationSec) {
  if (!/^https?:\/\//i.test(audioUrl || '')) return null;
  if (token !== djBeatMapToken || !djMode.active) return null;
  var introResp = await fetch('/api/podcast/dj-beatmap?url=' + encodeURIComponent(audioUrl) + '&duration=' + encodeURIComponent(durationSec || 0) + '&intro=180');
  if (token !== djBeatMapToken || !djMode.active) return null;
  var introData = await introResp.json().catch(function(){ return null; });
  if (introResp.ok && introData && introData.ok && introData.map && introData.map.cameraBeats && introData.map.cameraBeats.length >= 4) {
    return introData.map;
  }
  return null;
}

async function buildPodcastDjLowOnlyBeatMap(buffer, token) {
  if (!buffer) return null;
  var sr = buffer.sampleRate || 44100;
  var duration = buffer.duration || (buffer.length / sr) || 0;
  var hopSec = duration > 4200 ? 0.0125 : 0.010;
  var hopSize = Math.max(256, Math.floor(sr * hopSec));
  var nFrames = Math.max(1, Math.floor(buffer.length / hopSize));
  var lowEnergy = new Float32Array(nFrames);
  var hitEnergy = new Float32Array(nFrames);
  var channels = Math.max(1, buffer.numberOfChannels || 1);
  var ch0 = buffer.getChannelData(0);
  var ch1 = channels > 1 ? buffer.getChannelData(1) : null;
  var chList = null;
  if (channels > 2) {
    chList = [];
    for (var ch = 0; ch < channels; ch++) chList.push(buffer.getChannelData(ch));
  }
  function makeBiquad(type, freq, q) {
    freq = Math.max(8, Math.min(freq, sr * 0.45));
    var w0 = 2 * Math.PI * freq / sr;
    var cos = Math.cos(w0);
    var sin = Math.sin(w0);
    var alpha = sin / (2 * (q || 0.707));
    var b0, b1, b2, a0, a1, a2;
    if (type === 'highpass') {
      b0 = (1 + cos) * 0.5;
      b1 = -(1 + cos);
      b2 = (1 + cos) * 0.5;
    } else {
      b0 = (1 - cos) * 0.5;
      b1 = 1 - cos;
      b2 = (1 - cos) * 0.5;
    }
    a0 = 1 + alpha;
    a1 = -2 * cos;
    a2 = 1 - alpha;
    var inv = 1 / a0;
    return { b0:b0 * inv, b1:b1 * inv, b2:b2 * inv, a1:a1 * inv, a2:a2 * inv, x1:0, x2:0, y1:0, y2:0 };
  }
  function runBiquad(st, x) {
    var y = st.b0 * x + st.b1 * st.x1 + st.b2 * st.x2 - st.a1 * st.y1 - st.a2 * st.y2;
    st.x2 = st.x1; st.x1 = x; st.y2 = st.y1; st.y1 = y;
    return y;
  }
  var hp = makeBiquad('highpass', 32, 0.72);
  var lp = makeBiquad('lowpass', 178, 0.82);
  showBeatChip('DJ kick scan 0%');
  for (var f = 0; f < nFrames; f++) {
    var start = f * hopSize;
    var end = Math.min(buffer.length, start + hopSize);
    var sum = 0;
    var peak = 0;
    for (var i = start; i < end; i++) {
      var x;
      if (chList) {
        x = 0;
        for (var ci = 0; ci < channels; ci++) x += chList[ci][i];
        x /= channels;
      } else if (ch1) {
        x = (ch0[i] + ch1[i]) * 0.5;
      } else {
        x = ch0[i];
      }
      var y = runBiquad(lp, runBiquad(hp, x || 0));
      var ay = Math.abs(y);
      sum += y * y;
      if (ay > peak) peak = ay;
    }
    var count = Math.max(1, end - start);
    lowEnergy[f] = Math.sqrt(sum / count);
    hitEnergy[f] = peak;
    if (f > 0 && f % 720 === 0) {
      if (f % 4320 === 0) showBeatChip('DJ kick scan ' + Math.min(99, Math.round(f / nFrames * 100)) + '%');
      await yieldToPaint();
      if (token !== djBeatMapToken || !djMode.active) { hideBeatChip(); return null; }
    }
  }
  if (token !== djBeatMapToken || !djMode.active) { hideBeatChip(); return null; }

  function percentile(arr, p, maxSamples) {
    var len = arr ? arr.length : 0;
    if (!len) return 0.001;
    maxSamples = maxSamples || 14000;
    var sample;
    if (len <= maxSamples) {
      sample = Array.prototype.slice.call(arr);
    } else {
      sample = new Array(maxSamples);
      var step = (len - 1) / (maxSamples - 1);
      for (var si = 0; si < maxSamples; si++) sample[si] = arr[Math.min(len - 1, Math.floor(si * step))] || 0;
    }
    sample.sort(function(a,b){ return a - b; });
    return sample[Math.max(0, Math.min(sample.length - 1, Math.floor(sample.length * p)))] || 0.001;
  }
  function bandAt(arr, idx) {
    idx = Math.max(0, Math.min(nFrames - 1, idx | 0));
    var a = arr[Math.max(0, idx - 1)] || 0;
    var b = arr[idx] || 0;
    var c = arr[Math.min(nFrames - 1, idx + 1)] || 0;
    return (a + b * 2 + c) * 0.25;
  }
  function median(vals) {
    vals = vals.filter(function(v){ return isFinite(v); }).sort(function(a,b){ return a - b; });
    return vals.length ? vals[Math.floor(vals.length * 0.5)] : 0;
  }
  var lowFloor = Math.max(0.0004, percentile(lowEnergy, 0.22));
  var lowMid = Math.max(lowFloor + 0.0002, percentile(lowEnergy, 0.58));
  var lowRef = Math.max(lowMid + 0.0002, percentile(lowEnergy, 0.86));
  var lowCeil = Math.max(lowRef + 0.0004, percentile(lowEnergy, 0.96));
  var hitRef = Math.max(0.0004, percentile(hitEnergy, 0.86));

  showBeatChip('DJ locking kick grid...');
  var onset = new Float32Array(nFrames);
  for (var oi = 4; oi < nFrames; oi++) {
    var prev = lowEnergy[oi - 1] * 0.62 + lowEnergy[oi - 2] * 0.28 + lowEnergy[oi - 3] * 0.10;
    var lowRise = Math.max(0, lowEnergy[oi] - prev);
    var wideRise = Math.max(0, (lowEnergy[oi] + lowEnergy[oi - 1]) * 0.5 - (lowEnergy[oi - 3] + lowEnergy[oi - 4]) * 0.5);
    var peakRise = Math.max(0, hitEnergy[oi] - hitEnergy[oi - 2] * 0.84);
    onset[oi] = lowRise * 1.72 + wideRise * 0.86 + peakRise * 0.10;
  }

  var winN = Math.max(52, Math.round(0.82 / hopSec));
  var minFrameGap = Math.max(18, Math.round(0.215 / hopSec));
  var candidates = [];
  var sumO = 0, sqO = 0;
  for (var wi = 0; wi < winN; wi++) { var ow = onset[wi] || 0; sumO += ow; sqO += ow * ow; }
  for (var cf = winN + 4; cf < nFrames - 4; cf++) {
    var mean = sumO / winN;
    var std = Math.sqrt(Math.max(0, sqO / winN - mean * mean));
    var th = mean + std * 1.66 + lowRef * 0.0038;
    var o = onset[cf];
    if (o > th && o >= onset[cf - 1] && o > onset[cf + 1]) {
      var peakF = cf;
      var peakScore = o + lowEnergy[cf] * 0.10;
      for (var pf = cf - 2; pf <= cf + 3; pf++) {
        var ps = (onset[pf] || 0) + (lowEnergy[pf] || 0) * 0.10;
        if (ps > peakScore) { peakScore = ps; peakF = pf; }
      }
      var lowTone = Math.min(2.6, bandAt(lowEnergy, peakF) / lowRef);
      var hitTone = Math.min(2.6, bandAt(hitEnergy, peakF) / hitRef);
      var lowRel = clamp01((bandAt(lowEnergy, peakF) - lowFloor) / Math.max(0.0001, lowCeil - lowFloor));
      var score = (o - th) / Math.max(0.0006, std + mean * 0.38 + lowRef * 0.012);
      if (score > 0.16 && (lowTone > 0.32 || lowRel > 0.22 || hitTone > 0.52)) {
        var cand = {
          frame: peakF,
          time: peakF * hopSec,
          score: score,
          lowTone: lowTone,
          hitTone: hitTone,
          lowRel: lowRel,
          raw: o
        };
        cand.power = cand.score * 0.56 + Math.pow(clamp01((cand.lowTone - 0.22) / 1.42), 0.82) * 0.34 + Math.min(1.5, cand.hitTone) * 0.08 + cand.lowRel * 0.10;
        var last = candidates[candidates.length - 1];
        if (last && cand.frame - last.frame < minFrameGap) {
          if (cand.power > last.power) candidates[candidates.length - 1] = cand;
        } else {
          candidates.push(cand);
        }
      }
    }
    var old = onset[cf - winN] || 0;
    var next = onset[cf] || 0;
    sumO += next - old;
    sqO += next * next - old * old;
    if (cf > winN && cf % 3600 === 0) {
      await yieldToPaint();
      if (token !== djBeatMapToken || !djMode.active) { hideBeatChip(); return null; }
    }
  }
  if (!candidates.length) {
    return { kicks: [], beats: [], pulseBeats: [], cameraBeats: [], duration: duration, visualBeatCount: 0, tempoSource: 'podcast-dj-low-empty', analyzedAt: Date.now() };
  }

  var powers = candidates.map(function(c){ return c.power; });
  var p30 = percentile(powers, 0.30);
  var p50 = percentile(powers, 0.50);
  var p90 = Math.max(p50 + 0.001, percentile(powers, 0.90));
  var p96 = Math.max(p90 + 0.001, percentile(powers, 0.965));
  var strong = candidates.filter(function(c){ return c.power >= p50 && c.lowTone > 0.34; });
  if (strong.length < 16) strong = candidates.slice();
  function estimateStep(list) {
    if (!list || list.length < 3) return 0;
    var bin = 0.006;
    var hist = {};
    var medGaps = [];
    var minStep = 0.31;
    var maxStep = 0.86;
    for (var ai = 0; ai < list.length; ai++) {
      for (var bi = ai + 1; bi < list.length && bi < ai + 10; bi++) {
        var rawGap = list[bi].time - list[ai].time;
        if (rawGap < 0.24) continue;
        if (rawGap > 2.55) break;
        for (var div = 1; div <= 6; div++) {
          var g = rawGap / div;
          if (g < minStep) break;
          if (g > maxStep) continue;
          var weight = Math.sqrt(Math.max(0.001, list[ai].power * list[bi].power)) / Math.sqrt((bi - ai) * div);
          var key = Math.round(g / bin);
          hist[key] = (hist[key] || 0) + weight;
          medGaps.push(g);
        }
      }
    }
    var bestKey = null, bestScore = 0;
    Object.keys(hist).forEach(function(k){
      var key = parseInt(k, 10);
      var score = (hist[key] || 0) + (hist[key - 1] || 0) * 0.72 + (hist[key + 1] || 0) * 0.72;
      if (score > bestScore) { bestScore = score; bestKey = key; }
    });
    if (bestKey != null) return bestKey * bin;
    return median(medGaps);
  }
  var globalStep = estimateStep(strong) || estimateStep(candidates) || 0.50;
  globalStep = clampRange(globalStep, 0.32, 0.86);

  function nearestCandidate(center, windowSec, startIdx) {
    var best = null;
    var bestScore = -Infinity;
    var j = startIdx || 0;
    while (j < candidates.length && candidates[j].time < center - windowSec) j++;
    for (var ni = j; ni < candidates.length && candidates[ni].time <= center + windowSec; ni++) {
      var dist = Math.abs(candidates[ni].time - center);
      var score = candidates[ni].power * (1 - dist / Math.max(0.001, windowSec) * 0.42);
      if (score > bestScore) { best = candidates[ni]; bestScore = score; }
    }
    return best;
  }
  function scorePhase(anchorTime, step) {
    var start = anchorTime;
    while (start - step > 0.05) start -= step;
    var end = Math.min(duration, 180);
    var win = Math.max(0.055, Math.min(0.125, step * 0.18));
    var score = 0, count = 0, cursor = 0;
    for (var gt = start; gt < end; gt += step) {
      while (cursor < candidates.length && candidates[cursor].time < gt - win) cursor++;
      var best = null, bestScore = 0;
      for (var pi = cursor; pi < candidates.length && candidates[pi].time <= gt + win; pi++) {
        var dist = Math.abs(candidates[pi].time - gt);
        var s = candidates[pi].power * (1 - dist / win * 0.44);
        if (s > bestScore) { bestScore = s; best = candidates[pi]; }
      }
      score += best ? bestScore : -p30 * 0.08;
      count++;
    }
    return count ? score / count : -Infinity;
  }
  var phaseSource = strong.filter(function(c){ return c.time < Math.min(duration, 180); }).slice(0, 72);
  if (!phaseSource.length) phaseSource = strong.slice(0, 1);
  var bestAnchor = phaseSource[0] ? phaseSource[0].time : 0;
  var bestAnchorScore = -Infinity;
  for (var pa = 0; pa < phaseSource.length; pa++) {
    var sc = scorePhase(phaseSource[pa].time, globalStep);
    if (sc > bestAnchorScore) { bestAnchorScore = sc; bestAnchor = phaseSource[pa].time; }
  }
  var halfStep = globalStep * 0.5;
  if (halfStep >= 0.31) {
    var halfScore = scorePhase(bestAnchor, halfStep);
    if (halfScore > bestAnchorScore * 1.04) globalStep = halfStep;
  }
  var anchor = bestAnchor;
  while (anchor - globalStep > 0.05) anchor -= globalStep;

  var sectionLen = duration > 3600 ? 96 : 72;
  var sectionCount = Math.max(1, Math.ceil(duration / sectionLen));
  var sectionSteps = [];
  for (var secIdx = 0; secIdx < sectionCount; secIdx++) {
    var t0 = secIdx * sectionLen, t1 = Math.min(duration, t0 + sectionLen);
    var seg = strong.filter(function(c){ return c.time >= t0 && c.time < t1; });
    var prevStep = sectionSteps.length ? sectionSteps[sectionSteps.length - 1] : globalStep;
    var localStep = estimateStep(seg) || prevStep || globalStep;
    if (prevStep) localStep = clampRange(localStep, prevStep * 0.94, prevStep * 1.06);
    if (globalStep) localStep = clampRange(localStep, globalStep * 0.86, globalStep * 1.14);
    var blended = prevStep ? (localStep * 0.30 + prevStep * 0.70) : localStep;
    sectionSteps.push(blended || globalStep);
  }
  function stepAt(time) {
    var idx = Math.max(0, Math.min(sectionSteps.length - 1, Math.floor(time / sectionLen)));
    return sectionSteps[idx] || globalStep || 0.50;
  }

  var beats = [];
  var gridIndex = 0;
  var cursorIdx = 0;
  for (var gridT = anchor; gridT < duration - 0.04; ) {
    var localStep2 = stepAt(gridT) || globalStep || 0.50;
    var winSec = Math.max(0.060, Math.min(0.135, localStep2 * 0.20));
    while (cursorIdx < candidates.length && candidates[cursorIdx].time < gridT - winSec) cursorIdx++;
    var bestCand = nearestCandidate(gridT, winSec, cursorIdx);
    var gf = Math.max(0, Math.min(nFrames - 1, Math.round(gridT / hopSec)));
    var gridLow = bandAt(lowEnergy, gf);
    var gridHit = bandAt(hitEnergy, gf);
    var gridLowTone = Math.min(2.6, gridLow / lowRef);
    var gridHitTone = Math.min(2.6, gridHit / hitRef);
    var lowTone2 = bestCand ? Math.max(gridLowTone * 0.62, bestCand.lowTone) : gridLowTone;
    var hitTone2 = bestCand ? Math.max(gridHitTone * 0.62, bestCand.hitTone) : gridHitTone;
    var distPenalty = bestCand ? (1 - Math.min(1, Math.abs(bestCand.time - gridT) / winSec) * 0.26) : 0.54;
    var basePower = bestCand ? bestCand.power * distPenalty : (gridLowTone * 0.25 + gridHitTone * 0.06);
    var powerRel = clamp01((basePower - p30 * 0.78) / Math.max(0.001, p96 - p30 * 0.78));
    var lowRel2 = clamp01((gridLow - lowFloor) / Math.max(0.0001, lowCeil - lowFloor));
    var kickRel = clamp01(powerRel * 0.74 + lowRel2 * 0.22 + clamp01((hitTone2 - 0.26) / 1.70) * 0.04);
    var softGrid = (!bestCand && lowRel2 < 0.20) || kickRel < 0.16;
    var slot = gridIndex % 4;
    var combo = slot === 0 ? 'downbeat' : (slot === 1 ? 'push' : (slot === 2 ? 'drop' : 'rebound'));
    if (kickRel > 0.84 && combo !== 'downbeat') combo = 'accent';
    var visualRel = kickRel > 0.76 ? 0.76 + (kickRel - 0.76) * 0.52 : kickRel;
    var downLift = combo === 'downbeat' ? (visualRel > 0.18 ? (0.016 + visualRel * 0.036) : visualRel * 0.028) : 0;
    var sectionGate = clamp01((kickRel - 0.10) / 0.58);
    var impact = Math.max(0.020, Math.min(0.88, 0.022 + Math.pow(visualRel, 1.62) * 0.86 + downLift));
    var strength = Math.max(0.12, Math.min(0.93, 0.13 + Math.pow(visualRel, 1.12) * 0.68 + downLift * 0.70));
    if (softGrid) {
      var softMul = combo === 'downbeat' ? 0.48 : 0.30;
      impact *= softMul;
      strength *= 0.58 + sectionGate * 0.22;
    }
    var timingPull = bestCand ? (0.24 + clamp01((kickRel - 0.25) / 0.65) * 0.46) : 0;
    var sourceTime = bestCand ? (gridT * (1 - timingPull) + bestCand.time * timingPull) : gridT;
    var cameraActive = impact >= 0.13 || (combo === 'downbeat' && kickRel >= 0.14) || (bestCand && kickRel >= 0.18);
    var lowMix = Math.max(0.42, Math.min(0.90, 0.52 + visualRel * 0.32 + lowTone2 * 0.035 - (combo === 'accent' ? 0.10 : 0)));
    var bodyMix = Math.max(0.035, Math.min(0.54, 0.060 + visualRel * 0.12 + (combo === 'push' ? 0.18 : 0) + (combo === 'drop' ? 0.24 : 0)));
    var snapMix = Math.max(0.015, Math.min(0.62, 0.026 + (combo === 'accent' ? 0.40 : 0) + (combo === 'rebound' ? 0.08 : 0) + visualRel * 0.038));
    beats.push({
      time: sourceTime,
      strength: strength,
      confidence: Math.max(0.44, Math.min(0.99, 0.46 + kickRel * 0.43 + (bestCand ? 0.08 : -0.03))),
      impact: impact,
      primary: cameraActive,
      camera: cameraActive,
      pulse: impact > 0.16 || (combo === 'downbeat' && kickRel >= 0.18),
      tone: 'podcast-dj-low-grid',
      low: lowMix,
      body: bodyMix,
      snap: snapMix,
      mass: Math.max(0.36, Math.min(0.94, lowMix * 0.72 + Math.pow(visualRel, 1.22) * 0.24)),
      sharpness: Math.max(0.03, Math.min(0.28, snapMix * 1.18)),
      combo: combo,
      step: localStep2,
      index: beats.length,
      dj: true,
      grid: true,
      kickOnly: true
    });
    gridIndex++;
    gridT += localStep2;
    if (gridIndex > 0 && gridIndex % 1800 === 0) {
      await yieldToPaint();
      if (token !== djBeatMapToken || !djMode.active) { hideBeatChip(); return null; }
    }
  }

  var cameraBeats = beats.filter(function(b){ return b.camera !== false; });
  var pulseBeats = beats.filter(function(b){ return b.pulse !== false && (b.impact >= 0.16 || b.combo === 'downbeat'); }).map(function(b){
    return { time: b.time, strength: b.strength, impact: b.impact, combo: b.combo, low: b.low, body: b.body, snap: b.snap, dj: true };
  });
  console.log('podcast DJ low-only beatmap:', Math.round(duration) + 's', 'step:', globalStep.toFixed(3), 'candidates:', candidates.length, 'beats:', beats.length);
  return {
    kicks: beats.map(function(b){ return b.time; }),
    beats: beats,
    pulseBeats: pulseBeats,
    cameraBeats: cameraBeats,
    gridStep: globalStep,
    sectionSteps: sectionSteps,
    tempoSource: 'podcast-dj-low-offline',
    duration: duration,
    visualBeatCount: cameraBeats.length,
    analyzedAt: Date.now()
  };
}

async function analyzePodcastDjBeats(audioUrl, token, durationSec) {
  try {
    djBeatMapBusy = true;
    showBeatChip('DJ 离线锁拍…');
    await yieldToIdle(520);
    if (token !== djBeatMapToken || !djMode.active) { hideBeatChip(); return null; }
    durationSec = Math.max(0, Number(durationSec) || 0);
    var preferServerAnalysis = /^https?:\/\//i.test(audioUrl || '') && (durationSec <= 0 || durationSec > 3300);
    if (preferServerAnalysis) {
      showBeatChip('DJ 长播客后端锁拍...');
      var serverResp = await fetch('/api/podcast/dj-beatmap?url=' + encodeURIComponent(audioUrl) + '&duration=' + encodeURIComponent(durationSec));
      if (token !== djBeatMapToken || !djMode.active) { hideBeatChip(); return null; }
      var serverData = await serverResp.json().catch(function(){ return null; });
      if (serverResp.ok && serverData && serverData.ok && serverData.map) return serverData.map;
      console.warn('podcast DJ server analysis failed:', serverData && serverData.error);
      hideBeatChip();
      if (durationSec <= 0 || durationSec > 3300) return null;
    }
    var fetchAudioUrl = /^https?:\/\//i.test(audioUrl || '') ? ('/api/audio?url=' + encodeURIComponent(audioUrl)) : audioUrl;
    var resp = await fetch(fetchAudioUrl);
    if (token !== djBeatMapToken || !djMode.active) { hideBeatChip(); return null; }
    var ab = await resp.arrayBuffer();
    if (token !== djBeatMapToken || !djMode.active) { hideBeatChip(); return null; }

    showBeatChip('DJ 解码音频…');
    var TmpCtx = window.OfflineAudioContext || window.webkitOfflineAudioContext;
    var DecodeCtx = window.AudioContext || window.webkitAudioContext;
    if (!DecodeCtx) { hideBeatChip(); return null; }
    var dc = new DecodeCtx();
    var buffer = await new Promise(function(resolve, reject){
      dc.decodeAudioData(ab, resolve, reject);
    }).catch(function(e){ console.warn('podcast DJ decode failed:', e); return null; });
    ab = null;
    dc.close && dc.close();
    if (!buffer || token !== djBeatMapToken || !djMode.active) { hideBeatChip(); return null; }
    return await buildPodcastDjLowOnlyBeatMap(buffer, token);

    var sr = buffer.sampleRate;
    async function renderDjBand(hpFreq, lpFreq, label) {
      showBeatChip('DJ 分离' + label + '…');
      var off = new TmpCtx(1, buffer.length, sr);
      var src = off.createBufferSource();
      src.buffer = buffer;
      var node = src;
      if (hpFreq) {
        var hp = off.createBiquadFilter();
        hp.type = 'highpass';
        hp.frequency.value = Math.min(hpFreq, sr * 0.45);
        hp.Q.value = 0.78;
        node.connect(hp);
        node = hp;
      }
      if (lpFreq) {
        var lp = off.createBiquadFilter();
        lp.type = 'lowpass';
        lp.frequency.value = Math.min(lpFreq, sr * 0.45);
        lp.Q.value = 0.86;
        node.connect(lp);
        node = lp;
      }
      node.connect(off.destination);
      src.start(0);
      var rendered = await off.startRendering();
      if (token !== djBeatMapToken || !djMode.active) return null;
      await yieldToIdle(280);
      return rendered.getChannelData(0);
    }

    var lowPcm = await renderDjBand(34, 170, '低频');
    if (!lowPcm) { hideBeatChip(); return null; }
    var bodyPcm = await renderDjBand(150, 560, '鼓身');
    if (!bodyPcm) { hideBeatChip(); return null; }
    var snapPcm = await renderDjBand(1700, 9200, '高频');
    if (!snapPcm) { hideBeatChip(); return null; }

    var hopSec = 0.012;
    var hopSize = Math.max(256, Math.floor(sr * hopSec));
    async function makeEnergy(pcm, label) {
      showBeatChip('DJ 读取' + label + '…');
      var frames = Math.floor(pcm.length / hopSize);
      var out = new Float32Array(frames);
      for (var f = 0; f < frames; f++) {
        var sum = 0;
        var off2 = f * hopSize;
        for (var i = 0; i < hopSize; i++) {
          var v = pcm[off2 + i] || 0;
          sum += v * v;
        }
        out[f] = Math.sqrt(sum / hopSize);
        if (f > 0 && f % 1800 === 0) {
          await yieldToPaint();
          if (token !== djBeatMapToken || !djMode.active) return null;
        }
      }
      return out;
    }

    var lowEnergy = await makeEnergy(lowPcm, '低频');
    var bodyEnergy = await makeEnergy(bodyPcm, '鼓身');
    var snapEnergy = await makeEnergy(snapPcm, '高频');
    if (!lowEnergy || !bodyEnergy || !snapEnergy || token !== djBeatMapToken || !djMode.active) { hideBeatChip(); return null; }

    var nFrames = Math.min(lowEnergy.length, bodyEnergy.length, snapEnergy.length);
    function percentile(arr, p) {
      var copy = Array.prototype.slice.call(arr).sort(function(a,b){ return a-b; });
      return copy.length ? copy[Math.max(0, Math.min(copy.length - 1, Math.floor(copy.length * p)))] : 0.001;
    }
    function bandAt(arr, f) {
      var a = arr[Math.max(0, f - 1)] || 0;
      var b = arr[f] || 0;
      var c = arr[Math.min(nFrames - 1, f + 1)] || 0;
      return (a + b * 2 + c) * 0.25;
    }
    function median(vals) {
      vals = vals.filter(function(v){ return isFinite(v); }).sort(function(a,b){ return a-b; });
      return vals.length ? vals[Math.floor(vals.length * 0.5)] : 0;
    }
    var lowRef = Math.max(0.0008, percentile(lowEnergy, 0.86));
    var bodyRef = Math.max(0.0008, percentile(bodyEnergy, 0.84));
    var snapRef = Math.max(0.0008, percentile(snapEnergy, 0.84));

    showBeatChip('DJ 计算主拍…');
    var onset = new Float32Array(nFrames);
    for (var oi = 2; oi < nFrames; oi++) {
      var lowRise = Math.max(0, lowEnergy[oi] - lowEnergy[oi - 1]);
      var lowWide = Math.max(0, lowEnergy[oi] - lowEnergy[oi - 2]);
      var bodyRise = Math.max(0, bodyEnergy[oi] - bodyEnergy[oi - 1]);
      var snapRise = Math.max(0, snapEnergy[oi] - snapEnergy[oi - 1]);
      onset[oi] = lowRise * 1.52 + lowWide * 0.58 + bodyRise * 0.16 + snapRise * 0.035;
    }

    var winN = Math.max(44, Math.round(0.78 / hopSec));
    var minFrameGap = Math.max(18, Math.round(0.215 / hopSec));
    var candidates = [];
    var lastFrame = -minFrameGap;
    var sum = 0, sq = 0;
    for (var wi = 0; wi < winN; wi++) { sum += onset[wi] || 0; sq += (onset[wi] || 0) * (onset[wi] || 0); }
    for (var f2 = winN + 1; f2 < nFrames - 2; f2++) {
      var mean = sum / winN;
      var std = Math.sqrt(Math.max(0, sq / winN - mean * mean));
      var th = mean + std * 1.90 + lowRef * 0.006;
      var o = onset[f2];
      if (o > th && o >= onset[f2 - 1] && o > onset[f2 + 1] && f2 - lastFrame >= minFrameGap) {
        var lowTone = Math.min(2.2, bandAt(lowEnergy, f2) / lowRef);
        var bodyTone = Math.min(2.2, bandAt(bodyEnergy, f2) / bodyRef);
        var snapTone = Math.min(2.2, bandAt(snapEnergy, f2) / snapRef);
        var lowDom = lowTone / Math.max(0.001, bodyTone * 0.46 + snapTone * 0.18);
        var score = (o - th) / Math.max(0.0008, std + mean * 0.42);
        var kickLike = lowTone > 0.42 && (lowDom > 0.92 || lowTone > 0.82);
        if (kickLike && score > 0.28) {
          candidates.push({
            frame: f2,
            time: f2 * hopSec,
            score: score,
            lowTone: lowTone,
            bodyTone: bodyTone,
            snapTone: snapTone,
            lowDom: lowDom,
            raw: o
          });
          lastFrame = f2;
        }
      }
      var old = onset[f2 - winN] || 0;
      var next = onset[f2] || 0;
      sum += next - old;
      sq += next * next - old * old;
      if (f2 > winN && f2 % 2200 === 0) {
        await yieldToPaint();
        if (token !== djBeatMapToken || !djMode.active) { hideBeatChip(); return null; }
      }
    }

    if (!candidates.length) {
      hideBeatChip();
      return { kicks: [], beats: [], pulseBeats: [], cameraBeats: [], duration: buffer.duration, visualBeatCount: 0, tempoSource: 'podcast-dj-empty', analyzedAt: Date.now() };
    }

    var strong = candidates.filter(function(c){ return c.score > 0.52 && c.lowTone > 0.52; });
    if (strong.length < 8) strong = candidates.slice();
    var allGaps = [];
    for (var gi = 1; gi < strong.length; gi++) {
      var g = strong[gi].time - strong[gi - 1].time;
      while (g > 0.94) g *= 0.5;
      while (g < 0.30) g *= 2.0;
      if (g >= 0.30 && g <= 0.94) allGaps.push(g);
    }
    var globalStep = median(allGaps) || 0.50;
    var sectionLen = 48;
    var sectionCount = Math.max(1, Math.ceil(buffer.duration / sectionLen));
    var sectionSteps = [];
    for (var si = 0; si < sectionCount; si++) {
      var t0 = si * sectionLen, t1 = t0 + sectionLen;
      var seg = strong.filter(function(c){ return c.time >= t0 && c.time < t1; });
      var gaps = [];
      for (var sg = 1; sg < seg.length; sg++) {
        var gap = seg[sg].time - seg[sg - 1].time;
        while (gap > 0.94) gap *= 0.5;
        while (gap < 0.30) gap *= 2.0;
        if (gap >= 0.30 && gap <= 0.94) gaps.push(gap);
      }
      var prevSectionStep = sectionSteps.length ? sectionSteps[sectionSteps.length - 1] : globalStep;
      var step = median(gaps) || prevSectionStep || globalStep;
      if (globalStep) step = clampRange(step, globalStep * 0.90, globalStep * 1.10);
      if (prevSectionStep && Math.abs(step - prevSectionStep) / prevSectionStep > 0.08) {
        step = step * 0.28 + prevSectionStep * 0.72;
      } else if (prevSectionStep) {
        step = step * 0.42 + prevSectionStep * 0.58;
      }
      sectionSteps.push(step || globalStep);
    }
    function stepAt(time) {
      var idx = Math.max(0, Math.min(sectionSteps.length - 1, Math.floor(time / sectionLen)));
      return sectionSteps[idx] || globalStep || 0.50;
    }

    var powers = candidates.map(function(c){
      c.power = c.score * 0.50 + c.lowTone * 0.26 + Math.min(1.8, c.lowDom) * 0.16 + c.bodyTone * 0.06 + c.snapTone * 0.02;
      return c.power;
    });
    var p35 = percentile(powers, 0.35);
    var p50 = percentile(powers, 0.50);
    var p90 = Math.max(p50 + 0.001, percentile(powers, 0.90));
    var phaseSource = strong.length ? strong : candidates;
    var phaseCandidates = phaseSource.filter(function(c){ return c.time < Math.min(buffer.duration, 120); }).slice(0, 56);
    if (!phaseCandidates.length) phaseCandidates = phaseSource.slice(0, 1);
    function nearestCandidate(center, windowSec, startIdx) {
      var best = null;
      var bestScore = -Infinity;
      var j = startIdx || 0;
      while (j < candidates.length && candidates[j].time < center - windowSec) j++;
      for (var ni = j; ni < candidates.length && candidates[ni].time <= center + windowSec; ni++) {
        var dist = Math.abs(candidates[ni].time - center);
        var score = candidates[ni].power * (1 - dist / Math.max(0.001, windowSec) * 0.48);
        if (score > bestScore) {
          best = candidates[ni];
          bestScore = score;
        }
      }
      return best;
    }
    function scorePhase(anchorTime) {
      var step = globalStep || 0.50;
      var start = anchorTime;
      while (start - step > 0.05) start -= step;
      var end = Math.min(buffer.duration, 132);
      var win = Math.max(0.060, Math.min(0.130, step * 0.18));
      var score = 0, count = 0, cursor = 0;
      for (var gt = start; gt < end; gt += step) {
        while (cursor < candidates.length && candidates[cursor].time < gt - win) cursor++;
        var best = null, bestScore = 0;
        for (var pi = cursor; pi < candidates.length && candidates[pi].time <= gt + win; pi++) {
          var dist = Math.abs(candidates[pi].time - gt);
          var s = candidates[pi].power * (1 - dist / win * 0.45);
          if (s > bestScore) { bestScore = s; best = candidates[pi]; }
        }
        if (best) score += bestScore;
        else score -= p35 * 0.10;
        count++;
      }
      return count ? score / count : -Infinity;
    }
    var bestAnchor = phaseCandidates[0] ? phaseCandidates[0].time : 0;
    var bestAnchorScore = -Infinity;
    for (var pa = 0; pa < phaseCandidates.length; pa++) {
      var sc = scorePhase(phaseCandidates[pa].time);
      if (sc > bestAnchorScore) {
        bestAnchorScore = sc;
        bestAnchor = phaseCandidates[pa].time;
      }
    }
    var anchor = bestAnchor;
    while (anchor - (globalStep || 0.50) > 0.05) anchor -= (globalStep || 0.50);

    var beats = [];
    var gridIndex = 0;
    var cursorIdx = 0;
    for (var gridT = anchor; gridT < buffer.duration - 0.05; ) {
      var localStep = stepAt(gridT) || globalStep || 0.50;
      var winSec = Math.max(0.070, Math.min(0.145, localStep * 0.22));
      while (cursorIdx < candidates.length && candidates[cursorIdx].time < gridT - winSec) cursorIdx++;
      var bestCand = nearestCandidate(gridT, winSec, cursorIdx);
      var gf = Math.max(0, Math.min(nFrames - 1, Math.round(gridT / hopSec)));
      var gridLowTone = Math.min(2.2, bandAt(lowEnergy, gf) / lowRef);
      var gridBodyTone = Math.min(2.2, bandAt(bodyEnergy, gf) / bodyRef);
      var gridSnapTone = Math.min(2.2, bandAt(snapEnergy, gf) / snapRef);
      var sourceTime = bestCand ? (gridT * 0.38 + bestCand.time * 0.62) : gridT;
      var powerBase = bestCand ? bestCand.power : (gridLowTone * 0.22 + gridBodyTone * 0.04 + gridSnapTone * 0.02);
      var distPenalty = bestCand ? (1 - Math.min(1, Math.abs(bestCand.time - gridT) / winSec) * 0.30) : 0.58;
      var powerRel = clamp01(((powerBase * distPenalty) - p35 * 0.78) / Math.max(0.001, p90 - p35 * 0.78));
      var lowTone2 = bestCand ? Math.max(gridLowTone * 0.55, bestCand.lowTone) : gridLowTone;
      var bodyTone2 = bestCand ? Math.max(gridBodyTone * 0.50, bestCand.bodyTone) : gridBodyTone;
      var snapTone2 = bestCand ? Math.max(gridSnapTone * 0.50, bestCand.snapTone) : gridSnapTone;
      var toneTotal = Math.max(0.001, lowTone2 + bodyTone2 * 0.72 + snapTone2 * 0.48);
      var lowMix = lowTone2 / toneTotal;
      var bodyMix = (bodyTone2 * 0.72) / toneTotal;
      var snapMix = (snapTone2 * 0.48) / toneTotal;
      var comboSlot = gridIndex % 4;
      var combo = comboSlot === 0 ? 'downbeat' : (comboSlot === 1 ? 'push' : (comboSlot === 2 ? 'drop' : 'rebound'));
      if (powerRel > 0.86 && combo !== 'downbeat') combo = 'accent';
      var weakGrid = !bestCand && gridLowTone < 0.50 && powerRel < 0.24;
      if (!weakGrid || comboSlot === 0 || powerRel > 0.18) {
        var downLift = combo === 'downbeat' ? 0.06 : 0;
        var strength = Math.max(0.18, Math.min(0.94, 0.20 + Math.pow(powerRel, 1.22) * 0.54 + lowMix * 0.08 + downLift));
        var impact = Math.max(0.10, Math.min(0.96, Math.pow(powerRel, 1.36) * 0.82 + lowMix * 0.12 + downLift));
        beats.push({
          time: sourceTime,
          strength: strength,
          confidence: Math.max(0.46, Math.min(0.98, 0.50 + powerRel * 0.38 + lowMix * 0.10 - (bestCand ? 0 : 0.10))),
          impact: impact,
          primary: true,
          camera: true,
          pulse: impact > 0.18 || combo === 'downbeat',
          tone: 'podcast-dj-grid',
          low: Math.max(0.24, Math.min(0.90, lowMix * 0.78 + powerRel * 0.18)),
          body: Math.max(0.03, Math.min(0.60, bodyMix)),
          snap: Math.max(0.02, Math.min(0.50, snapMix)),
          mass: Math.max(0.28, Math.min(0.96, lowMix * 0.74 + Math.pow(powerRel, 1.25) * 0.24)),
          sharpness: Math.max(0.03, Math.min(0.62, snapMix * 1.10)),
          combo: combo,
          step: localStep,
          index: beats.length,
          dj: true,
          grid: true
        });
      }
      gridIndex++;
      gridT += localStep;
      if (gridIndex > 0 && gridIndex % 1800 === 0) {
        await yieldToPaint();
        if (token !== djBeatMapToken || !djMode.active) { hideBeatChip(); return null; }
      }
    }

    var pulseBeats = beats.filter(function(b){ return b.strength >= 0.38 || b.combo === 'downbeat'; }).map(function(b){
      return { time: b.time, strength: b.strength, impact: b.impact, combo: b.combo, low: b.low, body: b.body, snap: b.snap, dj: true };
    });
    await yieldToPaint();
    if (token !== djBeatMapToken || !djMode.active) { hideBeatChip(); return null; }
    return {
      kicks: beats.map(function(b){ return b.time; }),
      beats: beats,
      pulseBeats: pulseBeats,
      cameraBeats: beats,
      gridStep: globalStep,
      sectionSteps: sectionSteps,
      tempoSource: 'podcast-dj-offline',
      duration: buffer.duration,
      visualBeatCount: beats.length,
      analyzedAt: Date.now()
    };
  } catch (err) {
    console.warn('podcast DJ analysis failed:', err);
    hideBeatChip();
    return null;
  } finally {
    djBeatMapBusy = false;
  }
}

function applyPodcastDjProfileFromMap(map) {
  if (!map || !djMode.active) return;
  var density = (map.cameraBeats || []).length / Math.max(20, map.duration || 20);
  cinemaTrackProfile.density = density;
  var target = 0.82 + clamp01((density - 1.25) / 1.8) * 0.16;
  target = clampRange(target, 0.76, 1.10);
  cinemaTrackProfile.target = target;
  cinemaTrackProfile.scale += (target - cinemaTrackProfile.scale) * 0.34;
}

function smoothPodcastDjMapHandoff(songKey, map, token) {
  if (!map) return;
  showBeatChip('DJ 锁拍完成…');
  var apply = function() {
    if (token !== djBeatMapToken || !djMode.active || djMode.songKey !== songKey) return;
    djBeatMapCache[songKey] = map;
    currentDjBeatMap = map;
    applyPodcastDjProfileFromMap(map);
    syncPodcastDjMapCursor(audio ? audio.currentTime : 0, true);
    notifyDesktopLyricsBeatMapReady();
    hideBeatChip();
    showToast('DJ 离线锁拍完成: ' + (map.visualBeatCount || 0) + ' 个主拍');
  };
  scheduleVisualApply(apply, 260, 360);
}

function smoothPodcastDjIntroHandoff(songKey, map, token) {
  if (!map || !map.partial) return;
  if (currentDjBeatMap && !currentDjBeatMap.partial) return;
  var apply = function() {
    if (token !== djBeatMapToken || !djMode.active || djMode.songKey !== songKey) return;
    if (currentDjBeatMap && !currentDjBeatMap.partial) return;
    currentDjBeatMap = map;
    applyPodcastDjProfileFromMap(map);
    syncPodcastDjMapCursor(audio ? audio.currentTime : 0, true);
    notifyDesktopLyricsBeatMapReady();
    showBeatChip('DJ 开头已锁拍，全曲继续分析…');
  };
  scheduleVisualApply(apply, 0, 240);
}

function showBeatChip(text) {
  byId('beat-text').textContent = text || '分析节奏…';
  byId('beat-chip').classList.add('show');
  if (localBeatAnalysis && localBeatAnalysis.active) setLocalBeatStatus(text || '分析中...', 'warn');
}
function hideBeatChip() {
  byId('beat-chip').classList.remove('show');
}

function localBeatRound(v, scale) {
  v = Number(v);
  if (!isFinite(v)) return 0;
  scale = scale || 1000;
  return Math.round(v * scale) / scale;
}
function packLocalBeatEvent(ev) {
  if (typeof ev === 'number') return [localBeatRound(ev, 1000), 0.42, 0.72, 0.42, 0.62, 0.22, 0.16, 0, 7, 0.62, 0.12, 0];
  ev = ev || {};
  var comboIdx = Math.max(0, LOCAL_BEAT_COMBOS.indexOf(ev.combo || ''));
  var flags = 0;
  if (ev.primary !== false) flags |= 1;
  if (ev.camera !== false) flags |= 2;
  if (ev.pulse !== false) flags |= 4;
  if (ev.dj) flags |= 8;
  if (ev.grid) flags |= 16;
  if (ev.kickOnly) flags |= 32;
  return [
    localBeatRound(ev.time, 1000),
    localBeatRound(ev.strength == null ? 0.42 : ev.strength, 1000),
    localBeatRound(ev.confidence == null ? 0.72 : ev.confidence, 1000),
    localBeatRound(ev.impact == null ? (ev.strength == null ? 0.42 : ev.strength) : ev.impact, 1000),
    localBeatRound(ev.low == null ? 0.62 : ev.low, 1000),
    localBeatRound(ev.body == null ? 0.22 : ev.body, 1000),
    localBeatRound(ev.snap == null ? 0.16 : ev.snap, 1000),
    comboIdx,
    flags,
    localBeatRound(ev.mass == null ? 0.62 : ev.mass, 1000),
    localBeatRound(ev.sharpness == null ? 0.12 : ev.sharpness, 1000),
    localBeatRound(ev.step || 0, 1000)
  ];
}
function unpackLocalBeatEvent(row) {
  if (typeof row === 'number') return row;
  if (!Array.isArray(row)) return row;
  var flags = row[8] || 0;
  return {
    time: row[0] || 0,
    strength: row[1] == null ? 0.42 : row[1],
    confidence: row[2] == null ? 0.72 : row[2],
    impact: row[3] == null ? (row[1] || 0.42) : row[3],
    low: row[4] == null ? 0.62 : row[4],
    body: row[5] == null ? 0.22 : row[5],
    snap: row[6] == null ? 0.16 : row[6],
    combo: LOCAL_BEAT_COMBOS[row[7] || 0] || undefined,
    primary: !!(flags & 1),
    camera: !!(flags & 2),
    pulse: !!(flags & 4),
    dj: !!(flags & 8),
    grid: !!(flags & 16),
    kickOnly: !!(flags & 32),
    mass: row[9] == null ? 0.62 : row[9],
    sharpness: row[10] == null ? 0.12 : row[10],
    step: row[11] || 0
  };
}
function packLocalBeatMap(map) {
  if (!map) return null;
  var camera = (map.cameraBeats || map.beats || map.kicks || []).map(packLocalBeatEvent);
  var pulse = (map.pulseBeats || map.kicks || []).map(packLocalBeatEvent);
  return {
    v: 1,
    duration: localBeatRound(map.duration || 0, 1000),
    gridStep: localBeatRound(map.gridStep || 0, 1000),
    sectionSteps: (map.sectionSteps || []).map(function(v){ return localBeatRound(v, 1000); }),
    tempoSource: map.tempoSource || 'local',
    visualBeatCount: map.visualBeatCount || camera.length,
    analyzedAt: map.analyzedAt || Date.now(),
    partial: !!map.partial,
    partialUntilSec: map.partialUntilSec || 0,
    cameraBeats: camera,
    pulseBeats: pulse
  };
}
function unpackLocalBeatMap(stored) {
  if (!stored) return null;
  if (stored.v && stored.v !== 1 && stored.v !== 2) return stored;
  var camera = (stored.cameraBeats || []).map(unpackLocalBeatEvent);
  var pulse = (stored.pulseBeats || []).map(unpackLocalBeatEvent);
  return {
    kicks: camera.map(function(b){ return typeof b === 'number' ? b : b.time; }),
    beats: camera,
    pulseBeats: pulse,
    cameraBeats: camera,
    gridStep: stored.gridStep || 0,
    sectionSteps: stored.sectionSteps || [],
    tempoSource: stored.tempoSource || 'local',
    duration: stored.duration || 0,
    visualBeatCount: stored.visualBeatCount || camera.length,
    analyzedAt: stored.analyzedAt || Date.now(),
    partial: !!stored.partial,
    partialUntilSec: stored.partialUntilSec || 0
  };
}
function readLocalBeatPrefs() {
  try { return JSON.parse(localStorage.getItem(LOCAL_BEAT_PREF_STORE_KEY) || '{}') || {}; }
  catch (e) { return {}; }
}
function saveLocalBeatPrefs() {
  try { localStorage.setItem(LOCAL_BEAT_PREF_STORE_KEY, JSON.stringify(localBeatMapPrefs || {})); } catch (e) {}
}
function readLocalBeatMapCache() {
  var out = {};
  try {
    var raw = JSON.parse(localStorage.getItem(LOCAL_BEATMAP_STORE_KEY) || '{}') || {};
    Object.keys(raw).forEach(function(key){
      var entry = raw[key] || {};
      out[key] = { updatedAt: entry.updatedAt || 0 };
      if (entry.mr) out[key].mr = unpackLocalBeatMap(entry.mr);
      if (entry.dj) out[key].dj = unpackLocalBeatMap(entry.dj);
    });
  } catch (e) {
    out = {};
  }
  return out;
}
function packLocalBeatCache(maxEntries) {
  var entries = Object.keys(localBeatMapCache || {}).map(function(key){
    var entry = localBeatMapCache[key] || {};
    return { key:key, updatedAt: entry.updatedAt || 0, entry:entry };
  }).sort(function(a,b){ return b.updatedAt - a.updatedAt; });
  if (maxEntries) entries = entries.slice(0, maxEntries);
  var packed = {};
  entries.forEach(function(item){
    packed[item.key] = { updatedAt: item.entry.updatedAt || Date.now() };
    if (item.entry.mr) packed[item.key].mr = packLocalBeatMap(item.entry.mr);
    if (item.entry.dj) packed[item.key].dj = packLocalBeatMap(item.entry.dj);
  });
  return packed;
}
function saveLocalBeatMapCache() {
  var attempts = [12, 8, 5, 3];
  for (var i = 0; i < attempts.length; i++) {
    try {
      localStorage.setItem(LOCAL_BEATMAP_STORE_KEY, JSON.stringify(packLocalBeatCache(attempts[i])));
      return true;
    } catch (e) {}
  }
  return false;
}
function getLocalBeatEntry(localKey, mode) {
  var entry = localKey && localBeatMapCache ? localBeatMapCache[localKey] : null;
  return entry && entry[mode] ? entry[mode] : null;
}
function storeLocalBeatEntry(localKey, mode, map, song, opts) {
  if (!localKey || !map) return;
  opts = opts || {};
  var entry = localBeatMapCache[localKey] || {};
  entry[mode] = map;
  entry.updatedAt = Date.now();
  localBeatMapCache[localKey] = entry;
  localBeatMapPrefs[localKey] = mode;
  saveLocalBeatPrefs();
  saveLocalBeatMapCache();
  if (!opts.skipDisk) writeBeatDiskCache(localBeatDiskKey(localKey, mode), map, song || { type:'local', localKey:localKey }, mode);
}
function setLocalBeatStatus(text, tone) {
  var el = byId('local-beat-status');
  if (!el) return;
  el.textContent = text || '';
  el.classList.toggle('warn', tone === 'warn');
  el.classList.toggle('fail', tone === 'fail');
}

function localBeatVisualCount(map) {
  return map ? (map.visualBeatCount || (map.cameraBeats && map.cameraBeats.length) || (map.beats && map.beats.length) || 0) : 0;
}
function setLocalBeatPreference(localKey, mode) {
  if (!localKey) return;
  localBeatMapPrefs[localKey] = mode === 'dj' ? 'dj' : 'mr';
  saveLocalBeatPrefs();
}
function applyLocalBeatMap(song, mode, map, fromCache) {
  if (!song || !song.localKey || !map) return false;
  mode = mode === 'dj' ? 'dj' : 'mr';
  song.localBeatMode = mode;
  setLocalBeatPreference(song.localKey, mode);
  if (mode === 'dj') {
    setDjModeActive(true, song);
    currentBeatMap = null;
    beatMapNextIdx = 0;
    currentDjBeatMap = map;
    djBeatMapCache[djSongKey(song)] = map;
    applyPodcastDjProfileFromMap(map);
    syncPodcastDjMapCursor(audio ? audio.currentTime : 0, true);
    maybeAnnounceDjMode();
  } else {
    setDjModeActive(false, song);
    currentBeatMap = map;
    beatMapCache['local:' + song.localKey] = map;
    applyCinemaProfileFromBeatMap(map);
    syncBeatMapPlaybackCursor(audio ? audio.currentTime : 0, true);
  }
  hideBeatChip();
  notifyDesktopLyricsBeatMapReady();
  if (fromCache) showToast((mode === 'dj' ? 'DJ' : 'MR') + ' 本地节奏缓存已载入');
  return true;
}
function prepareLocalBeatAnalysis(song, audioUrl) {
  if (!song || !song.localKey || !audioUrl) return;
  var preferred = localBeatMapPrefs[song.localKey] === 'dj' ? 'dj' : 'mr';
  var cached = getLocalBeatEntry(song.localKey, preferred) ||
    getLocalBeatEntry(song.localKey, preferred === 'dj' ? 'mr' : 'dj');
  if (cached) {
    applyLocalBeatMap(song, cached === getLocalBeatEntry(song.localKey, 'dj') ? 'dj' : 'mr', cached, true);
    return;
  }
  var diskToken = trackSwitchToken;
  (async function(){
    var firstMode = preferred;
    var secondMode = preferred === 'dj' ? 'mr' : 'dj';
    var firstMap = await readBeatDiskCache(localBeatDiskKey(song.localKey, firstMode));
    var mode = firstMap ? firstMode : secondMode;
    var map = firstMap || await readBeatDiskCache(localBeatDiskKey(song.localKey, secondMode));
    if (diskToken !== trackSwitchToken || !currentLocalSong || currentLocalSong.localKey !== song.localKey) return;
    if (map) {
      storeLocalBeatEntry(song.localKey, mode, map, song, { skipDisk:true });
      applyLocalBeatMap(song, mode, map, true);
      return;
    }
    openLocalBeatModal(song, audioUrl);
  })().catch(function(){
    if (diskToken === trackSwitchToken && currentLocalSong && currentLocalSong.localKey === song.localKey) openLocalBeatModal(song, audioUrl);
  });
}
function openLocalBeatModal(song, audioUrl) {
  if (immersiveMode) setImmersiveMode(false);
  localBeatAnalysis.song = song || currentLocalSong;
  localBeatAnalysis.audioUrl = audioUrl || (audio && audio.src) || '';
  localBeatAnalysis.mode = (localBeatAnalysis.song && localBeatMapPrefs[localBeatAnalysis.song.localKey] === 'dj') ? 'dj' : 'mr';
  localBeatAnalysis.active = false;
  setLocalBeatStatus('', '');
  updateLocalBeatModal();
  openGsapModal(byId('local-beat-modal'));
}
function closeLocalBeatModal() {
  if (localBeatAnalysis.active) return;
  closeGsapModal(byId('local-beat-modal'));
}
function selectLocalBeatMode(mode) {
  if (localBeatAnalysis.active) return;
  localBeatAnalysis.mode = mode === 'dj' ? 'dj' : 'mr';
  updateLocalBeatModal();
}
function updateLocalBeatModal() {
  var song = localBeatAnalysis.song || currentLocalSong || {};
  var mode = localBeatAnalysis.mode === 'dj' ? 'dj' : 'mr';
  var modal = document.querySelector('#local-beat-modal .local-beat-modal');
  if (modal) modal.classList.toggle('analyzing', !!localBeatAnalysis.active);
  var title = byId('local-beat-title');
  var sub = byId('local-beat-sub');
  if (title) title.textContent = song.name || '本地歌曲';
  if (sub) {
    var cachedBits = [];
    if (song.localKey && getLocalBeatEntry(song.localKey, 'mr')) cachedBits.push('MR 已缓存');
    if (song.localKey && getLocalBeatEntry(song.localKey, 'dj')) cachedBits.push('DJ 已缓存');
    sub.textContent = cachedBits.length ? cachedBits.join(' / ') : '选择一种电影视角分析方式';
  }
  var mr = byId('local-beat-tab-mr');
  var dj = byId('local-beat-tab-dj');
  if (mr) mr.classList.toggle('active', mode === 'mr');
  if (dj) dj.classList.toggle('active', mode === 'dj');
  var desc = byId('local-beat-desc');
  if (desc) desc.textContent = mode === 'dj'
    ? '适合 DJ、长混音或鼓点密集的本地音频，会使用更稳定的低频锁拍并进入 DJ 视觉驱动。'
    : '适合普通歌曲和日常播放，会沿用 Mineradio 电影视角的综合节奏分析。';
  var start = byId('local-beat-start-btn');
  var cancel = byId('local-beat-cancel-btn');
  var later = byId('local-beat-later-btn');
  if (start) {
    start.disabled = !!localBeatAnalysis.active;
    start.textContent = getLocalBeatEntry(song.localKey, mode) ? '使用缓存' : '开始分析';
  }
  if (cancel) cancel.style.display = localBeatAnalysis.active ? '' : 'none';
  if (later) later.style.display = localBeatAnalysis.active ? 'none' : '';
}
function cancelLocalBeatAnalysis() {
  if (!localBeatAnalysis.active) {
    closeLocalBeatModal();
    return;
  }
  localBeatAnalysis.active = false;
  localBeatAnalysis.token++;
  beatMapToken++;
  djBeatMapToken++;
  beatMapBusy = false;
  djBeatMapBusy = false;
  cancelBeatAnalysisTimer();
  cancelDjBeatAnalysisTimer();
  hideBeatChip();
  if (localBeatAnalysis.mode === 'dj') setDjModeActive(false, localBeatAnalysis.song || currentLocalSong);
  setLocalBeatStatus('已取消分析', 'fail');
  updateLocalBeatModal();
}
async function startLocalBeatAnalysis(mode) {
  var song = localBeatAnalysis.song || currentLocalSong;
  var audioUrl = localBeatAnalysis.audioUrl || (song && song.localUrl) || (audio && audio.src) || '';
  mode = mode || localBeatAnalysis.mode;
  mode = mode === 'dj' ? 'dj' : 'mr';
  if (!song || !song.localKey || !audioUrl || localBeatAnalysis.active) return;
  var cached = getLocalBeatEntry(song.localKey, mode);
  if (cached) {
    applyLocalBeatMap(song, mode, cached, true);
    closeGsapModal(byId('local-beat-modal'));
    return;
  }
  localBeatAnalysis.active = true;
  localBeatAnalysis.mode = mode;
  localBeatAnalysis.token++;
  var localToken = localBeatAnalysis.token;
  updateLocalBeatModal();
  setLocalBeatStatus((mode === 'dj' ? 'DJ' : 'MR') + ' 分析准备中...', 'warn');
  try {
    var map = null;
    if (mode === 'dj') {
      setDjModeActive(true, song);
      djBeatMapToken++;
      resetDjBeatMapState();
      currentBeatMap = null;
      resetBeatCameraSync(audio ? audio.currentTime : 0);
      var djToken = djBeatMapToken;
      map = await analyzePodcastDjBeats(audioUrl, djToken, audio && isFinite(audio.duration) ? audio.duration : 0);
      if (localToken !== localBeatAnalysis.token || djToken !== djBeatMapToken) return;
      if (!map) throw new Error('DJ analysis returned empty map');
    } else {
      setDjModeActive(false, song);
      beatMapToken++;
      currentBeatMap = null;
      beatMapNextIdx = 0;
      resetBeatCameraSync(audio ? audio.currentTime : 0);
      var mrToken = beatMapToken;
      map = await analyzeAudioBeats(audioUrl, audio && isFinite(audio.duration) ? audio.duration : 0, mrToken, { background:false, song: song });
      if (localToken !== localBeatAnalysis.token || mrToken !== beatMapToken) return;
      if (!map) throw new Error('MR analysis returned empty map');
    }
    storeLocalBeatEntry(song.localKey, mode, map, song);
    applyLocalBeatMap(song, mode, map, false);
    localBeatAnalysis.active = false;
    setLocalBeatStatus((mode === 'dj' ? 'DJ' : 'MR') + ' 分析完成: ' + localBeatVisualCount(map) + ' 个主拍');
    updateLocalBeatModal();
    showToast((mode === 'dj' ? 'DJ' : 'MR') + ' 本地节奏分析完成');
    setTimeout(function(){
      if (!localBeatAnalysis.active) closeGsapModal(byId('local-beat-modal'));
    }, 900);
  } catch (err) {
    console.warn('local beat analysis failed:', err);
    localBeatAnalysis.active = false;
    hideBeatChip();
    if (mode === 'dj') setDjModeActive(false, song);
    setLocalBeatStatus('分析失败，请换另一种模式重试', 'fail');
    updateLocalBeatModal();
    showToast('本地节奏分析失败');
  }
}

function smoothBeatMapHandoff(songId, map, token, song) {
  if (!map) return;
  showBeatChip('节奏缓冲中…');
  var wait = Math.max(260, Math.min(720, 340 + (beatPulse + beatCam.punch) * 260));
  var apply = function() {
    if (token !== beatMapToken) return;
    beatMapCache[songId] = map;
    currentBeatMap = map;
    applyCinemaProfileFromBeatMap(map);
    var t = audio ? audio.currentTime : 0;
    syncBeatMapPlaybackCursor(t, true);
    hideBeatChip();
    notifyDesktopLyricsBeatMapReady();
    showToast('节奏分析完成: ' + (map.visualBeatCount || (map.cameraBeats && map.cameraBeats.length) || 0) + ' 个视觉主拍');
    writeBeatDiskCache(songId, map, song, 'mr');
    scheduleQueueBeatPrefetch(currentIdx, 1000);
  };
  scheduleVisualApply(apply, wait, 460);
}

function applyBeatMapCacheForCurrent(songId, map, token, message) {
  if (!songId || !map || token !== beatMapToken) return false;
  beatMapCache[songId] = map;
  currentBeatMap = map;
  applyCinemaProfileFromBeatMap(map);
  syncBeatMapPlaybackCursor(audio ? audio.currentTime : 0, true);
  hideBeatChip();
  notifyDesktopLyricsBeatMapReady();
  if (message) console.log(message, songId, map.visualBeatCount || 0);
  scheduleQueueBeatPrefetch(currentIdx, 1000);
  return true;
}

// 每帧调用 — 按 beatMap 触发预演鼓点
function syncBeatMapPlaybackCursor(t, preserveVisualState) {
  if (djMode.active) {
    syncPodcastDjMapCursor(t, preserveVisualState);
    return;
  }
  t = isFinite(t) ? t : 0;
  beatMapNextIdx = 0;
  var pulseEvents = currentBeatMap && (currentBeatMap.pulseBeats || currentBeatMap.kicks);
  if (pulseEvents) {
    while (beatMapNextIdx < pulseEvents.length && beatEventTime(pulseEvents[beatMapNextIdx]) < t) beatMapNextIdx++;
  }
  if (preserveVisualState) alignBeatCameraCursorToTime(t);
  else syncBeatCameraToTime(t);
}

function syncPodcastDjMapCursor(t, preserveVisualState) {
  t = isFinite(t) ? t : 0;
  djBeatMapNextIdx = 0;
  djBeatPulseNextIdx = 0;
  if (currentDjBeatMap) {
    var beatEvents = currentDjBeatMap.cameraBeats || currentDjBeatMap.beats || currentDjBeatMap.kicks || [];
    var camSyncTime = Math.max(0, t - 0.025);
    while (djBeatMapNextIdx < beatEvents.length && beatEventTime(beatEvents[djBeatMapNextIdx]) < camSyncTime) djBeatMapNextIdx++;
    var pulseEvents = currentDjBeatMap.pulseBeats || currentDjBeatMap.kicks || [];
    var pulseSyncTime = Math.max(0, t - 0.035);
    while (djBeatPulseNextIdx < pulseEvents.length && beatEventTime(pulseEvents[djBeatPulseNextIdx]) < pulseSyncTime) djBeatPulseNextIdx++;
  }
  if (!preserveVisualState) resetBeatCameraSync(t);
}

function tickPodcastDjBeatMap() {
  if (!djMode.active || !currentDjBeatMap || !audio || audio.paused) return;
  var t = audio.currentTime || 0;
  if (currentDjBeatMap.partialUntilSec && t > currentDjBeatMap.partialUntilSec + beatCam.lookahead) return;
  var beatEvents = currentDjBeatMap.cameraBeats || currentDjBeatMap.beats || currentDjBeatMap.kicks || [];
  var pulseEvents = currentDjBeatMap.pulseBeats || currentDjBeatMap.kicks || [];
  while (djBeatMapNextIdx < beatEvents.length) {
    var beat = beatEvents[djBeatMapNextIdx];
    var beatTime = beatEventTime(beat);
    if (beatTime > t + beatCam.lookahead) break;
    scheduleBeatCamera(beat, 'djmap');
    djBeatMapNextIdx++;
  }
  while (djBeatPulseNextIdx < pulseEvents.length && beatEventTime(pulseEvents[djBeatPulseNextIdx]) <= t) {
    triggerScheduledBeat(pulseEvents[djBeatPulseNextIdx]);
    djBeatPulseNextIdx++;
  }
}

function tickBeatMap() {
  if (djMode.active) return;
  if (!currentBeatMap || !audio || audio.paused) return;
  var t = audio.currentTime;
  var beatEvents = currentBeatMap.cameraBeats || currentBeatMap.beats || currentBeatMap.kicks || [];
  var pulseEvents = currentBeatMap.pulseBeats || currentBeatMap.kicks || [];
  var gridTimingLocked = currentBeatMap.tempoSource === 'music-tempo' && beatEvents.length >= 4;
  var liveFreshWindow = Math.max(0.50, rtBeat.tempoGap ? rtBeat.tempoGap * 1.18 : 0.50);
  var realtimeHasLock = rtBeat.lastHitAt > 0 && (t - rtBeat.lastHitAt) < liveFreshWindow;
  while (beatCam.nextIdx < beatEvents.length) {
    var beat = beatEvents[beatCam.nextIdx];
    var beatTime = typeof beat === 'number' ? beat : beat.time;
    if (beatTime > t + beatCam.lookahead) break;
    if (gridTimingLocked || !realtimeHasLock) scheduleBeatCamera(beat, 'map');
    beatCam.nextIdx++;
  }
  while (beatMapNextIdx < pulseEvents.length && beatEventTime(pulseEvents[beatMapNextIdx]) <= t) {
    // 触发预演冲击
    if (gridTimingLocked || !realtimeHasLock) triggerScheduledBeat(pulseEvents[beatMapNextIdx]);
    beatMapNextIdx++;
  }
}

function triggerScheduledBeat(beat) {
  var strength = typeof beat === 'number' ? 0.42 : Math.max(0, Math.min(1, beat && beat.strength != null ? beat.strength : 0.42));
  var impact = typeof beat === 'number' ? strength : Math.max(0, Math.min(1, beat && beat.impact != null ? beat.impact : strength));
  if (impact < 0.18 && strength < 0.52) return;
  if ((cinemaTrackProfile.scale || 1) < 0.52 && impact < 0.46 && strength < 0.74) return;
  var body = typeof beat === 'number' ? 0 : Math.max(0, Math.min(1, beat && beat.body != null ? beat.body : 0));
  var combo = typeof beat === 'number' ? null : beat && beat.combo;
  var comboLift = combo === 'downbeat' ? 0.08 : (combo === 'drop' ? 0.04 : 0);
  var dynScale = cameraDynamicsScale(0.88 + impact * 0.16);
  var djPulse = beat && beat.dj;
  var pulse = (0.14 + strength * 0.46 + impact * 0.18 + body * 0.08 + comboLift) * dynScale;
  if (djPulse) pulse = (0.12 + strength * 0.50 + impact * 0.28 + comboLift * 0.70) * clampRange(dynScale, 0.78, 1.18);
  pulse = Math.min(djPulse ? 0.92 : 0.78, pulse);
  scheduledBeatPulse = Math.max(scheduledBeatPulse, pulse);
  scheduledBeatFlag = true;
}
var scheduledBeatPulse = 0;
var scheduledBeatFlag = false;

function showAIDepthChip(text) {
  byId('ai-depth-text').textContent = text || 'AI 深度估计…';
  byId('ai-depth-chip').classList.add('show');
}
function hideAIDepthChip() {
  byId('ai-depth-chip').classList.remove('show');
}

function loadCoverFromUrl(directUrl, opts) {
  opts = opts || {};
  if (!directUrl || typeof directUrl !== 'string' || !/^(https?|data|blob|file):/i.test(directUrl)) {
    if (!coverApplyStillCurrent(opts)) return;
    currentCoverSource = null;
    coverProcessToken++;
    uniforms.uHasCover.value = 0; setCoverDepthState(0, 0, 1);
    resetFloatColorsToIdle();
    byId('album-bg').classList.remove('visible');
    byId('thumb-cover').removeAttribute('src');
    setControlCoverSrc('');
    return;
  }
  byId('album-bg').style.backgroundImage = "url(" + directUrl + ")";
  byId('album-bg').classList.add('visible');
  var proxiedUrl = coverProxySrc(directUrl);
  if (!proxiedUrl) {
    uniforms.uHasCover.value = 0; setCoverDepthState(0, 0, 1);
    resetFloatColorsToIdle();
    setControlCoverSrc('');
    return;
  }
  var img = new Image(); img.crossOrigin = 'anonymous'; img.decoding = 'async';
  img.onload = function() {
    if (!coverApplyStillCurrent(opts)) return;
    var size = coverTextureSizeForResolution(fx.coverResolution);
    var cv = document.createElement('canvas'); cv.width = cv.height = size;
    var cx = cv.getContext('2d');
    var iw = img.naturalWidth, ih = img.naturalHeight, s = Math.min(iw, ih);
    cx.drawImage(img, (iw-s)/2, (ih-s)/2, s, s, 0, 0, size, size);
    applyCoverCanvas(cv, proxiedUrl || directUrl, Object.assign({}, opts, { coverKey: directUrl || proxiedUrl || '', coverSourceKind: 'url', coverSource: directUrl }));
  };
  img.onerror = function() {
    var img2 = new Image(); img2.crossOrigin = 'anonymous'; img2.decoding = 'async';
    img2.onload = function() {
      if (!coverApplyStillCurrent(opts)) return;
      var size = coverTextureSizeForResolution(fx.coverResolution);
      var cv = document.createElement('canvas'); cv.width = cv.height = size;
      cv.getContext('2d').drawImage(img2, 0, 0, size, size);
      applyCoverCanvas(cv, directUrl, Object.assign({}, opts, { coverKey: directUrl || '', coverSourceKind: 'url', coverSource: directUrl }));
    };
    img2.onerror = function() {
      if (!coverApplyStillCurrent(opts)) return;
      currentCoverSource = null;
      uniforms.uHasCover.value = 0; setCoverDepthState(0, 0, 1);
      resetFloatColorsToIdle();
      setControlCoverSrc('');
    };
    img2.src = directUrl;
  };
  img.src = proxiedUrl;
}

function setAlbumBackground(src) {
  var bg = byId('album-bg');
  if (!bg) return;
  if (!src) {
    bg.classList.remove('visible');
    bg.style.backgroundImage = '';
    return;
  }
  bg.style.backgroundImage = "url(" + src + ")";
  bg.classList.add('visible');
}

function makeSquareCoverCanvas(img, size, crop) {
  size = size || 512;
  var cv = document.createElement('canvas');
  cv.width = cv.height = size;
  var cx = cv.getContext('2d');
  cx.clearRect(0, 0, size, size);
  var iw = img.naturalWidth || img.width;
  var ih = img.naturalHeight || img.height;
  if (crop) {
    cx.drawImage(img, crop.sx, crop.sy, crop.sSize, crop.sSize, 0, 0, size, size);
  } else {
    var s = Math.min(iw, ih);
    cx.drawImage(img, (iw - s) / 2, (ih - s) / 2, s, s, 0, 0, size, size);
  }
  return cv;
}

function coverCanvasToDataUrl(cv) {
  try {
    var webp = cv.toDataURL('image/webp', 0.88);
    if (/^data:image\/webp/i.test(webp)) return webp;
  } catch (e) {}
  return cv.toDataURL('image/jpeg', 0.88);
}

function applyCoverDataUrl(dataUrl, opts) {
  opts = opts || {};
  if (!dataUrl) return;
  var img = new Image();
  img.decoding = 'async';
  img.onload = function() {
    if (!coverApplyStillCurrent(opts)) return;
    var cv = makeSquareCoverCanvas(img, coverTextureSizeForResolution(fx.coverResolution));
    setAlbumBackground(dataUrl);
    applyCoverCanvas(cv, dataUrl, Object.assign({}, opts, { coverSourceKind: 'data', coverSource: dataUrl }));
  };
  img.src = dataUrl;
}

function commitCustomCoverCanvas(cv, opts) {
  var out = document.createElement('canvas');
  out.width = out.height = 512;
  out.getContext('2d').drawImage(cv, 0, 0, 512, 512);
  setCustomCoverForCurrent(coverCanvasToDataUrl(out), opts);
}

function loadCoverFromFile(file, opts) {
  var reader = new FileReader();
  reader.onload = function(e) {
    var img = new Image();
    img.onload = function() {
      var iw = img.naturalWidth || img.width;
      var ih = img.naturalHeight || img.height;
      if (Math.abs(iw - ih) <= 1) {
        commitCustomCoverCanvas(makeSquareCoverCanvas(img, 512), opts);
      } else {
        openCoverCropModal(img, e.target.result);
      }
    };
    img.src = e.target.result;
  };
  reader.readAsDataURL(file);
}

function bindCoverCropModal() {
  if (coverCropBound) return;
  coverCropBound = true;
  var stage = byId('cover-crop-stage');
  var zoom = byId('cover-crop-zoom');
  if (!stage || !zoom) return;
  stage.addEventListener('pointerdown', function(e) {
    if (!coverCropState) return;
    e.preventDefault();
    coverCropState.dragging = true;
    coverCropState.lastX = e.clientX;
    coverCropState.lastY = e.clientY;
    stage.classList.add('dragging');
    if (stage.setPointerCapture) {
      try { stage.setPointerCapture(e.pointerId); } catch (err) {}
    }
  });
  stage.addEventListener('pointermove', function(e) {
    if (!coverCropState || !coverCropState.dragging) return;
    e.preventDefault();
    var dx = e.clientX - coverCropState.lastX;
    var dy = e.clientY - coverCropState.lastY;
    coverCropState.lastX = e.clientX;
    coverCropState.lastY = e.clientY;
    coverCropState.x += dx;
    coverCropState.y += dy;
    updateCoverCropTransform();
  });
  function stopDrag() {
    if (!coverCropState) return;
    coverCropState.dragging = false;
    stage.classList.remove('dragging');
  }
  stage.addEventListener('pointerup', stopDrag);
  stage.addEventListener('pointercancel', stopDrag);
  stage.addEventListener('wheel', function(e) {
    if (!coverCropState) return;
    e.preventDefault();
    var next = coverCropState.scaleFactor + (e.deltaY < 0 ? 0.10 : -0.10);
    coverCropState.scaleFactor = Math.max(1, Math.min(3.2, next));
    zoom.value = coverCropState.scaleFactor;
    updateCoverCropTransform();
  }, { passive: false });
  zoom.addEventListener('input', function() {
    if (!coverCropState) return;
    coverCropState.scaleFactor = Math.max(1, Math.min(3.2, parseFloat(zoom.value) || 1));
    updateCoverCropTransform();
  });
}

function openCoverCropModal(img, dataUrl) {
  bindCoverCropModal();
  var modal = byId('cover-crop-modal');
  var stage = byId('cover-crop-stage');
  var imgEl = byId('cover-crop-img');
  var zoom = byId('cover-crop-zoom');
  if (!modal || !stage || !imgEl || !zoom) return;
  imgEl.src = dataUrl;
  zoom.value = '1';
  coverCropState = {
    img: img,
    dataUrl: dataUrl,
    naturalW: img.naturalWidth || img.width,
    naturalH: img.naturalHeight || img.height,
    stageSize: 0,
    baseScale: 1,
    scaleFactor: 1,
    x: 0,
    y: 0,
    dragging: false,
    lastX: 0,
    lastY: 0
  };
  openGsapModal(modal);
  requestAnimationFrame(function(){
    initCoverCropGeometry();
    pulseCoverCropStage();
  });
}

function initCoverCropGeometry() {
  if (!coverCropState) return;
  var stage = byId('cover-crop-stage');
  var rect = stage ? stage.getBoundingClientRect() : null;
  var size = rect ? Math.max(220, Math.round(rect.width)) : 312;
  coverCropState.stageSize = size;
  coverCropState.baseScale = size / Math.min(coverCropState.naturalW, coverCropState.naturalH);
  coverCropState.x = 0;
  coverCropState.y = 0;
  updateCoverCropTransform();
}

function clampCoverCropPan() {
  if (!coverCropState) return;
  var s = coverCropState.baseScale * coverCropState.scaleFactor;
  var rw = coverCropState.naturalW * s;
  var rh = coverCropState.naturalH * s;
  var maxX = Math.max(0, (rw - coverCropState.stageSize) / 2);
  var maxY = Math.max(0, (rh - coverCropState.stageSize) / 2);
  coverCropState.x = Math.max(-maxX, Math.min(maxX, coverCropState.x));
  coverCropState.y = Math.max(-maxY, Math.min(maxY, coverCropState.y));
}

function updateCoverCropTransform() {
  if (!coverCropState) return;
  clampCoverCropPan();
  var imgEl = byId('cover-crop-img');
  if (!imgEl) return;
  var baseW = coverCropState.naturalW * coverCropState.baseScale;
  var baseH = coverCropState.naturalH * coverCropState.baseScale;
  imgEl.style.width = baseW + 'px';
  imgEl.style.height = baseH + 'px';
  imgEl.style.transform = 'translate(-50%, -50%) translate(' + coverCropState.x + 'px,' + coverCropState.y + 'px) scale(' + coverCropState.scaleFactor + ')';
  drawCoverCropPreview();
}

function currentCoverCropRect() {
  if (!coverCropState) return null;
  var s = coverCropState.baseScale * coverCropState.scaleFactor;
  var rw = coverCropState.naturalW * s;
  var rh = coverCropState.naturalH * s;
  var left = coverCropState.stageSize / 2 - rw / 2 + coverCropState.x;
  var top = coverCropState.stageSize / 2 - rh / 2 + coverCropState.y;
  var sx = (0 - left) / s;
  var sy = (0 - top) / s;
  var sSize = coverCropState.stageSize / s;
  sx = Math.max(0, Math.min(coverCropState.naturalW - sSize, sx));
  sy = Math.max(0, Math.min(coverCropState.naturalH - sSize, sy));
  return { sx: sx, sy: sy, sSize: sSize };
}

function drawCoverCropPreview() {
  if (!coverCropState) return;
  var preview = byId('cover-crop-preview');
  var crop = currentCoverCropRect();
  if (!preview || !crop) return;
  var ctx = preview.getContext('2d');
  ctx.clearRect(0, 0, preview.width, preview.height);
  ctx.drawImage(coverCropState.img, crop.sx, crop.sy, crop.sSize, crop.sSize, 0, 0, preview.width, preview.height);
}

function pulseCoverCropStage() {
  var stage = byId('cover-crop-stage');
  if (!stage || !window.gsap) return;
  window.gsap.fromTo(stage, { scale: 0.985 }, { scale: 1, duration: 0.72, ease: 'expo.out', overwrite: true });
}

function closeCoverCropModal() {
  var modal = byId('cover-crop-modal');
  closeGsapModal(modal, function(){
    var imgEl = byId('cover-crop-img');
    if (imgEl) imgEl.removeAttribute('src');
    coverCropState = null;
  });
}

function commitCoverCrop() {
  if (!coverCropState) return;
  var crop = currentCoverCropRect();
  if (!crop) return;
  var cv = makeSquareCoverCanvas(coverCropState.img, 512, crop);
  commitCustomCoverCanvas(cv);
  closeCoverCropModal();
}


// ▼▼▼ [mineradio:control-glass] source lines 18803,19013 ▼▼▼
var controlGlassState = { key: '', searchBoxKey: '', searchPillKey: '' };
function normalizeControlGlassChromaticOffset(value) {
  var n = Number(value);
  if (!isFinite(n)) n = fxDefaults.controlGlassChromaticOffset;
  return clampRange(n, 0, 140);
}
function applyControlGlassChromaticOffset() {
  if (!fx) return;
  fx.controlGlassChromaticOffset = normalizeControlGlassChromaticOffset(fx.controlGlassChromaticOffset);
  var filter = byId('mineradio-control-glass-filter');
  if (!filter) return;
  var dx = String(-Math.round(fx.controlGlassChromaticOffset));
  filter.querySelectorAll('feOffset').forEach(function(node){
    node.setAttribute('dx', dx);
    node.setAttribute('dy', '0');
  });
}
function supportsControlGlassSvgFilter() {
  try {
    var ua = navigator.userAgent || '';
    if ((/Safari/.test(ua) && !/Chrome/.test(ua)) || /Firefox/.test(ua)) return false;
    var div = document.createElement('div');
    div.style.backdropFilter = 'url(#mineradio-control-glass-filter)';
    return div.style.backdropFilter !== '';
  } catch (e) {
    return false;
  }
}
function generateControlGlassDisplacementMap(width, height, radius) {
  width = Math.max(240, Math.round(width || 400));
  height = Math.max(48, Math.round(height || 92));
  radius = Math.max(12, Math.round(radius || 50));
  var borderWidth = 0.07;
  var edge = Math.min(width, height) * (borderWidth * 0.5);
  var innerW = Math.max(1, width - edge * 2);
  var innerH = Math.max(1, height - edge * 2);
  var svg = '<svg viewBox="0 0 ' + width + ' ' + height + '" xmlns="http://www.w3.org/2000/svg">' +
    '<defs>' +
    '<linearGradient id="glass-red" x1="100%" y1="0%" x2="0%" y2="0%"><stop offset="0%" stop-color="#0000"/><stop offset="100%" stop-color="red"/></linearGradient>' +
    '<linearGradient id="glass-blue" x1="0%" y1="0%" x2="0%" y2="100%"><stop offset="0%" stop-color="#0000"/><stop offset="100%" stop-color="blue"/></linearGradient>' +
    '</defs>' +
    '<rect x="0" y="0" width="' + width + '" height="' + height + '" fill="black"/>' +
    '<rect x="0" y="0" width="' + width + '" height="' + height + '" rx="' + radius + '" fill="url(#glass-red)"/>' +
    '<rect x="0" y="0" width="' + width + '" height="' + height + '" rx="' + radius + '" fill="url(#glass-blue)" style="mix-blend-mode:difference"/>' +
    '<rect x="' + edge.toFixed(2) + '" y="' + edge.toFixed(2) + '" width="' + innerW.toFixed(2) + '" height="' + innerH.toFixed(2) + '" rx="' + radius + '" fill="hsl(0 0% 50% / 1)" style="filter:blur(11px)"/>' +
    '</svg>';
  return 'data:image/svg+xml,' + encodeURIComponent(svg);
}
function updateGlassDisplacementMapForElement(el, img, stateKey) {
  if (!el || !img) return;
  var rect = el.getBoundingClientRect();
  if (rect.width < 2 || rect.height < 2) return;
  var radius = parseFloat(getComputedStyle(el).borderRadius) || 24;
  var key = Math.round(rect.width) + 'x' + Math.round(rect.height) + ':' + Math.round(radius);
  if (key === controlGlassState[stateKey]) return;
  controlGlassState[stateKey] = key;
  var href = generateControlGlassDisplacementMap(rect.width, rect.height, radius);
  img.setAttribute('href', href);
  try { img.setAttributeNS('http://www.w3.org/1999/xlink', 'href', href); } catch (e) {}
}
function updateControlGlassDisplacementMap() {
  updateGlassDisplacementMapForElement(
    byId('bottom-bar'),
    byId('control-glass-map'),
    'key'
  );
}
function updateSearchBoxGlassDisplacementMap() {
  updateGlassDisplacementMapForElement(
    byId('search-box'),
    byId('search-box-glass-map'),
    'searchBoxKey'
  );
}
function updateSearchPillGlassDisplacementMap() {
  var img = byId('search-pill-glass-map');
  if (!img) return;
  var nodes = Array.prototype.slice.call(__qsa('.search-mode-tabs button,.search-history-chip'));
  if (!nodes.length) return;
  var maxW = 0, maxH = 0, maxRadius = 14;
  nodes.forEach(function(el){
    if (!el || el.offsetParent === null) return;
    var rect = el.getBoundingClientRect();
    if (rect.width < 2 || rect.height < 2) return;
    maxW = Math.max(maxW, rect.width);
    maxH = Math.max(maxH, rect.height);
    maxRadius = Math.max(maxRadius, parseFloat(getComputedStyle(el).borderRadius) || Math.round(rect.height / 2) || 14);
  });
  if (maxW < 2 || maxH < 2) return;
  var width = Math.max(96, Math.round(maxW));
  var height = Math.max(32, Math.round(maxH));
  var radius = Math.max(12, Math.min(Math.round(maxRadius), Math.round(height / 2) + 10));
  var key = width + 'x' + height + ':' + radius;
  if (key === controlGlassState.searchPillKey) return;
  controlGlassState.searchPillKey = key;
  var href = generateControlGlassDisplacementMap(width, height, radius);
  img.setAttribute('href', href);
  try { img.setAttributeNS('http://www.w3.org/1999/xlink', 'href', href); } catch (e) {}
}
function initControlGlassSurface() {
  if (supportsControlGlassSvgFilter()) __cssRoot().classList.add("control-glass-svg-ok");
  applyControlGlassChromaticOffset();
  updateControlGlassDisplacementMap();
  updateSearchBoxGlassDisplacementMap();
  updateSearchPillGlassDisplacementMap();
  var bar = byId('bottom-bar');
  var searchBox = byId('search-box');
  var searchTabs = byId('search-mode-tabs');
  var searchResults = byId('search-results');
  if (window.ResizeObserver && (bar || searchBox || searchTabs || searchResults)) {
    var ro = new ResizeObserver(function(){
      requestAnimationFrame(updateControlGlassDisplacementMap);
      requestAnimationFrame(updateSearchBoxGlassDisplacementMap);
      requestAnimationFrame(updateSearchPillGlassDisplacementMap);
    });
    if (bar) ro.observe(bar);
    if (searchBox) ro.observe(searchBox);
    if (searchTabs) ro.observe(searchTabs);
    if (searchResults) ro.observe(searchResults);
  }
  if (window.MutationObserver && (searchTabs || searchResults)) {
    var mo = new MutationObserver(function(){
      requestAnimationFrame(updateSearchPillGlassDisplacementMap);
    });
    if (searchTabs) mo.observe(searchTabs, { childList: true, subtree: true, attributes: true, attributeFilter: ['class'] });
    if (searchResults) mo.observe(searchResults, { childList: true, subtree: true });
  }
  __on(window, 'resize', function(){
    requestAnimationFrame(updateControlGlassDisplacementMap);
    requestAnimationFrame(updateSearchBoxGlassDisplacementMap);
    requestAnimationFrame(updateSearchPillGlassDisplacementMap);
  });
}

function bindPlayerControlAnimations() {
  if (!window.gsap) return;
  __qsa('#bottom-bar .ctrl-btn').forEach(function(btn){
    if (!btn || btn.dataset.controlAnimBound === '1') return;
    btn.dataset.controlAnimBound = '1';
    var isPlay = btn.id === 'play-btn';
    var iconTarget = btn.querySelector('svg,.lyrics-word-icon,#quality-btn-label');
    function canAnimate() {
      return !btn.disabled && !btn.classList.contains('busy');
    }
    function hoverIn(e) {
      if (!canAnimate() || (e && e.pointerType === 'touch')) return;
      window.gsap.to(btn, { y: -2, scale: isPlay ? 1.07 : 1.08, duration: 0.20, ease: 'power2.out', overwrite: 'auto' });
      if (iconTarget) window.gsap.to(iconTarget, { scale: isPlay ? 1.08 : 1.10, duration: 0.22, ease: 'power2.out', overwrite: 'auto' });
    }
    function hoverOut() {
      window.gsap.to(btn, { y: 0, scale: 1, rotate: 0, duration: 0.26, ease: 'power2.out', overwrite: 'auto' });
      if (iconTarget) window.gsap.to(iconTarget, { scale: 1, rotate: 0, duration: 0.22, ease: 'power2.out', overwrite: 'auto' });
    }
    function pressDown() {
      if (!canAnimate()) return;
      window.gsap.to(btn, { y: 0, scale: isPlay ? 0.91 : 0.90, duration: 0.10, ease: 'power2.out', overwrite: 'auto' });
      if (iconTarget) window.gsap.to(iconTarget, { scale: 0.88, duration: 0.10, ease: 'power2.out', overwrite: 'auto' });
    }
    function release(e) {
      if (!canAnimate()) return;
      var hovered = e && e.pointerType !== 'touch' && btn.matches(':hover');
      window.gsap.to(btn, { y: hovered ? -2 : 0, scale: hovered ? (isPlay ? 1.07 : 1.08) : 1, duration: 0.24, ease: 'back.out(1.9)', overwrite: 'auto' });
      if (iconTarget) window.gsap.to(iconTarget, { scale: hovered ? 1.06 : 1, duration: 0.22, ease: 'back.out(1.8)', overwrite: 'auto' });
    }
    function clickPulse() {
      if (!canAnimate() || btn.id === 'play-mode-btn') return;
      var pulseSize = isPlay ? 18 : 10;
      var pulseColor = isPlay ? 'rgba(255,63,85,.34)' : 'rgba(255,255,255,.22)';
      window.gsap.killTweensOf(btn, 'boxShadow');
      window.gsap.fromTo(btn,
        { boxShadow: '0 0 0 0 ' + pulseColor },
        { boxShadow: '0 0 0 ' + pulseSize + 'px rgba(255,63,85,0)', duration: isPlay ? 0.58 : 0.42, ease: 'sine.out', overwrite: false, onComplete: function(){ window.gsap.set(btn, { clearProps: 'boxShadow' }); } }
      );
      if (iconTarget) window.gsap.fromTo(iconTarget, { rotate: isPlay ? 0 : -5 }, { rotate: 0, duration: 0.34, ease: 'elastic.out(1,0.55)', overwrite: 'auto' });
    }
    btn.addEventListener('pointerenter', hoverIn);
    btn.addEventListener('pointerleave', hoverOut);
    btn.addEventListener('pointercancel', hoverOut);
    btn.addEventListener('mousedown', function(e){ e.preventDefault(); });
    btn.addEventListener('pointerdown', pressDown);
    btn.addEventListener('pointerup', release);
    btn.addEventListener('click', clickPulse);
    btn.addEventListener('focus', function(){ hoverIn(); });
    btn.addEventListener('blur', hoverOut);
  });
}

function clearPlayerControlFocusState(reason) {
  try {
    __qsa('#bottom-bar .ctrl-btn').forEach(function(btn){
      if (!btn) return;
      if (document.activeElement === btn) btn.blur();
      btn.classList.remove('focus-visible');
      if (window.gsap) {
        window.gsap.killTweensOf(btn);
        window.gsap.set(btn, { y: 0, scale: 1, rotate: 0, clearProps: 'boxShadow' });
        var iconTarget = btn.querySelector('svg,.lyrics-word-icon,#quality-btn-label');
        if (iconTarget) {
          window.gsap.killTweensOf(iconTarget);
          window.gsap.set(iconTarget, { scale: 1, rotate: 0 });
        }
      } else {
        btn.style.transform = '';
        btn.style.boxShadow = '';
      }
    });
  } catch (e) {
    console.warn('[ControlFocusClear]', reason || 'unknown', e);
  }
}


// ▼▼▼ [mineradio:preset-meta] source lines 20084,20122 ▼▼▼
var presetMeta = [
  { name: 'emily专辑封面',  desc: '封面粒子 · 快速入场' },
  { name: '滚筒', desc: '隧道 · 沉浸感' },
  { name: '星球',  desc: '星球 · 雕塑感' },
  { name: '虚空', desc: '无粒子 · 自定义背景' },
  { name: '唱片', desc: '唱片 · 圆形封面' },
  { name: '星河', desc: '壁纸粒子 · 音乐律动' },
  { name: '安魂', desc: '骷髅·YUI7W', descHtml: '骷髅·<span class="pc-yui7w">YUI7W</span>' },
];
var presetIcons = [
  '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M3 14c3-2 5-2 8 0s5 2 8 0M3 10c3-2 5-2 8 0s5 2 8 0M3 18c3-2 5-2 8 0s5 2 8 0"/></svg>',
  '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="5"/><circle cx="12" cy="12" r="1.5" fill="currentColor"/></svg>',
  '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="7"/><path d="M5 12a7 7 0 0 0 14 0"/></svg>',
  '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><circle cx="12" cy="12" r="7"/><path d="M8.8 8.8l6.4 6.4"/></svg>',
  '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><circle cx="12" cy="12" r="8.5"/><circle cx="12" cy="12" r="4.4"/><path d="M16.5 5.2c2.1.9 3.4 2.4 4 4.5"/><path d="M18.8 3.2l1.5 4.8"/></svg>',
  '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><path d="M3 15c2.2-4.4 4.4-4.4 6.6 0s4.4 4.4 6.6 0S20.6 10.6 23 15"/><path d="M3 9c2.2 2.2 4.4 2.2 6.6 0s4.4-2.2 6.6 0S20.6 11.2 23 9"/><circle cx="12" cy="12" r="1.7" fill="currentColor"/></svg>',
  '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><path d="M10 3.2h4v6.2h4.2v3.8H14v7.6h-4v-7.6H5.8V9.4H10z"/></svg>',
];
var presetDisplayOrder = [0, 6, 5, 4, 2, 1, 3];
var lyricColorPresets = [
  { name:'雾蓝', color:'#a9b8c8' },
  { name:'银蓝', color:'#9db8cf' },
  { name:'冰川', color:'#7ec8d8' },
  { name:'青绿', color:'#66d2b5' },
  { name:'松针', color:'#7fa894' },
  { name:'月白', color:'#d7d2c4' },
  { name:'岩金', color:'#c3ae7c' },
  { name:'琥珀', color:'#d9a45f' },
  { name:'暮粉', color:'#c78aa4' },
  { name:'玫红', color:'#d76a8d' },
  { name:'烟紫', color:'#9b83d3' },
  { name:'电紫', color:'#8d70ff' },
  { name:'靛蓝', color:'#5e78d8' },
  { name:'海蓝', color:'#3c9fe0' },
  { name:'霓青', color:'#28c5c3' },
  { name:'夜绿', color:'#245c49' },
  { name:'酒红', color:'#6d1f35' },
  { name:'墨黑', color:'#111318' },
];

// ▼▼▼ [mineradio:fx-archives] source lines 20123,20621 ▼▼▼
var USER_FX_ARCHIVE_STORE_KEY = 'mineradio-user-fx-archives-v1';
var USER_FX_ARCHIVE_EXPORT_TYPE = 'mineradio-user-fx-archive';
var USER_FX_ARCHIVE_SCHEMA = 1;
function defaultUserFxArchiveName(index) {
  return '存档 ' + (index + 1);
}
function normalizeUserFxArchiveName(name, index) {
  name = String(name || '').replace(/\s+/g, ' ').trim();
  if (!name) name = defaultUserFxArchiveName(index);
  return name.slice(0, 18);
}
function archiveNumber(raw, key, fallback, min, max) {
  var value = raw && raw[key] != null ? Number(raw[key]) : fallback;
  if (!isFinite(value)) value = fallback;
  return clampRange(value, min, max);
}
function archiveMode(raw, key, pattern, fallback) {
  var value = String(raw && raw[key] != null ? raw[key] : fallback);
  return pattern.test(value) ? value : fallback;
}
function normalizeFxArchiveSnapshot(raw) {
  if (!raw || typeof raw !== 'object') return null;
  var savedPreset = clampRange(Number(raw.preset) || 0, 0, presetMeta.length - 1);
  if (savedPreset === 3 && raw.visualPresetSchema !== VISUAL_PRESET_SCHEMA) savedPreset = 5;
  return {
    visualPresetSchema: VISUAL_PRESET_SCHEMA,
    preset: savedPreset,
    intensity: archiveNumber(raw, 'intensity', fxDefaults.intensity, 0.2, 1.6),
    cinemaShake: archiveNumber(raw, 'cinemaShake', fxDefaults.cinemaShake, 0, 1.8),
    depth: archiveNumber(raw, 'depth', fxDefaults.depth, 0.2, 1.8),
    coverResolution: normalizeCoverResolution(raw.coverResolution),
    point: archiveNumber(raw, 'point', fxDefaults.point, 0.5, 2.2),
    speed: archiveNumber(raw, 'speed', fxDefaults.speed, 0.2, 2.5),
    twist: archiveNumber(raw, 'twist', fxDefaults.twist, 0, 0.6),
    color: archiveNumber(raw, 'color', fxDefaults.color, 0.5, 2.0),
    scatter: archiveNumber(raw, 'scatter', fxDefaults.scatter, 0, 0.5),
    bgFade: archiveNumber(raw, 'bgFade', fxDefaults.bgFade, 0, 1.2),
    bloomStrength: archiveNumber(raw, 'bloomStrength', fxDefaults.bloomStrength, 0, 1.6),
    lyricGlowStrength: archiveNumber(raw, 'lyricGlowStrength', fxDefaults.lyricGlowStrength, 0, 0.85),
    lyricScale: archiveNumber(raw, 'lyricScale', fxDefaults.lyricScale, 0.35, 1.65),
    lyricOffsetX: archiveNumber(raw, 'lyricOffsetX', fxDefaults.lyricOffsetX, -2.0, 2.0),
    lyricOffsetY: archiveNumber(raw, 'lyricOffsetY', fxDefaults.lyricOffsetY, -1.2, 1.35),
    lyricOffsetZ: archiveNumber(raw, 'lyricOffsetZ', fxDefaults.lyricOffsetZ, -1.6, 1.6),
    lyricTiltX: archiveNumber(raw, 'lyricTiltX', fxDefaults.lyricTiltX, -42, 42),
    lyricTiltY: archiveNumber(raw, 'lyricTiltY', fxDefaults.lyricTiltY, -42, 42),
    lyricCameraLock: !!raw.lyricCameraLock,
    lyricColorMode: raw.lyricColorMode === 'custom' ? 'custom' : 'auto',
    lyricColor: normalizeHexColor(raw.lyricColor || fxDefaults.lyricColor),
    lyricHighlightMode: raw.lyricHighlightMode === 'custom' ? 'custom' : 'auto',
    lyricHighlightColor: normalizeHexColor(raw.lyricHighlightColor || fxDefaults.lyricHighlightColor),
    lyricGlowLinked: raw.lyricGlowLinked !== false,
    lyricGlowColor: normalizeHexColor(raw.lyricGlowColor || fxDefaults.lyricGlowColor),
    lyricFont: normalizeLyricFontKey(raw.lyricFont),
    lyricLetterSpacing: archiveNumber(raw, 'lyricLetterSpacing', fxDefaults.lyricLetterSpacing, -0.04, 0.18),
    lyricLineHeight: archiveNumber(raw, 'lyricLineHeight', fxDefaults.lyricLineHeight, 0.86, 1.35),
    lyricWeight: archiveNumber(raw, 'lyricWeight', fxDefaults.lyricWeight, 500, 900),
    visualTintMode: raw.visualTintMode === 'custom' ? 'custom' : 'auto',
    visualTintColor: normalizeHexColor(raw.visualTintColor || fxDefaults.visualTintColor),
    uiAccentColor: normalizeHexColor(raw.uiAccentColor || fxDefaults.uiAccentColor, fxDefaults.uiAccentColor),
    homeAccentColor: normalizeHexColor(raw.homeAccentColor || fxDefaults.homeAccentColor, fxDefaults.homeAccentColor),
    homeIconColor: normalizeHexColor(raw.homeIconColor || fxDefaults.homeIconColor, fxDefaults.homeIconColor),
    visualIconColor: normalizeHexColor(raw.visualIconColor || fxDefaults.visualIconColor, fxDefaults.visualIconColor),
    backgroundColorMode: raw.backgroundColorMode === 'custom' || raw.backgroundColorCustom ? 'custom' : 'cover',
    backgroundColor: normalizeHexColor(raw.backgroundColor || fxDefaults.backgroundColor, fxDefaults.backgroundColor),
    backgroundOpacity: archiveNumber(raw, 'backgroundOpacity', fxDefaults.backgroundOpacity, 0, 1),
    controlGlassChromaticOffset: archiveNumber(raw, 'controlGlassChromaticOffset', fxDefaults.controlGlassChromaticOffset, 0, 140),
    backgroundColorCustom: raw.backgroundColorMode === 'custom' || !!raw.backgroundColorCustom,
    floatLayer: !!raw.floatLayer,
    cinema: raw.cinema !== false,
    edge: !!raw.edge,
    aiDepth: !!raw.aiDepth,
    bloom: !!raw.bloom,
    lyricGlow: raw.lyricGlow !== false,
    lyricGlowBeat: raw.lyricGlowBeat !== false,
    lyricGlowParticles: !!raw.lyricGlowParticles,
    desktopLyrics: !!raw.desktopLyrics,
    desktopLyricsSize: archiveNumber(raw, 'desktopLyricsSize', fxDefaults.desktopLyricsSize, 0.72, 1.55),
    desktopLyricsOpacity: archiveNumber(raw, 'desktopLyricsOpacity', fxDefaults.desktopLyricsOpacity, 0.28, 1),
    desktopLyricsY: archiveNumber(raw, 'desktopLyricsY', fxDefaults.desktopLyricsY, 0.08, 0.92),
    desktopLyricsClickThrough: raw.desktopLyricsClickThrough === true,
    desktopLyricsCinema: raw.desktopLyricsCinema !== false,
    desktopLyricsHighlight: raw.desktopLyricsHighlight === true,
    desktopLyricsFps: normalizeDesktopLyricsFps(Object.prototype.hasOwnProperty.call(raw, 'desktopLyricsFps') ? raw.desktopLyricsFps : fxDefaults.desktopLyricsFps),
    performanceBackground: normalizePerformanceBackgroundMode(raw.performanceBackground, raw.liveBackgroundKeep === true),
    performanceQuality: normalizePerformanceQuality(raw.performanceQuality),
    liveBackgroundKeep: normalizePerformanceBackgroundMode(raw.performanceBackground, raw.liveBackgroundKeep === true) === 'keep',
    particleLyrics: raw.particleLyrics !== false,
    backCover: !!raw.backCover,
    shelf: archiveMode(raw, 'shelf', /^(off|side|stage)$/, fxDefaults.shelf),
    shelfCameraMode: archiveMode(raw, 'shelfCameraMode', /^(dynamic|static)$/, fxDefaults.shelfCameraMode),
    shelfPresence: archiveMode(raw, 'shelfPresence', /^(auto|always)$/, fxDefaults.shelfPresence),
    shelfShowPodcasts: raw.shelfShowPodcasts !== false,
    shelfMergeCollections: raw.shelfMergeCollections === true,
    shelfSize: archiveNumber(raw, 'shelfSize', fxDefaults.shelfSize, 0.65, 1.45),
    shelfOffsetX: archiveNumber(raw, 'shelfOffsetX', fxDefaults.shelfOffsetX, -1.2, 1.2),
    shelfOffsetY: archiveNumber(raw, 'shelfOffsetY', fxDefaults.shelfOffsetY, -0.9, 0.9),
    shelfOffsetZ: archiveNumber(raw, 'shelfOffsetZ', fxDefaults.shelfOffsetZ, -0.9, 0.9),
    shelfAngleY: archiveNumber(raw, 'shelfAngleY', fxDefaults.shelfAngleY, -30, 30),
    shelfAngleYManual: raw.shelfAngleYManual === true,
    shelfOpacity: archiveNumber(raw, 'shelfOpacity', fxDefaults.shelfOpacity, 0.25, 1),
    shelfBgOpacity: archiveNumber(raw, 'shelfBgOpacity', fxDefaults.shelfBgOpacity, 0.25, 0.98),
    shelfAccentColor: normalizeHexColor(raw.shelfAccentColor || fxDefaults.shelfAccentColor, fxDefaults.shelfAccentColor),
    cam: archiveMode(raw, 'cam', /^(off|gesture)$/, fxDefaults.cam)
  };
}
function readUserFxArchives() {
  var raw = [];
  try {
    raw = JSON.parse(localStorage.getItem(USER_FX_ARCHIVE_STORE_KEY) || '[]') || [];
  } catch (e) {
    raw = [];
  }
  if (!Array.isArray(raw)) raw = [];
  return raw.map(function(slot, index){
    slot = slot && typeof slot === 'object' ? slot : {};
    var snapshot = normalizeFxArchiveSnapshot(slot.snapshot);
    return {
      name: normalizeUserFxArchiveName(slot.name, index),
      createdAt: Number(slot.createdAt) || (snapshot ? (Number(slot.savedAt) || Date.now()) : 0),
      savedAt: snapshot ? (Number(slot.savedAt) || Date.now()) : 0,
      snapshot: snapshot
    };
  }).filter(function(slot){
    return !!(slot.snapshot || slot.savedAt || slot.createdAt);
  });
}
function saveUserFxArchives() {
  try {
    localStorage.setItem(USER_FX_ARCHIVE_STORE_KEY, JSON.stringify(userFxArchives));
  } catch (e) {
    showToast('用户存档保存失败，本地存储空间可能不足');
  }
}
function hasStoredUserFxArchives() {
  try {
    return localStorage.getItem(USER_FX_ARCHIVE_STORE_KEY) != null;
  } catch (e) {
    return true;
  }
}
function createPackagedDefaultUserFxArchiveSlot() {
  return {
    name: normalizeUserFxArchiveName(PACKAGED_DEFAULT_USER_FX_ARCHIVE_NAME, 0),
    createdAt: PACKAGED_DEFAULT_USER_FX_ARCHIVE_EXPORTED_AT,
    savedAt: PACKAGED_DEFAULT_USER_FX_ARCHIVE_SAVED_AT,
    snapshot: normalizeFxArchiveSnapshot(clonePackagedDefaultFxSnapshot())
  };
}
function formatUserArchiveTime(ts) {
  ts = Number(ts) || 0;
  if (!ts) return '空槽位';
  var diff = Date.now() - ts;
  if (diff < 60000) return '刚刚保存';
  if (diff < 3600000) return Math.max(1, Math.round(diff / 60000)) + ' 分钟前';
  var d = new Date(ts);
  function pad(v) { return String(v).padStart(2, '0'); }
  return pad(d.getMonth() + 1) + '-' + pad(d.getDate()) + ' ' + pad(d.getHours()) + ':' + pad(d.getMinutes());
}
function captureFxArchiveSnapshot() {
  return normalizeFxArchiveSnapshot(Object.assign({ visualPresetSchema: VISUAL_PRESET_SCHEMA }, fx));
}
function applySavedLyricPaletteState() {
  if (!stageLyrics) return;
  setStageLyricPalette(fx.lyricColorMode === 'custom'
    ? lyricPaletteFromHex(fx.lyricColor)
    : (stageLyrics.coverPalette || stageLyrics.palette));
  updateLyricColorControls();
  updateLyricHighlightControls();
  updateLyricGlowControls();
}
function applyFxArchiveSnapshot(snapshot) {
  var data = normalizeFxArchiveSnapshot(snapshot);
  if (!data) return false;
  var targetPreset = data.preset;
  Object.keys(data).forEach(function(key){
    if (key === 'visualPresetSchema' || key === 'preset') return;
    fx[key] = data[key];
  });
  normalizeDevelopmentLockedFxState();
  setPreset(targetPreset, { silent: true, preserveCamera: false, skipTransition: false, noSave: true, commitPlaybackPreset: true });
  applyCoverParticleResolution(fx.coverResolution, { reload: true });
  if (fx.floatLayer) createFloatLayer(); else destroyFloatLayer();
  setParticleLyricsSilently(fx.particleLyrics);
  if (fx.backCover) createBackCoverLayer(); else destroyBackCoverLayer();
  if (fx.aiDepth) {
    aiDepthFailUntil = 0;
    queueAIDepthForCurrentCover(true);
  }
  setShelfMode(fx.shelf);
  if (shelfManager && shelfManager.rebuild) shelfManager.rebuild(true);
  if (shelfManager && shelfManager.refreshTheme) shelfManager.refreshTheme();
  setCamMode(fx.cam);
  updateFxInputs();
  applySavedLyricPaletteState();
  refreshCurrentLyricStyle();
  applyDesktopLyricsState(true);
  applyWallpaperModeState(true);
  updateRenderPowerClasses();
  applyRendererPowerMode();
  saveLyricLayout();
  return true;
}
var hadStoredUserFxArchives = hasStoredUserFxArchives();
var userFxArchives = readUserFxArchives();
if (!hadStoredUserFxArchives) {
  userFxArchives = [createPackagedDefaultUserFxArchiveSlot()];
  saveUserFxArchives();
}
var userFxArchiveEditing = -1;
function renderUserFxArchives() {
  var grid = byId('user-archive-grid');
  if (!grid) return;
  grid.innerHTML = userFxArchives.map(function(slot, index){
    var hasSave = !!slot.snapshot;
    var editing = userFxArchiveEditing === index;
    var nameHtml = editing
      ? '<input class="user-archive-input" id="user-archive-input-' + index + '" type="text" maxlength="18" value="' + escHtml(slot.name) + '" onkeydown="handleUserFxArchiveRenameKey(event,' + index + ')">'
      : '<div class="user-archive-name" title="' + escHtml(slot.name) + '">' + escHtml(slot.name) + '</div>';
    var actionsHtml = editing
      ? '<button type="button" onclick="commitUserFxArchiveRename(' + index + ')">确定</button>' +
        '<button type="button" onclick="cancelUserFxArchiveRename()">取消</button>'
      : '<button type="button" onclick="applyUserFxArchive(' + index + ')"' + (hasSave ? '' : ' disabled') + '>应用</button>' +
        '<button type="button" onclick="saveUserFxArchive(' + index + ')">保存</button>' +
        '<button type="button" onclick="renameUserFxArchive(' + index + ')">命名</button>';
    return '<div class="user-archive-slot' + (hasSave ? ' has-save' : '') + '" data-slot="' + index + '">' +
      nameHtml +
      '<div class="user-archive-meta">' + formatUserArchiveTime(slot.savedAt) + '</div>' +
      '<div class="user-archive-actions">' +
        actionsHtml +
      '</div>' +
    '</div>';
  }).join('');
  if (userFxArchiveEditing >= 0) {
    setTimeout(function(){
      var input = byId('user-archive-input-' + userFxArchiveEditing);
      if (input) {
        input.focus();
        input.select();
      }
    }, 0);
  }
}
function saveUserFxArchive(index) {
  index = clampRange(Number(index) || 0, 0, Math.max(0, userFxArchives.length - 1));
  userFxArchives[index].snapshot = captureFxArchiveSnapshot();
  userFxArchives[index].savedAt = Date.now();
  userFxArchives[index].name = normalizeUserFxArchiveName(userFxArchives[index].name, index);
  saveUserFxArchives();
  renderUserFxArchives();
  showToast('已保存到 ' + userFxArchives[index].name);
}
function applyUserFxArchive(index) {
  index = clampRange(Number(index) || 0, 0, Math.max(0, userFxArchives.length - 1));
  var slot = userFxArchives[index];
  if (!slot || !slot.snapshot) {
    showToast('这个用户存档还是空的');
    return;
  }
  if (applyFxArchiveSnapshot(slot.snapshot)) {
    showToast('已应用 ' + slot.name);
  }
}
function renameUserFxArchive(index) {
  index = clampRange(Number(index) || 0, 0, Math.max(0, userFxArchives.length - 1));
  userFxArchiveEditing = index;
  renderUserFxArchives();
}
function commitUserFxArchiveRename(index) {
  index = clampRange(Number(index) || 0, 0, Math.max(0, userFxArchives.length - 1));
  var input = byId('user-archive-input-' + index);
  userFxArchives[index].name = normalizeUserFxArchiveName(input && input.value, index);
  userFxArchiveEditing = -1;
  saveUserFxArchives();
  renderUserFxArchives();
  showToast('已命名为 ' + userFxArchives[index].name);
}
function cancelUserFxArchiveRename() {
  userFxArchiveEditing = -1;
  renderUserFxArchives();
}
function handleUserFxArchiveRenameKey(e, index) {
  if (e.key === 'Enter') {
    e.preventDefault();
    commitUserFxArchiveRename(index);
  } else if (e.key === 'Escape') {
    e.preventDefault();
    cancelUserFxArchiveRename();
  }
}

function defaultUserFxArchiveName(index) {
  return '用户存档 ' + (Number(index) + 1);
}
function normalizeUserFxArchiveName(name, index) {
  name = String(name || '').replace(/\s+/g, ' ').trim();
  if (!name) name = defaultUserFxArchiveName(index);
  return name.slice(0, 28);
}
function userFxArchiveAt(index) {
  index = Number(index);
  if (!isFinite(index)) return null;
  index = Math.floor(index);
  return index >= 0 && index < userFxArchives.length ? userFxArchives[index] : null;
}
function renderUserFxArchives() {
  var grid = byId('user-archive-grid');
  if (!grid) return;
  var toolbar =
    '<div class="user-archive-toolbar">' +
      '<div class="user-archive-note">空白新建，保存当前视觉参数；支持拖拽 JSON 导入，也可以导出为文件备份。</div>' +
      '<div class="user-archive-tools">' +
        '<button class="fx-mini-btn ghost" type="button" onclick="createUserFxArchive()">新建</button>' +
        '<button class="fx-mini-btn ghost" type="button" onclick="importUserFxArchiveFromDialog()">导入</button>' +
      '</div>' +
    '</div>';
  var cards = userFxArchives.map(function(slot, index){
    var hasSave = !!slot.snapshot;
    var editing = userFxArchiveEditing === index;
    var nameHtml = editing
      ? '<input class="user-archive-input" id="user-archive-input-' + index + '" type="text" maxlength="28" value="' + escHtml(slot.name) + '" onkeydown="handleUserFxArchiveRenameKey(event,' + index + ')">'
      : '<div class="user-archive-name" title="' + escHtml(slot.name) + '">' + escHtml(slot.name) + '</div>';
    var actionsHtml = editing
      ? '<button type="button" onclick="commitUserFxArchiveRename(' + index + ')">确定</button>' +
        '<button type="button" onclick="cancelUserFxArchiveRename()">取消</button>'
      : '<button type="button" onclick="applyUserFxArchive(' + index + ')"' + (hasSave ? '' : ' disabled') + '>应用</button>' +
        '<button type="button" onclick="saveUserFxArchive(' + index + ')">保存</button>' +
        '<button type="button" onclick="renameUserFxArchive(' + index + ')">命名</button>' +
        '<button type="button" onclick="exportUserFxArchive(' + index + ')"' + (hasSave ? '' : ' disabled') + '>导出</button>' +
        '<button type="button" onclick="removeUserFxArchive(' + index + ')">删除</button>';
    return '<div class="user-archive-slot' + (hasSave ? ' has-save' : '') + '" data-slot="' + index + '">' +
      nameHtml +
      '<div class="user-archive-meta">' + (hasSave ? formatUserArchiveTime(slot.savedAt) : '空白存档，点击保存写入当前视觉') + '</div>' +
      '<div class="user-archive-actions">' + actionsHtml + '</div>' +
    '</div>';
  }).join('');
  var addCard = '<button class="user-archive-slot is-new" type="button" onclick="createUserFxArchive()"><strong>＋ 新建空白存档</strong><span class="user-archive-meta">可继续创建，不限制 4 个</span></button>';
  grid.innerHTML = toolbar + cards + addCard;
  bindUserFxArchiveDrop();
  if (userFxArchiveEditing >= 0) {
    setTimeout(function(){
      var input = byId('user-archive-input-' + userFxArchiveEditing);
      if (input) {
        input.focus();
        input.select();
      }
    }, 0);
  }
}
function createUserFxArchive() {
  var index = userFxArchives.length;
  userFxArchives.push({
    name: normalizeUserFxArchiveName('', index),
    createdAt: Date.now(),
    savedAt: 0,
    snapshot: null
  });
  userFxArchiveEditing = index;
  saveUserFxArchives();
  renderUserFxArchives();
  showToast('已新建空白用户存档');
}
function saveUserFxArchive(index) {
  var slot = userFxArchiveAt(index);
  if (!slot) return;
  slot.snapshot = captureFxArchiveSnapshot();
  slot.savedAt = Date.now();
  slot.createdAt = slot.createdAt || slot.savedAt;
  slot.name = normalizeUserFxArchiveName(slot.name, index);
  saveUserFxArchives();
  renderUserFxArchives();
  showToast('已保存到 ' + slot.name);
}
function applyUserFxArchive(index) {
  var slot = userFxArchiveAt(index);
  if (!slot || !slot.snapshot) {
    showToast('这个用户存档还是空白');
    return;
  }
  if (applyFxArchiveSnapshot(slot.snapshot)) showToast('已应用 ' + slot.name);
}
function renameUserFxArchive(index) {
  if (!userFxArchiveAt(index)) return;
  userFxArchiveEditing = Math.floor(Number(index) || 0);
  renderUserFxArchives();
}
function commitUserFxArchiveRename(index) {
  var slot = userFxArchiveAt(index);
  if (!slot) return;
  var input = byId('user-archive-input-' + index);
  slot.name = normalizeUserFxArchiveName(input && input.value, index);
  slot.createdAt = slot.createdAt || Date.now();
  userFxArchiveEditing = -1;
  saveUserFxArchives();
  renderUserFxArchives();
  showToast('已命名为 ' + slot.name);
}
function cancelUserFxArchiveRename() {
  userFxArchiveEditing = -1;
  renderUserFxArchives();
}
function removeUserFxArchive(index) {
  if (!userFxArchiveAt(index)) return;
  userFxArchives.splice(index, 1);
  userFxArchiveEditing = -1;
  saveUserFxArchives();
  renderUserFxArchives();
  showToast('已删除用户存档');
}
function userFxArchiveExportPayload(slot) {
  return {
    type: USER_FX_ARCHIVE_EXPORT_TYPE,
    schema: USER_FX_ARCHIVE_SCHEMA,
    exportedAt: Date.now(),
    name: slot.name,
    savedAt: slot.savedAt,
    snapshot: slot.snapshot
  };
}
function safeArchiveFileName(name) {
  return String(name || 'Mineradio 用户存档').replace(/[\\/:*?"<>|]+/g, '-').slice(0, 48) + '.json';
}
function exportUserFxArchive(index) {
  var slot = userFxArchiveAt(index);
  if (!slot || !slot.snapshot) {
    showToast('空白存档不能导出');
    return;
  }
  var payload = userFxArchiveExportPayload(slot);
  var text = JSON.stringify(payload, null, 2);
  var api = getDesktopWindowApi && getDesktopWindowApi();
  if (api && typeof api.exportJsonFile === 'function') {
    api.exportJsonFile({ defaultName: safeArchiveFileName(slot.name), text: text }).then(function(res){
      if (res && res.ok) showToast('用户存档已导出');
      else if (!res || !res.canceled) showToast('用户存档导出失败');
    }).catch(function(){ showToast('用户存档导出失败'); });
    return;
  }
  var blob = new Blob([text], { type: 'application/json;charset=utf-8' });
  var url = URL.createObjectURL(blob);
  var a = document.createElement('a');
  a.href = url;
  a.download = safeArchiveFileName(slot.name);
  a.click();
  setTimeout(function(){ URL.revokeObjectURL(url); }, 1000);
}
function normalizeImportedFxArchivePayload(payload, fileName) {
  if (!payload || typeof payload !== 'object') return null;
  var snapshot = payload.snapshot ? normalizeFxArchiveSnapshot(payload.snapshot) : normalizeFxArchiveSnapshot(payload);
  if (!snapshot) return null;
  var baseName = String(fileName || '').split(/[\\/]/).pop().replace(/\.json$/i, '');
  return {
    name: normalizeUserFxArchiveName(payload.name || baseName, userFxArchives.length),
    createdAt: Date.now(),
    savedAt: Number(payload.savedAt) || Date.now(),
    snapshot: snapshot
  };
}
function importUserFxArchiveText(text, fileName) {
  var payload = null;
  try { payload = JSON.parse(String(text || '')); } catch (e) {}
  var slot = normalizeImportedFxArchivePayload(payload, fileName);
  if (!slot) {
    showToast('导入失败，文件不是有效的用户存档');
    return false;
  }
  userFxArchives.push(slot);
  saveUserFxArchives();
  renderUserFxArchives();
  showToast('已导入 ' + slot.name);
  return true;
}
function importUserFxArchiveFromDialog() {
  var api = getDesktopWindowApi && getDesktopWindowApi();
  if (api && typeof api.importJsonFile === 'function') {
    api.importJsonFile().then(function(res){
      if (res && res.ok) importUserFxArchiveText(res.text, res.filePath || '用户存档.json');
      else if (!res || !res.canceled) showToast('导入失败');
    }).catch(function(){ showToast('导入失败'); });
    return;
  }
  var input = document.createElement('input');
  input.type = 'file';
  input.accept = '.json,application/json';
  input.onchange = function(){
    var file = input.files && input.files[0];
    if (file) readUserFxArchiveImportFile(file);
  };
  input.click();
}
function readUserFxArchiveImportFile(file) {
  if (!file || !/\.json$/i.test(file.name || '')) {
    showToast('请导入 JSON 用户存档');
    return;
  }
  var reader = new FileReader();
  reader.onload = function(e){ importUserFxArchiveText(e.target && e.target.result, file.name); };
  reader.onerror = function(){ showToast('导入失败'); };
  reader.readAsText(file, 'utf-8');
}

// ▼▼▼ [mineradio:fx-colors-presets] source lines 20642,21312 ▼▼▼
function buildLyricColorControls() {
  var grid = byId('lyric-color-grid');
  if (!grid) return;
  var html = '<button class="lyric-swatch auto" type="button" data-auto="1" onclick="setLyricColorAuto()" title="封面取色">AUTO</button>';
  html += lyricColorPresets.map(function(p, i){
    return '<button class="lyric-swatch" type="button" data-color="' + p.color + '" onclick="setLyricColorPreset(' + i + ')" title="' + escHtml(p.name) + '" style="--swatch:' + p.color + '"></button>';
  }).join('');
  grid.innerHTML = html;
}
function updateLyricColorControls() {
  var picker = byId('lyric-color-picker');
  var value = byId('lyric-color-value');
  var autoBtn = byId('lyric-auto-btn');
  var color = normalizeHexColor(fx.lyricColor);
  if (picker) picker.value = color;
  if (value) value.textContent = fx.lyricColorMode === 'custom' ? color.toUpperCase() : '封面取色';
  if (autoBtn) autoBtn.classList.toggle('active', fx.lyricColorMode !== 'custom');
  __qsa('.lyric-swatch').forEach(function(btn){
    var isAuto = btn.dataset.auto === '1';
    var isColor = normalizeHexColor(btn.dataset.color || '') === color;
    btn.classList.toggle('active', isAuto ? fx.lyricColorMode !== 'custom' : (fx.lyricColorMode === 'custom' && isColor));
  });
}
function updateLyricHighlightControls() {
  var picker = byId('lyric-highlight-picker');
  var value = byId('lyric-highlight-value');
  var autoBtn = byId('lyric-highlight-auto-btn');
  var color = normalizeHexColor(fx.lyricHighlightColor);
  if (picker) picker.value = color;
  if (value) value.textContent = fx.lyricHighlightMode === 'custom' ? color.toUpperCase() : '跟随歌词';
  if (autoBtn) autoBtn.classList.toggle('active', fx.lyricHighlightMode !== 'custom');
}
function updateLyricGlowControls() {
  var row = byId('lyric-glow-row');
  var picker = byId('lyric-glow-picker');
  var value = byId('lyric-glow-value');
  var linkBtn = byId('lyric-glow-link-btn');
  var linked = fx.lyricGlowLinked !== false;
  var color = normalizeHexColor(fx.lyricGlowColor || '#9db8cf');
  if (picker) picker.value = color;
  if (row) row.classList.toggle('linked', linked);
  if (value) value.textContent = linked ? '跟随高亮' : color.toUpperCase();
  if (linkBtn) {
    linkBtn.classList.toggle('active', linked);
    linkBtn.textContent = linked ? '链接' : '独立';
    linkBtn.title = linked ? '点击后单独设置溢光颜色' : '点击后让溢光跟随高亮';
  }
}
function applyHomeAccentColor() {
  var color = normalizeHexColor(fx.homeAccentColor || '#00f5d4');
  var rgb = hexToRgb(color);
  __cssRoot().style.setProperty('--home-accent', color);
  __cssRoot().style.setProperty('--home-accent-rgb', rgb.r + ',' + rgb.g + ',' + rgb.b);
}
function updateHomeAccentControls() {
  applyHomeAccentColor();
  var color = normalizeHexColor(fx.homeAccentColor || '#00f5d4');
  var picker = byId('home-accent-picker');
  var value = byId('home-accent-value');
  if (picker) picker.value = color;
  if (value) value.textContent = color.toUpperCase();
}
function setHomeAccentColor(color, silent) {
  fx.homeAccentColor = normalizeHexColor(color || '#00f5d4');
  updateHomeAccentControls();
  saveLyricLayout();
  if (!silent) showToast('Home 填充: ' + fx.homeAccentColor.toUpperCase());
}
function resetHomeAccentColor() {
  setHomeAccentColor(fxDefaults.homeAccentColor || '#00f5d4');
}
function applyIconAccentColors() {
  var homeColor = normalizeHexColor(fx.homeIconColor || fxDefaults.homeIconColor || '#f4d28a', '#f4d28a');
  var visualColor = normalizeHexColor(fx.visualIconColor || fxDefaults.visualIconColor || '#7fd8ff', '#7fd8ff');
  var homeRgb = hexToRgb(homeColor);
  var visualRgb = hexToRgb(visualColor);
  var root = __cssRoot();
  root.style.setProperty('--home-icon-color', homeColor);
  root.style.setProperty('--home-icon-rgb', homeRgb.r + ',' + homeRgb.g + ',' + homeRgb.b);
  root.style.setProperty('--visual-icon-color', visualColor);
  root.style.setProperty('--visual-icon-rgb', visualRgb.r + ',' + visualRgb.g + ',' + visualRgb.b);
}
function updateIconAccentControls() {
  applyIconAccentColors();
  var homeColor = normalizeHexColor(fx.homeIconColor || fxDefaults.homeIconColor || '#f4d28a', '#f4d28a');
  var visualColor = normalizeHexColor(fx.visualIconColor || fxDefaults.visualIconColor || '#7fd8ff', '#7fd8ff');
  var homePicker = byId('home-icon-picker');
  var homeValue = byId('home-icon-value');
  var visualPicker = byId('visual-icon-picker');
  var visualValue = byId('visual-icon-value');
  if (homePicker) homePicker.value = homeColor;
  if (homeValue) homeValue.textContent = homeColor.toUpperCase();
  if (visualPicker) visualPicker.value = visualColor;
  if (visualValue) visualValue.textContent = visualColor.toUpperCase();
}
function setHomeIconColor(color, silent) {
  fx.homeIconColor = normalizeHexColor(color || fxDefaults.homeIconColor || '#f4d28a', '#f4d28a');
  updateIconAccentControls();
  saveLyricLayout();
  if (!silent) showToast('主页图标: ' + fx.homeIconColor.toUpperCase());
}
function resetHomeIconColor() {
  setHomeIconColor(fxDefaults.homeIconColor || '#f4d28a');
}
function setVisualIconColor(color, silent) {
  fx.visualIconColor = normalizeHexColor(color || fxDefaults.visualIconColor || '#7fd8ff', '#7fd8ff');
  updateIconAccentControls();
  saveLyricLayout();
  if (!silent) showToast('视觉图标: ' + fx.visualIconColor.toUpperCase());
}
function resetVisualIconColor() {
  setVisualIconColor(fxDefaults.visualIconColor || '#7fd8ff');
}
function applyCustomBackground() {
  var color = normalizeHexColor(fx.backgroundColor || '#000000', '#000000');
  var media = normalizeCustomBackgroundMedia(fx.backgroundMedia || fx.backgroundImage);
  var image = media && media.type === 'image' ? media.src : '';
  var hasVideo = !!(media && media.type === 'video');
  var opacity = clampRange(fx.backgroundOpacity == null ? 1 : Number(fx.backgroundOpacity), 0, 1);
  var customColor = fx.backgroundColorMode === 'custom' || !!fx.backgroundColorCustom;
  var override = !!media || customColor || opacity < 1;
  var root = __cssRoot();
  var layer = byId('custom-bg');
  var video = byId('custom-bg-video');
  root.style.setProperty('--custom-bg-color', color);
  __stateRoot().classList.toggle('custom-background-override', override);
  __stateRoot().classList.toggle('custom-background-flat', override && !media);
  __stateRoot().classList.toggle('custom-background-video', hasVideo);
  if (layer) {
    layer.style.setProperty('--custom-bg-image', image ? 'url("' + cssImageUrl(image) + '")' : 'none');
    layer.style.setProperty('--custom-bg-image-opacity', image ? opacity.toFixed(3) : '0');
    layer.style.setProperty('--custom-bg-video-opacity', hasVideo ? opacity.toFixed(3) : '0');
    layer.style.setProperty('--custom-bg-overlay-opacity', media ? '0.18' : '0');
  }
  var token = ++customBgApplyToken;
  if (!video) return;
  if (!hasVideo) {
    video.pause();
    video.removeAttribute('src');
    video.load();
    if (customBgObjectUrl) { URL.revokeObjectURL(customBgObjectUrl); customBgObjectUrl = ''; }
    return;
  }
  function setVideoSrc(src) {
    if (token !== customBgApplyToken || !src) return;
    if (customBgObjectUrl && customBgObjectUrl !== src) { URL.revokeObjectURL(customBgObjectUrl); customBgObjectUrl = ''; }
    if (video.getAttribute('src') !== src) {
      video.setAttribute('src', src);
      video.load();
    }
    video.muted = true;
    video.loop = true;
    video.playsInline = true;
    var p = video.play();
    if (p && p.catch) p.catch(function(){});
  }
  if (media.src) {
    setVideoSrc(media.src);
  } else if (media.id) {
    getCustomBackgroundBlob(media.id).then(function(blob){
      if (token !== customBgApplyToken || !blob) return;
      if (customBgObjectUrl) URL.revokeObjectURL(customBgObjectUrl);
      customBgObjectUrl = URL.createObjectURL(blob);
      setVideoSrc(customBgObjectUrl);
    }).catch(function(err){ console.warn('background video load failed:', err); });
  }
}
function updateCustomBackgroundControls() {
  applyCustomBackground();
  var color = normalizeHexColor(fx.backgroundColor || '#000000', '#000000');
  var picker = byId('bg-color-picker');
  var value = byId('bg-color-value');
  var imageValue = byId('bg-image-value');
  var customColor = fx.backgroundColorMode === 'custom' || !!fx.backgroundColorCustom;
  if (picker) picker.value = color;
  if (value) value.textContent = customColor ? color.toUpperCase() : '\u5c01\u9762\u6e10\u53d8';
  if (picker && picker.closest) {
    var row = picker.closest('.lyric-color-row');
    if (row) row.classList.toggle('bg-cover-mode', !customColor);
  }
  setRange('fx-bgopacity', fx.backgroundOpacity == null ? 1 : fx.backgroundOpacity);
  if (imageValue) imageValue.textContent = customBackgroundMediaLabel(fx.backgroundMedia || fx.backgroundImage);
  applyBackgroundMediaHint();
}
function setCustomBackgroundColor(color, silent, customFlag) {
  fx.backgroundColor = normalizeHexColor(color || '#000000', '#000000');
  fx.backgroundColorMode = customFlag === false ? 'cover' : 'custom';
  fx.backgroundColorCustom = customFlag !== false;
  updateCustomBackgroundControls();
  saveLyricLayout();
  if (!silent) showToast('背景颜色: ' + fx.backgroundColor.toUpperCase());
}
function setCustomBackgroundCoverMode(silent) {
  fx.backgroundColorMode = 'cover';
  fx.backgroundColorCustom = false;
  fx.backgroundColor = normalizeHexColor(fx.backgroundColor || fxDefaults.backgroundColor || '#000000', '#000000');
  updateCustomBackgroundControls();
  saveLyricLayout();
  if (!silent) showToast('\u80cc\u666f\u989c\u8272: \u5c01\u9762\u6e10\u53d8');
}
function resetCustomBackgroundColor() {
  setCustomBackgroundCoverMode(false);
}
function setCustomBackgroundOpacity(value, silent) {
  fx.backgroundOpacity = clampRange(Number(value), 0, 1);
  fx.backgroundColorMode = 'custom';
  fx.backgroundColorCustom = true;
  updateCustomBackgroundControls();
  saveLyricLayout();
  if (!silent) showToast('背景透明度: ' + Math.round(fx.backgroundOpacity * 100) + '%');
}
function setCustomBackgroundImage(src, silent) {
  var image = normalizeCustomBackgroundImage(src);
  fx.backgroundImage = image;
  fx.backgroundMedia = image ? { type: 'image', src: image } : null;
  updateCustomBackgroundControls();
  saveLyricLayout();
  if (!silent) showToast(fx.backgroundImage ? '背景图片已应用' : '背景图片已清除');
}
function clearCustomBackgroundImage() {
  setCustomBackgroundImage('');
}
function setCustomBackgroundMedia(media, silent) {
  media = normalizeCustomBackgroundMedia(media);
  fx.backgroundMedia = media;
  fx.backgroundImage = media && media.type === 'image' ? media.src : '';
  updateCustomBackgroundControls();
  saveLyricLayout();
  if (!silent) showToast(media ? (media.type === 'video' ? '背景视频已应用' : '背景图片已应用') : '背景媒体已清除');
}
function readBackgroundImageFile(file) {
  if (!file || !/^image\//i.test(file.type || '')) {
    showToast('请选择图片文件');
    return;
  }
  var reader = new FileReader();
  reader.onload = function(e) {
    var img = new Image();
    img.onload = function() {
      var maxSide = 2200;
      var iw = img.naturalWidth || img.width || 1;
      var ih = img.naturalHeight || img.height || 1;
      var scale = Math.min(1, maxSide / Math.max(iw, ih));
      var w = Math.max(1, Math.round(iw * scale));
      var h = Math.max(1, Math.round(ih * scale));
      var cv = document.createElement('canvas');
      cv.width = w; cv.height = h;
      var cx = cv.getContext('2d');
      cx.drawImage(img, 0, 0, w, h);
      var out = '';
      try { out = cv.toDataURL('image/webp', 0.84); } catch (err) {}
      if (!/^data:image\/webp/i.test(out)) {
        try { out = cv.toDataURL('image/jpeg', 0.86); } catch (err2) { out = String(e.target.result || ''); }
      }
      setCustomBackgroundImage(out);
    };
    img.onerror = function(){ showToast('背景图片读取失败'); };
    img.src = e.target.result;
  };
  reader.onerror = function(){ showToast('背景图片读取失败'); };
  reader.readAsDataURL(file);
}
function readBackgroundVideoFile(file) {
  if (!file || !/^video\//i.test(file.type || '')) {
    showToast('请选择视频文件');
    return;
  }
  var id = 'bg-video-' + Date.now() + '-' + Math.random().toString(16).slice(2);
  putCustomBackgroundBlob(id, file, { name: file.name || '', mime: file.type || '', size: file.size || 0 }).then(function(){
    setCustomBackgroundMedia({ type: 'video', id: id, name: file.name || '', mime: file.type || '', size: file.size || 0 });
  }).catch(function(err){
    console.warn('background video store failed:', err);
    if ((file.size || 0) > 18 * 1024 * 1024) {
      showToast('视频较大，当前环境无法保存，请换小一点的视频');
      return;
    }
    var reader = new FileReader();
    reader.onload = function(e){
      setCustomBackgroundMedia({ type: 'video', src: String(e.target.result || ''), name: file.name || '', mime: file.type || '', size: file.size || 0 });
    };
    reader.onerror = function(){ showToast('背景视频读取失败'); };
    reader.readAsDataURL(file);
  });
}
function readBackgroundMediaFile(file) {
  if (!file) return;
  if (/^image\//i.test(file.type || '')) readBackgroundImageFile(file);
  else if (/^video\//i.test(file.type || '')) readBackgroundVideoFile(file);
  else showToast('请选择图片或视频文件');
}
function applyUiAccentColor() {
  var color = normalizeHexColor(fx.uiAccentColor || '#00f5d4', '#00f5d4');
  var rgb = hexToRgb(color);
  var root = __cssRoot();
  root.style.setProperty('--fc-accent', color);
  root.style.setProperty('--fc-accent-hov', color);
  root.style.setProperty('--fc-accent-rgb', rgb.r + ',' + rgb.g + ',' + rgb.b);
  root.style.setProperty('--glass-border', 'rgba(' + rgb.r + ',' + rgb.g + ',' + rgb.b + ',.30)');
  root.style.setProperty('--glass-shadow-focus', '0 24px 72px rgba(0,0,0,.34),0 0 0 1px rgba(' + rgb.r + ',' + rgb.g + ',' + rgb.b + ',.13),0 0 42px rgba(' + rgb.r + ',' + rgb.g + ',' + rgb.b + ',.075),inset 0 1px 0 rgba(255,255,255,.20)');
}
function updateUiAccentControls() {
  applyUiAccentColor();
  var color = normalizeHexColor(fx.uiAccentColor || '#00f5d4', '#00f5d4');
  var picker = byId('ui-accent-picker');
  var value = byId('ui-accent-value');
  if (picker) picker.value = color;
  if (value) value.textContent = color.toUpperCase();
}
function setUiAccentColor(color, silent) {
  fx.uiAccentColor = normalizeHexColor(color || '#00f5d4', '#00f5d4');
  updateUiAccentControls();
  if (shelfManager && shelfManager.refreshTheme) shelfManager.refreshTheme();
  saveLyricLayout();
  if (!silent) showToast('界面高亮: ' + fx.uiAccentColor.toUpperCase());
}
function resetUiAccentColor() {
  setUiAccentColor(fxDefaults.uiAccentColor || '#00f5d4');
}
function updateVisualTintControls() {
  var picker = byId('visual-tint-picker');
  var value = byId('visual-tint-value');
  var autoBtn = byId('visual-tint-auto-btn');
  var color = normalizeHexColor(fx.visualTintColor || '#9db8cf');
  __cssRoot().style.setProperty('--visual-tint', color);
  if (picker) picker.value = color;
  if (value) value.textContent = fx.visualTintMode === 'custom' ? color.toUpperCase() : '封面取色';
  if (autoBtn) autoBtn.classList.toggle('active', fx.visualTintMode !== 'custom');
}
function setVisualTintAuto() {
  fx.visualTintMode = 'auto';
  updateVisualTintControls();
  syncFxUniforms();
  saveLyricLayout();
  showToast('视觉主色: 封面取色');
}
function resetVisualTintColor() {
  fx.visualTintMode = 'auto';
  fx.visualTintColor = normalizeHexColor(fxDefaults.visualTintColor || '#9db8cf');
  updateVisualTintControls();
  syncFxUniforms();
  saveLyricLayout();
  showToast('视觉主色已恢复默认');
}
function setVisualTintCustom(color, silent) {
  fx.visualTintMode = 'custom';
  fx.visualTintColor = normalizeHexColor(color || '#9db8cf');
  updateVisualTintControls();
  syncFxUniforms();
  saveLyricLayout();
  if (!silent) showToast('视觉主色: ' + fx.visualTintColor.toUpperCase());
}
var coverColorPickerState = { target: 'visualTint', canvas: null };
function currentCoverPickerCanvas() {
  if (coverPickerCanvas && coverPickerCanvas.getContext) return coverPickerCanvas;
  if (coverTex && coverTex.image && coverTex.image.getContext) return coverTex.image;
  return null;
}
function coverPickerSwatchColors() {
  var pal = stageLyrics.coverPalette || stageLyrics.palette || {};
  var list = [pal.primary, pal.secondary, pal.highlight, fx.visualTintColor, fx.uiAccentColor, fx.homeAccentColor]
    .map(function(c){ return normalizeHexColor(c || '', ''); })
    .filter(function(c){ return /^#[0-9a-f]{6}$/i.test(c); });
  var seen = {};
  return list.filter(function(c){
    if (seen[c]) return false;
    seen[c] = true;
    return true;
  }).slice(0, 5);
}
function setCoverPickerPreview(hex) {
  var preview = byId('cover-color-preview');
  if (preview) preview.style.setProperty('--picked', normalizeHexColor(hex || '#9db8cf'));
}
function renderCoverPickerSwatches() {
  var wrap = byId('cover-color-swatches');
  if (!wrap) return;
  var colors = coverPickerSwatchColors();
  wrap.innerHTML = colors.map(function(c){
    return '<button type="button" style="--c:' + c + '" title="' + c.toUpperCase() + '" onclick="applyCoverPickerColor(\'' + c + '\')"></button>';
  }).join('');
}
function openCoverColorPicker(target) {
  target = target || 'visualTint';
  var pop = byId('cover-color-pop');
  var art = byId('cover-color-art');
  var hint = byId('cover-color-hint');
  if (pop && pop.classList.contains('show') && coverColorPickerState.target === target) {
    closeCoverColorPicker();
    return;
  }
  var cv = currentCoverPickerCanvas();
  coverColorPickerState.target = target;
  coverColorPickerState.canvas = cv;
  if (!pop || !art) return;
  if (!cv) {
    setVisualTintAuto();
    closeCoverColorPicker();
    showToast('暂无封面，已切换为自动封面取色');
    return;
  }
  var imgSrc = '';
  try { imgSrc = cv.toDataURL('image/jpeg', 0.84); } catch (e) {}
  if (!imgSrc && currentCoverSource && currentCoverSource.src) imgSrc = currentCoverSource.src;
  art.style.backgroundImage = imgSrc ? 'url("' + cssImageUrl(imgSrc) + '")' : '';
  setCoverPickerPreview(fx.visualTintColor || (stageLyrics.coverPalette && stageLyrics.coverPalette.primary) || '#9db8cf');
  renderCoverPickerSwatches();
  if (hint) hint.textContent = '点击专辑封面任意位置取色，或使用下方推荐色。';
  pop.classList.add('show');
  placeFxFloatingPanel(pop, byId('visual-tint-auto-btn') || byId('visual-tint-picker') || art, { gap: 12, pad: 14 });
}
function closeCoverColorPicker() {
  var pop = byId('cover-color-pop');
  if (pop) pop.classList.remove('show');
  hideCoverColorLoupe();
}
function applyCoverPickerColor(hex) {
  hex = normalizeHexColor(hex || '#9db8cf');
  setCoverPickerPreview(hex);
  if (coverColorPickerState.target === 'visualTint') {
    setVisualTintCustom(hex, true);
    showToast('视觉主色: ' + hex.toUpperCase());
  }
  closeCoverColorPicker();
}
function moveCoverColorLoupe(e) {
  var cv = coverColorPickerState.canvas || currentCoverPickerCanvas();
  var loupe = byId('cover-color-loupe');
  var art = byId('cover-color-art');
  if (!cv || !loupe || !art) return;
  var rect = art.getBoundingClientRect();
  var x = clampRange((e.clientX - rect.left) / Math.max(1, rect.width), 0, 1);
  var y = clampRange((e.clientY - rect.top) / Math.max(1, rect.height), 0, 1);
  var imgSrc = '';
  try { imgSrc = cv.toDataURL('image/jpeg', 0.84); } catch (err) {}
  if (imgSrc) {
    loupe.style.backgroundImage = 'url("' + cssImageUrl(imgSrc) + '")';
    loupe.style.backgroundSize = '680% 680%';
    loupe.style.backgroundPosition = (x * 100).toFixed(2) + '% ' + (y * 100).toFixed(2) + '%';
  }
  loupe.style.left = Math.min(window.innerWidth - 128, e.clientX + 18) + 'px';
  loupe.style.top = Math.min(window.innerHeight - 128, e.clientY + 18) + 'px';
  loupe.classList.add('show');
}
function hideCoverColorLoupe() {
  var loupe = byId('cover-color-loupe');
  if (loupe) loupe.classList.remove('show');
}
function pickCoverColorFromArt(e) {
  var cv = coverColorPickerState.canvas || currentCoverPickerCanvas();
  if (!cv || !cv.getContext) return;
  var rect = e.currentTarget.getBoundingClientRect();
  var x = clampRange((e.clientX - rect.left) / Math.max(1, rect.width), 0, 1);
  var y = clampRange((e.clientY - rect.top) / Math.max(1, rect.height), 0, 1);
  var sx = Math.max(0, Math.min(cv.width - 1, Math.floor(x * cv.width)));
  var sy = Math.max(0, Math.min(cv.height - 1, Math.floor(y * cv.height)));
  try {
    var data = cv.getContext('2d').getImageData(sx, sy, 1, 1).data;
    applyCoverPickerColor(rgbToHexColor(data[0], data[1], data[2]));
  } catch (err) {
    showToast('封面取色不可用，已保留自动取色');
    setVisualTintAuto();
    closeCoverColorPicker();
  }
}
function updateLyricFontControls() {
  __qsa('#lyric-font-grid button').forEach(function(btn){
    btn.classList.toggle('active', btn.dataset.font === normalizeLyricFontKey(fx.lyricFont));
  });
}
function setLyricFont(key) {
  fx.lyricFont = normalizeLyricFontKey(key);
  updateLyricFontControls();
  refreshCurrentLyricStyle();
  saveLyricLayout();
  pushDesktopLyricsState(true);
  showToast('歌词字体已切换');
}
function setLyricGlowLinked(linked, openPicker) {
  fx.lyricGlowLinked = linked !== false;
  if (!fx.lyricGlowLinked) fx.lyricGlowColor = normalizeHexColor(fx.lyricGlowColor || fx.lyricHighlightColor || '#9db8cf');
  setStageLyricPalette(fx.lyricColorMode === 'custom' ? lyricPaletteFromHex(fx.lyricColor) : (stageLyrics.coverPalette || stageLyrics.palette));
  updateLyricGlowControls();
  saveLyricLayout();
  if (openPicker) {
    setTimeout(function(){
      var picker = byId('lyric-glow-picker');
      if (picker) picker.click();
    }, 0);
  }
}
function toggleLyricGlowLink(e) {
  if (e && e.stopPropagation) e.stopPropagation();
  setLyricGlowLinked(fx.lyricGlowLinked === false);
}
function handleLyricGlowRowClick(e) {
  if (fx.lyricGlowLinked !== false) {
    if (e && e.preventDefault) e.preventDefault();
    setLyricGlowLinked(false, true);
  }
}
function setLyricGlowCustom(color, silent) {
  fx.lyricGlowLinked = false;
  fx.lyricGlowColor = normalizeHexColor(color || '#9db8cf');
  setStageLyricPalette(fx.lyricColorMode === 'custom' ? lyricPaletteFromHex(fx.lyricColor) : (stageLyrics.coverPalette || stageLyrics.palette));
  updateLyricGlowControls();
  saveLyricLayout();
  pushDesktopLyricsState(true);
  if (!silent) showToast('溢光颜色: ' + fx.lyricGlowColor.toUpperCase());
}
function setLyricColorAuto() {
  fx.lyricColorMode = 'auto';
  setStageLyricPalette(stageLyrics.coverPalette || stageLyrics.palette);
  updateLyricColorControls();
  updateLyricHighlightControls();
  updateLyricGlowControls();
  saveLyricLayout();
  pushDesktopLyricsState(true);
  showToast('歌词颜色: 封面取色');
}
function setLyricColorCustom(color, silent) {
  fx.lyricColorMode = 'custom';
  fx.lyricColor = normalizeHexColor(color);
  setStageLyricPalette(lyricPaletteFromHex(fx.lyricColor));
  updateLyricColorControls();
  updateLyricHighlightControls();
  updateLyricGlowControls();
  saveLyricLayout();
  pushDesktopLyricsState(true);
  if (!silent) showToast('歌词颜色: ' + fx.lyricColor.toUpperCase());
}
function setLyricColorPreset(i) {
  var p = lyricColorPresets[i];
  if (!p) return;
  setLyricColorCustom(p.color);
}
function setLyricHighlightAuto() {
  fx.lyricHighlightMode = 'auto';
  setStageLyricPalette(fx.lyricColorMode === 'custom' ? lyricPaletteFromHex(fx.lyricColor) : (stageLyrics.coverPalette || stageLyrics.palette));
  updateLyricHighlightControls();
  updateLyricGlowControls();
  saveLyricLayout();
  pushDesktopLyricsState(true);
  showToast('高亮颜色: 跟随歌词');
}
function setLyricHighlightCustom(color, silent) {
  fx.lyricHighlightMode = 'custom';
  fx.lyricHighlightColor = normalizeHexColor(color);
  setStageLyricPalette(fx.lyricColorMode === 'custom' ? lyricPaletteFromHex(fx.lyricColor) : (stageLyrics.coverPalette || stageLyrics.palette));
  updateLyricHighlightControls();
  updateLyricGlowControls();
  saveLyricLayout();
  pushDesktopLyricsState(true);
  if (!silent) showToast('高亮颜色: ' + fx.lyricHighlightColor.toUpperCase());
}

function buildPresetGrid() {
  var grid = byId('preset-grid');
  if (!grid) return;
  var seen = {};
  var order = presetDisplayOrder.filter(function(id){
    var ok = id >= 0 && id < presetMeta.length && !seen[id];
    seen[id] = true;
    return ok;
  });
  presetMeta.forEach(function(_, id){
    if (!seen[id]) order.push(id);
  });
  grid.innerHTML = order.map(function(i){
    var p = presetMeta[i];
    var desc = p.descHtml || p.desc;
    return '<div class="preset-card" data-preset="' + i + '" onclick="setPreset(' + i + ')">' +
      '<div class="pc-icon">' + presetIcons[i] + '</div>' +
      '<div class="pc-name">' + p.name + '</div>' +
      '<div class="pc-desc">' + desc + '</div>' +
    '</div>';
  }).join('');
  refreshPresetGrid();
}
function refreshPresetGrid() {
  __qsa('.preset-card').forEach(function(el){
    el.classList.toggle('active', Number(el.dataset.preset) === fx.preset);
  });
}
function triggerPresetParticleTransition(fromPreset, toPreset) {
  presetTransition.active = true;
  presetTransition.start = uniforms.uTime.value;
  presetTransition.duration = toPreset === 5 ? 0.30 : 0.24;
  presetTransition.from = fromPreset;
  presetTransition.to = toPreset;
  var newVisual = toPreset >= 4;
  var wallpaperFlow = toPreset === 5;
  uniforms.uScatter.value = Math.max(uniforms.uScatter.value, fx.scatter + (newVisual ? (wallpaperFlow ? 0.008 : 0.024) : 0.12));
  uniforms.uBurstAmt.value = Math.max(uniforms.uBurstAmt.value, wallpaperFlow ? 0.05 : 0.15);
  camPunch = Math.max(camPunch, wallpaperFlow ? 0.04 : 0.12);
  for (var i = 0; i < 3; i++) {
    triggerRipple((Math.random() - 0.5) * 3.4, (Math.random() - 0.5) * 3.4, 0.58 + Math.random() * 0.32);
  }
  var card = document.querySelector('.preset-card[data-preset="' + toPreset + '"]');
  if (card) {
    card.classList.remove('switching');
    void card.offsetWidth;
    card.classList.add('switching');
    setTimeout(function(){ card.classList.remove('switching'); }, 760);
  }
}
function tickPresetTransition() {
  if (!presetTransition.active) return;
  var raw = (uniforms.uTime.value - presetTransition.start) / presetTransition.duration;
  var t = Math.max(0, Math.min(1, raw));
  var wave = Math.sin(t * Math.PI);
  var newVisual = presetTransition.to >= 4;
  var wallpaperFlow = presetTransition.to === 5;
  uniforms.uScatter.value = Math.max(uniforms.uScatter.value, fx.scatter + wave * (newVisual ? (wallpaperFlow ? 0.008 : 0.026) : 0.16));
  uniforms.uBurstAmt.value = Math.max(uniforms.uBurstAmt.value, wave * (wallpaperFlow ? 0.045 : (newVisual ? 0.12 : 0.15)));
  uniforms.uPointScale.value = fx.point * (1 + wave * (wallpaperFlow ? 0.016 : 0.048));
  if (raw >= 1) {
    presetTransition.active = false;
    syncFxUniforms();
  }
}
function setPreset(p, opts) {
  opts = opts || {};
  p = Math.max(0, Math.min(presetMeta.length - 1, Number(p) || 0));
  var prev = fx.preset;
  var changed = prev !== p;
  fx.preset = p;
  if (changed && prev === SKULL_PRESET_INDEX && p !== SKULL_PRESET_INDEX) clearSkullPresetResidue();
  if (p === SKULL_PRESET_INDEX) loadSkullParticleAsset();
  uniforms.uPreset.value = p;
  refreshPresetGrid();
  if (changed && !opts.skipTransition) triggerPresetParticleTransition(prev, p);
  // 每个预设对应的相机基线 (改 userOrbit)
  if (changed && !opts.preserveCamera) {
    if (p === 1)      { orbit.userRadius = 6.2; orbit.userPhi = 0.03; orbit.userTheta = 0.0; orbit.baselineRadius = 6.2; orbit.baselinePhi = 0.03; }
    else if (p === 2) { orbit.userRadius = 7.0; orbit.userPhi = 0.15; orbit.userTheta = 0.0; orbit.baselineRadius = 7.0; orbit.baselinePhi = 0.15; }
    else if (p === 3) { orbit.userRadius = 8.0; orbit.userPhi = 0.05; orbit.userTheta = 0.0; orbit.baselineRadius = 8.0; orbit.baselinePhi = 0.05; }
    else if (p === 4) { orbit.userRadius = 6.5; orbit.userPhi = 0.04; orbit.userTheta = 0.0; orbit.baselineRadius = 6.5; orbit.baselinePhi = 0.04; }
    else if (p === 5) { orbit.userRadius = 9.4; orbit.userPhi = 0.34; orbit.userTheta = -0.52; orbit.baselineRadius = 9.4; orbit.baselinePhi = 0.34; }
    else if (p === 6) { orbit.userRadius = 7.4; orbit.userPhi = 0.10; orbit.userTheta = 0.18; orbit.baselineRadius = 7.4; orbit.baselinePhi = 0.10; }
    else              { orbit.userRadius = 6.6; orbit.userPhi = 0.08; orbit.userTheta = 0.0; orbit.baselineRadius = 6.6; orbit.baselinePhi = 0.08; }
    orbit.baselineTheta = p === 5 ? -0.52 : (p === 6 ? 0.18 : 0.0);
  }
  if (changed && !opts.silent) showToast('视觉预设: ' + presetMeta[p].name);
  var shouldCommitPlaybackPreset = !!opts.commitPlaybackPreset || !opts.noSave;
  if (shouldCommitPlaybackPreset) {
    playbackVisualPreset = p;
    startupVisualPreviewActive = false;
  }
  if (!opts.noSave) {
    saveLyricLayout();
  }
}

function syncFxUniforms() {
  uniforms.uPreset.value = fx.preset;
  uniforms.uIntensity.value = fx.intensity;
  uniforms.uDepth.value = fx.depth;
  uniforms.uPointScale.value = fx.point;
  uniforms.uSpeed.value = fx.speed;
  uniforms.uTwist.value = fx.twist;
  uniforms.uColorBoost.value = fx.color;
  uniforms.uScatter.value = fx.scatter;
  uniforms.uCoverRes.value = normalizeCoverResolution(fx.coverResolution);
  uniforms.uBgFade.value = fx.bgFade;
  uniforms.uBloomStrength.value = fx.bloom ? fx.bloomStrength : 0;
  if (bloomParticles) bloomParticles.visible = fx.bloom && fx.bloomStrength > 0.01;
  uniforms.uEdgeEnabled.value = fx.edge ? 1 : 0;
  if (uniforms.uTintColor) uniforms.uTintColor.value.set(normalizeHexColor(fx.visualTintColor || '#9db8cf'));
  if (uniforms.uTintStrength) uniforms.uTintStrength.value = fx.visualTintMode === 'custom' ? 0.42 : 0;
  syncSkullParticleColors();
}

// ▼▼▼ [mineradio:perf-setters] source lines 21415,21439 ▼▼▼
function setPerformanceBackgroundMode(mode, silent) {
  var next = normalizePerformanceBackgroundMode(mode, false);
  fx.performanceBackground = next;
  fx.liveBackgroundKeep = next === 'keep';
  updatePerformanceControls();
  saveLyricLayout();
  updateRenderPowerClasses();
  applyRendererPowerMode();
  if (next === 'keep') recoverVisualsAfterBackground('performance-background-keep');
  else if (next === 'release' && isDeepBackgroundMode()) trimRuntimeCaches('performance-release', true);
  if (!silent) {
    showToast(next === 'keep' ? '后台策略: 保持运行' : (next === 'release' ? '后台策略: 停止并释放' : '后台策略: 自动优化'));
  }
}
function setPerformanceQualityMode(mode, silent) {
  var next = normalizePerformanceQuality(mode);
  fx.performanceQuality = next;
  updatePerformanceControls();
  applyRendererPowerMode();
  saveLyricLayout();
  if (!silent) {
    var label = next === 'eco' ? '低' : (next === 'balanced' ? '中' : (next === 'ultra' ? '超高' : '高'));
    showToast('画质档位: ' + label);
  }
}

// ▼▼▼ [mineradio:toggle-reset-fx] source lines 22360,22468 ▼▼▼
function toggleFx(key) {
  if (isDevelopmentLockedFx(key)) {
    normalizeDevelopmentLockedFxState();
    saveLyricLayout();
    updateFxInputs();
    applyDesktopLyricsState(true);
    applyWallpaperModeState(true);
    showToast('开发中，暂不可用');
    return;
  }
  fx[key] = !fx[key];
  var toggleId = 't-' + (key === 'floatLayer' ? 'float' : key === 'aiDepth' ? 'aidepth' : key);
  var toggle = byId(toggleId);
  if (toggle) toggle.classList.toggle('on', fx[key]);
  syncFxUniforms();
  if (key === 'lyricCameraLock' || key === 'lyricGlow' || key === 'lyricGlowBeat' || key === 'lyricGlowParticles' || key === 'bloom' || key === 'edge' || key === 'cinema' || key === 'desktopLyrics' || key === 'desktopLyricsClickThrough' || key === 'desktopLyricsCinema' || key === 'desktopLyricsHighlight' || key === 'wallpaperMode' || key === 'shelfShowPodcasts' || key === 'shelfMergeCollections' || key === 'liveBackgroundKeep') saveLyricLayout();
  if (key === 'floatLayer') { if (fx.floatLayer) createFloatLayer(); else destroyFloatLayer(); }
  if (key === 'desktopLyrics') applyDesktopLyricsState(true);
  if (key === 'desktopLyricsClickThrough' || key === 'desktopLyricsCinema' || key === 'desktopLyricsHighlight') pushDesktopLyricsState(true);
  if (key === 'lyricGlow' || key === 'lyricGlowBeat' || key === 'lyricGlowParticles') pushDesktopLyricsState(true);
  if (key === 'wallpaperMode') applyWallpaperModeState(true);
  if (key === 'shelfShowPodcasts' || key === 'shelfMergeCollections') {
    if (shelfManager && shelfManager.rebuild) shelfManager.rebuild(true);
    if (shelfManager && shelfManager.refreshTheme) shelfManager.refreshTheme();
  }
  if (key === 'liveBackgroundKeep') {
    fx.performanceBackground = fx.liveBackgroundKeep ? 'keep' : 'auto';
    updatePerformanceControls();
    saveLyricLayout();
    if (fx.liveBackgroundKeep && backgroundCacheTrimTimer) {
      clearTimeout(backgroundCacheTrimTimer);
      backgroundCacheTrimTimer = 0;
    }
    updateRenderPowerClasses();
    applyRendererPowerMode();
    if (fx.liveBackgroundKeep) recoverVisualsAfterBackground('live-background-keep');
  }
  if (key === 'lyricGlow') showToast(fx.lyricGlow ? '歌词溢光已开启' : '歌词溢光已关闭');
  if (key === 'lyricGlowBeat') showToast(fx.lyricGlowBeat ? '歌词溢光跟随鼓点' : '歌词溢光已脱离鼓点');
  if (key === 'lyricGlowParticles') showToast(fx.lyricGlowParticles ? '歌词光粒已开启' : '歌词光粒已关闭');
  if (key === 'desktopLyrics') showToast(fx.desktopLyrics ? '桌面歌词已开启' : '桌面歌词已关闭');
  if (key === 'desktopLyricsClickThrough') showToast(fx.desktopLyricsClickThrough !== false ? '桌面歌词已锁定' : '桌面歌词可移动');
  if (key === 'desktopLyricsCinema') showToast(fx.desktopLyricsCinema !== false ? '桌面歌词电影震动已开启' : '桌面歌词电影震动已关闭，基础漂浮保留');
  if (key === 'desktopLyricsHighlight') showToast(fx.desktopLyricsHighlight === true ? '桌面歌词高亮跟随已开启' : '桌面歌词高亮跟随已关闭');
  if (key === 'wallpaperMode') showToast(fx.wallpaperMode ? '壁纸模式已开启' : '壁纸模式已关闭');
  if (key === 'shelfShowPodcasts') showToast(fx.shelfShowPodcasts !== false ? '3D歌单架已显示播客歌单' : '3D歌单架已隐藏播客歌单');
  if (key === 'shelfMergeCollections') showToast(fx.shelfMergeCollections === true ? '我的歌单与收藏歌单已合并滚动' : '收藏歌单恢复滚到底切页');
  if (key === 'liveBackgroundKeep') showToast(fx.liveBackgroundKeep ? '直播后台保持已开启' : '直播后台保持已关闭');
  if (key === 'lyricCameraLock') showToast(fx.lyricCameraLock ? '歌词已绑定镜头' : '歌词已恢复自由漂浮');
  if (key === 'bloom') showToast(fx.bloom ? '溢光已开启' : '溢光已关闭');
  if (key === 'edge') showToast(fx.edge ? '已开启轮廓高亮' : '已关闭轮廓高亮');
  if (key === 'cinema') showToast(fx.cinema ? '已开启电影镜头' : '已关闭电影镜头');
  if (key === 'aiDepth') {
    if (fx.aiDepth) {
      aiDepthFailUntil = 0;
      queueAIDepthForCurrentCover(true);
    }
    showToast(fx.aiDepth ? '已开启后台 AI 立体增强' : '已关闭 AI 立体增强, 使用轻量弧面');
  }
}
function toggleFxPanel(force) {
  var el = byId('fx-panel');
  if (!el) return;
  if (!diyPlayerMode && force !== false) {
    showToast('开启 DIY 玩家模式后可打开视觉控制台');
    return;
  }
  var currentlyOpen = el.classList.contains('show') || el.classList.contains('peek');
  if (peekTimers && peekTimers.fx) { clearTimeout(peekTimers.fx); peekTimers.fx = null; }
  fxPanelPinned = false;
  if (force === false) {
    el.classList.remove('show', 'peek');
    el.classList.toggle('closing', currentlyOpen);
    setTimeout(function(){ el.classList.remove('closing'); }, 280);
    var fab = byId('fx-fab');
    if (fab) fab.classList.remove('active');
    return;
  }
  el.classList.remove('show', 'closing');
  setPeek(el, true, 'fx');
}
function resetFx() {
  var savedCam = fx.cam;
  var savedShelf = fx.shelf;
  var savedShelfCameraMode = normalizeShelfCameraMode(fx.shelfCameraMode || fxDefaults.shelfCameraMode);
  var savedShelfPresence = normalizeShelfPresence(fx.shelfPresence || fxDefaults.shelfPresence);
  fx = Object.assign({}, fxDefaults, {
    cam: savedCam,
    shelf: savedShelf,
    shelfCameraMode: savedShelfCameraMode,
    shelfPresence: savedShelfPresence,
    shelfAngleY: shelfDefaultAngleForCameraMode(savedShelfCameraMode),
    shelfAngleYManual: false
  });
  applyCoverParticleResolution(fx.coverResolution, { reload: true });
  updateFxInputs();
  applyDesktopLyricsState(true);
  applyWallpaperModeState(true);
  updateRenderPowerClasses();
  applyRendererPowerMode();
  setStageLyricPalette(stageLyrics.coverPalette || stageLyrics.palette);
  setPreset(fx.preset, { silent: true, preserveCamera: true, skipTransition: true });
  if (fx.floatLayer) createFloatLayer(); else destroyFloatLayer();
  if (shelfManager && shelfManager.rebuild) shelfManager.rebuild(true);
  if (shelfManager && shelfManager.refreshTheme) shelfManager.refreshTheme();
  saveLyricLayout();
  showToast('已恢复默认参数');
}


// ▼▼▼ [mineradio:gesture-spin] source lines 24756,24825 ▼▼▼
function startHeadTracking(){}     // stub: 兼容旧调用
function stopHeadTracking(){}      // stub

var gestureVideo = null, gestureCamera = null, gestureHands = null;
var gestureActive = false;
// 21 个关键点的平滑缓存 (EMA): [{x,y}, ...]
var handLmSmooth = null;
var handLmLastSeen = 0;
// 捏合状态
var pinchState = { active:false, lastX:0, lastY:0, lastT:0 };
// 物理旋转: 给 particles 一个角速度, 每帧衰减
var particleSpin = { vx: 0, vy: 0, damping: 0.90 };
// 手势驱动的总旋转 (累计角度), 输出到 particles
var gestureRotation = { x: 0, y: 0 };
var gestureGrip = { value: 0, target: 0, openness: 1, lastState: 'open', pulse: 0 };
var PARTICLE_POINTER_SPIN_X = 0.0032;
var PARTICLE_POINTER_SPIN_Y = 0.0034;
var PARTICLE_HAND_SPIN_X = 4.15;
var PARTICLE_HAND_SPIN_Y = 4.30;
var PARTICLE_SPIN_MAX = 6.2;

function clampParticleSpinVelocity(v) {
  if (!isFinite(v)) return 0;
  return Math.max(-PARTICLE_SPIN_MAX, Math.min(PARTICLE_SPIN_MAX, v));
}

function applyParticleSpinDrag(dx, dy, dt) {
  var rx = dy * PARTICLE_POINTER_SPIN_X;
  var ry = dx * PARTICLE_POINTER_SPIN_Y;
  gestureRotation.x += rx;
  gestureRotation.y += ry;
  if (dt > 0) {
    particleSpin.vx = clampParticleSpinVelocity(rx / dt * 0.46);
    particleSpin.vy = clampParticleSpinVelocity(ry / dt * 0.46);
  }
}

function resetParticleRotationTarget(syncVisual) {
  gestureRotation.x = 0;
  gestureRotation.y = 0;
  particleSpin.vx = 0;
  particleSpin.vy = 0;
  if (syncVisual && particles) {
    particles.rotation.set(0, 0, 0);
    if (bloomParticles) bloomParticles.rotation.set(0, 0, 0);
    if (floatGroup) floatGroup.rotation.set(0, 0, 0);
    if (backCoverGroup) backCoverGroup.rotation.set(0, 0, 0);
  }
}

function rebaseParticleRotationAxis(axis) {
  var limit = Math.PI * 10;
  if (Math.abs(gestureRotation[axis]) < limit) return;
  var offset = Math.round(gestureRotation[axis] / (Math.PI * 2)) * Math.PI * 2;
  gestureRotation[axis] -= offset;
  if (particles) particles.rotation[axis] -= offset;
  if (bloomParticles) bloomParticles.rotation[axis] -= offset;
  if (floatGroup) floatGroup.rotation[axis] -= offset;
  if (backCoverGroup) backCoverGroup.rotation[axis] -= offset;
  if (skullParticleGroup) skullParticleGroup.rotation[axis] -= offset;
  if (stageLyrics.group) stageLyrics.group.rotation[axis] -= offset;
}

function rebaseParticleRotationIfNeeded() {
  rebaseParticleRotationAxis('x');
  rebaseParticleRotationAxis('y');
}
// 手骨架 canvas
var handCanvas = null, handCanvasCtx = null;
// 平滑系数 (越小越平滑, 但反应越慢)

// ▼▼▼ [mineradio:gesture-tick] source lines 25113,25145 ▼▼▼
function tickGestureRotation(dt) {
  if (Math.abs(particleSpin.vx) > 0.0001 || Math.abs(particleSpin.vy) > 0.0001) {
    var rx = particleSpin.vx * dt;
    var ry = particleSpin.vy * dt;
    gestureRotation.x += rx;
    gestureRotation.y += ry;
    rebaseParticleRotationIfNeeded();
  }
  particleSpin.vx *= Math.pow(particleSpin.damping, dt * 60);
  particleSpin.vy *= Math.pow(particleSpin.damping, dt * 60);
  if (Math.abs(particleSpin.vx) < 0.01) particleSpin.vx = 0;
  if (Math.abs(particleSpin.vy) < 0.01) particleSpin.vy = 0;
  gestureGrip.value += (gestureGrip.target - gestureGrip.value) * (gestureGrip.target > gestureGrip.value ? 0.18 : 0.10);
  gestureGrip.pulse *= Math.pow(0.84, dt * 60);
  if (uniforms.uGestureGrip) uniforms.uGestureGrip.value = clampRange(gestureGrip.value + gestureGrip.pulse * 0.16, 0, 1);
  // hand active 自然衰减 (无手时)
  if (gestureActive && handLmSmooth && performance.now() - handLmLastSeen > 200) {
    uniforms.uHandActive.value *= 0.94;
    gestureGrip.target *= 0.92;
    if (uniforms.uHandActive.value < 0.02) uniforms.uHandActive.value = 0;
  }
}

function showGestureHUD(label, progress, detail) {
  var hud = byId('gesture-hud');
  if (!hud) return;
  byId('gesture-label').textContent = label || '待命';
  byId('gesture-confirm').textContent = detail || '将手放进摄像头视野';
  var fill = byId('gesture-fill');
  if (fill) fill.style.width = Math.max(0, Math.min(100, (progress || 0) * 100)) + '%';
  hud.classList.add('show');
}
function showGestureCursor(){}  // stub: 兼容旧调用

// ▼▼▼ [mineradio:viewport-refresh] source lines 25152,25167 ▼▼▼
function refreshMainRendererViewport(reason) {
  if (typeof camera !== 'undefined' && camera) {
    camera.aspect = Math.max(1, innerWidth) / Math.max(1, innerHeight);
    camera.updateProjectionMatrix();
  }
  applyRendererPowerMode();
  if (typeof requestStageLyricCameraSnap === 'function' && (desktopRuntimeState.fullscreen || document.fullscreenElement)) {
    requestStageLyricCameraSnap(reason === 'resize' ? 4 : 10);
  }
}
function scheduleMainRendererViewportRefresh(reason) {
  refreshMainRendererViewport(reason || 'sync');
  [48, 140, 320].forEach(function(delay){
    setTimeout(function(){ refreshMainRendererViewport(reason || 'sync'); }, delay);
  });
}

// ▼▼▼ [mineradio:main-loop] source lines 26564,26876 ▼▼▼
// ============================================================
//  主循环
// ============================================================
var prevTime = performance.now();
var renderPerfState = {
  mode: 'vsync',
  fps: 0,
  frames: 0,
  skipped: 0,
  longFrames: 0,
  lastRenderAt: 0,
  lastSampleAt: performance.now()
};
window.__mineradioPerf = renderPerfState;
var splashWarmRenderLast = 0;
function isMainSceneCoveredBySplash() {
  return __stateRoot().classList.contains('splash-active') && !__stateRoot().classList.contains('splash-revealing');
}
function getAdaptiveRenderFps() {
  if (isDeepBackgroundMode()) return 1;
  if (RENDER_VISIBLE_VSYNC) return 0;
  var tier = (typeof getRenderLoadTier === 'function') ? getRenderLoadTier() : 0;
  if (typeof isRenderInteractionActive === 'function' && isRenderInteractionActive()) {
    if (tier >= 2) return RENDER_INTERACTION_HUGE_FPS;
    if (tier >= 1) return RENDER_INTERACTION_LARGE_FPS;
    return RENDER_INTERACTION_FPS;
  }
  if (tier >= 2) return RENDER_HUGE_FPS;
  if (tier >= 1) return RENDER_LARGE_FPS;
  return RENDER_ACTIVE_FPS;
}
function shouldSkipAdaptiveRenderFrame(now) {
  var fps = getAdaptiveRenderFps();
  renderPerfState.mode = fps ? (fps + 'fps') : 'vsync';
  if (!fps) {
    renderPerfState.lastRenderAt = now;
    return false;
  }
  var minGap = 1000 / fps;
  if (now - renderPerfState.lastRenderAt < minGap) {
    renderPerfState.skipped += 1;
    return true;
  }
  renderPerfState.lastRenderAt = now;
  return false;
}
function sampleRenderPerf(now, dt) {
  renderPerfState.frames += 1;
  if (dt > 0.034) renderPerfState.longFrames += 1;
  if (now - renderPerfState.lastSampleAt >= 1000) {
    renderPerfState.fps = Math.round(renderPerfState.frames * 1000 / Math.max(1, now - renderPerfState.lastSampleAt));
    renderPerfState.frames = 0;
    renderPerfState.lastSampleAt = now;
  }
  maybeTrimRuntimeCaches(now);
}
function animate() {
  if (__disposed) return;
  requestAnimationFrame(animate);
  var now = performance.now();
  if (shouldSkipAdaptiveRenderFrame(now)) return;
  var dt = Math.min((now - prevTime) / 1000, 0.05);
  prevTime = now;
  sampleRenderPerf(now, dt);
  uniforms.uTime.value += dt;
  if (isMainSceneCoveredBySplash()) {
    if (now - splashWarmRenderLast > 520) {
      splashWarmRenderLast = now;
      renderer.render(scene, camera);
    }
    return;
  }
  pointerParallax.x += (pointerTarget.x - pointerParallax.x) * 0.040;
  pointerParallax.y += (pointerTarget.y - pointerParallax.y) * 0.040;

  // 频谱分析 — v7.1: 真正分离 kick 和人声
  // bin = sampleRate / fftSize = 44100/2048 ≈ 21.5Hz
  // kick 60-150Hz → bin 3-7 (用前 5 个 bin)
  // vocal 200-3000Hz → bin 9-140 (尽量不计入 bass/mid 的"鼓点"判断)
  // 真正的 mid 乐器/和声: 3000-6000Hz → bin 140-280
  // treble: 6000Hz+ → bin 280+
  beatOnsetFlag = false;
  if (analyser && playing && audio && !audio.paused) {
    if (audioCtx && audioCtx.state === 'suspended') resumeAudioAnalysis();
    analyser.getByteFrequencyData(frequencyData);
    analyser.getByteTimeDomainData(timeDomainData);
    var len = frequencyData.length;
    // 精确频段
    var kickEnd  = 7;                          // 60-150 Hz, 鼓 kick
    var vocalEnd = Math.min(len, 140);         // 200-3000 Hz, 人声主体
    var midEnd   = Math.min(len, 280);         // 3-6 kHz, 中高乐器
    // 累积
    var bKick = 0, mInst = 0, tHigh = 0, voc = 0, rms = 0;
    for (var i = 0; i < kickEnd; i++) bKick += frequencyData[i] / 255;
    for (var i = kickEnd; i < vocalEnd; i++) voc += frequencyData[i] / 255;
    for (var i = vocalEnd; i < midEnd; i++) mInst += frequencyData[i] / 255;
    for (var i = midEnd; i < len; i++) tHigh += frequencyData[i] / 255;
    for (var j = 0; j < timeDomainData.length; j++) {
      var tv = (timeDomainData[j] - 128) / 128;
      rms += tv * tv;
    }
    bKick /= kickEnd;
    voc /= (vocalEnd - kickEnd);
    mInst /= Math.max(1, midEnd - vocalEnd);
    tHigh /= Math.max(1, len - midEnd);
    rms = Math.sqrt(rms / timeDomainData.length);

    // 动态峰值跟踪
    bassPeak = Math.max(bassPeak * 0.994, bKick, 0.030);
    midPeak  = Math.max(midPeak  * 0.993, mInst, 0.026);
    treblePeak = Math.max(treblePeak * 0.992, tHigh, 0.018);
    energyPeak = Math.max(energyPeak * 0.995, rms, 0.030);

    var rb = Math.min(1, Math.pow(bKick / Math.max(0.038, bassPeak * 0.66), 0.78));
    var rm = Math.min(1, Math.pow(mInst / Math.max(0.025, midPeak  * 0.70), 0.86));
    var rt = Math.min(1, Math.pow(tHigh / Math.max(0.020, treblePeak * 0.74), 0.92));
    var re = Math.min(1, Math.pow(rms / Math.max(0.034, energyPeak * 0.68), 0.82));

    var bassOnset = Math.max(0, rb - smoothBass);
    var energyOnset = Math.max(0, re - prevEnergy);
    prevEnergy = prevEnergy * 0.88 + re * 0.12;

    var realtimeBeat = processRealtimeBeatEngine(dt);
    if (realtimeBeat && realtimeBeat.hit) {
      var dj = djMode.active;
      var djMapCoversCurrentTime = !dj || !currentDjBeatMap || !currentDjBeatMap.partialUntilSec || !audio || (audio.currentTime || 0) <= currentDjBeatMap.partialUntilSec - 1.25;
      var djBeatMapReadyForCamera = dj && currentDjBeatMap && currentDjBeatMap.cameraBeats && currentDjBeatMap.cameraBeats.length >= 4 && djMapCoversCurrentTime;
      var beatMapReadyForCamera = dj ? djBeatMapReadyForCamera : (currentBeatMap && currentBeatMap.cameraBeats && currentBeatMap.cameraBeats.length >= 4);
      var waitingForBeatMap = dj ? !djBeatMapReadyForCamera : (!beatMapReadyForCamera && (!!beatMapBusy || !!beatAnalysisTimer || ((audio && audio.currentTime) || 0) < 18));
      var liveKickFrame = dj
        ? (realtimeBeat.low > 0.48 && rb > 0.38 && bassOnset > 0.055 && energyOnset > 0.010 && (realtimeBeat.lowDominance || 0) > 0.82)
        : (realtimeBeat.low > 0.50 && rb > 0.42 && bassOnset > 0.070 && energyOnset > 0.016);
      var liveStrongHit = dj
        ? (realtimeBeat.confidence > 0.60 && realtimeBeat.strength > 0.56 && realtimeBeat.score > 0.50 && liveKickFrame)
        : (realtimeBeat.confidence > 0.76 && realtimeBeat.strength > 0.70 && realtimeBeat.score > 0.56 && liveKickFrame);
      var liveTempoHit = dj
        ? (realtimeBeat.tempoAssist && realtimeBeat.confidence > 0.62 && realtimeBeat.strength > 0.52 && realtimeBeat.low > 0.48 && (liveKickFrame || bassOnset > 0.046))
        : (realtimeBeat.tempoAssist && realtimeBeat.confidence > 0.80 && realtimeBeat.strength > 0.66 && realtimeBeat.low > 0.50 && bassOnset > 0.052);
      var liveFallbackOk = dj
        ? (liveStrongHit || liveTempoHit)
        : (waitingForBeatMap
          ? (liveStrongHit || liveTempoHit)
          : (realtimeBeat.confidence > 0.84 && realtimeBeat.strength > 0.80 && realtimeBeat.low > 0.54 && (liveKickFrame || realtimeBeat.score > 0.68)));
      if (!beatMapReadyForCamera && liveFallbackOk) {
        scheduleBeatCamera({
          time: realtimeBeat.time,
          strength: realtimeBeat.strength,
          confidence: realtimeBeat.confidence,
          low: realtimeBeat.low,
          body: realtimeBeat.body,
          snap: realtimeBeat.snap,
          mass: realtimeBeat.mass,
          sharpness: realtimeBeat.sharpness,
          combo: realtimeBeat.combo,
          impact: clamp01(realtimeBeat.strength * 0.46 + realtimeBeat.confidence * 0.20 + realtimeBeat.low * 0.28),
          preview: waitingForBeatMap,
          primary: true,
          dj: dj
        }, 'live');
      }
      if (!beatMapReadyForCamera && liveFallbackOk) {
        var previewPulseScale = waitingForBeatMap && !dj ? 0.68 : 1;
        var rtPulse = Math.min(dj ? 0.34 : (waitingForBeatMap ? 0.46 : 0.62), realtimeBeat.strength * (realtimeBeat.tempoAssist ? (dj ? 0.42 : 0.62) : (dj ? 0.48 : 0.68)) * previewPulseScale);
        if (rtPulse > beatPulse + 0.09) beatOnsetFlag = true;
        beatPulse = Math.max(beatPulse, rtPulse);
      }
    } else if (bassOnset > 0.075 && rb > 0.32 && energyOnset > 0.020) {
      beatPulse = Math.max(beatPulse, Math.min(0.12, bassOnset * 0.18));
    }
    beatPulse *= Math.pow(0.36, dt);

    // v7.2+: 预解析 beatmap 只在实时引擎暂时没锁住时补位.
    tickPodcastDjBeatMap();
    tickBeatMap();
    if (scheduledBeatFlag) {
      beatOnsetFlag = true;
      scheduledBeatFlag = false;
    }
    // scheduledBeatPulse 衰减并合并到 beatPulse
    if (scheduledBeatPulse > beatPulse) beatPulse = scheduledBeatPulse;
    scheduledBeatPulse *= Math.pow(0.32, dt);

    function env(prev, next, attack, release) {
      var k = next > prev ? attack : release;
      return prev + (next - prev) * k;
    }
    // smoothBass 主要由 kick 驱动 (不被人声干扰)
    smoothBass  = env(smoothBass, Math.min(0.82, rb * 0.78 + re * 0.025), 0.28, 0.075);
    // smoothMid 用 中高乐器, 不再混入人声
    smoothMid   = env(smoothMid,  Math.min(0.68, rm * 0.64 + re * 0.025), 0.18, 0.060);
    smoothTreb  = env(smoothTreb, Math.min(0.56, rt * 0.54), 0.18, 0.055);
    smoothEnergy= env(smoothEnergy, Math.min(0.72, re), 0.16, 0.055);
    updateCinemaDynamics(re, rb);
    updateCinemaTrackProfile({ energy: re, low: rb, vocal: voc, melody: rm, lowOnset: bassOnset, energyOnset: energyOnset });
    // 歌词阳光溢光: 独立于律动强度, 看持续能量 + 中高频抬升, 更像副歌/高音段落而不是单个鼓点.
    var sunEnergy = clamp01((smoothEnergy - 0.18) / 0.38);
    var sunVoice = clamp01((voc - 0.11) / 0.34);
    var sunMelody = clamp01((smoothMid - 0.16) / 0.27);
    var sunAir = clamp01((smoothTreb - 0.105) / 0.17);
    var sunRaw = clamp01(sunEnergy * 0.36 + sunVoice * 0.18 + sunMelody * 0.26 + sunAir * 0.20);
    sunRaw = sunRaw * sunRaw * (3 - 2 * sunRaw);
    lyricSunAvg += (sunRaw - lyricSunAvg) * 0.006;
    lyricSunPeak = Math.max(0.48, lyricSunPeak * 0.9985, sunRaw);
    var sunThreshold = Math.max(0.78, lyricSunAvg + 0.20, lyricSunPeak * 0.74);
    var sunGate = clamp01((sunRaw - sunThreshold) / Math.max(0.08, 1.0 - sunThreshold));
    sunGate = sunGate * sunGate * (3 - 2 * sunGate);
    lyricSunHold += (sunGate - lyricSunHold) * (sunGate > lyricSunHold ? 0.035 : 0.014);
    lyricSunTarget = lyricSunHold > 0.16 ? clamp01((lyricSunHold - 0.16) / 0.84) : 0;
    lyricSunEnergy += (lyricSunTarget - lyricSunEnergy) * (lyricSunTarget > lyricSunEnergy ? 0.075 : 0.030);
  } else {
    smoothBass *= 0.91; smoothMid *= 0.91; smoothTreb *= 0.91; smoothEnergy *= 0.91; beatPulse *= 0.82;
    liveCamAvg *= 0.94;
    liveCamPeak = Math.max(0.28, liveCamPeak * 0.98);
    liveCamLastRaw *= 0.80;
    lyricSunTarget = 0;
    lyricSunHold *= 0.90;
    lyricSunEnergy *= 0.92;
    lyricSunAvg *= 0.995;
    lyricSunPeak = Math.max(0.48, lyricSunPeak * 0.997);
  }
  audioEnergy = Math.max(smoothEnergy, beatPulse * 0.30);
  bass = Math.min(0.90, smoothBass * 1.05 + beatPulse * 0.18) * fx.intensity;
  mid  = Math.min(0.72, smoothMid * 1.12) * fx.intensity;
  treble = Math.min(0.62, smoothTreb * 1.20) * fx.intensity;
  if (fx.preset >= 4) {
    var wallpaperAudio = fx.preset === 5;
    var ringBass = smoothBass * (wallpaperAudio ? 1.10 : 1.58) + beatPulse * (wallpaperAudio ? 0.18 : 0.42) - smoothMid * 0.16 - smoothTreb * 0.06;
    var ringMid = smoothMid * (wallpaperAudio ? 1.16 : 1.82) - smoothBass * 0.14 - smoothTreb * 0.07;
    var ringTreble = smoothTreb * (wallpaperAudio ? 1.34 : 2.28) - smoothMid * 0.10 - smoothBass * 0.05;
    bass = Math.pow(clamp01((ringBass - 0.050) / 0.58), 0.72) * fx.intensity;
    mid = Math.pow(clamp01((ringMid - 0.045) / 0.46), 0.78) * fx.intensity;
    treble = Math.pow(clamp01((ringTreble - 0.030) / 0.34), 0.84) * fx.intensity;
    if (wallpaperAudio) {
      bass = Math.min(bass, 0.46 * fx.intensity);
      mid = Math.min(mid, 0.40 * fx.intensity);
      treble = Math.min(treble, 0.36 * fx.intensity);
      beatPulse *= 0.34;
    }
  }
  if (djMode.active) {
    bass = Math.min(1.00, bass * 1.06 + beatPulse * 0.085);
    mid = Math.min(0.76, mid * 1.00 + clamp01(djMode.sectionChange * 1.6) * 0.020);
    treble = Math.min(0.66, treble * 0.98);
    audioEnergy = Math.max(audioEnergy, beatPulse * 0.38, djMode.sectionEnergy * 0.54);
  }

  var vinylSpeedMul = isFinite(fx.speed) ? Math.max(0.05, fx.speed) : 1;
  var vinylSpinSpeed = (0.40 + smoothBass * 0.09) * vinylSpeedMul;
  uniforms.uVinylSpin.value = (uniforms.uVinylSpin.value + dt * vinylSpinSpeed) % (Math.PI * 2);

  updateParticlePointerFrame();
  uniforms.uBass.value   = bass;
  uniforms.uMid.value    = mid;
  uniforms.uTreble.value = treble;
  uniforms.uBeat.value   = beatPulse;
  uniforms.uEnergy.value = audioEnergy;
  uniforms.uMouseXY.value.set(mouseWorld.x, mouseWorld.y);
  uniforms.uMouseActive.value = mouseActive ? 1 : 0;
  var skullBackdropDim = fx && fx.preset === SKULL_PRESET_INDEX ? 0.58 : 1;
  var shelfDimTarget = shouldDimWallpaperForShelf() ? 0.48 : skullBackdropDim;
  var shelfDimEase = shelfDimTarget < uniforms.uParticleDim.value ? 0.18 : 0.10;
  uniforms.uParticleDim.value += (shelfDimTarget - uniforms.uParticleDim.value) * Math.min(1, shelfDimEase * Math.max(1, dt * 60));

  // 通用转场脉冲: 只作为切换预设时的短促提亮。
  uniforms.uBurstAmt.value *= 0.90;
  tickPresetTransition();

  updateRipples(dt);
  updateFloatLayer(dt);
  if (shelfManager) shelfManager.update(dt);
  tickLyricsParticles();
  updateHomeAudioVisual(dt);

  // 电影镜头
  updateCinema(dt);
  updateFreeCamera(dt);
  updateCamera();
  applySkullCameraPose(dt);

  // v7.2 旋转 = 头部+眼球追踪 + 鼠标/手势拖动 + 惯性
  tickGestureRotation(dt);
  var skullPresetActive = fx && fx.preset === SKULL_PRESET_INDEX;
  particles.visible = !skullPresetActive;
  if (bloomParticles) bloomParticles.visible = !skullPresetActive && fx.bloom && fx.bloomStrength > 0.01;
  if (floatGroup) floatGroup.visible = !skullPresetActive;
  if (backCoverGroup) backCoverGroup.visible = !skullPresetActive;
  var targetRotY = orbit.centerLocked ? 0 : (headParallax.active ? headParallax.x * 0.5 : 0) + gestureRotation.y;
  var targetRotX = orbit.centerLocked ? 0 : (headParallax.active ? -headParallax.y * 0.35 : 0) + gestureRotation.x;
  particles.rotation.y += (targetRotY - particles.rotation.y) * 0.055;
  particles.rotation.x += (targetRotX - particles.rotation.x) * 0.055;
  if (bloomParticles) {
    bloomParticles.rotation.copy(particles.rotation);
  }
  // 同步给背面粒子层
  if (floatGroup) {
    floatGroup.rotation.copy(particles.rotation);
  }
  if (backCoverGroup) {
    backCoverGroup.rotation.copy(particles.rotation);
  }
  updateSkullParticleLayer(dt);
  updateStageLyrics3D(dt);
  syncDesktopOverlayState();

  // 缩略图脉动
  if (currentIdx >= 0) {
    var s = 1 + bass * 0.08;
    var thumbCoverEl = byId('thumb-cover');
    if (thumbCoverEl) thumbCoverEl.style.transform = 'scale(' + s + ')';
  }

  renderer.render(scene, camera);
}
animate();

// ============================================================
//  宿主适配层（Sollin 桥接）
//  说明：本文件追加在 verbatim 提取段之后。同作用域内后置的函数声明
//  在提升阶段覆盖前面的同名声明，因此这里的重定义对全文件生效。
// ============================================================

// ---------- 提示 / 事件桥 ----------
function showToast(msg) { if (host.toast) host.toast(String(msg == null ? '' : msg)); }
function emitFxChange() { if (host.onFxChange && !__disposed) host.onFxChange(); }

// ---------- React 面板通过 onFxChange 订阅状态，DOM 控件同步全部改为事件 ----------
function buildLyricColorControls() { emitFxChange(); }
function updateLyricColorControls() { emitFxChange(); }
function updateLyricHighlightControls() { emitFxChange(); }
function updateLyricGlowControls() { emitFxChange(); }
function updateHomeAccentControls() { emitFxChange(); }
function updateIconAccentControls() { emitFxChange(); }
function updateCustomBackgroundControls() { emitFxChange(); }
function updateUiAccentControls() { emitFxChange(); }
function updateVisualTintControls() { emitFxChange(); }
function updateLyricFontControls() { emitFxChange(); }
function buildPresetGrid() { emitFxChange(); }
function refreshPresetGrid() { emitFxChange(); }
function renderUserFxArchives() { emitFxChange(); }
function updateDesktopLyricsFpsControls() { emitFxChange(); }
function updatePerformanceControls() { emitFxChange(); }
function updateFxInputs() { emitFxChange(); }
function setRange() {}
function applyBackgroundMediaHint() {}
function bindUserFxArchiveDrop() {}
function updateDevelopmentFxControls() {}
function animateFxResetButton() {}
function ensureFxSliderResetButton() {}

// ---------- 封面取色弹层由 React 实现，原生弹层桩掉 ----------
function openCoverColorPicker() {}
function closeCoverColorPicker() {}
function moveCoverColorLoupe() {}
function hideCoverColorLoupe() {}
function pickCoverColorFromArt() {}
function renderCoverPickerSwatches() {}
function setCoverPickerPreview() {}
function bindColorLabPicker() {}
function bindColorLabRows() {}
function closeColorLab() {}

// ---------- 桌面歌词 / 壁纸窗口（Sollin 自有桌面歌词，未移植） ----------
function pushDesktopLyricsState() {}
function pushWallpaperState() {}
function notifyDesktopLyricsBeatMapReady() {}
function getDesktopWindowApi() { return null; }
function applyDesktopLyricsState() {}
function applyWallpaperModeState() {}
function syncDesktopOverlayState() {}
function updateDesktopLyricsVisibility() {}

// ---------- 未移植的宿主子系统（歌单架 / Home / 模态 / 待机引导 / 手势摄像头） ----------
function updateHomeAudioVisual() {}
function ensureHomeWaveTrackBars() {}
function openGsapModal() {}
function closeGsapModal() {}
function closeMiniQueue() { miniQueueOpen = false; }
function setCamMode() {}
function setShelfMode() {}
function setShelfCameraMode() {}
function setShelfPresence() {}
function setShelfAccentColor() {}
function shelfAccentHex() { return fx.shelfAccentColor || '#ffffff'; }
function refreshShelfVisuals() {}
function updateShelfControlUi() {}
function safeShelfCloseContent() {}
function shouldDimWallpaperForShelf() { return false; }
function setPeek() {}
function toggleFxPanel() {}
function togglePlaylistPanel() {}
function idleGuidePointerDown() {}
function idleGuidePointerMove() {}
function idleGuidePointerUp() {}
function idleGuidePointerLeave() {}
function idleGuideWheel() {}
function setCustomCoverForCurrent() {}
function startGestureControl() {}
function stopGestureControl() {}
function setImmersiveMode(on) {
  immersiveMode = !!on;
  if (host.onImmersiveChange) host.onImmersiveChange(immersiveMode);
}
function toggleImmersiveMode() { setImmersiveMode(!immersiveMode); }
function updateImmersiveButton() {}
function closeImmersiveInterference() {}
function syncControlsAutoHideButton() {}
function setParticleLyricsSilently(on) {
  fx.particleLyrics = !!on;
  if (fx.particleLyrics) createLyricsParticles();
  else clearStageLyrics();
  lyricsVisible = fx.particleLyrics;
}

// ---------- 曲目信息 / 控制条封面由 React 渲染 ----------
function updateControlTrackInfo() {}
function setControlCoverSrc() {}

// ---------- Mineradio 本地服务端 API 不存在：磁盘节拍缓存与音源代理降级 ----------
function apiJson() { return Promise.reject(new Error('mineradio host api unavailable')); }
async function fetchBeatPrefetchAudioUrl() { return null; }

// ---------- music-tempo 由 npm 模块提供 ----------
function ensureMusicTempo() {
  if (!window.MusicTempo) { try { window.MusicTempo = MUSIC_TEMPO_MODULE; } catch (e) {} }
  return Promise.resolve(window.MusicTempo || MUSIC_TEMPO_MODULE || null);
}

// ---------- 音频分析（captureStream 旁路，不重路由宿主音频输出） ----------
var __audioStream = null;
var __audioAnalysisRebuildTimer = null;
var __audioAnalysisRebuildToken = 0;
var __audioAnalysisSrc = '';
function initAudio() {
  if (audioReady || !audio) return;
  try {
    var cap = audio.captureStream || audio.mozCaptureStream;
    if (typeof cap !== 'function') return;
    var stream = cap.call(audio);
    if (!stream || !stream.getAudioTracks || !stream.getAudioTracks().length) return;
    __audioStream = stream;
    audioCtx = new (window.AudioContext || window.webkitAudioContext)({ latencyHint: 'interactive' });
    source = audioCtx.createMediaStreamSource(stream);
    analyser = audioCtx.createAnalyser();
    beatAnalyser = audioCtx.createAnalyser();
    analyser.fftSize = FFT_SIZE;
    analyser.smoothingTimeConstant = 0.58;
    beatAnalyser.fftSize = BEAT_FFT_SIZE;
    beatAnalyser.smoothingTimeConstant = 0.10;
    source.connect(analyser);
    source.connect(beatAnalyser);
    __audioAnalysisSrc = audio.currentSrc || audio.src || '';
    frequencyData.fill(0);
    beatFrequencyData.fill(0);
    beatTimeDomainData.fill(128);
    resetRealtimeBeatEngine();
    audioReady = true;
  } catch (e) {
    console.warn('[mineradio] init audio analysis failed:', e);
  }
}
function teardownAudioAnalysis() {
  if (__audioAnalysisRebuildTimer) {
    clearTimeout(__audioAnalysisRebuildTimer);
    __audioAnalysisRebuildTimer = null;
  }
  try { if (source) source.disconnect(); } catch (e) {}
  try { if (analyser) analyser.disconnect(); } catch (e) {}
  try { if (beatAnalyser) beatAnalyser.disconnect(); } catch (e) {}
  try { if (audioCtx) audioCtx.close(); } catch (e) {}
  try { if (__audioStream && __audioStream.getTracks) __audioStream.getTracks().forEach(function(track){ track.stop(); }); } catch (e) {}
  source = null; analyser = null; beatAnalyser = null; audioCtx = null;
  __audioStream = null;
  __audioAnalysisSrc = '';
  audioReady = false;
}
function ensureAudioAnalysis() {
  if (!audio) return;
  var streamAlive = __audioStream && __audioStream.active
    && __audioStream.getAudioTracks().some(function(t){ return t.readyState === 'live' && t.enabled; });
  var currentSrc = audio.currentSrc || audio.src || '';
  if (audioReady && streamAlive && (!currentSrc || currentSrc === __audioAnalysisSrc)) { resumeAudioAnalysis(); return; }
  teardownAudioAnalysis();
  initAudio();
  resumeAudioAnalysis();
}
function scheduleAudioAnalysisRebuild(reason) {
  __audioAnalysisRebuildToken++;
  var token = __audioAnalysisRebuildToken;
  teardownAudioAnalysis();
  resetRealtimeBeatEngine();
  if (!audio || !playing) return;
  var attempts = 0;
  function retry() {
    if (__disposed || token !== __audioAnalysisRebuildToken || !audio || !playing) return;
    attempts++;
    ensureAudioAnalysis();
    if (!audioReady && attempts < 8) {
      __audioAnalysisRebuildTimer = setTimeout(retry, attempts < 3 ? 120 : 260);
    } else {
      __audioAnalysisRebuildTimer = null;
    }
  }
  __audioAnalysisRebuildTimer = setTimeout(retry, reason === 'track' ? 120 : 0);
}
function resumeAudioAnalysis() {
  if (audioCtx && audioCtx.state === 'suspended') return audioCtx.resume().catch(function(e){ console.warn('audio context resume failed:', e); });
  return Promise.resolve();
}

// ---------- 节拍角标（React 呈现） ----------
function showBeatChip(text) {
  if (host.onBeatChip && !__disposed) host.onBeatChip({ visible: true, text: String(text || '') });
}
function hideBeatChip() {
  if (host.onBeatChip && !__disposed) host.onBeatChip({ visible: false, text: '' });
}

// ---------- 进度条拖拽粒子（DOM 微粒，附着 body） ----------
var progressDragState = { lastParticleAt: 0 };
function emitProgressDragParticles(x, y) {
  var now = performance.now();
  if (now - progressDragState.lastParticleAt < 46) return;
  progressDragState.lastParticleAt = now;
  for (var i = 0; i < 3; i++) {
    var dot = document.createElement('span');
    dot.className = 'progress-drag-particle';
    var dx = (Math.random() - 0.5) * 34;
    var dy = -10 - Math.random() * 28;
    dot.style.setProperty('--px', x + 'px');
    dot.style.setProperty('--py', y + 'px');
    dot.style.setProperty('--dx', dx + 'px');
    dot.style.setProperty('--dy', dy + 'px');
    (host.overlayRoot || document.body).appendChild(dot);
    setTimeout((function(el){ return function(){ if (el && el.parentNode) el.parentNode.removeChild(el); }; })(dot), 700);
  }
}

// ============================================================
//  对外 API
// ============================================================
function revealMainParticleLayer(opts) {
  opts = opts || {};
  if (!uniforms || !uniforms.uAlpha) return;
  var target = opts.target == null ? 1 : clampRange(Number(opts.target) || 1, 0, 1);
  if (alphaTween && opts.instant) {
    cancelAnimationFrame(alphaTween.raf);
    alphaTween = null;
  }
  if (opts.instant) {
    uniforms.uAlpha.value = clampRange(Math.max(uniforms.uAlpha.value || 0, target), 0, 1);
    return;
  }
  var current = clampRange(uniforms.uAlpha.value || 0, 0, 1);
  if (current < target - 0.015) {
    tweenParticleAlpha(current, target, opts.duration || 320);
  }
}

// 空场星河预览：无歌曲时切到自发光的“星河”预设，避免封面粒子无封面而全黑；
// 正式播放时还原用户选择的播放预设（对应原 applyStartupStarfieldPreset /
// switchPlaybackVisualToEmily）。
function applyStartupStarfieldVisual() {
  if (playing || currentIdx >= 0) return;
  startupVisualPreviewActive = true;
  if (fx.preset !== 5) {
    setPreset(5, { silent: true, preserveCamera: false, skipTransition: true, noSave: true });
  } else {
    syncFxUniforms();
  }
  revealMainParticleLayer({ target: 0.96, duration: 520 });
}
function restorePlaybackVisualPreset() {
  var targetPreset = typeof playbackVisualPreset === 'number' ? playbackVisualPreset : fxDefaults.preset;
  startupVisualPreviewActive = false;
  if (fx.preset !== targetPreset) {
    setPreset(targetPreset, { silent: true, preserveCamera: false, noSave: true });
  } else {
    syncFxUniforms();
  }
  if (targetPreset !== SKULL_PRESET_INDEX) revealMainParticleLayer({ target: 1, duration: 260 });
}

function apiSetTrack(song) {
  if (__disposed) return;
  song = song || null;
  trackSwitchToken++;
  beatMapToken++;
  djBeatMapToken++;
  cancelBeatAnalysisTimer();
  cancelBeatPrefetchTimer();
  cancelDjBeatAnalysisTimer();
  hideBeatChip();
  currentBeatMap = null;
  beatMapNextIdx = 0;
  resetDjBeatMapState();
  setDjModeActive(false, null);
  playQueue = song ? [song] : [];
  currentIdx = song ? 0 : -1;
  currentLocalSong = null;
  lyricsLines = [];
  lyricsHasNativeKaraoke = false;
  lyricsTimingSource = 'none';
  try { clearStageLyrics(); } catch (e) {}
  resetCinemaTrackProfile(song);
  resetAudioVisualState();
  syncBeatCameraToTime(audio && isFinite(audio.currentTime) ? audio.currentTime : 0);
  if (playing) scheduleAudioAnalysisRebuild('track');
  if (song) {
    if (startupVisualPreviewActive) restorePlaybackVisualPreset();
    else if (fx.preset !== SKULL_PRESET_INDEX) revealMainParticleLayer({ target: 1, duration: 260 });
    var coverUrl = songCoverSrc(song, 400) || song.cover || '';
    loadCoverFromUrl(coverUrl);
    var beatKey = beatMapSongKey(song);
    var beatUrl = (audio && (audio.currentSrc || audio.src)) || song.url || '';
    if (beatKey && beatUrl) {
      if (beatMapCache[beatKey]) {
        currentBeatMap = beatMapCache[beatKey];
        beatMapNextIdx = 0;
        alignBeatCameraCursorToTime(audio && isFinite(audio.currentTime) ? audio.currentTime : 0);
        applyCinemaProfileFromBeatMap(currentBeatMap);
      } else {
        scheduleBeatAnalysis(beatKey, beatUrl, beatMapToken, song);
      }
    }
  } else {
    loadCoverFromUrl('');
    applyStartupStarfieldVisual();
  }
}

function apiSetLyrics(lines, meta) {
  if (__disposed) return;
  meta = meta || {};
  lyricsLines = Array.isArray(lines) ? lines : [];
  lyricsHasNativeKaraoke = !!meta.hasKaraoke;
  lyricsTimingSource = meta.timingSource || (lyricsLines.length ? 'line' : 'none');
  lyricsVisible = !!fx.particleLyrics;
  try { clearStageLyrics(); } catch (e) {}
}

function apiSetPlaying(isPlaying) {
  if (__disposed) return;
  playing = !!isPlaying;
  if (playing) scheduleAudioAnalysisRebuild('playing');
  else __audioAnalysisRebuildToken++;
}

function apiNotifySeek() {
  if (__disposed) return;
  syncBeatCameraToTime(audio && isFinite(audio.currentTime) ? audio.currentTime : 0);
}

function apiSetAudioElement(el) {
  if (__disposed) return;
  if (audio === el) return;
  teardownAudioAnalysis();
  audio = el || null;
  if (audio && playing) ensureAudioAnalysis();
}

// bindFxPanel 中滑块 input 的副作用复刻（React 面板调用）
function apiSetFxValue(key, value) {
  if (__disposed) return;
  var num = typeof value === 'number' ? value : parseFloat(value);
  fx[key] = isFinite(num) ? num : value;
  if (key === 'coverResolution') {
    fx.coverResolution = normalizeCoverResolution(fx.coverResolution);
    applyCoverParticleResolution(fx.coverResolution, { reload: true });
  }
  if (key === 'lyricWeight') fx.lyricWeight = Math.round(clampRange(fx.lyricWeight, 500, 900) / 50) * 50;
  if (key === 'backgroundOpacity') {
    fx.backgroundOpacity = clampRange(fx.backgroundOpacity, 0, 1);
    fx.backgroundColorMode = 'custom';
    fx.backgroundColorCustom = true;
    applyCustomBackground();
  }
  if (key === 'controlGlassChromaticOffset') {
    fx.controlGlassChromaticOffset = normalizeControlGlassChromaticOffset(fx.controlGlassChromaticOffset);
    applyControlGlassChromaticOffset();
  }
  if (key === 'desktopLyricsSize') fx.desktopLyricsSize = clampRange(fx.desktopLyricsSize, 0.72, 1.55);
  if (key === 'desktopLyricsOpacity') fx.desktopLyricsOpacity = clampRange(fx.desktopLyricsOpacity, 0.28, 1);
  if (key === 'desktopLyricsY') fx.desktopLyricsY = clampRange(fx.desktopLyricsY, 0.08, 0.92);
  if (key === 'lyricTiltX' || key === 'lyricTiltY') fx[key] = Math.round(clampRange(fx[key], -42, 42));
  syncFxUniforms();
  if (key === 'lyricLetterSpacing' || key === 'lyricLineHeight' || key === 'lyricWeight') refreshCurrentLyricStyle();
  saveLyricLayout();
  emitFxChange();
}

function apiGetState() {
  return {
    fx: fx,
    presetMeta: presetMeta,
    presetIcons: presetIcons,
    presetDisplayOrder: presetDisplayOrder,
    lyricColorPresets: lyricColorPresets,
    userFxArchives: userFxArchives,
    coverSwatches: (function(){ try { return coverPickerSwatchColors(); } catch (e) { return []; } })(),
    immersive: immersiveMode,
  };
}

function apiDestroy() {
  if (__disposed) return;
  __disposed = true;
  try { flushPersistentVisualState(); } catch (e) {}
  beatMapToken++; djBeatMapToken++; trackSwitchToken++;
  cancelBeatAnalysisTimer();
  cancelBeatPrefetchTimer();
  cancelDjBeatAnalysisTimer();
  __timers.forEach(function(id){ clearInterval(id); });
  __listeners.forEach(function(entry){
    try { entry[0].removeEventListener(entry[1], entry[2], entry[3]); } catch (e) {}
  });
  __listeners.length = 0;
  try {
    gsap.globalTimeline.getChildren(true, true, true).forEach(function(t){ try { t.kill(); } catch (e) {} });
  } catch (e) {}
  try {
    scene.traverse(function(obj){
      try { if (obj.geometry && obj.geometry.dispose) obj.geometry.dispose(); } catch (e) {}
      var m = obj.material;
      if (m) (Array.isArray(m) ? m : [m]).forEach(function(mm){
        if (!mm) return;
        try {
          Object.keys(mm).forEach(function(k){
            var v = mm[k];
            if (v && v.isTexture) { try { v.dispose(); } catch (e) {} }
          });
          if (mm.uniforms) Object.keys(mm.uniforms).forEach(function(k){
            var u = mm.uniforms[k];
            if (u && u.value && u.value.isTexture) { try { u.value.dispose(); } catch (e) {} }
          });
        } catch (e) {}
        try { mm.dispose(); } catch (e) {}
      });
    });
  } catch (e) {}
  teardownAudioAnalysis();
  try { renderer.dispose(); } catch (e) {}
  try { renderer.forceContextLoss && renderer.forceContextLoss(); } catch (e) {}
  try { if (renderer.domElement && renderer.domElement.parentNode) renderer.domElement.parentNode.removeChild(renderer.domElement); } catch (e) {}
  document.body.classList.remove('render-deep-sleep', 'render-background-eco', 'cursor-hidden');
}

// ============================================================
//  启动（对应原“启动”段的视觉子集）
// ============================================================
applySavedLyricPaletteState();
initControlGlassSurface();
bindPlayerControlAnimations();
scheduleUiWarmTask(function(){
  if (__disposed) return;
  updateControlGlassDisplacementMap();
  try { if (renderer && renderer.compile && scene && camera) renderer.compile(scene, camera); } catch (e) {}
}, 900);
if (fx.floatLayer) createFloatLayer();
if (fx.particleLyrics) createLyricsParticles();
if (fx.backCover) createBackCoverLayer();
installRenderPowerHooks();
applyCustomBackground();
applyUiAccentColor();
applyIconAccentColors();
syncFxUniforms();
applyStartupStarfieldVisual();
__on(window, 'resize', function(){ if (!__disposed) scheduleMainRendererViewportRefresh('resize'); });
scheduleMainRendererViewportRefresh('mount');

function apiDebug() {
  var dbg = { disposed: __disposed, fxPreset: fx.preset, playing: playing, audioReady: audioReady, currentIdx: currentIdx };
  try { dbg.hasCover = uniforms.uHasCover.value; } catch (e) { dbg.hasCover = 'ERR:' + e.message; }
  try { dbg.uPreset = uniforms.uPreset.value; } catch (e) {}
  try { dbg.uAlpha = Number(uniforms.uAlpha.value).toFixed(3); } catch (e) {}
  try { dbg.uBass = Number(uniforms.uBass.value).toFixed(3); dbg.uEnergy = Number(uniforms.uEnergy.value).toFixed(3); } catch (e) {}
  try { dbg.particlesVisible = particles.visible; dbg.particleCount = particles.geometry.attributes.position.count; } catch (e) { dbg.particlesVisible = 'ERR:' + e.message; }
  try { dbg.particleMaterialVisible = particles.material.visible; dbg.opacityU = particles.material.uniforms ? 'shader' : 'basic'; } catch (e) {}
  try { dbg.coverTexSet = typeof coverTex !== 'undefined' && !!coverTex; } catch (e) {}
  try { dbg.lyricsLines = lyricsLines.length; dbg.lyricsVisible = lyricsVisible; } catch (e) {}
  try { dbg.stageKeys = stageLyrics ? Object.keys(stageLyrics).slice(0, 30).join(',') : 'none'; } catch (e) {}
  try { dbg.stageCurrentText = stageLyrics && stageLyrics.current ? (stageLyrics.current.userData && stageLyrics.current.userData.text) : null; } catch (e) {}
  try { dbg.sceneChildren = scene.children.map(function(c){ return (c.type || '?') + ':' + (c.name || '') + ':' + (c.visible ? 'v' : 'h'); }).join(' | '); } catch (e) {}
  try { dbg.fps = renderPerfState.fps; dbg.frames = renderPerfState.frames; dbg.mode = renderPerfState.mode; } catch (e) {}
  try { dbg.cameraZ = Number(camera.position.z).toFixed(2); dbg.cameraFov = camera.fov; } catch (e) {}
  try { dbg.audioSrcSet = !!(audio && (audio.currentSrc || audio.src)); dbg.audioPaused = audio ? audio.paused : null; dbg.audioTime = audio ? Number(audio.currentTime).toFixed(2) : null; } catch (e) {}
  try {
    dbg.analyserSet = !!analyser;
    dbg.audioAnalysisSrc = __audioAnalysisSrc ? __audioAnalysisSrc.slice(0, 96) : '';
    dbg.audioStreamAlive = !!(__audioStream && __audioStream.active);
  } catch (e) {}
  try { dbg.deepSleep = isDeepBackgroundMode(); } catch (e) {}
  return dbg;
}
try { window.__mineradioDebug = apiDebug; } catch (e) {}

return {
  setTrack: apiSetTrack,
  setLyrics: apiSetLyrics,
  setPlaying: apiSetPlaying,
  notifySeek: apiNotifySeek,
  setAudioElement: apiSetAudioElement,
  getState: apiGetState,
  setFxValue: apiSetFxValue,
  toggleFx: function(key){ if (!__disposed) { toggleFx(key); emitFxChange(); } },
  setPreset: function(p){ if (!__disposed) { setPreset(p); if (Number(p) !== SKULL_PRESET_INDEX) revealMainParticleLayer({ target: 1, duration: 220 }); emitFxChange(); } },
  resetFx: function(){ if (!__disposed) { resetFx(); emitFxChange(); } },
  setLyricFont: function(k){ if (!__disposed) { setLyricFont(k); emitFxChange(); } },
  setLyricColorAuto: function(){ if (!__disposed) { setLyricColorAuto(); emitFxChange(); } },
  setLyricColorCustom: function(c){ if (!__disposed) { setLyricColorCustom(c, true); emitFxChange(); } },
  setLyricColorPreset: function(i){ if (!__disposed) { setLyricColorPreset(i); emitFxChange(); } },
  setLyricHighlightAuto: function(){ if (!__disposed) { setLyricHighlightAuto(); emitFxChange(); } },
  setLyricHighlightCustom: function(c){ if (!__disposed) { setLyricHighlightCustom(c, true); emitFxChange(); } },
  setLyricGlowLinked: function(on){ if (!__disposed) { setLyricGlowLinked(on); emitFxChange(); } },
  setLyricGlowCustom: function(c){ if (!__disposed) { setLyricGlowCustom(c, true); emitFxChange(); } },
  setUiAccentColor: function(c){ if (!__disposed) { setUiAccentColor(c, true); emitFxChange(); } },
  resetUiAccentColor: function(){ if (!__disposed) { resetUiAccentColor(); emitFxChange(); } },
  setVisualTintAuto: function(){ if (!__disposed) { setVisualTintAuto(); emitFxChange(); } },
  setVisualTintCustom: function(c){ if (!__disposed) { setVisualTintCustom(c, true); emitFxChange(); } },
  resetVisualTintColor: function(){ if (!__disposed) { resetVisualTintColor(); emitFxChange(); } },
  setCustomBackgroundColor: function(c){ if (!__disposed) { setCustomBackgroundColor(c, true); emitFxChange(); } },
  setCustomBackgroundCoverMode: function(){ if (!__disposed) { setCustomBackgroundCoverMode(); emitFxChange(); } },
  setCustomBackgroundOpacity: function(v){ if (!__disposed) { setCustomBackgroundOpacity(v, true); emitFxChange(); } },
  clearCustomBackgroundImage: function(){ if (!__disposed) { clearCustomBackgroundImage(); emitFxChange(); } },
  readBackgroundMediaFile: function(file){ if (!__disposed) readBackgroundMediaFile(file); },
  setPerformanceBackgroundMode: function(m){ if (!__disposed) { setPerformanceBackgroundMode(m); emitFxChange(); } },
  setPerformanceQualityMode: function(m){ if (!__disposed) { setPerformanceQualityMode(m); emitFxChange(); } },
  applyUserFxArchive: function(i){ if (!__disposed) { applyUserFxArchive(i); emitFxChange(); } },
  saveUserFxArchive: function(i){ if (!__disposed) { saveUserFxArchive(i); emitFxChange(); } },
  createUserFxArchive: function(){ if (!__disposed) { createUserFxArchive(); emitFxChange(); } },
  removeUserFxArchive: function(i){ if (!__disposed) { removeUserFxArchive(i); emitFxChange(); } },
  renameUserFxArchive: function(i, name){
    if (__disposed) return;
    var slot = userFxArchiveAt(i);
    if (!slot) return;
    slot.name = normalizeUserFxArchiveName(name, i);
    saveUserFxArchives();
    emitFxChange();
  },
  exportUserFxArchive: function(i){ if (!__disposed) exportUserFxArchive(i); },
  importUserFxArchiveText: function(text, fileName){ if (!__disposed) { importUserFxArchiveText(text, fileName); emitFxChange(); } },
  setImmersive: function(on){ if (!__disposed) setImmersiveMode(!!on); },
  toggleLyrics: function(){
    if (__disposed) return;
    setParticleLyricsSilently(!fx.particleLyrics);
    showToast(fx.particleLyrics ? '歌词已开启' : '歌词已关闭');
    saveLyricLayout();
    emitFxChange();
  },
  recenterCamera: function(){ if (!__disposed) recenterCamera(); },
  emitProgressDragParticles: emitProgressDragParticles,
  markRenderInteraction: function(reason){ if (!__disposed) markRenderInteraction(reason || 'host'); },
  syncViewport: function(){ if (!__disposed) scheduleMainRendererViewportRefresh('host'); },
  destroy: apiDestroy,
};
}
