#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>

#include <shellapi.h>
#include <strsafe.h>
#include <tlhelp32.h>

#include "ui.h"

#include <atomic>
#include <vector>
#include <string>

namespace {

constexpr wchar_t kWindowClass[] = L"ShutdownGuard.MainWindow";
constexpr wchar_t kWindowTitle[] = L"Shutdown Guard";
constexpr wchar_t kBlockReason[] = L"Shutdown Guard is blocking shutdown.";

constexpr int kIdStatus = 1001;
constexpr int kIdHookStatus = 1002;
constexpr int kIdApiStatus = 1003;
constexpr int kIdLastEvent = 1004;
constexpr int kIdToggle = 1005;
constexpr int kIdClose = 1006;
constexpr int kIdSelfTest = 1007;

constexpr UINT kMsgHookObserved = WM_APP + 1;

// Registered message for Explorer-tray restart
UINT g_uTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

using InstallGlobalShutdownHookFn = BOOL(WINAPI *)();
using RemoveGlobalShutdownHookFn = BOOL(WINAPI *)();
using SetGlobalShutdownBlockingFn = BOOL(WINAPI *)(BOOL);
using IsGlobalShutdownBlockingFn = BOOL(WINAPI *)();
using GetGlobalShutdownBlockedCountFn = LONG(WINAPI *)();
using GetGlobalShutdownHookLastErrorFn = DWORD(WINAPI *)();

struct AppState {
  HWND hwnd = nullptr;
  HWND statusLabel = nullptr;
  HWND hookLabel = nullptr;
  HWND apiLabel = nullptr;
  HWND lastEventLabel = nullptr;
  HWND toggleButton = nullptr;
  HWND closeButton = nullptr;
  HWND selfTestButton = nullptr;
  HFONT uiFont = nullptr;
  HHOOK callWndHook = nullptr;
  WNDPROC originalWindowProc = nullptr;
  bool windowProcHookInstalled = false;
  HMODULE globalHookModule = nullptr;
  bool globalHookInstalled = false;
  InstallGlobalShutdownHookFn installGlobalHook = nullptr;
  RemoveGlobalShutdownHookFn removeGlobalHook = nullptr;
  SetGlobalShutdownBlockingFn setGlobalBlocking = nullptr;
  IsGlobalShutdownBlockingFn isGlobalBlocking = nullptr;
  GetGlobalShutdownBlockedCountFn getGlobalBlockedCount = nullptr;
  GetGlobalShutdownHookLastErrorFn getGlobalHookLastError = nullptr;
  std::atomic_bool blockingEnabled = false;
  bool shutdownReasonCreated = false;
  DWORD lastError = ERROR_SUCCESS;

  // --- new fields ---
  bool trayIconAdded = false;
  bool darkMode = false;
  HBRUSH darkBgBrush = nullptr;
  bool toggleHovered = false;
  bool togglePressed = false;
};

AppState g_state;

std::string WideToUtf8(const std::wstring& value) {
  if (value.empty()) {
    return {};
  }

  const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                        static_cast<int>(value.size()),
                                        nullptr, 0, nullptr, nullptr);
  if (bytes <= 0) {
    return {};
  }

  std::string output(static_cast<size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(),
                      static_cast<int>(value.size()), output.data(), bytes,
                      nullptr, nullptr);
  return output;
}

std::wstring LogFilePath() {
  wchar_t localAppData[MAX_PATH]{};
  DWORD length =
      GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
  std::wstring directory;
  if (length > 0 && length < MAX_PATH) {
    directory.assign(localAppData, length);
    directory += L"\\ShutdownGuard";
  } else {
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    directory = modulePath;
    const size_t slash = directory.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
      directory.erase(slash);
    }
  }

  CreateDirectoryW(directory.c_str(), nullptr);
  return directory + L"\\ShutdownGuard.log";
}

void AppendLogLine(const std::wstring& text) {
  SYSTEMTIME now{};
  GetLocalTime(&now);

  wchar_t prefix[64]{};
  StringCchPrintfW(prefix, ARRAYSIZE(prefix),
                   L"%04u-%02u-%02u %02u:%02u:%02u.%03u ",
                   now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                   now.wSecond, now.wMilliseconds);

  const std::string line = WideToUtf8(std::wstring(prefix) + text + L"\r\n");
  if (line.empty()) {
    return;
  }

  const std::wstring path = LogFilePath();
  HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }

  DWORD written = 0;
  WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written,
            nullptr);
  CloseHandle(file);
}

std::wstring FormatSystemError(DWORD error) {
  if (error == ERROR_SUCCESS) {
    return L"OK";
  }

  LPWSTR buffer = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

  std::wstring message = L"error " + std::to_wstring(error);
  if (length > 0 && buffer != nullptr) {
    message.assign(buffer, length);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n')) {
      message.pop_back();
    }
    message += L" (" + std::to_wstring(error) + L")";
  }

  if (buffer != nullptr) {
    LocalFree(buffer);
  }

  return message;
}

void ConfigureShutdownPriority() {
  if (SetProcessShutdownParameters(0x3FF, 0)) {
    AppendLogLine(L"SetProcessShutdownParameters(0x3FF, 0) succeeded");
    return;
  }

  const DWORD error = GetLastError();
  AppendLogLine(L"SetProcessShutdownParameters failed: " +
                FormatSystemError(error));
}

std::wstring SessionFlagsToText(LPARAM flags) {
  if (flags == 0) {
    return L"shutdown/restart";
  }

  std::wstring text;
  if ((flags & ENDSESSION_CLOSEAPP) != 0) {
    text += L"close-app ";
  }
  if ((flags & ENDSESSION_CRITICAL) != 0) {
    text += L"critical ";
  }
  if ((flags & ENDSESSION_LOGOFF) != 0) {
    text += L"logoff ";
  }
  if (text.empty()) {
    text = L"unknown";
  }

  return text;
}

void SetControlText(HWND hwnd, const std::wstring& text) {
  SetWindowTextW(hwnd, text.c_str());
}

void ApplyFont(HWND hwnd) {
  SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_state.uiFont),
               TRUE);
}

// --- Colour helpers ---
COLORREF AdjustColor(COLORREF base, float factor) {
  int r = static_cast<int>(GetRValue(base) * factor);
  int g = static_cast<int>(GetGValue(base) * factor);
  int b = static_cast<int>(GetBValue(base) * factor);
  if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
  return RGB(r, g, b);
}

COLORREF StatusDotColor() {
  if (g_state.blockingEnabled.load()) {
    return g_state.darkMode ? RGB(0x50, 0xD0, 0x50) : RGB(0x10, 0x80, 0x10);
  }
  return g_state.darkMode ? RGB(0xFF, 0x55, 0x55) : RGB(0xCC, 0x00, 0x00);
}

void ReloadDarkMode() {
  g_state.darkMode = IsSystemDarkMode();
  if (g_state.hwnd) {
    ApplyDwmAttributes(g_state.hwnd, g_state.darkMode);
  }
  if (g_state.darkBgBrush) {
    DeleteObject(g_state.darkBgBrush);
    g_state.darkBgBrush = nullptr;
  }
  if (g_state.darkMode) {
    g_state.darkBgBrush = CreateSolidBrush(RGB(0x20, 0x20, 0x20));
  }
}

HWND CreateChild(HWND parent,
                 const wchar_t* className,
                 const wchar_t* text,
                 DWORD style,
                 int id) {
  HWND child = CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style,
                               0, 0, 0, 0, parent,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               GetModuleHandleW(nullptr), nullptr);
  if (child != nullptr) {
    ApplyFont(child);
  }
  return child;
}

void UpdateUi() {
  const bool blockingEnabled = g_state.blockingEnabled.load();

  std::wstring statusText = L"  " +
      std::wstring(blockingEnabled ? L"正在拦截关机/重启" : L"未拦截");
  SetControlText(g_state.statusLabel, statusText);

  SetControlText(g_state.hookLabel,
                 std::wstring(L"  拦截钩子: ") +
                     (g_state.windowProcHookInstalled ? L"已安装"
                                                       : L"未安装") +
                     L"    观察钩子: " +
                     (g_state.callWndHook != nullptr ? L"已安装"
                                                     : L"未安装"));

  SetControlText(g_state.apiLabel,
                 std::wstring(L"  全局 API 钩子 DLL: ") +
                     (g_state.globalHookInstalled ? L"已安装" : L"未安装"));

  SetControlText(g_state.toggleButton,
                 blockingEnabled ? L"拦截已开启 — 点击关闭" : L"拦截已关闭 — 点击开启");

  // Force redraw of the owner-draw toggle
  InvalidateRect(g_state.toggleButton, nullptr, TRUE);
}

void SetLastEvent(const std::wstring& text) {
  AppendLogLine(text);
  SetControlText(g_state.lastEventLabel, L"  " + text);
}

LRESULT CALLBACK CallWndProcHook(int code, WPARAM wParam, LPARAM lParam) {
  if (code >= 0 && lParam != 0) {
    const auto* info = reinterpret_cast<const CWPSTRUCT*>(lParam);
    if (info->hwnd == g_state.hwnd &&
        (info->message == WM_QUERYENDSESSION ||
         info->message == WM_ENDSESSION)) {
      PostMessageW(g_state.hwnd, kMsgHookObserved, info->message,
                   info->lParam);
    }
  }

  return CallNextHookEx(g_state.callWndHook, code, wParam, lParam);
}

bool InstallMessageHook() {
  if (g_state.callWndHook != nullptr) {
    return true;
  }

  g_state.callWndHook = SetWindowsHookExW(WH_CALLWNDPROC, CallWndProcHook,
                                          nullptr, GetCurrentThreadId());
  if (g_state.callWndHook == nullptr) {
    g_state.lastError = GetLastError();
    SetLastEvent(L"安装消息钩子失败: " + FormatSystemError(g_state.lastError));
    return false;
  }

  return true;
}

void RemoveMessageHook() {
  if (g_state.callWndHook == nullptr) {
    return;
  }

  UnhookWindowsHookEx(g_state.callWndHook);
  g_state.callWndHook = nullptr;
}

std::wstring ExecutableDirectory() {
  wchar_t modulePath[MAX_PATH]{};
  GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
  std::wstring directory = modulePath;
  const size_t slash = directory.find_last_of(L"\\/");
  if (slash != std::wstring::npos) {
    directory.erase(slash);
  }
  return directory;
}

std::wstring GlobalHookDllPath() {
  return ExecutableDirectory() + L"\\ShutdownGuardHook.dll";
}

template <typename T>
bool LoadExport(T& target, HMODULE module, const char* name) {
  target = reinterpret_cast<T>(GetProcAddress(module, name));
  return target != nullptr;
}

std::vector<DWORD> FindProcessIdsByName(const wchar_t* processName) {
  std::vector<DWORD> pids;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return pids;
  }

  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (lstrcmpiW(entry.szExeFile, processName) == 0) {
        pids.push_back(entry.th32ProcessID);
      }
    } while (Process32NextW(snapshot, &entry));
  }

  CloseHandle(snapshot);
  return pids;
}

bool IsModuleLoadedInProcess(DWORD pid, const wchar_t* moduleName) {
  HANDLE snapshot =
      CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return false;
  }

  bool loaded = false;
  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Module32FirstW(snapshot, &entry)) {
    do {
      if (lstrcmpiW(entry.szModule, moduleName) == 0) {
        loaded = true;
        break;
      }
    } while (Module32NextW(snapshot, &entry));
  }

  CloseHandle(snapshot);
  return loaded;
}

bool InjectDllIntoProcess(DWORD pid, const std::wstring& dllPath) {
  if (pid == GetCurrentProcessId()) {
    return true;
  }

  HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                   PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                                   PROCESS_VM_READ,
                               FALSE, pid);
  if (process == nullptr) {
    AppendLogLine(L"OpenProcess failed for pid " + std::to_wstring(pid) +
                  L": " + FormatSystemError(GetLastError()));
    return false;
  }

  const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
  void* remotePath =
      VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                     PAGE_READWRITE);
  if (remotePath == nullptr) {
    AppendLogLine(L"VirtualAllocEx failed for pid " + std::to_wstring(pid) +
                  L": " + FormatSystemError(GetLastError()));
    CloseHandle(process);
    return false;
  }

  BOOL ok = WriteProcessMemory(process, remotePath, dllPath.c_str(), bytes,
                               nullptr);
  if (!ok) {
    AppendLogLine(L"WriteProcessMemory failed for pid " + std::to_wstring(pid) +
                  L": " + FormatSystemError(GetLastError()));
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);
    return false;
  }

  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  auto loadLibraryW =
      reinterpret_cast<LPTHREAD_START_ROUTINE>(
          GetProcAddress(kernel32, "LoadLibraryW"));
  if (loadLibraryW == nullptr) {
    AppendLogLine(L"GetProcAddress(LoadLibraryW) failed");
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);
    return false;
  }

  HANDLE thread =
      CreateRemoteThread(process, nullptr, 0, loadLibraryW, remotePath, 0,
                         nullptr);
  if (thread == nullptr) {
    AppendLogLine(L"CreateRemoteThread failed for pid " + std::to_wstring(pid) +
                  L": " + FormatSystemError(GetLastError()));
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);
    return false;
  }

  const DWORD waitResult = WaitForSingleObject(thread, 7000);
  DWORD exitCode = 0;
  GetExitCodeThread(thread, &exitCode);

  CloseHandle(thread);
  VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
  CloseHandle(process);

  if (waitResult != WAIT_OBJECT_0 || exitCode == 0) {
    AppendLogLine(L"Remote LoadLibraryW did not confirm success for pid " +
                  std::to_wstring(pid));
    return false;
  }

  return true;
}

struct InjectionSummary {
  int targets = 0;
  int alreadyLoaded = 0;
  int injected = 0;
  int failed = 0;
};

InjectionSummary InjectHookDllIntoShellProcesses() {
  constexpr const wchar_t* kTargets[] = {
      L"explorer.exe",
      L"StartMenuExperienceHost.exe",
      L"ShellExperienceHost.exe",
      L"sihost.exe",
      L"RuntimeBroker.exe",
      L"ApplicationFrameHost.exe",
      L"cmd.exe",
      L"powershell.exe",
      L"pwsh.exe",
      L"WindowsTerminal.exe",
      L"wt.exe",
      L"OpenConsole.exe",
      L"wmic.exe",
      L"rundll32.exe",
  };

  InjectionSummary summary{};
  const std::wstring dllPath = GlobalHookDllPath();
  constexpr wchar_t kDllName[] = L"ShutdownGuardHook.dll";

  for (const wchar_t* target : kTargets) {
    const std::vector<DWORD> pids = FindProcessIdsByName(target);
    for (DWORD pid : pids) {
      ++summary.targets;
      if (IsModuleLoadedInProcess(pid, kDllName)) {
        ++summary.alreadyLoaded;
        AppendLogLine(std::wstring(L"Hook DLL already loaded in ") + target +
                      L" pid " + std::to_wstring(pid));
        continue;
      }

      if (InjectDllIntoProcess(pid, dllPath)) {
        ++summary.injected;
        AppendLogLine(std::wstring(L"Injected Hook DLL into ") + target +
                      L" pid " + std::to_wstring(pid));
      } else {
        ++summary.failed;
        AppendLogLine(std::wstring(L"Failed to inject Hook DLL into ") +
                      target + L" pid " + std::to_wstring(pid));
      }
    }
  }

  return summary;
}

void UnloadGlobalHookDll() {
  if (g_state.globalHookModule != nullptr) {
    FreeLibrary(g_state.globalHookModule);
    g_state.globalHookModule = nullptr;
  }

  g_state.installGlobalHook = nullptr;
  g_state.removeGlobalHook = nullptr;
  g_state.setGlobalBlocking = nullptr;
  g_state.isGlobalBlocking = nullptr;
  g_state.getGlobalBlockedCount = nullptr;
  g_state.getGlobalHookLastError = nullptr;
}

bool LoadGlobalHookDll() {
  if (g_state.globalHookModule != nullptr) {
    return true;
  }

  const std::wstring dllPath = GlobalHookDllPath();
  g_state.globalHookModule = LoadLibraryW(dllPath.c_str());
  if (g_state.globalHookModule == nullptr) {
    g_state.lastError = GetLastError();
    SetLastEvent(L"加载 ShutdownGuardHook.dll 失败: " +
                 FormatSystemError(g_state.lastError));
    return false;
  }

  const bool exportsOk =
      LoadExport(g_state.installGlobalHook, g_state.globalHookModule,
                 "InstallGlobalShutdownHook") &&
      LoadExport(g_state.removeGlobalHook, g_state.globalHookModule,
                 "RemoveGlobalShutdownHook") &&
      LoadExport(g_state.setGlobalBlocking, g_state.globalHookModule,
                 "SetGlobalShutdownBlocking") &&
      LoadExport(g_state.isGlobalBlocking, g_state.globalHookModule,
                 "IsGlobalShutdownBlocking") &&
      LoadExport(g_state.getGlobalBlockedCount, g_state.globalHookModule,
                 "GetGlobalShutdownBlockedCount") &&
      LoadExport(g_state.getGlobalHookLastError, g_state.globalHookModule,
                 "GetGlobalShutdownHookLastError");

  if (!exportsOk) {
    g_state.lastError = GetLastError();
    SetLastEvent(L"解析 ShutdownGuardHook.dll 导出函数失败");
    UnloadGlobalHookDll();
    return false;
  }

  return true;
}

bool InstallGlobalApiHook() {
  if (g_state.globalHookInstalled) {
    return true;
  }

  if (!LoadGlobalHookDll()) {
    UpdateUi();
    return false;
  }

  if (!g_state.installGlobalHook()) {
    g_state.lastError = g_state.getGlobalHookLastError != nullptr
                            ? g_state.getGlobalHookLastError()
                            : GetLastError();
    SetLastEvent(L"安装全局 API 钩子失败: " +
                 FormatSystemError(g_state.lastError));
    UnloadGlobalHookDll();
    UpdateUi();
    return false;
  }

  if (!g_state.setGlobalBlocking(TRUE)) {
    g_state.lastError = GetLastError();
    SetLastEvent(L"启用全局拦截状态失败: " +
                 FormatSystemError(g_state.lastError));
    g_state.removeGlobalHook();
    UnloadGlobalHookDll();
    UpdateUi();
    return false;
  }

  g_state.globalHookInstalled = true;
  const InjectionSummary summary = InjectHookDllIntoShellProcesses();
  SetLastEvent(L"全局 API 钩子已安装；目标进程 " +
               std::to_wstring(summary.targets) + L"，新注入 " +
               std::to_wstring(summary.injected) + L"，已存在 " +
               std::to_wstring(summary.alreadyLoaded) + L"，失败 " +
               std::to_wstring(summary.failed));
  return true;
}

void RemoveGlobalApiHook() {
  if (g_state.setGlobalBlocking != nullptr) {
    g_state.setGlobalBlocking(FALSE);
  }

  if (g_state.globalHookInstalled && g_state.removeGlobalHook != nullptr) {
    if (!g_state.removeGlobalHook()) {
      g_state.lastError = g_state.getGlobalHookLastError != nullptr
                              ? g_state.getGlobalHookLastError()
                              : GetLastError();
      SetLastEvent(L"卸载全局 API 钩子失败: " +
                   FormatSystemError(g_state.lastError));
    }
  }

  g_state.globalHookInstalled = false;
  UnloadGlobalHookDll();
}

bool EnableBlocking() {
  if (g_state.blockingEnabled.load()) {
    return true;
  }

  if (!InstallMessageHook()) {
    UpdateUi();
    return false;
  }

  if (!ShutdownBlockReasonCreate(g_state.hwnd, kBlockReason)) {
    g_state.lastError = GetLastError();
    SetLastEvent(L"设置关机阻止原因失败: " +
                 FormatSystemError(g_state.lastError));
    RemoveMessageHook();
    UpdateUi();
    return false;
  }
  g_state.shutdownReasonCreated = true;

  if (!InstallGlobalApiHook()) {
    ShutdownBlockReasonDestroy(g_state.hwnd);
    g_state.shutdownReasonCreated = false;
    RemoveMessageHook();
    UpdateUi();
    return false;
  }

  g_state.blockingEnabled.store(true);
  AppendLogLine(L"拦截已开启");
  UpdateUi();
  return true;
}

void DisableBlocking() {
  if (!g_state.blockingEnabled.load() && !g_state.shutdownReasonCreated) {
    RemoveMessageHook();
    RemoveGlobalApiHook();
    UpdateUi();
    return;
  }

  g_state.blockingEnabled.store(false);
  RemoveGlobalApiHook();

  if (g_state.shutdownReasonCreated) {
    ShutdownBlockReasonDestroy(g_state.hwnd);
    g_state.shutdownReasonCreated = false;
  }

  RemoveMessageHook();
  SetLastEvent(L"拦截已关闭");
  UpdateUi();
}

int DpiScale(int value, HWND hwnd) {
  return MulDiv(value, GetDpiForWindow(hwnd), 96);
}

void LayoutControls(HWND hwnd) {
  RECT client{};
  GetClientRect(hwnd, &client);

  const int dpi = GetDpiForWindow(hwnd);
  const int margin = MulDiv(20, dpi, 96);
  const int gap = MulDiv(12, dpi, 96);
  const int rowH = MulDiv(24, dpi, 96);
  const int width = client.right - client.left - margin * 2;

  int y = MulDiv(16, dpi, 96);

  // Status label with colored dot
  MoveWindow(g_state.statusLabel, margin, y, width, rowH, TRUE);
  y += rowH + gap;

  // Hook status
  MoveWindow(g_state.hookLabel, margin, y, width, rowH, TRUE);
  y += rowH + gap;

  // API status
  MoveWindow(g_state.apiLabel, margin, y, width, rowH, TRUE);
  y += rowH + MulDiv(8, dpi, 96);

  // Toggle switch area — centered
  const int toggleW = MulDiv(180, dpi, 96);
  const int toggleH = MulDiv(44, dpi, 96);
  const int toggleX = (client.right - client.left - toggleW) / 2;
  MoveWindow(g_state.toggleButton, toggleX, y, toggleW, toggleH, TRUE);
  y += toggleH + gap;

  // Last event
  MoveWindow(g_state.lastEventLabel, margin, y, width,
             MulDiv(32, dpi, 96), TRUE);
  y += MulDiv(36, dpi, 96) + gap;

  // Bottom buttons — right aligned
  const int btnW = MulDiv(100, dpi, 96);
  const int btnH = MulDiv(32, dpi, 96);
  MoveWindow(g_state.closeButton, client.right - margin - btnW,
             client.bottom - margin - btnH, btnW, btnH, TRUE);
  MoveWindow(g_state.selfTestButton,
             client.right - margin * 2 - btnW * 2,
             client.bottom - margin - btnH, btnW, btnH, TRUE);
}

// --- UI Creation (modernised) ---

// Custom paint for the status dot
void DrawStatusDot(HDC hdc, int x, int y, int size) {
  const COLORREF color = StatusDotColor();
  const HBRUSH brush = CreateSolidBrush(color);
  const HGDIOBJ old = SelectObject(hdc, brush);
  const HPEN pen = CreatePen(PS_NULL, 0, 0);
  const HGDIOBJ oldPen = SelectObject(hdc, pen);
  Ellipse(hdc, x, y, x + size, y + size);
  SelectObject(hdc, oldPen);
  SelectObject(hdc, old);
  DeleteObject(pen);
  DeleteObject(brush);
}

void CreateUi(HWND hwnd) {
  ReloadDarkMode();

  // Modern Segoe UI font
  g_state.uiFont = CreateModernFont(hwnd, 11);

  g_state.statusLabel =
      CreateChild(hwnd, L"STATIC", L"",
                  SS_LEFT | SS_NOPREFIX | SS_OWNERDRAW, kIdStatus);
  g_state.hookLabel =
      CreateChild(hwnd, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, kIdHookStatus);
  g_state.apiLabel =
      CreateChild(hwnd, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, kIdApiStatus);
  g_state.lastEventLabel =
      CreateChild(hwnd, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, kIdLastEvent);
  g_state.toggleButton =
      CreateChild(hwnd, L"BUTTON", L"",
                  BS_OWNERDRAW | BS_NOTIFY, kIdToggle);

  // Use flat-styled buttons
  g_state.selfTestButton =
      CreateChild(hwnd, L"BUTTON", L"Self test",
                  BS_PUSHBUTTON | BS_FLAT, kIdSelfTest);
  g_state.closeButton =
      CreateChild(hwnd, L"BUTTON", L"退出",
                  BS_PUSHBUTTON | BS_FLAT, kIdClose);

  SetLastEvent(L"应用已启动");
  UpdateUi();
  LayoutControls(hwnd);
}

LRESULT HandleQueryEndSession(LPARAM flags) {
  const std::wstring flagText = SessionFlagsToText(flags);
  if (g_state.blockingEnabled.load()) {
    AbortSystemShutdownW(nullptr);
    SetLastEvent(L"钩子拦截到 " + flagText + L" 请求，已阻止");
    return FALSE;
  }

  SetLastEvent(L"钩子拦截到 " + flagText + L" 请求，允许继续");
  return TRUE;
}

LRESULT CALLBACK HookedWindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                  LPARAM lParam) {
  switch (message) {
    case WM_QUERYENDSESSION:
      AppendLogLine(L"HookedWindowProc intercepted WM_QUERYENDSESSION");
      return HandleQueryEndSession(lParam);

    case WM_ENDSESSION:
      AppendLogLine(L"HookedWindowProc intercepted WM_ENDSESSION");
      SetLastEvent(std::wstring(L"WM_ENDSESSION: ") +
                   (wParam == TRUE ? L"会话正在结束" : L"会话结束已取消"));
      return 0;

    case WM_NCDESTROY: {
      WNDPROC originalWindowProc = g_state.originalWindowProc;
      if (g_state.windowProcHookInstalled && originalWindowProc != nullptr) {
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(originalWindowProc));
        g_state.windowProcHookInstalled = false;
        g_state.originalWindowProc = nullptr;
        return CallWindowProcW(originalWindowProc, hwnd, message, wParam,
                               lParam);
      }
      break;
    }

    default:
      break;
  }

  if (g_state.originalWindowProc != nullptr) {
    return CallWindowProcW(g_state.originalWindowProc, hwnd, message, wParam,
                           lParam);
  }

  return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool InstallWindowProcedureHook(HWND hwnd) {
  if (g_state.windowProcHookInstalled) {
    return true;
  }

  SetLastError(ERROR_SUCCESS);
  const LONG_PTR previous = SetWindowLongPtrW(
      hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWindowProc));
  if (previous == 0 && GetLastError() != ERROR_SUCCESS) {
    g_state.lastError = GetLastError();
    SetLastEvent(L"安装窗口过程拦截钩子失败: " +
                 FormatSystemError(g_state.lastError));
    UpdateUi();
    return false;
  }

  g_state.originalWindowProc = reinterpret_cast<WNDPROC>(previous);
  g_state.windowProcHookInstalled = true;
  SetLastEvent(L"窗口过程拦截钩子已安装");
  UpdateUi();
  return true;
}

/// Draw the status-label background and dot
void DrawStatusLabel(const DRAWITEMSTRUCT& dis) {
  HDC dc = dis.hDC;
  RECT rc = dis.rcItem;

  const COLORREF bg = g_state.darkMode ? RGB(0x20, 0x20, 0x20)
                                       : GetSysColor(COLOR_WINDOW);
  const COLORREF fg = g_state.darkMode ? RGB(0xE0, 0xE0, 0xE0)
                                       : RGB(0x1A, 0x1A, 0x1A);

  SetBkColor(dc, bg);
  SetTextColor(dc, fg);
  HBRUSH bgBrush = CreateSolidBrush(bg);
  FillRect(dc, &rc, bgBrush);
  DeleteObject(bgBrush);

  // Dot
  const int dotSize = 10;
  const int dotY = (rc.bottom - rc.top - dotSize) / 2;
  DrawStatusDot(dc, rc.left + 6, rc.top + dotY, dotSize);

  // Text
  const wchar_t* text = g_state.blockingEnabled.load()
                            ? L"正在拦截关机/重启"
                            : L"未拦截";
  rc.left += dotSize + 14;
  DrawTextW(dc, text, -1, &rc,
            DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
}

LRESULT CALLBACK BaseWindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                LPARAM lParam) {
  switch (message) {
    case WM_CREATE:
      g_state.hwnd = hwnd;
      CreateUi(hwnd);
      AddTrayIcon(hwnd);
      g_state.trayIconAdded = true;
      return 0;

    case WM_SIZE:
      LayoutControls(hwnd);
      return 0;

    case WM_COMMAND: {
      const int id = LOWORD(wParam);
      if (id == kIdToggle) {
        if (g_state.blockingEnabled.load()) {
          DisableBlocking();
        } else {
          EnableBlocking();
        }
        return 0;
      }
      if (id == kIdSelfTest) {
        const LRESULT result = SendMessageW(hwnd, WM_QUERYENDSESSION, 0, 0);
        SetLastEvent(std::wstring(L"Self test result: ") +
                     (result == FALSE ? L"blocked" : L"allowed"));
        return 0;
      }
      if (id == kIdClose) {
        // Minimise to tray instead of destroy
        ShowWindow(hwnd, SW_HIDE);
        ShowTrayBalloon(hwnd, L"Shutdown Guard",
                        L"仍在运行，拦截功能保持启用。\n右键托盘图标可选择退出。");
        return 0;
      }
      if (id == kIdTrayShow) {
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        return 0;
      }
      if (id == kIdTrayExit) {
        DisableBlocking();
        DestroyWindow(hwnd);
        return 0;
      }
      break;
    }

    case kMsgHookObserved:
      if (wParam == WM_QUERYENDSESSION) {
        SetLastEvent(L"消息钩子观察到 WM_QUERYENDSESSION (" +
                     SessionFlagsToText(lParam) + L")");
      } else if (wParam == WM_ENDSESSION) {
        SetLastEvent(L"消息钩子观察到 WM_ENDSESSION");
      }
      return 0;

    case WM_CLOSE:
      // Minimise to tray
      ShowWindow(hwnd, SW_HIDE);
      ShowTrayBalloon(hwnd, L"Shutdown Guard",
                      L"仍在运行，拦截功能保持启用。\n右键托盘图标可选择退出。");
      return 0;

    case WM_ENDSESSION:
      if (wParam) {
        RemoveTrayIcon(hwnd);
        g_state.trayIconAdded = false;
      }
      break;

    case WM_DESTROY:
      DisableBlocking();
      if (g_state.trayIconAdded) {
        RemoveTrayIcon(hwnd);
        g_state.trayIconAdded = false;
      }
      if (g_state.uiFont != nullptr) {
        DeleteObject(g_state.uiFont);
        g_state.uiFont = nullptr;
      }
      if (g_state.darkBgBrush != nullptr) {
        DeleteObject(g_state.darkBgBrush);
        g_state.darkBgBrush = nullptr;
      }
      PostQuitMessage(0);
      return 0;

    // --- Tray notifications ---
    case WM_APP + 2:   // kMsgTrayNotify
      switch (LOWORD(lParam)) {
        case WM_RBUTTONUP: {
          const UINT cmd = ShowTrayContextMenu(hwnd);
          if (cmd == kIdTrayShow) {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
          } else if (cmd == kIdTrayExit) {
            DisableBlocking();
            DestroyWindow(hwnd);
          }
          return 0;
        }
        case WM_LBUTTONDBLCLK:
          ShowWindow(hwnd, SW_SHOW);
          SetForegroundWindow(hwnd);
          return 0;
      }
      break;

    // --- CTLCOLOR for dark mode ---
    case WM_CTLCOLORSTATIC: {
      if (!g_state.darkMode) break;
      HDC hdc = reinterpret_cast<HDC>(wParam);
      SetTextColor(hdc, RGB(0xE0, 0xE0, 0xE0));
      SetBkColor(hdc, RGB(0x20, 0x20, 0x20));
      if (!g_state.darkBgBrush) {
        g_state.darkBgBrush = CreateSolidBrush(RGB(0x20, 0x20, 0x20));
      }
      return reinterpret_cast<LRESULT>(g_state.darkBgBrush);
    }

    case WM_CTLCOLORBTN: {
      if (!g_state.darkMode) break;
      HDC hdc = reinterpret_cast<HDC>(wParam);
      SetTextColor(hdc, RGB(0xE0, 0xE0, 0xE0));
      SetBkColor(hdc, RGB(0x2D, 0x2D, 0x2D));
      static HBRUSH btnBrush = CreateSolidBrush(RGB(0x2D, 0x2D, 0x2D));
      return reinterpret_cast<LRESULT>(btnBrush);
    }

    // --- Owner-draw ---
    case WM_DRAWITEM: {
      const auto& dis = *reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
      if (dis.CtlID == kIdToggle) {
        DrawToggleSwitch(dis, g_state.blockingEnabled.load(),
                         g_state.toggleHovered, g_state.togglePressed);
        return TRUE;
      }
      if (dis.CtlID == kIdStatus) {
        DrawStatusLabel(dis);
        return TRUE;
      }
      break;
    }

    // --- Theme change ---
    case WM_SETTINGCHANGE:
      if (lParam && lstrcmpW(reinterpret_cast<LPCWSTR>(lParam),
                             L"ImmersiveColorSet") == 0) {
        ReloadDarkMode();
        InvalidateRect(hwnd, nullptr, TRUE);
        UpdateUi();
        LayoutControls(hwnd);
      }
      break;

    // --- Make toggle respond to keyboard ---
    case WM_GETDLGCODE:
      if (wParam) {
        const HWND ctrl = reinterpret_cast<HWND>(lParam);
        if (ctrl == g_state.toggleButton) {
          return DLGC_WANTALLKEYS;
        }
      }
      break;

    // --- Toggle hover tracking ---
    case WM_MOUSEMOVE: {
      POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      RECT rc{};
      GetWindowRect(g_state.toggleButton, &rc);
      MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&rc), 2);
      bool wasHovered = g_state.toggleHovered;
      g_state.toggleHovered = PtInRect(&rc, pt);
      if (wasHovered != g_state.toggleHovered) {
        InvalidateRect(g_state.toggleButton, nullptr, FALSE);
      }
      break;
    }

    default:
      break;
  }

  // TaskbarCreated (value resolved at runtime)
  if (message == g_uTaskbarCreated) {
    AddTrayIcon(hwnd);
    g_state.trayIconAdded = true;
    return 0;
  }

  return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool RegisterMainWindowClass(HINSTANCE instance) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = BaseWindowProc;
  wc.hInstance = instance;
  wc.hIcon = LoadIconW(nullptr, IDI_SHIELD);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kWindowClass;
  wc.hIconSm = LoadIconW(nullptr, IDI_SHIELD);

  return RegisterClassExW(&wc) != 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
  ConfigureShutdownPriority();

  if (!RegisterMainWindowClass(instance)) {
    MessageBoxW(nullptr, L"注册窗口类失败。", kWindowTitle, MB_ICONERROR);
    return 1;
  }

  HWND hwnd = CreateWindowExW(0, kWindowClass, kWindowTitle,
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                  WS_MINIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT, 460, 370, nullptr,
                              nullptr, instance, nullptr);
  if (hwnd == nullptr) {
    MessageBoxW(nullptr, L"创建主窗口失败。", kWindowTitle, MB_ICONERROR);
    return 1;
  }

  // Apply modern DWM attributes (best-effort)
  const bool darkMode = IsSystemDarkMode();
  ApplyDwmAttributes(hwnd, darkMode);

  if (!InstallWindowProcedureHook(hwnd)) {
    MessageBoxW(hwnd, L"安装窗口过程拦截钩子失败。", kWindowTitle,
                MB_ICONERROR);
    DestroyWindow(hwnd);
    return 1;
  }

  ShowWindow(hwnd, showCommand);
  UpdateWindow(hwnd);
  EnableBlocking();

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  return static_cast<int>(message.wParam);
}
