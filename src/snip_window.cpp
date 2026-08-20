#include "snip_window.h"

#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <string>

namespace snaplite {
namespace {
constexpr wchar_t kSnipClass[] = L"SnapLiteSnipWindow";

RECT NormalizeRect(POINT a, POINT b) {
    return {
        std::min(a.x, b.x),
        std::min(a.y, b.y),
        std::max(a.x, b.x),
        std::max(a.y, b.y),
    };
}

bool IsSnapLiteWindow(HWND hwnd) {
    wchar_t className[128]{};
    GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    return wcsncmp(className, L"SnapLite", 8) == 0;
}

struct FindWindowContext {
    POINT point{};
    HWND exclude{};
    HWND result{};
};

BOOL CALLBACK FindWindowProc(HWND hwnd, LPARAM data) {
    auto* context = reinterpret_cast<FindWindowContext*>(data);
    if (hwnd == context->exclude || !IsWindowVisible(hwnd) || IsIconic(hwnd) || IsSnapLiteWindow(hwnd)) {
        return TRUE;
    }

    RECT rect{};
    if (!GetWindowRect(hwnd, &rect) || !PtInRect(&rect, context->point)) {
        return TRUE;
    }

    context->result = hwnd;
    return FALSE;
}

HWND FindElementAt(POINT screenPoint, HWND exclude) {
    FindWindowContext context{screenPoint, exclude, nullptr};
    EnumWindows(FindWindowProc, reinterpret_cast<LPARAM>(&context));
    if (!context.result) {
        return nullptr;
    }

    HWND current = context.result;
    for (;;) {
        POINT local = screenPoint;
        if (!ScreenToClient(current, &local)) {
            break;
        }

        HWND child = ChildWindowFromPointEx(
            current,
            local,
            CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
        if (!child || child == current || IsSnapLiteWindow(child)) {
            break;
        }
        current = child;
    }
    return current;
}

}  // namespace

SnipWindow::SnipWindow(HINSTANCE instance, HWND owner, CaptureCallback callback)
    : instance_(instance), owner_(owner), callback_(std::move(callback)), screen_(GetVirtualScreen()) {}

SnipWindow::~SnipWindow() {
    if (capture_) {
        DeleteObject(capture_);
        capture_ = nullptr;
    }
}

bool SnipWindow::Register(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kSnipClass;
    wc.style = CS_DBLCLKS;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool SnipWindow::Start(HINSTANCE instance, HWND owner, CaptureCallback callback) {
    auto* self = new SnipWindow(instance, owner, std::move(callback));
    self->capture_ = CaptureVirtualScreen();
    if (!self->capture_) {
        delete self;
        return false;
    }

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kSnipClass,
        L"Snap-Lite Snip",
        WS_POPUP,
        self->screen_.x,
        self->screen_.y,
        self->screen_.width,
        self->screen_.height,
        nullptr,
        nullptr,
        instance,
        self);

    if (!hwnd) {
        delete self;
        return false;
    }

    self->hwnd_ = hwnd;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    self->UpdateHover();
    return true;
}

RECT SnipWindow::CurrentFocusRect() const {
    if (dragging_) {
        return NormalizeRect(dragStart_, dragCurrent_);
    }
    return hasHover_ ? hoverRect_ : RECT{};
}

void SnipWindow::UpdateHover() {
    if (dragging_) {
        return;
    }

    POINT screenPoint{};
    GetCursorPos(&screenPoint);
    HWND element = FindElementAt(screenPoint, hwnd_);
    hasHover_ = false;

    if (element) {
        RECT rect{};
        if (GetWindowRect(element, &rect)) {
            OffsetRect(&rect, -screen_.x, -screen_.y);
            RECT bounds{0, 0, screen_.width, screen_.height};
            RECT clipped{};
            if (IntersectRect(&clipped, &rect, &bounds)) {
                hoverRect_ = clipped;
                hasHover_ = true;
            }
        }
    }

    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SnipWindow::PaintMagnifier(HDC dc, POINT clientPoint) {
    constexpr int sampleRadius = 5;
    constexpr int sampleSize = sampleRadius * 2 + 1;
    constexpr int zoom = 9;
    constexpr int lensSize = sampleSize * zoom;
    constexpr int panelHeight = lensSize + 28;

    const int clientX = static_cast<int>(clientPoint.x);
    const int clientY = static_cast<int>(clientPoint.y);
    int panelX = clientX + 24;
    int panelY = clientY + 24;
    if (panelX + lensSize + 4 > screen_.width) {
        panelX = clientX - lensSize - 28;
    }
    if (panelY + panelHeight + 4 > screen_.height) {
        panelY = clientY - panelHeight - 28;
    }
    panelX = std::max(4, panelX);
    panelY = std::max(4, panelY);

    HDC memory = CreateCompatibleDC(dc);
    const HGDIOBJ old = SelectObject(memory, capture_);
    SetStretchBltMode(dc, COLORONCOLOR);

    const int sourceX = std::clamp(clientX - sampleRadius, 0, std::max(0, screen_.width - sampleSize));
    const int sourceY = std::clamp(clientY - sampleRadius, 0, std::max(0, screen_.height - sampleSize));

    StretchBlt(
        dc,
        panelX,
        panelY,
        lensSize,
        lensSize,
        memory,
        sourceX,
        sourceY,
        sampleSize,
        sampleSize,
        SRCCOPY);

    HPEN border = CreatePen(PS_SOLID, 2, RGB(20, 20, 20));
    const HGDIOBJ oldPen = SelectObject(dc, border);
    const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, panelX, panelY, panelX + lensSize, panelY + lensSize);

    HPEN centerPen = CreatePen(PS_SOLID, 2, RGB(255, 70, 70));
    SelectObject(dc, centerPen);
    const int center = sampleRadius * zoom;
    Rectangle(dc, panelX + center, panelY + center, panelX + center + zoom + 1, panelY + center + zoom + 1);

    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(centerPen);
    DeleteObject(border);

    const COLORREF color = GetPixel(memory, clientX, clientY);
    SelectObject(memory, old);
    DeleteDC(memory);

    wchar_t text[64]{};
    if (color != CLR_INVALID) {
        swprintf_s(text, L"#%02X%02X%02X   C 复制颜色", GetRValue(color), GetGValue(color), GetBValue(color));
    } else {
        wcscpy_s(text, L"C 复制颜色");
    }

    RECT label{panelX, panelY + lensSize, panelX + lensSize + 120, panelY + panelHeight};
    SetBkColor(dc, RGB(28, 28, 28));
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, OPAQUE);
    DrawTextW(dc, text, -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void SnipWindow::Paint() {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd_, &ps);

    HDC memory = CreateCompatibleDC(dc);
    const HGDIOBJ old = SelectObject(memory, capture_);
    BitBlt(dc, 0, 0, screen_.width, screen_.height, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old);
    DeleteDC(memory);

    const RECT focus = CurrentFocusRect();
    if (focus.right > focus.left && focus.bottom > focus.top) {
        HPEN shadow = CreatePen(PS_SOLID, 4, RGB(0, 0, 0));
        HGDIOBJ oldPen = SelectObject(dc, shadow);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, focus.left, focus.top, focus.right, focus.bottom);
        SelectObject(dc, oldPen);
        DeleteObject(shadow);

        HPEN accent = CreatePen(PS_SOLID, 2, RGB(0, 174, 255));
        oldPen = SelectObject(dc, accent);
        Rectangle(dc, focus.left, focus.top, focus.right, focus.bottom);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(accent);

        wchar_t dimensions[48]{};
        swprintf_s(dimensions, L"%ld × %ld", focus.right - focus.left, focus.bottom - focus.top);
        RECT sizeRect{focus.left, std::max(0L, focus.top - 26), focus.left + 180, focus.top};
        SetBkColor(dc, RGB(28, 28, 28));
        SetTextColor(dc, RGB(255, 255, 255));
        SetBkMode(dc, OPAQUE);
        DrawTextW(dc, dimensions, -1, &sizeRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    RECT help{12, 12, 680, 42};
    SetBkColor(dc, RGB(28, 28, 28));
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, OPAQUE);
    DrawTextW(dc, L"单击自动选中窗口/控件 · 拖动自由选择 · C 取色 · 方向键微调 · Esc 取消", -1, &help,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    POINT cursor{};
    GetCursorPos(&cursor);
    POINT client{cursor.x - screen_.x, cursor.y - screen_.y};
    PaintMagnifier(dc, client);

    EndPaint(hwnd_, &ps);
}

void SnipWindow::CopyCurrentColor() {
    POINT cursor{};
    GetCursorPos(&cursor);
    const int x = cursor.x - screen_.x;
    const int y = cursor.y - screen_.y;
    const COLORREF color = ReadBitmapPixel(capture_, x, y);
    if (color == CLR_INVALID) {
        return;
    }

    wchar_t text[16]{};
    swprintf_s(text, L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
    CopyTextToClipboard(owner_, text);
}

void SnipWindow::FinishSelection(const RECT& rawSelection) {
    RECT bounds{0, 0, screen_.width, screen_.height};
    RECT selection{};
    if (!IntersectRect(&selection, &rawSelection, &bounds)) {
        return;
    }

    if (selection.right - selection.left < 2 || selection.bottom - selection.top < 2) {
        return;
    }

    HBITMAP result = CloneBitmapRegion(capture_, selection);
    if (!result) {
        return;
    }

    auto callback = callback_;
    DestroyWindow(hwnd_);
    if (callback) {
        callback(result);
    } else {
        DeleteObject(result);
    }
}

LRESULT SnipWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;

    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        return TRUE;

    case WM_PAINT:
        Paint();
        return 0;

    case WM_MOUSEMOVE:
        if (dragging_) {
            dragCurrent_ = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else {
            UpdateHover();
        }
        return 0;

    case WM_LBUTTONDOWN:
        dragging_ = true;
        dragStart_ = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        dragCurrent_ = dragStart_;
        pressedHoverRect_ = hasHover_ ? hoverRect_ : RECT{};
        SetCapture(hwnd_);
        return 0;

    case WM_LBUTTONUP: {
        if (!dragging_) {
            return 0;
        }
        dragging_ = false;
        dragCurrent_ = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ReleaseCapture();

        RECT selection = NormalizeRect(dragStart_, dragCurrent_);
        if (selection.right - selection.left < 4 && selection.bottom - selection.top < 4 &&
            pressedHoverRect_.right > pressedHoverRect_.left && pressedHoverRect_.bottom > pressedHoverRect_.top) {
            selection = pressedHoverRect_;
        }
        FinishSelection(selection);
        return 0;
    }

    case WM_RBUTTONDOWN:
        DestroyWindow(hwnd_);
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            DestroyWindow(hwnd_);
            return 0;
        }
        if (wParam == 'C') {
            CopyCurrentColor();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_UP || wParam == VK_DOWN) {
            POINT cursor{};
            GetCursorPos(&cursor);
            const int step = (GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1;
            if (wParam == VK_LEFT) cursor.x -= step;
            if (wParam == VK_RIGHT) cursor.x += step;
            if (wParam == VK_UP) cursor.y -= step;
            if (wParam == VK_DOWN) cursor.y += step;
            SetCursorPos(cursor.x, cursor.y);
            UpdateHover();
            return 0;
        }
        break;

    case WM_NCDESTROY:
        hwnd_ = nullptr;
        delete this;
        return 0;
    }

    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

LRESULT CALLBACK SnipWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    SnipWindow* self = reinterpret_cast<SnipWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<SnipWindow*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    return self ? self->HandleMessage(message, wParam, lParam)
                : DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace snaplite
