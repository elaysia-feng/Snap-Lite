"""Snap-Lite 的可选本地 PaddleOCR worker。

协议很小：读取一张 PNG，把按阅读顺序拼接后的文字写入 UTF-8 输出文件。
这样 C++ 主程序不需要引入 Python、JSON 或 Paddle 的头文件；worker 不存在
或执行失败时，主程序会自动回退到 Windows OCR。
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any


def _plain(value: Any) -> Any:
    """把 numpy 标量/数组和 Paddle 的结果对象转成标准 Python 对象。"""
    if hasattr(value, "tolist"):
        return _plain(value.tolist())
    if isinstance(value, dict):
        return {key: _plain(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_plain(item) for item in value]
    return value


def _result_mapping(result: Any) -> dict[str, Any]:
    """兼容 PaddleOCR 3.x Result 的 json 属性和字典结果。"""
    candidate = result
    if hasattr(candidate, "json"):
        candidate = candidate.json
        if callable(candidate):
            candidate = candidate()
    elif hasattr(candidate, "to_json"):
        candidate = candidate.to_json()
        if callable(candidate):
            candidate = candidate()

    if isinstance(candidate, str):
        candidate = json.loads(candidate)
    candidate = _plain(candidate)
    # PaddleOCR 3.x 的 json 结果有时会把识别字段包在顶层 res 中。
    if isinstance(candidate, dict) and isinstance(candidate.get("res"), dict):
        candidate = candidate["res"]
    if not isinstance(candidate, dict):
        raise ValueError("PaddleOCR 返回了无法解析的结果")
    return candidate


def _first_result(results: Any) -> dict[str, Any]:
    if isinstance(results, dict):
        return _result_mapping(results)
    iterator = iter(results)
    try:
        return _result_mapping(next(iterator))
    except StopIteration as exc:
        raise ValueError("PaddleOCR 没有返回识别结果") from exc


def _box_metrics(box: Any) -> tuple[float, float, float, float]:
    points = _plain(box)
    if not points:
        return 0.0, 0.0, 0.0, 0.0
    if len(points) >= 4 and all(isinstance(value, (int, float)) for value in points):
        return float(points[0]), float(points[1]), float(points[2]), float(points[3])
    xs = [float(point[0]) for point in points if len(point) >= 2]
    ys = [float(point[1]) for point in points if len(point) >= 2]
    if not xs or not ys:
        return 0.0, 0.0, 0.0, 0.0
    left, right = min(xs), max(xs)
    top, bottom = min(ys), max(ys)
    return left, top, right, bottom


def _join_text(left: str, right: str) -> str:
    if not left:
        return right
    if not right:
        return left
    if left[-1].isascii() and right[0].isascii() and left[-1].isalnum() and right[0].isalnum():
        return f"{left} {right}"
    return left + right


def _format_result(data: dict[str, Any]) -> str:
    texts = data.get("rec_texts") or []
    scores = data.get("rec_scores") or []
    boxes = data.get("rec_boxes") or data.get("rec_polys") or []

    items: list[dict[str, Any]] = []
    for index, raw_text in enumerate(texts):
        text = str(raw_text).strip()
        if not text:
            continue
        score = float(scores[index]) if index < len(scores) else 1.0
        left, top, right, bottom = _box_metrics(boxes[index]) if index < len(boxes) else (0, index, 0, index)
        items.append({
            "text": text,
            "score": score,
            "left": left,
            "top": top,
            "right": right,
            "bottom": bottom,
            "height": max(1.0, bottom - top),
        })

    if not items:
        return ""

    # 极低分文本通常是噪点误检；保留其余候选，避免整张截图因此变为空。
    confident = [item for item in items if item["score"] >= 0.25]
    if confident:
        items = confident
    items.sort(key=lambda item: (item["top"], item["left"]))

    lines: list[dict[str, Any]] = []
    for item in items:
        center = (item["top"] + item["bottom"]) / 2.0
        target = None
        for line in reversed(lines):
            tolerance = max(8.0, min(line["height"], item["height"]) * 0.55)
            if abs(center - line["center"]) <= tolerance:
                target = line
                break
            if item["top"] - line["bottom"] > tolerance:
                break
        if target is None:
            target = {
                "items": [],
                "center": center,
                "top": item["top"],
                "bottom": item["bottom"],
                "height": item["height"],
            }
            lines.append(target)
        target["items"].append(item)
        target["top"] = min(target["top"], item["top"])
        target["bottom"] = max(target["bottom"], item["bottom"])
        target["height"] = max(target["height"], item["height"])
        target["center"] = (target["top"] + target["bottom"]) / 2.0

    formatted: list[str] = []
    for line in lines:
        line["items"].sort(key=lambda item: item["left"])
        value = ""
        for item in line["items"]:
            value = _join_text(value, item["text"])
        if value:
            formatted.append(value)
    return "\n".join(formatted).strip()


def main() -> int:
    parser = argparse.ArgumentParser(description="Snap-Lite local PaddleOCR worker")
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--lang", default="ch")
    parser.add_argument("--cache-dir", type=Path)
    args = parser.parse_args()

    try:
        # 必须在导入 PaddleOCR 前设置，才能让发布包使用自身目录里的模型。
        if args.cache_dir:
            args.cache_dir.mkdir(parents=True, exist_ok=True)
            os.environ["PADDLE_PDX_CACHE_HOME"] = str(args.cache_dir.resolve())

        from paddleocr import PaddleOCR

        ocr = PaddleOCR(
            lang=args.lang,
            use_doc_orientation_classify=False,
            use_doc_unwarping=False,
            use_textline_orientation=False,
            engine="paddle",
            device="cpu",
        )
        text = _format_result(_first_result(ocr.predict(input=str(args.input))))
        args.output.write_text(text, encoding="utf-8", newline="\n")
        return 0
    except Exception as exc:  # noqa: BLE001 - worker must return a clean fallback signal
        print(f"Snap-Lite PaddleOCR worker failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
