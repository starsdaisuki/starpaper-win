#include "widgets.h"

#include <windowsx.h>
#include <cmath>
#include <cstdio>
#include <vector>

namespace widgets {
namespace {

constexpr wchar_t kClass[] = L"StarPaperW";

int   g_dpi = 96;
HFONT g_fn = nullptr, g_fb = nullptr, g_fs = nullptr, g_ft = nullptr;

int S(int px) { return MulDiv(px, g_dpi, 96); }

struct W {
    Kind   kind = W_LABEL;
    int    id   = 0;
    StrId  s    = S_TITLE;
    std::wstring text;          // 非空则盖过 T(s)

    // 滑杆
    double lo = 0, hi = 1, def = 0, value = 0;
    Fmt    fmt = F_PLAIN2;

    // 复选 / 分段
    bool   checked = false;
    StrId  segB    = S_TITLE;
    int    seg     = 0;

    bool   dim = false, bold = false, title = false, path = false, enabled = true;
    bool   hot = false, pressed = false, dragging = false;
};

W* Of(HWND h) { return reinterpret_cast<W*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

HWND Find(HWND parent, int id) { return GetDlgItem(parent, id); }
W*   FindW(HWND parent, int id) {
    HWND h = Find(parent, id);
    return h ? Of(h) : nullptr;
}

const wchar_t* Label(const W& w) {
    return w.text.empty() ? T(w.s) : w.text.c_str();
}

std::wstring Format(const W& w) {
    wchar_t b[64];
    switch (w.fmt) {
    case F_SIGNED2:    swprintf(b, 64, L"%+.2f", w.value); break;
    case F_INT:        swprintf(b, 64, L"%.0f",  w.value); break;
    case F_SIGNED_INT: swprintf(b, 64, L"%+.0f", w.value); break;
    case F_KELVIN:     swprintf(b, 64, L"%.0fK", w.value); break;
    case F_PERCENT:    swprintf(b, 64, L"%.0f%%", w.value * 100.0); break;
    case F_EV:         swprintf(b, 64, L"%+.2f EV", w.value); break;
    case F_CLOCK: {
        const int m = static_cast<int>(w.value + 0.5);
        swprintf(b, 64, L"%02d:%02d", (m / 60) % 24, m % 60);
        break;
    }
    case F_MINUTES:
        // 单位跟着界面语言走，所以要拼字符串。
        // ⚠️ 不能写成 swprintf(..., L"%.0f %s", v, unit) —— mingw 的宽字符 printf
        //    里 %s 是**窄**字符串，会输出乱码。这个坑今天已经踩过一次了。
        swprintf(b, 64, L"%.0f", w.value);
        return std::wstring(b) + L" " + T(S_MINUTE_UNIT);
    case F_PLAIN2:
    default:           swprintf(b, 64, L"%.2f", w.value); break;
    }
    return b;
}

void Notify(HWND h, const W& w, WORD code) {
    HWND p = GetParent(h);
    if (p) SendMessageW(p, WM_COMMAND, MAKEWPARAM(w.id, code), reinterpret_cast<LPARAM>(h));
}

// 圆角矩形填充。轨道和分段选择都用它 —— GDI 的 RoundRect 要 pen，
// 用 pen 会在边上留一圈；这里直接拿画刷填。
void FillRound(HDC dc, RECT r, int radius, COLORREF c) {
    HBRUSH br = CreateSolidBrush(c);
    HRGN rgn = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, radius * 2, radius * 2);
    FillRgn(dc, rgn, br);
    DeleteObject(rgn);
    DeleteObject(br);
}

void FillCircle(HDC dc, int cx, int cy, int r, COLORREF c) {
    HBRUSH br = CreateSolidBrush(c);
    HRGN rgn = CreateEllipticRgn(cx - r, cy - r, cx + r, cy + r);
    FillRgn(dc, rgn, br);
    DeleteObject(rgn);
    DeleteObject(br);
}

void DrawText_(HDC dc, const wchar_t* s, RECT r, COLORREF color, HFONT f, UINT flags) {
    HFONT old = static_cast<HFONT>(SelectObject(dc, f));
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &r, flags);
    SelectObject(dc, old);
}

// —— 滑杆几何 ——
// 一行 44 逻辑像素：上半行放「标签 … 数值」，下半行放轨道。
constexpr int kSliderH = 46;
constexpr int kKnobR   = 7;

RECT TrackRect(const RECT& c) {
    RECT t = c;
    t.left  += S(kKnobR);
    t.right -= S(kKnobR);
    const int y = c.top + S(32);
    t.top    = y - S(2);
    t.bottom = y + S(2);
    return t;
}

double PosToValue(const W& w, const RECT& c, int x) {
    const RECT t = TrackRect(c);
    const double span = static_cast<double>(t.right - t.left);
    if (span < 1) return w.lo;
    double f = (x - t.left) / span;
    f = f < 0 ? 0 : (f > 1 ? 1 : f);
    return w.lo + f * (w.hi - w.lo);
}

int ValueToPos(const W& w, const RECT& c) {
    const RECT t = TrackRect(c);
    const double f = (w.hi - w.lo) > 1e-9 ? (w.value - w.lo) / (w.hi - w.lo) : 0.0;
    return t.left + static_cast<int>(f * (t.right - t.left) + 0.5);
}

void SetVal(HWND h, W& w, double v, bool notify) {
    if (v < w.lo) v = w.lo;
    if (v > w.hi) v = w.hi;
    if (std::fabs(v - w.value) < 1e-9) return;
    w.value = v;
    InvalidateRect(h, nullptr, FALSE);
    if (notify) Notify(h, w, kValueChanged);
}

// 滚轮/方向键一步走多少。范围大的（色温 2500~10000）走大步，
// 小范围的（亮度 -0.6~0.6）走细步 —— 统一用「整个量程的 1/50」。
double Step(const W& w) {
    const double span = w.hi - w.lo;
    if (w.fmt == F_KELVIN)     return 100.0;
    if (w.fmt == F_SIGNED_INT) return 5.0;
    if (w.fmt == F_INT)        return 1.0;
    return span / 50.0;
}

void Paint(HWND h) {
    W* w = Of(h);
    if (!w) return;
    const Palette& P = Pal();

    RECT c;
    GetClientRect(h, &c);

    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(h, &ps);

    // 双缓冲：滑杆拖动时每帧全画，直接画到屏上会闪
    HDC     mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, c.right, c.bottom);
    HBITMAP ob  = static_cast<HBITMAP>(SelectObject(mem, bmp));

    HBRUSH bg = CreateSolidBrush(P.bg);
    FillRect(mem, &c, bg);
    DeleteObject(bg);

    const COLORREF fg    = w->enabled ? (w->dim ? P.textDim : P.text) : P.textDim;
    const COLORREF accent = w->enabled ? P.accent : P.track;

    switch (w->kind) {
    case W_LABEL: {
        RECT r = c;
        HFONT f = w->title ? g_ft : (w->bold ? g_fb : (w->dim ? g_fs : g_fn));
        const UINT flags = w->path
                         ? (DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX | DT_VCENTER)
                         : (DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        DrawText_(mem, Label(*w), r, fg, f, flags);
        break;
    }

    case W_SLIDER: {
        RECT lr = c;
        lr.bottom = lr.top + S(20);
        DrawText_(mem, Label(*w), lr, fg, g_fn, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
        const std::wstring v = Format(*w);
        DrawText_(mem, v.c_str(), lr, w->enabled ? P.textDim : P.track, g_fn,
                  DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);

        RECT t = TrackRect(c);
        FillRound(mem, t, S(2), P.track);
        const int knob = ValueToPos(*w, c);
        RECT filled = t;
        filled.right = knob;
        if (filled.right > filled.left) FillRound(mem, filled, S(2), accent);

        const int cy = (t.top + t.bottom) / 2;
        // 滑块：底下一圈边框色垫着，白点浮在上面 —— 浅色主题下纯白滑块
        // 压在浅灰轨道上会糊成一片，垫一圈就分得开了
        FillCircle(mem, knob, cy, S(kKnobR), P.border);
        FillCircle(mem, knob, cy, S(kKnobR) - S(1),
                   w->enabled ? (w->hot || w->dragging ? RGB(255,255,255) : RGB(0xF2,0xF2,0xF4))
                              : P.track);
        break;
    }

    case W_CHECK: {
        const int box = S(16);
        const int by  = (c.bottom - box) / 2;
        RECT b = { 0, by, box, by + box };
        if (w->checked) {
            FillRound(mem, b, S(4), accent);
            // 勾：两笔线段，比字体里的 ✓ 在小尺寸下清楚
            HPEN pen = CreatePen(PS_SOLID, S(2), RGB(255, 255, 255));
            HPEN op  = static_cast<HPEN>(SelectObject(mem, pen));
            const int x0 = b.left + S(4), y0 = by + box / 2;
            MoveToEx(mem, x0, y0, nullptr);
            LineTo(mem, x0 + S(3), y0 + S(4));
            LineTo(mem, b.right - S(4), by + S(4));
            SelectObject(mem, op);
            DeleteObject(pen);
        } else {
            FillRound(mem, b, S(4), w->hot ? P.hover : P.track);
            RECT in = { b.left + S(1), b.top + S(1), b.right - S(1), b.bottom - S(1) };
            FillRound(mem, in, S(3), P.bg);
        }
        RECT tr = c;
        tr.left = box + S(9);
        DrawText_(mem, Label(*w), tr, fg, g_fn,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        break;
    }

    case W_BUTTON: {
        COLORREF face = w->pressed ? P.sel : (w->hot ? P.hover : P.card);
        FillRound(mem, c, S(6), P.border);
        RECT in = { c.left + S(1), c.top + S(1), c.right - S(1), c.bottom - S(1) };
        FillRound(mem, in, S(5), face);
        DrawText_(mem, Label(*w), c, fg, g_fn,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        break;
    }

    case W_SEG: {
        FillRound(mem, c, S(7), P.track);
        const int half = (c.right - c.left) / 2;
        RECT act = c;
        if (w->seg == 0) act.right = c.left + half;
        else             act.left  = c.left + half;
        InflateRect(&act, -S(2), -S(2));
        FillRound(mem, act, S(6), accent);

        RECT a = c, b2 = c;
        a.right  = c.left + half;
        b2.left  = c.left + half;
        DrawText_(mem, T(w->s), a, w->seg == 0 ? RGB(255,255,255) : fg, g_fn,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        DrawText_(mem, T(w->segB), b2, w->seg == 1 ? RGB(255,255,255) : fg, g_fn,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        break;
    }
    }

    BitBlt(hdc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    W* w = Of(h);

    switch (msg) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }
    case WM_NCDESTROY:
        delete w;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return 0;

    case WM_ERASEBKGND: return 1;   // 全靠 WM_PAINT 的双缓冲
    case WM_PAINT:      Paint(h); return 0;

    case WM_MOUSEMOVE: {
        if (!w) break;
        if (!w->hot) {
            w->hot = true;
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(h, nullptr, FALSE);
        }
        if (w->dragging && w->enabled) {
            RECT c; GetClientRect(h, &c);
            SetVal(h, *w, PosToValue(*w, c, GET_X_LPARAM(lp)), true);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (w) { w->hot = false; InvalidateRect(h, nullptr, FALSE); }
        return 0;

    case WM_LBUTTONDOWN: {
        if (!w || !w->enabled) return 0;
        SetFocus(h);
        SetCapture(h);
        w->pressed = true;
        if (w->kind == W_SLIDER) {
            w->dragging = true;
            RECT c; GetClientRect(h, &c);
            SetVal(h, *w, PosToValue(*w, c, GET_X_LPARAM(lp)), true);
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!w) return 0;
        const bool was = w->pressed;
        w->pressed = w->dragging = false;
        ReleaseCapture();
        InvalidateRect(h, nullptr, FALSE);
        if (!was || !w->enabled) return 0;

        RECT c; GetClientRect(h, &c);
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (!PtInRect(&c, pt)) return 0;

        if (w->kind == W_CHECK) {
            w->checked = !w->checked;
            InvalidateRect(h, nullptr, FALSE);
            Notify(h, *w, BN_CLICKED);
        } else if (w->kind == W_BUTTON) {
            Notify(h, *w, BN_CLICKED);
        } else if (w->kind == W_SEG) {
            const int seg = (pt.x < (c.right - c.left) / 2) ? 0 : 1;
            if (seg != w->seg) {
                w->seg = seg;
                InvalidateRect(h, nullptr, FALSE);
                Notify(h, *w, kValueChanged);
            }
        }
        return 0;
    }

    // 双击滑杆＝回到默认值。14 个滑杆逐个手动拖回去太折磨人了。
    case WM_LBUTTONDBLCLK:
        if (w && w->kind == W_SLIDER && w->enabled) {
            w->dragging = false;
            SetVal(h, *w, w->def, true);
        }
        return 0;

    case WM_MOUSEWHEEL: {
        if (!w || w->kind != W_SLIDER || !w->enabled) break;
        const int d = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1;
        SetVal(h, *w, w->value + d * Step(*w), true);
        return 0;
    }

    case WM_GETDLGCODE:
        return (w && w->kind == W_SLIDER) ? DLGC_WANTARROWS : 0;

    case WM_KEYDOWN: {
        if (!w || !w->enabled) break;
        if (w->kind == W_SLIDER) {
            if (wp == VK_LEFT  || wp == VK_DOWN) { SetVal(h, *w, w->value - Step(*w), true); return 0; }
            if (wp == VK_RIGHT || wp == VK_UP)   { SetVal(h, *w, w->value + Step(*w), true); return 0; }
            if (wp == VK_HOME) { SetVal(h, *w, w->lo,  true); return 0; }
            if (wp == VK_END)  { SetVal(h, *w, w->hi,  true); return 0; }
        }
        if (wp == VK_SPACE || wp == VK_RETURN) {
            if (w->kind == W_CHECK)  { w->checked = !w->checked; InvalidateRect(h, nullptr, FALSE); Notify(h, *w, BN_CLICKED); return 0; }
            if (w->kind == W_BUTTON) { Notify(h, *w, BN_CLICKED); return 0; }
        }
        break;
    }

    case WM_SETCURSOR:
        if (w && w->enabled && (w->kind != W_LABEL)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HWND Create(HWND parent, W* w, int x, int y, int cw, int ch) {
    return CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                           S(x), S(y), S(cw), S(ch), parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(w->id)),
                           reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE)),
                           w);
}

} // namespace

void Register(HINSTANCE inst) {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = Proc;
    wc.hInstance     = inst;
    wc.lpszClassName = kClass;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.style         = CS_DBLCLKS;   // 不加这个收不到 WM_LBUTTONDBLCLK
    RegisterClassExW(&wc);
}

void SetDpi(int dpi) { g_dpi = dpi < 96 ? 96 : dpi; }
void SetFonts(HFONT n, HFONT b, HFONT s, HFONT t) { g_fn = n; g_fb = b; g_fs = s; g_ft = t; }

HWND AddLabel(HWND parent, int id, StrId s, int x, int y, int w, int h, bool dim, bool bold) {
    W* d = new W();
    d->kind = W_LABEL; d->id = id; d->s = s; d->dim = dim; d->bold = bold;
    return Create(parent, d, x, y, w, h);
}

HWND AddTitle(HWND parent, int id, StrId s, int x, int y, int w, int h) {
    W* d = new W();
    d->kind = W_LABEL; d->id = id; d->s = s; d->title = true;
    return Create(parent, d, x, y, w, h);
}

HWND AddPath(HWND parent, int id, int x, int y, int w, int h) {
    W* d = new W();
    d->kind = W_LABEL; d->id = id; d->s = S_TITLE; d->dim = true; d->path = true;
    d->text = L" ";
    return Create(parent, d, x, y, w, h);
}

HWND AddSlider(HWND parent, int id, StrId s, int x, int y, int w,
               double lo, double hi, double def, Fmt fmt) {
    W* d = new W();
    d->kind = W_SLIDER; d->id = id; d->s = s;
    d->lo = lo; d->hi = hi; d->def = def; d->value = def; d->fmt = fmt;
    return Create(parent, d, x, y, w, kSliderH);
}

HWND AddCheck(HWND parent, int id, StrId s, int x, int y, int w) {
    W* d = new W();
    d->kind = W_CHECK; d->id = id; d->s = s;
    return Create(parent, d, x, y, w, 24);
}

HWND AddButton(HWND parent, int id, StrId s, int x, int y, int w, int h) {
    W* d = new W();
    d->kind = W_BUTTON; d->id = id; d->s = s;
    return Create(parent, d, x, y, w, h);
}

HWND AddSeg(HWND parent, int id, StrId a, StrId b, int x, int y, int w, int h) {
    W* d = new W();
    d->kind = W_SEG; d->id = id; d->s = a; d->segB = b;
    return Create(parent, d, x, y, w, h);
}

double GetValue(HWND p, int id) { W* w = FindW(p, id); return w ? w->value : 0.0; }
void   SetValue(HWND p, int id, double v) {
    HWND h = Find(p, id);
    W*   w = h ? Of(h) : nullptr;
    if (!w) return;
    if (v < w->lo) v = w->lo;
    if (v > w->hi) v = w->hi;
    w->value = v;
    InvalidateRect(h, nullptr, FALSE);
}
bool GetCheck(HWND p, int id) { W* w = FindW(p, id); return w && w->checked; }
void SetCheck(HWND p, int id, bool on) {
    HWND h = Find(p, id);
    if (!h) return;
    if (W* w = Of(h)) { w->checked = on; InvalidateRect(h, nullptr, FALSE); }
}
int  GetSeg(HWND p, int id) { W* w = FindW(p, id); return w ? w->seg : 0; }
void SetSeg(HWND p, int id, int i) {
    HWND h = Find(p, id);
    if (!h) return;
    if (W* w = Of(h)) { w->seg = i; InvalidateRect(h, nullptr, FALSE); }
}

void SetText(HWND p, int id, const std::wstring& s) {
    HWND h = Find(p, id);
    if (!h) return;
    if (W* w = Of(h)) { w->text = s; InvalidateRect(h, nullptr, FALSE); }
}

void Enable(HWND p, int id, bool on) {
    HWND h = Find(p, id);
    if (!h) return;
    if (W* w = Of(h)) { w->enabled = on; InvalidateRect(h, nullptr, FALSE); }
}

void Restyle(HWND parent) {
    // 文案是每次 WM_PAINT 现取 T(s) 的，所以切语言/切配色只要重画
    for (HWND c = GetWindow(parent, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
        InvalidateRect(c, nullptr, FALSE);
    InvalidateRect(parent, nullptr, TRUE);
}

} // namespace widgets
