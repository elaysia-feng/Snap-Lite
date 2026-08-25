#pragma once

// Reuse all of the deterministic GDI tool glyphs, but rename the original
// dispatcher so this skin can wrap primary toolbar buttons with tiny
// character mascots without changing secondary-option rendering.
#define DrawTextOrIcon DrawBaseTextOrIcon
#include "toolbar_icon_render_gdi.h"
#undef DrawTextOrIcon

#include <array>

namespace snaplite::toolbaricons_gdi {

namespace anime_skin {

enum class Girl : int { Yui = 0, Mio, Mugi, Ritsu, Azusa };

inline COLORREF Hair(Girl girl) {
    switch (girl) {
    case Girl::Yui:   return RGB(117, 74, 52);
    case Girl::Mio:   return RGB(42, 38, 54);
    case Girl::Mugi:  return RGB(220, 176, 91);
    case Girl::Ritsu: return RGB(153, 92, 47);
    case Girl::Azusa: return RGB(48, 42, 48);
    }
    return RGB(90, 70, 60);
}

inline COLORREF Accent(Girl girl) {
    switch (girl) {
    case Girl::Yui:   return RGB(235, 119, 132);
    case Girl::Mio:   return RGB(122, 111, 188);
    case Girl::Mugi:  return RGB(229, 177, 74);
    case Girl::Ritsu: return RGB(239, 145, 63);
    case Girl::Azusa: return RGB(75, 160, 157);
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
    // Fixed pseudo-random distribution: every tool keeps a stable mascot while
    // the five girls are mixed across the toolbar instead of being five big tabs.
    if (Exact(text, count, L"选择"))   return Girl::Yui;
    if (Exact(text, count, L"形状"))   return Girl::Azusa;
    if (Exact(text, count, L"箭头"))   return Girl::Mugi;
    if (Exact(text, count, L"画笔"))   return Girl::Ritsu;
    if (Exact(text, count, L"马赛克")) return Girl::Mio;
    if (Exact(text, count, L"文字"))   return Girl::Azusa;
    if (Exact(text, count, L"图钉"))   return Girl::Mugi;
    if (Exact(text, count, L"撤销"))   return Girl::Yui;
    if (Exact(text, count, L"重做"))   return Girl::Ritsu;
    if (Exact(text, count, L"复制"))   return Girl::Mio;
    if (Exact(text, count, L"保存"))   return Girl::Mugi;
    if (Exact(text, count, L"另存为")) return Girl::Yui;
    return Girl::Azusa;
}

inline void DrawTile(HDC dc, const RECT& r, Girl girl) {
    RECT tile{r.left + 1, r.top + 1, r.right - 1, r.bottom - 1};
    const COLORREF accent = Accent(girl);
    const COLORREF fill = RGB(
        std::min(255, 247 + GetRValue(accent) / 32),
        std::min(255, 243 + GetGValue(accent) / 40),
        std::min(255, 239 + GetBValue(accent) / 48));

    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(
        (GetRValue(accent) + 230) / 2,
        (GetGValue(accent) + 218) / 2,
        (GetBValue(accent) + 218) / 2));
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, tile.left, tile.top, tile.right, tile.bottom, 10, 10);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

inline void DrawChibi(HDC dc, const RECT& r, Girl girl) {
    const int cx = (r.left + r.right) / 2;
    const int cy = (r.top + r.bottom) / 2;
    const COLORREF hair = Hair(girl);
    const COLORREF accent = Accent(girl);

    // Back hair / side locks.
    HBRUSH hairBrush = CreateSolidBrush(hair);
    HPEN hairPen = CreatePen(PS_SOLID, 1, hair);
    HGDIOBJ oldBrush = SelectObject(dc, hairBrush);
    HGDIOBJ oldPen = SelectObject(dc, hairPen);
    Ellipse(dc, cx - 8, cy - 9, cx + 8, cy + 8);
    if (girl == Girl::Mio || girl == Girl::Azusa) {
        Ellipse(dc, cx - 9, cy - 2, cx - 4, cy + 10);
        Ellipse(dc, cx + 4, cy - 2, cx + 9, cy + 10);
    }
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(hairPen);
    DeleteObject(hairBrush);

    // Face.
    HBRUSH skin = CreateSolidBrush(RGB(255, 224, 207));
    HPEN skinPen = CreatePen(PS_SOLID, 1, RGB(205, 151, 137));
    oldBrush = SelectObject(dc, skin);
    oldPen = SelectObject(dc, skinPen);
    Ellipse(dc, cx - 6, cy - 6, cx + 6, cy + 6);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(skinPen);
    DeleteObject(skin);

    // Bangs keep the tiny portraits readable at 100% DPI.
    HPEN bang = CreatePen(PS_SOLID, 2, hair);
    oldPen = SelectObject(dc, bang);
    MoveToEx(dc, cx - 5, cy - 5, nullptr); LineTo(dc, cx - 2, cy - 1);
    MoveToEx(dc, cx, cy - 6, nullptr); LineTo(dc, cx, cy - 1);
    MoveToEx(dc, cx + 5, cy - 5, nullptr); LineTo(dc, cx + 2, cy - 1);
    SelectObject(dc, oldPen);
    DeleteObject(bang);

    // Eyes + mouth.
    HBRUSH eye = CreateSolidBrush(RGB(67, 48, 54));
    RECT leftEye{cx - 4, cy, cx - 2, cy + 2};
    RECT rightEye{cx + 2, cy, cx + 4, cy + 2};
    FillRect(dc, &leftEye, eye);
    FillRect(dc, &rightEye, eye);
    DeleteObject(eye);
    SetPixel(dc, cx, cy + 4, RGB(190, 94, 103));

    // Character-specific identifying accessory.
    HPEN accentPen = CreatePen(PS_SOLID, 2, accent);
    oldPen = SelectObject(dc, accentPen);
    if (girl == Girl::Yui) {
        MoveToEx(dc, cx + 4, cy - 7, nullptr); LineTo(dc, cx + 7, cy - 5);
        MoveToEx(dc, cx + 4, cy - 5, nullptr); LineTo(dc, cx + 7, cy - 7);
    } else if (girl == Girl::Ritsu) {
        Arc(dc, cx - 7, cy - 10, cx + 7, cy, cx - 6, cy - 4, cx + 6, cy - 4);
    } else if (girl == Girl::Mugi) {
        MoveToEx(dc, cx - 5, cy - 1, nullptr); LineTo(dc, cx - 2, cy - 2);
        MoveToEx(dc, cx + 2, cy - 2, nullptr); LineTo(dc, cx + 5, cy - 1);
    } else if (girl == Girl::Azusa) {
        MoveToEx(dc, cx - 8, cy - 3, nullptr); LineTo(dc, cx - 11, cy - 6);
        MoveToEx(dc, cx + 8, cy - 3, nullptr); LineTo(dc, cx + 11, cy - 6);
    }
    SelectObject(dc, oldPen);
    DeleteObject(accentPen);

    // Tiny uniform ribbon/body hint.
    HBRUSH body = CreateSolidBrush(RGB(63, 59, 62));
    RECT torso{cx - 5, cy + 6, cx + 5, cy + 10};
    FillRect(dc, &torso, body);
    DeleteObject(body);
    HBRUSH ribbon = CreateSolidBrush(accent);
    RECT bow{cx - 2, cy + 5, cx + 3, cy + 8};
    FillRect(dc, &bow, ribbon);
    DeleteObject(ribbon);
}

inline void DrawMusicNote(HDC dc, int x, int y, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    Ellipse(dc, x, y + 6, x + 4, y + 10);
    MoveToEx(dc, x + 3, y + 7, nullptr); LineTo(dc, x + 3, y);
    MoveToEx(dc, x + 3, y, nullptr); LineTo(dc, x + 8, y + 2);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

inline void DrawSakura(HDC dc, int x, int y, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, x - 2, y - 6, x + 2, y - 1);
    Ellipse(dc, x + 1, y - 3, x + 6, y + 1);
    Ellipse(dc, x - 6, y - 3, x - 1, y + 1);
    Ellipse(dc, x + 1, y, x + 5, y + 5);
    Ellipse(dc, x - 5, y, x - 1, y + 5);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);
}

} // namespace anime_skin

inline void DrawAnimeBackdrop(HDC dc, const RECT& r) {
    if (!dc) return;
    const int width = r.right - r.left;
    if (width < 200) return;

    const COLORREF note = RGB(235, 201, 206);
    const COLORREF flower = RGB(246, 205, 214);
    // Keep the decorative cluster in the otherwise-empty right side of the
    // primary row so it never competes with real controls.
    anime_skin::DrawMusicNote(dc, r.right - 190, 12, note);
    anime_skin::DrawMusicNote(dc, r.right - 145, 18, note);
    anime_skin::DrawMusicNote(dc, r.right - 98, 9, note);
    anime_skin::DrawSakura(dc, r.right - 53, 18, flower);
    anime_skin::DrawSakura(dc, r.right - 26, 29, RGB(249, 220, 226));
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
        rect->top + std::max(1, (height - 24) / 2),
        rect->left + std::min(21, std::max(16, width / 2)),
        rect->bottom - std::max(1, (height - 24) / 2)};
    RECT symbol{
        rect->left + std::max(18, width / 2 - 1),
        rect->top + 6,
        rect->right - 2,
        rect->bottom - 4};

    anime_skin::DrawChibi(dc, mascot, girl);

    const COLORREF oldColor = GetTextColor(dc);
    SetTextColor(dc, Exact(text, count, L"取消") ? RGB(198, 72, 82) : RGB(74, 63, 64));
    const int result = DrawBaseTextOrIcon(dc, text, count, &symbol,
                                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(dc, oldColor);
    return result;
}

} // namespace snaplite::toolbaricons_gdi
