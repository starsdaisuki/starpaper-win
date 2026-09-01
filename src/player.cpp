#include "player.h"
#include "pipeline.h"

#include <mfapi.h>
#include <mfmediaengine.h>
#include <mferror.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>

namespace {

// Media Engine 是异步的：SetSource() 只是把活派下去，真正能播是等它回调。
// ⚠️ 回调在 Media Engine 自己的线程上跑，不能在这里碰 UI，只能 PostMessage 回主线程。
class EngineNotify : public IMFMediaEngineNotify {
public:
    explicit EngineNotify(HWND owner) : owner_(owner) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IMFMediaEngineNotify)) {
            *ppv = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override  { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG n = InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP EventNotify(DWORD event, DWORD_PTR param1, DWORD) override {
        if (owner_) PostMessageW(owner_, kMediaEvent, (WPARAM)event, (LPARAM)param1);
        return S_OK;
    }

    static constexpr UINT kMediaEvent = WM_APP + 21;

private:
    virtual ~EngineNotify() = default;
    HWND  owner_ = nullptr;
    ULONG ref_   = 1;
};

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

} // namespace

// ---------------------------------------------------------------------------

struct Player::Impl {
    EngineNotify*         notify  = nullptr;
    IMFMediaEngineEx*     ex      = nullptr;   // 借用 engine_ 的引用计数之外的一份
    IMFDXGIDeviceManager* dxgiMgr = nullptr;

    ID3D11Device*        dev  = nullptr;
    ID3D11DeviceContext* ctx  = nullptr;
    IDXGISwapChain1*     swap = nullptr;
    HWND                 hwnd = nullptr;

    // 渲染线程与主线程共享的那点状态，都在这把锁下面
    CRITICAL_SECTION cs{};
    // TransferVideoFrame / 设备上下文的互斥。抓预览帧时不能和渲染线程同时动。
    CRITICAL_SECTION renderCs{};
    Layout layout{};
    int    surfW = 0, surfH = 0;
    bool   pendingResize = false;

    HANDLE thread  = nullptr;
    volatile LONG  running = 0;
    volatile LONG  frames  = 0;
    volatile LONG  renderOk = 0;

    LONGLONG lastPts = -1;      // 只在渲染线程里读写
    bool     force   = false;   // 受 cs 保护：布局/尺寸变了，下一轮无条件重画一次

    Effects  fx{};              // 受 cs 保护
    FxChain  chain;             // 只在渲染线程里碰
    ID3D11Texture2D* stage = nullptr;   // 调色时视频帧先落到这里，再由 shader 画到后台缓冲
    int stageW = 0, stageH = 0;
    bool fxFailed = false;      // 编译失败过就别每帧重试

    bool EnsureStage(int w, int h);

    static DWORD WINAPI ThreadProc(void* self) {
        static_cast<Impl*>(self)->RenderLoop();
        return 0;
    }

    void RenderLoop();
    bool RenderOne();
    bool CreateSwapChain(int w, int h);
};

// 调色用的中间纹理。尺寸跟着目标矩形走 —— 只在窗口尺寸或摆法变化时重建。
bool Player::Impl::EnsureStage(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (stage && stageW == w && stageH == h) return true;
    if (stage) { stage->Release(); stage = nullptr; }
    stageW = stageH = 0;

    D3D11_TEXTURE2D_DESC d = {};
    d.Width  = static_cast<UINT>(w);
    d.Height = static_cast<UINT>(h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    // RENDER_TARGET 是 TransferVideoFrame 的要求，SHADER_RESOURCE 是我们要拿它当输入
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &stage))) return false;
    stageW = w;
    stageH = h;
    return true;
}

// D3D11 设备 + 交换链。
//
// ⚠️ 三个必须的开关，少一个 Media Engine 就不肯往我们的纹理里拷帧：
//    1. D3D11_CREATE_DEVICE_VIDEO_SUPPORT —— 没有它 ResetDevice 直接失败
//    2. D3D11_CREATE_DEVICE_BGRA_SUPPORT  —— 输出格式是 B8G8R8A8
//    3. SetMultithreadProtected(TRUE)     —— Media Engine 会从它自己的线程碰这个 device，
//                                            和我们的渲染线程并发，不加保护会随机崩
bool Player::Impl::CreateSwapChain(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    IDXGIDevice*  dxgiDev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    bool ok = false;

    do {
        if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) break;
        if (FAILED(dxgiDev->GetAdapter(&adapter))) break;
        if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) break;

        DXGI_SWAP_CHAIN_DESC1 d = {};
        d.Width       = static_cast<UINT>(w);
        d.Height      = static_cast<UINT>(h);
        d.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        d.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;
        d.Scaling     = DXGI_SCALING_STRETCH;
        if (FAILED(factory->CreateSwapChainForHwnd(dev, hwnd, &d, nullptr, nullptr, &swap))) break;
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        surfW = w;
        surfH = h;
        ok = true;
    } while (false);

    SafeRelease(factory);
    SafeRelease(adapter);
    SafeRelease(dxgiDev);
    return ok;
}

// 一帧的全部工作。
//
// ⭐ 裁剪在 **src** 上做，不在 dst 上做。
//    旧方案是让目标矩形比窗口大、超出的部分被窗口剪掉 —— 那要求 Media Engine 和我们
//    对目标坐标系的理解完全一致，而事实是不一致（DPI 缩放下对不上）。
//    现在目标永远老老实实待在 surface 里面，要少画哪一块就把源矩形收窄，
//    而源矩形是 0~1 的归一化值，没有任何单位可以被误解。
bool Player::Impl::RenderOne() {
    if (!swap || !ex) return false;

    Layout  L;
    Effects E;
    int sw, sh;
    EnterCriticalSection(&cs);
    L = layout;
    E = fx;
    sw = surfW;
    sh = surfH;
    LeaveCriticalSection(&cs);

    ID3D11Texture2D* back = nullptr;
    if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return false;

    RECT dst = L.dst;
    if (dst.right <= dst.left || dst.bottom <= dst.top) dst = { 0, 0, sw, sh };

    // 目标没铺满 surface（letterbox）时先刷黑，免得留着上一帧的残影
    const bool full = dst.left <= 0 && dst.top <= 0 && dst.right >= sw && dst.bottom >= sh;
    if (!full) {
        ID3D11RenderTargetView* rtv = nullptr;
        if (SUCCEEDED(dev->CreateRenderTargetView(back, nullptr, &rtv)) && rtv) {
            const FLOAT black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            ctx->ClearRenderTargetView(rtv, black);
            rtv->Release();
        }
    }

    const MFVideoNormalizedRect src = { L.srcLeft, L.srcTop, L.srcRight, L.srcBottom };
    const MFARGB border = { 0, 0, 0, 255 };

    // —— 调色路径 ——
    // 帧先落进 stage，再由 shader 链画到后台缓冲的 dst 区域。
    // 只在真的调了参数时才走，且任何一环失败都退回下面那条直通老路（顶多不生效，不黑屏）。
    bool done = false;
    if (E.Active() && !fxFailed) {
        if (!chain.Ready() && !chain.Init(dev, ctx)) {
            fxFailed = true;   // d3dcompiler 缺失或 shader 编译不过，别每帧重试
        } else {
            const int dw = static_cast<int>(dst.right - dst.left);
            const int dh = static_cast<int>(dst.bottom - dst.top);
            if (EnsureStage(dw, dh)) {
                const RECT sdst = { 0, 0, dw, dh };
                EnterCriticalSection(&renderCs);
                const HRESULT th = ex->TransferVideoFrame(stage, &src, &sdst, &border);
                if (SUCCEEDED(th)) done = chain.Render(stage, dw, dh, back, dst, E);
                LeaveCriticalSection(&renderCs);
            }
        }
    }

    HRESULT hr = S_OK;
    if (!done) {
        EnterCriticalSection(&renderCs);
        hr = ex->TransferVideoFrame(back, &src, &dst, &border);
        LeaveCriticalSection(&renderCs);
    }
    back->Release();

    if (FAILED(hr)) return false;
    swap->Present(1, 0);      // 交给垂直同步限速，不用自己数时间
    InterlockedIncrement(&frames);
    return true;
}

void Player::Impl::RenderLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    while (InterlockedCompareExchange(&running, 1, 1) == 1) {
        bool needResize = false, mustDraw = false;
        int  w = 0, h = 0;
        EnterCriticalSection(&cs);
        if (pendingResize) {
            pendingResize = false;
            needResize = true;
            w = surfW;
            h = surfH;
        }
        if (force) { force = false; mustDraw = true; }
        LeaveCriticalSection(&cs);

        if (needResize && swap) {
            swap->ResizeBuffers(0, static_cast<UINT>(w < 1 ? 1 : w),
                                static_cast<UINT>(h < 1 ? 1 : h),
                                DXGI_FORMAT_UNKNOWN, 0);
            mustDraw = true;   // ResizeBuffers 之后缓冲区内容是未定义的，不补一帧会黑屏
        }

        LONGLONG pts = 0;
        // ⭐ 判据是 **pts 变了没有**，不是 OnVideoStreamTick 返回什么。
        //    它在同一帧上会反复返回 S_OK：30fps 的视频在 144Hz 屏上实测每秒被搬 44 次，
        //    多出来的都是同一张画面（2026-08-22 用帧计数量到：2 秒 +89 帧）。
        //    每一次多余的 TransferVideoFrame 都是一次真实的 GPU 视频处理器调用。
        const bool tick = ex && ex->OnVideoStreamTick(&pts) == S_OK;
        bool draw = mustDraw || (tick && pts != lastPts);

        // ⚠️ 这里**不要**再加一道「按刷新率掐表」的节流，两条路都试过、都更差：
        //
        //   waitable swapchain（FRAME_LATENCY_WAITABLE_OBJECT）
        //       帧率确实压到 60.0，但 CPU 从 6.2% 涨到 15.6%（latency=2）/ 20.8%（latency=1）——
        //       等待本身比省下的搬运还贵。
        //   QueryPerformanceCounter 掐表 + Sleep(2)
        //       掉到 38.4fps：Windows 默认定时器精度是 15.6ms，Sleep(2) 实际睡掉一整个刷新周期，
        //       反而错过绘制时机。想修就得 timeBeginPeriod(1)，那是全局副作用，壁纸不配。
        //
        //   （2026-08-22 实测，都在同一段 4K60 壁纸上量的。）
        //   pts 去重之后自然速率是 69.8fps，比 60Hz 多出的那 16% 只是 GPU 上多一次 blit，
        //   比上面两种代价都小。

        if (draw) {
            if (tick) lastPts = pts;
            InterlockedExchange(&renderOk, RenderOne() ? 1 : 0);
        } else {
            Sleep(3);
        }
    }

    CoUninitialize();
}

// ---------------------------------------------------------------------------

bool Player::StartupMF() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return false;
    return SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
}

void Player::ShutdownMF() {
    MFShutdown();
    CoUninitialize();
}

Player::~Player() { Close(); }

bool Player::Open(HWND hwnd, const std::wstring& path) {
    Close();
    if (path.empty() || !hwnd) return false;

    impl_ = new Impl();
    InitializeCriticalSection(&impl_->cs);
    InitializeCriticalSection(&impl_->renderCs);
    impl_->hwnd = hwnd;

    // --- D3D11 设备 ---
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   &impl_->dev, &got, &impl_->ctx);
    if (FAILED(hr)) {
        // 没有硬件设备（极老的虚拟显卡 / RDP 会话）就退回 WARP 软件渲染，
        // 慢，但总比一片黑强
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                               levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                               &impl_->dev, &got, &impl_->ctx);
    }
    if (FAILED(hr)) { Close(); return false; }

    ID3D11Multithread* mt = nullptr;
    if (SUCCEEDED(impl_->ctx->QueryInterface(IID_PPV_ARGS(&mt))) && mt) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }

    // --- 交给 Media Engine 的 DXGI 设备管理器 ---
    UINT token = 0;
    if (FAILED(MFCreateDXGIDeviceManager(&token, &impl_->dxgiMgr))) { Close(); return false; }
    if (FAILED(impl_->dxgiMgr->ResetDevice(impl_->dev, token)))      { Close(); return false; }

    RECT rc{};
    GetClientRect(hwnd, &rc);
    if (!impl_->CreateSwapChain(rc.right - rc.left, rc.bottom - rc.top)) { Close(); return false; }

    // --- Media Engine ---
    IMFMediaEngineClassFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        Close();
        return false;
    }

    impl_->notify = new EngineNotify(hwnd);

    IMFAttributes* attrs = nullptr;
    if (FAILED(MFCreateAttributes(&attrs, 4))) { factory->Release(); Close(); return false; }

    // ⚠️ 这里**没有** MF_MEDIA_ENGINE_PLAYBACK_HWND —— 给了它就会自己往窗口上画，
    //    那正是我们要摆脱的那条路。给 DXGI manager 才进 frame-server 模式。
    attrs->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, impl_->notify);
    attrs->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, impl_->dxgiMgr);
    attrs->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);

    IMFMediaEngine* engine = nullptr;
    hr = factory->CreateInstance(0, attrs, &engine);
    attrs->Release();
    factory->Release();
    if (FAILED(hr) || !engine) { Close(); return false; }

    engine_ = engine;
    if (FAILED(engine->QueryInterface(IID_PPV_ARGS(&impl_->ex))) || !impl_->ex) {
        Close();
        return false;
    }

    engine->SetLoop(TRUE);      // 壁纸永远是循环的
    engine->SetMuted(TRUE);     // 默认静音；有声音的壁纸是灾难
    engine->SetAutoPlay(TRUE);

    BSTR url = SysAllocString(path.c_str());
    const HRESULT hs = engine->SetSource(url);
    SysFreeString(url);
    if (FAILED(hs)) { Close(); return false; }

    InterlockedExchange(&impl_->running, 1);
    impl_->thread = CreateThread(nullptr, 0, &Impl::ThreadProc, impl_, 0, nullptr);
    if (!impl_->thread) { Close(); return false; }

    paused_ = false;
    return true;
}

void Player::Close() {
    if (impl_) {
        // 先停线程再拆东西：反过来会在渲染中途把 swapchain 抽走
        InterlockedExchange(&impl_->running, 0);
        if (impl_->thread) {
            WaitForSingleObject(impl_->thread, 2000);
            CloseHandle(impl_->thread);
            impl_->thread = nullptr;
        }
    }
    if (engine_) {
        auto* e = static_cast<IMFMediaEngine*>(engine_);
        e->Shutdown();
        e->Release();
        engine_ = nullptr;
    }
    if (impl_) {
        SafeRelease(impl_->ex);
        if (impl_->notify) { impl_->notify->Release(); impl_->notify = nullptr; }
        // 调色链持的是 dev/ctx 的裸指针和一堆纹理，必须赶在设备释放之前拆
        impl_->chain.Shutdown();
        SafeRelease(impl_->stage);
        SafeRelease(impl_->swap);
        SafeRelease(impl_->dxgiMgr);
        SafeRelease(impl_->ctx);
        SafeRelease(impl_->dev);
        DeleteCriticalSection(&impl_->cs);
        DeleteCriticalSection(&impl_->renderCs);
        delete impl_;
        impl_ = nullptr;
    }
    paused_ = true;
}

void Player::Play() {
    if (!engine_) return;
    static_cast<IMFMediaEngine*>(engine_)->Play();
    paused_ = false;
}

void Player::Pause() {
    if (!engine_) return;
    static_cast<IMFMediaEngine*>(engine_)->Pause();
    paused_ = true;
}

void Player::SetMuted(bool muted) {
    if (!engine_) return;
    static_cast<IMFMediaEngine*>(engine_)->SetMuted(muted ? TRUE : FALSE);
}

void Player::SetVolume(double v) {
    if (!engine_) return;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    static_cast<IMFMediaEngine*>(engine_)->SetVolume(v);
}

double Player::Position() const {
    if (!engine_) return -1.0;
    return static_cast<IMFMediaEngine*>(engine_)->GetCurrentTime();
}

bool Player::GetVideoSize(int& w, int& h) const {
    if (!engine_) return false;
    DWORD cx = 0, cy = 0;
    if (FAILED(static_cast<IMFMediaEngine*>(engine_)->GetNativeVideoSize(&cx, &cy))) return false;
    if (cx == 0 || cy == 0) return false;
    w = static_cast<int>(cx);
    h = static_cast<int>(cy);
    return true;
}

bool Player::SetSurfaceSize(int w, int h) {
    if (!impl_ || !impl_->swap) return false;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    EnterCriticalSection(&impl_->cs);
    const bool changed = (impl_->surfW != w || impl_->surfH != h);
    if (changed) {
        impl_->surfW = w;
        impl_->surfH = h;
        impl_->pendingResize = true;   // 真正的 ResizeBuffers 由渲染线程做
        impl_->force = true;
    }
    LeaveCriticalSection(&impl_->cs);
    return true;
}

void Player::SetLayout(const Layout& layout) {
    if (!impl_) return;
    EnterCriticalSection(&impl_->cs);
    impl_->layout = layout;
    impl_->force  = true;   // 暂停状态下改取景也要立刻反映出来
    LeaveCriticalSection(&impl_->cs);
}

void Player::SetEffects(const Effects& fx) {
    if (!impl_) return;
    EnterCriticalSection(&impl_->cs);
    impl_->fx    = fx;
    impl_->force = true;   // 暂停时拖滑杆也要马上看到，不能等下一帧
    LeaveCriticalSection(&impl_->cs);
}

bool Player::FxReady() const {
    return impl_ && impl_->chain.Ready();
}

bool Player::Rendered() const {
    return impl_ && InterlockedCompareExchange(&impl_->renderOk, 0, 0) == 1;
}

DWORD Player::FrameCount() const {
    return impl_ ? static_cast<DWORD>(InterlockedCompareExchange(&impl_->frames, 0, 0)) : 0;
}

// 抓整幅当前帧。走的是和正常渲染同一条路（TransferVideoFrame），
// 只是目标换成一张离屏纹理、源取全图不裁 —— 预览底图必须是**完整画面**，
// 取景框才有东西可框。
bool Player::CapturePoster(int w, int h, std::vector<unsigned char>& out) {
    if (!impl_ || !impl_->ex || !impl_->dev || w < 1 || h < 1) return false;

    D3D11_TEXTURE2D_DESC rtd = {};
    rtd.Width = (UINT)w;
    rtd.Height = (UINT)h;
    rtd.MipLevels = 1;
    rtd.ArraySize = 1;
    rtd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rtd.SampleDesc.Count = 1;
    rtd.Usage = D3D11_USAGE_DEFAULT;
    rtd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    D3D11_TEXTURE2D_DESC sd = rtd;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D* rt = nullptr;
    ID3D11Texture2D* staging = nullptr;
    bool ok = false;

    EnterCriticalSection(&impl_->renderCs);
    do {
        if (FAILED(impl_->dev->CreateTexture2D(&rtd, nullptr, &rt))) break;
        if (FAILED(impl_->dev->CreateTexture2D(&sd, nullptr, &staging))) break;

        const MFVideoNormalizedRect full = { 0.0f, 0.0f, 1.0f, 1.0f };
        const RECT dst = { 0, 0, w, h };
        const MFARGB border = { 0, 0, 0, 255 };
        if (FAILED(impl_->ex->TransferVideoFrame(rt, &full, &dst, &border))) break;

        impl_->ctx->CopyResource(staging, rt);

        D3D11_MAPPED_SUBRESOURCE m = {};
        if (FAILED(impl_->ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) break;
        out.resize((size_t)w * h * 4);
        const unsigned char* src = static_cast<const unsigned char*>(m.pData);
        for (int y = 0; y < h; ++y)
            memcpy(&out[(size_t)y * w * 4], src + (size_t)y * m.RowPitch, (size_t)w * 4);
        impl_->ctx->Unmap(staging, 0);
        ok = true;
    } while (false);
    LeaveCriticalSection(&impl_->renderCs);

    SafeRelease(staging);
    SafeRelease(rt);
    return ok;
}
