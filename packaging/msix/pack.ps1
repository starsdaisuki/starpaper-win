# 把 make-layout.sh 组好的布局目录打成 .msix（在 Windows 上跑）。
#
#   .\pack.ps1 -Layout C:\msixbuild\layout-arm64 -Out C:\msixbuild\StarPaper-arm64.msix
#   .\pack.ps1 -Layout ... -Out ... -SelfSign     # 只为本地安装测试用，提交时不要签
#
# ⚠️ 提交到商店的包**不要自己签** —— 商店会用微软证书重签，自己签上去反而可能因为
# publisher 对不上而校验失败。自签名只用于「在本机装一遍真包」这种验证。

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Layout,
    [Parameter(Mandatory)][string]$Out,
    [switch]$SelfSign,
    [string]$SelfSignSubject
)

$ErrorActionPreference = 'Stop'

function Find-SdkTool([string]$name) {
    $root = 'C:\Program Files (x86)\Windows Kits\10\bin'
    if (-not (Test-Path $root)) { throw "没装 Windows SDK。见本目录 README 的「装 SDK」一节。" }
    # 优先挑和当前架构一致的那份
    $arch = @{ 'ARM64' = 'arm64'; 'AMD64' = 'x64'; 'x86' = 'x86' }[$env:PROCESSOR_ARCHITECTURE]
    $hit = Get-ChildItem $root -Recurse -Filter $name -EA SilentlyContinue |
           Sort-Object { $_.FullName -notlike "*\$arch\*" }, { $_.FullName } |
           Select-Object -First 1
    if (-not $hit) { throw "SDK 里找不到 $name。装的时候 feature 要带 OptionId.DesktopCPPx64 / DesktopCPParm64。" }
    return $hit.FullName
}

$makeappx = Find-SdkTool 'makeappx.exe'
Write-Host "makeappx: $makeappx"

$manifest = Join-Path $Layout 'AppxManifest.xml'
if (-not (Test-Path $manifest)) { throw "布局里没有 AppxManifest.xml：$Layout" }
$manifestText = Get-Content $manifest -Raw
if ($manifestText -match 'REPLACE-|__[A-Z0-9_]+__') {
    throw "AppxManifest.xml 里还有身份占位符；拒绝生成可能误传商店的包。"
}
[xml]$manifestXml = $manifestText
$manifestPublisher = [string]$manifestXml.Package.Identity.Publisher
if (-not $manifestPublisher) { throw 'AppxManifest.xml 缺少 Package/Identity/@Publisher。' }

# 不永久删除已有包。若同名目标存在，保留成带时间戳的旁路备份。
if (Test-Path -LiteralPath $Out) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $backup = "$Out.previous-$stamp"
    if (Test-Path -LiteralPath $backup) { throw "备份目标已存在：$backup" }
    Move-Item -LiteralPath $Out -Destination $backup
    Write-Host "previous package: $backup"
}
& $makeappx pack /d $Layout /p $Out /o
if ($LASTEXITCODE -ne 0) { throw "makeappx 失败（exit $LASTEXITCODE）" }

if ($SelfSign) {
    # ⚠️ Add-AppxPackage -AllowUnsigned 对普通包**不管用**，会报
    #    0x80073D2C "publisher is not in the unsigned namespace"。
    #    要在本机装真包验证，只能自签 + 把证书塞进 LocalMachine\TrustedPeople。
    $signtool = Find-SdkTool 'signtool.exe'
    if (-not $SelfSignSubject) { $SelfSignSubject = $manifestPublisher }
    if ($SelfSignSubject -ne $manifestPublisher) {
        throw "自签 Subject 必须逐字等于 manifest Publisher：$manifestPublisher"
    }
    $cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $SelfSignSubject } | Select-Object -First 1
    if (-not $cert) {
        $cert = New-SelfSignedCertificate -Type Custom -Subject $SelfSignSubject `
                  -KeyUsage DigitalSignature -FriendlyName 'StarPaper test signing' `
                  -CertStoreLocation 'Cert:\CurrentUser\My' `
                  -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
    }
    # 只把公钥证书复制进 TrustedPeople；签名直接使用 CurrentUser\My 中的私钥。
    # 不再把私钥导出成硬编码密码的 selfsign.pfx 落盘。
    $publicBytes = $cert.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert)
    $publicCert = New-Object Security.Cryptography.X509Certificates.X509Certificate2 -ArgumentList @(,$publicBytes)
    $trusted = New-Object Security.Cryptography.X509Certificates.X509Store('TrustedPeople', 'LocalMachine')
    try {
        $trusted.Open([Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
        $trusted.Add($publicCert)
    } finally {
        $trusted.Close()
    }
    & $signtool sign /fd SHA256 /s My /sha1 $cert.Thumbprint $Out
    if ($LASTEXITCODE -ne 0) { throw "signtool 失败（exit $LASTEXITCODE）" }
}

'{0}  {1:N1} KB' -f $Out, ((Get-Item $Out).Length / 1KB)
