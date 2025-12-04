#define OEMRESOURCE
#include "updater.h"
#include "resource.h"
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <Commctrl.h>
#include <string>
#include <vector>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Shlwapi.lib")

// ==============================
// Constants & globals
// ==============================
const wchar_t* APP_NAME = L"AutoHide Cursor";
const wchar_t* APP_VERSION = L"v1.0.0";

const wchar_t* REG_RUN_PATH = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* SETTINGS_PATH = L"SOFTWARE\\AutoHideCursor";
const wchar_t* REG_ENABLED = L"Enabled";
const wchar_t* REG_TIMEOUTMS = L"TimeoutMs";
const wchar_t* REG_ICON_PATH = L"CustomIcon";

HINSTANCE       g_hInstance = nullptr;
HWND            g_hWnd = nullptr;
HMENU           g_hMenu = nullptr;
NOTIFYICONDATAW g_nid = { sizeof(NOTIFYICONDATAW) };

bool        g_isStartup = false;
bool        g_enabled = true;
bool        g_cursorHidden = false;     // app state
bool        g_sysCursorsHidden = false; // system cursors swapped?
DWORD       g_timeoutMs = 10000;        // default 10s
ULONGLONG   g_lastMouseTick = 0;
HHOOK       g_hMouseHook = nullptr;
std::wstring g_customIconPath;

HICON hWhiteIcon = (HICON)LoadImage(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_ICON1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
HICON hBlackIcon = (HICON)LoadImage(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_ICON2), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);

UINT      WM_TASKBARCREATED = 0;
const UINT IDT_CHECK = 1; // heartbeat timer

// Menu IDs
enum : UINT {
    ID_MI_STARTUP = 1,
    ID_MI_CHANGEICO = 2,
    ID_MI_RESETICO = 3,
    ID_MI_ENABLE = 4,   // "Enable Auto Hide"
    ID_MI_TIMEOUT = 5,   // cycles 3/5/10/30/60s
    ID_MI_ABOUT = 6,
    ID_MI_EXIT = 7,
};

// timeouts cycled by menu item #5
static const DWORD kTimeouts[] = { 3000, 5000, 10000, 30000, 60000 };

// ==============================
// Helpers
// ==============================
static std::string WCharToString(const wchar_t* w) {
    if (!w) return "";
    size_t sz = 0; wcstombs_s(&sz, nullptr, 0, w, 0);
    if (!sz) return "";
    std::string s(sz, '\0'); size_t conv = 0;
    wcstombs_s(&conv, s.data(), sz, w, _TRUNCATE);
    if (!s.empty()) s.erase(0, 1);
    return s;
}

static ULONGLONG NowMs() { return GetTickCount64(); }

static bool IsTaskbarDarkMode() {
    HKEY hKey; DWORD v = 0, cb = sizeof(DWORD);
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueEx(hKey, L"SystemUsesLightTheme", nullptr, nullptr, (LPBYTE)&v, &cb) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return v == 0; // dark
        }
        RegCloseKey(hKey);
    }
    return true;
}

static void SaveSettings() {
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, SETTINGS_PATH, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        DWORD en = g_enabled ? 1u : 0u;
        RegSetValueEx(hKey, REG_ENABLED, 0, REG_DWORD, (const BYTE*)&en, sizeof(en));
        RegSetValueEx(hKey, REG_TIMEOUTMS, 0, REG_DWORD, (const BYTE*)&g_timeoutMs, sizeof(g_timeoutMs));
        if (!g_customIconPath.empty()) {
            RegSetValueEx(hKey, REG_ICON_PATH, 0, REG_SZ, (const BYTE*)g_customIconPath.c_str(), (DWORD)((g_customIconPath.size() + 1) * sizeof(wchar_t)));
        }
        else {
            RegDeleteValue(hKey, REG_ICON_PATH);
        }
        RegCloseKey(hKey);
    }
}

static void LoadSettings() {
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, SETTINGS_PATH, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD en = 1, to = g_timeoutMs, type = 0, cb = sizeof(DWORD);
        if (RegQueryValueEx(hKey, REG_ENABLED, nullptr, &type, (LPBYTE)&en, &cb) == ERROR_SUCCESS)
            g_enabled = (en != 0);
        cb = sizeof(DWORD);
        if (RegQueryValueEx(hKey, REG_TIMEOUTMS, nullptr, &type, (LPBYTE)&to, &cb) == ERROR_SUCCESS) {
            if (to >= 1000 && to <= 600000) g_timeoutMs = to;
        }
        wchar_t buf[MAX_PATH]; DWORD bsz = sizeof(buf);
        if (RegQueryValueEx(hKey, REG_ICON_PATH, nullptr, nullptr, (LPBYTE)buf, &bsz) == ERROR_SUCCESS)
            g_customIconPath = buf;
        RegCloseKey(hKey);
    }
}

static bool IsStartupEnabled() {
    HKEY key;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_RUN_PATH, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        wchar_t v[MAX_PATH]; DWORD cb = sizeof(v);
        const bool ok = (RegQueryValueEx(key, APP_NAME, nullptr, nullptr, (LPBYTE)v, &cb) == ERROR_SUCCESS);
        RegCloseKey(key);
        return ok;
    }
    return false;
}

static void ToggleStartup() {
    HKEY key;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_RUN_PATH, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        if (IsStartupEnabled()) {
            RegDeleteValue(key, APP_NAME);
        }
        else {
            wchar_t exe[MAX_PATH]; GetModuleFileName(nullptr, exe, MAX_PATH);
            RegSetValueEx(key, APP_NAME, 0, REG_SZ, (const BYTE*)exe, (DWORD)((wcslen(exe) + 1) * sizeof(wchar_t)));
        }
        RegCloseKey(key);
    }
}

static void UpdateTimeoutMenuText() {
    std::wstring label = L"Timeout: ";
    if (g_timeoutMs == 5000) label += L"5s";
    else if (g_timeoutMs == 10000) label += L"10s";
    else if (g_timeoutMs == 30000) label += L"30s";
    else if (g_timeoutMs == 60000) label += L"60s";
    else label += std::to_wstring(g_timeoutMs / 1000) + L"s";
    ModifyMenuW(g_hMenu, ID_MI_TIMEOUT, MF_BYCOMMAND | MF_STRING, ID_MI_TIMEOUT, label.c_str());
}

static void CycleTimeout() {
    size_t idx = 0;
    for (size_t i = 0; i < ARRAYSIZE(kTimeouts); ++i) { if (kTimeouts[i] == g_timeoutMs) { idx = i; break; } }
    idx = (idx + 1) % ARRAYSIZE(kTimeouts);
    g_timeoutMs = kTimeouts[idx];
    SaveSettings();
    UpdateTimeoutMenuText();
}

// ==============================
// Cursor hide via monochrome CreateCursor (no black box)
// ==============================

static HCURSOR CreateBlankCursor()
{
    const int cx = GetSystemMetrics(SM_CXCURSOR);
    const int cy = GetSystemMetrics(SM_CYCURSOR);

    // Each AND/XOR scanline must be WORD-aligned (16 bits)
    const int bytesPerLine = ((cx + 15) / 16) * 2;
    const size_t planeSize = (size_t)bytesPerLine * cy;

    // AND: all 1s (keep background), XOR: all 0s (no draw) -> fully invisible
    std::vector<BYTE> andPlane(planeSize, 0xFF);
    std::vector<BYTE> xorPlane(planeSize, 0x00);

    return CreateCursor(GetModuleHandle(nullptr),
        cx / 2, cy / 2,
        cx, cy,
        andPlane.data(),
        xorPlane.data());
}

static void HideSystemCursors()
{
    if (g_sysCursorsHidden) return;

    const int ids[] = {
        OCR_NORMAL/*, OCR_IBEAM, OCR_WAIT, OCR_CROSS, OCR_UP,
        OCR_SIZENWSE, OCR_SIZENESW, OCR_SIZEWE, OCR_SIZENS, OCR_SIZEALL,
        OCR_NO, OCR_HAND, OCR_APPSTARTING*/
    };

    for (int id : ids) {
        HCURSOR hBlank = CreateBlankCursor();
        SetSystemCursor(hBlank, id); // takes ownership, no DestroyCursor needed
    }

    g_sysCursorsHidden = true;
}

static void RestoreSystemCursors()
{
    if (!g_sysCursorsHidden) return;
    SystemParametersInfo(SPI_SETCURSORS, 0, nullptr, 0);
    g_sysCursorsHidden = false;
}

// ==============================
// Show/Hide cursor orchestration
// ==============================
static void EnsureCursorHidden() {
    if (g_cursorHidden) return;
    // balance ShowCursor counter defensively
    for (int i = 0; i < 100; ++i) { if (ShowCursor(FALSE) < 0) break; }
    HideSystemCursors();
    g_cursorHidden = true;
}

static void EnsureCursorVisible() {
    if (!g_cursorHidden) return;
    RestoreSystemCursors();
    for (int i = 0; i < 100; ++i) { if (ShowCursor(TRUE) >= 0) break; }
    g_cursorHidden = false;
}

// ==============================
// Tray icon & menu
// ==============================
static void UpdateIconColor() {
    HICON chosen = nullptr;
    if (!g_customIconPath.empty()) {
        chosen = (HICON)LoadImage(nullptr, g_customIconPath.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    }
    if (!chosen) {
        chosen = IsTaskbarDarkMode() ? hWhiteIcon : hBlackIcon;
        if (!chosen) chosen = LoadIcon(nullptr, IDI_APPLICATION);
    }
    g_nid.hIcon = chosen;
    g_nid.uFlags = NIF_ICON;
    Shell_NotifyIcon(NIM_MODIFY, &g_nid);
}

static void CreateTrayIcon(HWND hWnd) {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(NOTIFYICONDATA);
    g_nid.hWnd = hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP + 1;
    g_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, (std::wstring(APP_NAME) + L" " + APP_VERSION).c_str());

    Shell_NotifyIcon(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIcon(NIM_SETVERSION, &g_nid);

    UpdateIconColor();
}

static void RemoveTrayIcon() {
    if (g_nid.hWnd) Shell_NotifyIcon(NIM_DELETE, &g_nid);
}

static void ChangeTrayIcon() {
    wchar_t filePath[MAX_PATH] = L"";
    OPENFILENAME ofn = { sizeof(ofn) };
    ofn.hwndOwner = g_hWnd;
    ofn.lpstrFilter = L"Icon Files\0*.ico\0All Files\0*.*\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = L"Select a New Tray Icon";

    if (GetOpenFileName(&ofn)) {
        HICON hNew = (HICON)LoadImage(nullptr, filePath, IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
        if (hNew) {
            g_customIconPath = filePath;
            SaveSettings();
            UpdateIconColor();
        }
        else {
            MessageBox(g_hWnd, L"Failed to load icon.", L"Error", MB_ICONERROR);
        }
    }
}

static void ResetTrayIcon() {
    g_customIconPath.clear();
    SaveSettings();
    UpdateIconColor();
}

static void BuildMenuOnce() {
    g_hMenu = CreatePopupMenu();
    AppendMenu(g_hMenu, MF_STRING, ID_MI_STARTUP, L"Run at Startup");
    AppendMenu(g_hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(g_hMenu, MF_STRING, ID_MI_CHANGEICO, L"Change Icon");
    AppendMenu(g_hMenu, MF_STRING, ID_MI_RESETICO, L"Reset Icon");
    AppendMenu(g_hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(g_hMenu, MF_STRING, ID_MI_ENABLE, L"Enable Auto Hide");
    AppendMenu(g_hMenu, MF_STRING, ID_MI_TIMEOUT, L"Timeout: 10s"); // updated dynamically
    AppendMenu(g_hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(g_hMenu, MF_STRING, ID_MI_ABOUT, L"About");
    AppendMenu(g_hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(g_hMenu, MF_STRING, ID_MI_EXIT, L"Exit");

    UpdateTimeoutMenuText();
}

// ==============================
// Mouse hook
// ==============================
static LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_enabled) {
        switch (wParam) {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL:
            g_lastMouseTick = NowMs();
            if (g_cursorHidden) EnsureCursorVisible();
            break;
        }
    }
    return CallNextHookEx(g_hMouseHook, code, wParam, lParam);
}

static void InstallHook() { g_hMouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, g_hInstance, 0); }
static void UninstallHook() { if (g_hMouseHook) { UnhookWindowsHookEx(g_hMouseHook); g_hMouseHook = nullptr; } }

// ==============================
// Window proc — HideIcons flow
// ==============================
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TASKBARCREATED) { CreateTrayIcon(hWnd); return 0; }

    switch (msg) {
    case WM_APP + 1: {
        if (LOWORD(lParam) == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt);
            SetForegroundWindow(hWnd);

            // reflect checks before showing menu
            g_isStartup = IsStartupEnabled();
            CheckMenuItem(g_hMenu, ID_MI_STARTUP, MF_BYCOMMAND | (g_isStartup ? MF_CHECKED : MF_UNCHECKED));
            CheckMenuItem(g_hMenu, ID_MI_ENABLE, MF_BYCOMMAND | (g_enabled ? MF_CHECKED : MF_UNCHECKED));
            UpdateTimeoutMenuText();

            TrackPopupMenu(g_hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
        }
        else if (LOWORD(lParam) == WM_LBUTTONUP) {
            g_enabled = !g_enabled;
            if (!g_enabled) EnsureCursorVisible();
            SaveSettings();
        }
        break;
    }

    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case ID_MI_STARTUP:   ToggleStartup(); break;
        case ID_MI_CHANGEICO: ChangeTrayIcon(); break;
        case ID_MI_RESETICO:  ResetTrayIcon(); break;
        case ID_MI_ENABLE:
            g_enabled = !g_enabled;
            if (!g_enabled) EnsureCursorVisible();
            SaveSettings();
            break;
        case ID_MI_TIMEOUT:
            CycleTimeout();
            break;
        case ID_MI_ABOUT:
            MessageBox(hWnd, (std::wstring(APP_NAME) + L" " + APP_VERSION +
                L"\nHides the mouse cursor after mouse inactivity."
                L"\nLeft-click tray icon to toggle.")
                .c_str(),
                L"About", MB_OK | MB_ICONINFORMATION);
            break;
        case ID_MI_EXIT:
            SendMessage(hWnd, WM_CLOSE, 0, 0);
            break;
        }
        break;
    }

    case WM_SETTINGCHANGE: // theme change -> icon color
        UpdateIconColor();
        break;

    case WM_CREATE:
        CreateTrayIcon(hWnd);
        SetTimer(hWnd, IDT_CHECK, 250, nullptr); // snappy heartbeat
        break;

    case WM_TIMER:
        if (wParam == IDT_CHECK && g_enabled) {
            const ULONGLONG idle = NowMs() - g_lastMouseTick;
            if (!g_cursorHidden && idle >= g_timeoutMs) {
                EnsureCursorHidden();
            }
        }
        break;

    case WM_DESTROY:
        KillTimer(hWnd, IDT_CHECK);
        RemoveTrayIcon();
        EnsureCursorVisible(); // always restore
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// ==============================
// WinMain — mirrors HideIcons
// ==============================
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    Updater updater(WCharToString(APP_VERSION), "emp0ry", "AutoHideCursor", "AutoHideCursor");
    if (updater.checkAndUpdate())
        return 0;

    // single instance
    CreateMutexA(nullptr, FALSE, "Local\\AutoHideCursor_TrayApp");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBox(nullptr, L"AutoHide Cursor is already running!", APP_NAME, MB_ICONERROR | MB_OK);
        return -1;
    }

    g_hInstance = hInstance;
    WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");

    LoadSettings();
    g_lastMouseTick = NowMs();

    // hidden message window
    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"AutoHideCursorTrayWnd";
    RegisterClass(&wc);

    g_hWnd = CreateWindowEx(0, wc.lpszClassName, APP_NAME, 0, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
    if (!g_hWnd) return -1;

    // menu (IDs 1..7) — created once like HideIcons
    BuildMenuOnce();
    InstallHook();

    // loop
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_hMenu) DestroyMenu(g_hMenu);
    UninstallHook();
    return 0;
}
