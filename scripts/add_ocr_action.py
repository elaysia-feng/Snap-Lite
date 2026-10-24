from __future__ import annotations

import sys
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, got {count}")
    return text.replace(old, new, 1)


def patch_editor(path: Path) -> None:
    text = path.read_text(encoding="utf-8")

    text = replace_once(
        text,
        '#include "snip_window.h"\n#include "toolbar_icon_render_gdi_anime.h"',
        '#include "snip_window.h"\n#include "ocr.h"\n#include "ocr_result_panel.h"\n#include "toolbar_icon_render_gdi_anime.h"',
        "OCR header include",
    )
    text = replace_once(
        text,
        "enum class PrimaryAction { Pin = 0, Undo, Redo, Copy, Save, SaveAs, Cancel };",
        "enum class PrimaryAction { Pin = 0, Undo, Redo, Copy, Ocr, Save, SaveAs, Cancel };",
        "OCR primary action enum",
    )
    text = replace_once(
        text,
        'constexpr std::array<const wchar_t*, 7> kActionLabels = {\n'
        '    L"图钉", L"撤销", L"重做", L"复制", L"保存", L"另存为", L"取消"\n'
        '};',
        'constexpr std::array<const wchar_t*, 8> kActionLabels = {\n'
        '    L"图钉", L"撤销", L"重做", L"复制", L"取字", L"保存", L"另存为", L"取消"\n'
        '};',
        "OCR action label",
    )
    text = replace_once(
        text,
        "constexpr std::array<int, 7> kActionWidths = {48, 48, 48, 48, 48, 52, 48};",
        "constexpr std::array<int, 8> kActionWidths = {48, 48, 48, 48, 50, 48, 52, 48};",
        "OCR portrait badge action width",
    )
    text = replace_once(
        text,
        '            case PrimaryAction::Copy: return L"复制到剪贴板";\n'
        '            case PrimaryAction::Save: return L"保存到默认截图目录";',
        '            case PrimaryAction::Copy: return L"复制到剪贴板";\n'
        '            case PrimaryAction::Ocr: return L"提取文字：识别当前选区并在右侧显示，可选择性复制";\n'
        '            case PrimaryAction::Save: return L"保存到默认截图目录";',
        "OCR hover hint",
    )
    text = replace_once(
        text,
        "class ToolbarHost;\n\nstruct TextState {",
        "class ToolbarHost;\n\n"
        "// 当前正在识别的截图窗口：同一时刻只允许一个识别任务，重复点击取字\n"
        "// 不应叠加多个识别。按窗口记录，取消一个窗口的截图不会影响另一个。\n"
        "//\n"
        "// 标识窗口要同时用 HWND 和窗口实例序号。HWND 会被系统回收复用：旧截图\n"
        "// 窗口销毁后，新截图窗口完全可能拿到同一个句柄。只比 HWND 会把上一次\n"
        "// 的识别结果挂到新窗口上，还会提前清掉新窗口的在飞标志。\n"
        "HWND g_ocrInFlightWindow = nullptr;\n"
        "unsigned long long g_ocrInFlightInstanceId = 0;\n\n"
        "bool IsOcrInFlight(HWND owner, unsigned long long instanceId) {\n"
        "    return g_ocrInFlightWindow == owner && g_ocrInFlightInstanceId == instanceId;\n"
        "}\n\n"
        "// OCR 完成回调，在 UI 线程被调用。request 在回调期间有效。\n"
        "void HandleOcrCompleted(const OcrResult& result, const OcrRequest& request) {\n"
        "    if (g_ocrInFlightWindow == request.parent &&\n"
        "        g_ocrInFlightInstanceId == request.instanceId) {\n"
        "        g_ocrInFlightWindow = nullptr;\n"
        "        g_ocrInFlightInstanceId = 0;\n"
        "    }\n"
        "    SetCursor(LoadCursorW(nullptr, IDC_ARROW));\n"
        "    if (!IsWindow(request.parent)) {\n"
        "        // 识别期间用户已经取消或关闭了截图窗口。\n"
        "        return;\n"
        "    }\n"
        "    if (ReadSnipInstanceId(request.parent) != request.instanceId) {\n"
        "        // 原截图窗口已销毁，句柄被新的截图窗口复用了。这份识别结果属于\n"
        "        // 上一个窗口，丢弃，不能挂到新窗口上。\n"
        "        return;\n"
        "    }\n"
        "    if (!result.success) {\n"
        "        MessageBoxW(request.parent, result.error.c_str(), L\"Snap-Lite · 提取文字\",\n"
        "                    MB_OK | MB_ICONWARNING);\n"
        "    } else if (result.text.empty()) {\n"
        "        MessageBoxW(request.parent, L\"当前选区没有识别到文字。\", L\"Snap-Lite · 提取文字\",\n"
        "                    MB_OK | MB_ICONINFORMATION);\n"
        "    } else {\n"
        "        ShowOcrResultPanel(request.parent, request.selection, result.text);\n"
        "    }\n"
        "}\n\n"
        "struct TextState {",
        "OCR completion handler",
    )
    text = replace_once(
        text,
        '        case PrimaryAction::Copy: FinishWithText(SnipWindow::FinishAction::Copy); break;\n'
        '        case PrimaryAction::Save: FinishWithText(SnipWindow::FinishAction::Save); break;',
        '        case PrimaryAction::Copy: FinishWithText(SnipWindow::FinishAction::Copy); break;\n'
        '        case PrimaryAction::Ocr: {\n'
        '            CommitAllTextEdits();\n'
        '            const unsigned long long instanceId = ReadSnipInstanceId(parent_);\n'
        '            if (IsOcrInFlight(parent_, instanceId)) {\n'
        '                MessageBoxW(parent_, L"正在识别中，请稍候。", L"Snap-Lite · 提取文字",\n'
        '                            MB_OK | MB_ICONINFORMATION);\n'
        '                break;\n'
        '            }\n'
        '            SetCursor(LoadCursorW(nullptr, IDC_WAIT));\n'
        '            g_ocrInFlightWindow = parent_;\n'
        '            g_ocrInFlightInstanceId = instanceId;\n'
        '            // 识别在后台线程进行，UI 不会被冻结。\n'
        '            const bool started = StartTextExtractionFromBitmapRegion(\n'
        '                snip_->UiCaptureBitmap(), snip_->UiSelectionRect(), parent_, instanceId,\n'
        '                &HandleOcrCompleted);\n'
        '            if (!started) {\n'
        '                g_ocrInFlightWindow = nullptr;\n'
        '                g_ocrInFlightInstanceId = 0;\n'
        '                SetCursor(LoadCursorW(nullptr, IDC_ARROW));\n'
        '                MessageBoxW(parent_, L"无法读取当前截图选区。", L"Snap-Lite · 提取文字",\n'
        '                            MB_OK | MB_ICONWARNING);\n'
        '            }\n'
        '            break;\n'
        '        }\n'
        '        case PrimaryAction::Save: FinishWithText(SnipWindow::FinishAction::Save); break;',
        "OCR action execution",
    )

    path.write_text(text, encoding="utf-8", newline="\n")


def patch_anime_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    text = replace_once(
        text,
        '           Exact(text, count, L"重做") || Exact(text, count, L"复制") ||\n'
        '           Exact(text, count, L"保存") || Exact(text, count, L"另存为") ||',
        '           Exact(text, count, L"重做") || Exact(text, count, L"复制") ||\n'
        '           Exact(text, count, L"取字") || Exact(text, count, L"保存") ||\n'
        '           Exact(text, count, L"另存为") ||',
        "OCR anime primary label",
    )
    text = replace_once(
        text,
        '    if (Exact(text, count, L"复制"))   return Girl::Mio;\n'
        '    if (Exact(text, count, L"保存"))   return Girl::Ritsu;',
        '    if (Exact(text, count, L"复制"))   return Girl::Mio;\n'
        '    if (Exact(text, count, L"取字"))   return Girl::Azusa;\n'
        '    if (Exact(text, count, L"保存"))   return Girl::Ritsu;',
        "OCR anime mascot",
    )
    path.write_text(text, encoding="utf-8", newline="\n")


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: add_ocr_action.py <generated-editor.cpp> <generated-anime-header.h>", file=sys.stderr)
        return 2

    editor = Path(sys.argv[1])
    anime_header = Path(sys.argv[2])
    patch_editor(editor)
    patch_anime_header(anime_header)
    print(f"Added offline OCR toolbar action: {editor}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
