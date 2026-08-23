#include "pin_window.h"

#include "capture.h"

#include <commdlg.h>
#include <gdiplus.h>
#include <objidl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <unordered_map>
#include <vector>

namespace snaplite {
namespace {
constexpr wchar_t kPinClass[] = L"SnapLitePinWindow";
constexpr wchar_t kPinMenuClass[] = L"SnapLitePinMenuWindow";

constexpr UINT WM_PIN_SET_OPACITY = WM_APP + 101;
constexpr UINT WM_PIN_COPY = WM_APP + 102;
constexpr UINT WM_PIN_SAVE_AS = WM_APP + 103;
constexpr UINT WM_PIN_CLOSE = WM_APP + 104;
constexpr UINT WM_PIN_MENU_CLOSED = WM_APP + 105;

constexpr int kMenuWidth = 244;
constexpr int kMenuHeight = 196;
constexpr BYTE kMinOpacity = 26;

DWORD gPendingClipboardSequence = 0;
// Intentionally monotonic: a dismissed pin stays dismissed for the lifetime
// of the app session to prevent re-pinning the same content from clipboard
// updates. See README.
DWORD gDismissedClipboardSequence = 0;
std::unordered_map<HWND, DWORD> gPinClipboardSequences;

struct PinMenuState {
    HWND owner{};
    BYTE opacity{255};
    bool dragging{};
    int hoverRow{-1};
};

RECT SliderRect() {
    return {20, 48, kMenuWidth - 20, 64};
}

RECT MenuRowRect(int row) {
    switch (row) {
    case 0: return {12, 82, kMenuWidth - 12, 112};
    case 1: return {12, 114, kMenuWidth - 12, 144};
    case 2: return {12, 154, kMenuWidth - 12, 184};
    default: return {};
    }
}

int HitMenuRow(POINT point) {
    for (int row = 0; row < 3; ++row) {
        RECT rect = MenuRowRect(row);
        if (PtInRect(&rect, point)) {
            return row;
        }
    }
    return -1;
}

BYTE OpacityFromX(int x) {
    const RECT slider = SliderRect();
    const int left = static_cast<int>(slider.left);
    const int right = static_cast<int>(slider.right);
    const int clamped = std::clamp(x, left, right);
    const double ratio = static_cast<double>(clamped - left) /
                         static_cast<double>(right - left);
    const int value = static_cast<int>(kMinOpacity + ratio * (255 - kMinOpacity));
    return static_cast<BYTE>(std::clamp(value, static_cast<int>(kMinOpacity), 255));
}

int SliderXFromOpacity(BYTE opacity) {
    const RECT slider = SliderRect();
    const double ratio = static_cast<double>(opacity - kMinOpacity) /
                         static_cast<double>(255 - kMinOpacity);
    return static_cast<int>(slider.left) +
           static_cast<int>(ratio * (slider.right - slider.left));
}

void UpdateMenuOpacity(HWND hwnd, PinMenuState* state, int x) {
    if (!state) {
        return;
    }
    state->opacity = OpacityFromX(x);
    if (state->owner && IsWindow(state->owner)) {
        SendMessageW(state->owner, WM_PIN_SET_OPACITY, state->opacity, 0);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

void PaintPinMenu(HWND hwnd, PinMenuState* state) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    if (!state) {
        EndPaint(hwnd, &ps);
        return;
    }

    RECT client{};
    GetClientRect(hwnd, &client);

    HBRUSH background = CreateSolidBrush(RGB(24, 30, 48));
    FillRect(dc, &client, background);
    DeleteObject(background);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(229, 241, 255));

    HFONT font = CreateFontW(
        -15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);

    const int percent = static_cast<int>((static_cast<int>(state->opacity) * 100 + 127) / 255);
    wchar_t opacityText[64]{};
    swprintf_s(opacityText, L"透明度  %d%%", percent);
    RECT label{20, 14, kMenuWidth - 20, 40};
    DrawTextW(dc, opacityText, -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const RECT slider = SliderRect();
    const int centerY = static_cast<int>((slider.top + slider.bottom) / 2);
    const int knobX = SliderXFromOpacity(state->opacity);

    HPEN basePen = CreatePen(PS_SOLID, 4, RGB(68, 81, 111));
    HPEN activePen = CreatePen(PS_SOLID, 4, RGB(111, 231, 255));
    HGDIOBJ oldPen = SelectObject(dc, basePen);
    MoveToEx(dc, slider.left, centerY, nullptr);
    LineTo(dc, slider.right, centerY);
    SelectObject(dc, activePen);
    MoveToEx(dc, slider.left, centerY, nullptr);
    LineTo(dc, knobX, centerY);

    HBRUSH knobBrush = CreateSolidBrush(RGB(202, 247, 255));
    HGDIOBJ oldBrush = SelectObject(dc, knobBrush);
    Ellipse(dc, knobX - 7, centerY - 7, knobX + 7, centerY + 7);
    SelectObject(dc, oldBrush);
    DeleteObject(knobBrush);
    SelectObject(dc, oldPen);
    DeleteObject(basePen);
    DeleteObject(activePen);

    const wchar_t* labels[3] = {L"复制贴图", L"另存为...", L"关闭贴图"};
    for (int row = 0; row < 3; ++row) {
        RECT rect = MenuRowRect(row);
        if (state->hoverRow == row) {
            HBRUSH hover = CreateSolidBrush(row == 2 ? RGB(72, 42, 55) : RGB(39, 55, 83));
            FillRect(dc, &rect, hover);
            DeleteObject(hover);
        }

        SetTextColor(dc, row == 2 ? RGB(255, 166, 190) : RGB(224, 241, 255));
        RECT textRect = rect;
        textRect.left += 10;
        DrawTextW(dc, labels[row], -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    HPEN separator = CreatePen(PS_SOLID, 1, RGB(53, 66, 91));
    oldPen = SelectObject(dc, separator);
    MoveToEx(dc, 16, 149, nullptr);
    LineTo(dc, kMenuWidth - 16, 149);
    SelectObject(dc, oldPen);
    DeleteObject(separator);

    SelectObject(dc, oldFont);
    DeleteObject(font);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK PinMenuWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<PinMenuState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<PinMenuState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
    case WM_CREATE: {
        HRGN region = CreateRoundRectRgn(0, 0, kMenuWidth + 1, kMenuHeight + 1, 18, 18);
        SetWindowRgn(hwnd, region, TRUE);
        return 0;
    }
    case WM_PAINT:
        PaintPinMenu(hwnd, state);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        if (!state) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (state->dragging) {
            UpdateMenuOpacity(hwnd, state, point.x);
            return 0;
        }
        const int hover = HitMenuRow(point);
        if (hover != state->hoverRow) {
            state->hoverRow = hover;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (!state) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RECT slider = SliderRect();
        InflateRect(&slider, 0, 8);
        if (PtInRect(&slider, point)) {
            state->dragging = true;
            SetCapture(hwnd);
            UpdateMenuOpacity(hwnd, state, point.x);
            return 0;
        }

        const int row = HitMenuRow(point);
        if (row >= 0 && state->owner && IsWindow(state->owner)) {
            const UINT command = row == 0 ? WM_PIN_COPY : (row == 1 ? WM_PIN_SAVE_AS : WM_PIN_CLOSE);
            PostMessageW(state->owner, command, 0, 0);
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (state && state->dragging) {
            state->dragging = false;
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (state) {
            state->dragging = false;
        }
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && state && !state->dragging) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_NCDESTROY:
        if (state) {
            if (state->owner && IsWindow(state->owner)) {
                PostMessageW(state->owner, WM_PIN_MENU_CLOSED, reinterpret_cast<WPARAM>(hwnd), 0);
            }
            delete state;
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

HWND CreatePinMenu(HWND owner, BYTE opacity, POINT cursor) {
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(monitor, &monitorInfo);

    int x = cursor.x;
    int y = cursor.y;
    if (x + kMenuWidth > monitorInfo.rcWork.right) {
        x = static_cast<int>(monitorInfo.rcWork.right) - kMenuWidth;
    }
    if (y + kMenuHeight > monitorInfo.rcWork.bottom) {
        y = static_cast<int>(monitorInfo.rcWork.bottom) - kMenuHeight;
    }
    x = std::max(x, static_cast<int>(monitorInfo.rcWork.left));
    y = std::max(y, static_cast<int>(monitorInfo.rcWork.top));

    auto* state = new PinMenuState{};
    state->owner = owner;
    state->opacity = opacity;

    const HINSTANCE hInst =
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kPinMenuClass,
        L"Snap-Lite Pin Menu",
        WS_POPUP,
        x,
        y,
        kMenuWidth,
        kMenuHeight,
        owner,
        nullptr,
        hInst,
        state);

    if (!hwnd) {
        delete state;
        return nullptr;
    }

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    return hwnd;
}

bool OpenClipboardWithRetry() {
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (OpenClipboard(nullptr)) {
            return true;
        }
        Sleep(15);
    }
    return false;
}

HBITMAP BitmapFromPngClipboard() {
    const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
    if (pngFormat == 0) {
        return nullptr;
    }

    HGLOBAL source = static_cast<HGLOBAL>(GetClipboardData(pngFormat));
    if (!source) {
        return nullptr;
    }

    const SIZE_T size = GlobalSize(source);
    if (size == 0) {
        return nullptr;
    }

    const void* sourceBytes = GlobalLock(source);
    if (!sourceBytes) {
        return nullptr;
    }

    HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!copy) {
        GlobalUnlock(source);
        return nullptr;
    }

    void* targetBytes = GlobalLock(copy);
    if (!targetBytes) {
        GlobalFree(copy);
        GlobalUnlock(source);
        return nullptr;
    }

    std::memcpy(targetBytes, sourceBytes, size);
    GlobalUnlock(copy);
    GlobalUnlock(source);

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(copy, TRUE, &stream)) || !stream) {
        GlobalFree(copy);
        return nullptr;
    }

    HBITMAP bitmap = nullptr;
    {
        Gdiplus::Bitmap image(stream);
        if (image.GetLastStatus() == Gdiplus::Ok) {
            image.GetHBITMAP(Gdiplus::Color(255, 255, 255, 255), &bitmap);
        }
    }
    stream->Release();
    return bitmap;
}

HBITMAP BitmapFromDibClipboard(UINT format) {
    HGLOBAL memory = static_cast<HGLOBAL>(GetClipboardData(format));
    if (!memory) {
        return nullptr;
    }

    const SIZE_T totalBytes = GlobalSize(memory);
    const auto* base = static_cast<const BYTE*>(GlobalLock(memory));
    if (!base || totalBytes < sizeof(BITMAPINFOHEADER)) {
        if (base) GlobalUnlock(memory);
        return nullptr;
    }

    const auto* header = reinterpret_cast<const BITMAPINFOHEADER*>(base);
    if (header->biSize < sizeof(BITMAPINFOHEADER) ||
        static_cast<SIZE_T>(header->biSize) > totalBytes ||
        header->biPlanes != 1 ||
        header->biWidth <= 0 ||
        header->biHeight == 0 ||
        header->biHeight == std::numeric_limits<LONG>::min()) {
        GlobalUnlock(memory);
        return nullptr;
    }

    const WORD bitCount = header->biBitCount;
    const bool supportedBitCount =
        bitCount == 1 || bitCount == 4 || bitCount == 8 ||
        bitCount == 16 || bitCount == 24 || bitCount == 32;
    const bool supportedCompression =
        header->biCompression == BI_RGB || header->biCompression == BI_BITFIELDS
#ifdef BI_ALPHABITFIELDS
        || header->biCompression == BI_ALPHABITFIELDS
#endif
        ;
    if (!supportedBitCount || !supportedCompression) {
        GlobalUnlock(memory);
        return nullptr;
    }

    const std::uint64_t width64 = static_cast<std::uint64_t>(header->biWidth);
    const std::uint64_t height64 = static_cast<std::uint64_t>(
        header->biHeight < 0 ? -static_cast<std::int64_t>(header->biHeight)
                             : static_cast<std::int64_t>(header->biHeight));
    constexpr std::uint64_t kMaxClipboardDimension = 32768;
    if (width64 == 0 || height64 == 0 ||
        width64 > kMaxClipboardDimension || height64 > kMaxClipboardDimension) {
        GlobalUnlock(memory);
        return nullptr;
    }

    SIZE_T pixelOffset = header->biSize;
    if (header->biSize == sizeof(BITMAPINFOHEADER)) {
        if (header->biCompression == BI_BITFIELDS) {
            pixelOffset += 3 * sizeof(DWORD);
        }
#ifdef BI_ALPHABITFIELDS
        else if (header->biCompression == BI_ALPHABITFIELDS) {
            pixelOffset += 4 * sizeof(DWORD);
        }
#endif
    }

    if (header->biBitCount <= 8) {
        const DWORD colorCount = header->biClrUsed != 0
            ? header->biClrUsed
            : (1u << header->biBitCount);
        pixelOffset += static_cast<SIZE_T>(colorCount) * sizeof(RGBQUAD);
    }

    if (pixelOffset >= totalBytes) {
        GlobalUnlock(memory);
        return nullptr;
    }

    // For uncompressed/bitfield DIBs, every scanline is DWORD aligned. Validate
    // that the advertised dimensions actually fit inside the clipboard block
    // before StretchDIBits sees the foreign buffer.
    const std::uint64_t bitsPerRow = width64 * bitCount;
    const std::uint64_t stride64 = ((bitsPerRow + 31u) / 32u) * 4u;
    if (stride64 == 0 || stride64 > totalBytes - pixelOffset ||
        height64 > (totalBytes - pixelOffset) / stride64) {
        GlobalUnlock(memory);
        return nullptr;
    }

    const int width = static_cast<int>(width64);
    const int height = static_cast<int>(height64);

    const void* pixels = base + pixelOffset;
    const auto* info = reinterpret_cast<const BITMAPINFO*>(base);

    HDC screenDc = GetDC(nullptr);
    HDC memoryDc = screenDc ? CreateCompatibleDC(screenDc) : nullptr;
    HBITMAP bitmap = screenDc ? CreateCompatibleBitmap(screenDc, width, height) : nullptr;

    bool ok = false;
    if (memoryDc && bitmap) {
        const HGDIOBJ old = SelectObject(memoryDc, bitmap);
        const int result = StretchDIBits(
            memoryDc,
            0,
            0,
            width,
            height,
            0,
            0,
            width,
            height,
            pixels,
            info,
            DIB_RGB_COLORS,
            SRCCOPY);
        SelectObject(memoryDc, old);
        ok = result != GDI_ERROR && result != 0;
    }

    if (memoryDc) DeleteDC(memoryDc);
    if (screenDc) ReleaseDC(nullptr, screenDc);
    GlobalUnlock(memory);

    if (!ok) {
        if (bitmap) DeleteObject(bitmap);
        return nullptr;
    }
    return bitmap;
}

HBITMAP BitmapFromFileDropClipboard() {
    HDROP drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
    if (!drop) {
        return nullptr;
    }

    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    if (count == 0) {
        return nullptr;
    }

    const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
    if (length == 0) {
        return nullptr;
    }

    std::vector<wchar_t> path(length + 1, L'\0');
    if (DragQueryFileW(drop, 0, path.data(), static_cast<UINT>(path.size())) == 0) {
        return nullptr;
    }

    const std::filesystem::path file(path.data());
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error) || error) {
        return nullptr;
    }

    Gdiplus::Bitmap image(file.c_str());
    if (image.GetLastStatus() != Gdiplus::Ok) {
        return nullptr;
    }

    HBITMAP bitmap = nullptr;
    image.GetHBITMAP(Gdiplus::Color(255, 255, 255, 255), &bitmap);
    return bitmap;
}

HBITMAP BitmapFromClipboard() {
    HBITMAP bitmap = nullptr;

    if (HBITMAP source = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP))) {
        bitmap = CloneBitmap(source);
    }
    if (!bitmap) {
        bitmap = BitmapFromPngClipboard();
    }
    if (!bitmap && IsClipboardFormatAvailable(CF_DIBV5)) {
        bitmap = BitmapFromDibClipboard(CF_DIBV5);
    }
    if (!bitmap && IsClipboardFormatAvailable(CF_DIB)) {
        bitmap = BitmapFromDibClipboard(CF_DIB);
    }
    if (!bitmap && IsClipboardFormatAvailable(CF_HDROP)) {
        bitmap = BitmapFromFileDropClipboard();
    }

    return bitmap;
}

}  // namespace

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
    if (contextMenu_ && IsWindow(contextMenu_)) {
        DestroyWindow(contextMenu_);
        contextMenu_ = nullptr;
    }
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
    const bool pinRegistered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;

    WNDCLASSEXW menuClass{};
    menuClass.cbSize = sizeof(menuClass);
    menuClass.lpfnWndProc = PinMenuWindowProc;
    menuClass.hInstance = instance;
    menuClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    menuClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    menuClass.lpszClassName = kPinMenuClass;
    const bool menuRegistered =
        RegisterClassExW(&menuClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;

    return pinRegistered && menuRegistered;
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

    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(monitor, &monitorInfo);
    const int workWidth = std::max(1L, monitorInfo.rcWork.right - monitorInfo.rcWork.left);
    const int workHeight = std::max(1L, monitorInfo.rcWork.bottom - monitorInfo.rcWork.top);
    const int maxWidth = static_cast<int>(workWidth * 0.65);
    const int maxHeight = static_cast<int>(workHeight * 0.65);
    self->zoom_ = std::min({1.0,
        static_cast<double>(maxWidth) / self->bitmapWidth_,
        static_cast<double>(maxHeight) / self->bitmapHeight_});

    const int width = std::max(1, static_cast<int>(self->bitmapWidth_ * self->zoom_));
    const int height = std::max(1, static_cast<int>(self->bitmapHeight_ * self->zoom_));
    int x = cursor.x + 16;
    int y = cursor.y + 16;
    x = std::clamp(x, static_cast<int>(monitorInfo.rcWork.left),
                   std::max(static_cast<int>(monitorInfo.rcWork.left), static_cast<int>(monitorInfo.rcWork.right) - width));
    y = std::clamp(y, static_cast<int>(monitorInfo.rcWork.top),
                   std::max(static_cast<int>(monitorInfo.rcWork.top), static_cast<int>(monitorInfo.rcWork.bottom) - height));

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        kPinClass,
        L"Snap-Lite Pin",
        WS_POPUP,
        x,
        y,
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
    if (gPendingClipboardSequence != 0) {
        gPinClipboardSequences[hwnd] = gPendingClipboardSequence;
    }
    SetLayeredWindowAttributes(hwnd, 0, self->opacity_, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    return true;
}

bool PinWindow::CreateFromClipboard(HINSTANCE instance) {
    const DWORD clipboardSequence = GetClipboardSequenceNumber();
    if (clipboardSequence != 0 && clipboardSequence == gDismissedClipboardSequence) {
        return true;
    }

    if (!OpenClipboardWithRetry()) {
        return false;
    }

    HBITMAP bitmap = BitmapFromClipboard();
    CloseClipboard();

    if (!bitmap) {
        return false;
    }

    gPendingClipboardSequence = clipboardSequence;
    const bool created = Create(instance, bitmap);
    gPendingClipboardSequence = 0;
    return created;
}

void PinWindow::ResizeForZoom() {
    RECT current{};
    GetWindowRect(hwnd_, &current);
    const POINT center{
        static_cast<LONG>((current.left + current.right) / 2),
        static_cast<LONG>((current.top + current.bottom) / 2)};

    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    const HMONITOR monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(monitor, &monitorInfo);
    const int workWidth = std::max(1L, monitorInfo.rcWork.right - monitorInfo.rcWork.left);
    const int workHeight = std::max(1L, monitorInfo.rcWork.bottom - monitorInfo.rcWork.top);

    // Clamp the zoom itself, not width and height independently, so very wide
    // or very tall images never get distorted.
    const double monitorMaxZoom = std::min(
        static_cast<double>(workWidth) / bitmapWidth_,
        static_cast<double>(workHeight) / bitmapHeight_);
    const double hardMaxZoom = std::min(
        4096.0 / bitmapWidth_,
        4096.0 / bitmapHeight_);
    const double maxZoom = std::max(0.01, std::min(8.0, std::min(monitorMaxZoom, hardMaxZoom)));
    zoom_ = std::clamp(zoom_, 0.01, maxZoom);

    const int width = std::max(1, static_cast<int>(bitmapWidth_ * zoom_));
    const int height = std::max(1, static_cast<int>(bitmapHeight_ * zoom_));
    int x = center.x - width / 2;
    int y = center.y - height / 2;
    x = std::clamp(x, static_cast<int>(monitorInfo.rcWork.left),
                   std::max(static_cast<int>(monitorInfo.rcWork.left), static_cast<int>(monitorInfo.rcWork.right) - width));
    y = std::clamp(y, static_cast<int>(monitorInfo.rcWork.top),
                   std::max(static_cast<int>(monitorInfo.rcWork.top), static_cast<int>(monitorInfo.rcWork.bottom) - height));

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PinWindow::AdjustZoom(int wheelDelta) {
    zoom_ *= wheelDelta > 0 ? 1.1 : (1.0 / 1.1);
    zoom_ = std::clamp(zoom_, 0.1, 8.0);
    ResizeForZoom();
}

void PinWindow::SetOpacity(BYTE opacity) {
    opacity_ = static_cast<BYTE>(std::clamp(static_cast<int>(opacity), static_cast<int>(kMinOpacity), 255));
    if (hwnd_) {
        SetLayeredWindowAttributes(hwnd_, 0, opacity_, LWA_ALPHA);
    }
}

void PinWindow::AdjustOpacity(int wheelDelta) {
    const int next = static_cast<int>(opacity_) + (wheelDelta > 0 ? 13 : -13);
    SetOpacity(static_cast<BYTE>(std::clamp(next, static_cast<int>(kMinOpacity), 255)));
}

void PinWindow::ShowContextMenu() {
    if (contextMenu_ && IsWindow(contextMenu_)) {
        DestroyWindow(contextMenu_);
        contextMenu_ = nullptr;
    }

    POINT cursor{};
    GetCursorPos(&cursor);
    contextMenu_ = CreatePinMenu(hwnd_, opacity_, cursor);
}

void PinWindow::CopyPin() {
    if (!CopyBitmapToClipboard(hwnd_, bitmap_)) {
        MessageBoxW(hwnd_, L"复制贴图失败。", L"Snap-Lite", MB_OK | MB_ICONERROR);
    }
}

void PinWindow::SavePinAs() {
    SYSTEMTIME time{};
    GetLocalTime(&time);

    wchar_t fileName[128]{};
    swprintf_s(
        fileName,
        L"SnapLite-pin-%04d%02d%02d-%02d%02d%02d.png",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond);

    std::vector<wchar_t> pathBuffer(32768, L'\0');
    wcsncpy_s(pathBuffer.data(), pathBuffer.size(), fileName, _TRUNCATE);

    static constexpr wchar_t filter[] =
        L"PNG 图片 (*.png)\0*.png\0所有文件 (*.*)\0*.*\0\0";

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = pathBuffer.data();
    dialog.nMaxFile = static_cast<DWORD>(pathBuffer.size());
    dialog.lpstrDefExt = L"png";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR
#if defined(OFN_ALLOW_LONG_PATHS)
                   | OFN_ALLOW_LONG_PATHS
#endif
        ;

    if (!GetSaveFileNameW(&dialog)) {
        return;
    }

    if (!SaveBitmapPng(bitmap_, std::filesystem::path(pathBuffer.data()))) {
        MessageBoxW(hwnd_, L"保存贴图失败。", L"Snap-Lite", MB_OK | MB_ICONERROR);
    }
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
        if (contextMenu_ && IsWindow(contextMenu_)) {
            DestroyWindow(contextMenu_);
            contextMenu_ = nullptr;
        }
        ReleaseCapture();
        SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return 0;

    case WM_LBUTTONDBLCLK:
        DestroyWindow(hwnd_);
        return 0;

    case WM_RBUTTONUP:
        ShowContextMenu();
        return 0;

    case WM_PIN_SET_OPACITY:
        SetOpacity(static_cast<BYTE>(std::clamp(static_cast<int>(wParam), 0, 255)));
        return 0;

    case WM_PIN_COPY:
        CopyPin();
        return 0;

    case WM_PIN_SAVE_AS:
        SavePinAs();
        return 0;

    case WM_PIN_CLOSE:
        DestroyWindow(hwnd_);
        return 0;

    case WM_PIN_MENU_CLOSED:
        contextMenu_ = nullptr;
        return 0;

    case WM_NCDESTROY: {
        if (contextMenu_ && IsWindow(contextMenu_)) {
            DestroyWindow(contextMenu_);
            contextMenu_ = nullptr;
        }
        const auto it = gPinClipboardSequences.find(hwnd_);
        if (it != gPinClipboardSequences.end()) {
            gDismissedClipboardSequence = it->second;
            gPinClipboardSequences.erase(it);
        }
        hwnd_ = nullptr;
        delete this;
        return 0;
    }
    default:
        break;
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
