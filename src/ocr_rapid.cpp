#include "ocr.h"

// Keep the Windows OCR implementation as a fallback for users who run the
// standalone SnapLite.exe without the packaged RapidOCR companion.
#define ExtractTextFromBitmapRegion ExtractTextFromBitmapRegionWindowsFallback
#include "ocr_v2.cpp"
#undef ExtractTextFromBitmapRegion

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace snaplite {
namespace {

std::filesystem::path ExecutableDirectory() {
    std::vector<wchar_t> buffer(1024);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size() - 1) {
            return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring QuoteArgument(const std::filesystem::path& path) {
    return L"\"" + path.wstring() + L"\"";
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};

    const char* data = value.data();
    int size = static_cast<int>(value.size());
    if (size >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF) {
        data += 3;
        size -= 3;
    }
    if (size <= 0) return {};

    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, size, nullptr, 0);
    if (needed <= 0) return {};

    std::wstring result(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, size, result.data(), needed);
    return result;
}

void TrimTrailingWhitespace(std::wstring& text) {
    while (!text.empty()) {
        const wchar_t ch = text.back();
        if (ch != L' ' && ch != L'\t' && ch != L'\r' && ch != L'\n') break;
        text.pop_back();
    }
}

bool ReadUtf8File(const std::filesystem::path& path, std::wstring& text) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    const std::string bytes(
        (std::istreambuf_iterator<char>(stream)),
        std::istreambuf_iterator<char>());
    text = Utf8ToWide(bytes);
    TrimTrailingWhitespace(text);
    return true;
}

OcrResult TryRapidOcr(HBITMAP bitmap, const RECT& region) {
    OcrResult output;

    const std::filesystem::path worker = ExecutableDirectory() / L"SnapLiteOCR.exe";
    if (!std::filesystem::exists(worker)) {
        output.error = L"未找到 SnapLiteOCR.exe";
        return output;
    }

    HBITMAP cropped = CropBitmap(bitmap, region);
    if (!cropped) {
        output.error = L"无法读取当前截图选区。";
        return output;
    }

    const std::filesystem::path imagePath = MakeTempOcrPath(90);
    std::filesystem::path outputPath = imagePath;
    outputPath.replace_extension(L".txt");

    const bool saved = SaveBitmapAsPng(cropped, imagePath);
    DeleteObject(cropped);
    if (!saved) {
        output.error = L"无法准备 RapidOCR 图像。";
        return output;
    }

    std::wstring command = QuoteArgument(worker) + L" --input " + QuoteArgument(imagePath) +
                           L" --output " + QuoteArgument(outputPath);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    const BOOL started = CreateProcessW(
        worker.c_str(),
        mutableCommand.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        ExecutableDirectory().c_str(),
        &startup,
        &process);

    if (!started) {
        std::error_code ignored;
        std::filesystem::remove(imagePath, ignored);
        output.error = L"无法启动 SnapLiteOCR.exe";
        return output;
    }

    const DWORD waitResult = WaitForSingleObject(process.hProcess, 45000);
    DWORD exitCode = 1;
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 124);
        WaitForSingleObject(process.hProcess, 2000);
        output.error = L"RapidOCR 识别超时。";
    } else if (waitResult == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &exitCode) && exitCode == 0) {
        std::wstring text;
        if (ReadUtf8File(outputPath, text)) {
            output.success = true;
            output.text = std::move(text);
        } else {
            output.error = L"RapidOCR 没有返回可读取的结果。";
        }
    } else {
        output.error = L"RapidOCR 识别进程执行失败。";
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    std::error_code ignored;
    std::filesystem::remove(imagePath, ignored);
    std::filesystem::remove(outputPath, ignored);
    return output;
}

}  // namespace

OcrResult ExtractTextFromBitmapRegion(HBITMAP bitmap, const RECT& region) {
    OcrResult rapid = TryRapidOcr(bitmap, region);
    if (rapid.success) return rapid;

    OcrResult fallback = ExtractTextFromBitmapRegionWindowsFallback(bitmap, region);
    if (!fallback.success && !rapid.error.empty()) {
        if (!fallback.error.empty()) fallback.error += L"\n\n";
        fallback.error += L"RapidOCR：";
        fallback.error += rapid.error;
    }
    return fallback;
}

}  // namespace snaplite
