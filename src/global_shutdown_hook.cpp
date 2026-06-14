#include <windows.h>
#include <shellapi.h>
#include <wtsapi32.h>

#include <detours.h>

#include <cctype>
#include <cwctype>
#include <string>

using NTSTATUS = LONG;

namespace {

constexpr wchar_t kStateName[] = L"Local\\ShutdownGuard.GlobalHookState";

struct HookState {
  volatile LONG enabled;
  volatile LONG blockedCalls;
};

using ExitWindowsExFn = BOOL(WINAPI *)(UINT, DWORD);
using InitiateShutdownWFn = DWORD(WINAPI *)(LPWSTR, LPWSTR, DWORD, DWORD, DWORD);
using InitiateShutdownAFn = DWORD(WINAPI *)(LPSTR, LPSTR, DWORD, DWORD, DWORD);
using InitiateSystemShutdownWFn =
    BOOL(WINAPI *)(LPWSTR, LPWSTR, DWORD, BOOL, BOOL);
using InitiateSystemShutdownAFn =
    BOOL(WINAPI *)(LPSTR, LPSTR, DWORD, BOOL, BOOL);
using InitiateSystemShutdownExWFn =
    BOOL(WINAPI *)(LPWSTR, LPWSTR, DWORD, BOOL, BOOL, DWORD);
using InitiateSystemShutdownExAFn =
    BOOL(WINAPI *)(LPSTR, LPSTR, DWORD, BOOL, BOOL, DWORD);
using NtShutdownSystemFn = NTSTATUS(WINAPI *)(int);
using NtInitiatePowerActionFn = NTSTATUS(WINAPI *)(POWER_ACTION,
                                                   SYSTEM_POWER_STATE,
                                                   ULONG,
                                                   BOOLEAN);
using CreateProcessWFn = BOOL(WINAPI *)(LPCWSTR,
                                         LPWSTR,
                                         LPSECURITY_ATTRIBUTES,
                                         LPSECURITY_ATTRIBUTES,
                                         BOOL,
                                         DWORD,
                                         LPVOID,
                                         LPCWSTR,
                                         LPSTARTUPINFOW,
                                         LPPROCESS_INFORMATION);
using CreateProcessAFn = BOOL(WINAPI *)(LPCSTR,
                                         LPSTR,
                                         LPSECURITY_ATTRIBUTES,
                                         LPSECURITY_ATTRIBUTES,
                                         BOOL,
                                         DWORD,
                                         LPVOID,
                                         LPCSTR,
                                         LPSTARTUPINFOA,
                                         LPPROCESS_INFORMATION);
using ShellExecuteExWFn = BOOL(WINAPI *)(SHELLEXECUTEINFOW*);
using ShellExecuteExAFn = BOOL(WINAPI *)(SHELLEXECUTEINFOA*);
using ShellExecuteWFn = HINSTANCE(WINAPI *)(HWND,
                                            LPCWSTR,
                                            LPCWSTR,
                                            LPCWSTR,
                                            LPCWSTR,
                                            INT);
using ShellExecuteAFn = HINSTANCE(WINAPI *)(HWND,
                                            LPCSTR,
                                            LPCSTR,
                                            LPCSTR,
                                            LPCSTR,
                                            INT);
using WinExecFn = UINT(WINAPI *)(LPCSTR, UINT);
using WTSLogoffSessionFn = BOOL(WINAPI *)(HANDLE, DWORD, BOOL);
using WTSShutdownSystemFn = BOOL(WINAPI *)(HANDLE, DWORD);

HMODULE g_module = nullptr;
HHOOK g_hook = nullptr;
HANDLE g_stateMapping = nullptr;
HookState* g_state = nullptr;
bool g_apiHooksInstalled = false;
DWORD g_lastError = ERROR_SUCCESS;

ExitWindowsExFn g_realExitWindowsEx = ::ExitWindowsEx;
InitiateShutdownWFn g_realInitiateShutdownW = ::InitiateShutdownW;
InitiateShutdownAFn g_realInitiateShutdownA = ::InitiateShutdownA;
InitiateSystemShutdownWFn g_realInitiateSystemShutdownW =
    ::InitiateSystemShutdownW;
InitiateSystemShutdownAFn g_realInitiateSystemShutdownA =
    ::InitiateSystemShutdownA;
InitiateSystemShutdownExWFn g_realInitiateSystemShutdownExW =
    ::InitiateSystemShutdownExW;
InitiateSystemShutdownExAFn g_realInitiateSystemShutdownExA =
    ::InitiateSystemShutdownExA;
NtShutdownSystemFn g_realNtShutdownSystem = nullptr;
NtInitiatePowerActionFn g_realNtInitiatePowerAction = nullptr;
CreateProcessWFn g_realCreateProcessW = ::CreateProcessW;
CreateProcessAFn g_realCreateProcessA = ::CreateProcessA;
ShellExecuteExWFn g_realShellExecuteExW = ::ShellExecuteExW;
ShellExecuteExAFn g_realShellExecuteExA = ::ShellExecuteExA;
ShellExecuteWFn g_realShellExecuteW = ::ShellExecuteW;
ShellExecuteAFn g_realShellExecuteA = ::ShellExecuteA;
WinExecFn g_realWinExec = ::WinExec;
WTSLogoffSessionFn g_realWTSLogoffSession = ::WTSLogoffSession;
WTSShutdownSystemFn g_realWTSShutdownSystem = ::WTSShutdownSystem;

bool EnsureState() {
  if (g_state != nullptr) {
    return true;
  }

  g_stateMapping =
      CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                         sizeof(HookState), kStateName);
  if (g_stateMapping == nullptr) {
    g_lastError = GetLastError();
    return false;
  }

  g_state = static_cast<HookState*>(
      MapViewOfFile(g_stateMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HookState)));
  if (g_state == nullptr) {
    g_lastError = GetLastError();
    CloseHandle(g_stateMapping);
    g_stateMapping = nullptr;
    return false;
  }

  return true;
}

void CloseState() {
  if (g_state != nullptr) {
    UnmapViewOfFile(g_state);
    g_state = nullptr;
  }
  if (g_stateMapping != nullptr) {
    CloseHandle(g_stateMapping);
    g_stateMapping = nullptr;
  }
}

bool BlockingEnabled() {
  return EnsureState() &&
         InterlockedCompareExchange(&g_state->enabled, 0, 0) != 0;
}

bool IsSpace(wchar_t ch) {
  return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

bool IsSpaceA(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

std::wstring ToLower(std::wstring value) {
  for (wchar_t& ch : value) {
    ch = static_cast<wchar_t>(std::towlower(ch));
  }
  return value;
}

std::string ToLowerA(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(
        std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

bool ContainsI(const std::wstring& value, const wchar_t* needle) {
  return ToLower(value).find(ToLower(needle)) != std::wstring::npos;
}

bool ContainsIA(const std::string& value, const char* needle) {
  return ToLowerA(value).find(ToLowerA(needle)) != std::string::npos;
}

std::wstring SafeString(const wchar_t* value) {
  return value != nullptr ? std::wstring(value) : std::wstring();
}

std::string SafeStringA(const char* value) {
  return value != nullptr ? std::string(value) : std::string();
}

std::wstring JoinCommandLine(const wchar_t* first, const wchar_t* second) {
  std::wstring joined = SafeString(first);
  if (second != nullptr && *second != L'\0') {
    if (!joined.empty()) {
      joined += L' ';
    }
    joined += second;
  }
  return joined;
}

std::string JoinCommandLineA(const char* first, const char* second) {
  std::string joined = SafeStringA(first);
  if (second != nullptr && *second != '\0') {
    if (!joined.empty()) {
      joined += ' ';
    }
    joined += second;
  }
  return joined;
}

const wchar_t* BaseName(const wchar_t* path) {
  if (path == nullptr) {
    return L"";
  }

  const wchar_t* base = path;
  for (const wchar_t* cursor = path; *cursor != L'\0'; ++cursor) {
    if (*cursor == L'\\' || *cursor == L'/' || *cursor == L':') {
      base = cursor + 1;
    }
  }
  return base;
}

const char* BaseNameA(const char* path) {
  if (path == nullptr) {
    return "";
  }

  const char* base = path;
  for (const char* cursor = path; *cursor != '\0'; ++cursor) {
    if (*cursor == '\\' || *cursor == '/' || *cursor == ':') {
      base = cursor + 1;
    }
  }
  return base;
}

bool IsShutdownExecutableName(const wchar_t* value) {
  const wchar_t* base = BaseName(value);
  return lstrcmpiW(base, L"shutdown") == 0 ||
         lstrcmpiW(base, L"shutdown.exe") == 0;
}

bool IsShutdownExecutableNameA(const char* value) {
  const char* base = BaseNameA(value);
  return lstrcmpiA(base, "shutdown") == 0 ||
         lstrcmpiA(base, "shutdown.exe") == 0;
}

bool IsLogoffExecutableName(const wchar_t* value) {
  const wchar_t* base = BaseName(value);
  return lstrcmpiW(base, L"logoff") == 0 ||
         lstrcmpiW(base, L"logoff.exe") == 0;
}

bool IsLogoffExecutableNameA(const char* value) {
  const char* base = BaseNameA(value);
  return lstrcmpiA(base, "logoff") == 0 ||
         lstrcmpiA(base, "logoff.exe") == 0;
}

bool IsDirectlyBlockedExecutableName(const wchar_t* value) {
  const wchar_t* base = BaseName(value);
  return IsLogoffExecutableName(value) ||
         lstrcmpiW(base, L"slidetoshutdown.exe") == 0 ||
         lstrcmpiW(base, L"psshutdown.exe") == 0;
}

bool IsDirectlyBlockedExecutableNameA(const char* value) {
  const char* base = BaseNameA(value);
  return IsLogoffExecutableNameA(value) ||
         lstrcmpiA(base, "slidetoshutdown.exe") == 0 ||
         lstrcmpiA(base, "psshutdown.exe") == 0;
}

bool IsCommandHostExecutableName(const wchar_t* value) {
  const wchar_t* base = BaseName(value);
  return lstrcmpiW(base, L"cmd.exe") == 0 ||
         lstrcmpiW(base, L"powershell.exe") == 0 ||
         lstrcmpiW(base, L"pwsh.exe") == 0 ||
         lstrcmpiW(base, L"WindowsTerminal.exe") == 0 ||
         lstrcmpiW(base, L"wt.exe") == 0 ||
         lstrcmpiW(base, L"OpenConsole.exe") == 0 ||
         lstrcmpiW(base, L"wmic.exe") == 0 ||
         lstrcmpiW(base, L"rundll32.exe") == 0 ||
         lstrcmpiW(base, L"nircmd.exe") == 0;
}

bool IsCommandHostExecutableNameA(const char* value) {
  const char* base = BaseNameA(value);
  return lstrcmpiA(base, "cmd.exe") == 0 ||
         lstrcmpiA(base, "powershell.exe") == 0 ||
         lstrcmpiA(base, "pwsh.exe") == 0 ||
         lstrcmpiA(base, "WindowsTerminal.exe") == 0 ||
         lstrcmpiA(base, "wt.exe") == 0 ||
         lstrcmpiA(base, "OpenConsole.exe") == 0 ||
         lstrcmpiA(base, "wmic.exe") == 0 ||
         lstrcmpiA(base, "rundll32.exe") == 0 ||
         lstrcmpiA(base, "nircmd.exe") == 0;
}

bool FirstCommandTokenMatches(const wchar_t* commandLine,
                              bool (*predicate)(const wchar_t*)) {
  if (commandLine == nullptr) {
    return false;
  }

  const wchar_t* cursor = commandLine;
  while (IsSpace(*cursor)) {
    ++cursor;
  }

  wchar_t token[MAX_PATH]{};
  size_t index = 0;
  if (*cursor == L'"') {
    ++cursor;
    while (*cursor != L'\0' && *cursor != L'"' && index + 1 < ARRAYSIZE(token)) {
      token[index++] = *cursor++;
    }
  } else {
    while (*cursor != L'\0' && !IsSpace(*cursor) &&
           index + 1 < ARRAYSIZE(token)) {
      token[index++] = *cursor++;
    }
  }

  token[index] = L'\0';
  return predicate(token);
}

bool FirstCommandTokenMatchesA(const char* commandLine,
                               bool (*predicate)(const char*)) {
  if (commandLine == nullptr) {
    return false;
  }

  const char* cursor = commandLine;
  while (IsSpaceA(*cursor)) {
    ++cursor;
  }

  char token[MAX_PATH]{};
  size_t index = 0;
  if (*cursor == '"') {
    ++cursor;
    while (*cursor != '\0' && *cursor != '"' && index + 1 < ARRAYSIZE(token)) {
      token[index++] = *cursor++;
    }
  } else {
    while (*cursor != '\0' && !IsSpaceA(*cursor) &&
           index + 1 < ARRAYSIZE(token)) {
      token[index++] = *cursor++;
    }
  }

  token[index] = '\0';
  return predicate(token);
}

bool ContainsShutdownAbortOnly(const std::wstring& line) {
  if (!ContainsI(line, L"shutdown")) {
    return false;
  }
  if (!ContainsI(line, L"/a") && !ContainsI(line, L"-a")) {
    return false;
  }

  constexpr const wchar_t* kDestructive[] = {
      L"/s", L"-s", L"/r", L"-r", L"/l", L"-l", L"/p", L"-p",
      L"/h", L"-h", L"/g", L"-g", L"/sg", L"-sg", L"/fw", L"-fw",
      L"/o", L"-o",
  };
  for (const wchar_t* token : kDestructive) {
    if (ContainsI(line, token)) {
      return false;
    }
  }
  return true;
}

bool ContainsShutdownAbortOnlyA(const std::string& line) {
  if (!ContainsIA(line, "shutdown")) {
    return false;
  }
  if (!ContainsIA(line, "/a") && !ContainsIA(line, "-a")) {
    return false;
  }

  constexpr const char* kDestructive[] = {
      "/s", "-s", "/r", "-r", "/l", "-l", "/p", "-p",
      "/h", "-h", "/g", "-g", "/sg", "-sg", "/fw", "-fw",
      "/o", "-o",
  };
  for (const char* token : kDestructive) {
    if (ContainsIA(line, token)) {
      return false;
    }
  }
  return true;
}

bool ContainsDangerousSessionCommand(const std::wstring& line) {
  if (ContainsShutdownAbortOnly(line)) {
    return false;
  }

  constexpr const wchar_t* kNeedles[] = {
      L"shutdown",       L"shutdown.exe",     L"logoff",
      L"logoff.exe",     L"stop-computer",    L"restart-computer",
      L"win32shutdown",  L"win32_shutdown",   L"reboot",
      L"poweroff",       L"shut down",        L"shexitwindowsex",
      L"exitwindowsex",  L"exitwin",          L"slidetoshutdown",
  };
  for (const wchar_t* needle : kNeedles) {
    if (ContainsI(line, needle)) {
      return true;
    }
  }
  return false;
}

bool ContainsDangerousSessionCommandA(const std::string& line) {
  if (ContainsShutdownAbortOnlyA(line)) {
    return false;
  }

  constexpr const char* kNeedles[] = {
      "shutdown",       "shutdown.exe",     "logoff",
      "logoff.exe",     "stop-computer",    "restart-computer",
      "win32shutdown",  "win32_shutdown",   "reboot",
      "poweroff",       "shut down",        "shexitwindowsex",
      "exitwindowsex",  "exitwin",          "slidetoshutdown",
  };
  for (const char* needle : kNeedles) {
    if (ContainsIA(line, needle)) {
      return true;
    }
  }
  return false;
}

bool ShouldBlockProcessLaunchW(const wchar_t* applicationName,
                               const wchar_t* commandLine,
                               const wchar_t* parameters = nullptr) {
  if (!BlockingEnabled()) {
    return false;
  }

  const std::wstring joined = JoinCommandLine(commandLine, parameters);
  if (IsShutdownExecutableName(applicationName) ||
      FirstCommandTokenMatches(commandLine, IsShutdownExecutableName)) {
    return !ContainsShutdownAbortOnly(joined);
  }

  if (IsDirectlyBlockedExecutableName(applicationName) ||
      FirstCommandTokenMatches(commandLine, IsDirectlyBlockedExecutableName)) {
    return true;
  }

  return (IsCommandHostExecutableName(applicationName) ||
          FirstCommandTokenMatches(commandLine, IsCommandHostExecutableName)) &&
         ContainsDangerousSessionCommand(joined);
}

bool ShouldBlockProcessLaunchA(const char* applicationName,
                               const char* commandLine,
                               const char* parameters = nullptr) {
  if (!BlockingEnabled()) {
    return false;
  }

  const std::string joined = JoinCommandLineA(commandLine, parameters);
  if (IsShutdownExecutableNameA(applicationName) ||
      FirstCommandTokenMatchesA(commandLine, IsShutdownExecutableNameA)) {
    return !ContainsShutdownAbortOnlyA(joined);
  }

  if (IsDirectlyBlockedExecutableNameA(applicationName) ||
      FirstCommandTokenMatchesA(commandLine, IsDirectlyBlockedExecutableNameA)) {
    return true;
  }

  return (IsCommandHostExecutableNameA(applicationName) ||
          FirstCommandTokenMatchesA(commandLine, IsCommandHostExecutableNameA)) &&
         ContainsDangerousSessionCommandA(joined);
}

bool ShouldInjectChildProcessW(const wchar_t* applicationName,
                               const wchar_t* commandLine) {
  if (!BlockingEnabled()) {
    return false;
  }

  return IsCommandHostExecutableName(applicationName) ||
         FirstCommandTokenMatches(commandLine, IsCommandHostExecutableName);
}

bool ShouldInjectChildProcessA(const char* applicationName,
                               const char* commandLine) {
  if (!BlockingEnabled()) {
    return false;
  }

  return IsCommandHostExecutableNameA(applicationName) ||
         FirstCommandTokenMatchesA(commandLine, IsCommandHostExecutableNameA);
}

void CountBlockedCall(const wchar_t* apiName) {
  if (EnsureState()) {
    InterlockedIncrement(&g_state->blockedCalls);
  }

  wchar_t message[160]{};
  wsprintfW(message, L"ShutdownGuard blocked %s in process %lu", apiName,
            GetCurrentProcessId());
  OutputDebugStringW(message);
}

void TryAbortSystemShutdown() {
  AbortSystemShutdownW(nullptr);
}

std::wstring CurrentDllPath() {
  wchar_t dllPath[MAX_PATH]{};
  const DWORD length = GetModuleFileNameW(g_module, dllPath, ARRAYSIZE(dllPath));
  if (length == 0 || length >= ARRAYSIZE(dllPath)) {
    return {};
  }
  return dllPath;
}

bool InjectCurrentDllIntoProcess(HANDLE process) {
  const std::wstring dllPath = CurrentDllPath();
  if (dllPath.empty()) {
    return false;
  }

  const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
  void* remotePath =
      VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                     PAGE_READWRITE);
  if (remotePath == nullptr) {
    return false;
  }

  BOOL ok =
      WriteProcessMemory(process, remotePath, dllPath.c_str(), bytes, nullptr);
  if (!ok) {
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    return false;
  }

  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  auto loadLibraryW = reinterpret_cast<LPTHREAD_START_ROUTINE>(
      GetProcAddress(kernel32, "LoadLibraryW"));
  if (loadLibraryW == nullptr) {
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    return false;
  }

  HANDLE thread =
      CreateRemoteThread(process, nullptr, 0, loadLibraryW, remotePath, 0,
                         nullptr);
  if (thread == nullptr) {
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    return false;
  }

  const DWORD waitResult = WaitForSingleObject(thread, 5000);
  DWORD exitCode = 0;
  GetExitCodeThread(thread, &exitCode);

  CloseHandle(thread);
  VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);

  return waitResult == WAIT_OBJECT_0 && exitCode != 0;
}

void MaybeInjectChildProcess(bool processCreated,
                             bool shouldInject,
                             bool resumeAfterInjection,
                             LPPROCESS_INFORMATION processInformation) {
  if (!processCreated || !shouldInject || processInformation == nullptr ||
      processInformation->hProcess == nullptr) {
    return;
  }

  InjectCurrentDllIntoProcess(processInformation->hProcess);

  if (resumeAfterInjection && processInformation->hThread != nullptr) {
    ResumeThread(processInformation->hThread);
  }
}

BOOL WINAPI HookedExitWindowsEx(UINT flags, DWORD reason) {
  if (BlockingEnabled()) {
    CountBlockedCall(L"ExitWindowsEx");
    TryAbortSystemShutdown();
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }

  return g_realExitWindowsEx(flags, reason);
}

DWORD WINAPI HookedInitiateShutdownW(LPWSTR machineName,
                                     LPWSTR message,
                                     DWORD gracePeriod,
                                     DWORD shutdownFlags,
                                     DWORD reason) {
  if (BlockingEnabled()) {
    CountBlockedCall(L"InitiateShutdownW");
    TryAbortSystemShutdown();
    return ERROR_ACCESS_DENIED;
  }

  return g_realInitiateShutdownW(machineName, message, gracePeriod,
                                 shutdownFlags, reason);
}

DWORD WINAPI HookedInitiateShutdownA(LPSTR machineName,
                                     LPSTR message,
                                     DWORD gracePeriod,
                                     DWORD shutdownFlags,
                                     DWORD reason) {
  if (BlockingEnabled()) {
    CountBlockedCall(L"InitiateShutdownA");
    TryAbortSystemShutdown();
    return ERROR_ACCESS_DENIED;
  }

  return g_realInitiateShutdownA(machineName, message, gracePeriod,
                                 shutdownFlags, reason);
}

BOOL WINAPI HookedInitiateSystemShutdownW(LPWSTR machineName,
                                          LPWSTR message,
                                          DWORD timeout,
                                          BOOL forceAppsClosed,
                                          BOOL rebootAfterShutdown) {
  if (BlockingEnabled()) {
    CountBlockedCall(L"InitiateSystemShutdownW");
    TryAbortSystemShutdown();
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }

  return g_realInitiateSystemShutdownW(machineName, message, timeout,
                                       forceAppsClosed, rebootAfterShutdown);
}

BOOL WINAPI HookedInitiateSystemShutdownA(LPSTR machineName,
                                          LPSTR message,
                                          DWORD timeout,
                                          BOOL forceAppsClosed,
                                          BOOL rebootAfterShutdown) {
  if (BlockingEnabled()) {
    CountBlockedCall(L"InitiateSystemShutdownA");
    TryAbortSystemShutdown();
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }

  return g_realInitiateSystemShutdownA(machineName, message, timeout,
                                       forceAppsClosed, rebootAfterShutdown);
}

BOOL WINAPI HookedInitiateSystemShutdownExW(LPWSTR machineName,
                                            LPWSTR message,
                                            DWORD timeout,
                                            BOOL forceAppsClosed,
                                            BOOL rebootAfterShutdown,
                                            DWORD reason) {
  if (BlockingEnabled()) {
    CountBlockedCall(L"InitiateSystemShutdownExW");
    TryAbortSystemShutdown();
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }

  return g_realInitiateSystemShutdownExW(machineName, message, timeout,
                                         forceAppsClosed, rebootAfterShutdown,
                                         reason);
}

BOOL WINAPI HookedInitiateSystemShutdownExA(LPSTR machineName,
                                            LPSTR message,
                                            DWORD timeout,
                                            BOOL forceAppsClosed,
                                            BOOL rebootAfterShutdown,
                                            DWORD reason) {
  if (BlockingEnabled()) {
    CountBlockedCall(L"InitiateSystemShutdownExA");
    TryAbortSystemShutdown();
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }

  return g_realInitiateSystemShutdownExA(machineName, message, timeout,
                                         forceAppsClosed, rebootAfterShutdown,
                                         reason);
}

NTSTATUS WINAPI HookedNtShutdownSystem(int action) {
  if (BlockingEnabled()) {
    CountBlockedCall(L"NtShutdownSystem");
    TryAbortSystemShutdown();
    return static_cast<NTSTATUS>(0xC0000022L);
  }

  return g_realNtShutdownSystem != nullptr
             ? g_realNtShutdownSystem(action)
             : static_cast<NTSTATUS>(0xC0000002L);
}

NTSTATUS WINAPI HookedNtInitiatePowerAction(POWER_ACTION systemAction,
                                            SYSTEM_POWER_STATE minSystemState,
                                            ULONG flags,
                                            BOOLEAN asynchronous) {
  if (BlockingEnabled()) {
    CountBlockedCall(L"NtInitiatePowerAction");
    TryAbortSystemShutdown();
    return static_cast<NTSTATUS>(0xC0000022L);
  }

  return g_realNtInitiatePowerAction != nullptr
             ? g_realNtInitiatePowerAction(systemAction, minSystemState, flags,
                                           asynchronous)
             : static_cast<NTSTATUS>(0xC0000002L);
}

BOOL WINAPI HookedCreateProcessW(LPCWSTR applicationName,
                                 LPWSTR commandLine,
                                 LPSECURITY_ATTRIBUTES processAttributes,
                                 LPSECURITY_ATTRIBUTES threadAttributes,
                                 BOOL inheritHandles,
                                 DWORD creationFlags,
                                 LPVOID environment,
                                 LPCWSTR currentDirectory,
                                 LPSTARTUPINFOW startupInfo,
                                 LPPROCESS_INFORMATION processInformation) {
  if (ShouldBlockProcessLaunchW(applicationName, commandLine)) {
    CountBlockedCall(L"CreateProcessW(session command)");
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }

  const bool shouldInject =
      processInformation != nullptr &&
      ShouldInjectChildProcessW(applicationName, commandLine);
  const bool resumeAfterInjection =
      shouldInject && (creationFlags & CREATE_SUSPENDED) == 0;
  const DWORD effectiveCreationFlags =
      shouldInject ? (creationFlags | CREATE_SUSPENDED) : creationFlags;

  const BOOL created = g_realCreateProcessW(
      applicationName, commandLine, processAttributes, threadAttributes,
      inheritHandles, effectiveCreationFlags, environment, currentDirectory,
      startupInfo, processInformation);
  MaybeInjectChildProcess(created, shouldInject, resumeAfterInjection,
                          processInformation);
  return created;
}

BOOL WINAPI HookedCreateProcessA(LPCSTR applicationName,
                                 LPSTR commandLine,
                                 LPSECURITY_ATTRIBUTES processAttributes,
                                 LPSECURITY_ATTRIBUTES threadAttributes,
                                 BOOL inheritHandles,
                                 DWORD creationFlags,
                                 LPVOID environment,
                                 LPCSTR currentDirectory,
                                 LPSTARTUPINFOA startupInfo,
                                 LPPROCESS_INFORMATION processInformation) {
  if (ShouldBlockProcessLaunchA(applicationName, commandLine)) {
    CountBlockedCall(L"CreateProcessA(session command)");
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }

  const bool shouldInject =
      processInformation != nullptr &&
      ShouldInjectChildProcessA(applicationName, commandLine);
  const bool resumeAfterInjection =
      shouldInject && (creationFlags & CREATE_SUSPENDED) == 0;
  const DWORD effectiveCreationFlags =
      shouldInject ? (creationFlags | CREATE_SUSPENDED) : creationFlags;

  const BOOL created = g_realCreateProcessA(
      applicationName, commandLine, processAttributes, threadAttributes,
      inheritHandles, effectiveCreationFlags, environment, currentDirectory,
      startupInfo, processInformation);
  MaybeInjectChildProcess(created, shouldInject, resumeAfterInjection,
                          processInformation);
  return created;
}

BOOL WINAPI HookedShellExecuteExW(SHELLEXECUTEINFOW* executeInfo) {
  if (executeInfo != nullptr &&
      ShouldBlockProcessLaunchW(executeInfo->lpFile, executeInfo->lpFile,
                                executeInfo->lpParameters)) {
    CountBlockedCall(L"ShellExecuteExW(session command)");
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }

  return g_realShellExecuteExW(executeInfo);
}

BOOL WINAPI HookedShellExecuteExA(SHELLEXECUTEINFOA* executeInfo) {
  if (executeInfo != nullptr &&
      ShouldBlockProcessLaunchA(executeInfo->lpFile, executeInfo->lpFile,
                                executeInfo->lpParameters)) {
    CountBlockedCall(L"ShellExecuteExA(session command)");
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }

  return g_realShellExecuteExA(executeInfo);
}

HINSTANCE WINAPI HookedShellExecuteW(HWND hwnd,
                                     LPCWSTR operation,
                                     LPCWSTR file,
                                     LPCWSTR parameters,
                                     LPCWSTR directory,
                                     INT showCommand) {
  if (ShouldBlockProcessLaunchW(file, file, parameters)) {
    CountBlockedCall(L"ShellExecuteW(session command)");
    SetLastError(ERROR_ACCESS_DENIED);
    return reinterpret_cast<HINSTANCE>(SE_ERR_ACCESSDENIED);
  }

  return g_realShellExecuteW(hwnd, operation, file, parameters, directory,
                             showCommand);
}

HINSTANCE WINAPI HookedShellExecuteA(HWND hwnd,
                                     LPCSTR operation,
                                     LPCSTR file,
                                     LPCSTR parameters,
                                     LPCSTR directory,
                                     INT showCommand) {
  if (ShouldBlockProcessLaunchA(file, file, parameters)) {
    CountBlockedCall(L"ShellExecuteA(session command)");
    SetLastError(ERROR_ACCESS_DENIED);
    return reinterpret_cast<HINSTANCE>(SE_ERR_ACCESSDENIED);
  }

  return g_realShellExecuteA(hwnd, operation, file, parameters, directory,
                             showCommand);
}

UINT WINAPI HookedWinExec(LPCSTR commandLine, UINT showCommand) {
  if (ShouldBlockProcessLaunchA(nullptr, commandLine)) {
    CountBlockedCall(L"WinExec(session command)");
    SetLastError(ERROR_ACCESS_DENIED);
    return ERROR_ACCESS_DENIED;
  }

  return g_realWinExec(commandLine, showCommand);
}

BOOL WINAPI HookedWTSLogoffSession(HANDLE server, DWORD sessionId, BOOL wait) {
  if (BlockingEnabled()) {
    CountBlockedCall(L"WTSLogoffSession");
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }

  return g_realWTSLogoffSession(server, sessionId, wait);
}

BOOL WINAPI HookedWTSShutdownSystem(HANDLE server, DWORD shutdownFlag) {
  if (BlockingEnabled()) {
    CountBlockedCall(L"WTSShutdownSystem");
    TryAbortSystemShutdown();
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }

  return g_realWTSShutdownSystem(server, shutdownFlag);
}

template <typename T>
LONG AttachOne(T* target, T detour) {
  return DetourAttach(reinterpret_cast<PVOID*>(target),
                      reinterpret_cast<PVOID>(detour));
}

template <typename T>
LONG DetachOne(T* target, T detour) {
  return DetourDetach(reinterpret_cast<PVOID*>(target),
                      reinterpret_cast<PVOID>(detour));
}

LONG AttachApiHooks() {
  if (g_apiHooksInstalled) {
    return NO_ERROR;
  }

  LONG status = DetourTransactionBegin();
  if (status != NO_ERROR) {
    return status;
  }

  status = DetourUpdateThread(GetCurrentThread());
  if (status == NO_ERROR) {
    status = AttachOne(&g_realCreateProcessW, HookedCreateProcessW);
  }
  if (status == NO_ERROR) {
    status = AttachOne(&g_realCreateProcessA, HookedCreateProcessA);
  }
  if (status == NO_ERROR) {
    status = AttachOne(&g_realShellExecuteExW, HookedShellExecuteExW);
  }
  if (status == NO_ERROR) {
    status = AttachOne(&g_realShellExecuteExA, HookedShellExecuteExA);
  }
  if (status == NO_ERROR) {
    status = AttachOne(&g_realShellExecuteW, HookedShellExecuteW);
  }
  if (status == NO_ERROR) {
    status = AttachOne(&g_realShellExecuteA, HookedShellExecuteA);
  }
  if (status == NO_ERROR) {
    status = AttachOne(&g_realWinExec, HookedWinExec);
  }
  if (status == NO_ERROR) {
    status = AttachOne(&g_realWTSLogoffSession, HookedWTSLogoffSession);
  }
  if (status == NO_ERROR) {
    status = AttachOne(&g_realWTSShutdownSystem, HookedWTSShutdownSystem);
  }
  if (status == NO_ERROR) {
    status = AttachOne(&g_realExitWindowsEx, HookedExitWindowsEx);
  }
  if (status == NO_ERROR && g_realNtShutdownSystem != nullptr) {
    status = AttachOne(&g_realNtShutdownSystem, HookedNtShutdownSystem);
  }
  if (status == NO_ERROR && g_realNtInitiatePowerAction != nullptr) {
    status =
        AttachOne(&g_realNtInitiatePowerAction, HookedNtInitiatePowerAction);
  }
  if (status == NO_ERROR) {
    status = AttachOne(&g_realInitiateShutdownW, HookedInitiateShutdownW);
  }
  if (status == NO_ERROR) {
    status = AttachOne(&g_realInitiateShutdownA, HookedInitiateShutdownA);
  }
  if (status == NO_ERROR) {
    status =
        AttachOne(&g_realInitiateSystemShutdownW, HookedInitiateSystemShutdownW);
  }
  if (status == NO_ERROR) {
    status =
        AttachOne(&g_realInitiateSystemShutdownA, HookedInitiateSystemShutdownA);
  }
  if (status == NO_ERROR) {
    status = AttachOne(&g_realInitiateSystemShutdownExW,
                       HookedInitiateSystemShutdownExW);
  }
  if (status == NO_ERROR) {
    status = AttachOne(&g_realInitiateSystemShutdownExA,
                       HookedInitiateSystemShutdownExA);
  }

  if (status != NO_ERROR) {
    DetourTransactionAbort();
    return status;
  }

  status = DetourTransactionCommit();
  if (status == NO_ERROR) {
    g_apiHooksInstalled = true;
  }
  return status;
}

LONG DetachApiHooks() {
  if (!g_apiHooksInstalled) {
    return NO_ERROR;
  }

  LONG status = DetourTransactionBegin();
  if (status != NO_ERROR) {
    return status;
  }

  status = DetourUpdateThread(GetCurrentThread());
  if (status == NO_ERROR) {
    status = DetachOne(&g_realInitiateSystemShutdownExA,
                       HookedInitiateSystemShutdownExA);
  }
  if (status == NO_ERROR) {
    status = DetachOne(&g_realInitiateSystemShutdownExW,
                       HookedInitiateSystemShutdownExW);
  }
  if (status == NO_ERROR) {
    status =
        DetachOne(&g_realInitiateSystemShutdownA, HookedInitiateSystemShutdownA);
  }
  if (status == NO_ERROR) {
    status =
        DetachOne(&g_realInitiateSystemShutdownW, HookedInitiateSystemShutdownW);
  }
  if (status == NO_ERROR) {
    status = DetachOne(&g_realInitiateShutdownA, HookedInitiateShutdownA);
  }
  if (status == NO_ERROR) {
    status = DetachOne(&g_realInitiateShutdownW, HookedInitiateShutdownW);
  }
  if (status == NO_ERROR) {
    status = DetachOne(&g_realExitWindowsEx, HookedExitWindowsEx);
  }
  if (status == NO_ERROR) {
    status = DetachOne(&g_realWTSShutdownSystem, HookedWTSShutdownSystem);
  }
  if (status == NO_ERROR) {
    status = DetachOne(&g_realWTSLogoffSession, HookedWTSLogoffSession);
  }
  if (status == NO_ERROR && g_realNtInitiatePowerAction != nullptr) {
    status =
        DetachOne(&g_realNtInitiatePowerAction, HookedNtInitiatePowerAction);
  }
  if (status == NO_ERROR && g_realNtShutdownSystem != nullptr) {
    status = DetachOne(&g_realNtShutdownSystem, HookedNtShutdownSystem);
  }
  if (status == NO_ERROR) {
    status = DetachOne(&g_realWinExec, HookedWinExec);
  }
  if (status == NO_ERROR) {
    status = DetachOne(&g_realShellExecuteA, HookedShellExecuteA);
  }
  if (status == NO_ERROR) {
    status = DetachOne(&g_realShellExecuteW, HookedShellExecuteW);
  }
  if (status == NO_ERROR) {
    status = DetachOne(&g_realShellExecuteExA, HookedShellExecuteExA);
  }
  if (status == NO_ERROR) {
    status = DetachOne(&g_realShellExecuteExW, HookedShellExecuteExW);
  }
  if (status == NO_ERROR) {
    status = DetachOne(&g_realCreateProcessA, HookedCreateProcessA);
  }
  if (status == NO_ERROR) {
    status = DetachOne(&g_realCreateProcessW, HookedCreateProcessW);
  }

  if (status != NO_ERROR) {
    DetourTransactionAbort();
    return status;
  }

  status = DetourTransactionCommit();
  if (status == NO_ERROR) {
    g_apiHooksInstalled = false;
  }
  return status;
}

LRESULT CALLBACK CbtHookProc(int code, WPARAM wParam, LPARAM lParam) {
  (void)wParam;
  (void)lParam;
  return CallNextHookEx(nullptr, code, wParam, lParam);
}

}  // namespace

extern "C" __declspec(dllexport) BOOL WINAPI SetGlobalShutdownBlocking(
    BOOL enabled) {
  if (!EnsureState()) {
    return FALSE;
  }

  InterlockedExchange(&g_state->enabled, enabled ? 1 : 0);
  return TRUE;
}

extern "C" __declspec(dllexport) BOOL WINAPI IsGlobalShutdownBlocking() {
  return BlockingEnabled() ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) LONG WINAPI GetGlobalShutdownBlockedCount() {
  if (!EnsureState()) {
    return 0;
  }

  return InterlockedCompareExchange(&g_state->blockedCalls, 0, 0);
}

extern "C" __declspec(dllexport) DWORD WINAPI GetGlobalShutdownHookLastError() {
  return g_lastError;
}

extern "C" __declspec(dllexport) BOOL WINAPI InstallGlobalShutdownHook() {
  if (!EnsureState()) {
    return FALSE;
  }

  const LONG detourStatus = AttachApiHooks();
  if (detourStatus != NO_ERROR) {
    g_lastError = static_cast<DWORD>(detourStatus);
    return FALSE;
  }

  if (g_hook != nullptr) {
    return TRUE;
  }

  g_hook = SetWindowsHookExW(WH_CBT, CbtHookProc, g_module, 0);
  if (g_hook == nullptr) {
    g_lastError = GetLastError();
    return FALSE;
  }

  return TRUE;
}

extern "C" __declspec(dllexport) BOOL WINAPI RemoveGlobalShutdownHook() {
  BOOL ok = TRUE;
  if (g_hook != nullptr) {
    ok = UnhookWindowsHookEx(g_hook);
    if (!ok) {
      g_lastError = GetLastError();
    }
    g_hook = nullptr;
  }

  const LONG detourStatus = DetachApiHooks();
  if (detourStatus != NO_ERROR) {
    g_lastError = static_cast<DWORD>(detourStatus);
    ok = FALSE;
  }

  return ok;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = module;
    DisableThreadLibraryCalls(module);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll != nullptr) {
      g_realNtShutdownSystem = reinterpret_cast<NtShutdownSystemFn>(
          GetProcAddress(ntdll, "NtShutdownSystem"));
      g_realNtInitiatePowerAction = reinterpret_cast<NtInitiatePowerActionFn>(
          GetProcAddress(ntdll, "NtInitiatePowerAction"));
    }
    EnsureState();
    AttachApiHooks();
  } else if (reason == DLL_PROCESS_DETACH) {
    if (g_hook != nullptr) {
      UnhookWindowsHookEx(g_hook);
      g_hook = nullptr;
    }
    DetachApiHooks();
    CloseState();
  }

  return TRUE;
}
