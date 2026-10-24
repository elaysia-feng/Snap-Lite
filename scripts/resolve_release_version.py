"""解析要发布的版本号。

这个逻辑原先写在 .github/workflows/release.yml 的 PowerShell 里，既没有
本地测试手段，还带着两个真实缺陷：

1. `git diff --name-only` 会把**被删除**的文件也算进来，而发布说明文件
   恰好常被"删掉旧的、加上新的"。`Select-Object -Last 1` 选中哪个取决于
   路径的字节序，于是可能选中一个已经被删掉的文件——拿到的版本号是旧的，
   甚至会用旧 tag 重新发布、并覆盖已发布 release 的产物。
2. 文件名用 `GetFileNameWithoutExtension` 宽松解析，`v1.2.1`（无扩展名）
   这类名字也能蒙混过关。

这里把规则收成纯函数：只认"新增或改名"的文件、文件名必须严格匹配、版本号
必须与项目版本同一条版本线（按分量比较，不是字符串前缀）。能直接用 Python
单测覆盖输入矩阵。

三个真实缺陷都在这里堵掉：

1. `git diff --name-only` 会把**被删除**的文件也算进来，而发布说明文件
   恰好常被"删掉旧的、加上新的"。`Select-Object -Last 1` 选中哪个取决于
   路径的字节序，于是可能选中一个已经被删掉的文件——拿到的版本号是旧的，
   甚至会用旧 tag 重新发布、并覆盖已发布 release 的产物。
2. 只认 `A`（新增）会漏掉 `git mv` 改名：改名在 diff 里是 `R100`，
   `--diff-filter=A` 对它输出**空列表**，版本解析于是"没有候选"。
3. 没有候选时**静默回落**到项目版本号：一次"只改了旧说明文件"的提交
   也会触发发布，于是拿旧 tag 配 `--clobber` 覆盖已发布的产物。
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# 发布说明文件：v<主版本>.<次版本>.<修订>[-预发布后缀].<md|txt>
_RELEASE_NOTE = re.compile(
    r"^v(?P<version>\d+\.\d+\.\d+(?:-[0-9A-Za-z][0-9A-Za-z.\-]*)?)\.(?:md|txt)$"
)

# "像是发布说明、但名字不合规"的文件名。
#
# 目录里放 README.md、.gitkeep 之类是很平常的事——因为它们就发不了版属于
# 误伤，这类文件一律静默忽略。但 v1.6.0.bak、v1.6.md 这种明显是"把说明
# 文件写坏了"的，必须报错：静默跳过会让一次错误的发布看起来是成功的。
_NOTE_LIKE = re.compile(r"^v\d")

# 项目版本号（与 CMakeLists.txt 的 project(... VERSION ...) 一致）
# 必须开 re.MULTILINE：project(...) 在文件第 2 行，没有 MULTILINE 时
# 行首锚点永远匹配不上，"不给 --base-version 就自己读"这条路径是死的。
_BASE_VERSION = re.compile(
    r"^project\(SnapLite VERSION (?P<version>\d+\.\d+\.\d+)",
    re.MULTILINE,
)


class ResolveError(RuntimeError):
    """版本号无法确定。"""


def read_text(path: Path) -> str:
    """读文本文件。

    BOM 的处理只有**一层**：在 candidate_versions 里逐行剥（见那里的说明）。
    这里刻意**不**用 utf-8-sig——那会形成两层看起来都有效的防线，实际
    上一层是多余的：逐行剥已经把"第一行带 BOM"和"中间某行带 BOM"都覆盖了，
    而多一层只会让人误以为"删掉任意一层都还会被另一层挡住"，于是
    守卫里那条"两层同删才红"的用例其实只钉住了一层（上一轮验收实测：
    单独删掉 utf-8-sig，全部用例照样绿）。
    """
    return path.read_text(encoding="utf-8")


def parse_base_version(cmake_text: str) -> str:
    """从 CMakeLists.txt 的文本里取出项目版本号。"""
    match = _BASE_VERSION.search(cmake_text)
    if not match:
        raise ResolveError("无法从 CMakeLists.txt 读出项目版本号")
    return match.group("version")


def candidate_versions(changed_files: list[str]) -> list[tuple[str, str]]:
    """从变更文件列表里挑出发布说明文件，返回 (文件名, 版本号)。

    只认 `.release/` **顶层**的文件。

    目录里的**无关**文件（README.md、.gitkeep）以及**子目录**里的一切
    静默忽略：加一个无关文件就发不了版，属于误伤。但顶层"像是发布说明、
    名字却不合规"的文件（v1.6.0.bak、v1.6.md）必须报错——静默跳过会让
    "说明文件写错了名字"变成一次看起来很成功的错误发布。

    为什么只认顶层：目录层级不该影响判定。`.release/notes/v1.6.0.md` 与
    `.release/v1.6.0.md` 长得一样，认了前者就会出现"说明文件放在子目录里
    也能发版"这种说不清的行为；子目录里连"名字写坏了"都不报，静默得
    彻底。放在子目录里的话，解析结果是"没有候选"→ 响亮失败 → 一样能
    让人看见。
    """
    candidates: list[tuple[str, str]] = []
    for path in changed_files:
        # 逐行剥 BOM：变更文件列表可能来自别处拼装（管道、临时文件），
        # 第一行带 BOM 时整行都会带上。**这是唯一的 BOM 防线**，
        # 读文件时不再额外剥一层（见 read_text 的说明）。
        normalized = path.replace("\\", "/").strip().lstrip("﻿")
        if not normalized:
            continue
        if not normalized.startswith(".release/"):
            continue
        relative = normalized[len(".release/") :]
        if "/" in relative:
            # 子目录：静默忽略（见上面的说明）。
            continue
        name = relative
        match = _RELEASE_NOTE.match(name)
        if not match:
            if _NOTE_LIKE.match(name):
                raise ResolveError(f"发布说明文件名不合规范：{normalized}")
            continue
        candidates.append((normalized, match.group("version")))
    return candidates


def numeric_parts(version: str) -> tuple[int, ...]:
    """版本号的主版本分量（去掉预发布后缀）。

    分量必须写成纯数字，且**不能带前导零**："1.06.0" 几乎一定是想写
    "1.6.0" 的手误。放它过去会让同一件事在两层给出不同结论——resolver
    按分量判成"同一条版本线"（int('06') == 6），工作流按字符串判成
    "不同线"并抛错（上一轮验收实测：口径不一致）。
    """
    base = version.split("-", 1)[0]
    parts = base.split(".")
    numbers: list[int] = []
    for part in parts:
        if not re.fullmatch(r"[0-9]+", part):
            raise ResolveError(f"无法解析版本号 {version}")
        if len(part) > 1 and part.startswith("0"):
            raise ResolveError(f"版本号 {version} 的分量不能带前导零")
        numbers.append(int(part))
    return tuple(numbers)


def same_release_line(base_version: str, release_version: str) -> bool:
    """发布版本与项目版本是否属于同一条版本线。

    只做字符串前缀比较会放过 "1.6.01"——它以 "1.6.0" 开头，看起来像
    同一版本线，其实主版本分量已经变了。按分量比较才准确。

    版本号本身解析不了时按"不是同一条线"处理，不抛异常：这是个判断
    函数，调用方要的是结论。
    """
    try:
        return numeric_parts(release_version) == numeric_parts(base_version)
    except ResolveError:
        return False


def resolve_release_version(
    base_version: str,
    changed_files: list[str],
    *,
    allow_base_fallback: bool = False,
) -> str:
    """算出这次要发布的版本号。

    候选只从**新增或改名**的发布说明文件里取（调用方用
    `git diff --diff-filter=AR` 过滤；只认 `A` 会漏掉 `git mv` 改名，
    改名在 diff 里是 `R100`）。选择规则：

    1. 只保留与项目版本**同一条版本线**的说明文件：`1.6.0` 与 `1.6.0-rc.1`
       同属 1.6.0 线，`1.5.0` 不是。别的版本线的说明文件是历史遗留，
       不该影响这次发布。
    2. 同一条版本线上有多个说明文件时，**与项目版本完全相同**的那个优先
       （`v1.6.0.md` 胜过 `v1.6.0-rc.1.md`）。
    3. 仍有多个候选 → 无法判断该发哪个，报错而不是随便挑一个。
    4. 一个候选都没有 → **响亮失败**。原先这里静默回落到项目版本号，
       于是"只改了旧说明文件"这种不引入新版本的提交也会触发一次发布，
       拿旧 tag 配 `--clobber` 覆盖已发布的产物。
    """
    candidates = candidate_versions(changed_files)
    if not candidates:
        if allow_base_fallback:
            return base_version
        raise ResolveError(
            "本次变更没有新增或改名的发布说明文件（.release/v<版本>.md），"
            f"无法确定发布版本（项目版本 {base_version}）"
        )

    on_line = [
        (path, version)
        for path, version in candidates
        if same_release_line(base_version, version)
    ]
    if not on_line:
        listed = ", ".join(sorted({version for _, version in candidates}))
        raise ResolveError(
            f"发布说明的版本 {listed} 与项目版本 {base_version} 不是同一条版本线"
        )

    exact = [(path, version) for path, version in on_line if version == base_version]
    pool = exact if exact else on_line
    versions = {version for _, version in pool}
    if len(versions) > 1:
        listed = ", ".join(sorted(versions))
        raise ResolveError(f"本次变更包含多个不同版本号的发布说明：{listed}")
    return versions.pop()


def numeric_version(release_version: str) -> str:
    """给 Inno Setup 的 VersionInfoVersion 用的纯数字版本号。

    Inno 的 VersionInfoVersion 只接受"最多 4 个点分数字"，预发布后缀
    （如 1.6.0-rc.1）会让安装包在最后一步编译失败——此时 exe 已经构建好、
    CI 时间和 token 都花光了。纯数字版本留给 VersionInfoVersion，带后缀的
    版本继续用在 AppVersion / 文件名 / ProductVersion 这些字符串字段上。
    """
    base = release_version.split("-", 1)[0]
    if not re.fullmatch(r"\d+\.\d+\.\d+", base):
        raise ResolveError(f"无法从 {release_version} 推出纯数字版本号")
    return base


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--base-version",
        help="项目版本号；不给就从 CMakeLists.txt 读",
    )
    parser.add_argument(
        "--changed-files",
        help="变更文件列表文件；不给就读标准输入（每行一个）",
    )
    parser.add_argument(
        "--allow-base-fallback",
        action="store_true",
        help="找不到发布说明文件时回落到项目版本号（默认响亮失败）",
    )
    args = parser.parse_args(argv)

    try:
        if args.base_version:
            base_version = args.base_version
        else:
            repo_root = Path(__file__).resolve().parent.parent
            base_version = parse_base_version(read_text(repo_root / "CMakeLists.txt"))

        if args.changed_files:
            changed = read_text(Path(args.changed_files)).splitlines()
        elif args.allow_base_fallback:
            # 明确要走"没有候选就回落"的分支时**不碰标准输入**：
            # 在交互式终端里读 stdin 会一直等用户敲回车。
            changed = []
        else:
            # 走 stdin 时强制按 UTF-8 解码：Windows 交互式会话的默认编码
            # 是 GBK，路径里有非 ASCII 字符会被解成乱码，文件名就匹配不上了。
            raw = sys.stdin.buffer.read().decode("utf-8-sig", errors="replace")
            changed = raw.splitlines()

        release_version = resolve_release_version(
            base_version,
            changed,
            allow_base_fallback=args.allow_base_fallback,
        )
    except ResolveError as error:
        print(f"[release] {error}", file=sys.stderr)
        return 1
    except OSError as error:
        print(f"[release] 读取变更文件列表失败：{error}", file=sys.stderr)
        return 1

    print(release_version)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
