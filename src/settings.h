#pragma once
#include <windows.h>

// 图形化设置窗口。托盘只是入口，设置是一个独立的普通窗口 ——
// 托盘菜单里塞不下可视化取景框。
namespace settings {

void Register(HINSTANCE inst);   // 注册窗口类，进程启动时调一次
void Show();                     // 打开；已经开着就前置
void Close();
void Refresh();                  // 外部（托盘菜单）改了设置，让窗口跟着变
bool IsOpen();
// 当前设置窗的句柄，没开就是 nullptr。给错误框当属主用 ——
// MessageBoxW 传 nullptr 的话弹出来会被设置窗盖住，用户完全看不见（2026-08-27 实测）。
HWND Wnd();

// g.darkMode / g.english 被外部改过之后重新上色和换文案
void ReloadTheme();

// 切到第 n 个分类（0=内容 1=取景 2=画面 3=通用）。验证工具用，用户是点左栏。
void GoToPage(int page);

// 主消息循环转交：Tab / 方向键 / 空格由对话框逻辑处理掉就返回 true
bool HandleDialogMessage(MSG* msg);

} // namespace settings
