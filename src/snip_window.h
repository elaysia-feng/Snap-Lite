#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "capture.h"

namespace snaplite {

class SnipWindow;

namespace detail {

// Forward declarations for the themed GDI shims so SnipWindow can friend
// them and grant access to the per-instance shape drawing state. The actual
// implementations live in snip_window.cpp.
HPEN ThemedCreatePen(SnipWindow& owner, int style, int width, COLORREF color);
BOOL ThemedRectangle(SnipWindow& owner, HDC dc, int left, int top, int right, int bottom);
BOOL ThemedMoveToEx(SnipWindow& owner, HDC dc, int x, int y, LPPOINT oldPoint);
BOOL ThemedLineTo(SnipWindow& owner, HDC dc, int x, int y);
BOOL ThemedPolygon(SnipWindow& owner, HDC dc, const POINT* points, int count);

}  // namespace detail

// Stable, macro-immune annotation red. RGB() is hijacked inside snip_window.cpp
// during the .inc include, but this helper always produces the same COLORREF
// regardless of any macro state. Use it for baked-text defaults and other
// cross-TU color constants so the value doesn't drift when annotation themes
// change.
inline constexpr COLORREF kDefaultAnnotationRed() {
    // (r=235, g=70, b=70) packed as COLORREF (0x00BBGGRR).
    return static_cast<COLORREF>((70u << 16) | (70u << 8) | 235u);
}

struct TextOverlay {
    std::wstring text;
    POINT origin{};
    COLORREF color{kDefaultAnnotationRed()};
    int sizePt{16};
};

class BitmapHistory : public std::vector<HBITMAP> {
public:
    void push_back(HBITMAP bitmap) {
        std::vector<HBITMAP>::push_back(bitmap);
        Trim();
    }

private:
    static SIZE_T BitmapBytes(HBITMAP bitmap) {
        if (!bitmap) {
            return 0;
        }

        BITMAP info{};
        if (GetObjectW(bitmap, sizeof(info), &info) == 0) {
            return 0;
        }

        const SIZE_T stride = static_cast<SIZE_T>(
            info.bmWidthBytes >= 0 ? info.bmWidthBytes : -info.bmWidthBytes);
        const SIZE_T height = static_cast<SIZE_T>(
            info.bmHeight >= 0 ? info.bmHeight : -info.bmHeight);
        return stride * height;
    }

    SIZE_T TotalBytes() const {
        SIZE_T total = 0;
        for (HBITMAP bitmap : *this) {
            total += BitmapBytes(bitmap);
        }
        return total;
    }

    void Trim() {
        constexpr size_t kMaxEntries = 6;
        constexpr SIZE_T kMaxBytes = 32ull * 1024ull * 1024ull;

        while (size() > 1 && (size() > kMaxEntries || TotalBytes() > kMaxBytes)) {
            if (front()) {
                DeleteObject(front());
            }
            erase(begin());
        }
    }
};

class SnipWindow {
public:
    enum class FinishAction {
        Copy,
        Save,
        SaveAs,
        Pin,
    };

    using CaptureCallback = std::function<void(HBITMAP, FinishAction)>;

    static bool Register(HINSTANCE instance);
    static bool Start(HINSTANCE instance, HWND owner, CaptureCallback callback);

    bool UiHasSelection() const;
    RECT UiSelectionRect() const;
    RECT UiLegacyToolbarRect() const;
    int UiActiveTool() const;
    void UiSetTool(int toolIndex);

    int UiShapeKind() const;
    void UiSetShapeKind(int kind);
    int UiShapeFillMode() const;
    void UiSetShapeFillMode(int mode);
    int UiArrowKind() const;
    void UiSetArrowKind(int kind);
    int UiStrokeWidth() const;
    void UiSetStrokeWidth(int width);

    COLORREF UiColor() const;
    void UiSetColor(COLORREF color);
    int UiTextSize() const;
    void UiSetTextSize(int points);
    void UiUndo();
    void UiRedo();
    void UiBakeTextOverlays(const std::vector<TextOverlay>& overlays);
    void UiFinish(FinishAction action);
    void UiCancel();
    HWND UiHwnd() const;
    HBITMAP UiCaptureBitmap() const;

private:
    enum class Tool {
        None,
        Rectangle,
        Arrow,
        Pen,
        Mosaic,
        Text,
    };

    enum class DragMode {
        None,
        NewSelection,
        Move,
        ResizeTopLeft,
        ResizeTop,
        ResizeTopRight,
        ResizeRight,
        ResizeBottomRight,
        ResizeBottom,
        ResizeBottomLeft,
        ResizeLeft,
    };

    SnipWindow(HINSTANCE instance, HWND owner, CaptureCallback callback);
    ~SnipWindow();

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void UpdateHover();
    void EnsureFonts();
    void Paint();
    HDC AcquireFrameBuffer(HDC reference);
    void ReleaseFrameBuffer();
    void PaintMagnifier(HDC dc, POINT clientPoint);
    void PaintSelection(HDC dc);
    void PaintHint(HDC dc);
    void PaintToolbar(HDC dc);
    void PaintToolbarIcon(HDC dc, int index, const RECT& rect, bool active, bool hovered);
    void PaintPreview(HDC dc);

    RECT ToolbarRect() const;
    RECT ToolbarButtonRect(const RECT& bar, int index) const;
    RECT NormalizedSelection() const;
    DragMode HitSelection(POINT point) const;
    int HitToolbar(POINT point) const;
    void SetCursorForPoint(POINT point);

    void BeginSelectionDrag(POINT point);
    void UpdateSelectionDrag(POINT point);
    void FinishSelectionDrag(POINT point);
    void ClampSelection();
    void ClearHistory();

    void BeginEdit();
    void Undo();
    void Redo();
    HBITMAP SnapshotSelection() const;
    void RestoreSelection(HBITMAP snapshot);
    void DrawShape(HDC dc, Tool tool, POINT from, POINT to);
    void DrawPenSegment(POINT from, POINT to);
    void ApplyMosaic(POINT point);

    void HandleToolbarClick(int index);
    void Finish(FinishAction action);
    void CopyCurrentColor();

    // Friends: the themed GDI shims in snaplite::detail need access to the
    // per-instance shape drawing state (shapeDrawing_, arrowFrom_, arrowTo_)
    // because the macro hijack forwards a SnipWindow reference to them.
    friend HPEN detail::ThemedCreatePen(SnipWindow& owner, int style, int width, COLORREF color);
    friend BOOL detail::ThemedRectangle(SnipWindow& owner, HDC dc, int left, int top, int right, int bottom);
    friend BOOL detail::ThemedMoveToEx(SnipWindow& owner, HDC dc, int x, int y, LPPOINT oldPoint);
    friend BOOL detail::ThemedLineTo(SnipWindow& owner, HDC dc, int x, int y);
    friend BOOL detail::ThemedPolygon(SnipWindow& owner, HDC dc, const POINT* points, int count);

    HINSTANCE instance_{};
    HWND owner_{};
    HWND hwnd_{};
    HBITMAP capture_{};
    HDC frameDc_{};
    HBITMAP frameBitmap_{};
    HGDIOBJ frameOldBitmap_{};
    VirtualScreen screen_{};
    CaptureCallback callback_;

    bool selected_{false};
    bool hasHover_{false};
    bool edited_{false};
    bool dragging_{false};
    bool drawing_{false};
    RECT selection_{};
    RECT hoverRect_{};
    RECT pressedHoverRect_{};
    RECT dragOrigin_{};
    POINT dragStart_{};
    POINT dragCurrent_{};
    DragMode dragMode_{DragMode::None};

    Tool tool_{Tool::None};
    POINT drawStart_{};
    POINT drawCurrent_{};
    BitmapHistory undo_;
    BitmapHistory redo_;

    // Per-instance shape drawing state used by the themed GDI shims. These
    // were previously module-level globals in snaplite::detail, which made
    // them unsafe across multiple SnipWindow instances or toolbar leftovers.
    bool shapeDrawing_{false};
    POINT arrowFrom_{};
    POINT arrowTo_{};

    int hoverToolbar_{-1};

    COLORREF annotationColor_{RGB(235, 70, 70)};
    int textSizePt_{16};

    HFONT uiFont_{};
    HFONT monoFont_{};
};

}  // namespace snaplite
