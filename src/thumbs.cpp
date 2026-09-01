#include "thumbs.h"

#include <shobjidl.h>
#include <shlobj.h>
#include <map>
#include <deque>
#include <string>

namespace thumbs {
namespace {

struct Entry {
    HBITMAP bmp = nullptr;
    bool    tried = false;    // 试过但失败：别一直重排队
};

CRITICAL_SECTION      g_cs;
std::map<std::wstring, Entry> g_cache;
std::deque<std::wstring>      g_queue;
HANDLE  g_thread = nullptr;
HANDLE  g_wake   = nullptr;
HWND    g_notify = nullptr;
int     g_w = 160, g_h = 90;
volatile LONG g_running = 0;
bool    g_inited = false;

// 真正去要图。会阻塞（首次可能要解码视频），所以只在后台线程里调。
HBITMAP Fetch(const std::wstring& path, int w, int h) {
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) || !item)
        return nullptr;

    IShellItemImageFactory* fac = nullptr;
    HBITMAP bmp = nullptr;
    if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&fac))) && fac) {
        const SIZE sz = { w, h };
        // 先只要真缩略图。要不到（比如格式没装解码器）再退一步收图标 ——
        // 有个东西显示总比空着强。
        if (FAILED(fac->GetImage(sz, SIIGBF_BIGGERSIZEOK | SIIGBF_THUMBNAILONLY, &bmp)))
            if (FAILED(fac->GetImage(sz, SIIGBF_BIGGERSIZEOK, &bmp)))
                bmp = nullptr;
        fac->Release();
    }
    item->Release();
    return bmp;
}

DWORD WINAPI Worker(void*) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    while (InterlockedCompareExchange(&g_running, 1, 1) == 1) {
        std::wstring job;
        EnterCriticalSection(&g_cs);
        if (!g_queue.empty()) { job = g_queue.front(); g_queue.pop_front(); }
        LeaveCriticalSection(&g_cs);

        if (job.empty()) {
            WaitForSingleObject(g_wake, 500);
            continue;
        }

        const int w = g_w, h = g_h;
        HBITMAP bmp = Fetch(job, w, h);

        EnterCriticalSection(&g_cs);
        Entry& e = g_cache[job];
        if (e.bmp && e.bmp != bmp) DeleteObject(e.bmp);
        e.bmp   = bmp;
        e.tried = true;
        HWND notify = g_notify;
        LeaveCriticalSection(&g_cs);

        if (notify) PostMessageW(notify, kReady, 0, 0);
    }
    CoUninitialize();
    return 0;
}

} // namespace

void Init() {
    if (g_inited) return;
    InitializeCriticalSection(&g_cs);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    InterlockedExchange(&g_running, 1);
    g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    g_inited = true;
}

void Shutdown() {
    if (!g_inited) return;
    InterlockedExchange(&g_running, 0);
    if (g_wake) SetEvent(g_wake);
    if (g_thread) {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_wake) { CloseHandle(g_wake); g_wake = nullptr; }
    Clear();
    DeleteCriticalSection(&g_cs);
    g_inited = false;
}

HBITMAP Get(const std::wstring& path, int w, int h, HWND notify) {
    if (!g_inited || path.empty()) return nullptr;

    EnterCriticalSection(&g_cs);
    g_notify = notify;
    // 尺寸变了（换 DPI）就整批作废重来 —— 拉伸小图会糊
    if (w != g_w || h != g_h) {
        g_w = w; g_h = h;
        for (auto& kv : g_cache) if (kv.second.bmp) DeleteObject(kv.second.bmp);
        g_cache.clear();
        g_queue.clear();
    }

    auto it = g_cache.find(path);
    if (it != g_cache.end()) {
        HBITMAP b = it->second.bmp;
        LeaveCriticalSection(&g_cs);
        return b;                       // 可能是 nullptr（试过、拿不到），那就别再排队
    }

    g_cache[path] = Entry{};            // 占位，免得重复入队
    bool queued = false;
    for (const auto& q : g_queue) if (q == path) { queued = true; break; }
    if (!queued) g_queue.push_back(path);
    LeaveCriticalSection(&g_cs);

    if (g_wake) SetEvent(g_wake);
    return nullptr;
}

void Forget(const std::wstring& path) {
    if (!g_inited) return;
    EnterCriticalSection(&g_cs);
    auto it = g_cache.find(path);
    if (it != g_cache.end()) {
        if (it->second.bmp) DeleteObject(it->second.bmp);
        g_cache.erase(it);
    }
    LeaveCriticalSection(&g_cs);
}

void Clear() {
    EnterCriticalSection(&g_cs);
    for (auto& kv : g_cache) if (kv.second.bmp) DeleteObject(kv.second.bmp);
    g_cache.clear();
    g_queue.clear();
    LeaveCriticalSection(&g_cs);
}

} // namespace thumbs
