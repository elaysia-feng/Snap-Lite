#include "ocr.h"

// WIN32_LEAN_AND_MEAN removes several COM declarations that GDI+ imaging
// expects. Pull them in explicitly before gdiplus.h.
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <future>
#include <string>
#include <system_error>
#include <vector>

namespace snaplite {
namespace {

bool FindPngEncoder(CLSID& clsid) {
    UINT count = 0;
    UINT bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok ||
        count == 0 || bytes == 0) {
        return false;
    }

    std::vector<BYTE> buffer(bytes);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    if (Gdiplus::GetImageEncoders(count, bytes, codecs) != Gdiplus::Ok) {
        return false;
    }

    for (UINT i = 0; i < count; ++i) {
        if (codecs[i].MimeType && wcscmp(codecs[i].MimeType, L"image/png") == 0) {
            clsid = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

HBITMAP CropBitmap(HBITMAP source, const RECT& requested) {
    if (!source) return nullptr;

    BITMAP info{};
    if (GetObjectW(source, sizeof(info), &info) == 0) return nullptr;

    const LONG sourceWidth = std::abs(info.bmWidth);
    const LONG sourceHeight = std::abs(info.bmHeight);
    RECT region{
        std::clamp<LONG>(requested.left, 0, sourceWidth),
        std::clamp<LONG>(requested.top, 0, sourceHeight),
        std::clamp<LONG>(requested.right, 0, sourceWidth),
        std::clamp<LONG>(requested.bottom, 0, sourceHeight),
    };
    if (region.right < region.left) std::swap(region.right, region.left);
    if (region.bottom < region.top) std::swap(region.bottom, region.top);

    const int width = static_cast<int>(region.right - region.left);
    const int height = static_cast<int>(region.bottom - region.top);
    if (width <= 0 || height <= 0) return nullptr;

    HDC sourceDc = CreateCompatibleDC(nullptr);
    HDC targetDc = CreateCompatibleDC(nullptr);
    HDC screenDc = GetDC(nullptr);
    if (!sourceDc || !targetDc || !screenDc) {
        if (screenDc) ReleaseDC(nullptr, screenDc);
        if (targetDc) DeleteDC(targetDc);
        if (sourceDc) DeleteDC(sourceDc);
        return nullptr;
    }

    HBITMAP cropped = CreateCompatibleBitmap(screenDc, width, height);
    ReleaseDC(nullptr, screenDc);
    if (!cropped) {
        DeleteDC(targetDc);
        DeleteDC(sourceDc);
        return nullptr;
    }

    const HGDIOBJ oldSource = SelectObject(sourceDc, source);
    const HGDIOBJ oldTarget = SelectObject(targetDc, cropped);
    const BOOL copied = BitBlt(
        targetDc,
        0,
        0,
        width,
        height,
        sourceDc,
        region.left,
        region.top,
        SRCCOPY);
    SelectObject(targetDc, oldTarget);
    SelectObject(sourceDc, oldSource);
    DeleteDC(targetDc);
    DeleteDC(sourceDc);

    if (!copied) {
        DeleteObject(cropped);
        return nullptr;
    }
    return cropped;
}

std::filesystem::path MakeTempOcrPath() {
    wchar_t tempDirectory[MAX_PATH + 1]{};
    const DWORD length = GetTempPathW(MAX_PATH, tempDirectory);
    if (length == 0 || length > MAX_PATH) return {};

    std::wstring name = L"SnapLite-OCR-";
    name += std::to_wstring(GetCurrentProcessId());
    name += L"-";
    name += std::to_wstring(GetTickCount64());
    name += L".png";
    return std::filesystem::path(tempDirectory) / name;
}

bool SaveBitmapAsPng(HBITMAP bitmap, const std::filesystem::path& path) {
    if (!bitmap || path.empty()) return false;

    CLSID pngEncoder{};
    if (!FindPngEncoder(pngEncoder)) return false;

    Gdiplus::Bitmap image(bitmap, nullptr);
    if (image.GetLastStatus() != Gdiplus::Ok) return false;
    return image.Save(path.c_str(), &pngEncoder, nullptr) == Gdiplus::Ok;
}

OcrResult RecognizePng(const std::filesystem::path& path) {
    OcrResult output;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        using namespace winrt::Windows::Graphics::Imaging;
        using namespace winrt::Windows::Media::Ocr;
        using namespace winrt::Windows::Storage;

        const auto file = StorageFile::GetFileFromPathAsync(winrt::hstring(path.wstring())).get();
        const auto stream = file.OpenAsync(FileAccessMode::Read).get();
        const auto decoder = BitmapDecoder::CreateAsync(stream).get();
        const auto softwareBitmap = decoder
            .GetSoftwareBitmapAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied)
            .get();

        const auto engine = OcrEngine::TryCreateFromUserProfileLanguages();
        if (!engine) {
            output.error = L"Windows OCR 不可用，请在系统语言设置中安装中文或英文 OCR 语言包。";
            return output;
        }

        const auto result = engine.RecognizeAsync(softwareBitmap).get();
        const auto recognized = result.Text();
        output.text.assign(recognized.c_str(), recognized.size());
        while (!output.text.empty() && iswspace(output.text.back())) {
            output.text.pop_back();
        }
        output.success = true;
        return output;
    } catch (const winrt::hresult_error& error) {
        output.error = L"OCR 识别失败：";
        output.error += error.message().c_str();
        return output;
    } catch (...) {
        output.error = L"OCR 识别失败：发生未知错误。";
        return output;
    }
}

}  // namespace

OcrResult ExtractTextFromBitmapRegion(HBITMAP bitmap, const RECT& region) {
    OcrResult output;
    HBITMAP cropped = CropBitmap(bitmap, region);
    if (!cropped) {
        output.error = L"无法读取当前截图选区。";
        return output;
    }

    const std::filesystem::path tempPath = MakeTempOcrPath();
    const bool saved = SaveBitmapAsPng(cropped, tempPath);
    DeleteObject(cropped);
    if (!saved) {
        output.error = L"无法准备 OCR 临时图像。";
        return output;
    }

    output = std::async(std::launch::async, [tempPath]() {
        return RecognizePng(tempPath);
    }).get();

    std::error_code ignored;
    std::filesystem::remove(tempPath, ignored);
    return output;
}

bool CopyUnicodeTextToClipboard(HWND owner, const std::wstring& text) {
    if (text.empty() || !OpenClipboard(owner)) return false;

    if (!EmptyClipboard()) {
        CloseClipboard();
        return false;
    }

    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        CloseClipboard();
        return false;
    }

    void* destination = GlobalLock(memory);
    if (!destination) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    std::memcpy(destination, text.c_str(), bytes);
    GlobalUnlock(memory);

    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }

    CloseClipboard();
    return true;
}

}  // namespace snaplite
