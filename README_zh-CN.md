# ShutdownGuard

ShutdownGuard 是一个原生 Win32 应用程序，用于阻止常见的 Windows 关机、重启及
注销操作。它采用多层防御策略：窗口过程钩子、全局 CBT 钩子、API 级 Detours 拦截，
以及向 shell 和命令行进程主动注入 DLL。

## 项目结构

```
ShutdownGuard/
├── CMakeLists.txt              # 根构建文件
├── CMakePresets.json           # 构建预设 (vs2022-x64 debug/release)
├── src/
│   ├── main.cpp                # 控制端 UI (ShutdownGuard.exe)
│   └── global_shutdown_hook.cpp # 钩子 DLL (ShutdownGuardHook.dll)
└── third_party/detours/        # 内置 Microsoft Detours (MIT 协议)
    ├── CMakeLists.txt
    ├── LICENSE
    └── *.cpp / *.h             # Detours 源码文件
```

## 设计

### 多层防御

ShutdownGuard 通过四层机制拦截关机请求：

**第一层 — 窗口过程钩子**
主窗口创建后，控制端调用 `SetWindowLongPtr(GWLP_WNDPROC, ...)` 将窗口过程替换为
`HookedWindowProc`。该钩子直接拦截 `WM_QUERYENDSESSION` —— 拦截开启时返回
`FALSE` 以取消会话结束请求。

**第二层 — 关机阻止原因**
`ShutdownBlockReasonCreate` 向 Windows 注册阻止原因字符串。这是应用程序阻止关机
的标准 API，使 ShutdownGuard 在 Windows 自带的阻止界面中可见。

**第三层 — 全局 CBT 钩子（向 GUI 进程注入 DLL）**
DLL 中的 `InstallGlobalShutdownHook` 调用 `SetWindowsHookEx(WH_CBT, ...)` 并
传入线程 ID 为 0。Windows 会将 `ShutdownGuardHook.dll` 加载到当前会话中所有具有
消息泵和窗口钩子链的进程中 —— 最重要的是 `explorer.exe`。

**第四层 — 向命令行进程主动注入 DLL**
控制端枚举常见的 shell 和命令行宿主进程，通过 `CreateRemoteThread` +
`LoadLibraryW` 加载钩子 DLL。目标进程：

| 类别 | 进程 |
|---|---|
| Shell 宿主 | `explorer.exe`、`StartMenuExperienceHost.exe`、`ShellExperienceHost.exe`、`sihost.exe` |
| 应用宿主 | `RuntimeBroker.exe`、`ApplicationFrameHost.exe` |
| 命令行宿主 | `cmd.exe`、`powershell.exe`、`pwsh.exe`、`WindowsTerminal.exe`、`wt.exe`、`OpenConsole.exe` |
| 工具 | `wmic.exe`、`rundll32.exe` |

### 注入后行为

DLL 加载到目标进程后，`DllMain` 立即调用 `AttachApiHooks()`，使用 Microsoft
Detours 拦截以下 API：

**关机/注销 API** — 拦截开启时始终阻止：
- `ExitWindowsEx`
- `InitiateShutdownA` / `InitiateShutdownW`
- `InitiateSystemShutdownA` / `InitiateSystemShutdownW`
- `InitiateSystemShutdownExA` / `InitiateSystemShutdownExW`
- `WTSLogoffSession`
- `WTSShutdownSystem`
- `NtShutdownSystem`（通过 `ntdll.dll`）
- `NtInitiatePowerAction`（通过 `ntdll.dll`）

**进程启动 API** — 命令行包含危险内容时阻止：
- `CreateProcessA` / `CreateProcessW`
- `ShellExecuteA` / `ShellExecuteW`
- `ShellExecuteExA` / `ShellExecuteExW`
- `WinExec`

### 进程启动过滤逻辑

钩子 DLL 检查每个进程创建请求的可执行文件名和命令行：

- **直接阻止的可执行文件**：`shutdown.exe`（仅破坏性模式）、`logoff.exe`、
  `slidetoshutdown.exe`、`psshutdown.exe`
- **`shutdown /a` 始终放行**，以便仍可取消已调度的关机
- **命令行宿主进程**（`cmd.exe`、PowerShell、Windows Terminal、`wmic`、
  `rundll32`、`nircmd`）会被检查 —— 如果命令行包含关机或注销关键字则阻止
- 未被阻止的命令行宿主进程以暂停状态启动（`CREATE_SUSPENDED`），注入
  `ShutdownGuardHook.dll` 后再恢复。这确保 DLL 也存在于子 shell 进程中

### 跨进程状态共享

DLL 使用命名文件映射（`Local\ShutdownGuard.GlobalHookState`）在进程间共享拦截
状态，控制端切换拦截开关时，所有已注入实例即时感知变化。

### 可观测性

- 控制端中的线程局部 `WH_CALLWNDPROC` 钩子记录 `WM_QUERYENDSESSION` 和
  `WM_ENDSESSION` 消息何时经过主窗口 —— 仅用于诊断。
- 所有重要事件记录到 `%LOCALAPPDATA%\ShutdownGuard\ShutdownGuard.log`。
- 控制端调用 `SetProcessShutdownParameters(0x3FF, 0)` 以尽可能早地接收关机消息。

### 局限性

强制关机或关键关机路径仍可能继续执行。高权限、系统级、受保护或不同位宽
（32 位与 64 位）进程不会加载用户态钩子 DLL，不在覆盖范围内。

## 环境要求

- Windows 10 或 Windows 11
- Visual Studio 2022，包含"使用 C++ 的桌面开发"工作负载
- Windows SDK
- CMake 3.21 或更高版本

Microsoft Detours 已内置于 `third_party/detours/` 目录中，会在构建时从源码编译。
无需外部依赖或额外下载。

## 构建

```powershell
# 首次配置（仅需一次）
cmake --preset vs2022-x64

# 构建（Debug 或 Release）
cmake --build build/vs2022-x64 --config Debug
cmake --build build/vs2022-x64 --config Release

# 或直接使用构建预设
cmake --build --preset debug
cmake --build --preset release
```

输出文件：
- `build/vs2022-x64/<Config>/ShutdownGuard.exe`
- `build/vs2022-x64/<Config>/ShutdownGuardHook.dll`

## 使用说明

1. 将 `ShutdownGuardHook.dll` 放在 `ShutdownGuard.exe` 同目录下。
2. 运行 `ShutdownGuard.exe`。拦截功能在启动时自动开启。
3. 点击 **Self test**（自检）—— 预期结果为 `blocked`（已阻止）。
4. 尝试以下操作 —— 应被阻止：
   - Windows 资源管理器 → 电源 → 关机 / 重启
   - `shutdown /s`、`shutdown /r`、`shutdown /l`
   - `logoff.exe`
   - `slidetoshutdown.exe`
   - PowerShell：`Stop-Computer`、`Restart-Computer`
5. 点击切换按钮或退出应用即可正常关机/重启/注销。

## API 参考

- [WM_QUERYENDSESSION](https://learn.microsoft.com/en-us/windows/win32/shutdown/wm-queryendsession)
- [WM_ENDSESSION](https://learn.microsoft.com/en-us/windows/win32/shutdown/wm-endsession)
- [ShutdownBlockReasonCreate](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-shutdownblockreasoncreate)
- [SetWindowsHookExW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowshookexw)
- [SetWindowLongPtrW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowlongptrw)
- [ExitWindowsEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-exitwindowsex)
- [InitiateSystemShutdownExW](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-initiatesystemshutdownexw)
- [InitiateShutdownW](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-initiateshutdownw)
- [WTSLogoffSession](https://learn.microsoft.com/en-us/windows/win32/api/wtsapi32/nf-wtsapi32-wtslogoffsession)
- [WTSShutdownSystem](https://learn.microsoft.com/en-us/windows/win32/api/wtsapi32/nf-wtsapi32-wtsshutdownsystem)
- [shutdown 命令](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/shutdown)
- [Microsoft Detours](https://github.com/microsoft/Detours)

## 致谢

特别感谢 [@q2154245967-alt](https://github.com/q2154245967-alt) 为本项目做出的贡献。
