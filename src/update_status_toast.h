#pragma once

#include <windows.h>

#include <string>

namespace snaplite {

enum class UpdateStatusToastState {
    Checking,
    UpToDate,
    Available,
    CheckFailed,
    Downloading,
    DownloadFailed,
};

enum class UpdateStatusToastAction : WPARAM {
    Check = 1,
    Download = 2,
};

inline constexpr UINT WM_SNAPLITE_UPDATE_TOAST_ACTION = WM_APP + 6;

void ShowUpdateStatusToast(
    HWND owner,
    UpdateStatusToastState state,
    const std::wstring& currentVersion,
    const std::wstring& latestVersion = {},
    const std::wstring& detail = {});
void CloseUpdateStatusToast(HWND owner);

}  // namespace snaplite
