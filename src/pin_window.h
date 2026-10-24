#pragma once

#include <windows.h>

namespace snaplite {

// 贴图结果。必须区分"剪贴板被占用"和"剪贴板里没有图片"：两者的处理方式
// 完全不同，混成一句话会让用户以为是自己没复制好图片。
enum class PinClipboardResult {
    Ok,            // 贴图窗口已创建
    Dismissed,     // 同一份剪贴板内容刚被贴过并关闭过，本次调用被有意忽略
    NoImage,       // 剪贴板里没有可用的图片
    ClipboardBusy, // 剪贴板正被别的进程占用，稍后重试即可
    Failed,        // 其它失败（贴图窗口创建失败等）
};

class PinWindow {
public:
    static bool Register(HINSTANCE instance);
    static bool Create(HINSTANCE instance, HBITMAP bitmap);

    // 从剪贴板贴图。返回细分的结果，调用方据此给出准确提示，
    // 否则用户会以为程序没有反应。
    static PinClipboardResult CreateFromClipboard(HINSTANCE instance);

private:
    PinWindow(HINSTANCE instance, HBITMAP bitmap);
    ~PinWindow();

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void ResizeForZoom();
    void RebuildScaledBitmap(int width, int height);
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
    // 按当前窗口尺寸预先缩放好的位图。贴图尺寸不变时重绘直接复用，
    // 不用每次都把整幅原图重新拉伸一遍。
    HBITMAP scaledBitmap_{};
    int scaledWidth_{0};
    int scaledHeight_{0};
    int bitmapWidth_{};
    int bitmapHeight_{};
    double zoom_{1.0};
    BYTE opacity_{255};
};

}  // namespace snaplite
