#include "ocr.h"

#include "capture.h"

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
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
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
    // OCR 引擎对单边像素有硬上限（MaxImageDimension=2600），超出会直接抛异常。
    //
    // 缩放策略必须连续：小的选区最多放大 3 倍（太小识别不准），同时保证长边
    // 永远不超过 2560（给引擎 2600 上限留 40px 余量）。之前的写法里
    // "scale = max(1.0, scale)" 会把上限收敛抵消掉，大选区必然识别失败；
    // 而 900/901 之间的硬阈值分支还会让分辨率在断崖处突然掉一档。
    constexpr double kOcrMaxDimension = 2560.0;
    constexpr double kOcrMaxUpscale = 3.0;
    const double scale = std::min(
        kOcrMaxUpscale, kOcrMaxDimension / static_cast<double>(sourceMax));

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

    // 文件名必须唯一。GetTickCount64() 的分辨率是系统时钟粒度（约 15.6ms），
    // 同一 tick 内的两次请求会算出同一个路径，两个后台线程互相覆盖、
    // 甚至删掉同一批 PNG。这里补一个进程内自增序号兜底。
    static std::atomic<unsigned long long> g_tempPathCounter{0};

    std::wstring name = L"SnapLite-OCR2-";
    name += std::to_wstring(GetCurrentProcessId());
    name += L"-";
    name += std::to_wstring(GetTickCount64());
    name += L"-";
    name += std::to_wstring(
        g_tempPathCounter.fetch_add(1, std::memory_order_relaxed));
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
                if (!engines.empty()) break;
            }
        }

        if (engines.empty()) {
            addEngine(OcrEngine::TryCreateFromUserProfileLanguages());
        }

        for (uint32_t i = 0; i < availableCount && engines.empty(); ++i) {
            const Language language = available.GetAt(i);
            const auto tagValue = language.LanguageTag();
            const std::wstring tag(tagValue.c_str(), tagValue.size());
            if (StartsWithIgnoreCase(tag, L"en")) {
                addEngine(OcrEngine::TryCreateFromLanguage(language));
                if (!engines.empty()) break;
            }
        }

        if (engines.empty()) {
            output.error = L"Windows OCR 不可用，请在系统语言设置中安装中文或英文 OCR 语言包。";
            return output;
        }

        std::wstring bestText;
        double bestScore = -std::numeric_limits<double>::infinity();

        for (size_t engineIndex = 0; engineIndex < engines.size(); ++engineIndex) {
            // Color and contrast cover the normal screenshot cases. Binary is kept as a
            // rescue pass for low-contrast captures so the portable fallback does not
            // spend three OCR passes on every ordinary selection.
            const size_t primaryVariantCount = std::min<size_t>(paths.size(), 2);
            for (size_t variantIndex = 0; variantIndex < primaryVariantCount; ++variantIndex) {
                const std::wstring text = RecognizePng(paths[variantIndex], engines[engineIndex]);
                double score = TextQualityScore(text);
                score -= static_cast<double>(engineIndex) * 0.18;

                if (score > bestScore) {
                    bestScore = score;
                    bestText = text;
                }
            }

            if (paths.size() > primaryVariantCount && bestScore < 45.0) {
                const size_t binaryIndex = paths.size() - 1;
                const std::wstring text = RecognizePng(paths[binaryIndex], engines[engineIndex]);
                const double score = TextQualityScore(text);
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

// 完整的 OCR 流水线：预处理变体 → 临时 PNG → 系统 OCR 择优。
// 本函数会释放 cropped。
OcrResult RunOcrPipeline(HBITMAP cropped) {
    OcrResult output;
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

    // 直接使用 Windows 自带 OCR，候选图像只在系统 OCR 内部择优。
    output = RecognizePreparedVariants(paths);

    for (const auto& path : paths) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    return output;
}

// --- 后台识别 -> UI 线程回调 -----------------------------------------------

constexpr wchar_t kOcrCompletionWindowClass[] = L"SnapLiteOcrCompletionWindow";
constexpr UINT WM_SNAPLITE_OCR_COMPLETE = WM_APP + 0x2A1;

struct OcrCompletionPayload {
    OcrResult result;
    OcrRequest request;
    OcrCompletionCallback callback{};
};

// 还没被 UI 线程处理掉的完成消息。
//
// 消息是 PostMessage 过去的：进程在消息被处理之前退出，那份 payload
// 就泄漏了（识别结果连同它持有的东西一起丢在内存里）。
//
// 这里登记一份，退出时统一回收。所有权转移都走"从表里摘走"这一种方式：
// 摘到的人负责释放，摘不到说明别人已经回收过了——不会出现二次释放。
std::mutex g_pendingPayloadMutex;
std::vector<OcrCompletionPayload*> g_pendingPayloads;

void RegisterPendingPayload(OcrCompletionPayload* payload) {
    std::lock_guard<std::mutex> lock(g_pendingPayloadMutex);
    g_pendingPayloads.push_back(payload);
}

// 从待处理表里摘走一份 payload。返回 nullptr 表示它已经被回收过了。
OcrCompletionPayload* TakePendingPayload(OcrCompletionPayload* payload) {
    std::lock_guard<std::mutex> lock(g_pendingPayloadMutex);
    for (auto it = g_pendingPayloads.begin(); it != g_pendingPayloads.end(); ++it) {
        if (*it != payload) continue;
        g_pendingPayloads.erase(it);
        return payload;
    }
    return nullptr;
}

LRESULT CALLBACK OcrCompletionWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_SNAPLITE_OCR_COMPLETE) {
        // 所有权从待处理表里转移过来：摘不到说明退出流程已经回收了它。
        std::unique_ptr<OcrCompletionPayload> payload(
            TakePendingPayload(reinterpret_cast<OcrCompletionPayload*>(lParam)));
        if (payload && payload->callback) {
            payload->callback(payload->result, payload->request);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// 消息专属窗口：只负责把后台线程的结果搬回 UI 线程。
// 契约：StartTextExtractionFromBitmapRegion 只能从 UI 线程调用，
// 窗口因此在 UI 线程创建，回调也在 UI 线程执行。
HWND EnsureOcrCompletionWindow() {
    static std::mutex createMutex;
    static HWND window = nullptr;
    if (window != nullptr && IsWindow(window)) return window;

    std::lock_guard<std::mutex> guard(createMutex);
    if (window != nullptr && IsWindow(window)) return window;

    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = OcrCompletionWindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kOcrCompletionWindowClass;
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return nullptr;
        }
        classRegistered = true;
    }

    window = CreateWindowExW(
        0,
        kOcrCompletionWindowClass,
        L"",
        0,
        0, 0, 0, 0,
        HWND_MESSAGE,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    return window;
}

}  // namespace

bool StartTextExtractionFromBitmapRegion(
    HBITMAP bitmap,
    const RECT& region,
    HWND parent,
    unsigned long long instanceId,
    OcrCompletionCallback callback) {
    if (!bitmap || !callback || !parent || !IsWindow(parent)) return false;

    // 先在调用线程裁一份独立副本：后台线程不能读 UI 线程正在改写的位图。
    HBITMAP cropped = CropBitmap(bitmap, region);
    if (!cropped) return false;

    HWND completionWindow = EnsureOcrCompletionWindow();
    if (!completionWindow) {
        DeleteObject(cropped);
        return false;
    }

    OcrRequest request;
    request.parent = parent;
    request.instanceId = instanceId;
    request.selection = region;

    // 线程创建失败（资源耗尽）时 std::thread 会抛 std::system_error。
    // 不接住的话就是 std::terminate —— 整个截图程序直接没了，
    // 而这只是一次可选的后台识别。
    // 线程创建包在 try 里，detach 必须放在 try 外面（理由见 detach 处）。
    std::thread worker;
    try {
        worker = std::thread([cropped, request, completionWindow, callback]() {
            // GDI+ 需要每个使用它的线程各自初始化。
            ULONG_PTR gdiplusToken = 0;
            Gdiplus::GdiplusStartupInput gdiplusInput;
            const bool gdiplusReady =
                Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr) == Gdiplus::Ok;

            auto* payload = new OcrCompletionPayload();
            payload->request = request;
            payload->callback = callback;
            payload->result = RunOcrPipeline(cropped);

            if (gdiplusReady) {
                Gdiplus::GdiplusShutdown(gdiplusToken);
            }

            // 先登记再投递：消息被处理之前进程退出的话，退出流程会把
            // 它回收掉（见 ReleasePendingOcrCompletions）。
            RegisterPendingPayload(payload);
            if (!PostMessageW(
                    completionWindow,
                    WM_SNAPLITE_OCR_COMPLETE,
                    0,
                    reinterpret_cast<LPARAM>(payload))) {
                // 投递失败：没人会来处理这条消息，payload 由这里回收。
                if (TakePendingPayload(payload)) {
                    delete payload;
                }
            }
        });
    } catch (const std::system_error&) {
        // 线程没起来，worker 就没跑，位图还是调用方的，在这里释放。
        DeleteObject(cropped);
        return false;
    }
    // detach 必须放在 try 外面：
    //  - 线程一旦创建成功就已经在跑，RunOcrPipeline 会**接管并释放** cropped。
    //    detach 抛异常时若在 catch 里再 DeleteObject(cropped)，
    //    就是同一块位图被释放两次（double free）。
    //  - try 作用域结束时 worker 的析构函数发现线程还 joinable，
    //    会直接 std::terminate()——整个截图程序当场没了。
    try {
        worker.detach();
    } catch (const std::system_error&) {
        // detach 只在"线程不可 join"时抛，理论上到不了这里。
        //
        // 关键：**绝不能**在这里改用 join() 兜底。上一轮验收实测：
        // join 在这种状态下会抛 invalid argument，而调用点（OCR 动作）
        // 没有 try/catch，异常一路穿到 Win32 消息处理 → std::terminate，
        // 整个截图程序当场没了——兜底代码自己把进程杀了。
        //
        // detach 抛了，说明这个 std::thread **已经不是 joinable 状态**
        // （线程已经结束、或者它本来就不是 joinable 的），析构函数因此
        // 不会 terminate。这里什么都不用做。
    }
    return true;
}

void ReleasePendingOcrCompletions() {
    // 退出时回收还没被处理的完成消息。摘走即拥有：UI 线程随后就算
    // 收到那条消息，也摘不到了（TakePendingPayload 返回 nullptr），
    // 不会出现二次释放。
    std::vector<OcrCompletionPayload*> pending;
    {
        std::lock_guard<std::mutex> lock(g_pendingPayloadMutex);
        pending.swap(g_pendingPayloads);
    }
    for (OcrCompletionPayload* payload : pending) {
        delete payload;
    }
}

bool CopyUnicodeTextToClipboard(HWND owner, const std::wstring& text) {
    if (text.empty()) return false;
    // 剪贴板可能正被别的进程占用，走带重试的入口，避免"点了复制没反应"。
    if (!OpenClipboardWithRetry(owner)) return false;

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
