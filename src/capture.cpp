#include "capture.h"

#include <gdiplus.h>
#include <shlobj.h>

#include <cstdlib>
#include <cwchar>

#pragma comment(lib, "gdiplus.lib")

namespace snaplite {
namespace {

int GetEncoderClsid(const wchar_t* mimeType, CLSID* clsid) {
    UINT count = 0;
    UINT bytes = 0;
    Gdiplus::GetImageEncodersSize(&count, &bytes);
    if (bytes == 0) {
        return -1;
    }

    auto* codecs = static_cast<Gdiplus::ImageCodecInfo*>(std::malloc(bytes));
    if (!codecs) {
        return -1;
    }

    Gdiplus::GetImageEncoders(count, bytes, codecs);
    for (UINT i = 0; i < count; ++i) {
        if (std::wcscmp(codecs[i].MimeType, mimeType) == 0) {
            *clsid = codecs[i].Clsid;
            std::free(codecs);
            return static_cast<int>(i);
        }
    }

    std::free(codecs);
    return -1;
}

}  // namespace

VirtualScreen GetVirtualScreen() {
    return {
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_CYVIRTUALSCREEN),
    };
}

HBITMAP CaptureRect(int x, int y, int width, int height) {
    if (width <= 0 || height <= 0) {
        return nullptr;
    }

    HDC screenDc = GetDC(nullptr);
    if (!screenDc) {
        return nullptr;
    }

    HDC memoryDc = CreateCompatibleDC(screenDc);
    HBITMAP bitmap = CreateCompatibleBitmap(screenDc, width, height);
    if (!memoryDc || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (memoryDc) DeleteDC(memoryDc);
        ReleaseDC(nullptr, screenDc);
        return nullptr;
    }

    const HGDIOBJ old = SelectObject(memoryDc, bitmap);
    const BOOL ok = BitBlt(
        memoryDc,
        0,
        0,
        width,
        height,
        screenDc,
        x,
        y,
        SRCCOPY | CAPTUREBLT);
    SelectObject(memoryDc, old);

    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);

    if (!ok) {
        DeleteObject(bitmap);
        return nullptr;
    }
    return bitmap;
}

HBITMAP CaptureVirtualScreen() {
    const auto screen = GetVirtualScreen();
    return CaptureRect(screen.x, screen.y, screen.width, screen.height);
}

HBITMAP CloneBitmap(HBITMAP source) {
    if (!source) {
        return nullptr;
    }
    return static_cast<HBITMAP>(CopyImage(source, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
}

HBITMAP CloneBitmapRegion(HBITMAP source, const RECT& rect) {
    if (!source) {
        return nullptr;
    }

    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) {
        return nullptr;
    }

    HDC screenDc = GetDC(nullptr);
    HDC sourceDc = CreateCompatibleDC(screenDc);
    HDC targetDc = CreateCompatibleDC(screenDc);
    HBITMAP result = CreateCompatibleBitmap(screenDc, width, height);

    if (!sourceDc || !targetDc || !result) {
        if (result) DeleteObject(result);
        if (sourceDc) DeleteDC(sourceDc);
        if (targetDc) DeleteDC(targetDc);
        ReleaseDC(nullptr, screenDc);
        return nullptr;
    }

    const HGDIOBJ oldSource = SelectObject(sourceDc, source);
    const HGDIOBJ oldTarget = SelectObject(targetDc, result);
    const BOOL ok = BitBlt(
        targetDc,
        0,
        0,
        width,
        height,
        sourceDc,
        rect.left,
        rect.top,
        SRCCOPY);

    SelectObject(sourceDc, oldSource);
    SelectObject(targetDc, oldTarget);
    DeleteDC(sourceDc);
    DeleteDC(targetDc);
    ReleaseDC(nullptr, screenDc);

    if (!ok) {
        DeleteObject(result);
        return nullptr;
    }
    return result;
}

COLORREF ReadBitmapPixel(HBITMAP bitmap, int x, int y) {
    if (!bitmap) {
        return CLR_INVALID;
    }

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) {
        return CLR_INVALID;
    }

    const HGDIOBJ old = SelectObject(dc, bitmap);
    const COLORREF color = GetPixel(dc, x, y);
    SelectObject(dc, old);
    DeleteDC(dc);
    return color;
}

bool CopyBitmapToClipboard(HWND owner, HBITMAP bitmap) {
    if (!bitmap) {
        return false;
    }

    HBITMAP copy = CloneBitmap(bitmap);
    if (!copy) {
        return false;
    }

    if (!OpenClipboard(owner)) {
        DeleteObject(copy);
        return false;
    }

    EmptyClipboard();
    const HANDLE result = SetClipboardData(CF_BITMAP, copy);
    CloseClipboard();

    if (!result) {
        DeleteObject(copy);
        return false;
    }
    return true;
}

bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) {
        return false;
    }

    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        CloseClipboard();
        return false;
    }

    void* target = GlobalLock(memory);
    if (!target) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }

    memcpy(target, text.c_str(), bytes);
    GlobalUnlock(memory);

    const HANDLE result = SetClipboardData(CF_UNICODETEXT, memory);
    CloseClipboard();
    if (!result) {
        GlobalFree(memory);
        return false;
    }
    return true;
}

bool SaveBitmapPng(HBITMAP bitmap, const std::filesystem::path& path) {
    if (!bitmap) {
        return false;
    }

    CLSID encoder{};
    if (GetEncoderClsid(L"image/png", &encoder) < 0) {
        return false;
    }

    Gdiplus::Bitmap image(bitmap, nullptr);
    return image.Save(path.c_str(), &encoder, nullptr) == Gdiplus::Ok;
}

std::filesystem::path ScreenshotDirectory() {
    PWSTR pictures = nullptr;
    std::filesystem::path directory;

    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_DEFAULT, nullptr, &pictures))) {
        directory = std::filesystem::path(pictures) / L"Snap-Lite";
        CoTaskMemFree(pictures);
    } else {
        directory = std::filesystem::current_path() / L"Snap-Lite";
    }

    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return directory;
}

std::filesystem::path NextScreenshotPath() {
    SYSTEMTIME now{};
    GetLocalTime(&now);

    wchar_t filename[96]{};
    swprintf_s(
        filename,
        L"SnapLite_%04u-%02u-%02u_%02u-%02u-%02u-%03u.png",
        now.wYear,
        now.wMonth,
        now.wDay,
        now.wHour,
        now.wMinute,
        now.wSecond,
        now.wMilliseconds);

    return ScreenshotDirectory() / filename;
}

}  // namespace snaplite
