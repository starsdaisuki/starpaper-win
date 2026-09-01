#pragma once
#include <windows.h>

namespace desktop {

// 找到（必要时先逼 Explorer 生成）那个"在桌面图标背后"的壁纸宿主窗口。
// 不同 Windows 版本上它可能是 WorkerW，也可能就是 Progman 本身。
HWND FindWallpaperHost();

// 把 hwnd 挂进壁纸层，并对齐到 monitorRect（虚拟屏幕坐标）。
bool AttachToDesktop(HWND hwnd, HWND host, const RECT& monitorRect);

// 虚拟屏幕矩形（多显示器时是包住所有屏的那个大矩形）。
RECT VirtualScreenRect();

} // namespace desktop
