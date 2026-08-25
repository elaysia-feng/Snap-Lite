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
    text = replace_once(
        text,
        "        HPEN border = CreatePen(PS_SOLID, 1, RGB(229, 223, 216));",
        "        HPEN border = CreatePen(PS_SOLID, 1, RGB(238, 211, 216));",
        "pastel outer border",
    )

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding="utf-8", newline="\n")
    print(f"Generated compact anime toolbar source: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
