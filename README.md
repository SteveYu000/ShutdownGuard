# ShutdownGuard

ShutdownGuard is a native Win32 application that blocks common Windows
shutdown, restart, and logoff paths. It uses a multi-layered strategy of window
procedure hooks, a global CBT hook, API-level Detours, and active DLL injection
into shell and command processes.

## Project Structure

```
ShutdownGuard/
├── CMakeLists.txt              # Root build file
├── CMakePresets.json           # Build presets (vs2022-x64 debug/release)
├── src/
│   ├── main.cpp                # Controller UI (ShutdownGuard.exe)
│   └── global_shutdown_hook.cpp # Hook DLL (ShutdownGuardHook.dll)
└── third_party/detours/        # Vendored Microsoft Detours (MIT)
    ├── CMakeLists.txt
    ├── LICENSE
    └── *.cpp / *.h             # Detours source files
```

## Design

### Layered Defense

ShutdownGuard uses four layers to intercept shutdown requests:

**Layer 1 — Window Procedure Hook**
After the main window is created, the controller calls
`SetWindowLongPtr(GWLP_WNDPROC, ...)` to replace the window procedure with
`HookedWindowProc`. This intercepts `WM_QUERYENDSESSION` directly — when
blocking is enabled, the hook returns `FALSE` and cancels the session-end
request.

**Layer 2 — Shutdown Block Reason**
`ShutdownBlockReasonCreate` registers a reason string with Windows. This is
the standard API for applications that need to block shutdown, and it makes
ShutdownGuard visible in Windows' own blocker UI.

**Layer 3 — Global CBT Hook (DLL injection into GUI processes)**
`InstallGlobalShutdownHook` in the DLL calls `SetWindowsHookEx(WH_CBT, ...)`
with a thread ID of 0. Windows responds by loading `ShutdownGuardHook.dll` into
every process in the current session that has a message pump and a window hook
chain — most importantly into `explorer.exe`.

**Layer 4 — Active DLL injection into command processes**
The controller enumerates common shell and command-host processes and uses
`CreateRemoteThread` + `LoadLibraryW` to load the hook DLL. Target processes:

| Category | Processes |
|---|---|
| Shell hosts | `explorer.exe`, `StartMenuExperienceHost.exe`, `ShellExperienceHost.exe`, `sihost.exe` |
| App hosts | `RuntimeBroker.exe`, `ApplicationFrameHost.exe` |
| Command hosts | `cmd.exe`, `powershell.exe`, `pwsh.exe`, `WindowsTerminal.exe`, `wt.exe`, `OpenConsole.exe` |
| Utilities | `wmic.exe`, `rundll32.exe` |

### Inside Injected Processes

Once loaded in a target process, `DllMain` immediately calls `AttachApiHooks()`,
which uses Microsoft Detours to intercept these APIs:

**Shutdown/logoff APIs** — always blocked when enabled:
- `ExitWindowsEx`
- `InitiateShutdownA` / `InitiateShutdownW`
- `InitiateSystemShutdownA` / `InitiateSystemShutdownW`
- `InitiateSystemShutdownExA` / `InitiateSystemShutdownExW`
- `WTSLogoffSession`
- `WTSShutdownSystem`
- `NtShutdownSystem` (via `ntdll.dll`)
- `NtInitiatePowerAction` (via `ntdll.dll`)

**Process launch APIs** — blocked when the command line is dangerous:
- `CreateProcessA` / `CreateProcessW`
- `ShellExecuteA` / `ShellExecuteW`
- `ShellExecuteExA` / `ShellExecuteExW`
- `WinExec`

### Process Launch Filtering Logic

The hook DLL inspects the executable name and command line of every process
creation attempt:

- **Directly blocked executables**: `shutdown.exe` (destructive modes only),
  `logoff.exe`, `slidetoshutdown.exe`, `psshutdown.exe`
- **`shutdown /a` is always allowed** so an already-scheduled shutdown can be
  aborted
- **Command-host processes** (`cmd.exe`, PowerShell, Windows Terminal, `wmic`,
  `rundll32`, `nircmd`) are checked — if the command line contains shutdown or
  logoff keywords, the process is blocked
- Command-host processes that are **not** blocked are started suspended
  (`CREATE_SUSPENDED`), injected with `ShutdownGuardHook.dll`, then resumed.
  This ensures the DLL is present in child shell processes.

### Inter-Process State

The DLL uses a named file mapping (`Local\ShutdownGuard.GlobalHookState`) to
share blocking state across processes, so the controller can toggle blocking
on/off and all injected instances see the change instantly.

### Observability

- A thread-local `WH_CALLWNDPROC` hook in the controller records when
  `WM_QUERYENDSESSION` and `WM_ENDSESSION` messages pass through the main window
  — purely diagnostic.
- All significant events are logged to
  `%LOCALAPPDATA%\ShutdownGuard\ShutdownGuard.log`.
- The controller calls `SetProcessShutdownParameters(0x3FF, 0)` to receive
  shutdown messages as early as possible.

### Limitations

Forced or critical shutdown paths may still proceed. Elevated, system,
protected, or opposite-bitness (32-bit vs 64-bit) processes do not load
user-mode hook DLLs and are not covered.

## Requirements

- Windows 10 or Windows 11
- Visual Studio 2022 with the "Desktop development with C++" workload
- Windows SDK
- CMake 3.21 or newer

Microsoft Detours is vendored in `third_party/detours/` and compiled from
source as part of the build. No external dependencies or downloads are needed.

## Build

```powershell
# First-time configure (only needed once)
cmake --preset vs2022-x64

# Build (Debug or Release)
cmake --build build/vs2022-x64 --config Debug
cmake --build build/vs2022-x64 --config Release

# Or use the build presets directly
cmake --build --preset debug
cmake --build --preset release
```

Output files:
- `build/vs2022-x64/<Config>/ShutdownGuard.exe`
- `build/vs2022-x64/<Config>/ShutdownGuardHook.dll`

## Usage

1. Place `ShutdownGuardHook.dll` next to `ShutdownGuard.exe`.
2. Run `ShutdownGuard.exe`. Blocking is enabled automatically at startup.
3. Click **Self test** — the expected result is `blocked`.
4. Try any of the following — they should be blocked:
   - Windows Explorer → Power → Shut down / Restart
   - `shutdown /s`, `shutdown /r`, `shutdown /l`
   - `logoff.exe`
   - `slidetoshutdown.exe`
   - PowerShell: `Stop-Computer`, `Restart-Computer`
5. Click the toggle button or exit the application to allow normal
   shutdown/restart/logoff.

## API References

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
- [shutdown command](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/shutdown)
- [Microsoft Detours](https://github.com/microsoft/Detours)
