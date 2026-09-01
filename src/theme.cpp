#include "theme.h"
#include "app.h"

namespace {

// 深色：以 #1E1E20 打底，比纯黑柔和，长时间看不刺眼。
// 强调色用偏亮的蓝（#5B9DF9）——深色底上 #0A84FF 会显得发闷。
constexpr Palette kDark = {
    /*bg*/      RGB(0x1E, 0x1E, 0x20),
    /*sidebar*/ RGB(0x25, 0x25, 0x28),
    /*card*/    RGB(0x2A, 0x2A, 0x2E),
    /*text*/    RGB(0xE8, 0xE8, 0xEA),
    /*textDim*/ RGB(0x92, 0x92, 0x99),
    /*accent*/  RGB(0x5B, 0x9D, 0xF9),
    /*border*/  RGB(0x3A, 0x3A, 0x40),
    /*track*/   RGB(0x3E, 0x3E, 0x45),
    /*hover*/   RGB(0x33, 0x33, 0x3A),
    /*sel*/     RGB(0x3A, 0x3A, 0x44),
};

constexpr Palette kLight = {
    /*bg*/      RGB(0xF5, 0xF5, 0xF7),
    /*sidebar*/ RGB(0xEC, 0xEC, 0xEF),
    /*card*/    RGB(0xFF, 0xFF, 0xFF),
    /*text*/    RGB(0x1D, 0x1D, 0x1F),
    /*textDim*/ RGB(0x6E, 0x6E, 0x73),
    /*accent*/  RGB(0x0A, 0x84, 0xFF),
    /*border*/  RGB(0xD6, 0xD6, 0xDB),
    /*track*/   RGB(0xD3, 0xD3, 0xD8),
    /*hover*/   RGB(0xE4, 0xE4, 0xE8),
    /*sel*/     RGB(0xDC, 0xDC, 0xE2),
};

struct Str { const wchar_t* zh; const wchar_t* en; };

const Str kStr[] = {
    { L"StarPaper 设置",              L"StarPaper Settings" },       // S_TITLE
    { L"内容",                        L"Content" },                  // S_TAB_CONTENT
    { L"取景",                        L"Crop" },                     // S_TAB_CROP
    { L"画面",                        L"Image" },                    // S_TAB_IMAGE
    { L"播放",                        L"Playback" },                 // S_TAB_PLAYBACK
    { L"日程",                        L"Schedule" },                 // S_TAB_SCHEDULE
    { L"声音",                        L"Audio" },                    // S_TAB_AUDIO
    { L"电源",                        L"Power" },                    // S_TAB_POWER
    { L"通用",                        L"General" },                  // S_TAB_GENERAL

    { L"当前视频",                    L"Current video" },            // S_VIDEO_CURRENT
    { L"选择视频…",                   L"Choose Video…" },            // S_VIDEO_PICK
    { L"（还没选）",                  L"(none)" },                   // S_VIDEO_NONE
    { L"还没有选视频",                L"No video selected" },        // S_PREVIEW_NONE
    { L"正在读取画面…",               L"Reading a frame…" },          // S_PREVIEW_READING
    { L"填满屏幕（裁掉多余）",        L"Fill screen (crop overflow)" }, // S_FILL
    { L"静音",                        L"Mute" },                     // S_MUTE
    { L"开机启动",                    L"Launch at login" },          // S_AUTOSTART
    { L"被窗口盖住时暂停",            L"Pause when covered" },       // S_COVERED
    { L"省电，但切回桌面会顿一下",    L"Saves power, but resuming stutters" }, // S_COVERED_HINT

    { L"缩放",                        L"Zoom" },                     // S_CROP_ZOOM
    { L"水平",                        L"Horizontal" },               // S_CROP_X
    { L"垂直",                        L"Vertical" },                 // S_CROP_Y
    { L"重置为居中 100%",             L"Reset to center 100%" },     // S_CROP_RESET
    { L"拖动画面上的取景框，或用滚轮缩放",
      L"Drag the box on the preview, or scroll to zoom" },           // S_CROP_HINT
    { L"取景只在「填满屏幕」时有意义",
      L"Cropping only applies in Fill mode" },                       // S_CROP_NEEDFILL

    { L"影调",                        L"Tone" },                     // S_TONE
    { L"色彩",                        L"Color" },                    // S_COLOR
    { L"效果",                        L"Effects" },                  // S_EFFECT
    { L"曝光",                        L"Exposure" },                 // S_EXPOSURE
    { L"亮度",                        L"Brightness" },               // S_BRIGHTNESS
    { L"对比度",                      L"Contrast" },                 // S_CONTRAST
    { L"高光",                        L"Highlights" },               // S_HIGHLIGHTS
    { L"阴影",                        L"Shadows" },                  // S_SHADOWS
    { L"伽马",                        L"Gamma" },                    // S_GAMMA
    { L"饱和度",                      L"Saturation" },               // S_SATURATION
    { L"自然饱和度",                  L"Vibrance" },                 // S_VIBRANCE
    { L"色温",                        L"Temperature" },              // S_TEMPERATURE
    { L"色调",                        L"Tint" },                     // S_TINT
    { L"模糊",                        L"Blur" },                     // S_BLUR
    { L"锐化",                        L"Sharpen" },                  // S_SHARPEN
    { L"暗角",                        L"Vignette" },                 // S_VIGNETTE
    { L"暗角范围",                    L"Vignette Size" },            // S_VIGRADIUS
    { L"压暗",                        L"Dim" },                      // S_DIM
    { L"全部恢复默认",                L"Reset All" },                // S_IMG_RESET
    { L"「高光」只压最亮的部分，暗部不动 —— 某一块太刺眼时用它，不是用对比度",
      L"Highlights only pulls down the brightest areas and leaves shadows alone." }, // S_HINT_HIGHLIGHTS
    { L"「自然饱和度」比饱和度聪明：只提发灰的颜色，本来就艳的不动",
      L"Vibrance boosts muted colors and leaves saturated ones alone." },  // S_HINT_VIBRANCE
    { L"「压暗」是叠一层黑，比调低亮度更好 —— 桌面图标仍然认得清",
      L"Dim overlays black — better than lowering brightness; icons stay readable." }, // S_HINT_DIM

    { L"外观",                        L"Appearance" },               // S_APPEARANCE
    { L"主题",                        L"Theme" },                    // S_THEME
    { L"深色",                        L"Dark" },                     // S_DARK
    { L"浅色",                        L"Light" },                    // S_LIGHT
    { L"语言",                        L"Language" },                 // S_LANGUAGE
    { L"中文",                        L"中文" },                     // S_ZH
    { L"English",                     L"English" },                  // S_EN
    { L"播放",                        L"Playback" },                 // S_PLAYBACK
    { L"暂停",                        L"Pause" },                    // S_PAUSE
    { L"继续播放",                    L"Resume" },                   // S_RESUME
    { L"关闭",                        L"Close" },                    // S_CLOSE

    { L"播放中",                      L"Playing" },                  // S_STATE_PLAYING
    { L"已暂停",                      L"Paused" },                   // S_STATE_PAUSED
    { L"暂停（被盖住）",              L"Paused (covered)" },         // S_STATE_COVERED
    { L"块屏",                        L"screen" },                   // S_SCREEN
    { L"块屏",                        L"screens" },                  // S_SCREENS

    { L"视频库",                      L"Library" },                  // S_LIBRARY
    { L"添加视频…",                   L"Add Videos…" },              // S_ADD_VIDEOS
    { L"移除",                        L"Remove" },                   // S_REMOVE
    { L"清空",                        L"Clear" },                    // S_CLEAR
    { L"库是空的 —— 点「添加视频」把常用的壁纸都放进来",
      L"Library is empty — use “Add Videos” to put your wallpapers here" },   // S_LIB_EMPTY
    { L"点一下缩略图就切换过去；右键移除单个",
      L"Click a thumbnail to switch; right-click to remove one" },   // S_LIB_HINT
    { L"正在播放",                    L"Now playing" },              // S_NOW_PLAYING
    { L"自动轮播",                    L"Auto-advance" },             // S_AUTOPLAY
    { L"随机顺序",                    L"Shuffle" },                  // S_SHUFFLE
    { L"切换时机",                    L"Advance when" },             // S_ADVANCE
    { L"播完一遍",                    L"Video ends" },               // S_ADV_END
    { L"定时",                        L"On a timer" },               // S_ADV_INTERVAL
    { L"间隔",                        L"Interval" },                 // S_INTERVAL
    { L"分钟",                        L"min" },                      // S_MINUTE_UNIT

    { L"日程",                        L"Schedule" },                 // S_SCHEDULE
    { L"按时间自动换壁纸",            L"Switch wallpaper by time of day" },  // S_SCHEDULE_ON
    { L"白天",                        L"Day" },                      // S_DAY_VIDEO
    { L"夜间",                        L"Night" },                    // S_NIGHT_VIDEO
    { L"白天从",                      L"Day starts" },               // S_DAY_START
    { L"夜间从",                      L"Night starts" },             // S_NIGHT_START
    { L"日程开着的时候，它说了算 —— 会盖过自动轮播",
      L"When the schedule is on it wins — it overrides auto-advance" },  // S_SCHEDULE_HINT
    { L"选…",                         L"Pick…" },                    // S_PICK_SHORT

    { L"声音",                        L"Audio" },                    // S_AUDIO
    { L"音量",                        L"Volume" },                   // S_VOLUME
    { L"只有主显示器那一份会出声；壁纸默认静音",
      L"Only the primary screen plays sound; muted by default" },    // S_AUDIO_HINT

    { L"什么时候暂停",                L"Pause when" },               // S_POWER
    { L"锁屏时",                      L"Screen is locked" },         // S_PAUSE_LOCKED
    { L"用电池时",                    L"On battery" },               // S_PAUSE_BATTERY
    { L"开了节电模式时",              L"Battery saver is on" },      // S_PAUSE_SAVER
    { L"暂停时解码整个停掉，几乎不耗电；恢复要重新起播，会顿一下",
      L"Pausing stops decoding entirely; resuming re-starts playback and stutters once" },  // S_POWER_HINT

    { L"设置…",                       L"Settings…" },                // S_MENU_SETTINGS
    { L"取景…",                       L"Crop…" },                    // S_MENU_CROP
    { L"退出",                        L"Quit" },                     // S_MENU_EXIT
    // ⚠️ %% 是给 wsprintfW 的转义，别在这里写成单个 %
    { L"放大 +10%%    （当前 %d%%）",  L"Zoom in +10%%    (now %d%%)" },  // S_MENU_ZOOMIN_FMT
    { L"缩小 -10%",                   L"Zoom out -10%" },            // S_MENU_ZOOMOUT
    { L"开机启动（已在任务管理器中禁用）",
      L"Launch at login (disabled in Task Manager)" },               // S_AUTOSTART_LOCKED

    { L"左上",                        L"Top left" },                 // S_POS_TL
    { L"上",                          L"Top" },                      // S_POS_T
    { L"右上",                        L"Top right" },                // S_POS_TR
    { L"左",                          L"Left" },                     // S_POS_L
    { L"居中",                        L"Center" },                   // S_POS_C
    { L"右",                          L"Right" },                    // S_POS_R
    { L"左下",                        L"Bottom left" },              // S_POS_BL
    { L"下",                          L"Bottom" },                   // S_POS_B
    { L"右下",                        L"Bottom right" },             // S_POS_BR

    { L"锁屏暂停",                    L"Paused (screen locked)" },   // S_TIP_LOCKED
    { L"部分暂停",                    L"Partly paused" },            // S_TIP_PARTIAL

    { L"视频文件",                    L"Video files" },              // S_FILTER_VIDEO
    { L"所有文件",                    L"All files" },                // S_FILTER_ALL

    { L"这个视频打不开。\n换一个视频文件试试。",
      L"Can't open this video.\nTry another video file." },          // S_ERR_OPEN
    { L"加载失败。",                  L"Failed to load." },          // S_ERR_LOAD
    { L"Media Foundation 初始化失败。",
      L"Failed to initialize Media Foundation." },                   // S_ERR_MF
    { L"创建窗口失败。",              L"Failed to create the window." },  // S_ERR_WINDOW
    { L"找不到桌面壁纸层。\n如果你换过第三方 shell，这个机制可能不可用。",
      L"Can't find the desktop wallpaper layer.\n"
      L"This mechanism may be unavailable if you replaced the Windows shell." },  // S_ERR_WORKERW
};

static_assert(sizeof(kStr) / sizeof(kStr[0]) == S_COUNT,
              "文案表和 StrId 枚举对不上了 —— 加枚举时记得同一位置加一行");

} // namespace

const Palette& Pal() { return g.darkMode ? kDark : kLight; }

const wchar_t* T(StrId id) {
    if (id < 0 || id >= S_COUNT) return L"";
    return g.english ? kStr[id].en : kStr[id].zh;
}
