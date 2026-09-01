// StarPaper for Windows —— 把一段本地视频挂成桌面壁纸。
//
// 设计目标只有一条：**不装任何运行时**。
// 整个程序是一个 Win32 exe：视频解码交给系统的 Media Foundation Media Engine，
// 呈现由我们自己用 D3D11 做（frame-server 模式，见 player.h），
// 桌面挂载用 Explorer 那个没文档的 WorkerW 机制。没有 .NET，没有 mpv，没有 Chromium。

#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <wtsapi32.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "app.h"
#include "desktop.h"
#include "player.h"
#include "settings.h"
#include "startup.h"
#include "theme.h"
#include "thumbs.h"

// ⚠️ 这里**没有**匿名命名空间：settings.cpp 要用到下面一批函数（app.h 里声明）。

constexpr wchar_t kAppName[]   = L"StarPaper";
constexpr wchar_t kWallClass[] = L"StarPaperWallpaper";
constexpr wchar_t kTrayClass[] = L"StarPaperTray";
constexpr wchar_t kRegPath[]   = L"Software\\StarPaper";

constexpr UINT WM_TRAY        = WM_APP + 1;
constexpr UINT WM_MEDIA_EVENT = WM_APP + 21;   // 与 player.cpp 里的 kMediaEvent 对应

enum : UINT {
    kMenuSettings = 999,
    kMenuToggle = 1000,
    kMenuPick,
    kMenuMute,
    kMenuPauseCovered,
    kMenuFill,
    kMenuZoomIn,
    kMenuZoomOut,
    kMenuFocusReset,
    kMenuAutostart,
    kMenuExit,

    // 九宫格取景位置，连号：kMenuFocus + row*3 + col
    kMenuFocus = 1100,

    // 不在菜单里，只给验证工具用：外部改完注册表里的 Fx*，发这条就热重载。
    // 有它才能不重启进程就切换调色场景 —— 重启一次要 3 秒，一轮验证十几个场景差别很大。
    kCmdReloadFx = 1200,
    // 同上，给验证工具切主题/语言用：注册表改完发这条，界面立刻重新上色
    kCmdReloadUi = 1201,
    // 重读库 / 轮播 / 日程 / 声音 / 电源那一组设置
    kCmdReloadPlay = 1202,
    // + page：切到设置窗口的第 n 个分类
    kCmdGoPage   = 1210,
};

void LoadEffects();   // 定义在下面，WndProc 要先用到

constexpr UINT_PTR kTimerCover = 1;

App g;

// 必须在创建任何窗口之前调用。
//
// ⚠️ 不声明的话进程处于「系统替你缩放」的状态：GetMonitorInfo 给的是物理像素，
//    但窗口和交换链会被系统再缩放一遍，150% 的屏幕上等于按 2/3 分辨率渲染再放大，
//    画面糊一圈。声明之后窗口尺寸、交换链尺寸、显示器尺寸三者都是同一套物理像素。
void EnableDpiAwareness() {
    using SetCtx = BOOL (WINAPI*)(HANDLE);
    if (HMODULE u = GetModuleHandleW(L"user32.dll")) {
        if (auto f = reinterpret_cast<SetCtx>(GetProcAddress(u, "SetProcessDpiAwarenessContext"))) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
            if (f(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) return;
        }
    }
    SetProcessDPIAware();   // Win8.1 及更早的兜底
}

// ---------------------------------------------------------------- 配置

std::wstring RegReadString(const wchar_t* name) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return L"";
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof(buf), type = 0;
    const LSTATUS st = RegQueryValueExW(key, name, nullptr, &type,
                                        reinterpret_cast<BYTE*>(buf), &size);
    RegCloseKey(key);
    return (st == ERROR_SUCCESS && type == REG_SZ) ? std::wstring(buf) : std::wstring();
}

void RegWriteString(const wchar_t* name, const std::wstring& value) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(key, name, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

// 开机启动分两套机制（打包 / 非打包），实现在 startup.cpp。
// ⚠️ 别在这里退回到直接写 Run 键 —— MSIX 包里那样写是静默失效的。
bool IsAutostart() {
    return startup::Query() == startup::State::On;
}

void SetAutostart(bool on) {
    startup::Set(on);
}

// 取景参数存成字符串。注册表这层只认 REG_SZ，浮点自己转，
// 省得为三个数再引一套序列化。
void SaveFraming() {
    wchar_t buf[64];
    wsprintfW(buf, L"%d", static_cast<int>(g.focusX * 100 + 0.5)); RegWriteString(L"FocusX", buf);
    wsprintfW(buf, L"%d", static_cast<int>(g.focusY * 100 + 0.5)); RegWriteString(L"FocusY", buf);
    wsprintfW(buf, L"%d", static_cast<int>(g.zoom   * 100 + 0.5)); RegWriteString(L"Zoom",   buf);
}

double LoadPercent(const wchar_t* name, double fallback, double lo, double hi) {
    const std::wstring v = RegReadString(name);
    if (v.empty()) return fallback;
    const double d = _wtoi(v.c_str()) / 100.0;
    if (d < lo || d > hi) return fallback;
    return d;
}

// 用户明确选过语言时以选择为准；首次启动只在系统 UI 主语言确实是中文时用简体中文，
// 其它所有语言一律英文。英文是安全兜底：中文用户看到英文仍能操作，非中文用户
// 若意外掉进中文界面则很难自救。
bool LoadEnglishPref() {
    const std::wstring v = RegReadString(L"Lang");
    if (v == L"zh") return false;
    if (v == L"en") return true;
    return PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_CHINESE;
}

// ---------------------------------------------------------------- 状态

// 把内部状态挂在托盘窗口标题上。
// 这个窗口从不显示，标题对用户不可见，但外部可以 GetWindowTextW 读到 ——
// 排查「到底暂停没暂停」时不用靠 CPU 反推，也不用写日志文件。
void UpdateTrayTip(const wchar_t* text) {
    if (!g.tray) return;
    static std::wstring last;
    if (last == text) return;         // 每秒无脑刷会让 tooltip 抖，变了才写
    last = text;

    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = g.tray;
    nid.uID    = 1;
    nid.uFlags = NIF_TIP;
    lstrcpynW(nid.szTip, text, ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void UpdateStateTitle() {
    if (!g.tray) return;
    int playing = 0, paused = 0;
    for (const auto& v : g.views) {
        if (!v->player.HasMedia()) continue;
        v->player.IsPaused() ? ++paused : ++playing;
    }
    const int screens = static_cast<int>(g.views.size());

    // fx  = 调色参数有没有偏离默认；fxok = shader 管线有没有真的建起来。
    // 两个都要有：只看 fx 分不出「调了但 d3dcompiler 挂了」和「调了且生效」。
    const int fxOn = g.fx.Active() ? 1 : 0;
    const int fxOk = (!g.views.empty() && g.views.front()->player.FxReady()) ? 1 : 0;

    wchar_t st[256];
    wsprintfW(st, L"StarPaper screens=%d playing=%d paused=%d usr=%d lock=%d opt=%d fill=%d fx=%d fxok=%d",
              screens, playing, paused,
              g.userPaused ? 1 : 0, g.locked ? 1 : 0, g.pauseCovered ? 1 : 0,
              g.fillMode ? 1 : 0, fxOn, fxOk);
    if (!g.views.empty()) {
        const View& v = *g.views.front();
        const Player::Layout& L = v.layout;
        const RECT& d = L.dst;
        // 源矩形是 0~1 的浮点，wsprintfW 不支持 %f，一律乘 1000 取整看千分比。
        // 帧计数是「画面真的在动」的唯一硬证据 —— CPU 占用反推不出来（实测同一状态 9%~18%）。
        wchar_t dbg[160];
        wsprintfW(dbg, L" src=%d,%d..%d,%d dst=%d,%d..%d,%d f=%d/%d z=%d fr=%u",
                  static_cast<int>(L.srcLeft   * 1000 + 0.5f),
                  static_cast<int>(L.srcTop    * 1000 + 0.5f),
                  static_cast<int>(L.srcRight  * 1000 + 0.5f),
                  static_cast<int>(L.srcBottom * 1000 + 0.5f),
                  d.left, d.top, d.right, d.bottom,
                  static_cast<int>(g.focusX * 100 + 0.5),
                  static_cast<int>(g.focusY * 100 + 0.5),
                  static_cast<int>(g.zoom * 100 + 0.5),
                  v.player.FrameCount());
        lstrcatW(st, dbg);
    }
    SetWindowTextW(g.tray, st);

    // 鼠标悬停托盘图标就能看到的人话版 —— 投屏时用来确认多出来的那块屏认到了没有
    wchar_t tip[128];
    const wchar_t* what = g.userPaused ? T(S_STATE_PAUSED)
                        : g.locked     ? T(S_TIP_LOCKED)
                        : (paused > 0 && playing == 0) ? T(S_STATE_COVERED)
                        : (paused > 0) ? T(S_TIP_PARTIAL)
                        : T(S_STATE_PLAYING);
    wsprintfW(tip, L"StarPaper · %s · %d %s", what, screens,
              T(screens == 1 ? S_SCREEN : S_SCREENS));
    UpdateTrayTip(tip);
}

// 电池 / 节电模式。两个都从同一次 GetSystemPowerStatus 里读：
//   ACLineStatus == 0        → 正在用电池
//   SystemStatusFlag  &  1   → 系统的「节电模式」开着（Win10 1703 起才有这个字段）
void ReadPowerState(bool& onBattery, bool& saver) {
    onBattery = saver = false;
    SYSTEM_POWER_STATUS ps = {};
    if (!GetSystemPowerStatus(&ps)) return;
    onBattery = (ps.ACLineStatus == 0);
    saver     = (ps.SystemStatusFlag & 1) != 0;
}

void ApplyPlaybackState() {
    bool onBattery = false, saver = false;
    if (g.pauseBattery || g.pauseSaver) ReadPowerState(onBattery, saver);
    const bool powerPause = (g.pauseBattery && onBattery) || (g.pauseSaver && saver);

    for (auto& v : g.views) {
        if (!v->player.HasMedia()) continue;
        const bool shouldPause = g.userPaused || v->coveredPause
                              || (g.locked && g.pauseLocked) || powerPause;
        if (shouldPause && !v->player.IsPaused())      v->player.Pause();
        else if (!shouldPause && v->player.IsPaused()) v->player.Play();
    }
    UpdateStateTitle();
}

// 前台窗口是否把这块屏的**工作区**整个盖住了。
//
// ⚠️ 比的是 rcWork 不是 rcMonitor：最大化窗口填的是工作区，任务栏那条它盖不到。
//    拿 rcMonitor 去比，最大化窗口永远差任务栏那几十像素，判据永远不成立
//    （2026-08-22 实测：notepad 最大化 rect 是 -7,-7..2024,1220，屏是 0,0..2016,1260）。
//    真全屏窗口覆盖 rcMonitor，自然也覆盖 rcWork，所以这样写两种都对。
bool ViewIsCovered(const View& v) {
    if (!g.pauseCovered) return false;

    HWND fg = GetForegroundWindow();
    if (!fg || fg == g.tray) return false;
    for (const auto& x : g.views) if (fg == x->hwnd) return false;
    if (!IsWindowVisible(fg) || IsIconic(fg)) return false;

    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW")) return false;

    RECT wr;
    if (!GetWindowRect(fg, &wr)) return false;

    RECT self = v.rect;
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (!GetMonitorInfoW(MonitorFromRect(&self, MONITOR_DEFAULTTONEAREST), &mi)) return false;

    const RECT& w = mi.rcWork;
    return wr.left <= w.left && wr.top <= w.top && wr.right >= w.right && wr.bottom >= w.bottom;
}

// ---------------------------------------------------------------- 显示器视图

// 取景框在**源画面**上的归一化矩形（object-fit: cover + 缩放 + 平移）。
//
// ⭐ 设置窗口画预览框用的是同一个函数 —— 所以「预览里看到的框」和「桌面上真实裁出来的」
//    不可能对不上。这正是当初不敢先做可视化选框的原因：底层不准的话，界面会跟着骗人。
void FramingRect(int videoW, int videoH, int screenW, int screenH,
                 double focusX, double focusY, double zoom,
                 double& left, double& top, double& right, double& bottom) {
    left = 0.0; top = 0.0; right = 1.0; bottom = 1.0;
    if (videoW <= 0 || videoH <= 0 || screenW <= 0 || screenH <= 0) return;
    if (zoom < 1.0) zoom = 1.0;

    const double screenAR = static_cast<double>(screenW) / screenH;
    const double videoAR  = static_cast<double>(videoW)  / videoH;

    double sw = videoW, sh = videoH;
    if (videoAR > screenAR) sw = videoH * screenAR;   // 视频更宽 → 左右各切掉一些
    else                    sh = videoW / screenAR;   // 视频更高 → 上下各切掉一些
    sw /= zoom;
    sh /= zoom;

    const double ox = (videoW - sw) * focusX;
    const double oy = (videoH - sh) * focusY;
    left   = ox / videoW;
    top    = oy / videoH;
    right  = (ox + sw) / videoW;
    bottom = (oy + sh) / videoH;
}

// ⭐ 画面怎么摆 —— 全部换算成两个矩形交给播放层：
//
//   src —— 源视频上的**归一化矩形**（0~1）。要裁掉哪一块，就把这个矩形收窄。
//   dst —— 交换链后台缓冲上的**像素矩形**，尺寸由我们自己创建，不经任何 DPI 换算。
//
// ⚠️ 别再回到「让目标矩形比窗口大、多出来的让窗口剪掉」那条路。
//    那要求 Media Engine 与我们对目标坐标系的理解一致，而 150% 缩放下它不一致：
//    传物理像素画面放大 1.5 倍、传逻辑像素缩成 2/3、传 NULL 仍放大还多一条黑边，
//    三种假设互相矛盾（2026-08-22，用四角带色块的测试图案才看出来，真实壁纸完全看不出）。
//    现在裁剪表达在归一化的源上，没有单位可以被误解，那个歧义从根上不存在了。
void ApplyScaleMode(View& v) {
    if (!v.hwnd) return;

    const RECT vs = desktop::VirtualScreenRect();
    const int mx = v.rect.left - vs.left;
    const int my = v.rect.top  - vs.top;
    const int mw = v.rect.right  - v.rect.left;
    const int mh = v.rect.bottom - v.rect.top;
    if (mw <= 0 || mh <= 0) return;

    // 窗口永远就是这块屏，一像素不多一像素不少
    SetWindowRgn(v.hwnd, nullptr, TRUE);
    SetWindowPos(v.hwnd, HWND_BOTTOM, mx, my, mw, mh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    v.player.SetSurfaceSize(mw, mh);

    Player::Layout L;
    L.dst = { 0, 0, mw, mh };

    int vw = 0, vh = 0;
    if (v.player.GetVideoSize(vw, vh) && vw > 0 && vh > 0) {
        if (g.fillMode) {
            double l, t, r, b;
            FramingRect(vw, vh, mw, mh, g.focusX, g.focusY, g.zoom, l, t, r, b);
            L.srcLeft   = static_cast<float>(l);
            L.srcTop    = static_cast<float>(t);
            L.srcRight  = static_cast<float>(r);
            L.srcBottom = static_cast<float>(b);
        } else {
            // 完整显示不裁剪：源取全图，目标按比例缩放后居中，四周留黑边
            const double s = (std::min)(static_cast<double>(mw) / vw,
                                        static_cast<double>(mh) / vh);
            const int dw = (std::max)(1, static_cast<int>(vw * s + 0.5));
            const int dh = (std::max)(1, static_cast<int>(vh * s + 0.5));
            const int ox = (mw - dw) / 2;
            const int oy = (mh - dh) / 2;
            L.dst = { ox, oy, ox + dw, oy + dh };
        }
    }

    v.layout = L;
    v.player.SetLayout(L);
    // 调色和音量跟着一起下发 —— 新接上的显示器、换视频后重建的 view 才不会漏掉
    v.player.SetEffects(g.fx);
    v.player.SetVolume(g.volume);
}

// 多屏时只让主屏那份有机会出声，否则同一段音频会叠 N 遍。
void ApplyMute() {
    bool first = true;
    for (auto& v : g.views) { v->player.SetMuted(g.muted || !first); first = false; }
}

void ApplyScaleModeAll() {
    for (auto& v : g.views) ApplyScaleMode(*v);
}

void ApplyEffectsAll() {
    for (auto& v : g.views) v->player.SetEffects(g.fx);
}

// 调色参数存注册表。全部按「值 ×1000 取整」存成字符串 ——
// 复用现成的 RegReadString/RegWriteString，不为几个浮点再引一套 REG_BINARY。
namespace {
void SaveFx(const wchar_t* name, double v) {
    wchar_t buf[32];
    const int n = static_cast<int>(v * 1000 + (v < 0 ? -0.5 : 0.5));
    wsprintfW(buf, L"%d", n);
    RegWriteString(name, buf);
}
float LoadFx(const wchar_t* name, double fallback, double lo, double hi) {
    const std::wstring v = RegReadString(name);
    if (v.empty()) return static_cast<float>(fallback);
    const double d = _wtoi(v.c_str()) / 1000.0;
    return static_cast<float>(d < lo ? lo : (d > hi ? hi : d));
}
} // namespace

void SaveEffects() {
    SaveFx(L"FxExposure",   g.fx.exposure);
    SaveFx(L"FxBrightness", g.fx.brightness);
    SaveFx(L"FxContrast",   g.fx.contrast);
    SaveFx(L"FxHighlights", g.fx.highlights);
    SaveFx(L"FxShadows",    g.fx.shadows);
    SaveFx(L"FxGamma",      g.fx.gamma);
    SaveFx(L"FxSaturation", g.fx.saturation);
    SaveFx(L"FxVibrance",   g.fx.vibrance);
    SaveFx(L"FxTemp",       g.fx.temperature);
    SaveFx(L"FxTint",       g.fx.tint);
    SaveFx(L"FxBlur",       g.fx.blur);
    SaveFx(L"FxSharpen",    g.fx.sharpen);
    SaveFx(L"FxVignette",   g.fx.vignette);
    SaveFx(L"FxVigRadius",  g.fx.vignetteRadius);
    SaveFx(L"FxDim",        g.fx.dim);
}

// —— 视频库 ——
//
// 存法：路径之间用 \n 隔开塞进一个 REG_SZ。路径里不可能有换行，
// 所以不需要转义；REG_SZ 上限约 16K 字符，够放几十条。
void SaveLibrary() {
    std::wstring joined;
    for (size_t i = 0; i < g.library.size(); ++i) {
        if (i) joined += L"\n";
        joined += g.library[i];
    }
    RegWriteString(L"Library", joined);
}

void LoadLibrary_() {
    g.library.clear();
    const std::wstring v = RegReadString(L"Library");
    size_t start = 0;
    while (start <= v.size()) {
        const size_t nl = v.find(L'\n', start);
        const std::wstring one = v.substr(start, nl == std::wstring::npos ? nl : nl - start);
        if (!one.empty()) g.library.push_back(one);
        if (nl == std::wstring::npos) break;
        start = nl + 1;
    }
}

void SavePlayback() {
    wchar_t b[32];
    RegWriteString(L"PlaylistAuto",    g.playlistAuto    ? L"1" : L"0");
    RegWriteString(L"PlaylistShuffle", g.playlistShuffle ? L"1" : L"0");
    wsprintfW(b, L"%d", g.playlistAdvance);  RegWriteString(L"PlaylistAdvance",  b);
    wsprintfW(b, L"%d", g.playlistInterval); RegWriteString(L"PlaylistInterval", b);

    RegWriteString(L"ScheduleOn", g.scheduleEnabled ? L"1" : L"0");
    RegWriteString(L"DayVideo",   g.dayVideo);
    RegWriteString(L"NightVideo", g.nightVideo);
    wsprintfW(b, L"%d", g.dayStartMin);   RegWriteString(L"DayStart",   b);
    wsprintfW(b, L"%d", g.nightStartMin); RegWriteString(L"NightStart", b);

    wsprintfW(b, L"%d", static_cast<int>(g.volume * 100 + 0.5));
    RegWriteString(L"Volume", b);

    RegWriteString(L"PauseLocked",  g.pauseLocked  ? L"1" : L"0");
    RegWriteString(L"PauseBattery", g.pauseBattery ? L"1" : L"0");
    RegWriteString(L"PauseSaver",   g.pauseSaver   ? L"1" : L"0");
}

void LoadPlayback() {
    auto num = [](const wchar_t* k, int fallback, int lo, int hi) {
        const std::wstring v = RegReadString(k);
        if (v.empty()) return fallback;
        const int n = _wtoi(v.c_str());
        return n < lo ? lo : (n > hi ? hi : n);
    };
    g.playlistAuto     = RegReadString(L"PlaylistAuto")    == L"1";
    g.playlistShuffle  = RegReadString(L"PlaylistShuffle") == L"1";
    g.playlistAdvance  = num(L"PlaylistAdvance",  0, 0, 1);
    g.playlistInterval = num(L"PlaylistInterval", 30, 1, 720);

    g.scheduleEnabled = RegReadString(L"ScheduleOn") == L"1";
    g.dayVideo        = RegReadString(L"DayVideo");
    g.nightVideo      = RegReadString(L"NightVideo");
    g.dayStartMin     = num(L"DayStart",   7 * 60,  0, 1439);
    g.nightStartMin   = num(L"NightStart", 19 * 60, 0, 1439);

    g.volume = num(L"Volume", 50, 0, 100) / 100.0;

    // 锁屏暂停默认开：锁着的时候没人看得见，白烧 CPU
    const std::wstring pl = RegReadString(L"PauseLocked");
    g.pauseLocked  = pl.empty() ? true : (pl == L"1");
    g.pauseBattery = RegReadString(L"PauseBattery") == L"1";
    const std::wstring ps = RegReadString(L"PauseSaver");
    g.pauseSaver   = ps.empty() ? true : (ps == L"1");
}

void ApplyVolume() {
    for (auto& v : g.views) v->player.SetVolume(g.volume);
}

// 库里的第 index 个 —— 点缩略图走这条
bool PlayFromLibrary(int index) {
    if (index < 0 || index >= static_cast<int>(g.library.size())) return false;
    return LoadVideo(g.library[index]);   // libIndex 由 LoadVideo 统一认
}

// 轮播推进。随机的时候避开当前这一个，不然「随机」经常原地不动。
void AdvanceLibrary(bool forward) {
    const int n = static_cast<int>(g.library.size());
    if (n <= 1) return;

    int next;
    if (g.playlistShuffle) {
        // 没有 rand 种子的顾虑：用 tick 当熵源，壁纸不需要密码学级别的随机
        next = static_cast<int>(GetTickCount64() % static_cast<unsigned>(n));
        if (next == g.libIndex) next = (next + 1) % n;
    } else {
        const int cur = (g.libIndex >= 0 && g.libIndex < n) ? g.libIndex : 0;
        next = forward ? (cur + 1) % n : (cur - 1 + n) % n;
    }
    PlayFromLibrary(next);
}

// 日程：到点了就换片。开着的时候它说了算，会盖过自动轮播。
void TickSchedule() {
    if (!g.scheduleEnabled) return;
    if (g.dayVideo.empty() && g.nightVideo.empty()) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    const int m = st.wHour * 60 + st.wMinute;

    // 允许「夜间跨午夜」：19:00 开始、次日 07:00 结束是常态
    const bool isDay = (g.dayStartMin <= g.nightStartMin)
                     ? (m >= g.dayStartMin && m < g.nightStartMin)
                     : !(m >= g.nightStartMin && m < g.dayStartMin);

    const std::wstring& want = isDay ? g.dayVideo : g.nightVideo;
    if (want.empty() || want == g.video) return;
    if (GetFileAttributesW(want.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    LoadVideo(want);
}

// 自动轮播。
//
// ⭐ 「播完一遍」的判据是**播放位置回绕**，不是「位置接近片长」。
//    壁纸是 SetLoop(TRUE) 播的，Media Engine 永远不会报结束；而「接近片长」
//    在 1 秒一次的心跳下只有几分之一的概率正好采到，会漏掉大部分轮次。
//    位置突然变小则是回绕的确凿信号，一次都不会漏。
void TickPlaylist() {
    static double   lastPos   = -1.0;
    static ULONGLONG lastSwitch = 0;

    if (g.scheduleEnabled) return;            // 日程开着就不轮播
    if (!g.playlistAuto || g.library.size() < 2) { lastPos = -1.0; return; }
    if (g.views.empty() || g.userPaused) return;

    const ULONGLONG now = GetTickCount64();
    // 刚换过片的三秒内不判 —— 新片起播时位置本来就是 0，会被当成回绕
    if (now - lastSwitch < 3000) return;

    if (g.playlistAdvance == 1) {
        if (lastSwitch == 0) { lastSwitch = now; return; }
        if (now - lastSwitch >= static_cast<ULONGLONG>(g.playlistInterval) * 60ULL * 1000ULL) {
            AdvanceLibrary(true);
            lastSwitch = now;
            lastPos = -1.0;
        }
        return;
    }

    const double pos = g.views.front()->player.Position();
    if (pos < 0) return;
    if (lastPos > 0.5 && pos < lastPos - 0.5) {   // 回绕了＝播完一遍
        AdvanceLibrary(true);
        lastSwitch = now;
        lastPos = -1.0;
        return;
    }
    lastPos = pos;
}

void LoadEffects() {
    g.fx.exposure       = LoadFx(L"FxExposure",   0.0,   -2.0,  2.0);
    g.fx.brightness     = LoadFx(L"FxBrightness", 0.0,   -0.6,  0.6);
    g.fx.contrast       = LoadFx(L"FxContrast",   1.0,    0.4,  2.0);
    g.fx.highlights     = LoadFx(L"FxHighlights", 1.0,    0.0,  1.0);
    g.fx.shadows        = LoadFx(L"FxShadows",    0.0,   -1.0,  1.0);
    g.fx.gamma          = LoadFx(L"FxGamma",      1.0,    0.4,  2.0);
    g.fx.saturation     = LoadFx(L"FxSaturation", 1.0,    0.0,  2.0);
    g.fx.vibrance       = LoadFx(L"FxVibrance",   0.0,   -1.0,  1.0);
    g.fx.temperature    = LoadFx(L"FxTemp",    6500.0, 2500.0, 10000.0);
    g.fx.tint           = LoadFx(L"FxTint",       0.0, -100.0, 100.0);
    g.fx.blur           = LoadFx(L"FxBlur",       0.0,    0.0, 60.0);
    g.fx.sharpen        = LoadFx(L"FxSharpen",    0.0,    0.0,  2.0);
    g.fx.vignette       = LoadFx(L"FxVignette",   0.0,    0.0,  2.0);
    g.fx.vignetteRadius = LoadFx(L"FxVigRadius",  1.0,    0.3,  3.0);
    g.fx.dim            = LoadFx(L"FxDim",        0.0,    0.0,  0.9);
}

void DestroyViews() {
    for (auto& v : g.views) {
        v->player.Close();
        if (v->hwnd) DestroyWindow(v->hwnd);
    }
    g.views.clear();
}

BOOL CALLBACK MonitorProc(HMONITOR mon, HDC, LPRECT, LPARAM) {
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(MONITORINFOEXW);
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;
    auto v = std::make_unique<View>();
    v->rect   = mi.rcMonitor;
    v->device = mi.szDevice;
    g.views.push_back(std::move(v));
    return TRUE;
}

// 显示器拓扑变了（投屏接入虚拟显示器、外接屏、改分辨率）就整个重建。
// 重建比增量同步简单得多，而这件事一天也发生不了几次。
void RebuildViews() {
    DestroyViews();
    g.host = desktop::FindWallpaperHost();
    if (!g.host) return;

    EnumDisplayMonitors(nullptr, nullptr, MonitorProc, 0);

    bool first = true;
    for (auto& v : g.views) {
        v->hwnd = CreateWindowExW(
            WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
            kWallClass, kAppName, WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, g.inst, nullptr);
        if (!v->hwnd) continue;
        SetWindowLongPtrW(v->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(v.get()));

        desktop::AttachToDesktop(v->hwnd, g.host, v->rect);

        if (!g.video.empty() && v->player.Open(v->hwnd, g.video)) {
            // 多屏时只让主屏那份有机会出声，否则同一段音频会叠 N 遍
            v->player.SetMuted(g.muted || !first);
        }
        first = false;
    }
    ApplyPlaybackState();
}

// —— 加载看门狗 ——
//
// 兜底：万一 Media Engine 既不 ready 也不报 MF_MEDIA_ENGINE_EVENT_ERROR，
// 事件 10 / 14 永远不来，屏幕就一直黑着而用户拿不到任何提示。
// 掐个表，到点还没 ready 就按失败处理，走和错误事件同一条路。
//
// ⚠️ 已知有一类文件会让加载失败：分片 MP4（moof/mdat）里声明了一条非音视频轨
//    （比如相机/无人机的 tmcd 时间码轨），而它的样本数据在文件最末尾。
//    那种情况下 Media Engine 约 8 秒后**会**正常报错，所以走的不是这个看门狗。
//    绕行办法是重封装：ffmpeg -i in.mp4 -map 0:v:0 -c copy -write_tmcd 0 out.mp4
constexpr ULONGLONG kLoadTimeoutMs = 15000;
ULONGLONG g_loadStartMs = 0;      // 0 = 当前没在等哪个视频
bool      g_loadWarned  = false;  // 一次加载只提示一次（多屏会各报一次）

void ArmLoadWatchdog()    { g_loadStartMs = GetTickCount64(); g_loadWarned = false; }
void DisarmLoadWatchdog() { g_loadStartMs = 0; }

void ReportLoadFailure() {
    DisarmLoadWatchdog();
    if (g_loadWarned) return;
    g_loadWarned = true;
    // ⚠️ 属主必须给，不能传 nullptr。设置窗开着的时候，无属主的 MessageBox 会被它
    //    整个盖住 —— 框确实弹了（EnumWindows 找得到），但用户屏幕上一点都看不见，
    //    看到的只有「选了视频却没反应」。2026-08-27 实测复现。
    //    只给属主还不够：MessageBox 自己会跑一个模态消息循环，期间仍然分发消息，
    //    所以「先报错、用户再从托盘打开设置窗」时，设置窗照样能盖到它上面去。
    //    MB_TOPMOST 才是真正保证看得见的那一个。
    HWND owner = settings::Wnd();
    if (!owner) owner = g.tray;
    MessageBoxW(owner, T(S_ERR_OPEN), kAppName,
                MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
}

bool LoadVideo(const std::wstring& path) {
    if (path.empty()) return false;
    g.video      = path;
    g.userPaused = false;
    RegWriteString(L"Video", path);
    ArmLoadWatchdog();

    // 当前播的这个在库里排第几 —— 不论是点缩略图、日程换的还是启动时读回来的，
    // 都在这里统一认一次。不认的话 libIndex 会停在 -1，轮播「下一个」就永远
    // 从头开始：起点明明是第 3 个，第一次却跳去第 2 个（2026-08-23 实测到过）。
    g.libIndex = -1;
    for (size_t i = 0; i < g.library.size(); ++i)
        if (g.library[i] == path) { g.libIndex = static_cast<int>(i); break; }

    RebuildViews();
    for (const auto& v : g.views) if (v->player.HasMedia()) return true;
    return false;
}

// ---------------------------------------------------------------- 托盘

void TrayAdd(HWND hwnd) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hwnd;
    nid.uID    = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    // 资源里 ID=1 那颗星（macOS 版的图标转过来的）；万一没编进去就退回系统默认
    nid.hIcon  = LoadIconW(g.inst, MAKEINTRESOURCEW(1));
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(nid.szTip, kAppName, ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void TrayRemove(HWND hwnd) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hwnd;
    nid.uID    = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void ShowTrayMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuSettings, T(S_MENU_SETTINGS));
    SetMenuDefaultItem(menu, kMenuSettings, FALSE);   // 双击托盘图标走这一项
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuToggle, g.userPaused ? T(S_RESUME) : T(S_PAUSE));
    AppendMenuW(menu, MF_STRING, kMenuPick,   T(S_VIDEO_PICK));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g.muted ? MF_CHECKED : 0),        kMenuMute,         T(S_MUTE));
    AppendMenuW(menu, MF_STRING | (g.fillMode ? MF_CHECKED : 0), kMenuFill, T(S_FILL));

    // 取景子菜单：九宫格 + 放大。只有填满模式下才有东西可裁，否则整个灰掉。
    HMENU framing = CreatePopupMenu();
    const wchar_t* kCells[9] = {
        T(S_POS_TL), T(S_POS_T), T(S_POS_TR),
        T(S_POS_L),  T(S_POS_C), T(S_POS_R),
        T(S_POS_BL), T(S_POS_B), T(S_POS_BR),
    };
    for (int i = 0; i < 9; ++i) {
        const double fx = (i % 3) * 0.5;
        const double fy = (i / 3) * 0.5;
        const bool on = (g.focusX == fx && g.focusY == fy);
        AppendMenuW(framing, MF_STRING | (on ? MF_CHECKED : 0), kMenuFocus + i, kCells[i]);
    }
    AppendMenuW(framing, MF_SEPARATOR, 0, nullptr);
    wchar_t zoomLabel[64];
    wsprintfW(zoomLabel, T(S_MENU_ZOOMIN_FMT), static_cast<int>(g.zoom * 100 + 0.5));
    AppendMenuW(framing, MF_STRING | (g.zoom >= 3.0 ? MF_GRAYED : 0), kMenuZoomIn,  zoomLabel);
    AppendMenuW(framing, MF_STRING | (g.zoom <= 1.0 ? MF_GRAYED : 0), kMenuZoomOut, T(S_MENU_ZOOMOUT));
    AppendMenuW(framing, MF_STRING, kMenuFocusReset, T(S_CROP_RESET));
    AppendMenuW(menu, MF_POPUP | (g.fillMode ? 0 : MF_GRAYED),
                reinterpret_cast<UINT_PTR>(framing), T(S_MENU_CROP));
    AppendMenuW(menu, MF_STRING | (g.pauseCovered ? MF_CHECKED : 0), kMenuPauseCovered, T(S_COVERED));
    // ⚠️ DisabledByUser 是用户在「任务管理器 → 启动」里关的，程序无权再打开
    // （RequestEnableAsync 会静默失败）。置灰并说明，别让用户点了没反应。
    {
        const startup::State as = startup::Query();
        const UINT flags = MF_STRING
                         | (as == startup::State::On ? MF_CHECKED : 0)
                         | (as == startup::State::LockedByUser || as == startup::State::Unavailable
                                ? MF_GRAYED : 0);
        AppendMenuW(menu, flags, kMenuAutostart,
                    as == startup::State::LockedByUser ? T(S_AUTOSTART_LOCKED)
                                                       : T(S_AUTOSTART));
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, T(S_MENU_EXIT));

    POINT pt;
    GetCursorPos(&pt);
    // 这两句是托盘菜单的标准配方：不 SetForegroundWindow 菜单点一下就消失，
    // 不补一个 WM_NULL 菜单关掉后会残留。
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// OPENFILENAME 的 filter 是「\0 分隔、\0\0 收尾」的一整块，字面量塞不进变量，
// 得现拼一个 wstring 出来（c_str() 之后中间那些 \0 还在）。
std::wstring VideoFilter() {
    std::wstring f;
    f += T(S_FILTER_VIDEO);                          f.push_back(0);
    f += L"*.mp4;*.mov;*.mkv;*.avi;*.wmv;*.m4v";     f.push_back(0);
    f += T(S_FILTER_ALL);                            f.push_back(0);
    f += L"*.*";                                     f.push_back(0);
    f.push_back(0);
    return f;
}

std::wstring PickVideoDialog(HWND owner) {
    const std::wstring filter = VideoFilter();
    wchar_t file[MAX_PATH * 2] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = owner;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = ARRAYSIZE(file);
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    return GetOpenFileNameW(&ofn) ? std::wstring(file) : std::wstring();
}

// 多选版，往视频库里加东西用。
//
// ⚠️ OFN_ALLOWMULTISELECT 的返回格式很别扭：多选时是
//    「目录\0文件1\0文件2\0\0」，只选一个时又退化成一个完整路径 + 单个 \0。
//    两种都要处理，不然「只加了一个」会拿到一条不存在的路径。
//    缓冲区也要给够 —— 选二三十个 4K 壁纸很容易超过 MAX_PATH*2。
std::vector<std::wstring> PickVideosDialog(HWND owner) {
    const std::wstring filter = VideoFilter();
    std::vector<std::wstring> out;
    std::vector<wchar_t> buf(64 * 1024, 0);

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = owner;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile   = buf.data();
    ofn.nMaxFile    = static_cast<DWORD>(buf.size());
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT;
    if (!GetOpenFileNameW(&ofn)) return out;

    const wchar_t* p2 = buf.data();
    std::wstring dir = p2;
    p2 += dir.size() + 1;
    if (*p2 == 0) { out.push_back(dir); return out; }      // 只选了一个

    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    while (*p2) {
        out.push_back(dir + p2);
        p2 += lstrlenW(p2) + 1;
    }
    return out;
}

// ---------------------------------------------------------------- 窗口过程

LRESULT CALLBACK WallpaperProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MEDIA_EVENT:
        // 10 = LOADEDMETADATA（到这里才知道视频多大，也才算得出要放大多少），
        // 14 = CANPLAY 作为兜底：metadata 那次万一没送到，这里再算一遍，
        //      重复算是幂等的，不会有副作用
        if (wp == 10 || wp == 14) {
            DisarmLoadWatchdog();   // 能算尺寸了就说明真的加载出来了
            if (auto* v = reinterpret_cast<View*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)))
                ApplyScaleMode(*v);
            return 0;
        }
        if (wp == 5 /* MF_MEDIA_ENGINE_EVENT_ERROR */) ReportLoadFailure();
        return 0;

    case WM_ERASEBKGND:
        return 1;   // 交换链每帧 Present 铺满，别再擦一遍造成闪烁

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK TrayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONDBLCLK) settings::Show();
        else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP)
            ShowTrayMenu(hwnd);
        return 0;

    // ⚠️ 必须在这个窗口上收：壁纸窗口 SetParent 之后是子窗口，
    //    而 WM_DISPLAYCHANGE 只广播给顶层窗口。
    case WM_DISPLAYCHANGE:
        RebuildViews();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kMenuSettings:
            settings::Show();
            break;
        case kCmdReloadFx:
            LoadEffects();
    // ⚠️ 库必须在下面 LoadVideo 之前读好 —— LoadVideo 要靠它认出当前视频的下标
    LoadLibrary_();
    LoadPlayback();
            ApplyEffectsAll();
            settings::Refresh();
            break;
        case kCmdReloadPlay:
            LoadLibrary_();
            LoadPlayback();
            ApplyVolume();
            ApplyPlaybackState();
            settings::Refresh();
            break;
        case kCmdReloadUi:
            g.darkMode = RegReadString(L"DarkMode") != L"0";
            g.english  = LoadEnglishPref();
            settings::ReloadTheme();
            break;
        case kMenuToggle:
            g.userPaused = !g.userPaused;
            ApplyPlaybackState();
            settings::Refresh();
            break;
        case kMenuPick: {
            const std::wstring path = PickVideoDialog(hwnd);
            if (!path.empty() && !LoadVideo(path))
                MessageBoxW(nullptr, T(S_ERR_LOAD), kAppName, MB_OK | MB_ICONWARNING);
            break;
        }
        case kMenuMute:
            g.muted = !g.muted;
            ApplyMute();
            RegWriteString(L"Muted", g.muted ? L"1" : L"0");
            settings::Refresh();
            break;
        case kMenuPauseCovered:
            g.pauseCovered = !g.pauseCovered;
            RegWriteString(L"PauseWhenCovered", g.pauseCovered ? L"1" : L"0");
            if (!g.pauseCovered) for (auto& v : g.views) v->coveredPause = false;
            ApplyPlaybackState();
            settings::Refresh();
            break;
        case kMenuFill:
            g.fillMode = !g.fillMode;
            RegWriteString(L"Fill", g.fillMode ? L"1" : L"0");
            ApplyScaleModeAll();
            settings::Refresh();
            break;
        case kMenuZoomIn:
        case kMenuZoomOut:
        case kMenuFocusReset:
            if (LOWORD(wp) == kMenuZoomIn)       g.zoom = (std::min)(3.0, g.zoom + 0.1);
            else if (LOWORD(wp) == kMenuZoomOut) g.zoom = (std::max)(1.0, g.zoom - 0.1);
            else { g.zoom = 1.0; g.focusX = 0.5; g.focusY = 0.5; }
            SaveFraming();
            ApplyScaleModeAll();
            settings::Refresh();
            break;
        case kMenuAutostart:
            SetAutostart(!IsAutostart());
            settings::Refresh();
            break;
        case kMenuExit:
            DestroyWindow(hwnd);
            break;
        default:
            // ⚠️ 切页**不能**写成 `case kCmdGoPage + 0 ... + 3` 那样逐个列 ——
            //    页面一多就会漏（2026-08-23 从 4 页加到 8 页时漏了后 4 个，
            //    表现为「切日程页没反应」）。统一按区间收。
            if (LOWORD(wp) >= kCmdGoPage && LOWORD(wp) < kCmdGoPage + 32) {
                settings::GoToPage(static_cast<int>(LOWORD(wp)) - kCmdGoPage);
            } else if (LOWORD(wp) >= kMenuFocus && LOWORD(wp) < kMenuFocus + 9) {
                const int i = LOWORD(wp) - kMenuFocus;
                g.focusX = (i % 3) * 0.5;
                g.focusY = (i / 3) * 0.5;
                SaveFraming();
                ApplyScaleModeAll();
                settings::Refresh();
            }
            break;
        }
        return 0;

    case WM_TIMER:
        if (wp == kTimerCover) {
            for (auto& v : g.views) v->coveredPause = ViewIsCovered(*v);
            ApplyPlaybackState();
            TickSchedule();
            TickPlaylist();
            // 蹭这个每秒一次的 tick 掐加载超时，不另开一个计时器
            if (g_loadStartMs && GetTickCount64() - g_loadStartMs > kLoadTimeoutMs)
                ReportLoadFailure();
        }
        return 0;

    case WM_WTSSESSION_CHANGE:
        // 锁屏时没人看得见壁纸，白烧 CPU
        if (wp == WTS_SESSION_LOCK)        { g.locked = true;  ApplyPlaybackState(); }
        else if (wp == WTS_SESSION_UNLOCK) { g.locked = false; ApplyPlaybackState(); }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kTimerCover);
        WTSUnRegisterSessionNotification(hwnd);
        TrayRemove(hwnd);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}



// ---------------------------------------------------------------- 入口

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    EnableDpiAwareness();   // 必须在建窗口之前

    // 单实例：两份壁纸叠在一起只会互相打架
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"StarPaperWin.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g.inst = inst;
    // ⚠️ 必须在这里就定语言：下面几个错误框比读设置（LoadSettings）早得多，
    //    留到那时候再定的话，启动失败的提示永远是中文。
    g.english = LoadEnglishPref();
    if (!Player::StartupMF()) {
        MessageBoxW(nullptr, T(S_ERR_MF), kAppName, MB_OK | MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc   = WallpaperProc;
    wc.hInstance     = inst;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kWallClass;
    wc.hIcon         = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm       = wc.hIcon;
    RegisterClassExW(&wc);

    wc.lpfnWndProc   = TrayProc;
    wc.lpszClassName = kTrayClass;
    RegisterClassExW(&wc);

    settings::Register(inst);

    g.tray = CreateWindowExW(0, kTrayClass, kAppName, WS_OVERLAPPED,
                             0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g.tray) {
        MessageBoxW(nullptr, T(S_ERR_WINDOW), kAppName, MB_OK | MB_ICONERROR);
        return 1;
    }

    if (!desktop::FindWallpaperHost()) {
        MessageBoxW(nullptr, T(S_ERR_WORKERW), kAppName, MB_OK | MB_ICONERROR);
        return 1;
    }

    TrayAdd(g.tray);
    SetTimer(g.tray, kTimerCover, 1000, nullptr);
    WTSRegisterSessionNotification(g.tray, NOTIFY_FOR_THIS_SESSION);

    g.muted        = RegReadString(L"Muted") != L"0";
    g.pauseCovered = RegReadString(L"PauseWhenCovered") == L"1";
    g.fillMode     = RegReadString(L"Fill") != L"0";        // 缺省即为填满
    g.focusX       = LoadPercent(L"FocusX", 0.5, 0.0, 1.0);
    g.focusY       = LoadPercent(L"FocusY", 0.5, 0.0, 1.0);
    g.zoom         = LoadPercent(L"Zoom",   1.0, 1.0, 3.0);
    g.darkMode     = RegReadString(L"DarkMode") != L"0";    // 缺省深色
    g.english      = LoadEnglishPref();
    LoadEffects();
    // ⚠️ 库必须在下面 LoadVideo 之前读好 —— LoadVideo 要靠它认出当前视频的下标
    LoadLibrary_();
    LoadPlayback();

    // 命令行给了路径就用它，否则用上次那个，再否则弹窗让选。
    //
    // ⚠️ 不要用 wWinMain 的 lpCmdLine —— 它由 CRT 交付，不同工具链（MSVC / mingw）
    //    行为不一致，实测 mingw + -municode 下拿不到参数。
    //    GetCommandLineW() + CommandLineToArgvW() 是 Win32 自己的解析，
    //    顺带把引号和空格路径也处理干净了。
    std::wstring path;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            if (argc > 1) path = argv[1];
            LocalFree(argv);
        }
    }
    if (path.empty()) path = RegReadString(L"Video");
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        path = PickVideoDialog(g.tray);
    if (!path.empty()) LoadVideo(path);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // 设置窗口里的 Tab / 方向键 / 空格要交给对话框逻辑，否则控件之间没法用键盘走
        if (settings::IsOpen() && settings::HandleDialogMessage(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroyViews();
    thumbs::Shutdown();      // 缩略图后台线程，得在进程退出前收干净
    Player::ShutdownMF();
    if (mutex) CloseHandle(mutex);
    return 0;
}
