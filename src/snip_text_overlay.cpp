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

        // Render each line at an incremented y so explicit \r\n separators
        // produce hard breaks without soft-wrapping to a box width.
        TEXTMETRICW metrics{};
        if (oldFont) GetTextMetricsW(dc, &metrics);
        const int lineHeight = std::max<int>(1, static_cast<int>(metrics.tmHeight));
        int y = overlay.origin.y;
        const int x = overlay.origin.x;

        size_t start = 0;
        const std::wstring& text = overlay.text;
        while (start <= text.size()) {
            const size_t end = text.find(L'\n', start);
            const size_t chunkLen = (end == std::wstring::npos)
                                        ? (text.size() - start)
                                        : (end - start);
            const std::wstring line = text.substr(start, chunkLen);
            if (!line.empty()) {
                TextOutW(dc, x, y, line.c_str(), static_cast<int>(line.size()));
            }
            y += lineHeight;
            if (end == std::wstring::npos) break;
            start = end + 1;
        }

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
