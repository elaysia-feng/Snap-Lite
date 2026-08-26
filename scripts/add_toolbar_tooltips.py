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
        "        HideHoverPopup();\n"
        "        if (tooltip_ && IsWindow(tooltip_)) DestroyWindow(tooltip_);\n"
        "        if (font_) DeleteObject(font_);\n"
        "        if (smallFont_) DeleteObject(smallFont_);\n"
        "    }",
        "hover popup cleanup",
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
        "        EnsureHoverPopup();\n"
        "        ApplyRoundedRegion();\n"
        "        UpdatePosition();\n"
        "    }\n\n"
        "    void EnsureHoverPopup() {\n"
        "        if (tooltip_ || !toolbar_) return;\n"
        "        tooltip_ = CreateWindowExW(\n"
        "            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,\n"
        "            L\"STATIC\",\n"
        "            L\"\",\n"
        "            WS_POPUP | WS_BORDER | SS_CENTER | SS_CENTERIMAGE,\n"
        "            0, 0, 0, 0,\n"
        "            parent_, nullptr, instance_, nullptr);\n"
        "        if (!tooltip_) return;\n"
        "        SendMessageW(tooltip_, WM_SETFONT,\n"
        "                     reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);\n"
        "        SetWindowPos(tooltip_, HWND_TOPMOST, 0, 0, 0, 0,\n"
        "                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_HIDEWINDOW);\n"
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
        "    void HideHoverPopup() {\n"
        "        if (tooltip_ && IsWindowVisible(tooltip_)) {\n"
        "            ShowWindow(tooltip_, SW_HIDE);\n"
        "        }\n"
        "    }\n\n"
        "    void UpdateHoverPopup(POINT clientPoint) {\n"
        "        const wchar_t* label = HoverTooltipLabel();\n"
        "        if (!tooltip_ || !label || !*label) {\n"
        "            HideHoverPopup();\n"
        "            return;\n"
        "        }\n\n"
        "        tooltipText_ = label;\n"
        "        SetWindowTextW(tooltip_, tooltipText_.c_str());\n\n"
        "        SIZE textSize{40, 18};\n"
        "        HDC dc = GetDC(tooltip_);\n"
        "        if (dc) {\n"
        "            HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));\n"
        "            HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;\n"
        "            SIZE measured{};\n"
        "            if (GetTextExtentPoint32W(dc, tooltipText_.c_str(),\n"
        "                                      static_cast<int>(tooltipText_.size()), &measured)) {\n"
        "                textSize = measured;\n"
        "            }\n"
        "            if (oldFont) SelectObject(dc, oldFont);\n"
        "            ReleaseDC(tooltip_, dc);\n"
        "        }\n\n"
        "        const int width = std::max(44, static_cast<int>(textSize.cx) + 18);\n"
        "        const int height = std::max(26, static_cast<int>(textSize.cy) + 10);\n"
        "        POINT screen = clientPoint;\n"
        "        ClientToScreen(toolbar_, &screen);\n"
        "        screen.x += 12;\n"
        "        screen.y += 20;\n\n"
        "        HMONITOR monitor = MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST);\n"
        "        MONITORINFO monitorInfo{sizeof(monitorInfo)};\n"
        "        if (monitor && GetMonitorInfoW(monitor, &monitorInfo)) {\n"
        "            if (screen.x + width > monitorInfo.rcWork.right - 4)\n"
        "                screen.x = monitorInfo.rcWork.right - width - 4;\n"
        "            if (screen.y + height > monitorInfo.rcWork.bottom - 4)\n"
        "                screen.y -= height + 28;\n"
        "            screen.x = std::max<LONG>(screen.x, monitorInfo.rcWork.left + 4);\n"
        "            screen.y = std::max<LONG>(screen.y, monitorInfo.rcWork.top + 4);\n"
        "        }\n\n"
        "        SetWindowPos(tooltip_, HWND_TOPMOST, screen.x, screen.y, width, height,\n"
        "                     SWP_NOACTIVATE | SWP_SHOWWINDOW);\n"
        "        RedrawWindow(tooltip_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);\n"
        "    }",
        "explicit hover popup",
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
        "        UpdateHoverPopup(p);\n"
        "    }",
        "hover popup updates",
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
        "            HideHoverPopup();\n"
        "            if (hoverTool_ != -1 || hoverAction_ != -1 || hoverSecondary_ != -1) {\n"
        "                hoverTool_ = hoverAction_ = hoverSecondary_ = -1;\n"
        "                InvalidateRect(hwnd, nullptr, FALSE);\n"
        "            }\n"
        "            return 0;",
        "hide popup on mouse leave",
    )

    text = replace_once(
        text,
        "    HFONT font_{};\n"
        "    HFONT smallFont_{};",
        "    HWND tooltip_{};\n"
        "    std::wstring tooltipText_;\n"
        "    HFONT font_{};\n"
        "    HFONT smallFont_{};",
        "hover popup members",
    )

    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"Added explicit toolbar hover popup: {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
