#pragma once

#include <windows.h>
#include <gdiplus.h>
#include <cwchar>

namespace snaplite::toolbarvisual {

inline COLORREF MapColorRef(COLORREF color) {
    if (color == RGB(250, 250, 251)) return RGB(252, 250, 247);  // toolbar surface
    if (color == RGB(63, 78, 104)) return RGB(99, 82, 62);      // active primary ink
    if (color == RGB(65, 66, 70)) return RGB(58, 56, 53);       // primary ink
    if (color == RGB(170, 72, 72)) return RGB(196, 76, 72);     // cancel ink
    if (color == RGB(72, 73, 77)) return RGB(70, 67, 63);       // action ink
    if (color == RGB(59, 76, 108)) return RGB(105, 84, 62);     // selected secondary ink
    if (color == RGB(78, 79, 84)) return RGB(82, 78, 73);       // secondary ink
    if (color == RGB(116, 117, 122)) return RGB(137, 128, 118); // hint ink
    return color;
}

inline HBRUSH CreateSolidBrushThemed(COLORREF color) {
    return ::CreateSolidBrush(MapColorRef(color));
}

inline COLORREF SetTextColorThemed(HDC dc, COLORREF color) {
    return ::SetTextColor(dc, MapColorRef(color));
}

inline bool IsToolbarWindow(HWND hwnd) {
    if (!hwnd) return false;
    wchar_t name[96]{};
    if (!GetClassNameW(hwnd, name, static_cast<int>(std::size(name)))) return false;
    return wcscmp(name, L"SnapLiteEditorToolbarChild") == 0;
}

inline BOOL BitBltThemed(HDC dest, int x, int y, int cx, int cy,
                         HDC src, int sx, int sy, DWORD rop) {
    const BOOL result = ::BitBlt(dest, x, y, cx, cy, src, sx, sy, rop);

    HWND hwnd = WindowFromDC(dest);
    if (!result || !IsToolbarWindow(hwnd)) return result;

    RECT client{};
    if (!GetClientRect(hwnd, &client)) return result;
    if (client.right <= 1 || client.bottom <= 1) return result;

    HPEN border = CreatePen(PS_SOLID, 1, RGB(229, 223, 216));
    HGDIOBJ oldPen = SelectObject(dest, border);
    HGDIOBJ oldBrush = SelectObject(dest, GetStockObject(NULL_BRUSH));
    RoundRect(dest, 0, 0, client.right, client.bottom, 18, 18);
    SelectObject(dest, oldBrush);
    SelectObject(dest, oldPen);
    DeleteObject(border);
    return result;
}

inline Gdiplus::ARGB MapArgb(BYTE alpha, BYTE red, BYTE green, BYTE blue) {
    auto pack = [](BYTE a, BYTE r, BYTE g, BYTE b) -> Gdiplus::ARGB {
        return (static_cast<Gdiplus::ARGB>(a) << 24) |
               (static_cast<Gdiplus::ARGB>(r) << 16) |
               (static_cast<Gdiplus::ARGB>(g) << 8) |
               static_cast<Gdiplus::ARGB>(b);
    };

    if (red == 229 && green == 230 && blue == 233) return pack(alpha, 232, 226, 219);
    if (red == 232 && green == 236 && blue == 244) return pack(alpha, 240, 233, 224);
    if (red == 241 && green == 242 && blue == 244) return pack(alpha, 247, 243, 238);
    if (red == 250 && green == 236 && blue == 236) return pack(alpha, 251, 237, 235);
    if (red == 230 && green == 235 && blue == 245) return pack(alpha, 239, 232, 222);
    if (red == 240 && green == 241 && blue == 243) return pack(alpha, 247, 243, 238);
    if (red == 247 && green == 247 && blue == 248) return pack(alpha, 252, 250, 247);
    if (red == 75 && green == 94 && blue == 128) return pack(alpha, 151, 128, 99);
    if (red == 190 && green == 192 && blue == 197) return pack(alpha, 198, 190, 181);
    return pack(alpha, red, green, blue);
}

}  // namespace snaplite::toolbarvisual

namespace Gdiplus {

class SnapLiteThemeColor : public Color {
public:
    SnapLiteThemeColor() : Color() {}
    explicit SnapLiteThemeColor(ARGB argb)
        : Color(argb) {}
    SnapLiteThemeColor(BYTE red, BYTE green, BYTE blue)
        : Color(snaplite::toolbarvisual::MapArgb(255, red, green, blue)) {}
    SnapLiteThemeColor(BYTE alpha, BYTE red, BYTE green, BYTE blue)
        : Color(snaplite::toolbarvisual::MapArgb(alpha, red, green, blue)) {}
};

}  // namespace Gdiplus

#define Color SnapLiteThemeColor
#define CreateSolidBrush(...) snaplite::toolbarvisual::CreateSolidBrushThemed(__VA_ARGS__)
#define SetTextColor(...) snaplite::toolbarvisual::SetTextColorThemed(__VA_ARGS__)
#define BitBlt(...) snaplite::toolbarvisual::BitBltThemed(__VA_ARGS__)
