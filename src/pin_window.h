#pragma once

#include <windows.h>

namespace snaplite {

class PinWindow {
public:
    static bool Register(HINSTANCE instance);
    static bool Create(HINSTANCE instance, HBITMAP bitmap);
    static bool CreateFromClipboard(HINSTANCE instance);

private:
    PinWindow(HINSTANCE instance, HBITMAP bitmap);
    ~PinWindow();

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void ResizeForZoom();
    void AdjustZoom(int wheelDelta);
    void AdjustOpacity(int wheelDelta);
    void SetOpacity(BYTE opacity);
    void ShowContextMenu();
    void CopyPin();
    void SavePinAs();

    HINSTANCE instance_{};
    HWND hwnd_{};
    HWND contextMenu_{};
    HBITMAP bitmap_{};
    int bitmapWidth_{};
    int bitmapHeight_{};
    double zoom_{1.0};
    BYTE opacity_{255};
};

}  // namespace snaplite
