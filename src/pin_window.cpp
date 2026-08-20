#include "pin_window.h"

#include "capture.h"

#include <algorithm>

namespace snaplite {
namespace {
constexpr wchar_t kPinClass[] = L"SnapLitePinWindow";
constexpr UINT CMD_OPACITY_100 = 2001;
constexpr UINT CMD_OPACITY_80 = 2002;
constexpr UINT CMD_OPACITY_60 = 2003;
constexpr UINT CMD_CLOSE = 2004;
}

PinWindow::PinWindow(HINSTANCE instance, HBITMAP bitmap)
    : instance_(instance), bitmap_(bitmap) {
    BITMAP info{};
    if (bitmap_) {
        GetObjectW(bitmap_, sizeof(info), &info);
        bitmapWidth_ = info.bmWidth;
        bitmapHeight_ = info.bmHeight;
    }
}

PinWindow::~PinWindow() {
    if (bitmap_) {
        DeleteObject(bitmap_);
        bitmap_ = nullptr;
    }
}

bool PinWindow::Register(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kPinClass;
    wc.style = CS_DBLCLKS;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool PinWindow::Create(HINSTANCE instance, HBITMAP bitmap) {
    if (!bitmap) {
        return false;
    }

    auto* self = new PinWindow(instance, bitmap);
    if (self->bitmapWidth_ <= 0 || self->bitmapHeight_ <= 0) {
        delete self;
        return false;
    }

    const int maxWidth = static_cast<int>(GetSystemMetrics(SM_CXSCREEN) * 0.65);
    const int maxHeight = static_cast<int>(GetSystemMetrics(SM_CYSCREEN) * 0.65);
    self->zoom_ = std::min({1.0,
        static_cast<double>(maxWidth) / self->bitmapWidth_,
        static_cast<double>(maxHeight) / self->bitmapHeight_});

    const int width = std::max(1, static_cast<int>(self->bitmapWidth_ * self->zoom_));
    const int height = std::max(1, static_cast<int>(self->bitmapHeight_ * self->zoom_));

    POINT cursor{};
    GetCursorPos(&cursor);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        kPinClass,
        L"Snap-Lite Pin",
        WS_POPUP,
        cursor.x + 16,
        cursor.y + 16,
        width,
        height,
        nullptr,
        nullptr,
        instance,
        self);

    if (!hwnd) {
        delete self;
        return false;
    }

    self->hwnd_ = hwnd;
    SetLayeredWindowAttributes(hwnd, 0, self->opacity_, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    return true;
}

bool PinWindow::CreateFromClipboard(HINSTANCE instance) {
    if (!OpenClipboard(nullptr)) {
        return false;
    }

    HBITMAP source = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
    HBITMAP copy = source ? CloneBitmap(source) : nullptr;
    CloseClipboard();

    if (!copy) {
        return false;
    }

    return Create(instance, copy);
}

void PinWindow::ResizeForZoom() {
    RECT current{};
    GetWindowRect(hwnd_, &current);

    const int width = std::clamp(static_cast<int>(bitmapWidth_ * zoom_), 48, 4096);
    const int height = std::clamp(static_cast<int>(bitmapHeight_ * zoom_), 48, 4096);
    const int centerX = (current.left + current.right) / 2;
    const int centerY = (current.top + current.bottom) / 2;

    SetWindowPos(
        hwnd_,
        HWND_TOPMOST,
        centerX - width / 2,
        centerY - height / 2,
        width,
        height,
        SWP_NOACTIVATE);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PinWindow::AdjustZoom(int wheelDelta) {
    zoom_ *= wheelDelta > 0 ? 1.1 : (1.0 / 1.1);
    zoom_ = std::clamp(zoom_, 0.1, 8.0);
    ResizeForZoom();
}

void PinWindow::AdjustOpacity(int wheelDelta) {
    int next = static_cast<int>(opacity_) + (wheelDelta > 0 ? 16 : -16);
    opacity_ = static_cast<BYTE>(std::clamp(next, 48, 255));
    SetLayeredWindowAttributes(hwnd_, 0, opacity_, LWA_ALPHA);
}

void PinWindow::ShowContextMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    AppendMenuW(menu, MF_STRING, CMD_OPACITY_100, L"透明度 100%");
    AppendMenuW(menu, MF_STRING, CMD_OPACITY_80, L"透明度 80%");
    AppendMenuW(menu, MF_STRING, CMD_OPACITY_60, L"透明度 60%");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_CLOSE, L"关闭贴图");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN, cursor.x, cursor.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

LRESULT PinWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd_, &ps);
        RECT client{};
        GetClientRect(hwnd_, &client);

        HDC memory = CreateCompatibleDC(dc);
        const HGDIOBJ old = SelectObject(memory, bitmap_);
        SetStretchBltMode(dc, HALFTONE);
        StretchBlt(
            dc,
            0,
            0,
            client.right,
            client.bottom,
            memory,
            0,
            0,
            bitmapWidth_,
            bitmapHeight_,
            SRCCOPY);
        SelectObject(memory, old);
        DeleteDC(memory);
        EndPaint(hwnd_, &ps);
        return 0;
    }

    case WM_MOUSEWHEEL:
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            AdjustOpacity(GET_WHEEL_DELTA_WPARAM(wParam));
        } else {
            AdjustZoom(GET_WHEEL_DELTA_WPARAM(wParam));
        }
        return 0;

    case WM_LBUTTONDOWN:
        ReleaseCapture();
        SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return 0;

    case WM_LBUTTONDBLCLK:
        DestroyWindow(hwnd_);
        return 0;

    case WM_RBUTTONUP:
        ShowContextMenu();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case CMD_OPACITY_100:
            opacity_ = 255;
            SetLayeredWindowAttributes(hwnd_, 0, opacity_, LWA_ALPHA);
            return 0;
        case CMD_OPACITY_80:
            opacity_ = 204;
            SetLayeredWindowAttributes(hwnd_, 0, opacity_, LWA_ALPHA);
            return 0;
        case CMD_OPACITY_60:
            opacity_ = 153;
            SetLayeredWindowAttributes(hwnd_, 0, opacity_, LWA_ALPHA);
            return 0;
        case CMD_CLOSE:
            DestroyWindow(hwnd_);
            return 0;
        default:
            break;
        }
        break;

    case WM_NCDESTROY:
        hwnd_ = nullptr;
        delete this;
        return 0;
    }

    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

LRESULT CALLBACK PinWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    PinWindow* self = reinterpret_cast<PinWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<PinWindow*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    return self ? self->HandleMessage(message, wParam, lParam)
                : DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace snaplite
