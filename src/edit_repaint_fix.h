#pragma once

#include "snip_window.h"

#include <windows.h>
#include <commctrl.h>
#include <cwchar>
#include <iterator>
#include <string>

namespace snaplite::editrepaintfix {

constexpr UINT_PTR kTransparentEditRefreshSubclassId = 0x534C14F2;

inline bool IsEditControl(HWND hwnd) {
    wchar_t className[32]{};
    if (!hwnd || !GetClassNameW(hwnd, className, static_cast<int>(std::size(className)))) {
        return false;
    }
    return _wcsicmp(className, L"Edit") == 0;
}

inline SnipWindow* FindSnipWindow(HWND edit, HWND* hostOut = nullptr, HWND* snipHwndOut = nullptr) {
    if (!edit) return nullptr;
    HWND host = GetParent(edit);
    HWND snipHwnd = host ? GetParent(host) : nullptr;
    if (hostOut) *hostOut = host;
    if (snipHwndOut) *snipHwndOut = snipHwnd;
    if (!host || !snipHwnd) return nullptr;

    wchar_t className[64]{};
    if (!GetClassNameW(snipHwnd, className, static_cast<int>(std::size(className))) ||
        _wcsicmp(className, L"SnapLiteSnipWindow") != 0) {
        return nullptr;
    }
    return reinterpret_cast<SnipWindow*>(GetWindowLongPtrW(snipHwnd, GWLP_USERDATA));
}

inline bool PaintCaptureBackground(HWND edit, HDC dc) {
    HWND host{};
    HWND snipHwnd{};
    SnipWindow* snip = FindSnipWindow(edit, &host, &snipHwnd);
    if (!snip || !dc || !host || !snipHwnd) return false;

    HBITMAP capture = snip->UiCaptureBitmap();
    if (!capture) return false;

    RECT client{};
    if (!GetClientRect(edit, &client)) return false;

    POINT sourceOrigin{0, 0};
    if (!MapWindowPoints(edit, snipHwnd, &sourceOrigin, 1)) {
        // MapWindowPoints may legitimately return zero when the mapped point is
        // also (0,0), so only treat it as a failure when GetLastError says so.
        if (GetLastError() != ERROR_SUCCESS) return false;
    }

    HDC source = CreateCompatibleDC(dc);
    if (!source) return false;
    const HGDIOBJ old = SelectObject(source, capture);
    const BOOL copied = BitBlt(
        dc,
        0,
        0,
        client.right - client.left,
        client.bottom - client.top,
        source,
        sourceOrigin.x,
        sourceOrigin.y,
        SRCCOPY);
    SelectObject(source, old);
    DeleteDC(source);
    return copied != FALSE;
}

inline std::wstring CurrentEditText(HWND edit) {
    const int length = std::max(0, GetWindowTextLengthW(edit));
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(edit, text.data(), length + 1);
    }
    text.resize(static_cast<size_t>(length));
    return text;
}

inline void ResizeEditToContent(HWND edit) {
    if (!edit || !IsWindow(edit)) return;

    HWND host{};
    HWND snipHwnd{};
    SnipWindow* snip = FindSnipWindow(edit, &host, &snipHwnd);
    if (!host || !snipHwnd) return;

    DWORD selStart = 0;
    DWORD selEnd = 0;
    SendMessageW(edit, EM_GETSEL,
                 reinterpret_cast<WPARAM>(&selStart),
                 reinterpret_cast<LPARAM>(&selEnd));

    const std::wstring text = CurrentEditText(edit);
    HDC dc = GetDC(edit);
    SIZE textSize{0, 0};
    TEXTMETRICW metrics{};
    if (dc) {
        const HFONT font = reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0));
        const HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
        if (!text.empty()) {
            GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &textSize);
        }
        GetTextMetricsW(dc, &metrics);
        if (oldFont) SelectObject(dc, oldFont);
        ReleaseDC(edit, dc);
    }

    int width = std::max(56, static_cast<int>(textSize.cx) + 18);
    int height = std::max(28, static_cast<int>(metrics.tmHeight) + 10);

    POINT hostOrigin{0, 0};
    MapWindowPoints(host, snipHwnd, &hostOrigin, 1);
    if (snip && snip->UiHasSelection()) {
        const RECT selection = snip->UiSelectionRect();
        const int maxWidth = std::max(24, static_cast<int>(selection.right - hostOrigin.x));
        const int maxHeight = std::max(24, static_cast<int>(selection.bottom - hostOrigin.y));
        width = std::clamp(width, 24, maxWidth);
        height = std::clamp(height, 24, maxHeight);
    }

    SetWindowPos(host, nullptr, 0, 0, width, height,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(edit, nullptr, 0, 0, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    // Erase using the actual captured pixels, then let the native EDIT draw its
    // current text and caret. No hide/show cycle: hiding caused EN_KILLFOCUS and
    // could commit/delete the whole annotation on a single Backspace/Delete.
    RedrawWindow(edit, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    SendMessageW(edit, EM_SETSEL, selStart, selEnd);
}

inline LRESULT CALLBACK TransparentEditRefreshProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR, DWORD_PTR) {

    if (message == WM_ERASEBKGND) {
        HDC dc = reinterpret_cast<HDC>(wParam);
        if (PaintCaptureBackground(hwnd, dc)) {
            return 1;
        }
    }

    const bool contentMayChange =
        message == WM_CHAR || message == WM_CUT || message == WM_CLEAR ||
        message == WM_PASTE || message == WM_SETTEXT || message == WM_IME_COMPOSITION ||
        (message == WM_KEYDOWN && wParam == VK_DELETE);

    const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);

    if (contentMayChange && IsWindow(hwnd)) {
        ResizeEditToContent(hwnd);
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
        ResizeEditToContent(hwnd);
    }
    return ok;
}

}  // namespace snaplite::editrepaintfix

#define SetWindowSubclass(...) \
    snaplite::editrepaintfix::SetWindowSubclassWithTransparentEditRefresh(__VA_ARGS__)
