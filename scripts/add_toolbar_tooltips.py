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
        print("usage: add_toolbar_tooltips.py <generated-editor-toolbar.cpp>", file=sys.stderr)
        return 2

    path = Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    text = replace_once(
        text,
        "    ~ToolbarHost() {\n"
        "        if (font_) DeleteObject(font_);\n"
        "        if (smallFont_) DeleteObject(smallFont_);\n"
        "    }",
        "    ~ToolbarHost() {\n"
        "        if (tooltip_ && IsWindow(tooltip_)) DestroyWindow(tooltip_);\n"
        "        if (font_) DeleteObject(font_);\n"
        "        if (smallFont_) DeleteObject(smallFont_);\n"
        "    }",
        "tooltip cleanup",
    )

    text = replace_once(
        text,
        "    void AttachToolbar(HWND child) {\n"
        "        toolbar_ = child;\n"
        "        ApplyRoundedRegion();\n"
        "        UpdatePosition();\n"
        "    }",
        "    void AttachToolbar(HWND child) {\n"
        "        toolbar_ = child;\n"
        "        EnsureTooltips();\n"
        "        ApplyRoundedRegion();\n"
        "        UpdatePosition();\n"
        "    }\n\n"
        "    void EnsureTooltips() {\n"
        "        if (tooltip_ || !toolbar_) return;\n"
        "        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};\n"
        "        InitCommonControlsEx(&controls);\n"
        "        tooltip_ = CreateWindowExW(\n"
        "            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,\n"
        "            TOOLTIPS_CLASSW,\n"
        "            nullptr,\n"
        "            WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,\n"
        "            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,\n"
        "            toolbar_, nullptr, instance_, nullptr);\n"
        "        if (!tooltip_) return;\n\n"
        "        SetWindowPos(tooltip_, HWND_TOPMOST, 0, 0, 0, 0,\n"
        "                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);\n"
        "        SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_INITIAL, 300);\n"
        "        SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_RESHOW, 80);\n"
        "        SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 180);\n\n"
        "        auto add = [this](UINT_PTR id, const RECT& rect, const wchar_t* label) {\n"
        "            TOOLINFOW info{};\n"
        "            info.cbSize = sizeof(info);\n"
        "            info.uFlags = TTF_SUBCLASS;\n"
        "            info.hwnd = toolbar_;\n"
        "            info.uId = id;\n"
        "            info.rect = rect;\n"
        "            info.lpszText = const_cast<LPWSTR>(label);\n"
        "            SendMessageW(tooltip_, TTM_ADDTOOLW, 0,\n"
        "                         reinterpret_cast<LPARAM>(&info));\n"
        "        };\n\n"
        "        for (int i = 0; i < kToolCount; ++i) {\n"
        "            add(static_cast<UINT_PTR>(i + 1), ToolRect(i), kToolLabels[i]);\n"
        "        }\n"
        "        for (int i = 0; i < static_cast<int>(kActionLabels.size()); ++i) {\n"
        "            add(static_cast<UINT_PTR>(100 + i), ActionRect(i), kActionLabels[i]);\n"
        "        }\n"
        "    }",
        "primary hover tooltips",
    )

    text = replace_once(
        text,
        "    HFONT font_{};\n"
        "    HFONT smallFont_{};",
        "    HWND tooltip_{};\n"
        "    HFONT font_{};\n"
        "    HFONT smallFont_{};",
        "tooltip member",
    )

    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"Added toolbar hover tooltips: {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
