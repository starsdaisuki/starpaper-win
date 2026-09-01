#pragma once

// 调色管线的 HLSL。运行时用 D3DCompile 编译（d3dcompiler_47.dll 是系统自带的，
// Win7 以后都在），所以不需要 fxc、也不用把字节码塞进仓库。
//
// ⭐ 说明白：这不是逐位复刻 Core Image。
//    mac 版用 CIFilter，滤镜内部实现是 Apple 的私有算法（尤其 CIHighlightShadowAdjust
//    是局部的、CIVignette 的 radius 语义没有公开公式）。这里做的是**手感对齐**：
//    滑杆范围、默认值、"往右是变亮还是变暖"全部与 mac 版一致，
//    观感接近但像素值不会完全相同。两台机器并排比会看得出细微差别。
//
// 三个 pixel shader，按需组合成 1~4 趟：
//    PSColor    逐像素：曝光 / 高光阴影 / gamma / 白平衡 / 饱和 / 自然饱和 / 亮度对比 / 暗角 / 压暗
//    PSSharpen  3x3 unsharp mask，只作用在亮度上
//    PSBlur     一维高斯，横竖各一趟；半径大时先降采样（见 player.cpp 里的 pass 编排）
static const char kShaderSource[] = R"HLSL(

Texture2D    tex : register(t0);
SamplerState smp : register(s0);

cbuffer CB : register(b0) {
    float4 p0;   // exposure, brightness, contrast, gamma
    float4 p1;   // highlights, shadows, saturation, vibrance
    float4 p2;   // 白平衡增益 rgb, dim
    float4 p3;   // vignette, vignetteRadius, sharpen, (未用)
    float4 p4;   // texelX, texelY, dirX, dirY   —— 模糊/锐化用
    float4 p5;   // sigma, taps, (未用), (未用)
};

#define EXPOSURE    p0.x
#define BRIGHTNESS  p0.y
#define CONTRAST    p0.z
#define GAMMA       p0.w
#define HIGHLIGHTS  p1.x
#define SHADOWS     p1.y
#define SATURATION  p1.z
#define VIBRANCE    p1.w
#define WBGAIN      p2.rgb
#define DIM         p2.w
#define VIGNETTE    p3.x
#define VIGRADIUS   p3.y
#define SHARPEN     p3.z
#define TEXEL       p4.xy
#define BLURDIR     p4.zw
#define SIGMA       p5.x
#define TAPS        p5.y
#define DOWNTAPS    p5.z

static const float3 LUMA = float3(0.2126, 0.7152, 0.0722);

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

// 全屏三角形，不需要顶点缓冲 —— 三个顶点由 SV_VertexID 现算。
// 用一个盖住屏幕的大三角形而不是两个三角形拼的方块：对角线上没有接缝，
// 也少一次光栅化的边界处理。
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float3 SrgbToLinear(float3 c) {
    c = max(c, 0.0);
    return lerp(c / 12.92, pow((c + 0.055) / 1.055, 2.4), step(0.04045, c));
}
float3 LinearToSrgb(float3 c) {
    c = max(c, 0.0);
    return lerp(c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055, step(0.0031308, c));
}

float4 PSColor(VSOut i) : SV_Target {
    float3 s   = tex.Sample(smp, i.uv).rgb;
    float3 lin = SrgbToLinear(s);

    // 曝光在线性空间做乘法 —— 这才是「多进一档光」的物理含义，
    // 在显示空间加常数那是 brightness 干的事（下面还有）。
    lin *= exp2(EXPOSURE);

    float l = dot(lin, LUMA);

    // 高光压制：只动亮的那一头，0.35 以下完全不碰，权重平方过渡免得出硬边
    if (HIGHLIGHTS < 0.999) {
        float w = saturate((l - 0.35) / 0.65);
        lin *= lerp(1.0, HIGHLIGHTS, w * w);
    }
    // 暗部提亮：对暗区局部套一条 gamma，比整体加亮少发灰
    if (abs(SHADOWS) > 0.001) {
        float w = saturate(1.0 - l * 2.0);
        float p = 1.0 / (1.0 + SHADOWS * 0.8);
        lin = lerp(lin, pow(max(lin, 1e-5), p), w * w);
    }
    if (abs(GAMMA - 1.0) > 0.001) lin = pow(max(lin, 1e-5), GAMMA);

    // 白平衡：增益在 CPU 侧按色温算好（见 player.cpp WhiteBalanceGain），
    // 这里只剩一次乘法。线性空间乘增益才不会在暗部偏色。
    lin *= WBGAIN;

    float3 c = LinearToSrgb(lin);

    // 下面几项刻意放在显示空间：滑杆是给眼睛调的，
    // 在线性空间做 contrast 会让中间调跑得跟预期不一样。
    if (abs(SATURATION - 1.0) > 0.001) {
        float y = dot(c, LUMA);
        c = lerp(float3(y, y, y), c, SATURATION);
    }

    // 自然饱和度：已经很艳的地方少动，灰的地方多提 —— 所以按 (1 - 现有饱和度) 加权
    if (abs(VIBRANCE) > 0.001) {
        float mx  = max(c.r, max(c.g, c.b));
        float mn  = min(c.r, min(c.g, c.b));
        float sat = saturate(mx - mn);
        float y   = dot(c, LUMA);
        c = lerp(float3(y, y, y), c, 1.0 + VIBRANCE * (1.0 - sat));
    }

    c += BRIGHTNESS;
    if (abs(CONTRAST - 1.0) > 0.001) c = (c - 0.5) * CONTRAST + 0.5;

    // 暗角：uv 空间的正圆（角落 d = 1.414）。不按宽高比校正是有意的 ——
    // 校正后在超宽屏上左右两边会几乎没有暗角，看着像只压了上下。
    if (VIGNETTE > 0.01) {
        float d = length((i.uv - 0.5) * 2.0);
        float t = saturate((d - VIGRADIUS * 0.6) / max(VIGRADIUS * 0.9, 0.001));
        c *= 1.0 - saturate(VIGNETTE * 0.5 * t * t);
    }

    // 压暗放最后，且是乘法。加法压暗（brightness 调负）会让画面发灰、
    // 桌面图标反而更难认；乘法保留对比度。
    c *= 1.0 - DIM;

    return float4(saturate(c), 1.0);
}

float4 PSSharpen(VSOut i) : SV_Target {
    float3 c = tex.Sample(smp, i.uv).rgb;

    // 3x3 高斯（1 2 1 / 2 4 2 / 1 2 1）/16
    float3 b = c * 4.0;
    b += (tex.Sample(smp, i.uv + float2( TEXEL.x, 0)).rgb +
          tex.Sample(smp, i.uv + float2(-TEXEL.x, 0)).rgb +
          tex.Sample(smp, i.uv + float2(0,  TEXEL.y)).rgb +
          tex.Sample(smp, i.uv + float2(0, -TEXEL.y)).rgb) * 2.0;
    b += (tex.Sample(smp, i.uv + float2( TEXEL.x,  TEXEL.y)).rgb +
          tex.Sample(smp, i.uv + float2(-TEXEL.x,  TEXEL.y)).rgb +
          tex.Sample(smp, i.uv + float2( TEXEL.x, -TEXEL.y)).rgb +
          tex.Sample(smp, i.uv + float2(-TEXEL.x, -TEXEL.y)).rgb);
    b /= 16.0;

    // 只加回亮度差：对彩色边缘加整个 rgb 差值会把色边（紫边）也放大
    float add = (dot(c, LUMA) - dot(b, LUMA)) * SHARPEN;
    return float4(saturate(c + add), 1.0);
}

float4 PSBlur(VSOut i) : SV_Target {
    // ⚠️ 不能把这个变量叫 step —— step() 是 HLSL 内建函数，同名局部变量会遮蔽它，
    //    而上面的 SrgbToLinear 正好要用 step()。编译期不一定报错，行为却是错的。
    float2 dv = TEXEL * BLURDIR;
    float  s2 = 2.0 * SIGMA * SIGMA;

    float3 sum  = tex.Sample(smp, i.uv).rgb;
    float  wsum = 1.0;

    // 线性采样技巧：一次双线性采样顶两个 texel。
    // 把第 2k-1 和第 2k 个 texel 的权重合并，采样点落在两者的加权中点上，
    // 采样次数直接减半（GPU 的双线性插值是免费的）。
    int taps = (int)TAPS;
    [loop] for (int k = 1; k <= taps; ++k) {
        float o1 = (float)(k * 2 - 1);
        float o2 = (float)(k * 2);
        float w1 = exp(-o1 * o1 / s2);
        float w2 = exp(-o2 * o2 / s2);
        float w  = w1 + w2;
        float o  = (o1 * w1 + o2 * w2) / max(w, 1e-6);
        sum  += (tex.Sample(smp, i.uv + dv * o).rgb +
                 tex.Sample(smp, i.uv - dv * o).rgb) * w;
        wsum += 2.0 * w;
    }
    return float4(sum / wsum, 1.0);
}

// 降采样。
//
// ⚠️ 不能只做一次 tex.Sample —— 双线性只平均 2x2 个 texel，
//    从 3024 缩到 504（6 倍）等于每 36 个 texel 只看了 4 个，剩下的直接丢。
//    丢掉的高频会混叠回来，实测表现为**模糊画面里一道道很淡的水平条纹**
//    （2026-08-23 在 blur=60 的截图上肉眼可见）。
//
//    这里改成正经的 box filter：采 N×N 个点，每个点靠双线性各自盖住 2×2 texel，
//    N = ceil(降采样倍数 / 2)，正好把整块区域覆盖完，一个 texel 都不漏。
float4 PSDown(VSOut i) : SV_Target {
    int n = (int)DOWNTAPS;
    if (n <= 1) return float4(tex.Sample(smp, i.uv).rgb, 1.0);

    float3 sum = 0.0;
    float  c   = (float)(n - 1) * 0.5;
    [loop] for (int y = 0; y < n; ++y) {
        [loop] for (int x = 0; x < n; ++x) {
            float2 o = (float2((float)x, (float)y) - c) * 2.0 * TEXEL;
            sum += tex.Sample(smp, i.uv + o).rgb;
        }
    }
    return float4(sum / (float)(n * n), 1.0);
}
)HLSL";
