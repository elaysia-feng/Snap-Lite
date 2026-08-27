from __future__ import annotations

import sys
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, got {count}")
    return text.replace(old, new, 1)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: apply_unified_toolbar_theme.py <generated-anime-header.h>", file=sys.stderr)
        return 2

    path = Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    text = replace_once(
        text,
        '#include "toolbar_chibi_azusa_v2.h"\n',
        '#include "toolbar_chibi_azusa_v2.h"\n#include "ui_theme.h"\n',
        "toolbar shared theme include",
    )
    text = replace_once(
        text,
        '    HBRUSH brush = CreateSolidBrush(RGB(255, 250, 248));',
        '    HBRUSH brush = CreateSolidBrush(snaplite::ui::kCream);',
        "toolbar cream surface",
    )
    text = replace_once(
        text,
        '        Exact(text, count, L"取消") ? RGB(201, 75, 83) : RGB(74, 63, 64));',
        '        Exact(text, count, L"取消") ? snaplite::ui::kDanger : snaplite::ui::kText);',
        "toolbar text palette",
    )

    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"Applied shared Snap-Lite UI palette: {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
