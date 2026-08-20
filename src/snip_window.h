#pragma once

#include <windows.h>

#include <functional>

#include "capture.h"

namespace snaplite {

class SnipWindow {
public:
    using CaptureCallback = std::function<void(HBITMAP)>;

    static bool Register(HINSTANCE instance);
    static bool Start(HINSTANCE instance, HWND owner, CaptureCallback callback);

private:
    SnipWindow(HINSTANCE instance, HWND owner, CaptureCallback callback);
    ~SnipWindow();

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void UpdateHover();
    void Paint();
    void PaintMagnifier(HDC dc, POINT clientPoint);
    void FinishSelection(const RECT& selection);
    void CopyCurrentColor();
    RECT CurrentFocusRect() const;

    HINSTANCE instance_{};
    HWND owner_{};
    HWND hwnd_{};
    HBITMAP capture_{};
    VirtualScreen screen_{};
    CaptureCallback callback_;

    bool dragging_{false};
    bool hasHover_{false};
    POINT dragStart_{};
    POINT dragCurrent_{};
    RECT hoverRect_{};
    RECT pressedHoverRect_{};
};

}  // namespace snaplite
