#include "app.h"

#include "capture.h"
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
constexpr int HOTKEY_PIN_FALLBACK = 4;

constexpr UINT CMD_SNIP = 1001;
constexpr UINT CMD_FULLSCREEN = 1002;
constexpr UINT CMD_PIN = 1003;
constexpr UINT CMD_OPEN_FOLDER = 1004;
constexpr UINT CMD_EXIT = 1005;

HICON CreateSnapLiteIcon(int size) {
    HDC screen = GetDC(nullptr);
    if (!screen) return nullptr;

    HDC colorDc = CreateCompatibleDC(screen);
    HDC maskDc = CreateCompatibleDC(screen);
    HBITMAP colorBitmap = CreateCompatibleBitmap(screen, size, size);
    HBITMAP maskBitmap = CreateBitmap(size, size, 1, 1, nullptr);
    if (!colorDc || !maskDc || !colorBitmap || !maskBitmap) {
        if (colorBitmap) DeleteObject(colorBitmap);
        if (maskBitmap) DeleteObject(maskBitmap);
        if (colorDc) DeleteDC(colorDc);
        if (maskDc) DeleteDC(maskDc);
        ReleaseDC(nullptr, screen);
        return nullptr;
    }

    const HGDIOBJ oldColor = SelectObject(colorDc, colorBitmap);
    const HGDIOBJ oldMask = SelectObject(maskDc, maskBitmap);
    PatBlt(maskDc, 0, 0, size, size, BLACKNESS);

    const float u = static_cast<float>(size) / 32.0f;
    {
        Gdiplus::Graphics g(colorDc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        Gdiplus::LinearGradientBrush bg(
            Gdiplus::PointF(0.0f, 0.0f),
            Gdiplus::PointF(static_cast<float>(size), static_cast<float>(size)),
            Gdiplus::Color(255, 35, 108, 244),
            Gdiplus::Color(255, 91, 171, 255));
        g.FillRectangle(&bg, 0.0f, 0.0f, static_cast<float>(size), static_cast<float>(size));

        // Screenshot-frame corners keep the utility identity visible at 16px.
        Gdiplus::Pen frame(Gdiplus::Color(225, 242, 249, 255), 1.7f * u);
        frame.SetStartCap(Gdiplus::LineCapRound);
        frame.SetEndCap(Gdiplus::LineCapRound);
        const float m = 5.0f * u;
        const float arm = 5.0f * u;
        g.DrawLine(&frame, m, m + arm, m, m);
        g.DrawLine(&frame, m, m, m + arm, m);
        g.DrawLine(&frame, size - m - arm, size - m, size - m, size - m);
        g.DrawLine(&frame, size - m, size - m, size - m, size - m - arm);

        // Minimal anime eye: white sclera, cyan iris, navy pupil and a hair swoosh.
        Gdiplus::GraphicsPath eye;
        eye.StartFigure();
        eye.AddBezier(7.0f*u, 17.0f*u, 11.0f*u, 11.5f*u, 21.0f*u, 11.5f*u, 25.0f*u, 16.5f*u);
        eye.AddBezier(25.0f*u, 16.5f*u, 20.5f*u, 22.0f*u, 11.0f*u, 22.0f*u, 7.0f*u, 17.0f*u);
        eye.CloseFigure();
        Gdiplus::SolidBrush eyeWhite(Gdiplus::Color(245, 250, 253, 255));
        g.FillPath(&eyeWhite, &eye);

        Gdiplus::SolidBrush iris(Gdiplus::Color(255, 74, 211, 255));
        g.FillEllipse(&iris, 13.0f*u, 12.8f*u, 8.0f*u, 8.0f*u);
        Gdiplus::SolidBrush pupil(Gdiplus::Color(255, 22, 55, 112));
        g.FillEllipse(&pupil, 15.2f*u, 15.0f*u, 4.1f*u, 4.1f*u);
        Gdiplus::SolidBrush glint(Gdiplus::Color(255, 255, 255, 255));
        g.FillEllipse(&glint, 15.2f*u, 14.0f*u, 1.8f*u, 1.8f*u);

        Gdiplus::Pen lash(Gdiplus::Color(255, 18, 49, 105), 1.7f*u);
        lash.SetStartCap(Gdiplus::LineCapRound);
        lash.SetEndCap(Gdiplus::LineCapRound);
        g.DrawBezier(&lash, 7.0f*u, 16.8f*u, 11.0f*u, 11.3f*u, 21.5f*u, 11.4f*u, 25.2f*u, 16.3f*u);

        Gdiplus::Pen hair(Gdiplus::Color(235, 226, 241, 255), 2.2f*u);
        hair.SetStartCap(Gdiplus::LineCapRound);
        hair.SetEndCap(Gdiplus::LineCapRound);
        g.DrawBezier(&hair, 8.0f*u, 10.0f*u, 12.0f*u, 4.5f*u, 21.0f*u, 5.0f*u, 24.0f*u, 10.0f*u);
        g.DrawBezier(&hair, 10.0f*u, 9.0f*u, 13.0f*u, 6.0f*u, 16.0f*u, 6.0f*u, 17.0f*u, 10.5f*u);
    }

    SelectObject(colorDc, oldColor);
    SelectObject(maskDc, oldMask);

    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = colorBitmap;
    info.hbmMask = maskBitmap;
    HICON icon = CreateIconIndirect(&info);

    DeleteObject(colorBitmap);
    DeleteObject(maskBitmap);
    DeleteDC(colorDc);
    DeleteDC(maskDc);
    ReleaseDC(nullptr, screen);
    return icon;
}
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

    appIcon_ = CreateSnapLiteIcon(32);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance_;
    wc.hIcon = appIcon_ ? appIcon_ : LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kMainClass;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    if (!SnipWindow::Register(instance_) || !PinWindow::Register(instance_)) {
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

    const bool snipHotkeyRegistered =
        RegisterHotKey(hwnd_, HOTKEY_SNIP, MOD_NOREPEAT, VK_F1) != FALSE;
    const bool pinHotkeyRegistered =
        RegisterHotKey(hwnd_, HOTKEY_PIN, MOD_NOREPEAT, VK_F3) != FALSE;
    const bool fullscreenHotkeyRegistered =
        RegisterHotKey(hwnd_, HOTKEY_FULLSCREEN, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'F') != FALSE;
    const bool pinFallbackRegistered = pinHotkeyRegistered ||
        RegisterHotKey(hwnd_, HOTKEY_PIN_FALLBACK, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'P') != FALSE;

    AddTrayIcon();

    if (!pinHotkeyRegistered) {
        if (pinFallbackRegistered) {
            ShowNotice(L"F3 热键被其他程序占用，贴图备用快捷键已切换为 Ctrl+Alt+P");
        } else {
            ShowNotice(L"F3 热键注册失败，可右键托盘图标选择“贴图”");
        }
    } else if (!snipHotkeyRegistered || !fullscreenHotkeyRegistered) {
        ShowNotice(L"部分全局快捷键注册失败，可能被其他程序占用");
    }

    return true;
}

void App::Shutdown() {
    if (hwnd_) {
        UnregisterHotKey(hwnd_, HOTKEY_SNIP);
        UnregisterHotKey(hwnd_, HOTKEY_PIN);
        UnregisterHotKey(hwnd_, HOTKEY_FULLSCREEN);
        UnregisterHotKey(hwnd_, HOTKEY_PIN_FALLBACK);
    }

    if (tray_.hWnd) {
        Shell_NotifyIconW(NIM_DELETE, &tray_);
        tray_ = {};
    }

    if (appIcon_) {
        DestroyIcon(appIcon_);
        appIcon_ = nullptr;
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
    tray_.hIcon = appIcon_ ? appIcon_ : LoadIconW(nullptr, IDI_APPLICATION);
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
    const bool started = SnipWindow::Start(
        instance_,
        hwnd_,
        [this](HBITMAP bitmap, SnipWindow::FinishAction action, const RECT& sourceScreenRect) {
            if (!bitmap) {
                ShowNotice(L"截图失败");
                return;
            }

            if (action == SnipWindow::FinishAction::Save) {
                const auto path = NextScreenshotPath();
                const bool saved = SaveBitmapPng(bitmap, path);
                DeleteObject(bitmap);
                if (saved) {
                    const std::wstring message = L"截图已保存：\n" + path.wstring();
                    ShowNotice(message.c_str());
                } else {
                    ShowNotice(L"截图保存失败");
                }
                return;
            }

            if (action == SnipWindow::FinishAction::Pin) {
                // Keep the normal save + clipboard behavior, but create the pin
                // directly from a clone so the screenshot's desktop position is
                // preserved instead of being lost through a clipboard round trip.
                HBITMAP pinBitmap = CloneBitmap(bitmap);
                CommitCapture(bitmap);
                if (!pinBitmap || !PinWindow::CreateAt(instance_, pinBitmap, sourceScreenRect)) {
                    ShowNotice(L"贴图创建失败");
                }
                return;
            }

            CommitCapture(bitmap);
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
        ShowNotice(L"剪贴板中没有可贴出的图片，支持截图、PNG/DIB 图片和复制的图片文件");
    }
}

void App::CommitCapture(HBITMAP bitmap) {
    if (!bitmap) {
        ShowNotice(L"截图失败");
        return;
    }

    const auto path = NextScreenshotPath();
    const bool saved = SaveBitmapPng(bitmap, path);
    const bool copied = CopyBitmapToClipboard(
        hwnd_,
        bitmap,
        saved ? path : std::filesystem::path{});
    DeleteObject(bitmap);

    if (copied && saved) {
        const std::wstring message = L"已复制，可直接粘贴到支持图片的 CLI：\n" + path.wstring();
        ShowNotice(message.c_str());
    } else if (copied) {
        ShowNotice(L"已复制到图片剪贴板");
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
        } else if (wParam == HOTKEY_PIN || wParam == HOTKEY_PIN_FALLBACK) {
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
