#include "pipeline.h"
#include "shaders.h"

#include <d3d11.h>
#include <d3dcommon.h>
#include <cmath>
#include <vector>

namespace {

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

using PFN_D3DCompile = HRESULT (WINAPI*)(LPCVOID, SIZE_T, LPCSTR,
                                         const D3D_SHADER_MACRO*, ID3DInclude*,
                                         LPCSTR, LPCSTR, UINT, UINT,
                                         ID3DBlob**, ID3DBlob**);

PFN_D3DCompile LoadCompiler() {
    static PFN_D3DCompile fn = nullptr;
    static bool tried = false;
    if (tried) return fn;
    tried = true;
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!m) m = LoadLibraryW(L"d3dcompiler_43.dll");   // 老系统上的备胎
    if (m) fn = reinterpret_cast<PFN_D3DCompile>(
                    reinterpret_cast<void*>(GetProcAddress(m, "D3DCompile")));
    return fn;
}

// 色温 → RGB。Tanner Helland 的经典近似，够用且不需要查表。
// 返回的是 sRGB 显示值。
void KelvinRGB(double k, double& r, double& g, double& b) {
    const double t = (k < 1000 ? 1000 : (k > 40000 ? 40000 : k)) / 100.0;
    auto cl = [](double v) { return v < 0 ? 0.0 : (v > 255 ? 255.0 : v); };

    r = (t <= 66) ? 255.0 : cl(329.698727446 * std::pow(t - 60, -0.1332047592));
    g = (t <= 66) ? cl(99.4708025861 * std::log(t) - 161.1195681661)
                  : cl(288.1221695283 * std::pow(t - 60, -0.0755148492));
    b = (t >= 66) ? 255.0
                  : (t <= 19 ? 0.0 : cl(138.5177312231 * std::log(t - 10) - 305.0447927307));
    r /= 255.0; g /= 255.0; b /= 255.0;
}

// 白平衡增益（**线性空间**用）。
//
// 语义跟 mac 版的 CITemperatureAndTint 对齐：滑杆值放在「原片白点」那一侧，
// 所以调高＝声称原片偏冷＝校正后更暖，和 Lightroom 的手感一致。
void WhiteBalanceGain(float temperature, float tint, float out[3]) {
    double sr, sg, sb, dr, dg, db;
    KelvinRGB(temperature, sr, sg, sb);   // 原片被声称的白点
    KelvinRGB(6500.0,      dr, dg, db);   // 校正到的目标

    auto lin = [](double v) { return std::pow(v <= 0.0 ? 0.0 : v, 2.2); };
    double g0 = lin(dr) / (lin(sr) + 1e-6);
    double g1 = lin(dg) / (lin(sg) + 1e-6);
    double g2 = lin(db) / (lin(sb) + 1e-6);

    // 色调：正＝往品红（提 R/B、压 G），负＝往绿
    const double t = tint / 100.0 * 0.18;
    g0 *= 1.0 + t; g2 *= 1.0 + t; g1 *= 1.0 - t;

    // 按亮度归一 —— 不做的话调色温会顺带把画面整体调亮或调暗
    const double y = 0.2126 * g0 + 0.7152 * g1 + 0.0722 * g2;
    if (y > 1e-6) { g0 /= y; g1 /= y; g2 /= y; }

    out[0] = static_cast<float>(g0);
    out[1] = static_cast<float>(g1);
    out[2] = static_cast<float>(g2);
}

struct CB {
    float p0[4];   // exposure, brightness, contrast, gamma
    float p1[4];   // highlights, shadows, saturation, vibrance
    float p2[4];   // 白平衡增益 rgb, dim
    float p3[4];   // vignette, vignetteRadius, sharpen, -
    float p4[4];   // texelX, texelY, dirX, dirY
    float p5[4];   // sigma, taps, -, -
};

// 一块中间渲染目标。尺寸不对就整块重建 —— 壁纸的分辨率一天变不了几次。
struct RT {
    ID3D11Texture2D*          tex = nullptr;
    ID3D11RenderTargetView*   rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;

    void Release() { SafeRelease(srv); SafeRelease(rtv); SafeRelease(tex); w = h = 0; }

    bool Ensure(ID3D11Device* dev, int nw, int nh) {
        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;
        if (tex && w == nw && h == nh) return true;
        Release();
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = static_cast<UINT>(nw);
        d.Height = static_cast<UINT>(nh);
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &tex))) return false;
        if (FAILED(dev->CreateRenderTargetView(tex, nullptr, &rtv))) { Release(); return false; }
        if (FAILED(dev->CreateShaderResourceView(tex, nullptr, &srv))) { Release(); return false; }
        w = nw; h = nh;
        return true;
    }
};

} // namespace

// ---------------------------------------------------------------------------

struct FxChain::Impl {
    ID3D11Device*        dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;

    ID3D11VertexShader* vs        = nullptr;
    ID3D11PixelShader*  psColor   = nullptr;
    ID3D11PixelShader*  psSharpen = nullptr;
    ID3D11PixelShader*  psBlur    = nullptr;
    ID3D11PixelShader*  psDown    = nullptr;
    ID3D11SamplerState* samp      = nullptr;
    ID3D11Buffer*       cb        = nullptr;
    ID3D11BlendState*   blend     = nullptr;
    ID3D11RasterizerState* rast   = nullptr;

    RT full[2];    // 全分辨率 ping-pong（调色、锐化）
    RT small_[2];  // 降采样后的 ping-pong（大半径模糊）

    // 输入纹理的 SRV。src 纹理是 player 那边建的，这里只缓存一份视图。
    ID3D11Texture2D*          srcTex = nullptr;
    ID3D11ShaderResourceView* srcSrv = nullptr;

    ID3D11Texture2D*        outTex = nullptr;
    ID3D11RenderTargetView* outRtv = nullptr;

    bool CompileAll();
    void Draw(ID3D11PixelShader* ps, ID3D11ShaderResourceView* in,
              ID3D11RenderTargetView* out, int x, int y, int w, int h, const CB& cb);
};

bool FxChain::Impl::CompileAll() {
    PFN_D3DCompile compile = LoadCompiler();
    if (!compile) return false;

    // D3DCOMPILE_OPTIMIZATION_LEVEL3。写字面量是因为这里**故意不 include d3dcompiler.h** ——
    // 编译器 DLL 是运行时 LoadLibrary 取的，不想在链接期引入依赖。
    const UINT flags = 0x8000;
    auto build = [&](const char* entry, const char* target, ID3DBlob** out) -> bool {
        ID3DBlob* err = nullptr;
        const HRESULT hr = compile(kShaderSource, sizeof(kShaderSource) - 1,
                                   "starpaper.hlsl", nullptr, nullptr,
                                   entry, target, flags, 0, out, &err);
        SafeRelease(err);
        return SUCCEEDED(hr) && *out;
    };

    ID3DBlob* b = nullptr;
    bool ok = true;

    if (ok && build("VSMain", "vs_4_0", &b)) {
        ok = SUCCEEDED(dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs));
        SafeRelease(b);
    } else ok = false;

    struct { const char* entry; ID3D11PixelShader** dst; } list[] = {
        { "PSColor",   &psColor   },
        { "PSSharpen", &psSharpen },
        { "PSBlur",    &psBlur    },
        { "PSDown",    &psDown    },
    };
    for (auto& e : list) {
        if (!ok) break;
        if (build(e.entry, "ps_4_0", &b)) {
            ok = SUCCEEDED(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, e.dst));
            SafeRelease(b);
        } else ok = false;
    }
    if (!ok) return false;

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    // CLAMP 很关键：模糊在画面边缘要往外取样，WRAP 会把对面边缘卷进来，
    // 表现为「模糊之后左右两边渗出对侧的颜色」。
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, &samp))) return false;

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth      = sizeof(CB);
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &cb))) return false;

    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bl, &blend))) return false;

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    if (FAILED(dev->CreateRasterizerState(&rd, &rast))) return false;

    return true;
}

void FxChain::Impl::Draw(ID3D11PixelShader* ps, ID3D11ShaderResourceView* in,
                         ID3D11RenderTargetView* out, int x, int y, int w, int h,
                         const CB& data) {
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &data, sizeof(CB));
        ctx->Unmap(cb, 0);
    }

    // 先把输入槽清空再绑输出 —— ping-pong 时上一趟的输出正是这一趟的输入，
    // 不解绑的话 D3D 会因为「同一资源既读又写」把绑定悄悄丢掉（画面变黑，且没有报错）。
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);

    ctx->OMSetRenderTargets(1, &out, nullptr);

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = static_cast<FLOAT>(x);
    vp.TopLeftY = static_cast<FLOAT>(y);
    vp.Width    = static_cast<FLOAT>(w);
    vp.Height   = static_cast<FLOAT>(h);
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(rast);

    const FLOAT bf[4] = { 0, 0, 0, 0 };
    ctx->OMSetBlendState(blend, bf, 0xffffffff);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &in);
    ctx->PSSetSamplers(0, 1, &samp);
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ctx->Draw(3, 0);

    ctx->PSSetShaderResources(0, 1, &none);
    ID3D11RenderTargetView* noRt = nullptr;
    ctx->OMSetRenderTargets(1, &noRt, nullptr);
}

// ---------------------------------------------------------------------------

bool FxChain::Init(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (ready_) return true;
    if (!dev || !ctx) return false;
    impl_ = new Impl();
    impl_->dev = dev;
    impl_->ctx = ctx;
    if (!impl_->CompileAll()) { Shutdown(); return false; }
    ready_ = true;
    return true;
}

void FxChain::Shutdown() {
    if (!impl_) { ready_ = false; return; }
    impl_->full[0].Release();  impl_->full[1].Release();
    impl_->small_[0].Release(); impl_->small_[1].Release();
    SafeRelease(impl_->srcSrv);
    impl_->srcTex = nullptr;
    SafeRelease(impl_->outRtv);
    impl_->outTex = nullptr;
    SafeRelease(impl_->rast);
    SafeRelease(impl_->blend);
    SafeRelease(impl_->cb);
    SafeRelease(impl_->samp);
    SafeRelease(impl_->psDown);
    SafeRelease(impl_->psBlur);
    SafeRelease(impl_->psSharpen);
    SafeRelease(impl_->psColor);
    SafeRelease(impl_->vs);
    delete impl_;
    impl_  = nullptr;
    ready_ = false;
}

bool FxChain::Render(ID3D11Texture2D* src, int srcW, int srcH,
                     ID3D11Texture2D* outRt, const RECT& dst, const Effects& fx) {
    if (!ready_ || !impl_ || !src || !outRt) return false;
    Impl& I = *impl_;

    const int dw = static_cast<int>(dst.right - dst.left);
    const int dh = static_cast<int>(dst.bottom - dst.top);
    if (dw < 1 || dh < 1 || srcW < 1 || srcH < 1) return false;

    // 视图缓存：纹理换了才重建
    if (I.srcTex != src) {
        SafeRelease(I.srcSrv);
        if (FAILED(I.dev->CreateShaderResourceView(src, nullptr, &I.srcSrv))) return false;
        I.srcTex = src;
    }
    if (I.outTex != outRt) {
        SafeRelease(I.outRtv);
        if (FAILED(I.dev->CreateRenderTargetView(outRt, nullptr, &I.outRtv))) return false;
        I.outTex = outRt;
    }

    // —— 模糊的规模：半径按 1080p 标定，屏越高半径越大，这样不同分辨率观感一致 ——
    float radiusPx = 0.0f;
    int   down     = 1;
    if (fx.BlurActive()) {
        radiusPx = fx.blur * static_cast<float>(dh) / 1080.0f;
        // 半径大到一定程度就先缩小再模糊：模糊本来就丢高频，缩小不影响结果，
        // 但采样数从 O(r) 降到 O(r/down)。不这么做的话 4K 上 60 的半径要 240 次采样。
        down = static_cast<int>(std::ceil(radiusPx / 24.0f));
        if (down < 1) down = 1;
        if (down > 8) down = 8;
    }

    // —— 排出这一帧要跑哪几趟 ——
    enum Op { OP_COLOR, OP_SHARPEN, OP_DOWN, OP_BLUR_H, OP_BLUR_V };
    std::vector<Op> ops;
    if (fx.ColorActive())   ops.push_back(OP_COLOR);
    if (fx.SharpenActive()) ops.push_back(OP_SHARPEN);
    if (fx.BlurActive()) {
        if (down > 1) ops.push_back(OP_DOWN);
        ops.push_back(OP_BLUR_H);
        ops.push_back(OP_BLUR_V);
    }
    if (ops.empty()) return false;

    const int smallW = dw / down < 1 ? 1 : dw / down;
    const int smallH = dh / down < 1 ? 1 : dh / down;

    // 只建真正会被写到的中间纹理。4K 下每块 BGRA 就是 32MB，
    // 「反正先建两块」在只调了个亮度的常见情形下白占 64MB 显存。
    int fullWrites = 0, smallWrites = 0;
    for (size_t i = 0; i + 1 < ops.size(); ++i) {   // 最后一趟直接写后台缓冲，不占中间纹理
        switch (ops[i]) {
        case OP_COLOR:
        case OP_SHARPEN: ++fullWrites; break;
        case OP_DOWN:    ++smallWrites; break;
        case OP_BLUR_H:  (down > 1) ? ++smallWrites : ++fullWrites; break;
        default: break;
        }
    }
    for (int i = 0; i < fullWrites && i < 2; ++i)
        if (!I.full[i].Ensure(I.dev, dw, dh)) return false;
    for (int i = 0; i < smallWrites && i < 2; ++i)
        if (!I.small_[i].Ensure(I.dev, smallW, smallH)) return false;
    if (fullWrites  < 2) I.full[1].Release();
    if (fullWrites  < 1) I.full[0].Release();
    if (smallWrites < 2) I.small_[1].Release();
    if (smallWrites < 1) I.small_[0].Release();

    // 白平衡增益只跟参数有关，一帧算一次就行（就算每帧算也只是几次 pow）
    float wb[3] = { 1.0f, 1.0f, 1.0f };
    if (fx.temperature != 6500.0f || fx.tint != 0.0f)
        WhiteBalanceGain(fx.temperature, fx.tint, wb);

    ID3D11ShaderResourceView* in = I.srcSrv;
    int inW = srcW, inH = srcH;
    int fullSlot = 0, smallSlot = 0;

    for (size_t i = 0; i < ops.size(); ++i) {
        const bool last = (i + 1 == ops.size());
        const Op   op   = ops[i];

        CB c = {};
        c.p0[0] = fx.exposure; c.p0[1] = fx.brightness; c.p0[2] = fx.contrast; c.p0[3] = fx.gamma;
        c.p1[0] = fx.highlights; c.p1[1] = fx.shadows; c.p1[2] = fx.saturation; c.p1[3] = fx.vibrance;
        c.p2[0] = wb[0]; c.p2[1] = wb[1]; c.p2[2] = wb[2]; c.p2[3] = fx.dim;
        c.p3[0] = fx.vignette; c.p3[1] = fx.vignetteRadius; c.p3[2] = fx.sharpen;
        c.p4[0] = 1.0f / static_cast<float>(inW);
        c.p4[1] = 1.0f / static_cast<float>(inH);

        ID3D11PixelShader* ps = I.psDown;

        switch (op) {
        case OP_COLOR:   ps = I.psColor;   break;
        case OP_SHARPEN: ps = I.psSharpen; break;
        case OP_DOWN:
            ps = I.psDown;
            // box filter 的边长：每个采样点盖 2 个 texel，所以采 ceil(down/2) 个就够覆盖
            c.p5[2] = static_cast<float>((down + 1) / 2);
            break;
        case OP_BLUR_H:
        case OP_BLUR_V: {
            ps = I.psBlur;
            c.p4[2] = (op == OP_BLUR_H) ? 1.0f : 0.0f;
            c.p4[3] = (op == OP_BLUR_H) ? 0.0f : 1.0f;
            // 高斯的 sigma 取半径的 1/3：3σ 之外的权重不到 1%，肉眼看不出截断
            const float sigma = (radiusPx / static_cast<float>(down)) / 3.0f;
            c.p5[0] = sigma < 0.3f ? 0.3f : sigma;
            // 一次线性采样顶两个 texel，所以 taps 是「半径 texel 数 / 2」
            int taps = static_cast<int>(std::ceil(c.p5[0] * 3.0f / 2.0f));
            if (taps < 1)  taps = 1;
            if (taps > 24) taps = 24;
            c.p5[1] = static_cast<float>(taps);
            break;
        }
        }

        if (last) {
            // 最后一趟直接画进后台缓冲的目标矩形里 —— 中间不再多一次全屏拷贝
            I.Draw(ps, in, I.outRtv,
                   static_cast<int>(dst.left), static_cast<int>(dst.top), dw, dh, c);
        } else {
            RT* target = nullptr;
            if (op == OP_COLOR || op == OP_SHARPEN) {
                target = &I.full[fullSlot];
                fullSlot ^= 1;
            } else if (op == OP_DOWN) {
                target = &I.small_[smallSlot];
                smallSlot ^= 1;
            } else { // OP_BLUR_H
                target = (down > 1) ? &I.small_[smallSlot] : &I.full[fullSlot];
                if (down > 1) smallSlot ^= 1; else fullSlot ^= 1;
            }
            if (!target || !target->rtv) return false;
            I.Draw(ps, in, target->rtv, 0, 0, target->w, target->h, c);
            in  = target->srv;
            inW = target->w;
            inH = target->h;
        }
    }
    return true;
}
