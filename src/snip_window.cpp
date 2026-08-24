// Theme and editor-state wrapper for the original snip implementation.
// The capture/selection engine remains isolated in snip_window_original.inc.

#include "snip_window.h"

#include <windowsx.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <string>
#include <utility>

namespace snaplite::detail {

COLORREF gAnnotationColor = RGB(235, 70, 70);
int gTextSizePt = 14;
int gActiveToolIndex = -1;
int gShapeKind = 0;
int gShapeFillMode = 0;
int gArrowKind = 0;
int gStrokeWidth = 3;

HFONT ThemedCreateFontW(
    int height, int width, int escapement, int orientation, int weight,
    DWORD italic, DWORD underline, DWORD strikeOut, DWORD charSet,
    DWORD outPrecision, DWORD clipPrecision, DWORD quality,
    DWORD pitchAndFamily, LPCWSTR faceName) {
    if (height == -20 && faceName && _wcsicmp(faceName, L"Segoe UI") == 0) {
        const UINT dpi = GetDpiForSystem();
        height = -MulDiv(std::clamp(gTextSizePt, 10, 72), static_cast<int>(dpi), 72);
    }
    return ::CreateFontW(
        height, width, escapement, orientation, weight, italic, underline,
        strikeOut, charSet, outPrecision, clipPrecision, quality,
        pitchAndFamily, faceName);
}

HWND ThemedCreateWindowExW(
    DWORD exStyle, LPCWSTR className, LPCWSTR windowName, DWORD style,
    int x, int y, int width, int height, HWND parent, HMENU menu,
    HINSTANCE instance, LPVOID param) {
    if (reinterpret_cast<ULONG_PTR>(className) > 0xFFFF &&
        className && _wcsicmp(className, L"EDIT") == 0) {
        exStyle |= WS_EX_TRANSPARENT;
        style &= ~WS_BORDER;
    }
    return ::CreateWindowExW(
        exStyle, className, windowName, style, x, y, width, height,
        parent, menu, instance, param);
}

HPEN ThemedCreatePen(snaplite::SnipWindow& owner, int style, int width, COLORREF color) {
    if (color == gAnnotationColor) {
        if (width == 3 && (gActiveToolIndex == 0 || gActiveToolIndex == 1)) {
            owner.shapeDrawing_ = true;
        }
        if ((width == 3 && (gActiveToolIndex == 0 || gActiveToolIndex == 1)) ||
            (width == 4 && gActiveToolIndex == 2)) {
            width = std::clamp(gStrokeWidth, 1, 12);
        }
    }
    return ::CreatePen(style, width, color);
}

void DrawArrowHeadRaw(HDC dc, POINT tip, POINT tail, COLORREF color, int size, bool filled) {
    const double angle = std::atan2(
        static_cast<double>(tip.y - tail.y),
        static_cast<double>(tip.x - tail.x));
    constexpr double spread = 0.55;
    POINT head[3] = {
        tip,
        {static_cast<LONG>(tip.x - size * std::cos(angle - spread)),
         static_cast<LONG>(tip.y - size * std::sin(angle - spread))},
        {static_cast<LONG>(tip.x - size * std::cos(angle + spread)),
         static_cast<LONG>(tip.y - size * std::sin(angle + spread))},
    };

    if (filled) {
        HBRUSH brush = ::CreateSolidBrush(color);
        const HGDIOBJ oldBrush = ::SelectObject(dc, brush);
        ::Polygon(dc, head, 3);
        ::SelectObject(dc, oldBrush);
        ::DeleteObject(brush);
    } else {
        ::MoveToEx(dc, head[1].x, head[1].y, nullptr);
        ::LineTo(dc, tip.x, tip.y);
        ::LineTo(dc, head[2].x, head[2].y);
    }
}

void DrawAdvancedArrow(HDC dc, POINT from, POINT to) {
    const int kind = std::clamp(gArrowKind, 0, 6);
    int width = std::clamp(gStrokeWidth, 1, 12);
    if (kind == 1) width = 1;
    if (kind == 2) width = std::max(width, 6);

    HPEN pen = ::CreatePen(PS_SOLID, width, gAnnotationColor);
    const HGDIOBJ oldPen = ::SelectObject(dc, pen);
    const bool filledHead = kind != 1;
    const int headSize = kind == 2 ? 22 : std::max(13, 12 + width);
    POINT tailForEnd = from;

    switch (kind) {
    case 4: {
        const LONG dx = to.x - from.x;
        const LONG dy = to.y - from.y;
        const double len = std::max(
            1.0,
            std::hypot(static_cast<double>(dx), static_cast<double>(dy)));
        const double nx = -dy / len;
        const double ny = dx / len;
        const double bend = std::min(70.0, len * 0.22);
        POINT bezier[4] = {
            from,
            {static_cast<LONG>(from.x + dx / 3.0 + nx * bend),
             static_cast<LONG>(from.y + dy / 3.0 + ny * bend)},
            {static_cast<LONG>(from.x + dx * 2.0 / 3.0 + nx * bend),
             static_cast<LONG>(from.y + dy * 2.0 / 3.0 + ny * bend)},
            to,
        };
        ::PolyBezier(dc, bezier, 4);
        tailForEnd = bezier[2];
        break;
    }
    case 5: {
        POINT points[4] = {
            from,
            {(from.x + to.x) / 2, from.y},
            {(from.x + to.x) / 2, to.y},
            to,
        };
        ::Polyline(dc, points, 4);
        tailForEnd = points[2];
        break;
    }
    case 6: {
        const LONG dx = to.x - from.x;
        POINT points[4] = {
            from,
            {from.x + dx / 3, from.y},
            {from.x + dx / 3, to.y},
            to,
        };
        ::Polyline(dc, points, 4);
        tailForEnd = points[2];
        break;
    }
    default:
        ::MoveToEx(dc, from.x, from.y, nullptr);
        ::LineTo(dc, to.x, to.y);
        break;
    }

    DrawArrowHeadRaw(dc, to, tailForEnd, gAnnotationColor, headSize, filledHead);
    if (kind == 3) {
        DrawArrowHeadRaw(dc, from, to, gAnnotationColor, headSize, true);
    }

    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

BOOL ThemedRectangle(snaplite::SnipWindow& owner, HDC dc, int left, int top, int right, int bottom) {
    if (!owner.shapeDrawing_ || gActiveToolIndex != 0) {
        return ::Rectangle(dc, left, top, right, bottom);
    }

    owner.shapeDrawing_ = false;
    const int kind = std::clamp(gShapeKind, 0, 7);
    const int fillMode = std::clamp(gShapeFillMode, 0, 2);

    HPEN pen = fillMode == 1
        ? static_cast<HPEN>(::GetStockObject(NULL_PEN))
        : ::CreatePen(PS_SOLID, std::clamp(gStrokeWidth, 1, 12), gAnnotationColor);
    HBRUSH brush = fillMode == 0
        ? static_cast<HBRUSH>(::GetStockObject(NULL_BRUSH))
        : ::CreateSolidBrush(gAnnotationColor);

    const HGDIOBJ oldPen = ::SelectObject(dc, pen);
    const HGDIOBJ oldBrush = ::SelectObject(dc, brush);
    const int width = std::abs(right - left);
    const int height = std::abs(bottom - top);
    const int cx = (left + right) / 2;
    const int cy = (top + bottom) / 2;

    switch (kind) {
    case 0:
        ::Rectangle(dc, left, top, right, bottom);
        break;
    case 1:
        ::RoundRect(dc, left, top, right, bottom, 18, 18);
        break;
    case 2: {
        const int side = std::min(width, height);
        ::Ellipse(dc, cx - side / 2, cy - side / 2, cx + side / 2, cy + side / 2);
        break;
    }
    case 3:
        ::Ellipse(dc, left, top, right, bottom);
        break;
    case 4:
        ::MoveToEx(dc, left, top, nullptr);
        ::LineTo(dc, right, bottom);
        break;
    case 5: {
        POINT points[3] = {{cx, top}, {right, bottom}, {left, bottom}};
        ::Polygon(dc, points, 3);
        break;
    }
    case 6: {
        POINT points[4] = {{cx, top}, {right, cy}, {cx, bottom}, {left, cy}};
        ::Polygon(dc, points, 4);
        break;
    }
    case 7: {
        const int quarter = std::max(1, width / 4);
        POINT points[6] = {
            {left + quarter, top}, {right - quarter, top}, {right, cy},
            {right - quarter, bottom}, {left + quarter, bottom}, {left, cy},
        };
        ::Polygon(dc, points, 6);
        break;
    }
    }

    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    if (fillMode != 1) ::DeleteObject(pen);
    if (fillMode != 0) ::DeleteObject(brush);
    return TRUE;
}

BOOL ThemedMoveToEx(snaplite::SnipWindow& owner, HDC dc, int x, int y, LPPOINT oldPoint) {
    if (owner.shapeDrawing_ && gActiveToolIndex == 1) {
        owner.arrowFrom_ = {x, y};
        if (oldPoint) {
            oldPoint->x = x;
            oldPoint->y = y;
        }
        return TRUE;
    }
    return ::MoveToEx(dc, x, y, oldPoint);
}

BOOL ThemedLineTo(snaplite::SnipWindow& owner, HDC dc, int x, int y) {
    if (owner.shapeDrawing_ && gActiveToolIndex == 1) {
        owner.arrowTo_ = {x, y};
        return TRUE;
    }
    return ::LineTo(dc, x, y);
}

BOOL ThemedPolygon(snaplite::SnipWindow& owner, HDC dc, const POINT* points, int count) {
    if (owner.shapeDrawing_ && gActiveToolIndex == 1) {
        owner.shapeDrawing_ = false;
        DrawAdvancedArrow(dc, owner.arrowFrom_, owner.arrowTo_);
        return TRUE;
    }
    return ::Polygon(dc, points, count);
}

}  // namespace snaplite::detail

namespace Gdiplus {

class SnapLiteThemeColor : public Color {
public:
    SnapLiteThemeColor() : Color() {}
    explicit SnapLiteThemeColor(ARGB value) : Color(value) {}
    SnapLiteThemeColor(BYTE alpha, BYTE red, BYTE green, BYTE blue)
        : Color(ThemeArgb(alpha, red, green, blue)) {}
    SnapLiteThemeColor(BYTE red, BYTE green, BYTE blue)
        : Color(ThemeArgb(255, red, green, blue)) {}

private:
    static ARGB Pack(BYTE alpha, BYTE red, BYTE green, BYTE blue) {
        return (static_cast<ARGB>(alpha) << 24) |
               (static_cast<ARGB>(red) << 16) |
               (static_cast<ARGB>(green) << 8) |
               static_cast<ARGB>(blue);
    }

    static ARGB ThemeArgb(BYTE alpha, BYTE red, BYTE green, BYTE blue) {
        if (red == 27 && green == 30 && blue == 35) return Pack(alpha, 38, 38, 42);
        if (red == 255 && green == 197 && blue == 61) return Pack(alpha, 111, 146, 214);
        if (red == 236 && green == 239 && blue == 244) return Pack(alpha, 245, 245, 247);
        if (red == 255 && green == 107 && blue == 91) return Pack(alpha, 218, 92, 92);
        return Pack(alpha, red, green, blue);
    }
};

}  // namespace Gdiplus

#ifdef RGB
#undef RGB
#endif
#define SNAP_PACK_RGB(r, g, b) \
    ((COLORREF)(((BYTE)(r)) | ((WORD)((BYTE)(g)) << 8) | (((DWORD)(BYTE)(b)) << 16)))
#define RGB(r, g, b) \
    ((((r) == 235) && ((g) == 70) && ((b) == 70)) \
         ? snaplite::detail::gAnnotationColor \
         : SNAP_PACK_RGB((r), (g), (b)))

// NOTE: The Win32 API shims below hijack a handful of macros for the duration of
// the .inc file. This is a single-TU hack to keep the legacy capture/selection
// engine as a verbatim text inclusion. Every macro listed here is `#undef`-ed
// both before (defensive, in case some header pulled them in earlier) and
// after the include so the rest of this translation unit sees the real Win32
// API. Future refactor: split snip_window_original.inc into its own .cpp/.h
// pair and drop the macro trick entirely.
#ifdef CreateFontW
#undef CreateFontW
#endif
#ifdef CreateWindowExW
#undef CreateWindowExW
#endif
#ifdef CreatePen
#undef CreatePen
#endif
#ifdef Rectangle
#undef Rectangle
#endif
#ifdef MoveToEx
#undef MoveToEx
#endif
#ifdef LineTo
#undef LineTo
#endif
#ifdef Polygon
#undef Polygon
#endif
#ifdef Color
#undef Color
#endif

#define CreateFontW snaplite::detail::ThemedCreateFontW
#define CreateWindowExW snaplite::detail::ThemedCreateWindowExW
#define CreatePen(...) snaplite::detail::ThemedCreatePen(*this, __VA_ARGS__)
#define Rectangle(...) snaplite::detail::ThemedRectangle(*this, __VA_ARGS__)
#define MoveToEx(...) snaplite::detail::ThemedMoveToEx(*this, __VA_ARGS__)
#define LineTo(...) snaplite::detail::ThemedLineTo(*this, __VA_ARGS__)
#define Polygon(...) snaplite::detail::ThemedPolygon(*this, __VA_ARGS__)
#define Color SnapLiteThemeColor
#include "snip_window_original.inc"
#undef Color
#undef Polygon
#undef LineTo
#undef MoveToEx
#undef Rectangle
#undef CreatePen
#undef CreateWindowExW
#undef CreateFontW
#undef RGB
#undef SNAP_PACK_RGB

namespace snaplite {

bool SnipWindow::UiHasSelection() const {
    detail::gAnnotationColor = annotationColor_;
    detail::gTextSizePt = textSizePt_;
    switch (tool_) {
    case Tool::Rectangle: detail::gActiveToolIndex = 0; break;
    case Tool::Arrow: detail::gActiveToolIndex = 1; break;
    case Tool::Pen: detail::gActiveToolIndex = 2; break;
    case Tool::Mosaic: detail::gActiveToolIndex = 3; break;
    case Tool::Text: detail::gActiveToolIndex = 4; break;
    default: detail::gActiveToolIndex = -1; break;
    }
    return selected_;
}

RECT SnipWindow::UiSelectionRect() const { return selected_ ? NormalizedSelection() : RECT{}; }
RECT SnipWindow::UiLegacyToolbarRect() const { return ToolbarRect(); }

int SnipWindow::UiActiveTool() const {
    switch (tool_) {
    case Tool::Rectangle: return 0;
    case Tool::Arrow: return 1;
    case Tool::Pen: return 2;
    case Tool::Mosaic: return 3;
    case Tool::Text: return 4;
    default: return -1;
    }
}

void SnipWindow::UiSetTool(int toolIndex) {
    switch (toolIndex) {
    case 0: tool_ = Tool::Rectangle; break;
    case 1: tool_ = Tool::Arrow; break;
    case 2: tool_ = Tool::Pen; break;
    case 3: tool_ = Tool::Mosaic; break;
    case 4: tool_ = Tool::Text; break;
    default: tool_ = Tool::None; break;
    }
    detail::gActiveToolIndex = toolIndex >= 0 && toolIndex <= 4 ? toolIndex : -1;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int SnipWindow::UiShapeKind() const { return detail::gShapeKind; }
void SnipWindow::UiSetShapeKind(int kind) {
    detail::gShapeKind = std::clamp(kind, 0, 7);
    UiSetTool(0);
}
int SnipWindow::UiShapeFillMode() const { return detail::gShapeFillMode; }
void SnipWindow::UiSetShapeFillMode(int mode) {
    detail::gShapeFillMode = std::clamp(mode, 0, 2);
    InvalidateRect(hwnd_, nullptr, FALSE);
}
int SnipWindow::UiArrowKind() const { return detail::gArrowKind; }
void SnipWindow::UiSetArrowKind(int kind) {
    detail::gArrowKind = std::clamp(kind, 0, 6);
    UiSetTool(1);
}
int SnipWindow::UiStrokeWidth() const { return detail::gStrokeWidth; }
void SnipWindow::UiSetStrokeWidth(int width) {
    detail::gStrokeWidth = std::clamp(width, 1, 12);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

COLORREF SnipWindow::UiColor() const { return annotationColor_; }
void SnipWindow::UiSetColor(COLORREF color) {
    annotationColor_ = color;
    detail::gAnnotationColor = color;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int SnipWindow::UiTextSize() const { return textSizePt_; }
void SnipWindow::UiSetTextSize(int points) {
    textSizePt_ = std::clamp(points, 10, 72);
    detail::gTextSizePt = textSizePt_;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SnipWindow::UiUndo() { Undo(); }
void SnipWindow::UiRedo() { Redo(); }
void SnipWindow::UiFinish(FinishAction action) { Finish(action); }
void SnipWindow::UiCancel() { if (hwnd_) DestroyWindow(hwnd_); }
HWND SnipWindow::UiHwnd() const { return hwnd_; }
HBITMAP SnipWindow::UiCaptureBitmap() const { return capture_; }

HDC SnipWindow::AcquireFrameBuffer(HDC reference) {
    if (frameDc_ && frameBitmap_) {
        return frameDc_;
    }
    ReleaseFrameBuffer();
    frameDc_ = CreateCompatibleDC(reference);
    frameBitmap_ = frameDc_ ? CreateCompatibleBitmap(reference, screen_.width, screen_.height) : nullptr;
    if (!frameDc_ || !frameBitmap_) {
        ReleaseFrameBuffer();
        return nullptr;
    }
    frameOldBitmap_ = SelectObject(frameDc_, frameBitmap_);
    return frameDc_;
}

void SnipWindow::ReleaseFrameBuffer() {
    if (frameDc_ && frameOldBitmap_) {
        SelectObject(frameDc_, frameOldBitmap_);
    }
    frameOldBitmap_ = nullptr;
    if (frameBitmap_) {
        DeleteObject(frameBitmap_);
        frameBitmap_ = nullptr;
    }
    if (frameDc_) {
        DeleteDC(frameDc_);
        frameDc_ = nullptr;
    }
}

}  // namespace snaplite
