// 开机启动的两套实现，见 startup.h 的说明。
//
// ⚠️ 这个文件里手写了一段 WinRT 的 ABI 声明。原因：mingw / llvm-mingw 自带的 WinRT 头
// （windows.applicationmodel.h）**没有** IStartupTask 和 IStartupTaskStatics，
// 而我们又不想为了四个方法引入 C++/WinRT（那要 MSVC）。
//
// 下面的 IID 和 vtable 顺序照抄 Windows SDK 的 Windows.ApplicationModel.idl，
// **顺序写错不会编译报错，只会在运行时调到别的函数**，所以不要凭印象改：
//
//   [uuid(F75C23C8-B5F2-4F6C-88DD-36CB1D599D17)] interface IStartupTask : IInspectable {
//       HRESULT RequestEnableAsync(IAsyncOperation<StartupTaskState>** operation);
//       HRESULT Disable();
//       [propget] HRESULT State(StartupTaskState* value);
//       [propget] HRESULT TaskId(HSTRING* value);
//   }
//   [uuid(EE5B60BD-A148-41A7-B26E-E8B88A1E62F8)] interface IStartupTaskStatics : IInspectable {
//       HRESULT GetForCurrentPackageAsync(IAsyncOperation<IVectorView<StartupTask*>*>** operation);
//       HRESULT GetAsync(HSTRING taskId, IAsyncOperation<StartupTask*>** operation);
//   }
//   enum StartupTaskState { Disabled = 0, DisabledByUser = 1, Enabled = 2 };

#include <windows.h>
#include <appmodel.h>
#include <roapi.h>
#include <winstring.h>
#include <string>

#include "startup.h"

namespace {

constexpr wchar_t kAppName[] = L"StarPaper";
constexpr wchar_t kRunPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// AppxManifest.xml 里 <desktop:StartupTask TaskId="..."> 的值，**必须一字不差**。
constexpr wchar_t kTaskId[]  = L"StarPaperStartupTask";

// —— 非打包：老路，写 Run 键 ——

bool RunKeyHas() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunPath, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    const bool has = RegQueryValueExW(key, kAppName, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return has;
}

void RunKeySet(bool on) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunPath, 0, KEY_WRITE, &key) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::wstring quoted = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(key, kAppName, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(quoted.c_str()),
                       static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kAppName);
    }
    RegCloseKey(key);
}

// —— 打包：WinRT StartupTask ——

// IInspectable 的前 6 项，所有 WinRT 接口都以它开头。
#define STARPAPER_IINSPECTABLE_HEAD                                        \
    HRESULT (STDMETHODCALLTYPE* QueryInterface)(void*, REFIID, void**);    \
    ULONG   (STDMETHODCALLTYPE* AddRef)(void*);                            \
    ULONG   (STDMETHODCALLTYPE* Release)(void*);                           \
    HRESULT (STDMETHODCALLTYPE* GetIids)(void*, ULONG*, IID**);            \
    HRESULT (STDMETHODCALLTYPE* GetRuntimeClassName)(void*, HSTRING*);     \
    HRESULT (STDMETHODCALLTYPE* GetTrustLevel)(void*, INT32*)

struct StaticsVtbl {
    STARPAPER_IINSPECTABLE_HEAD;
    HRESULT (STDMETHODCALLTYPE* GetForCurrentPackageAsync)(void*, void**);
    HRESULT (STDMETHODCALLTYPE* GetAsync)(void*, HSTRING, void**);
};

struct TaskVtbl {
    STARPAPER_IINSPECTABLE_HEAD;
    HRESULT (STDMETHODCALLTYPE* RequestEnableAsync)(void*, void**);
    HRESULT (STDMETHODCALLTYPE* Disable)(void*);
    HRESULT (STDMETHODCALLTYPE* get_State)(void*, INT32*);
    HRESULT (STDMETHODCALLTYPE* get_TaskId)(void*, HSTRING*);
};

// IAsyncInfo（非泛型，IID 固定）；IAsyncOperation<T> 的 GetResults 在 vtable 第 9 项
// （IInspectable 6 项 + put_Completed / get_Completed / GetResults）。
struct AsyncInfoVtbl {
    STARPAPER_IINSPECTABLE_HEAD;
    HRESULT (STDMETHODCALLTYPE* get_Id)(void*, UINT32*);
    HRESULT (STDMETHODCALLTYPE* get_Status)(void*, INT32*);
    HRESULT (STDMETHODCALLTYPE* get_ErrorCode)(void*, HRESULT*);
    HRESULT (STDMETHODCALLTYPE* Cancel)(void*);
    HRESULT (STDMETHODCALLTYPE* Close)(void*);
};

struct AsyncOpVtbl {
    STARPAPER_IINSPECTABLE_HEAD;
    HRESULT (STDMETHODCALLTYPE* put_Completed)(void*, void*);
    HRESULT (STDMETHODCALLTYPE* get_Completed)(void*, void**);
    HRESULT (STDMETHODCALLTYPE* GetResults)(void*, void* out);
};

template <typename V>
struct Obj { const V* vtbl; };

const GUID kIID_StartupTaskStatics =
    {0xEE5B60BD, 0xA148, 0x41A7, {0xB2, 0x6E, 0xE8, 0xB8, 0x8A, 0x1E, 0x62, 0xF8}};
const GUID kIID_AsyncInfo =
    {0x00000036, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

void ReleaseAny(void* p) {
    if (!p) return;
    auto* o = static_cast<Obj<AsyncInfoVtbl>*>(p);
    o->vtbl->Release(p);
}

// 等一个 IAsyncOperation<T> 出结果。`out` 是 GetResults 的出参地址
// （T = StartupTask* 时传 void**，T = StartupTaskState 时传 INT32*）。
//
// ⚠️ 不实现 IAsyncOperationCompletedHandler，而是 QI 到 IAsyncInfo 轮询 Status ——
// 泛型接口的 IID 要按参数化类型算，手写成本高；这两个操作都是本地的、瞬间完成。
bool AwaitAsync(void* op, void* out) {
    if (!op) return false;
    auto* opObj = static_cast<Obj<AsyncOpVtbl>*>(op);

    void* info = nullptr;
    if (FAILED(opObj->vtbl->QueryInterface(op, kIID_AsyncInfo, &info)) || !info) {
        ReleaseAny(op);
        return false;
    }
    auto* infoObj = static_cast<Obj<AsyncInfoVtbl>*>(info);

    // AsyncStatus: Started=0 Completed=1 Canceled=2 Error=3
    INT32 status = 0;
    for (int i = 0; i < 400; ++i) {          // 最多等 2 秒，别把 UI 线程挂死
        if (FAILED(infoObj->vtbl->get_Status(info, &status))) break;
        if (status != 0) break;
        Sleep(5);
    }
    ReleaseAny(info);

    bool ok = false;
    if (status == 1 && SUCCEEDED(opObj->vtbl->GetResults(op, out))) ok = true;
    ReleaseAny(op);
    return ok;
}

// 拿到 IStartupTask*。调用方负责 Release。
void* GetTask() {
    static bool roTried = false;
    if (!roTried) {
        roTried = true;
        // 线程通常已经被 CoInitializeEx(APARTMENTTHREADED) 初始化过（见 player.cpp）。
        // 这里只是补一手；RPC_E_CHANGED_MODE / S_FALSE 都不算失败，且**不配对 Uninitialize**。
        RoInitialize(RO_INIT_SINGLETHREADED);
    }

    HSTRING clsName = nullptr;
    HSTRING_HEADER clsHeader;
    static const wchar_t kClass[] = L"Windows.ApplicationModel.StartupTask";
    if (FAILED(WindowsCreateStringReference(kClass, ARRAYSIZE(kClass) - 1, &clsHeader, &clsName)))
        return nullptr;

    void* statics = nullptr;
    if (FAILED(RoGetActivationFactory(clsName, kIID_StartupTaskStatics, &statics)) || !statics)
        return nullptr;
    auto* st = static_cast<Obj<StaticsVtbl>*>(statics);

    HSTRING taskId = nullptr;
    HSTRING_HEADER idHeader;
    if (FAILED(WindowsCreateStringReference(kTaskId, ARRAYSIZE(kTaskId) - 1, &idHeader, &taskId))) {
        ReleaseAny(statics);
        return nullptr;
    }

    void* op = nullptr;
    const HRESULT hr = st->vtbl->GetAsync(statics, taskId, &op);
    ReleaseAny(statics);
    if (FAILED(hr) || !op) return nullptr;

    void* task = nullptr;
    if (!AwaitAsync(op, &task)) return nullptr;
    return task;
}

}  // namespace

namespace startup {

bool IsPackaged() {
    static const bool packaged = [] {
        UINT32 len = 0;
        // 没有包身份时返回 APPMODEL_ERROR_NO_PACKAGE；有包身份时因为 len=0 返回
        // ERROR_INSUFFICIENT_BUFFER。⚠️ 判据是「不等于 NO_PACKAGE」，不是「成功」。
        return GetCurrentPackageFullName(&len, nullptr) != APPMODEL_ERROR_NO_PACKAGE;
    }();
    return packaged;
}

State Query() {
    if (!IsPackaged()) return RunKeyHas() ? State::On : State::Off;

    void* task = GetTask();
    if (!task) return State::Unavailable;
    auto* t = static_cast<Obj<TaskVtbl>*>(task);

    INT32 state = 0;
    const bool ok = SUCCEEDED(t->vtbl->get_State(task, &state));
    ReleaseAny(task);
    if (!ok) return State::Unavailable;

    switch (state) {
        case 2:  return State::On;            // Enabled
        case 1:  return State::LockedByUser;  // DisabledByUser
        default: return State::Off;           // Disabled
    }
}

void Set(bool on) {
    if (!IsPackaged()) { RunKeySet(on); return; }

    void* task = GetTask();
    if (!task) return;
    auto* t = static_cast<Obj<TaskVtbl>*>(task);

    if (on) {
        // 打包的桌面程序调这个**不弹用户确认框**（只有 UWP 才弹）。
        // 用户此前在任务管理器里关过的话，这里会原样返回 DisabledByUser —— 覆盖不了，设计如此。
        void* op = nullptr;
        if (SUCCEEDED(t->vtbl->RequestEnableAsync(task, &op)) && op) {
            INT32 newState = 0;
            AwaitAsync(op, &newState);
        }
    } else {
        t->vtbl->Disable(task);
    }
    ReleaseAny(task);
}

}  // namespace startup
