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

// Writes several clipboard representations at once so native apps and AI CLIs
// can consume the screenshot using whichever format they support best.
bool CopyBitmapToClipboard(
    HWND owner,
    HBITMAP bitmap,
    const std::filesystem::path& savedFile = {});
bool CopyTextToClipboard(HWND owner, const std::wstring& text);
bool SaveBitmapPng(HBITMAP bitmap, const std::filesystem::path& path);

std::filesystem::path ScreenshotDirectory();
std::filesystem::path NextScreenshotPath();

}  // namespace snaplite
