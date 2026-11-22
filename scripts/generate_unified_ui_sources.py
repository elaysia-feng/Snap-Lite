from __future__ import annotations

import re
import sys
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    """替换唯一匹配的源码锚点。

    Args:
        text: 原始源码文本。
        old: 必须恰好出现一次的旧片段。
        new: 用于替换的新片段。
        label: 锚点名称，用于构建失败提示。

    Returns:
        替换后的源码文本。

    Raises:
        RuntimeError: 旧片段缺失或出现多次时抛出。
    """
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, got {count}")
    return text.replace(old, new, 1)


def replace_or_keep(text: str, old: str, new: str, label: str) -> str:
    """若新片段尚不存在则替换一次，否则保留已处理文本。

    Args:
        text: 原始源码文本。
        old: 尚未处理时应替换的旧片段。
        new: 目标源码片段。
        label: 锚点名称，用于构建失败提示。

    Returns:
        替换后的源码文本，或原样返回已处理文本。
    """
    if new in text:
        return text
    return replace_once(text, old, new, label)


def patch_pin(source: Path, target: Path) -> None:
    """为贴图设置窗口生成共享主题版本。

    Args:
        source: 项目中的贴图窗口源文件。
        target: 构建目录中的生成源文件目标路径。

    Raises:
        RuntimeError: 预期源码锚点缺失或重复时抛出。
    """
    text = source.read_text(encoding="utf-8")

    # 1. 引入共享主题并调整贴图菜单尺寸。
    text = replace_or_keep(
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

    # 2. 定位并替换贴图菜单布局函数。
    # 布局块用正则定位，但必须命中且只命中一次：
    # 上游一旦重命名或重排 SliderRect/MenuRowRect，静默匹配 0 次会让贴图菜单
    # 回退旧布局且构建全绿，这里改成响亮失败。
    #
    # 注意：配对正则要求 SliderRect 后面紧跟 MenuRowRect，所以"只复制一份
    # SliderRect"这类改动仍会被配对数=1 掩盖。因此先单独断言两个函数的
    # 出现次数各为 1。
    for symbol in ("RECT SliderRect()", "RECT MenuRowRect(int row)"):
        occurrences = text.count(symbol)
        if occurrences != 1:
            raise RuntimeError(
                f"pin menu layout symbol {symbol!r}: expected exactly one "
                f"occurrence, got {occurrences}"
            )

    layout_pattern = re.compile(
        r'RECT SliderRect\(\) \{.*?\n\}\n\nRECT MenuRowRect\(int row\) \{.*?\n\}',
        re.S,
    )
    layout_matches = layout_pattern.findall(text)
    if len(layout_matches) != 1:
        raise RuntimeError(
            "pin menu layout block: expected exactly one match, "
            f"got {len(layout_matches)}"
        )
    text = layout_pattern.sub(
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
    )

    # 3. 重写绘制函数，使背景、文字、边框和操作状态使用当前主题。
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
    RoundRect(dc, 0, 0, client.right, client.bottom, 20, 20);
    SelectObject(dc, oldOutline);
    SelectObject(dc, oldBrush);
    DeleteObject(outline);

    const UINT dpi = GetDpiForWindow(hwnd);
    HFONT titleFont = ui::MakeFont(dpi, 12, FW_SEMIBOLD);
    HFONT bodyFont = ui::MakeFont(dpi, 10, FW_NORMAL);
    HFONT smallFont = ui::MakeFont(dpi, 9, FW_NORMAL);

    SetBkMode(dc, TRANSPARENT);

    RECT accent{16, 17, 26, 27};
    ui::FillRoundRect(dc, accent, 4, ui::kAccent, ui::kAccent);

    HGDIOBJ oldFont = SelectObject(dc, titleFont);
    SetTextColor(dc, ui::kText);
    RECT title{34, 9, kMenuWidth - 88, 37};
    DrawTextW(dc, L"贴图设置", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT pinBadge{kMenuWidth - 82, 13, kMenuWidth - 16, 33};
    ui::DrawAccentBadge(dc, pinBadge, 10, ui::kAccentStrong);
    SelectObject(dc, smallFont);
    SetTextColor(dc, ui::kAccentAction);
    DrawTextW(dc, L"置顶贴图", -1, &pinBadge, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SetTextColor(dc, ui::kMutedText);
    RECT subtitle{16, 34, kMenuWidth - 16, 54};
    DrawTextW(dc, L"滚轮缩放 · Ctrl+滚轮调透明度", -1, &subtitle,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT opacityCard{14, 58, kMenuWidth - 14, 119};
    ui::DrawElevatedCard(dc, opacityCard, 12);

    SelectObject(dc, bodyFont);
    SetTextColor(dc, ui::kText);
    RECT opacityLabel{26, 62, 126, 87};
    DrawTextW(dc, L"透明度", -1, &opacityLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const int percent = static_cast<int>((static_cast<int>(state->opacity) * 100 + 127) / 255);
    wchar_t percentText[32]{};
    swprintf_s(percentText, L"%d%%", percent);
    RECT percentPill{kMenuWidth - 76, 64, kMenuWidth - 24, 85};
    ui::FillRoundRect(dc, percentPill, 10, ui::kCream, ui::kBorder);
    SelectObject(dc, smallFont);
    SetTextColor(dc, ui::kAccentAction);
    DrawTextW(dc, percentText, -1, &percentPill, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const RECT slider = SliderRect();
    const int centerY = static_cast<int>((slider.top + slider.bottom) / 2);
    const int knobX = SliderXFromOpacity(state->opacity);

    {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const auto toGdiColor = [](COLORREF c, BYTE alpha = 255) {
            return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
        };
        const auto fillPill = [&](float x, float y, float w, float h, float r, COLORREF c) {
            const float d = r * 2.0f;
            const float safeW = std::max(d, w);
            const float safeH = std::max(d, h);
            Gdiplus::GraphicsPath path;
            path.AddArc(x, y, d, d, 180.0f, 90.0f);
            path.AddArc(x + safeW - d, y, d, d, 270.0f, 90.0f);
            path.AddArc(x + safeW - d, y + safeH - d, d, d, 0.0f, 90.0f);
            path.AddArc(x, y + safeH - d, d, d, 90.0f, 90.0f);
            path.CloseFigure();
            Gdiplus::SolidBrush brush(toGdiColor(c));
            graphics.FillPath(&brush, &path);
        };

        const float trackTop = static_cast<float>(centerY) - 2.5f;
        fillPill(
            static_cast<float>(slider.left),
            trackTop,
            static_cast<float>(slider.right - slider.left),
            5.0f,
            2.5f,
            ui::kSliderTrack);
        const float activeW = static_cast<float>(knobX - slider.left);
        if (activeW > 1.0f) {
            fillPill(
                static_cast<float>(slider.left),
                trackTop,
                activeW,
                5.0f,
                2.5f,
                ui::kAccent);
        }

        const float kx = static_cast<float>(knobX);
        const float ky = static_cast<float>(centerY);
        Gdiplus::SolidBrush haloBrush(toGdiColor(ui::kAccent, 52));
        graphics.FillEllipse(&haloBrush, kx - 9.0f, ky - 9.0f, 18.0f, 18.0f);
        Gdiplus::SolidBrush knobFill(toGdiColor(ui::kCard));
        graphics.FillEllipse(&knobFill, kx - 6.5f, ky - 6.5f, 13.0f, 13.0f);
        Gdiplus::Pen knobRing(toGdiColor(ui::kAccentStrong), 2.0f);
        graphics.DrawEllipse(&knobRing, kx - 6.5f, ky - 6.5f, 13.0f, 13.0f);
        Gdiplus::SolidBrush knobInner(toGdiColor(ui::kAccent));
        graphics.FillEllipse(&knobInner, kx - 2.8f, ky - 2.8f, 5.6f, 5.6f);
    }

    const wchar_t* labels[3] = {L"复制贴图", L"另存为...", L"关闭贴图"};
    for (int row = 0; row < 3; ++row) {
        RECT rect = MenuRowRect(row);
        const bool danger = row == 2;
        const bool pressed = state->pressedRow == row;
        const bool hovered = state->hoverRow == row;
        COLORREF fill = ui::kCard;
        COLORREF border = ui::kBorder;
        if (pressed) {
            fill = danger ? ui::Blend(ui::kDangerHover, ui::kPressed) : ui::kPressed;
            border = danger ? ui::kDanger : ui::kAccentStrong;
        } else if (hovered) {
            fill = danger ? ui::kDangerHover : ui::kHover;
            border = danger ? ui::kDanger : ui::kAccent;
        }
        ui::FillRoundRect(dc, rect, 10, fill, border);

        const int cy = static_cast<int>((rect.top + rect.bottom) / 2);
        const COLORREF iconTone = danger ? ui::kDanger : ui::kAccentAction;
        RECT iconPlate{22, cy - 11, 44, cy + 11};
        ui::FillRoundRect(
            dc,
            iconPlate,
            7,
            ui::BlendRatio(iconTone, ui::kCard, 1, 5),
            ui::BlendRatio(iconTone, ui::kBorder, 2, 3));

        {
            Gdiplus::Graphics graphics(dc);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const float cx = 33.0f;
            const float fcy = static_cast<float>(cy);
            Gdiplus::Pen iconPen(
                Gdiplus::Color(255, GetRValue(iconTone), GetGValue(iconTone), GetBValue(iconTone)),
                1.45f);
            iconPen.SetStartCap(Gdiplus::LineCapRound);
            iconPen.SetEndCap(Gdiplus::LineCapRound);
            iconPen.SetLineJoin(Gdiplus::LineJoinRound);
            if (row == 0) {
                graphics.DrawRectangle(&iconPen, cx - 2.0f, fcy - 2.0f, 6.5f, 6.5f);
                graphics.DrawLine(&iconPen, cx - 4.5f, fcy + 1.5f, cx - 4.5f, fcy - 4.5f);
                graphics.DrawLine(&iconPen, cx - 4.5f, fcy - 4.5f, cx + 1.5f, fcy - 4.5f);
            } else if (row == 1) {
                graphics.DrawLine(&iconPen, cx, fcy - 5.0f, cx, fcy + 2.0f);
                graphics.DrawLine(&iconPen, cx - 3.0f, fcy - 1.0f, cx, fcy + 2.0f);
                graphics.DrawLine(&iconPen, cx + 3.0f, fcy - 1.0f, cx, fcy + 2.0f);
                graphics.DrawLine(&iconPen, cx - 4.5f, fcy + 4.8f, cx + 4.5f, fcy + 4.8f);
            } else {
                graphics.DrawLine(&iconPen, cx - 3.5f, fcy - 3.5f, cx + 3.5f, fcy + 3.5f);
                graphics.DrawLine(&iconPen, cx + 3.5f, fcy - 3.5f, cx - 3.5f, fcy + 3.5f);
            }
        }

        SelectObject(dc, bodyFont);
        SetTextColor(dc, danger ? ui::kDanger : ui::kText);
        RECT textRect = rect;
        textRect.left = 53;
        textRect.right -= danger ? 72 : 12;
        DrawTextW(dc, labels[row], -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        if (danger) {
            SelectObject(dc, smallFont);
            SetTextColor(dc, ui::kMutedText);
            RECT hintRect = rect;
            hintRect.right -= 12;
            DrawTextW(dc, L"双击关闭", -1, &hintRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
    }

    if (state->hasFocus) {
        HPEN focusPen = CreatePen(PS_SOLID, 1, ui::kAccentStrong);
        HGDIOBJ previousPen = SelectObject(dc, focusPen);
        HGDIOBJ previousBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, 2, 2, client.right - 2, client.bottom - 2, 16, 16);
        SelectObject(dc, previousBrush);
        SelectObject(dc, previousPen);
        DeleteObject(focusPen);
    }

    SelectObject(dc, oldFont);
    DeleteObject(titleFont);
    DeleteObject(bodyFont);
    DeleteObject(smallFont);
    EndPaint(hwnd, &ps);
}

'''
    text = text[:paint_start] + paint + text[paint_end:]

    # 4. 调整圆角和窗口阴影，再输出构建文件。
    text = replace_once(
        text,
        '        HRGN region = CreateRoundRectRgn(0, 0, kMenuWidth + 1, kMenuHeight + 1, 18, 18);\n'
        '        if (region && SetWindowRgn(hwnd, region, TRUE) == 0) {\n'
        '            DeleteObject(region);\n'
        '        }',
        '        ui::SetRoundedWindowRegion(hwnd, 20);',
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
    """为 OCR 结果面板生成读取运行时主题的源码。

    Args:
        source: OCR 结果面板基础源码。
        target: 构建目录中的生成源文件目标路径。

    Raises:
        RuntimeError: 预期源码锚点缺失或重复时抛出。
    """
    text = source.read_text(encoding="utf-8")
    # 1. 引入共享主题，并将固定颜色改为指向当前主题的引用。
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
    new_colors = '''const COLORREF& kPanelBg = ui::kSurface;
const COLORREF& kCardBg = ui::kCard;
const COLORREF& kBorder = ui::kBorder;
const COLORREF& kAccent = ui::kAccent;
const COLORREF& kAccentPressed = ui::kAccentStrong;
const COLORREF& kText = ui::kText;
const COLORREF& kMutedText = ui::kMutedText;
const COLORREF& kSecondaryBg = ui::kCream;
const COLORREF& kSecondaryPressed = ui::kPressed;'''
    text = replace_once(text, old_colors, new_colors, "OCR shared palette")
    # 2. 保持角标形状统一，并保留选区对应显示器的定位方式。
    if "ui::FillRoundRect(dc, dotRect," not in text:
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

    # 3. 写入生成文件。
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding="utf-8", newline="\n")


def main() -> int:
    """生成贴图设置窗口和 OCR 结果面板的主题源码。

    Returns:
        int: 命令行退出码。
    """
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
