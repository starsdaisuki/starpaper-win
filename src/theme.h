#pragma once
#include <windows.h>

// 配色与文案。设置窗口的所有颜色、所有可见文字都从这里取，
// 别在界面代码里写死 —— 深色/浅色、中文/英文都是运行时切的。

struct Palette {
    COLORREF bg;        // 窗口底色
    COLORREF sidebar;   // 左侧分类栏
    COLORREF card;      // 内容分组的底
    COLORREF text;      // 正文
    COLORREF textDim;   // 次要说明
    COLORREF accent;    // 强调色（滑块、选中项、复选勾）
    COLORREF border;    // 分隔线
    COLORREF track;     // 滑杆轨道的未填充段
    COLORREF hover;     // 悬停底色
    COLORREF sel;       // 左栏选中项底色
};

// 当前配色（跟着 g.darkMode 走）
const Palette& Pal();

// ---------------------------------------------------------------------------
// 文案。枚举与下面 theme.cpp 里的表**必须同序**，末尾有 static_assert 兜着。
enum StrId {
    S_TITLE = 0,
    S_TAB_CONTENT, S_TAB_CROP, S_TAB_IMAGE, S_TAB_PLAYBACK, S_TAB_SCHEDULE, S_TAB_AUDIO, S_TAB_POWER, S_TAB_GENERAL,

    S_VIDEO_CURRENT, S_VIDEO_PICK, S_VIDEO_NONE, S_PREVIEW_NONE, S_PREVIEW_READING,
    S_FILL, S_MUTE, S_AUTOSTART, S_COVERED, S_COVERED_HINT,

    S_CROP_ZOOM, S_CROP_X, S_CROP_Y, S_CROP_RESET, S_CROP_HINT, S_CROP_NEEDFILL,

    S_TONE, S_COLOR, S_EFFECT,
    S_EXPOSURE, S_BRIGHTNESS, S_CONTRAST, S_HIGHLIGHTS, S_SHADOWS, S_GAMMA,
    S_SATURATION, S_VIBRANCE, S_TEMPERATURE, S_TINT,
    S_BLUR, S_SHARPEN, S_VIGNETTE, S_VIGRADIUS, S_DIM,
    S_IMG_RESET, S_HINT_HIGHLIGHTS, S_HINT_VIBRANCE, S_HINT_DIM,

    S_APPEARANCE, S_THEME, S_DARK, S_LIGHT, S_LANGUAGE, S_ZH, S_EN,
    S_PLAYBACK, S_PAUSE, S_RESUME, S_CLOSE,

    S_STATE_PLAYING, S_STATE_PAUSED, S_STATE_COVERED, S_SCREEN, S_SCREENS,

    // 播放列表（视频库）
    S_LIBRARY, S_ADD_VIDEOS, S_REMOVE, S_CLEAR, S_LIB_EMPTY, S_LIB_HINT, S_NOW_PLAYING,
    S_AUTOPLAY, S_SHUFFLE, S_ADVANCE, S_ADV_END, S_ADV_INTERVAL, S_INTERVAL, S_MINUTE_UNIT,
    // 日程
    S_SCHEDULE, S_SCHEDULE_ON, S_DAY_VIDEO, S_NIGHT_VIDEO, S_DAY_START, S_NIGHT_START,
    S_SCHEDULE_HINT, S_PICK_SHORT,
    // 声音
    S_AUDIO, S_VOLUME, S_AUDIO_HINT,
    // 电源
    S_POWER, S_PAUSE_LOCKED, S_PAUSE_BATTERY, S_PAUSE_SAVER, S_POWER_HINT,

    // 托盘菜单、悬停提示、文件对话框、错误框（main.cpp）。
    // ⚠️ 这一组以前是写死的中文 —— 英文系统上界面能切英文、托盘却还是中文。
    S_MENU_SETTINGS, S_MENU_CROP, S_MENU_EXIT, S_MENU_ZOOMIN_FMT, S_MENU_ZOOMOUT,
    S_AUTOSTART_LOCKED,
    S_POS_TL, S_POS_T, S_POS_TR, S_POS_L, S_POS_C, S_POS_R, S_POS_BL, S_POS_B, S_POS_BR,
    S_TIP_LOCKED, S_TIP_PARTIAL,
    S_FILTER_VIDEO, S_FILTER_ALL,
    S_ERR_OPEN, S_ERR_LOAD, S_ERR_MF, S_ERR_WINDOW, S_ERR_WORKERW,

    S_COUNT
};

// 当前语言下的文案（跟着 g.english 走）
const wchar_t* T(StrId id);
