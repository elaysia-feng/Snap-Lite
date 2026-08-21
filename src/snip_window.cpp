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
constexpr COLORREF kInk = RGB(236, 239, 244);
constexpr BYTE kDimSelected = 148;
constexpr BYTE kDimIdle = 66;

Gdiplus::Color Chrome(BYTE alpha = 242) { return {alpha, 27, 30, 35}; }
Gdiplus::Color ChromeLine() { return {56, 255, 255, 255}; }
Gdiplus::Color ChromeShade() { return {110, 0, 0, 0}; }
Gdiplus::Color Accent(BYTE alpha = 255) { return {alpha, 255, 197, 61}; }
Gdiplus::Color Ink(BYTE alpha = 255) { return {alpha, 236, 239, 244}; }
Gdiplus::Color Danger() { return {255, 255, 107, 91}; }

constexpr int kHandleRadius = 4;
constexpr int kHandleHitRadius = 9;
constexpr int kToolbarButton = 36;
constexpr int kToolbarHeight = 40;
constexpr int kToolbarButtons = 12;
constexpr int kToolbarPadX = 6;
constexpr int kToolbarGroupGap = 11;
constexpr int kToolbarRadius = 10;
constexpr int kStyleHeight = 38;
constexpr int kMinSelection = 8;

constexpr COLORREF kAnnotationColors[] = {
    RGB(235, 70, 70),
    RGB(255, 145, 45),
    RGB(250, 205, 55),
    RGB(65, 190, 105),
    RGB(68, 145, 245),
    RGB(245, 245, 245),
};
constexpr int kStrokeWidths[] = {2, 4, 7};
constexpr int kFontSizes[] = {16, 22, 30};
constexpr int kMosaicBlocks[] = {8, 12, 18};

constexpr const wchar_t* kToolbarTips[kToolbarButtons] = {
    L"矩形 (R)",
    L"椭圆 (O)",
    L"箭头 (A)",
    L"画笔 (P)",
    L"马赛克 (M)",
    L"文字 (T)",
    L"撤销 (Ctrl+Z)",
    L"重做 (Ctrl+Y)",
    L"取消 (Esc)",
    L"贴到桌面",
    L"保存图片",
    L"完成并复制 (Enter)",
};

constexpr bool GroupBreakAfter(int index) {
    return index == 5 || index == 7 || index == 8;
}

constexpr int ToolbarButtonOffset(int index) {
    int x = kToolbarPadX;
    for (int i = 0; i < index; ++i) {
        x += kToolbarButton;
        if (GroupBreakAfter(i)) {
            x += kToolbarGroupGap;
        }
    }
    return x;
}

constexpr int kToolbarWidth = ToolbarButtonOffset(kToolbarButtons) + kToolbarPadX;

void AddRoundRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius) {
    path.Reset();
    if (radius <= 0.0f) {
        path.AddRectangle(r);
        path.CloseFigure();
        return;
    }
    const float d = radius * 2.0f;
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.GetBottom() - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void FillRoundRect(Gdiplus::Graphics& g, const Gdiplus::Color& color, const Gdiplus::RectF& r, float radius) {
    Gdiplus::GraphicsPath path;
    AddRoundRect(path, r, radius);
    Gdiplus::SolidBrush brush(color);
    g.FillPath(&brush, &path);
}

void TwoToneRoundRect(Gdiplus::Graphics& g, const Gdiplus::Color& tone, const Gdiplus::RectF& r, float radius) {
    Gdiplus::GraphicsPath path;
    Gdiplus::RectF outer(r.X - 1.0f, r.Y - 1.0f, r.Width + 2.0f, r.Height + 2.0f);
    AddRoundRect(path, outer, radius + 1.0f);
    Gdiplus::Pen shade(ChromeShade(), 1.0f);
    g.DrawPath(&shade, &path);

    AddRoundRect(path, r, radius);
    Gdiplus::Pen pen(tone, 1.0f);
    g.DrawPath(&pen, &path);
}

void DropShadow(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius, int spread) {
    Gdiplus::GraphicsPath path;
    for (int i = spread; i >= 1; --i) {
        const float f = static_cast<float>(i);
        Gdiplus::RectF s(r.X - f, r.Y - f + 2.0f, r.Width + f * 2.0f, r.Height + f * 2.0f);
        AddRoundRect(path, s, radius + f);
        Gdiplus::SolidBrush brush(Gdiplus::Color(13, 0, 0, 0));
        g.FillPath(&brush, &path);
    }
}

void Plate(Gdiplus::Graphics& g, const RECT& r, float radius) {
    Gdiplus::RectF box(
        static_cast<float>(r.left) + 0.5f,
        static_cast<float>(r.top) + 0.5f,
        static_cast<float>(r.right - r.left) - 1.0f,
        static_cast<float>(r.bottom - r.top) - 1.0f);
    DropShadow(g, box, radius, 4);
    FillRoundRect(g, Chrome(235), box, radius);
    TwoToneRoundRect(g, ChromeLine(), box, radius);
}

int MeasureText(HDC dc, const wchar_t* text) {
    SIZE size{};
    GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &size);
    return size.cx;
}

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

Gdiplus::Color ToGdiColor(COLORREF color, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
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
    if (uiFont_) {
        DeleteObject(uiFont_);
        uiFont_ = nullptr;
    }
    if (monoFont_) {
        DeleteObject(monoFont_);
        monoFont_ = nullptr;
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

RECT SnipWindow::ToolbarButtonRect(const RECT& bar, int index) const {
    const int left = bar.left + ToolbarButtonOffset(index);
    return {left, bar.top + 2, left + kToolbarButton, bar.bottom - 2};
}

RECT SnipWindow::StyleBarRect() const {
    if (!selected_ || tool_ == Tool::None) {
        return {};
    }

    const RECT mainBar = ToolbarRect();
    const RECT selection = NormalizedSelection();
    const int width = tool_ == Tool::Mosaic ? 190 : 342;
    const int x = std::clamp(
        static_cast<int>(mainBar.left),
        8,
        std::max(8, screen_.width - width - 8));

    int y{};
    if (mainBar.top >= selection.bottom) {
        y = mainBar.bottom + 6;
        if (y + kStyleHeight > screen_.height - 8) {
            y = mainBar.top - kStyleHeight - 6;
        }
    } else {
        y = mainBar.top - kStyleHeight - 6;
        if (y < 8) {
            y = mainBar.bottom + 6;
        }
    }
    y = std::clamp(y, 8, std::max(8, screen_.height - kStyleHeight - 8));
    return {x, y, x + width, y + kStyleHeight};
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
    for (int i = 0; i < kToolbarButtons; ++i) {
        const RECT button = ToolbarButtonRect(bar, i);
        if (PtInRect(&button, point)) {
            return i;
        }
    }
    return -1;
}

int SnipWindow::HitStyle(POINT point) const {
    const RECT bar = StyleBarRect();
    if (bar.right <= bar.left || !PtInRect(&bar, point)) {
        return -1;
    }

    if (tool_ == Tool::Mosaic) {
        const int start = bar.left + 54;
        for (int i = 0; i < 3; ++i) {
            RECT item{start + i * 40, bar.top + 4, start + i * 40 + 36, bar.bottom - 4};
            if (PtInRect(&item, point)) return 100 + i;
        }
        return -1;
    }

    const int colorStart = bar.left + 48;
    for (int i = 0; i < static_cast<int>(std::size(kAnnotationColors)); ++i) {
        RECT item{colorStart + i * 25, bar.top + 5, colorStart + i * 25 + 22, bar.bottom - 5};
        if (PtInRect(&item, point)) return i;
    }

    const int optionStart = bar.left + 232;
    for (int i = 0; i < 3; ++i) {
        RECT item{optionStart + i * 34, bar.top + 4, optionStart + i * 34 + 30, bar.bottom - 4};
        if (PtInRect(&item, point)) return 100 + i;
    }
    return -1;
}

void SnipWindow::SetCursorForPoint(POINT point) {
    if (HitToolbar(point) >= 0 || HitStyle(point) >= 0) {
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

void SnipWindow::EnsureFonts() {
    if (!uiFont_) {
        uiFont_ = CreateFontW(
            -13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }
    if (!monoFont_) {
        monoFont_ = CreateFontW(
            -12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            FIXED_PITCH | FF_MODERN, L"Consolas");
    }
}

void SnipWindow::RecreateTextFont() {
    if (textFont_) {
        DeleteObject(textFont_);
        textFont_ = nullptr;
    }
    textFont_ = CreateFontW(
        -fontSize_, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (textEdit_ && textFont_) {
        SendMessageW(textEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(textFont_), TRUE);
    }
}

void SnipWindow::PaintMagnifier(HDC dc, POINT clientPoint) {
    constexpr int sampleRadius = 6;
    constexpr int sampleSize = sampleRadius * 2 + 1;
    constexpr int zoom = 10;
    constexpr int lensSize = sampleSize * zoom;
    constexpr int pad = 6;
    constexpr int readout = 24;
    constexpr int panelW = lensSize + pad * 2;
    constexpr int panelH = lensSize + pad * 2 + readout;

    const int clientX = static_cast<int>(clientPoint.x);
    const int clientY = static_cast<int>(clientPoint.y);
    int panelX = clientX + 22;
    int panelY = clientY + 22;
    if (panelX + panelW + 6 > screen_.width) panelX = clientX - panelW - 22;
    if (panelY + panelH + 6 > screen_.height) panelY = clientY - panelH - 22;
    panelX = std::max(6, panelX);
    panelY = std::max(6, panelY);

    HDC memory = CreateCompatibleDC(dc);
    if (!memory) return;
    const HGDIOBJ old = SelectObject(memory, capture_);
    const COLORREF color = GetPixel(memory, clientX, clientY);

    const RECT panel{panelX, panelY, panelX + panelW, panelY + panelH};
    {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Plate(graphics, panel, 8.0f);
    }

    const int lensX = panelX + pad;
    const int lensY = panelY + pad;
    SetStretchBltMode(dc, COLORONCOLOR);
    const int sourceX = std::clamp(clientX - sampleRadius, 0, std::max(0, screen_.width - sampleSize));
    const int sourceY = std::clamp(clientY - sampleRadius, 0, std::max(0, screen_.height - sampleSize));
    StretchBlt(dc, lensX, lensY, lensSize, lensSize, memory, sourceX, sourceY, sampleSize, sampleSize, SRCCOPY);

    SelectObject(memory, old);
    DeleteDC(memory);

    {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeNone);
        Gdiplus::Pen grid(Gdiplus::Color(90, 128, 128, 128), 1.0f);
        for (int i = 1; i < sampleSize; ++i) {
            const float offset = static_cast<float>(i * zoom);
            graphics.DrawLine(&grid, lensX + offset, static_cast<float>(lensY), lensX + offset, static_cast<float>(lensY + lensSize));
            graphics.DrawLine(&grid, static_cast<float>(lensX), lensY + offset, static_cast<float>(lensX + lensSize), lensY + offset);
        }
        const float cell = static_cast<float>(sampleRadius * zoom);
        Gdiplus::Pen mark(Accent(), 1.0f);
        graphics.DrawRectangle(&mark, lensX + cell, lensY + cell, static_cast<float>(zoom), static_cast<float>(zoom));
        Gdiplus::Pen edge(ChromeLine(), 1.0f);
        graphics.DrawRectangle(&edge, lensX - 1.0f, lensY - 1.0f, lensSize + 1.0f, lensSize + 1.0f);
    }

    EnsureFonts();
    wchar_t text[32]{};
    if (color != CLR_INVALID) {
        swprintf_s(text, L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
    } else {
        wcscpy_s(text, L"------");
    }

    const int rowY = lensY + lensSize + pad;
    RECT row{panelX + pad, rowY, panelX + panelW - pad, rowY + readout};
    SetBkMode(dc, TRANSPARENT);
    const HGDIOBJ oldFont = SelectObject(dc, monoFont_);
    SetTextColor(dc, kInk);
    DrawTextW(dc, text, -1, &row, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, uiFont_);
    SetTextColor(dc, RGB(146, 152, 164));
    DrawTextW(dc, L"C 复制", -1, &row, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
}

void SnipWindow::PaintSelection(HDC dc) {
    if (!selected_) return;

    const RECT r = NormalizedSelection();
    {
        Gdiplus::Graphics graphics(dc);
        Gdiplus::SolidBrush dimBrush(Gdiplus::Color(kDimSelected, 0, 0, 0));
        const auto fillDim = [&](LONG x, LONG y, LONG width, LONG height) {
            if (width > 0 && height > 0) {
                graphics.FillRectangle(&dimBrush, static_cast<INT>(x), static_cast<INT>(y), static_cast<INT>(width), static_cast<INT>(height));
            }
        };
        fillDim(0, 0, screen_.width, r.top);
        fillDim(0, r.bottom, screen_.width, screen_.height - r.bottom);
        fillDim(0, r.top, r.left, r.bottom - r.top);
        fillDim(r.right, r.top, screen_.width - r.right, r.bottom - r.top);

        graphics.SetSmoothingMode(Gdiplus::SmoothingModeNone);
        const float left = static_cast<float>(r.left);
        const float top = static_cast<float>(r.top);
        const float width = static_cast<float>(r.right - r.left);
        const float height = static_cast<float>(r.bottom - r.top);
        Gdiplus::Pen shade(ChromeShade(), 1.0f);
        graphics.DrawRectangle(&shade, left - 1.0f, top - 1.0f, width + 1.0f, height + 1.0f);
        Gdiplus::Pen frame(Accent(), 1.0f);
        graphics.DrawRectangle(&frame, left, top, width - 1.0f, height - 1.0f);

        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const LONG cx = (r.left + r.right) / 2;
        const LONG cy = (r.top + r.bottom) / 2;
        const POINT handles[8] = {
            {r.left, r.top}, {cx, r.top}, {r.right, r.top}, {r.right, cy},
            {r.right, r.bottom}, {cx, r.bottom}, {r.left, r.bottom}, {r.left, cy},
        };
        Gdiplus::SolidBrush handleFill(Accent());
        Gdiplus::Pen handleRing(ChromeShade(), 1.0f);
        Gdiplus::GraphicsPath path;
        for (const POINT handle : handles) {
            Gdiplus::RectF box(
                static_cast<float>(handle.x - kHandleRadius),
                static_cast<float>(handle.y - kHandleRadius),
                kHandleRadius * 2.0f,
                kHandleRadius * 2.0f);
            AddRoundRect(path, box, 1.5f);
            graphics.FillPath(&handleFill, &path);
            graphics.DrawPath(&handleRing, &path);
        }
    }

    EnsureFonts();
    wchar_t sizeText[64]{};
    swprintf_s(sizeText, L"%ld × %ld", r.right - r.left, r.bottom - r.top);
    SetBkMode(dc, TRANSPARENT);
    const HGDIOBJ oldFont = SelectObject(dc, monoFont_);
    constexpr int badgeHeight = 24;
    const int badgeWidth = MeasureText(dc, sizeText) + 26;
    RECT badge{r.left, r.top - badgeHeight - 8, r.left + badgeWidth, r.top - 8};
    if (badge.top < 6) {
        badge.left = r.left + 8;
        badge.right = badge.left + badgeWidth;
        badge.top = r.top + 8;
        badge.bottom = badge.top + badgeHeight;
    }
    if (badge.right > screen_.width - 6) {
        const int shift = badge.right - (screen_.width - 6);
        badge.left -= shift;
        badge.right -= shift;
    }
    {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Plate(graphics, badge, 6.0f);
    }
    SelectObject(dc, monoFont_);
    SetTextColor(dc, kInk);
    RECT badgeText{badge.left + 10, badge.top, badge.right - 7, badge.bottom};
    DrawTextW(dc, sizeText, -1, &badgeText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
}

void SnipWindow::PaintHint(HDC dc) {
    EnsureFonts();
    static constexpr wchar_t kHint[] = L"单击窗口 / 控件     拖动自由选择     C 取色     Esc 取消";
    SetBkMode(dc, TRANSPARENT);
    const HGDIOBJ oldFont = SelectObject(dc, uiFont_);
    RECT plate{18, 18, 18 + MeasureText(dc, kHint) + 30, 50};
    {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Plate(graphics, plate, 8.0f);
    }
    SelectObject(dc, uiFont_);
    SetTextColor(dc, kInk);
    DrawTextW(dc, kHint, -1, &plate, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
}

void SnipWindow::PaintToolbarIcon(HDC dc, int index, const RECT& rect, bool active, bool hovered) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float cx = (rect.left + rect.right) / 2.0f;
    const float cy = (rect.top + rect.bottom) / 2.0f;

    if (hovered) {
        Gdiplus::RectF box(
            static_cast<float>(rect.left) + 2.0f,
            static_cast<float>(rect.top) + 2.0f,
            static_cast<float>(rect.right - rect.left) - 4.0f,
            static_cast<float>(rect.bottom - rect.top) - 4.0f);
        FillRoundRect(graphics, Gdiplus::Color(28, 255, 255, 255), box, 6.0f);
    }
    if (active) {
        Gdiplus::SolidBrush accent(Accent());
        graphics.FillRectangle(&accent, cx - 9.0f, static_cast<float>(rect.bottom) - 6.0f, 18.0f, 2.0f);
    }

    Gdiplus::Color tone = Ink(hovered ? 255 : 205);
    if (index == 8) tone = hovered ? Danger() : Ink(170);
    if (index == 11) tone = Accent(hovered ? 255 : 225);
    if (active) tone = Accent();

    Gdiplus::Pen pen(tone, 1.7f);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::SolidBrush brush(tone);

    switch (index) {
    case 0:
        graphics.DrawRectangle(&pen, cx - 8.0f, cy - 6.5f, 16.0f, 13.0f);
        break;
    case 1:
        graphics.DrawEllipse(&pen, cx - 8.0f, cy - 7.0f, 16.0f, 14.0f);
        break;
    case 2: {
        graphics.DrawLine(&pen, cx - 8.0f, cy + 7.0f, cx + 5.5f, cy - 5.5f);
        const Gdiplus::PointF head[3] = {{cx + 8.0f, cy - 8.0f}, {cx + 0.5f, cy - 6.5f}, {cx + 6.5f, cy - 0.5f}};
        graphics.FillPolygon(&brush, head, 3);
        break;
    }
    case 3:
        graphics.DrawLine(&pen, cx + 4.0f, cy - 8.0f, cx + 8.0f, cy - 4.0f);
        graphics.DrawLine(&pen, cx + 4.0f, cy - 8.0f, cx - 6.0f, cy + 2.0f);
        graphics.DrawLine(&pen, cx + 8.0f, cy - 4.0f, cx - 2.0f, cy + 6.0f);
        graphics.DrawLine(&pen, cx - 6.0f, cy + 2.0f, cx - 7.5f, cy + 7.5f);
        graphics.DrawLine(&pen, cx - 2.0f, cy + 6.0f, cx - 7.5f, cy + 7.5f);
        break;
    case 4:
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                if ((row + col) % 2 == 0) {
                    graphics.FillRectangle(&brush, cx - 8.0f + col * 5.5f, cy - 8.0f + row * 5.5f, 4.5f, 4.5f);
                }
            }
        }
        break;
    case 5:
        graphics.DrawLine(&pen, cx - 7.0f, cy - 7.0f, cx + 7.0f, cy - 7.0f);
        graphics.DrawLine(&pen, cx, cy - 7.0f, cx, cy + 8.0f);
        break;
    case 6: {
        graphics.DrawArc(&pen, cx - 8.0f, cy - 6.0f, 16.0f, 14.0f, 180.0f, 200.0f);
        const Gdiplus::PointF head[3] = {{cx - 11.5f, cy - 1.5f}, {cx - 4.5f, cy - 1.5f}, {cx - 8.0f, cy + 5.5f}};
        graphics.FillPolygon(&brush, head, 3);
        break;
    }
    case 7: {
        graphics.DrawArc(&pen, cx - 8.0f, cy - 6.0f, 16.0f, 14.0f, 0.0f, -200.0f);
        const Gdiplus::PointF head[3] = {{cx + 4.5f, cy - 1.5f}, {cx + 11.5f, cy - 1.5f}, {cx + 8.0f, cy + 5.5f}};
        graphics.FillPolygon(&brush, head, 3);
        break;
    }
    case 8:
        graphics.DrawLine(&pen, cx - 6.5f, cy - 6.5f, cx + 6.5f, cy + 6.5f);
        graphics.DrawLine(&pen, cx + 6.5f, cy - 6.5f, cx - 6.5f, cy + 6.5f);
        break;
    case 9: {
        Gdiplus::GraphicsPath cap;
        AddRoundRect(cap, Gdiplus::RectF(cx - 7.0f, cy - 8.5f, 14.0f, 4.5f), 2.0f);
        graphics.FillPath(&brush, &cap);
        const Gdiplus::PointF body[4] = {{cx - 4.5f, cy - 4.0f}, {cx + 4.5f, cy - 4.0f}, {cx + 6.5f, cy + 1.5f}, {cx - 6.5f, cy + 1.5f}};
        graphics.FillPolygon(&brush, body, 4);
        graphics.DrawLine(&pen, cx, cy + 1.5f, cx, cy + 8.5f);
        break;
    }
    case 10:
        graphics.DrawLine(&pen, cx, cy - 8.0f, cx, cy + 2.5f);
        graphics.DrawLine(&pen, cx - 4.5f, cy - 2.0f, cx, cy + 2.5f);
        graphics.DrawLine(&pen, cx + 4.5f, cy - 2.0f, cx, cy + 2.5f);
        graphics.DrawLine(&pen, cx - 8.0f, cy + 7.5f, cx + 8.0f, cy + 7.5f);
        break;
    case 11: {
        Gdiplus::GraphicsPath path;
        AddRoundRect(path, Gdiplus::RectF(cx - 3.0f, cy - 3.0f, 11.0f, 11.0f), 2.0f);
        graphics.DrawPath(&pen, &path);
        graphics.DrawLine(&pen, cx - 6.5f, cy + 1.0f, cx - 6.5f, cy - 6.5f);
        graphics.DrawLine(&pen, cx - 6.5f, cy - 6.5f, cx + 1.0f, cy - 6.5f);
        break;
    }
    default:
        break;
    }
}

void SnipWindow::PaintToolbar(HDC dc) {
    if (!selected_) return;

    const RECT bar = ToolbarRect();
    {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::RectF box(
            static_cast<float>(bar.left) + 0.5f,
            static_cast<float>(bar.top) + 0.5f,
            static_cast<float>(bar.right - bar.left) - 1.0f,
            static_cast<float>(bar.bottom - bar.top) - 1.0f);
        DropShadow(graphics, box, static_cast<float>(kToolbarRadius), 7);
        FillRoundRect(graphics, Chrome(244), box, static_cast<float>(kToolbarRadius));
        TwoToneRoundRect(graphics, ChromeLine(), box, static_cast<float>(kToolbarRadius));
        Gdiplus::Pen divider(Gdiplus::Color(38, 255, 255, 255), 1.0f);
        for (int i = 0; i < kToolbarButtons - 1; ++i) {
            if (!GroupBreakAfter(i)) continue;
            const float x = static_cast<float>(bar.left + ToolbarButtonOffset(i) + kToolbarButton + kToolbarGroupGap / 2);
            graphics.DrawLine(&divider, x, static_cast<float>(bar.top) + 11.0f, x, static_cast<float>(bar.bottom) - 11.0f);
        }
    }

    for (int i = 0; i < kToolbarButtons; ++i) {
        const RECT button = ToolbarButtonRect(bar, i);
        const bool active =
            (i == 0 && tool_ == Tool::Rectangle) ||
            (i == 1 && tool_ == Tool::Ellipse) ||
            (i == 2 && tool_ == Tool::Arrow) ||
            (i == 3 && tool_ == Tool::Pen) ||
            (i == 4 && tool_ == Tool::Mosaic) ||
            (i == 5 && tool_ == Tool::Text);
        PaintToolbarIcon(dc, i, button, active, hoverToolbar_ == i);
    }
}

void SnipWindow::PaintStyleBar(HDC dc) {
    const RECT bar = StyleBarRect();
    if (bar.right <= bar.left) return;

    EnsureFonts();
    {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Plate(graphics, bar, 8.0f);
    }

    const HGDIOBJ oldFont = SelectObject(dc, uiFont_);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(190, 195, 205));

    if (tool_ == Tool::Mosaic) {
        RECT label{bar.left + 10, bar.top, bar.left + 52, bar.bottom};
        DrawTextW(dc, L"颗粒", -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        const int start = bar.left + 54;
        for (int i = 0; i < 3; ++i) {
            RECT item{start + i * 40, bar.top + 4, start + i * 40 + 36, bar.bottom - 4};
            const bool active = mosaicBlock_ == kMosaicBlocks[i];
            {
                Gdiplus::Graphics graphics(dc);
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                if (active || hoverStyle_ == 100 + i) {
                    Gdiplus::RectF chip(static_cast<float>(item.left), static_cast<float>(item.top), static_cast<float>(item.right - item.left), static_cast<float>(item.bottom - item.top));
                    FillRoundRect(graphics, active ? Gdiplus::Color(42, 255, 197, 61) : Gdiplus::Color(24, 255, 255, 255), chip, 5.0f);
                }
            }
            wchar_t text[8]{};
            swprintf_s(text, L"%d", kMosaicBlocks[i]);
            SetTextColor(dc, active ? RGB(255, 210, 95) : kInk);
            DrawTextW(dc, text, -1, &item, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(dc, oldFont);
        return;
    }

    RECT colorLabel{bar.left + 10, bar.top, bar.left + 47, bar.bottom};
    DrawTextW(dc, L"颜色", -1, &colorLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const int colorStart = bar.left + 48;
    for (int i = 0; i < static_cast<int>(std::size(kAnnotationColors)); ++i) {
        RECT item{colorStart + i * 25, bar.top + 5, colorStart + i * 25 + 22, bar.bottom - 5};
        const bool active = annotationColor_ == kAnnotationColors[i];
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        if (hoverStyle_ == i) {
            Gdiplus::RectF hover(static_cast<float>(item.left - 2), static_cast<float>(item.top - 2), 26.0f, 30.0f);
            FillRoundRect(graphics, Gdiplus::Color(24, 255, 255, 255), hover, 5.0f);
        }
        Gdiplus::RectF swatch(static_cast<float>(item.left + 4), static_cast<float>(item.top + 6), 14.0f, 14.0f);
        FillRoundRect(graphics, ToGdiColor(kAnnotationColors[i]), swatch, 7.0f);
        if (active) {
            Gdiplus::Pen ring(Accent(), 2.0f);
            graphics.DrawEllipse(&ring, swatch.X - 3.0f, swatch.Y - 3.0f, swatch.Width + 6.0f, swatch.Height + 6.0f);
        }
    }

    RECT optionLabel{bar.left + 194, bar.top, bar.left + 231, bar.bottom};
    DrawTextW(dc, tool_ == Tool::Text ? L"字号" : L"粗细", -1, &optionLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const int optionStart = bar.left + 232;
    for (int i = 0; i < 3; ++i) {
        RECT item{optionStart + i * 34, bar.top + 4, optionStart + i * 34 + 30, bar.bottom - 4};
        const bool active = tool_ == Tool::Text ? fontSize_ == kFontSizes[i] : strokeWidth_ == kStrokeWidths[i];
        {
            Gdiplus::Graphics graphics(dc);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            if (active || hoverStyle_ == 100 + i) {
                Gdiplus::RectF chip(static_cast<float>(item.left), static_cast<float>(item.top), static_cast<float>(item.right - item.left), static_cast<float>(item.bottom - item.top));
                FillRoundRect(graphics, active ? Gdiplus::Color(42, 255, 197, 61) : Gdiplus::Color(24, 255, 255, 255), chip, 5.0f);
            }
            if (tool_ != Tool::Text) {
                Gdiplus::Pen sample(active ? Accent() : Ink(), static_cast<float>(kStrokeWidths[i]));
                sample.SetStartCap(Gdiplus::LineCapRound);
                sample.SetEndCap(Gdiplus::LineCapRound);
                graphics.DrawLine(&sample, static_cast<float>(item.left + 6), static_cast<float>((item.top + item.bottom) / 2), static_cast<float>(item.right - 6), static_cast<float>((item.top + item.bottom) / 2));
            }
        }
        if (tool_ == Tool::Text) {
            wchar_t text[8]{};
            swprintf_s(text, L"%d", kFontSizes[i]);
            SetTextColor(dc, active ? RGB(255, 210, 95) : kInk);
            DrawTextW(dc, text, -1, &item, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    SelectObject(dc, oldFont);
}

void SnipWindow::PaintTooltip(HDC dc) {
    if (hoverToolbar_ < 0 || hoverToolbar_ >= kToolbarButtons) return;

    EnsureFonts();
    const wchar_t* text = kToolbarTips[hoverToolbar_];
    const HGDIOBJ oldFont = SelectObject(dc, uiFont_);
    SetBkMode(dc, TRANSPARENT);
    const int width = MeasureText(dc, text) + 22;
    const RECT button = ToolbarButtonRect(ToolbarRect(), hoverToolbar_);
    int left = (button.left + button.right - width) / 2;
    left = std::clamp(left, 6, std::max(6, screen_.width - width - 6));
    int top = ToolbarRect().top - 34;
    if (top < 6) top = ToolbarRect().bottom + 6;
    RECT tip{left, top, left + width, top + 28};
    {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Plate(graphics, tip, 6.0f);
    }
    SelectObject(dc, uiFont_);
    SetTextColor(dc, kInk);
    DrawTextW(dc, text, -1, &tip, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
}

void SnipWindow::PaintPreview(HDC dc) {
    if (!drawing_ || (tool_ != Tool::Rectangle && tool_ != Tool::Ellipse && tool_ != Tool::Arrow)) return;
    DrawShape(dc, tool_, drawStart_, drawCurrent_);
}

void SnipWindow::Paint() {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd_, &ps);

    HDC frameDc = CreateCompatibleDC(dc);
    HBITMAP frameBitmap = CreateCompatibleBitmap(dc, screen_.width, screen_.height);
    if (!frameDc || !frameBitmap) {
        if (frameBitmap) DeleteObject(frameBitmap);
        if (frameDc) DeleteDC(frameDc);
        EndPaint(hwnd_, &ps);
        return;
    }
    const HGDIOBJ oldFrame = SelectObject(frameDc, frameBitmap);

    HDC captureDc = CreateCompatibleDC(dc);
    if (!captureDc) {
        SelectObject(frameDc, oldFrame);
        DeleteObject(frameBitmap);
        DeleteDC(frameDc);
        EndPaint(hwnd_, &ps);
        return;
    }
    const HGDIOBJ oldCapture = SelectObject(captureDc, capture_);
    BitBlt(frameDc, 0, 0, screen_.width, screen_.height, captureDc, 0, 0, SRCCOPY);
    SelectObject(captureDc, oldCapture);
    DeleteDC(captureDc);

    if (selected_) {
        PaintSelection(frameDc);
        PaintPreview(frameDc);
        PaintToolbar(frameDc);
        PaintStyleBar(frameDc);
        PaintTooltip(frameDc);
    } else {
        {
            Gdiplus::Graphics graphics(frameDc);
            Gdiplus::SolidBrush dimBrush(Gdiplus::Color(kDimIdle, 0, 0, 0));
            const auto fillDim = [&](LONG x, LONG y, LONG width, LONG height) {
                if (width > 0 && height > 0) {
                    graphics.FillRectangle(&dimBrush, static_cast<INT>(x), static_cast<INT>(y), static_cast<INT>(width), static_cast<INT>(height));
                }
            };
            if (hasHover_) {
                const RECT h = hoverRect_;
                fillDim(0, 0, screen_.width, h.top);
                fillDim(0, h.bottom, screen_.width, screen_.height - h.bottom);
                fillDim(0, h.top, h.left, h.bottom - h.top);
                fillDim(h.right, h.top, screen_.width - h.right, h.bottom - h.top);
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeNone);
                Gdiplus::Pen frame(Accent(), 1.0f);
                graphics.DrawRectangle(&frame, static_cast<float>(h.left), static_cast<float>(h.top), static_cast<float>(h.right - h.left) - 1.0f, static_cast<float>(h.bottom - h.top) - 1.0f);
            } else {
                fillDim(0, 0, screen_.width, screen_.height);
            }
        }
        POINT cursor{};
        GetCursorPos(&cursor);
        POINT client{cursor.x - screen_.x, cursor.y - screen_.y};
        PaintMagnifier(frameDc, client);
        PaintHint(frameDc);
    }

    BitBlt(dc, 0, 0, screen_.width, screen_.height, frameDc, 0, 0, SRCCOPY);
    SelectObject(frameDc, oldFrame);
    DeleteObject(frameBitmap);
    DeleteDC(frameDc);
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
    hoverToolbar_ = -1;
    hoverStyle_ = -1;
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
        OffsetRect(&next, point.x - dragStart_.x, point.y - dragStart_.y);
        selection_ = next;
        ClampSelection();
        return;
    }

    switch (dragMode_) {
    case DragMode::ResizeTopLeft: next.left = point.x; next.top = point.y; break;
    case DragMode::ResizeTop: next.top = point.y; break;
    case DragMode::ResizeTopRight: next.right = point.x; next.top = point.y; break;
    case DragMode::ResizeRight: next.right = point.x; break;
    case DragMode::ResizeBottomRight: next.right = point.x; next.bottom = point.y; break;
    case DragMode::ResizeBottom: next.bottom = point.y; break;
    case DragMode::ResizeBottomLeft: next.left = point.x; next.bottom = point.y; break;
    case DragMode::ResizeLeft: next.left = point.x; break;
    default: break;
    }
    selection_ = next;
    ClampSelection();
}

void SnipWindow::FinishSelectionDrag(POINT point) {
    if (!dragging_) return;
    UpdateSelectionDrag(point);
    dragging_ = false;
    ReleaseCapture();

    RECT r = NormalizedSelection();
    if (dragMode_ == DragMode::NewSelection &&
        r.right - r.left < 4 && r.bottom - r.top < 4 &&
        pressedHoverRect_.right > pressedHoverRect_.left && pressedHoverRect_.bottom > pressedHoverRect_.top) {
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
    return selected_ ? CloneBitmapRegion(capture_, NormalizedSelection()) : nullptr;
}

void SnipWindow::RestoreSelection(HBITMAP snapshot) {
    if (!snapshot || !selected_) return;
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
    if (undo_.empty()) return;
    if (HBITMAP current = SnapshotSelection()) redo_.push_back(current);
    HBITMAP previous = undo_.back();
    undo_.pop_back();
    RestoreSelection(previous);
    DeleteObject(previous);
    edited_ = !undo_.empty() || !redo_.empty();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SnipWindow::Redo() {
    CommitTextEdit();
    if (redo_.empty()) return;
    if (HBITMAP current = SnapshotSelection()) undo_.push_back(current);
    HBITMAP next = redo_.back();
    redo_.pop_back();
    RestoreSelection(next);
    DeleteObject(next);
    edited_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SnipWindow::DrawShape(HDC dc, Tool tool, POINT from, POINT to) {
    HPEN pen = CreatePen(PS_SOLID, strokeWidth_, annotationColor_);
    const HGDIOBJ oldPen = SelectObject(dc, pen);
    const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));

    if (tool == Tool::Rectangle) {
        const RECT rect = NormalizeRect(from, to);
        Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
    } else if (tool == Tool::Ellipse) {
        const RECT rect = NormalizeRect(from, to);
        ::Ellipse(dc, rect.left, rect.top, rect.right, rect.bottom);
    } else if (tool == Tool::Arrow) {
        MoveToEx(dc, from.x, from.y, nullptr);
        LineTo(dc, to.x, to.y);
        const double angle = std::atan2(static_cast<double>(to.y - from.y), static_cast<double>(to.x - from.x));
        constexpr double spread = 0.55;
        const double length = 12.0 + strokeWidth_ * 1.8;
        POINT arrow[3] = {
            to,
            {static_cast<LONG>(to.x - length * std::cos(angle - spread)), static_cast<LONG>(to.y - length * std::sin(angle - spread))},
            {static_cast<LONG>(to.x - length * std::cos(angle + spread)), static_cast<LONG>(to.y - length * std::sin(angle + spread))},
        };
        HBRUSH brush = CreateSolidBrush(annotationColor_);
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
    HPEN pen = CreatePen(PS_SOLID, strokeWidth_, annotationColor_);
    const HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, from.x, from.y, nullptr);
    LineTo(dc, to.x, to.y);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBitmap);
    DeleteObject(pen);
    DeleteDC(dc);
}

void SnipWindow::ApplyMosaic(POINT point) {
    const int block = mosaicBlock_;
    const RECT r = NormalizedSelection();
    const int px = static_cast<int>(point.x);
    const int py = static_cast<int>(point.y);
    const int left = std::clamp((px / block) * block, static_cast<int>(r.left), std::max(static_cast<int>(r.left), static_cast<int>(r.right) - 1));
    const int top = std::clamp((py / block) * block, static_cast<int>(r.top), std::max(static_cast<int>(r.top), static_cast<int>(r.bottom) - 1));
    const int right = std::min(static_cast<int>(r.right), left + block);
    const int bottom = std::min(static_cast<int>(r.bottom), top + block);

    HDC dc = CreateCompatibleDC(nullptr);
    const HGDIOBJ oldBitmap = SelectObject(dc, capture_);
    const COLORREF color = GetPixel(dc, std::min(static_cast<int>(r.right) - 1, left + block / 2), std::min(static_cast<int>(r.bottom) - 1, top + block / 2));
    HBRUSH brush = CreateSolidBrush(color == CLR_INVALID ? RGB(128, 128, 128) : color);
    RECT blockRect{left, top, right, bottom};
    FillRect(dc, &blockRect, brush);
    DeleteObject(brush);
    SelectObject(dc, oldBitmap);
    DeleteDC(dc);
}

void SnipWindow::BeginTextEdit(POINT point) {
    CommitTextEdit();
    if (!selected_) return;

    BeginEdit();
    textOrigin_ = ClampPoint(point, NormalizedSelection());
    const RECT r = NormalizedSelection();
    const int width = std::max(100, std::min(320, static_cast<int>(r.right - textOrigin_.x)));
    const int height = fontSize_ + 14;

    textEdit_ = CreateWindowExW(
        WS_EX_TOPMOST,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        textOrigin_.x,
        textOrigin_.y,
        width,
        height,
        hwnd_,
        nullptr,
        instance_,
        nullptr);
    if (!textEdit_) return;

    if (!textFont_) RecreateTextFont();
    if (textFont_) SendMessageW(textEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(textFont_), TRUE);
    SetFocus(textEdit_);
}

void SnipWindow::CommitTextEdit() {
    if (!textEdit_) return;

    const int length = GetWindowTextLengthW(textEdit_);
    std::wstring text(static_cast<size_t>(std::max(0, length)), L'\0');
    if (length > 0) GetWindowTextW(textEdit_, text.data(), length + 1);

    HWND edit = textEdit_;
    textEdit_ = nullptr;
    DestroyWindow(edit);

    if (!text.empty()) {
        HDC dc = CreateCompatibleDC(nullptr);
        const HGDIOBJ oldBitmap = SelectObject(dc, capture_);
        const HGDIOBJ oldFont = textFont_ ? SelectObject(dc, textFont_) : nullptr;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, annotationColor_);
        TextOutW(dc, textOrigin_.x, textOrigin_.y, text.c_str(), static_cast<int>(text.size()));
        if (oldFont) SelectObject(dc, oldFont);
        SelectObject(dc, oldBitmap);
        DeleteDC(dc);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SnipWindow::HandleStyleClick(int index) {
    if (index < 0) return;
    if (tool_ != Tool::Mosaic && index >= 0 && index < static_cast<int>(std::size(kAnnotationColors))) {
        annotationColor_ = kAnnotationColors[index];
    } else if (index >= 100 && index < 103) {
        const int option = index - 100;
        if (tool_ == Tool::Text) {
            fontSize_ = kFontSizes[option];
            RecreateTextFont();
        } else if (tool_ == Tool::Mosaic) {
            mosaicBlock_ = kMosaicBlocks[option];
        } else {
            strokeWidth_ = kStrokeWidths[option];
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SnipWindow::HandleToolbarClick(int index) {
    if (index < 0 || index >= kToolbarButtons) return;
    if (index != 5) CommitTextEdit();

    switch (index) {
    case 0: tool_ = tool_ == Tool::Rectangle ? Tool::None : Tool::Rectangle; break;
    case 1: tool_ = tool_ == Tool::Ellipse ? Tool::None : Tool::Ellipse; break;
    case 2: tool_ = tool_ == Tool::Arrow ? Tool::None : Tool::Arrow; break;
    case 3: tool_ = tool_ == Tool::Pen ? Tool::None : Tool::Pen; break;
    case 4: tool_ = tool_ == Tool::Mosaic ? Tool::None : Tool::Mosaic; break;
    case 5: tool_ = tool_ == Tool::Text ? Tool::None : Tool::Text; break;
    case 6: Undo(); return;
    case 7: Redo(); return;
    case 8: DestroyWindow(hwnd_); return;
    case 9: Finish(FinishAction::Pin); return;
    case 10: Finish(FinishAction::Save); return;
    case 11: Finish(FinishAction::Copy); return;
    default: return;
    }
    hoverStyle_ = -1;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SnipWindow::Finish(FinishAction action) {
    CommitTextEdit();
    if (!selected_) return;
    HBITMAP result = CloneBitmapRegion(capture_, NormalizedSelection());
    if (!result) return;
    auto callback = callback_;
    DestroyWindow(hwnd_);
    if (callback) callback(result, action);
    else DeleteObject(result);
}

void SnipWindow::CopyCurrentColor() {
    POINT cursor{};
    GetCursorPos(&cursor);
    const int x = cursor.x - screen_.x;
    const int y = cursor.y - screen_.y;
    const COLORREF color = ReadBitmapPixel(capture_, x, y);
    if (color == CLR_INVALID) return;
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
            if (tool_ == Tool::Pen) DrawPenSegment(drawCurrent_, point);
            else if (tool_ == Tool::Mosaic) ApplyMosaic(point);
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
            const int hoveredToolbar = HitToolbar(point);
            const int hoveredStyle = HitStyle(point);
            if (hoveredToolbar != hoverToolbar_ || hoveredStyle != hoverStyle_) {
                hoverToolbar_ = hoveredToolbar;
                hoverStyle_ = hoveredStyle;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            SetCursorForPoint(point);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int styleIndex = HitStyle(point);
        if (styleIndex >= 0) {
            HandleStyleClick(styleIndex);
            return 0;
        }
        const int toolbarIndex = HitToolbar(point);
        if (toolbarIndex >= 0) {
            HandleToolbarClick(toolbarIndex);
            return 0;
        }
        if (selected_) {
            const RECT bar = ToolbarRect();
            const RECT style = StyleBarRect();
            if (PtInRect(&bar, point) || (style.right > style.left && PtInRect(&style, point))) return 0;
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
            if (tool_ == Tool::Mosaic) ApplyMosaic(drawStart_);
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
        if (!edited_) BeginSelectionDrag(point);
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (drawing_) {
            point = ClampPoint(point, NormalizedSelection());
            drawCurrent_ = point;
            ReleaseCapture();
            if (tool_ == Tool::Rectangle || tool_ == Tool::Ellipse || tool_ == Tool::Arrow) {
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
        if (dragging_) FinishSelectionDrag(point);
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const RECT r = NormalizedSelection();
        if (selected_ && PtInRect(&r, point)) Finish(FinishAction::Copy);
        return 0;
    }

    case WM_RBUTTONDOWN:
        if (tool_ != Tool::None) {
            tool_ = Tool::None;
            hoverStyle_ = -1;
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
                hoverStyle_ = -1;
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
            if (wParam == 'O') tool_ = Tool::Ellipse;
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
