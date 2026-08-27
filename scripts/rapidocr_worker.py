from __future__ import annotations

import argparse
import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

import cv2
import numpy as np
from rapidocr import RapidOCR


@dataclass
class Token:
    text: str
    score: float
    left: float
    top: float
    right: float
    bottom: float

    @property
    def cy(self) -> float:
        return (self.top + self.bottom) * 0.5

    @property
    def height(self) -> float:
        return max(1.0, self.bottom - self.top)


def has_meaningful_char(text: str) -> bool:
    for ch in text:
        code = ord(ch)
        if ch.isalnum() or 0x3400 <= code <= 0x4DBF or 0x4E00 <= code <= 0x9FFF:
            return True
    return False


def clean_text(text: str) -> str:
    text = " ".join(text.replace("\u3000", " ").split())
    return text.strip()


def prepare_variants(image: np.ndarray) -> list[np.ndarray]:
    h, w = image.shape[:2]
    max_side = max(h, w)
    scale = 3.0 if max_side <= 900 else 2.0
    if max_side > 0:
        scale = min(scale, 2600.0 / float(max_side))
    scale = max(1.0, scale)

    enlarged = cv2.resize(
        image,
        (max(1, round(w * scale)), max(1, round(h * scale))),
        interpolation=cv2.INTER_CUBIC,
    )

    # Mild unsharp masking improves small Windows UI glyph edges without turning
    # antialiasing into heavy halos.
    blurred = cv2.GaussianBlur(enlarged, (0, 0), 1.0)
    sharpened = cv2.addWeighted(enlarged, 1.45, blurred, -0.45, 0)

    gray = cv2.cvtColor(enlarged, cv2.COLOR_BGR2GRAY)
    clahe = cv2.createCLAHE(clipLimit=1.8, tileGridSize=(8, 8)).apply(gray)
    gray_bgr = cv2.cvtColor(clahe, cv2.COLOR_GRAY2BGR)

    return [sharpened, gray_bgr]


def to_tokens(result) -> list[Token]:
    boxes = getattr(result, "boxes", None)
    texts = getattr(result, "txts", None)
    scores = getattr(result, "scores", None)
    if texts is None:
        return []

    if boxes is None:
        tokens: list[Token] = []
        for i, raw in enumerate(texts):
            text = clean_text(str(raw))
            if text and has_meaningful_char(text):
                score = float(scores[i]) if scores is not None and i < len(scores) else 1.0
                tokens.append(Token(text, score, 0.0, float(i * 40), 100.0, float(i * 40 + 30)))
        return tokens

    tokens = []
    count = min(len(boxes), len(texts))
    for i in range(count):
        text = clean_text(str(texts[i]))
        if not text or not has_meaningful_char(text):
            continue

        score = float(scores[i]) if scores is not None and i < len(scores) else 1.0
        if score < 0.38:
            continue

        pts = np.asarray(boxes[i], dtype=np.float32).reshape(-1, 2)
        left = float(np.min(pts[:, 0]))
        right = float(np.max(pts[:, 0]))
        top = float(np.min(pts[:, 1]))
        bottom = float(np.max(pts[:, 1]))
        if right <= left or bottom <= top:
            continue
        tokens.append(Token(text, score, left, top, right, bottom))
    return tokens


def result_quality(tokens: Iterable[Token]) -> float:
    total = 0.0
    count = 0
    for token in tokens:
        visible = sum(1 for ch in token.text if not ch.isspace())
        total += visible * max(0.2, token.score)
        if token.score >= 0.7:
            total += 0.6
        count += 1
    return total + min(count, 40) * 0.08


def rebuild_lines(tokens: list[Token]) -> str:
    if not tokens:
        return ""

    heights = sorted(t.height for t in tokens)
    median_height = heights[len(heights) // 2]
    y_tolerance = max(8.0, median_height * 0.58)

    rows: list[list[Token]] = []
    row_centers: list[float] = []
    for token in sorted(tokens, key=lambda t: (t.cy, t.left)):
        best = -1
        best_distance = 1e30
        for idx, center in enumerate(row_centers):
            distance = abs(token.cy - center)
            if distance <= y_tolerance and distance < best_distance:
                best = idx
                best_distance = distance
        if best < 0:
            rows.append([token])
            row_centers.append(token.cy)
        else:
            rows[best].append(token)
            row_centers[best] = sum(t.cy for t in rows[best]) / len(rows[best])

    ordered = sorted(zip(row_centers, rows), key=lambda item: item[0])
    row_heights = [max(t.bottom for t in row) - min(t.top for t in row) for _, row in ordered]
    typical_row_height = sorted(row_heights)[len(row_heights) // 2] if row_heights else median_height

    out: list[str] = []
    previous_bottom: float | None = None
    for _, row in ordered:
        row.sort(key=lambda t: t.left)
        current_top = min(t.top for t in row)
        current_bottom = max(t.bottom for t in row)

        if previous_bottom is not None:
            gap = current_top - previous_bottom
            if gap > max(typical_row_height * 1.35, median_height * 1.55):
                out.append("")

        # OCR usually returns one box per visual text line. When a UI row has
        # multiple boxes/columns, keep a readable separation without flattening
        # the whole screenshot into one sentence.
        line_parts: list[str] = []
        previous_right: float | None = None
        for token in row:
            if previous_right is not None and token.left - previous_right > median_height * 1.4:
                line_parts.append("  ")
            elif line_parts:
                line_parts.append(" ")
            line_parts.append(token.text)
            previous_right = token.right

        line = "".join(line_parts).strip()
        if line:
            out.append(line)
        previous_bottom = current_bottom

    return "\n".join(out).strip()


def recognize(engine: RapidOCR, image_path: Path) -> str:
    raw = cv2.imread(str(image_path), cv2.IMREAD_COLOR)
    if raw is None:
        raise RuntimeError("无法读取截图临时图像")

    best_tokens: list[Token] = []
    best_quality = -1.0
    for variant in prepare_variants(raw):
        result = engine(variant, use_det=True, use_cls=False, use_rec=True)
        tokens = to_tokens(result)
        quality = result_quality(tokens)
        if quality > best_quality:
            best_quality = quality
            best_tokens = tokens

    return rebuild_lines(best_tokens)


def main() -> int:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--input")
    parser.add_argument("--output")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    os.environ.setdefault("OMP_NUM_THREADS", "2")
    os.environ.setdefault("OMP_WAIT_POLICY", "PASSIVE")

    try:
        engine = RapidOCR()
        if args.self_test:
            # Construction loads/configures the packaged models and ONNX runtime.
            return 0

        if not args.input or not args.output:
            return 2

        text = recognize(engine, Path(args.input))
        Path(args.output).write_text(text, encoding="utf-8", newline="\n")
        return 0
    except Exception as exc:  # worker errors are returned through an exit code
        if args.output:
            try:
                Path(args.output).write_text("", encoding="utf-8")
            except Exception:
                pass
        print(f"SnapLiteOCR error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
