#pragma once

#include <windows.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>

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
        RGB(255, 249, 251), RGB(255, 241, 245), RGB(255, 255, 255),
        RGB(246, 218, 228), RGB(244, 114, 150), RGB(236, 72, 122),
        RGB(220, 46, 98), RGB(190, 32, 80), RGB(36, 28, 34),
        RGB(136, 116, 126), RGB(98, 80, 90), RGB(255, 236, 242),
        RGB(252, 220, 231), RGB(242, 218, 226), RGB(220, 56, 72),
        RGB(255, 235, 238),
    },
    {
        L"魔女之旅 · 伊蕾娜",
        RGB(250, 248, 255), RGB(243, 238, 255), RGB(255, 255, 255),
        RGB(224, 214, 246), RGB(155, 114, 235), RGB(132, 84, 222),
        RGB(114, 64, 206), RGB(94, 48, 180), RGB(34, 28, 48),
        RGB(128, 116, 152), RGB(92, 80, 118), RGB(241, 235, 255),
        RGB(229, 218, 252), RGB(224, 214, 244), RGB(220, 56, 72),
        RGB(255, 235, 238),
    },
    {
        L"绫波丽",
        RGB(246, 251, 255), RGB(236, 246, 255), RGB(255, 255, 255),
        RGB(206, 228, 246), RGB(56, 168, 235), RGB(24, 142, 218),
        RGB(14, 122, 198), RGB(10, 98, 166), RGB(24, 38, 52),
        RGB(108, 130, 148), RGB(72, 94, 114), RGB(228, 243, 254),
        RGB(208, 232, 250), RGB(210, 228, 242), RGB(220, 56, 72),
        RGB(255, 235, 238),
    },
    {
        L"明日香",
        RGB(255, 249, 246), RGB(255, 240, 232), RGB(255, 255, 255),
        RGB(248, 218, 202), RGB(246, 104, 62), RGB(232, 76, 36),
        RGB(214, 56, 22), RGB(184, 42, 14), RGB(44, 28, 22),
        RGB(146, 112, 98), RGB(110, 76, 62), RGB(255, 235, 224),
        RGB(252, 218, 200), RGB(242, 214, 198), RGB(220, 56, 72),
        RGB(255, 235, 238),
    },
    {
        L"原神",
        RGB(247, 250, 255), RGB(238, 244, 255), RGB(255, 255, 255),
        RGB(212, 225, 246), RGB(74, 144, 238), RGB(48, 118, 220),
        RGB(37, 99, 205), RGB(29, 78, 176), RGB(28, 36, 52),
        RGB(112, 124, 146), RGB(76, 88, 112), RGB(232, 241, 254),
        RGB(214, 228, 250), RGB(214, 226, 244), RGB(220, 56, 72),
        RGB(255, 235, 238),
    },
    {
        L"绝区零",
        RGB(246, 251, 253), RGB(234, 247, 251), RGB(255, 255, 255),
        RGB(204, 228, 236), RGB(22, 178, 212), RGB(12, 152, 188),
        RGB(8, 132, 168), RGB(6, 108, 140), RGB(22, 34, 42),
        RGB(106, 124, 134), RGB(70, 88, 98), RGB(226, 245, 250),
        RGB(204, 235, 244), RGB(208, 228, 236), RGB(220, 56, 72),
        RGB(255, 235, 238),
    },
    {
        L"崩坏3",
        RGB(252, 248, 255), RGB(246, 237, 255), RGB(255, 255, 255),
        RGB(230, 212, 246), RGB(178, 108, 232), RGB(156, 78, 216),
        RGB(138, 58, 198), RGB(114, 44, 170), RGB(38, 28, 48),
        RGB(134, 114, 152), RGB(98, 78, 118), RGB(244, 234, 254),
        RGB(232, 216, 250), RGB(228, 214, 242), RGB(220, 56, 72),
        RGB(255, 235, 238),
    },
    {
        L"鸣潮",
        RGB(246, 252, 251), RGB(234, 248, 245), RGB(255, 255, 255),
        RGB(202, 232, 226), RGB(32, 178, 166), RGB(18, 154, 142),
        RGB(13, 134, 124), RGB(10, 110, 102), RGB(22, 38, 36),
        RGB(104, 128, 124), RGB(68, 94, 90), RGB(224, 246, 242),
        RGB(202, 236, 230), RGB(206, 230, 226), RGB(220, 56, 72),
        RGB(255, 235, 238),
    },
    {
        L"崩坏：星穹铁道",
        RGB(247, 248, 255), RGB(238, 240, 255), RGB(255, 255, 255),
        RGB(214, 218, 246), RGB(99, 112, 238), RGB(79, 88, 220),
        RGB(67, 72, 202), RGB(54, 58, 172), RGB(28, 32, 52),
        RGB(114, 120, 148), RGB(78, 84, 114), RGB(233, 236, 254),
        RGB(216, 221, 250), RGB(216, 220, 242), RGB(220, 56, 72),
        RGB(255, 235, 238),
    },
    {
        L"摇曳露营",
        RGB(246, 252, 248), RGB(235, 248, 240), RGB(255, 255, 255),
        RGB(204, 232, 214), RGB(52, 178, 118), RGB(34, 154, 96),
        RGB(22, 134, 80), RGB(16, 110, 64), RGB(24, 40, 30),
        RGB(106, 128, 114), RGB(70, 94, 78), RGB(226, 246, 233),
        RGB(206, 236, 218), RGB(208, 230, 216), RGB(220, 56, 72),
        RGB(255, 235, 238),
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
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;

    Gdiplus::Graphics graphics(dc);
    if (graphics.GetLastStatus() == Gdiplus::Ok) {
        const Gdiplus::RectF box(
            static_cast<float>(rect.left) + 0.5f,
            static_cast<float>(rect.top) + 0.5f,
            static_cast<float>(rect.right - rect.left) - 1.0f,
            static_cast<float>(rect.bottom - rect.top) - 1.0f);
        const float minBoxSide = (std::min)(box.Width, box.Height);
        if (minBoxSide >= 2.0f) {
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            const int minRectSide = (std::min)(
                static_cast<int>(rect.right - rect.left),
                static_cast<int>(rect.bottom - rect.top));
            const float d = radius >= minRectSide
                ? minBoxSide
                : std::clamp(static_cast<float>(radius) * 1.25f, 2.0f, minBoxSide);
            Gdiplus::GraphicsPath path;
            path.AddArc(box.X, box.Y, d, d, 180.0f, 90.0f);
            path.AddArc(box.GetRight() - d, box.Y, d, d, 270.0f, 90.0f);
            path.AddArc(box.GetRight() - d, box.GetBottom() - d, d, d, 0.0f, 90.0f);
            path.AddArc(box.X, box.GetBottom() - d, d, d, 90.0f, 90.0f);
            path.CloseFigure();
            const Gdiplus::Color fillColor(255, GetRValue(fill), GetGValue(fill), GetBValue(fill));
            const Gdiplus::Color borderColor(255, GetRValue(border), GetGValue(border), GetBValue(border));
            Gdiplus::SolidBrush brush(fillColor);
            graphics.FillPath(&brush, &path);
            Gdiplus::Pen pen(borderColor, 1.0f);
            graphics.DrawPath(&pen, &path);
            return;
        }
    }

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
    const BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(hwnd, 3, &disableTransitions, sizeof(disableTransitions));
    const DWORD cornerPreference = 2;  // DWMWCP_ROUND
    if (SUCCEEDED(DwmSetWindowAttribute(hwnd, 33, &cornerPreference, sizeof(cornerPreference)))) {
        SetWindowRgn(hwnd, nullptr, TRUE);
        return;
    }

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

inline void DrawElevatedCard(HDC dc, const RECT& rect, int radius) {
    RECT s2 = rect;
    s2.top += 2;
    s2.bottom += 2;
    const COLORREF shadow2 = BlendRatio(kBorder, kSurface, 1, 2);
    FillRoundRect(dc, s2, radius, shadow2, shadow2);

    RECT s1 = rect;
    s1.top += 1;
    s1.bottom += 1;
    const COLORREF shadow1 = BlendRatio(kBorder, kSurface, 2, 3);
    FillRoundRect(dc, s1, radius, shadow1, shadow1);

    FillRoundRect(dc, rect, radius, kCard, BlendRatio(kBorder, kSurface, 4, 1));
}

inline void DrawKeycap(
    HDC dc,
    const RECT& rect,
    int radius,
    COLORREF fill,
    COLORREF border,
    COLORREF shadow) {
    FillRoundRect(dc, rect, radius, shadow, border);
    const RECT face{
        rect.left,
        rect.top,
        rect.right,
        (std::max)(static_cast<int>(rect.top + 2), static_cast<int>(rect.bottom - 2)),
    };
    FillRoundRect(dc, face, radius, fill, border);
}

}  // namespace snaplite::ui
