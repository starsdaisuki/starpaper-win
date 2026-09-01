#pragma once

// 开机启动。有两套完全不同的机制，取决于本进程是不是跑在 MSIX 包里：
//
//   非打包（scoop / 直接跑 exe）→ 写 HKCU\...\CurrentVersion\Run
//   打包（Microsoft Store）     → WinRT 的 Windows.ApplicationModel.StartupTask
//
// ⚠️ 打包状态下写 Run 键是**静默失效**的：RegSetValueEx 返回成功，但值不会出现在
// 真实的 HKCU 里，开机也不会启动，且没有任何报错。所以这里必须分流，不能只留一套。

namespace startup {

enum class State {
    Off,            // 没开，可以开
    On,             // 开着
    LockedByUser,   // 用户在「任务管理器 → 启动」里关掉了 —— 程序无权再打开
    Unavailable,    // 打包环境但 StartupTask 拿不到（缺 manifest 声明 / 组策略禁用）
};

// 本进程是否跑在 MSIX 包里。结果缓存，可以随便调。
bool IsPackaged();

State Query();
void  Set(bool on);

}  // namespace startup
