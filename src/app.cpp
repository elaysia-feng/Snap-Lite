#include "app.h"

#include "capture.h"
#include "editor_window.h"
#include "pin_window.h"
#include "snip_window.h"

#include <shellapi.h>

#include <string>

namespace snaplite {
namespace {
constexpr wchar_t kMainClass[] = L"SnapLiteMainWindow";
constexpr wchar_t kAppName[] = L"Snap-Lite";
constexpr UINT WM_TRAYICON = WM_APP + 1;

constexpr int HOTKEY_SNIP = 1;
constexpr int HOTKEY_PIN = 2;
constexpr int HOTKEY_FULLSCREEN = 3;

constexpr UINT CMD_SNIP = 1001;
constexpr UINT CMD_FULLSCREEN = 1002;
constexpr UINT CMD_PIN = 1003;
constexpr UINT CMD_OPEN_FOLDER = 1004;
constexpr UINT CMD_EXIT = 1005;
}

App::App(HINSTANCE instance) : instance_(instance) {}

App::~App() {
    Shutdown();
}

bool App::Initialize() {
    singleInstance_ = CreateMutexW(nullptr, TRUE, L"Local\\SnapLiteSingleInstance");
    if (!singleInstance_ || GetLastError() == ERROR_ALREADY_EXISTS) {
        return false;
    }

    Gdiplus::GdiplusStartupInput gdiplusInput;
    if (Gdiplus::GdiplusStartup(&gdiplusToken_, &gdiplusInput, nullptr) != Gdiplus::Ok) {
        return false;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance_;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kMainClass;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    if (!SnipWindow::Register(instance_) || !PinWindow::Register(instance_) || !EditorWindow::Register(instance_)) {
        return false;
    }

    hwnd_ = CreateWindowExW(
        0,
        kMainClass,
        kAppName,
        WS_OVERLAPPED,
        0,
        0,
        0,
        0,
        nullptr,
        nullptr,
        instance_,
        this);
    if (!hwnd_) {
        return false;
    }

    RegisterHotKey(hwnd_, HOTKEY_SNIP, MOD_NOREPEAT, VK_F1);
    RegisterHotKey(hwnd_, HOTKEY_PIN, MOD_NOREPEAT, VK_F3);
    RegisterHotKey(hwnd_, HOTKEY_FULLSCREEN, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'F');

    AddTrayIcon();
    return true;
}

void App::Shutdown() {
    if (hwnd_) {
        UnregisterHotKey(hwnd_, HOTKEY_SNIP);
        UnregisterHotKey(hwnd_, HOTKEY_PIN);
        UnregisterHotKey(hwnd_, HOTKEY_FULLSCREEN);
    }

    if (tray_.hWnd) {
        Shell_NotifyIconW(NIM_DELETE, &tray_);
        tray_ = {};
    }

    if (gdiplusToken_) {
        Gdiplus::GdiplusShutdown(gdiplusToken_);
        gdiplusToken_ = 0;
    }

    if (singleInstance_) {
        ReleaseMutex(singleInstance_);
        CloseHandle(singleInstance_);
        singleInstance_ = nullptr;
    }
}

void App::AddTrayIcon() {
    tray_ = {};
    tray_.cbSize = sizeof(tray_);
    tray_.hWnd = hwnd_;
    tray_.uID = 1;
    tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    tray_.uCallbackMessage = WM_TRAYICON;
    tray_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(tray_.szTip, L"Snap-Lite · F1 截图 · F3 贴图");
    Shell_NotifyIconW(NIM_ADD, &tray_);
}

void App::ShowNotice(const wchar_t* message) {
    if (!tray_.hWnd) {
        return;
    }

    tray_.uFlags = NIF_INFO;
    wcscpy_s(tray_.szInfoTitle, kAppName);
    wcsncpy_s(tray_.szInfo, message, _TRUNCATE);
    tray_.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &tray_);
    tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
}

void App::ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    AppendMenuW(menu, MF_STRING, CMD_SNIP, L"截图\tF1");
    AppendMenuW(menu, MF_STRING, CMD_PIN, L"贴图\tF3");
    AppendMenuW(menu, MF_STRING, CMD_FULLSCREEN, L"全屏截图\tCtrl+Shift+F");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_OPEN_FOLDER, L"打开截图目录");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_EXIT, L"退出 Snap-Lite");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN, cursor.x, cursor.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

void App::StartSnip() {
    const bool started = SnipWindow::Start(instance_, hwnd_, [this](HBITMAP bitmap) {
        if (!EditorWindow::Open(instance_, bitmap, [this](HBITMAP edited) { CommitCapture(edited); })) {
            ShowNotice(L"无法打开标注编辑器");
        }
    });

    if (!started) {
        ShowNotice(L"无法启动截图");
    }
}

void App::CaptureFullscreen() {
    HBITMAP bitmap = CaptureVirtualScreen();
    if (!bitmap) {
        ShowNotice(L"全屏截图失败");
        return;
    }
    CommitCapture(bitmap);
}

void App::PinClipboard() {
    if (!PinWindow::CreateFromClipboard(instance_)) {
        ShowNotice(L"剪贴板中没有可贴出的图片");
    }
}

void App::CommitCapture(HBITMAP bitmap) {
    if (!bitmap) {
        ShowNotice(L"截图失败");
        return;
    }

    const auto path = NextScreenshotPath();
    const bool copied = CopyBitmapToClipboard(hwnd_, bitmap);
    const bool saved = SaveBitmapPng(bitmap, path);
    DeleteObject(bitmap);

    if (copied && saved) {
        const std::wstring message = L"已复制到剪贴板并保存：\n" + path.wstring();
        ShowNotice(message.c_str());
    } else if (copied) {
        ShowNotice(L"已复制到剪贴板");
    } else if (saved) {
        ShowNotice(L"截图已保存");
    } else {
        ShowNotice(L"截图处理失败");
    }
}

LRESULT App::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_HOTKEY:
        if (wParam == HOTKEY_SNIP) {
            StartSnip();
        } else if (wParam == HOTKEY_PIN) {
            PinClipboard();
        } else if (wParam == HOTKEY_FULLSCREEN) {
            CaptureFullscreen();
        }
        return 0;

    case WM_TRAYICON:
        if (lParam == WM_LBUTTONUP) {
            StartSnip();
        } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
            ShowTrayMenu();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case CMD_SNIP:
            StartSnip();
            return 0;
        case CMD_PIN:
            PinClipboard();
            return 0;
        case CMD_FULLSCREEN:
            CaptureFullscreen();
            return 0;
        case CMD_OPEN_FOLDER:
            ShellExecuteW(hwnd_, L"open", ScreenshotDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        case CMD_EXIT:
            DestroyWindow(hwnd_);
            return 0;
        default:
            break;
        }
        break;

    case WM_DESTROY:
        hwnd_ = nullptr;
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

LRESULT CALLBACK App::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    App* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<App*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    return self ? self->HandleMessage(message, wParam, lParam)
                : DefWindowProcW(hwnd, message, wParam, lParam);
}

int App::Run(int) {
    if (!Initialize()) {
        return 0;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

}  // namespace snaplite
