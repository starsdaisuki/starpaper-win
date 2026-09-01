# MSIX packaging (Microsoft Store)

商店版和 scoop 版来自同一份源码；不同发布渠道会各自全量编译，PE 构建时间戳可能不同，
所以不能用「另一个渠道跑过」代替对当前最终包的验证。

## 为什么需要这个目录

MSIX 里**写 `HKCU\...\CurrentVersion\Run` 是静默失效的** —— 值不会进真实注册表，
开机不会启动，也不报错。所以打包版的「开机启动」走的是这个 manifest 里声明的
`windows.startupTask`，由 `src/startup.cpp` 通过 WinRT 调用。

⚠️ `<desktop:StartupTask TaskId="...">` 必须和 `src/startup.cpp` 里的 `kTaskId`
**一字不差**，否则 `StartupTask.GetAsync` 找不到任务，开机启动这一项会自动置灰。

## 三步流程

```bash
# 1) Mac 上全量重编并组布局（C++ 与版本资源都会重建）
./packaging/msix/make-layout.sh arm64
./packaging/msix/make-layout.sh x64
```
```powershell
# 2) Windows 上打包
.\packaging\msix\pack.ps1 -Layout C:\msixbuild\layout-arm64 -Out C:\msixbuild\StarPaper-arm64.msix

# 3) 想在本机装一遍真包验证，加 -SelfSign（提交时**不要**加）
.\packaging\msix\pack.ps1 -Layout ... -Out ... -SelfSign
Add-AppxPackage -Path C:\msixbuild\StarPaper-arm64.msix
```

`pack.ps1` 遇到同名输出时不会永久删除旧包，而是改名保留为
`*.previous-YYYYMMDD-HHMMSS`。`-SelfSign` 会从 manifest 自动读取 Publisher；
不再生成带硬编码密码的 PFX 文件。

⚠️ **`Add-AppxPackage -AllowUnsigned` 对普通包不管用**，会报
`0x80073D2C  publisher is not in the unsigned namespace`。本地装真包只能自签，
且自签证书的 Subject 必须和 manifest 里 `Identity/@Publisher` **一字不差**。

## 手工组布局（make-layout.sh 做的事）

```
layout/
  AppxManifest.xml      ← 本目录这份，三个 __IDENTITY_*__ 占位符换成 Partner Center 真值
  StarPaper.exe         ← make / make arm64 的产物
  Assets/
    StoreLogo.png            50x50
    Square44x44Logo.png      44x44   （另可加 targetsize-16/24/32/48/256 变体）
    Square150x150Logo.png    150x150
    Square71x71Logo.png      71x71
```

布局图标从已经审过的 `res/icon.png` 缩放；EXE 内嵌图标仍来自 `res/StarPaper.ico`。

⚠️ `DefaultTile` 里写了 `Square310x310Logo` 就**必须**同时给 `Wide310x150Logo`，
否则部署报 `0x80080204 manifest is invalid`。

## 装 SDK（只有打包这步需要）

```powershell
# 版本要对上目标系统；下面是 26100 的安装器
Invoke-WebRequest 'https://go.microsoft.com/fwlink/?linkid=2376216' -OutFile winsdksetup.exe
.\winsdksetup.exe /quiet /norestart /ceip off /features `
    OptionId.DesktopCPPx86 OptionId.DesktopCPPx64 OptionId.DesktopCPParm64 `
    OptionId.SigningTools OptionId.WindowsSoftwareLogoToolkit
```

⚠️ 三个踩过的坑：

1. **`makeappx.exe` 属于 Desktop Tools，不属于 Signing Tools。**
   只装 `OptionId.SigningTools` 只会得到 signtool / makecat / cert2spc。
2. **`OptionId.WindowsSoftwareDevelopmentKit` 不是合法的 feature 名**，会报
   `The following features specified by the /features switch are not available`，
   而且**退出码是 1001、看起来像环境问题**。真因只在
   `%TEMP%\windowssdk\*.log` 里，别照着退出码猜。
3. **同一个 bundle 装过之后再用不同 `/features` 跑一次会直接 1001**（被判成「已安装」）。
   要么一次装齐，要么先 `/uninstall`。

⚠️ **WACK（`appcert.exe`）没有 ARM64 版** —— 安装器只提供
`WindowsAppCertificationKitx86` / `x64` 两个包，所以 ARM64 机器上装不出来。
要跑 WACK 得找一台 x64 Windows。Partner Center 不把本地 WACK 设成上传前置条件，
商店认证也会运行它；但微软明确要求开发者提交前先跑，因此把它记录为**推荐发布门禁**，
不要再笼统写成「非必需」。

## 松散注册验证（不需要证书，也不需要 SDK）

```powershell
# 开发者模式
New-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock' `
  -Name AllowDevelopmentWithoutDevLicense -Value 1 -PropertyType DWord -Force

Add-AppxPackage -Register '<layout>\AppxManifest.xml'
Start-Process 'explorer.exe' 'shell:AppsFolder\<PackageFamilyName>!StarPaper'
```

⚠️ **这两条命令必须在用户自己的交互会话里跑。** 从 SSH（session 0）执行会报
`0x80070005 Access is denied`，真正的原因藏在 `Get-AppPackageLog` 里的
`Failed to initialize PLM` —— 查 ACL / 策略 / 服务全都是白费功夫。

## 提交到商店

- MSIX **不用自己签名**：商店会用微软证书重签（裸 EXE/MSI 那条路才要自己买证书）。
- 包版本号四段、**末段必须 0**、**首段不能是 0**。
- `Identity` 的 `Name` / `Publisher` 必须逐字等于 Partner Center 里的 App identity。
- 用户明确选择的语言优先；首次启动仅系统 UI 主语言为中文时默认简体中文，
  其它语言一律默认英文，未知/异常环境也以英文为安全兜底。

## 最终产物门禁

打包完成后，不以「MSIX 里的 EXE 等于本地 EXE」作为完成证据。运行：

```bash
python3 tools/verify-release.py \
  build/submit/StarPaper-1.0.2.0-x64.msix \
  build/submit/StarPaper-1.0.2.0-arm64.msix
```

它会直接打开最终 MSIX，检查：manifest/PE 架构、manifest 与 EXE 版本、Store identity
占位符、是否误签、全部中英文文案是否进入最终 EXE、文案枚举顺序、语言回退约束、
精确包内文件清单、PNG 元数据、调试/私密文件和本机标识。正式发布前在已提交的干净源码上
再加 `--require-clean`。

静态门禁通过后仍必须安装当前 x64 包，在交互式 Windows 会话验证首次启动英文、
托盘菜单、播放和开机启动；上架后再从 Store 安装微软重签后的版本做端到端复核。
