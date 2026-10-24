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

bool AdvancedArrow(HDC dc, POINT from, POINT to, const POINT* hit = nullptr, int tolerance = 10) {
    if (!dc) return false;

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
    if (hit) {
        Gdiplus::Pen hitPen(ink, std::max(width, static_cast<float>(tolerance * 2)));
        hitPen.SetLineJoin(Gdiplus::LineJoinRound);
        // 命中使用实际曲线和折线路径，箭头尖端另加宽容范围。
        return shaft.IsOutlineVisible(static_cast<INT>(hit->x), static_cast<INT>(hit->y), &hitPen) ||
            std::hypot(static_cast<double>(hit->x - to.x), static_cast<double>(hit->y - to.y)) <= headSize + tolerance / 2;
    }
    Gdiplus::Pen pen(ink, width);
    // 箭杆会延伸到箭头尖端；圆形端帽会越过尖端形成多余的小点。
    pen.SetStartCap(kind == 3 ? Gdiplus::LineCapFlat : Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapFlat);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    graphics.DrawPath(&pen, &shaft);

    DrawArrowHeadSmooth(graphics, end, tailForEnd, gAnnotationColor, headSize, width, filledHead);
    if (kind == 3) {
        DrawArrowHeadSmooth(graphics, start, end, gAnnotationColor, headSize, width, true);
    }
    return true;
}

void DrawAdvancedArrow(HDC dc, POINT from, POINT to) {
    AdvancedArrow(dc, from, to);
}

bool AdvancedShape(HDC dc, int left, int top, int right, int bottom, const POINT* hit = nullptr, int tolerance = 10) {
    if (!dc) return false;

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

    bool matched = false;
    const auto drawPath = [&](Gdiplus::GraphicsPath& path) {
        if (hit) {
            Gdiplus::Pen hitPen(ink, static_cast<float>(std::max(gStrokeWidth, tolerance * 2)));
            matched = (fillMode != 0 && path.IsVisible(static_cast<INT>(hit->x), static_cast<INT>(hit->y))) ||
                path.IsOutlineVisible(static_cast<INT>(hit->x), static_cast<INT>(hit->y), &hitPen);
            return;
        }
        if (fillMode != 0) graphics.FillPath(&brush, &path);
        if (fillMode != 1) graphics.DrawPath(&pen, &path);
    };

    switch (kind) {
    case 0: {
        Gdiplus::GraphicsPath path;
        path.AddRectangle(bounds);
        drawPath(path);
        break;
    }
    case 1: {
        Gdiplus::GraphicsPath path;
        AddAnnotationRoundRect(path, bounds, 9.0f);
        drawPath(path);
        break;
    }
    case 2: {
        const float side = std::min(width, height);
        const Gdiplus::RectF circle(cx - side / 2.0f, cy - side / 2.0f, side, side);
        Gdiplus::GraphicsPath path;
        path.AddEllipse(circle);
        drawPath(path);
        break;
    }
    case 3: {
        Gdiplus::GraphicsPath path;
        path.AddEllipse(bounds);
        drawPath(path);
        break;
    }
    case 4: {
        Gdiplus::GraphicsPath path;
        path.AddLine(
            Gdiplus::PointF(static_cast<float>(normalizedLeft), static_cast<float>(normalizedTop)),
            Gdiplus::PointF(static_cast<float>(normalizedRight), static_cast<float>(normalizedBottom)));
        if (hit) drawPath(path);
        else graphics.DrawPath(&pen, &path);
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

    return hit ? matched : true;
}

BOOL ThemedRectangle(snaplite::SnipWindow& owner, HDC dc, int left, int top, int right, int bottom) {
    if (!owner.shapeDrawing_) return ::Rectangle(dc, left, top, right, bottom);
    owner.shapeDrawing_ = false;
    return AdvancedShape(dc, left, top, right, bottom);
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
        // GetRValue 之类的宏会做常量截断，用显式取字节避免 C4310 警告。
        const auto channel = [](COLORREF color, int index) -> BYTE {
            return static_cast<BYTE>((static_cast<DWORD>(color) >> (index * 8)) & 0xFF);
        };
        if (red == 255 && green == 197 && blue == 61) {
            return Pack(
                alpha,
                channel(snaplite::ui::kAccent, 0),
                channel(snaplite::ui::kAccent, 1),
                channel(snaplite::ui::kAccent, 2));
        }
        if (red == 236 && green == 239 && blue == 244) return Pack(alpha, 245, 245, 247);
        if (red == 255 && green == 107 && blue == 91) {
            return Pack(
                alpha,
                channel(snaplite::ui::kDanger, 0),
                channel(snaplite::ui::kDanger, 1),
                channel(snaplite::ui::kDanger, 2));
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

void SnipWindow::PaintArrows(HDC dc) const {
    const int saved = SaveDC(dc);
    const RECT r = NormalizedSelection();
    IntersectClipRect(dc, r.left, r.top, r.right, r.bottom);
    const auto color = detail::gAnnotationColor;
    const int kind = detail::gArrowKind;
    const int width = detail::gStrokeWidth;
    const int shapeKind = detail::gShapeKind;
    const int fillMode = detail::gShapeFillMode;
    for (const auto& arrow : arrows_) {
        detail::gAnnotationColor = arrow.color;
        detail::gArrowKind = arrow.kind;
        detail::gStrokeWidth = arrow.width;
        if (arrow.tool == Tool::Arrow) {
            detail::DrawAdvancedArrow(dc, arrow.from, arrow.to);
        } else {
            detail::gShapeKind = arrow.kind;
            detail::gShapeFillMode = arrow.fillMode;
            detail::AdvancedShape(dc, arrow.from.x, arrow.from.y, arrow.to.x, arrow.to.y);
        }
    }
    detail::gAnnotationColor = color;
    detail::gArrowKind = kind;
    detail::gStrokeWidth = width;
    detail::gShapeKind = shapeKind;
    detail::gShapeFillMode = fillMode;
    RestoreDC(dc, saved);
}

int SnipWindow::HitArrow(POINT point, int* part) const {
    const RECT r = NormalizedSelection();
    if (!selected_ || !PtInRect(&r, point)) return -1;
    const int radius = std::max(8, MulDiv(10, static_cast<int>(GetDpiForWindow(hwnd_)), 96));
    HDC dc = GetDC(hwnd_);
    if (!dc) return -1;
    const int kind = detail::gArrowKind;
    const int width = detail::gStrokeWidth;
    const int shapeKind = detail::gShapeKind;
    const int fillMode = detail::gShapeFillMode;
    int found = -1;
    for (int i = static_cast<int>(arrows_.size()) - 1; i >= 0; --i) {
        const auto& arrow = arrows_[i];
        int hitPart = 0;
        const bool handles = arrow.tool == Tool::Arrow || selectedArrow_ == i;
        if (handles && Near(point, arrow.to, radius)) hitPart = 3;
        else if (handles && Near(point, arrow.from, radius)) hitPart = 2;
        else {
            detail::gArrowKind = arrow.kind;
            detail::gStrokeWidth = arrow.width;
            if (arrow.tool == Tool::Arrow) {
                if (detail::AdvancedArrow(dc, arrow.from, arrow.to, &point, radius)) hitPart = 1;
            } else {
                detail::gShapeKind = arrow.kind;
                detail::gShapeFillMode = arrow.fillMode;
                if (detail::AdvancedShape(dc, arrow.from.x, arrow.from.y, arrow.to.x, arrow.to.y, &point, radius)) hitPart = 1;
            }
        }
        if (hitPart) {
            found = i;
            if (part) *part = hitPart;
            break;
        }
    }
    detail::gArrowKind = kind;
    detail::gStrokeWidth = width;
    detail::gShapeKind = shapeKind;
    detail::gShapeFillMode = fillMode;
    ReleaseDC(hwnd_, dc);
    return found;
}

void SnipWindow::UpdateArrowDrag(POINT point) {
    if (!arrowDragPart_ || selectedArrow_ < 0) return;
    const RECT r = NormalizedSelection();
    ArrowAnnotation next = arrowDragOrigin_;
    LONG dx = point.x - arrowDragStart_.x;
    LONG dy = point.y - arrowDragStart_.y;
    if (arrowDragPart_ == 1) {
        if (GetKeyState(VK_SHIFT) & 0x8000) {
            if (std::abs(dx) >= std::abs(dy)) dy = 0;
            else dx = 0;
        }
        dx = std::clamp(dx, r.left - std::min(next.from.x, next.to.x), r.right - 1 - std::max(next.from.x, next.to.x));
        dy = std::clamp(dy, r.top - std::min(next.from.y, next.to.y), r.bottom - 1 - std::max(next.from.y, next.to.y));
        next.from.x += dx; next.from.y += dy;
        next.to.x += dx; next.to.y += dy;
    } else {
        POINT& endpoint = arrowDragPart_ == 2 ? next.from : next.to;
        endpoint = ClampPoint({endpoint.x + dx, endpoint.y + dy}, r);
    }
    const auto& current = arrows_[selectedArrow_];
    if (current.from.x == next.from.x && current.from.y == next.from.y &&
        current.to.x == next.to.x && current.to.y == next.to.y) return;
    arrowDragChanged_ = true;
    arrows_[selectedArrow_] = next;
    // 拖动中途就会改变合成图的像素，这里不走 BeginEdit，必须单独计数。
    ++contentRevision_;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SnipWindow::FinishArrowDrag(bool cancel) {
    if (!arrowDragPart_) return;
    arrowDragPart_ = 0;
    if (arrowDragChanged_ && selectedArrow_ >= 0) {
        const ArrowAnnotation moved = arrows_[selectedArrow_];
        arrows_[selectedArrow_] = arrowDragOrigin_;
        const bool changed = moved.from.x != arrowDragOrigin_.from.x || moved.from.y != arrowDragOrigin_.from.y ||
            moved.to.x != arrowDragOrigin_.to.x || moved.to.y != arrowDragOrigin_.to.y;
        // 取消拖动会把标注还原成拖动前的位置。合成图缓存键必须跟着变，
        // 否则用户取消拖动后读到的仍是"拖动到一半"的旧图。
        ++contentRevision_;
        // 松开时再提交历史，取消或拖回原位不会吞掉重做记录。
        if (!cancel && changed) {
            BeginEdit();
            arrows_[selectedArrow_] = moved;
        }
    }
    arrowDragChanged_ = false;
    if (GetCapture() == hwnd_) ReleaseCapture();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SnipWindow::EraseArrows(POINT from, POINT to) {
    // 几何标注是可编辑对象，橡皮触碰后移除整个对象；同一次笔画仍只占一步历史。
    const int steps = std::max(1, static_cast<int>(std::hypot(
        static_cast<double>(to.x - from.x), static_cast<double>(to.y - from.y)) / 4));
    for (int step = 0; step <= steps; ++step) {
        POINT point{from.x + (to.x - from.x) * step / steps, from.y + (to.y - from.y) * step / steps};
        for (int hit = HitArrow(point); hit >= 0; hit = HitArrow(point)) {
            arrows_.erase(arrows_.begin() + hit);
        }
    }
    selectedArrow_ = -1;
    // 橡皮会移除几何标注对象，合成图缓存键必须跟着变。
    ++contentRevision_;
}

bool SnipWindow::UiHasSelection() const {
    detail::gAnnotationColor = annotationColor_;
    detail::gTextSizePt = textSizePt_;
    switch (tool_) {
    case Tool::Rectangle:
        detail::gActiveToolIndex = static_cast<int>(UiToolIndex::Rectangle);
        break;
    case Tool::Arrow:
        detail::gActiveToolIndex = static_cast<int>(UiToolIndex::Arrow);
        break;
    case Tool::Pen:
        detail::gActiveToolIndex = static_cast<int>(UiToolIndex::Pen);
        break;
    case Tool::Mosaic:
        detail::gActiveToolIndex = static_cast<int>(UiToolIndex::Mosaic);
        break;
    case Tool::Text:
        detail::gActiveToolIndex = static_cast<int>(UiToolIndex::Text);
        break;
    default: detail::gActiveToolIndex = static_cast<int>(UiToolIndex::None); break;
    }
    return selected_;
}

RECT SnipWindow::UiSelectionRect() const { return selected_ ? NormalizedSelection() : RECT{}; }
RECT SnipWindow::UiLegacyToolbarRect() const { return ToolbarRect(); }

int SnipWindow::UiActiveTool() const {
    switch (tool_) {
    case Tool::Rectangle: return static_cast<int>(UiToolIndex::Rectangle);
    case Tool::Arrow: return static_cast<int>(UiToolIndex::Arrow);
    case Tool::Pen: return static_cast<int>(UiToolIndex::Pen);
    case Tool::Mosaic: return static_cast<int>(UiToolIndex::Mosaic);
    case Tool::Text: return static_cast<int>(UiToolIndex::Text);
    default: return static_cast<int>(UiToolIndex::None);
    }
}

void SnipWindow::UiSetTool(int toolIndex) {
    switch (static_cast<UiToolIndex>(toolIndex)) {
    case UiToolIndex::Rectangle: tool_ = Tool::Rectangle; break;
    case UiToolIndex::Arrow: tool_ = Tool::Arrow; break;
    case UiToolIndex::Pen: tool_ = Tool::Pen; break;
    case UiToolIndex::Mosaic: tool_ = Tool::Mosaic; break;
    case UiToolIndex::Text: tool_ = Tool::Text; break;
    default: tool_ = Tool::None; break;
    }
    if (selectedArrow_ >= 0 && tool_ != Tool::None && arrows_[selectedArrow_].tool != tool_) {
        selectedArrow_ = -1;
    }
    const bool isValidTool =
        toolIndex >= static_cast<int>(UiToolIndex::Rectangle) &&
        toolIndex <= static_cast<int>(UiToolIndex::Text);
    detail::gActiveToolIndex =
        isValidTool ? toolIndex : static_cast<int>(UiToolIndex::None);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int SnipWindow::UiShapeKind() const { return detail::gShapeKind; }
void SnipWindow::UiSetShapeKind(int kind) {
    detail::gShapeKind = std::clamp(kind, 0, 7);
    UiSetTool(static_cast<int>(UiToolIndex::Rectangle));
    UpdateSelectedAnnotationStyle();
}
int SnipWindow::UiShapeFillMode() const { return detail::gShapeFillMode; }
void SnipWindow::UiSetShapeFillMode(int mode) {
    detail::gShapeFillMode = std::clamp(mode, 0, 2);
    UpdateSelectedAnnotationStyle();
    InvalidateRect(hwnd_, nullptr, FALSE);
}
int SnipWindow::UiArrowKind() const { return detail::gArrowKind; }
void SnipWindow::UiSetArrowKind(int kind) {
    detail::gArrowKind = std::clamp(kind, 0, 6);
    UiSetTool(static_cast<int>(UiToolIndex::Arrow));
    UpdateSelectedAnnotationStyle();
}
int SnipWindow::UiStrokeWidth() const { return detail::gStrokeWidth; }
void SnipWindow::UiSetStrokeWidth(int width) {
    detail::gStrokeWidth = std::clamp(width, 1, 12);
    UpdateSelectedAnnotationStyle();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

COLORREF SnipWindow::UiColor() const { return annotationColor_; }
void SnipWindow::UiSetColor(COLORREF color) {
    annotationColor_ = color;
    detail::gAnnotationColor = color;
    UpdateSelectedAnnotationStyle();
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
bool SnipWindow::UiInteractionActive() const { return dragging_ || drawing_ || arrowDragPart_ != 0; }
bool SnipWindow::UiSelectionDragActive() const { return dragging_; }
bool SnipWindow::UiHitSelectionBorder(POINT point) const {
    const DragMode hit = HitSelection(point);
    return hit != DragMode::None && hit != DragMode::Move;
}
bool SnipWindow::UiHitArrow(POINT point) const { return HitArrow(point) >= 0; }
unsigned long long SnipWindow::UiEditRevision() const { return editRevision_; }
void SnipWindow::UiDeselectAnnotation() {
    selectedArrow_ = -1;
    InvalidateRect(hwnd_, nullptr, FALSE);
}
bool SnipWindow::UiHandleAnnotationKey(WPARAM key) {
    if (UiInteractionActive() ||
        (tool_ != Tool::None && tool_ != Tool::Arrow && tool_ != Tool::Rectangle) ||
        (GetKeyState(VK_MENU) & 0x8000)) return false;
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (key == VK_TAB && !control && !arrows_.empty()) {
        const int count = static_cast<int>(arrows_.size());
        const bool reverse = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        SelectAnnotation(selectedArrow_ < 0 ? (reverse ? count - 1 : 0) :
            (selectedArrow_ + (reverse ? count - 1 : 1)) % count);
        return true;
    }
    if (selectedArrow_ < 0 || selectedArrow_ >= static_cast<int>(arrows_.size())) return false;
    if (key == VK_ESCAPE && !control) {
        selectedArrow_ = -1;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    if (control && key == 'D') {
        ArrowAnnotation copy = arrows_[selectedArrow_];
        const RECT r = NormalizedSelection();
        const LONG dx = std::clamp(16L, r.left - std::min(copy.from.x, copy.to.x), r.right - 1 - std::max(copy.from.x, copy.to.x));
        const LONG dy = std::clamp(16L, r.top - std::min(copy.from.y, copy.to.y), r.bottom - 1 - std::max(copy.from.y, copy.to.y));
        copy.from.x += dx; copy.from.y += dy;
        copy.to.x += dx; copy.to.y += dy;
        BeginEdit();
        arrows_.push_back(copy);
        SelectAnnotation(static_cast<int>(arrows_.size()) - 1);
        return true;
    }
    if (control && (key == VK_HOME || key == VK_END || key == VK_PRIOR || key == VK_NEXT)) {
        const int last = static_cast<int>(arrows_.size()) - 1;
        const int target = key == VK_HOME ? last : key == VK_END ? 0 :
            std::clamp(selectedArrow_ + (key == VK_PRIOR ? 1 : -1), 0, last);
        if (target != selectedArrow_) {
            BeginEdit();
            const auto item = arrows_[selectedArrow_];
            arrows_.erase(arrows_.begin() + selectedArrow_);
            arrows_.insert(arrows_.begin() + target, item);
            SelectAnnotation(target);
        }
        return true;
    }
    if (control) return false;
    if (key == VK_DELETE) {
        BeginEdit();
        arrows_.erase(arrows_.begin() + selectedArrow_);
        selectedArrow_ = -1;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    if (key != VK_LEFT && key != VK_RIGHT && key != VK_UP && key != VK_DOWN) return false;
    const LONG step = (GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1;
    const RECT r = NormalizedSelection();
    ArrowAnnotation next = arrows_[selectedArrow_];
    LONG dx = key == VK_LEFT ? -step : key == VK_RIGHT ? step : 0;
    LONG dy = key == VK_UP ? -step : key == VK_DOWN ? step : 0;
    dx = std::clamp(dx, r.left - std::min(next.from.x, next.to.x), r.right - 1 - std::max(next.from.x, next.to.x));
    dy = std::clamp(dy, r.top - std::min(next.from.y, next.to.y), r.bottom - 1 - std::max(next.from.y, next.to.y));
    // 边缘处无实际移动时不增加历史，也不清空重做。
    if (dx || dy) {
        BeginEdit();
        next.from.x += dx; next.from.y += dy;
        next.to.x += dx; next.to.y += dy;
        arrows_[selectedArrow_] = next;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    return true;
}

void SnipWindow::SelectAnnotation(int index) {
    selectedArrow_ = index;
    const auto& item = arrows_[index];
    annotationColor_ = item.color;
    detail::gAnnotationColor = item.color;
    detail::gStrokeWidth = item.width;
    if (item.tool == Tool::Arrow) detail::gArrowKind = item.kind;
    else {
        detail::gShapeKind = item.kind;
        detail::gShapeFillMode = item.fillMode;
    }
    // 选择工具继续保留选择模式；绘图模式则显示当前对象对应的样式面板。
    if (tool_ != Tool::None) tool_ = item.tool;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SnipWindow::UpdateSelectedAnnotationStyle() {
    if (UiInteractionActive() || selectedArrow_ < 0) return;
    auto& item = arrows_[selectedArrow_];
    const int kind = item.tool == Tool::Arrow ? detail::gArrowKind : detail::gShapeKind;
    const int fill = item.tool == Tool::Arrow ? 0 : detail::gShapeFillMode;
    if (item.color == annotationColor_ && item.width == detail::gStrokeWidth &&
        item.kind == kind && item.fillMode == fill) return;
    BeginEdit();
    item.color = annotationColor_;
    item.width = detail::gStrokeWidth;
    item.kind = kind;
    item.fillMode = fill;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

HBITMAP SnipWindow::UiCaptureBitmap() const {
    if (arrows_.empty()) {
        if (arrowComposite_) { DeleteObject(arrowComposite_); arrowComposite_ = nullptr; }
        return capture_;
    }

    // 合成图（capture_ + 箭头）缓存：文字标注每敲一个键都会重绘画布背景，
    // OCR / 复制 / 保存也会取它。每次都整屏拷贝（4K 约 33MB）代价很高。
    // 缓存键覆盖了所有会改变像素的路径：
    //   editRevision_    —— 所有 BeginEdit 驱动的改动（画笔/马赛克/橡皮/新增或
    //                       移动几何标注/改样式）
    //   contentRevision_ —— 不经过 BeginEdit 的改动：撤销/重做、拖动中途、
    //                       拖动取消还原、逐段写入的画笔/马赛克笔画、橡皮擦除
    //   selection        —— 箭头按选区裁剪，选区变了合成结果就不同
    //
    // 另有一道兜底：笔画或拖动进行中一律不复用缓存。绘制过程中的像素是
    // "半成品"，任何在绘制期间重建的合成图都不该被当成最终结果缓存下来。
    const RECT selection = NormalizedSelection();
    const bool interactionInProgress = drawing_ || arrowDragPart_ != 0;
    const bool cacheValid =
        !interactionInProgress &&
        arrowComposite_ != nullptr &&
        compositeEditRevision_ == editRevision_ &&
        compositeContentRevision_ == contentRevision_ &&
        compositeSelection_.left == selection.left &&
        compositeSelection_.top == selection.top &&
        compositeSelection_.right == selection.right &&
        compositeSelection_.bottom == selection.bottom;
    if (cacheValid) {
        return arrowComposite_;
    }

    // 复用整屏位图，但每次刷新像素，避免拖动或撤销后显示旧图。
    if (!arrowComposite_) arrowComposite_ = CloneBitmap(capture_);
    HDC dc = arrowComposite_ ? CreateCompatibleDC(nullptr) : nullptr;
    if (!dc) return capture_;
    HDC source = CreateCompatibleDC(nullptr);
    if (!source) { DeleteDC(dc); return capture_; }
    const HGDIOBJ old = SelectObject(dc, arrowComposite_);
    const HGDIOBJ oldSource = SelectObject(source, capture_);
    BitBlt(dc, 0, 0, screen_.width, screen_.height, source, 0, 0, SRCCOPY);
    PaintArrows(dc);
    SelectObject(source, oldSource);
    DeleteDC(source);
    SelectObject(dc, old);
    DeleteDC(dc);

    compositeEditRevision_ = editRevision_;
    compositeContentRevision_ = contentRevision_;
    compositeSelection_ = selection;
    return arrowComposite_;
}

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
