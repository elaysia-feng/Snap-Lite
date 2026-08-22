#pragma once

#include <windows.h>
#include <cwchar>
#include <iterator>

namespace snaplite::dragpaintfix {

inline bool IsSnipWindow(HWND hwnd) {
    if (!hwnd) return false;
    wchar_t className[64]{};
    if (!GetClassNameW(hwnd, className, static_cast<int>(std::size(className)))) {
        return false;
    }
    return _wcsicmp(className, L"SnapLiteSnipWindow") == 0;
}

inline BOOL InvalidateRectImmediateDuringDrag(HWND hwnd, const RECT* rect, BOOL erase) {
    // During a captured drag, queueing WM_PAINT lets DWM briefly present old
    // selection frames when the mouse moves faster than the paint queue. Paint
    // the latest state synchronously instead. Outside an active drag, keep the
    // normal Win32 invalidation behavior.
    if (hwnd && GetCapture() == hwnd && IsSnipWindow(hwnd)) {
        const UINT flags = RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_NOCHILDREN;
        return RedrawWindow(hwnd, rect, nullptr, flags);
    }
    return ::InvalidateRect(hwnd, rect, erase);
}

}  // namespace snaplite::dragpaintfix

#define InvalidateRect(...) \
    snaplite::dragpaintfix::InvalidateRectImmediateDuringDrag(__VA_ARGS__)
