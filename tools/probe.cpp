// 验收探针 —— 在**交互会话**里跑，把 StarPaper 拉起来、抓状态、截一张桌面图。
//
// ⭐ 为什么不是 PowerShell 脚本：
//    1. SSH 进来的是非交互会话（session 0），那里的"桌面"是一块 1024x768 的假屏，
//       截不到真实画面，也看不到真实显示器。必须靠计划任务 (LogonType Interactive) 落到 session 1。
//    2. 落到 session 1 的 PowerShell 脚本里一旦出现内联 Add-Type + user32 P/Invoke，
//       Windows Defender 的 AMSI 直接判定 "malicious content" 拒绝执行
//       （2026-08-22 实测，报 ScriptContainedMaliciousContent）。
//    编译成一个普通 exe 两个问题一起没有了。
//
// 用法：probe.exe <StarPaper.exe> <video> <输出目录>
//   → <输出目录>\result.txt  文本状态
//   → <输出目录>\shot.jpg    整个虚拟屏幕的截图

#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <cstdio>
#include <psapi.h>
#include <cmath>

#pragma comment(lib, "gdiplus.lib")

namespace {

FILE* g_out = nullptr;

void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_out) { fputs(buf, g_out); fputs("\r\n", g_out); fflush(g_out); }
    printf("%s\n", buf);
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring TitleOf(const wchar_t* cls) {
    HWND h = FindWindowW(cls, nullptr);
    if (!h) return L"<no window>";
    wchar_t buf[512] = {};
    GetWindowTextW(h, buf, 512);
    return buf;
}

// 桌面图标那一层。截图前藏起来，否则左侧的刻度线全被图标压住。
HWND FindDefView() {
    HWND pg = FindWindowW(L"Progman", nullptr);
    if (HWND d = FindWindowExW(pg, nullptr, L"SHELLDLL_DefView", nullptr)) return d;
    HWND w = nullptr;
    while ((w = FindWindowExW(nullptr, w, L"WorkerW", nullptr)) != nullptr) {
        if (HWND d = FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr)) return d;
    }
    return nullptr;
}

bool GetJpegClsid(CLSID* out) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<BYTE> raw(size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(raw.data());
    Gdiplus::GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, L"image/jpeg") == 0) { *out = codecs[i].Clsid; return true; }
    }
    return false;
}

bool SaveScreenshot(const std::wstring& path) {
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HDC screen = GetDC(nullptr);
    HDC mem    = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    const bool ok = BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT) != 0;
    SelectObject(mem, old);

    bool saved = false;
    if (ok) {
        Gdiplus::Bitmap gb(bmp, nullptr);
        CLSID clsid;
        if (GetJpegClsid(&clsid)) {
            ULONG quality = 88;
            Gdiplus::EncoderParameters ep;
            ep.Count = 1;
            ep.Parameter[0].Guid = Gdiplus::EncoderQuality;
            ep.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
            ep.Parameter[0].NumberOfValues = 1;
            ep.Parameter[0].Value = &quality;
            saved = gb.Save(path.c_str(), &clsid, &ep) == Gdiplus::Ok;
        }
    }

    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    Log("   shot  : %s  %dx%d  %s", saved ? "ok" : "FAILED", w, h,
        Narrow(path.substr(path.find_last_of(L'\\') + 1)).c_str());
    return saved;
}

BOOL CALLBACK MonProc(HMONITOR mon, HDC, LPRECT, LPARAM) {
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;
    UINT dx = 96, dy = 96;
    using GetDpiForMonitorFn = HRESULT (WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (HMODULE s = LoadLibraryW(L"shcore.dll")) {
        if (auto f = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(s, "GetDpiForMonitor")))
            f(mon, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy);
        FreeLibrary(s);
    }
    Log("monitor        : %s  rcMonitor=%d,%d..%d,%d  rcWork=%d,%d..%d,%d  dpi=%u (%.0f%%)%s",
        Narrow(mi.szDevice).c_str(),
        (int)mi.rcMonitor.left, (int)mi.rcMonitor.top, (int)mi.rcMonitor.right, (int)mi.rcMonitor.bottom,
        (int)mi.rcWork.left, (int)mi.rcWork.top, (int)mi.rcWork.right, (int)mi.rcWork.bottom,
        dx, dx * 100.0 / 96.0,
        (mi.dwFlags & MONITORINFOF_PRIMARY) ? "  [primary]" : "");
    return TRUE;
}

void KillStarPaper() {
    // 用窗口发 WM_CLOSE，比 TerminateProcess 干净（托盘图标会被摘掉）
    while (HWND h = FindWindowW(L"StarPaperTray", nullptr)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        PostMessageW(h, WM_CLOSE, 0, 0);
        if (HANDLE p = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid)) {
            if (WaitForSingleObject(p, 3000) != WAIT_OBJECT_0) TerminateProcess(p, 0);
            CloseHandle(p);
        }
        Sleep(300);
        if (FindWindowW(L"StarPaperTray", nullptr) == h) break;   // 关不掉就别死循环
    }
}

} // namespace

// 托盘菜单的命令 ID（与 main.cpp 里的枚举一一对应）。
// 直接 PostMessage 过去，等于替用户点了那一项 —— 比模拟鼠标点菜单可靠得多。
enum : UINT {
    kMenuToggle = 1000, kMenuPick, kMenuMute, kMenuPauseCovered, kMenuFill,
    kMenuZoomIn, kMenuZoomOut, kMenuFocusReset, kMenuAutostart, kMenuExit,
    kMenuFocus = 1100,   // + row*3 + col
    kCmdReloadFx = 1200, // 让 StarPaper 重读注册表里的 Fx*（不在它的菜单里）
};

void Command(UINT id, int times = 1) {
    HWND tray = FindWindowW(L"StarPaperTray", nullptr);
    if (!tray) return;
    for (int i = 0; i < times; ++i) {
        PostMessageW(tray, WM_COMMAND, MAKEWPARAM(id, 0), 0);
        Sleep(120);
    }
}

int ScenarioMain(const std::wstring& dir) {
    const std::wstring resultPath = dir + L"\\scenarios.txt";
    DeleteFileW(resultPath.c_str());
    _wfopen_s(&g_out, resultPath.c_str(), L"wb");

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdiToken = 0;
    Gdiplus::GdiplusStartup(&gdiToken, &gsi, nullptr);

    if (!FindWindowW(L"StarPaperTray", nullptr)) {
        Log("ERROR: StarPaper 没在跑，先用 run 模式把它拉起来");
        if (g_out) fclose(g_out);
        return 1;
    }

    // 桌面图标整个序列期间都藏着，最后统一恢复 —— 每张截图都切换会抖
    HWND dv = FindDefView();
    if (dv) { ShowWindow(dv, SW_HIDE); Sleep(600); }

    struct Step { const wchar_t* tag; UINT cmd; int times; const char* what; };
    const Step steps[] = {
        { L"a-default",   0,               0, "默认：填满 + 居中 + 100%" },
        { L"b-topleft",   kMenuFocus + 0,  1, "取景改左上角" },
        { L"c-botright",  kMenuFocus + 8,  1, "取景改右下角" },
        { L"d-zoom140",   kMenuZoomIn,     4, "居中后放大到 140%" },
        { L"e-fit",       kMenuFill,       1, "关掉填满：完整显示 + 黑边" },
        { L"f-paused",    kMenuToggle,     1, "暂停（画面必须留在屏幕上，不能变黑）" },
        { L"g-resumed",   kMenuToggle,     1, "恢复播放" },
    };

    for (const auto& st : steps) {
        if (wcscmp(st.tag, L"d-zoom140") == 0) Command(kMenuFocusReset);   // 先回正中再放大
        if (st.cmd) Command(st.cmd, st.times);
        Sleep(1500);
        Log("[%ls] %s", st.tag, st.what);
        Log("   state : %s", Narrow(TitleOf(L"StarPaperTray")).c_str());
        SaveScreenshot(dir + L"\\shot-" + st.tag + L".jpg");
    }

    // 收尾：恢复成默认那一套，别给用户留一个奇怪的状态
    Command(kMenuFill);          // 回到填满
    Command(kMenuFocusReset);
    Sleep(800);
    Log("restored : %s", Narrow(TitleOf(L"StarPaperTray")).c_str());

    if (dv) ShowWindow(dv, SW_SHOW);
    Gdiplus::GdiplusShutdown(gdiToken);
    if (g_out) fclose(g_out);
    return 0;
}

// 调色验收。
//
// ⭐ 不重启 StarPaper：写完注册表发一条 kCmdReloadFx（1200）让它自己重读。
//    重启一轮 3 秒，十几个场景就是一分钟，还每次都要等 Media Engine 重新起播。
//
// 每个场景都先把 Fx* 全清掉再写自己那几个键 —— 否则上一个场景的参数会串进来，
// 而"串进来"恰恰是最难从截图上看出来的那种错。
namespace {

const wchar_t* kFxKeys[] = {
    L"FxExposure", L"FxBrightness", L"FxContrast", L"FxHighlights", L"FxShadows",
    L"FxGamma", L"FxSaturation", L"FxVibrance", L"FxTemp", L"FxTint",
    L"FxBlur", L"FxSharpen", L"FxVignette", L"FxVigRadius", L"FxDim",
};

void FxClearAll() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\StarPaper", 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS)
        return;
    for (const wchar_t* n : kFxKeys) RegDeleteValueW(k, n);
    RegCloseKey(k);
}

void FxWrite(const wchar_t* name, double value) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\StarPaper", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    wchar_t buf[32];
    swprintf(buf, 32, L"%d", static_cast<int>(value * 1000 + (value < 0 ? -0.5 : 0.5)));
    RegSetValueExW(k, name, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(buf),
                   static_cast<DWORD>((wcslen(buf) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}

struct FxSet { const wchar_t* key; double value; };
struct FxCase { const wchar_t* tag; const char* what; FxSet sets[4]; };

} // namespace

// 只截某个窗口那块矩形。设置窗口才 900x646 逻辑像素，
// 截整块 3024x1890 的屏再让人去里面找，图大又看不清。
bool SaveRectShot(RECT r, const std::wstring& path) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w < 8 || h < 8) { Log("   shot  : FAILED 窗口矩形不合法"); return false; }

    HDC screen = GetDC(nullptr);
    HDC mem    = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    const bool ok = BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT) != 0;
    SelectObject(mem, old);

    bool saved = false;
    if (ok) {
        Gdiplus::Bitmap gb(bmp, nullptr);
        CLSID clsid;
        if (GetJpegClsid(&clsid)) {
            ULONG quality = 92;
            Gdiplus::EncoderParameters ep;
            ep.Count = 1;
            ep.Parameter[0].Guid = Gdiplus::EncoderQuality;
            ep.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
            ep.Parameter[0].NumberOfValues = 1;
            ep.Parameter[0].Value = &quality;
            saved = gb.Save(path.c_str(), &clsid, &ep) == Gdiplus::Ok;
        }
    }
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    Log("   shot  : %s  %dx%d  %s", saved ? "ok" : "FAILED", w, h,
        Narrow(path.substr(path.find_last_of(L'\\') + 1)).c_str());
    return saved;
}

namespace {
void RegStr(const wchar_t* name, const wchar_t* val) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\StarPaper", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(val),
                   static_cast<DWORD>((wcslen(val) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}
} // namespace

// 自动轮播 / 日程的**行为**验收。
//
// ⭐ 界面画对了不等于会换片。这一条不看界面，只盯注册表里的 `Video` ——
//    LoadVideo 每次换片都会写它，所以它变几次就是真的换了几次。
namespace {

std::wstring RegRead(const wchar_t* name) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\StarPaper", 0, KEY_READ, &k) != ERROR_SUCCESS)
        return L"";
    wchar_t buf[1024] = {};
    DWORD cb = sizeof(buf), type = 0;
    const LSTATUS st = RegQueryValueExW(k, name, nullptr, &type,
                                        reinterpret_cast<BYTE*>(buf), &cb);
    RegCloseKey(k);
    return (st == ERROR_SUCCESS && type == REG_SZ) ? std::wstring(buf) : std::wstring();
}

std::wstring Base(const std::wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

} // namespace

int PlayMain(const std::wstring& dir) {
    const std::wstring resultPath = dir + L"\\play.txt";
    DeleteFileW(resultPath.c_str());
    _wfopen_s(&g_out, resultPath.c_str(), L"wb");

    HWND tray = FindWindowW(L"StarPaperTray", nullptr);
    if (!tray) { Log("ERROR: StarPaper 没在跑"); if (g_out) fclose(g_out); return 1; }

    int fails = 0;

    // ---- 1) 自动轮播：3 秒一个的测试片，播完就换 ----
    Log("=== 自动轮播（切换时机 = 播完一遍）===");
    RegStr(L"ScheduleOn",      L"0");
    RegStr(L"PlaylistAuto",    L"1");
    RegStr(L"PlaylistAdvance", L"0");
    RegStr(L"PlaylistShuffle", L"0");
    PostMessageW(tray, WM_COMMAND, MAKEWPARAM(1202 /* kCmdReloadPlay */, 0), 0);
    Sleep(1200);

    std::wstring last = RegRead(L"Video");
    Log("起点 : %s", Narrow(Base(last)).c_str());
    int switches = 0;
    for (int i = 0; i < 40; ++i) {         // 20 秒，3 秒的片子够换五六次
        Sleep(500);
        const std::wstring now = RegRead(L"Video");
        if (!now.empty() && now != last) {
            ++switches;
            Log("  换片 %d : %s", switches, Narrow(Base(now)).c_str());
            last = now;
        }
    }
    if (switches >= 3) Log("PASS 20 秒内换了 %d 次（3 秒一个的片子，预期 5 次上下）", switches);
    else { Log("**FAIL** 20 秒内只换了 %d 次", switches); ++fails; }

    // ---- 2) 日程：把「白天」的时间窗口套在此刻 ----
    Log("");
    Log("=== 日程 ===");
    SYSTEMTIME st;
    GetLocalTime(&st);
    const int nowMin = st.wHour * 60 + st.wMinute;
    auto wrap = [](int m) { m %= 1440; return m < 0 ? m + 1440 : m; };

    wchar_t b[32];
    RegStr(L"PlaylistAuto", L"0");
    RegStr(L"ScheduleOn",   L"1");
    swprintf(b, 32, L"%d", wrap(nowMin - 5));  RegStr(L"DayStart",   b);
    swprintf(b, 32, L"%d", wrap(nowMin + 60)); RegStr(L"NightStart", b);
    PostMessageW(tray, WM_COMMAND, MAKEWPARAM(1202, 0), 0);
    Sleep(3000);
    std::wstring cur = RegRead(L"Video");
    const std::wstring day   = RegRead(L"DayVideo");
    const std::wstring night = RegRead(L"NightVideo");
    Log("此刻 %02d:%02d，白天窗口 = [%02d:%02d, %02d:%02d)",
        st.wHour, st.wMinute, wrap(nowMin - 5) / 60, wrap(nowMin - 5) % 60,
        wrap(nowMin + 60) / 60, wrap(nowMin + 60) % 60);
    if (cur == day) Log("PASS 切到了白天那条：%s", Narrow(Base(cur)).c_str());
    else { Log("**FAIL** 期望 %s，实际 %s", Narrow(Base(day)).c_str(), Narrow(Base(cur)).c_str()); ++fails; }

    // 反过来：把此刻挪进夜间窗口
    swprintf(b, 32, L"%d", wrap(nowMin + 60)); RegStr(L"DayStart",   b);
    swprintf(b, 32, L"%d", wrap(nowMin - 5));  RegStr(L"NightStart", b);
    PostMessageW(tray, WM_COMMAND, MAKEWPARAM(1202, 0), 0);
    Sleep(3000);
    cur = RegRead(L"Video");
    if (cur == night) Log("PASS 切到了夜间那条：%s", Narrow(Base(cur)).c_str());
    else { Log("**FAIL** 期望 %s，实际 %s", Narrow(Base(night)).c_str(), Narrow(Base(cur)).c_str()); ++fails; }

    // ---- 3) 日程开着时应该盖过轮播 ----
    Log("");
    Log("=== 日程盖过轮播 ===");
    RegStr(L"PlaylistAuto", L"1");
    PostMessageW(tray, WM_COMMAND, MAKEWPARAM(1202, 0), 0);
    Sleep(1000);
    const std::wstring before = RegRead(L"Video");
    Sleep(12000);      // 轮播若生效，3 秒的片子这会儿早换过好几轮了
    const std::wstring after = RegRead(L"Video");
    if (before == after) Log("PASS 12 秒没动过：%s", Narrow(Base(after)).c_str());
    else { Log("**FAIL** 日程开着却被轮播换走了：%s → %s",
               Narrow(Base(before)).c_str(), Narrow(Base(after)).c_str()); ++fails; }

    // 收尾：都关掉，别给用户留个会自己换片的状态
    RegStr(L"ScheduleOn",   L"0");
    RegStr(L"PlaylistAuto", L"0");
    PostMessageW(tray, WM_COMMAND, MAKEWPARAM(1202, 0), 0);

    Log("");
    Log(fails == 0 ? "全部通过" : "有 %d 项没过", fails);
    if (g_out) fclose(g_out);
    return fails == 0 ? 0 : 1;
}

// 视频库验收：预置一批路径 → 打开播放页 → 截图 → **点一张缩略图** → 确认真的切过去了。
//
// 「点一下就切换」是这一版的核心诉求，所以这里不满足于「界面画出来了」，
// 一定要走完一次真实点击，并从状态串里读出当前视频变了。
int LibMain(const std::wstring& dir) {
    const std::wstring resultPath = dir + L"\\lib.txt";
    DeleteFileW(resultPath.c_str());
    _wfopen_s(&g_out, resultPath.c_str(), L"wb");

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdiToken = 0;
    Gdiplus::GdiplusStartup(&gdiToken, &gsi, nullptr);

    HWND tray = FindWindowW(L"StarPaperTray", nullptr);
    if (!tray) { Log("ERROR: StarPaper 没在跑"); if (g_out) fclose(g_out); return 1; }

    PostMessageW(tray, WM_COMMAND, MAKEWPARAM(999, 0), 0);
    Sleep(2500);
    HWND win = FindWindowW(L"StarPaperSettings", nullptr);
    if (!win) { Log("ERROR: 设置窗口没开出来"); if (g_out) fclose(g_out); return 1; }
    SetWindowPos(win, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(win);
    Sleep(500);

    PostMessageW(tray, WM_COMMAND, MAKEWPARAM(1210 + 3 /* 播放页 */, 0), 0);
    Sleep(1500);   // 缩略图是后台线程生成的，给它一点时间

    RECT wr{};
    GetWindowRect(win, &wr);
    Log("[lib-grid] 视频库，五张缩略图");
    Log("   state : %s", Narrow(TitleOf(L"StarPaperTray")).c_str());
    SaveRectShot(wr, dir + L"\\lib-1-grid.jpg");

    // 再等一轮：首次生成缩略图要真去解码，1.5 秒未必够
    Sleep(3000);
    SaveRectShot(wr, dir + L"\\lib-2-thumbs.jpg");

    HWND grid = FindWindowExW(win, nullptr, L"StarPaperLib", nullptr);
    if (!grid) { Log("ERROR: 找不到库网格控件"); }
    else {
        UINT dpi = 96;
        using GetDpiFn = UINT (WINAPI*)(HWND);
        if (HMODULE u = GetModuleHandleW(L"user32.dll"))
            if (auto f = reinterpret_cast<GetDpiFn>(GetProcAddress(u, "GetDpiForWindow"))) dpi = f(win);
        auto S = [&](int px) { return MulDiv(px, static_cast<int>(dpi), 96); };

        // 第三张卡片的中心（卡片 156 宽、12 间距，和 settings.cpp 里的常量对齐）
        const int cx = 2 * (S(156) + S(12)) + S(78);
        const int cy = S(44);
        Log("[lib-click] 点第 3 张缩略图 @ %d,%d (dpi=%u)", cx, cy, dpi);
        Log("   before : %s", Narrow(TitleOf(L"StarPaperTray")).c_str());

        PostMessageW(grid, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(cx, cy));
        Sleep(2500);
        Log("   after  : %s", Narrow(TitleOf(L"StarPaperTray")).c_str());
        SaveRectShot(wr, dir + L"\\lib-3-clicked.jpg");

        // 桌面上真的换了没有 —— 只看设置窗口不算数
        Sleep(500);
        HWND dv = FindDefView();
        if (dv) { ShowWindow(dv, SW_HIDE); Sleep(400); }
        ShowWindow(win, SW_HIDE);
        Sleep(700);
        SaveScreenshot(dir + L"\\lib-4-desktop.jpg");
        ShowWindow(win, SW_SHOW);
        if (dv) ShowWindow(dv, SW_SHOW);
    }

    // 日程页和声音页也各来一张
    const struct { int page; const wchar_t* tag; } more[] = {
        { 4, L"lib-5-schedule" }, { 5, L"lib-6-audio" }, { 6, L"lib-7-power" },
    };
    for (const auto& m : more) {
        PostMessageW(tray, WM_COMMAND, MAKEWPARAM(1210 + m.page, 0), 0);
        Sleep(900);
        GetWindowRect(win, &wr);
        Log("[%ls]", m.tag);
        SaveRectShot(wr, dir + L"\\" + m.tag + L".jpg");
    }

    PostMessageW(tray, WM_COMMAND, MAKEWPARAM(1210 + 3, 0), 0);
    Gdiplus::GdiplusShutdown(gdiToken);
    if (g_out) fclose(g_out);
    return 0;
}

// 设置窗口的外观验收：主题 × 语言 × 分类，每种组合一张窗口截图。
int UiMain(const std::wstring& dir) {
    const std::wstring resultPath = dir + L"\\ui.txt";
    DeleteFileW(resultPath.c_str());
    _wfopen_s(&g_out, resultPath.c_str(), L"wb");

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdiToken = 0;
    Gdiplus::GdiplusStartup(&gdiToken, &gsi, nullptr);

    HWND tray = FindWindowW(L"StarPaperTray", nullptr);
    if (!tray) { Log("ERROR: StarPaper 没在跑"); if (g_out) fclose(g_out); return 1; }

    PostMessageW(tray, WM_COMMAND, MAKEWPARAM(999 /* kMenuSettings */, 0), 0);
    Sleep(2500);
    HWND win = FindWindowW(L"StarPaperSettings", nullptr);
    if (!win) { Log("ERROR: 设置窗口没开出来"); if (g_out) fclose(g_out); return 1; }

    SetWindowPos(win, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(win);
    Sleep(600);

    // ⚠️ page 是**下标**，加了页就得跟着改。2026-08-23 从 4 页扩到 8 页时
    //    这里没改，"通用"那两条截出来的其实是"播放"页。
    //    页序：0 内容 1 取景 2 画面 3 播放 4 日程 5 声音 6 电源 7 通用
    struct Case { const wchar_t* tag; const wchar_t* dark; const wchar_t* lang; int page; const char* what; };
    const Case cases[] = {
        { L"dark-zh-content", L"1", L"zh", 0, "深色 中文 · 内容（文件名 + 完整路径）" },
        { L"dark-zh-crop",    L"1", L"zh", 1, "深色 中文 · 取景（预览 + 取景框）" },
        { L"dark-zh-image",   L"1", L"zh", 2, "深色 中文 · 画面（15 个滑杆）" },
        { L"dark-zh-play",    L"1", L"zh", 3, "深色 中文 · 播放（视频库）" },
        { L"dark-zh-general", L"1", L"zh", 7, "深色 中文 · 通用（主题 / 语言）" },
        { L"light-zh-image",  L"0", L"zh", 2, "浅色 中文 · 画面 —— 浅色下滑杆要看得清" },
        { L"light-zh-play",   L"0", L"zh", 3, "浅色 中文 · 播放 —— 浅色下缩略图卡片" },
        { L"light-zh-content",L"0", L"zh", 0, "浅色 中文 · 内容" },
        { L"dark-en-content", L"1", L"en", 0, "深色 English · 内容" },
        { L"dark-en-play",    L"1", L"en", 3, "深色 English · 播放" },
        { L"dark-en-image",   L"1", L"en", 2, "深色 English · 画面 —— 英文长词不能截断" },
        { L"dark-en-general", L"1", L"en", 7, "深色 English · 通用" },
    };

    for (const auto& c : cases) {
        RegStr(L"DarkMode", c.dark);
        RegStr(L"Lang",     c.lang);
        PostMessageW(tray, WM_COMMAND, MAKEWPARAM(1201 /* kCmdReloadUi */, 0), 0);
        Sleep(400);
        PostMessageW(tray, WM_COMMAND, MAKEWPARAM(1210 + c.page /* kCmdGoPage */, 0), 0);
        Sleep(1100);   // 取景页要等一次 poster 刷新

        RECT wr{};
        GetWindowRect(win, &wr);
        Log("[%ls] %s", c.tag, c.what);
        Log("   rect  : %dx%d", (int)(wr.right - wr.left), (int)(wr.bottom - wr.top));
        SaveRectShot(wr, dir + L"\\ui-" + c.tag + L".jpg");
    }

    // 收尾：回到深色中文的内容页
    RegStr(L"DarkMode", L"1");
    RegStr(L"Lang", L"zh");
    PostMessageW(tray, WM_COMMAND, MAKEWPARAM(1201, 0), 0);
    Sleep(300);
    PostMessageW(tray, WM_COMMAND, MAKEWPARAM(1210, 0), 0);

    Gdiplus::GdiplusShutdown(gdiToken);
    if (g_out) fclose(g_out);
    return 0;
}

int FxMain(const std::wstring& dir) {
    const std::wstring resultPath = dir + L"\\fx.txt";
    DeleteFileW(resultPath.c_str());
    _wfopen_s(&g_out, resultPath.c_str(), L"wb");

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdiToken = 0;
    Gdiplus::GdiplusStartup(&gdiToken, &gsi, nullptr);

    if (!FindWindowW(L"StarPaperTray", nullptr)) {
        Log("ERROR: StarPaper 没在跑");
        if (g_out) fclose(g_out);
        return 1;
    }

    HWND dv = FindDefView();
    if (dv) { ShowWindow(dv, SW_HIDE); Sleep(600); }

    const FxCase cases[] = {
        { L"00-off",       "全默认（基准图，后面每张都跟它比）", {} },
        { L"01-mono",      "饱和度 0：应该是纯黑白",            { { L"FxSaturation", 0.0 } } },
        { L"02-sat200",    "饱和度 2.0：颜色明显更艳",          { { L"FxSaturation", 2.0 } } },
        { L"03-dim60",     "压暗 60%：整体变暗但对比度还在",     { { L"FxDim", 0.6 } } },
        { L"04-bright",    "亮度 +0.3：整体抬亮（会发灰）",      { { L"FxBrightness", 0.3 } } },
        { L"05-contrast",  "对比度 1.8：亮的更亮暗的更暗",       { { L"FxContrast", 1.8 } } },
        { L"06-warm",      "色温 9500K：明显偏暖偏黄",           { { L"FxTemp", 9500.0 } } },
        { L"07-cool",      "色温 3000K：明显偏冷偏蓝",           { { L"FxTemp", 3000.0 } } },
        { L"08-tint",      "色调 +80：偏品红",                   { { L"FxTint", 80.0 } } },
        { L"09-expo",      "曝光 +1.5EV：大幅提亮",              { { L"FxExposure", 1.5 } } },
        { L"10-gamma",     "gamma 0.5：暗部大幅提亮",            { { L"FxGamma", 0.5 } } },
        { L"11-blur",      "模糊 40：画面明显糊掉",              { { L"FxBlur", 40.0 } } },
        { L"12-blur-max",  "模糊 60：最大半径，走降采样那条路",   { { L"FxBlur", 60.0 } } },
        { L"13-vignette",  "暗角 1.5：四角压暗、中间不动",        { { L"FxVignette", 1.5 }, { L"FxVigRadius", 1.0 } } },
        { L"14-sharpen",   "锐化 2.0：边缘变硬",                 { { L"FxSharpen", 2.0 } } },
        { L"15-highlight", "高光 0.3：只压亮部，暗部不动",        { { L"FxHighlights", 0.3 } } },
        { L"16-shadows",   "阴影 +0.8：只提暗部，亮部不动",       { { L"FxShadows", 0.8 } } },
        { L"17-vibrance",  "自然饱和 +1：灰的地方变艳",          { { L"FxVibrance", 1.0 } } },
        // 多项叠加：这一条查的是 pass 编排（色彩 → 锐化 → 模糊）会不会互相踩
        { L"18-combo",     "调色+锐化+模糊三趟一起跑",
          { { L"FxContrast", 1.4 }, { L"FxSaturation", 1.5 }, { L"FxSharpen", 1.0 }, { L"FxBlur", 12.0 } } },
        { L"19-restore",   "全清回默认（必须和 00 一模一样）", {} },
    };

    for (const auto& c : cases) {
        FxClearAll();
        for (const auto& st : c.sets) {
            if (!st.key) break;
            FxWrite(st.key, st.value);
        }
        Command(kCmdReloadFx);
        Sleep(900);
        Log("[%ls] %s", c.tag, c.what);
        Log("   state : %s", Narrow(TitleOf(L"StarPaperTray")).c_str());
        SaveScreenshot(dir + L"\\fx-" + c.tag + L".jpg");
    }

    FxClearAll();
    Command(kCmdReloadFx);
    Sleep(500);
    Log("restored : %s", Narrow(TitleOf(L"StarPaperTray")).c_str());

    if (dv) ShowWindow(dv, SW_SHOW);
    Gdiplus::GdiplusShutdown(gdiToken);
    if (g_out) fclose(g_out);
    return 0;
}

// 改分辨率 —— 走的是 ResizeBuffers 那条路，也是投屏 / RustDesk 改分辨率时的真实路径。
//
// ⚠️⚠️ 恢复只能用 ChangeDisplaySettingsExW(NULL, NULL, ...) 让系统读回注册表里的设置。
//      **绝不能"先 EnumDisplaySettings 读原值、再写回去"**：
//      2026-08-22 就是那样把屏幕卡在 1280x800 —— EnumDisplaySettingsW(NULL, ...) 那次
//      静默返回了一个全零的 DEVMODE，写回去等于把分辨率设成 0，rc=-2 且不可逆。
int ResolutionMain(const std::wstring& dir) {
    const std::wstring resultPath = dir + L"\\resolution.txt";
    DeleteFileW(resultPath.c_str());
    _wfopen_s(&g_out, resultPath.c_str(), L"wb");

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdiToken = 0;
    Gdiplus::GdiplusStartup(&gdiToken, &gsi, nullptr);

    DEVMODEW cur = {};
    cur.dmSize = sizeof(cur);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &cur) || cur.dmPelsWidth == 0) {
        Log("ERROR: 读不到当前显示模式，放弃（不敢在这种状态下改分辨率）");
        if (g_out) fclose(g_out);
        return 1;
    }
    Log("current mode   : %ux%u @%uHz %ubpp",
        cur.dmPelsWidth, cur.dmPelsHeight, cur.dmDisplayFrequency, cur.dmBitsPerPel);

    // 找一个**宽高比不同**的模式：比例变了才会重算裁剪，才验得出东西
    const double curAR = (double)cur.dmPelsWidth / cur.dmPelsHeight;
    DEVMODEW pick = {};
    bool found = false;
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); ++i) {
        if (dm.dmBitsPerPel != cur.dmBitsPerPel) continue;
        if (dm.dmPelsWidth < 1280) continue;
        const double ar = (double)dm.dmPelsWidth / dm.dmPelsHeight;
        if (fabs(ar - curAR) < 0.05) continue;               // 比例一样的没意义
        if (fabs(ar - 16.0/9.0) > 0.02) continue;            // 就要 16:9，正好等于测试图案的比例
        if (!found || dm.dmPelsWidth > pick.dmPelsWidth) { pick = dm; found = true; }
    }
    if (!found) {
        Log("ERROR: 没有可用的 16:9 模式，跳过这项测试");
        if (g_out) fclose(g_out);
        return 1;
    }
    Log("target mode    : %ux%u @%uHz  (16:9 —— 与测试图案同比例，预期 src 变成 0,0..1000,1000 不裁)",
        pick.dmPelsWidth, pick.dmPelsHeight, pick.dmDisplayFrequency);

    HWND dv = FindDefView();
    if (dv) { ShowWindow(dv, SW_HIDE); Sleep(500); }

    DEVMODEW want = pick;
    want.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    // ⚠️ flags 必须是 0（只改当前会话），**不能带 CDS_UPDATEREGISTRY**。
    //    带上它等于把测试用的分辨率写进注册表，随后 ChangeDisplaySettings(NULL,0)
    //    "恢复注册表设置"恢复出来的就是那个测试值，屏幕再也回不去
    //    （2026-08-22 实测踩到：切到 3840x2160 后两次恢复都仍是 3840x2160）。
    const LONG rc = ChangeDisplaySettingsExW(nullptr, &want, nullptr, 0, nullptr);
    Log("switch         : rc=%ld %s", rc, rc == DISP_CHANGE_SUCCESSFUL ? "(ok)" : "(FAILED)");

    if (rc == DISP_CHANGE_SUCCESSFUL) {
        Sleep(4000);   // 等 WM_DISPLAYCHANGE 传到、视图重建完
        DEVMODEW now = {};
        now.dmSize = sizeof(now);
        EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &now);
        Log("now            : %ux%u  virtual-screen %dx%d",
            now.dmPelsWidth, now.dmPelsHeight,
            GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
        Log("state          : %s", Narrow(TitleOf(L"StarPaperTray")).c_str());
        SaveScreenshot(dir + L"\\shot-res-switched.jpg");
    }

    // 恢复。只用 NULL 这一种方式，且做两次确认。
    const LONG back = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    Sleep(2500);
    DEVMODEW after = {};
    after.dmSize = sizeof(after);
    EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &after);
    Log("restore        : rc=%ld  now %ux%u  (was %ux%u)",
        back, after.dmPelsWidth, after.dmPelsHeight, cur.dmPelsWidth, cur.dmPelsHeight);
    if (after.dmPelsWidth != cur.dmPelsWidth || after.dmPelsHeight != cur.dmPelsHeight) {
        ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
        Sleep(2500);
        EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &after);
        Log("restore (2nd)  : now %ux%u", after.dmPelsWidth, after.dmPelsHeight);
    }

    Sleep(2500);
    Log("state after    : %s", Narrow(TitleOf(L"StarPaperTray")).c_str());
    SaveScreenshot(dir + L"\\shot-res-restored.jpg");

    if (dv) ShowWindow(dv, SW_SHOW);
    Gdiplus::GdiplusShutdown(gdiToken);
    if (g_out) fclose(g_out);
    return 0;
}

// 显式把分辨率设成指定值并写进注册表 —— 出事之后用来把屏幕救回来。
// 只在**调用者手里有确凿的原始值**时使用（比如日志里记着切换前是多少）。
int SetModeMain(int w, int h, int hz) {
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    dm.dmPelsWidth  = (DWORD)w;
    dm.dmPelsHeight = (DWORD)h;
    dm.dmBitsPerPel = 32;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    if (hz > 0) { dm.dmDisplayFrequency = (DWORD)hz; dm.dmFields |= DM_DISPLAYFREQUENCY; }
    const LONG rc = ChangeDisplaySettingsExW(nullptr, &dm, nullptr, CDS_UPDATEREGISTRY, nullptr);
    printf("setmode %dx%d@%d -> rc=%ld\n", w, h, hz, rc);
    Sleep(2000);
    DEVMODEW now = {};
    now.dmSize = sizeof(now);
    EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &now);
    printf("now %ux%u @%uHz\n", now.dmPelsWidth, now.dmPelsHeight, now.dmDisplayFrequency);
    return (now.dmPelsWidth == (DWORD)w && now.dmPelsHeight == (DWORD)h) ? 0 : 1;
}

// 只报告现状，什么都不改。SSH 那边看到的是 session 0 的假屏（1024x768），
// 想知道真实分辨率必须由交互会话里的进程写文件回来。
int InfoMain(const std::wstring& dir) {
    const std::wstring path = dir + L"\\info.txt";
    DeleteFileW(path.c_str());
    _wfopen_s(&g_out, path.c_str(), L"wb");
    DEVMODEW now = {};
    now.dmSize = sizeof(now);
    EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &now);
    Log("display mode   : %ux%u @%uHz %ubpp", now.dmPelsWidth, now.dmPelsHeight,
        now.dmDisplayFrequency, now.dmBitsPerPel);
    Log("virtual-screen : %dx%d", GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
    EnumDisplayMonitors(nullptr, nullptr, MonProc, 0);
    Log("starpaper      : %s", Narrow(TitleOf(L"StarPaperTray")).c_str());
    if (g_out) fclose(g_out);
    return 0;
}

// 连续多轮测量。单次 6 秒的数字噪声很大（同一状态量到过 6.2% 和 16.1%），
// 要下性能结论就得看多轮。不重启 StarPaper，量的是稳态。
int MeasureMain(const std::wstring& dir, int rounds, int secs) {
    const std::wstring path = dir + L"\\measure.txt";
    DeleteFileW(path.c_str());
    _wfopen_s(&g_out, path.c_str(), L"wb");

    HWND tray = FindWindowW(L"StarPaperTray", nullptr);
    if (!tray) { Log("ERROR: StarPaper 没在跑"); if (g_out) fclose(g_out); return 1; }
    DWORD pid = 0;
    GetWindowThreadProcessId(tray, &pid);
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) { Log("ERROR: 打不开进程 %lu", pid); if (g_out) fclose(g_out); return 1; }

    auto frames = [&]() -> unsigned {
        wchar_t buf[512] = {};
        GetWindowTextW(tray, buf, 512);
        if (const wchar_t* p = wcsstr(buf, L"fr=")) return (unsigned)_wtoi(p + 3);
        return 0;
    };
    auto cpu = [&]() -> double {
        FILETIME c, e, k, u;
        if (!GetProcessTimes(proc, &c, &e, &k, &u)) return 0.0;
        ULARGE_INTEGER K, U;
        K.LowPart = k.dwLowDateTime; K.HighPart = k.dwHighDateTime;
        U.LowPart = u.dwLowDateTime; U.HighPart = u.dwHighDateTime;
        return (K.QuadPart + U.QuadPart) / 1e7;
    };

    Log("pid=%lu  rounds=%d  %ds each", pid, rounds, secs);
    double fpsSum = 0, cpuSum = 0;
    for (int i = 0; i < rounds; ++i) {
        const unsigned f0 = frames(); const double c0 = cpu(); const DWORD t0 = GetTickCount();
        Sleep((DWORD)secs * 1000);
        const unsigned f1 = frames(); const double c1 = cpu(); const DWORD t1 = GetTickCount();
        const double el = (t1 - t0) / 1000.0;
        const double fps = (f1 - f0) / el, cp = (c1 - c0) / el * 100.0;
        fpsSum += fps; cpuSum += cp;
        PROCESS_MEMORY_COUNTERS pmc = {};
        double mb = 0;
        using GetMemFn = BOOL (WINAPI*)(HANDLE, PROCESS_MEMORY_COUNTERS*, DWORD);
        if (HMODULE ps = LoadLibraryW(L"psapi.dll")) {
            if (auto f = reinterpret_cast<GetMemFn>(GetProcAddress(ps, "GetProcessMemoryInfo")))
                if (f(proc, &pmc, sizeof(pmc))) mb = pmc.WorkingSetSize / 1048576.0;
            FreeLibrary(ps);
        }
        Log("round %d        : %.1f fps   cpu %.1f%% of one core   ws %.1f MB", i + 1, fps, cp, mb);
    }
    Log("average        : %.1f fps   cpu %.1f%% of one core", fpsSum / rounds, cpuSum / rounds);
    SYSTEM_INFO si2; GetSystemInfo(&si2);
    Log("               : = %.2f%% of the whole machine (%u cores)",
        cpuSum / rounds / si2.dwNumberOfProcessors, si2.dwNumberOfProcessors);
    CloseHandle(proc);
    if (g_out) fclose(g_out);
    return 0;
}

// 设置窗口：开出来、截图、模拟拖一次取景框、再截一次。
//
// 拖框走的是直接给预览子窗口发 WM_LBUTTONDOWN / WM_MOUSEMOVE / WM_LBUTTONUP，
// 不模拟真实鼠标 —— 计划任务跑起来时鼠标可能在任何地方，不能去动它。
int SettingsMain(const std::wstring& dir) {
    const std::wstring path = dir + L"\\settings.txt";
    DeleteFileW(path.c_str());
    _wfopen_s(&g_out, path.c_str(), L"wb");

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdiToken = 0;
    Gdiplus::GdiplusStartup(&gdiToken, &gsi, nullptr);

    HWND tray = FindWindowW(L"StarPaperTray", nullptr);
    if (!tray) { Log("ERROR: StarPaper 没在跑"); if (g_out) fclose(g_out); return 1; }

    Log("before         : %s", Narrow(TitleOf(L"StarPaperTray")).c_str());

    PostMessageW(tray, WM_COMMAND, MAKEWPARAM(999 /* kMenuSettings */, 0), 0);
    Sleep(2500);

    HWND win = FindWindowW(L"StarPaperSettings", nullptr);
    if (!win) { Log("ERROR: 设置窗口没开出来"); if (g_out) fclose(g_out); return 1; }
    RECT wr{};
    GetWindowRect(win, &wr);
    UINT dpi = 0;
    using GetDpiFn = UINT (WINAPI*)(HWND);
    if (HMODULE u = GetModuleHandleW(L"user32.dll"))
        if (auto f = reinterpret_cast<GetDpiFn>(GetProcAddress(u, "GetDpiForWindow"))) dpi = f(win);
    Log("settings wnd   : %d,%d..%d,%d  (%dx%d)  dpi=%u",
        (int)wr.left, (int)wr.top, (int)wr.right, (int)wr.bottom,
        (int)(wr.right - wr.left), (int)(wr.bottom - wr.top), dpi);

    // 前台锁定规则下 StarPaper 自己的 SetForegroundWindow 不一定抢得到，这里再顶一次
    SetWindowPos(win, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(win);
    Sleep(1200);   // 等第一次 poster 定时刷新
    SaveScreenshot(dir + L"\\shot-settings.jpg");

    HWND prev = FindWindowExW(win, nullptr, L"StarPaperPreview", nullptr);
    if (!prev) { Log("ERROR: 找不到预览控件"); }
    else {
        RECT pc{};
        GetClientRect(prev, &pc);
        Log("preview        : client %dx%d", (int)pc.right, (int)pc.bottom);

        // 从预览正中往左上拖 —— focus 应该同时变小
        const int cx = pc.right / 2, cy = pc.bottom / 2;
        const int steps = 8;
        const int dx = -pc.right / 3, dy = -pc.bottom / 3;
        PostMessageW(prev, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(cx, cy));
        Sleep(120);
        for (int i = 1; i <= steps; ++i) {
            PostMessageW(prev, WM_MOUSEMOVE, MK_LBUTTON,
                         MAKELPARAM(cx + dx * i / steps, cy + dy * i / steps));
            Sleep(90);
        }
        PostMessageW(prev, WM_LBUTTONUP, 0, MAKELPARAM(cx + dx, cy + dy));
        Sleep(900);
        Log("after drag     : %s", Narrow(TitleOf(L"StarPaperTray")).c_str());
        SaveScreenshot(dir + L"\\shot-settings-dragged.jpg");

        // 再拖回右下，确认两个方向都动
        PostMessageW(prev, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(cx, cy));
        Sleep(120);
        for (int i = 1; i <= steps; ++i) {
            PostMessageW(prev, WM_MOUSEMOVE, MK_LBUTTON,
                         MAKELPARAM(cx - dx * i / steps, cy - dy * i / steps));
            Sleep(90);
        }
        PostMessageW(prev, WM_LBUTTONUP, 0, MAKELPARAM(cx - dx, cy - dy));
        Sleep(900);
        Log("after drag 2   : %s", Narrow(TitleOf(L"StarPaperTray")).c_str());
        SaveScreenshot(dir + L"\\shot-settings-dragged2.jpg");
    }

    // 收尾：重置取景 + 关窗
    PostMessageW(win, WM_COMMAND, MAKEWPARAM(105 /* kIdReset */, BN_CLICKED),
                 (LPARAM)GetDlgItem(win, 105));
    Sleep(700);
    Log("after reset    : %s", Narrow(TitleOf(L"StarPaperTray")).c_str());
    PostMessageW(win, WM_CLOSE, 0, 0);
    Sleep(700);
    Log("closed         : %s", FindWindowW(L"StarPaperSettings", nullptr) ? "STILL OPEN" : "ok");

    Gdiplus::GdiplusShutdown(gdiToken);
    if (g_out) fclose(g_out);
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    // ⚠️ 必须声明：不然 BitBlt 截出来的是被系统缩放过的低分辨率图，
    //    150% 的屏幕上会拿到 1344x840 而不是 2016x1260，一切像素判断全废。
    using SetCtx = BOOL (WINAPI*)(HANDLE);
    if (HMODULE u = GetModuleHandleW(L"user32.dll")) {
        if (auto f = reinterpret_cast<SetCtx>(GetProcAddress(u, "SetProcessDpiAwarenessContext")))
            f(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));   // PER_MONITOR_AWARE_V2
    }

    // ⚠️ 把自己的控制台窗口藏掉。probe 是 console 程序，那个黑框会盖在桌面上，
    //    截图里就多一块跟被测对象无关的东西 —— 更糟的是它会压住设置窗口
    //    （前台锁定规则下 StarPaper 的 SetForegroundWindow 抢不过来）。
    if (HWND con = GetConsoleWindow()) ShowWindow(con, SW_HIDE);

    if (argc >= 3 && wcscmp(argv[1], L"scenarios") == 0)  return ScenarioMain(argv[2]);
    if (argc >= 3 && wcscmp(argv[1], L"resolution") == 0) return ResolutionMain(argv[2]);
    if (argc >= 3 && wcscmp(argv[1], L"info") == 0)       return InfoMain(argv[2]);
    if (argc >= 3 && wcscmp(argv[1], L"settings") == 0)   return SettingsMain(argv[2]);
    if (argc >= 3 && wcscmp(argv[1], L"fx") == 0)         return FxMain(argv[2]);
    if (argc >= 3 && wcscmp(argv[1], L"ui") == 0)         return UiMain(argv[2]);
    if (argc >= 3 && wcscmp(argv[1], L"lib") == 0)        return LibMain(argv[2]);
    if (argc >= 3 && wcscmp(argv[1], L"play") == 0)       return PlayMain(argv[2]);
    if (argc >= 3 && wcscmp(argv[1], L"measure") == 0)
        return MeasureMain(argv[2], argc >= 4 ? _wtoi(argv[3]) : 4, argc >= 5 ? _wtoi(argv[4]) : 6);
    if (argc >= 4 && wcscmp(argv[1], L"setmode") == 0)
        return SetModeMain(_wtoi(argv[2]), _wtoi(argv[3]), argc >= 5 ? _wtoi(argv[4]) : 0);

    if (argc < 4) {
        printf("usage: probe.exe <StarPaper.exe> <video> <outdir>\n");
        printf("       probe.exe scenarios <outdir>\n");
        printf("       probe.exe fx <outdir>          调色验收，20 个场景\n");
        return 2;
    }
    const std::wstring exe   = argv[1];
    const std::wstring video = argv[2];
    const std::wstring dir   = argv[3];

    const std::wstring resultPath = dir + L"\\result.txt";
    const std::wstring shotPath   = dir + L"\\shot.jpg";
    DeleteFileW(resultPath.c_str());
    DeleteFileW(shotPath.c_str());

    _wfopen_s(&g_out, resultPath.c_str(), L"wb");

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdiToken = 0;
    Gdiplus::GdiplusStartup(&gdiToken, &gsi, nullptr);

    Log("virtual-screen : %dx%d at %d,%d",
        GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN));
    EnumDisplayMonitors(nullptr, nullptr, MonProc, 0);

    KillStarPaper();
    Sleep(800);

    std::wstring cmd = L"\"" + exe + L"\" \"" + video + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        Log("launch         : FAILED (%lu)", GetLastError());
        if (g_out) fclose(g_out);
        return 1;
    }
    CloseHandle(pi.hThread);
    Log("launch         : pid=%lu", pi.dwProcessId);

    Sleep(6000);
    Log("state@6s       : %s", Narrow(TitleOf(L"StarPaperTray")).c_str());

    // 帧率与 CPU：都从进程自己身上量。
    // ⚠️ 别拿 CPU 占用反推「播没播」——同一状态实测能在 9%~18% 之间跳（2026-08-22）。
    //    帧计数是硬证据，CPU 只用来看代价。
    {
        auto frames = []() -> unsigned {
            HWND h = FindWindowW(L"StarPaperTray", nullptr);
            if (!h) return 0;
            wchar_t buf[512] = {};
            GetWindowTextW(h, buf, 512);
            if (const wchar_t* p = wcsstr(buf, L"fr=")) return (unsigned)_wtoi(p + 3);
            return 0;
        };
        auto cpuTime = [&]() -> double {
            FILETIME c, e, k, u;
            if (!GetProcessTimes(pi.hProcess, &c, &e, &k, &u)) return 0.0;
            ULARGE_INTEGER K, U;
            K.LowPart = k.dwLowDateTime; K.HighPart = k.dwHighDateTime;
            U.LowPart = u.dwLowDateTime; U.HighPart = u.dwHighDateTime;
            return (K.QuadPart + U.QuadPart) / 1e7;   // 100ns 单位 → 秒
        };

        const unsigned f0 = frames();
        const double   c0 = cpuTime();
        const DWORD    t0 = GetTickCount();
        Sleep(6000);
        const unsigned f1 = frames();
        const double   c1 = cpuTime();
        const DWORD    t1 = GetTickCount();

        const double secs = (t1 - t0) / 1000.0;
        SYSTEM_INFO si2;
        GetSystemInfo(&si2);
        Log("throughput     : %.1f fps over %.1fs  (frames %u -> %u)",
            (f1 - f0) / secs, secs, f0, f1);
        Log("cpu            : %.1f%% of one core  (%.2fs over %.1fs, %u cores)",
            (c1 - c0) / secs * 100.0, c1 - c0, secs, si2.dwNumberOfProcessors);

        PROCESS_MEMORY_COUNTERS pmc = {};
        using GetMemFn = BOOL (WINAPI*)(HANDLE, PROCESS_MEMORY_COUNTERS*, DWORD);
        if (HMODULE psapi = LoadLibraryW(L"psapi.dll")) {
            if (auto f = reinterpret_cast<GetMemFn>(GetProcAddress(psapi, "GetProcessMemoryInfo")))
                if (f(pi.hProcess, &pmc, sizeof(pmc)))
                    Log("memory         : working set %.1f MB", pmc.WorkingSetSize / 1048576.0);
            FreeLibrary(psapi);
        }
    }

    if (HWND wall = FindWindowW(L"StarPaperWallpaper", nullptr)) {
        RECT r{}, c{};
        GetWindowRect(wall, &r);
        GetClientRect(wall, &c);
        using GetDpiFn = UINT (WINAPI*)(HWND);
        UINT dpi = 0;
        if (HMODULE u = GetModuleHandleW(L"user32.dll"))
            if (auto f = reinterpret_cast<GetDpiFn>(GetProcAddress(u, "GetDpiForWindow"))) dpi = f(wall);
        wchar_t pcls[64] = {};
        GetClassNameW(GetParent(wall), pcls, 64);
        Log("wallpaper hwnd : rect=%d,%d..%d,%d  client=%dx%d  dpi=%u  parent=%s",
            (int)r.left, (int)r.top, (int)r.right, (int)r.bottom,
            (int)c.right, (int)c.bottom, dpi, Narrow(pcls).c_str());
    } else {
        Log("wallpaper hwnd : <not found>");
    }

    HWND dv = FindDefView();
    if (dv) { ShowWindow(dv, SW_HIDE); Sleep(700); }
    SaveScreenshot(shotPath);
    if (dv) ShowWindow(dv, SW_SHOW);    // 图标绝不能留在隐藏态

    Sleep(2000);
    Log("state@final       : %s", Narrow(TitleOf(L"StarPaperTray")).c_str());

    Gdiplus::GdiplusShutdown(gdiToken);
    if (g_out) fclose(g_out);
    CloseHandle(pi.hProcess);
    return 0;
}
