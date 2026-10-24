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
    # NOTE: src/snip_window.h and src/snip_window_original.inc are no longer
    # patched here. The compressed BitmapHistory, the Eraser tool enum and the
    # take_back()-based undo/redo calls now live directly in the repository
    # sources, so the generated copies are byte-identical to src/ and the two
    # "same header, different Tool enum" variants can no longer diverge.

    # 1) Core eraser: preserve one pristine screenshot bitmap per active snip.
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
        "    // 采样点很多（400px 笔画约 100 个），每个采样点新建三个 Region 会\n"
        "    // 产生成百上千次 GDI 对象创建/销毁。这里把 Region 提到笔画级复用：\n"
        "    // 椭圆靠 OffsetRgn 平移，剪裁区每轮重算。\n"
        "    HRGN ellipse = CreateEllipticRgn(-radius, -radius, radius + 1, radius + 1);\n"
        "    HRGN limit = CreateRectRgn(bounds.left, bounds.top, bounds.right, bounds.bottom);\n"
        "    HRGN clip = CreateRectRgn(0, 0, 0, 0);\n\n"
        "    int ellipseX = 0;\n"
        "    int ellipseY = 0;\n"
        "    bool ellipsePlaced = false;\n\n"
        "    if (ellipse && limit && clip) {\n"
        "        for (int i = 0; i <= samples; ++i) {\n"
        "            const int x = static_cast<int>(from.x + dx * i / samples);\n"
        "            const int y = static_cast<int>(from.y + dy * i / samples);\n\n"
        "            if (!ellipsePlaced) {\n"
        "                OffsetRgn(ellipse, x, y);\n"
        "                ellipsePlaced = true;\n"
        "            } else {\n"
        "                OffsetRgn(ellipse, x - ellipseX, y - ellipseY);\n"
        "            }\n"
        "            ellipseX = x;\n"
        "            ellipseY = y;\n\n"
        "            RECT patch{\n"
        "                std::max(static_cast<int>(bounds.left), x - radius),\n"
        "                std::max(static_cast<int>(bounds.top), y - radius),\n"
        "                std::min(static_cast<int>(bounds.right), x + radius + 1),\n"
        "                std::min(static_cast<int>(bounds.bottom), y + radius + 1),\n"
        "            };\n"
        "            if (patch.right <= patch.left || patch.bottom <= patch.top) continue;\n\n"
        "            CombineRgn(clip, ellipse, limit, RGN_AND);\n"
        "            SelectClipRgn(dst, clip);\n"
        "            BitBlt(dst, patch.left, patch.top, patch.right - patch.left,\n"
        "                   patch.bottom - patch.top, src, patch.left, patch.top, SRCCOPY);\n"
        "            SelectClipRgn(dst, nullptr);\n"
        "        }\n"
        "    }\n\n"
        "    if (clip) DeleteObject(clip);\n"
        "    if (limit) DeleteObject(limit);\n"
        "    if (ellipse) DeleteObject(ellipse);\n\n"
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
        "                EraseArrows(drawCurrent_, point);\n"
        "                RestoreOriginalBrush(this, capture_, NormalizedSelection(),\n"
        "                                     drawCurrent_, point,\n"
        "                                     std::clamp(detail::gStrokeWidth * 4, 8, 48));\n"
        "                // 橡皮把原始像素写回 capture_，不走 BeginEdit，\n"
        "                // 合成图缓存键必须跟着变（EraseArrows 自己也会记一次）。\n"
        "                ++contentRevision_;\n"
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
        "                EraseArrows(drawStart_, drawStart_);\n"
        "                RestoreOriginalBrush(this, capture_, NormalizedSelection(),\n"
        "                                     drawStart_, drawStart_,\n"
        "                                     std::clamp(detail::gStrokeWidth * 4, 8, 48));\n"
        "                ++contentRevision_;\n"
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

    # 3) 公共 UI 工具索引由枚举统一维护，新增工具时同步更新映射。
    replace_once(
        "src/snip_window.cpp",
        "    case Tool::Pen:\n"
        "        detail::gActiveToolIndex = static_cast<int>(UiToolIndex::Pen);\n"
        "        break;\n"
        "    case Tool::Mosaic:\n"
        "        detail::gActiveToolIndex = static_cast<int>(UiToolIndex::Mosaic);\n"
        "        break;\n"
        "    case Tool::Text:\n"
        "        detail::gActiveToolIndex = static_cast<int>(UiToolIndex::Text);\n"
        "        break;",
        "    case Tool::Pen:\n"
        "        detail::gActiveToolIndex = static_cast<int>(UiToolIndex::Pen);\n"
        "        break;\n"
        "    case Tool::Eraser:\n"
        "        detail::gActiveToolIndex = static_cast<int>(UiToolIndex::Eraser);\n"
        "        break;\n"
        "    case Tool::Mosaic:\n"
        "        detail::gActiveToolIndex = static_cast<int>(UiToolIndex::Mosaic);\n"
        "        break;\n"
        "    case Tool::Text:\n"
        "        detail::gActiveToolIndex = static_cast<int>(UiToolIndex::Text);\n"
        "        break;",
        "active eraser index",
    )
    replace_once(
        "src/snip_window.cpp",
        "    case Tool::Pen: return static_cast<int>(UiToolIndex::Pen);\n"
        "    case Tool::Mosaic: return static_cast<int>(UiToolIndex::Mosaic);\n"
        "    case Tool::Text: return static_cast<int>(UiToolIndex::Text);",
        "    case Tool::Pen: return static_cast<int>(UiToolIndex::Pen);\n"
        "    case Tool::Eraser: return static_cast<int>(UiToolIndex::Eraser);\n"
        "    case Tool::Mosaic: return static_cast<int>(UiToolIndex::Mosaic);\n"
        "    case Tool::Text: return static_cast<int>(UiToolIndex::Text);",
        "eraser UiActiveTool mapping",
    )
    replace_once(
        "src/snip_window.cpp",
        "    case UiToolIndex::Pen: tool_ = Tool::Pen; break;\n"
        "    case UiToolIndex::Mosaic: tool_ = Tool::Mosaic; break;\n"
        "    case UiToolIndex::Text: tool_ = Tool::Text; break;",
        "    case UiToolIndex::Pen: tool_ = Tool::Pen; break;\n"
        "    case UiToolIndex::Eraser: tool_ = Tool::Eraser; break;\n"
        "    case UiToolIndex::Mosaic: tool_ = Tool::Mosaic; break;\n"
        "    case UiToolIndex::Text: tool_ = Tool::Text; break;",
        "eraser UiSetTool mapping",
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
        "    case SnipWindow::UiToolIndex::Pen: return Category::Pen;\n"
        "    case SnipWindow::UiToolIndex::Mosaic: return Category::Mosaic;",
        "    case SnipWindow::UiToolIndex::Pen: return Category::Pen;\n"
        "    case SnipWindow::UiToolIndex::Eraser: return Category::Eraser;\n"
        "    case SnipWindow::UiToolIndex::Mosaic: return Category::Mosaic;",
        "eraser toolbar tool-index mapping",
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
        "        case Category::Pen: return L\"画笔：选择粗细和颜色后自由绘制\";\n"
        "        case Category::Mosaic: return L\"马赛克：按住鼠标左键涂抹需要隐藏的区域\";",
        "        case Category::Pen: return L\"画笔：选择粗细和颜色后自由绘制\";\n"
        "        case Category::Eraser: return L\"橡皮：拖动恢复原始截图像素；可选择擦除粗细\";\n"
        "        case Category::Mosaic: return L\"马赛克：按住鼠标左键涂抹需要隐藏的区域\";",
        "eraser hint",
    )
    replace_once(
        "src/editor_toolbar.cpp",
        "        case Category::Pen:\n"
        "            snip_->UiSetTool(static_cast<int>(SnipWindow::UiToolIndex::Pen));\n"
        "            DeselectText();\n"
        "            break;\n"
        "        case Category::Mosaic:\n"
        "            snip_->UiSetTool(static_cast<int>(SnipWindow::UiToolIndex::Mosaic));\n"
        "            DeselectText();\n"
        "            break;\n"
        "        case Category::Text:\n"
        "            snip_->UiSetTool(static_cast<int>(SnipWindow::UiToolIndex::Text));\n"
        "            break;",
        "        case Category::Pen:\n"
        "            snip_->UiSetTool(static_cast<int>(SnipWindow::UiToolIndex::Pen));\n"
        "            DeselectText();\n"
        "            break;\n"
        "        case Category::Eraser:\n"
        "            snip_->UiSetTool(static_cast<int>(SnipWindow::UiToolIndex::Eraser));\n"
        "            DeselectText();\n"
        "            break;\n"
        "        case Category::Mosaic:\n"
        "            snip_->UiSetTool(static_cast<int>(SnipWindow::UiToolIndex::Mosaic));\n"
        "            DeselectText();\n"
        "            break;\n"
        "        case Category::Text:\n"
        "            snip_->UiSetTool(static_cast<int>(SnipWindow::UiToolIndex::Text));\n"
        "            break;",
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
