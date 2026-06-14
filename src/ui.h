#pragma once

#include <windows.h>

// ---------------------------------------------------------------------------
// Tray icon
// ---------------------------------------------------------------------------

inline constexpr UINT kMsgTrayNotify = WM_APP + 2;
inline constexpr UINT kTrayIconId    = 1;
inline constexpr UINT kIdTrayShow    = 2001;
inline constexpr UINT kIdTrayExit    = 2002;

/// Add the tray icon.  Call from WM_CREATE or when handling TaskbarCreated.
void AddTrayIcon(HWND hwnd);

/// Remove the tray icon.  Call from WM_DESTROY / WM_ENDSESSION.
void RemoveTrayIcon(HWND hwnd);

/// Show a balloon tooltip over the tray icon.
void ShowTrayBalloon(HWND hwnd, const wchar_t* title, const wchar_t* text);

/// Show a right-click context menu.  Returns the selected command id or 0.
UINT ShowTrayContextMenu(HWND hwnd);

// ---------------------------------------------------------------------------
// Modern UI
// ---------------------------------------------------------------------------

/// Returns true when the system is in dark mode (reads the registry key).
bool IsSystemDarkMode();

/// Apply Win11 rounded corners and dark-mode title bar to the window.
/// All calls are best-effort; failures are silently ignored.
void ApplyDwmAttributes(HWND hwnd, bool darkMode);

/// Create a Segoe UI font at the given point size.
HFONT CreateModernFont(HWND hwnd, int pointSize, int weight = FW_NORMAL);

/// Create a solid brush (delegate to the caller to select colour).
inline HBRUSH CreateSolidBrush(COLORREF color) {
  return ::CreateSolidBrush(color);
}

// ---------------------------------------------------------------------------
// Owner-draw toggle switch
// ---------------------------------------------------------------------------

/// Draw a Win11-style toggle switch.
void DrawToggleSwitch(const DRAWITEMSTRUCT& dis, bool enabled, bool hovered,
                      bool pressed);
