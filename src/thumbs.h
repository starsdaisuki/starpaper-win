#pragma once
#include <windows.h>
#include <string>

// 视频缩略图。
//
// ⭐ 不自己解码 —— 用 Shell 的 IShellItemImageFactory 直接要，
//    拿到的就是资源管理器里显示的那一张，系统还替我们缓存好了。
//    自己开 Media Foundation 去 seek 第一帧要多几百行，而且第一帧
//    往往是黑场或者版权页，反而不如系统挑的那张有代表性。
namespace thumbs {

void Init();
void Shutdown();

// 缓存里有就立刻返回；没有就返回 nullptr，同时排进后台队列，
// 生成好之后给 notify 窗口发 kReady，那时候再调一次就有了。
//
// ⚠️ 返回的 HBITMAP 归本模块所有，**调用方不要 DeleteObject**。
HBITMAP Get(const std::wstring& path, int w, int h, HWND notify);

// 某个文件的缓存作废（从库里移除时用）
void Forget(const std::wstring& path);
void Clear();

constexpr UINT kReady = WM_APP + 40;   // wParam/lParam 都不用，收到就重画

} // namespace thumbs
