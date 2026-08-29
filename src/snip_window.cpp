// Theme and editor-state wrapper for the original snip implementation.
// The capture/selection engine remains isolated in snip_window_original.inc.

#include "snip_window.h"
#include "ui_theme.h"

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
        if (width == 3 && gActiveToolIndex == 0) {
            owner.shapeDrawing_ = true;
        }
        if ((width == 3 && gActiveToolIndex == 0) ||
            (width == 4 && gActiveToolIndex == 2)) {
            width = std::clamp(gStrokeWidth, 1, 12);
        }
    }
    return ::CreatePen(style, width, color);
}

Gdiplus::Color AnnotationInk(COLORREF color, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

void PrepareAnnotationGraphics(Gdiplus::Graphics& graphics) {
    // 使用所有 Windows SDK 都支持的高质量抗锯齿模式，避免旧版 GDI+ 头文件缺少 8x8 枚举。
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
}

void AddAnnotationRoundRect(
    Gdiplus::GraphicsPath& path,
    const Gdiplus::RectF& rect,
    float radius) {
    path.Reset();
    const float limitedRadius = std::min(
        radius, std::min(rect.Width, rect.Height) / 2.0f);
    if (limitedRadius <= 0.0f) {
        path.AddRectangle(rect);
        path.CloseFigure();
        return;
    }

    const float diameter = limitedRadius * 2.0f;
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(
        rect.GetRight() - diameter,
        rect.GetBottom() - diameter,
        diameter,
        diameter,
        0.0f,
        90.0f);
    path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

void DrawArrowHeadSmooth(
    Gdiplus::Graphics& graphics,
    Gdiplus::PointF tip,
    Gdiplus::PointF tail,
    COLORREF color,
    float size,
    float strokeWidth,
    bool filled) {
    const double dx = static_cast<double>(tip.X - tail.X);
    const double dy = static_cast<double>(tip.Y - tail.Y);
    const double length = std::hypot(dx, dy);
    if (length < 0.01) return;

    const double angle = std::atan2(dy, dx);
    constexpr double spread = 0.55;
    const Gdiplus::PointF head[3] = {
        tip,
        {static_cast<float>(tip.X - size * std::cos(angle - spread)),
         static_cast<float>(tip.Y - size * std::sin(angle - spread))},
        {static_cast<float>(tip.X - size * std::cos(angle + spread)),
         static_cast<float>(tip.Y - size * std::sin(angle + spread))},
    };
    const Gdiplus::Color ink = AnnotationInk(color);

    if (filled) {
        Gdiplus::SolidBrush brush(ink);
        graphics.FillPolygon(&brush, head, 3);
        return;
    }

    Gdiplus::Pen pen(ink, std::max(1.0f, strokeWidth));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::GraphicsPath path;
    path.AddLine(head[1], head[0]);
    path.AddLine(head[0], head[2]);
    graphics.DrawPath(&pen, &path);
}

void DrawAdvancedArrow(HDC dc, POINT from, POINT to) {
    if (!dc) return;

    const int kind = std::clamp(gArrowKind, 0, 6);
    float width = static_cast<float>(std::clamp(gStrokeWidth, 1, 12));
    if (kind == 1) width = 1;
    if (kind == 2) width = std::max(width, 6.0f);

    Gdiplus::Graphics graphics(dc);
    PrepareAnnotationGraphics(graphics);
    // 半像素对齐可避免细斜线被吸附到不一致的整数边界，同时保留抗锯齿边缘。
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    const Gdiplus::PointF start(
        static_cast<float>(from.x), static_cast<float>(from.y));
    const Gdiplus::PointF end(
        static_cast<float>(to.x), static_cast<float>(to.y));
    Gdiplus::GraphicsPath shaft;
    Gdiplus::PointF tailForEnd = start;
    const bool filledHead = kind != 1;
    const float headSize = kind == 2 ? 22.0f : std::max(13.0f, 12.0f + width);

    switch (kind) {
    case 4: {
        const double dx = static_cast<double>(to.x - from.x);
        const double dy = static_cast<double>(to.y - from.y);
        const double len = std::max(
            1.0, std::hypot(dx, dy));
        const double nx = -dy / len;
        const double ny = dx / len;
        const double bend = std::min(70.0, len * 0.22);
        const Gdiplus::PointF control1(
            static_cast<float>(from.x + dx / 3.0 + nx * bend),
            static_cast<float>(from.y + dy / 3.0 + ny * bend));
        const Gdiplus::PointF control2(
            static_cast<float>(from.x + dx * 2.0 / 3.0 + nx * bend),
            static_cast<float>(from.y + dy * 2.0 / 3.0 + ny * bend));
        shaft.AddBezier(start, control1, control2, end);
        tailForEnd = control2;
        break;
    }
    case 5: {
        const float middleX = (static_cast<float>(from.x) + static_cast<float>(to.x)) / 2.0f;
        const Gdiplus::PointF points[4] = {
            start,
            {middleX, start.Y},
            {middleX, end.Y},
            end,
        };
        for (size_t i = 1; i < sizeof(points) / sizeof(points[0]); ++i) {
            shaft.AddLine(points[i - 1], points[i]);
        }
        tailForEnd = points[2];
        break;
    }
    case 6: {
        const float stepX = static_cast<float>(from.x) +
            static_cast<float>(to.x - from.x) / 3.0f;
        const Gdiplus::PointF points[4] = {
            start,
            {stepX, start.Y},
            {stepX, end.Y},
            end,
        };
        for (size_t i = 1; i < sizeof(points) / sizeof(points[0]); ++i) {
            shaft.AddLine(points[i - 1], points[i]);
        }
        tailForEnd = points[2];
        break;
    }
    default:
        shaft.AddLine(start, end);
        break;
    }

    const Gdiplus::Color ink = AnnotationInk(gAnnotationColor);
    Gdiplus::Pen pen(ink, width);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    graphics.DrawPath(&pen, &shaft);

    DrawArrowHeadSmooth(graphics, end, tailForEnd, gAnnotationColor, headSize, width, filledHead);
    if (kind == 3) {
        DrawArrowHeadSmooth(graphics, start, end, gAnnotationColor, headSize, width, true);
    }
}

BOOL ThemedRectangle(snaplite::SnipWindow& owner, HDC dc, int left, int top, int right, int bottom) {
    if (!owner.shapeDrawing_) {
        return ::Rectangle(dc, left, top, right, bottom);
    }

    owner.shapeDrawing_ = false;
    if (!dc) return FALSE;

    const int kind = std::clamp(gShapeKind, 0, 7);
    const int fillMode = std::clamp(gShapeFillMode, 0, 2);

    Gdiplus::Graphics graphics(dc);
    PrepareAnnotationGraphics(graphics);
    const Gdiplus::Color ink = AnnotationInk(gAnnotationColor);
    Gdiplus::Pen pen(ink, static_cast<float>(std::clamp(gStrokeWidth, 1, 12)));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::SolidBrush brush(ink);

    const int normalizedLeft = std::min(left, right);
    const int normalizedTop = std::min(top, bottom);
    const int normalizedRight = std::max(left, right);
    const int normalizedBottom = std::max(top, bottom);
    const float width = static_cast<float>(normalizedRight - normalizedLeft);
    const float height = static_cast<float>(normalizedBottom - normalizedTop);
    const float cx = (static_cast<float>(normalizedLeft) + normalizedRight) / 2.0f;
    const float cy = (static_cast<float>(normalizedTop) + normalizedBottom) / 2.0f;
    const Gdiplus::RectF bounds(
        static_cast<float>(normalizedLeft),
        static_cast<float>(normalizedTop),
        width,
        height);

    const auto drawPath = [&](Gdiplus::GraphicsPath& path) {
        if (fillMode != 0) graphics.FillPath(&brush, &path);
        if (fillMode != 1) graphics.DrawPath(&pen, &path);
    };

    switch (kind) {
    case 0:
        if (fillMode != 0) graphics.FillRectangle(&brush, bounds);
        if (fillMode != 1) graphics.DrawRectangle(&pen, bounds);
        break;
    case 1: {
        Gdiplus::GraphicsPath path;
        AddAnnotationRoundRect(path, bounds, 9.0f);
        drawPath(path);
        break;
    }
    case 2: {
        const float side = std::min(width, height);
        const Gdiplus::RectF circle(cx - side / 2.0f, cy - side / 2.0f, side, side);
        if (fillMode != 0) graphics.FillEllipse(&brush, circle);
        if (fillMode != 1) graphics.DrawEllipse(&pen, circle);
        break;
    }
    case 3:
        if (fillMode != 0) graphics.FillEllipse(&brush, bounds);
        if (fillMode != 1) graphics.DrawEllipse(&pen, bounds);
        break;
    case 4: {
        Gdiplus::GraphicsPath path;
        path.AddLine(
            Gdiplus::PointF(static_cast<float>(normalizedLeft), static_cast<float>(normalizedTop)),
            Gdiplus::PointF(static_cast<float>(normalizedRight), static_cast<float>(normalizedBottom)));
        if (fillMode != 1) graphics.DrawPath(&pen, &path);
        break;
    }
    case 5: {
        const Gdiplus::PointF points[3] = {
            {cx, static_cast<float>(normalizedTop)},
            {static_cast<float>(normalizedRight), static_cast<float>(normalizedBottom)},
            {static_cast<float>(normalizedLeft), static_cast<float>(normalizedBottom)},
        };
        Gdiplus::GraphicsPath path;
        path.AddPolygon(points, 3);
        drawPath(path);
        break;
    }
    case 6: {
        const Gdiplus::PointF points[4] = {
            {cx, static_cast<float>(normalizedTop)},
            {static_cast<float>(normalizedRight), cy},
            {cx, static_cast<float>(normalizedBottom)},
            {static_cast<float>(normalizedLeft), cy},
        };
        Gdiplus::GraphicsPath path;
        path.AddPolygon(points, 4);
        drawPath(path);
        break;
    }
    case 7: {
        const float quarter = std::max(1.0f, width / 4.0f);
        const Gdiplus::PointF points[6] = {
            {static_cast<float>(normalizedLeft) + quarter, static_cast<float>(normalizedTop)},
            {static_cast<float>(normalizedRight) - quarter, static_cast<float>(normalizedTop)},
            {static_cast<float>(normalizedRight), cy},
            {static_cast<float>(normalizedRight) - quarter, static_cast<float>(normalizedBottom)},
            {static_cast<float>(normalizedLeft) + quarter, static_cast<float>(normalizedBottom)},
            {static_cast<float>(normalizedLeft), cy},
        };
        Gdiplus::GraphicsPath path;
        path.AddPolygon(points, 6);
        drawPath(path);
        break;
    }
    }

    return TRUE;
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
        if (red == 255 && green == 197 && blue == 61) {
            return Pack(alpha, GetRValue(snaplite::ui::kAccent), GetGValue(snaplite::ui::kAccent), GetBValue(snaplite::ui::kAccent));
        }
        if (red == 236 && green == 239 && blue == 244) return Pack(alpha, 245, 245, 247);
        if (red == 255 && green == 107 && blue == 91) {
            return Pack(alpha, GetRValue(snaplite::ui::kDanger), GetGValue(snaplite::ui::kDanger), GetBValue(snaplite::ui::kDanger));
        }
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
#ifdef Color
#undef Color
#endif

#define CreateFontW snaplite::detail::ThemedCreateFontW
#define CreateWindowExW snaplite::detail::ThemedCreateWindowExW
#define CreatePen(...) snaplite::detail::ThemedCreatePen(*this, __VA_ARGS__)
#define Rectangle(...) snaplite::detail::ThemedRectangle(*this, __VA_ARGS__)
#define Color SnapLiteThemeColor
#include "snip_window_original.inc"
#undef Color
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
