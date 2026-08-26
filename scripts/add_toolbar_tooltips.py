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
        "        HideTrackedTooltip();\n"
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
        "            parent_, nullptr, instance_, nullptr);\n"
        "        if (!tooltip_) return;\n\n"
        "        SetWindowPos(tooltip_, HWND_TOPMOST, 0, 0, 0, 0,\n"
        "                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);\n"
        "        SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 160);\n\n"
        "        TOOLINFOW info{};\n"
        "        info.cbSize = sizeof(info);\n"
        "        info.uFlags = TTF_TRACK | TTF_ABSOLUTE;\n"
        "        info.hwnd = toolbar_;\n"
        "        info.uId = 1;\n"
        "        info.lpszText = const_cast<LPWSTR>(L\"\");\n"
        "        SendMessageW(tooltip_, TTM_ADDTOOLW, 0,\n"
        "                     reinterpret_cast<LPARAM>(&info));\n"
        "    }\n\n"
        "    const wchar_t* HoverTooltipLabel() const {\n"
        "        if (hoverTool_ >= 0 && hoverTool_ < kToolCount) {\n"
        "            return kToolLabels[static_cast<size_t>(hoverTool_)];\n"
        "        }\n"
        "        if (hoverAction_ >= 0 && hoverAction_ < static_cast<int>(kActionLabels.size())) {\n"
        "            return kActionLabels[static_cast<size_t>(hoverAction_)];\n"
        "        }\n"
        "        return nullptr;\n"
        "    }\n\n"
        "    void HideTrackedTooltip() {\n"
        "        if (!tooltip_ || !tooltipVisible_) return;\n"
        "        TOOLINFOW info{};\n"
        "        info.cbSize = sizeof(info);\n"
        "        info.hwnd = toolbar_;\n"
        "        info.uId = 1;\n"
        "        SendMessageW(tooltip_, TTM_TRACKACTIVATE, FALSE,\n"
        "                     reinterpret_cast<LPARAM>(&info));\n"
        "        tooltipVisible_ = false;\n"
        "    }\n\n"
        "    void UpdateTrackedTooltip(POINT clientPoint) {\n"
        "        const wchar_t* label = HoverTooltipLabel();\n"
        "        if (!tooltip_ || !label) {\n"
        "            HideTrackedTooltip();\n"
        "            return;\n"
        "        }\n\n"
        "        tooltipText_ = label;\n"
        "        TOOLINFOW info{};\n"
        "        info.cbSize = sizeof(info);\n"
        "        info.uFlags = TTF_TRACK | TTF_ABSOLUTE;\n"
        "        info.hwnd = toolbar_;\n"
        "        info.uId = 1;\n"
        "        info.lpszText = tooltipText_.data();\n"
        "        SendMessageW(tooltip_, TTM_UPDATETIPTEXTW, 0,\n"
        "                     reinterpret_cast<LPARAM>(&info));\n\n"
        "        POINT screen = clientPoint;\n"
        "        ClientToScreen(toolbar_, &screen);\n"
        "        screen.x += 12;\n"
        "        screen.y += 22;\n"
        "        SendMessageW(tooltip_, TTM_TRACKPOSITION, 0,\n"
        "                     MAKELPARAM(screen.x, screen.y));\n"
        "        SendMessageW(tooltip_, TTM_TRACKACTIVATE, TRUE,\n"
        "                     reinterpret_cast<LPARAM>(&info));\n"
        "        tooltipVisible_ = true;\n"
        "    }",
        "actively tracked primary hover tooltips",
    )

    text = replace_once(
        text,
        "    void UpdateHover(POINT p) {\n"
        "        const int tool = HitTool(p);\n"
        "        const int action = tool < 0 ? HitAction(p) : -1;\n"
        "        const int secondary = (tool < 0 && action < 0) ? HitSecondary(p) : -1;\n"
        "        if (tool != hoverTool_ || action != hoverAction_ || secondary != hoverSecondary_) {\n"
        "            hoverTool_ = tool;\n"
        "            hoverAction_ = action;\n"
        "            hoverSecondary_ = secondary;\n"
        "            InvalidateRect(toolbar_, nullptr, FALSE);\n"
        "        }\n"
        "    }",
        "    void UpdateHover(POINT p) {\n"
        "        const int tool = HitTool(p);\n"
        "        const int action = tool < 0 ? HitAction(p) : -1;\n"
        "        const int secondary = (tool < 0 && action < 0) ? HitSecondary(p) : -1;\n"
        "        if (tool != hoverTool_ || action != hoverAction_ || secondary != hoverSecondary_) {\n"
        "            hoverTool_ = tool;\n"
        "            hoverAction_ = action;\n"
        "            hoverSecondary_ = secondary;\n"
        "            InvalidateRect(toolbar_, nullptr, FALSE);\n"
        "        }\n"
        "        UpdateTrackedTooltip(p);\n"
        "    }",
        "active tooltip hover updates",
    )

    text = replace_once(
        text,
        "        case WM_MOUSELEAVE:\n"
        "            if (hoverTool_ != -1 || hoverAction_ != -1 || hoverSecondary_ != -1) {\n"
        "                hoverTool_ = hoverAction_ = hoverSecondary_ = -1;\n"
        "                InvalidateRect(hwnd, nullptr, FALSE);\n"
        "            }\n"
        "            return 0;",
        "        case WM_MOUSELEAVE:\n"
        "            HideTrackedTooltip();\n"
        "            if (hoverTool_ != -1 || hoverAction_ != -1 || hoverSecondary_ != -1) {\n"
        "                hoverTool_ = hoverAction_ = hoverSecondary_ = -1;\n"
        "                InvalidateRect(hwnd, nullptr, FALSE);\n"
        "            }\n"
        "            return 0;",
        "hide tracked tooltip on leave",
    )

    text = replace_once(
        text,
        "    HFONT font_{};\n"
        "    HFONT smallFont_{};",
        "    HWND tooltip_{};\n"
        "    bool tooltipVisible_{false};\n"
        "    std::wstring tooltipText_;\n"
        "    HFONT font_{};\n"
        "    HFONT smallFont_{};",
        "tooltip members",
    )

    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"Added actively tracked toolbar hover tooltips: {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
