#pragma once

#include "snip_window.h"

#include <windows.h>
#include <dwmapi.h>

namespace snaplite::detail {

inline BOOL GetAccurateWindowRect(HWND hwnd, LPRECT rect) {
    if (!hwnd || !rect) {
        return FALSE;
    }

    // Child controls should keep their regular client-derived window bounds.
    // For top-level windows, prefer the DWM visible frame so Win10/11's
    // invisible resize border is not included in auto-snapped selections.
    if (GetAncestor(hwnd, GA_ROOT) == hwnd) {
        RECT visible{};
        const HRESULT result = DwmGetWindowAttribute(
            hwnd,
            DWMWA_EXTENDED_FRAME_BOUNDS,
            &visible,
            sizeof(visible));

        if (SUCCEEDED(result) &&
            visible.right > visible.left &&
            visible.bottom > visible.top) {
            *rect = visible;
            return TRUE;
        }
    }

    return ::GetWindowRect(hwnd, rect);
}

inline void HideEditorToolbarAndRestoreBackground(HWND snipHwnd) {
    if (!snipHwnd) return;

    // Only selection creation / move / resize should hide the toolbar. Drawing
    // annotations may also use mouse capture, but in that case the active tool
    // is non-negative and the toolbar should stay available.
    auto* snip = reinterpret_cast<SnipWindow*>(GetWindowLongPtrW(snipHwnd, GWLP_USERDATA));
    if (!snip || snip->UiActiveTool() >= 0) return;

    HWND toolbar = FindWindowExW(
        snipHwnd,
        nullptr,
        L"SnapLiteEditorToolbarChild",
        nullptr);
    if (!toolbar || !IsWindowVisible(toolbar)) return;

    RECT screenRect{};
    if (!::GetWindowRect(toolbar, &screenRect)) {
        ShowWindow(toolbar, SW_HIDE);
        return;
    }

    POINT topLeft{screenRect.left, screenRect.top};
    POINT bottomRight{screenRect.right, screenRect.bottom};
    ScreenToClient(snipHwnd, &topLeft);
    ScreenToClient(snipHwnd, &bottomRight);
    RECT dirty{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};

    // Hide before moving the selection. Because the parent uses WS_CLIPCHILDREN,
    // explicitly repaint the area that the child used to cover; otherwise the
    // old white toolbar surface can remain visible for several frames and looks
    // like a trail/ghost when the mouse moves quickly.
    ShowWindow(toolbar, SW_HIDE);
    InflateRect(&dirty, 3, 3);
    RedrawWindow(
        snipHwnd,
        &dirty,
        nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOCHILDREN);
}

inline HWND SetCaptureForSnip(HWND hwnd) {
    HideEditorToolbarAndRestoreBackground(hwnd);
    return ::SetCapture(hwnd);
}

}  // namespace snaplite::detail

// This header is force-included only for snip_window.cpp. It transparently
// replaces a few Win32 calls inside the legacy capture implementation while
// leaving every other translation unit untouched.
#define GetWindowRect(hwnd, rect) \
    snaplite::detail::GetAccurateWindowRect((hwnd), (rect))
#define SetCapture(hwnd) \
    snaplite::detail::SetCaptureForSnip((hwnd))
