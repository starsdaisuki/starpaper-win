#pragma once

// 画面调节参数。范围与 macOS 版 AppSettings 一一对齐 —— 同一套滑杆刻度，
// 两边调出来的手感应该是一样的（不是逐位复刻 Core Image，见 shader 里的注释）。
struct Effects {
    // —— 影调 ——
    float exposure   = 0.0f;    // -2 .. 2   EV，线性空间乘 2^EV
    float brightness = 0.0f;    // -0.6 .. 0.6  显示空间加法偏移
    float contrast   = 1.0f;    // 0.4 .. 2
    float highlights = 1.0f;    // 0 .. 1    越小高光压得越狠
    float shadows    = 0.0f;    // -1 .. 1   越大暗部提得越亮
    float gamma      = 1.0f;    // 0.4 .. 2

    // —— 色彩 ——
    float saturation  = 1.0f;   // 0 .. 2
    float vibrance    = 0.0f;   // -1 .. 1   只提低饱和的部分
    float temperature = 6500.f; // 2500 .. 10000 K，调高＝更暖
    float tint        = 0.0f;   // -100 .. 100，调高＝更品红

    // —— 效果 ——
    float blur           = 0.0f;  // 0 .. 60   以 1080p 为基准的半径，实际按屏高等比放大
    float sharpen        = 0.0f;  // 0 .. 2    只锐化亮度
    float vignette       = 0.0f;  // 0 .. 2
    float vignetteRadius = 1.0f;  // 0.3 .. 3
    float dim            = 0.0f;  // 0 .. 0.9  黑遮罩，乘法压暗

    // 逐像素那一趟要不要跑
    bool ColorActive() const {
        return exposure != 0.0f || brightness != 0.0f || contrast != 1.0f
            || highlights != 1.0f || shadows != 0.0f || gamma != 1.0f
            || saturation != 1.0f || vibrance != 0.0f
            || temperature != 6500.0f || tint != 0.0f
            || vignette > 0.01f || dim > 0.001f;
    }
    bool SharpenActive() const { return sharpen > 0.01f; }
    bool BlurActive()    const { return blur    > 0.01f; }

    // 全默认时整条 shader 管线都不建，帧直接进后台缓冲 —— 和以前一模一样的零开销路径。
    bool Active() const { return ColorActive() || SharpenActive() || BlurActive(); }

    void Reset() { *this = Effects(); }
};
