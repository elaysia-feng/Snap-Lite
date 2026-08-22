#include "snip_window.h"

#include <algorithm>

namespace snaplite {

void SnipWindow::UiBakeTextOverlays(const std::vector<TextOverlay>& overlays) {
    if (!capture_ || overlays.empty()) {
        return;
    }

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) {
        return;
    }

    const HGDIOBJ oldBitmap = SelectObject(dc, capture_);
    SetBkMode(dc, TRANSPARENT);

    const UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : GetDpiForSystem();

    for (const auto& overlay : overlays) {
        if (overlay.text.empty()) {
            continue;
        }

        const int points = std::clamp(overlay.sizePt, 10, 72);
        const int height = -MulDiv(points, static_cast<int>(dpi), 72);
        HFONT font = CreateFontW(
            height,
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_TT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei UI");

        const HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
        SetTextColor(dc, overlay.color);
        TextOutW(
            dc,
            overlay.origin.x,
            overlay.origin.y,
            overlay.text.c_str(),
            static_cast<int>(overlay.text.size()));

        if (oldFont) {
            SelectObject(dc, oldFont);
        }
        if (font) {
            DeleteObject(font);
        }
    }

    SelectObject(dc, oldBitmap);
    DeleteDC(dc);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

}  // namespace snaplite
