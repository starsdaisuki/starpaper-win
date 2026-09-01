# tools —— 验收工具

这套东西存在的唯一理由：**几何数字全对，画面照样可以是错的**。

2026-08-22 犯过两次同样的错：读回矩形、确认数值符合预期、宣布修好，
然后一张真实截图把结论推翻。更糟的是，**用真实壁纸截图也看不出来** ——
两张 focus 相反的截图肉眼完全一样。只有带刻度的合成图案能暴露。

所以验收流程固定成三步，缺一不可：

| 步骤 | 工具 | 回答什么 |
|---|---|---|
| 1. 造一张能暴露错误的图案 | `gen_testcard.py` + ffmpeg | 画面被裁掉多少，能不能读出来 |
| 2. 在**交互会话**里跑起来并截图 | `probe.exe` | 屏幕上真正显示的是什么 |
| 3. 拿像素位置对预测值 | `verify_crop.py` | 边界落点差了几个像素 |

发布包另走 [`verify-release.py`](verify-release.py)：它直接打开最终 MSIX，核对
版本、架构、Store identity、全部中英文文案、语言回退约束、隐私标识与本地 EXE 哈希。
它不替代真机交互测试；完整流程见 [`../packaging/msix/README.md`](../packaging/msix/README.md)。

## gen_testcard.py

生成 1920x1080 的刻度图案：10% 网格、5%/95% 的青线与黄线、最外圈红框、
四角色块、每 2.5% 一个边缘刻度。需要 `pillow`。

```bash
python3 tools/gen_testcard.py                    # → testcard.png
ffmpeg -y -loop 1 -framerate 30 -i testcard.png -t 10 \
  -vf "drawbox=x='96+mod(t*340\,1690)':y=640:w=48:h=48:t=fill:c=red" \
  -c:v libx264 -pix_fmt yuv420p -crf 20 -r 30 testcard.mp4
```

那个来回移动的红块是**在播没在播**的肉眼判据（帧计数是程序侧的判据）。

## probe.exe

```bash
x86_64-w64-mingw32-g++ -std=c++17 -O2 -municode tools/probe.cpp -o probe.exe \
  -static -static-libgcc -static-libstdc++ -lgdiplus -lgdi32 -luser32
```

| 子命令 | 干什么 |
|---|---|
| `probe.exe <StarPaper.exe> <video> <dir>` | 拉起程序、抓状态、量帧率/CPU/内存、截图 |
| `probe.exe scenarios <dir>` | 依次走取景/缩放/letterbox/暂停，每步一张截图 |
| `probe.exe measure <dir> [轮数] [每轮秒数]` | 稳态多轮测量（单轮噪声很大，别信一轮的数） |
| `probe.exe resolution <dir>` | 切一次分辨率再切回来，验 ResizeBuffers 那条路 |
| `probe.exe info <dir>` | 只报告现状，什么都不改 |
| `probe.exe setmode <w> <h> [hz]` | 救场用：显式设分辨率并写注册表 |

**必须用计划任务跑，不能直接 SSH 执行**：SSH 进来是 session 0，那里的桌面是一块
1024x768 的假屏，截不到真实画面。

```powershell
$d = "$HOME\starpaper-win"
$act = New-ScheduledTaskAction -Execute "$d\probe.exe" -Argument "scenarios `"$d`""
# ⚠️ UserId 要用 主机名\用户名；$env:USERDOMAIN 在工作组机器上给的是 WORKGROUP，会报
#    "No mapping between account names and security IDs"
$pri = New-ScheduledTaskPrincipal -UserId ($env:COMPUTERNAME + "\" + $env:USERNAME) `
       -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName SPScen -Action $act -Principal $pri -Force
Start-ScheduledTask -TaskName SPScen
```

**为什么是 exe 不是 PowerShell 脚本**：脚本里一旦出现内联 `Add-Type` + user32 P/Invoke，
Windows Defender 的 AMSI 直接判 `ScriptContainedMaliciousContent` 拒绝执行。
（另外 UTF-8 无 BOM 的 .ps1 里写中文，PowerShell 5.1 会按 GBK 读，解析直接崩。）

## verify_crop.py

把截图上的刻度位置和预测值逐条对比。src/dst 直接抄程序状态栏里那一串。

```bash
WORK_BOTTOM=1818 python3 tools/verify_crop.py shot.jpg 50 0 950 1000
#                                             ↑图     ↑src 千分比（l t r b）
python3 tools/verify_crop.py shot.jpg 0 0 1000 1000  0 94 3024 1795   # 带 dst = letterbox
```

`WORK_BOTTOM` 是任务栏顶边：任务栏是半透明毛玻璃，既不黑也不保色，
落在里面的采样一律不作数（不设这个会误判成 FAIL）。
