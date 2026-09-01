<#
  register-app.ps1 - make the portable StarPaper.exe discoverable by Windows search.

  StarPaper ships as a single portable exe. Windows Start menu search does not scan
  the disk for exe files: it lists the .lnk shortcuts found under the Start Menu
  Programs folders. Dropping one shortcut there is the whole trick - no installer,
  no MSI, no Program Files, no signing.

  Everything here is per-user (HKCU + %APPDATA%), so it needs no admin rights and
  touches nothing machine-wide.

    .\register-app.ps1                        # shortcut + App Paths
    .\register-app.ps1 -WithUninstallEntry    # also show up in Settings > Apps
    .\register-app.ps1 -Remove                # undo everything

  ASCII only on purpose: a UTF-8 .ps1 copied to a CP936 host mangles literals.
#>
[CmdletBinding()]
param(
    [string] $ExePath = "$env:USERPROFILE\starpaper-win\StarPaper.exe",
    [string] $Name    = 'StarPaper',
    [switch] $WithUninstallEntry,
    [switch] $Remove
)

$ErrorActionPreference = 'Stop'

$programs = [Environment]::GetFolderPath('Programs')
$linkPath = Join-Path $programs "$Name.lnk"
$appPaths = "HKCU:\Software\Microsoft\Windows\CurrentVersion\App Paths\$Name.exe"
$uninst   = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$Name"

if ($Remove) {
    # File deletion must be recoverable. The recycle-bin API silently degrades to
    # permanent deletion in SSH/session 0, so refuse removal outside a user session.
    $sessionId = (Get-Process -Id $PID).SessionId
    if ($sessionId -eq 0) {
        throw 'Remove must run in an interactive user session; session 0 cannot reliably use the Recycle Bin.'
    }
    if (Test-Path $linkPath) {
        Add-Type -AssemblyName Microsoft.VisualBasic
        [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteFile(
            $linkPath,
            [Microsoft.VisualBasic.FileIO.UIOption]::OnlyErrorDialogs,
            [Microsoft.VisualBasic.FileIO.RecycleOption]::SendToRecycleBin
        )
        Write-Output "recycled shortcut: $linkPath"
    }
    else                     { Write-Output "no shortcut at: $linkPath" }

    # Registry removal is backed up first so -Remove remains reversible.
    $backupDir = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'StarPaper Backups'
    New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $keys = @(
        @{ PsPath = $appPaths; NativePath = "HKCU\Software\Microsoft\Windows\CurrentVersion\App Paths\$Name.exe"; File = "app-paths-$stamp.reg" },
        @{ PsPath = $uninst; NativePath = "HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\$Name"; File = "uninstall-$stamp.reg" }
    )
    foreach ($key in $keys) {
        if (Test-Path $key.PsPath) {
            $backup = Join-Path $backupDir $key.File
            & reg.exe export $key.NativePath $backup /y | Out-Null
            if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $backup)) {
                throw "registry backup failed: $($key.NativePath)"
            }
            Remove-Item $key.PsPath -Recurse -Force
            Write-Output "removed registry key after backup: $($key.PsPath)"
            Write-Output "backup: $backup"
        }
    }
    Write-Output 'done (removed)'
    return
}

if (-not (Test-Path -LiteralPath $ExePath -PathType Leaf)) {
    throw "exe not found: $ExePath  (pass -ExePath to point at it)"
}
$ExePath = (Resolve-Path -LiteralPath $ExePath).Path
$installDir = Split-Path -Parent $ExePath

# --- L1: the Start menu shortcut. This alone is what makes search find it. ---
$shell = New-Object -ComObject WScript.Shell
$lnk = $shell.CreateShortcut($linkPath)
$lnk.TargetPath       = $ExePath
$lnk.WorkingDirectory = $installDir
$lnk.IconLocation     = "$ExePath,0"    # uses the icon compiled into the exe
$lnk.Description      = 'Play a local video as your desktop wallpaper'
$lnk.Save()
Write-Output "shortcut  -> $linkPath"

# --- L2: App Paths, so Win+R "starpaper" works from anywhere ---
New-Item -Path $appPaths -Force | Out-Null
New-ItemProperty -Path $appPaths -Name '(Default)' -Value $ExePath     -PropertyType String -Force | Out-Null
New-ItemProperty -Path $appPaths -Name 'Path'      -Value $installDir  -PropertyType String -Force | Out-Null
Write-Output "app paths -> $appPaths"

# --- L3 (opt-in): an entry in Settings > Apps ---
if ($WithUninstallEntry) {
    $selfCopy = Join-Path $installDir 'register-app.ps1'
    if ($PSCommandPath -and ($PSCommandPath -ne $selfCopy)) {
        Copy-Item -LiteralPath $PSCommandPath -Destination $selfCopy -Force
    }
    $sizeKb = [int]((Get-Item -LiteralPath $ExePath).Length / 1KB)
    New-Item -Path $uninst -Force | Out-Null
    $props = @{
        DisplayName     = $Name
        DisplayIcon     = $ExePath
        DisplayVersion  = (Get-Item -LiteralPath $ExePath).VersionInfo.FileVersion
        Publisher       = 'starsdaisuki'
        InstallLocation = $installDir
        UninstallString = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$selfCopy`" -Remove"
    }
    foreach ($k in $props.Keys) {
        if ($props[$k]) { New-ItemProperty -Path $uninst -Name $k -Value $props[$k] -PropertyType String -Force | Out-Null }
    }
    New-ItemProperty -Path $uninst -Name 'EstimatedSize' -Value $sizeKb -PropertyType DWord -Force | Out-Null
    foreach ($k in 'NoModify','NoRepair') {
        New-ItemProperty -Path $uninst -Name $k -Value 1 -PropertyType DWord -Force | Out-Null
    }
    Write-Output "uninstall -> $uninst"
}

Write-Output ''
Write-Output "done. press Start and type '$Name'."
Write-Output "if it does not show up, check the search service: Get-Service WSearch"
