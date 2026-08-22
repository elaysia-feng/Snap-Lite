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

}  // namespace snaplite::detail

// Force-included only for snip_window.cpp so legacy element detection uses the
// visible DWM frame without changing every call site.
#define GetWindowRect(hwnd, rect) \
    snaplite::detail::GetAccurateWindowRect((hwnd), (rect))
