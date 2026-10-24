#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace snaplite {

inline std::optional<std::array<std::uint32_t, 3>> ParseReleaseVersion(
    std::wstring_view version) {
    if (!version.empty() && (version.front() == L'v' || version.front() == L'V')) {
        version.remove_prefix(1);
    }

    std::array<std::uint32_t, 3> parts{};
    std::size_t cursor = 0;
    for (std::size_t part = 0; part < parts.size(); ++part) {
        const std::size_t start = cursor;
        std::uint32_t value = 0;
        while (cursor < version.size() && version[cursor] >= L'0' && version[cursor] <= L'9') {
            const std::uint32_t digit = static_cast<std::uint32_t>(version[cursor] - L'0');
            if (value > (std::numeric_limits<std::uint32_t>::max() - digit) / 10) {
                return std::nullopt;
            }
            value = value * 10 + digit;
            ++cursor;
        }
        if (cursor == start || (cursor - start > 1 && version[start] == L'0')) {
            return std::nullopt;
        }
        parts[part] = value;

        if (part + 1 < parts.size()) {
            if (cursor >= version.size() || version[cursor] != L'.') return std::nullopt;
            ++cursor;
        }
    }

    if (cursor != version.size()) return std::nullopt;
    return parts;
}

inline std::optional<int> CompareReleaseVersions(
    std::wstring_view left,
    std::wstring_view right) {
    const auto leftParts = ParseReleaseVersion(left);
    const auto rightParts = ParseReleaseVersion(right);
    if (!leftParts || !rightParts) return std::nullopt;
    if (*leftParts < *rightParts) return -1;
    if (*leftParts > *rightParts) return 1;
    return 0;
}

}  // namespace snaplite
