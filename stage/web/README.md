# MuyunStage 舞台页面资源

本目录是独立进程 `MuyunStage.exe`（纯 Win32 + WebView2 壳）加载的舞台视觉页面资源。
经 `SetVirtualHostNameToFolderMapping` 映射为 `https://muyunstage.local/`。

## 许可边界（重要）

- `mineradio/` 子目录（`engine.js` / `mineradio.css` / `music-tempo.*` /
  `skull-decimation-points.bin`）：**GPL-3.0**，移植自 Mineradio，版权归原作者，
  详见 `mineradio/README.md`。
- `vendor/` 子目录：第三方运行时库
  - `three.module.js` — three r128（MIT）
  - `gsap.min.js` / `gsap-esm.js` — GSAP（GreenSock 标准许可）
  - `music-tempo.min.js` / `music-tempo-esm.js` — music-tempo（MIT）
- `index.html` / `host.js` / `host.css`：**暮云音乐自研宿主 glue**，随主程序按
  其自身许可分发；它们只通过 DOM 契约与 `createMineradioEngine()` 调用外部引擎，
  不与 GPL 代码静态链接。

## 为什么这样切分（方案C）

主程序 `MuyunMusic.exe` 是 MIT、纯 C++/Qt，**不内嵌、不链接任何 GPL 代码，也不执行
任何 JS**。它通过 `QProcess` 拉起本壳进程，用两条命名管道传递 JSON（曲目/歌词/播放态
/控制事件）。GPL 引擎只活在 `MuyunStage.exe` 这个独立进程里，由 WebView2 加载执行。
进程间以数据交换协作（非链接），主程序与引擎是各自独立的程序——这是"聚合"而非
"结合著作物"，从而允许主程序维持 MIT。

若将来要正式分发并彻底消除许可争议，兜底选项：把本 glue 与 IPC 协议一并开源，
或将整体改为 GPL。纯自用则无需顾虑。

## 调试

- 主程序侧：`MuyunMusic.exe --test-stage`（端到端自检）；`set MUYUN_STAGE_DEBUG=1`
  打开桥日志并向壳传 `--log`。
- 壳单独跑：`MuyunStage.exe --cmd-pipe=X --web-dir=<本目录> --ev-retries=1 --probe=...`
  （见 stage/src/stage_main.cpp 顶部参数）。
