# 暮云音乐 MuyunMusic

**基于 Qt 6 / QML 的 Windows 桌面音乐播放器**：多平台音乐搜索、榜单与歌单广场，逐字歌词 / 桌面歌词 /
三种全屏播放页（含 Mineradio 舞台），全格式音效管线（均衡器 · 环境混响 · 3D 环绕 · 响度均衡），
兼容洛雪（LX Music）音源脚本与手机端同步协议。

A Qt 6 / QML desktop music player for Windows — multi-platform search & playlists, word-by-word lyrics,
full-format audio effects (EQ / reverb / 3D surround / loudness), LX Music script compatible, LAN sync.

> ⚠ **本程序不内置任何音乐播放/下载源**。内置的五个平台适配只做**搜索、榜单、封面、歌词、歌单广场**；
> 实际播放与下载 100% 依赖**你自己导入的 LX 音源脚本**（设置 → 在线音源 → 导入）。
> 请自行确认所在地法律与平台条款，仅用于学习与个人使用。

**当前版本：v1.0.1** ｜ [下载](#下载) ｜ [已知问题](#已知问题) ｜ [从源码构建](#从源码构建) ｜ [许可](#许可)

---

## 功能

### 播放与队列

- 播放 / 暂停 / 上一首 / 下一首 / 进度拖拽 / 音量 / 静音，四种播放模式（顺序、列表循环、单曲循环、随机）
- 播放队列抽屉、下一首播放、失败自动跳过、设备切换续播
- 在线音频缓存（key = 歌曲身份 + 音质），启动自动清理 3 天前缓存

### 歌词

- 逐字歌词、翻译 / 罗马音合并显示、全屏播放页三种样式（经典 / Apple Music 风 / Mineradio 舞台）
- 独立桌面歌词窗：工具条 + 设置窗，字号与高度自适应

### 音效（全格式）

- 10 段均衡器、环境混响（小房间 / 金属板 / 大厅 / 教堂四预设）、3D 环绕（等功率声像摆动）、响度均衡
- MP3 走 minimp3（时长样本级精确），其余格式（FLAC / M4A / OGG / OPUS / WAV …）走 Qt 自带的 FFmpeg
- 管道：后台解码 → DSP（源采样率）→ 重采样 → 环形缓冲，主线程只做 memcpy；参数调整即时生效不重解码

### 音乐库

- 本地音乐扫描、标签读写、时长精确校准（MP3 帧头遍历 / OGG granule position）
- 收藏、自建歌单、最近播放；收藏页与自建歌单支持批量（全选 / 批量下载 / 批量移除）
- 收藏在线歌单（整单进侧栏）、按歌曲换源

### 多平台与网络

- 搜索 + 联想（跨平台聚合）、榜单、推荐歌单、歌单广场（分类 + 分页，带列表缓存与并行预热）
- LX 音源脚本按官方协议实现 `globalThis.lx` 全套宿主 API：`request`（含取消函数）、`utils.buffer`、
  `utils.crypto`（`md5` / `aesEncrypt` / `randomBytes` / `rsaEncrypt`）、`utils.zlib`、
  `currentScriptInfo`、`setTimeout` / `clearTimeout`，并按 `inited.sources` 声明的音质取链接
- 局域网同步：暮云 ↔ 暮云，并兼容洛雪移动版协议（配对码 / AES+RSA 握手 / WS / 三方合并）

### 其它

- 无边框窗口（原生边缘缩放、最大化 / 全屏 / ESC 阶梯：舞台沉浸 → 全屏播放页 → 窗口全屏 →
  退出最大化 → 关闭主窗口，弹层在场时让位给弹层）、深浅色主题、系统托盘
- 单实例守护：重复启动唤起已有窗口；权限不一致（管理员 ↔ 普通）时也**不双开**，会明确提示
- 启动静默检查更新（GitHub 版本清单），支持"不再提醒此版本"

## 已知问题

1. **最大化对第三方任务栏美化 / 透明工具"不可见"**：我们的最大化是**手动改窗口几何**（不走系统
   `ShowWindow(SW_MAXIMIZE)`），窗口因此没有 `WS_MAXIMIZE` 状态位、`IsZoomed()` 为假——
   靠"检测到最大化窗口就把任务栏从全透明切成毛玻璃 / 其他材质"的工具（TranslucentTB 一类）认不出来。
   用 **F11 全屏**（真全屏）不受影响。
2. **已运行的实例若是管理员权限**，用普通权限再启动时只能弹提示、**无法把它的窗口唤到前台**
   （Windows 完整性级别限制，不是我们的 bug）。
3. **启动时偶发 1~2 条 Qt 跨线程警告**（`Cannot create children for a parent that is in a different thread`），
   不影响功能，仍在排查。
4. **升级安装前请先退出正在运行的暮云音乐**：当前安装器不会自动关闭正在跑的实例，
   程序开着时装新版会因文件被占用而失败，且失败回滚可能把你已有的安装弄成半新半旧。
   托盘里也请右键退出（`⚙ 设置 → 关闭主窗口时` 选"最小化到托盘"时，程序其实还在运行）。

> 说明：**音源脚本只支持新版 `globalThis.lx` 格式**。旧版 `userApi` / `registerSource` 不支持，
> 导入时会明确提示——这是刻意的取舍，不是缺陷。

## 下载

到 [Releases](https://github.com/MyunU/Muyun-Music-Desktop/releases) 下载：

- `MuyunMusic-<版本>.exe` —— 安装版（Windows 10/11 x64，无需另装 Qt）
- `MuyunMusic-<版本>.zip` —— 便携版（解压后直接运行 `MuyunMusic.exe`）

> 从旧版升级：**先退出正在运行的暮云音乐**（含托盘里的实例），再运行新版安装包。详见[已知问题](#已知问题)第 4 条。

## 从源码构建

### 依赖

| 需要 | 版本 | 说明 |
|---|---|---|
| Qt | 6.8.3 MinGW 64-bit | 需含 Core / Gui / Qml / Quick / QuickControls2 / Network / Multimedia / Concurrent / Sql / Svg / Widgets |
| MinGW GCC | 13.1（Qt 自带 `Tools/mingw1310_64`） | 其它版本（如 GCC 16）编译会段错误 |
| CMake | ≥ 3.20 | 生成器用 `MinGW Makefiles` |

### 步骤

```powershell
# 1) 拉取舞台（MuyunStage）需要的一次性第三方文件：WebView2 SDK 头 + Loader DLL、three.js、gsap
pwsh -File stage\sdk\fetch-sdk.ps1

# 2) 配置 + 编译（把 <Qt> 换成你的 Qt 路径）
cmake -S . -B build -G "MinGW Makefiles" `
      -DCMAKE_PREFIX_PATH="E:/Qt/6.8.3/6.8.3/mingw_64" `
      -DCMAKE_C_COMPILER="E:/Qt/Tools/Tools/mingw1310_64/bin/gcc.exe" `
      -DCMAKE_CXX_COMPILER="E:/Qt/Tools/Tools/mingw1310_64/bin/g++.exe"
cmake --build build --target MuyunMusic --parallel 4
```

产物在 `build/MuyunMusic.exe`（依赖由 `windeployqt` 自动部署到同目录）。

### 开关

- `-DMUYUN_CONSOLE=ON`：编译成控制台程序（跑 `--test-*` 自检、看运行日志用）
- `-DMUYUN_SELFTES=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXE_LINKER_FLAGS=-s`：发布构建
  （编译级剔除全部自检与调试输出，体积 4.6MB → 3.2MB）
- `-DMUYUN_STAGE=OFF`：不编译舞台（纯 Win32 + WebView2 的独立进程 `MuyunStage.exe`）

### 打包（可选）

`package/build_payload.ps1` 出便携 zip、`package/build_installer.ps1` 出安装包
（需 .NET Framework 4 的 `csc.exe`）。

## 数据目录

运行数据统一在 `%USERPROFILE%\.muyun`：`store/`（收藏、设置、队列）、`lx-sources/`（导入的音源脚本）、
`lx-sync/`（配对与列表快照）、`cache/`（封面缓存）。同机所有实例共用这一份（便携版与安装版也共用）。

## 自检

开发版（`-DMUYUN_SELFTES=ON`）内置 29 项端到端自检，例如：

```powershell
$env:MUYUN_STORE_ROOT = "$env:TEMP\muyun-test"   # 隔离数据目录（会写数据的自检必须设，否则拒绝运行）
MuyunMusic.exe --test-dsp              # DSP 波形：EQ / 混响 / 环绕 / 响度均衡
MuyunMusic.exe --test-effects-live     # 音效管线真实出声
MuyunMusic.exe --test-lyric-merge      # 歌词译文配对
MuyunMusic.exe --test-update           # 更新提示（离线，可用 MUYUN_UPDATE_LIVE_URL 验真网络）
MuyunMusic.exe --test-lxsource         # LX 音源脚本协议一致性（逐条断言，全离线）
MuyunMusic.exe --test-esc-ladder       # ESC 阶梯（含退最大化 / 关主窗口 / 弹层让位）
MuyunMusic.exe --test-ime              # 输入法上下文守护（任何焦点下都能切中英文）
MuyunMusic.exe --test-single-instance  # 单实例守护（第二个实例不双开、能唤起主窗）
```

## 许可

- **主程序：MIT**，见 [LICENSE](LICENSE)。
- **`stage/`（Mineradio 舞台内核）: GPL-3.0**。它以**独立进程** `MuyunStage.exe` 与主程序松耦合聚合，
  因此主程序仍按 MIT 分发；再分发时请保留 `stage/web/README.md`、`stage/web/mineradio/README.md`、
  `stage/web/mineradio/music-tempo.LICENCE` 等许可声明。
- 第三方组件与完整许可清单见 [THIRD-PARTY.md](THIRD-PARTY.md)
  （Qt / FFmpeg / minimp3 / QuickJS / WebView2 SDK / three.js / GSAP / music-tempo 等）。

## 致谢

- [Sollin-Music-Desktop](https://github.com/Ryderwe/Sollin-Music-Desktop)（MIT）
- [lx-music-desktop](https://github.com/lyswhut/lx-music-desktop) /
  [lx-music-mobile](https://github.com/lyswhut/lx-music-mobile)（Apache-2.0）
- [Mineradio](https://github.com/XxHuberrr/Mineradio)（GPL-3.0）

## 免责声明

本项目为个人学习与技术研究用途。程序不提供、不存储、不分发任何音乐内容，所有音乐数据均来自
用户自行导入的第三方音源脚本与各平台公开接口；使用者需自行承担合规责任。
