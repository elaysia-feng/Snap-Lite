#pragma once

#include "snip_window.h"

#include <windows.h>
#include <algorithm>
#include <cwchar>

namespace snaplite::toolbarcompact {

constexpr int kPrimaryOnlyHeight = 42;
constexpr int kExpandedHeight = 42 + 42 + 24;

inline bool IsEditorToolbar(HWND hwnd) {
    if (!hwnd) return false;
    wchar_t className[96]{};
    if (!GetClassNameW(hwnd, className, static_cast<int>(std::size(className)))) return false;
    return wcscmp(className, L"SnapLiteEditorToolbarChild") == 0;
}

inline SnipWindow* SnipFromToolbar(HWND toolbar) {
    HWND parent = GetParent(toolbar);
    if (!parent) return nullptr;
    return reinterpret_cast<SnipWindow*>(GetWindowLongPtrW(parent, GWLP_USERDATA));
}

inline bool HasSecondaryOptions(HWND toolbar) {
    SnipWindow* snip = SnipFromToolbar(toolbar);
    if (!snip) return false;

    // Shape, arrow, pen and text have real secondary controls.
    // Select and mosaic stay compact until they actually gain parameters.
    const int tool = snip->UiActiveTool();
    return tool == 0 || tool == 1 || tool == 2 || tool == 4;
}

inline int DesiredHeight(HWND toolbar) {
    return HasSecondaryOptions(toolbar) ? kExpandedHeight : kPrimaryOnlyHeight;
}

inline int ClampToolbarY(HWND toolbar, int y, int height) {
    HWND parent = GetParent(toolbar);
    if (!parent) return y;
    RECT client{};
    if (!GetClientRect(parent, &client)) return y;
    return std::clamp(y, 6, std::max(6, static_cast<int>(client.bottom) - height - 6));
}

inline void ApplyRoundedRegion(HWND hwnd) {
    if (!IsEditorToolbar(hwnd)) return;
    RECT client{};
    if (!GetClientRect(hwnd, &client)) return;
    const int width = std::max(1L, client.right - client.left);
    const int height = std::max(1L, client.bottom - client.top);
    const int radius = height <= kPrimaryOnlyHeight ? 18 : 16;
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius, radius);
    if (!region) return;
    if (SetWindowRgn(hwnd, region, TRUE) == 0) {
        DeleteObject(region);
    }
}

inline BOOL SetWindowPosCompact(
    HWND hwnd,
    HWND insertAfter,
    int x,
    int y,
    int cx,
    int cy,
    UINT flags) {

    if (IsEditorToolbar(hwnd)) {
        const int requestedHeight = cy;
        const int desiredHeight = DesiredHeight(hwnd);

        if (!(flags & SWP_NOSIZE)) {
            cy = desiredHeight;
        }

        // The original positioning code calculates the top coordinate using the
        // fully expanded toolbar height. When the toolbar is above the selection,
        // keep its bottom edge anchored while collapsing it.
        if (!(flags & SWP_NOMOVE) && requestedHeight > desiredHeight) {
            SnipWindow* snip = SnipFromToolbar(hwnd);
            if (snip) {
                const RECT selection = snip->UiSelectionRect();
                if (y < selection.top) {
                    y += requestedHeight - desiredHeight;
                }
            }
        }

        if (!(flags & SWP_NOMOVE)) {
            y = ClampToolbarY(hwnd, y, cy);
        }
    }

    const BOOL result = ::SetWindowPos(hwnd, insertAfter, x, y, cx, cy, flags);
    if (result && IsEditorToolbar(hwnd)) ApplyRoundedRegion(hwnd);
    return result;
}

inline void ResizeToolbarForCurrentTool(HWND toolbar) {
    if (!IsEditorToolbar(toolbar)) return;

    RECT rect{};
    if (!GetWindowRect(toolbar, &rect)) return;

    HWND parent = GetParent(toolbar);
    if (!parent) return;

    POINT origin{rect.left, rect.top};
    ScreenToClient(parent, &origin);

    const int currentHeight = std::max(1L, rect.bottom - rect.top);
    const int desiredHeight = DesiredHeight(toolbar);
    if (currentHeight == desiredHeight) {
        ApplyRoundedRegion(toolbar);
        return;
    }

    SnipWindow* snip = SnipFromToolbar(toolbar);
    int y = origin.y;
    if (snip) {
        const RECT selection = snip->UiSelectionRect();
        if (origin.y < selection.top) {
            y += currentHeight - desiredHeight;
        }
    }
    y = ClampToolbarY(toolbar, y, desiredHeight);

    if (::SetWindowPos(
            toolbar,
            HWND_TOP,
            origin.x,
            y,
            rect.right - rect.left,
            desiredHeight,
            SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
        ApplyRoundedRegion(toolbar);
    }
}

inline BOOL InvalidateRectCompact(HWND hwnd, const RECT* rect, BOOL erase) {
    if (IsEditorToolbar(hwnd)) {
        // Category changes call InvalidateRect immediately. Resize here as well
        // so the secondary panel appears/disappears in the same interaction,
        // without waiting for a later parent mouse-move/paint event.
        ResizeToolbarForCurrentTool(hwnd);
    }
    return ::InvalidateRect(hwnd, rect, erase);
}

}  // namespace snaplite::toolbarcompact

#define SetWindowPos(...) snaplite::toolbarcompact::SetWindowPosCompact(__VA_ARGS__)
#define InvalidateRect(...) snaplite::toolbarcompact::InvalidateRectCompact(__VA_ARGS__)
