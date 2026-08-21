#pragma once

#include <windows.h>
#include <shellapi.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

namespace snaplite {

class App {
public:
    explicit App(HINSTANCE instance);
    ~App();

    int Run(int showCommand);

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool Initialize();
    void Shutdown();
    void AddTrayIcon();
    void ShowTrayMenu();
    void ShowNotice(const wchar_t* message);
    void StartSnip();
    void CaptureFullscreen();
    void PinClipboard();
    void CommitCapture(HBITMAP bitmap);

    HINSTANCE instance_{};
    HWND hwnd_{};
    NOTIFYICONDATAW tray_{};
    ULONG_PTR gdiplusToken_{};
    HICON appIcon_{};
    HANDLE singleInstance_{};
};

}  // namespace snaplite
