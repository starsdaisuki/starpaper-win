<div align="center">

<img src="res/icon.png" width="120" alt="StarPaper">

# StarPaper for Windows

**把一段本地视频挂成 Windows 桌面壁纸。一个 exe，不装任何运行时。**

[![Microsoft Store](https://img.shields.io/badge/Microsoft%20Store-Get-0067b8)](https://apps.microsoft.com/detail/9NNB1P5TXVFB)
[![release](https://img.shields.io/github/v/release/starsdaisuki/starpaper-win?label=release&color=4c1)](https://github.com/starsdaisuki/starpaper-win/releases)
[![arch](https://img.shields.io/badge/arch-x64%20%7C%20ARM64-555)](#安装)
[![license](https://img.shields.io/github/license/starsdaisuki/starpaper-win)](LICENSE)

[English](README.md) · **简体中文**

</div>

一个 3.1 MB 的可执行文件，一个进程。画面走 Direct3D 11，解码走 Media Foundation ——
两者都是 Windows 自带的，所以不需要装 .NET 运行时，不外挂播放器，也不常驻服务。

## 安装

三条路，装出来的是同一个程序。**x64 和 ARM64 都是原生编译**，ARM 笔记本上不走模拟。

### Microsoft Store（推荐）

<https://apps.microsoft.com/detail/9NNB1P5TXVFB>

```powershell
winget install 9NNB1P5TXVFB
```

商店版会自动更新，卸载也最干净：配置随包一起走，注册表不留东西。开机启动走 MSIX 的
`StartupTask`，能在「任务管理器 → 启动」里被用户接管。

### scoop

```powershell
scoop bucket add starsdaisuki https://github.com/starsdaisuki/scoop-bucket
scoop install starpaper
```

### 直接下载

[Releases](https://github.com/starsdaisuki/starpaper-win/releases) 里的 zip 解压即用，免安装。
配置写在 `HKCU\Software\StarPaper`，卸载时记得顺手清掉这个键。

## 功能

- 图形化设置窗口：**深色 / 浅色主题**、**English / 简体中文**、八个分类页。首次启动跟随
  系统语言：中文系统用简体中文，其它一律英文
- **视频库**：常用壁纸导进来，**点一下缩略图就切换**（缩略图走系统的，不自己解码）
- **自动轮播**：播完一遍换 / 定时换，可随机 —— 是库上面的一个开关，关着库照样能点
- **日程**：白天 / 夜间两个视频按时间自动切，支持夜间跨午夜
- **四个独立的暂停开关**：被窗口盖住 / 锁屏 / 用电池 / 系统节电模式
- **拖框选取景**：预览里直接拖，滚轮缩放，桌面实时跟随
- **调色 15 个参数**：曝光 / 亮度 / 对比度 / 高光 / 阴影 / 伽马 / 饱和度 / 自然饱和度 /
  色温 / 色调 / 模糊 / 锐化 / 暗角 / 暗角范围 / 压暗
- 循环播放 mp4 / mov / mkv / avi / wmv / m4v，硬件解码 —— 这句话的边界见
  [视频格式](#视频格式)
- **默认填满整块屏**（等比放大、裁掉多余），视频和屏幕比例不同也没有黑边
- **每个显示器一个独立窗口**，各自播放、各自适应分辨率
- 显示器拓扑变化（外接屏、投屏的虚拟显示器、改分辨率）自动重建
- 托盘菜单：暂停 / 继续、选择视频、静音、被窗口盖住时暂停、开机启动、退出
- 静音开关 + 音量滑杆（只有主显示器那一份会出声）
- 鼠标事件穿透，桌面右键和双击照常
- 配置存在 `HKCU\Software\StarPaper`，不写任何文件

## 视频格式

**StarPaper 自己一个解码器都不带。** 它把文件交给 Windows 自带的 Media Foundation，
只接收解好的帧。这正是 3.1 MB 和零依赖的来源 —— 同时也是它相比那些更重的播放器，
**明确更差的那一点**。

### 我这个视频能不能播？

用 Windows 自带的 **媒体播放器**（或「电影和电视」）打开它。**那边能播，StarPaper
就能播；那边不能，StarPaper 也不能** —— 同一套解码器，同一个答案。

mp4 里的 H.264 不需要任何额外东西，是壁纸最稳的选择。其余的失败大多靠微软商店的两个包
解决：**HEVC 视频扩展**（H.265 用，很多 OEM 机器预装，零售版 Windows 上是付费项）和
**AV1 Video Extension**（免费）。10bit / HDR 片源还额外取决于显卡驱动，可能出现
「同一编码的 8bit 版能播、10bit 版不行」。

视频加载不了时会弹一个警告框，而不是让桌面默默黑着。通用解法是转一次码：

```bat
ffmpeg -i input.mkv -c:v libx264 -crf 18 -c:a aac output.mp4
```

### 一个和编码无关的已知失败

**分片 MP4**（`moof`/`mdat`）里声明了一条非音视频轨 —— 比如相机 / 无人机导出的
时间码轨（`tmcd`）—— 而它的样本数据在文件最末尾。这种情况 Media Foundation 会在
约 8 秒后放弃。重封装就够，不用重新编码：

```bat
ffmpeg -i input.mp4 -map 0:v:0 -c copy -write_tmcd 0 output.mp4
```

### 这是一笔交易

基于 ffmpeg 或 mpv 的播放器上面这些开箱即播，代价是 78~200 MB 和一整套自带解码器。
StarPaper 选的是这笔交易的另一边：只播 Windows 本来就能解的，换 3.1 MB。

## 构建

**Windows（推荐，产物更小）**

开 `x64 Native Tools Command Prompt`（ARM64 机器上开 `ARM64 Native Tools Command
Prompt`），然后：

```bat
build.bat
```

代码里没有任何架构相关的东西，换个命令行就是原生 ARM64 版本。

**在 macOS / Linux 上交叉编译**

```bash
brew install mingw-w64
make          # → StarPaper.exe (x64)
make deps     # 核对产物只依赖系统 DLL
```

ARM64 同样能在 Mac 上编，不用开 Windows：

```bash
# llvm-mingw 不在 brew 里，去 release 页下 macos-universal 包，解压成 ~/.local/llvm-mingw
#   https://github.com/mstorsjo/llvm-mingw/releases
make arm64        # → StarPaper-arm64.exe（原生 AArch64，不走 Prism 模拟）
make deps-arm64
```

两套工具链共用同一份 `CXXFLAGS` / `LDFLAGS` / `LIBS`。

## 运行

```
StarPaper.exe                      首次运行弹窗选视频，之后记住
StarPaper.exe "D:\wallpaper.mp4"   直接指定
```

### 让它出现在开始菜单里

StarPaper 是单文件绿色 exe，放哪都能跑。但 **Windows 的开始菜单搜索不扫磁盘上的
exe** —— 它列的是 `%APPDATA%\Microsoft\Windows\Start Menu\Programs` 下的 `.lnk`。
所以「搜不到 StarPaper」跟便携与否无关，只是那里没有快捷方式。

`tools/register-app.ps1` 一次搞定，**全部写在 HKCU + %APPDATA%，不需要管理员**：

```powershell
.\tools\register-app.ps1                       # 开始菜单快捷方式 + App Paths（Win+R 敲 starpaper）
.\tools\register-app.ps1 -WithUninstallEntry   # 再加一条「设置 → 应用」里的条目
.\tools\register-app.ps1 -Remove               # 全部撤销
# exe 不在默认位置时加 -ExePath "D:\path\StarPaper.exe"
```

这条路不依赖 Windows Search 服务：应用列表走的是 AppResolver，`WSearch` 被设成
Disabled 的机器上照样搜得到。

## 已知限制

- 能播什么完全取决于 Windows 能解什么 —— 见[视频格式](#视频格式)
- 预览用的是**第一块屏**的宽高比；多屏且比例不同时，另一块屏的取景框只是近似
- 没有桌面时钟、每屏不同视频、全局快捷键 —— 那些还只在 macOS 版 StarPaper 里
- 所有显示器播同一个视频
- 调色不是逐位复刻 macOS 版的 Core Image 滤镜（`CIHighlightShadowAdjust` 是局部算法、
  `CIVignette` 的 radius 没有公开公式）。滑杆刻度和默认值一致，观感接近，但像素值会有
  细微差别
- 遮挡检测只看前台窗口，不做逐窗口可见区域计算
- 换过第三方 shell（不是 Explorer）时桌面挂载机制不可用
- **多显示器路径尚未在真的双屏环境下实测**，只验证过单屏 + 分辨率切换

## 原理

挂到桌面层的做法、Media Foundation 的 frame-server 路径、为什么裁剪表达在源矩形上、
shader 链子，以及实测代价 —— 都在
[`docs/internals.zh-CN.md`](docs/internals.zh-CN.md)（[English](docs/internals.md)）。

## 相关

macOS 版：[starsdaisuki/starpaper](https://github.com/starsdaisuki/starpaper)
