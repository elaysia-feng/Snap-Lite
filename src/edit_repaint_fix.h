#pragma once

#include <windows.h>
#include <commctrl.h>
#include <cwchar>
#include <iterator>

namespace snaplite::editrepaintfix {

constexpr UINT_PTR kTransparentEditRefreshSubclassId = 0x534C14F2;

inline bool IsEditControl(HWND hwnd) {
    wchar_t className[32]{};
    if (!hwnd || !GetClassNameW(hwnd, className, static_cast<int>(std::size(className)))) {
        return false;
    }
    return _wcsicmp(className, L"Edit") == 0;
}

inline void RefreshTransparentEdit(HWND edit) {
    if (!edit || !IsWindow(edit)) return;
    HWND host = GetParent(edit);
    if (!host || !IsWindow(host)) return;

    DWORD selStart = 0;
    DWORD selEnd = 0;
    SendMessageW(edit, EM_GETSEL,
                 reinterpret_cast<WPARAM>(&selStart),
                 reinterpret_cast<LPARAM>(&selEnd));

    // A transparent EDIT never paints an opaque background, so shrinking text
    // can leave the old glyph pixels behind. Temporarily remove the EDIT, let
    // its annotation host restore the captured pixels, then repaint the EDIT
    // with the already-updated text.
    ShowWindow(edit, SW_HIDE);
    RedrawWindow(host, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_NOCHILDREN);
    ShowWindow(edit, SW_SHOWNA);
    RedrawWindow(edit, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    SetFocus(edit);
    SendMessageW(edit, EM_SETSEL, selStart, selEnd);
}

inline LRESULT CALLBACK TransparentEditRefreshProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR, DWORD_PTR) {

    const bool mayShrink =
        message == WM_CHAR || message == WM_CUT || message == WM_CLEAR ||
        message == WM_SETTEXT ||
        (message == WM_KEYDOWN && wParam == VK_DELETE);

    int beforeLength = -1;
    if (mayShrink && IsWindow(hwnd)) {
        beforeLength = GetWindowTextLengthW(hwnd);
    }

    const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);

    if (mayShrink && IsWindow(hwnd) && beforeLength >= 0) {
        const int afterLength = GetWindowTextLengthW(hwnd);
        if (afterLength < beforeLength) {
            RefreshTransparentEdit(hwnd);
        }
    }

    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, TransparentEditRefreshProc,
                             kTransparentEditRefreshSubclassId);
    }
    return result;
}

inline BOOL SetWindowSubclassWithTransparentEditRefresh(
    HWND hwnd, SUBCLASSPROC proc, UINT_PTR id, DWORD_PTR refData) {
    const BOOL ok = ::SetWindowSubclass(hwnd, proc, id, refData);
    if (ok && IsEditControl(hwnd)) {
        ::SetWindowSubclass(hwnd, TransparentEditRefreshProc,
                            kTransparentEditRefreshSubclassId, 0);
    }
    return ok;
}

}  // namespace snaplite::editrepaintfix

#define SetWindowSubclass(...) \
    snaplite::editrepaintfix::SetWindowSubclassWithTransparentEditRefresh(__VA_ARGS__)
