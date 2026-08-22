#pragma once

#include "snip_window.h"
#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <string>

namespace snaplite::editrepaintfix {

constexpr UINT_PTR kTransparentEditRefreshSubclassId = 0x534C14F2;

inline std::wstring CurrentEditText(HWND edit) {
    int len = std::max(0, GetWindowTextLengthW(edit));
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    if (len > 0) GetWindowTextW(edit, text.data(), len + 1);
    text.resize(static_cast<size_t>(len));
    return text;
}

inline void ResizeEditToContent(HWND edit) {
    if (!edit || !IsWindow(edit)) return;

    std::wstring text = CurrentEditText(edit);
    HDC dc = GetDC(edit);
    SIZE size{};
    TEXTMETRICW tm{};
    if (dc) {
        HFONT font = reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0));
        HGDIOBJ old = font ? SelectObject(dc, font) : nullptr;
        if (!text.empty()) GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &size);
        GetTextMetricsW(dc, &tm);
        if (old) SelectObject(dc, old);
        ReleaseDC(edit, dc);
    }

    int width = std::max(56, size.cx + 18);
    int height = std::max(28, (int)tm.tmHeight + 10);

    SetWindowPos(edit, nullptr, 0, 0, width, height,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

inline LRESULT CALLBACK TransparentEditRefreshProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR, DWORD_PTR) {

    LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_CHAR:
    case WM_PASTE:
    case WM_CUT:
    case WM_CLEAR:
    case WM_IME_COMPOSITION:
    case WM_SETTEXT:
    case WM_KEYUP:
        ResizeEditToContent(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, TransparentEditRefreshProc,
            kTransparentEditRefreshSubclassId);
        break;
    default:
        break;
    }

    return result;
}

inline BOOL SetWindowSubclassWithTransparentEditRefresh(
    HWND hwnd, SUBCLASSPROC proc, UINT_PTR id, DWORD_PTR refData) {
    BOOL ok = ::SetWindowSubclass(hwnd, proc, id, refData);
    if (ok) {
        ::SetWindowSubclass(hwnd, TransparentEditRefreshProc,
            kTransparentEditRefreshSubclassId, 0);
        ResizeEditToContent(hwnd);
    }
    return ok;
}

}

#define SetWindowSubclass(...) \
    snaplite::editrepaintfix::SetWindowSubclassWithTransparentEditRefresh(__VA_ARGS__)
