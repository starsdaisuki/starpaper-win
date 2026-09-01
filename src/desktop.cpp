#include "desktop.h"

// Win10 1803+ 的 Progman 带这个扩展样式时，桌面是"raised desktop"结构，
// WorkerW 变成 Progman 的**子窗口**，不能再用枚举兄弟窗口那套找。
#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

namespace {

struct EnumCtx {
    HWND worker = nullptr;
};

// 经典结构下的找法：
//
//   0x00010190 "" WorkerW
//     0x000100EE "" SHELLDLL_DefView        ← 桌面图标住在这里
//       0x000100F0 "FolderView" SysListView32
//   0x00100B8A "" WorkerW                   ← 我们要的就是这个（上一个的下一个兄弟）
//   0x000100EC "Program Manager" Progman
//
// 所以：枚举顶层窗口，找到"有 SHELLDLL_DefView 这个子窗口"的那个，
// 再取它后面那个同类兄弟。
BOOL CALLBACK EnumProc(HWND top, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumCtx*>(lp);
    if (FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr)) {
        HWND next = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
        if (next) {
            ctx->worker = next;
            return FALSE;   // 找到了就停
        }
    }
    return TRUE;
}

} // namespace

namespace desktop {

RECT VirtualScreenRect() {
    RECT r;
    r.left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    r.top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r.right  = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.bottom = r.top  + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return r;
}

HWND FindWallpaperHost() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return nullptr;

    // 0x052C 是没有文档的 Progman 私有消息，作用是"在桌面图标背后生成一个 WorkerW"。
    // 已经有了就什么都不做，所以重复发是安全的。
    // 用 SendMessageTimeout 而不是 SendMessage：Explorer 卡住时不能把自己也拖死。
    DWORD_PTR unused = 0;
    SendMessageTimeoutW(progman, 0x052C, (WPARAM)0xD, (LPARAM)0x1,
                        SMTO_NORMAL, 1000, &unused);

    // 先判断桌面是哪种结构，两种找法完全不同。
    const LONG_PTR ex = GetWindowLongPtrW(progman, GWL_EXSTYLE);
    if (ex & WS_EX_NOREDIRECTIONBITMAP) {
        // raised desktop：WorkerW 是 Progman 的子窗口
        HWND w = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
        if (w) return w;
    }

    EnumCtx ctx;
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.worker) return ctx.worker;

    // 兜底：Win7 和某些第三方 shell 下没有独立 WorkerW，Progman 自己就是壁纸层。
    return progman;
}

bool AttachToDesktop(HWND hwnd, HWND host, const RECT& monitorRect) {
    if (!hwnd || !host) return false;
    if (!SetParent(hwnd, host)) return false;

    // SetParent 之后坐标变成相对父窗口客户区。
    // 宿主窗口的客户区原点 = 虚拟屏幕原点，所以把显示器矩形平移过去即可。
    const RECT v = VirtualScreenRect();
    SetWindowPos(hwnd, HWND_BOTTOM,
                 monitorRect.left - v.left,
                 monitorRect.top  - v.top,
                 monitorRect.right  - monitorRect.left,
                 monitorRect.bottom - monitorRect.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

} // namespace desktop
