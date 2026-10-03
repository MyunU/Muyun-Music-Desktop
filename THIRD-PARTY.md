# 第三方组件与许可（THIRD-PARTY NOTICES）

本文件列出暮云音乐（MuyunMusic）使用的第三方组件、许可与使用方式。
**主程序按 MIT 分发；`stage/`（Mineradio 舞台内核）按 GPL-3.0 分发**（见 [LICENSE](LICENSE)）。

> **THIRD-PARTY.md 是什么**：开源发布时的"第三方组件清单 / 许可声明"文件。
> 用了别人的库就必须在再分发时保留其版权与许可声明——这个文件就是集中放这些声明的地方，
> 它既是法律义务（MIT/BSD/Apache/LGPL 都要求保留声明），也方便使用者一眼看清依赖与风险。

## 一、随程序分发的组件

| 组件 | 版本 / 来源 | 许可 | 使用方式 |
|---|---|---|---|
| **Qt** | 6.8.3（Core / Gui / Qml / Quick / QuickControls2 / Network / Multimedia / Concurrent / Sql / Svg / Widgets） | LGPL-3.0（另有 GPL / 商业双授权） | **动态链接**随包分发的 Qt DLL（`windeployqt` 部署），未修改 Qt 源码；满足 LGPL 的"动态链接 + 保留声明 + 可替换库"要求 |
| **FFmpeg** | libavformat-61 / libavcodec-61 / libavutil-59 / libswresample-5 / swscale-8（Qt 多媒体自带的构建） | LGPL-2.1-or-later | 音效管线**运行时 `LoadLibrary` 动态加载**这些 DLL（未修改、未静态链接） |
| **FFmpeg 公共头（最小闭包 36 个）** | 取自 FFmpeg n7.1 源码，位于 `src/vendor/ffmpeg/include/` | LGPL-2.1-or-later | 仅作**编译期类型声明**（读 `AVFrame`/`AVCodecContext` 等结构体字段）；`libavutil/avconfig.h` 是 configure 生成物，本仓库内为手写替身，文件头已注明 |
| **minimp3** | `src/vendor/minimp3/` | CC0-1.0（公有领域） | MP3 解码（音效管线，时长样本级精确） |
| **QuickJS** | 2024-01-13，`src/vendor/quickjs/` | MIT | 执行用户导入的 LX 音源脚本 |
| **three.js** | 0.128.0，`stage/web/vendor/three.module.js` | MIT | 舞台粒子 / 3D 视觉 |
| **GSAP** | `stage/web/vendor/gsap.min.js` | GreenSock 标准"免版税"许可（Standard "No Charge" License） | 舞台动画 |
| **music-tempo** | killercrush，`stage/web/vendor/music-tempo.min.js` + `stage/web/mineradio/music-tempo.LICENCE` | MIT | 离线节拍检测（舞台节拍注入） |
| **Mineradio 舞台内核** | `stage/web/mineradio/`（`README.md` 内附许可说明） | **GPL-3.0** | 独立进程 `MuyunStage.exe`（纯 Win32 + WebView2）承载，与主程序经命名管道通信 |
| **WebView2** | Microsoft Edge WebView2 Runtime（终端用户系统自带或由安装器引导安装） | 微软运行时许可 | `MuyunStage.exe` 内嵌浏览器控件 |

## 二、仅在构建期拉取、不随仓库分发的组件

| 组件 | 获取方式 | 许可 | 说明 |
|---|---|---|---|
| **WebView2 SDK**（`WebView2.h` + `WebView2Loader.dll`） | 运行 `stage\sdk\fetch-sdk.ps1`，从 nuget.org 拉取 `Microsoft.Web.WebView2` | 微软 SDK 许可 | 已在 `.gitignore` 中排除，避免重复分发微软 SDK；构建 `MuyunStage` 前必须先执行该脚本 |
| **three.js / gsap 压缩包** | 同上脚本从 npm registry 拉取 | 同上游 | 解包后只保留 `stage/web/vendor/` 下的产物文件 |

## 三、参考与致谢（未拷贝代码，或仅移植协议思路）

| 项目 | 许可 | 关系 |
|---|---|---|
| **Sollin-Music-Desktop** | MIT | 早期 Electron 原型参考（本项目为 Qt/QML 重写） |
| **lx-music-desktop** / **lx-music-mobile** | Apache-2.0 | 音源脚本运行机制、各平台接口端点与同步协议的移植参考；如有逐行拷贝的文件会保留其原版权头 |
| **Mineradio** | GPL-3.0 | 舞台视觉与节拍方案（见 `stage/`，随仓库以 GPL-3.0 分发） |

## 四、音乐内容与接口

本程序**不包含**任何音乐文件、歌词内容或平台账号数据。内置的平台适配只访问各平台**公开接口**做搜索、
榜单、封面、歌词与歌单浏览；**播放与下载完全依赖使用者自行导入的第三方 LX 音源脚本**。
使用者应自行确认其行为符合所在地法律及平台服务条款。

## 五、再分发注意

若你基于本仓库分发二进制包，请确保：

1. 保留本文件、`LICENSE`，以及 `stage/web/README.md`、`stage/web/mineradio/README.md`、
   `stage/web/mineradio/music-tempo.LICENCE`；
2. 随包分发 Qt / FFmpeg 等 LGPL 组件的**许可文本**，且不要静态链接 LGPL 组件（本项目按动态链接设计）；
3. `stage/` 部分若被修改，整体需按 **GPL-3.0** 提供对应源码。
