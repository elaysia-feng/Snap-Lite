#pragma once

#include <windows.h>

#include <functional>
#include <vector>

#include "capture.h"

namespace snaplite {

class SnipWindow {
public:
    enum class FinishAction {
        Copy,
        Save,
        Pin,
    };

    using CaptureCallback = std::function<void(HBITMAP, FinishAction, const RECT&)>;

    static bool Register(HINSTANCE instance);
    static bool Start(HINSTANCE instance, HWND owner, CaptureCallback callback);

private:
    enum class Tool {
        None,
        Rectangle,
        Ellipse,
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
    void RecreateTextFont();
    void Paint();
    void PaintMagnifier(HDC dc, POINT clientPoint);
    void PaintSelection(HDC dc);
    void PaintHint(HDC dc);
    void PaintToolbar(HDC dc);
    void PaintToolbarIcon(HDC dc, int index, const RECT& rect, bool active, bool hovered);
    void PaintStyleBar(HDC dc);
    void PaintTooltip(HDC dc);
    void PaintPreview(HDC dc);

    RECT ToolbarRect() const;
    RECT ToolbarButtonRect(const RECT& bar, int index) const;
    RECT StyleBarRect() const;
    RECT NormalizedSelection() const;
    DragMode HitSelection(POINT point) const;
    int HitToolbar(POINT point) const;
    int HitStyle(POINT point) const;
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

    void BeginTextEdit(POINT point);
    void CommitTextEdit();

    void HandleToolbarClick(int index);
    void HandleStyleClick(int index);
    void Finish(FinishAction action);
    void CopyCurrentColor();

    HINSTANCE instance_{};
    HWND owner_{};
    HWND hwnd_{};
    HBITMAP capture_{};
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
    std::vector<HBITMAP> undo_;
    std::vector<HBITMAP> redo_;

    int hoverToolbar_{-1};
    int hoverStyle_{-1};
    COLORREF annotationColor_{RGB(235, 70, 70)};
    int strokeWidth_{3};
    int fontSize_{20};
    int mosaicBlock_{12};

    HWND textEdit_{};
    POINT textOrigin_{};
    HFONT textFont_{};
    HFONT uiFont_{};
    HFONT monoFont_{};
};

}  // namespace snaplite
