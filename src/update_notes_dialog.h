#pragma once

#include <windows.h>

#include <string>

namespace snaplite {

enum class UpdateNotesDialogChoice {
    Install,
    Later,
    Failed,
};

// 显示固定尺寸、支持滚动的更新说明窗口。
// 返回用户选择；窗口无法创建时返回 Failed。
UpdateNotesDialogChoice ShowUpdateNotesDialog(
    HWND owner,
    const std::wstring& version,
    const std::wstring& notes);

}  // namespace snaplite
