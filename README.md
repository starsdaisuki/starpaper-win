<div align="center">

<img src="res/icon.png" width="120" alt="StarPaper">

# StarPaper for Windows

**Turn a local video into your Windows desktop wallpaper. One exe, no runtime to install.**

[![Microsoft Store](https://img.shields.io/badge/Microsoft%20Store-Get-0067b8)](https://apps.microsoft.com/detail/9NNB1P5TXVFB)
[![release](https://img.shields.io/github/v/release/starsdaisuki/starpaper-win?label=release&color=4c1)](https://github.com/starsdaisuki/starpaper-win/releases)
[![arch](https://img.shields.io/badge/arch-x64%20%7C%20ARM64-555)](#install)
[![license](https://img.shields.io/github/license/starsdaisuki/starpaper-win)](LICENSE)

**English** · [简体中文](README.zh-CN.md)

</div>

A single 3.1 MB executable and one process. It draws through Direct3D 11 and decodes
through Media Foundation — both already part of Windows — so there is no .NET runtime to
install, no bundled player and no background service.

## Install

Three routes, same program. **x64 and ARM64 are both natively compiled**; ARM laptops do
not run this under emulation.

### Microsoft Store (recommended)

<https://apps.microsoft.com/detail/9NNB1P5TXVFB>

```powershell
winget install 9NNB1P5TXVFB
```

The Store build updates itself and uninstalls cleanly — settings travel with the package
and nothing is left in the registry. Launch-at-login uses the MSIX `StartupTask`, so it
shows up under Task Manager → Startup where the user can turn it off.

### scoop

```powershell
scoop bucket add starsdaisuki https://github.com/starsdaisuki/scoop-bucket
scoop install starpaper
```

### Direct download

Unzip a build from [Releases](https://github.com/starsdaisuki/starpaper-win/releases) and
run it. Settings live in `HKCU\Software\StarPaper`; remove that key when you uninstall.

## Features

- Settings window with **dark / light themes**, **English / 简体中文**, and eight
  category pages. On first run the UI follows the system language: Simplified Chinese on
  Chinese systems, English everywhere else.
- **Video library** — add the wallpapers you use often and **click a thumbnail to
  switch**. Thumbnails come from the shell, so nothing is decoded to build them.
- **Auto-advance** on end or on a timer, optionally shuffled. It is a switch above the
  library; the library still works with it off.
- **Schedules** — separate day and night videos that swap on time, including night ranges
  that cross midnight.
- **Four independent pause conditions**: covered by a window, screen locked, on battery,
  system power-saving mode.
- **Drag-to-frame cropping** — drag the box in the preview, scroll to zoom, and the
  desktop follows live.
- **15 image adjustments**: exposure, brightness, contrast, highlights, shadows, gamma,
  saturation, vibrance, temperature, tint, blur, sharpen, vignette, vignette radius, dim.
- Loops mp4, mov, mkv, avi, wmv and m4v with hardware decoding — see
  [Video formats](#video-formats) for what that does and does not cover.
- **Fills the screen by default** (scaled up proportionally, overflow cropped), so a video
  whose aspect ratio differs from the display still has no black bars.
- **One window per monitor**, each playing and fitting to its own resolution.
- Rebuilds automatically when the display topology changes — an external screen, a virtual
  display from a streaming host, or a resolution change.
- Tray menu: pause / resume, pick a video, mute, pause when covered, launch at login, quit.
- Mute toggle and a volume slider (only the primary display's copy makes sound).
- Mouse events pass through — right-click and double-click on the desktop still work.
- Settings are stored in `HKCU\Software\StarPaper`; no files are written.

## Video formats

**StarPaper bundles no decoders.** It hands the file to Media Foundation, the decoding
stack that ships with Windows, and only takes the finished frames. That is what buys the
3.1 MB binary and the zero-dependency install — and it is also the one thing StarPaper is
clearly worse at than heavier players.

### Will my video play?

Open it in Windows' own **Media Player** (or Movies & TV). **If that plays it, StarPaper
plays it. If it does not, StarPaper will not either** — same decoders, same answer.

H.264 in an mp4 needs nothing extra and is the safe choice for a wallpaper. Most other
failures are fixed by one of two Microsoft Store packages: *HEVC Video Extensions* (for
H.265 — preinstalled by many OEMs, a paid item on retail Windows) or *AV1 Video Extension*
(free). Ten-bit and HDR sources depend on the GPU driver as well, and can fail even when
the eight-bit version of the same codec plays.

When a video cannot be loaded you get a warning dialog rather than a silent black desktop.
The general fix is to transcode once:

```bat
ffmpeg -i input.mkv -c:v libx264 -crf 18 -c:a aac output.mp4
```

### A known failure that is not about codecs

A **fragmented MP4** (`moof`/`mdat`) that declares a non-audio/video track — a timecode
(`tmcd`) track from a camera or a drone, for example — whose sample data sits at the very
end of the file. Media Foundation gives up on it after about eight seconds. Remuxing is
enough; no re-encode needed:

```bat
ffmpeg -i input.mp4 -map 0:v:0 -c copy -write_tmcd 0 output.mp4
```

### The trade

Players built on ffmpeg or mpv play everything above out of the box. They pay 78–200 MB
and a bundled decoder stack for it. StarPaper takes the other side of that trade
deliberately: whatever Windows can already decode, at 3.1 MB.

## Build

**On Windows (recommended — smaller output)**

Open `x64 Native Tools Command Prompt` (or `ARM64 Native Tools Command Prompt` on an ARM
machine) and run:

```bat
build.bat
```

Nothing in the source is architecture-specific; a different command prompt is the whole
difference between an x64 and a native ARM64 build.

**Cross-compiling from macOS / Linux**

```bash
brew install mingw-w64
make          # → StarPaper.exe (x64)
make deps     # verify the output only needs system DLLs
```

ARM64 cross-compiles from a Mac too, with no Windows machine involved:

```bash
# llvm-mingw is not in brew — download the macos-universal release and unpack it
# to ~/.local/llvm-mingw:  https://github.com/mstorsjo/llvm-mingw/releases
make arm64        # → StarPaper-arm64.exe (native AArch64, no Prism emulation)
make deps-arm64
```

`CXXFLAGS`, `LDFLAGS` and `LIBS` are shared between the two toolchains verbatim.

## Run

```
StarPaper.exe                      asks for a video on first run, then remembers
StarPaper.exe "D:\wallpaper.mp4"   open a specific file
```

### Adding it to the Start menu

StarPaper is a single portable exe and runs from anywhere. But **Windows' Start menu does
not scan the disk for executables** — it lists `.lnk` files under
`%APPDATA%\Microsoft\Windows\Start Menu\Programs`. "I can't find StarPaper in search" is
not about being portable; there is simply no shortcut there.

`tools/register-app.ps1` handles it, writing only to `HKCU` and `%APPDATA%` — no
administrator rights needed:

```powershell
.\tools\register-app.ps1                       # Start menu shortcut + App Paths (Win+R: starpaper)
.\tools\register-app.ps1 -WithUninstallEntry   # also add a Settings → Apps entry
.\tools\register-app.ps1 -Remove               # undo everything
# add -ExePath "D:\path\StarPaper.exe" if the exe is not in the default location
```

This does not depend on the Windows Search service: the app list is resolved by
AppResolver, and search still finds StarPaper on machines where `WSearch` is disabled.

## Known limitations

- Format support is whatever Windows can decode — see [Video formats](#video-formats).
- The preview uses the **first display's** aspect ratio; on a multi-monitor setup with
  differing ratios, the framing box on the other display is only an approximation.
- No desktop clock, no per-display videos, no global hotkeys — those exist only in the
  macOS build of StarPaper so far.
- All displays play the same video.
- Image adjustments are not a bit-exact port of the macOS build's Core Image filters
  (`CIHighlightShadowAdjust` is a local algorithm and `CIVignette`'s radius has no
  published formula). Slider ranges and defaults match and the result looks the same, but
  pixel values differ slightly.
- Occlusion detection only looks at the foreground window; it does not compute per-window
  visible regions.
- The desktop attach mechanism does not work under a replacement shell (anything that is
  not Explorer).
- **The multi-monitor path has not been tested on real dual-screen hardware** — only on a
  single display with resolution changes.

## How it works

The desktop attach trick, the Media Foundation frame-server path, why the crop is
expressed on the source rectangle, the shader chain, and the measured cost — all in
[`docs/internals.md`](docs/internals.md) ([简体中文](docs/internals.zh-CN.md)).

## Related

macOS build: [starsdaisuki/starpaper](https://github.com/starsdaisuki/starpaper)
