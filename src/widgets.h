#pragma once
#include <windows.h>
#include <string>
#include "theme.h"

// 自绘控件。
//
// ⭐ 为什么不用系统原生控件：这个窗口要能切深色。
//    comctl32 的 trackbar 在深色下没法看（轨道和滑块都由主题绘制，
//    SetWindowTheme(L"DarkMode_Explorer") 对它基本不起作用），
//    BS_AUTOCHECKBOX 的勾选框同理。与其和主题引擎搏斗，不如自己画 ——
//    顺带还能把「标签 + 轨道 + 数值」压进一行，14 个滑杆才排得下。
namespace widgets {

enum Kind { W_LABEL, W_SLIDER, W_CHECK, W_BUTTON, W_SEG };

// 数值怎么显示。跟 mac 版 LabeledSlider 的 format 闭包一一对应。
enum Fmt {
    F_PLAIN2,      // 1.25
    F_SIGNED2,     // +0.30 / -0.30
    F_INT,         // 40
    F_SIGNED_INT,  // +80
    F_KELVIN,      // 6500K
    F_PERCENT,     // 60%   （value 是 0~1）
    F_EV,          // +1.50 EV
    F_CLOCK,       // 07:30  （value 是「当天第几分钟」）
    F_MINUTES,     // 30 分钟 / 30 min
};

void Register(HINSTANCE inst);
void SetDpi(int dpi);
void SetFonts(HFONT normal, HFONT bold, HFONT small, HFONT title);

HWND AddLabel (HWND parent, int id, StrId s, int x, int y, int w, int h, bool dim = false, bool bold = false);
HWND AddTitle (HWND parent, int id, StrId s, int x, int y, int w, int h);   // 页面大标题
// 路径专用：单行、中间省略（DT_PATH_ELLIPSIS 会保住文件名那一头）
HWND AddPath  (HWND parent, int id, int x, int y, int w, int h);
HWND AddSlider(HWND parent, int id, StrId s, int x, int y, int w,
               double lo, double hi, double def, Fmt fmt);
HWND AddCheck (HWND parent, int id, StrId s, int x, int y, int w);
HWND AddButton(HWND parent, int id, StrId s, int x, int y, int w, int h);
HWND AddSeg   (HWND parent, int id, StrId a, StrId b, int x, int y, int w, int h);

double GetValue(HWND parent, int id);
void   SetValue(HWND parent, int id, double v);   // 不发通知
bool   GetCheck(HWND parent, int id);
void   SetCheck(HWND parent, int id, bool on);
int    GetSeg  (HWND parent, int id);
void   SetSeg  (HWND parent, int id, int index);

// 覆盖显示文本（视频文件名这种动态内容）。传空串则回到 T(StrId)。
void SetText(HWND parent, int id, const std::wstring& s);
void Enable (HWND parent, int id, bool on);

// 语言或配色变了：全部重画。文案是每次绘制现取 T()，所以不用逐个改文字。
void Restyle(HWND parent);

// 滑杆值变化时父窗口收到的通知码（WM_COMMAND 的 HIWORD）
constexpr WORD kValueChanged = 0x7001;

} // namespace widgets
