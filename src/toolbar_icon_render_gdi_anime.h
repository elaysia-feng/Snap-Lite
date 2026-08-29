#pragma once

// Primary-toolbar anime skin. Tool glyphs stay deterministic GDI primitives;
// the five mascot portraits come from the approved compact toolbar concept and
// are embedded as tiny indexed sprites so the release remains a single EXE.
#define DrawTextOrIcon DrawBaseTextOrIcon
#include "toolbar_icon_render_gdi.h"
#undef DrawTextOrIcon

#include "toolbar_chibi_assets.h"
#include "toolbar_chibi_azusa_v2.h"
#include "ui_theme.h"

#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <cstdint>

namespace snaplite::toolbaricons_gdi {
namespace anime_skin {

enum class Girl : int { Yui = 0, Mio, Mugi, Ritsu, Azusa };

inline COLORREF Accent(Girl girl) {
    switch (girl) {
    case Girl::Yui:   return RGB(235, 119, 132);
    case Girl::Mio:   return RGB(118, 127, 168);
    case Girl::Mugi:  return RGB(226, 169, 85);
    case Girl::Ritsu: return RGB(232, 133, 91);
    case Girl::Azusa: return RGB(94, 151, 155);
    }
    return RGB(220, 130, 140);
}

inline bool PrimaryLabel(LPCWSTR text, int count) {
    return Exact(text, count, L"选择") || Exact(text, count, L"形状") ||
           Exact(text, count, L"箭头") || Exact(text, count, L"画笔") ||
           Exact(text, count, L"马赛克") || Exact(text, count, L"文字") ||
           Exact(text, count, L"图钉") || Exact(text, count, L"撤销") ||
           Exact(text, count, L"重做") || Exact(text, count, L"复制") ||
           Exact(text, count, L"保存") || Exact(text, count, L"另存为") ||
           Exact(text, count, L"取消");
}

inline Girl GirlForLabel(LPCWSTR text, int count) {
    if (Exact(text, count, L"选择"))   return Girl::Yui;
    if (Exact(text, count, L"形状"))   return Girl::Ritsu;
    if (Exact(text, count, L"箭头"))   return Girl::Mugi;
    if (Exact(text, count, L"画笔"))   return Girl::Yui;
    if (Exact(text, count, L"马赛克")) return Girl::Mio;
    if (Exact(text, count, L"文字"))   return Girl::Azusa;
    if (Exact(text, count, L"图钉"))   return Girl::Mugi;
    if (Exact(text, count, L"撤销"))   return Girl::Yui;
    if (Exact(text, count, L"重做"))   return Girl::Mugi;
    if (Exact(text, count, L"复制"))   return Girl::Mio;
    if (Exact(text, count, L"保存"))   return Girl::Ritsu;
    if (Exact(text, count, L"另存为")) return Girl::Yui;
    return Girl::Azusa;
}

inline int Base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

inline std::array<std::uint32_t, 32 * 32> DecodeSprite(const char* encoded) {
    constexpr size_t kRawSize = 32u * sizeof(std::uint32_t) + 32u * 32u;
    std::array<std::uint8_t, kRawSize> raw{};

    std::uint32_t buffer = 0;
    int bits = 0;
    size_t out = 0;
    for (const char* p = encoded; p && *p && *p != '='; ++p) {
        const int value = Base64Value(*p);
        if (value < 0) continue;
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out < raw.size()) {
                raw[out++] = static_cast<std::uint8_t>((buffer >> bits) & 0xFFu);
            }
        }
    }

    std::array<std::uint32_t, 32> palette{};
    for (size_t i = 0; i < palette.size(); ++i) {
        const size_t b = i * 4;
        palette[i] =
            static_cast<std::uint32_t>(raw[b]) |
            (static_cast<std::uint32_t>(raw[b + 1]) << 8) |
            (static_cast<std::uint32_t>(raw[b + 2]) << 16) |
            (static_cast<std::uint32_t>(raw[b + 3]) << 24);
    }

    std::array<std::uint32_t, 32 * 32> pixels{};
    constexpr size_t kIndexOffset = 32u * sizeof(std::uint32_t);
    for (size_t i = 0; i < pixels.size(); ++i) {
        const auto index = static_cast<size_t>(raw[kIndexOffset + i] & 31u);
        pixels[i] = palette[index];
    }
    return pixels;
}

inline const std::array<std::uint32_t, 32 * 32>& SpritePixels(Girl girl) {
    static const auto yui = DecodeSprite(snaplite::toolbar_chibi_assets::kYui);
    static const auto mio = DecodeSprite(snaplite::toolbar_chibi_assets::kMio);
    static const auto mugi = DecodeSprite(snaplite::toolbar_chibi_assets::kMugi);
    static const auto ritsu = DecodeSprite(snaplite::toolbar_chibi_assets::kRitsu);
    static const auto azusa = DecodeSprite(snaplite::toolbar_chibi_assets::kAzusaV2);

    switch (girl) {
    case Girl::Yui:   return yui;
    case Girl::Mio:   return mio;
    case Girl::Mugi:  return mugi;
    case Girl::Ritsu: return ritsu;
    case Girl::Azusa: return azusa;
    }
    return yui;
}

inline Gdiplus::Color ToColor(COLORREF color, BYTE alpha = 255) {
    return Gdiplus::Color(
        alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

inline void AddRoundRectPath(
    Gdiplus::GraphicsPath& path,
    const Gdiplus::RectF& rect,
    float radius) {
    path.Reset();
    const float diameter = radius * 2.0f;
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(
        rect.GetRight() - diameter,
        rect.GetBottom() - diameter,
        diameter,
        diameter,
        0.0f,
        90.0f);
    path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

inline void FillSoftRoundRect(
    Gdiplus::Graphics& graphics,
    const Gdiplus::RectF& rect,
    float radius,
    const Gdiplus::Color& color) {
    Gdiplus::GraphicsPath path;
    AddRoundRectPath(path, rect, radius);
    Gdiplus::SolidBrush brush(color);
    graphics.FillPath(&brush, &path);
}

inline void DrawTile(HDC dc, const RECT& rect, Girl girl) {
    // 人物是按钮的主视觉：默认不画按钮边框，只用低对比度圆形底座和
    // 角色专属色细环托住头像，避免工具栏重新变成一排拥挤的小方块。
    if (!dc) return;
    const int width = std::max(1L, rect.right - rect.left);
    const int height = std::max(1L, rect.bottom - rect.top);
    const int portraitSide = std::clamp(std::min(width - 16, height - 8), 24, 30);
    const int portraitLeft = rect.left + 3;
    const int portraitTop = rect.top + (height - portraitSide) / 2;

    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const COLORREF accent = Accent(girl);
    Gdiplus::SolidBrush shadow(Gdiplus::Color(22, 62, 48, 55));
    graphics.FillEllipse(
        &shadow,
        static_cast<float>(portraitLeft + 1),
        static_cast<float>(portraitTop + 2),
        static_cast<float>(portraitSide),
        static_cast<float>(portraitSide));
    Gdiplus::SolidBrush stage(ToColor(accent, 20));
    graphics.FillEllipse(
        &stage,
        static_cast<float>(portraitLeft),
        static_cast<float>(portraitTop),
        static_cast<float>(portraitSide),
        static_cast<float>(portraitSide));
    Gdiplus::Pen ring(ToColor(accent, 128), 1.25f);
    graphics.DrawEllipse(
        &ring,
        static_cast<float>(portraitLeft) + 0.5f,
        static_cast<float>(portraitTop) + 0.5f,
        static_cast<float>(portraitSide - 1),
        static_cast<float>(portraitSide - 1));
}

inline void DrawChibi(HDC dc, const RECT& r, Girl girl) {
    if (!dc) return;

    const auto& pixels = SpritePixels(girl);
    Gdiplus::Bitmap sprite(
        snaplite::toolbar_chibi_assets::kSpriteWidth,
        snaplite::toolbar_chibi_assets::kSpriteHeight,
        snaplite::toolbar_chibi_assets::kSpriteWidth * static_cast<int>(sizeof(std::uint32_t)),
        PixelFormat32bppARGB,
        reinterpret_cast<BYTE*>(const_cast<std::uint32_t*>(pixels.data())));
    if (sprite.GetLastStatus() != Gdiplus::Ok) return;

    Gdiplus::Graphics graphics(dc);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    const int width = std::max(1L, r.right - r.left);
    const int height = std::max(1L, r.bottom - r.top);
    const int side = std::min(width, height);
    const int x = r.left + (width - side) / 2;
    const int y = r.top + (height - side) / 2;
    graphics.DrawImage(&sprite, Gdiplus::Rect(x, y, side, side));
}

inline void DrawPrimaryState(
    Gdiplus::Graphics& graphics,
    const RECT& rect,
    LPCWSTR text,
    int count,
    bool active,
    bool hovered) {
    if (!text || (!active && !hovered)) return;

    const bool danger = Exact(text, count, L"取消");
    const COLORREF accent = danger ? snaplite::ui::kDanger : snaplite::ui::kAccentStrong;
    const Gdiplus::RectF pill(
        static_cast<float>(rect.left + 1),
        static_cast<float>(rect.top + 1),
        static_cast<float>(rect.right - rect.left - 2),
        static_cast<float>(rect.bottom - rect.top - 2));
    FillSoftRoundRect(
        graphics,
        pill,
        16.0f,
        active ? ToColor(accent, 30) : ToColor(accent, 13));

    Gdiplus::Pen focus(ToColor(accent, active ? 225 : 110), active ? 2.2f : 1.2f);
    const float y = static_cast<float>(rect.bottom - 2);
    graphics.DrawLine(
        &focus,
        static_cast<float>(rect.left + (active ? 10 : 14)),
        y,
        static_cast<float>(rect.right - (active ? 10 : 14)),
        y);
}

inline void DrawAnimeDividers(
    Gdiplus::Graphics& graphics,
    int width,
    int height,
    int primaryWidth,
    int actionStart,
    int primaryHeight,
    int secondaryHeight) {
    Gdiplus::Pen divider(Gdiplus::Color(255, 239, 226, 231), 1.0f);
    if (height > primaryHeight) {
        graphics.DrawLine(
            &divider,
            14.0f,
            static_cast<float>(primaryHeight),
            static_cast<float>(width - 14),
            static_cast<float>(primaryHeight));
    }
    if (height > primaryHeight + secondaryHeight) {
        const float y = static_cast<float>(primaryHeight + secondaryHeight);
        graphics.DrawLine(&divider, 14.0f, y, static_cast<float>(width - 14), y);
    }

    const int splitRight = std::min(width - 12, primaryWidth - 8);
    if (actionStart > 14 && actionStart < splitRight) {
        Gdiplus::Pen split(Gdiplus::Color(255, 232, 214, 221), 1.0f);
        graphics.DrawLine(
            &split,
            static_cast<float>(actionStart),
            14.0f,
            static_cast<float>(actionStart),
            static_cast<float>(primaryHeight - 14));
    }
}

} // namespace anime_skin

inline void DrawAnimeBackdrop(
    HDC dc,
    const RECT& all,
    int primarySplitX,
    int primaryWidth = -1,
    int primaryHeight = 52) {
    if (!dc) return;
    (void)primarySplitX;

    const int right = std::max(12L, all.right - 3);
    const int topRight = std::max(
        12,
        std::min(right, primaryWidth > 0 ? primaryWidth - 2 : right));
    const RECT shadow{2, 4, right, std::max(7L, all.bottom - 1)};
    snaplite::ui::FillRoundRect(
        dc, shadow, 20, RGB(238, 222, 228), RGB(238, 222, 228));

    if (all.bottom > primaryHeight) {
        const RECT secondary{
            3,
            primaryHeight - 2,
            right,
            std::max(primaryHeight, static_cast<int>(all.bottom) - 2)};
        snaplite::ui::FillRoundRect(
            dc, secondary, 18, RGB(255, 252, 250), snaplite::ui::kBorder);
    }

    const RECT primary{3, 1, topRight, primaryHeight - 1};
    snaplite::ui::FillRoundRect(
        dc, primary, 20, RGB(255, 253, 252), snaplite::ui::kBorder);
}

inline int DrawTextOrIcon(HDC dc, LPCWSTR text, int count, LPRECT rect, UINT format) {
    if (!dc || !text || !rect)
        return DrawBaseTextOrIcon(dc, text, count, rect, format);

    if (!anime_skin::PrimaryLabel(text, count))
        return DrawBaseTextOrIcon(dc, text, count, rect, format);

    const anime_skin::Girl girl = anime_skin::GirlForLabel(text, count);
    anime_skin::DrawTile(dc, *rect, girl);

    const int width = rect->right - rect->left;
    const int height = rect->bottom - rect->top;
    const int centerY = (rect->top + rect->bottom) / 2;
    const int mascotSide = std::clamp(std::min(width - 16, height - 8), 24, 30);
    RECT mascot{
        rect->left + 3,
        centerY - mascotSide / 2,
        rect->left + 3 + mascotSide,
        centerY + mascotSide / 2};
    const int badgeSide = std::clamp(height / 2, 18, 20);
    RECT badge{
        rect->right - badgeSide - 3,
        centerY - badgeSide / 2,
        rect->right - 3,
        centerY + badgeSide / 2};
    RECT symbol{
        badge.left + 3,
        badge.top + 3,
        badge.right - 3,
        badge.bottom - 3};

    anime_skin::DrawChibi(dc, mascot, girl);

    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush badgeShadow(Gdiplus::Color(26, 58, 48, 54));
    graphics.FillEllipse(
        &badgeShadow,
        static_cast<float>(badge.left),
        static_cast<float>(badge.top + 1),
        static_cast<float>(badge.right - badge.left),
        static_cast<float>(badge.bottom - badge.top));
    Gdiplus::SolidBrush badgeFill(Gdiplus::Color(250, 255, 253, 252));
    graphics.FillEllipse(
        &badgeFill,
        static_cast<float>(badge.left),
        static_cast<float>(badge.top),
        static_cast<float>(badge.right - badge.left),
        static_cast<float>(badge.bottom - badge.top));
    const COLORREF badgeBorder = Exact(text, count, L"取消")
        ? snaplite::ui::kDanger
        : anime_skin::Accent(girl);
    Gdiplus::Pen badgeRing(anime_skin::ToColor(badgeBorder, 150), 1.0f);
    graphics.DrawEllipse(
        &badgeRing,
        static_cast<float>(badge.left) + 0.5f,
        static_cast<float>(badge.top) + 0.5f,
        static_cast<float>(badge.right - badge.left - 1),
        static_cast<float>(badge.bottom - badge.top - 1));

    const COLORREF oldColor = GetTextColor(dc);
    SetTextColor(
        dc,
        Exact(text, count, L"取消") ? snaplite::ui::kDanger : snaplite::ui::kText);
    const int result = DrawBaseTextOrIcon(
        dc, text, count, &symbol, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(dc, oldColor);
    return result;
}

} // namespace snaplite::toolbaricons_gdi
