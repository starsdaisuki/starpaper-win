# StarPaper for Windows — internals and measurements

**English** · [简体中文](internals.zh-CN.md)

For anyone reading or changing the code. The README keeps what a user needs; the traps and
the numbers live here.

---

## 1. Attaching to the desktop layer

`src/desktop.cpp`

Send the undocumented `0x052C` message to `Progman`. That forces Explorer to spawn a
`WorkerW` window behind the desktop icons; `SetParent` then puts our window inside it. The
wallpaper ends up below the icons and above the desktop background.

⚠️ Windows 10 1803+ has two different desktop structures. Check whether `Progman` has
`WS_EX_NOREDIRECTIONBITMAP` to tell them apart — in the new one `WorkerW` is a **child** of
`Progman`, in the old one a **sibling**, and the search is completely different. This is
the only genuinely easy thing to get wrong in this file.

The mechanism is a publicly known Windows technique, not anyone's proprietary work.

## 2. Playback: frame-server mode

`src/player.cpp`

Decoding goes to Media Foundation's **Media Engine** (`IMFMediaEngine`), but **we do the
presentation ourselves**. Give the Media Engine an `IMFDXGIDeviceManager` and do **not**
give it `MF_MEDIA_ENGINE_PLAYBACK_HWND`; it then stops drawing into a window on its own and
instead waits for us to call `TransferVideoFrame` to copy the current frame into our
texture.

We hold a D3D11 device and a swap chain. The render loop is three steps:

```
OnVideoStreamTick(&pts)     ← is there a new frame? (judge by pts changing, not the return value)
TransferVideoFrame(back, &src, &dst, &border)
Present(1, 0)
```

**Why these few dozen lines are worth writing**: `TransferVideoFrame`'s source rectangle is
in **normalised 0..1 coordinates** and the destination is **pixels on a texture we created
ourselves** — nothing anywhere in that chain will do a DPI conversion on our behalf. The
alternative, letting the Media Engine lay it out itself via `UpdateVideoStream`, cannot be
made to line up at 150% scaling no matter what you pass.

Side benefit: the frames pass through our hands, so brightness, contrast and blur are just
a shader away.

Dependencies stay limited to system DLLs — D3D11, DXGI and Media Foundation all ship with
Windows. No bundled mpv (78–111 MB) and no WPF `MediaElement` (which drags in .NET).

## 3. Fill-the-screen: express the crop on the source, not the destination

```
display 3024x1890 (1.6:1)  ×  video 1920x1080 (16:9)

video is wider → cut the sides, keep the full height
sw  = 1080 x 1.6 = 1728          (largest source rect matching the display's ratio)
ox  = (1920 - 1728) x 0.5 = 96   (focus = 0.5, centred)
src = 96/1920 .. 1824/1920 = 0.050 .. 0.950   ← handed to TransferVideoFrame
dst = 0,0 .. 3024,1890                        ← the whole surface
```

Then multiply in `zoom` (narrow the source rect) and `focusX`/`focusY` (shift it). A
normalised source rectangle has no unit that can be misread, **so there is no DPI ambiguity
on this path**.

> ⚠️ **Do not go back to "make the destination rect larger than the window and let the
> window clip it."**
>
> That requires the Media Engine and us to agree exactly on the destination coordinate
> space, and at 150% scaling we do not. Three hypotheses were tried and they contradict
> each other: passing physical pixels scales the picture up ~1.5x; passing logical pixels
> shrinks it to 2/3 and exposes the system wallpaper at the bottom right; passing `NULL`
> and letting it compute still scales up *and* adds a black band at the top. Declaring
> `PER_MONITOR_AWARE_V2` changes none of them. The window geometry itself was diagnosed as
> exactly correct — the problem is inside that black box, so the only way around it is not
> to use it.
>
> An earlier trap: the first attempt was "oversize the window and clip with
> `SetWindowRgn`". The geometry was exact, but the Media Engine **does not follow a window
> resize**, so the picture was wrong anyway.

> 📏 **How to verify**: this class of error is **invisible on a real wallpaper** — two
> screenshots with opposite focus values look identical to the eye. You need a synthetic
> pattern with a ruler on it and you have to compare boundary positions as numbers. The
> tools and the fixed procedure are in [`../tools/`](../tools/). Measured for the
> parameters above:
>
> ```
> x=5% cyan line    predicted 0.0     measured 6     Δ +6.0
> x=95% cyan line   predicted 3024.0  measured 3019  Δ -5.0   (the line is 6px wide x1.75 scaling)
> 10% grid line     predicted 840.0   measured 840   Δ +0.0
> ```

Turning off fill-the-screen falls back to letterboxing: the source is the whole frame, the
destination is scaled proportionally and centred, and we clear the surroundings ourselves
with `ClearRenderTargetView`.

## 4. Image adjustments: at defaults the shader chain is never built

With every parameter at its default the Media Engine still writes straight into the back
buffer and there is no extra cost at all. Touching any parameter switches to this path:

```
Media Engine ──TransferVideoFrame──▶ intermediate texture
                                        │
                          1–4 fullscreen passes (combined as needed)
                          adjust → sharpen → downsample → blur X → blur Y
                                        ▼
                              destination rect in the back buffer ──▶ Present
```

Large blur radii **downsample first**: a radius of 60 at 4K would need 240 taps directly,
but only 9 after a 6x reduction — and blur discards high frequencies anyway, so the result
is indistinguishable. That downsample pass must be a box filter, not a single bilinear tap:
bilinear averages only 2×2 texels, so going from 3024 to 504 misses 8/9 of the pixels and
aliases into faint horizontal banding.

`d3dcompiler_47.dll` is fetched at runtime with `LoadLibrary` and is not a link-time
dependency. If some stripped-down system does not have it, adjustments simply do nothing
and playback continues.

## 5. Settings window and visual framing

The white box in the preview is **exactly the region that will appear on the desktop**;
the dimmed area outside it gets cropped. Drag the box to compose, scroll to zoom, and the
desktop follows **live**.

> ⭐ The preview box and the desktop crop call the **same function**, `FramingRect()` in
> `main.cpp`, so they cannot disagree. This is also why visual framing had to come *after*
> the frame-server work: when the lower layer is wrong, the UI just lies along with it.
>
> Measured (3024x1890 at 150% scaling, 1920x1080 test pattern): the box occupies **89.99%**
> of the image width (expected 90%), with 4.97% / 5.04% margins — sub-pixel agreement.

⚠️ A UI trap worth recording: **in a per-monitor-v2 process, the `lfMessageFont` returned
by `SystemParametersInfoW` is already sized for the current DPI and must not be scaled
again.** Doing so makes text 1.5x too large at 150%, so button labels no longer fit and
descriptions get truncated. Use `SystemParametersInfoForDpi` to get a DPI-specific font.

## 6. Pause-when-covered is off by default

Pausing while covered saves CPU, but resuming has to restart playback, so Win+D back to the
desktop stutters before it moves. Default is off; the tray menu has the switch.

## 7. Multiple displays and streaming

| Scenario | What happens | How it is handled |
|---|---|---|
| Resolution of the primary display changes during streaming | `WM_DISPLAYCHANGE` | Windows are rebuilt and refitted (verified) |
| A virtual display is enabled (e.g. Sunshine's Zako Display Adapter) | An extra screen appears | It gets its own window and player, showing a complete picture rather than half of one |

⚠️ An early version used one window spanning the bounding box of all displays. With
multiple monitors that stretched a single picture across both, each showing half. **That is
why it is one window per display now.**

The cost is decoding once per display. Wallpaper Engine, Lively and mpv-based solutions all
pay it. Avoiding it means giving up the system's windowed presentation and writing D3D11
shared textures by hand — more than double the code, not worth it yet.

## 8. Diagnostics: state is published in a window title

The tray window (`StarPaperTray`, never shown) carries live internal state in its title:

```
StarPaper screens=1 playing=1 paused=0 usr=0 lock=0 opt=0 fill=1
          src=50,0..950,1000 dst=0,0..3024,1890 f=50/50 z=100 fr=634
```

- `src` is the **normalised crop rectangle on the source**, in per-mille
  (`50,0..950,1000` = 5% cut from each side)
- `dst` is the **destination rectangle in the swap chain** (inset when letterboxing)
- `fr` is the cumulative count of frames actually drawn — **the only hard evidence that the
  picture is moving**

`GetWindowTextW` reads it; no log file needed.

⚠️ Do not infer "is it actually paused?" from CPU usage: the same state has been measured
anywhere between 9% and 18%. A single sample is equally untrustworthy — across four
steady-state rounds CPU was measured at both 11.7% and 26.8% while the frame rate held at
60.

## 9. Measured cost

3024x1890 display at 150% scaling, a 4K60 H.264 wallpaper, four steady-state rounds of 8
seconds:

| | |
|---|---|
| Frame rate | 59.8 fps (the video is 60 fps; pts dedup draws not one frame extra) |
| CPU | 22.3% of one core |
| Memory | 140–146 MB working set |

A 1080p30 pattern gives 30.0 fps / about 7% of one core / 109 MB.

> Two attempts at throttling away redundant frames were **both worse — do not retry them**:
> a waitable swapchain pinned the rate at 60.0 but pushed CPU to 15.6–20.8%;
> `QueryPerformanceCounter` timing plus `Sleep(2)` dropped straight to 38.4 fps, because
> Windows' default timer resolution is 15.6 ms and `Sleep(2)` actually sleeps through a
> whole refresh interval. Deduplicating on `pts` is already enough.

### Do not use numbers measured inside a VM

The same 4K60 wallpaper inside Parallels / Windows 11 ARM64 on Apple Silicon costs
**32.8% (normalised over 4 cores) and about 420 MB** — an order of magnitude above the
table. The software did not get heavier: **VMs on Apple Silicon have no hardware video
decode engine** (the GPU counters expose only `engtype_3d`, no `engtype_VideoDecode`), so
4K60 H.264 is decoded entirely on the CPU. Parallels, VMware Fusion and UTM are all the
same — DXVA on ARM does not map to a hardware decoder; UTM is worse still, with no 3D
acceleration on the Windows side at all.

⭐ **The number that matters is on the host.** The percentage seen inside the guest badly
underestimates the cost. Measuring the host's `prl_vm_app` at the same moment: **4K60 costs
the host 1.78 cores** (an idle VM baselines at 0.9%) while the guest reports only 28.4%.
The difference is frames crossing the virtualised display pipeline. **To run this in a VM
long-term, use 1080p30**: host goes 178.4% → **85.1% (−52%)**. Lowering the frame rate is
−37% and lowering the resolution −42%; together they are the best combination.

One counter-intuitive result from the same environment: **the x64 build under Prism
emulation costs 37.7% and the native ARM64 build 32.8% — only 13% apart.** The reason is
that Windows on ARM ships its system DLLs as **ARM64X hybrid binaries**: the
`msmpeg2vdec.dll` (H.264 software decoder), `MFPlat.dll` and `d3d11.dll` loaded by an x64
process already execute as native ARM64 inside, and Prism only translates the application's
own few thousand lines. **Going native saves the application's own overhead, not the
decoding that dominates.**
