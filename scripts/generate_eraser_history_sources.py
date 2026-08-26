from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path


PATCHED_FILES = (
    "snip_window.h",
    "snip_window.cpp",
    "snip_window_original.inc",
    "editor_toolbar.cpp",
    "toolbar_icon_render_gdi.h",
    "toolbar_icon_render_gdi_anime.h",
)

# These headers are not modified by the eraser patch, but they include
# snip_window.h by a quoted relative include. Copying them beside the generated
# header prevents one translation unit from loading both generated and original
# SnipWindow definitions.
COMPANION_HEADERS = (
    "edit_repaint_fix.h",
    "window_bounds_fix.h",
)


def main() -> int:
    if len(sys.argv) != 3:
        print(
            "usage: generate_eraser_history_sources.py <source-root> <output-root>",
            file=sys.stderr,
        )
        return 2

    source_root = Path(sys.argv[1]).resolve()
    output_root = Path(sys.argv[2]).resolve()
    generated_src = output_root / "src"
    generated_src.mkdir(parents=True, exist_ok=True)
    (output_root / ".release").mkdir(parents=True, exist_ok=True)

    for name in PATCHED_FILES + COMPANION_HEADERS:
        shutil.copy2(source_root / "src" / name, generated_src / name)

    # The patch helper also bumps CMake/release metadata when run against the
    # real repository. For generated build sources we give it a disposable
    # compatibility file so only the generated sources matter.
    (output_root / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(SnapLite VERSION 1.3.10 LANGUAGES CXX RC)\n",
        encoding="utf-8",
        newline="\n",
    )

    subprocess.run(
        [sys.executable, str(source_root / "scripts" / "apply_eraser_history_patch.py")],
        cwd=output_root,
        check=True,
    )

    required = [generated_src / name for name in PATCHED_FILES + COMPANION_HEADERS]
    missing = [str(path) for path in required if not path.exists()]
    if missing:
        raise RuntimeError(f"generated eraser sources missing: {missing}")

    print(f"Generated eraser/history sources: {generated_src}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
