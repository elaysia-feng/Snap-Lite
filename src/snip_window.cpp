// Theme and editor-state wrapper for the original snip implementation.
// The capture/selection engine remains isolated in snip_window_original.inc.

#include "snip_window.h"

#include <windowsx.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <string>
#include <utility>

namespace snaplite::detail {

COLORREF gAnnotationColor = RGB(235, 70, 70);
int gTextSizePt = 16;

HFONT ThemedCreateFontW(
    int height,
    int width,
    int escapement,
    int orientation,
    int weight,
    DWORD italic,
    DWORD underline,
    DWORD strikeOut,
    DWORD charSet,
    DWORD outPrecision,
    DWORD clipPrecision,
    DWORD quality,
    DWORD pitchAndFamily,
    LPCWSTR faceName) {
    if (height == -20 && faceName && _wcsicmp(faceName, L"Segoe UI") == 0) {
        const UINT dpi = GetDpiForSystem();
        height = -MulDiv(std::clamp(gTextSizePt, 10, 72), static_cast<int>(dpi), 72);
    }
    return ::CreateFontW(
        height,
        width,
        escapement,
        orientation,
        weight,
        italic,
        underline,
        strikeOut,
        charSet,
        outPrecision,
        clipPrecision,
        quality,
        pitchAndFamily,
        faceName);
}

HWND ThemedCreateWindowExW(
    DWORD exStyle,
    LPCWSTR className,
    LPCWSTR windowName,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    HWND parent,
    HMENU menu,
    HINSTANCE instance,
    LPVOID param) {
    if (reinterpret_cast<ULONG_PTR>(className) > 0xFFFF &&
        className && _wcsicmp(className, L"EDIT") == 0) {
        exStyle |= WS_EX_TRANSPARENT;
        style &= ~WS_BORDER;
    }

    return ::CreateWindowExW(
        exStyle,
        className,
        windowName,
        style,
        x,
        y,
        width,
        height,
        parent,
        menu,
        instance,
        param);
}

}  // namespace snaplite::detail

namespace Gdiplus {

class SnapLiteThemeColor : public Color {
public:
    SnapLiteThemeColor() : Color() {}
    explicit SnapLiteThemeColor(ARGB value) : Color(value) {}

    SnapLiteThemeColor(BYTE alpha, BYTE red, BYTE green, BYTE blue)
        : Color(ThemeArgb(alpha, red, green, blue)) {}

    SnapLiteThemeColor(BYTE red, BYTE green, BYTE blue)
        : Color(ThemeArgb(255, red, green, blue)) {}

private:
    static ARGB Pack(BYTE alpha, BYTE red, BYTE green, BYTE blue) {
        return (static_cast<ARGB>(alpha) << 24) |
               (static_cast<ARGB>(red) << 16) |
               (static_cast<ARGB>(green) << 8) |
               static_cast<ARGB>(blue);
    }

    static ARGB ThemeArgb(BYTE alpha, BYTE red, BYTE green, BYTE blue) {
        if (red == 27 && green == 30 && blue == 35) {
            return Pack(alpha, 17, 24, 44);
        }
        if (red == 255 && green == 197 && blue == 61) {
            return Pack(alpha, 105, 225, 255);
        }
        if (red == 236 && green == 239 && blue == 244) {
            return Pack(alpha, 232, 247, 255);
        }
        if (red == 255 && green == 107 && blue == 91) {
            return Pack(alpha, 255, 112, 170);
        }
        if (red == 0 && green == 0 && blue == 0) {
            if (alpha == 148 || alpha == 66) {
                return Pack(alpha, 20, 25, 51);
            }
            if (alpha == 110) {
                return Pack(alpha, 8, 14, 32);
            }
            if (alpha == 13) {
                return Pack(alpha, 45, 95, 175);
            }
        }
        if (red == 255 && green == 255 && blue == 255) {
            if (alpha == 56) return Pack(86, 144, 231, 255);
            if (alpha == 38) return Pack(55, 112, 225, 255);
            if (alpha == 26) return Pack(38, 112, 225, 255);
        }
        return Pack(alpha, red, green, blue);
    }
};

}  // namespace Gdiplus

// The legacy implementation uses RGB(235,70,70) as annotation ink. Redirect
// only that exact token to the user-selected colour, leaving UI colours alone.
#ifdef RGB
#undef RGB
#endif
#define SNAP_PACK_RGB(r, g, b) \
    ((COLORREF)(((BYTE)(r)) | ((WORD)((BYTE)(g)) << 8) | (((DWORD)(BYTE)(b)) << 16)))
#define RGB(r, g, b) \
    ((((r) == 235) && ((g) == 70) && ((b) == 70)) \
         ? snaplite::detail::gAnnotationColor \
         : SNAP_PACK_RGB((r), (g), (b)))

#define CreateFontW snaplite::detail::ThemedCreateFontW
#define CreateWindowExW snaplite::detail::ThemedCreateWindowExW
#define Color SnapLiteThemeColor
#include "snip_window_original.inc"
#undef Color
#undef CreateWindowExW
#undef CreateFontW
#undef RGB
#undef SNAP_PACK_RGB

namespace snaplite {

bool SnipWindow::UiHasSelection() const {
    return selected_;
}

RECT SnipWindow::UiSelectionRect() const {
    return selected_ ? NormalizedSelection() : RECT{};
}

RECT SnipWindow::UiLegacyToolbarRect() const {
    return ToolbarRect();
}

int SnipWindow::UiActiveTool() const {
    switch (tool_) {
    case Tool::Rectangle: return 0;
    case Tool::Arrow: return 1;
    case Tool::Pen: return 2;
    case Tool::Mosaic: return 3;
    case Tool::Text: return 4;
    default: return -1;
    }
}

void SnipWindow::UiSetTool(int toolIndex) {
    if (textEdit_) {
        CommitTextEdit();
    }

    Tool next = Tool::None;
    switch (toolIndex) {
    case 0: next = Tool::Rectangle; break;
    case 1: next = Tool::Arrow; break;
    case 2: next = Tool::Pen; break;
    case 3: next = Tool::Mosaic; break;
    case 4: next = Tool::Text; break;
    default: break;
    }

    tool_ = tool_ == next ? Tool::None : next;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

COLORREF SnipWindow::UiColor() const {
    return annotationColor_;
}

void SnipWindow::UiSetColor(COLORREF color) {
    annotationColor_ = color;
    detail::gAnnotationColor = color;
    if (textEdit_) {
        InvalidateRect(textEdit_, nullptr, TRUE);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int SnipWindow::UiTextSize() const {
    return textSizePt_;
}

void SnipWindow::UiSetTextSize(int points) {
    textSizePt_ = std::clamp(points, 10, 72);
    detail::gTextSizePt = textSizePt_;

    if (textFont_) {
        DeleteObject(textFont_);
        textFont_ = nullptr;
    }

    if (textEdit_) {
        const UINT dpi = GetDpiForWindow(hwnd_);
        const int height = -MulDiv(textSizePt_, static_cast<int>(dpi), 72);
        textFont_ = ::CreateFontW(
            height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        if (textFont_) {
            SendMessageW(textEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(textFont_), TRUE);
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SnipWindow::UiUndo() {
    Undo();
}

void SnipWindow::UiRedo() {
    Redo();
}

void SnipWindow::UiFinish(FinishAction action) {
    Finish(action);
}

void SnipWindow::UiCancel() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
    }
}

HWND SnipWindow::UiHwnd() const {
    return hwnd_;
}

}  // namespace snaplite
