#include "framework.h"
#include "Resource.h"
#include "MultiThreadDownloader.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <atomic>
#include <cwctype>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

#if defined _M_IX86
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='x86' publicKeyToken='6595b64144ccf1df' language='*'\"")
#elif defined _M_IA64
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='ia64' publicKeyToken='6595b64144ccf1df' language='*'\"")
#elif defined _M_X64
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")
#else
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif

#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif

#define IDC_URL_EDIT        1001
#define IDC_PATH_EDIT       1002
#define IDC_PATH_BROWSE     1003
#define IDC_THREADS_EDIT    1004
#define IDC_SPEEDLIMIT_EDIT 1005
#define IDC_UA_COMBO        1006
#define IDC_UA_CUSTOM       1007
#define IDC_DOWNLOAD_BTN    1008
#define IDC_PROGRESS        1009
#define IDC_STATUS_LABEL    1010
#define IDC_INFO_LABEL      1011
#define IDC_PAUSE_BTN       1012
#define IDC_CANCEL_BTN      1013
#define IDT_PROGRESS        2001

#define WM_DOWNLOAD_COMPLETE (WM_USER + 101)

#define IDM_EDIT_UNDO       60001
#define IDM_EDIT_CUT        60002
#define IDM_EDIT_COPY       60003
#define IDM_EDIT_PASTE      60004
#define IDM_EDIT_DELETE     60005
#define IDM_EDIT_SELECT_ALL 60006

static HINSTANCE g_hInst;
static MultiThreadDownloader g_downloader;
static std::thread g_downloadThread;
static std::atomic<bool> g_downloadActive{false};
static std::atomic<bool> g_downloadSuccess{false};
static HBRUSH g_hBackgroundBrush = nullptr;
static HFONT g_hMainFont = nullptr;
static HFONT g_hPrimaryBtnFont = nullptr;
static std::wstring g_saveDirectory;

constexpr COLORREF kBackgroundColor = RGB(246, 248, 252);
constexpr COLORREF kTextColor = RGB(30, 34, 42);
constexpr COLORREF kPrimaryBtnColor = RGB(22, 119, 255);
constexpr COLORREF kPrimaryBtnPressedColor = RGB(10, 91, 214);
constexpr COLORREF kSecondaryBtnColor = RGB(255, 255, 255);
constexpr COLORREF kSecondaryBtnPressedColor = RGB(238, 242, 248);
constexpr COLORREF kSecondaryBorderColor = RGB(192, 201, 214);
constexpr COLORREF kDisabledColor = RGB(220, 225, 233);
constexpr COLORREF kDisabledTextColor = RGB(138, 146, 160);
constexpr wchar_t kSettingsFileName[] = L"PCLDownloader.ini";
constexpr wchar_t kDefaultSaveFolderName[] = L"Downloads";

static std::wstring GetModuleDirectory() {
    wchar_t exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return L".";
    std::wstring path(exePath, len);
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L".";
    return path.substr(0, pos);
}

static std::wstring CombinePath(const std::wstring& dir,
                                const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + L"\\" + name;
}

static std::wstring GetSettingsIniPath() {
    return CombinePath(GetModuleDirectory(), kSettingsFileName);
}

static std::wstring NormalizePathInput(std::wstring path) {
    path.erase(path.begin(),
        std::find_if(path.begin(), path.end(), [](wchar_t ch) {
            return !iswspace(ch);
        }));
    while (!path.empty() && iswspace(path.back())) {
        path.pop_back();
    }
    if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"') {
        path = path.substr(1, path.size() - 2);
    }
    std::replace(path.begin(), path.end(), L'/', L'\\');
    return path;
}

static bool IsAbsolutePath(const std::wstring& path) {
    if (path.size() >= 2 && path[1] == L':') return true;
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') return true;
    return false;
}

static std::wstring ToAbsolutePath(std::wstring path) {
    path = NormalizePathInput(path);
    if (path.empty() || IsAbsolutePath(path)) return path;
    return CombinePath(GetModuleDirectory(), path);
}

static std::wstring GetParentDirectory(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L"";
    if (pos == 2 && path.size() >= 3 && path[1] == L':') {
        return path.substr(0, 3);
    }
    if (pos == 0) return path.substr(0, 1);
    return path.substr(0, pos);
}

static std::wstring TrimDirectorySuffix(std::wstring dir) {
    while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/')) {
        dir.pop_back();
    }
    return dir;
}

static bool EnsureDirectoryExists(const std::wstring& dir) {
    if (dir.empty()) return false;

    DWORD attrs = GetFileAttributesW(dir.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    int ret = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (ret == ERROR_SUCCESS || ret == ERROR_ALREADY_EXISTS || ret == ERROR_FILE_EXISTS) {
        return true;
    }

    attrs = GetFileAttributesW(dir.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static std::wstring GetDefaultSaveDirectory() {
    return CombinePath(GetModuleDirectory(), kDefaultSaveFolderName);
}

static void SaveAppSettings() {
    if (g_saveDirectory.empty()) return;
    std::wstring iniPath = GetSettingsIniPath();
    WritePrivateProfileStringW(L"UI", L"SaveDirectory",
                               g_saveDirectory.c_str(), iniPath.c_str());
}

static void LoadAppSettings() {
    wchar_t buffer[4096] = {};
    std::wstring iniPath = GetSettingsIniPath();
    GetPrivateProfileStringW(L"UI", L"SaveDirectory", L"",
                             buffer, 4096, iniPath.c_str());

    g_saveDirectory = NormalizePathInput(buffer);
    if (g_saveDirectory.empty()) {
        g_saveDirectory = GetDefaultSaveDirectory();
    }

    g_saveDirectory = ToAbsolutePath(g_saveDirectory);
    g_saveDirectory = TrimDirectorySuffix(g_saveDirectory);

    if (!EnsureDirectoryExists(g_saveDirectory)) {
        g_saveDirectory = GetDefaultSaveDirectory();
        EnsureDirectoryExists(g_saveDirectory);
    }

    SaveAppSettings();
}

static bool LooksLikeFilePath(const std::wstring& path) {
    size_t slashPos = path.find_last_of(L"\\/");
    size_t dotPos = path.find_last_of(L'.');
    return dotPos != std::wstring::npos &&
           (slashPos == std::wstring::npos || dotPos > slashPos);
}

static bool ResolveSavePath(const std::wstring& userInput,
                            const std::wstring& fileName,
                            std::wstring& outFilePath,
                            std::wstring& outDirectory,
                            std::wstring& outError) {
    std::wstring input = NormalizePathInput(userInput);
    if (input.empty()) input = g_saveDirectory;
    if (input.empty()) input = GetDefaultSaveDirectory();

    input = ToAbsolutePath(input);
    if (input.empty()) {
        outError = L"保存目录无效";
        return false;
    }

    DWORD attrs = GetFileAttributesW(input.c_str());
    std::wstring filePath;
    std::wstring dirPath;

    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            dirPath = input;
        } else {
            filePath = input;
            dirPath = GetParentDirectory(filePath);
        }
    } else {
        const bool treatAsDirectory =
            !input.empty() && (input.back() == L'\\' || !LooksLikeFilePath(input));
        if (treatAsDirectory) {
            dirPath = input;
        } else {
            filePath = input;
            dirPath = GetParentDirectory(filePath);
        }
    }

    dirPath = TrimDirectorySuffix(dirPath);
    if (dirPath.empty()) {
        outError = L"保存路径缺少有效目录";
        return false;
    }

    if (!EnsureDirectoryExists(dirPath)) {
        outError = L"保存目录不可用或无法创建";
        return false;
    }

    std::wstring safeFileName = fileName.empty() ? L"download" : fileName;
    if (filePath.empty()) {
        filePath = CombinePath(dirPath, safeFileName);
    }

    outDirectory = dirPath;
    outFilePath = filePath;
    outError.clear();
    return true;
}

static void JoinDownloadThreadIfNeeded() {
    if (g_downloadThread.joinable()) {
        g_downloadThread.join();
    }
}

static void ApplyWinUI3LikeWindowChrome(HWND hWnd) {
    const DWORD cornerPreference = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hWnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &cornerPreference, sizeof(cornerPreference));

    const COLORREF captionColor = kBackgroundColor;
    DwmSetWindowAttribute(hWnd, DWMWA_CAPTION_COLOR,
                          &captionColor, sizeof(captionColor));

    const COLORREF captionTextColor = kTextColor;
    DwmSetWindowAttribute(hWnd, DWMWA_TEXT_COLOR,
                          &captionTextColor, sizeof(captionTextColor));
}

static bool PasteFilteredNumericText(HWND hEdit) {
    if (!OpenClipboard(hEdit)) return false;

    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (!hData) {
        CloseClipboard();
        return false;
    }

    const wchar_t* clipText = static_cast<const wchar_t*>(GlobalLock(hData));
    if (!clipText) {
        CloseClipboard();
        return false;
    }

    std::wstring filtered;
    for (const wchar_t* p = clipText; *p != L'\0'; ++p) {
        if (iswdigit(*p)) filtered.push_back(*p);
    }

    GlobalUnlock(hData);
    CloseClipboard();

    if (filtered.empty()) return true;
    SendMessageW(hEdit, EM_REPLACESEL, TRUE,
                 reinterpret_cast<LPARAM>(filtered.c_str()));
    return true;
}

static bool HandleEditCommand(HWND hEdit, UINT commandId) {
    const bool numericOnly =
        (GetWindowLongPtrW(hEdit, GWL_STYLE) & ES_NUMBER) != 0;

    switch (commandId) {
    case IDM_EDIT_UNDO:
        SendMessageW(hEdit, WM_UNDO, 0, 0);
        return true;
    case IDM_EDIT_CUT:
        SendMessageW(hEdit, WM_CUT, 0, 0);
        return true;
    case IDM_EDIT_COPY:
        SendMessageW(hEdit, WM_COPY, 0, 0);
        return true;
    case IDM_EDIT_PASTE:
        if (numericOnly) return PasteFilteredNumericText(hEdit);
        SendMessageW(hEdit, WM_PASTE, 0, 0);
        return true;
    case IDM_EDIT_DELETE:
        SendMessageW(hEdit, WM_CLEAR, 0, 0);
        return true;
    case IDM_EDIT_SELECT_ALL:
        SendMessageW(hEdit, EM_SETSEL, 0, -1);
        return true;
    default:
        return false;
    }
}

static LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT msg,
                                         WPARAM wParam, LPARAM lParam,
                                         UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_KEYDOWN:
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            switch (wParam) {
            case 'A':
                SendMessageW(hWnd, EM_SETSEL, 0, -1);
                return 0;
            default:
                break;
            }
        }
        break;

    case WM_PASTE:
        if ((GetWindowLongPtrW(hWnd, GWL_STYLE) & ES_NUMBER) != 0) {
            PasteFilteredNumericText(hWnd);
            return 0;
        }
        break;

    case WM_CONTEXTMENU: {
        HMENU menu = CreatePopupMenu();
        if (!menu) break;

        DWORD selStart = 0;
        DWORD selEnd = 0;
        SendMessageW(hWnd, EM_GETSEL,
                     reinterpret_cast<WPARAM>(&selStart),
                     reinterpret_cast<LPARAM>(&selEnd));
        const bool hasSelection = selEnd > selStart;

        AppendMenuW(menu, MF_STRING, IDM_EDIT_UNDO, L"撤销");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_EDIT_CUT, L"剪切");
        AppendMenuW(menu, MF_STRING, IDM_EDIT_COPY, L"复制");
        AppendMenuW(menu, MF_STRING, IDM_EDIT_PASTE, L"粘贴");
        AppendMenuW(menu, MF_STRING, IDM_EDIT_DELETE, L"删除");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_EDIT_SELECT_ALL, L"全选");

        if (!SendMessageW(hWnd, EM_CANUNDO, 0, 0)) {
            EnableMenuItem(menu, IDM_EDIT_UNDO, MF_BYCOMMAND | MF_GRAYED);
        }
        if (!hasSelection) {
            EnableMenuItem(menu, IDM_EDIT_CUT, MF_BYCOMMAND | MF_GRAYED);
            EnableMenuItem(menu, IDM_EDIT_COPY, MF_BYCOMMAND | MF_GRAYED);
            EnableMenuItem(menu, IDM_EDIT_DELETE, MF_BYCOMMAND | MF_GRAYED);
        }
        if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            EnableMenuItem(menu, IDM_EDIT_PASTE, MF_BYCOMMAND | MF_GRAYED);
        }

        POINT pt{};
        if ((short)LOWORD(lParam) == -1 && (short)HIWORD(lParam) == -1) {
            GetCaretPos(&pt);
            ClientToScreen(hWnd, &pt);
        } else {
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
        }

        const UINT cmd = TrackPopupMenu(
            menu, TPM_RIGHTBUTTON | TPM_RETURNCMD,
            pt.x, pt.y, 0, hWnd, nullptr);
        if (cmd != 0) {
            HandleEditCommand(hWnd, cmd);
        }
        DestroyMenu(menu);
        return 0;
    }

    case WM_NCDESTROY:
        RemoveWindowSubclass(hWnd, EditSubclassProc, 1);
        break;

    default:
        break;
    }
    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

static std::wstring S2W(const std::string& str) {
    if (str.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring r(static_cast<size_t>(len) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &r[0], len);
    return r;
}

static std::string W2S(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1,
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string r(static_cast<size_t>(len) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1,
                        &r[0], len, nullptr, nullptr);
    return r;
}

static std::string FormatSize(int64_t bytes) {
    char buf[64];
    if (bytes < 0) { snprintf(buf, sizeof(buf), "未知"); return buf; }
    if (bytes < 1024) snprintf(buf, sizeof(buf), "%lld B", (long long)bytes);
    else if (bytes < 1048576) snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    else if (bytes < 1073741824LL) snprintf(buf, sizeof(buf), "%.2f MB", bytes / 1048576.0);
    else snprintf(buf, sizeof(buf), "%.2f GB", bytes / 1073741824.0);
    return buf;
}

static std::string FormatSpeed(double bps) {
    char buf[64];
    if (bps <= 0) { snprintf(buf, sizeof(buf), "0 B/s"); return buf; }
    if (bps < 1024) snprintf(buf, sizeof(buf), "%.0f B/s", bps);
    else if (bps < 1048576) snprintf(buf, sizeof(buf), "%.1f KB/s", bps / 1024.0);
    else snprintf(buf, sizeof(buf), "%.2f MB/s", bps / 1048576.0);
    return buf;
}

static std::string FormatEta(int64_t seconds) {
    if (seconds < 0) return "--:--";
    int h = static_cast<int>(seconds / 3600);
    int m = static_cast<int>((seconds % 3600) / 60);
    int s = static_cast<int>(seconds % 60);
    char buf[32];
    if (h > 0) snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    else snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
    return buf;
}

static HWND CreateLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h) {
    return CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                         x, y, w, h, parent, nullptr, g_hInst, nullptr);
}

static HWND CreateEdit(HWND parent, int id, const wchar_t* text,
                       int x, int y, int w, int h, DWORD style = 0) {
    HWND hEdit = CreateWindowExW(
        0, L"EDIT", text,
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | style,
        x, y, w, h, parent, (HMENU)(intptr_t)id, g_hInst, nullptr);
    if (hEdit) {
        SetWindowTheme(hEdit, L"Explorer", nullptr);
        SetWindowSubclass(hEdit, EditSubclassProc, 1, 0);
    }
    return hEdit;
}

static HWND CreateBtn(HWND parent, int id, const wchar_t* text,
                      int x, int y, int w, int h, DWORD style = 0) {
    HWND hBtn = CreateWindowW(L"BUTTON", text,
                              WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | style,
                              x, y, w, h, parent, (HMENU)(intptr_t)id,
                              g_hInst, nullptr);
    return hBtn;
}

static void CreateControls(HWND hWnd) {
    if (!g_hMainFont) {
        g_hMainFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Variable Text");
    }
    if (!g_hPrimaryBtnFont) {
        g_hPrimaryBtnFont = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Variable Text");
    }

    int y = 18, gap = 40, lblW = 82, left = 20;

    CreateLabel(hWnd, L"下载链接:", left, y + 3, lblW, 22);
    CreateEdit(hWnd, IDC_URL_EDIT, L"", left + lblW, y, 540, 26);

    y += gap;
    CreateLabel(hWnd, L"保存目录:", left, y + 3, lblW, 22);
    CreateEdit(hWnd, IDC_PATH_EDIT, g_saveDirectory.c_str(), left + lblW, y, 460, 26);
    CreateBtn(hWnd, IDC_PATH_BROWSE, L"浏览...", left + lblW + 468, y, 72, 26);

    y += gap;
    CreateLabel(hWnd, L"线程数:", left, y + 3, 55, 22);
    CreateEdit(hWnd, IDC_THREADS_EDIT, L"8", left + 55, y, 45, 26, ES_NUMBER);
    CreateLabel(hWnd, L"限速(KB/s):", left + 120, y + 3, 85, 22);
    CreateEdit(hWnd, IDC_SPEEDLIMIT_EDIT, L"0", left + 205, y, 75, 26, ES_NUMBER);
    CreateLabel(hWnd, L"(0=不限速)", left + 285, y + 3, 80, 22);

    y += gap;
    CreateLabel(hWnd, L"UA伪装:", left, y + 3, 65, 22);
    HWND hCombo = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        left + 65, y, 165, 200, hWnd, (HMENU)(intptr_t)IDC_UA_COMBO,
        g_hInst, nullptr);
    SetWindowTheme(hCombo, L"Explorer", nullptr);
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"PCL2 模式");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Chrome 浏览器");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Firefox 浏览器");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"百度网盘");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"自定义");
    SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
    CreateEdit(hWnd, IDC_UA_CUSTOM, L"", left + 240, y, 320, 26);
    EnableWindow(GetDlgItem(hWnd, IDC_UA_CUSTOM), FALSE);

    y += gap + 5;
    HWND hDownBtn = CreateBtn(hWnd, IDC_DOWNLOAD_BTN, L"开始下载", left, y, 620, 38);
    SendMessageW(hDownBtn, WM_SETFONT, (WPARAM)g_hPrimaryBtnFont, TRUE);

    y += 50;
    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icex);
    HWND hProg = CreateWindowW(PROGRESS_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        left, y, 620, 26, hWnd, (HMENU)(intptr_t)IDC_PROGRESS,
        g_hInst, nullptr);
    SetWindowTheme(hProg, L"Explorer", nullptr);
    SendMessageW(hProg, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));

    y += 36;
    HWND hStatus = CreateWindowW(L"STATIC", L"就绪",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        left, y, 620, 22, hWnd, (HMENU)(intptr_t)IDC_STATUS_LABEL,
        g_hInst, nullptr);

    y += 28;
    HWND hInfo = CreateWindowW(L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        left, y, 620, 22, hWnd, (HMENU)(intptr_t)IDC_INFO_LABEL,
        g_hInst, nullptr);

    y += 35;
    CreateBtn(hWnd, IDC_PAUSE_BTN, L"暂停", left, y, 100, 32);
    CreateBtn(hWnd, IDC_CANCEL_BTN, L"取消", left + 110, y, 100, 32);
    EnableWindow(GetDlgItem(hWnd, IDC_PAUSE_BTN), FALSE);
    EnableWindow(GetDlgItem(hWnd, IDC_CANCEL_BTN), FALSE);

    EnumChildWindows(hWnd, [](HWND child, LPARAM lParam) -> BOOL {
        SendMessageW(child, WM_SETFONT, (WPARAM)lParam, TRUE);
        return TRUE;
    }, (LPARAM)g_hMainFont);

    SendMessageW(hStatus, WM_SETFONT, (WPARAM)g_hMainFont, TRUE);
    SendMessageW(hInfo, WM_SETFONT, (WPARAM)g_hMainFont, TRUE);
}

static void UpdateProgressDisplay(HWND hWnd) {
    auto p = g_downloader.getProgress();
    SendDlgItemMessageW(hWnd, IDC_PROGRESS, PBM_SETPOS,
                        static_cast<WPARAM>(p.percentage * 10), 0);
    SetDlgItemTextW(hWnd, IDC_STATUS_LABEL, S2W(p.statusText).c_str());

    std::string info = FormatSpeed(p.speed) + " | " +
        FormatSize(p.downloadedBytes) + " / " +
        (p.totalBytes > 0 ? FormatSize(p.totalBytes) : "未知") + " | " +
        "分片: " + std::to_string(p.completedChunks) + "/" +
        std::to_string(p.totalChunks) +
        (p.etaSeconds > 0 ? " | 剩余: " + FormatEta(p.etaSeconds) : "");
    SetDlgItemTextW(hWnd, IDC_INFO_LABEL, S2W(info).c_str());
}

static void StartDownload(HWND hWnd) {
    if (g_downloadActive.load(std::memory_order_relaxed)) return;

    JoinDownloadThreadIfNeeded();

    WCHAR urlBuf[4096] = {};
    GetDlgItemTextW(hWnd, IDC_URL_EDIT, urlBuf, 4096);
    std::string url = W2S(urlBuf);
    if (url.empty()) {
        MessageBoxW(hWnd, L"请输入下载链接", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }

    wchar_t pathBuf[4096] = {};
    GetDlgItemTextW(hWnd, IDC_PATH_EDIT, pathBuf, 4096);

    const std::wstring fileName =
        S2W(MultiThreadDownloader::extractFilename(url));
    std::wstring finalPathW;
    std::wstring saveDirW;
    std::wstring pathError;
    if (!ResolveSavePath(pathBuf, fileName, finalPathW, saveDirW, pathError)) {
        MessageBoxW(hWnd, pathError.c_str(), L"路径错误", MB_OK | MB_ICONWARNING);
        return;
    }

    std::string savePath = W2S(finalPathW);
    if (savePath.empty()) {
        MessageBoxW(hWnd, L"保存路径编码失败", L"路径错误", MB_OK | MB_ICONWARNING);
        return;
    }

    g_saveDirectory = saveDirW;
    SaveAppSettings();
    SetDlgItemTextW(hWnd, IDC_PATH_EDIT, g_saveDirectory.c_str());

    int threads = GetDlgItemInt(hWnd, IDC_THREADS_EDIT, nullptr, FALSE);
    if (threads <= 0) threads = 8;
    if (threads > 128) threads = 128;

    int speedKB = GetDlgItemInt(hWnd, IDC_SPEEDLIMIT_EDIT, nullptr, FALSE);
    int64_t speedLimit = speedKB > 0 ? static_cast<int64_t>(speedKB) * 1024 : 0;

    int uaIdx = static_cast<int>(
        SendDlgItemMessageW(hWnd, IDC_UA_COMBO, CB_GETCURSEL, 0, 0));
    std::string ua;
    switch (uaIdx) {
    case 0:
        ua = "PCL2/MultiThreadDownloader Mozilla/5.0 AppleWebKit/537.36 Chrome/63.0.3239.132 Safari/537.36";
        break;
    case 1:
        ua = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
        break;
    case 2:
        ua = "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:121.0) Gecko/20100101 Firefox/121.0";
        break;
    case 3:
        ua = "LogStatistic";
        break;
    case 4: {
        WCHAR uaBuf[512] = {};
        GetDlgItemTextW(hWnd, IDC_UA_CUSTOM, uaBuf, 512);
        ua = W2S(uaBuf);
        if (ua.empty()) ua = "PCL2/MultiThreadDownloader";
        break;
    }
    default:
        ua = "PCL2/MultiThreadDownloader";
    }

    DownloadConfig config;
    config.threadCount = threads;
    config.speedLimit = speedLimit;
    config.userAgent = ua;
    g_downloader.setConfig(config);

    EnableWindow(GetDlgItem(hWnd, IDC_DOWNLOAD_BTN), FALSE);
    EnableWindow(GetDlgItem(hWnd, IDC_CANCEL_BTN), TRUE);
    EnableWindow(GetDlgItem(hWnd, IDC_PAUSE_BTN), TRUE);
    SetDlgItemTextW(hWnd, IDC_PAUSE_BTN, L"暂停");
    EnableWindow(GetDlgItem(hWnd, IDC_URL_EDIT), FALSE);
    EnableWindow(GetDlgItem(hWnd, IDC_PATH_EDIT), FALSE);
    EnableWindow(GetDlgItem(hWnd, IDC_PATH_BROWSE), FALSE);
    EnableWindow(GetDlgItem(hWnd, IDC_THREADS_EDIT), FALSE);
    EnableWindow(GetDlgItem(hWnd, IDC_SPEEDLIMIT_EDIT), FALSE);
    EnableWindow(GetDlgItem(hWnd, IDC_UA_COMBO), FALSE);
    EnableWindow(GetDlgItem(hWnd, IDC_UA_CUSTOM), FALSE);

    g_downloadActive.store(true, std::memory_order_relaxed);
    g_downloadSuccess.store(false, std::memory_order_relaxed);

    g_downloadThread = std::thread([url, savePath, hWnd]() {
        const bool success = g_downloader.download(url, savePath);
        g_downloadSuccess.store(success, std::memory_order_relaxed);
        g_downloadActive.store(false, std::memory_order_relaxed);
        if (IsWindow(hWnd)) {
            PostMessageW(hWnd, WM_DOWNLOAD_COMPLETE, success ? 1 : 0, 0);
        }
    });

    SetTimer(hWnd, IDT_PROGRESS, 150, nullptr);
}

static void OnDownloadComplete(HWND hWnd, bool success) {
    KillTimer(hWnd, IDT_PROGRESS);
    JoinDownloadThreadIfNeeded();

    UpdateProgressDisplay(hWnd);

    EnableWindow(GetDlgItem(hWnd, IDC_DOWNLOAD_BTN), TRUE);
    EnableWindow(GetDlgItem(hWnd, IDC_CANCEL_BTN), FALSE);
    EnableWindow(GetDlgItem(hWnd, IDC_PAUSE_BTN), FALSE);
    EnableWindow(GetDlgItem(hWnd, IDC_URL_EDIT), TRUE);
    EnableWindow(GetDlgItem(hWnd, IDC_PATH_EDIT), TRUE);
    EnableWindow(GetDlgItem(hWnd, IDC_PATH_BROWSE), TRUE);
    EnableWindow(GetDlgItem(hWnd, IDC_THREADS_EDIT), TRUE);
    EnableWindow(GetDlgItem(hWnd, IDC_SPEEDLIMIT_EDIT), TRUE);
    EnableWindow(GetDlgItem(hWnd, IDC_UA_COMBO), TRUE);
    int idx = static_cast<int>(
        SendDlgItemMessageW(hWnd, IDC_UA_COMBO, CB_GETCURSEL, 0, 0));
    EnableWindow(GetDlgItem(hWnd, IDC_UA_CUSTOM), idx == 4);

    if (success) {
        MessageBoxW(hWnd, L"下载完成!", L"成功", MB_OK | MB_ICONINFORMATION);
    } else {
        auto p = g_downloader.getProgress();
        std::string msg = "下载失败: " + p.statusText;
        MessageBoxW(hWnd, S2W(msg).c_str(), L"失败", MB_OK | MB_ICONERROR);
    }
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        LoadAppSettings();
        if (!g_hBackgroundBrush) {
            g_hBackgroundBrush = CreateSolidBrush(kBackgroundColor);
        }
        ApplyWinUI3LikeWindowChrome(hWnd);
        CreateControls(hWnd);
        return 0;

    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hWnd, &rc);
        FillRect((HDC)wParam, &rc, g_hBackgroundBrush);
        return 1;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, kTextColor);
        return reinterpret_cast<LRESULT>(g_hBackgroundBrush);
    }

    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (!dis || dis->CtlType != ODT_BUTTON) break;

        const bool isPrimary = dis->CtlID == IDC_DOWNLOAD_BTN;
        const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
        const bool pressed = (dis->itemState & ODS_SELECTED) != 0;

        COLORREF bg = isPrimary ? kPrimaryBtnColor : kSecondaryBtnColor;
        COLORREF border = isPrimary ? kPrimaryBtnColor : kSecondaryBorderColor;
        COLORREF text = isPrimary ? RGB(255, 255, 255) : kTextColor;

        if (pressed && !disabled) {
            bg = isPrimary ? kPrimaryBtnPressedColor : kSecondaryBtnPressedColor;
            border = isPrimary ? kPrimaryBtnPressedColor : RGB(166, 177, 194);
        }
        if (disabled) {
            bg = kDisabledColor;
            border = kDisabledColor;
            text = kDisabledTextColor;
        }

        HBRUSH bgBrush = CreateSolidBrush(bg);
        HPEN borderPen = CreatePen(PS_SOLID, 1, border);
        HGDIOBJ oldBrush = SelectObject(dis->hDC, bgBrush);
        HGDIOBJ oldPen = SelectObject(dis->hDC, borderPen);

        RECT rc = dis->rcItem;
        RoundRect(dis->hDC, rc.left, rc.top, rc.right, rc.bottom, 8, 8);

        SelectObject(dis->hDC, oldBrush);
        SelectObject(dis->hDC, oldPen);
        DeleteObject(bgBrush);
        DeleteObject(borderPen);

        wchar_t caption[256] = {};
        GetWindowTextW(dis->hwndItem, caption, 256);

        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, text);
        HFONT drawFont = isPrimary && g_hPrimaryBtnFont ? g_hPrimaryBtnFont : g_hMainFont;
        HGDIOBJ oldFont = SelectObject(dis->hDC, drawFont);
        DrawTextW(dis->hDC, caption, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dis->hDC, oldFont);

        if ((dis->itemState & ODS_FOCUS) != 0) {
            RECT focus = dis->rcItem;
            InflateRect(&focus, -4, -4);
            DrawFocusRect(dis->hDC, &focus);
        }

        return TRUE;
    }

    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        switch (wmId) {
        case IDC_DOWNLOAD_BTN:
            StartDownload(hWnd);
            break;
        case IDC_PATH_BROWSE: {
            WCHAR szDir[MAX_PATH] = {};
            BROWSEINFOW bi = {};
            bi.hwndOwner = hWnd;
            bi.pszDisplayName = szDir;
            bi.lpszTitle = L"选择保存文件夹";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                SHGetPathFromIDListW(pidl, szDir);
                std::wstring selectedDir = TrimDirectorySuffix(NormalizePathInput(szDir));
                if (EnsureDirectoryExists(selectedDir)) {
                    g_saveDirectory = selectedDir;
                    SaveAppSettings();
                    SetDlgItemTextW(hWnd, IDC_PATH_EDIT, g_saveDirectory.c_str());
                } else {
                    MessageBoxW(hWnd, L"目录不可用", L"路径错误", MB_OK | MB_ICONWARNING);
                }
                CoTaskMemFree(pidl);
            }
            break;
        }
        case IDC_PAUSE_BTN:
            if (g_downloader.isPaused()) {
                g_downloader.resume();
                SetDlgItemTextW(hWnd, IDC_PAUSE_BTN, L"暂停");
            } else {
                g_downloader.pause();
                SetDlgItemTextW(hWnd, IDC_PAUSE_BTN, L"继续");
            }
            break;
        case IDC_CANCEL_BTN:
            g_downloader.cancel();
            break;
        case IDC_UA_COMBO: {
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                int idx = static_cast<int>(
                    SendDlgItemMessageW(hWnd, IDC_UA_COMBO, CB_GETCURSEL, 0, 0));
                EnableWindow(GetDlgItem(hWnd, IDC_UA_CUSTOM), idx == 4);
            }
            break;
        }
        case IDM_EXIT:
            DestroyWindow(hWnd);
            break;
        default:
            return DefWindowProcW(hWnd, message, wParam, lParam);
        }
        break;
    }

    case WM_TIMER:
        if (wParam == IDT_PROGRESS && g_downloadActive.load(std::memory_order_relaxed)) {
            UpdateProgressDisplay(hWnd);
        }
        break;

    case WM_DOWNLOAD_COMPLETE:
        OnDownloadComplete(hWnd, wParam != 0);
        break;

    case WM_DESTROY:
        if (GetDlgItem(hWnd, IDC_PATH_EDIT)) {
            wchar_t pathBuf[4096] = {};
            GetDlgItemTextW(hWnd, IDC_PATH_EDIT, pathBuf, 4096);
            std::wstring finalPathW;
            std::wstring saveDirW;
            std::wstring err;
            if (ResolveSavePath(pathBuf, L"download.tmp", finalPathW, saveDirW, err)) {
                g_saveDirectory = saveDirW;
            }
        }
        SaveAppSettings();
        g_downloader.cancel();
        JoinDownloadThreadIfNeeded();
        KillTimer(hWnd, IDT_PROGRESS);
        if (g_hBackgroundBrush) {
            DeleteObject(g_hBackgroundBrush);
            g_hBackgroundBrush = nullptr;
        }
        if (g_hMainFont) {
            DeleteObject(g_hMainFont);
            g_hMainFont = nullptr;
        }
        if (g_hPrimaryBtnFont) {
            DeleteObject(g_hPrimaryBtnFont);
            g_hPrimaryBtnFont = nullptr;
        }
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }
    return 0;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                      _In_opt_ HINSTANCE hPrevInstance,
                      _In_ LPWSTR lpCmdLine,
                      _In_ int nCmdShow) {
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    g_hInst = hInstance;

    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(wcex);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(107));
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = nullptr;
    wcex.lpszClassName = L"PCLDownloader";
    RegisterClassExW(&wcex);

    HWND hWnd = CreateWindowExW(
        0, L"PCLDownloader", L"PCL 多线程下载引擎",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, 0, 680, 470,
        nullptr, nullptr, hInstance, nullptr);

    if (!hWnd) return FALSE;

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}
