#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <gdiplus.h>

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>

#pragma comment(lib, "gdiplus.lib")

namespace {
constexpr wchar_t kAppClass[] = L"SnapLiteMainWindow";
constexpr wchar_t kOverlayClass[] = L"SnapLiteOverlayWindow";
constexpr wchar_t kAppName[] = L"Snap-Lite";

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr int HOTKEY_REGION = 1;
constexpr int HOTKEY_FULLSCREEN = 2;

constexpr UINT CMD_REGION = 1001;
constexpr UINT CMD_FULLSCREEN = 1002;
constexpr UINT CMD_OPEN_FOLDER = 1003;
constexpr UINT CMD_EXIT = 1004;

HWND g_mainWindow = nullptr;
HWND g_overlayWindow = nullptr;
NOTIFYICONDATAW g_tray{};
ULONG_PTR g_gdiplusToken = 0;
HANDLE g_singleInstance = nullptr;

HBITMAP g_overlayCapture = nullptr;
bool g_dragging = false;
POINT g_dragStart{};
POINT g_dragCurrent{};

RECT NormalizeRect(POINT a, POINT b) {
    RECT rect{};
    rect.left = std::min(a.x, b.x);
    rect.top = std::min(a.y, b.y);
    rect.right = std::max(a.x, b.x);
    rect.bottom = std::max(a.y, b.y);
    return rect;
}

std::filesystem::path SaveDirectory() {
    PWSTR pictures = nullptr;
    std::filesystem::path dir;

    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_DEFAULT, nullptr, &pictures))) {
        dir = std::filesystem::path(pictures) / L"Snap-Lite";
        CoTaskMemFree(pictures);
    } else {
        dir = std::filesystem::current_path() / L"Snap-Lite";
    }

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::filesystem::path NextScreenshotPath() {
    SYSTEMTIME st{};
    GetLocalTime(&st);

    wchar_t filename[96]{};
    swprintf_s(
        filename,
        L"SnapLite_%04u-%02u-%02u_%02u-%02u-%02u-%03u.png",
        st.wYear,
        st.wMonth,
        st.wDay,
        st.wHour,
        st.wMinute,
        st.wSecond,
        st.wMilliseconds);

    return SaveDirectory() / filename;
}

int GetEncoderClsid(const WCHAR* format, CLSID* clsid) {
    UINT count = 0;
    UINT bytes = 0;
    Gdiplus::GetImageEncodersSize(&count, &bytes);
    if (bytes == 0) {
        return -1;
    }

    auto* data = static_cast<Gdiplus::ImageCodecInfo*>(malloc(bytes));
    if (!data) {
        return -1;
    }

    Gdiplus::GetImageEncoders(count, bytes, data);
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(data[i].MimeType, format) == 0) {
            *clsid = data[i].Clsid;
            free(data);
            return static_cast<int>(i);
        }
    }

    free(data);
    return -1;
}

bool SavePng(HBITMAP bitmap, const std::filesystem::path& path) {
    if (!bitmap) {
        return false;
    }

    CLSID encoder{};
    if (GetEncoderClsid(L"image/png", &encoder) < 0) {
        return false;
    }

    Gdiplus::Bitmap image(bitmap, nullptr);
    return image.Save(path.c_str(), &encoder, nullptr) == Gdiplus::Ok;
}

void ShowTrayMessage(const wchar_t* title, const std::wstring& message) {
    if (!g_mainWindow) {
        return;
    }

    g_tray.uFlags = NIF_INFO;
    wcsncpy_s(g_tray.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(g_tray.szInfo, message.c_str(), _TRUNCATE);
    g_tray.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_tray);

    g_tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
}

HBITMAP CaptureScreenRect(int x, int y, int width, int height) {
    if (width <= 0 || height <= 0) {
        return nullptr;
    }

    HDC screen = GetDC(nullptr);
    if (!screen) {
        return nullptr;
    }

    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    if (!memory || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return nullptr;
    }

    HGDIOBJ old = SelectObject(memory, bitmap);
    const BOOL ok = BitBlt(memory, 0, 0, width, height, screen, x, y, SRCCOPY | CAPTUREBLT);
    SelectObject(memory, old);

    DeleteDC(memory);
    ReleaseDC(nullptr, screen);

    if (!ok) {
        DeleteObject(bitmap);
        return nullptr;
    }
    return bitmap;
}

HBITMAP CaptureVirtualScreen() {
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return CaptureScreenRect(x, y, width, height);
}

HBITMAP CopyBitmapRegion(HBITMAP source, const RECT& rect) {
    if (!source) {
        return nullptr;
    }

    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) {
        return nullptr;
    }

    HDC screen = GetDC(nullptr);
    HDC srcDc = CreateCompatibleDC(screen);
    HDC dstDc = CreateCompatibleDC(screen);
    HBITMAP result = CreateCompatibleBitmap(screen, width, height);

    if (!srcDc || !dstDc || !result) {
        if (result) DeleteObject(result);
        if (srcDc) DeleteDC(srcDc);
        if (dstDc) DeleteDC(dstDc);
        ReleaseDC(nullptr, screen);
        return nullptr;
    }

    HGDIOBJ oldSrc = SelectObject(srcDc, source);
    HGDIOBJ oldDst = SelectObject(dstDc, result);
    const BOOL ok = BitBlt(dstDc, 0, 0, width, height, srcDc, rect.left, rect.top, SRCCOPY);
    SelectObject(srcDc, oldSrc);
    SelectObject(dstDc, oldDst);

    DeleteDC(srcDc);
    DeleteDC(dstDc);
    ReleaseDC(nullptr, screen);

    if (!ok) {
        DeleteObject(result);
        return nullptr;
    }
    return result;
}

bool CopyToClipboard(HBITMAP bitmap) {
    if (!bitmap || !OpenClipboard(g_mainWindow)) {
        return false;
    }

    EmptyClipboard();
    const HANDLE result = SetClipboardData(CF_BITMAP, bitmap);
    CloseClipboard();
    return result != nullptr;
}

void FinishScreenshot(HBITMAP bitmap) {
    if (!bitmap) {
        ShowTrayMessage(kAppName, L"截图失败");
        return;
    }

    const auto path = NextScreenshotPath();
    const bool saved = SavePng(bitmap, path);
    const bool copied = CopyToClipboard(bitmap);

    if (!copied) {
        DeleteObject(bitmap);
    }

    if (saved && copied) {
        ShowTrayMessage(kAppName, L"已复制到剪贴板，并保存到：\n" + path.wstring());
    } else if (saved) {
        ShowTrayMessage(kAppName, L"已保存到：\n" + path.wstring());
    } else if (copied) {
        ShowTrayMessage(kAppName, L"已复制到剪贴板");
    } else {
        ShowTrayMessage(kAppName, L"截图保存失败");
    }
}

void CaptureFullscreen() {
    FinishScreenshot(CaptureVirtualScreen());
}

void PaintOverlay(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);

    RECT client{};
    GetClientRect(hwnd, &client);

    if (g_overlayCapture) {
        HDC memory = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(memory, g_overlayCapture);
        BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old);
        DeleteDC(memory);
    }

    SetBkMode(dc, OPAQUE);
    SetBkColor(dc, RGB(32, 32, 32));
    SetTextColor(dc, RGB(255, 255, 255));
    RECT hint{16, 16, 420, 48};
    DrawTextW(dc, L"拖动选择截图区域  ·  Esc / 右键取消", -1, &hint, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

    if (g_dragging || g_dragStart.x != g_dragCurrent.x || g_dragStart.y != g_dragCurrent.y) {
        RECT selection = NormalizeRect(g_dragStart, g_dragCurrent);

        HPEN shadowPen = CreatePen(PS_SOLID, 4, RGB(0, 0, 0));
        HGDIOBJ oldPen = SelectObject(dc, shadowPen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, selection.left, selection.top, selection.right, selection.bottom);
        SelectObject(dc, oldPen);
        DeleteObject(shadowPen);

        HPEN pen = CreatePen(PS_SOLID, 2, RGB(0, 174, 255));
        oldPen = SelectObject(dc, pen);
        Rectangle(dc, selection.left, selection.top, selection.right, selection.bottom);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(pen);

        const int width = selection.right - selection.left;
        const int height = selection.bottom - selection.top;
        if (width > 0 && height > 0) {
            std::wstringstream label;
            label << width << L" × " << height;
            RECT sizeRect{selection.left, std::max(0L, selection.top - 28L), selection.left + 150, selection.top};
            SetBkColor(dc, RGB(32, 32, 32));
            SetTextColor(dc, RGB(255, 255, 255));
            DrawTextW(dc, label.str().c_str(), -1, &sizeRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        }
    }

    EndPaint(hwnd, &ps);
}

void CancelRegionCapture() {
    if (g_overlayWindow) {
        DestroyWindow(g_overlayWindow);
    }
}

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        return TRUE;

    case WM_PAINT:
        PaintOverlay(hwnd);
        return 0;

    case WM_LBUTTONDOWN:
        g_dragging = true;
        g_dragStart = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        g_dragCurrent = g_dragStart;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_MOUSEMOVE:
        if (g_dragging) {
            g_dragCurrent = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_dragging) {
            g_dragging = false;
            g_dragCurrent = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ReleaseCapture();

            const RECT selection = NormalizeRect(g_dragStart, g_dragCurrent);
            HBITMAP region = nullptr;
            if (selection.right - selection.left >= 3 && selection.bottom - selection.top >= 3) {
                region = CopyBitmapRegion(g_overlayCapture, selection);
            }

            DestroyWindow(hwnd);
            if (region) {
                FinishScreenshot(region);
            }
        }
        return 0;

    case WM_RBUTTONDOWN:
        CancelRegionCapture();
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            CancelRegionCapture();
            return 0;
        }
        break;

    case WM_DESTROY:
        if (g_overlayCapture) {
            DeleteObject(g_overlayCapture);
            g_overlayCapture = nullptr;
        }
        g_overlayWindow = nullptr;
        g_dragging = false;
        g_dragStart = {};
        g_dragCurrent = {};
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void StartRegionCapture() {
    if (g_overlayWindow) {
        SetForegroundWindow(g_overlayWindow);
        return;
    }

    g_overlayCapture = CaptureVirtualScreen();
    if (!g_overlayCapture) {
        ShowTrayMessage(kAppName, L"无法读取屏幕内容");
        return;
    }

    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    g_overlayWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kOverlayClass,
        kAppName,
        WS_POPUP,
        x,
        y,
        width,
        height,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);

    if (!g_overlayWindow) {
        DeleteObject(g_overlayCapture);
        g_overlayCapture = nullptr;
        ShowTrayMessage(kAppName, L"无法创建截图选择窗口");
        return;
    }

    ShowWindow(g_overlayWindow, SW_SHOW);
    UpdateWindow(g_overlayWindow);
    SetForegroundWindow(g_overlayWindow);
    SetFocus(g_overlayWindow);
}

void ShowTrayMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    AppendMenuW(menu, MF_STRING, CMD_REGION, L"区域截图\tCtrl+Shift+A");
    AppendMenuW(menu, MF_STRING, CMD_FULLSCREEN, L"全屏截图\tCtrl+Shift+F");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_OPEN_FOLDER, L"打开截图目录");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_EXIT, L"退出 Snap-Lite");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, cursor.x, cursor.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

void AddTrayIcon(HWND hwnd) {
    g_tray = {};
    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = hwnd;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_tray.uCallbackMessage = WM_TRAYICON;
    g_tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(g_tray.szTip, L"Snap-Lite · Ctrl+Shift+A 截图", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &g_tray);
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_HOTKEY:
        if (wParam == HOTKEY_REGION) {
            StartRegionCapture();
        } else if (wParam == HOTKEY_FULLSCREEN) {
            CaptureFullscreen();
        }
        return 0;

    case WM_TRAYICON:
        if (lParam == WM_LBUTTONUP) {
            StartRegionCapture();
        } else if (lParam == WM_RBUTTONUP) {
            ShowTrayMenu(hwnd);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case CMD_REGION:
            StartRegionCapture();
            return 0;
        case CMD_FULLSCREEN:
            CaptureFullscreen();
            return 0;
        case CMD_OPEN_FOLDER: {
            const auto dir = SaveDirectory();
            ShellExecuteW(hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        case CMD_EXIT:
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_DESTROY:
        CancelRegionCapture();
        UnregisterHotKey(hwnd, HOTKEY_REGION);
        UnregisterHotKey(hwnd, HOTKEY_FULLSCREEN);
        Shell_NotifyIconW(NIM_DELETE, &g_tray);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool RegisterWindows(HINSTANCE instance) {
    WNDCLASSEXW mainClass{};
    mainClass.cbSize = sizeof(mainClass);
    mainClass.hInstance = instance;
    mainClass.lpfnWndProc = MainProc;
    mainClass.lpszClassName = kAppClass;
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);

    WNDCLASSEXW overlayClass{};
    overlayClass.cbSize = sizeof(overlayClass);
    overlayClass.hInstance = instance;
    overlayClass.lpfnWndProc = OverlayProc;
    overlayClass.lpszClassName = kOverlayClass;
    overlayClass.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    overlayClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));

    return RegisterClassExW(&mainClass) != 0 && RegisterClassExW(&overlayClass) != 0;
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDPIAware();

    g_singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\SnapLite.SingleInstance");
    if (!g_singleInstance || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_singleInstance) CloseHandle(g_singleInstance);
        return 0;
    }

    Gdiplus::GdiplusStartupInput gdiplusInput;
    if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr) != Gdiplus::Ok) {
        CloseHandle(g_singleInstance);
        return 1;
    }

    if (!RegisterWindows(instance)) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        CloseHandle(g_singleInstance);
        return 1;
    }

    g_mainWindow = CreateWindowExW(
        0,
        kAppClass,
        kAppName,
        WS_OVERLAPPED,
        0,
        0,
        0,
        0,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (!g_mainWindow) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        CloseHandle(g_singleInstance);
        return 1;
    }

    AddTrayIcon(g_mainWindow);

    RegisterHotKey(g_mainWindow, HOTKEY_REGION, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'A');
    RegisterHotKey(g_mainWindow, HOTKEY_FULLSCREEN, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'F');

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    ReleaseMutex(g_singleInstance);
    CloseHandle(g_singleInstance);
    return static_cast<int>(msg.wParam);
}
