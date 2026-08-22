#pragma once

#include <windows.h>
#include <commctrl.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>
#include <cwchar>

namespace snaplite::toolbarvisual {

constexpr UINT_PTR kTextSurfaceSubclassId = 0x534C2201;
constexpr UINT_PTR kLiveEditSubclassId = 0x534C2202;

inline COLORREF MapColorRef(COLORREF color) {
    if (color == RGB(250, 250, 251)) return RGB(252, 250, 247);
    if (color == RGB(63, 78, 104)) return RGB(99, 82, 62);
    if (color == RGB(65, 66, 70)) return RGB(58, 56, 53);
    if (color == RGB(170, 72, 72)) return RGB(196, 76, 72);
    if (color == RGB(72, 73, 77)) return RGB(70, 67, 63);
    if (color == RGB(59, 76, 108)) return RGB(105, 84, 62);
    if (color == RGB(78, 79, 84)) return RGB(82, 78, 73);
    if (color == RGB(116, 117, 122)) return RGB(137, 128, 118);
    return color;
}

inline HBRUSH CreateSolidBrushThemed(COLORREF color) {
    return ::CreateSolidBrush(MapColorRef(color));
}

inline COLORREF SetTextColorThemed(HDC dc, COLORREF color) {
    return ::SetTextColor(dc, MapColorRef(color));
}

inline bool ClassEquals(HWND hwnd, const wchar_t* expected) {
    if (!hwnd || !expected) return false;
    wchar_t name[96]{};
    if (!GetClassNameW(hwnd, name, static_cast<int>(_countof(name)))) return false;
    return wcscmp(name, expected) == 0;
}

inline bool IsToolbarWindow(HWND hwnd) {
    return ClassEquals(hwnd, L"SnapLiteEditorToolbarChild");
}

inline bool IsTextAnnotationWindow(HWND hwnd) {
    return ClassEquals(hwnd, L"SnapLiteTextAnnotationChild");
}

inline LRESULT CALLBACK TextSurfaceSubclassProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR, DWORD_PTR) {

    if (message == WM_ERASEBKGND) {
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND parent = GetParent(hwnd);
        if (!dc || !parent) return 1;

        RECT client{};
        GetClientRect(hwnd, &client);
        POINT source{0, 0};
        MapWindowPoints(hwnd, parent, &source, 1);

        HDC parentDc = GetDC(parent);
        if (parentDc) {
            ::BitBlt(
                dc, 0, 0,
                client.right - client.left,
                client.bottom - client.top,
                parentDc, source.x, source.y, SRCCOPY);
            ReleaseDC(parent, parentDc);
        }
        return 1;
    }

    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, TextSurfaceSubclassProc, kTextSurfaceSubclassId);
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

inline void RefreshTextEditSurface(HWND edit) {
    HWND surface = GetParent(edit);
    if (!surface || !IsTextAnnotationWindow(surface)) return;

    // First restore the screenshot pixels under the transparent text surface,
    // then repaint the EDIT with its current buffer. This makes Backspace and
    // Delete visually immediate instead of leaving the old glyph behind.
    RedrawWindow(surface, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    InvalidateRect(edit, nullptr, FALSE);
    UpdateWindow(edit);
}

inline LRESULT CALLBACK LiveEditSubclassProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR, DWORD_PTR) {

    const bool refreshAfter =
        message == WM_CHAR ||
        message == WM_CUT ||
        message == WM_CLEAR ||
        message == WM_PASTE ||
        message == WM_UNDO ||
        message == WM_IME_COMPOSITION ||
        (message == WM_KEYDOWN && wParam == VK_DELETE);

    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, LiveEditSubclassProc, kLiveEditSubclassId);
        return DefSubclassProc(hwnd, message, wParam, lParam);
    }

    const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
    if (refreshAfter && IsWindow(hwnd)) RefreshTextEditSurface(hwnd);
    return result;
}

inline HWND CreateWindowExWThemed(
    DWORD exStyle, LPCWSTR className, LPCWSTR windowName, DWORD style,
    int x, int y, int width, int height, HWND parent, HMENU menu,
    HINSTANCE instance, LPVOID param) {

    HWND hwnd = ::CreateWindowExW(
        exStyle, className, windowName, style,
        x, y, width, height, parent, menu, instance, param);
    if (!hwnd || reinterpret_cast<ULONG_PTR>(className) <= 0xFFFF || !className) {
        return hwnd;
    }

    if (wcscmp(className, L"SnapLiteTextAnnotationChild") == 0) {
        SetWindowSubclass(hwnd, TextSurfaceSubclassProc, kTextSurfaceSubclassId, 0);
    } else if (wcscmp(className, L"EDIT") == 0 && parent && IsTextAnnotationWindow(parent)) {
        SetWindowSubclass(hwnd, LiveEditSubclassProc, kLiveEditSubclassId, 0);
    }
    return hwnd;
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
    explicit SnapLiteThemeColor(ARGB argb) : Color(argb) {}
    SnapLiteThemeColor(BYTE red, BYTE green, BYTE blue)
        : Color(snaplite::toolbarvisual::MapArgb(255, red, green, blue)) {}
    SnapLiteThemeColor(BYTE alpha, BYTE red, BYTE green, BYTE blue)
        : Color(snaplite::toolbarvisual::MapArgb(alpha, red, green, blue)) {}
};

}  // namespace Gdiplus

#define Color SnapLiteThemeColor
#define CreateWindowExW(...) snaplite::toolbarvisual::CreateWindowExWThemed(__VA_ARGS__)
#define CreateSolidBrush(...) snaplite::toolbarvisual::CreateSolidBrushThemed(__VA_ARGS__)
#define SetTextColor(...) snaplite::toolbarvisual::SetTextColorThemed(__VA_ARGS__)
#define BitBlt(...) snaplite::toolbarvisual::BitBltThemed(__VA_ARGS__)
