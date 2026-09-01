#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>

#include "player.h"
#include "effects.h"

// 每个显示器一份。
//
// ⭐ 为什么不是"一个窗口铺满所有屏"：那样多显示器下一张画面会被拉伸横跨两块屏，
//    每块各看到一半。投屏（Sunshine 虚拟显示器）、外接屏、RDP 虚拟显示器都会踩到。
struct View {
    HWND           hwnd = nullptr;
    RECT           rect{};          // 虚拟屏幕坐标系里的显示器矩形
    std::wstring   device;          // \\.\DISPLAY1 之类，用来认人（不是认分辨率）
    Player         player;
    bool           coveredPause = false;
    Player::Layout layout{};        // 最后一次交给播放层的摆法（排查用）
};

struct App {
    HINSTANCE inst = nullptr;
    HWND      tray = nullptr;
    HWND      host = nullptr;     // 壁纸宿主（WorkerW 或 Progman）
    std::vector<std::unique_ptr<View>> views;
    std::wstring video;
    bool muted        = true;
    bool pauseCovered = false;    // 默认关 —— 恢复播放要重新起播，切回桌面会顿一下
    bool fillMode     = true;     // 默认填满：等比放大到盖住整块屏，多出来的裁掉
    // 取景（只在填满模式下有意义）：
    //   focus 0.0/0.5/1.0 = 贴左上 / 居中 / 贴右下，决定多出来的部分从哪边裁掉
    //   zoom  1.0 = 刚好盖住，再大就是主动放大后多裁一些
    double focusX = 0.5;
    double focusY = 0.5;
    double zoom   = 1.0;
    bool userPaused   = false;
    bool locked       = false;

    Effects fx{};                 // 调色参数（全默认＝不走 shader）
    bool    darkMode  = true;     // 设置窗口的配色，默认深色
    bool    english   = true;     // 安全兜底英文；首次启动仅中文设备自动用简体中文

    // —— 视频库 ——
    // ⭐ 这里和 macOS 版的 playlist 不是一回事。mac 那边是「一串视频自动轮播」，
    //    这里要的是 Wallpaper Engine / Lively 那种**库**：把常用壁纸都导进来，
    //    点一下缩略图就换过去。所以「库」和「自动轮播」是两件独立的事 ——
    //    轮播只是库上面的一个开关，关着的时候库照样能用。
    std::vector<std::wstring> library;
    bool playlistAuto     = false;   // 自动轮播
    bool playlistShuffle  = false;
    int  playlistAdvance  = 0;       // 0 = 播完一遍就换，1 = 定时换
    int  playlistInterval = 30;      // 分钟
    int  libIndex         = -1;      // 当前视频在库里的下标，不在库里就是 -1

    // —— 日程 ——
    bool         scheduleEnabled = false;
    std::wstring dayVideo, nightVideo;
    int          dayStartMin   = 7 * 60;    // 07:00
    int          nightStartMin = 19 * 60;   // 19:00

    // —— 声音 ——
    double volume = 0.5;

    // —— 什么时候暂停 ——
    bool pauseLocked  = true;
    bool pauseBattery = false;
    bool pauseSaver   = true;    // 系统节电模式打开时
};

extern App g;

// —— main.cpp 提供，设置窗口要用 ——
std::wstring RegReadString(const wchar_t* name);
void         RegWriteString(const wchar_t* name, const std::wstring& value);
bool         IsAutostart();
void         SetAutostart(bool on);
void         SaveFraming();
void         SaveEffects();
void         SaveLibrary();
void         SavePlayback();     // 轮播 / 日程 / 声音 / 电源那几项
void         ApplyVolume();
bool         PlayFromLibrary(int index);   // 点缩略图切过去
void         AdvanceLibrary(bool forward); // 轮播推进（考虑随机）
void         ApplyEffectsAll();       // 把 g.fx 推给所有 view，桌面立刻反映
void         ApplyScaleModeAll();
void         ApplyPlaybackState();
void         ApplyMute();            // 把 g.muted 应用到所有 view（只有主屏可能出声）
bool         LoadVideo(const std::wstring& path);
std::wstring PickVideoDialog(HWND owner);
std::vector<std::wstring> PickVideosDialog(HWND owner);   // 多选，往库里加
void         UpdateStateTitle();

// 取景框在**源画面**上的归一化矩形。设置窗口画预览框、main.cpp 算裁剪，用的是同一个函数
// —— 预览和桌面因此不可能对不上。
void FramingRect(int videoW, int videoH, int screenW, int screenH,
                 double focusX, double focusY, double zoom,
                 double& left, double& top, double& right, double& bottom);
