#pragma once
#include <windows.h>
#include <string>
#include <vector>

#include "effects.h"

// 视频播放层 —— **frame-server 模式**。
//
// 解码仍然交给 Media Foundation 的 Media Engine（IMFMediaEngine），
// 但呈现由我们自己做：Media Engine 把每一帧拷进我们的 D3D11 纹理，
// 我们再 Present 到窗口的交换链上。
//
// ⭐ 为什么不用它自带的 windowed 模式（给它一个 HWND 让它自己画）：
//    那条路上「目标矩形」的坐标系由 Media Engine 解释，150% DPI 缩放下
//    它和我们的理解对不上，裁剪永远差一截，而且三种假设（物理像素 / 逻辑像素 / NULL）
//    互相矛盾（三种假设都实测过，见 README「填满屏幕是怎么做的」一节）。
//
//    frame-server 模式下：
//      - 源矩形是**归一化坐标 0~1**，本来就与 DPI 无关；
//      - 目标矩形是**我们自己创建的纹理上的像素**，尺寸是我们定的。
//    整条链路里没有任何一处会替我们做 DPI 换算，歧义从根上消失了。
//
//    附带好处：帧经过我们的手，之后加亮度/对比度/模糊只是一个 shader 的事。
//
// 依旧不需要任何运行时：D3D11 / DXGI / MF 都是系统自带的。
class Player {
public:
    Player() = default;
    ~Player();

    // 画面怎么摆。两个矩形分属两个坐标系，别混：
    //   src —— **源视频上的归一化矩形**（0~1）。裁剪就是在这里表达的。
    //   dst —— **交换链后台缓冲上的像素矩形**。填满模式下就是整块 surface。
    struct Layout {
        float srcLeft   = 0.0f;
        float srcTop    = 0.0f;
        float srcRight  = 1.0f;
        float srcBottom = 1.0f;
        RECT  dst{};            // 空矩形 = 整块 surface
    };

    static bool StartupMF();
    static void ShutdownMF();

    bool Open(HWND hwnd, const std::wstring& path);
    void Close();

    void Play();
    void Pause();
    bool IsPaused() const { return paused_; }

    void SetMuted(bool muted);
    void SetVolume(double v);          // 0~1

    // 播放位置（秒）。自动轮播靠**位置回绕**判断「播完一遍了」——
    // 壁纸是 SetLoop(TRUE) 循环播的，永远不会真的触发结束事件。
    double Position() const;
    bool HasMedia() const { return engine_ != nullptr; }

    // 视频原始像素尺寸。只有 Media Engine 报出 LOADEDMETADATA 之后才有效。
    bool GetVideoSize(int& w, int& h) const;

    // 交换链尺寸（物理像素）。窗口大小变了必须调，内部会 ResizeBuffers。
    bool SetSurfaceSize(int w, int h);

    void SetLayout(const Layout& layout);

    // 调色参数。全默认时整条 shader 管线不会被建起来，走的还是
    // 「Media Engine 直接写后台缓冲」那条零开销老路。
    void SetEffects(const Effects& fx);

    // 调色管线有没有成功建起来（d3dcompiler 缺失 / 编译失败时为 false）。
    // 只在真的用到调色之后才有意义 —— 排查用。
    bool FxReady() const;

    // 上一帧有没有真的画出去 —— 排查用，不靠猜。
    bool  Rendered() const;
    DWORD FrameCount() const;

    // 抓当前这一帧的**整幅画面**（不裁剪）到一块 BGRA 缓冲，给设置窗口做预览底图。
    // out 会被调整成 w*h*4 字节，自上而下。
    // 与渲染线程互斥，调用方在 UI 线程上直接调即可。
    bool CapturePoster(int w, int h, std::vector<unsigned char>& out);

private:
    struct Impl;
    Impl* impl_   = nullptr;
    void* engine_ = nullptr;    // IMFMediaEngine*，不想在头文件里拖进 mfmediaengine.h
    bool  paused_ = true;
};
