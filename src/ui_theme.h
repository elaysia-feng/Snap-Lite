#pragma once

#include <windows.h>

#include <algorithm>

namespace snaplite::ui {

// Snap-Lite shared visual language. Keep custom surfaces aligned with the
// compact anime toolbar instead of letting every popup invent its own palette.
inline constexpr COLORREF kSurface = RGB(255, 248, 250);
inline constexpr COLORREF kCream = RGB(255, 250, 248);
inline constexpr COLORREF kCard = RGB(255, 255, 255);
inline constexpr COLORREF kBorder = RGB(244, 216, 224);
inline constexpr COLORREF kAccent = RGB(239, 139, 166);
inline constexpr COLORREF kAccentStrong = RGB(224, 119, 151);
inline constexpr COLORREF kText = RGB(57, 49, 55);
inline constexpr COLORREF kMutedText = RGB(137, 119, 128);
inline constexpr COLORREF kHover = RGB(255, 239, 244);
inline constexpr COLORREF kPressed = RGB(250, 228, 235);
inline constexpr COLORREF kSliderTrack = RGB(237, 222, 227);
inline constexpr COLORREF kDanger = RGB(201, 75, 83);
inline constexpr COLORREF kDangerHover = RGB(255, 237, 239);

inline HFONT MakeFont(UINT dpi, int pointSize, int weight = FW_NORMAL) {
    return CreateFontW(
        -MulDiv(pointSize, static_cast<int>(dpi), 72),
        0,
        0,
        0,
        weight,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_TT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Microsoft YaHei UI");
}

inline void FillRoundRect(
    HDC dc,
    const RECT& rect,
    int radius,
    COLORREF fill,
    COLORREF border = kBorder) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    const HGDIOBJ oldBrush = SelectObject(dc, brush);
    const HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

inline void FillSolid(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

inline void SetRoundedWindowRegion(HWND hwnd, int radius) {
    RECT rect{};
    GetClientRect(hwnd, &rect);
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    HRGN region = CreateRoundRectRgn(
        0,
        0,
        rect.right - rect.left + 1,
        rect.bottom - rect.top + 1,
        radius,
        radius);
    if (region) SetWindowRgn(hwnd, region, TRUE);
}

inline COLORREF Blend(COLORREF a, COLORREF b) {
    return RGB(
        (GetRValue(a) + GetRValue(b)) / 2,
        (GetGValue(a) + GetGValue(b)) / 2,
        (GetBValue(a) + GetBValue(b)) / 2);
}

}  // namespace snaplite::ui
