#pragma once

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace snaplite::ui {

// 枚举值会写入注册表；新增主题应追加到末尾，不能重排已发布项。
enum class ThemeId : std::uint32_t {
    KOn,
    WitchJourney,
    ReiAyanami,
    Asuka,
    Genshin,
    ZenlessZoneZero,
    HonkaiImpact3,
    WutheringWaves,
    HonkaiStarRail,
    YuruCamp,
};

struct ThemePalette {
    LPCWSTR name;
    COLORREF surface;
    COLORREF cream;
    COLORREF card;
    COLORREF border;
    COLORREF accent;
    COLORREF accentStrong;
    COLORREF accentAction;
    COLORREF accentActionPressed;
    COLORREF text;
    COLORREF mutedText;
    COLORREF textSecondary;
    COLORREF hover;
    COLORREF pressed;
    COLORREF sliderTrack;
    COLORREF danger;
    COLORREF dangerHover;
};

inline constexpr std::array<ThemePalette, 10> kThemePalettes{{
    {
        L"轻音少女",
        RGB(255, 248, 250), RGB(255, 250, 248), RGB(255, 255, 255),
        RGB(244, 216, 224), RGB(239, 139, 166), RGB(224, 119, 151),
        RGB(194, 74, 115), RGB(174, 58, 96), RGB(57, 49, 55),
        RGB(137, 119, 128), RGB(112, 94, 103), RGB(255, 239, 244),
        RGB(250, 228, 235), RGB(237, 222, 227), RGB(201, 75, 83),
        RGB(255, 237, 239),
    },
    {
        L"魔女之旅 · 伊蕾娜",
        RGB(248, 246, 255), RGB(255, 251, 245), RGB(255, 255, 255),
        RGB(220, 209, 238), RGB(169, 137, 207), RGB(139, 101, 183),
        RGB(117, 77, 162), RGB(98, 62, 141), RGB(49, 42, 62),
        RGB(135, 121, 154), RGB(104, 89, 126), RGB(243, 237, 252),
        RGB(232, 222, 247), RGB(222, 209, 239), RGB(201, 75, 83),
        RGB(255, 237, 239),
    },
    {
        L"绫波丽",
        RGB(238, 246, 250), RGB(248, 252, 255), RGB(255, 255, 255),
        RGB(188, 213, 226), RGB(115, 171, 203), RGB(79, 142, 180),
        RGB(53, 112, 151), RGB(42, 91, 126), RGB(31, 47, 59),
        RGB(105, 129, 144), RGB(76, 101, 117), RGB(225, 239, 247),
        RGB(207, 228, 239), RGB(204, 222, 234), RGB(201, 75, 83),
        RGB(255, 237, 239),
    },
    {
        L"明日香",
        RGB(255, 246, 239), RGB(255, 251, 245), RGB(255, 255, 255),
        RGB(246, 213, 190), RGB(239, 123, 72), RGB(220, 84, 54),
        RGB(184, 52, 36), RGB(158, 37, 29), RGB(61, 42, 33),
        RGB(150, 115, 95), RGB(117, 77, 58), RGB(255, 235, 220),
        RGB(250, 218, 192), RGB(238, 210, 188), RGB(201, 75, 83),
        RGB(255, 237, 239),
    },
    {
        L"原神",
        RGB(246, 249, 253), RGB(255, 251, 245), RGB(255, 255, 255),
        RGB(212, 220, 231), RGB(103, 157, 190), RGB(69, 124, 160),
        RGB(54, 103, 136), RGB(38, 83, 116), RGB(45, 48, 56),
        RGB(119, 124, 135), RGB(86, 91, 104), RGB(230, 239, 247),
        RGB(212, 229, 240), RGB(207, 220, 230), RGB(201, 75, 83),
        RGB(255, 237, 239),
    },
    {
        L"绝区零",
        RGB(246, 247, 249), RGB(252, 251, 247), RGB(255, 255, 255),
        RGB(202, 208, 215), RGB(49, 171, 203), RGB(27, 140, 175),
        RGB(31, 107, 139), RGB(23, 87, 116), RGB(32, 38, 43),
        RGB(116, 126, 133), RGB(83, 93, 101), RGB(227, 242, 247),
        RGB(208, 233, 242), RGB(205, 222, 230), RGB(201, 75, 83),
        RGB(255, 237, 239),
    },
    {
        L"崩坏3",
        RGB(248, 246, 253), RGB(255, 250, 246), RGB(255, 255, 255),
        RGB(225, 211, 236), RGB(174, 139, 203), RGB(145, 103, 180),
        RGB(117, 76, 155), RGB(98, 61, 133), RGB(49, 43, 60),
        RGB(135, 121, 151), RGB(103, 88, 122), RGB(243, 237, 250),
        RGB(231, 221, 244), RGB(223, 211, 235), RGB(201, 75, 83),
        RGB(255, 237, 239),
    },
    {
        L"鸣潮",
        RGB(240, 247, 246), RGB(249, 251, 247), RGB(255, 255, 255),
        RGB(206, 223, 221), RGB(81, 152, 157), RGB(53, 125, 133),
        RGB(43, 100, 108), RGB(32, 83, 90), RGB(38, 52, 55),
        RGB(111, 132, 135), RGB(78, 104, 108), RGB(225, 239, 238),
        RGB(207, 229, 227), RGB(207, 223, 222), RGB(201, 75, 83),
        RGB(255, 237, 239),
    },
    {
        L"崩坏：星穹铁道",
        RGB(244, 246, 252), RGB(251, 250, 248), RGB(255, 255, 255),
        RGB(211, 218, 235), RGB(123, 140, 195), RGB(91, 108, 166),
        RGB(70, 85, 136), RGB(54, 67, 116), RGB(43, 47, 62),
        RGB(124, 130, 149), RGB(91, 98, 119), RGB(231, 236, 248),
        RGB(213, 221, 240), RGB(213, 220, 234), RGB(201, 75, 83),
        RGB(255, 237, 239),
    },
    {
        L"摇曳露营",
        RGB(245, 249, 242), RGB(255, 251, 244), RGB(255, 255, 255),
        RGB(218, 228, 211), RGB(132, 166, 119), RGB(99, 137, 90),
        RGB(77, 113, 70), RGB(61, 94, 55), RGB(49, 57, 47),
        RGB(121, 133, 112), RGB(91, 103, 83), RGB(234, 243, 226),
        RGB(220, 234, 211), RGB(217, 228, 210), RGB(201, 75, 83),
        RGB(255, 237, 239),
    },
}};

inline constexpr std::size_t kThemeCount = kThemePalettes.size();
inline ThemeId gCurrentThemeId = ThemeId::KOn;
inline ThemePalette gCurrentPalette = kThemePalettes[0];

// 这些引用保留现有控件的用色接口，同时让切换主题后下次绘制立即读取新颜色。
inline COLORREF& kSurface = gCurrentPalette.surface;
inline COLORREF& kCream = gCurrentPalette.cream;
inline COLORREF& kCard = gCurrentPalette.card;
inline COLORREF& kBorder = gCurrentPalette.border;
inline COLORREF& kAccent = gCurrentPalette.accent;
inline COLORREF& kAccentStrong = gCurrentPalette.accentStrong;
inline COLORREF& kAccentAction = gCurrentPalette.accentAction;
inline COLORREF& kAccentActionPressed = gCurrentPalette.accentActionPressed;
inline COLORREF& kText = gCurrentPalette.text;
inline COLORREF& kMutedText = gCurrentPalette.mutedText;
inline COLORREF& kTextSecondary = gCurrentPalette.textSecondary;
inline COLORREF& kHover = gCurrentPalette.hover;
inline COLORREF& kPressed = gCurrentPalette.pressed;
inline COLORREF& kSliderTrack = gCurrentPalette.sliderTrack;
inline COLORREF& kDanger = gCurrentPalette.danger;
inline COLORREF& kDangerHover = gCurrentPalette.dangerHover;

inline bool IsValidThemeValue(DWORD value) {
    return value < kThemeCount;
}

inline bool SetTheme(ThemeId theme) {
    const auto index = static_cast<std::size_t>(theme);
    if (index >= kThemePalettes.size()) {
        return false;
    }
    gCurrentThemeId = theme;
    gCurrentPalette = kThemePalettes[index];
    return true;
}

inline ThemeId CurrentThemeId() {
    return gCurrentThemeId;
}

inline LPCWSTR ThemeName(ThemeId theme) {
    const auto index = static_cast<std::size_t>(theme);
    return index < kThemePalettes.size()
        ? kThemePalettes[index].name
        : kThemePalettes[0].name;
}

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
    if (region) {
        if (SetWindowRgn(hwnd, region, TRUE) == 0) {
            DeleteObject(region);
        }
    }
}

inline COLORREF Blend(COLORREF a, COLORREF b) {
    return RGB(
        (GetRValue(a) + GetRValue(b)) / 2,
        (GetGValue(a) + GetGValue(b)) / 2,
        (GetBValue(a) + GetBValue(b)) / 2);
}

inline COLORREF BlendRatio(COLORREF a, COLORREF b, int weightA, int weightB) {
    const int total = weightA + weightB;
    if (total <= 0) return a;
    return RGB(
        (GetRValue(a) * weightA + GetRValue(b) * weightB) / total,
        (GetGValue(a) * weightA + GetGValue(b) * weightB) / total,
        (GetBValue(a) * weightA + GetBValue(b) * weightB) / total);
}

inline void DrawAccentBadge(HDC dc, const RECT& rect, int radius, COLORREF accentColor) {
    const COLORREF fill = BlendRatio(accentColor, kCard, 1, 5);
    const COLORREF border = BlendRatio(accentColor, kBorder, 3, 2);
    FillRoundRect(dc, rect, radius, fill, border);
}

}  // namespace snaplite::ui
