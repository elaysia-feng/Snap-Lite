#pragma once

#include <windows.h>

#include <functional>
#include <vector>

namespace snaplite {

class EditorWindow {
public:
    using DoneCallback = std::function<void(HBITMAP)>;

    enum class Tool {
        Rectangle,
        Ellipse,
        Arrow,
        Pen,
        Mosaic,
    };

    static bool Register(HINSTANCE instance);
    static bool Open(HINSTANCE instance, HBITMAP bitmap, DoneCallback callback);

private:
    EditorWindow(HINSTANCE instance, HBITMAP bitmap, DoneCallback callback);
    ~EditorWindow();

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void Paint();
    void PaintToolbar(HDC dc);
    void DrawPreview(HDC dc);
    void DrawShape(HDC dc, Tool tool, POINT from, POINT to);
    void DrawPenSegment(POINT from, POINT to);
    void ApplyMosaic(POINT point);
    void BeginEdit();
    void Undo();
    void Redo();
    void Finish();
    void HandleToolbarClick(int x);

    HINSTANCE instance_{};
    HWND hwnd_{};
    HBITMAP bitmap_{};
    int imageWidth_{};
    int imageHeight_{};
    int windowWidth_{};
    DoneCallback callback_;

    Tool tool_{Tool::Rectangle};
    bool drawing_{false};
    POINT drawStart_{};
    POINT drawCurrent_{};
    std::vector<HBITMAP> undo_;
    std::vector<HBITMAP> redo_;
};

}  // namespace snaplite
