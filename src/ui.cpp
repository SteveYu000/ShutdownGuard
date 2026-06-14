#include "ui.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <strsafe.h>

#include <algorithm>

// ---------------------------------------------------------------------------
// Tray icon
// ---------------------------------------------------------------------------

void AddTrayIcon(HWND hwnd) {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(NOTIFYICONDATAW);
  nid.hWnd = hwnd;
  nid.uID = kTrayIconId;
  nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  nid.uCallbackMessage = kMsgTrayNotify;
  nid.hIcon = static_cast<HICON>(
      LoadImageW(nullptr, IDI_SHIELD, IMAGE_ICON,
                 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                 LR_SHARED));
  if (!nid.hIcon) {
    nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  }
  StringCchCopyW(nid.szTip, ARRAYSIZE(nid.szTip), L"Shutdown Guard");
  Shell_NotifyIconW(NIM_ADD, &nid);
}

void RemoveTrayIcon(HWND hwnd) {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(NOTIFYICONDATAW);
  nid.hWnd = hwnd;
  nid.uID = kTrayIconId;
  Shell_NotifyIconW(NIM_DELETE, &nid);
}

void ShowTrayBalloon(HWND hwnd, const wchar_t* title, const wchar_t* text) {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(NOTIFYICONDATAW);
  nid.hWnd = hwnd;
  nid.uID = kTrayIconId;
  nid.uFlags = NIF_INFO;
  nid.dwInfoFlags = NIIF_INFO;
  nid.uTimeout = 4000;
  StringCchCopyW(nid.szInfo, ARRAYSIZE(nid.szInfo), text);
  StringCchCopyW(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), title);
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

UINT ShowTrayContextMenu(HWND hwnd) {
  HMENU menu = CreatePopupMenu();
  if (!menu) {
    return 0;
  }

  AppendMenuW(menu, MF_STRING, kIdTrayShow, L"显示");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kIdTrayExit, L"退出");

  POINT pt{};
  GetCursorPos(&pt);
  SetForegroundWindow(hwnd);
  const UINT cmd =
      static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                       pt.x, pt.y, 0, hwnd, nullptr));
  DestroyMenu(menu);
  return cmd;
}

// ---------------------------------------------------------------------------
// Dark mode detection
// ---------------------------------------------------------------------------

bool IsSystemDarkMode() {
  HKEY hKey = nullptr;
  if (RegOpenKeyExW(
          HKEY_CURRENT_USER,
          L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
          0, KEY_READ, &hKey) != ERROR_SUCCESS) {
    return false;
  }

  DWORD value = 1;  // default: light
  DWORD size = sizeof(value);
  RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr,
                   reinterpret_cast<LPBYTE>(&value), &size);
  RegCloseKey(hKey);
  return value == 0;
}

// ---------------------------------------------------------------------------
// DWM attributes
// ---------------------------------------------------------------------------

void ApplyDwmAttributes(HWND hwnd, bool darkMode) {
  const BOOL dark = darkMode ? TRUE : FALSE;
  DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                        sizeof(dark));

  const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
  DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner,
                        sizeof(corner));
}

// ---------------------------------------------------------------------------
// Modern font
// ---------------------------------------------------------------------------

HFONT CreateModernFont(HWND hwnd, int pointSize, int weight) {
  LOGFONTW lf{};
  lf.lfHeight = -MulDiv(pointSize, GetDpiForWindow(hwnd), 72);
  lf.lfWeight = weight;
  lf.lfQuality = CLEARTYPE_QUALITY;
  lf.lfCharSet = DEFAULT_CHARSET;
  StringCchCopyW(lf.lfFaceName, LF_FACESIZE, L"Segoe UI");
  return CreateFontIndirectW(&lf);
}

// ---------------------------------------------------------------------------
// Owner-draw toggle switch
// ---------------------------------------------------------------------------

namespace {

constexpr int kToggleWidth  = 46;
constexpr int kToggleHeight = 24;
constexpr int kThumbPad     = 3;
constexpr int kThumbSize    = kToggleHeight - kThumbPad * 2;  // 18

COLORREF Blend(COLORREF a, COLORREF b, float t) {
  const int r = static_cast<int>(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t);
  const int g = static_cast<int>(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t);
  const int bl = static_cast<int>(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t);
  return RGB(r, g, bl);
}

}  // namespace

void DrawToggleSwitch(const DRAWITEMSTRUCT& dis, bool enabled, bool hovered,
                      bool pressed) {
  HDC dc = dis.hDC;
  const RECT& rc = dis.rcItem;

  // Save DC state
  const int saved = SaveDC(dc);

  // --- Background ---
  const COLORREF bgOn  = RGB(0x00, 0x78, 0xD4);  // Win11 accent
  const COLORREF bgOff = RGB(0x78, 0x78, 0x80);  // Win11 inactive
  COLORREF bg = enabled ? bgOn : bgOff;
  if (hovered) bg = Blend(bg, RGB(0xFF, 0xFF, 0xFF), pressed ? 0.25f : 0.12f);

  const HBRUSH bgBrush = CreateSolidBrush(bg);
  const HGDIOBJ oldBrush = SelectObject(dc, bgBrush);
  const HPEN bgPen = CreatePen(PS_NULL, 0, 0);
  const HGDIOBJ oldPen = SelectObject(dc, bgPen);

  RECT pill = rc;
  pill.left   = (rc.left + rc.right - kToggleWidth) / 2;
  pill.right  = pill.left + kToggleWidth;
  pill.top    = (rc.top + rc.bottom - kToggleHeight) / 2;
  pill.bottom = pill.top + kToggleHeight;
  RoundRect(dc, pill.left, pill.top, pill.right, pill.bottom,
            kToggleHeight, kToggleHeight);

  // --- Thumb ---
  const int thumbX = enabled
                         ? pill.right - kThumbPad - kThumbSize
                         : pill.left + kThumbPad;
  RECT thumb = {thumbX, pill.top + kThumbPad,
                thumbX + kThumbSize, pill.top + kThumbPad + kThumbSize};

  const HBRUSH thumbBrush = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
  SelectObject(dc, thumbBrush);
  SelectObject(dc, GetStockObject(NULL_PEN));
  Ellipse(dc, thumb.left, thumb.top, thumb.right, thumb.bottom);

  // --- Focus rect ---
  if (dis.itemState & ODS_FOCUS) {
    const HBRUSH focusBrush = CreateSolidBrush(RGB(0, 0, 0));
    RECT fr = pill;
    InflateRect(&fr, 2, 2);
    DrawFocusRect(dc, &fr);
    DeleteObject(focusBrush);
  }

  // --- Cleanup ---
  DeleteObject(SelectObject(dc, oldPen));
  DeleteObject(SelectObject(dc, oldBrush));
  DeleteObject(thumbBrush);

  RestoreDC(dc, saved);
}
