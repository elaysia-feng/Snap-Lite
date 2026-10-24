"""校验构建期生成产物的**内容**，而不只是脚本退出码。

CMake 里 8 个 execute_process 只检查返回码：脚本即使"什么都没改"或
"改错了地方"，只要返回 0 构建就会全绿。这是本项目最大的 CI 盲区
（改进审计报告 P1-21）。

本脚本对生成目录做硬断言，任何一条不满足即以非零码退出。

用法:
    python scripts/verify_generated_sources.py [build_dir]
"""

from __future__ import annotations

import sys
from pathlib import Path


# (相对路径, 必须出现的片段, 必须不出现的片段)
CHECKS: list[tuple[str, tuple[str, ...], tuple[str, ...]]] = [
    (
        "generated_eraser_history/src/snip_window.h",
        (
            # 压缩版 BitmapHistory 已落地（不再继承 std::vector<HBITMAP>）
            "class BitmapHistory {",
            "kHotEntries",
            "kMaxEntries",
            "kMaxStoredBytes",
        ),
        (
            "class BitmapHistory : public std::vector<HBITMAP>",
        ),
    ),
    (
        "generated_eraser_history/src/snip_window.cpp",
        (
            # 工具栏类别顺序是 shape/arrow/pen/eraser/mosaic/text。橡皮是
            # 脚本注入的：仓库模板里没有，产物里必须有。少了它，橡皮/马赛克/
            # 文字三个工具会整体错位一格，而且编译期完全静默。
            "case Tool::Eraser:\n        detail::gActiveToolIndex = static_cast<int>(UiToolIndex::Eraser);",
            "case Tool::Mosaic:\n        detail::gActiveToolIndex = static_cast<int>(UiToolIndex::Mosaic);",
            "case Tool::Text:\n        detail::gActiveToolIndex = static_cast<int>(UiToolIndex::Text);",
            "case Tool::Eraser: return static_cast<int>(UiToolIndex::Eraser);",
            "case Tool::Mosaic: return static_cast<int>(UiToolIndex::Mosaic);",
            "case Tool::Text: return static_cast<int>(UiToolIndex::Text);",
            "case UiToolIndex::Eraser: tool_ = Tool::Eraser;",
            "case UiToolIndex::Mosaic: tool_ = Tool::Mosaic;",
            "case UiToolIndex::Text: tool_ = Tool::Text;",
            "toolIndex >= static_cast<int>(UiToolIndex::Rectangle)",
            "toolIndex <= static_cast<int>(UiToolIndex::Text)",
        ),
        (
            # 补丁注入前的旧映射。产物里一旦出现这组，说明脚本没跑到——
            # 这是"编译通过但工具错位"的静默失败，必须拦下。
            "case Tool::Mosaic: detail::gActiveToolIndex = 3;",
            "case Tool::Text: detail::gActiveToolIndex = 4;",
            "case 3: tool_ = Tool::Mosaic;",
            "case 4: tool_ = Tool::Text;",
            "toolIndex >= 0 && toolIndex <= 4",
        ),
    ),
    (
        "generated_eraser_history/src/snip_window_original.inc",
        (
            # 橡皮实现由脚本注入：原始像素快照 + 恢复 + 擦除分支
            # （needle 末尾不能带 '('：contains_token 会要求其后一位不是
            #  标识符字符，而 "(" 后面紧跟的正是参数名。）
            "RestoreOriginalBrush",
            "SetPristineCapture",
            "ClearPristineCapture",
            "tool_ == Tool::Eraser",
            # 光有标识符证明力不够：橡皮写回像素必须同时记内容版本号，
            # 否则合成图缓存会把"半成品"缓存下来。
            "++contentRevision_;",
        ),
        (),
    ),
    (
        "editor_toolbar_anime.cpp",
        (
            # 脚本注入的 OCR 主操作必须存在
            "PrimaryAction::Ocr",
            'L"取字"',
            # 产物常量：模板常量与编译产物不一致曾导致读源码 ≠ 读产物
            "kToolCount = 7",
            "kPrimaryHeight = 52",
            "kSecondaryHeight = 44",
            "kHintHeight = 26",
            "kToolWidth = 50",
            "kGap = 5",
            "kActionWidths = {48, 48, 48, 48, 50, 48, 52, 48}",
            "CategoryFromToolIndex",
            "SnipWindow::UiToolIndex::Text",
            # 橡皮工具（脚本注入）
            "enum class Category { Select = 0, Shape, Arrow, Pen, Eraser, Mosaic, Text }",
        ),
        (),
    ),
    (
        "generated_ui/pin_window.cpp",
        (
            "#include \"ui_theme.h\"",
            "kMenuWidth = 270",
            "kMenuHeight = 252",
        ),
        (),
    ),
    (
        "generated_ui/ocr_result_panel_v2.cpp",
        (
            "#include \"ui_theme.h\"",
            # 面板定位必须跟随选区所在屏幕，而不是 owner 窗口所在屏幕
            "MonitorFromPoint(rightTop",
        ),
        (),
    ),
    (
        "generated_eraser_history/src/editor_toolbar.cpp",
        (
            # 橡皮工具由脚本注入：仓库模板里没有，产物里必须有
            "enum class Category { Select = 0, Shape, Arrow, Pen, Eraser, Mosaic, Text }",
            'L"橡皮"',
        ),
        (),
    ),
]


# (生成目录内的相对路径, 仓库内必须逐字节一致的相对路径)
MUST_MATCH_REPOSITORY: list[tuple[str, str]] = [
    (
        "generated_eraser_history/src/snip_window.h",
        "src/snip_window.h",
    ),
]

_IDENT_CHARS = frozenset(
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"
)


def strip_comments(text: str) -> str:
    """把注释内容替换成空格（长度与换行不变）。

    不剥注释的话，两种假象都会出现：注释里写一句需要的片段就能满足
    "必需内容"；注释里提到一句不该出现的旧映射，又会让"禁用内容"永远
    失败——而那句旧映射正是仓库模板当前的样子，将来谁在模板里补一句
    历史注释，构建就会从此一直红。
    """
    out = list(text)
    index = 0
    length = len(text)
    while index < length:
        ch = text[index]
        if ch == '"':
            index += 1
            while index < length:
                if text[index] == "\\":
                    index += 2
                    continue
                if text[index] == '"':
                    index += 1
                    break
                index += 1
            continue
        if ch == "'":
            index += 1
            while index < length:
                if text[index] == "\\":
                    index += 2
                    continue
                if text[index] == "'":
                    index += 1
                    break
                index += 1
            continue
        if text.startswith("//", index):
            while index < length and text[index] != "\n":
                out[index] = " "
                index += 1
            continue
        if text.startswith("/*", index):
            out[index] = " "
            out[index + 1] = " "
            index += 2
            while index < length and not text.startswith("*/", index):
                if text[index] != "\n":
                    out[index] = " "
                index += 1
            if index < length:
                out[index] = " "
                out[index + 1] = " "
                index += 2
            continue
        index += 1
    return "".join(out)


def contains_token(text: str, needle: str) -> bool:
    """片段是否以完整标识符出现。

    只做子串匹配时，"PrimaryAction::Ocr" 会被改名成 "PrimaryAction::OcrX"
    蒙混过关——产物内容已经变了，校验却仍然通过。这里要求片段前后都不能
    再跟标识符字符（前缀也要管：枚举类改名成 XPrimaryAction 同样该被拦下）。
    """
    start = 0
    while True:
        index = text.find(needle, start)
        if index < 0:
            return False
        head_ok = index == 0 or text[index - 1] not in _IDENT_CHARS
        end = index + len(needle)
        tail_ok = end >= len(text) or text[end] not in _IDENT_CHARS
        if head_ok and tail_ok:
            return True
        start = end


def main(argv: list[str]) -> int:
    if len(argv) > 1:
        build_dir = Path(argv[1]).resolve()
    else:
        repo_root = Path(__file__).resolve().parent.parent
        build_dir = repo_root / "build"

    if not build_dir.is_dir():
        print(f"[verify] 找不到构建目录：{build_dir}", file=sys.stderr)
        return 2

    failures: list[str] = []
    checked = 0
    for relative, required, forbidden in CHECKS:
        path = build_dir / relative
        if not path.is_file():
            failures.append(f"{relative}: 文件不存在（生成步骤未执行？）")
            continue
        text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        for needle in required:
            checked += 1
            if not contains_token(text, needle):
                failures.append(f"{relative}: 缺少必需内容 {needle!r}")
        for needle in forbidden:
            checked += 1
            if needle in text:
                failures.append(f"{relative}: 出现不应存在的内容 {needle!r}")

    # 生成的头文件必须与仓库里的那份逐字节一致：同一二进制里存在两份
    # snip_window.h 会让 Tool 枚举静默错位。
    repo_root = Path(__file__).resolve().parent.parent
    for generated_relative, repository_relative in MUST_MATCH_REPOSITORY:
        generated_path = build_dir / generated_relative
        repository_path = repo_root / repository_relative
        if not generated_path.is_file():
            continue
        if not repository_path.is_file():
            failures.append(f"{repository_relative}: 仓库源文件不存在")
            continue
        checked += 1
        if generated_path.read_bytes() != repository_path.read_bytes():
            failures.append(
                f"{generated_relative} 与 {repository_relative} 不一致："
                "两份同名头文件会作为不同定义进入同一二进制（Tool 枚举可能错位）"
            )

    if failures:
        print("[verify] 生成产物内容校验失败：", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print(f"[verify] 生成产物内容校验通过（{len(CHECKS)} 个文件 / {checked} 条断言）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
