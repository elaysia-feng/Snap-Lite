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
        '#include "snip_window.h"\n#include "ocr.h"\n#include "toolbar_icon_render_gdi_anime.h"',
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
        "constexpr std::array<int, 7> kActionWidths = {44, 44, 44, 44, 44, 48, 44};",
        "constexpr std::array<int, 8> kActionWidths = {44, 44, 44, 44, 48, 44, 48, 44};",
        "OCR compact action width",
    )
    text = replace_once(
        text,
        '            case PrimaryAction::Copy: return L"复制到剪贴板";\n'
        '            case PrimaryAction::Save: return L"保存到默认截图目录";',
        '            case PrimaryAction::Copy: return L"复制到剪贴板";\n'
        '            case PrimaryAction::Ocr: return L"提取文字：识别当前选区并复制到剪贴板";\n'
        '            case PrimaryAction::Save: return L"保存到默认截图目录";',
        "OCR hover hint",
    )
    text = replace_once(
        text,
        '        case PrimaryAction::Copy: FinishWithText(SnipWindow::FinishAction::Copy); break;\n'
        '        case PrimaryAction::Save: FinishWithText(SnipWindow::FinishAction::Save); break;',
        '        case PrimaryAction::Copy: FinishWithText(SnipWindow::FinishAction::Copy); break;\n'
        '        case PrimaryAction::Ocr: {\n'
        '            CommitAllTextEdits();\n'
        '            SetCursor(LoadCursorW(nullptr, IDC_WAIT));\n'
        '            const OcrResult result = ExtractTextFromBitmapRegion(\n'
        '                snip_->UiCaptureBitmap(), snip_->UiSelectionRect());\n'
        '            SetCursor(LoadCursorW(nullptr, IDC_ARROW));\n'
        '            if (!result.success) {\n'
        '                MessageBoxW(parent_, result.error.c_str(), L"Snap-Lite · 提取文字",\n'
        '                            MB_OK | MB_ICONWARNING);\n'
        '            } else if (result.text.empty()) {\n'
        '                MessageBoxW(parent_, L"当前选区没有识别到文字。", L"Snap-Lite · 提取文字",\n'
        '                            MB_OK | MB_ICONINFORMATION);\n'
        '            } else if (!CopyUnicodeTextToClipboard(parent_, result.text)) {\n'
        '                MessageBoxW(parent_, L"文字已识别，但复制到剪贴板失败。", L"Snap-Lite · 提取文字",\n'
        '                            MB_OK | MB_ICONWARNING);\n'
        '            } else {\n'
        '                MessageBoxW(parent_, L"文字已提取并复制到剪贴板。", L"Snap-Lite · 提取文字",\n'
        '                            MB_OK | MB_ICONINFORMATION);\n'
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
