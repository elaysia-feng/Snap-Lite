from __future__ import annotations

from pathlib import Path


def replace_once(path: str, old: str, new: str, label: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match in {path}, got {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")


def main() -> None:
    # 1) History: the previous 6-entry / 32MB cap was the direct reason Ctrl+Z
    # stopped after only a few edits. Keep a practical cap, but large enough for
    # normal editing sessions.
    replace_once(
        "src/snip_window.h",
        "        constexpr size_t kMaxEntries = 6;\n"
        "        constexpr SIZE_T kMaxBytes = 32ull * 1024ull * 1024ull;",
        "        constexpr size_t kMaxEntries = 128;\n"
        "        constexpr SIZE_T kMaxBytes = 512ull * 1024ull * 1024ull;",
        "expand bitmap undo history",
    )
    replace_once(
        "src/snip_window.h",
        "        Pen,\n"
        "        Mosaic,\n"
        "        Text,",
        "        Pen,\n"
        "        Eraser,\n"
        "        Mosaic,\n"
        "        Text,",
        "add eraser tool enum",
    )

    # 2) Core eraser: preserve one pristine screenshot bitmap per active snip.
    replace_once(
        "src/snip_window_original.inc",
        "#include <string>\n"
        "#include <utility>",
        "#include <string>\n"
        "#include <unordered_map>\n"
        "#include <utility>",
        "eraser pristine map include",
    )
    replace_once(
        "src/snip_window_original.inc",
        "constexpr int kMinSelection = 8;\n",
        "constexpr int kMinSelection = 8;\n\n"
        "std::unordered_map<const SnipWindow*, HBITMAP> gPristineCaptures;\n\n"
        "HBITMAP PristineCaptureFor(const SnipWindow* owner) {\n"
        "    const auto it = gPristineCaptures.find(owner);\n"
        "    return it == gPristineCaptures.end() ? nullptr : it->second;\n"
        "}\n\n"
        "void SetPristineCapture(const SnipWindow* owner, HBITMAP bitmap) {\n"
        "    if (!owner || !bitmap) return;\n"
        "    auto [it, inserted] = gPristineCaptures.emplace(owner, bitmap);\n"
        "    if (!inserted) {\n"
        "        if (it->second) DeleteObject(it->second);\n"
        "        it->second = bitmap;\n"
        "    }\n"
        "}\n\n"
        "void ClearPristineCapture(const SnipWindow* owner) {\n"
        "    const auto it = gPristineCaptures.find(owner);\n"
        "    if (it == gPristineCaptures.end()) return;\n"
        "    if (it->second) DeleteObject(it->second);\n"
        "    gPristineCaptures.erase(it);\n"
        "}\n\n"
        "void RestoreOriginalBrush(const SnipWindow* owner, HBITMAP target, const RECT& bounds,\n"
        "                          POINT from, POINT to, int diameter) {\n"
        "    HBITMAP pristine = PristineCaptureFor(owner);\n"
        "    if (!target || !pristine) return;\n\n"
        "    const int radius = std::max(4, diameter / 2);\n"
        "    const LONG dx = to.x - from.x;\n"
        "    const LONG dy = to.y - from.y;\n"
        "    const int distance = std::max(std::abs(static_cast<int>(dx)),\n"
        "                                  std::abs(static_cast<int>(dy)));\n"
        "    const int step = std::max(1, radius / 2);\n"
        "    const int samples = std::max(1, (distance + step - 1) / step);\n\n"
        "    HDC dst = CreateCompatibleDC(nullptr);\n"
        "    HDC src = CreateCompatibleDC(nullptr);\n"
        "    if (!dst || !src) {\n"
        "        if (dst) DeleteDC(dst);\n"
        "        if (src) DeleteDC(src);\n"
        "        return;\n"
        "    }\n"
        "    const HGDIOBJ oldDst = SelectObject(dst, target);\n"
        "    const HGDIOBJ oldSrc = SelectObject(src, pristine);\n\n"
        "    for (int i = 0; i <= samples; ++i) {\n"
        "        const int x = static_cast<int>(from.x + dx * i / samples);\n"
        "        const int y = static_cast<int>(from.y + dy * i / samples);\n"
        "        RECT patch{\n"
        "            std::max(static_cast<int>(bounds.left), x - radius),\n"
        "            std::max(static_cast<int>(bounds.top), y - radius),\n"
        "            std::min(static_cast<int>(bounds.right), x + radius + 1),\n"
        "            std::min(static_cast<int>(bounds.bottom), y + radius + 1),\n"
        "        };\n"
        "        if (patch.right <= patch.left || patch.bottom <= patch.top) continue;\n\n"
        "        HRGN ellipse = CreateEllipticRgn(x - radius, y - radius,\n"
        "                                        x + radius + 1, y + radius + 1);\n"
        "        HRGN limit = CreateRectRgn(bounds.left, bounds.top, bounds.right, bounds.bottom);\n"
        "        HRGN clip = CreateRectRgn(0, 0, 0, 0);\n"
        "        if (ellipse && limit && clip) {\n"
        "            CombineRgn(clip, ellipse, limit, RGN_AND);\n"
        "            SelectClipRgn(dst, clip);\n"
        "            BitBlt(dst, patch.left, patch.top, patch.right - patch.left,\n"
        "                   patch.bottom - patch.top, src, patch.left, patch.top, SRCCOPY);\n"
        "            SelectClipRgn(dst, nullptr);\n"
        "        }\n"
        "        if (clip) DeleteObject(clip);\n"
        "        if (limit) DeleteObject(limit);\n"
        "        if (ellipse) DeleteObject(ellipse);\n"
        "    }\n\n"
        "    SelectObject(src, oldSrc);\n"
        "    SelectObject(dst, oldDst);\n"
        "    DeleteDC(src);\n"
        "    DeleteDC(dst);\n"
        "}\n",
        "pristine capture and eraser helper",
    )
    replace_once(
        "src/snip_window_original.inc",
        "SnipWindow::~SnipWindow() {\n"
        "    ReleaseFrameBuffer();",
        "SnipWindow::~SnipWindow() {\n"
        "    ClearPristineCapture(this);\n"
        "    ReleaseFrameBuffer();",
        "release pristine capture",
    )
    replace_once(
        "src/snip_window_original.inc",
        "    self->capture_ = CaptureVirtualScreen();\n"
        "    if (!self->capture_) {\n"
        "        delete self;\n"
        "        return false;\n"
        "    }\n\n"
        "    HWND hwnd = CreateWindowExW(",
        "    self->capture_ = CaptureVirtualScreen();\n"
        "    if (!self->capture_) {\n"
        "        delete self;\n"
        "        return false;\n"
        "    }\n"
        "    HBITMAP pristine = CloneBitmap(self->capture_);\n"
        "    if (!pristine) {\n"
        "        delete self;\n"
        "        return false;\n"
        "    }\n"
        "    SetPristineCapture(self, pristine);\n\n"
        "    HWND hwnd = CreateWindowExW(",
        "snapshot pristine screenshot",
    )
    replace_once(
        "src/snip_window_original.inc",
        "            if (tool_ == Tool::Pen) {\n"
        "                DrawPenSegment(drawCurrent_, point);\n"
        "            } else if (tool_ == Tool::Mosaic) {\n"
        "                ApplyMosaic(point);\n"
        "            }",
        "            if (tool_ == Tool::Pen) {\n"
        "                DrawPenSegment(drawCurrent_, point);\n"
        "            } else if (tool_ == Tool::Eraser) {\n"
        "                RestoreOriginalBrush(this, capture_, NormalizedSelection(),\n"
        "                                     drawCurrent_, point,\n"
        "                                     std::clamp(detail::gStrokeWidth * 4, 8, 48));\n"
        "            } else if (tool_ == Tool::Mosaic) {\n"
        "                ApplyMosaic(point);\n"
        "            }",
        "erase while dragging",
    )
    replace_once(
        "src/snip_window_original.inc",
        "            if (tool_ == Tool::Mosaic) {\n"
        "                ApplyMosaic(drawStart_);\n"
        "            }\n"
        "            return 0;",
        "            if (tool_ == Tool::Mosaic) {\n"
        "                ApplyMosaic(drawStart_);\n"
        "            } else if (tool_ == Tool::Eraser) {\n"
        "                RestoreOriginalBrush(this, capture_, NormalizedSelection(),\n"
        "                                     drawStart_, drawStart_,\n"
        "                                     std::clamp(detail::gStrokeWidth * 4, 8, 48));\n"
        "            }\n"
        "            return 0;",
        "initial eraser stamp",
    )
    replace_once(
        "src/snip_window_original.inc",
        "            if (wParam == 'P') tool_ = Tool::Pen;\n"
        "            if (wParam == 'M') tool_ = Tool::Mosaic;\n"
        "            if (wParam == 'T') tool_ = Tool::Text;",
        "            if (wParam == 'P') tool_ = Tool::Pen;\n"
        "            if (wParam == 'E') tool_ = Tool::Eraser;\n"
        "            if (wParam == 'M') tool_ = Tool::Mosaic;\n"
        "            if (wParam == 'T') tool_ = Tool::Text;",
        "eraser keyboard shortcut",
    )
    replace_once(
        "src/snip_window_original.inc",
        "        // BitmapHistory::Trim enforces size > 6 OR bytes > 32MB on every\n"
        "        // push_back, so no manual trimming is needed here.",
        "        // BitmapHistory::Trim keeps a generous 128-step / 512MB session cap.\n"
        "        // This avoids the old behavior where Ctrl+Z stopped after ~6 edits.",
        "undo history comment",
    )

    # 3) Public UI tool mapping. Keep indices aligned with toolbar category order:
    # shape=0, arrow=1, pen=2, eraser=3, mosaic=4, text=5.
    replace_once(
        "src/snip_window.cpp",
        "    case Tool::Pen: detail::gActiveToolIndex = 2; break;\n"
        "    case Tool::Mosaic: detail::gActiveToolIndex = 3; break;\n"
        "    case Tool::Text: detail::gActiveToolIndex = 4; break;",
        "    case Tool::Pen: detail::gActiveToolIndex = 2; break;\n"
        "    case Tool::Eraser: detail::gActiveToolIndex = 3; break;\n"
        "    case Tool::Mosaic: detail::gActiveToolIndex = 4; break;\n"
        "    case Tool::Text: detail::gActiveToolIndex = 5; break;",
        "active eraser index",
    )
    replace_once(
        "src/snip_window.cpp",
        "    case Tool::Pen: return 2;\n"
        "    case Tool::Mosaic: return 3;\n"
        "    case Tool::Text: return 4;",
        "    case Tool::Pen: return 2;\n"
        "    case Tool::Eraser: return 3;\n"
        "    case Tool::Mosaic: return 4;\n"
        "    case Tool::Text: return 5;",
        "eraser UiActiveTool mapping",
    )
    replace_once(
        "src/snip_window.cpp",
        "    case 2: tool_ = Tool::Pen; break;\n"
        "    case 3: tool_ = Tool::Mosaic; break;\n"
        "    case 4: tool_ = Tool::Text; break;",
        "    case 2: tool_ = Tool::Pen; break;\n"
        "    case 3: tool_ = Tool::Eraser; break;\n"
        "    case 4: tool_ = Tool::Mosaic; break;\n"
        "    case 5: tool_ = Tool::Text; break;",
        "eraser UiSetTool mapping",
    )
    replace_once(
        "src/snip_window.cpp",
        "    detail::gActiveToolIndex = toolIndex >= 0 && toolIndex <= 4 ? toolIndex : -1;",
        "    detail::gActiveToolIndex = toolIndex >= 0 && toolIndex <= 5 ? toolIndex : -1;",
        "active tool index range",
    )

    # 4) Editor toolbar category and behavior.
    replace_once("src/editor_toolbar.cpp", "constexpr int kToolCount = 6;", "constexpr int kToolCount = 7;", "toolbar tool count")
    replace_once(
        "src/editor_toolbar.cpp",
        "enum class Category { Select = 0, Shape, Arrow, Pen, Mosaic, Text };",
        "enum class Category { Select = 0, Shape, Arrow, Pen, Eraser, Mosaic, Text };",
        "eraser toolbar category",
    )
    replace_once(
        "src/editor_toolbar.cpp",
        "    L\"选择\", L\"形状\", L\"箭头\", L\"画笔\", L\"马赛克\", L\"文字\"",
        "    L\"选择\", L\"形状\", L\"箭头\", L\"画笔\", L\"橡皮\", L\"马赛克\", L\"文字\"",
        "eraser toolbar label",
    )
    replace_once(
        "src/editor_toolbar.cpp",
        "    case Category::Text:\n"
        "        items = {",
        "    case Category::Eraser:\n"
        "        items = {\n"
        "            {L\"细\", ItemAction::StrokeWidth, 2, 36}, {L\"普通\", ItemAction::StrokeWidth, 4, 44},\n"
        "            {L\"粗\", ItemAction::StrokeWidth, 8, 36}, {L\"很粗\", ItemAction::StrokeWidth, 12, 44},\n"
        "        };\n"
        "        break;\n"
        "    case Category::Text:\n"
        "        items = {",
        "eraser secondary sizes",
    )
    replace_once(
        "src/editor_toolbar.cpp",
        "        return category_ == Category::Shape || category_ == Category::Arrow ||\n"
        "               category_ == Category::Pen || category_ == Category::Text;",
        "        return category_ == Category::Shape || category_ == Category::Arrow ||\n"
        "               category_ == Category::Pen || category_ == Category::Eraser ||\n"
        "               category_ == Category::Text;",
        "eraser secondary row visibility",
    )
    replace_once(
        "src/editor_toolbar.cpp",
        "        if (tool >= 0 && tool <= 4) {",
        "        if (tool >= 0 && tool <= 5) {",
        "sync category range",
    )
    replace_once(
        "src/editor_toolbar.cpp",
        "            if (tool == 4 && PtInRect(&selection, p)) {",
        "            if (tool == 5 && PtInRect(&selection, p)) {",
        "text tool shifted index",
    )
    replace_once(
        "src/editor_toolbar.cpp",
        "            if (tool >= 0 && tool <= 3 && PtInRect(&selection, p)) {",
        "            if (tool >= 0 && tool <= 4 && PtInRect(&selection, p)) {",
        "raster history eraser range",
    )
    replace_once(
        "src/editor_toolbar.cpp",
        "        case Category::Pen: return L\"画笔：选择粗细和颜色后自由绘制\";\n"
        "        case Category::Mosaic: return L\"马赛克：按住鼠标左键涂抹需要隐藏的区域\";",
        "        case Category::Pen: return L\"画笔：选择粗细和颜色后自由绘制\";\n"
        "        case Category::Eraser: return L\"橡皮：拖动恢复原始截图像素；可选择擦除粗细\";\n"
        "        case Category::Mosaic: return L\"马赛克：按住鼠标左键涂抹需要隐藏的区域\";",
        "eraser hint",
    )
    replace_once(
        "src/editor_toolbar.cpp",
        "        case Category::Pen: snip_->UiSetTool(2); DeselectText(); break;\n"
        "        case Category::Mosaic: snip_->UiSetTool(3); DeselectText(); break;\n"
        "        case Category::Text: snip_->UiSetTool(4); break;",
        "        case Category::Pen: snip_->UiSetTool(2); DeselectText(); break;\n"
        "        case Category::Eraser: snip_->UiSetTool(3); DeselectText(); break;\n"
        "        case Category::Mosaic: snip_->UiSetTool(4); DeselectText(); break;\n"
        "        case Category::Text: snip_->UiSetTool(5); break;",
        "eraser category selection",
    )

    # 5) Deterministic eraser glyph, then let the anime skin add a mascot.
    replace_once(
        "src/toolbar_icon_render_gdi.h",
        "inline void DrawMosaic(HDC dc, const RECT& r) {",
        "inline void DrawEraser(HDC dc, const RECT& r) {\n"
        "    const POINT c = Center(r);\n"
        "    POINT body[] = {\n"
        "        {c.x - 8, c.y + 4}, {c.x - 2, c.y - 7},\n"
        "        {c.x + 8, c.y - 2}, {c.x + 2, c.y + 8},\n"
        "        {c.x - 8, c.y + 4}};\n"
        "    HPEN pen = MakePen(dc, 2);\n"
        "    HGDIOBJ old = SelectObject(dc, pen);\n"
        "    Polyline(dc, body, static_cast<int>(sizeof(body) / sizeof(body[0])));\n"
        "    MoveToEx(dc, c.x - 4, c.y + 1, nullptr);\n"
        "    LineTo(dc, c.x + 5, c.y + 6);\n"
        "    SelectObject(dc, old);\n"
        "    DeleteObject(pen);\n"
        "}\n\n"
        "inline void DrawMosaic(HDC dc, const RECT& r) {",
        "eraser base glyph",
    )
    replace_once(
        "src/toolbar_icon_render_gdi.h",
        "    else if (Exact(text,count,L\"画笔\")) DrawPen(dc,*rect);\n"
        "    else if (Exact(text,count,L\"马赛克\")) DrawMosaic(dc,*rect);",
        "    else if (Exact(text,count,L\"画笔\")) DrawPen(dc,*rect);\n"
        "    else if (Exact(text,count,L\"橡皮\")) DrawEraser(dc,*rect);\n"
        "    else if (Exact(text,count,L\"马赛克\")) DrawMosaic(dc,*rect);",
        "eraser base glyph routing",
    )
    replace_once(
        "src/toolbar_icon_render_gdi_anime.h",
        "           Exact(text, count, L\"箭头\") || Exact(text, count, L\"画笔\") ||\n"
        "           Exact(text, count, L\"马赛克\") || Exact(text, count, L\"文字\") ||",
        "           Exact(text, count, L\"箭头\") || Exact(text, count, L\"画笔\") ||\n"
        "           Exact(text, count, L\"橡皮\") || Exact(text, count, L\"马赛克\") ||\n"
        "           Exact(text, count, L\"文字\") ||",
        "eraser anime primary label",
    )
    replace_once(
        "src/toolbar_icon_render_gdi_anime.h",
        "    if (Exact(text, count, L\"画笔\"))   return Girl::Yui;\n"
        "    if (Exact(text, count, L\"马赛克\")) return Girl::Mio;",
        "    if (Exact(text, count, L\"画笔\"))   return Girl::Yui;\n"
        "    if (Exact(text, count, L\"橡皮\"))   return Girl::Ritsu;\n"
        "    if (Exact(text, count, L\"马赛克\")) return Girl::Mio;",
        "eraser mascot",
    )

    # 6) Version + release notes.
    replace_once(
        "CMakeLists.txt",
        "project(SnapLite VERSION 1.3.10 LANGUAGES CXX RC)",
        "project(SnapLite VERSION 1.3.11 LANGUAGES CXX RC)",
        "bump version",
    )
    Path(".release/v1.3.11.md").write_text(
        "# Snap-Lite v1.3.11\n\n"
        "- Add a new 橡皮 tool to the compact anime toolbar.\n"
        "- Eraser restores pixels from the pristine screenshot instead of painting white.\n"
        "- Add eraser sizes and the E keyboard shortcut.\n"
        "- Expand raster undo history from 6 entries / 32MB to 128 entries / 512MB.\n"
        "- Keep existing compact character-button styling and hover function labels.\n",
        encoding="utf-8",
        newline="\n",
    )


if __name__ == "__main__":
    main()
