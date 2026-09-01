#pragma once
#include <windows.h>
#include "effects.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11RenderTargetView;

// 调色渲染链。
//
// 视频帧先被 Media Engine 拷进一块中间纹理，再由这里跑 1~4 趟全屏 shader
// 画到交换链后台缓冲上。参数全默认时**根本不会走到这里**（见 Effects::Active），
// 那条老路仍然是 TransferVideoFrame 直接写后台缓冲，零额外开销。
//
// ⭐ d3dcompiler_47.dll 是运行时 LoadLibrary 拿的，不在链接期依赖。
//    Win8 以后系统自带；万一某个精简系统没有，Init 返回 false，
//    程序照常播放，只是调色不生效 —— 不会启动失败。
class FxChain {
public:
    bool Init(ID3D11Device* dev, ID3D11DeviceContext* ctx);
    void Shutdown();
    bool Ready() const { return ready_; }

    // src   —— 视频帧所在的纹理（必须带 SHADER_RESOURCE 绑定）
    // outRt —— 交换链后台缓冲
    // dst   —— 要画到 outRt 上的哪块像素矩形（letterbox 时不等于整块）
    bool Render(ID3D11Texture2D* src, int srcW, int srcH,
                ID3D11Texture2D* outRt, const RECT& dst,
                const Effects& fx);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool  ready_ = false;
};
