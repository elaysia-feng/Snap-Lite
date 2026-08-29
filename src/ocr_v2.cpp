#include "ocr.h"

#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

namespace snaplite {
namespace {

enum class PreparedVariant {
    Color = 0,
    GrayContrast,
    Binary,
};

struct OcrLineSnapshot {
    std::wstring text;
    float top{};
    float bottom{};
    float height{};
    bool hasBounds{false};
};

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

int Luminance(BYTE b, BYTE g, BYTE r) {
    return (29 * static_cast<int>(b) + 150 * static_cast<int>(g) +
            77 * static_cast<int>(r)) >> 8;
}

int OtsuThreshold(const BYTE* pixels, int width, int height) {
    std::array<unsigned long long, 256> histogram{};
    unsigned long long totalLuminance = 0;
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);

    for (size_t i = 0; i < pixelCount; ++i) {
        const BYTE* p = pixels + i * 4;
        const int gray = Luminance(p[0], p[1], p[2]);
        ++histogram[static_cast<size_t>(gray)];
        totalLuminance += static_cast<unsigned long long>(gray);
    }

    if (pixelCount == 0) return 128;

    unsigned long long backgroundWeight = 0;
    unsigned long long backgroundSum = 0;
    double bestVariance = -1.0;
    int bestThreshold = 128;

    for (int threshold = 0; threshold < 256; ++threshold) {
        backgroundWeight += histogram[static_cast<size_t>(threshold)];
        if (backgroundWeight == 0) continue;

        const unsigned long long foregroundWeight = pixelCount - backgroundWeight;
        if (foregroundWeight == 0) break;

        backgroundSum +=
            static_cast<unsigned long long>(threshold) * histogram[static_cast<size_t>(threshold)];
        const double backgroundMean =
            static_cast<double>(backgroundSum) / static_cast<double>(backgroundWeight);
        const double foregroundMean =
            static_cast<double>(totalLuminance - backgroundSum) /
            static_cast<double>(foregroundWeight);
        const double difference = backgroundMean - foregroundMean;
        const double variance =
            static_cast<double>(backgroundWeight) * static_cast<double>(foregroundWeight) *
            difference * difference;

        if (variance > bestVariance) {
            bestVariance = variance;
            bestThreshold = threshold;
        }
    }

    return bestThreshold;
}

HBITMAP PrepareBitmap(HBITMAP source, PreparedVariant variant) {
    if (!source) return nullptr;

    BITMAP sourceInfo{};
    if (GetObjectW(source, sizeof(sourceInfo), &sourceInfo) == 0) return nullptr;

    const int sourceWidth = std::abs(sourceInfo.bmWidth);
    const int sourceHeight = std::abs(sourceInfo.bmHeight);
    if (sourceWidth <= 0 || sourceHeight <= 0) return nullptr;

    const int sourceMax = std::max(sourceWidth, sourceHeight);
    double scale = sourceMax <= 900 ? 3.0 : 2.0;
    if (sourceMax > 0) {
        scale = std::min(scale, 2600.0 / static_cast<double>(sourceMax));
    }
    scale = std::max(1.0, scale);

    const int width = std::max(1, static_cast<int>(std::lround(sourceWidth * scale)));
    const int height = std::max(1, static_cast<int>(std::lround(sourceHeight * scale)));

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP prepared = CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!prepared || !bits) {
        if (prepared) DeleteObject(prepared);
        return nullptr;
    }

    HDC sourceDc = CreateCompatibleDC(nullptr);
    HDC targetDc = CreateCompatibleDC(nullptr);
    if (!sourceDc || !targetDc) {
        if (sourceDc) DeleteDC(sourceDc);
        if (targetDc) DeleteDC(targetDc);
        DeleteObject(prepared);
        return nullptr;
    }

    const HGDIOBJ oldSource = SelectObject(sourceDc, source);
    const HGDIOBJ oldTarget = SelectObject(targetDc, prepared);
    SetStretchBltMode(targetDc, HALFTONE);
    SetBrushOrgEx(targetDc, 0, 0, nullptr);
    const BOOL copied = StretchBlt(
        targetDc,
        0,
        0,
        width,
        height,
        sourceDc,
        0,
        0,
        sourceWidth,
        sourceHeight,
        SRCCOPY);
    SelectObject(targetDc, oldTarget);
    SelectObject(sourceDc, oldSource);
    DeleteDC(targetDc);
    DeleteDC(sourceDc);

    if (!copied) {
        DeleteObject(prepared);
        return nullptr;
    }

    auto* pixels = static_cast<BYTE*>(bits);
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);

    if (variant == PreparedVariant::Binary) {
        const int threshold = OtsuThreshold(pixels, width, height);
        unsigned long long sum = 0;
        for (size_t i = 0; i < pixelCount; ++i) {
            BYTE* p = pixels + i * 4;
            sum += static_cast<unsigned long long>(Luminance(p[0], p[1], p[2]));
        }
        const double average = pixelCount == 0
            ? 255.0
            : static_cast<double>(sum) / static_cast<double>(pixelCount);
        const bool darkBackground = average < 128.0;

        for (size_t i = 0; i < pixelCount; ++i) {
            BYTE* p = pixels + i * 4;
            const int gray = Luminance(p[0], p[1], p[2]);
            const BYTE value = darkBackground
                ? static_cast<BYTE>(gray > threshold ? 0 : 255)
                : static_cast<BYTE>(gray <= threshold ? 0 : 255);
            p[0] = value;
            p[1] = value;
            p[2] = value;
            p[3] = 255;
        }
    } else if (variant == PreparedVariant::GrayContrast) {
        for (size_t i = 0; i < pixelCount; ++i) {
            BYTE* p = pixels + i * 4;
            const int gray = Luminance(p[0], p[1], p[2]);
            const int contrasted = std::clamp(
                static_cast<int>(std::lround((gray - 128) * 1.72 + 128)), 0, 255);
            const BYTE value = static_cast<BYTE>(contrasted);
            p[0] = value;
            p[1] = value;
            p[2] = value;
            p[3] = 255;
        }
    } else {
        for (size_t i = 0; i < pixelCount; ++i) {
            pixels[i * 4 + 3] = 255;
        }
    }

    return prepared;
}

std::filesystem::path MakeTempOcrPath(int index) {
    wchar_t tempDirectory[MAX_PATH + 1]{};
    const DWORD length = GetTempPathW(MAX_PATH, tempDirectory);
    if (length == 0 || length > MAX_PATH) return {};

    std::wstring name = L"SnapLite-OCR2-";
    name += std::to_wstring(GetCurrentProcessId());
    name += L"-";
    name += std::to_wstring(GetTickCount64());
    name += L"-";
    name += std::to_wstring(index);
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

std::filesystem::path MakeTempOcrResultPath() {
    wchar_t tempDirectory[MAX_PATH + 1]{};
    const DWORD length = GetTempPathW(MAX_PATH, tempDirectory);
    if (length == 0 || length > MAX_PATH) return {};

    std::wstring name = L"SnapLite-OCR2-";
    name += std::to_wstring(GetCurrentProcessId());
    name += L"-";
    name += std::to_wstring(GetTickCount64());
    name += L"-result.txt";
    return std::filesystem::path(tempDirectory) / name;
}

std::filesystem::path PaddleWorkerPath() {
    wchar_t modulePath[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath, 32768);
    if (length == 0 || length >= 32768) return {};
    return std::filesystem::path(modulePath).parent_path() / L"paddle_ocr_worker.exe";
}

std::wstring QuoteWindowsArgument(const std::wstring& value) {
    std::wstring quoted = L"\"";
    for (const wchar_t character : value) {
        if (character == L'\"') quoted += L"\\\"";
        else quoted += character;
    }
    quoted += L"\"";
    return quoted;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (length <= 0) {
        length = MultiByteToWideChar(
            CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    }
    if (length <= 0) return {};

    std::wstring converted(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        converted.data(), length);
    if (!converted.empty() && converted.front() == 0xFEFF) converted.erase(0, 1);
    return converted;
}

void TrimLineEnd(std::wstring& text);

OcrResult TryPaddleOcr(const std::filesystem::path& input) {
    OcrResult output;
    const auto worker = PaddleWorkerPath();
    if (worker.empty() || GetFileAttributesW(worker.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return output;
    }

    const auto resultPath = MakeTempOcrResultPath();
    if (resultPath.empty()) return output;

    std::wstring command = QuoteWindowsArgument(worker.wstring());
    command += L" --input ";
    command += QuoteWindowsArgument(input.wstring());
    command += L" --output ";
    command += QuoteWindowsArgument(resultPath.wstring());
    command += L" --lang ch --cache-dir ";
    command += QuoteWindowsArgument((worker.parent_path() / L"paddle_models").wstring());
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    const BOOL launched = CreateProcessW(
        worker.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, worker.parent_path().c_str(), &startup, &process);
    if (!launched) return output;

    constexpr DWORD kWorkerTimeoutMs = 90000;
    const DWORD waitResult = WaitForSingleObject(process.hProcess, kWorkerTimeoutMs);
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1);
    }

    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    if (waitResult != WAIT_OBJECT_0 || exitCode != 0) {
        std::error_code ignored;
        std::filesystem::remove(resultPath, ignored);
        return output;
    }

    std::ifstream file(resultPath, std::ios::binary);
    if (file) {
        const std::string bytes(
            std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        output.text = Utf8ToWide(bytes);
        TrimLineEnd(output.text);
        output.success = !output.text.empty();
    }

    std::error_code ignored;
    std::filesystem::remove(resultPath, ignored);
    return output;
}

bool StartsWithIgnoreCase(const std::wstring& value, const wchar_t* prefix) {
    if (!prefix) return false;
    const std::wstring wanted(prefix);
    if (value.size() < wanted.size()) return false;
    for (size_t i = 0; i < wanted.size(); ++i) {
        if (towlower(value[i]) != towlower(wanted[i])) return false;
    }
    return true;
}

void TrimLineEnd(std::wstring& text) {
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t' ||
                             text.back() == L'\r' || text.back() == L'\n')) {
        text.pop_back();
    }
}

OcrLineSnapshot SnapshotLine(const winrt::Windows::Media::Ocr::OcrLine& line) {
    OcrLineSnapshot snapshot;
    const auto lineText = line.Text();
    snapshot.text.assign(lineText.c_str(), lineText.size());
    TrimLineEnd(snapshot.text);

    const auto words = line.Words();
    const uint32_t wordCount = words.Size();
    if (wordCount == 0) return snapshot;

    float top = std::numeric_limits<float>::max();
    float bottom = -std::numeric_limits<float>::max();
    for (uint32_t i = 0; i < wordCount; ++i) {
        const auto rect = words.GetAt(i).BoundingRect();
        top = std::min(top, rect.Y);
        bottom = std::max(bottom, rect.Y + rect.Height);
    }

    if (bottom >= top && top != std::numeric_limits<float>::max()) {
        snapshot.top = top;
        snapshot.bottom = bottom;
        snapshot.height = std::max(1.0f, bottom - top);
        snapshot.hasBounds = true;
    }
    return snapshot;
}

std::wstring RebuildTextWithLayout(const winrt::Windows::Media::Ocr::OcrResult& result) {
    const auto lines = result.Lines();
    const uint32_t lineCount = lines.Size();
    if (lineCount == 0) {
        const auto raw = result.Text();
        std::wstring fallback(raw.c_str(), raw.size());
        TrimLineEnd(fallback);
        return fallback;
    }

    std::vector<OcrLineSnapshot> snapshots;
    snapshots.reserve(lineCount);
    for (uint32_t i = 0; i < lineCount; ++i) {
        auto snapshot = SnapshotLine(lines.GetAt(i));
        if (!snapshot.text.empty()) snapshots.push_back(std::move(snapshot));
    }

    if (snapshots.empty()) {
        const auto raw = result.Text();
        std::wstring fallback(raw.c_str(), raw.size());
        TrimLineEnd(fallback);
        return fallback;
    }

    std::wstring text;
    for (size_t i = 0; i < snapshots.size(); ++i) {
        if (i > 0) {
            bool paragraphGap = false;
            const auto& previous = snapshots[i - 1];
            const auto& current = snapshots[i];
            if (previous.hasBounds && current.hasBounds) {
                const float gap = current.top - previous.bottom;
                const float referenceHeight = std::max(previous.height, current.height);
                paragraphGap = gap > referenceHeight * 0.82f;
            }
            text += paragraphGap ? L"\r\n\r\n" : L"\r\n";
        }
        text += snapshots[i].text;
    }
    return text;
}

double TextQualityScore(const std::wstring& text) {
    if (text.empty()) return -std::numeric_limits<double>::infinity();

    size_t meaningful = 0;
    size_t cjk = 0;
    size_t odd = 0;
    size_t newlines = 0;

    for (wchar_t ch : text) {
        if (ch == L'\n') {
            ++newlines;
            continue;
        }
        if (iswspace(ch)) continue;

        const bool isCjk =
            (ch >= 0x3400 && ch <= 0x4DBF) ||
            (ch >= 0x4E00 && ch <= 0x9FFF) ||
            (ch >= 0xF900 && ch <= 0xFAFF);
        if (isCjk) {
            ++meaningful;
            ++cjk;
            continue;
        }

        if (iswalnum(ch)) {
            ++meaningful;
            continue;
        }

        switch (ch) {
        case L'.': case L',': case L':': case L';': case L'-': case L'_':
        case L'/': case L'\\': case L'(': case L')': case L'[': case L']':
        case L'{': case L'}': case L'<': case L'>': case L'=': case L'+':
        case L'*': case L'#': case L'@': case L'!': case L'?': case L'"':
        case L'\'': case L'，': case L'。': case L'：': case L'；': case L'！':
        case L'？': case L'（': case L'）': case L'【': case L'】':
            ++meaningful;
            break;
        default:
            ++odd;
            break;
        }
    }

    if (meaningful == 0) return -std::numeric_limits<double>::infinity();
    const size_t visible = meaningful + odd;
    const double cleanRatio = visible == 0
        ? 0.0
        : static_cast<double>(meaningful) / static_cast<double>(visible);

    return static_cast<double>(meaningful) * 3.0 +
           static_cast<double>(cjk) * 0.9 +
           std::min<double>(static_cast<double>(newlines), 40.0) * 0.35 +
           cleanRatio * 25.0 -
           static_cast<double>(odd) * 1.8;
}

std::wstring RecognizePng(
    const std::filesystem::path& path,
    const winrt::Windows::Media::Ocr::OcrEngine& engine) {
    using namespace winrt::Windows::Graphics::Imaging;
    using namespace winrt::Windows::Storage;

    const auto file = StorageFile::GetFileFromPathAsync(winrt::hstring(path.wstring())).get();
    const auto stream = file.OpenAsync(FileAccessMode::Read).get();
    const auto decoder = BitmapDecoder::CreateAsync(stream).get();
    const auto softwareBitmap = decoder
        .GetSoftwareBitmapAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied)
        .get();

    const auto result = engine.RecognizeAsync(softwareBitmap).get();
    return RebuildTextWithLayout(result);
}

OcrResult RecognizePreparedVariants(const std::vector<std::filesystem::path>& paths) {
    OcrResult output;

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        using winrt::Windows::Globalization::Language;
        using winrt::Windows::Media::Ocr::OcrEngine;

        std::vector<OcrEngine> engines;
        std::vector<std::wstring> engineTags;

        auto addEngine = [&](const OcrEngine& engine) {
            if (!engine) return;
            const auto language = engine.RecognizerLanguage();
            const auto tagValue = language.LanguageTag();
            const std::wstring tag(tagValue.c_str(), tagValue.size());
            if (std::find(engineTags.begin(), engineTags.end(), tag) != engineTags.end()) return;
            engines.push_back(engine);
            engineTags.push_back(tag);
        };

        const auto available = OcrEngine::AvailableRecognizerLanguages();
        const uint32_t availableCount = available.Size();

        // Chinese screenshots are the main Snap-Lite use case. Prefer Simplified Chinese
        // whenever Windows has that OCR language installed.
        for (uint32_t i = 0; i < availableCount; ++i) {
            const Language language = available.GetAt(i);
            const auto tagValue = language.LanguageTag();
            const std::wstring tag(tagValue.c_str(), tagValue.size());
            if (StartsWithIgnoreCase(tag, L"zh-Hans") || StartsWithIgnoreCase(tag, L"zh-CN")) {
                addEngine(OcrEngine::TryCreateFromLanguage(language));
            }
        }

        addEngine(OcrEngine::TryCreateFromUserProfileLanguages());

        for (uint32_t i = 0; i < availableCount; ++i) {
            const Language language = available.GetAt(i);
            const auto tagValue = language.LanguageTag();
            const std::wstring tag(tagValue.c_str(), tagValue.size());
            if (StartsWithIgnoreCase(tag, L"en")) {
                addEngine(OcrEngine::TryCreateFromLanguage(language));
            }
        }

        if (engines.empty()) {
            output.error = L"Windows OCR 不可用，请在系统语言设置中安装中文或英文 OCR 语言包。";
            return output;
        }

        std::wstring bestText;
        double bestScore = -std::numeric_limits<double>::infinity();

        for (size_t engineIndex = 0; engineIndex < engines.size(); ++engineIndex) {
            for (size_t variantIndex = 0; variantIndex < paths.size(); ++variantIndex) {
                const std::wstring text = RecognizePng(paths[variantIndex], engines[engineIndex]);
                double score = TextQualityScore(text);
                score -= static_cast<double>(engineIndex) * 0.18;
                if (variantIndex == 1) score += 0.12;

                if (score > bestScore) {
                    bestScore = score;
                    bestText = text;
                }
            }
        }

        output.text = std::move(bestText);
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

    const std::array<PreparedVariant, 3> variants = {
        PreparedVariant::Color,
        PreparedVariant::GrayContrast,
        PreparedVariant::Binary,
    };

    std::vector<std::filesystem::path> paths;
    paths.reserve(variants.size());

    for (size_t i = 0; i < variants.size(); ++i) {
        HBITMAP prepared = PrepareBitmap(cropped, variants[i]);
        if (!prepared) continue;

        const std::filesystem::path path = MakeTempOcrPath(static_cast<int>(i));
        const bool saved = SaveBitmapAsPng(prepared, path);
        DeleteObject(prepared);
        if (saved) paths.push_back(path);
    }
    DeleteObject(cropped);

    if (paths.empty()) {
        output.error = L"无法准备 OCR 图像。";
        return output;
    }

    // 发布包如果带有 PaddleOCR worker，优先使用它；没有 worker 或启动失败
    // 时继续走 Windows OCR，保证便携版和未安装模型的环境仍然可用。
    output = TryPaddleOcr(paths.front());
    if (output.success) {
        for (const auto& path : paths) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
        return output;
    }

    output = std::async(std::launch::async, [paths]() {
        return RecognizePreparedVariants(paths);
    }).get();

    for (const auto& path : paths) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
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
