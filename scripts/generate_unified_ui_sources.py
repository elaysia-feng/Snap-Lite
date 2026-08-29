from __future__ import annotations

import re
import sys
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, got {count}")
    return text.replace(old, new, 1)


def replace_or_keep(text: str, old: str, new: str, label: str) -> str:
    if new in text:
        return text
    return replace_once(text, old, new, label)


def patch_pin(source: Path, target: Path) -> None:
    text = source.read_text(encoding="utf-8")

    text = replace_once(
        text,
        '#include "capture.h"\n',
        '#include "capture.h"\n#include "ui_theme.h"\n',
        "pin theme include",
    )
    text = replace_once(
        text,
        'constexpr int kMenuWidth = 244;\nconstexpr int kMenuHeight = 196;',
        'constexpr int kMenuWidth = 270;\nconstexpr int kMenuHeight = 252;',
        "pin menu dimensions",
    )

    text = re.sub(
        r'RECT SliderRect\(\) \{.*?\n\}\n\nRECT MenuRowRect\(int row\) \{.*?\n\}',
        '''RECT SliderRect() {
    return {28, 91, kMenuWidth - 28, 109};
}

RECT MenuRowRect(int row) {
    switch (row) {
    case 0: return {14, 130, kMenuWidth - 14, 162};
    case 1: return {14, 166, kMenuWidth - 14, 198};
    case 2: return {14, 206, kMenuWidth - 14, 238};
    default: return {};
    }
}''',
        text,
        count=1,
        flags=re.S,
    )

    paint_start = text.find('void PaintPinMenu(HWND hwnd, PinMenuState* state) {')
    paint_end = text.find('LRESULT CALLBACK PinMenuWindowProc', paint_start)
    if paint_start < 0 or paint_end < 0:
        raise RuntimeError("pin menu paint function anchors not found")

    paint = r'''void PaintPinMenu(HWND hwnd, PinMenuState* state) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    if (!state) {
        EndPaint(hwnd, &ps);
        return;
    }

    RECT client{};
    GetClientRect(hwnd, &client);
    ui::FillSolid(dc, client, ui::kSurface);

    // Soft outer outline keeps the popup visually aligned with the OCR result
    // panel and the cream/pink screenshot toolbar.
    HBRUSH hollow = static_cast<HBRUSH>(GetStockObject(HOLLOW_BRUSH));
    HPEN outline = CreatePen(PS_SOLID, 1, ui::kBorder);
    const HGDIOBJ oldBrush = SelectObject(dc, hollow);
    const HGDIOBJ oldOutline = SelectObject(dc, outline);
    RoundRect(dc, 0, 0, client.right, client.bottom, 18, 18);
    SelectObject(dc, oldOutline);
    SelectObject(dc, oldBrush);
    DeleteObject(outline);

    const UINT dpi = GetDpiForWindow(hwnd);
    HFONT titleFont = ui::MakeFont(dpi, 12, FW_SEMIBOLD);
    HFONT bodyFont = ui::MakeFont(dpi, 10, FW_NORMAL);
    HFONT smallFont = ui::MakeFont(dpi, 9, FW_NORMAL);

    SetBkMode(dc, TRANSPARENT);

    RECT accent{18, 18, 28, 28};
    ui::FillRoundRect(dc, accent, 4, ui::kAccent, ui::kAccent);

    HGDIOBJ oldFont = SelectObject(dc, titleFont);
    SetTextColor(dc, ui::kText);
    RECT title{36, 9, kMenuWidth - 18, 39};
    DrawTextW(dc, L"贴图设置", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, smallFont);
    SetTextColor(dc, ui::kMutedText);
    RECT subtitle{18, 34, kMenuWidth - 18, 56};
    DrawTextW(dc, L"滚轮缩放 · Ctrl+滚轮调透明度", -1, &subtitle,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT opacityCard{14, 58, kMenuWidth - 14, 119};
    ui::FillRoundRect(dc, opacityCard, 12, ui::kCard, ui::kBorder);

    SelectObject(dc, bodyFont);
    SetTextColor(dc, ui::kText);
    RECT opacityLabel{26, 62, 126, 87};
    DrawTextW(dc, L"透明度", -1, &opacityLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const int percent = static_cast<int>((static_cast<int>(state->opacity) * 100 + 127) / 255);
    wchar_t percentText[32]{};
    swprintf_s(percentText, L"%d%%", percent);
    SetTextColor(dc, ui::kMutedText);
    RECT percentRect{kMenuWidth - 82, 62, kMenuWidth - 26, 87};
    DrawTextW(dc, percentText, -1, &percentRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    const RECT slider = SliderRect();
    const int centerY = static_cast<int>((slider.top + slider.bottom) / 2);
    const int knobX = SliderXFromOpacity(state->opacity);

    HPEN trackPen = CreatePen(PS_SOLID, 4, ui::kSliderTrack);
    HPEN activePen = CreatePen(PS_SOLID, 4, ui::kAccent);
    HGDIOBJ oldPen = SelectObject(dc, trackPen);
    MoveToEx(dc, slider.left, centerY, nullptr);
    LineTo(dc, slider.right, centerY);
    SelectObject(dc, activePen);
    MoveToEx(dc, slider.left, centerY, nullptr);
    LineTo(dc, knobX, centerY);
    SelectObject(dc, oldPen);
    DeleteObject(trackPen);
    DeleteObject(activePen);

    RECT knob{knobX - 7, centerY - 7, knobX + 7, centerY + 7};
    ui::FillRoundRect(dc, knob, 14, ui::kCard, ui::kAccentStrong);
    RECT knobInner{knobX - 3, centerY - 3, knobX + 3, centerY + 3};
    ui::FillRoundRect(dc, knobInner, 6, ui::kAccent, ui::kAccent);

    const wchar_t* labels[3] = {L"复制贴图", L"另存为...", L"关闭贴图"};
    for (int row = 0; row < 3; ++row) {
        RECT rect = MenuRowRect(row);
        const bool danger = row == 2;
        if (state->hoverRow == row) {
            const COLORREF hover = danger ? ui::kDangerHover : ui::kHover;
            ui::FillRoundRect(dc, rect, 10, hover, hover);
        }

        const int cy = static_cast<int>((rect.top + rect.bottom) / 2);
        RECT dot{26, cy - 4, 34, cy + 4};
        const COLORREF dotColor = danger ? ui::kDanger : ui::kAccent;
        ui::FillRoundRect(dc, dot, 8, dotColor, dotColor);

        SetTextColor(dc, danger ? ui::kDanger : ui::kText);
        RECT textRect = rect;
        textRect.left = 44;
        textRect.right -= 10;
        DrawTextW(dc, labels[row], -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(dc, oldFont);
    DeleteObject(titleFont);
    DeleteObject(bodyFont);
    DeleteObject(smallFont);
    EndPaint(hwnd, &ps);
}

'''
    text = text[:paint_start] + paint + text[paint_end:]

    text = replace_once(
        text,
        'HRGN region = CreateRoundRectRgn(0, 0, kMenuWidth + 1, kMenuHeight + 1, 18, 18);',
        'HRGN region = CreateRoundRectRgn(0, 0, kMenuWidth + 1, kMenuHeight + 1, 20, 20);',
        "pin menu round region",
    )
    text = replace_once(
        text,
        '    menuClass.hInstance = instance;\n    menuClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);',
        '    menuClass.hInstance = instance;\n    menuClass.style = CS_DROPSHADOW;\n    menuClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);',
        "pin menu shadow",
    )

    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding="utf-8", newline="\n")


def patch_ocr_panel(source: Path, target: Path) -> None:
    text = source.read_text(encoding="utf-8")
    text = replace_once(
        text,
        '#include "ocr.h"\n',
        '#include "ocr.h"\n#include "ui_theme.h"\n',
        "OCR theme include",
    )

    old_colors = '''constexpr COLORREF kPanelBg = RGB(255, 248, 250);
constexpr COLORREF kCardBg = RGB(255, 255, 255);
constexpr COLORREF kBorder = RGB(244, 216, 224);
constexpr COLORREF kAccent = RGB(239, 139, 166);
constexpr COLORREF kAccentPressed = RGB(224, 119, 151);
constexpr COLORREF kText = RGB(57, 49, 55);
constexpr COLORREF kMutedText = RGB(137, 119, 128);
constexpr COLORREF kSecondaryBg = RGB(255, 252, 253);
constexpr COLORREF kSecondaryPressed = RGB(250, 239, 243);'''
    new_colors = '''constexpr COLORREF kPanelBg = ui::kSurface;
constexpr COLORREF kCardBg = ui::kCard;
constexpr COLORREF kBorder = ui::kBorder;
constexpr COLORREF kAccent = ui::kAccent;
constexpr COLORREF kAccentPressed = ui::kAccentStrong;
constexpr COLORREF kText = ui::kText;
constexpr COLORREF kMutedText = ui::kMutedText;
constexpr COLORREF kSecondaryBg = ui::kCream;
constexpr COLORREF kSecondaryPressed = ui::kPressed;'''
    text = replace_once(text, old_colors, new_colors, "OCR shared palette")
    text = replace_or_keep(
        text,
        "    const RECT dotRect{18, 18, 28, 28};\n"
        "    HBRUSH accentBrush = CreateSolidBrush(kAccent);\n"
        "    FillRect(dc, &dotRect, accentBrush);\n"
        "    DeleteObject(accentBrush);",
        "    const RECT dotRect{18, 18, 28, 28};\n"
        "    ui::FillRoundRect(dc, dotRect, 4, kAccent, kAccent);",
        "OCR rounded accent marker",
    )
    text = replace_or_keep(
        text,
        "    HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);",
        "    // 截图窗口覆盖整个虚拟桌面，窗口原点不能可靠代表当前选区所在的屏幕。\n"
        "    HMONITOR monitor = MonitorFromPoint(rightTop, MONITOR_DEFAULTTONEAREST);",
        "OCR selection monitor",
    )

    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding="utf-8", newline="\n")


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: generate_unified_ui_sources.py <source-root> <output-root>", file=sys.stderr)
        return 2

    root = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2]).resolve()
    patch_pin(root / "src" / "pin_window.cpp", output / "pin_window.cpp")
    patch_ocr_panel(root / "src" / "ocr_result_panel_v2.cpp", output / "ocr_result_panel_v2.cpp")
    print(f"Generated unified UI sources: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
