#include "snip_window.h"

#include <windowsx.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <string>
#include <utility>

namespace snaplite {
namespace {
constexpr wchar_t kSnipClass[] = L"SnapLiteSnipWindow";
constexpr COLORREF kAccent = RGB(35, 132, 255);
constexpr COLORREF kToolbarText = RGB(35, 35, 35);
constexpr int kHandleRadius = 5;
constexpr int kHandleHitRadius = 9;
constexpr int kToolbarButton = 38;
constexpr int kToolbarHeight = 34;
constexpr int kToolbarButtons = 11;
constexpr int kToolbarWidth = kToolbarButton * kToolbarButtons;
constexpr int kMinSelection = 8;

RECT NormalizeRect(POINT a, POINT b) {
    return {
        std::min(a.x, b.x),
        std::min(a.y, b.y),
        std::max(a.x, b.x),
        std::max(a.y, b.y),
    };
}

POINT ClampPoint(POINT point, const RECT& rect) {
    point.x = std::clamp(point.x, rect.left, std::max(rect.left, rect.right - 1));
    point.y = std::clamp(point.y, rect.top, std::max(rect.top, rect.bottom - 1));
    return point;
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

bool Near(POINT point, POINT target, int radius) {
    return std::abs(static_cast<int>(point.x - target.x)) <= radius &&
           std::abs(static_cast<int>(point.y - target.y)) <= radius;
}

void DeleteBitmaps(std::vector<HBITMAP>& bitmaps) {
    for (HBITMAP bitmap : bitmaps) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
    }
    bitmaps.clear();
}

}  // namespace

SnipWindow::SnipWindow(HINSTANCE instance, HWND owner, CaptureCallback callback)
    : instance_(instance), owner_(owner), callback_(std::move(callback)), screen_(GetVirtualScreen()) {}

SnipWindow::~SnipWindow() {
    if (textEdit_) {
        DestroyWindow(textEdit_);
        textEdit_ = nullptr;
    }
    if (textFont_) {
        DeleteObject(textFont_);
        textFont_ = nullptr;
    }
    DeleteBitmaps(undo_);
    DeleteBitmaps(redo_);
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

RECT SnipWindow::NormalizedSelection() const {
    return {
        std::min(selection_.left, selection_.right),
        std::min(selection_.top, selection_.bottom),
        std::max(selection_.left, selection_.right),
        std::max(selection_.top, selection_.bottom),
    };
}

void SnipWindow::ClampSelection() {
    selection_ = NormalizedSelection();
    const LONG width = selection_.right - selection_.left;
    const LONG height = selection_.bottom - selection_.top;

    if (selection_.left < 0) {
        selection_.left = 0;
        selection_.right = std::min<LONG>(screen_.width, width);
    }
    if (selection_.top < 0) {
        selection_.top = 0;
        selection_.bottom = std::min<LONG>(screen_.height, height);
    }
    if (selection_.right > screen_.width) {
        selection_.right = screen_.width;
        selection_.left = std::max<LONG>(0, selection_.right - width);
    }
    if (selection_.bottom > screen_.height) {
        selection_.bottom = screen_.height;
        selection_.top = std::max<LONG>(0, selection_.bottom - height);
    }
}

void SnipWindow::UpdateHover() {
    if (selected_ || dragging_ || drawing_) {
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

RECT SnipWindow::ToolbarRect() const {
    if (!selected_) {
        return {};
    }

    const RECT selection = NormalizedSelection();
    const int maxX = std::max(8, screen_.width - kToolbarWidth - 8);
    const int x = std::clamp(static_cast<int>(selection.left), 8, maxX);
    int y = static_cast<int>(selection.bottom) + 10;
    if (y + kToolbarHeight > screen_.height - 8) {
        y = static_cast<int>(selection.top) - kToolbarHeight - 10;
    }
    y = std::clamp(y, 8, std::max(8, screen_.height - kToolbarHeight - 8));
    return {x, y, x + kToolbarWidth, y + kToolbarHeight};
}

SnipWindow::DragMode SnipWindow::HitSelection(POINT point) const {
    if (!selected_ || edited_) {
        return DragMode::None;
    }

    const RECT r = NormalizedSelection();
    const LONG cx = (r.left + r.right) / 2;
    const LONG cy = (r.top + r.bottom) / 2;
    const POINT handles[8] = {
        {r.left, r.top}, {cx, r.top}, {r.right, r.top}, {r.right, cy},
        {r.right, r.bottom}, {cx, r.bottom}, {r.left, r.bottom}, {r.left, cy},
    };
    const DragMode modes[8] = {
        DragMode::ResizeTopLeft,
        DragMode::ResizeTop,
        DragMode::ResizeTopRight,
        DragMode::ResizeRight,
        DragMode::ResizeBottomRight,
        DragMode::ResizeBottom,
        DragMode::ResizeBottomLeft,
        DragMode::ResizeLeft,
    };

    for (int i = 0; i < 8; ++i) {
        if (Near(point, handles[i], kHandleHitRadius)) {
            return modes[i];
        }
    }
    return PtInRect(&r, point) ? DragMode::Move : DragMode::None;
}

int SnipWindow::HitToolbar(POINT point) const {
    if (!selected_) {
        return -1;
    }
    const RECT bar = ToolbarRect();
    if (!PtInRect(&bar, point)) {
        return -1;
    }
    const int index = static_cast<int>((point.x - bar.left) / kToolbarButton);
    return std::clamp(index, 0, kToolbarButtons - 1);
}

void SnipWindow::SetCursorForPoint(POINT point) {
    if (HitToolbar(point) >= 0) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return;
    }

    switch (HitSelection(point)) {
    case DragMode::ResizeTopLeft:
    case DragMode::ResizeBottomRight:
        SetCursor(LoadCursorW(nullptr, IDC_SIZENWSE));
        return;
    case DragMode::ResizeTopRight:
    case DragMode::ResizeBottomLeft:
        SetCursor(LoadCursorW(nullptr, IDC_SIZENESW));
        return;
    case DragMode::ResizeTop:
    case DragMode::ResizeBottom:
        SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
        return;
    case DragMode::ResizeLeft:
    case DragMode::ResizeRight:
        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        return;
    case DragMode::Move:
        SetCursor(LoadCursorW(nullptr, tool_ == Tool::None ? IDC_SIZEALL : IDC_CROSS));
        return;
    default:
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        return;
    }
}

void SnipWindow::PaintMagnifier(HDC dc, POINT clientPoint) {
    constexpr int sampleRadius = 5;
    constexpr int sampleSize = sampleRadius * 2 + 1;
    constexpr int zoom = 9;
    constexpr int lensSize = sampleSize * zoom;
    constexpr int panelHeight = lensSize + 26;

    const int clientX = static_cast<int>(clientPoint.x);
    const int clientY = static_cast<int>(clientPoint.y);
    int panelX = clientX + 24;
    int panelY = clientY + 24;
    if (panelX + lensSize + 4 > screen_.width) panelX = clientX - lensSize - 28;
    if (panelY + panelHeight + 4 > screen_.height) panelY = clientY - panelHeight - 28;
    panelX = std::max(4, panelX);
    panelY = std::max(4, panelY);

    HDC memory = CreateCompatibleDC(dc);
    const HGDIOBJ old = SelectObject(memory, capture_);
    SetStretchBltMode(dc, COLORONCOLOR);

    const int sourceX = std::clamp(clientX - sampleRadius, 0, std::max(0, screen_.width - sampleSize));
    const int sourceY = std::clamp(clientY - sampleRadius, 0, std::max(0, screen_.height - sampleSize));
    StretchBlt(dc, panelX, panelY, lensSize, lensSize, memory, sourceX, sourceY, sampleSize, sampleSize, SRCCOPY);

    HPEN border = CreatePen(PS_SOLID, 1, RGB(20, 20, 20));
    const HGDIOBJ oldPen = SelectObject(dc, border);
    const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, panelX, panelY, panelX + lensSize, panelY + lensSize);

    HPEN centerPen = CreatePen(PS_SOLID, 2, RGB(255, 70, 70));
    SelectObject(dc, centerPen);
    const int center = sampleRadius * zoom;
    Rectangle(dc, panelX + center, panelY + center, panelX + center + zoom + 1, panelY + center + zoom + 1);

    const COLORREF color = GetPixel(memory, clientX, clientY);
    SelectObject(memory, old);
    DeleteDC(memory);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(centerPen);
    DeleteObject(border);

    wchar_t text[64]{};
    if (color != CLR_INVALID) {
        swprintf_s(text, L"#%02X%02X%02X   C 复制", GetRValue(color), GetGValue(color), GetBValue(color));
    } else {
        wcscpy_s(text, L"C 复制颜色");
    }

    RECT label{panelX, panelY + lensSize, panelX + lensSize + 80, panelY + panelHeight};
    SetBkColor(dc, RGB(35, 35, 35));
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, OPAQUE);
    DrawTextW(dc, text, -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void SnipWindow::PaintSelection(HDC dc) {
    if (!selected_) {
        return;
    }

    const RECT r = NormalizedSelection();
    Gdiplus::Graphics graphics(dc);
    Gdiplus::SolidBrush dimBrush(Gdiplus::Color(105, 0, 0, 0));
    const auto fillDim = [&](LONG x, LONG y, LONG width, LONG height) {
        if (width > 0 && height > 0) {
            graphics.FillRectangle(
                &dimBrush,
                static_cast<INT>(x),
                static_cast<INT>(y),
                static_cast<INT>(width),
                static_cast<INT>(height));
        }
    };

    fillDim(0, 0, screen_.width, r.top);
    fillDim(0, r.bottom, screen_.width, screen_.height - r.bottom);
    fillDim(0, r.top, r.left, r.bottom - r.top);
    fillDim(r.right, r.top, screen_.width - r.right, r.bottom - r.top);

    HPEN border = CreatePen(PS_SOLID, 2, kAccent);
    const HGDIOBJ oldPen = SelectObject(dc, border);
    const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);

    HBRUSH handleBrush = CreateSolidBrush(RGB(255, 255, 255));
    SelectObject(dc, handleBrush);
    const LONG cx = (r.left + r.right) / 2;
    const LONG cy = (r.top + r.bottom) / 2;
    const POINT handles[8] = {
        {r.left, r.top}, {cx, r.top}, {r.right, r.top}, {r.right, cy},
        {r.right, r.bottom}, {cx, r.bottom}, {r.left, r.bottom}, {r.left, cy},
    };
    for (const POINT handle : handles) {
        Ellipse(
            dc,
            handle.x - kHandleRadius,
            handle.y - kHandleRadius,
            handle.x + kHandleRadius + 1,
            handle.y + kHandleRadius + 1);
    }

    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(handleBrush);
    DeleteObject(border);

    wchar_t sizeText[64]{};
    swprintf_s(sizeText, L"%ld × %ld  px", r.right - r.left, r.bottom - r.top);
    RECT label{r.left + 2, r.top - 34, r.left + 126, r.top - 7};
    if (label.top < 4) {
        label.top = r.top + 8;
        label.bottom = label.top + 27;
    }
    HBRUSH labelBrush = CreateSolidBrush(RGB(42, 42, 42));
    FillRect(dc, &label, labelBrush);
    DeleteObject(labelBrush);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, sizeText, -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void SnipWindow::PaintToolbarIcon(HDC dc, int index, const RECT& rect, bool active) {
    if (active) {
        HBRUSH accent = CreateSolidBrush(RGB(224, 239, 255));
        FillRect(dc, &rect, accent);
        DeleteObject(accent);
    }

    const int cx = (rect.left + rect.right) / 2;
    const int cy = (rect.top + rect.bottom) / 2;
    HPEN pen = CreatePen(PS_SOLID, 2, active ? kAccent : kToolbarText);
    const HGDIOBJ oldPen = SelectObject(dc, pen);
    const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));

    switch (index) {
    case 0:
        Rectangle(dc, cx - 9, cy - 7, cx + 9, cy + 7);
        break;
    case 1:
        MoveToEx(dc, cx - 10, cy + 6, nullptr);
        LineTo(dc, cx + 9, cy - 7);
        MoveToEx(dc, cx + 9, cy - 7, nullptr);
        LineTo(dc, cx + 2, cy - 7);
        MoveToEx(dc, cx + 9, cy - 7, nullptr);
        LineTo(dc, cx + 7, cy);
        break;
    case 2:
        MoveToEx(dc, cx - 9, cy + 8, nullptr);
        LineTo(dc, cx + 7, cy - 8);
        MoveToEx(dc, cx - 10, cy + 9, nullptr);
        LineTo(dc, cx - 5, cy + 7);
        break;
    case 3:
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                RECT block{cx - 9 + col * 6, cy - 9 + row * 6, cx - 4 + col * 6, cy - 4 + row * 6};
                if ((row + col) % 2 == 0) {
                    HBRUSH brush = CreateSolidBrush(active ? kAccent : kToolbarText);
                    FillRect(dc, &block, brush);
                    DeleteObject(brush);
                } else {
                    Rectangle(dc, block.left, block.top, block.right, block.bottom);
                }
            }
        }
        break;
    case 4: {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, active ? kAccent : kToolbarText);
        RECT textRect = rect;
        DrawTextW(dc, L"T", -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case 5: {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kToolbarText);
        RECT textRect = rect;
        DrawTextW(dc, L"↶", -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case 6: {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kToolbarText);
        RECT textRect = rect;
        DrawTextW(dc, L"↷", -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case 7:
        MoveToEx(dc, cx - 7, cy - 7, nullptr);
        LineTo(dc, cx + 7, cy + 7);
        MoveToEx(dc, cx + 7, cy - 7, nullptr);
        LineTo(dc, cx - 7, cy + 7);
        break;
    case 8:
        MoveToEx(dc, cx, cy - 9, nullptr);
        LineTo(dc, cx, cy + 8);
        MoveToEx(dc, cx - 6, cy - 3, nullptr);
        LineTo(dc, cx + 6, cy - 3);
        MoveToEx(dc, cx - 6, cy - 3, nullptr);
        LineTo(dc, cx, cy + 2);
        LineTo(dc, cx + 6, cy - 3);
        break;
    case 9:
        Rectangle(dc, cx - 8, cy - 9, cx + 8, cy + 9);
        Rectangle(dc, cx - 5, cy - 6, cx + 4, cy - 1);
        Rectangle(dc, cx - 4, cy + 3, cx + 4, cy + 8);
        break;
    case 10:
        Rectangle(dc, cx - 9, cy - 7, cx + 5, cy + 7);
        Rectangle(dc, cx - 4, cy - 10, cx + 10, cy + 4);
        MoveToEx(dc, cx + 1, cy + 5, nullptr);
        LineTo(dc, cx + 5, cy + 9);
        LineTo(dc, cx + 11, cy + 1);
        break;
    default:
        break;
    }

    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
}

void SnipWindow::PaintToolbar(HDC dc) {
    if (!selected_) {
        return;
    }

    const RECT bar = ToolbarRect();
    RECT shadow{bar.left + 2, bar.top + 3, bar.right + 2, bar.bottom + 3};
    HBRUSH shadowBrush = CreateSolidBrush(RGB(80, 80, 80));
    FillRect(dc, &shadow, shadowBrush);
    DeleteObject(shadowBrush);

    HBRUSH background = CreateSolidBrush(RGB(250, 250, 250));
    FillRect(dc, &bar, background);
    DeleteObject(background);

    HPEN border = CreatePen(PS_SOLID, 1, RGB(205, 205, 205));
    const HGDIOBJ oldPen = SelectObject(dc, border);
    const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, bar.left, bar.top, bar.right, bar.bottom);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(border);

    for (int i = 0; i < kToolbarButtons; ++i) {
        RECT button{
            bar.left + i * kToolbarButton,
            bar.top,
            bar.left + (i + 1) * kToolbarButton,
            bar.bottom,
        };
        const bool active =
            (i == 0 && tool_ == Tool::Rectangle) ||
            (i == 1 && tool_ == Tool::Arrow) ||
            (i == 2 && tool_ == Tool::Pen) ||
            (i == 3 && tool_ == Tool::Mosaic) ||
            (i == 4 && tool_ == Tool::Text);
        PaintToolbarIcon(dc, i, button, active);
    }
}

void SnipWindow::PaintPreview(HDC dc) {
    if (!drawing_ || (tool_ != Tool::Rectangle && tool_ != Tool::Arrow)) {
        return;
    }
    DrawShape(dc, tool_, drawStart_, drawCurrent_);
}

void SnipWindow::Paint() {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd_, &ps);

    HDC memory = CreateCompatibleDC(dc);
    const HGDIOBJ old = SelectObject(memory, capture_);
    BitBlt(dc, 0, 0, screen_.width, screen_.height, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old);
    DeleteDC(memory);

    if (selected_) {
        PaintSelection(dc);
        PaintPreview(dc);
        PaintToolbar(dc);
    } else {
        if (hasHover_) {
            HPEN pen = CreatePen(PS_SOLID, 2, kAccent);
            const HGDIOBJ oldPen = SelectObject(dc, pen);
            const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, hoverRect_.left, hoverRect_.top, hoverRect_.right, hoverRect_.bottom);
            SelectObject(dc, oldPen);
            SelectObject(dc, oldBrush);
            DeleteObject(pen);
        }

        POINT cursor{};
        GetCursorPos(&cursor);
        POINT client{cursor.x - screen_.x, cursor.y - screen_.y};
        PaintMagnifier(dc, client);

        RECT help{12, 12, 690, 40};
        SetBkColor(dc, RGB(35, 35, 35));
        SetTextColor(dc, RGB(255, 255, 255));
        SetBkMode(dc, OPAQUE);
        DrawTextW(dc, L"单击窗口/控件 · 拖动自由选择 · C 取色 · Esc 取消", -1, &help,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    EndPaint(hwnd_, &ps);
}

void SnipWindow::ClearHistory() {
    DeleteBitmaps(undo_);
    DeleteBitmaps(redo_);
    edited_ = false;
}

void SnipWindow::BeginSelectionDrag(POINT point) {
    CommitTextEdit();
    ClearHistory();
    tool_ = Tool::None;
    selected_ = false;
    dragging_ = true;
    dragMode_ = DragMode::NewSelection;
    dragStart_ = point;
    dragCurrent_ = point;
    selection_ = {point.x, point.y, point.x, point.y};
    pressedHoverRect_ = hasHover_ ? hoverRect_ : RECT{};
    SetCapture(hwnd_);
}

void SnipWindow::UpdateSelectionDrag(POINT point) {
    dragCurrent_ = point;

    if (dragMode_ == DragMode::NewSelection) {
        selection_ = NormalizeRect(dragStart_, point);
        ClampSelection();
        selected_ = true;
        return;
    }

    RECT next = dragOrigin_;
    if (dragMode_ == DragMode::Move) {
        const LONG dx = point.x - dragStart_.x;
        const LONG dy = point.y - dragStart_.y;
        OffsetRect(&next, dx, dy);
        selection_ = next;
        ClampSelection();
        return;
    }

    switch (dragMode_) {
    case DragMode::ResizeTopLeft:
        next.left = point.x;
        next.top = point.y;
        break;
    case DragMode::ResizeTop:
        next.top = point.y;
        break;
    case DragMode::ResizeTopRight:
        next.right = point.x;
        next.top = point.y;
        break;
    case DragMode::ResizeRight:
        next.right = point.x;
        break;
    case DragMode::ResizeBottomRight:
        next.right = point.x;
        next.bottom = point.y;
        break;
    case DragMode::ResizeBottom:
        next.bottom = point.y;
        break;
    case DragMode::ResizeBottomLeft:
        next.left = point.x;
        next.bottom = point.y;
        break;
    case DragMode::ResizeLeft:
        next.left = point.x;
        break;
    default:
        break;
    }

    selection_ = next;
    ClampSelection();
}

void SnipWindow::FinishSelectionDrag(POINT point) {
    if (!dragging_) {
        return;
    }

    UpdateSelectionDrag(point);
    dragging_ = false;
    ReleaseCapture();

    RECT r = NormalizedSelection();
    if (dragMode_ == DragMode::NewSelection &&
        r.right - r.left < 4 && r.bottom - r.top < 4 &&
        pressedHoverRect_.right > pressedHoverRect_.left &&
        pressedHoverRect_.bottom > pressedHoverRect_.top) {
        selection_ = pressedHoverRect_;
        r = selection_;
    }

    selected_ = r.right - r.left >= kMinSelection && r.bottom - r.top >= kMinSelection;
    dragMode_ = DragMode::None;
    if (!selected_) {
        selection_ = {};
        UpdateHover();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

HBITMAP SnipWindow::SnapshotSelection() const {
    if (!selected_) {
        return nullptr;
    }
    return CloneBitmapRegion(capture_, NormalizedSelection());
}

void SnipWindow::RestoreSelection(HBITMAP snapshot) {
    if (!snapshot || !selected_) {
        return;
    }

    const RECT r = NormalizedSelection();
    HDC screenDc = GetDC(nullptr);
    HDC sourceDc = CreateCompatibleDC(screenDc);
    HDC targetDc = CreateCompatibleDC(screenDc);
    const HGDIOBJ oldSource = SelectObject(sourceDc, snapshot);
    const HGDIOBJ oldTarget = SelectObject(targetDc, capture_);
    BitBlt(targetDc, r.left, r.top, r.right - r.left, r.bottom - r.top, sourceDc, 0, 0, SRCCOPY);
    SelectObject(sourceDc, oldSource);
    SelectObject(targetDc, oldTarget);
    DeleteDC(sourceDc);
    DeleteDC(targetDc);
    ReleaseDC(nullptr, screenDc);
}

void SnipWindow::BeginEdit() {
    HBITMAP snapshot = SnapshotSelection();
    if (snapshot) {
        undo_.push_back(snapshot);
        if (undo_.size() > 20) {
            DeleteObject(undo_.front());
            undo_.erase(undo_.begin());
        }
    }
    DeleteBitmaps(redo_);
    edited_ = true;
}

void SnipWindow::Undo() {
    CommitTextEdit();
    if (undo_.empty()) {
        return;
    }

    if (HBITMAP current = SnapshotSelection()) {
        redo_.push_back(current);
    }

    HBITMAP previous = undo_.back();
    undo_.pop_back();
    RestoreSelection(previous);
    DeleteObject(previous);
    edited_ = !undo_.empty() || !redo_.empty();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SnipWindow::Redo() {
    CommitTextEdit();
    if (redo_.empty()) {
        return;
    }

    if (HBITMAP current = SnapshotSelection()) {
        undo_.push_back(current);
    }

    HBITMAP next = redo_.back();
    redo_.pop_back();
    RestoreSelection(next);
    DeleteObject(next);
    edited_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SnipWindow::DrawShape(HDC dc, Tool tool, POINT from, POINT to) {
    HPEN pen = CreatePen(PS_SOLID, 3, RGB(235, 70, 70));
    const HGDIOBJ oldPen = SelectObject(dc, pen);
    const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));

    if (tool == Tool::Rectangle) {
        const RECT rect = NormalizeRect(from, to);
        Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
    } else if (tool == Tool::Arrow) {
        MoveToEx(dc, from.x, from.y, nullptr);
        LineTo(dc, to.x, to.y);

        const double angle = std::atan2(
            static_cast<double>(to.y - from.y),
            static_cast<double>(to.x - from.x));
        constexpr double spread = 0.55;
        constexpr double length = 17.0;
        POINT arrow[3] = {
            to,
            {static_cast<LONG>(to.x - length * std::cos(angle - spread)),
             static_cast<LONG>(to.y - length * std::sin(angle - spread))},
            {static_cast<LONG>(to.x - length * std::cos(angle + spread)),
             static_cast<LONG>(to.y - length * std::sin(angle + spread))},
        };
        HBRUSH brush = CreateSolidBrush(RGB(235, 70, 70));
        SelectObject(dc, brush);
        Polygon(dc, arrow, 3);
        SelectObject(dc, oldBrush);
        DeleteObject(brush);
    }

    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
}

void SnipWindow::DrawPenSegment(POINT from, POINT to) {
    HDC dc = CreateCompatibleDC(nullptr);
    const HGDIOBJ oldBitmap = SelectObject(dc, capture_);
    HPEN pen = CreatePen(PS_SOLID, 4, RGB(235, 70, 70));
    const HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, from.x, from.y, nullptr);
    LineTo(dc, to.x, to.y);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBitmap);
    DeleteObject(pen);
    DeleteDC(dc);
}

void SnipWindow::ApplyMosaic(POINT point) {
    constexpr int block = 12;
    const RECT r = NormalizedSelection();
    const int px = static_cast<int>(point.x);
    const int py = static_cast<int>(point.y);
    const int left = std::clamp((px / block) * block, static_cast<int>(r.left), std::max(static_cast<int>(r.left), static_cast<int>(r.right) - 1));
    const int top = std::clamp((py / block) * block, static_cast<int>(r.top), std::max(static_cast<int>(r.top), static_cast<int>(r.bottom) - 1));
    const int right = std::min(static_cast<int>(r.right), left + block);
    const int bottom = std::min(static_cast<int>(r.bottom), top + block);

    HDC dc = CreateCompatibleDC(nullptr);
    const HGDIOBJ oldBitmap = SelectObject(dc, capture_);
    const COLORREF color = GetPixel(
        dc,
        std::min(static_cast<int>(r.right) - 1, left + block / 2),
        std::min(static_cast<int>(r.bottom) - 1, top + block / 2));
    HBRUSH brush = CreateSolidBrush(color == CLR_INVALID ? RGB(128, 128, 128) : color);
    RECT blockRect{left, top, right, bottom};
    FillRect(dc, &blockRect, brush);
    DeleteObject(brush);
    SelectObject(dc, oldBitmap);
    DeleteDC(dc);
}

void SnipWindow::BeginTextEdit(POINT point) {
    CommitTextEdit();
    if (!selected_) {
        return;
    }

    BeginEdit();
    textOrigin_ = ClampPoint(point, NormalizedSelection());
    const RECT r = NormalizedSelection();
    const int width = std::max(80, std::min(240, static_cast<int>(r.right - textOrigin_.x)));

    textEdit_ = CreateWindowExW(
        WS_EX_TOPMOST,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        textOrigin_.x,
        textOrigin_.y,
        width,
        30,
        hwnd_,
        nullptr,
        instance_,
        nullptr);

    if (!textEdit_) {
        return;
    }

    if (!textFont_) {
        textFont_ = CreateFontW(
            -20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }
    if (textFont_) {
        SendMessageW(textEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(textFont_), TRUE);
    }
    SetFocus(textEdit_);
}

void SnipWindow::CommitTextEdit() {
    if (!textEdit_) {
        return;
    }

    const int length = GetWindowTextLengthW(textEdit_);
    std::wstring text(static_cast<size_t>(std::max(0, length)), L'\0');
    if (length > 0) {
        GetWindowTextW(textEdit_, text.data(), length + 1);
    }

    HWND edit = textEdit_;
    textEdit_ = nullptr;
    DestroyWindow(edit);

    if (!text.empty()) {
        HDC dc = CreateCompatibleDC(nullptr);
        const HGDIOBJ oldBitmap = SelectObject(dc, capture_);
        const HGDIOBJ oldFont = textFont_ ? SelectObject(dc, textFont_) : nullptr;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(235, 70, 70));
        TextOutW(dc, textOrigin_.x, textOrigin_.y, text.c_str(), static_cast<int>(text.size()));
        if (oldFont) SelectObject(dc, oldFont);
        SelectObject(dc, oldBitmap);
        DeleteDC(dc);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SnipWindow::HandleToolbarClick(int index) {
    if (index < 0 || index >= kToolbarButtons) {
        return;
    }

    if (index != 4) {
        CommitTextEdit();
    }

    switch (index) {
    case 0:
        tool_ = tool_ == Tool::Rectangle ? Tool::None : Tool::Rectangle;
        break;
    case 1:
        tool_ = tool_ == Tool::Arrow ? Tool::None : Tool::Arrow;
        break;
    case 2:
        tool_ = tool_ == Tool::Pen ? Tool::None : Tool::Pen;
        break;
    case 3:
        tool_ = tool_ == Tool::Mosaic ? Tool::None : Tool::Mosaic;
        break;
    case 4:
        tool_ = tool_ == Tool::Text ? Tool::None : Tool::Text;
        break;
    case 5:
        Undo();
        return;
    case 6:
        Redo();
        return;
    case 7:
        DestroyWindow(hwnd_);
        return;
    case 8:
        Finish(FinishAction::Pin);
        return;
    case 9:
        Finish(FinishAction::Save);
        return;
    case 10:
        Finish(FinishAction::Copy);
        return;
    default:
        return;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SnipWindow::Finish(FinishAction action) {
    CommitTextEdit();
    if (!selected_) {
        return;
    }

    HBITMAP result = CloneBitmapRegion(capture_, NormalizedSelection());
    if (!result) {
        return;
    }

    auto callback = callback_;
    DestroyWindow(hwnd_);
    if (callback) {
        callback(result, action);
    } else {
        DeleteObject(result);
    }
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

LRESULT SnipWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;

    case WM_SETCURSOR: {
        POINT cursor{};
        GetCursorPos(&cursor);
        ScreenToClient(hwnd_, &cursor);
        SetCursorForPoint(cursor);
        return TRUE;
    }

    case WM_PAINT:
        Paint();
        return 0;

    case WM_MOUSEMOVE: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (drawing_) {
            point = ClampPoint(point, NormalizedSelection());
            if (tool_ == Tool::Pen) {
                DrawPenSegment(drawCurrent_, point);
            } else if (tool_ == Tool::Mosaic) {
                ApplyMosaic(point);
            }
            drawCurrent_ = point;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (dragging_) {
            UpdateSelectionDrag(point);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (!selected_) {
            UpdateHover();
        } else {
            SetCursorForPoint(point);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int toolbarIndex = HitToolbar(point);
        if (toolbarIndex >= 0) {
            HandleToolbarClick(toolbarIndex);
            return 0;
        }

        if (!selected_) {
            BeginSelectionDrag(point);
            return 0;
        }

        CommitTextEdit();
        const RECT r = NormalizedSelection();
        if (tool_ == Tool::Text && PtInRect(&r, point)) {
            BeginTextEdit(point);
            return 0;
        }

        if (tool_ != Tool::None && PtInRect(&r, point)) {
            drawing_ = true;
            drawStart_ = ClampPoint(point, r);
            drawCurrent_ = drawStart_;
            BeginEdit();
            SetCapture(hwnd_);
            if (tool_ == Tool::Mosaic) {
                ApplyMosaic(drawStart_);
            }
            return 0;
        }

        dragMode_ = HitSelection(point);
        if (dragMode_ != DragMode::None) {
            dragging_ = true;
            dragStart_ = point;
            dragCurrent_ = point;
            dragOrigin_ = selection_;
            SetCapture(hwnd_);
            return 0;
        }

        if (!edited_) {
            BeginSelectionDrag(point);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (drawing_) {
            point = ClampPoint(point, NormalizedSelection());
            drawCurrent_ = point;
            ReleaseCapture();

            if (tool_ == Tool::Rectangle || tool_ == Tool::Arrow) {
                HDC dc = CreateCompatibleDC(nullptr);
                const HGDIOBJ oldBitmap = SelectObject(dc, capture_);
                DrawShape(dc, tool_, drawStart_, drawCurrent_);
                SelectObject(dc, oldBitmap);
                DeleteDC(dc);
            }
            drawing_ = false;
            edited_ = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (dragging_) {
            FinishSelectionDrag(point);
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const RECT r = NormalizedSelection();
        if (selected_ && PtInRect(&r, point)) {
            Finish(FinishAction::Copy);
        }
        return 0;
    }

    case WM_RBUTTONDOWN:
        if (tool_ != Tool::None) {
            tool_ = Tool::None;
            CommitTextEdit();
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else {
            DestroyWindow(hwnd_);
        }
        return 0;

    case WM_COMMAND:
        if (textEdit_ && reinterpret_cast<HWND>(lParam) == textEdit_ && HIWORD(wParam) == EN_KILLFOCUS) {
            CommitTextEdit();
            return 0;
        }
        break;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            if (tool_ != Tool::None || textEdit_) {
                CommitTextEdit();
                tool_ = Tool::None;
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else {
                DestroyWindow(hwnd_);
            }
            return 0;
        }
        if (wParam == VK_RETURN && selected_) {
            Finish(FinishAction::Copy);
            return 0;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'Z') {
            Undo();
            return 0;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'Y') {
            Redo();
            return 0;
        }
        if (!selected_ && wParam == 'C') {
            CopyCurrentColor();
            return 0;
        }
        if (selected_) {
            if (wParam == 'R') tool_ = Tool::Rectangle;
            if (wParam == 'A') tool_ = Tool::Arrow;
            if (wParam == 'P') tool_ = Tool::Pen;
            if (wParam == 'M') tool_ = Tool::Mosaic;
            if (wParam == 'T') tool_ = Tool::Text;

            if (!edited_ && (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_UP || wParam == VK_DOWN)) {
                const int step = (GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1;
                if (wParam == VK_LEFT) OffsetRect(&selection_, -step, 0);
                if (wParam == VK_RIGHT) OffsetRect(&selection_, step, 0);
                if (wParam == VK_UP) OffsetRect(&selection_, 0, -step);
                if (wParam == VK_DOWN) OffsetRect(&selection_, 0, step);
                ClampSelection();
            }
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