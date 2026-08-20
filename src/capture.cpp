#include "capture.h"

#include <gdiplus.h>
#include <objidl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdlib>
#include <cstring>
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

HGLOBAL CreateDibClipboardData(HBITMAP bitmap) {
    BITMAP info{};
    if (!bitmap || GetObjectW(bitmap, sizeof(info), &info) == 0) {
        return nullptr;
    }

    const int width = info.bmWidth;
    const int height = std::abs(info.bmHeight);
    if (width <= 0 || height <= 0) {
        return nullptr;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    bmi.bmiHeader.biSizeImage = static_cast<DWORD>(width * 4ULL * height);

    const SIZE_T pixelBytes = static_cast<SIZE_T>(width) * 4ULL * height;
    const SIZE_T totalBytes = sizeof(BITMAPINFOHEADER) + pixelBytes;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, totalBytes);
    if (!memory) {
        return nullptr;
    }

    void* target = GlobalLock(memory);
    if (!target) {
        GlobalFree(memory);
        return nullptr;
    }

    HDC dc = GetDC(nullptr);
    if (!dc) {
        GlobalUnlock(memory);
        GlobalFree(memory);
        return nullptr;
    }

    auto* pixels = static_cast<BYTE*>(target) + sizeof(BITMAPINFOHEADER);
    const int lines = GetDIBits(
        dc,
        bitmap,
        0,
        static_cast<UINT>(height),
        pixels,
        &bmi,
        DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);

    if (lines == 0) {
        GlobalUnlock(memory);
        GlobalFree(memory);
        return nullptr;
    }

    std::memcpy(target, &bmi.bmiHeader, sizeof(BITMAPINFOHEADER));
    GlobalUnlock(memory);
    return memory;
}

HGLOBAL CreatePngClipboardData(HBITMAP bitmap) {
    if (!bitmap) {
        return nullptr;
    }

    CLSID encoder{};
    if (GetEncoderClsid(L"image/png", &encoder) < 0) {
        return nullptr;
    }

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, FALSE, &stream)) || !stream) {
        return nullptr;
    }

    Gdiplus::Bitmap image(bitmap, nullptr);
    const Gdiplus::Status status = image.Save(stream, &encoder, nullptr);
    stream->Commit(STGC_DEFAULT);

    HGLOBAL memory = nullptr;
    GetHGlobalFromStream(stream, &memory);
    stream->Release();

    if (status != Gdiplus::Ok || !memory) {
        if (memory) {
            GlobalFree(memory);
        }
        return nullptr;
    }
    return memory;
}

HGLOBAL CreateFileDropClipboardData(const std::filesystem::path& path) {
    if (path.empty()) {
        return nullptr;
    }

    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) {
        return nullptr;
    }

    const std::wstring file = path.wstring();
    const SIZE_T chars = file.size() + 2;
    const SIZE_T totalBytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, totalBytes);
    if (!memory) {
        return nullptr;
    }

    auto* drop = static_cast<DROPFILES*>(GlobalLock(memory));
    if (!drop) {
        GlobalFree(memory);
        return nullptr;
    }

    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;

    auto* names = reinterpret_cast<wchar_t*>(
        reinterpret_cast<BYTE*>(drop) + sizeof(DROPFILES));
    std::memcpy(names, file.c_str(), file.size() * sizeof(wchar_t));
    names[file.size()] = L'\0';
    names[file.size() + 1] = L'\0';

    GlobalUnlock(memory);
    return memory;
}

void FreeBitmapCandidate(HBITMAP bitmap) {
    if (bitmap) {
        DeleteObject(bitmap);
    }
}

void FreeGlobalCandidate(HGLOBAL memory) {
    if (memory) {
        GlobalFree(memory);
    }
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

bool CopyBitmapToClipboard(
    HWND owner,
    HBITMAP bitmap,
    const std::filesystem::path& savedFile) {
    if (!bitmap) {
        return false;
    }

    HBITMAP bitmapCopy = CloneBitmap(bitmap);
    HGLOBAL dibData = CreateDibClipboardData(bitmap);
    HGLOBAL pngData = CreatePngClipboardData(bitmap);
    HGLOBAL fileDrop = CreateFileDropClipboardData(savedFile);

    if (!bitmapCopy && !dibData && !pngData) {
        FreeGlobalCandidate(fileDrop);
        return false;
    }

    if (!OpenClipboard(owner)) {
        FreeBitmapCandidate(bitmapCopy);
        FreeGlobalCandidate(dibData);
        FreeGlobalCandidate(pngData);
        FreeGlobalCandidate(fileDrop);
        return false;
    }

    if (!EmptyClipboard()) {
        CloseClipboard();
        FreeBitmapCandidate(bitmapCopy);
        FreeGlobalCandidate(dibData);
        FreeGlobalCandidate(pngData);
        FreeGlobalCandidate(fileDrop);
        return false;
    }

    bool imagePlaced = false;

    if (bitmapCopy) {
        if (SetClipboardData(CF_BITMAP, bitmapCopy)) {
            imagePlaced = true;
            bitmapCopy = nullptr;
        }
    }

    if (dibData) {
        if (SetClipboardData(CF_DIB, dibData)) {
            imagePlaced = true;
            dibData = nullptr;
        }
    }

    if (pngData) {
        const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
        if (pngFormat != 0 && SetClipboardData(pngFormat, pngData)) {
            imagePlaced = true;
            pngData = nullptr;
        }
    }

    if (fileDrop) {
        if (SetClipboardData(CF_HDROP, fileDrop)) {
            fileDrop = nullptr;
        }
    }

    CloseClipboard();

    FreeBitmapCandidate(bitmapCopy);
    FreeGlobalCandidate(dibData);
    FreeGlobalCandidate(pngData);
    FreeGlobalCandidate(fileDrop);
    return imagePlaced;
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

    std::memcpy(target, text.c_str(), bytes);
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
