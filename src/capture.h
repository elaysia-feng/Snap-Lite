#pragma once

#include <windows.h>
#include <objidl.h>
#include <propidl.h>

#include <filesystem>
#include <string>

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

bool CopyBitmapToClipboard(
    HWND owner,
    HBITMAP bitmap,
    const std::filesystem::path& savedFile = {});
bool CopyTextToClipboard(HWND owner, const std::wstring& text);
bool SaveBitmapPng(HBITMAP bitmap, const std::filesystem::path& path);

// Legacy/default helpers retained for compatibility.
std::filesystem::path ScreenshotDirectory();
std::filesystem::path NextScreenshotPath();

// User-configurable screenshot storage. The selected folder is persisted in
// HKCU so portable builds do not need a config file beside the executable.
std::filesystem::path ConfiguredScreenshotDirectory();
bool SetScreenshotDirectory(const std::filesystem::path& directory);
std::filesystem::path NextConfiguredScreenshotPath();

}  // namespace snaplite
