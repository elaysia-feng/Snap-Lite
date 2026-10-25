#pragma once

#include <filesystem>
#include <string>

namespace snaplite {

struct UpdateCheckResult {
    bool succeeded{};
    std::wstring latestVersion;
    std::wstring error;
};

struct UpdateDownloadResult {
    bool succeeded{};
    std::filesystem::path installerPath;
    std::wstring releaseNotes;
    std::wstring error;
};

bool IsUpdateCheckDueToday();
void RecordUpdateCheckToday();
std::wstring CachedLatestVersion();
void CacheLatestVersion(const std::wstring& version);
UpdateCheckResult CheckLatestRelease();
UpdateDownloadResult DownloadVerifiedUpdate(const std::wstring& version);

}  // namespace snaplite
