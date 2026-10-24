"""构建期生成管线的端到端回归测试。

这些脚本用严格模式（匹配数必须恰好为 1）改写 C++ 源码。问题是：
一旦上游重构导致 anchor 失配，要么脚本响亮失败——此时构建整个挂掉；
要么某处用了未检查的正则——静默产出错误产物且构建全绿。

本测试用 stdlib unittest 跑完整条管线（纯 Python，不需要 Windows 桌面，
因此可以在 CI 上稳定运行），并断言产物内容符合预期。

运行：
    python -m unittest discover -s tests -p "test_*.py"
"""

from __future__ import annotations

import importlib.util
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = REPO_ROOT / "scripts"


# --- C++ 源码文本工具 ---------------------------------------------------
#
# 纯文本断言有三个已经被变异测试证实过的漏洞：
#   1. 注释里写一句 "DelayOverlay::Cancel();" 就能满足"必须包含"，
#      真正的调用被挪走也照样通过；
#   2. "把一行拆成两行"是无意义的重排，不该让守卫失败；
#   3. "if (false) { ... }" 里的调用根本不会执行，等于把调用删了，
#      但文本上还在。
# 下面的工具把这三种情况都处理掉，让守卫既抓得住缺陷、又不误报重排。

_CONTROL_KEYWORDS = frozenset(
    {
        "if",
        "for",
        "while",
        "switch",
        "catch",
        "else",
        "do",
        "return",
        "sizeof",
        "case",
        "default",
        "try",
        "defined",
    }
)


def strip_comments(text: str) -> str:
    """把注释内容替换成空格（长度、换行、字符串字面量都不变）。

    位置保持不变很重要：守卫要靠下标比较来断"谁在前谁在后"。
    """
    out = list(text)
    index = 0
    length = len(text)
    while index < length:
        ch = text[index]
        if ch == '"':
            # 字符串字面量：原样保留，里面的 // 不是注释
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
            # 字符字面量：L'"' 这种里含双引号，不处理的话会被当成字符串
            # 开头，把后面一大段代码误当成字符串吞掉。
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


def _tokenize(needle: str) -> list[str]:
    """把代码片段切成 token：标识符、字符串字面量（整体）、单个标点。

    字符串字面量必须是**整体**的一个 token：否则模式会在引号内部插入
    \s*，"SnapLiteSnipWindow "（多一个尾随空格）也会被当成
    "SnapLiteSnipWindow" 命中，类名漂移就漏抓了。
    """
    tokens: list[str] = []
    index = 0
    length = len(needle)
    while index < length:
        ch = needle[index]
        if ch == '"':
            end = needle.find('"', index + 1)
            if end < 0:
                end = length - 1
            tokens.append(needle[index : end + 1])
            index = end + 1
            continue
        if ch.isalnum() or ch == "_":
            start = index
            while index < length and (needle[index].isalnum() or needle[index] == "_"):
                index += 1
            tokens.append(needle[start:index])
            continue
        tokens.append(ch)
        index += 1
    return tokens


def code_pattern(needle: str) -> re.Pattern:
    """把一段代码编成容忍空白与换行的正则（带标识符边界）。

    容忍空白是为了不因"把一行拆两行"这种无害重排误报——包括把空括号
    拆成 "(\n)" 这种；加边界是为了不把 XDelayOverlay::Cancel() 当成
    DelayOverlay::Cancel()。
    """
    pieces = [re.escape(token) for token in _tokenize(needle)]
    body = r"\s*".join(pieces)
    prefix = r"\b" if needle[:1].isalnum() or needle[:1] == "_" else ""
    suffix = r"\b" if needle[-1:].isalnum() or needle[-1:] == "_" else ""
    return re.compile(prefix + body + suffix)


_RAW_STRING = re.compile(r'(?:[uUL]|u8)*R"([^\s()\\]{0,16})\(')


def string_spans(code: str) -> list[tuple[int, int]]:
    """返回所有字符串字面量的区间（含引号）。

    匹配必须避开字符串内部：源码里写一句 L"DelayOverlay::Cancel();"
    就能骗过"必须调用 DelayOverlay::Cancel()"这种断言。
    """
    spans: list[tuple[int, int]] = []
    index = 0
    length = len(code)
    while index < length:
        raw_string = _RAW_STRING.match(code, index)
        if raw_string:
            terminator = ")" + raw_string.group(1) + '"'
            end = code.find(terminator, raw_string.end())
            end = length if end < 0 else end + len(terminator)
            spans.append((index, end))
            index = end
            continue
        if code[index] == '"':
            end = index + 1
            while end < length:
                if code[end] == "\\":
                    end += 2
                    continue
                if code[end] == '"':
                    end += 1
                    break
                end += 1
            spans.append((index, end))
            index = end
            continue
        index += 1
    return spans


def find_calls(code: str, span: tuple[int, int], needle: str) -> list[int]:
    """返回 span 内所有匹配 needle 的位置。

    容忍空白与换行；落在字符串字面量内部的匹配不算数。
    """
    start, end = span
    spans = string_spans(code)
    hits: list[int] = []
    for match in code_pattern(needle).finditer(code, start, end):
        position = match.start()
        if any(low <= position < high for low, high in spans):
            continue
        hits.append(position)
    return hits


def _call_arguments(code: str, position: int) -> list[str]:
    """取某个调用点的实参列表（按顶层逗号切分）。

    position 是**函数名**的起点（find_calls 返回的就是它）。
    """
    open_paren = code.find("(", position)
    if open_paren < 0:
        return []
    depth = 0
    end = -1
    for index in range(open_paren, len(code)):
        if code[index] == "(":
            depth += 1
        elif code[index] == ")":
            depth -= 1
            if depth == 0:
                end = index
                break
    if end < 0:
        return []
    inner = code[open_paren + 1 : end]
    parts: list[str] = []
    depth = 0
    current = ""
    for ch in inner:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(current.strip())
            current = ""
            continue
        current += ch
    if current.strip():
        parts.append(current.strip())
    return parts


def resolve_needle(
    code: str, span: tuple[int, int], needle: str
) -> tuple[list[int], list[int]]:
    """返回 (span 内的调用点, needle 实际出现的位置)。

    直接调用时两者相同；被抽进 helper 时，调用点是 helper 的调用点，
    而 needle 真正出现的位置在 helper 体内——要检查"这个调用点后面那
    一块里有没有提示"，必须用后者。
    """
    direct = find_calls(code, span, needle)
    if direct:
        return direct, direct
    sites: list[int] = []
    positions: list[int] = []
    for name, function_range in functions(code):
        if function_range[0] >= span[0] and function_range[1] <= span[1]:
            continue
        hits = find_calls(code, function_range, needle)
        if not hits:
            continue
        positions.extend(hits)
        # 调用点也要能穿透多层 helper。
        sites.extend(resolve_calls(code, span, name + "("))
    return sorted(sites), sorted(positions)


def block_after(code: str, position: int) -> str:
    """取 position 之后那个 "{" 开始的块（到配对的 "}" 为止）。"""
    open_brace = code.index("{", position)
    return code[open_brace : matching_close(code, open_brace) + 1]


def statement_at(code: str, position: int) -> str:
    """取包含该位置的那条语句（按分号切分）。"""
    start = code.rfind(";", 0, position) + 1
    end = code.find(";", position)
    if end < 0:
        end = len(code)
    return code[start:end]


def strip_cmake_comments(text: str) -> str:
    """去掉 CMake 的 # 行注释（位置保持不变）。

    CMake 的注释只在行首（可含空白）才成立。
    """
    out = list(text)
    index = 0
    length = len(text)
    while index < length:
        line_start = index
        while index < length and text[index] in " \t":
            index += 1
        if index < length and text[index] == "#":
            while index < length and text[index] != "\n":
                out[index] = " "
                index += 1
            continue
        index = line_start + 1
    return "".join(out)


_CMAKE_SET = re.compile(r"set\(\s*([A-Za-z_][A-Za-z0-9_]*)\s+([^\)\n]*)\)")


def cmake_variables(code: str) -> dict[str, str]:
    """解析 CMakeLists.txt 里的 set(VAR value)。"""
    values: dict[str, str] = {}
    for match in _CMAKE_SET.finditer(code):
        values[match.group(1)] = match.group(2).strip().strip('"')
    return values


def resolve_cmake(value: str, values: dict[str, str], depth: int = 0) -> str:
    """把 ${VAR} 逐层展开。

    展开不出来的内置变量（如 CMAKE_CURRENT_BINARY_DIR）保持原样——
    守卫正是靠它判断"这个路径是不是指向构建目录"。
    """
    if depth > 8:
        return value

    def substitute(match: re.Match[str]) -> str:
        return values.get(match.group(1), match.group(0))

    expanded = re.sub(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}", substitute, value)
    return resolve_cmake(expanded, values, depth + 1)


def body_span(code: str, signature: str) -> tuple[int, int]:
    """返回某个函数体在 code 中的 [起, 止) 绝对下标。

    找不到签名时要给出**可读**的失败信息：裸的 ValueError traceback 在
    CI 日志里看不出"是哪个锚点没了"，而锚点失效恰恰是守卫最需要报警的
    场景——源码被改了，守卫却蒙在鼓里。

    必须跳过**前向声明**：声明的后面跟的是分号，不是花括号，按"下一个
    '{' 就是函数体"去找会一路找到后面另一个函数的头上——守卫于是把
    别人的函数体当成了自己要查的函数体，凭空判红（上一轮验收实测：
    把一个函数整体前移并加前向声明 → 守卫假红）。
    """
    search_from = 0
    while True:
        start = code.find(signature, search_from)
        if start < 0:
            raise AssertionError(f"源码里找不到 {signature!r}：守卫锚点已失效")
        open_brace = code.find("{", start)
        if open_brace < 0:
            raise AssertionError(f"函数体括号不配平：{signature}")
        # 参数表结束的位置。签名本身可能不带参数表（"struct Foo {"），
        # 那种情况就用签名末尾当分界。
        header_end = start
        open_paren = code.find("(", start)
        if 0 <= open_paren < open_brace:
            depth = 0
            for index in range(open_paren, len(code)):
                if code[index] == "(":
                    depth += 1
                elif code[index] == ")":
                    depth -= 1
                    if depth == 0:
                        header_end = index
                        break
        if ";" in code[header_end + 1 : open_brace]:
            # 这是**声明**（分号结尾），不是定义：继续找下一个。
            search_from = start + 1
            continue
        depth = 0
        for index in range(open_brace, len(code)):
            if code[index] == "{":
                depth += 1
            elif code[index] == "}":
                depth -= 1
                if depth == 0:
                    return start, index + 1
        raise AssertionError(f"函数体括号不配平：{signature}")


def function_body(code: str, signature: str) -> str:
    """截取某个函数的函数体（大括号配平）。"""
    start, end = body_span(code, signature)
    return code[start:end]


def matching_close(code: str, open_brace: int) -> int:
    """返回与 open_brace 配对的右花括号下标。"""
    depth = 0
    for index in range(open_brace, len(code)):
        if code[index] == "{":
            depth += 1
        elif code[index] == "}":
            depth -= 1
            if depth == 0:
                return index
    raise AssertionError("花括号不配平")


def dead_spans(code: str) -> list[tuple[int, int]]:
    """找出"永远不执行"的代码块：if (false) / if (0) / while (0) 之类。

    把关键调用藏进这样的分支里等于把调用删了，但纯文本断言看不出来。
    """
    spans: list[tuple[int, int]] = []
    pattern = re.compile(
        r"\b(?:if|while)\s*\(\s*(?:constexpr\s*)?(?:false|FALSE|0)\s*\)"
        r"|\bif\s+constexpr\s*\(\s*(?:false|FALSE)\s*\)"
    )
    for match in pattern.finditer(code):
        open_brace = code.find("{", match.end())
        if open_brace < 0:
            continue
        spans.append((match.start(), matching_close(code, open_brace) + 1))
    return spans


def in_dead_branch(code: str, position: int) -> bool:
    """该位置是否落在永远不执行的分支里。"""
    return any(start <= position < end for start, end in dead_spans(code))


def brace_depths(code: str) -> list[int]:
    """每个位置所处的花括号深度（字符串字面量里的花括号不计）。

    深度 1 = 直接写在函数体里；深度 2 = 嵌在某个 if / for 里面。
    """
    depths = [0] * (len(code) + 1)
    depth = 0
    index = 0
    length = len(code)
    while index < length:
        ch = code[index]
        if ch == '"' or ch == "'":
            # 字符串与字符字面量：里面的花括号不是代码块
            quote = ch
            index += 1
            while index < length:
                if code[index] == "\\":
                    index += 2
                    continue
                if code[index] == quote:
                    index += 1
                    break
                index += 1
            continue
        depths[index] = depth
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
        index += 1
    depths[length] = depth
    return depths


def functions(code: str) -> list[tuple[str, tuple[int, int]]]:
    """粗略切出文件里所有具名函数的 (名字, 体区间)。

    只需要认得出"某个函数体里有这段调用"，从而容忍"把关键调用抽成
    helper"这种等价重构——那是重构，不是缺陷。
    """
    result: list[tuple[str, tuple[int, int]]] = []
    index = 0
    length = len(code)
    # 步数预算：每次循环至少推进一个字符，超预算说明某条分支忘了推进下标。
    # 少了这个预算，一个"continue 却不推进"就会变成永不退出的循环——
    # 而守卫最需要报错的场景恰恰是"关键调用被删掉"，那时会走进这条分支。
    budget = ScanBudget(length)
    while index < length:
        budget.step()
        if code[index] != "{":
            index += 1
            continue
        close = matching_close(code, index)
        header_start = 0
        for back in range(index - 1, -1, -1):
            if code[back] in ";{}":
                header_start = back + 1
                break
        header = code[header_start:index]
        if header.count("(") != header.count(")"):
            # 头尾不配平说明这不是函数定义（lambda、聚合体初始化等），
            # 别把上一条语句的函数头错安到这个块上。
            #
            # 必须推进下标再 continue：源码里出现
            # "for (auto& x : {1, 2, 3}) {" 这种块时，反向扫描会在内层 '{'
            # 处停下，头部拿到 ") "——括号不配平。早先这里直接 continue
            # 而不推进下标，函数就会在同一个 '{' 上永远转下去。
            index += 1
            continue
        name = _header_function_name(header)
        if name:
            result.append((name, (index, close + 1)))
        # 继续往里扫：namespace / class 里的函数也在这个文件里，
        # 整块跳过去会把它们全漏掉。
        index += 1
    return result


def _header_function_name(header: str) -> str:
    """从函数头里取出函数名；控制语句、lambda 等返回空串。"""
    open_paren = header.find("(")
    if open_paren < 0:
        return ""
    prefix = header[:open_paren].strip()
    if not prefix:
        return ""
    tail = prefix[-1]
    if not (tail.isalnum() or tail == "_"):
        return ""
    name = ""
    for ch in reversed(prefix):
        if ch.isalnum() or ch == "_":
            name = ch + name
        else:
            break
    if not name or name in _CONTROL_KEYWORDS:
        return ""
    return name


def resolve_calls(
    code: str, span: tuple[int, int], needle: str, max_depth: int = 4
) -> list[int]:
    """在 span 内找到 needle 的调用位置（容忍被包进 helper 的写法，可多层）。

    直接调用就返回调用点；被抽进 helper 时返回 helper 的调用点——
    "把关键调用抽成函数"是等价重构，不该让守卫失败。

    **必须支持多层**：把"销毁 + 催重绘"抽成一个 helper、再把"催重绘"
    抽成另一个 helper 之后，needle 离 span 就有两层了。只解析一层的话
    这种等价重构会被判成缺陷（上一轮验收实测：抽两层 → 守卫假红）。
    """
    direct = find_calls(code, span, needle)
    if direct:
        return direct

    all_functions = functions(code)
    sites: list[int] = []
    # 自底向上：needle 住在哪个函数里？那个函数被谁调用？一层层往上推到
    # span 所在的函数，那里的调用点才是答案。
    frontier = [
        function_span
        for _name, function_span in all_functions
        if find_calls(code, function_span, needle)
    ]
    seen: set[tuple[int, int]] = set()
    for _ in range(max_depth):
        if not frontier:
            break
        next_frontier: list[tuple[int, int]] = []
        for holder in frontier:
            if holder in seen:
                continue
            seen.add(holder)
            holder_name = _function_name_at(all_functions, holder)
            if not holder_name:
                continue
            for _caller_name, caller_span in all_functions:
                if caller_span == holder:
                    continue
                hits = find_calls(code, caller_span, holder_name + "(")
                if not hits:
                    continue
                if caller_span[0] >= span[0] and caller_span[1] <= span[1]:
                    sites.extend(hits)
                elif caller_span not in seen:
                    next_frontier.append(caller_span)
        frontier = next_frontier
    return sorted(sites)


def _function_name_at(
    all_functions: list[tuple[str, tuple[int, int]]], target: tuple[int, int]
) -> str:
    """取出某个函数区间对应的函数名。"""
    for name, function_span in all_functions:
        if function_span == target:
            return name
    return ""


def enclosing_if_conditions(code: str, position: int) -> list[str]:
    """返回包住该位置的所有 if 条件源码（由内到外）。

    用来判断"某个调用到底被哪一层条件守着"——只看"前面出现过某个
    标识符"分辨不出"它在 if 里面"还是"它只是恰好写在前面"。
    """
    conditions: list[str] = []
    index = position
    while index > 0:
        open_brace = -1
        depth = 0
        for back in range(index - 1, -1, -1):
            if code[back] == "}":
                depth += 1
            elif code[back] == "{":
                if depth == 0:
                    open_brace = back
                    break
                depth -= 1
        if open_brace < 0:
            return conditions
        header_start = 0
        for back in range(open_brace - 1, -1, -1):
            if code[back] in ";{}":
                header_start = back + 1
                break
        header = code[header_start:open_brace].strip()
        match = re.match(r"(?:else\s+)?if\s*\((.*)\)\s*$", header, re.DOTALL)
        if match:
            conditions.append(match.group(1))
        index = open_brace
    return conditions


def mask_literals(code: str) -> str:
    """把字符串与字符字面量替换成等长的空格（保留下标不变）。"""
    out = list(code)
    index = 0
    length = len(code)
    while index < length:
        ch = code[index]
        if ch not in "\"'":
            index += 1
            continue
        quote = ch
        index += 1
        while index < length:
            if code[index] == "\\":
                out[index] = " "
                if index + 1 < length:
                    out[index + 1] = " "
                index += 2
                continue
            if code[index] == quote:
                index += 1
                break
            out[index] = " "
            index += 1
    return "".join(out)


_IF_HEAD = re.compile(r"\b(?:if|while)\s*\(")


def if_block_spans(code: str) -> list[tuple[int, int, str]]:
    """所有 if/while 的 (块起, 块止, 条件文本)。

    与 if_conditions 的区别：这里给的是**块**的区间，而不是条件的区间。
    """
    masked = mask_literals(code)
    spans: list[tuple[int, int, str]] = []
    for match in _IF_HEAD.finditer(masked):
        open_paren = masked.index("(", match.start())
        depth = 0
        end = -1
        for index in range(open_paren, len(masked)):
            if masked[index] == "(":
                depth += 1
            elif masked[index] == ")":
                depth -= 1
                if depth == 0:
                    end = index
                    break
        if end < 0:
            continue
        brace = masked.find("{", end)
        if brace < 0:
            continue
        close = matching_close(masked, brace)
        if close < 0:
            continue
        spans.append((brace, close, masked[open_paren + 1 : end]))
    return spans


def if_conditions(code: str) -> list[tuple[int, int, str, str]]:
    """列出所有 if/while 的 (条件起, 条件止, 条件文本, 块文本)。"""
    masked = mask_literals(code)
    result: list[tuple[int, int, str, str]] = []
    for match in _IF_HEAD.finditer(masked):
        open_paren = masked.index("(", match.start())
        depth = 0
        end = -1
        for index in range(open_paren, len(masked)):
            if masked[index] == "(":
                depth += 1
            elif masked[index] == ")":
                depth -= 1
                if depth == 0:
                    end = index
                    break
        if end < 0:
            continue
        brace = masked.find("{", end)
        if brace < 0:
            continue
        close = matching_close(code, brace)
        result.append(
            (open_paren + 1, end, code[open_paren + 1 : end], code[brace : close + 1])
        )
    return result


_LOOP_HEAD = re.compile(r"\b(?:for|while)\s*\(")

# 可以当"时钟"用的 API。
#
# 不能只认 GetTickCount64：把时钟换成 QueryPerformanceCounter 是**完全等价**
# 的重构，守卫却会因此找不到"等待实现"那个函数、判成缺陷
# （上一轮验收实测：换时钟 → 静态红 4 条）。
_CLOCK_APIS = (
    "GetTickCount64(",
    "QueryPerformanceCounter(",
    "GetSystemTimeAsFileTime(",
    "timeGetTime(",
)


_TEST_HOOK_MACROS = frozenset(
    {"SNAPLITE_DELAY_TEST_HOOKS", "SNAPLITE_EDIT_TEST_HOOKS"}
)


def _directive_at(line: str) -> tuple[str, str] | None:
    """这一行是不是预处理指令？是的话返回 (指令名, 参数)。"""
    stripped = line.strip()
    if not stripped.startswith("#"):
        return None
    match = re.match(r"#\s*([A-Za-z_]+)\s*(.*)$", stripped)
    if not match:
        return None
    return match.group(1), match.group(2)


def compiled_spans(code: str, undefined_macros: set[str]) -> list[tuple[int, int]]:
    """给定宏"未定义"时，哪些代码区间会被编译进产品。

    必须**按配对栈**处理，不能"从 `#ifdef` 找第一个 `#endif`"：
    - 嵌套的 `#ifdef` 会让"第一个 `#endif`"配错对（假红：
      合法写法被误判成"钩子跑到了条件编译块之外"）；
    - `#else` 分支在宏未定义时**是会被编译进去的**——钩子实现放在
    `#else` 里，符号照样进产品，而"找第一个 #endif"的写法会把它
   当成"在块内"放过去（漏判）。
    """
    spans: list[tuple[int, int]] = []
    # 每层两件事：当前分支局部是否编译、这一层是否已有分支被编译
    stack: list[list[bool]] = []
    segment_start = 0
    index = 0
    length = len(code)

    def outer_compiled() -> bool:
        return all(frame[0] for frame in stack)

    while index < length:
        line_end = code.find("\n", index)
        if line_end < 0:
            line_end = length
        directive = _directive_at(code[index:line_end])
        if directive is not None:
            kind, rest = directive
            if kind in ("if", "ifdef", "ifndef"):
                if outer_compiled():
                    spans.append((segment_start, index))
                outer = outer_compiled()
                if kind == "ifdef":
                    local = rest.strip() not in undefined_macros
                elif kind == "ifndef":
                    local = rest.strip() in undefined_macros
                else:
                    local = rest.strip() not in undefined_macros
                stack.append([outer and local, outer and local])
                segment_start = line_end + 1
            elif kind in ("else", "elif"):
                if not stack:
                    index = line_end + 1
                    continue
                if outer_compiled():
                    spans.append((segment_start, index))
                frame = stack[-1]
                outer = all(parent[0] for parent in stack[:-1])
                if kind == "elif":
                    # 条件表达式不做求值：保守当成"会编译"（宁可多报，
                    # 不可漏报——钩子漏进产品比误报严重得多）。
                    local = True
                else:
                    local = not frame[1]
                frame[0] = outer and local
                frame[1] = frame[1] or (outer and local)
                segment_start = line_end + 1
            elif kind == "endif":
                if outer_compiled():
                    spans.append((segment_start, index))
                if stack:
                    stack.pop()
                segment_start = line_end + 1
        index = line_end + 1
    if outer_compiled():
        spans.append((segment_start, length))
    return spans


def excluded_spans(code: str, undefined_macros: set[str]) -> list[tuple[int, int]]:
    """给定宏"未定义"时，哪些代码区间**不会**被编译进产品。

    也就是"产品构建里被条件编译挡掉"的部分——测试钩子必须待在这里。
    """
    compiled = compiled_spans(code, undefined_macros)
    result: list[tuple[int, int]] = []
    cursor = 0
    for start, end in compiled:
        if start > cursor:
            result.append((cursor, start))
        cursor = max(cursor, end)
    if cursor < len(code):
        result.append((cursor, len(code)))
    return result


def clock_calling_functions(code: str) -> set[str]:
    """所有"（直接或间接）用到时钟"的函数名。

    要算**传递闭包**：把 `GetTickCount64()` 包进自己的
    `MonotonicNowMs()` 里是常见的等价重构，只认"函数体里直接出现
    时钟 API"就会把这种改写判成缺陷（上一轮验收实测：换时钟 →
    静态红 4 条）。
    """
    defined = functions(code)
    names: set[str] = set()
    for name, fn_span in defined:
        if any(find_calls(code, fn_span, api) for api in _CLOCK_APIS):
            names.add(name)
    changed = True
    while changed:
        changed = False
        for name, fn_span in defined:
            if name in names:
                continue
            if any(find_calls(code, fn_span, other + "(") for other in names):
                names.add(name)
                changed = True
    return names


def uses_clock(code: str, fn_span: tuple[int, int], clocks: set[str]) -> bool:
    """这个函数体里（直接或间接）用了时钟没有。"""
    if any(find_calls(code, fn_span, api) for api in _CLOCK_APIS):
        return True
    return any(find_calls(code, fn_span, other + "(") for other in clocks)


def loop_blocks(code: str) -> list[tuple[int, int, str]]:
    """列出所有 for/while 的 (头起, 头止, 块文本)。"""
    masked = mask_literals(code)
    result: list[tuple[int, int, str]] = []
    for match in _LOOP_HEAD.finditer(masked):
        open_paren = masked.index("(", match.start())
        depth = 0
        end = -1
        for index in range(open_paren, len(masked)):
            if masked[index] == "(":
                depth += 1
            elif masked[index] == ")":
                depth -= 1
                if depth == 0:
                    end = index
                    break
        if end < 0:
            continue
        brace = masked.find("{", end)
        if brace < 0:
            continue
        close = matching_close(code, brace)
        result.append((open_paren + 1, end, code[brace : close + 1]))
    # do { ... } while (...); —— 循环体在 while 头**之前**，
    # 只认 for/while 会把这种等价改写判成"没有轮询循环"
    # （上一轮验收实测：改成 do-while → 静态假红）。
    for match in re.finditer(r"\bdo\s*\{", masked):
        brace = masked.index("{", match.start())
        close = matching_close(code, brace)
        result.append((match.start(), match.end(), code[brace : close + 1]))
    return result


def condition_containing(code: str, position: int) -> tuple[str, str] | None:
    """返回**最内层**包住该位置的 if 条件与它的块。"""
    found: tuple[str, str] | None = None
    for start, end, condition, block in if_conditions(code):
        if start <= position <= end:
            found = (condition, block)
    return found


def next_condition_after(code: str, position: int, limit: int) -> str | None:
    """返回 position 之后（且在 limit 之前）最近的一条 if 条件。"""
    for start, _end, condition, _block in if_conditions(code):
        if position < start < limit:
            return condition
    return None


def dead_condition(condition: str) -> bool:
    """条件本身是不是恒假的（if (false) / if (false && ...)）。"""
    text = condition.strip()
    return bool(
        re.match(r"^(?:constexpr\s*)?(?:false|FALSE|0)\s*(?:&&|\)|$)", text)
    )


def parameter_names(header: str) -> list[str]:
    """取出函数头里的参数名（按顺序）。"""
    open_paren = header.find("(")
    if open_paren < 0:
        return []
    depth = 0
    end = -1
    for index in range(open_paren, len(header)):
        if header[index] == "(":
            depth += 1
        elif header[index] == ")":
            depth -= 1
            if depth == 0:
                end = index
                break
    if end < 0:
        return []
    pieces: list[str] = []
    current = ""
    depth = 0
    for ch in header[open_paren + 1 : end]:
        if ch in "([<":
            depth += 1
        elif ch in ")]>":
            depth -= 1
        if ch == "," and depth == 0:
            pieces.append(current)
            current = ""
            continue
        current += ch
    pieces.append(current)
    names: list[str] = []
    for piece in pieces:
        identifiers = re.findall(r"[A-Za-z_][A-Za-z0-9_]*", piece)
        if identifiers:
            names.append(identifiers[-1])
    return names


class ScanBudget:
    """扫描步数预算：分析器自己的保险丝。

    守卫最需要报错的场景恰恰是"关键调用被删掉"——那时分析器会走进
    某个"不推进下标"的分支。没有这道保险丝，一次分析就是一次 CI 超时，
    而不是一条"哪里不对"的失败信息。

    单独做成一个类，是为了让"预算真的会触发"这件事本身可以被测试。
    """

    def __init__(self, total: int) -> None:
        self.remaining = total + 1

    def step(self) -> None:
        self.remaining -= 1
        if self.remaining < 0:
            raise AssertionError(
                "扫描步数超限，说明分析逻辑在某个分支上没有推进下标"
            )


def _load_module(name: str):
    """按文件路径加载 scripts/ 下的模块（该目录不是包）。"""
    path = SCRIPTS / f"{name}.py"
    spec = importlib.util.spec_from_file_location(f"_snaplite_{name}", path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class GenerationPipelineTest(unittest.TestCase):
    """把整条生成管线在临时目录里跑一遍，然后对产物做断言。

    整条管线在 setUpClass 里一次跑完，单个测试之间不依赖执行顺序。
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls._tmp = tempfile.TemporaryDirectory(prefix="snaplite-gen-")
        cls.tmp = Path(cls._tmp.name)
        cls.history_root = cls.tmp / "generated_eraser_history"
        cls.generated_src = cls.history_root / "src"
        cls.anime_source = cls.tmp / "editor_toolbar_anime.cpp"
        cls.ui_root = cls.tmp / "generated_ui"
        cls.failures: list[str] = []

        cls._run(
            "generate_eraser_history_sources.py",
            str(REPO_ROOT),
            str(cls.history_root),
        )
        cls._run(
            "generate_anime_toolbar_source.py",
            str(cls.generated_src / "editor_toolbar.cpp"),
            str(cls.anime_source),
        )
        cls._run("add_toolbar_tooltips.py", str(cls.anime_source))
        cls._run(
            "add_ocr_action.py",
            str(cls.anime_source),
            str(cls.generated_src / "toolbar_icon_render_gdi_anime.h"),
        )
        cls._run(
            "apply_unified_toolbar_theme.py",
            str(cls.generated_src / "toolbar_icon_render_gdi_anime.h"),
        )
        cls._run("generate_unified_ui_sources.py", str(REPO_ROOT), str(cls.ui_root))

    @classmethod
    def tearDownClass(cls) -> None:
        cls._tmp.cleanup()

    @classmethod
    def _run(cls, script: str, *args: str) -> None:
        result = subprocess.run(
            [sys.executable, str(SCRIPTS / script), *args],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        if result.returncode != 0:
            raise RuntimeError(
                f"{script} 失败：\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
            )

    # --- 产物存在性 -----------------------------------------------------

    def test_generated_files_exist(self) -> None:
        for name in (
            "editor_toolbar.cpp",
            "snip_window.cpp",
            "snip_window.h",
            "snip_window_original.inc",
            "toolbar_icon_render_gdi_anime.h",
            "toolbar_icon_render_gdi.h",
            "edit_repaint_fix.h",
            "window_bounds_fix.h",
        ):
            self.assertTrue(
                (self.generated_src / name).is_file(),
                msg=f"缺少生成文件 {name}",
            )
        self.assertTrue(self.anime_source.is_file())
        self.assertTrue((self.ui_root / "pin_window.cpp").is_file())
        self.assertTrue((self.ui_root / "ocr_result_panel_v2.cpp").is_file())

    # --- 关键不变量：生成的头文件必须与仓库里的完全一致 ------------------

    def test_generated_snip_window_header_matches_repository(self) -> None:
        """双份 snip_window.h 是 Tool 枚举错位的根源：仓库版没有 Eraser，
        生成版有。任何一边漂移都会让新写的 TU 编译出错误的工具索引。"""
        generated = (self.generated_src / "snip_window.h").read_text(encoding="utf-8")
        repository = (REPO_ROOT / "src" / "snip_window.h").read_text(encoding="utf-8")
        self.assertEqual(
            generated,
            repository,
            msg=(
                "生成的 snip_window.h 与 src/snip_window.h 不一致："
                "两者会作为不同定义进入同一二进制，Tool 枚举可能错位"
            ),
        )

    def test_generated_snip_window_header_has_eraser_and_compressed_history(self) -> None:
        header = (self.generated_src / "snip_window.h").read_text(encoding="utf-8")
        self.assertIn("class BitmapHistory {", header)
        self.assertNotIn("class BitmapHistory : public std::vector<HBITMAP>", header)
        self.assertIn("kMaxStoredBytes", header)
        self.assertIn("enum class UiToolIndex : int", header)
        self.assertIn("Eraser", header)

    def test_generated_toolbar_has_eraser_and_ocr(self) -> None:
        text = self.anime_source.read_text(encoding="utf-8")
        self.assertIn(
            "enum class Category { Select = 0, Shape, Arrow, Pen, Eraser, Mosaic, Text }",
            text,
        )
        self.assertIn("CategoryFromToolIndex", text)
        self.assertIn("SnipWindow::UiToolIndex::Text", text)
        self.assertIn("kPrimaryHeight = 52", text)
        self.assertIn("kSecondaryHeight = 44", text)
        self.assertIn("kHintHeight = 26", text)
        self.assertIn("kToolWidth = 50", text)
        self.assertIn("kGap = 5", text)
        self.assertIn("kActionWidths = {48, 48, 48, 48, 50, 48, 52, 48}", text)
        self.assertIn("PrimaryAction::Ocr", text)
        self.assertIn('L"取字"', text)

    def test_generated_unified_ui_has_theme(self) -> None:
        pin = (self.ui_root / "pin_window.cpp").read_text(encoding="utf-8")
        panel = (self.ui_root / "ocr_result_panel_v2.cpp").read_text(encoding="utf-8")
        self.assertIn('#include "ui_theme.h"', pin)
        self.assertIn("kMenuWidth = 270", pin)
        self.assertIn('#include "ui_theme.h"', panel)
        self.assertIn("MonitorFromPoint(rightTop", panel)

    def test_generated_toolbar_compares_window_instance_id(self) -> None:
        """生成的工具栏源码必须真的拿实例序号做比较。

        只断言"标识符出现过"是不够的：把比较表达式改成 if (false) 也能过。
        这里直接检查**生成产物**（真正的 C++ 文本），要求那几段比较在。
        """
        text = self.anime_source.read_text(encoding="utf-8")
        flat = "".join(text.split())
        for pattern, message in (
            (
                "ReadSnipInstanceId(request.parent) != request.instanceId",
                "OCR 回调必须拿实例序号做比较，不能只比 HWND",
            ),
            (
                "g_ocrInFlightWindow == request.parent && "
                "g_ocrInFlightInstanceId == request.instanceId",
                "在飞标志必须同时记 HWND 与实例序号",
            ),
            (
                "unsigned long long g_ocrInFlightInstanceId = 0;",
                "在飞标志的实例序号必须有声明",
            ),
            (
                "bool IsOcrInFlight(HWND owner, unsigned long long instanceId)",
                "在飞判定必须带实例序号参数",
            ),
            (
                "g_ocrInFlightInstanceId = instanceId;",
                "发起识别时必须记下当前窗口的实例序号",
            ),
        ):
            self.assertIn(
                "".join(pattern.split()),
                flat,
                msg=message,
            )

    # --- 校验器自身 -----------------------------------------------------

    def test_verify_generated_sources_accepts_pipeline_output(self) -> None:
        """校验器必须认可本管线刚产出的东西。"""
        self._run("verify_generated_sources.py", str(self.tmp))

    def test_verify_generated_sources_rejects_empty_build(self) -> None:
        """校验器必须拒绝一个没有产物的构建目录（否则等于没校验）。"""
        with tempfile.TemporaryDirectory(prefix="snaplite-empty-") as empty:
            result = subprocess.run(
                [sys.executable, str(SCRIPTS / "verify_generated_sources.py"), empty],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
            )
            self.assertNotEqual(result.returncode, 0)

    def test_verify_generated_sources_detects_tampered_header(self) -> None:
        """校验器必须能抓到"产物内容被改坏"——否则它只是装饰。"""
        tampered = Path(tempfile.mkdtemp(prefix="snaplite-tamper-"))
        try:
            shutil.copytree(self.tmp, tampered, dirs_exist_ok=True)
            header = tampered / "generated_eraser_history" / "src" / "snip_window.h"
            header.write_text("// tampered\n", encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(SCRIPTS / "verify_generated_sources.py"), str(tampered)],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
            )
            self.assertNotEqual(result.returncode, 0)
        finally:
            shutil.rmtree(tampered, ignore_errors=True)


class StrictReplacementTest(unittest.TestCase):
    """严格替换助手本身的行为：anchor 失配必须响亮失败。"""

    def test_replace_once_accepts_single_match(self) -> None:
        module = _load_module("generate_unified_ui_sources")
        self.assertEqual(
            module.replace_once("aXbc", "X", "Y", "t"), "aYbc"
        )

    def test_replace_once_rejects_zero_match(self) -> None:
        module = _load_module("generate_unified_ui_sources")
        with self.assertRaises(RuntimeError):
            module.replace_once("abc", "X", "Y", "t")

    def test_replace_once_rejects_ambiguous_match(self) -> None:
        module = _load_module("generate_unified_ui_sources")
        with self.assertRaises(RuntimeError):
            module.replace_once("aXbXc", "X", "Y", "t")


class TemplateAnchorTest(unittest.TestCase):
    """模板源码里必须仍然含有脚本依赖的关键结构。"""

    def test_toolbar_template_has_category_enum(self) -> None:
        text = (REPO_ROOT / "src" / "editor_toolbar.cpp").read_text(encoding="utf-8")
        self.assertIn("enum class Category", text)

    def test_pin_template_has_menu_rect_helpers(self) -> None:
        text = (REPO_ROOT / "src" / "pin_window.cpp").read_text(encoding="utf-8")
        self.assertIn("RECT SliderRect()", text)
        self.assertIn("RECT MenuRowRect(int row)", text)

    def test_snip_header_has_bitmap_history(self) -> None:
        text = (REPO_ROOT / "src" / "snip_window.h").read_text(encoding="utf-8")
        self.assertIn("class BitmapHistory", text)

    def test_obsolete_optimizer_script_is_gone(self) -> None:
        """optimize_bitmap_history.py 的职责已落地进 src/snip_window.h。
        它若复活，会因锚点 `class BitmapHistory : public std::vector<HBITMAP>`
        不再存在而报错，或悄悄把历史策略改回旧值。"""
        self.assertFalse((SCRIPTS / "optimize_bitmap_history.py").exists())


class SourceGuardTest(unittest.TestCase):
    """源码级守卫：把曾经出过问题的结构性不变量钉死在测试里。

    这些约束用运行时交互测试很难覆盖（要复现"连续点两次延时截图"、
    "第二实例退出"这类时序），但它们一旦被改坏就是明确的缺陷，所以用
    源码断言守住。断言写成"必须包含某段逻辑"而不是逐字符匹配，避免
    无意义的重排触发失败。
    """

    @staticmethod
    def _body(text: str, signature: str) -> str:
        """截取某个函数的函数体（大括号配平）。"""
        return function_body(text, signature)

    def _read(self, relative: str) -> str:
        return (REPO_ROOT / relative).read_text(encoding="utf-8")

    @staticmethod
    def _called_functions(
        code: str, span: tuple[int, int]
    ) -> list[tuple[str, tuple[int, int]]]:
        """列出 span 内调用到的、文件里定义了的函数（**名字 + 体区间**）。

        必须递归展开：只展开一层的话，"把等待循环再抽一层 helper"这种
        等价重构会被判成缺陷（上一轮验收实测：抽两层 → 守卫假红）。
        重构不是缺陷，把关键调用藏进 helper 也不是——要断的是"调用还在
        不在"，不是"它写在第几层"。

        而且必须按 (名字, 区间) 记，不能只按名字：文件里有两个同名函数时
        （src/app.cpp 里就有两个 App），只记名字会把其中一个吞掉，
        后面按名字回查函数体就查错了对象。
        """
        defined = functions(code)
        found: list[tuple[str, tuple[int, int]]] = []
        seen: set[tuple[str, tuple[int, int]]] = set()
        frontier: list[tuple[int, int]] = [span]
        visited: set[tuple[int, int]] = {span}
        while frontier:
            current = frontier.pop()
            for name, function_span in defined:
                key = (name, function_span)
                if key in seen:
                    continue
                if not find_calls(code, current, name + "("):
                    continue
                seen.add(key)
                found.append(key)
                if function_span not in visited:
                    visited.add(function_span)
                    frontier.append(function_span)
        return found

    @staticmethod
    def _header_of(code: str, span: tuple[int, int]) -> str:
        """取函数的函数头（含参数列表）。"""
        open_brace = span[0]
        start = max(
            code.rfind(";", 0, open_brace), code.rfind("}", 0, open_brace)
        ) + 1
        return code[start : open_brace + 1]

    # --- 延时截图覆盖层：状态必须绑在窗口上 ---------------------------

    def test_delay_overlay_state_is_bound_to_its_window(self) -> None:
        """延时覆盖层曾用全局 g_state 释放状态。

        倒计时进行中再次触发"延时截图"时，旧窗口的取消消息会晚于新窗口的
        创建才被处理，那时 g_state 已指向新实例——按全局指针释放就会释放
        新实例的状态（悬垂指针 + 泄漏旧实例），表现为倒计时冻结、幽灵窗口
        和释放后使用。状态必须从窗口自己的 GWLP_USERDATA 取。
        """
        text = strip_comments(self._read("src/delay_overlay.cpp"))
        self.assertIn("TakeOverlayState(", text)
        body = self._body(text, "void FinishOverlay(HWND hwnd, bool notify)")
        self.assertNotIn("state = g_state", body)
        self.assertNotIn("delete g_state", body)
        self.assertIn("TakeOverlayState(hwnd)", body)
        # 全局指针只在"指向的就是本窗口状态"时才清空。
        self.assertIn("g_state == state", body)

    def test_delay_overlay_cancels_synchronously(self) -> None:
        """Cancel 必须同步销毁窗口，不能只投递一条消息。

        投递的消息要等消息泵才处理，期间旧实例还活着，而新的倒计时已经
        开始，两个实例会同时占用全局状态。
        """
        code = strip_comments(self._read("src/delay_overlay.cpp"))
        span = body_span(code, "void DelayOverlay::Cancel()")
        destroys = resolve_calls(code, span, "DestroyOverlayAndWaitForRepaint(")
        self.assertTrue(
            destroys,
            msg="Cancel 必须同步销毁覆盖层窗口",
        )
        self.assertFalse(
            in_dead_branch(code, destroys[0]),
            msg="销毁调用落在了不会执行的分支里，等于根本没销毁",
        )
        self.assertFalse(
            find_calls(code, span, "PostMessageW"),
            msg="Cancel 不能只投递消息——投递要等消息泵才处理",
        )

    def test_delay_overlay_destroy_helper_waits_for_repaint(self) -> None:
        """销毁覆盖层后必须等桌面重绘，才允许后续截图。

        覆盖层不是分层窗口，屏幕抓取一定会把它拍进去。桌面窗口属于
        explorer 进程，重绘由它自己调度，所以只能主动作废那块区域再等它画回来。

        这里断的是**关系**："销毁 → 作废 → 等重绘"三步的先后顺序，
        而且"等重绘"必须是真正会读像素、会看时钟的那个函数（不能是
        随便一个同名的东西，也不能是抽出来的空壳）。

        还要断两件上一轮验收实测出来的事：

        - **可达性**：把任一步藏进 `if (false) { ... }` 等于把它删了，
          而纯文本断言看不出来；
        - **销毁后的那次作废不能被"轮询期那次"兑现**：轮询等待期间也会
          催重绘，但那个调用点在等待实现里，不在销毁流程里。少了
          "销毁后主动作废"这一步，繁忙机器上截图会把覆盖层拍进去
          （上一轮验收实测：删掉销毁后的那次请求，守卫仍然全绿）。
        """
        code = strip_comments(self._read("src/delay_overlay.cpp"))
        span = body_span(code, "void DestroyOverlayAndWaitForRepaint(")
        # 销毁与作废都要用 resolve_calls：把这两步抽进 helper 是等价重构，
        # 不该让守卫变红。find_calls 只认直接调用，两种工具混用等于把
        # "等价重构"从允许列表里划掉了一半（上一轮验收实测：抽 helper → 假红）。
        destroys = resolve_calls(code, span, "DestroyWindow(")
        self.assertTrue(destroys, msg="必须销毁覆盖层窗口")

        # "等重绘"：被调用的函数里，必须有一个真的会读覆盖层同色像素、
        # 而且会看时钟的那个——只看函数名会被同名空壳骗过去。
        # 调用点要能穿透多层 helper：把等待循环再抽一层也是等价重构。
        waits: list[int] = []
        clocks = clock_calling_functions(code)
        for name, function_span in self._called_functions(code, span):
            if not find_calls(code, function_span, "CountOverlayColours("):
                continue
            if not uses_clock(code, function_span, clocks):
                continue
            waits.extend(resolve_calls(code, span, name + "("))
        self.assertTrue(waits, msg="必须等桌面重绘")

        # 销毁之后主动作废的那一次。
        #
        # 调用点必须落在本函数体里，而且不能是"等重绘"那个调用点本身：
        # 等待实现内部也会催重绘，那是从等待调用**传递**过来的，
        # 拿它来兑现"销毁后主动作废"这一步，等于什么都没断言。
        repaints = [
            position
            for position in resolve_calls(code, span, "InvalidateRect(")
            if span[0] <= position < span[1] and position not in set(waits)
        ]
        self.assertTrue(
            repaints,
            msg="必须主动作废覆盖层所在区域（轮询期那次不能拿来兑现这一步）",
        )

        for position in destroys + waits + repaints:
            self.assertFalse(
                in_dead_branch(code, position),
                msg="销毁/作废/等待落在了不会执行的分支里，等于根本没做",
            )

        # 顺序：先销毁，再作废。
        #
        # 要在**真正执行这两个动作的函数体里**比，而不是在调用点比：
        # 把"销毁+作废"抽成一个 helper 之后，两步落在同一次调用里，
        # 按调用点比会得出"两者同一位置"的结论——那是等价重构，不是缺陷
        # （上一轮验收实测：抽 helper → 守卫假红）。
        checked = 0
        for _name, holder in functions(code):
            destroy_at = find_calls(code, holder, "DestroyWindow(")
            repaint_at = find_calls(code, holder, "RequestDesktopRepaint(")
            if not destroy_at or not repaint_at:
                continue
            checked += 1
            self.assertLess(
                destroy_at[0],
                repaint_at[0],
                msg="必须先销毁覆盖层，再让桌面重画那一块",
            )
        if checked == 0:
            # 两者不在同一个函数里（拆得更细了）：退回到调用点的先后。
            self.assertLessEqual(
                min(destroys + repaints),
                waits[0],
                msg="必须先销毁并作废，再等重绘",
            )

        self.assertLess(repaints[0], waits[0], msg="必须先作废再等重绘")
        self.assertLess(destroys[0], waits[0], msg="必须先销毁再等待重绘")

    def test_desktop_repaint_never_erases_or_covers_the_whole_screen(self) -> None:
        """对桌面窗口的重绘请求必须限定区域、而且不擦背景。

        桌面窗口横跨**所有**显示器。拿它整块重绘（区域传 nullptr）或者
        带擦除（erase=TRUE / RDW_ERASE）重绘，explorer 会把整块虚拟桌面
        重画一遍——用户在另一块显示器上干活时会被闪到。

        这是实测踩到的：延时覆盖层的运行时测试在桌面一跑，桌面二跟着闪；
        产品的"催桌面重绘"每 30ms 一次、800ms 内二十多次，远程桌面下
        这一串重绘会被放大成明显的闪屏。

        这里断的是**实参**：区域必须非空、不能带擦除标志。

        还要断"确实催了"：只检查实参的话，把两处调用整段删掉就没人报警
        了——而那正是产品在繁忙机器上避免"截图拍到覆盖层"的依托。
        """
        for relative in ("src/delay_overlay.cpp", "tests/delay_overlay_test.cpp"):
            code = strip_comments(self._read(relative))
            for callee in ("InvalidateRect", "RedrawWindow"):
                for position in self._desktop_repaint_sites(code, callee):
                    self.assertFalse(
                        in_dead_branch(code, position),
                        msg=(
                            f"{relative}: 对桌面窗口的重绘请求落在了不会执行的分支里，"
                            "等于根本没请求"
                        ),
                    )
            for position, args in self._desktop_repaint_calls(code, "InvalidateRect"):
                parts = [part.strip() for part in args.split(",")]
                self.assertGreaterEqual(
                    len(parts), 3, msg=f"{relative}: InvalidateRect 的实参不完整"
                )
                owner = self._enclosing_function(code, position)
                owner_span = owner[1] if owner else None
                # 区域与擦除标志都可能是"记录器"式的实参
                # （RepaintTarget(&region) / RepaintEraseFlag()），
                # 也可能是某个 helper 的形参（把重绘整段抽进
                # IssueDesktopInvalidate(rect, erase)），
                # 得往里看一层、并追到**每一个**调用点，
                # 才知道真正交给 Win32 的是什么。
                #
                # 报错里要带上**真正传下去的值**：只报形参名，排查时
                # 会指向错的地方（上一轮验收实测）。
                for rect in self._effective_arguments(
                    code, parts[1], fn_span=owner_span
                ):
                    self.assertTrue(
                        rect.startswith("&"),
                        msg=(
                            f"{relative}: 对桌面窗口的重绘必须限定区域"
                            f"（实参 {args} → 真正传下去的是 {rect}）"
                            "——整块重绘会把别的显示器也闪一遍"
                        ),
                    )
                for erase in self._effective_arguments(
                    code, parts[2], fn_span=owner_span
                ):
                    self.assertNotEqual(
                        erase.upper(),
                        "TRUE",
                        msg=(
                            f"{relative}: 不要擦桌面背景（实参 {args}"
                            f" → 真正传下去的是 {erase}）"
                            "——会连带引起壁纸黑闪"
                        ),
                    )
            for position, args in self._desktop_repaint_calls(code, "RedrawWindow"):
                parts = [part.strip() for part in args.split(",")]
                self.assertGreaterEqual(
                    len(parts), 2, msg=f"{relative}: RedrawWindow 的实参不完整"
                )
                owner = self._enclosing_function(code, position)
                owner_span = owner[1] if owner else None
                for rect in self._effective_arguments(
                    code, parts[1], fn_span=owner_span
                ):
                    self.assertTrue(
                        rect.startswith("&"),
                        msg=(
                            f"{relative}: 对桌面窗口的重绘必须限定区域"
                            f"（实参 {args} → 真正传下去的是 {rect}）"
                            "——整块重绘会把别的显示器也闪一遍"
                        ),
                    )
                self.assertNotIn(
                    "RDW_ERASE",
                    args,
                    msg=(
                        f"{relative}: 不要用 RDW_ERASE 重绘桌面（实参 {args}）"
                        f"——擦背景会引起跨屏闪烁"
                    ),
                )

        # 产品必须**确实催过**桌面重绘：只查实参的话，把调用整段删掉
        # 反而没人报警——而那正是产品在繁忙机器上避免"截图拍到覆盖层"的依托。
        #
        # 两种调用只要有**一种**就行：RDW_INVALIDATE 等价于
        # InvalidateRect(..., FALSE)，两个都留着只是把同一块区域作废两次。
        product = strip_comments(self._read("src/delay_overlay.cpp"))
        product_calls = self._desktop_repaint_arguments(product, "InvalidateRect") + [
            args for args in self._desktop_repaint_arguments(product, "RedrawWindow")
        ]
        self.assertTrue(
            product_calls,
            msg="产品必须主动请求桌面重画那一块区域",
        )

    def test_delay_overlay_scales_the_residue_tolerance_by_dpi(self) -> None:
        """残留容忍度必须按覆盖层所在显示器的 DPI 缩放。

        照搬 96 DPI 下的 64 像素，高 DPI 下就永远等不满（圆角更大、
        抗锯齿边缘更长），后果是每次延时截图都吃满等待上限，把界面冻住。

        这一条只能放在源码级：运行时测试在 96 DPI 的机器上量不出差别
        （系数是 1），写死一个 64 放进运行时测试又会在产品合法调参时
        假红。
        """
        code = strip_comments(self._read("src/delay_overlay.cpp"))
        self.assertIn(
            "MulDiv(kOverlayResidueTolerance",
            code,
            msg="残留容忍度必须按覆盖层所在屏的 DPI 缩放",
        )
        self.assertIn(
            "state->residueTolerance =",
            code,
            msg="缩放后的容忍度必须真的用上",
        )
        # 圆角半径与顶部偏移同样要缩放。
        self.assertIn("MulDiv(kOverlayCornerRadius", code)
        self.assertIn("MulDiv(kOverlayTopOffset", code)

    def test_delay_overlay_repaint_counter_needs_a_real_request(self) -> None:
        """催重绘的计数只能在请求**真的发出去**之后才涨。

        以前计数是"调用之后无条件 ++"，写在调用点旁边。于是把重绘调用
        整段删掉之后，计数照样在涨，运行时测试 49 项全过：它验的是
        "计数器涨了"，不是"桌面真的被要求重画了"（上一轮验收实测）。

        这里断的是**关系**：

        - 请求的结果必须被一个布尔量接住，而且那个布尔量来自一个
          **真的会调用 Win32 重绘 API** 的函数（不能是同名空壳）；
        - 计数必须落在"请求没成功就到此为止"这道闸门**后面**
          （闸门可以是提前 return，也可以是 if 包住计数块）。
        """
        code = strip_comments(self._read("src/delay_overlay.cpp"))
        request_functions = self._repaint_result_sources(code)
        self.assertTrue(
            request_functions,
            msg="产品里必须有一个真的会针对桌面窗口发重绘请求的函数",
        )

        # 找出"请求结果"变量：赋值给了某个函数调用结果的布尔变量，
        # 而那个函数确实会发重绘请求。
        #
        # 类型名不能只认 bool：产品用 BOOL 接 InvalidateRect 的返回值
        # 是完全正常的写法，只认 bool 会把那种写法误判成"没接住"
        # （上一轮验收实测：把重绘抽进 helper 并改用 BOOL → 守卫假红）。
        #
        # 而且要收**全部**候选，不能只取第一个：抽 helper 之后链路上会
        # 有两处接住返回值（helper 里那个、调用方那个），只有其中一个
        # 真正守在计数前面。只取第一个会挑错对象，凭空判红
        # （上一轮验收实测：抽 helper → 守卫假红）。
        result_variables: list[str] = []
        for match in re.finditer(
            r"(?:const\s+)?(?:bool|BOOL|int|unsigned|DWORD|LONG|long)\s+"
            r"(\w+)\s*=\s*(\w+)\s*\(",
            code,
        ):
            if match.group(2) in request_functions:
                result_variables.append(match.group(1))
        self.assertTrue(
            result_variables,
            msg="催重绘的请求结果必须被接住（赋给一个布尔量），否则没法据此计数",
        )

        increments = list(
            re.finditer(
                r"\+\+\s*g_last(?:WaitPoll|Destroy|Poll)RepaintCount", code
            )
        )
        self.assertTrue(
            increments,
            msg="产品必须统计催了几次重绘（运行时测试靠它判断有没有真的催过）",
        )
        for match in increments:
            self.assertTrue(
                any(
                    self._dominated_by(code, match.start(), name)
                    for name in result_variables
                ),
                msg=(
                    "催重绘的计数必须落在“请求没成功就到此为止”这道闸门后面："
                    "否则把重绘调用删掉，计数照样在涨，运行时测试就看不出"
                    "产品到底有没有真的催过"
                ),
            )

        # 重绘函数自己的返回值必须**由 API 的返回值决定**。
        #
        # 上面那条只断"计数被某个变量支配"，挡不住"调了 API 但忽略
        # 返回值、无条件 return true"：那种写法下请求根本没成功，
        # 计数照样在涨（上一轮验收实测的漏判 M11b）。
        for name, fn_span in self._repaint_request_functions(code):
            body = code[fn_span[0] : fn_span[1]]
            api_positions = find_calls(
                code, fn_span, "InvalidateRect("
            ) + find_calls(code, fn_span, "RedrawWindow(")
            self.assertTrue(api_positions, msg=f"{name} 必须真的调用重绘 API")
            result_names: set[str] = set()
            for position in api_positions:
                statement = statement_at(code, position)
                assigned = re.search(
                    r"([A-Za-z_][A-Za-z0-9_]*)\s*=\s*[^;]*\b"
                    r"(?:InvalidateRect|RedrawWindow)\s*\(",
                    statement,
                )
                # 直接把 API 的返回值 return 出去，也是"接住了"——而且更直接。
                # 把重绘整段抽成 helper 时，helper 里往往就是这么写的
                # （上一轮验收实测：抽 helper → 守卫假红）。
                returned = re.search(
                    r"\breturn\b[^;]*\b(?:InvalidateRect|RedrawWindow)\s*\(",
                    statement,
                )
                self.assertTrue(
                    assigned or returned,
                    msg=(
                        f"{name} 里重绘 API 的返回值必须被接住："
                        "忽略它就意味着请求失败时也照样算催过了"
                    ),
                )
                if assigned:
                    result_names.add(assigned.group(1))
            # 派生变量也算：布尔量可能由 API 的结果算出来（取反、三目）。
            derived = set(result_names)
            for _ in range(4):
                grew = False
                for match in re.finditer(
                    r"([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^;]+);", body
                ):
                    variable = match.group(1)
                    if variable in derived:
                        continue
                    if any(
                        re.search(r"\b" + re.escape(source) + r"\b", match.group(2))
                        for source in derived
                    ):
                        derived.add(variable)
                        grew = True
                if not grew:
                    break
            # 每一条 return 都要过一遍：
            #  - API 调用**之后**的 return 必须由 API 的返回值决定
            #    （不能无条件 return true：那样请求失败时计数照样在涨）；
            #  - API 调用**之前**的 return 是"提前退出"的闸门，只能是
            #    假值（return false / 0）：提前退出意味着"这次没催成"，
            #    写成 return true 等于把失败也报成成功。
            #
            # 上一轮验收实测的漏判 X1：多一条无条件 `return true` 就能
            # 绕过"至少有一条 return 由 API 决定"这种写法。
            returns = [
                (match.start(), match.group(1))
                for match in re.finditer(r"return\s+([^;]+);", body)
            ]
            self.assertTrue(
                returns,
                msg=f"{name} 必须有返回值（请求成功与否要能告诉调用方）",
            )
            first_api = min(api_positions) - fn_span[0]
            for position, expression in returns:
                derived_hit = any(
                    re.search(r"\b" + re.escape(source) + r"\b", expression)
                    for source in derived
                )
                if not derived_hit and re.search(
                    r"\b(?:InvalidateRect|RedrawWindow)\s*\(", expression
                ):
                    # 直接把 API 的返回值 return 出去：它当然由 API 决定。
                    derived_hit = True
                if not derived_hit and re.fullmatch(
                    r"(?:true|false|TRUE|FALSE|0|1)\b", expression.strip()
                ):
                    # 裸布尔值：看它是被哪个 if 支配的。
                    #
                    # `if (effective != FALSE) { return true; } return false;`
                    # 与 `return effective != FALSE;` 完全等价，但返回表达式
                    # 里没有派生变量——第 14 轮验收实测：这一条会假红。
                    #
                    # 两种形态都要认：
                    # 1. return 就**在**那个 if 的块里；
                    # 2. return **紧跟着**那个 if 的块（也就是它的 else 分支）。
                    absolute = fn_span[0] + position
                    for block_start, block_end, condition in if_block_spans(code):
                        inside = block_start <= absolute < block_end
                        follows = 0 <= absolute - block_end <= 32
                        if not (inside or follows):
                            continue
                        if any(
                            re.search(r"\b" + re.escape(source) + r"\b", condition)
                            for source in derived
                        ):
                            derived_hit = True
                            break
                if position > first_api:
                    self.assertTrue(
                        derived_hit,
                        msg=(
                            f"{name} 里 API 调用之后的返回值必须由 API 的返回值决定，"
                            "不能无条件 return true：那样请求失败时计数照样在涨"
                        ),
                    )
                else:
                    if re.search(
                        r"\b(?:InvalidateRect|RedrawWindow)\s*\(", expression
                    ):
                        # 这条 return **就是** API 的返回值本身
                        # （`return InvalidateRect(...);`），不是"提前退出"。
                        continue
                    self.assertIsNotNone(
                        re.fullmatch(
                            r"(?:false|FALSE|False|0)\b", expression.strip()
                        ),
                        msg=(
                            f"{name} 里 API 调用之前的提前退出只能返回假值"
                            "（return false）：返回真值等于把\"没催成\"报成\"催成了\""
                        ),
                    )

    @staticmethod
    def _repaint_request_functions(code: str) -> list[tuple[str, tuple[int, int]]]:
        """找出所有"会针对桌面窗口发重绘请求"的函数（**直接**调 API 的）。"""
        result: list[tuple[str, tuple[int, int]]] = []
        for name, function_span in functions(code):
            for callee in ("InvalidateRect(", "RedrawWindow("):
                for position in find_calls(code, function_span, callee):
                    arguments = _call_arguments(code, position)
                    if not arguments:
                        continue
                    if not arguments[0].strip().startswith("GetDesktopWindow("):
                        continue
                    result.append((name, function_span))
                    break
                else:
                    continue
                break
        return result

    @classmethod
    def _repaint_result_sources(cls, code: str) -> set[str]:
        """能拿到"这次重绘请求成败"的函数名（直接发请求的 + 转发它们的）。

        转发者也要算：把重绘整段抽进 IssueDesktopInvalidate 之后，
        RepaintDesktopRegion 变成"转发"——它返回的仍然是那次请求的
        成败，而计数正是被它返回的那个值守住的。漏掉转发者的话，
        "抽 helper"这种等价重构会被判成缺陷（上一轮验收实测）。
        """
        names = {name for name, _span in cls._repaint_request_functions(code)}
        changed = True
        while changed:
            changed = False
            for name, function_span in functions(code):
                if name in names:
                    continue
                body = code[function_span[0] : function_span[1]]
                if any(
                    find_calls(code, function_span, existing + "(")
                    for existing in names
                ):
                    names.add(name)
                    changed = True
        return names

    @staticmethod
    def _dominated_by(code: str, position: int, variable: str) -> bool:
        """某个位置是不是被"变量为真"这件事支配着。

        两种写法都算数：

        - 提前 return：``if (!变量) { return; }`` 出现在该位置之前；
        - 条件块：``if (变量) { ... }`` 把该位置包在里面。

        而且闸门必须与被支配的位置在**同一个函数**里：闸门在别的函数里
        不算数。否则"把计数从 helper 搬到调用点"这种改法就绕过去了
        ——helper 里的 ``if (!变量) { return; }`` 还在，但那个 return
        管不到调用点（上一轮验收实测：静态层对这条完全没反应）。
        """
        for condition in enclosing_if_conditions(code, position):
            if variable in condition:
                return True
        holder: tuple[int, int] | None = None
        for _name, fn_span in functions(code):
            if fn_span[0] <= position < fn_span[1]:
                if holder is None or fn_span[0] > holder[0]:
                    holder = fn_span
        pattern = re.compile(r"if\s*\(\s*!\s*" + re.escape(variable) + r"\s*\)")
        for match in pattern.finditer(code):
            if match.start() >= position:
                break
            if holder is not None and not (holder[0] <= match.start() < holder[1]):
                # 闸门在别的函数里：那个 return 管不到这里。
                continue
            open_brace = code.find("{", match.end())
            if open_brace < 0:
                continue
            close = matching_close(code, open_brace)
            if close < position and "return" in code[open_brace:close]:
                return True
        return False

    @staticmethod
    def _desktop_repaint_sites(code: str, callee: str) -> list[int]:
        """列出所有针对桌面窗口的重绘调用的位置。"""
        sites: list[int] = []
        for match in re.finditer(
            rf"{re.escape(callee)}\s*\(\s*GetDesktopWindow\s*\(\s*\)", code
        ):
            sites.append(match.start())
        return sites

    @staticmethod
    def _desktop_repaint_calls(code: str, callee: str) -> list[tuple[int, str]]:
        """列出所有针对桌面窗口的重绘调用：(位置, 实参文本)。

        位置是必需的：要判断"实参到底传给了 Win32 什么"，得知道这个调用
        处在哪个函数里——形参要追到调用点，形参的实参又属于**调用方**
        的作用域。
        """
        results: list[tuple[int, str]] = []
        for match in re.finditer(
            rf"{re.escape(callee)}\s*\(\s*GetDesktopWindow\s*\(\s*\)", code
        ):
            open_paren = code.index("(", match.start())
            depth = 0
            end = -1
            for index in range(open_paren, len(code)):
                if code[index] == "(":
                    depth += 1
                elif code[index] == ")":
                    depth -= 1
                    if depth == 0:
                        end = index
                        break
            if end < 0:
                continue
            results.append((match.start(), code[open_paren + 1 : end]))
        return results

    @classmethod
    def _desktop_repaint_arguments(cls, code: str, callee: str) -> list[str]:
        """列出所有针对桌面窗口的重绘调用，返回它们的实参文本。"""
        return [args for _position, args in cls._desktop_repaint_calls(code, callee)]

    @staticmethod
    def _enclosing_function(
        code: str, position: int
    ) -> tuple[str, tuple[int, int]] | None:
        """某个位置所在的最内层函数（名字 + 体区间）。"""
        best: tuple[str, tuple[int, int]] | None = None
        for name, fn_span in functions(code):
            if fn_span[0] <= position < fn_span[1]:
                if best is None or fn_span[0] > best[1][0]:
                    best = (name, fn_span)
        return best

    @staticmethod
    def _resolve_expression(code: str, expression: str, depth: int = 0) -> str:
        """把表达式里的变量替换成它被赋的值。"""
        if depth > 4:
            return expression
        expression = expression.strip()
        assigned = re.search(
            r"(?:const\s+)?(?:BOOL|bool|int|unsigned|DWORD|LONG|long)\s+"
            + re.escape(expression)
            + r"\s*=\s*([^;]+);",
            code,
        )
        if assigned:
            return SourceGuardTest._resolve_expression(
                code, assigned.group(1), depth + 1
            )
        return expression

    def _effective_arguments(
        self,
        code: str,
        argument: str,
        depth: int = 0,
        fn_span: tuple[int, int] | None = None,
    ) -> list[str]:
        """解开"记录器"式的实参，返回真正交给 Win32 的那些表达式。

        产品把记录器放在实参表达式里（`RepaintTarget(&region)`、
        `RepaintEraseFlag()`），这样"换了实参"和"删掉调用"都会被运行时
        抓住。守卫也得跟着往里看一层，否则就只看到记录器的名字。

        还要能**穿透 helper 的参数**：把重绘调用抽成
        `IssueDesktopInvalidate(rect, erase)` 之后，调用点上的实参是那个
        helper 的形参名，真正有边界的是"调用方传给它的那个实参"。
        只看形参名就判红，等于把"抽 helper"这种等价重构判成缺陷
        （上一轮验收实测：抽 helper → 守卫假红两处）。

        解不开（记录器不透明、调用的是不认识的函数）时返回空串——保守
        地判为不合格，总比放过整块重绘强。
        """
        if depth > 5:
            return [argument]
        argument = argument.strip()

        # 形参：追到调用点实际传了什么进来。
        if fn_span is not None:
            params = parameter_names(self._header_of(code, fn_span))
            if argument in params:
                index = params.index(argument)
                holder_name = _function_name_at(functions(code), fn_span)
                resolved: list[str] = []
                if holder_name:
                    for _cname, caller_span in functions(code):
                        if caller_span == fn_span:
                            continue
                        for position in find_calls(
                            code, caller_span, holder_name + "("
                        ):
                            args = _call_arguments(code, position)
                            if index >= len(args):
                                continue
                            resolved.extend(
                                self._effective_arguments(
                                    code, args[index], depth + 1, caller_span
                                )
                            )
                if resolved:
                    return resolved
                return [argument]

        call = re.match(
            r"^([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)\)$", argument, re.S
        )
        if not call:
            return [self._resolve_expression(code, argument)]
        name = call.group(1)
        inner = call.group(2).strip()
        for fn_name, fn_span in functions(code):
            if fn_name != name:
                continue
            body = code[fn_span[0] : fn_span[1]]
            params = parameter_names(self._header_of(code, fn_span))
            returned = re.search(r"return\s+([^;]+);", body)
            if not returned:
                return [""]
            value = returned.group(1).strip()
            if value in params:
                # 记录器原样返回它的参数：真正交给 Win32 的就是调用点
                # 传进去的那个值（注意 inner 属于**调用方**的作用域，
                # 所以 fn_span 要原样传下去，不能换成记录器自己的）。
                return self._effective_arguments(code, inner, depth + 1, fn_span)
            return [self._resolve_expression(code, value, depth + 1)]
        return [""]

    def test_delay_overlay_test_hooks_stay_out_of_the_product_binary(self) -> None:
        """为测试存在的接口不能编进产品二进制。

        延时覆盖层为了让运行时测试问得出"产品到底做了什么决定"（等了几次、
        等了多久、是不是等满了上限、是不是退回了睡固定时长、催了几次
        重绘、间隔多久），加了一组只读计数器。这些接口只对测试有意义，
        所以必须被 SNAPLITE_DELAY_TEST_HOOKS 挡住——产品目标不定义那个宏，
        这些符号就不该出现在 SnapLite.exe 里。

        **判据说明（上一轮验收实测出来的）**：
        拿产品 exe 的字节去搜符号名是**恒真**的——MSVC 的 Release 版
        exe 里根本不含符号名（函数名只活在 obj/PDB 里，而 PDB 不随产品
        发布）。实测把宏加进产品目标、编出一个**故意泄漏**的产品，
        exe 字节扫描仍然是 0 命中。所以真正有效的门禁必须在源码级：

        1. 头文件里这组接口的**声明**在 #ifdef 块内；
        2. **实现**（在 delay_overlay.cpp 里）同样在 #ifdef 块内——
           只看头文件会漏掉这一半：声明被挡住了、实现没挡住，符号照样
           进产品；
        3. 没有任何源文件无条件 #define 这个宏；
        4. CMake 里只有测试目标定义这个宏，产品目标不定义。

        还要断一件相反的事：**记录器不是测试符号**。`RepaintTarget` /
        `RepaintEraseFlag` 是产品自己的函数（它们在实参表达式里，
        必须在产品构建里一直存在），把它们当"测试钩子"藏进 #ifdef
        会让运行时测试彻底失去鉴别力（计数不再涨，两边对不上）。
        """
        header = strip_comments(self._read("src/delay_overlay.h"))
        self.assertIn("#ifdef SNAPLITE_DELAY_TEST_HOOKS", header)
        # "产品构建里会不会编译到"要按**配对栈**算，不能用"从 #ifdef 找
        # 第一个 #endif"那种切分：
        # - 嵌套的 #ifdef 会让切分配错对（合法写法被判成"钩子跑到了
        #   条件编译块之外"——假红）；
        # - #else 分支在宏未定义时**是会被编译进去的**——钩子实现放在
        #   #else 里，符号照样进产品，而那种切分会把它当成"在块内"放过去
        #   （漏判）。
        header_excluded = excluded_spans(header, set(_TEST_HOOK_MACROS))

        hooks = (
            "SurfaceColour",
            "BorderColour",
            "LastBaselineColours",
            "WaitForOverlayColoursGone",
            "LastWaitCallCount",
            "LastWaitElapsedMs",
            "LastWaitTimedOut",
            "LastWaitUsedFixedSleepFallback",
            "LastDestroyRepaintCount",
            "LastPollRepaintCount",
            "LastRepaintRegion",
            "LastRepaintCallCount",
            "LastRepaintEraseFlagCount",
            "LastRepaintTargetWasNull",
            "LastRepaintErased",
            "ForceRepaintFailure",
            "LastWaitTimeoutMs",
            "LastResidueTolerance",
            "LastStartFailure",
            "LastRepaintNudgeIntervalMs",
            "LastWaitPollRepaintCount",
            "LastPollNudgeGapMs",
            "LastWaitRestIntervalMs",
            "LastWaitRestMs",
            "LastWaitSampleCount",
        )
        for hook in hooks:
            declarations = [
                match.start()
                for match in re.finditer(r"\b" + re.escape(hook) + r"\s*\(", header)
            ]
            self.assertTrue(
                declarations,
                msg=f"测试钩子 {hook} 的声明必须存在",
            )
            for position in declarations:
                self.assertTrue(
                    any(low <= position < high for low, high in header_excluded),
                    msg=f"测试钩子 {hook} 必须留在 SNAPLITE_DELAY_TEST_HOOKS 里",
                )

        # 实现也要在条件编译块内。
        source = strip_comments(self._read("src/delay_overlay.cpp"))
        source_excluded = excluded_spans(source, set(_TEST_HOOK_MACROS))
        source_compiled = compiled_spans(source, set(_TEST_HOOK_MACROS))
        self.assertTrue(
            source_excluded,
            msg="实现文件里必须有 SNAPLITE_DELAY_TEST_HOOKS 条件编译块",
        )
        for hook in hooks:
            hits = find_calls(source, (0, len(source)), f"DelayOverlay::{hook}(")
            self.assertTrue(
                hits,
                msg=f"测试钩子 {hook} 的实现必须存在（实现缺了，测试根本编不过）",
            )
            for position in hits:
                self.assertTrue(
                    any(low <= position < high for low, high in source_excluded),
                    msg=(
                        f"测试钩子 {hook} 的实现必须留在 SNAPLITE_DELAY_TEST_HOOKS 里"
                        "——声明被挡住、实现没挡住，符号照样进产品二进制"
                    ),
                )

        # 记录器是**产品函数**，不能被藏进条件编译块。
        #
        # 记录器**不能按名字认**：把 `RepaintTarget` 改名成
        # `RepaintTargetRecorder` 是合法重构，按名字认就会误判
        # （上一轮验收实测：改名 → 静态假红）。这里改成**按行为**认：
        # 谁在**写** `g_lastRepaint*` 这几个记录位，谁就是记录器。
        # （只看"提到"不行：钩子的 getter 也提到它们，但 getter 本来
        # 就该待在条件编译块里。）
        recorder_globals = (
            "g_lastRepaintCallCount",
            "g_lastRepaintEraseFlagCount",
            "g_lastRepaintErased",
            "g_lastRepaintRegion",
            "g_lastRepaintTargetWasNull",
        )

        def writes(body: str, name: str) -> bool:
            escaped = re.escape(name)
            return bool(
                re.search(r"\+\+\s*" + escaped + r"\b", body)
                or re.search(escaped + r"\s*\+\+", body)
                or re.search(escaped + r"\s*=(?!=)", body)
            )

        recorders = [
            (name, fn_span)
            for name, fn_span in functions(source)
            if any(
                writes(source[fn_span[0] : fn_span[1]], global_name)
                for global_name in recorder_globals
            )
        ]
        self.assertTrue(
            recorders,
            msg="找不到产品自己的记录器函数（它们负责把实参记下来）",
        )
        for name, fn_span in recorders:
            self.assertTrue(
                any(low <= fn_span[0] < high for low, high in source_compiled),
                msg=(
                    f"{name} 是产品函数，必须在产品构建里存在——"
                    "它要一直在实参表达式里，藏进条件编译块运行时测试就失去鉴别力"
                ),
            )

        # 没有任何源文件可以无条件定义这个宏。
        for path in sorted((REPO_ROOT / "src").rglob("*.cpp")) + sorted(
            (REPO_ROOT / "src").rglob("*.h")
        ):
            text = strip_comments(path.read_text(encoding="utf-8"))
            self.assertNotIn(
                "#define SNAPLITE_DELAY_TEST_HOOKS",
                text,
                msg=f"{path.name} 不能无条件定义测试钩子宏",
            )

        cmake = strip_cmake_comments(self._read("CMakeLists.txt"))
        self.assertEqual(
            cmake.count("SNAPLITE_DELAY_TEST_HOOKS"),
            1,
            msg="只有测试目标可以定义测试钩子宏",
        )
        test_block = cmake.split("add_executable(SnapLiteDelayOverlayTests", 1)[1]
        test_block = test_block.split("add_test(NAME delay_overlay", 1)[0]
        self.assertIn(
            "SNAPLITE_DELAY_TEST_HOOKS",
            test_block,
            msg="延时覆盖层测试目标必须定义测试钩子宏",
        )
        product_block = cmake.split("add_executable(SnapLite WIN32", 1)[1]
        product_block = product_block.split("add_executable(SnapLiteInteractionTests", 1)[0]
        self.assertNotIn(
            "SNAPLITE_DELAY_TEST_HOOKS",
            product_block,
            msg="产品目标不能定义测试钩子宏",
        )

    def test_edit_test_hooks_stay_out_of_the_product_binary(self) -> None:
        """为测试存在的开关不能编进产品二进制。

        历史长度的上限覆盖（SNAPLITE_EDIT_TEST_HOOKS）只为运行时测试存在：
        裁剪路径平时要攒够 64MB 或 128 条才触发一次，测试里根本压不到，
        只能靠这个开关把它压到 2 条。产品目标不定义那个宏，这些符号就不该
        出现在 SnapLite.exe 里。
        """
        # 先剥注释：注释里提到钩子的名字不算"钩子出现在条件编译块之外"。
        header = strip_comments(self._read("src/snip_window.h"))
        self.assertIn("#ifdef SNAPLITE_EDIT_TEST_HOOKS", header)
        # 条件编译块**可能不止一个**（BitmapHistory 里一份，SnipWindow 里
        # 再一份给产品自己的入口）。只认第一个的话，第二个块里的符号会被
        # 误判成"跑到了条件编译块之外"（上一轮验收实测的假红）。
        spans: list[tuple[int, int]] = []
        for match in re.finditer(r"#ifdef\s+SNAPLITE_EDIT_TEST_HOOKS", header):
            end = header.find("#endif", match.end())
            if end < 0:
                continue
            spans.append((match.start(), end))
        self.assertTrue(spans, msg="必须有 SNAPLITE_EDIT_TEST_HOOKS 条件编译块")
        self.assertTrue(
            any(
                "SetMaxEntriesOverrideForTest" in header[start:end]
                for start, end in spans
            ),
            msg="历史长度的上限覆盖必须留在 SNAPLITE_EDIT_TEST_HOOKS 里",
        )
        for match in re.finditer(r"SetMaxEntriesOverrideForTest", header):
            self.assertTrue(
                any(start <= match.start() < end for start, end in spans),
                msg=(
                    "历史长度的上限覆盖不能出现在条件编译块之外——"
                    "那会进产品二进制"
                ),
            )

        cmake = strip_cmake_comments(self._read("CMakeLists.txt"))
        self.assertEqual(
            cmake.count("SNAPLITE_EDIT_TEST_HOOKS"),
            1,
            msg="只有测试目标可以定义测试钩子宏",
        )
        test_block = cmake.split("add_executable(SnapLiteInteractionTests", 1)[1]
        test_block = test_block.split("add_executable(SnapLiteDelayOverlayTests", 1)[0]
        self.assertIn(
            "SNAPLITE_EDIT_TEST_HOOKS",
            test_block,
            msg="交互测试目标必须定义测试钩子宏",
        )
        product_block = cmake.split("add_executable(SnapLite WIN32", 1)[1]
        product_block = product_block.split("add_executable(SnapLiteInteractionTests", 1)[0]
        self.assertNotIn(
            "SNAPLITE_EDIT_TEST_HOOKS",
            product_block,
            msg="产品目标不能定义测试钩子宏",
        )

    def test_delay_overlay_repaint_wait_is_verified_and_bounded(self) -> None:
        """等重绘必须"盯着像素"判断，而且必须有上限。

        原来这里是"睡 120ms 去赌 explorer 已经画完"：机器繁忙时那点时间
        经常不够，截图就把覆盖层拍进去了（实测负载高时出现过）。

        这里断的是**关系**，不是"字符串出现过"：
        - 像素计数的结果必须参与比较；
        - 比较的另一端必须是一个**参数**（也就是基线），不能是写死的数字
          ——写死的数字会让"基线"这条约束形同虚设；
        - 超时判断所在的分支里必须有 return，否则删掉上限就变成死循环，
          截图永远不发生；
        - 关键判断不能藏在 if (false) 这种不会执行的分支里。
        """
        code = strip_comments(self._read("src/delay_overlay.cpp"))
        # "看时钟"要认**任意**时钟 API：把 GetTickCount64 换成
        # QueryPerformanceCounter 是完全等价的重构，只认一个名字就会把
        # 这种改写判成缺陷（上一轮验收实测：换时钟 → 静态红 4 条）。
        span = self._wait_implementation(code)
        self.assertIsNotNone(
            span,
            msg="找不到负责等重绘的函数（它必须读像素、看时钟、轮询时歇一下）",
        )
        assert span is not None
        positions = resolve_needle(code, span, "CountOverlayColours(")[1]
        self.assertTrue(positions, msg="必须检查那块区域的像素来判断是否重绘完成")
        count_at = positions[0]
        self.assertFalse(
            in_dead_branch(code, count_at),
            msg="像素判定藏在不会执行的分支里，等于没有判定",
        )

        # 轮询之间必须歇一下，不能空转烧 CPU。
        #
        # 三个坑，一个比一个隐蔽：
        #  1. 只断言"函数里出现过 Sleep("——回退路径里那一次固定等待就能
        #     满足它；
        #  2. 只断言"循环体里有 Sleep"——但循环必须**在那个函数里**。
        #     原来只判了"循环起点在该函数结束之前"，文件前面别的函数里
        #     的循环（带 Sleep）也能顶替（上一轮验收实测的漏判 N03）；
        #  3. 兜底的那次固定等待不能顶替轮询的歇息——它是另一个常量，
        #     语义上是"放弃验证、睡一会儿去赌"，不是"轮询之间歇口气"。
        loops: list[tuple[int, int]] = []
        for start, _end, block in loop_blocks(code):
            if not (span[0] <= start < span[1]):
                continue
            block_start = code.find(block, start)
            if block_start < 0:
                continue
            loops.append((block_start, block_start + len(block)))
        self.assertTrue(loops, msg="等待必须是一个轮询循环，不能一次就判死")
        poll_sleeps: list[int] = []
        for start, end in loops:
            for position in find_calls(code, (start, end), "Sleep("):
                arguments = _call_arguments(code, position)
                if arguments and "kRepaintWaitMs" in arguments[0]:
                    # 兜底的固定等待：不是轮询的歇息。
                    continue
                if position <= count_at:
                    # 歇息必须在**看过像素之后**：先歇再判，等于每次
                    # 迭代都白等一轮。
                    continue
                if in_dead_branch(code, position):
                    # 歇息藏在不会执行的分支里，等于没歇。
                    continue
                poll_sleeps.append(position)
        self.assertTrue(
            poll_sleeps,
            msg=(
                "轮询循环里必须歇一下（不能用兜底的那次固定等待顶替，"
                "也不能歇在像素判定之前），不能空转烧 CPU"
            ),
        )

        # 像素计数的结果变量。
        statement = statement_at(code, count_at)
        assigned = re.search(
            r"([A-Za-z_][A-Za-z0-9_]*)\s*=\s*[^;]*CountOverlayColours\(", statement
        )
        self.assertIsNotNone(assigned, msg="像素计数必须存进一个变量再判断")
        result_name = assigned.group(1)

        # 紧跟着的判断里，计数结果必须参与比较。
        condition = next_condition_after(code, count_at, span[1])
        self.assertIsNotNone(condition, msg="像素计数之后必须有基于它的判断")
        self.assertIn(
            result_name, condition, msg="判断必须用刚数出来的像素数，而不是别的数"
        )
        self.assertRegex(condition, r"<=|>=|<|>", msg="判断必须是比较，不能是恒真")

        # 比较的另一端必须是一个参数（基线），不能是写死的数字。
        #
        # 要在**整个等待实现里**找，而不是只看紧跟着的那一条判断：
        # 把 `if (remaining >= 0 && remaining <= 基线 + 容忍度)` 拆成嵌套的
        # 两层是等价重构，只看第一条会把这种改写判成缺陷
        # （上一轮验收实测：假红）。
        #
        # 但"样本必须与某个参数比较过"这条不能放松：把基线比较换成写死的
        # 数字时，等待实现里就再也找不到这种比较了。
        header = self._header_of(code, span)
        params = parameter_names(header)
        compared_with_parameter = False
        for start, end, cond, _block in if_conditions(code):
            if not (span[0] <= start and end <= span[1]):
                continue
            if result_name not in cond:
                continue
            operands = set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", cond))
            if [name for name in params if name in operands]:
                compared_with_parameter = True
                break
        self.assertTrue(
            compared_with_parameter,
            msg=f"比较的另一端必须是基线参数（可用参数：{params}），不能是写死的数字",
        )

        # 超时上限：判断超时的分支里必须有 return，而且这个判断不能恒假。
        ticks: list[int] = []
        clocks = clock_calling_functions(code)
        for api in _CLOCK_APIS:
            ticks.extend(find_calls(code, span, api))
        for other in clocks:
            ticks.extend(find_calls(code, span, other + "("))
        self.assertTrue(ticks, msg="等待必须有超时上限，不能无限等下去")
        bounded = False
        for start, end, cond, block in if_conditions(code):
            if not any(start <= tick <= end for tick in ticks):
                continue
            if "return" not in block:
                continue
            if dead_condition(cond):
                continue
            bounded = True
        self.assertTrue(bounded, msg="超时分支必须真的返回，否则会无限等下去")

    @classmethod
    def _clock_calling_functions(cls, code: str) -> set[str]:
        """所有"（直接或间接）用到时钟"的函数名。"""
        return clock_calling_functions(code)

    @classmethod
    def _wait_implementation(cls, code: str) -> tuple[int, int] | None:
        """找出"等桌面重绘"那个实现函数的体区间。

        它是唯一一个同时做三件事的函数：读像素、看时钟、轮询时歇一下。

        "看时钟"要认**任意**时钟 API（直接或包一层）：
        把 `GetTickCount64()` 换成 `QueryPerformanceCounter()` 是完全等价
        的重构，只认一个名字就会把这种改写判成缺陷
        （上一轮验收实测：换时钟 → 静态红 4 条）。
        """
        clock_functions = cls._clock_calling_functions(code)
        for _name, function_span in functions(code):
            if (
                find_calls(code, function_span, "CountOverlayColours(")
                and (
                    any(
                        find_calls(code, function_span, api) for api in _CLOCK_APIS
                    )
                    or any(
                        find_calls(code, function_span, other + "(")
                        for other in clock_functions
                    )
                )
                and find_calls(code, function_span, "Sleep(")
            ):
                return function_span
        return None

    @staticmethod
    def _condition_is_constant(condition: str) -> bool:
        """条件本身是不是写死的常量（恒真 / 恒假）。"""
        text = condition.strip()
        return bool(
            re.match(
                r"^(?:constexpr\s*)?(?:true|TRUE|false|FALSE|0|1)\s*(?:&&|\|\||\)|$)",
                text,
            )
        )

    @staticmethod
    def _condition_has_comparison(code: str, condition: str) -> bool:
        """条件里有没有比较。

        抽成 helper 也算：条件写成 `ShouldNudge(now, last)` 时，真正的
        比较在那个 helper 的函数体里。等价重构不该让守卫变红
        （上一轮验收实测：抽 helper → 守卫假红）。
        """
        if re.search(r"(?:>=|<=|==|!=|<|>)", condition):
            return True
        for match in re.finditer(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(", condition):
            name = match.group(1)
            for fn_name, fn_span in functions(code):
                if fn_name != name:
                    continue
                body = code[fn_span[0] : fn_span[1]]
                if re.search(r"(?:>=|<=|==|!=|<|>)", body):
                    return True
        return False

    @staticmethod
    def _else_block(code: str, block_end: int) -> str:
        """某个 if 块之后的 else 块文本（没有 else 就返回空串）。

        block_end 是**右花括号自己的下标**，所以要从它**后面**开始找
        else——从它自己开始找永远找不到（上一轮验收实测：这一条差一，
        "出路在 else 里"那种写法被误判成缺陷）。
        """
        index = block_end + 1
        while index < len(code) and code[index] in " \t\r\n":
            index += 1
        if not code.startswith("else", index):
            return ""
        index += 4
        while index < len(code) and code[index] in " \t\r\n":
            index += 1
        if index >= len(code) or code[index] != "{":
            return ""
        close = matching_close(code, index)
        return code[index : close + 1]

    @staticmethod
    def _comparisons(condition: str) -> list[tuple[str, str, str]]:
        """把条件里的比较拆成 (左, 运算符, 右)。

        只拆**单个**比较项：`now - last >= 200` 会拆成 ("last", ">=", "200")，
        左边的 `now -` 归到"表达式"里——守卫要判断的是**阈值**，不是整个表达式。
        """
        pattern = re.compile(
            r"([A-Za-z_][A-Za-z0-9_]*|[-+]?\d+)\s*"
            r"(>=|<=|==|!=|>|<)\s*"
            r"([A-Za-z_][A-Za-z0-9_]*|[-+]?\d+)"
        )
        return [(m.group(1), m.group(2), m.group(3)) for m in pattern.finditer(condition)]

    @staticmethod
    def _constant_value(code: str, expression: str) -> int | None:
        """取一个常量表达式的值；认不出来就返回 None。

        认得出的两种：`constexpr DWORD kX = 200;` 这类声明，
        以及字面量 `200`。
        """
        text = expression.strip()
        if re.fullmatch(r"[-+]?\d+", text):
            return int(text)
        match = re.search(
            r"constexpr\s+\w+\s+" + re.escape(text) + r"\s*=\s*([-+]?\d+)\s*;",
            code,
        )
        if match:
            return int(match.group(1))
        return None

    @classmethod
    def _time_variables(cls, code: str) -> set[str]:
        """所有"从时钟读来的"变量名（`x = GetTickCount64()`）。

        要算**传递闭包**：`lastRepaintAt` 本身不是从时钟读的，它是
        `lastRepaintAt = now;` 这样从时钟变量派生的。只认直接赋值的话，
        `if (now - lastRepaintAt >= 0)` 这种恒真写法就会被漏过去
        （上一轮验收实测：守卫绿）。

        时钟可以包一层（`x = MonotonicNowMs();`），所以也要认
        "间接用到时钟"的函数。
        """
        names: set[str] = set()
        clock_pattern = "|".join(re.escape(api) for api in _CLOCK_APIS)
        clock_functions = cls._clock_calling_functions(code)
        function_pattern = (
            "|".join(re.escape(name) + r"\s*\(" for name in clock_functions)
            if clock_functions
            else r"(?!)"
        )
        for match in re.finditer(
            r"([A-Za-z_][A-Za-z0-9_]*)\s*=\s*[^;]*(?:"
            + clock_pattern
            + "|"
            + function_pattern
            + ")",
            code,
        ):
            names.add(match.group(1))
        changed = True
        while changed:
            changed = False
            for match in re.finditer(
                r"([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\s*;",
                code,
            ):
                variable = match.group(1)
                source = match.group(2)
                if variable in names or source not in names:
                    continue
                names.add(variable)
                changed = True
        return names

    def _static_comparison(
        self, code: str, left: str, op: str, right: str
    ) -> bool | None:
        """两个操作数都是已知常量时，直接算出这个比较的真假。"""
        left_value = self._constant_value(code, left)
        right_value = self._constant_value(code, right)
        if left_value is None or right_value is None:
            return None
        if op == ">=":
            return left_value >= right_value
        if op == "<=":
            return left_value <= right_value
        if op == "==":
            return left_value == right_value
        if op == "!=":
            return left_value != right_value
        if op == ">":
            return left_value > right_value
        return left_value < right_value

    def _condition_is_dead(self, code: str, condition: str) -> bool:
        """这个条件是不是永远不成立（分支是死代码）。

        认得出的形态：
        - 条件本身是字面量 `false`；
        - `&&` 的某一个合取项恒假（比如 `kRepaintTimeoutMs < 0`——那个
          常量是 800，永远不成立，整条 `&&` 就永远不成立）。
        """
        text = condition.strip()
        if re.fullmatch(r"(?:constexpr\s*)?(?:false|FALSE|0)", text):
            return True
        for conjunct in re.split(r"&&", text):
            piece = conjunct.strip()
            if re.fullmatch(r"(?:constexpr\s*)?(?:false|FALSE|0)", piece):
                return True
            for left, op, right in self._comparisons(piece):
                verdict = self._static_comparison(code, left, op, right)
                if verdict is False:
                    return True
        return False

    def _sample_availability(
        self, code: str, condition: str, sample_names: set[str]
    ) -> str:
        """这个条件在说什么时候"样本拿不到"。

        返回 "unavailable" / "available" / ""（认不出来）。

        要认得出**操作数对调**：`0 > remaining` 与 `remaining < 0` 完全等价
        （上一轮验收实测：只认一种方向 → 无害改写被判成缺陷）。
        """
        for left, op, right in self._comparisons(condition):
            for sample_side, other_side, sample_is_left in (
                (left, right, True),
                (right, left, False),
            ):
                if not any(name in sample_side for name in sample_names):
                    continue
                value = self._constant_value(code, other_side)
                if value is None:
                    continue
                if sample_is_left:
                    if op == "<" and value <= 0:
                        return "unavailable"
                    if op == "<=" and value <= -1:
                        return "unavailable"
                    if op == ">=" and value >= 0:
                        return "available"
                    if op == ">" and value >= -1:
                        return "available"
                else:
                    if op == ">" and value <= 0:
                        return "unavailable"
                    if op == ">=" and value <= -1:
                        return "unavailable"
                    if op == "<" and value >= 0:
                        return "available"
                    if op == "<=" and value >= -1:
                        return "available"
        return ""

    def _rate_limit_threshold_is_positive(self, code: str, condition: str) -> bool:
        """限频比较里的阈值必须 > 0。

        `if (now - last >= 0)` 这种写法看着有比较、其实恒真——等于把限频
        彻底去掉（上一轮验收实测：守卫绿）。只认**跟时钟变量比**的那一项，
        别把条件里别的比较（`&& x > 0` 之类）也当阈值。
        阈值认不出来时放行：交给运行时断言兜。
        """
        time_variables = self._time_variables(code)
        for left, op, right in self._comparisons(condition):
            if any(name in left for name in time_variables):
                threshold = right
            elif any(name in right for name in time_variables):
                threshold = left
            else:
                continue
            value = self._constant_value(code, threshold)
            if value is None:
                continue
            if value <= 0:
                return False
        return True

    def test_delay_overlay_rate_limits_the_repaint_nudges(self) -> None:
        """催桌面重绘必须限频。

        这是用户实测过的那个问题的直接修复点："你在桌面一测试居然还能
        影响到我桌面二"——桌面窗口横跨所有显示器，一串密集的重绘请求
        下去，别的屏也跟着重画。

        上一轮验收实测：这条限频**一条断言都没有**——去掉限频之后运行时
        68 项、静态 75 项全绿，800ms 窗口内的催重绘从 4 次变成 18 次。

        这里断的是**结构**：轮询期催重绘的调用点必须落在一个**有内容
        的**条件里——不能是 `if (true)`（恒真，等于不限频），也不能是
        `if (false)`（恒假，等于不催）；而且那个条件里必须有比较，
        因为限频的本质就是"跟上次比一下，够间隔了才催"。

        抽 helper 不该让这条变红：条件写成 `ShouldNudge(now, last)` 也算，
        判定会去看那个 helper 的函数体里有没有比较。
        """
        code = strip_comments(self._read("src/delay_overlay.cpp"))
        wait_span = self._wait_implementation(code)
        self.assertIsNotNone(wait_span, msg="找不到等重绘的实现")
        assert wait_span is not None

        # 只看"从等待实现里能到达"的函数：销毁覆盖层之后催的那一次
        # 不在轮询期，不受这条限频约束。
        reachable = [wait_span] + [
            fn_span for _name, fn_span in self._called_functions(code, wait_span)
        ]
        gated = False
        saw_nudge = False
        for holder in reachable:
            for site in find_calls(code, holder, "RequestDesktopRepaint("):
                saw_nudge = True
                conditions = enclosing_if_conditions(code, site)
                if not conditions:
                    continue
                condition = conditions[0]
                if self._condition_is_constant(condition):
                    # 恒真 = 不限频；恒假 = 根本不催。两种都是缺陷。
                    continue
                if not self._condition_has_comparison(code, condition):
                    continue
                if not self._rate_limit_threshold_is_positive(code, condition):
                    # 阈值是 0：`if (now - last >= 0)` 恒真，等于不限频
                    # （上一轮验收实测：守卫绿）。
                    continue
                gated = True
                break
            if gated:
                break
        self.assertTrue(
            saw_nudge,
            msg="等待期间必须催桌面重绘（覆盖层那块像素要靠它退回来）",
        )
        self.assertTrue(
            gated,
            msg=(
                "轮询期催重绘必须受一个'距上次催至少间隔多久'的条件约束，"
                "而且那个条件不能是写死的常量（if (true) 等于不限频，"
                "if (false) 等于不催）"
            ),
        )

    def test_delay_overlay_falls_back_when_the_picture_cannot_be_captured(
        self,
    ) -> None:
        """抓不到画面时必须退回固定等待，不能空转到超时。

        这一条正是"不要为了赌而把界面冻住"的实现：抓屏失败时，随后那次
        抓屏一样拿不到东西，干等 800ms 只是白把界面冻住。

        上一轮验收实测：把这条兜底删掉（`if (remaining < 0)` 改成
        `if (false)`），运行时 68 项、静态 75 项全绿——**一条断言都没有**。

        运行时没法让抓屏失败，所以这里只能靠源码级断言：等待实现里
        必须有一条"样本拿不到"的分支，而且那条分支必须通向 return。
        """
        code = strip_comments(self._read("src/delay_overlay.cpp"))
        wait_span = self._wait_implementation(code)
        self.assertIsNotNone(wait_span, msg="找不到等重绘的实现")
        assert wait_span is not None

        # 样本变量：那一轮取样（CountOverlayColours）的结果。
        sample_names: set[str] = set()
        for match in re.finditer(
            r"([A-Za-z_][A-Za-z0-9_]*)\s*=\s*[^;]*CountOverlayColours\(",
            code[wait_span[0] : wait_span[1]],
        ):
            sample_names.add(match.group(1))
        self.assertTrue(sample_names, msg="像素计数必须存进一个变量再判断")

        found = False
        for block_start, block_end, condition in if_block_spans(code):
            if not (wait_span[0] <= block_start and block_end <= wait_span[1]):
                continue
            if not any(name in condition for name in sample_names):
                continue
            if self._condition_is_dead(code, condition):
                # 条件永远不成立：这条分支根本不会执行，兜底等于没有
                # （上一轮验收实测：钉成恒假 → 守卫绿）。
                continue
            block = code[block_start:block_end]
            availability = self._sample_availability(
                code, condition, sample_names
            )
            # "样本拿不到"的方向：与负数比较（含操作数对调）。
            if availability == "unavailable":
                if "return" in block:
                    found = True
                    break
                continue
            # 另一种写法：条件是"样本可用"，出路在 else 里。
            if availability == "available":
                else_block = self._else_block(code, block_end)
                if else_block and "return" in else_block:
                    found = True
                    break
        self.assertTrue(
            found,
            msg=(
                "抓不到画面时必须有一条通向 return 的兜底分支"
                "（退回固定等待），不能空转到超时——否则界面会被冻住 800ms"
            ),
        )

    def test_delay_overlay_captures_a_baseline_before_showing(self) -> None:
        """覆盖层出现之前必须先记下那块区域的基线。

        桌面壁纸里可能正好有和覆盖层一样的颜色。没有基线，销毁后
        "这块区域里还有同色像素"就分不清是覆盖层没退干净还是本来就有，
        等待逻辑要么空转要么提前放行。
        """
        code = strip_comments(self._read("src/delay_overlay.cpp"))
        span = body_span(code, "bool DelayOverlay::Start(")
        creates = find_calls(code, span, "CreateWindowExW(")
        self.assertTrue(creates, msg="必须创建覆盖层窗口")

        # 取样点必须在 Start 本体里，或者在 Start **直接**调用的 helper 里。
        #
        # 不能顺着调用链一路找下去：Start 会调 Cancel()，Cancel 一路走到
        # WaitForOverlayColoursGoneImpl，那儿也调 CountOverlayColours——
        # 那是**拆覆盖层**时用来判断残留的调用，不是基线取样。顺着链找，
        # 真基线被删掉之后守卫照样绿（上一轮验收实测的漏判 N02）。
        teardown = self._transitive_callees(code, "Cancel")
        samples = list(find_calls(code, span, "CountOverlayColours("))
        if not samples:
            # 容许"把取样抽成一个 helper"这种等价重构。
            for name, fn_span in functions(code):
                if name in teardown or fn_span == span:
                    continue
                if not find_calls(code, fn_span, "CountOverlayColours("):
                    continue
                samples.extend(find_calls(code, span, name + "("))
        self.assertTrue(samples, msg="启动时必须先取这块区域的基线")
        for position in samples:
            self.assertFalse(
                in_dead_branch(code, position),
                msg="基线取样藏在不会执行的分支里，等于没有取基线",
            )
        # 基线必须在覆盖层**显示**之前取。
        #
        # 不是"在窗口创建之前"：WS_POPUP 窗口创建出来时还没显示，屏幕上
        # 还是原来的内容——那时候取样同样是对的（上一轮验收实测：这种
        # 挪动是无害的，运行时也断不出来，静态却判了红）。真正会坏掉的
        # 是"显示之后才取"，那时候采到的已经是覆盖层自己的颜色。
        shows = find_calls(code, span, "ShowWindow(")
        self.assertTrue(shows, msg="覆盖层必须被显示出来")
        self.assertLess(
            min(samples),
            shows[0],
            msg="基线必须在覆盖层显示之前取，否则取到的已经是覆盖层自己的颜色",
        )
        # 覆盖层不能一建出来就可见：那样"创建"与"显示"是同一刻，
        # 上面那条比较就失去了意义。
        create_arguments = _call_arguments(code, creates[0])
        self.assertGreaterEqual(
            len(create_arguments), 4, msg="CreateWindowExW 的实参不完整"
        )
        self.assertNotIn(
            "WS_VISIBLE",
            create_arguments[3],
            msg="覆盖层不能带着 WS_VISIBLE 创建——它一建出来就会挡住屏幕",
        )

        # 上报的基线必须**取样之后**才赋值。
        #
        # 只比"取样 vs ShowWindow"的位置是不够的：把取样挪到
        # `g_lastBaselineColours = state->baselineOverlayColours;`
        # **之后**，取样仍然在 ShowWindow 之前（静态看着没事），但报出去
        # 的基线是"没采到"的默认值——运行时红、静态绿
        # （上一轮验收实测）。
        sample_target = ""
        for match in re.finditer(
            r"([A-Za-z_][A-Za-z0-9_]*(?:(?:->|\.)[A-Za-z_][A-Za-z0-9_]*)*)"
            r"\s*=\s*[^;]*CountOverlayColours\(",
            code[span[0] : span[1]],
        ):
            sample_target = match.group(1)
            break
        self.assertTrue(sample_target, msg="取到的基线必须存进一个变量")
        report = re.search(
            r"[A-Za-z_][A-Za-z0-9_]*\s*=\s*" + re.escape(sample_target) + r"\s*;",
            code[span[0] : span[1]],
        )
        self.assertIsNotNone(
            report,
            msg="取到的基线必须报出去（否则运行时读到的永远是默认值）",
        )
        assert report is not None
        self.assertGreater(
            span[0] + report.start(),
            min(samples),
            msg="上报的基线必须在取样之后赋值——否则报出去的是没采到的默认值",
        )

    @staticmethod
    def _transitive_callees(code: str, root: str) -> set[str]:
        """从某个函数出发能到达的所有函数名（不含它自己）。"""
        all_functions = functions(code)
        reached: set[str] = set()
        frontier = [root]
        while frontier:
            current = frontier.pop()
            for name, fn_span in all_functions:
                if name != current:
                    continue
                for other_name, _other_span in all_functions:
                    if other_name == name or other_name in reached:
                        continue
                    if find_calls(code, fn_span, other_name + "("):
                        reached.add(other_name)
                        frontier.append(other_name)
        return reached

    def test_bitmap_copy_does_not_flip_rows(self) -> None:
        """复制位图时不能逐行翻转。

        翻转分支曾经是死代码（`GetObject` 填 BITMAP 时 bmHeight 永远是正的，
        方向信息在那个结构里根本不保留），一旦有人把它"修好"变成可达，就会把
        撤销出来的图上下颠倒——比不做处理更糟。方向本来就交给 BitBlt：
        位图被 SelectObject 进 DC 之后，DC 的 (0,0) 就是它的视觉左上角。
        """
        code = strip_comments(self._read("src/snip_window.h"))
        span = body_span(code, "static HBITMAP CopyBitmap(")
        body = code[span[0] : span[1]]
        self.assertNotIn(
            "topDown",
            body,
            msg="不要按 bmHeight 判断方向：GetObject 填 BITMAP 时那个值永远是正的",
        )
        self.assertNotRegex(
            body,
            r"height\s*-\s*1\s*-\s*row",
            msg="不能逐行翻转：方向交给 BitBlt 即可",
        )

    def test_history_limit_test_hook_targets_the_product_stack(self) -> None:
        """裁剪上限的测试钩子必须作用在**产品真正用的那一个**实例化上。

        BitmapHistory 的测试钩子是**按模板实例化**存的静态量，每一份
        实例化各有一份。测试自己造一个 BitmapHistory<int> 再设上限，
        压不到产品那一份 BitmapHistory<vector<ArrowAnnotation>>——
        于是"裁剪 → 编辑 → 撤销"这条产品路径根本没有运行时覆盖
        （上一轮验收实测：把平行标注栈原样装回产品，259 项运行时断言
        全绿，只有源码文本守卫会红）。

        所以产品必须自己提供一个入口，并且它必须指向产品真正用的
        那个实例化。
        """
        header = strip_comments(self._read("src/snip_window.h"))
        self.assertIn(
            "SetHistoryEntryLimitForTest",
            header,
            msg="产品必须提供一个测试入口来压自己的历史上限",
        )
        # 断言必须落在入口的**函数体**里，不能只查"附近有没有这个实例化
        # 的字样"。第 14 轮验收实测：钩子实际指向 BitmapHistory<int>，
        # 只要在旁边留一行
        #   using ProductStack = BitmapHistory<vector<ArrowAnnotation>>;
        # 就能让邻近式匹配放行——而运行时那几条断言对这一条同样是绿的，
        # 两层同时失守，CI 全绿。
        body = re.search(
            r"SetHistoryEntryLimitForTest\s*\([^)]*\)\s*\{([^}]*)\}", header
        )
        self.assertIsNotNone(
            body,
            msg="测试入口必须是一个能看出它作用在哪个实例化上的函数体",
        )
        compact = re.sub(r"\s+", "", body.group(1))
        self.assertEqual(
            compact.count("BitmapHistory<"),
            1,
            msg="测试入口里只该出现一个实例化：多了就说明旁边有诱饵",
        )
        self.assertIn(
            "BitmapHistory<std::vector<ArrowAnnotation>>",
            compact,
            msg=(
                "测试入口必须指向产品真正用的那个实例化："
                "BitmapHistory<vector<ArrowAnnotation>>"
            ),
        )
        self.assertIn(
            "SetMaxEntriesOverrideForTest",
            compact,
            msg="测试入口必须真的调用上限覆盖",
        )
        # 产品自己的两个栈必须就是这个实例化——入口指向它才有意义。
        whole = re.sub(r"\s+", "", header)
        for member in ("undo_", "redo_"):
            self.assertIn(
                f"BitmapHistory<std::vector<ArrowAnnotation>>{member};",
                whole,
                msg=(
                    f"产品的 {member} 栈必须就是 "
                    "BitmapHistory<vector<ArrowAnnotation>>"
                ),
            )
        # 这个入口本身也必须留在条件编译块里（它只为测试存在）。
        self.assertRegex(
            header,
            r"#ifdef\s+SNAPLITE_EDIT_TEST_HOOKS[\s\S]{0,2000}?"
            r"SetHistoryEntryLimitForTest",
            msg="产品自己的测试入口也必须留在 SNAPLITE_EDIT_TEST_HOOKS 里",
        )

    def test_undo_takes_the_bitmap_before_pushing_the_other_stack(self) -> None:
        """撤销/重做必须先取位图，再压对面的栈。

        顺序反了会出事：take_back() 拿不到位图时（条目只剩 PNG、而那张 PNG
        解不开），旧代码已经把当前状态压进了 redo_/arrowRedo_ 然后直接
        return——位图栈少一条、箭头栈一条没少，两个栈从此对不齐，之后每次
        撤销恢复出来的标注都是错位的（实测表现为"撤销之后箭头还在"）。
        """
        code = strip_comments(self._read("src/snip_window_original.inc"))
        for signature, other_stack in (
            ("void SnipWindow::Undo()", "redo_.push_back("),
            ("void SnipWindow::Redo()", "undo_.push_back("),
        ):
            span = body_span(code, signature)
            takes = find_calls(code, span, "take_back(")
            pushes = find_calls(code, span, other_stack)
            self.assertTrue(takes, msg=f"{signature} 必须从历史里取出一帧")
            self.assertTrue(pushes, msg=f"{signature} 必须把当前状态压进对面的栈")
            self.assertLess(
                takes[0],
                pushes[0],
                msg=f"{signature} 必须先取位图再压栈：取不到位图时两栈会失同步",
            )

    def test_undo_keeps_the_bitmap_and_arrow_stacks_in_step(self) -> None:
        """位图与标注状态必须存在**同一条历史条目**里，不能是两个平行的栈。

        两个平行的栈靠"同时 push、同时 pop"保持配对，迟早会对不齐：
        位图栈有 64MB/128 条的裁剪，裁剪是从**队首**丢老条目的，而标注
        栈不会跟着丢——两栈长度可能仍然相等，配对却整体错开一位
        （实测表现：撤销之后箭头还在，而且越撤越错）。

        所以现在标注状态就存在条目里：裁剪丢掉的是"位图+标注"一整条，
        取回失败丢掉的也是一整条。配对从此是结构性的，不靠两边同步。
        """
        header = strip_comments(self._read("src/snip_window.h"))
        code = strip_comments(self._read("src/snip_window_original.inc"))

        # 标注状态必须存在历史条目里。
        self.assertIn(
            "Companion companion",
            header,
            msg="历史条目里必须带着与位图一一对应的标注状态",
        )
        # 不能有平行的标注栈：它不会跟着位图栈的裁剪走。
        for name in ("arrowUndo_", "arrowRedo_"):
            self.assertNotIn(
                name,
                header,
                msg=(
                    f"不能有与位图栈平行的标注栈（{name}）："
                    "位图栈被裁剪时它不跟着丢，两边配对会整体错开一位"
                ),
            )
            self.assertNotIn(
                name,
                code,
                msg=(
                    f"不能有与位图栈平行的标注栈（{name}）："
                    "位图栈被裁剪时它不跟着丢，两边配对会整体错开一位"
                ),
            )

        for signature in ("void SnipWindow::Undo()", "void SnipWindow::Redo()"):
            span = body_span(code, signature)
            # 取的时候必须一次取全：位图和标注状态从同一次 take_back 出来。
            takes = find_calls(code, span, "take_back(")
            self.assertTrue(takes, msg=f"{signature} 必须从历史里取出一帧")
            arguments = _call_arguments(code, takes[0])
            self.assertTrue(
                arguments,
                msg=f"{signature} 取历史时必须带标注状态的出参",
            )
            self.assertIn(
                "&",
                arguments[0],
                msg=(
                    f"{signature} 取历史时必须把标注状态一起取出来："
                    "两样东西存在同一条条目里，取的时候必须一次取全"
                ),
            )
            # 取不到位图就直接返回：不许继续往下走，更不许压对面的栈。
            failure = re.search(
                r"if\s*\(\s*!\s*\w+\s*\)\s*\{", code[takes[0] : span[1]]
            )
            self.assertTrue(
                failure,
                msg=f"{signature} 取不到位图时必须直接返回，不许继续往下走",
            )
            open_brace = takes[0] + failure.end() - 1
            close = matching_close(code, open_brace)
            block = code[open_brace:close]
            self.assertNotIn(
                "push_back(",
                block,
                msg=(
                    f"{signature} 取不到位图时不能压对面的栈："
                    "那会让两个栈从此对不齐"
                ),
            )

        # 压的时候必须把标注状态一起压进去。
        for signature, stack in (
            ("void SnipWindow::Undo()", "redo_.push_back("),
            ("void SnipWindow::Redo()", "undo_.push_back("),
        ):
            span = body_span(code, signature)
            pushes = find_calls(code, span, stack)
            self.assertTrue(pushes, msg=f"{signature} 必须把当前状态压进对面的栈")
            arguments = _call_arguments(code, pushes[0])
            self.assertGreaterEqual(
                len(arguments),
                2,
                msg=(
                    f"{signature} 压栈时必须把标注状态一起压进去："
                    "两样东西存在同一条条目里，压的时候必须一次压全"
                ),
            )

    def test_background_worker_start_is_exception_safe(self) -> None:
        """起后台压缩线程必须包在 try 里，失败不能让异常穿出去。

        调用方是窗口过程，那条路上**没有** try/catch：异常一路穿出去就是
        std::terminate，整个截图工具当场崩掉。

        置位（workerStarted_）也必须在线程起成功**之后**：先置位的话，
        失败时状态已经不可回滚——后面的 StopWorker() 会拿着空的
        workerState_ 去解引用。
        """
        code = strip_comments(self._read("src/snip_window.h"))
        span = body_span(code, "bool EnsureWorker(")
        body = code[span[0] : span[1]]
        self.assertIn("try", body, msg="起线程必须包在 try 里")
        self.assertIn("catch", body, msg="起线程失败必须被接住，不能让异常穿出去")

        assigns = [m.start() for m in re.finditer(r"workerStarted_\s*=\s*true", body)]
        threads = [m.start() for m in re.finditer(r"std::thread\s*\(", body)]
        self.assertTrue(assigns, msg="起线程成功后必须记下“后台已经起来了”")
        self.assertTrue(threads, msg="必须真的起一个后台线程")
        self.assertLess(
            threads[0],
            assigns[0],
            msg="必须先起线程成功，再置位：先置位的话失败时状态不可回滚",
        )

    def test_history_png_is_verified_before_it_is_installed(self) -> None:
        """后台压出来的 PNG 必须先解一遍再交出去。

        PNG 解不开时，失败会推迟到**用户按 Ctrl+Z 那一刻**才出现：撤销拿
        不到位图，位图栈与箭头栈当场失同步。验证放在后台线程做，不占用
        UI 线程。
        """
        code = strip_comments(self._read("src/snip_window.h"))
        span = body_span(code, "static void CompressLoop(")
        decodes = find_calls(code, span, "DecodePng(")
        installs = find_calls(code, span, "result.png = std::move(png)")
        self.assertTrue(decodes, msg="后台压出来的 PNG 必须先解一遍验证")
        self.assertTrue(installs, msg="必须把 PNG 装进结果里")
        self.assertLess(
            decodes[0],
            installs[0],
            msg="必须先验证再装回去，否则失败会推迟到用户撤销那一刻",
        )

    def test_delay_overlay_is_removed_before_capture(self) -> None:
        """倒计时归零时必须先销毁覆盖层、等桌面重绘，再通知调用方截图。

        顺序反了的话，截图会在覆盖层还留在屏幕上时抓取，截出来的图顶部
        中央会多一个倒计时数字框。
        """
        code = strip_comments(self._read("src/delay_overlay.cpp"))
        span = body_span(code, "void FinishOverlay(HWND hwnd, bool notify)")
        destroys = find_calls(code, span, "DestroyOverlayAndWaitForRepaint(hwnd,")
        posts = find_calls(code, span, "PostMessageW(notifyWindow")
        self.assertTrue(destroys, msg="必须先销毁覆盖层")
        self.assertTrue(posts, msg="必须通知调用方去截图")
        self.assertLess(destroys[0], posts[0], msg="必须先销毁覆盖层再通知截图")

    def test_normal_capture_cancels_the_delay_overlay(self) -> None:
        """延时倒计时期间改走普通截图，必须先取消倒计时。

        倒计时覆盖层不是分层窗口，屏幕抓取（BitBlt + CAPTUREBLT）一定会
        把它拍进去。不取消的话，这张截图顶部中央会多一个倒计时数字框。

        这里要断的是**顺序**和**可达性**：取消必须发生在抓屏之前，也
        必须无条件发生——藏进 if (false) 或某个分支里的取消等于没有取消。
        只断言"函数体里出现过 Cancel"的话，把它挪到抓屏之后守卫照样通过，
        缺陷会原样复现。
        """
        code = strip_comments(self._read("src/app.cpp"))
        depths = brace_depths(code)
        for signature, capture in (
            ("void App::StartSnip()", "SnipWindow::Start("),
            ("void App::CaptureFullscreen()", "CaptureVirtualScreen()"),
        ):
            span = body_span(code, signature)
            cancels = resolve_calls(code, span, "DelayOverlay::Cancel()")
            self.assertTrue(
                cancels,
                msg=f"{signature} 没有取消延时倒计时",
            )
            cancel = cancels[0]
            self.assertFalse(
                in_dead_branch(code, cancel),
                msg=f"{signature} 的取消倒计时落在不会执行的分支里",
            )
            self.assertEqual(
                depths[cancel] - depths[span[0]],
                1,
                msg=f"{signature} 的取消倒计时必须无条件执行，不能藏在分支里",
            )
            captures = resolve_calls(code, span, capture)
            self.assertTrue(captures, msg=f"{signature} 没有抓屏调用")
            self.assertLess(
                cancel,
                captures[0],
                msg=f"{signature} 必须先取消倒计时再抓屏",
            )

    def test_delayed_capture_is_refused_while_a_snip_window_is_open(self) -> None:
        """已有截图窗口时不能启动延时倒计时。

        截图窗口是独占的：到点时新截图会被"已有窗口就返回"的早退吃掉，
        用户对着空气等了半天什么也没发生。这里必须明确拒绝并给出提示。

        四件事都要断：**检查存在**、**用的类名与真源一致**、**拒绝发生在
        启动倒计时之前**、**拒绝真的中止了流程**。只断"函数体里出现过
        ShowNotice"的话，把整个拒绝块挪到 DelayOverlay::Start 之后、或把
        return 删掉，守卫照样通过，缺陷会原样复现。
        """
        code = strip_comments(self._read("src/app.cpp"))
        class_name = self._snip_window_class_name()
        span = body_span(code, "void App::StartSnipDelayed(int seconds)")
        sites, positions = resolve_needle(
            code, span, f'FindWindowW(L"{class_name}"'
        )
        self.assertTrue(
            sites,
            msg=f"延时截图必须检查是否已有截图窗口（类名 {class_name}）",
        )
        starts = resolve_calls(code, span, "DelayOverlay::Start(")
        self.assertTrue(starts, msg="延时截图必须启动倒计时覆盖层")
        self.assertLess(
            sites[0],
            starts[0],
            msg="必须在启动倒计时之前拒绝，否则倒计时照常开始",
        )
        # 拒绝块：检查点所在的那个 if 块。检查点被抽进 helper 时，块在
        # helper 体内——按"调用点后面那一块"看会看错地方。
        block = block_after(code, positions[0])
        self.assertTrue(
            re.search(
                r"(?i)\b\w*(notice|notify|warn|alert|message|msgbox|toast)\w*\s*\(",
                block,
            ),
            msg="已有截图窗口时必须明确提示，而不是静默失败",
        )
        self.assertRegex(
            block,
            r"\breturn\b",
            msg="拒绝之后必须中止流程，否则倒计时照样会启动",
        )

    def _snip_window_class_name(self) -> str:
        """从真源（生成管线的模板）里取出截图窗口注册的类名。

        守卫不能自己写死类名：真源改了而调用点没改，守卫会拿着同一个
        旧字面量一起去"通过"，缺陷完全隐形。这里改成从真源派生，
        真源漂移时守卫会立刻报出来。
        """
        template = self._read("src/snip_window_original.inc")
        match = re.search(r'kSnipClass\[\]\s*=\s*L"([^"]+)"', template)
        self.assertIsNotNone(match, msg="真源里找不到截图窗口的类名定义")
        return match.group(1)

    # --- 第二实例不得清理主实例的临时文件 ----------------------------

    def test_only_primary_instance_cleans_ocr_temp_files(self) -> None:
        """第二实例退出时不能清理 OCR 临时文件。

        那些文件是主实例正在识别用的，删掉会让主实例那次识别直接失败。
        """
        code = strip_comments(self._read("src/app.cpp"))
        span = body_span(code, "void App::Shutdown()")
        cleanups = resolve_calls(code, span, "CleanupLeftoverOcrImages()")
        self.assertTrue(cleanups, msg="退出时必须清理遗留的 OCR 临时文件")
        conditions = enclosing_if_conditions(code, cleanups[0])
        self.assertTrue(conditions, msg="清理必须被某个条件守住")
        innermost = conditions[0].strip()
        self.assertIn(
            "primaryInstance_",
            innermost,
            msg="清理必须被 primaryInstance_ 守住",
        )
        self.assertNotIn(
            "!primaryInstance_",
            innermost,
            msg="条件极性不能反：否则第二实例会清掉主实例正在用的文件",
        )

    # --- 合成图缓存键必须覆盖所有写像素路径 ----------------------------

    def test_composite_cache_key_covers_every_pixel_writer(self) -> None:
        """合成图缓存的键必须跟着每一次像素/标注变化走。

        画笔与马赛克是逐段写进 capture_ 的，橡皮会写回原始像素并移除几何
        标注，拖动取消会把标注还原——这些路径都不走 BeginEdit，必须各自
        记一次内容版本，否则"半成品"会被缓存下来并长期显示。
        """
        inc = self._read("src/snip_window_original.inc")
        for signature in (
            "void SnipWindow::DrawPenSegment(POINT from, POINT to)",
            "void SnipWindow::ApplyMosaic(POINT point)",
        ):
            self.assertIn(
                "++contentRevision_;",
                self._body(inc, signature),
                msg=f"{signature} 改了像素却没记内容版本",
            )

        cpp = self._read("src/snip_window.cpp")
        for signature in (
            "void SnipWindow::FinishArrowDrag(bool cancel)",
            "void SnipWindow::EraseArrows(POINT from, POINT to)",
        ):
            self.assertIn(
                "++contentRevision_;",
                self._body(cpp, signature),
                msg=f"{signature} 改了内容却没记内容版本",
            )

        overlay = self._read("src/snip_text_overlay.cpp")
        self.assertIn(
            "++contentRevision_;",
            self._body(overlay, "void SnipWindow::UiBakeTextOverlays"),
            msg="文字烘焙改了 capture_ 却没记内容版本",
        )

        patch = self._read("scripts/apply_eraser_history_patch.py")
        # 两条一起看：总数守住"少了一处"，位置守住"挪错了地方"。
        self.assertGreaterEqual(
            patch.count("++contentRevision_;"),
            2,
            msg="橡皮注入的两处像素写入都要记内容版本",
        )
        # 橡皮的两处注入必须各自带上版本号。只数次数不够——位置错了照样漏。
        # 搜索窗口截到下一个 replace_once( 调用为止，避免一处漏了却蹭到另一处的。
        for anchor in (
            "EraseArrows(drawCurrent_, point);",
            "EraseArrows(drawStart_, drawStart_);",
        ):
            index = patch.find(anchor)
            self.assertNotEqual(index, -1, msg=f"橡皮注入锚点不见了：{anchor}")
            boundary = patch.find("replace_once(", index + len(anchor))
            window = patch[index : boundary if boundary != -1 else len(patch)]
            self.assertIn(
                "++contentRevision_;",
                window,
                msg=f"这处橡皮写入没有记内容版本：{anchor}",
            )

    def test_composite_cache_is_not_reused_during_interaction(self) -> None:
        """绘制过程中的像素是半成品，绘制期间一律不复用缓存。"""
        text = self._read("src/snip_window.cpp")
        body = self._body(text, "HBITMAP SnipWindow::UiCaptureBitmap() const")
        self.assertIn("interactionInProgress", body)
        self.assertIn("!interactionInProgress &&", body)

    def test_paint_recomposes_only_the_invalid_region(self) -> None:
        """局部失效时只合成并复制 rcPaint，避免每次都搬动整张桌面图。"""
        code = self._read("src/snip_window_original.inc")
        paint = "".join(
            self._body(code, "void SnipWindow::Paint()").split()
        )
        self.assertIn("constRECTdirty=ps.rcPaint;", paint)
        self.assertIn(
            "SaveDC(frameDc)",
            paint,
            msg="局部裁剪必须在保存的 DC 状态中执行",
        )
        self.assertIn(
            "IntersectClipRect(frameDc,dirty.left,dirty.top,dirty.right,dirty.bottom)",
            paint,
            msg="离屏合成必须限制在无效区域",
        )
        self.assertIn("RestoreDC(frameDc,savedFrameState)", paint)
        self.assertIn(
            "BitBlt(dc,dirty.left,dirty.top,dirtyWidth,dirtyHeight,frameDc,dirty.left,dirty.top,SRCCOPY)",
            paint,
            msg="回屏复制必须限于 rcPaint",
        )

    # --- 异步 OCR 必须能区分窗口实例 ----------------------------------

    def test_ocr_request_carries_window_instance_id(self) -> None:
        """HWND 会被系统回收复用，只比 HWND 会把上一次的识别结果挂到新窗口。

        OCR 是异步的，回来时原窗口可能早已销毁，而新截图窗口完全可能拿到
        同一个句柄。请求里必须带窗口实例序号，回调时据此校验。
        """
        header = self._read("src/ocr.h")
        self.assertIn("instanceId", header)
        request = self._body(header, "struct OcrRequest {")
        self.assertIn("instanceId", request)

        snip = self._read("src/snip_window.h")
        self.assertIn("kSnipInstanceIdProp", snip)
        self.assertIn("ReadSnipInstanceId", snip)

        action = self._read("scripts/add_ocr_action.py")
        self.assertIn("ReadSnipInstanceId", action)
        self.assertIn("g_ocrInFlightInstanceId", action)
        self.assertIn("instanceId", action)

    # --- 剪贴板重试必须统一入口 --------------------------------------

    def test_text_copy_paths_use_retrying_clipboard_open(self) -> None:
        """取色、OCR 面板复制、贴图这些路径曾一次失败就放弃。

        剪贴板被别的进程短暂占用是常态，一次失败就返回会让用户看到
        "点了复制没反应"。
        """
        capture = self._read("src/capture.cpp")
        body = self._body(capture, "bool CopyTextToClipboard(HWND owner, const std::wstring& text)")
        self.assertIn("OpenClipboardWithRetry(owner)", body)

        ocr = self._read("src/ocr_v2.cpp")
        body = self._body(ocr, "bool CopyUnicodeTextToClipboard(HWND owner, const std::wstring& text)")
        self.assertIn("OpenClipboardWithRetry(owner)", body)

        pin = self._read("src/pin_window.cpp")
        body = self._body(pin, "PinClipboardResult PinWindow::CreateFromClipboard(HINSTANCE instance)")
        self.assertIn("OpenClipboardWithRetry(nullptr)", body)
        # 不允许再留一份自己的重试实现，否则"统一入口"只是说法。
        self.assertNotIn("bool OpenClipboardWithRetry(", pin)

    # --- 构建必须编译生成副本，不能直接编译仓库里的模板 ---------------

    def test_cmake_compiles_generated_copies_not_repository_templates(self) -> None:
        """编译的必须是生成副本，不是仓库里的模板。

        src/snip_window.cpp、src/pin_window.cpp、src/ocr_result_panel_v2.cpp、
        src/editor_toolbar.cpp 都是模板：构建期脚本会复制并改写它们，编译的
        是 build/ 下的产物。仓库里那份与产物并不一致（工具索引、主题常量
        都对不上），一旦 CMake 改成直接编 src/，橡皮/马赛克/文字会整体
        错位一格，而编译期一声不吭。
        """
        code = strip_cmake_comments(self._read("CMakeLists.txt"))
        start = code.index("add_executable(SnapLite ")
        end = code.index(")", start)
        target_sources = code[start:end]
        values = cmake_variables(code)
        for variable, template in (
            ("SNAPLITE_SNIP_SOURCE", "src/snip_window.cpp"),
            ("SNAPLITE_PIN_SOURCE", "src/pin_window.cpp"),
            ("SNAPLITE_OCR_PANEL_SOURCE", "src/ocr_result_panel_v2.cpp"),
            ("SNAPLITE_EDITOR_SOURCE", "src/editor_toolbar.cpp"),
        ):
            self.assertIn(
                variable,
                target_sources,
                msg=f"SnapLite 目标必须用生成副本 {variable}",
            )
            # 光看"变量名出现过"不够：把变量改指回仓库模板，守卫照样通过。
            # 必须把取值展开，确认它真的落在构建目录下。
            resolved = resolve_cmake(values.get(variable, ""), values)
            self.assertIn(
                "CMAKE_CURRENT_BINARY_DIR",
                resolved,
                msg=f"{variable} 必须指向构建目录下的生成副本，不能是仓库模板 {template}",
            )

    def test_chinese_ninja_build_sets_a_valid_msvc_include_prefix(self) -> None:
        """中文 locale 下修正 MSVC 头文件前缀，保证 Ninja 跟踪增量依赖。"""
        cmake = strip_cmake_comments(self._read("CMakeLists.txt"))
        self.assertIn('CMAKE_GENERATOR MATCHES "^Ninja"', cmake)
        self.assertIn("SNAPLITE_MSVC_SHOWINCLUDES_PREFIX", cmake)
        self.assertIn("set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX", cmake)
        self.assertIn("set(CMAKE_CL_SHOWINCLUDES_PREFIX", cmake)

        build_script = self._read("scripts/build_local.ps1")
        self.assertIn("$PSCulture -eq 'zh-CN'", build_script)
        self.assertIn(
            "-DSNAPLITE_MSVC_SHOWINCLUDES_PREFIX=注意: 包含文件:",
            build_script,
        )

    # --- 退出时必须收掉倒计时覆盖层 -----------------------------------

    def test_shutdown_cancels_the_delay_overlay(self) -> None:
        """退出时倒计时覆盖层必须被收掉。

        覆盖层是顶层窗口，不销毁的话会在进程真正结束前留在屏幕上闪一下。
        """
        code = strip_comments(self._read("src/app.cpp"))
        depths = brace_depths(code)
        span = body_span(code, "void App::Shutdown()")
        cancels = resolve_calls(code, span, "DelayOverlay::Cancel()")
        self.assertTrue(cancels, msg="退出时必须取消延时倒计时覆盖层")
        cancel = cancels[0]
        self.assertFalse(
            in_dead_branch(code, cancel),
            msg="退出时的取消落在了不会执行的分支里",
        )
        self.assertEqual(
            depths[cancel] - depths[span[0]],
            1,
            msg="退出时的取消必须是无条件执行的直接语句，不能藏在 if 里",
        )
        # 必须早于资源拆除：覆盖层要在应用收摊之前就没掉。
        for teardown in (
            "CleanupLeftoverOcrImages(",
            "Shell_NotifyIconW(",
            "Gdiplus::GdiplusShutdown(",
        ):
            for position in find_calls(code, span, teardown):
                self.assertLess(
                    cancel,
                    position,
                    msg=f"取消覆盖层必须早于 {teardown}",
                )

    # --- DPI 感知失败必须明确告知用户 ---------------------------------

    def test_dpi_awareness_failure_is_reported(self) -> None:
        """DPI 感知失败不能静默。

        失败后进程会被系统虚拟化，选区、命中半径、贴图尺寸整体错位，
        而且完全静默——用户只觉得"鼠标点不准"，没有任何线索指向 DPI 感知。
        """
        code = strip_comments(self._read("src/app.cpp"))
        whole = (0, len(code))
        pmv2 = find_calls(
            code,
            whole,
            "SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)",
        )
        per_monitor = find_calls(
            code,
            whole,
            "SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)",
        )
        legacy = find_calls(code, whole, "SetProcessDPIAware()")
        self.assertTrue(pmv2, "必须尝试 PER_MONITOR_AWARE_V2")
        self.assertTrue(per_monitor, "必须回退到 PER_MONITOR_AWARE")
        self.assertTrue(legacy, "必须回退到 SetProcessDPIAware")
        # 顺序不能反：SetProcessDPIAware 放最前会让 PMv2 永远失败。
        self.assertLess(pmv2[0], per_monitor[0], "PMv2 必须最先尝试")
        self.assertLess(per_monitor[0], legacy[0], "SetProcessDPIAware 必须最后兜底")

        header = self._read("src/app.h")
        self.assertIn("dpiAware_", header)

        initialize_span = body_span(code, "bool App::Initialize()")
        initialize = code[initialize_span[0] : initialize_span[1]]
        self.assertIn("EnablePerMonitorDpiAwareness()", initialize)
        self.assertIn("dpiAware_", initialize)
        # 提示必须落在"感知失败"那个分支里：Initialize 里本来就有别的
        # ShowNotice，分开断言等于什么都没断。
        branches = find_calls(code, initialize_span, "if (!dpiAware_)")
        self.assertTrue(branches, "必须判断 DPI 感知是否成功")
        open_brace = code.index("{", branches[0])
        block = code[open_brace : matching_close(code, open_brace) + 1]
        self.assertIn(
            "ShowNotice(",
            block,
            msg="DPI 感知失败时必须在那个分支里明确告知用户",
        )

        # main.cpp 里不能再有裸调用：必须早于任何窗口创建，且失败要能被
        # App 感知到。
        main = strip_comments(self._read("src/main.cpp"))
        self.assertNotIn(
            "SetProcessDpiAwarenessContext",
            main,
            msg="DPI 感知必须走 App::Initialize 的带回退的入口",
        )

    # --- SetWindowRgn 失败必须自己释放区域 ----------------------------

    def test_set_window_rgn_releases_the_region_on_failure(self) -> None:
        """SetWindowRgn 失败时会泄漏一个 HRGN。

        成功后区域的所有权交给系统，失败时必须自己释放。src/pin_window.cpp
        里已经是这个写法，两处漏的写法要补齐。
        """
        for relative, signature in (
            ("src/ui_theme.h", "inline void SetRoundedWindowRegion(HWND hwnd, int radius)"),
            ("src/ocr_result_panel_v2.cpp", "void SetRoundedWindowRegion(HWND hwnd)"),
        ):
            code = strip_comments(self._read(relative))
            span = body_span(code, signature)
            self.assertTrue(
                find_calls(code, span, "SetWindowRgn("),
                msg=f"{relative} 必须给窗口套上圆角区域",
            )
            deletes = find_calls(code, span, "DeleteObject(region)")
            self.assertTrue(
                deletes,
                msg=f"{relative} 的 SetWindowRgn 失败路径必须释放 region",
            )
            # 释放必须由 SetWindowRgn 的失败判断守住：无条件释放会在成功
            # 时把系统已经接管的区域再删一次，死分支里释放等于没释放。
            conditions = enclosing_if_conditions(code, deletes[0])
            self.assertTrue(
                conditions,
                msg=f"{relative} 的 region 释放必须在失败分支里，不能无条件释放",
            )
            self.assertIn(
                "SetWindowRgn",
                conditions[0],
                msg=f"{relative} 的 region 释放必须由 SetWindowRgn 的失败判断守住",
            )

    # --- OCR 临时文件名必须唯一 ---------------------------------------

    def test_ocr_temp_paths_are_unique(self) -> None:
        """OCR 临时 PNG 的文件名必须唯一。

        GetTickCount64() 的分辨率是系统时钟粒度（约 15.6ms），同一 tick 内
        的两次请求会算出同一个路径，两个后台线程互相覆盖、甚至删掉同一批
        PNG。
        """
        code = strip_comments(self._read("src/ocr_v2.cpp"))
        span = body_span(code, "std::filesystem::path MakeTempOcrPath(int index)")
        fetches = find_calls(code, span, "g_tempPathCounter.fetch_add(")
        self.assertTrue(fetches, msg="必须用进程内自增序号保证文件名唯一")
        # 计数器必须真的拼进文件名：只声明计数器、只用它做别的事，
        # 文件名照样会撞。
        statement = statement_at(code, fetches[0])
        self.assertRegex(
            statement,
            r"name\s*\+=",
            msg="计数器必须真的拼进文件名",
        )
        self.assertIn("<atomic>", self._read("src/ocr_v2.cpp"))


class OcrTempCleanupGuardTest(unittest.TestCase):
    """OCR 临时文件按 PID 归属清理的源码级守卫。

    这段逻辑的两个缺陷都出在"判不出来的时候怎么办"：

    1. PID 位数不封顶 → DWORD 里**静默回绕**成另一个 PID，
       "这是谁的临时文件"因此判错。
    2. ``OpenProcess`` 打不开（受保护进程是 ERROR_ACCESS_DENIED，
       **不是**"进程没了"）被一律当成"已死" → 误删别人正在用的临时 PNG，
       那次识别直接失败。
    """

    @staticmethod
    def _read(relative: str) -> str:
        return (REPO_ROOT / relative).read_text(encoding="utf-8")

    def test_ocr_temp_pid_must_not_wrap_around(self) -> None:
        code = strip_comments(self._read("src/app.cpp"))
        span = body_span(code, "DWORD ParseOcrTempFilePid(const wchar_t* fileName)")
        body = code[span[0] : span[1]]
        # 累加必须用 64 位，并在超出 DWORD 上限时立刻放弃。
        self.assertIn("unsigned long long", body)
        self.assertIn("0xFFFFFFFF", body)
        # 不能用 DWORD 累加：DWORD 里会静默回绕。
        self.assertNotIn("DWORD pid = 0", body)

    def test_process_liveness_must_not_treat_cannot_open_as_dead(self) -> None:
        code = strip_comments(self._read("src/app.cpp"))
        # 不能再用 OpenProcess 的"打不开就算死"判据。
        self.assertNotIn("OpenProcess(", code)
        # 必须有"判不出来"这个状态：宁可留下文件，也不要误删别人的。
        self.assertIn("ProcessState::kUnknown", code)
        self.assertIn("QueryProcessState", code)

    def test_ocr_cleanup_only_deletes_files_of_dead_processes(self) -> None:
        code = strip_comments(self._read("src/app.cpp"))
        span = body_span(code, "void CleanupLeftoverOcrImages()")
        body = code[span[0] : span[1]]
        # 只有确认进程已死才删；判不出来（kUnknown）必须跳过。
        self.assertIn("QueryProcessState(", body)
        self.assertIn("ProcessState::kDead", body)
        # 进程快照只能取**一次**：以前是每个文件取一次，文件一多，
        # 退出时要把整张进程表枚举 N 遍，明显变慢。
        self.assertEqual(
            len(find_calls(code, span, "SnapshotAliveProcessIds(")),
            1,
            msg="进程快照必须只取一次（每个文件取一次会让退出明显变慢）",
        )
        # 快照必须在文件枚举**之后**：反过来的话，新实例刚建的文件会被
        # 当成"主人已经死了"而删掉，那次识别直接失败。
        enumerate_at = find_calls(code, span, "FindFirstFileW(")
        snapshot_at = find_calls(code, span, "SnapshotAliveProcessIds(")
        self.assertTrue(enumerate_at, msg="必须枚举临时文件")
        self.assertTrue(snapshot_at, msg="必须取一次进程快照")
        self.assertLess(
            enumerate_at[0],
            snapshot_at[0],
            msg="必须先把文件列完再取进程快照，否则会误删刚建出来的文件",
        )

    def test_detach_must_be_outside_the_try_block(self) -> None:
        """detach 必须在 try 外面。

        线程一旦创建成功就已经在跑，``RunOcrPipeline`` 会**接管并释放**
        cropped。detach 抛异常时若落进 catch 里再 ``DeleteObject(cropped)``，
        就是同一块位图被释放两次；而 try 作用域结束时 worker 析构发现线程
        还 joinable，会直接 ``std::terminate()``——整个截图程序当场没了。
        """
        code = strip_comments(self._read("src/ocr_v2.cpp"))
        span = body_span(code, "bool StartTextExtractionFromBitmapRegion(")
        body = code[span[0] : span[1]]
        catch_at = body.index("catch (const std::system_error&)")
        catch_end = body.index("}", catch_at)
        detach_at = body.index("worker.detach()")
        self.assertLess(
            catch_end,
            detach_at,
            msg="detach 必须放在线程创建的 try/catch 之外",
        )
        # 线程没起来时位图仍归调用方，catch 里必须释放它。
        catch_body = body[catch_at:catch_end]
        self.assertIn("DeleteObject(cropped)", catch_body)

    def test_overlay_placement_must_stay_on_the_same_monitor_as_the_dpi(self) -> None:
        """覆盖层的落位与 DPI 必须来自同一块显示器。

        DPI 与落位来自**不同**来源时，会按一块屏的 DPI 量尺寸、摆到另一块
        屏上：面板要么大一号要么小一号。

        现在的来源是**光标所在的那块屏**（延时截图是全局快捷键触发的，
        用户在哪块屏干活、鼠标通常就在哪块屏，倒计时就该出现在那儿）。
        通知窗口是钉在 (0,0) 的隐藏窗口、永远属于主屏——用它来落位，
        用户在副屏干活时倒计时跑到了另一块屏上（用户双屏环境）。

        拿不到光标位置（或那块屏的 DPI）时退回通知窗口所在屏，两条路径
        各自保持 DPI 与落位同源。
        """
        code = self._read("src/delay_overlay.cpp")
        self.assertIn(
            "MonitorFromPoint(",
            code,
            msg="落位必须按光标所在的那块屏来选（通知窗口永远在主屏）",
        )
        self.assertIn(
            "MonitorDpi(monitor)",
            code,
            msg="DPI 必须取**同一块**屏的（monitor 既是落位来源也是 DPI 来源）",
        )
        # 退回路径也必须同源：MonitorFromWindow + GetDpiForWindow。
        self.assertIn("MonitorFromWindow(notifyWindow", code)
        self.assertIn("GetDpiForWindow(notifyWindow", code)
        # 这句注释是不成立的：主窗口是 WS_OVERLAPPED，系统把请求的 0×0
        # 撑成最小尺寸（约 136×39），矩形完整落在 (0,0) 所在的显示器上，
        # MonitorFromWindow 并不会因此退回主显示器。
        self.assertNotIn("跟任何显示器都没有交集", code)


class AnalysisHelperTest(unittest.TestCase):
    """源码分析辅助函数自身的行为。

    这些辅助函数是守卫的发动机：源码被改坏时，守卫必须**快速失败**，
    报出一条"哪里不对"的信息。要是分析代码自己卡死，CI 上看到的就
    是一份超时报告，而不是失败信息——那等于守卫在最需要它的场景
    （关键调用被删掉）里失效了。
    """

    def test_scan_budget_reports_an_overrun_instead_of_hanging(self) -> None:
        """步数预算必须真的会触发。

        预算本身没有测试，就只是一句注释：哪天有人把"推进下标"那行删了，
        预算还在，可没人知道它还能不能拦住一次挂死。这里直接把预算跑到
        超限，确认它给出的是一条可读的失败信息，而不是一次挂死。
        """
        budget = ScanBudget(4)
        for _ in range(5):
            budget.step()
        # 再走一步就必须报错（预算 = total + 1）。
        with self.assertRaises(AssertionError):
            budget.step()

    def test_undefined_macro_else_branch_is_compiled(self) -> None:
        """未定义宏的 #else 分支属于产品编译内容，不能当成测试专用代码。"""
        code = (
            "#ifdef SNAPLITE_DELAY_TEST_HOOKS\n"
            "TestOnlyHook();\n"
            "#else\n"
            "ProductFallback();\n"
            "#endif\n"
        )
        product_position = code.index("ProductFallback")
        test_position = code.index("TestOnlyHook")
        compiled = compiled_spans(code, {"SNAPLITE_DELAY_TEST_HOOKS"})
        excluded = excluded_spans(code, {"SNAPLITE_DELAY_TEST_HOOKS"})
        self.assertTrue(any(start <= product_position < end for start, end in compiled))
        self.assertFalse(any(start <= product_position < end for start, end in excluded))
        self.assertTrue(any(start <= test_position < end for start, end in excluded))

    def test_functions_terminates_on_unbalanced_block_headers(self) -> None:
        """块头括号不配平（for-range 里的花括号初始化）不能让扫描卡死。

        反向扫描块头时会在内层 '{' 处停下，拿到的头部是 ") "，
        括号数不配平。曾经这里直接 continue 却不推进下标，
        扫描就会在同一个 '{' 上永远转下去。
        """
        code = (
            "void Foo() {\n"
            "    for (auto& x : {1, 2, 3}) {\n"
            "        Bar();\n"
            "    }\n"
            "}\n"
        )
        found = functions(code)
        self.assertIn("Foo", [name for name, _ in found])

    def test_resolve_calls_returns_empty_when_the_call_is_missing(self) -> None:
        """关键调用被删掉时，分析必须返回空列表让守卫报失败，而不是卡死。"""
        code = (
            "void App::Shutdown() {\n"
            "    if (hwnd_) {\n"
            "        Teardown();\n"
            "    }\n"
            "}\n"
        )
        span = body_span(code, "void App::Shutdown()")
        self.assertEqual(resolve_calls(code, span, "DelayOverlay::Cancel()"), [])

    def test_resolve_calls_still_finds_a_call_wrapped_in_a_helper(self) -> None:
        """等价的重构（抽成 helper）不该让守卫误报。"""
        code = (
            "void CancelOverlay() {\n"
            "    DelayOverlay::Cancel();\n"
            "}\n"
            "void App::Shutdown() {\n"
            "    CancelOverlay();\n"
            "}\n"
        )
        span = body_span(code, "void App::Shutdown()")
        self.assertTrue(resolve_calls(code, span, "DelayOverlay::Cancel()"))


class ReleaseVersionTest(unittest.TestCase):
    """发布版本解析必须有本地测试手段。

    这段逻辑原先长在 release.yml 里，两个真实缺陷都没人看得见：预发布
    版本号会让 Inno 安装包在最后一步编译失败（那时 exe 已构建好、CI
    时间和 token 都花光了）；"取最后一个变更文件"会选中被删除的文件，
    拿到的版本号是旧的。
    """

    def setUp(self) -> None:
        self.resolver = _load_module("resolve_release_version")

    @staticmethod
    def _read(relative: str) -> str:
        return (REPO_ROOT / relative).read_text(encoding="utf-8")

    # ===== PowerShell 片段分析 =====
    #
    # 发布链是 YAML 里嵌的 PowerShell。纯文本断言在它面前有一串老洞：
    #  1. 注释里的 "throw" 也能满足"必须 throw"；
    #  2. 把 throw 换成 Write-Host，条件形状照样对；
    #  3. 把 throw 藏进 if ($false) 里，纯文本看不出来。
    #
    # 上一轮验收把这三条都实测成了"守卫应红却绿"。所以这里补了几个
    # 能问出**真实行为**的小工具：剥注释、找 if 块、认死分支、展开变量。
    #
    # 再上一轮验收又挖出**五类更深的洞**（每一条都有实测记录）：
    #  4. 字符串字面量里的 "throw" 也算数
    #     （`Write-Host "resolver failed; throw not reached"`）；
    #  5. throw 藏进**任何**嵌套 if 里都算数
    #     （`if ($env:SNAPLITE_NEVER -eq 'yes') { throw ... }`——
    #      条件不是字面 $false，"死分支"识别认不出来，但闸门照样形同虚设，
    #      pwsh 仿真实测 job 照样是绿的）；
    #  6. 变量赋值取"首个匹配"，前面放个诱饵空赋值就能骗过；
    #  7. 反引号续行会把赋值截断，拼接出来的开关认不出来；
    #  8. 条件里的括号不配平时整个 if 块被静默丢掉（守卫报"没有闸门"）。
    #
    # 这一轮的教训：**形状不等于语义**。断言必须问"这段代码真的会
    # 这么做吗"，而不是"这段代码长得像吗"。

    @classmethod
    def _powershell_mask_literals(cls, script: str) -> str:
        """把 PowerShell 的字符串字面量替换成等长的空格（下标不变）。

        PowerShell 两种引号的转义规则不一样：
        - 单引号串里，'' 表示一个单引号，反斜杠**不是**转义符；
        - 双引号串里，"" 表示一个双引号，反引号是转义符。

        不做这一步的话，一句 `Write-Host "resolver failed; throw not reached"`
        就能满足"必须有 throw"（上一轮验收实测的漏判 X3）。
        """
        out = list(script)
        index = 0
        length = len(script)
        while index < length:
            ch = script[index]
            if ch == "`":
                # 反引号转义：它和后面那个字符一起被遮掉。
                out[index] = " "
                index += 1
                if index < length:
                    out[index] = " "
                    index += 1
                continue
            if ch == "@" and index + 1 < length and script[index + 1] in "'\"":
                # here-string：@'...'@ 或 @"..."@
                #
                # 终止符必须**独占一行**（PowerShell 的硬性要求）。
                # 不认它就会出事：一句
                #   $note = @'
                #   ' ; throw 'tag mismatch' ; '
                #   '@
                # 里那个 throw 根本不会执行，可遮罩认不出来时它照样
                # 满足"必须有一条真会执行的 throw"（第 14 轮验收实测：
                # pwsh 仿真下 tag 闸门完全失效，job 照样是绿的）。
                quote = script[index + 1]
                terminator = quote + "@"
                cursor = index + 2
                closed = False
                while cursor < length:
                    line_end = script.find("\n", cursor)
                    if line_end < 0:
                        line_end = length
                    if script[cursor:line_end].strip() == terminator:
                        for position in range(index, line_end + 1):
                            out[position] = " "
                        index = line_end + 1
                        closed = True
                        break
                    cursor = line_end + 1
                if not closed:
                    # 没找到终止符：把剩下的全遮掉（语法本来就不完整）。
                    for position in range(index, length):
                        out[position] = " "
                    index = length
                continue
            if ch not in "'\"":
                index += 1
                continue
            quote = ch
            index += 1
            while index < length:
                if script[index] == quote:
                    # 成对的引号表示一个引号（字符串继续）。
                    if index + 1 < length and script[index + 1] == quote:
                        out[index] = " "
                        out[index + 1] = " "
                        index += 2
                        continue
                    out[index] = " "
                    index += 1
                    break
                if quote == '"' and script[index] == "`":
                    out[index] = " "
                    index += 1
                    if index < length:
                        out[index] = " "
                        index += 1
                    continue
                out[index] = " "
                index += 1
        return "".join(out)

    @classmethod
    def _powershell_braces(cls, script: str, open_brace: int) -> int:
        """返回与 open_brace 配对的右花括号下标。

        字符串里的花括号不算（用遮罩后的文本数）。花括号不配平时
        抛 AssertionError——调用方要能发现自己拿到的是半截代码，
        而不是拿着一段乱码继续找 if 块。
        """
        masked = cls._powershell_mask_literals(script)
        depth = 0
        for index in range(open_brace, len(masked)):
            ch = masked[index]
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return index
        raise AssertionError("花括号不配平")

    @classmethod
    def _powershell_if_blocks(
        cls, script: str
    ) -> list[tuple[str, str, int, int]]:
        """列出 PowerShell 里所有 if 块：(条件, 块体, 块体起点, 块体终点)。

        括号配对必须在**遮掉字符串之后**数：条件里出现
        `if ($x -eq 'a)b')` 时，字符串里那个括号会让配对跑偏，
        整个 if 块被静默丢掉——守卫于是报"没有闸门"，方向是假红，
        但报出来的原因跟真正的问题没关系（上一轮验收实测）。

        花括号不配平的块直接跳过，不抛异常：守卫要以断言失败结束，
        而不是以 ERROR 结束。
        """
        results: list[tuple[str, str, int, int]] = []
        masked = cls._powershell_mask_literals(script)
        for match in re.finditer(r"\bif\s*\(", masked):
            open_paren = masked.find("(", match.start())
            depth = 0
            index = open_paren
            end_paren = -1
            while index < len(masked):
                if masked[index] == "(":
                    depth += 1
                elif masked[index] == ")":
                    depth -= 1
                    if depth == 0:
                        end_paren = index
                        break
                index += 1
            if end_paren < 0:
                continue
            open_brace = script.find("{", end_paren)
            if open_brace < 0:
                continue
            if script[end_paren + 1 : open_brace].strip():
                # 括号与花括号之间还有别的东西：不是普通的块写法。
                continue
            try:
                close = cls._powershell_braces(script, open_brace)
            except AssertionError:
                continue
            results.append(
                (
                    script[open_paren + 1 : end_paren],
                    script[open_brace : close + 1],
                    open_brace,
                    close,
                )
            )
        return results

    @staticmethod
    def _powershell_condition_is_false(condition: str) -> bool:
        """条件是不是**恒假**（只看字面量，不看运行时变量）。

        认得出的恒假形态：
        - $false / 0 / $null
        - 用 -and / -or 拼起来的纯字面量表达式
          （按 PowerShell 的"-and 优先于 -or"求值）

        认不出的（变量参与的比较）不算恒假——那种要靠"throw 必须
        直接属于这个块"来挡（第 5 条洞）。
        """
        text = condition.strip()
        if not text:
            return False
        parts = re.split(r"(\s+-(?:and|or)\s+)", text, flags=re.IGNORECASE)
        operands: list[bool] = []
        operators: list[str] = []
        for index, part in enumerate(parts):
            if index % 2 == 1:
                operators.append(part.strip().lower())
                continue
            token = part.strip().lower()
            if token == "$true":
                operands.append(True)
            elif token in ("$false", "0", "$null"):
                operands.append(False)
            else:
                # 掺了运行时变量的条件：不是恒假。
                return False
        if not operands:
            return False
        # -and 优先于 -or：按 -or 切段，段内全是 -and。
        groups: list[list[bool]] = [[operands[0]]]
        for operator, operand in zip(operators, operands[1:]):
            if operator == "-and":
                groups[-1].append(operand)
            else:
                groups.append([operand])
        return not any(all(group) for group in groups)

    @classmethod
    def _powershell_dead_spans(cls, script: str) -> list[tuple[int, int]]:
        """找出 PowerShell 里永远不执行的块（恒假条件后面的整块）。"""
        spans: list[tuple[int, int]] = []
        masked = cls._powershell_mask_literals(script)
        for match in re.finditer(r"\bif\s*\(", masked):
            open_paren = masked.find("(", match.start())
            depth = 0
            index = open_paren
            end_paren = -1
            while index < len(masked):
                if masked[index] == "(":
                    depth += 1
                elif masked[index] == ")":
                    depth -= 1
                    if depth == 0:
                        end_paren = index
                        break
                index += 1
            if end_paren < 0:
                continue
            condition = script[open_paren + 1 : end_paren]
            if not cls._powershell_condition_is_false(condition):
                continue
            open_brace = script.find("{", end_paren)
            if open_brace < 0:
                continue
            try:
                close = cls._powershell_braces(script, open_brace)
            except AssertionError:
                continue
            spans.append((match.start(), close + 1))
        return spans

    @classmethod
    def _powershell_nested_block_spans(cls, script: str) -> list[tuple[int, int]]:
        """列出脚本里所有"比最外层更深一层"的花括号块的内部区间。

        用于判断某条语句是不是**直接**属于最外层：藏进嵌套 if 里的
        throw 不算这个块的直接语句。
        """
        masked = cls._powershell_mask_literals(script)
        spans: list[tuple[int, int]] = []
        stack: list[int] = []
        for index, ch in enumerate(masked):
            if ch == "{":
                stack.append(index)
            elif ch == "}":
                if stack:
                    open_brace = stack.pop()
                    if stack:
                        spans.append((open_brace + 1, index))
        return spans

    @classmethod
    def _powershell_conditional_spans(cls, script: str) -> list[tuple[int, int]]:
        """脚本里"可能不执行"的区间。

        - if 的块：条件不是恒真时，块可能一次都不执行；
        - else / while / for / foreach / catch / try / switch 的块：同理；
        - do 的块：至少执行一次，不算；
        - 普通的 { } 分组块：只是分组，永远执行，不算；
        - & { } 这种被调用的脚本块：会执行，不算。

        第 14 轮验收实测的两条漏判都落在这一层：
        - throw 藏进 `if ($env:NEVER -eq 'yes') { throw }`（永不成立）；
        - throw 藏进 `try { throw } catch { }`（被吞掉，job 照样绿）。
        """
        spans: list[tuple[int, int]] = []
        for condition, _body, start, end in cls._powershell_if_blocks(script):
            if cls._powershell_condition_is_true(condition):
                continue
            spans.append((start, end + 1))
        masked = cls._powershell_mask_literals(script)
        for match in re.finditer(
            r"\b(?:else|while|for|foreach|catch|try|switch)\b[^\n]{0,200}?\{",
            masked,
        ):
            open_brace = masked.find("{", match.start())
            if open_brace < 0:
                continue
            try:
                close = cls._powershell_braces(script, open_brace)
            except AssertionError:
                continue
            spans.append((open_brace + 1, close))
        return spans

    @staticmethod
    def _powershell_condition_is_true(condition: str) -> bool:
        """条件是不是**恒真**（只看字面量，不看运行时变量）。

        与 _powershell_condition_is_false 互为镜像。
        """
        text = condition.strip()
        if not text:
            return False
        parts = re.split(r"(\s+-(?:and|or)\s+)", text, flags=re.IGNORECASE)
        operands: list[bool] = []
        operators: list[str] = []
        for index, part in enumerate(parts):
            if index % 2 == 1:
                operators.append(part.strip().lower())
                continue
            token = part.strip().lower()
            if token in ("$true", "1"):
                operands.append(True)
            elif token in ("$false", "0", "$null"):
                operands.append(False)
            else:
                # 掺了运行时变量的条件：认不出真假。
                return False
        if not operands:
            return False
        groups: list[list[bool]] = [[operands[0]]]
        for operator, operand in zip(operators, operands[1:]):
            if operator == "-and":
                groups[-1].append(operand)
            else:
                groups.append([operand])
        return any(all(group) for group in groups)

    @classmethod
    def _powershell_real_throws(cls, script: str) -> bool:
        """脚本里有没有一条**真的会执行**的 throw 语句。

        "真的"有三层意思：
        - 它是一条语句（不是字符串里的 "throw" 字样）；
        - 它不在 if ($false) 这种不会执行的分支里；
        - 它不在**可能不执行**的块里（if / else / while / for / foreach /
          catch / try / switch 的块）。

        第三条是第 12/13 轮验收实测出来的漏判：
        - throw 藏进 `if ($env:SNAPLITE_NEVER -eq 'yes') { throw ... }`
          这种"永不成立但不是字面 $false"的条件里，闸门就形同虚设
          （漏判 X9，pwsh 仿真实测 job 照样是绿的，还打印了一句
          误导性的 "FORCE_REBUILD is set"）；
        - throw 藏进 `try { throw } catch { }` 里，被 catch 吞掉。

        但也不能过严——下面这些写法里的 throw **真的会执行**，不能算漏网
        （第 14 轮验收实测的假红）：
        - `& { throw ... }`（调用脚本块）；
        - `do { throw ... } while ($false)`（do 的块至少执行一次）；
        - `if ($true) { throw ... }`（条件恒真，块一定执行）；
        - 普通的 `{ ... }` 分组块（只是分组，不是条件）。
        """
        masked = cls._powershell_mask_literals(script)
        dead = cls._powershell_dead_spans(script)
        conditional = cls._powershell_conditional_spans(script)
        for match in re.finditer(
            r"(?:^|[;{}])\s*(throw)\b", masked, re.MULTILINE
        ):
            # 取 **throw 关键字本身**的位置：整个匹配还包括前面的
            # 分隔符（"{" / ";"），拿它去比块的区间会把
            # "嵌套块里的 throw" 误算成直接语句。
            position = match.start(1)
            if any(start <= position < end for start, end in dead):
                continue
            if any(start <= position < end for start, end in conditional):
                continue
            return True
        return False

    @classmethod
    def _powershell_assignment_value(cls, script: str, start: int) -> str | None:
        """取一次赋值的值（从赋值号后面开始）。

        普通赋值取到行尾；**反引号续行要接下去**（上一轮验收实测的
        漏判 X15：`$flags = '--allow-base' + \`` ↵ `'-fallback'`
        只取到第一行，展开后认不出这个开关）。
        """
        rest = script[start:]
        stripped = rest.lstrip()
        offset = start + (len(rest) - len(stripped))
        if stripped.startswith("@(") or stripped.startswith("("):
            index = offset + 1
            depth = 0
            while index < len(script):
                ch = script[index]
                if ch in "'\"":
                    quote = ch
                    index += 1
                    while index < len(script):
                        if script[index] == "\\":
                            index += 2
                            continue
                        if script[index] == quote:
                            index += 1
                            break
                        index += 1
                    continue
                if ch == "(":
                    depth += 1
                elif ch == ")":
                    depth -= 1
                    if depth == 0:
                        return script[start : index + 1]
                index += 1
            return None
        pieces: list[str] = []
        position = offset
        while position < len(script):
            line_end = script.find("\n", position)
            line = script[position:] if line_end < 0 else script[position:line_end]
            pieces.append(line.strip())
            if not line.rstrip().endswith("`"):
                break
            if line_end < 0:
                break
            position = line_end + 1
        value = " ".join(piece for piece in pieces if piece)
        # 去掉行尾注释（引号里的 # 不算）。
        comment = value.find("#")
        if comment >= 0:
            before = value[:comment]
            if before.count("'") % 2 == 0 and before.count('"') % 2 == 0:
                value = before
        return value

    @classmethod
    def _powershell_variable_values(cls, script: str, name: str) -> list[str]:
        """列出脚本里给某个变量赋过的**所有**值（按出现顺序）。

        必须取**全部**而不是第一个：前面放一个诱饵空赋值
        （`$flags = ''`）再赋真值，取引入会漏掉真值
        （上一轮验收实测的漏判 X4）。
        """
        masked = cls._powershell_mask_literals(script)
        patterns = (
            # $name = ...
            re.compile(
                r"(?<![A-Za-z0-9_$])\$" + re.escape(name) + r"\s*=(?!=)\s*"
            ),
            # ${name} = ...
            re.compile(
                r"(?<![A-Za-z0-9_$])\$\{" + re.escape(name) + r"\}\s*=(?!=)\s*"
            ),
            # $name[0] = ...（下标赋值）
            re.compile(
                r"(?<![A-Za-z0-9_$])\$" + re.escape(name)
                + r"\s*\[[^\]\n]*\]\s*=(?!=)\s*"
            ),
        )
        values: list[str] = []
        for pattern in patterns:
            for match in pattern.finditer(masked):
                value = cls._powershell_assignment_value(script, match.end())
                if value is not None:
                    values.append(value)
        # Set-Variable -Name name -Value <值>
        for match in re.finditer(
            r"\bSet-Variable\b[^\n]{0,200}?-Name\s+"
            + re.escape(name)
            + r"\b[^\n]{0,200}?-Value\s+",
            masked,
        ):
            value = cls._powershell_assignment_value(script, match.end())
            if value is not None:
                values.append(value)
        return values

    @classmethod
    def _powershell_variable_value(cls, script: str, name: str) -> str | None:
        """取脚本里给某个变量赋的值（取**最后一次**赋值，找不到返回 None）。"""
        values = cls._powershell_variable_values(script, name)
        return values[-1] if values else None

    @classmethod
    def _powershell_expand(cls, text: str, script: str, depth: int = 0) -> str:
        """把文本里的 $var / @var 换成脚本里给这个变量赋的值。

        一个变量可能有多个赋值（前面那个是诱饵），所以把所有赋值
        **都**放进去：任何一种取值里出现了不该出现的开关，都算数
        （上一轮验收实测的漏判 X4 / X15）。
        """
        if depth > 4:
            return text

        def replace(match: re.Match[str]) -> str:
            name = match.group(1)
            # 先看这个变量在**这段文本里**是不是被赋过值：
            # 是的话它的值由这段文本决定，不能拿脚本别处的同名赋值
            # 来展开（$resolved 在 push 段和手动段各被赋过一次，
            # 混起来会看出本来不该有的东西）。
            values = cls._powershell_variable_values(text, name)
            if not values:
                values = cls._powershell_variable_values(script, name)
            if not values:
                return match.group(0)
            joined = " ".join(values)
            # 自引用（$x = ... $x ...）：别再展开下去，否则会原地打转
            # （$resolved = & python ... 这种，展开 $resolved 等于把
            # 整条命令再塞回去）。
            if re.search(r"[@$]\{?" + re.escape(name) + r"\}?\s*=", joined):
                return match.group(0)
            return cls._powershell_expand(joined, script, depth + 1)

        return re.sub(r"[@$]\{?([A-Za-z_][A-Za-z0-9_]*)\}?", replace, text)

    @staticmethod
    def _powershell_normalize(text: str) -> str:
        """归一化：去掉引号、连接符和空白，让拼接出来的开关也认得出来。"""
        return re.sub(r"[\s'\"+]", "", text).lower()

    @staticmethod
    def _powershell_exit_code_is_failure(condition: str) -> bool:
        """条件是不是"退出码失败时才成立"。

        认得的写法（引号风格、空格、操作数顺序都**不该**影响判定）：
        - $LASTEXITCODE -ne 0 / -ne '0' / -ne "0" / -ne $null
        - $LASTEXITCODE -gt 0
        - 0 -ne $LASTEXITCODE 等（操作数对调，语义等价）
        - if ($LASTEXITCODE)   （退出码非零即真）

        **复合条件一律不认**（含 -and / -or）：
        - `$LASTEXITCODE -ne 0 -and $false` 恒假 → 闸门永不触发；
        - `$LASTEXITCODE -ne 0 -or $LASTEXITCODE -eq 0` 恒真 → 每次正常
          发布都被误杀。
        这两种都是"看着像失败判断、其实不是"。第 14 轮验收用 pwsh 真实
        仿真证明了两种后果（resolver 失败被吞掉 / 正常发布必失败）。

        写成 `-eq 0` 就反了：成功的退出码反而被当成失败
        （第 12 轮验收实测的漏判 X2，pwsh 仿真实测正常发布必失败）。
        """
        text = condition.strip()
        if re.search(r"-(?:and|or)\b", text, re.IGNORECASE):
            # 复合条件：光看形状认不出来真假，一律不认。
            return False
        # 去掉全部空白后比较：引号风格和空格都不该影响判定。
        compact = re.sub(r"\s+", "", text)
        zero = r"(?:0|'0'|\"0\"|\$null)"
        forward = re.fullmatch(
            rf"\$LASTEXITCODE-(?:ne|gt){zero}(?![0-9A-Za-z_])", compact
        )
        backward = re.fullmatch(
            rf"{zero}-ne\$LASTEXITCODE(?![0-9A-Za-z_])", compact
        )
        truthy = re.fullmatch(r"\$LASTEXITCODE", compact)
        return bool(forward or backward or truthy)

    @staticmethod
    def _changed_files_argument(text: str) -> str | None:
        """取 --changed-files 后面那个变量名（引号可有可无）。"""
        match = re.search(
            r"--changed-files\s+['\"]?(\$[A-Za-z_][A-Za-z0-9_]*)", text
        )
        return match.group(1) if match else None

    @staticmethod
    def _resolver_extra_arguments(text: str, allowed_flags: set[str]) -> list[str]:
        """列出 resolver 调用里"不该有"的参数。

        脚本名之后只允许两种东西：白名单里的开关，和变量引用。
        以下都算"多出来的"：
        - splat（@xxx）：@flags / @extraArgs 一律不行；
        - 没见过的开关；
        - 多余的位置参数。

        这一条比"把变量展开再找开关"更靠得住：不管那个变量是怎么赋值
        出来的（${name} / Set-Variable / 下标 / 全局遮蔽 / 拼接），
        多出来的参数都会被它抓住。第 14 轮验收实测：`${extraArgs} =
        @('--allow-base' + '-fallback')` + `@extraArgs` 这一类写法，
        展开根本看不见，守卫全绿——而 pwsh 真实仿真证明 push 路径
        确实静默回落、拿旧 tag 覆盖已发布产物。
        """
        compact = re.sub(r"`[ \t]*\r?\n", " ", text)
        compact = re.sub(r"#[^\n]*", "", compact)
        tokens = [token for token in re.split(r"\s+", compact) if token]
        extras: list[str] = []
        seen_script = False
        for token in tokens:
            if not seen_script:
                if "resolve_release_version.py" in token:
                    seen_script = True
                continue
            if token.startswith("$"):
                continue
            # 引号包起来的变量引用（"$changedFile"）仍然是变量引用。
            bare = token.strip("'\"")
            if bare.startswith("$"):
                continue
            if token.startswith("@"):
                extras.append(token)
                continue
            if bare.startswith("-"):
                if bare not in allowed_flags:
                    extras.append(bare)
                continue
            extras.append(bare)
        return extras

    @staticmethod
    def _git_changed_files_variable(code: str) -> str | None:
        """找出 git diff 把变更文件列表写进去的那个变量名。

        用来回答"哪条路径读的是 git 算出来的列表"——这是"回落开关
        挂在哪条路径上"这件事唯一可靠的判据：按字面量分组时，把
        开关从手动路径挪到 push 路径，分组结果还是"一带一不带"，
        守卫照样全绿（上一轮验收实测的漏判 X5）。
        """
        patterns = (
            # 管道 + Out-File/Set-Content（-FilePath / -Path / 位置参数都认；
            # 管道符号也可能落在下一行行首——pwsh 7 合法）
            r"git\b[^\n]*\bdiff\b[\s\S]{0,200}?\|\s*(?:Out-File|Set-Content)"
            r"[^\n]{0,200}?(?:-FilePath|-Path)\s+(\$[A-Za-z_][A-Za-z0-9_]*)",
            r"git\b[^\n]*\bdiff\b[\s\S]{0,200}?\|\s*(?:Out-File|Set-Content)"
            r"[^\n]{0,200}?\s(\$[A-Za-z_][A-Za-z0-9_]*)",
            r"git\b[^\n]*\bdiff\b[^\n]*>\s*(\$[A-Za-z_][A-Za-z0-9_]*)",
        )
        for pattern in patterns:
            match = re.search(pattern, code)
            if match:
                return match.group(1)
        return None

    @staticmethod
    def _yaml_step_run_block(yaml_text: str, step_name: str) -> str:
        """取出某个 step 的 run 块内容。"""
        lines = yaml_text.splitlines()
        for index, line in enumerate(lines):
            if line.strip() != f"- name: {step_name}":
                continue
            for follow in range(index + 1, len(lines)):
                stripped = lines[follow].strip()
                if stripped.startswith("run: |"):
                    indent = len(lines[follow]) - len(lines[follow].lstrip())
                    body: list[str] = []
                    for current in lines[follow + 1 :]:
                        if current.strip() and (
                            len(current) - len(current.lstrip())
                        ) <= indent:
                            break
                        body.append(current)
                    return "\n".join(body)
                if stripped.startswith("- name:"):
                    break
        return ""

    @staticmethod
    def _powershell_run_blocks(yaml_text: str) -> list[str]:
        """取出 YAML 里每个 `run: |` 块的内容（按缩进判定结束）。"""
        blocks: list[str] = []
        lines = yaml_text.splitlines()
        index = 0
        while index < len(lines):
            line = lines[index]
            if line.strip().startswith("run: |"):
                indent = len(line) - len(line.lstrip())
                body: list[str] = []
                index += 1
                while index < len(lines):
                    current = lines[index]
                    if current.strip() and (
                        len(current) - len(current.lstrip())
                    ) <= indent:
                        break
                    body.append(current)
                    index += 1
                blocks.append("\n".join(body))
                continue
            index += 1
        return blocks

    def test_resolver_uses_the_new_release_note(self) -> None:
        self.assertEqual(
            self.resolver.resolve_release_version("1.6.1", [".release/v1.6.1.md"]),
            "1.6.1",
        )
        self.assertEqual(
            self.resolver.resolve_release_version("1.6.0", [".release/v1.6.0-rc.1.md"]),
            "1.6.0-rc.1",
        )

    def test_resolver_fails_loudly_without_a_release_note(self) -> None:
        """没有发布说明文件时必须**响亮失败**，不能静默回落项目版本号。

        静默回落的后果：一次"只改了旧说明文件"的提交也会触发发布，
        于是拿旧 tag 配 `--clobber` 覆盖已发布 release 的产物。
        """
        with self.assertRaises(self.resolver.ResolveError):
            self.resolver.resolve_release_version("1.6.0", [])
        # 变更里只有普通源码，同样没有候选。
        with self.assertRaises(self.resolver.ResolveError):
            self.resolver.resolve_release_version("1.6.0", ["src/app.cpp"])
        # 显式要求回落时才回落（手动触发发布走这条路）。
        self.assertEqual(
            self.resolver.resolve_release_version(
                "1.6.0", [], allow_base_fallback=True
            ),
            "1.6.0",
        )

    def test_resolver_prefers_the_exact_version_on_the_same_line(self) -> None:
        """同一条版本线上有多个说明文件时，与项目版本完全相同的优先。

        预发布流程：先发 1.6.0-rc.1，再发 1.6.0。两个说明文件可能同时
        出现在变更列表里，这时该发的是 1.6.0。
        """
        self.assertEqual(
            self.resolver.resolve_release_version(
                "1.6.0", [".release/v1.6.0-rc.1.md", ".release/v1.6.0.md"]
            ),
            "1.6.0",
        )
        # 两个不同的预发布版本 → 无法判断该发哪个，报错而不是随便挑一个。
        with self.assertRaises(self.resolver.ResolveError):
            self.resolver.resolve_release_version(
                "1.6.0", [".release/v1.6.0-rc.1.md", ".release/v1.6.0-rc.2.md"]
            )

    def test_resolver_strips_utf8_bom_from_the_file_list(self) -> None:
        """带 BOM 的变更文件列表不能让发布说明被静默跳过。

        BOM 粘在第一行前面，第一行就变成 "﻿.release/v1.6.0.md"，
        startswith(".release/") 判断失败 → 候选为空 → 回落项目版本号
        → 用旧 tag 重发。
        """
        self.assertEqual(
            self.resolver.resolve_release_version(
                "1.6.0", ["﻿.release/v1.6.0.md"]
            ),
            "1.6.0",
        )

    def test_resolver_rejects_malformed_release_note_names(self) -> None:
        # 无扩展名的旧文件、说明文件写错名字，都要报错而不是静默跳过：
        # 静默跳过会把一次错误的发布放过去。
        with self.assertRaises(self.resolver.ResolveError):
            self.resolver.resolve_release_version("1.6.0", [".release/v1.2.1"])
        with self.assertRaises(self.resolver.ResolveError):
            self.resolver.resolve_release_version("1.6.0", [".release/release-notes.md"])

    def test_resolver_rejects_versions_from_another_base(self) -> None:
        with self.assertRaises(self.resolver.ResolveError):
            self.resolver.resolve_release_version("1.6.0", [".release/v1.5.0.md"])

    def test_resolver_rejects_conflicting_candidates(self) -> None:
        with self.assertRaises(self.resolver.ResolveError):
            self.resolver.resolve_release_version(
                "1.6.0", [".release/v1.6.1.md", ".release/v1.6.2.md"]
            )

    def test_base_version_can_be_read_from_cmake(self) -> None:
        """不给 --base-version 时必须能从 CMakeLists.txt 里读出项目版本。

        这条路径曾经 100% 失效：正则带行首锚点却没开 re.MULTILINE，而
        project(...) 在文件第 2 行。
        """
        cmake = (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertEqual(self.resolver.parse_base_version(cmake), "1.7.2")

    def test_version_comparison_uses_components_not_prefixes(self) -> None:
        """版本比较必须按分量，不能只做字符串前缀比较。

        "1.6.01" 以 "1.6.0" 开头，看起来像同一条版本线，其实主版本
        分量已经变了。
        """
        self.assertTrue(self.resolver.same_release_line("1.6.0", "1.6.0-rc.1"))
        self.assertTrue(self.resolver.same_release_line("1.6.0", "1.6.0"))
        self.assertFalse(self.resolver.same_release_line("1.6.0", "1.6.01"))
        self.assertFalse(self.resolver.same_release_line("1.6.0", "1.60.0"))
        self.assertFalse(self.resolver.same_release_line("1.6.0", "1.6.0x"))

        # 前导零几乎一定是手误（"1.06.0" 想写的是 "1.6.0"）。放它过去，
        # resolver 按分量判成"同一条线"，工作流按字符串判成"不同线"——
        # 同一件事两层给出不同结论（上一轮验收实测）。
        self.assertFalse(self.resolver.same_release_line("1.6.0", "1.06.0"))
        with self.assertRaises(self.resolver.ResolveError):
            self.resolver.resolve_release_version("1.6.0", [".release/v1.06.0.md"])

        with self.assertRaises(self.resolver.ResolveError):
            self.resolver.resolve_release_version("1.6.0", [".release/v1.6.01.md"])

    def test_resolver_ignores_unrelated_files_in_the_release_folder(self) -> None:
        """.release/ 下的无关文件不能挡住发布。

        目录里放一个 README.md、.gitkeep 是很平常的事；因为它就发不了版
        属于误伤（上一轮验收实测：加个 README.md → job 直接变红）。这类
        文件一律静默忽略。

        但"像是发布说明、名字却不合规"的文件（v1.6.0.bak）仍然要报错：
        那是把说明文件写坏了，必须让人看见——静默跳过会让一次错误的
        发布看起来很成功。
        """
        self.assertEqual(self.resolver.candidate_versions([".release/README.md"]), [])
        self.assertEqual(self.resolver.candidate_versions([".release/.gitkeep"]), [])
        self.assertEqual(
            self.resolver.candidate_versions([".release/notes/keep.md"]), []
        )
        with self.assertRaises(self.resolver.ResolveError):
            self.resolver.candidate_versions([".release/v1.6.0.bak"])

    def test_resolver_only_accepts_release_notes_at_the_top_level(self) -> None:
        """发布说明文件只认 `.release/` **顶层**。

        目录层级不该影响判定：`.release/notes/v1.6.0.md` 与
        `.release/v1.6.0.md` 长得一样，认了前者就会出现"说明文件放在
        子目录里也能发版"这种说不清的行为；子目录里连"名字写坏了"都
        不报，静默得彻底。

        顶层"像是发布说明、名字却不合规"的文件（v1.6.0.bak）仍然要报错。
        """
        self.assertEqual(
            self.resolver.candidate_versions([".release/notes/v1.6.0.md"]), []
        )
        self.assertEqual(
            self.resolver.candidate_versions([".release/notes/v1.6.0.bak"]), []
        )
        self.assertEqual(
            self.resolver.candidate_versions([".release/v1.6.0.md"]),
            [(".release/v1.6.0.md", "1.6.0")],
        )
        with self.assertRaises(self.resolver.ResolveError):
            self.resolver.candidate_versions([".release/v1.6.0.bak"])

    def test_cli_accepts_a_bom_prefixed_file_list(self) -> None:
        """端到端：带 BOM 的变更文件列表必须能解析出版本号。

        剥 BOM 只有**一层**（在 candidate_versions 里逐行剥，见
        scripts/resolve_release_version.py 的 read_text 说明）。这里从
        **进程**外面走一遍：真的写一个带 BOM 的列表文件、真的调用脚本、
        看退出码和输出——这一层被删时，函数级用例会先红，但端到端这条
        能保证"整条路真的能走通"，而不只是"某个函数返回了正确的值"。
        """
        with tempfile.TemporaryDirectory(prefix="snaplite-bom-") as tmp:
            listing = Path(tmp) / "changed.txt"
            listing.write_bytes("\ufeff.release/v1.6.0.md\n".encode("utf-8"))
            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPTS / "resolve_release_version.py"),
                    "--base-version",
                    "1.6.0",
                    "--changed-files",
                    str(listing),
                ],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            self.assertEqual(result.stdout.strip(), "1.6.0")

    def test_numeric_version_drops_the_prerelease_suffix(self) -> None:
        # Inno 的 VersionInfoVersion 只接受点分数字。
        self.assertEqual(self.resolver.numeric_version("1.6.0-rc.1"), "1.6.0")
        self.assertEqual(self.resolver.numeric_version("1.6.0"), "1.6.0")

    def test_inno_uses_a_separate_numeric_version_define(self) -> None:
        installer = self._read("installer/SnapLite.iss")
        self.assertIn("MyAppVersionInfo", installer)
        self.assertIn("VersionInfoVersion={#MyAppVersionInfo}", installer)
        # 预发布后缀不能进 VersionInfoVersion：Inno 只接受点分数字，
        # 带着后缀会让安装包在最后一步编译失败。
        self.assertNotIn("VersionInfoVersion={#MyAppVersion}", installer)
        # 带后缀的版本仍然用在这些字符串字段上。
        self.assertIn("AppVersion={#MyAppVersion}", installer)
        self.assertIn("VersionInfoProductVersion={#MyAppVersion}", installer)

    def test_release_workflow_only_looks_at_added_or_renamed_files(self) -> None:
        workflow = self._read(".github/workflows/release.yml")
        code = self._yaml_without_comments(workflow)
        self.assertIn("resolve_release_version.py", workflow)
        self.assertIn("/DMyAppVersionInfo=", workflow)

        # 过滤器的**字符集合必须恰好是 {A, R}**。
        #
        # 上一轮验收实测的漏判 X6：把 AR 改成 ARD（把"被删"也算进来），
        # 只按字面量查 "--diff-filter=AR" 是拦不住的——"ARD" 里就含有
        # "AR" 这个子串。pwsh 仿真实测：before 提交里有 v1.6.0.md、
        # 本次删掉它并新增 v1.6.0-rc.2.md，ARD 版本选出来的是**被删
        # 文件的老 tag** 1.6.0——正是 resolver docstring 描述的原始缺陷，
        # 拿它 --clobber 重发就是覆盖已发布的产物。
        filters = re.findall(r"--diff-filter=([A-Za-z]+)", code)
        self.assertTrue(filters, msg="必须用 --diff-filter 过滤变更文件")
        for value in filters:
            self.assertEqual(
                set(value.upper()),
                {"A", "R"},
                msg=(
                    f"--diff-filter={value} 不对：只能认新增或改名（A 或 R）。"
                    "把 D（被删）算进来会选中被删文件的老 tag"
                ),
            )
        # 只取文件名：去掉 --name-only 会把完整 diff 写进列表文件，
        # resolver 读到的就不是文件名了（上一轮验收实测的漏判 X12）。
        self.assertRegex(
            code,
            r"--name-only",
            msg="必须只取文件名（--name-only），不能把完整 diff 写进列表文件",
        )
    def test_release_workflow_reads_non_ascii_paths_verbatim(self) -> None:
        """git 必须关掉 core.quotepath，否则非 ASCII 路径匹配不上。

        git 默认把非 ASCII 路径加引号并转义成八进制
        （".release/\\350\\257\\264..."），这种路径匹配不上"发布说明文件"，
        会被静默跳过，最后报成"没有候选"——一次莫名其妙的发布失败。

        这一条以前只改了代码、没有守卫：删掉这个开关，全部用例照样绿
        （上一轮验收实测）。而且它以前查的是**原文**：把这个开关
        只写进注释里，守卫照样全绿（上一轮验收实测的漏判 X13）。
        """
        workflow = self._read(".github/workflows/release.yml")
        code = self._yaml_without_comments(workflow)
        self.assertIn(
            "-c core.quotepath=false",
            code,
            msg="git 必须关掉 core.quotepath，否则非 ASCII 路径会被转义",
        )
        self.assertNotIn(
            "core.quotepath=true",
            workflow,
            msg="core.quotepath 不能被打开",
        )
    def test_release_workflow_never_silently_falls_back(self) -> None:
        """发布工作流不能静默回落到项目版本号。

        回落 = 拿旧 tag 配 --clobber 重发，覆盖已发布 release 的产物。
        手动触发那条路径必须显式声明 --allow-base-fallback。

        断言必须与**排版**无关：上一版靠 `split("} else {")` 切分支，
        引号风格一改、else 换行，就变成假红（上一轮验收实测：两种纯格式
        改动都让守卫变红）。这里改成"看每一处 resolver 调用本身带着什么
        开关"，不再去解析 if/else 怎么排。

        还要挡住"把开关藏进变量再展开"：只对调用文本做字面量匹配的话，
        `$flags = '--allow-base-fallback'` + `@flags` 就能绕过（上一轮
        验收实测：push 路径带回落开关，守卫仍然全绿）。所以回落开关在
        整个工作流里只能出现**一次**，且必须出现在手动那条命令里。

        上一轮验收又实测出两条更刁钻的：
        1. 把开关**拼**出来（`$flags = '--allow-base' + '-fallback'`），
           字面量一次都不出现，"只出现一次"照样成立——所以还要把关
           变量**展开**之后再查一遍；
        2. 把开关**从手动路径挪到 push 路径**（上一轮验收实测的漏判
           X5）：只按字面量分组的话，两条 invocation 依然是"一带一
           不带"，守卫照样全绿——pwsh 仿真实测 push 路径静默回落、
           job 照样是绿的。

        所以这里加一条**语义**断言：带回落开关的那条路径读的文件，
        必须**不是** git 算出来的那份变更列表；不带开关的那条必须
        读的就是它。回落开关因此不可能串到 push 路径上去。
        """
        workflow = self._read(".github/workflows/release.yml")
        code = self._yaml_without_comments(workflow)
        invocations = self._resolver_invocations(code)
        with_fallback = [
            (position, text) for position, text in invocations
            if "--allow-base-fallback" in text
        ]
        without_fallback = [
            (position, text) for position, text in invocations
            if "--allow-base-fallback" not in text
        ]
        self.assertEqual(len(with_fallback), 1, msg="只有手动触发那条路径允许显式回落")
        self.assertTrue(without_fallback, msg="push 那条路径不能带回落开关")

        git_variable = self._git_changed_files_variable(code)
        self.assertIsNotNone(
            git_variable,
            msg="必须有一处 git diff 把变更文件列表写进一个变量",
        )
        manual_variable = self._changed_files_argument(with_fallback[0][1])
        self.assertIsNotNone(
            manual_variable,
            msg="手动触发那条路径必须显式给一个文件（不能靠读标准输入）",
        )
        self.assertNotEqual(
            manual_variable,
            git_variable,
            msg=(
                "带回落开关的那条路径不能读 git 的变更列表："
                "那说明回落开关挂到了 push 路径上，"
                "push 路径会静默回落并拿旧 tag 覆盖已发布的产物"
            ),
        )
        self.assertRegex(
            code,
            r"(?:New-Item|Set-Content)[\s\S]{0,200}?-(?:File)?Path\s+"
            + re.escape(manual_variable),
            msg="手动触发那条路径必须自己造一个空列表文件",
        )
        for _position, text in without_fallback:
            self.assertEqual(
                self._changed_files_argument(text),
                git_variable,
                msg="不带回落开关的那条路径必须读 git 算出来的变更文件列表",
            )
        # 回落开关在整个工作流里只能出现一次：藏进变量再展开同样算出现。
        occurrences = len(re.findall(r"--allow-base-fallback", code))
        self.assertEqual(
            occurrences,
            1,
            msg=(
                "回落开关在整个工作流里只能出现一次（藏进变量再展开也算出现）："
                "否则 push 路径可以带着回落开关而守卫看不出来"
            ),
        )
        # 逐条把关变量展开之后再查一遍：藏进变量、用拼接拼出来的开关
        # 都必须被认出来。
        for _position, text in without_fallback:
            expanded = self._powershell_expand(text, code)
            self.assertNotIn(
                "fallback",
                self._powershell_normalize(expanded),
                msg=(
                    "push 那条路径不能带回落开关："
                    "藏进变量、用拼接拼出来都算（展开后仍要认得出这个开关）"
                ),
            )
        # 白名单：push 那条调用里只允许 --base-version 与 --changed-files。
        #
        # 这一条比"把变量展开再找开关"更靠得住：不管那个变量是怎么赋值
        # 出来的（${name} / Set-Variable / 下标 / 全局遮蔽 / 拼接），
        # 多出来的参数都会被它抓住。
        #
        # 第 14 轮验收实测：`${extraArgs} = @('--allow-base' + '-fallback')`
        # + `@extraArgs` 这一类写法，展开根本看不见，守卫全绿——而 pwsh
        # 真实仿真证明 push 路径确实静默回落、拿旧 tag 覆盖已发布产物。
        self.assertEqual(
            self._resolver_extra_arguments(
                without_fallback[0][1], {"--base-version", "--changed-files"}
            ),
            [],
            msg=(
                "push 那条路径不能有多余的参数："
                "splat（@flags）、多余的位置参数、没见过的开关都不行"
            ),
        )
        self.assertEqual(
            self._resolver_extra_arguments(
                with_fallback[0][1],
                {"--base-version", "--changed-files", "--allow-base-fallback"},
            ),
            [],
            msg="手动那条路径也不能有多余的参数",
        )
    def test_release_version_step_fails_loudly_on_any_error(self) -> None:
        """Read release version 这一步的三处失败处理必须有回归保护。

        第 14 轮验收实测：删掉其中任何一处，全部测试照样全绿——而这三处
        恰恰是唯一拦住"静默发布项目版本号"的东西。pwsh 真实仿真实测：
        三处全删之后，resolver 失败时 exit 0、`GITHUB_OUTPUT` 照样写出
        `version=1.6.0` / `tag=v1.6.0`，随后用旧 tag 配 --clobber
        覆盖已发布的产物。
        """
        workflow = self._read(".github/workflows/release.yml")
        block = self._yaml_step_run_block(workflow, "Read release version")
        self.assertTrue(block, msg="必须能找到 Read release version 这一步")
        # 只看代码：整行注释去掉，行尾注释保留（这段里没有行尾注释）。
        lines = [
            line for line in block.splitlines() if not line.strip().startswith("#")
        ]
        code = "\n".join(lines)

        # 1. 任何没接住的错误都必须中断。
        preference = re.search(
            r"\$ErrorActionPreference\s*=\s*['\"]Stop['\"]", code
        )
        self.assertIsNotNone(
            preference,
            msg=(
                "这一步必须显式 $ErrorActionPreference = 'Stop'："
                "PowerShell 默认 Continue，一条非终止错误（比如对 $null 调 "
                ".Trim()）不中断脚本，会带着旧值继续跑"
            ),
        )
        first_call = code.find("resolve_release_version.py")
        self.assertGreater(first_call, 0, msg="这一步必须真的调用版本解析脚本")
        self.assertLess(
            preference.start(),
            first_call,
            msg="$ErrorActionPreference 必须在调用解析脚本**之前**设好",
        )

        # 2. resolver 没输出时必须失败（两条路径各一处）。
        empty_guards = [
            (condition, body)
            for condition, body, _s, _e in self._powershell_if_blocks(code)
            if "-not" in condition and "$resolved" in condition
        ]
        self.assertGreaterEqual(
            len(empty_guards),
            2,
            msg="两条路径都要检查 resolver 有没有输出（只查一条，另一条还是漏的）",
        )
        for _condition, body in empty_guards:
            self.assertTrue(
                self._powershell_real_throws(body),
                msg="resolver 没输出的闸门里必须有一条真会执行的 throw",
            )

        # 3. 解析出来的版本号必须非空。
        version_guards = [
            (condition, body)
            for condition, body, _s, _e in self._powershell_if_blocks(code)
            if "IsNullOrWhiteSpace" in condition and "$releaseVersion" in condition
        ]
        self.assertTrue(
            version_guards,
            msg="必须断言解析出来的版本号非空（解析失败却继续走就会发布旧版本）",
        )
        self.assertTrue(
            any(
                self._powershell_real_throws(body) for _c, body in version_guards
            ),
            msg="版本号非空的闸门里必须有一条真会执行的 throw",
        )

    def test_release_workflow_fails_loudly_when_the_resolver_fails(self) -> None:
        """resolver 失败必须让 job 变红，不能静默回落到项目版本号。

        删掉 $LASTEXITCODE 检查之后，工作流会把项目版本号写进
        GITHUB_OUTPUT（version=1.6.0），"没有发布说明文件"这件事被静默
        吞掉，流程照样往下走——正是 resolve_release_version.py 存在的理由
        （上一轮验收实测：pwsh 仿真下 GITHUB_OUTPUT=version=1.6.0）。
        """
        workflow = self._read(".github/workflows/release.yml")
        # 检查窗口必须读**剥掉注释之后**的代码：一句
        # "# 这里必须检查 $LASTEXITCODE 并 throw" 就能把守卫满足掉，
        # 而真正的检查已经被删了（上一轮验收实测的漏判 P06）。
        code = self._yaml_without_comments(workflow)
        invocations = self._resolver_invocations(code)
        self.assertTrue(invocations, msg="工作流里必须真的调用版本解析脚本")
        for index, (position, text) in enumerate(invocations):
            # 检查窗口到**下一条**解析命令为止：不能把下一条命令的退出码
            # 检查算成这一条的（那样删掉这一条的检查也不会红）。
            limit = 300
            if index + 1 < len(invocations):
                limit = min(limit, invocations[index + 1][0] - position)
            after = self._text_after(code, position, len(text), limit)
            self.assertIn(
                "$LASTEXITCODE",
                after,
                msg="resolver 失败必须检查 $LASTEXITCODE",
            )
            self.assertIn(
                "throw",
                after,
                msg="resolver 失败必须 throw，让 job 变红",
            )
            # 更严一点：检查和 throw 必须在**同一个 if 块**里，
            # 那个 if 的条件真的提到 $LASTEXITCODE，而且 throw 不是
            # 藏在不会执行的分支里。
            guarded = [
                (condition, body)
                for condition, body, _s, _e in self._powershell_if_blocks(after)
                if "$LASTEXITCODE" in condition
            ]
            self.assertTrue(
                guarded,
                msg="检查 $LASTEXITCODE 的那个判断必须真的存在（不能只在注释里）",
            )
            self.assertTrue(
                any(self._powershell_real_throws(body) for _c, body in guarded),
                msg=(
                    "检查 $LASTEXITCODE 的那个分支里必须有一条真会执行的 throw，"
                    "让 job 变红"
                ),
            )

        # 整条工作流里，凡是"条件提到 $LASTEXITCODE、块里有一条直接
        # throw"的判断，条件都必须是**失败条件**（退出码非零才进）。
        #
        # 上一轮验收实测的漏判 X2：把 `-ne 0` 写成 `-eq 0`，守卫只查
        # "条件里提到过 $LASTEXITCODE"，照样全绿；pwsh 仿真实测：
        # 正常发布**必然失败**（成功的退出码被当成失败）。
        for condition, body, _s, _e in self._powershell_if_blocks(code):
            if "$LASTEXITCODE" not in condition:
                continue
            if not self._powershell_real_throws(body):
                continue
            self.assertTrue(
                self._powershell_exit_code_is_failure(condition),
                msg=(
                    "检查 $LASTEXITCODE 的条件必须是失败条件"
                    "（-ne 0 / -gt 0 / 真值判断），实际是："
                    + condition.strip()
                ),
            )
    def test_release_workflow_refuses_to_republish_a_tag_from_another_commit(self) -> None:
        """tag 已存在且指向别的 commit 时必须失败（除非显式 force_rebuild）。

        产物是按**当前 commit** 构建的，而 tag 是不可变的：tag 指向别的
        commit 时，release 里的二进制与它挂着的 tag 对不上，SHA256SUMS
        也跟着变。恢复路径（重跑同一个 commit）不受影响——那时产物是
        一致的。
        """
        workflow = self._read(".github/workflows/release.yml")
        code = self._yaml_without_comments(workflow)
        # 引号风格和操作数顺序都**不该**影响判定。
        #
        # 上一轮验收实测的两条假红：'true' 改成 "true"、或者把
        # 'true' -ne $env:FORCE_REBUILD 的操作数对调——语义完全等价，
        # 守卫却变红。
        force_rebuild_pattern = (
            r"\$env:FORCE_REBUILD\s*-ne\s*['\"]true['\"]"
            r"|['\"]true['\"]\s*-ne\s*\$env:FORCE_REBUILD"
        )
        self.assertRegex(
            code,
            r"if\s*\(\s*\$tagCommit\s*-ne\s*\$env:GITHUB_SHA\s*\)",
            msg="必须比对 tag 指向的 commit 与当前 commit",
        )
        self.assertRegex(
            code,
            r"if\s*\(\s*(?:" + force_rebuild_pattern + r")\s*\)",
            msg="不勾 force_rebuild 时必须直接失败",
        )
        self.assertIn(
            "force_rebuild",
            workflow,
            msg="必须提供显式的确认开关（workflow_dispatch 的输入）",
        )
        # env 映射本身也要钉住：写成
        #   FORCE_REBUILD: ${{ inputs.force_rebuild || true }}
        # 就恒为 'true'，tag 冲突闸门永远不触发（tag 指向别的 commit 时
        # 照样 --clobber 覆盖已发布产物）。第 14 轮验收实测：这一条
        # 守卫全绿。
        self.assertRegex(
            workflow,
            r"FORCE_REBUILD:\s*\$\{\{\s*inputs\.force_rebuild\s*\}\}",
            msg=(
                "FORCE_REBUILD 必须直接来自 inputs.force_rebuild："
                "被 || true 之类的表达式顶掉就恒为真，闸门形同虚设"
            ),
        )

        # 光有这两个 if **不够**：闸门必须真的会让 job 变红。
        #
        # 把 throw 换成 Write-Host、或者把 throw 藏进 if ($false) 里，
        # 上面两个正则都照样成立，但 tag 指向别的 commit 时 job 照样是绿的，
        # 随后 `gh release upload --clobber` 静默覆盖已发布产物
        # （上一轮验收实测的两条漏判：M05b / P07）。
        blocks = self._powershell_if_blocks(code)
        gate = [
            body
            for condition, body, _s, _e in blocks
            if re.search(force_rebuild_pattern, condition)
        ]
        self.assertTrue(
            gate,
            msg="必须真的在没勾 force_rebuild 时拦下来（不能只写个空 if）",
        )
        self.assertTrue(
            any(self._powershell_real_throws(body) for body in gate),
            msg=(
                "闸门里必须有一条真会执行的 throw："
                "换成 Write-Host、藏进 if ($false)、"
                "或者藏进更深的嵌套 if 都等于没拦"
            ),
        )
        # 闸门必须**在**"tag 指向别的 commit"那个判断里面。
        outer = [
            body
            for condition, body, _s, _e in blocks
            if re.search(r"\$tagCommit\s*-ne\s*\$env:GITHUB_SHA", condition)
        ]
        self.assertTrue(outer, msg="必须真的在 tag 指向别的 commit 时做判断")
        self.assertTrue(
            any("FORCE_REBUILD" in body for body in outer),
            msg="force_rebuild 的闸门必须在 tag 指向别的 commit 那个判断里面",
        )
    def test_release_workflow_compares_version_lines_by_component(self) -> None:
        """工作流里的版本线比较必须按分量，不能用字符串前缀。

        "1.60.0" 以 "1.6.0" 开头，但主版本分量已经变了。resolver 里已经
        按分量拦了一道，工作流这里是纵深防御——纵深防御静默退化没人
        知道，所以要用断言钉住它的形态（上一轮验收实测：把它改成
        StartsWith，13 个测试全绿）。

        而且两层的口径必须**一致**：resolver 按分量比（1.06.0 与 1.6.0
        数值上相同），工作流若按字符串比就会把同一条版本线判成不同线，
        于是本来能发的版发不出去（上一轮验收实测）。
        """
        workflow = self._read(".github/workflows/release.yml")
        code = self._yaml_without_comments(workflow)
        self.assertNotIn(
            "StartsWith($baseVersion)",
            workflow,
            msg="版本线比较不能用字符串前缀：'1.60.0' 以 '1.6.0' 开头",
        )
        # 发布版本和项目版本**各自**都要先剥掉预发布后缀。
        #
        # 只断言"出现过一次 -split"是不够的：上一轮验收实测，那条断言
        # 被后面第三处（取数字版本号那行）满足了，改掉比较用的那两行
        # 也发现不了。
        self.assertRegex(
            code,
            r"\$release\w*\s*=\s*[^\n]*\$releaseVersion\s+-split\s+['\"]-['\"]",
            msg=(
                "发布版本必须先剥掉预发布后缀再比较"
                "（1.6.0-rc.1 与 1.6.0 是同一条线）"
            ),
        )
        self.assertRegex(
            code,
            r"\$base\w*\s*=\s*[^\n]*\$baseVersion\s+-split\s+['\"]-['\"]",
            msg="项目版本也必须先剥掉预发布后缀再比较",
        )
        self.assertNotIn(
            "$releaseNumeric -ne $baseNumeric",
            workflow,
            msg=(
                "不能直接比字符串：'1.06.0' 与 '1.6.0' 数值相同、字符串不同，"
                "两层的口径会不一致"
            ),
        )
        # 两侧都要转成数字分量：只转一侧就是拿数字比字符串。
        int_casts = re.findall(r"\[int\]\s*\(?\s*\$_\s*\)?", code)
        self.assertGreaterEqual(
            len(int_casts),
            2,
            msg="比较之前两侧都必须把版本分量转成数字（1.06.0 与 1.6.0 是同一个版本）",
        )
    def test_release_workflow_powershell_blocks_are_balanced(self) -> None:
        """每段 PowerShell 的花括号必须配平。

        配平不了，守卫就**看不见**那一段里的 if 块——于是"必须有
        throw"的断言会报"没有闸门"，方向是假红，可报出来的原因跟
        真正的问题没关系。更糟的是原来 _powershell_braces 直接抛
        AssertionError，整个守卫会以 ERROR 结束（上一轮验收实测）。
        """
        workflow = self._read(".github/workflows/release.yml")
        blocks = self._powershell_run_blocks(workflow)
        self.assertTrue(blocks, msg="工作流里必须有 run 块")
        for index, block in enumerate(blocks):
            masked = self._powershell_mask_literals(block)
            depth = 0
            for ch in masked:
                if ch == "{":
                    depth += 1
                elif ch == "}":
                    depth -= 1
                self.assertGreaterEqual(
                    depth,
                    0,
                    msg=f"第 {index + 1} 段 run 块多了一个右花括号",
                )
            self.assertEqual(
                depth,
                0,
                msg=f"第 {index + 1} 段 run 块的花括号不配平",
            )

    @classmethod
    def _resolver_invocations(cls, script: str) -> list[tuple[int, str]]:
        """列出脚本里每一次真正调用版本解析脚本的命令（位置 + 文本）。

        只认"用 python 解释器调用 scripts/resolve_release_version.py"的
        命令；注释里提到这个文件名不算数。

        解释器的写法必须放宽：`python` / `python3` / `py -3` /
        `python -X utf8` / `python ./scripts/…` 都是合法写法，把它们判成
        "不是调用"会让守卫在纯排版改动上假红（上一轮验收实测：5 种合法
        写法假红）。
        """
        results: list[tuple[int, str]] = []
        offset = 0
        pending_start = -1
        pending_text = ""
        for line in script.splitlines(keepends=True):
            stripped = line.strip()
            if pending_start < 0:
                if stripped and not stripped.startswith("#"):
                    pending_start = offset
                    pending_text = ""
            if not stripped.startswith("#"):
                pending_text += line
            offset += len(line)
            if pending_start < 0:
                continue
            if stripped.endswith("`"):
                # 反引号续行：还是同一条命令。
                continue
            if "resolve_release_version.py" in pending_text and re.search(
                r"(?:^|[\s`(&])(?:python[0-9.]*|py)\b", pending_text
            ):
                results.append((pending_start, pending_text))
            pending_start = -1
            pending_text = ""
        if pending_start >= 0 and pending_text:
            if "resolve_release_version.py" in pending_text and re.search(
                r"(?:^|[\s`(&])(?:python[0-9.]*|py)\b", pending_text
            ):
                results.append((pending_start, pending_text))
        return results

    @staticmethod
    def _context_before(script: str, position: int) -> str:
        """取命令之前的全部文本（用来判断这条命令处在哪个分支里）。"""
        if position < 0:
            return ""
        return script[:position]

    @staticmethod
    def _text_after(script: str, position: int, length: int, limit: int) -> str:
        """取命令之后的有限长度文本。"""
        if position < 0:
            return ""
        start = position + length
        return script[start : start + limit]

    @staticmethod
    def _yaml_without_comments(script: str) -> str:
        """去掉 YAML 注释（整行注释 + 行尾注释），行数保持不变。"""
        lines: list[str] = []
        for line in script.splitlines():
            stripped = line.lstrip()
            if stripped.startswith("#"):
                lines.append("")
                continue
            index = line.find("#")
            if index >= 0:
                before = line[:index]
                if before.count("'") % 2 == 0 and before.count('"') % 2 == 0:
                    line = before
            lines.append(line)
        return "\n".join(lines)


if __name__ == "__main__":
    unittest.main()
