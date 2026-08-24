#pragma once

#include "snip_window.h"

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <iterator>
#include <string>

namespace snaplite::editrepaintfix {

constexpr UINT_PTR kTextEditRefreshSubclassId = 0x534C14F2;
inline thread_local int gEditMutationDepth = 0;

inline bool IsClass(HWND hwnd, const wchar_t* expected) {
    if (!hwnd || !expected) return false;
    wchar_t className[64]{};
    if (!GetClassNameW(hwnd, className, static_cast<int>(std::size(className)))) {
        return false;
    }
    return _wcsicmp(className, expected) == 0;
}

inline bool IsEditControl(HWND hwnd) {
    return IsClass(hwnd, L"Edit");
}

inline bool IsTextHost(HWND hwnd) {
    return IsClass(hwnd, L"SnapLiteTextAnnotationChild");
}

inline SnipWindow* FindSnipWindow(HWND edit, HWND* hostOut = nullptr,
                                  HWND* snipHwndOut = nullptr) {
    if (!edit || !IsEditControl(edit)) return nullptr;

    HWND host = GetParent(edit);
    HWND snipHwnd = host ? GetParent(host) : nullptr;
    if (hostOut) *hostOut = host;
    if (snipHwndOut) *snipHwndOut = snipHwnd;

    if (!host || !snipHwnd || !IsTextHost(host) ||
        !IsClass(snipHwnd, L"SnapLiteSnipWindow")) {
        return nullptr;
    }

    return reinterpret_cast<SnipWindow*>(GetWindowLongPtrW(snipHwnd, GWLP_USERDATA));
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

inline SIZE MeasureLiveEdit(HWND edit) {
    SIZE measured{0, 0};
    TEXTMETRICW metrics{};

    HDC dc = GetDC(edit);
    if (!dc) return {56, 28};

    const HFONT font = reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0));
    const HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    const std::wstring text = CurrentEditText(edit);

    LONG maxWidth = 0;
    int lineCount = 0;
    if (!text.empty()) {
        // Split on \n so \r\n from the multiline EDIT yields one row per
        // visual line. The widest line dictates the width; height is the
        // number of lines times the font's line height.
        size_t start = 0;
        while (start <= text.size()) {
            const size_t end = text.find(L'\n', start);
            const size_t chunkLen = (end == std::wstring::npos)
                                        ? (text.size() - start)
                                        : (end - start);
            const std::wstring line = text.substr(start, chunkLen);
            if (!line.empty()) {
                SIZE lineSize{};
                GetTextExtentPoint32W(dc, line.c_str(),
                                      static_cast<int>(line.size()), &lineSize);
                maxWidth = std::max<LONG>(maxWidth, lineSize.cx);
            }
            ++lineCount;
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
    }

    GetTextMetricsW(dc, &metrics);
    if (oldFont) SelectObject(dc, oldFont);
    ReleaseDC(edit, dc);

    const LONG lineHeight = std::max<LONG>(1, metrics.tmHeight);
    measured.cx = std::max<LONG>(56, maxWidth + 18);
    measured.cy = std::max<LONG>(
        28,
        static_cast<LONG>(lineCount) * lineHeight + 10);
    return measured;
}

inline RECT WindowRectInClient(HWND hwnd, HWND parent) {
    RECT rect{};
    if (!hwnd || !parent || !GetWindowRect(hwnd, &rect)) return rect;
    POINT points[2]{{rect.left, rect.top}, {rect.right, rect.bottom}};
    MapWindowPoints(nullptr, parent, points, 2);
    return {points[0].x, points[0].y, points[1].x, points[1].y};
}

inline void PaintCaptureIntoHost(HWND edit) {
    HWND host{};
    HWND snipHwnd{};
    SnipWindow* snip = FindSnipWindow(edit, &host, &snipHwnd);
    if (!snip || !host || !snipHwnd) return;

    HBITMAP capture = snip->UiCaptureBitmap();
    if (!capture) return;

    RECT client{};
    if (!GetClientRect(host, &client)) return;

    POINT sourceOrigin{0, 0};
    SetLastError(ERROR_SUCCESS);
    MapWindowPoints(host, snipHwnd, &sourceOrigin, 1);

    HDC dc = GetDC(host);
    if (!dc) return;
    HDC source = CreateCompatibleDC(dc);
    if (!source) {
        ReleaseDC(host, dc);
        return;
    }

    const HGDIOBJ oldBitmap = SelectObject(source, capture);
    BitBlt(dc,
           0,
           0,
           client.right - client.left,
           client.bottom - client.top,
           source,
           sourceOrigin.x,
           sourceOrigin.y,
           SRCCOPY);
    SelectObject(source, oldBitmap);
    DeleteDC(source);
    ReleaseDC(host, dc);
}

inline void ResizeHostToLiveText(HWND edit) {
    HWND host{};
    HWND snipHwnd{};
    SnipWindow* snip = FindSnipWindow(edit, &host, &snipHwnd);
    if (!snip || !host || !snipHwnd) return;

    const RECT oldRect = WindowRectInClient(host, snipHwnd);
    SIZE size = MeasureLiveEdit(edit);

    POINT hostOrigin{0, 0};
    MapWindowPoints(host, snipHwnd, &hostOrigin, 1);

    if (snip->UiHasSelection()) {
        const RECT selection = snip->UiSelectionRect();
        const int maxWidth = std::max(
            24,
            static_cast<int>(selection.right) - static_cast<int>(hostOrigin.x));
        const int maxHeight = std::max(
            24,
            static_cast<int>(selection.bottom) - static_cast<int>(hostOrigin.y));
        size.cx = static_cast<LONG>(std::clamp(static_cast<int>(size.cx), 24, maxWidth));
        size.cy = static_cast<LONG>(std::clamp(static_cast<int>(size.cy), 24, maxHeight));
    }

    SetWindowPos(host,
                 nullptr,
                 0,
                 0,
                 static_cast<int>(size.cx),
                 static_cast<int>(size.cy),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(edit,
                 nullptr,
                 0,
                 0,
                 static_cast<int>(size.cx),
                 static_cast<int>(size.cy),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    const RECT newRect = WindowRectInClient(host, snipHwnd);
    RECT dirty{};
    UnionRect(&dirty, &oldRect, &newRect);
    InflateRect(&dirty, 2, 2);

    // Redraw only the screenshot parent here. The edit itself is painted below,
    // so shrinking a text box cannot leave pixels from the old, wider box behind.
    ::RedrawWindow(snipHwnd,
                   &dirty,
                   nullptr,
                   RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOCHILDREN);
}

inline void RefreshEditVisual(HWND edit) {
    if (!edit || !IsWindow(edit) || !IsEditControl(edit)) return;
    PaintCaptureIntoHost(edit);

    // Keep the native EDIT in charge of glyphs, caret, selection and IME.
    // We only restore the captured pixels below it, then ask Windows to paint
    // the current text once. No hide/show and no focus transition is involved.
    ::RedrawWindow(edit,
                   nullptr,
                   nullptr,
                   RDW_INVALIDATE | RDW_UPDATENOW);
}

inline void LayoutAndRefresh(HWND edit) {
    if (!edit || !IsWindow(edit) || !IsEditControl(edit)) return;
    ResizeHostToLiveText(edit);
    RefreshEditVisual(edit);
}

inline bool ContentMayChange(UINT message, WPARAM wParam) {
    if (message == WM_CHAR || message == WM_PASTE || message == WM_CUT ||
        message == WM_CLEAR || message == WM_SETTEXT || message == WM_UNDO ||
        message == WM_IME_COMPOSITION || message == WM_IME_ENDCOMPOSITION) {
        return true;
    }
    return message == WM_KEYDOWN && wParam == VK_DELETE;
}

inline LRESULT CALLBACK TextEditRefreshProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR,
    DWORD_PTR) {

    const bool contentMutation = ContentMayChange(message, wParam);
    if (contentMutation) ++gEditMutationDepth;

    const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);

    if (contentMutation) --gEditMutationDepth;

    if (contentMutation && IsWindow(hwnd)) {
        LayoutAndRefresh(hwnd);
    }

    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, TextEditRefreshProc, kTextEditRefreshSubclassId);
    }
    return result;
}

inline BOOL SetWindowSubclassWithTextEditRefresh(
    HWND hwnd,
    SUBCLASSPROC proc,
    UINT_PTR id,
    DWORD_PTR refData) {
    const BOOL ok = ::SetWindowSubclass(hwnd, proc, id, refData);

    // Critical: the old Beta.5 wrapper attached its edit logic to every window
    // passed to SetWindowSubclass, including the screenshot parent. Never do that.
    if (ok && IsEditControl(hwnd)) {
        ::SetWindowSubclass(hwnd, TextEditRefreshProc, kTextEditRefreshSubclassId, 0);
        LayoutAndRefresh(hwnd);
    }
    return ok;
}

inline BOOL StableRedrawWindow(
    HWND hwnd,
    const RECT* updateRect,
    HRGN updateRegion,
    UINT flags) {
    if (hwnd && IsTextHost(hwnd)) {
        HWND edit = FindWindowExW(hwnd, nullptr, L"Edit", nullptr);
        if (edit) {
            // EN_UPDATE is sent while the native edit is still processing the
            // keystroke. Suppress the old recursive parent/child repaint there;
            // TextEditRefreshProc performs one deterministic repaint afterwards.
            if (gEditMutationDepth > 0) return TRUE;
            RefreshEditVisual(edit);
            return TRUE;
        }
    }
    return ::RedrawWindow(hwnd, updateRect, updateRegion, flags);
}

}  // namespace snaplite::editrepaintfix

#define SetWindowSubclass(...) \
    snaplite::editrepaintfix::SetWindowSubclassWithTextEditRefresh(__VA_ARGS__)
#define RedrawWindow(...) \
    snaplite::editrepaintfix::StableRedrawWindow(__VA_ARGS__)
