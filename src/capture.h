#pragma once

#include <windows.h>
#include <objidl.h>
#include <propidl.h>

#include <filesystem>
#include <string>
#include <vector>

namespace snaplite {

struct VirtualScreen {
    int x{};
    int y{};
    int width{};
    int height{};
};

VirtualScreen GetVirtualScreen();
HBITMAP CaptureRect(int x, int y, int width, int height);
HBITMAP CaptureVirtualScreen();
HBITMAP CloneBitmap(HBITMAP source);
HBITMAP CloneBitmapRegion(HBITMAP source, const RECT& rect);
COLORREF ReadBitmapPixel(HBITMAP bitmap, int x, int y);

// 一次性读出整块位图的像素：每像素 4 字节，顺序 B、G、R、保留位。
// 行序自上而下（第 0 行是位图最上面一行）。
//
// 逐像素调 ReadBitmapPixel 每读一个像素就要建一个临时 DC 再删掉：扫一块
// 148x104 的区域要建三万次。延时截图等桌面重绘（要反复扫同一块区域）和
// 它的回归测试都受不了这个开销，所以另开一个批量入口。
bool ReadBitmapPixels(HBITMAP bitmap, int width, int height, std::vector<BYTE>& pixels);

bool CopyBitmapToClipboard(
    HWND owner,
    HBITMAP bitmap,
    const std::filesystem::path& savedFile = {});
bool CopyTextToClipboard(HWND owner, const std::wstring& text);
bool SaveBitmapPng(HBITMAP bitmap, const std::filesystem::path& path);

// 打开剪贴板，失败时短暂重试。
//
// 剪贴板经常被别的进程短暂占用。直接 OpenClipboard 失败会让"复制"静默
// 什么都没发生，所以统一走这个带重试的入口：图片复制、文字复制、OCR 面板
// 复制都走它，避免某一条路径还留在"一次失败就放弃"的老行为上。
bool OpenClipboardWithRetry(HWND owner);

// Legacy/default helpers retained for compatibility.
std::filesystem::path ScreenshotDirectory();
std::filesystem::path NextScreenshotPath();

// User-configurable screenshot storage. The selected folder is persisted in
// HKCU so portable builds do not need a config file beside the executable.
std::filesystem::path ConfiguredScreenshotDirectory();
bool SetScreenshotDirectory(const std::filesystem::path& directory);
std::filesystem::path NextConfiguredScreenshotPath();

}  // namespace snaplite
