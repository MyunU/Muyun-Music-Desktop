# Mineradio 播放界面移植（vendored）

本目录代码移植自 [Mineradio](https://github.com/XxHuberrr/Mineradio) 项目的
`public/index.html`（GPL-3.0 授权）。

## 内容

- `engine.js` — 播放界面视觉引擎（由下面三个部件拼装，**不要直接编辑**）：
  - `_preamble.js.part` — 手写的全局状态子集、宿主适配辅助、模块导入
  - `_adapted_sections.js.part` — 从 Mineradio 原文件按行提取的 verbatim 段
    （每段有 `// ▼▼▼ [mineradio:xxx] source lines a,b ▼▼▼` 溯源标记），
    仅做了机械替换：`document.getElementById → byId`（覆盖层作用域）、
    `addEventListener → __on`（可拆卸）、CSS 变量根、资产路径、RAF 销毁守卫
  - `_postamble.js.part` — 宿主适配层：桩掉未移植子系统（3D 歌单架 / 桌面歌词 /
    登录 / 手势摄像头）、captureStream 音频分析、对外 API、启动与销毁
- `engine.d.ts` — 引擎 TypeScript 类型声明
- `mineradio.css` — 提取并作用域化（`.mineradio-player`）的界面样式

重新拼装：`cat _preamble.js.part _adapted_sections.js.part _postamble.js.part > engine.js`

## 移植范围

保留：封面粒子系统（7 预设 shader）、3D 舞台歌词（逐字卡拉OK）、电影镜头
（实时节拍引擎 + music-tempo 离线节拍解析）、涟漪、浮空粒子、骷髅预设、
封面深度处理、视觉控制台（fx 状态 / 预设 / 用户存档）、玻璃控制条。

未移植：3D 歌单架、天气电台、桌面歌词/壁纸副窗口（Sollin 有自己的实现）、
手势摄像头交互、登录/搜索/队列管理（由 Sollin 宿主提供）。

## 许可

本目录及资产（`public/mineradio/`）按 **GPL-3.0** 授权分发，
版权归 Mineradio 原作者所有。
