from __future__ import annotations

import sys
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, got {count}")
    return text.replace(old, new, 1)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: generate_anime_toolbar_source.py <input.cpp> <output.cpp>", file=sys.stderr)
        return 2

    source = Path(sys.argv[1])
    output = Path(sys.argv[2])
    text = source.read_text(encoding="utf-8")

    text = replace_once(
        text,
        '#include "toolbar_icon_render_gdi.h"',
        '#include "toolbar_icon_render_gdi_anime.h"',
        "anime icon renderer include",
    )
    text = replace_once(
        text,
        "constexpr int kToolWidth = 78;",
        "constexpr int kToolWidth = 48;",
        "compact primary tool width",
    )
    text = replace_once(
        text,
        "constexpr std::array<int, 7> kActionWidths = {46, 46, 46, 50, 50, 62, 50};",
        "constexpr std::array<int, 7> kActionWidths = {44, 44, 44, 44, 44, 48, 44};",
        "compact action widths",
    )

    # Keep the button size exactly as-is, but make the host width follow the
    # actual content. Expanded rows may be wider than the primary row; the
    # window region clips the unused top-right area instead of showing a long
    # empty strip.
    text = replace_once(
        text,
        "    int DesiredHeight() const {\n"
        "        return HasSecondaryOptions() ? kToolbarHeight : kPrimaryHeight;\n"
        "    }",
        "    int DesiredHeight() const {\n"
        "        return HasSecondaryOptions() ? kToolbarHeight : kPrimaryHeight;\n"
        "    }\n\n"
        "    int PrimaryContentWidth() const {\n"
        "        int width = kActionStart;\n"
        "        for (size_t i = 0; i < kActionWidths.size(); ++i) {\n"
        "            width += kActionWidths[i];\n"
        "            if (i + 1 < kActionWidths.size()) width += kActionGap;\n"
        "        }\n"
        "        return width + kPad;\n"
        "    }\n\n"
        "    int SecondaryContentWidth() const {\n"
        "        const auto items = BuildSecondary(category_);\n"
        "        if (items.empty()) return PrimaryContentWidth();\n"
        "        int width = kPad * 2;\n"
        "        for (size_t i = 0; i < items.size(); ++i) {\n"
        "            width += items[i].width;\n"
        "            if (i + 1 < items.size()) width += kGap;\n"
        "        }\n"
        "        return width;\n"
        "    }\n\n"
        "    int DesiredWidth() const {\n"
        "        const int primary = PrimaryContentWidth();\n"
        "        return HasSecondaryOptions() ? std::max(primary, SecondaryContentWidth()) : primary;\n"
        "    }",
        "dynamic toolbar dimensions",
    )

    text = replace_once(
        text,
        "    void ApplyRoundedRegion() {\n"
        "        if (!toolbar_) return;\n"
        "        RECT client{};\n"
        "        if (!GetClientRect(toolbar_, &client)) return;\n"
        "        const int width = std::max(1L, client.right - client.left);\n"
        "        const int height = std::max(1L, client.bottom - client.top);\n"
        "        const int ellipse = height <= kPrimaryHeight ? 18 : 16;\n"
        "        HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, ellipse, ellipse);\n"
        "        if (region && SetWindowRgn(toolbar_, region, TRUE) == 0) {\n"
        "            DeleteObject(region);\n"
        "        }\n"
        "    }",
        "    void ApplyRoundedRegion() {\n"
        "        if (!toolbar_) return;\n"
        "        RECT client{};\n"
        "        if (!GetClientRect(toolbar_, &client)) return;\n"
        "        const int width = std::max(1L, client.right - client.left);\n"
        "        const int height = std::max(1L, client.bottom - client.top);\n"
        "        const int primaryWidth = std::min(width, PrimaryContentWidth());\n\n"
        "        HRGN top = CreateRoundRectRgn(0, 0, primaryWidth + 1,\n"
        "                                          std::min(height, kPrimaryHeight) + 1, 18, 18);\n"
        "        if (!top) return;\n"
        "        if (height <= kPrimaryHeight) {\n"
        "            if (SetWindowRgn(toolbar_, top, TRUE) == 0) DeleteObject(top);\n"
        "            return;\n"
        "        }\n\n"
        "        HRGN lower = CreateRoundRectRgn(0, kPrimaryHeight - 1, width + 1, height + 1, 16, 16);\n"
        "        HRGN merged = CreateRectRgn(0, 0, 0, 0);\n"
        "        if (!lower || !merged) {\n"
        "            if (lower) DeleteObject(lower);\n"
        "            if (merged) DeleteObject(merged);\n"
        "            DeleteObject(top);\n"
        "            return;\n"
        "        }\n"
        "        CombineRgn(merged, top, lower, RGN_OR);\n"
        "        DeleteObject(top);\n"
        "        DeleteObject(lower);\n"
        "        if (SetWindowRgn(toolbar_, merged, TRUE) == 0) DeleteObject(merged);\n"
        "    }",
        "content-shaped toolbar region",
    )

    text = replace_once(
        text,
        "        const int height = DesiredHeight();",
        "        const int height = DesiredHeight();\n        const int width = DesiredWidth();",
        "dynamic toolbar width local",
    )
    text = replace_once(
        text,
        "        x = std::clamp(x, 6, std::max(6, static_cast<int>(client.right) - kToolbarWidth - 6));",
        "        x = std::clamp(x, 6, std::max(6, static_cast<int>(client.right) - width - 6));",
        "dynamic toolbar x clamp",
    )
    text = replace_once(
        text,
        "        SetWindowPos(toolbar_, HWND_TOP, x, y, kToolbarWidth, height,\n"
        "                     SWP_NOACTIVATE | SWP_SHOWWINDOW);",
        "        SetWindowPos(toolbar_, HWND_TOP, x, y, width, height,\n"
        "                     SWP_NOACTIVATE | SWP_SHOWWINDOW);",
        "dynamic toolbar SetWindowPos",
    )
    text = replace_once(
        text,
        "            if (x + item.width > kToolbarWidth - kPad) break;",
        "            if (x + item.width > DesiredWidth() - kPad) break;",
        "secondary row dynamic width",
    )

    # Paint to the actual client dimensions instead of the old 900px backing
    # bitmap. This keeps the visible frame snug in both primary-only and
    # expanded states.
    text = replace_once(
        text,
        "        HDC mem = CreateCompatibleDC(dc);\n"
        "        HBITMAP bitmap = CreateCompatibleBitmap(dc, kToolbarWidth, kToolbarHeight);",
        "        RECT paintClient{};\n"
        "        GetClientRect(hwnd, &paintClient);\n"
        "        const int paintWidth = std::max(1L, paintClient.right - paintClient.left);\n"
        "        const int paintHeight = std::max(1L, paintClient.bottom - paintClient.top);\n"
        "        HDC mem = CreateCompatibleDC(dc);\n"
        "        HBITMAP bitmap = CreateCompatibleBitmap(dc, paintWidth, paintHeight);",
        "dynamic paint bitmap",
    )
    text = replace_once(
        text,
        "        RECT all{0, 0, kToolbarWidth, kToolbarHeight};",
        "        RECT all{0, 0, paintWidth, paintHeight};",
        "dynamic paint bounds",
    )
    text = replace_once(
        text,
        "        g.DrawLine(&divider, 10.0f, static_cast<float>(kPrimaryHeight),\n"
        "                   static_cast<float>(kToolbarWidth - 10), static_cast<float>(kPrimaryHeight));\n"
        "        g.DrawLine(&divider, 10.0f, static_cast<float>(kPrimaryHeight + kSecondaryHeight),\n"
        "                   static_cast<float>(kToolbarWidth - 10), static_cast<float>(kPrimaryHeight + kSecondaryHeight));",
        "        g.DrawLine(&divider, 10.0f, static_cast<float>(kPrimaryHeight),\n"
        "                   static_cast<float>(paintWidth - 10), static_cast<float>(kPrimaryHeight));\n"
        "        g.DrawLine(&divider, 10.0f, static_cast<float>(kPrimaryHeight + kSecondaryHeight),\n"
        "                   static_cast<float>(paintWidth - 10), static_cast<float>(kPrimaryHeight + kSecondaryHeight));",
        "dynamic divider width",
    )
    text = replace_once(
        text,
        "        RECT hint{kPad, kPrimaryHeight + kSecondaryHeight, kToolbarWidth-kPad, kToolbarHeight};",
        "        RECT hint{kPad, kPrimaryHeight + kSecondaryHeight, paintWidth-kPad, paintHeight};",
        "dynamic hint bounds",
    )
    text = replace_once(
        text,
        "        BitBlt(dc, 0, 0, kToolbarWidth, kToolbarHeight, mem, 0, 0, SRCCOPY);",
        "        BitBlt(dc, 0, 0, paintWidth, paintHeight, mem, 0, 0, SRCCOPY);",
        "dynamic toolbar blit",
    )

    text = replace_once(
        text,
        "        RECT client{};\n"
        "        GetClientRect(hwnd, &client);\n"
        "        HPEN border = CreatePen(PS_SOLID, 1, RGB(229, 223, 216));\n"
        "        const HGDIOBJ oldPen = SelectObject(dc, border);\n"
        "        const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));\n"
        "        RoundRect(dc, 0, 0, client.right, client.bottom, 18, 18);",
        "        RECT client{};\n"
        "        GetClientRect(hwnd, &client);\n"
        "        HPEN border = CreatePen(PS_SOLID, 1, RGB(238, 211, 216));\n"
        "        const HGDIOBJ oldPen = SelectObject(dc, border);\n"
        "        const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));\n"
        "        if (client.bottom <= kPrimaryHeight) {\n"
        "            RoundRect(dc, 0, 0, client.right, client.bottom, 18, 18);\n"
        "        } else {\n"
        "            const int primaryWidth = std::min<int>(client.right, PrimaryContentWidth());\n"
        "            RoundRect(dc, 0, 0, primaryWidth, kPrimaryHeight, 18, 18);\n"
        "            RoundRect(dc, 0, kPrimaryHeight - 1, client.right, client.bottom, 16, 16);\n"
        "        }",
        "content-shaped outer border",
    )

    text = replace_once(
        text,
        "        HBRUSH background = CreateSolidBrush(RGB(252, 250, 247));\n"
        "        FillRect(mem, &all, background);\n"
        "        DeleteObject(background);",
        "        HBRUSH background = CreateSolidBrush(RGB(255, 249, 248));\n"
        "        FillRect(mem, &all, background);\n"
        "        DeleteObject(background);\n"
        "        toolbaricons_gdi::DrawAnimeBackdrop(mem, all);",
        "anime toolbar backdrop",
    )
    text = replace_once(
        text,
        "        Gdiplus::Pen divider(Gdiplus::Color(255, 232, 226, 219), 1.0f);",
        "        Gdiplus::Pen divider(Gdiplus::Color(255, 241, 220, 224), 1.0f);",
        "pastel divider",
    )

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding="utf-8", newline="\n")
    print(f"Generated compact anime toolbar source: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
