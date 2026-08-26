#pragma once

// Primary-toolbar anime skin. Tool glyphs stay deterministic GDI primitives;
// the five mascot portraits come from the approved compact toolbar concept and
// are embedded as tiny indexed sprites so the release remains a single EXE.
#define DrawTextOrIcon DrawBaseTextOrIcon
#include "toolbar_icon_render_gdi.h"
#undef DrawTextOrIcon

#include "toolbar_chibi_assets.h"

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
    static const auto azusa = DecodeSprite(snaplite::toolbar_chibi_assets::kAzusa);

    switch (girl) {
    case Girl::Yui:   return yui;
    case Girl::Mio:   return mio;
    case Girl::Mugi:  return mugi;
    case Girl::Ritsu: return ritsu;
    case Girl::Azusa: return azusa;
    }
    return yui;
}

inline void DrawTile(HDC dc, const RECT& r, Girl girl) {
    RECT tile{r.left + 1, r.top + 1, r.right - 1, r.bottom - 1};
    const COLORREF accent = Accent(girl);

    HBRUSH brush = CreateSolidBrush(RGB(255, 250, 248));
    HPEN pen = CreatePen(
        PS_SOLID,
        1,
        RGB((GetRValue(accent) + 238) / 2,
            (GetGValue(accent) + 226) / 2,
            (GetBValue(accent) + 228) / 2));
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, tile.left, tile.top, tile.right, tile.bottom, 9, 9);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
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

} // namespace anime_skin

inline void DrawAnimeBackdrop(HDC, const RECT&) {
    // Final approved version deliberately has no decorative empty tail.
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
    RECT mascot{
        rect->left + 2,
        rect->top + std::max(2, (height - 28) / 2),
        rect->left + std::min(25, std::max(21, width / 2 + 1)),
        rect->bottom - std::max(2, (height - 28) / 2)};
    RECT symbol{
        rect->left + std::max(21, width / 2),
        rect->top + 5,
        rect->right - 2,
        rect->bottom - 4};

    anime_skin::DrawChibi(dc, mascot, girl);

    const COLORREF oldColor = GetTextColor(dc);
    SetTextColor(
        dc,
        Exact(text, count, L"取消") ? RGB(201, 75, 83) : RGB(74, 63, 64));
    const int result = DrawBaseTextOrIcon(
        dc, text, count, &symbol, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(dc, oldColor);
    return result;
}

} // namespace snaplite::toolbaricons_gdi
