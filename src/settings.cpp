// 设置窗口。
//
// 布局：左侧自绘分类栏 + 右侧页面（内容 / 取景 / 画面 / 通用）。
// 页面切换不销毁控件，只 Show/Hide —— 15 个滑杆重建一遍要几十次窗口创建，
// 切页会看得见地卡一下。
//
// 所有控件都是自绘的（见 widgets.cpp），所有颜色和文字都从 theme.cpp 取，
// 深色/浅色、中文/英文都能在运行时切。
#include "settings.h"

#include <commctrl.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <algorithm>
#include <string>
#include <vector>

#include "app.h"
#include "theme.h"
#include "widgets.h"
#include "thumbs.h"

namespace settings {
namespace {

constexpr wchar_t kClass[] = L"StarPaperSettings";

// 控件 ID。分段留空是为了以后往中间插东西不用整体挪。
enum : int {
    kIdNav = 90, kIdPreview,

    kIdTitle = 100, kIdState, kIdClose,

    kIdPick = 110, kIdVideoName, kIdVideoCap, kIdVideoPath,
    kIdFill, kIdMute, kIdCovered, kIdCoveredHint, kIdAutostart,

    kIdZoom = 130, kIdFocusX, kIdFocusY, kIdCropReset, kIdCropHint,

    kIdFx = 150,           // 连号 15 个，见 kFxRows
    kIdFxReset = 170, kIdHeadTone, kIdHeadColor, kIdHeadEffect,
    kIdHintVibrance, kIdHintDim,

    kIdTheme = 200, kIdLang, kIdPlayPause,
    kIdHeadAppearance, kIdHeadPlayback, kIdLblTheme, kIdLblLang,

    // 播放页
    kIdLib = 220, kIdLibAdd, kIdLibRemove, kIdLibClear, kIdLibHint, kIdHeadLibrary,
    kIdAuto, kIdShuffle, kIdAdvance, kIdInterval, kIdLblAdvance, kIdLblInterval,

    // 日程页
    kIdSchedOn = 240, kIdDayPick, kIdNightPick, kIdDayPath, kIdNightPath,
    kIdDayStart, kIdNightStart, kIdLblDay, kIdLblNight, kIdSchedHint,

    // 声音页
    kIdMute2 = 260, kIdVolume, kIdAudioHint,

    // 电源页
    kIdPwLocked = 270, kIdPwBattery, kIdPwSaver, kIdPwCovered, kIdPowerHint, kIdHeadPower,
};

// 页面。左栏点一下就是切这个，控件不销毁，只 Show/Hide。
// ⚠️ 顺序必须和 theme.h 里 S_TAB_* 的顺序一致 —— 左栏和页面标题都靠
//    `S_TAB_CONTENT + page` 取文案。
enum Page { PG_CONTENT, PG_CROP, PG_IMAGE, PG_PLAYBACK, PG_SCHEDULE,
            PG_AUDIO, PG_POWER, PG_GENERAL, PG_COUNT };

// 窗口尺寸与栅格，全是逻辑像素（真正用的时候过一遍 S()）
constexpr int kWinW = 900, kWinH = 692;
constexpr int kNavW = 172;
constexpr int kPad  = 24;
constexpr int kColX = kNavW + kPad;              // 左列
constexpr int kColW = 312;
constexpr int kCol2X = kColX + kColW + 34;       // 右列
constexpr int kBodyY = 74;                       // 页面标题下方
// 取景页：预览在上、滑杆在下
constexpr int kPrevW = 500, kPrevH = 300;
constexpr int kCropSlidersY = kBodyY + kPrevH + 26;
// 播放页：视频库网格的高度（3 行卡片）
constexpr int kLibH = 372;

constexpr UINT_PTR kTimerPoster = 1;   // 预览底图定时刷新，让它是活的而不是一张死图

HWND  g_wnd      = nullptr;
HWND  g_nav      = nullptr;
HWND  g_preview  = nullptr;
HWND  g_lib      = nullptr;
HFONT g_font     = nullptr;
HFONT g_fontBold = nullptr;
HFONT g_fontSmall = nullptr;
HFONT g_fontTitle = nullptr;
int   g_dpi      = 96;
bool  g_syncing  = false;   // 我们自己在刷控件，别把它当成用户操作
int   g_page     = PG_CONTENT;
int   g_navHot   = -1;

std::vector<HWND> g_pageCtrls[PG_COUNT];


int S(int px) { return MulDiv(px, g_dpi, 96); }   // 逻辑像素 → 当前 DPI 的物理像素

// ---------------------------------------------------------------- 预览底图

// 完整的一帧（不裁剪），两份：原样的和压暗的。
// 画的时候暗版铺满、亮版只画框内 —— 框外压暗的效果就出来了，不用 AlphaBlend。
struct Poster {
    int  w = 0, h = 0;
    HBITMAP bright = nullptr;
    HBITMAP dim    = nullptr;
    bool valid() const { return bright && dim && w > 0 && h > 0; }
    void reset() {
        if (bright) { DeleteObject(bright); bright = nullptr; }
        if (dim)    { DeleteObject(dim);    dim = nullptr; }
        w = h = 0;
    }
};
Poster g_poster;

HBITMAP MakeDib(int w, int h, const unsigned char* bgra, double scale) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;         // 负数 = 自上而下，和我们拿到的数据一致
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) return nullptr;

    auto* out = static_cast<unsigned char*>(bits);
    const size_t n = static_cast<size_t>(w) * h * 4;
    if (scale >= 0.999) {
        memcpy(out, bgra, n);
    } else {
        for (size_t i = 0; i < n; i += 4) {
            out[i + 0] = static_cast<unsigned char>(bgra[i + 0] * scale);
            out[i + 1] = static_cast<unsigned char>(bgra[i + 1] * scale);
            out[i + 2] = static_cast<unsigned char>(bgra[i + 2] * scale);
            out[i + 3] = 255;
        }
    }
    return bmp;
}

// 从第一块屏的播放器抓一帧。抓不到（还没 ready / 没视频）就保持上一张。
void RefreshPoster(int wantW, int wantH) {
    if (g.views.empty() || !g.views.front()->player.HasMedia()) { g_poster.reset(); return; }
    Player& p = g.views.front()->player;

    int vw = 0, vh = 0;
    if (!p.GetVideoSize(vw, vh) || vw <= 0 || vh <= 0) return;

    // 按视频比例、不超过预览区
    double s = (std::min)(static_cast<double>(wantW) / vw, static_cast<double>(wantH) / vh);
    int w = (std::max)(16, static_cast<int>(vw * s));
    int h = (std::max)(16, static_cast<int>(vh * s));

    std::vector<unsigned char> px;
    if (!p.CapturePoster(w, h, px)) return;

    g_poster.reset();
    g_poster.bright = MakeDib(w, h, px.data(), 1.0);
    g_poster.dim    = MakeDib(w, h, px.data(), 0.42);
    g_poster.w = w;
    g_poster.h = h;
}

// ---------------------------------------------------------------- 预览控件

// 预览区里的三个矩形：底图摆在哪、取景框在哪。
struct PreviewGeo {
    RECT image{};    // 底图（完整画面）在控件里的位置
    RECT box{};      // 取景框
    bool ok = false;
};

PreviewGeo ComputeGeo(HWND hwnd) {
    PreviewGeo geo;
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (!g_poster.valid()) return geo;

    const int cw = rc.right, ch = rc.bottom;
    // 底图按 aspect fit 居中
    double s = (std::min)(static_cast<double>(cw) / g_poster.w,
                          static_cast<double>(ch) / g_poster.h);
    const int iw = (std::max)(1, static_cast<int>(g_poster.w * s));
    const int ih = (std::max)(1, static_cast<int>(g_poster.h * s));
    const int ix = (cw - iw) / 2;
    const int iy = (ch - ih) / 2;
    geo.image = { ix, iy, ix + iw, iy + ih };

    // 取景框：和桌面裁剪同一个公式
    int vw = 0, vh = 0;
    if (g.views.empty() || !g.views.front()->player.GetVideoSize(vw, vh)) return geo;
    const RECT& mon = g.views.front()->rect;
    const int mw = mon.right - mon.left, mh = mon.bottom - mon.top;

    double l, t, r, b;
    if (g.fillMode) {
        FramingRect(vw, vh, mw, mh, g.focusX, g.focusY, g.zoom, l, t, r, b);
    } else {
        l = 0.0; t = 0.0; r = 1.0; b = 1.0;   // 完整显示模式：整幅都要，框就是整张图
    }
    geo.box = {
        ix + static_cast<int>(l * iw + 0.5),
        iy + static_cast<int>(t * ih + 0.5),
        ix + static_cast<int>(r * iw + 0.5),
        iy + static_cast<int>(b * ih + 0.5),
    };
    geo.ok = true;
    return geo;
}

struct Drag {
    bool   active = false;
    POINT  start{};
    double focusX = 0, focusY = 0;
};
Drag g_drag;

void SyncControls();

LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;   // 全部在 WM_PAINT 里双缓冲画完，别再擦一遍

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        // 双缓冲：直接往 dc 上画会闪，拖框的时候尤其明显
        HDC     mem = CreateCompatibleDC(dc);
        HBITMAP buf = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ oldBuf = SelectObject(mem, buf);

        HBRUSH bg = CreateSolidBrush(g.darkMode ? RGB(24, 24, 27) : RGB(226, 226, 231));
        FillRect(mem, &rc, bg);
        DeleteObject(bg);

        const PreviewGeo geo = ComputeGeo(hwnd);
        if (g_poster.valid()) {
            HDC src = CreateCompatibleDC(dc);
            const int iw = geo.image.right - geo.image.left;
            const int ih = geo.image.bottom - geo.image.top;

            SetStretchBltMode(mem, HALFTONE);
            SetBrushOrgEx(mem, 0, 0, nullptr);

            // 1) 压暗版铺满整张底图
            HGDIOBJ oldSrc = SelectObject(src, g_poster.dim);
            StretchBlt(mem, geo.image.left, geo.image.top, iw, ih,
                       src, 0, 0, g_poster.w, g_poster.h, SRCCOPY);

            // 2) 亮版只画框内 —— 框外压暗的效果就是这么来的
            if (geo.ok) {
                SelectObject(src, g_poster.bright);
                const double sx = static_cast<double>(g_poster.w) / iw;
                const double sy = static_cast<double>(g_poster.h) / ih;
                const int bx = geo.box.left - geo.image.left;
                const int by = geo.box.top  - geo.image.top;
                const int bw = geo.box.right - geo.box.left;
                const int bh = geo.box.bottom - geo.box.top;
                StretchBlt(mem, geo.box.left, geo.box.top, bw, bh,
                           src, static_cast<int>(bx * sx), static_cast<int>(by * sy),
                           static_cast<int>(bw * sx), static_cast<int>(bh * sy), SRCCOPY);
            }
            SelectObject(src, oldSrc);
            DeleteDC(src);

            // 3) 白框 + 四角把手
            if (geo.ok) {
                HPEN pen = CreatePen(PS_SOLID, (std::max)(1, S(2)), RGB(255, 255, 255));
                HGDIOBJ oldPen = SelectObject(mem, pen);
                HGDIOBJ oldBr  = SelectObject(mem, GetStockObject(NULL_BRUSH));
                Rectangle(mem, geo.box.left, geo.box.top, geo.box.right, geo.box.bottom);
                SelectObject(mem, oldBr);
                SelectObject(mem, oldPen);
                DeleteObject(pen);

                HBRUSH knob = CreateSolidBrush(RGB(255, 255, 255));
                const int k = S(5);
                const POINT corners[4] = {
                    { geo.box.left,  geo.box.top },    { geo.box.right, geo.box.top },
                    { geo.box.left,  geo.box.bottom }, { geo.box.right, geo.box.bottom },
                };
                for (const POINT& c : corners) {
                    RECT r = { c.x - k, c.y - k, c.x + k, c.y + k };
                    FillRect(mem, &r, knob);
                }
                DeleteObject(knob);
            }
        } else {
            SetBkMode(mem, TRANSPARENT);
            SetTextColor(mem, Pal().textDim);
            HGDIOBJ oldF = SelectObject(mem, g_font);
            const wchar_t* msg2 = g.video.empty()
                                ? T(S_PREVIEW_NONE)
                                : T(S_PREVIEW_READING);
            DrawTextW(mem, msg2, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(mem, oldF);
        }

        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBuf);
        DeleteObject(buf);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SETCURSOR: {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        const PreviewGeo geo = ComputeGeo(hwnd);
        if (geo.ok && PtInRect(&geo.box, pt)) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return TRUE;
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        if (!g.fillMode) return 0;          // 完整显示模式没得裁，也就没得拖
        const PreviewGeo geo = ComputeGeo(hwnd);
        if (!geo.ok) return 0;
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (!PtInRect(&geo.box, pt)) return 0;
        g_drag.active = true;
        g_drag.start  = pt;
        g_drag.focusX = g.focusX;
        g_drag.focusY = g.focusY;
        SetCapture(hwnd);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!g_drag.active) return 0;
        const PreviewGeo geo = ComputeGeo(hwnd);
        if (!geo.ok) return 0;
        // 拖动量除以「框能活动的范围」就是 focus 的变化量 —— 和 macOS 版同一套换算
        const int spanX = (geo.image.right - geo.image.left) - (geo.box.right - geo.box.left);
        const int spanY = (geo.image.bottom - geo.image.top) - (geo.box.bottom - geo.box.top);
        const int dx = GET_X_LPARAM(lp) - g_drag.start.x;
        const int dy = GET_Y_LPARAM(lp) - g_drag.start.y;
        if (spanX > 0) g.focusX = (std::min)(1.0, (std::max)(0.0, g_drag.focusX + static_cast<double>(dx) / spanX));
        if (spanY > 0) g.focusY = (std::min)(1.0, (std::max)(0.0, g_drag.focusY + static_cast<double>(dy) / spanY));
        ApplyScaleModeAll();     // 桌面实时跟着动，不用松手才看到效果
        SyncControls();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONUP:
        if (g_drag.active) {
            g_drag.active = false;
            ReleaseCapture();
            SaveFraming();
        }
        return 0;

    case WM_MOUSEWHEEL: {
        if (!g.fillMode) return 0;
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        g.zoom = (std::min)(3.0, (std::max)(1.0, g.zoom + (delta > 0 ? 0.05 : -0.05)));
        ApplyScaleModeAll();
        SaveFraming();
        SyncControls();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- 视频库网格

// 卡片：上面一张缩略图，下面一行文件名。
// 156 宽是量出来的：内容区 660 逻辑像素，4 × 156 + 3 × 12 = 660，正好排满不留半张。
constexpr int kCardW  = 156;
constexpr int kCardTh = 88;    // 缩略图高（16:9）
constexpr int kCardH  = 116;   // 卡片总高（缩略图 + 文件名那一行）
constexpr int kCardGap = 12;

int g_libScroll = 0;      // 纵向滚动偏移（物理像素）
int g_libHot    = -1;

int LibCols(int clientW) {
    const int cw = S(kCardW) + S(kCardGap);
    int n = (clientW + S(kCardGap)) / (cw > 0 ? cw : 1);
    return n < 1 ? 1 : n;
}

int LibContentH(int clientW) {
    const int n = static_cast<int>(g.library.size());
    if (n == 0) return 0;
    const int cols = LibCols(clientW);
    const int rows = (n + cols - 1) / cols;
    return rows * (S(kCardH) + S(kCardGap));
}

RECT LibCardRect(int i, int clientW) {
    const int cols = LibCols(clientW);
    const int r = i / cols, c = i % cols;
    const int x = c * (S(kCardW) + S(kCardGap));
    const int y = r * (S(kCardH) + S(kCardGap)) - g_libScroll;
    return { x, y, x + S(kCardW), y + S(kCardH) };
}

int LibHitTest(HWND h, POINT pt) {
    RECT rc;
    GetClientRect(h, &rc);
    for (int i = 0; i < static_cast<int>(g.library.size()); ++i) {
        RECT r = LibCardRect(i, static_cast<int>(rc.right));
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

void LibClampScroll(HWND h) {
    RECT rc;
    GetClientRect(h, &rc);
    const int maxScroll = (std::max)(0, LibContentH(static_cast<int>(rc.right))
                                        - static_cast<int>(rc.bottom));
    if (g_libScroll > maxScroll) g_libScroll = maxScroll;
    if (g_libScroll < 0) g_libScroll = 0;
}

std::wstring BaseName(const std::wstring& path) {
    const size_t s2 = path.find_last_of(L"\\/");
    return s2 == std::wstring::npos ? path : path.substr(s2 + 1);
}

LRESULT CALLBACK LibProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const Palette& P = Pal();
    switch (msg) {
    case WM_ERASEBKGND: return 1;

    case thumbs::kReady:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        HDC     mem = CreateCompatibleDC(dc);
        HBITMAP buf = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ ob  = SelectObject(mem, buf);

        HBRUSH bg = CreateSolidBrush(P.bg);
        FillRect(mem, &rc, bg);
        DeleteObject(bg);
        SetBkMode(mem, TRANSPARENT);

        if (g.library.empty()) {
            SelectObject(mem, g_font);
            SetTextColor(mem, P.textDim);
            RECT t = rc;
            DrawTextW(mem, T(S_LIB_EMPTY), -1, &t,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }

        HDC src = CreateCompatibleDC(dc);
        for (int i = 0; i < static_cast<int>(g.library.size()); ++i) {
            RECT r = LibCardRect(i, static_cast<int>(rc.right));
            if (r.bottom < 0 || r.top > rc.bottom) continue;    // 滚出去的不画

            const std::wstring& path = g.library[i];
            const bool current = (path == g.video);

            RECT th = { r.left, r.top, r.right, r.top + S(kCardTh) };
            HBRUSH cb = CreateSolidBrush(i == g_libHot ? P.hover : P.card);
            FillRect(mem, &th, cb);
            DeleteObject(cb);

            HBITMAP bmp = thumbs::Get(path, S(kCardW), S(kCardTh), hwnd);
            if (bmp) {
                BITMAP bm = {};
                GetObjectW(bmp, sizeof(bm), &bm);
                if (bm.bmWidth > 0 && bm.bmHeight > 0) {
                    // 等比 fit 进缩略图区，居中。缩略图的比例跟视频走，
                    // 竖屏视频塞进 16:9 的槽里左右留黑，不拉伸。
                            const int aw = static_cast<int>(th.right - th.left);
                    const int ah = static_cast<int>(th.bottom - th.top);
                    const double k = (std::min)(static_cast<double>(aw) / bm.bmWidth,
                                                static_cast<double>(ah) / bm.bmHeight);
                    const int dw = (std::max)(1, static_cast<int>(bm.bmWidth * k));
                    const int dh = (std::max)(1, static_cast<int>(bm.bmHeight * k));
                    HGDIOBJ os = SelectObject(src, bmp);
                    SetStretchBltMode(mem, HALFTONE);
                    SetBrushOrgEx(mem, 0, 0, nullptr);
                    StretchBlt(mem, th.left + (aw - dw) / 2, th.top + (ah - dh) / 2, dw, dh,
                               src, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
                    SelectObject(src, os);
                }
            } else {
                SelectObject(mem, g_fontSmall);
                SetTextColor(mem, P.textDim);
                RECT t = th;
                DrawTextW(mem, L"…", -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }

            // 正在播放的那张：整圈强调色边框 + 缩略图上压一条标签
            if (current) {
                HPEN pen = CreatePen(PS_SOLID, (std::max)(1, S(2)), P.accent);
                HGDIOBJ op = SelectObject(mem, pen);
                HGDIOBJ obr = SelectObject(mem, GetStockObject(NULL_BRUSH));
                Rectangle(mem, th.left, th.top, th.right, th.bottom);
                SelectObject(mem, obr);
                SelectObject(mem, op);
                DeleteObject(pen);

                RECT tag = { th.left, th.bottom - S(20), th.right, th.bottom };
                HBRUSH tb = CreateSolidBrush(P.accent);
                FillRect(mem, &tag, tb);
                DeleteObject(tb);
                SelectObject(mem, g_fontSmall);
                SetTextColor(mem, RGB(255, 255, 255));
                DrawTextW(mem, T(S_NOW_PLAYING), -1, &tag,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }

            RECT nr = { r.left + S(2), th.bottom + S(4), r.right - S(2), r.bottom };
            SelectObject(mem, g_fontSmall);
            SetTextColor(mem, current ? P.text : P.textDim);
            std::wstring nm = BaseName(path);
            DrawTextW(mem, nm.c_str(), -1, &nr,
                      DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        DeleteDC(src);

        // 内容超出可视区时右边给一条细滚动指示 —— 不然不知道下面还有
        const int ch    = static_cast<int>(rc.bottom);
        const int total = LibContentH(static_cast<int>(rc.right));
        if (total > ch && ch > 0) {
            const int barH = (std::max)(S(24), ch * ch / total);
            const int maxScroll = total - ch;
            const int barY = maxScroll > 0 ? (ch - barH) * g_libScroll / maxScroll : 0;
            RECT b = { rc.right - S(4), barY, rc.right - S(1), barY + barH };
            HBRUSH sb = CreateSolidBrush(P.track);
            FillRect(mem, &b, sb);
            DeleteObject(sb);
        }

        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(buf);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        const int hot = LibHitTest(hwnd, pt);
        if (hot != g_libHot) {
            g_libHot = hot;
            InvalidateRect(hwnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_libHot = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        const int i = LibHitTest(hwnd, pt);
        if (i >= 0) SendMessageW(GetParent(hwnd), WM_APP + 31, static_cast<WPARAM>(i), 0);
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        const int i = LibHitTest(hwnd, pt);
        if (i >= 0) SendMessageW(GetParent(hwnd), WM_APP + 32, static_cast<WPARAM>(i), 0);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        const int d = GET_WHEEL_DELTA_WPARAM(wp);
        g_libScroll -= d * S(kCardH) / 240;      // 一格滚半张卡片
        LibClampScroll(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_SETCURSOR:
        if (g_libHot >= 0) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- 画面页的表

// 15 个调色滑杆。范围/默认值/显示格式与 mac 版 SettingsView.imageTab 逐条对齐，
// 成员指针让「同步控件」和「读回参数」各写一次循环就够了。
struct FxRow {
    int          id;
    StrId        label;
    double       lo, hi, def;
    widgets::Fmt fmt;
    float Effects::* field;
};

const FxRow kFxRows[] = {
    // —— 影调（6）——
    { kIdFx + 0,  S_EXPOSURE,   -2.0,   2.0,    0.0, widgets::F_EV,         &Effects::exposure   },
    { kIdFx + 1,  S_BRIGHTNESS, -0.6,   0.6,    0.0, widgets::F_SIGNED2,    &Effects::brightness },
    { kIdFx + 2,  S_CONTRAST,    0.4,   2.0,    1.0, widgets::F_PLAIN2,     &Effects::contrast   },
    { kIdFx + 3,  S_HIGHLIGHTS,  0.0,   1.0,    1.0, widgets::F_PLAIN2,     &Effects::highlights },
    { kIdFx + 4,  S_SHADOWS,    -1.0,   1.0,    0.0, widgets::F_SIGNED2,    &Effects::shadows    },
    { kIdFx + 5,  S_GAMMA,       0.4,   2.0,    1.0, widgets::F_PLAIN2,     &Effects::gamma      },
    // —— 色彩（4）——
    { kIdFx + 6,  S_SATURATION,  0.0,   2.0,    1.0, widgets::F_PLAIN2,     &Effects::saturation },
    { kIdFx + 7,  S_VIBRANCE,   -1.0,   1.0,    0.0, widgets::F_SIGNED2,    &Effects::vibrance   },
    { kIdFx + 8,  S_TEMPERATURE, 2500., 10000., 6500., widgets::F_KELVIN,   &Effects::temperature},
    { kIdFx + 9,  S_TINT,       -100.,  100.,   0.0, widgets::F_SIGNED_INT, &Effects::tint       },
    // —— 效果（5）——
    { kIdFx + 10, S_BLUR,        0.0,   60.0,   0.0, widgets::F_INT,        &Effects::blur       },
    { kIdFx + 11, S_SHARPEN,     0.0,   2.0,    0.0, widgets::F_PLAIN2,     &Effects::sharpen    },
    { kIdFx + 12, S_VIGNETTE,    0.0,   2.0,    0.0, widgets::F_PLAIN2,     &Effects::vignette   },
    { kIdFx + 13, S_VIGRADIUS,   0.3,   3.0,    1.0, widgets::F_PLAIN2,     &Effects::vignetteRadius },
    { kIdFx + 14, S_DIM,         0.0,   0.9,    0.0, widgets::F_PERCENT,    &Effects::dim        },
};
constexpr int kFxCount  = static_cast<int>(sizeof(kFxRows) / sizeof(kFxRows[0]));
constexpr int kToneN    = 6;
constexpr int kColorN   = 4;
static_assert(kFxCount == 15, "调色滑杆的数量变了，记得同步下面的分组和布局");

// ---------------------------------------------------------------- 同步

void SyncControls() {
    if (!g_wnd) return;
    g_syncing = true;

    // 内容
    widgets::SetCheck(g_wnd, kIdFill,      g.fillMode);
    widgets::SetCheck(g_wnd, kIdAutostart, IsAutostart());
    widgets::SetText(g_wnd, kIdVideoName, g.video.empty() ? T(S_VIDEO_NONE) : BaseName(g.video));
    widgets::SetText(g_wnd, kIdVideoPath, g.video);

    // 播放
    widgets::SetCheck (g_wnd, kIdAuto,     g.playlistAuto);
    widgets::SetCheck (g_wnd, kIdShuffle,  g.playlistShuffle);
    widgets::SetSeg   (g_wnd, kIdAdvance,  g.playlistAdvance);
    widgets::SetValue (g_wnd, kIdInterval, g.playlistInterval);
    for (int id : { kIdShuffle, kIdAdvance, kIdLblAdvance }) widgets::Enable(g_wnd, id, g.playlistAuto);
    widgets::Enable(g_wnd, kIdInterval, g.playlistAuto && g.playlistAdvance == 1);

    // 日程
    widgets::SetCheck(g_wnd, kIdSchedOn,    g.scheduleEnabled);
    widgets::SetText (g_wnd, kIdDayPath,    g.dayVideo.empty()   ? T(S_VIDEO_NONE) : g.dayVideo);
    widgets::SetText (g_wnd, kIdNightPath,  g.nightVideo.empty() ? T(S_VIDEO_NONE) : g.nightVideo);
    widgets::SetValue(g_wnd, kIdDayStart,   g.dayStartMin);
    widgets::SetValue(g_wnd, kIdNightStart, g.nightStartMin);
    for (int id : { kIdDayPick, kIdNightPick, kIdDayStart, kIdNightStart, kIdLblDay, kIdLblNight })
        widgets::Enable(g_wnd, id, g.scheduleEnabled);

    // 声音
    widgets::SetCheck (g_wnd, kIdMute2,  g.muted);
    widgets::SetValue (g_wnd, kIdVolume, g.volume);
    widgets::Enable   (g_wnd, kIdVolume, !g.muted);

    // 电源
    widgets::SetCheck(g_wnd, kIdPwCovered, g.pauseCovered);
    widgets::SetCheck(g_wnd, kIdPwLocked,  g.pauseLocked);
    widgets::SetCheck(g_wnd, kIdPwBattery, g.pauseBattery);
    widgets::SetCheck(g_wnd, kIdPwSaver,   g.pauseSaver);

    if (g_lib) InvalidateRect(g_lib, nullptr, FALSE);

    // 取景
    widgets::SetValue(g_wnd, kIdZoom,   g.zoom);
    widgets::SetValue(g_wnd, kIdFocusX, g.focusX);
    widgets::SetValue(g_wnd, kIdFocusY, g.focusY);
    for (int id : { kIdZoom, kIdFocusX, kIdFocusY, kIdCropReset })
        widgets::Enable(g_wnd, id, g.fillMode);
    widgets::SetText(g_wnd, kIdCropHint, T(g.fillMode ? S_CROP_HINT : S_CROP_NEEDFILL));

    // 画面
    for (const FxRow& r : kFxRows)
        widgets::SetValue(g_wnd, r.id, g.fx.*(r.field));
    // 暗角范围只有开了暗角才有意义 —— 和 mac 版同样的联动
    widgets::Enable(g_wnd, kIdFx + 13, g.fx.vignette > 0.01f);

    // 通用
    widgets::SetSeg(g_wnd, kIdTheme, g.darkMode ? 0 : 1);
    widgets::SetSeg(g_wnd, kIdLang,  g.english  ? 1 : 0);
    widgets::SetText(g_wnd, kIdPlayPause, T(g.userPaused ? S_RESUME : S_PAUSE));

    // 底部状态
    int playing = 0, paused = 0;
    for (const auto& v : g.views) {
        if (!v->player.HasMedia()) continue;
        v->player.IsPaused() ? ++paused : ++playing;
    }
    const wchar_t* what = g.userPaused ? T(S_STATE_PAUSED)
                        : (paused > 0 && playing == 0) ? T(S_STATE_COVERED)
                        : T(S_STATE_PLAYING);
    // ⚠️ 这里**不能**用 swprintf 的 %s。
    //    mingw 链接的是 msvcrt.dll，它的宽字符版 printf 里 %s 表示的是**窄**字符串
    //    （%ls 才是宽）—— 实测中文输出成乱码、英文 "Playing" 只剩一个 'P'
    //    （把 UTF-16 的 'P'=0x0050 当成了 "P\0"）。直接拼 wstring，一了百了。
    wchar_t num[16];
    swprintf(num, 16, L"%d", static_cast<int>(g.views.size()));
    std::wstring info = std::wstring(what) + L" · " + num + L" "
                      + T(g.views.size() == 1 ? S_SCREEN : S_SCREENS);
    widgets::SetText(g_wnd, kIdState, info);

    widgets::SetText(g_wnd, kIdTitle, T(static_cast<StrId>(S_TAB_CONTENT + g_page)));

    g_syncing = false;
}

// ---------------------------------------------------------------- 左侧分类栏

// 自绘。用 SysTabControl32 的话深色下没法看，而且横向 tab 放不下四个中文分类
// 还要跟标题抢地方；左栏纵向排反而更像 macOS 的设置。
LRESULT CALLBACK NavProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const Palette& P = Pal();
    switch (msg) {
    case WM_ERASEBKGND: return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        HDC     mem = CreateCompatibleDC(dc);
        HBITMAP buf = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ ob  = SelectObject(mem, buf);

        HBRUSH bg = CreateSolidBrush(P.sidebar);
        FillRect(mem, &rc, bg);
        DeleteObject(bg);

        SetBkMode(mem, TRANSPARENT);

        RECT tr = { S(20), S(22), rc.right - S(12), S(50) };
        SelectObject(mem, g_fontBold);
        SetTextColor(mem, P.text);
        DrawTextW(mem, L"StarPaper", -1, &tr, DT_LEFT | DT_SINGLELINE);

        SelectObject(mem, g_font);
        for (int i = 0; i < PG_COUNT; ++i) {
            RECT r = { S(10), S(74 + i * 42), rc.right - S(10), S(74 + i * 42 + 36) };
            if (i == g_page) {
                HBRUSH b = CreateSolidBrush(P.sel);
                HRGN rgn = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, S(14), S(14));
                FillRgn(mem, rgn, b);
                DeleteObject(rgn);
                DeleteObject(b);
                // 选中项左边一道强调色竖条，比只换底色更好认
                RECT bar = { r.left + S(3), r.top + S(9), r.left + S(6), r.bottom - S(9) };
                HBRUSH ab = CreateSolidBrush(P.accent);
                FillRect(mem, &bar, ab);
                DeleteObject(ab);
            } else if (i == g_navHot) {
                HBRUSH b = CreateSolidBrush(P.hover);
                HRGN rgn = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, S(14), S(14));
                FillRgn(mem, rgn, b);
                DeleteObject(rgn);
                DeleteObject(b);
            }
            RECT t2 = r;
            t2.left += S(18);
            SetTextColor(mem, i == g_page ? P.text : P.textDim);
            DrawTextW(mem, T(static_cast<StrId>(S_TAB_CONTENT + i)), -1, &t2,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        }

        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(buf);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        const int y = GET_Y_LPARAM(lp);
        int hot = -1;
        for (int i = 0; i < PG_COUNT; ++i)
            if (y >= S(74 + i * 42) && y < S(74 + i * 42 + 36)) hot = i;
        if (hot != g_navHot) {
            g_navHot = hot;
            InvalidateRect(hwnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_navHot = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_LBUTTONDOWN: {
        const int y = GET_Y_LPARAM(lp);
        for (int i = 0; i < PG_COUNT; ++i) {
            if (y >= S(74 + i * 42) && y < S(74 + i * 42 + 36)) {
                if (i != g_page) SendMessageW(GetParent(hwnd), WM_APP + 30, i, 0);
                break;
            }
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (g_navHot >= 0) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- 组装页面

void Reg(int page, HWND h) { if (h) g_pageCtrls[page].push_back(h); }

void ShowPage(int page) {
    g_page = page;
    for (int i = 0; i < PG_COUNT; ++i)
        for (HWND h : g_pageCtrls[i]) ShowWindow(h, i == page ? SW_SHOW : SW_HIDE);
    ShowWindow(g_preview, page == PG_CROP ? SW_SHOW : SW_HIDE);
    if (g_lib) ShowWindow(g_lib, page == PG_PLAYBACK ? SW_SHOW : SW_HIDE);
    if (g_nav) InvalidateRect(g_nav, nullptr, FALSE);
    widgets::SetText(g_wnd, kIdTitle, T(static_cast<StrId>(S_TAB_CONTENT + page)));
    InvalidateRect(g_wnd, nullptr, TRUE);
    SyncControls();
}

void BuildControls() {
    using namespace widgets;
    for (auto& v : g_pageCtrls) v.clear();

    // 固定件：页面标题、底部状态、关闭
    AddTitle(g_wnd, kIdTitle, S_TAB_CONTENT, kColX, 24, 400, 40);
    AddLabel(g_wnd, kIdState, S_STATE_PLAYING, kColX, kWinH - 44, 380, 22, true);
    AddButton(g_wnd, kIdClose, S_CLOSE, kWinW - kPad - 96, kWinH - 52, 96, 32);

    // —— 内容 ——
    {
        const int p = PG_CONTENT;
        int y = kBodyY;
        Reg(p, AddLabel (g_wnd, kIdVideoCap,  S_VIDEO_CURRENT, kColX, y, kColW, 22, false, true)); y += 26;
        // 文件名一行、完整路径一行 —— 照 mac 版的样子，同名文件放在不同目录时
        // 只看文件名分不出来是哪一个
        Reg(p, AddLabel (g_wnd, kIdVideoName, S_VIDEO_NONE,    kColX, y, 640,   22));              y += 24;
        Reg(p, AddPath  (g_wnd, kIdVideoPath,                  kColX, y, 640,   20));              y += 32;
        Reg(p, AddButton(g_wnd, kIdPick,      S_VIDEO_PICK,    kColX, y, 140,   32));              y += 52;

        Reg(p, AddCheck(g_wnd, kIdFill,      S_FILL,      kColX, y, kColW + 120)); y += 36;
        Reg(p, AddCheck(g_wnd, kIdAutostart, S_AUTOSTART, kColX, y, kColW + 120));
    }

    // —— 取景 ——
    // 预览摆上面占满一行、滑杆排下面。原来预览挤在左边只有 300 宽，
    // 横屏视频 fit 进去后上下留一大片黑，取景框反而小得不好拖。
    {
        const int p = PG_CROP;
        const int sx = kColX;
        int y = kCropSlidersY;
        Reg(p, AddSlider(g_wnd, kIdZoom,   S_CROP_ZOOM, sx, y, 420, 1.0, 3.0, 1.0, F_PERCENT)); y += 48;
        Reg(p, AddSlider(g_wnd, kIdFocusX, S_CROP_X,    sx, y, 420, 0.0, 1.0, 0.5, F_PERCENT)); y += 48;
        Reg(p, AddSlider(g_wnd, kIdFocusY, S_CROP_Y,    sx, y, 420, 0.0, 1.0, 0.5, F_PERCENT)); y += 54;
        Reg(p, AddButton(g_wnd, kIdCropReset, S_CROP_RESET, sx, y, 190, 32));
        Reg(p, AddLabel (g_wnd, kIdCropHint,  S_CROP_HINT,  sx + 210, y + 6, 380, 40, true));
    }

    // —— 画面：两列。左列影调，右列色彩 + 效果 ——
    {
        const int p = PG_IMAGE;
        int y = kBodyY;
        Reg(p, AddLabel(g_wnd, kIdHeadTone, S_TONE, kColX, y, kColW, 22, false, true));
        y += 28;
        for (int i = 0; i < kToneN; ++i) {
            const FxRow& r = kFxRows[i];
            Reg(p, AddSlider(g_wnd, r.id, r.label, kColX, y, kColW, r.lo, r.hi, r.def, r.fmt));
            y += 46;
        }

        int y2 = kBodyY;
        Reg(p, AddLabel(g_wnd, kIdHeadColor, S_COLOR, kCol2X, y2, kColW, 22, false, true));
        y2 += 28;
        for (int i = kToneN; i < kToneN + kColorN; ++i) {
            const FxRow& r = kFxRows[i];
            Reg(p, AddSlider(g_wnd, r.id, r.label, kCol2X, y2, kColW, r.lo, r.hi, r.def, r.fmt));
            y2 += 46;
        }
        Reg(p, AddLabel(g_wnd, kIdHintVibrance, S_HINT_VIBRANCE, kCol2X, y2, kColW, 42, true));
        y2 += 46;
        Reg(p, AddLabel(g_wnd, kIdHeadEffect, S_EFFECT, kCol2X, y2, kColW, 22, false, true));
        y2 += 28;
        for (int i = kToneN + kColorN; i < kFxCount; ++i) {
            const FxRow& r = kFxRows[i];
            Reg(p, AddSlider(g_wnd, r.id, r.label, kCol2X, y2, kColW, r.lo, r.hi, r.def, r.fmt));
            y2 += 46;
        }
        Reg(p, AddLabel(g_wnd, kIdHintDim, S_HINT_DIM, kCol2X, y2, kColW, 44, true));

        // 「全部恢复默认」放左列下方 —— 左列到影调结束还剩一大块空
        Reg(p, AddButton(g_wnd, kIdFxReset, S_IMG_RESET, kColX, y + 16, 190, 32));
    }

    // —— 播放：视频库 + 自动轮播 ——
    {
        const int p = PG_PLAYBACK;
        // 网格本身是独立窗口类，在 Show() 里建，这里只排它周围的东西
        int y = kBodyY + kLibH + 12;
        Reg(p, AddButton(g_wnd, kIdLibAdd,    S_ADD_VIDEOS, kColX,       y, 150, 32));
        Reg(p, AddButton(g_wnd, kIdLibClear,  S_CLEAR,      kColX + 162, y, 96,  32));
        Reg(p, AddLabel (g_wnd, kIdLibHint,   S_LIB_HINT,   kColX + 274, y + 7, 380, 20, true));
        y += 46;

        Reg(p, AddCheck (g_wnd, kIdAuto,      S_AUTOPLAY,   kColX, y, 300));
        Reg(p, AddCheck (g_wnd, kIdShuffle,   S_SHUFFLE,    kCol2X, y, 300));
        y += 32;
        Reg(p, AddLabel (g_wnd, kIdLblAdvance, S_ADVANCE,   kColX, y + 6, 90, 22));
        Reg(p, AddSeg   (g_wnd, kIdAdvance, S_ADV_END, S_ADV_INTERVAL, kColX + 96, y, 220, 32));
        Reg(p, AddSlider(g_wnd, kIdInterval, S_INTERVAL, kCol2X, y - 8, kColW, 1, 240, 30, F_MINUTES));
    }

    // —— 日程 ——
    {
        const int p = PG_SCHEDULE;
        int y = kBodyY;
        Reg(p, AddCheck(g_wnd, kIdSchedOn, S_SCHEDULE_ON, kColX, y, 460)); y += 34;
        Reg(p, AddLabel(g_wnd, kIdSchedHint, S_SCHEDULE_HINT, kColX + 25, y, 560, 20, true)); y += 40;

        Reg(p, AddLabel (g_wnd, kIdLblDay,   S_DAY_VIDEO, kColX, y + 8, 70, 22, false, true));
        Reg(p, AddButton(g_wnd, kIdDayPick,  S_PICK_SHORT, kColX + 76, y, 80, 32));
        Reg(p, AddPath  (g_wnd, kIdDayPath,  kColX + 168, y + 6, 470, 20)); y += 42;
        Reg(p, AddSlider(g_wnd, kIdDayStart, S_DAY_START, kColX, y, 420, 0, 1439, 7 * 60, F_CLOCK));
        y += 60;

        Reg(p, AddLabel (g_wnd, kIdLblNight,   S_NIGHT_VIDEO, kColX, y + 8, 70, 22, false, true));
        Reg(p, AddButton(g_wnd, kIdNightPick,  S_PICK_SHORT, kColX + 76, y, 80, 32));
        Reg(p, AddPath  (g_wnd, kIdNightPath,  kColX + 168, y + 6, 470, 20)); y += 42;
        Reg(p, AddSlider(g_wnd, kIdNightStart, S_NIGHT_START, kColX, y, 420, 0, 1439, 19 * 60, F_CLOCK));
    }

    // —— 声音 ——
    {
        const int p = PG_AUDIO;
        int y = kBodyY;
        Reg(p, AddCheck (g_wnd, kIdMute2,  S_MUTE,   kColX, y, 300)); y += 38;
        Reg(p, AddSlider(g_wnd, kIdVolume, S_VOLUME, kColX, y, 420, 0.0, 1.0, 0.5, F_PERCENT)); y += 54;
        Reg(p, AddLabel (g_wnd, kIdAudioHint, S_AUDIO_HINT, kColX, y, 560, 40, true));
    }

    // —— 电源 ——
    {
        const int p = PG_POWER;
        int y = kBodyY;
        Reg(p, AddLabel(g_wnd, kIdHeadPower, S_POWER, kColX, y, kColW, 22, false, true)); y += 32;
        Reg(p, AddCheck(g_wnd, kIdPwCovered, S_COVERED,      kColX, y, 460)); y += 24;
        Reg(p, AddLabel(g_wnd, kIdCoveredHint, S_COVERED_HINT, kColX + 25, y, 460, 20, true)); y += 26;
        Reg(p, AddCheck(g_wnd, kIdPwLocked,  S_PAUSE_LOCKED,  kColX, y, 460)); y += 32;
        Reg(p, AddCheck(g_wnd, kIdPwBattery, S_PAUSE_BATTERY, kColX, y, 460)); y += 32;
        Reg(p, AddCheck(g_wnd, kIdPwSaver,   S_PAUSE_SAVER,   kColX, y, 460)); y += 40;
        Reg(p, AddLabel(g_wnd, kIdPowerHint, S_POWER_HINT, kColX, y, 560, 44, true));
    }

    // —— 通用 ——
    {
        const int p = PG_GENERAL;
        int y = kBodyY;
        Reg(p, AddLabel(g_wnd, kIdHeadAppearance, S_APPEARANCE, kColX, y, kColW, 22, false, true)); y += 32;
        Reg(p, AddLabel(g_wnd, kIdLblTheme, S_THEME, kColX, y + 6, 110, 22));
        Reg(p, AddSeg  (g_wnd, kIdTheme, S_DARK, S_LIGHT, kColX + 120, y, 200, 32));               y += 46;
        Reg(p, AddLabel(g_wnd, kIdLblLang,  S_LANGUAGE, kColX, y + 6, 110, 22));
        Reg(p, AddSeg  (g_wnd, kIdLang, S_ZH, S_EN, kColX + 120, y, 200, 32));                     y += 62;

        Reg(p, AddLabel (g_wnd, kIdHeadPlayback, S_PLAYBACK, kColX, y, kColW, 22, false, true));    y += 32;
        Reg(p, AddButton(g_wnd, kIdPlayPause, S_PAUSE, kColX, y, 150, 32));
    }
}

void MakeFonts() {
    if (g_font) { DeleteObject(g_font); g_font = nullptr; }
    if (g_fontBold) { DeleteObject(g_fontBold); g_fontBold = nullptr; }
    if (g_fontSmall) { DeleteObject(g_fontSmall); g_fontSmall = nullptr; }
    if (g_fontTitle) { DeleteObject(g_fontTitle); g_fontTitle = nullptr; }

    // ⚠️ 必须用 SystemParametersInfoForDpi，不能用 SystemParametersInfoW 再自己乘 dpi/96。
    //    这个进程是 per-monitor-v2 感知的，后者返回的 lfMessageFont **已经是当前 DPI 下的尺寸**，
    //    再乘一遍就是双重缩放 —— 150% 的屏上字大一圈，按钮里的文字被截断。
    NONCLIENTMETRICSW m = { sizeof(m) };
    using PFN = BOOL (WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
    PFN forDpi = nullptr;
    if (HMODULE u = GetModuleHandleW(L"user32.dll"))
        forDpi = reinterpret_cast<PFN>(reinterpret_cast<void*>(
                     GetProcAddress(u, "SystemParametersInfoForDpi")));

    bool got = false;
    if (forDpi) got = forDpi(SPI_GETNONCLIENTMETRICS, sizeof(m), &m, 0, g_dpi) != FALSE;
    if (!got)   got = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(m), &m, 0) != FALSE;

    LOGFONTW lf = m.lfMessageFont;
    if (!got || lf.lfHeight == 0) {
        lf = LOGFONTW{};
        lf.lfHeight = -MulDiv(12, g_dpi, 96);
        lstrcpyW(lf.lfFaceName, L"Segoe UI");
    }
    g_font = CreateFontIndirectW(&lf);

    LOGFONTW lb = lf;
    lb.lfWeight = FW_SEMIBOLD;
    g_fontBold = CreateFontIndirectW(&lb);

    LOGFONTW ls = lf;
    ls.lfHeight = static_cast<LONG>(lf.lfHeight * 0.90);
    g_fontSmall = CreateFontIndirectW(&ls);

    LOGFONTW lt = lf;
    lt.lfHeight = static_cast<LONG>(lf.lfHeight * 1.55);
    lt.lfWeight = FW_SEMIBOLD;
    g_fontTitle = CreateFontIndirectW(&lt);

    widgets::SetFonts(g_font, g_fontBold, g_fontSmall, g_fontTitle);
}

// 标题栏跟着主题走。
//
// ⭐ 只设 DWMWA_USE_IMMERSIVE_DARK_MODE 是不够的：那个属性**只能强制深色**。
//    系统本身处于深色模式时，把它设成 FALSE 并不会得到浅色标题栏 ——
//    DWM 会回落到「跟随系统」，于是还是深色。实测就是这样：深色主题正常，
//    切浅色时标题栏纹丝不动（抖窗口大小、RDW_FRAME 都试过，没用，
//    因为根本不是刷新的问题）。
//
//    Win11 22000+ 的 DWMWA_CAPTION_COLOR / TEXT_COLOR / BORDER_COLOR 才是能
//    指定具体颜色的那组。用它们把标题栏染成和窗口一样的底色，
//    两个主题下都严丝合缝，比系统默认还整齐。
//    Win10 上这三个会返回失败（无害），那里就靠 IMMERSIVE_DARK_MODE 兜底。
void ApplyTitlebarTheme(HWND h) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    using PFN = HRESULT (WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto set = reinterpret_cast<PFN>(reinterpret_cast<void*>(
                   GetProcAddress(dwm, "DwmSetWindowAttribute")));
    if (set) {
        // Win10 兜底：19 是 20H1 之前的编号，20 是正式的。设错的那个会被忽略。
        BOOL on = g.darkMode ? TRUE : FALSE;
        set(h, 19, &on, sizeof(on));
        set(h, 20, &on, sizeof(on));

        const Palette& P = Pal();
        COLORREF cap = P.bg, txt = P.text, bd = P.border;
        set(h, 35 /* DWMWA_CAPTION_COLOR */, &cap, sizeof(cap));
        set(h, 36 /* DWMWA_TEXT_COLOR    */, &txt, sizeof(txt));
        set(h, 34 /* DWMWA_BORDER_COLOR  */, &bd,  sizeof(bd));
    }
    FreeLibrary(dwm);
}

void Restyle() {
    if (!g_wnd) return;
    ApplyTitlebarTheme(g_wnd);
    SetWindowTextW(g_wnd, T(S_TITLE));
    widgets::Restyle(g_wnd);
    if (g_nav)     InvalidateRect(g_nav, nullptr, FALSE);
    if (g_preview) InvalidateRect(g_preview, nullptr, FALSE);
    if (g_lib)     InvalidateRect(g_lib, nullptr, FALSE);
    SyncControls();
}

// ---------------------------------------------------------------- 主窗口

constexpr UINT_PTR kTimerSaveFx = 2;   // 拖滑杆时延迟落盘，别每一帧都写注册表

void OnFxChanged(int id) {
    for (const FxRow& r : kFxRows) {
        if (r.id != id) continue;
        g.fx.*(r.field) = static_cast<float>(widgets::GetValue(g_wnd, id));
        ApplyEffectsAll();
        // 暗角范围的可用性跟着暗角强度走
        if (id == kIdFx + 12) widgets::Enable(g_wnd, kIdFx + 13, g.fx.vignette > 0.01f);
        SetTimer(g_wnd, kTimerSaveFx, 600, nullptr);
        return;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH b = CreateSolidBrush(Pal().bg);
        FillRect(reinterpret_cast<HDC>(wp), &rc, b);
        DeleteObject(b);
        return 1;
    }

    case WM_APP + 30:          // 左栏点了某一项
        ShowPage(static_cast<int>(wp));
        return 0;

    case WM_APP + 31: {        // 点了库里的某张缩略图 —— 立刻切过去
        const int i = static_cast<int>(wp);
        if (PlayFromLibrary(i)) {
            SyncControls();
            if (g_lib)     InvalidateRect(g_lib, nullptr, FALSE);
            if (g_preview) InvalidateRect(g_preview, nullptr, FALSE);
        }
        return 0;
    }
    case WM_APP + 32: {        // 右键：从库里移掉这一个（不动磁盘上的文件）
        const int i = static_cast<int>(wp);
        if (i >= 0 && i < static_cast<int>(g.library.size())) {
            thumbs::Forget(g.library[i]);
            g.library.erase(g.library.begin() + i);
            if (g.libIndex == i) g.libIndex = -1;
            else if (g.libIndex > i) --g.libIndex;
            SaveLibrary();
            SyncControls();
            if (g_lib) InvalidateRect(g_lib, nullptr, FALSE);
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == kTimerPoster) {
            if (g_page == PG_CROP && g_preview) {
                RECT rc;
                GetClientRect(g_preview, &rc);
                RefreshPoster(rc.right, rc.bottom);
                InvalidateRect(g_preview, nullptr, FALSE);
            }
            SyncControls();
        } else if (wp == kTimerSaveFx) {
            KillTimer(hwnd, kTimerSaveFx);
            SaveEffects();
            SavePlayback();
        }
        return 0;

    case WM_COMMAND: {
        if (g_syncing) return 0;
        const int id = LOWORD(wp);

        if (id >= kIdFx && id < kIdFx + kFxCount) { OnFxChanged(id); return 0; }

        switch (id) {
        case kIdClose:
            ShowWindow(hwnd, SW_HIDE);
            return 0;

        case kIdPick: {
            const std::wstring path = PickVideoDialog(hwnd);
            if (!path.empty()) LoadVideo(path);
            SyncControls();
            return 0;
        }
        case kIdFill:
            g.fillMode = widgets::GetCheck(hwnd, kIdFill);
            RegWriteString(L"Fill", g.fillMode ? L"1" : L"0");
            ApplyScaleModeAll();
            SyncControls();
            if (g_preview) InvalidateRect(g_preview, nullptr, FALSE);
            return 0;
        case kIdMute2:
            g.muted = widgets::GetCheck(hwnd, kIdMute2);
            RegWriteString(L"Muted", g.muted ? L"1" : L"0");
            ApplyMute();
            SyncControls();
            return 0;
        case kIdVolume:
            g.volume = widgets::GetValue(hwnd, kIdVolume);
            ApplyVolume();
            SetTimer(hwnd, kTimerSaveFx, 600, nullptr);   // 和调色共用延迟落盘
            return 0;

        case kIdPwCovered:
            g.pauseCovered = widgets::GetCheck(hwnd, kIdPwCovered);
            RegWriteString(L"PauseWhenCovered", g.pauseCovered ? L"1" : L"0");
            ApplyPlaybackState();
            return 0;
        case kIdPwLocked:
        case kIdPwBattery:
        case kIdPwSaver:
            g.pauseLocked  = widgets::GetCheck(hwnd, kIdPwLocked);
            g.pauseBattery = widgets::GetCheck(hwnd, kIdPwBattery);
            g.pauseSaver   = widgets::GetCheck(hwnd, kIdPwSaver);
            SavePlayback();
            ApplyPlaybackState();
            return 0;

        // —— 视频库 ——
        case kIdLibAdd: {
            const std::vector<std::wstring> picked = PickVideosDialog(hwnd);
            for (const auto& one : picked) {
                bool dup = false;
                for (const auto& have : g.library) if (have == one) { dup = true; break; }
                if (!dup) g.library.push_back(one);
            }
            if (!picked.empty()) {
                SaveLibrary();
                // 库刚从空变成非空、而且当前没在放东西：顺手播第一个，
                // 省得他还要再点一次才看得到效果
                if (g.video.empty() && !g.library.empty()) PlayFromLibrary(0);
                SyncControls();
                if (g_lib) InvalidateRect(g_lib, nullptr, FALSE);
            }
            return 0;
        }
        case kIdLibClear:
            g.library.clear();
            g.libIndex = -1;
            SaveLibrary();
            thumbs::Clear();
            SyncControls();
            if (g_lib) InvalidateRect(g_lib, nullptr, FALSE);
            return 0;

        // —— 自动轮播 ——
        case kIdAuto:
            g.playlistAuto = widgets::GetCheck(hwnd, kIdAuto);
            SavePlayback();
            SyncControls();
            return 0;
        case kIdShuffle:
            g.playlistShuffle = widgets::GetCheck(hwnd, kIdShuffle);
            SavePlayback();
            return 0;
        case kIdAdvance:
            g.playlistAdvance = widgets::GetSeg(hwnd, kIdAdvance);
            SavePlayback();
            SyncControls();
            return 0;
        case kIdInterval:
            g.playlistInterval = static_cast<int>(widgets::GetValue(hwnd, kIdInterval) + 0.5);
            SetTimer(hwnd, kTimerSaveFx, 600, nullptr);
            return 0;

        // —— 日程 ——
        case kIdSchedOn:
            g.scheduleEnabled = widgets::GetCheck(hwnd, kIdSchedOn);
            SavePlayback();
            SyncControls();
            return 0;
        case kIdDayPick:
        case kIdNightPick: {
            const std::wstring path = PickVideoDialog(hwnd);
            if (!path.empty()) {
                (id == kIdDayPick ? g.dayVideo : g.nightVideo) = path;
                SavePlayback();
                SyncControls();
            }
            return 0;
        }
        case kIdDayStart:
        case kIdNightStart:
            g.dayStartMin   = static_cast<int>(widgets::GetValue(hwnd, kIdDayStart) + 0.5);
            g.nightStartMin = static_cast<int>(widgets::GetValue(hwnd, kIdNightStart) + 0.5);
            SetTimer(hwnd, kTimerSaveFx, 600, nullptr);
            return 0;
        case kIdAutostart:
            SetAutostart(widgets::GetCheck(hwnd, kIdAutostart));
            SyncControls();
            return 0;

        case kIdZoom:
        case kIdFocusX:
        case kIdFocusY:
            g.zoom   = widgets::GetValue(hwnd, kIdZoom);
            g.focusX = widgets::GetValue(hwnd, kIdFocusX);
            g.focusY = widgets::GetValue(hwnd, kIdFocusY);
            ApplyScaleModeAll();
            SaveFraming();
            if (g_preview) InvalidateRect(g_preview, nullptr, FALSE);
            return 0;
        case kIdCropReset:
            g.focusX = g.focusY = 0.5;
            g.zoom = 1.0;
            ApplyScaleModeAll();
            SaveFraming();
            SyncControls();
            if (g_preview) InvalidateRect(g_preview, nullptr, FALSE);
            return 0;

        case kIdFxReset:
            g.fx.Reset();
            ApplyEffectsAll();
            SaveEffects();
            SyncControls();
            return 0;

        case kIdTheme:
            g.darkMode = widgets::GetSeg(hwnd, kIdTheme) == 0;
            RegWriteString(L"DarkMode", g.darkMode ? L"1" : L"0");
            Restyle();
            return 0;
        case kIdLang:
            g.english = widgets::GetSeg(hwnd, kIdLang) == 1;
            RegWriteString(L"Lang", g.english ? L"en" : L"zh");
            Restyle();
            return 0;
        case kIdPlayPause:
            g.userPaused = !g.userPaused;
            ApplyPlaybackState();
            UpdateStateTitle();
            SyncControls();
            return 0;
        }
        return 0;
    }

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        widgets::SetDpi(g_dpi);
        MakeFonts();
        // 控件位置全是按 S() 算的，DPI 变了只能整个重建一遍
        for (auto& v : g_pageCtrls) { for (HWND h : v) DestroyWindow(h); v.clear(); }
        BuildControls();
        const RECT* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_nav) SetWindowPos(g_nav, nullptr, 0, 0, S(kNavW), S(kWinH), SWP_NOZORDER);
        if (g_preview) SetWindowPos(g_preview, nullptr, S(kColX), S(kBodyY),
                                    S(kPrevW), S(kPrevH), SWP_NOZORDER);
        if (g_lib) SetWindowPos(g_lib, nullptr, S(kColX), S(kBodyY),
                                S(kColW * 2 + 36), S(kLibH), SWP_NOZORDER);
        ShowPage(g_page);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, kTimerPoster);
        g_wnd = nullptr;
        g_lib = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

constexpr wchar_t kPreviewClass[] = L"StarPaperPreview";

} // namespace

void Register(HINSTANCE inst) {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.lpszClassName = kClass;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm       = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);

    WNDCLASSEXW pc = { sizeof(pc) };
    pc.lpfnWndProc   = PreviewProc;
    pc.hInstance     = inst;
    pc.lpszClassName = kPreviewClass;
    pc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&pc);

    WNDCLASSEXW lc = { sizeof(lc) };
    lc.lpfnWndProc   = LibProc;
    lc.hInstance     = inst;
    lc.lpszClassName = L"StarPaperLib";
    lc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&lc);

    WNDCLASSEXW nc = { sizeof(nc) };
    nc.lpfnWndProc   = NavProc;
    nc.hInstance     = inst;
    nc.lpszClassName = L"StarPaperNav";
    nc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&nc);

    widgets::Register(inst);
    thumbs::Init();
}

void Show() {
    if (g_wnd) {
        ShowWindow(g_wnd, SW_SHOW);
        SetForegroundWindow(g_wnd);
        SyncControls();
        return;
    }

    g_wnd = CreateWindowExW(0, kClass, T(S_TITLE),
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
                            nullptr, nullptr, g.inst, nullptr);
    if (!g_wnd) return;

    using PFNDpi = UINT (WINAPI*)(HWND);
    if (HMODULE u = GetModuleHandleW(L"user32.dll")) {
        if (auto f = reinterpret_cast<PFNDpi>(reinterpret_cast<void*>(
                         GetProcAddress(u, "GetDpiForWindow"))))
            g_dpi = static_cast<int>(f(g_wnd));
    }
    if (g_dpi < 96) g_dpi = 96;
    widgets::SetDpi(g_dpi);
    MakeFonts();
    ApplyTitlebarTheme(g_wnd);

    // 客户区要正好放得下 kWinW × kWinH 的逻辑布局
    RECT want = { 0, 0, S(kWinW), S(kWinH) };
    AdjustWindowRectEx(&want, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);
    SetWindowPos(g_wnd, nullptr, 0, 0, want.right - want.left, want.bottom - want.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    g_nav = CreateWindowExW(0, L"StarPaperNav", L"", WS_CHILD | WS_VISIBLE,
                            0, 0, S(kNavW), S(kWinH), g_wnd, nullptr, g.inst, nullptr);

    g_preview = CreateWindowExW(0, kPreviewClass, L"", WS_CHILD,
                                S(kColX), S(kBodyY), S(kPrevW), S(kPrevH),
                                g_wnd,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdPreview)),
                                g.inst, nullptr);

    g_lib = CreateWindowExW(0, L"StarPaperLib", L"", WS_CHILD,
                            S(kColX), S(kBodyY), S(kColW * 2 + 36), S(kLibH),
                            g_wnd,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdLib)),
                            g.inst, nullptr);

    BuildControls();
    SetTimer(g_wnd, kTimerPoster, 900, nullptr);

    // 居中到主屏
    RECT wr;
    GetWindowRect(g_wnd, &wr);
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(g_wnd, nullptr,
                 (sw - (wr.right - wr.left)) / 2, (sh - (wr.bottom - wr.top)) / 2,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    ShowPage(PG_CONTENT);
    ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
}

void Close() { if (g_wnd) DestroyWindow(g_wnd); }

void Refresh() {
    if (g_wnd && IsWindowVisible(g_wnd)) {
        SyncControls();
        if (g_preview) InvalidateRect(g_preview, nullptr, FALSE);
    }
}

HWND Wnd() { return g_wnd; }

bool IsOpen() { return g_wnd && IsWindowVisible(g_wnd); }

void ReloadTheme() {
    if (g_wnd) Restyle();
}

void GoToPage(int page) {
    if (g_wnd && page >= 0 && page < PG_COUNT) ShowPage(page);
}

bool HandleDialogMessage(MSG* msg) {
    if (!g_wnd || !IsWindowVisible(g_wnd)) return false;
    if (msg->hwnd != g_wnd && !IsChild(g_wnd, msg->hwnd)) return false;
    return IsDialogMessageW(g_wnd, msg) != FALSE;
}

} // namespace settings
