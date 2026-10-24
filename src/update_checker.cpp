#include "update_checker.h"

#include "update_version.h"

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace snaplite {
namespace {

constexpr wchar_t kSettingsKey[] = L"Software\\Snap-Lite";
constexpr wchar_t kLastUpdateCheckValue[] = L"LastUpdateCheck";
constexpr wchar_t kCachedLatestVersionValue[] = L"CachedLatestVersion";
constexpr std::size_t kMetadataLimit = 128 * 1024;
constexpr std::size_t kInstallerLimit = 100 * 1024 * 1024;
constexpr wchar_t kUserAgent[] = L"Snap-Lite/" SNAP_LITE_VERSION;

class InternetHandle {
public:
    explicit InternetHandle(HINTERNET handle = nullptr) : handle_(handle) {}
    ~InternetHandle() {
        if (handle_) WinHttpCloseHandle(handle_);
    }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    HINTERNET get() const { return handle_; }

private:
    HINTERNET handle_{};
};

struct HttpResponse {
    DWORD status{};
    std::wstring redirect;
    std::vector<BYTE> body;
};

std::optional<std::wstring> ReadHeader(HINTERNET request, DWORD header) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(
        request,
        header,
        WINHTTP_HEADER_NAME_BY_INDEX,
        nullptr,
        &bytes,
        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(wchar_t)) {
        return std::nullopt;
    }

    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(
            request,
            header,
            WINHTTP_HEADER_NAME_BY_INDEX,
            buffer.data(),
            &bytes,
            WINHTTP_NO_HEADER_INDEX)) {
        return std::nullopt;
    }
    return std::wstring(buffer.data());
}

std::optional<HttpResponse> GetUrlOnce(const std::wstring& url, std::size_t maxBytes) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) return std::nullopt;

    const std::wstring scheme(parts.lpszScheme, parts.dwSchemeLength);
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) {
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    if (path.empty()) path = L"/";

    InternetHandle session(WinHttpOpen(
        kUserAgent,
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session.get()) return std::nullopt;
    WinHttpSetTimeouts(session.get(), 5000, 5000, 10000, 10000);

    InternetHandle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    if (!connection.get()) return std::nullopt;

    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    InternetHandle request(WinHttpOpenRequest(
        connection.get(),
        L"GET",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags));
    if (!request.get()) return std::nullopt;

    WinHttpAddRequestHeaders(
        request.get(),
        L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n",
        static_cast<DWORD>(-1),
        WINHTTP_ADDREQ_FLAG_ADD);
    if (!WinHttpSendRequest(
            request.get(),
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        return std::nullopt;
    }

    HttpResponse response;
    DWORD statusBytes = sizeof(response.status);
    if (!WinHttpQueryHeaders(
            request.get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &response.status,
            &statusBytes,
            WINHTTP_NO_HEADER_INDEX)) {
        return std::nullopt;
    }

    if (response.status >= 300 && response.status < 400) {
        auto location = ReadHeader(request.get(), WINHTTP_QUERY_LOCATION);
        if (!location) return std::nullopt;
        response.redirect = std::move(*location);
        return response;
    }
    if (response.status != 200) return response;

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) return std::nullopt;
        if (available == 0) break;
        if (response.body.size() + available > maxBytes) return std::nullopt;

        const std::size_t offset = response.body.size();
        response.body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), response.body.data() + offset, available, &read)) {
            return std::nullopt;
        }
        response.body.resize(offset + read);
    }
    return response;
}

std::optional<HttpResponse> GetUrl(const std::wstring& url, std::size_t maxBytes, int redirects = 0) {
    if (redirects > 5) return std::nullopt;
    auto response = GetUrlOnce(url, maxBytes);
    if (!response || response->redirect.empty()) return response;

    std::wstring next = response->redirect;
    if (next.find(L"://") == std::wstring::npos) {
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwSchemeLength = static_cast<DWORD>(-1);
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) return std::nullopt;
        const std::wstring scheme(parts.lpszScheme, parts.dwSchemeLength);
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        next = scheme + L"://" + host + (next.starts_with(L"/") ? next : L"/" + next);
    }
    return GetUrl(next, maxBytes, redirects + 1);
}

std::optional<std::string> ExtractTagName(const std::vector<BYTE>& body) {
    const std::string_view json(reinterpret_cast<const char*>(body.data()), body.size());
    const std::size_t key = json.find("\"tag_name\"");
    if (key == std::string_view::npos) return std::nullopt;
    const std::size_t colon = json.find(':', key + 10);
    if (colon == std::string_view::npos) return std::nullopt;
    std::size_t cursor = colon + 1;
    while (cursor < json.size() && std::isspace(static_cast<unsigned char>(json[cursor]))) ++cursor;
    if (cursor >= json.size() || json[cursor++] != '"') return std::nullopt;

    std::string tag;
    while (cursor < json.size() && json[cursor] != '"') {
        if (json[cursor] == '\\' || static_cast<unsigned char>(json[cursor]) >= 0x80) {
            return std::nullopt;
        }
        tag.push_back(json[cursor++]);
    }
    if (cursor >= json.size() || tag.empty()) return std::nullopt;
    return tag;
}

std::optional<std::wstring> Utf8ToWide(const std::vector<BYTE>& bytes) {
    if (bytes.empty() || bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    const char* data = reinterpret_cast<const char*>(bytes.data());
    int length = static_cast<int>(bytes.size());
    if (length >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF) {
        data += 3;
        length -= 3;
    }
    const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, length, nullptr, 0);
    if (chars <= 0) return std::nullopt;
    std::wstring result(static_cast<std::size_t>(chars), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, length, result.data(), chars) != chars) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::string> ExpectedInstallerHash(
    const std::vector<BYTE>& sums,
    const std::string& expectedFile) {
    const std::string text(reinterpret_cast<const char*>(sums.data()), sums.size());
    std::size_t lineStart = 0;
    while (lineStart < text.size()) {
        std::size_t lineEnd = text.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = text.size();
        std::string_view line(text.data() + lineStart, lineEnd - lineStart);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.size() >= 66) {
            const std::string hash(line.substr(0, 64));
            std::string_view name = line.substr(64);
            while (!name.empty() && (name.front() == ' ' || name.front() == '\t' || name.front() == '*')) {
                name.remove_prefix(1);
            }
            const bool validHash = std::all_of(hash.begin(), hash.end(), [](unsigned char c) {
                return std::isxdigit(c) != 0;
            });
            if (validHash && name == expectedFile) return hash;
        }
        lineStart = lineEnd + 1;
    }
    return std::nullopt;
}

std::optional<std::string> Sha256Hex(const std::vector<BYTE>& bytes) {
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        return std::nullopt;
    }

    DWORD objectLength = 0;
    DWORD hashLength = 0;
    DWORD returned = 0;
    const bool propertiesRead =
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
                          sizeof(objectLength), &returned, 0) >= 0 &&
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength),
                          sizeof(hashLength), &returned, 0) >= 0;
    if (!propertiesRead || bytes.size() > std::numeric_limits<ULONG>::max()) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return std::nullopt;
    }

    std::vector<UCHAR> object(objectLength);
    std::vector<UCHAR> digest(hashLength);
    BCRYPT_HASH_HANDLE hash{};
    const bool hashed = BCryptCreateHash(
                            algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) >= 0 &&
                        BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()),
                                       static_cast<ULONG>(bytes.size()), 0) >= 0 &&
                        BCryptFinishHash(hash, digest.data(), hashLength, 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!hashed) return std::nullopt;

    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const UCHAR byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0F]);
    }
    return result;
}

UpdateDownloadResult FailedDownload(std::wstring message) {
    UpdateDownloadResult result;
    result.error = std::move(message);
    return result;
}

}  // namespace

bool IsUpdateCheckDueToday() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const DWORD today = now.wYear * 10000u + now.wMonth * 100u + now.wDay;

    DWORD lastCheck = 0;
    DWORD size = sizeof(lastCheck);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, kLastUpdateCheckValue,
                     RRF_RT_REG_DWORD, nullptr, &lastCheck, &size) != ERROR_SUCCESS) {
        return true;
    }
    return lastCheck != today;
}

void RecordUpdateCheckToday() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const DWORD today = now.wYear * 10000u + now.wMonth * 100u + now.wDay;

    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    RegSetValueExW(key, kLastUpdateCheckValue, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&today), sizeof(today));
    RegCloseKey(key);
}

std::wstring CachedLatestVersion() {
    wchar_t version[32]{};
    DWORD bytes = sizeof(version);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, kCachedLatestVersionValue,
                     RRF_RT_REG_SZ, nullptr, version, &bytes) != ERROR_SUCCESS) {
        return {};
    }
    return version;
}

void CacheLatestVersion(const std::wstring& version) {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    RegSetValueExW(key, kCachedLatestVersionValue, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(version.c_str()),
                   static_cast<DWORD>((version.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

UpdateCheckResult CheckLatestRelease() {
    constexpr wchar_t kLatestReleaseUrl[] =
        L"https://api.github.com/repos/elaysia-feng/Snap-Lite/releases/latest";
    const auto response = GetUrl(kLatestReleaseUrl, kMetadataLimit);
    if (!response || response->status != 200) return {};

    const auto tag = ExtractTagName(response->body);
    if (!tag) return {};
    std::vector<BYTE> tagBytes(tag->begin(), tag->end());
    const auto version = Utf8ToWide(tagBytes);
    if (!version || !CompareReleaseVersions(*version, SNAP_LITE_VERSION)) return {};

    UpdateCheckResult result;
    result.succeeded = true;
    result.latestVersion = *version;
    if (!result.latestVersion.empty() && result.latestVersion.front() == L'v') {
        result.latestVersion.erase(result.latestVersion.begin());
    }
    return result;
}

UpdateDownloadResult DownloadVerifiedUpdate(const std::wstring& version) {
    const auto comparison = CompareReleaseVersions(version, SNAP_LITE_VERSION);
    if (!comparison || *comparison <= 0) {
        return FailedDownload(L"更新版本号无效或不高于当前版本");
    }

    const std::wstring tag = L"v" + version;
    const std::wstring fileName = L"SnapLite-Setup-" + version + L".exe";
    const std::wstring rawBase = L"https://raw.githubusercontent.com/elaysia-feng/Snap-Lite/" + tag;
    const std::wstring releaseBase = L"https://github.com/elaysia-feng/Snap-Lite/releases/download/" + tag;

    const auto notesResponse = GetUrl(rawBase + L"/.release/" + tag + L".txt", kMetadataLimit);
    if (!notesResponse || notesResponse->status != 200) {
        return FailedDownload(L"读取更新说明失败，请稍后重试");
    }
    const auto notes = Utf8ToWide(notesResponse->body);
    if (!notes) return FailedDownload(L"更新说明编码无效");

    const auto sumsResponse = GetUrl(releaseBase + L"/SHA256SUMS.txt", 16 * 1024);
    if (!sumsResponse || sumsResponse->status != 200) {
        return FailedDownload(L"下载校验文件失败");
    }
    const auto expectedHash = ExpectedInstallerHash(
        sumsResponse->body,
        std::string(fileName.begin(), fileName.end()));
    if (!expectedHash) return FailedDownload(L"校验文件中没有安装包 SHA-256");

    const auto installerResponse = GetUrl(releaseBase + L"/" + fileName, kInstallerLimit);
    if (!installerResponse || installerResponse->status != 200 || installerResponse->body.empty()) {
        return FailedDownload(L"下载安装包失败，请检查网络");
    }
    const auto actualHash = Sha256Hex(installerResponse->body);
    if (!actualHash || _stricmp(actualHash->c_str(), expectedHash->c_str()) != 0) {
        return FailedDownload(L"安装包 SHA-256 校验失败，已取消更新");
    }

    std::error_code error;
    const std::filesystem::path installerPath =
        std::filesystem::temp_directory_path(error) / fileName;
    if (error) return FailedDownload(L"无法定位临时目录");

    std::ofstream file(installerPath, std::ios::binary | std::ios::trunc);
    if (!file || !file.write(
                     reinterpret_cast<const char*>(installerResponse->body.data()),
                     static_cast<std::streamsize>(installerResponse->body.size()))) {
        return FailedDownload(L"无法保存更新安装包");
    }
    file.close();
    if (!file) return FailedDownload(L"写入更新安装包失败");

    UpdateDownloadResult result;
    result.succeeded = true;
    result.installerPath = installerPath;
    result.releaseNotes = *notes;
    return result;
}

}  // namespace snaplite
