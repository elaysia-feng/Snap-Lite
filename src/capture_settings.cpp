#include "capture.h"

#include <windows.h>

#include <filesystem>

namespace snaplite {
namespace {

constexpr wchar_t kSettingsKey[] = L"Software\\Snap-Lite";
constexpr wchar_t kScreenshotDirectoryValue[] = L"ScreenshotDirectory";

std::filesystem::path ReadConfiguredDirectory() {
    wchar_t buffer[32768]{};
    DWORD bytes = sizeof(buffer);
    const LONG result = RegGetValueW(
        HKEY_CURRENT_USER,
        kSettingsKey,
        kScreenshotDirectoryValue,
        RRF_RT_REG_SZ,
        nullptr,
        buffer,
        &bytes);
    if (result != ERROR_SUCCESS || buffer[0] == L'\0') {
        return {};
    }
    return std::filesystem::path(buffer);
}

}  // namespace

std::filesystem::path ConfiguredScreenshotDirectory() {
    std::filesystem::path directory = ReadConfiguredDirectory();
    if (directory.empty()) {
        directory = ScreenshotDirectory();
    }

    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return ScreenshotDirectory();
    }
    return directory;
}

bool SetScreenshotDirectory(const std::filesystem::path& directory) {
    if (directory.empty()) {
        return false;
    }

    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return false;
    }

    HKEY key{};
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER,
            kSettingsKey,
            0,
            nullptr,
            0,
            KEY_SET_VALUE,
            nullptr,
            &key,
            nullptr) != ERROR_SUCCESS) {
        return false;
    }

    const std::wstring value = directory.wstring();
    const LONG result = RegSetValueExW(
        key,
        kScreenshotDirectoryValue,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

std::filesystem::path NextConfiguredScreenshotPath() {
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

    return ConfiguredScreenshotDirectory() / filename;
}

}  // namespace snaplite
