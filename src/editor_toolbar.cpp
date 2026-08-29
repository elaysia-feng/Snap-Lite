#include "anime_toolbar.h"
#include "snip_window.h"
#include "toolbar_icon_render_gdi.h"
#include "edit_repaint_fix.h"

#include <commctrl.h>
#include <commdlg.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <imm.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace snaplite {
namespace {

constexpr wchar_t kSnipClass[] = L"SnapLiteSnipWindow";
constexpr wchar_t kToolbarClass[] = L"SnapLiteEditorToolbarChild";
constexpr wchar_t kTextClass[] = L"SnapLiteTextAnnotationChild";
constexpr wchar_t kHostProp[] = L"SnapLiteEditorToolbarHost";
constexpr UINT_PTR kParentSubclassId = 0x534C1400;
constexpr UINT_PTR kEditSubclassId = 0x534C1401;

constexpr int kToolbarWidth = 900;
constexpr int kPrimaryHeight = 42;
constexpr int kSecondaryHeight = 42;
constexpr int kHintHeight = 24;
constexpr int kToolbarHeight = kPrimaryHeight + kSecondaryHeight + kHintHeight;
constexpr int kToolCount = 6;
constexpr int kToolWidth = 78;
constexpr int kPad = 10;
constexpr int kGap = 3;
constexpr int kActionGap = 3;
// Distance in pixels the mouse must travel after LBDOWN before a click on a
// text annotation is treated as a drag rather than a select. 4 px matches the
// standard Windows drag threshold.
constexpr int kDragThreshold = 4;
// Horizontal / vertical inset used by both the in-canvas preview and the
// baked overlay so saved images match what the user saw on screen.
constexpr int kTextInsetX = 4;
constexpr int kTextInsetY = 2;
constexpr int kActionStart = kPad + kToolCount * kToolWidth + 12;

class ToolbarHost;

struct TextState {
    std::wstring text;
    POINT origin{};
    COLORREF color{RGB(235, 70, 70)};
    int sizePt{14};
};

bool SameTextState(const TextState& a, const TextState& b) {
    return a.text == b.text && a.origin.x == b.origin.x && a.origin.y == b.origin.y &&
           a.color == b.color && a.sizePt == b.sizePt;
}

bool SameTextStates(const std::vector<TextState>& a, const std::vector<TextState>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!SameTextState(a[i], b[i])) return false;
    }
    return true;
}

struct TextItem {
    ToolbarHost* host{};
    HWND hwnd{};
    HWND edit{};
    HFONT font{};
    std::wstring text;
    std::wstring editBackup;
    COLORREF editBackupColor{RGB(235, 70, 70)};
    int editBackupSizePt{14};
    POINT origin{};
    COLORREF color{RGB(235, 70, 70)};
    int sizePt{14};
    bool selected{false};
    bool dragging{false};
    bool editWasNew{false};
    POINT dragStartScreen{};
    POINT dragOrigin{};
    std::vector<TextState> editBefore;
    std::vector<TextState> dragBefore;

    ~TextItem() {
        if (font) DeleteObject(font);
    }
};

enum class Category { Select = 0, Shape, Arrow, Pen, Mosaic, Text };
enum class ItemAction {
    None,
    ShapeKind,
    ShapeFill,
    ArrowKind,
    StrokeWidth,
    TextSize,
    ColorPreset,
    ColorCustom,
};
enum class PrimaryAction { Pin = 0, Undo, Redo, Copy, Save, SaveAs, Cancel };

struct SecondaryItem {
    std::wstring label;
    ItemAction action{ItemAction::None};
    int value{};
    int width{40};
    COLORREF color{CLR_INVALID};
};

struct HistoryAction {
    enum class Kind { Raster, Text } kind{Kind::Raster};
    std::vector<TextState> before;
    std::vector<TextState> after;
};

constexpr std::array<const wchar_t*, kToolCount> kToolLabels = {
    L"选择", L"形状", L"箭头", L"画笔", L"马赛克", L"文字"
};
constexpr std::array<const wchar_t*, 7> kActionLabels = {
    L"图钉", L"撤销", L"重做", L"复制", L"保存", L"另存为", L"取消"
};
constexpr std::array<int, 7> kActionWidths = {46, 46, 46, 50, 50, 62, 50};

void AddRoundRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius) {
    path.Reset();
    const float d = radius * 2.0f;
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.GetBottom() - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void FillRoundRect(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius, const Gdiplus::Color& c) {
    Gdiplus::GraphicsPath path;
    AddRoundRect(path, r, radius);
    Gdiplus::SolidBrush brush(c);
    g.FillPath(&brush, &path);
}

std::vector<SecondaryItem> ColorItems() {
    return {
        {L"", ItemAction::ColorPreset, 0, 24, RGB(235, 70, 70)},
        {L"", ItemAction::ColorPreset, 0, 24, RGB(245, 166, 35)},
        {L"", ItemAction::ColorPreset, 0, 24, RGB(70, 170, 105)},
        {L"", ItemAction::ColorPreset, 0, 24, RGB(70, 120, 210)},
        {L"", ItemAction::ColorPreset, 0, 24, RGB(145, 105, 195)},
        {L"", ItemAction::ColorPreset, 0, 24, RGB(35, 35, 38)},
        {L"", ItemAction::ColorPreset, 0, 24, RGB(245, 245, 245)},
        {L"更多", ItemAction::ColorCustom, 0, 42},
    };
}

std::vector<SecondaryItem> BuildSecondary(Category category) {
    std::vector<SecondaryItem> items;
    switch (category) {
    case Category::Shape:
        items = {
            {L"矩形", ItemAction::ShapeKind, 0, 40}, {L"圆角", ItemAction::ShapeKind, 1, 40},
            {L"圆形", ItemAction::ShapeKind, 2, 40}, {L"椭圆", ItemAction::ShapeKind, 3, 40},
            {L"直线", ItemAction::ShapeKind, 4, 40}, {L"三角", ItemAction::ShapeKind, 5, 40},
            {L"菱形", ItemAction::ShapeKind, 6, 40}, {L"六边", ItemAction::ShapeKind, 7, 40},
            {L"描边", ItemAction::ShapeFill, 0, 40}, {L"填充", ItemAction::ShapeFill, 1, 40},
            {L"两者", ItemAction::ShapeFill, 2, 40}, {L"1", ItemAction::StrokeWidth, 1, 26},
            {L"2", ItemAction::StrokeWidth, 2, 26}, {L"4", ItemAction::StrokeWidth, 4, 26},
            {L"6", ItemAction::StrokeWidth, 6, 26},
        };
        break;
    case Category::Arrow:
        items = {
            {L"直箭头", ItemAction::ArrowKind, 0, 48}, {L"细箭头", ItemAction::ArrowKind, 1, 48},
            {L"粗箭头", ItemAction::ArrowKind, 2, 48}, {L"双向", ItemAction::ArrowKind, 3, 42},
            {L"弯曲", ItemAction::ArrowKind, 4, 42}, {L"折线", ItemAction::ArrowKind, 5, 42},
            {L"阶梯", ItemAction::ArrowKind, 6, 42}, {L"1", ItemAction::StrokeWidth, 1, 26},
            {L"2", ItemAction::StrokeWidth, 2, 26}, {L"4", ItemAction::StrokeWidth, 4, 26},
            {L"6", ItemAction::StrokeWidth, 6, 26},
        };
        break;
    case Category::Pen:
        items = {
            {L"细", ItemAction::StrokeWidth, 1, 36}, {L"普通", ItemAction::StrokeWidth, 3, 44},
            {L"粗", ItemAction::StrokeWidth, 6, 36}, {L"很粗", ItemAction::StrokeWidth, 10, 44},
        };
        break;
    case Category::Text:
        items = {
            {L"12", ItemAction::TextSize, 12, 34}, {L"14", ItemAction::TextSize, 14, 34},
            {L"16", ItemAction::TextSize, 16, 34}, {L"20", ItemAction::TextSize, 20, 34},
            {L"24", ItemAction::TextSize, 24, 34}, {L"32", ItemAction::TextSize, 32, 34},
            {L"48", ItemAction::TextSize, 48, 34},
        };
        break;
    case Category::Mosaic:
    case Category::Select:
        break;
    }

    if (category == Category::Shape || category == Category::Arrow ||
        category == Category::Pen || category == Category::Text) {
        auto colors = ColorItems();
        items.insert(items.end(), colors.begin(), colors.end());
    }
    return items;
}

LRESULT CALLBACK ToolbarWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK TextWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                 UINT_PTR, DWORD_PTR refData);
LRESULT CALLBACK ParentSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                   UINT_PTR, DWORD_PTR refData);

class ToolbarHost {
public:
    ToolbarHost(HINSTANCE instance, HWND parent, SnipWindow* snip)
        : instance_(instance), parent_(parent), snip_(snip) {}

    ~ToolbarHost() {
        if (font_) DeleteObject(font_);
        if (smallFont_) DeleteObject(smallFont_);
    }

    void AttachToolbar(HWND child) {
        toolbar_ = child;
        ApplyRoundedRegion();
        UpdatePosition();
    }

    bool HasSecondaryOptions() const {
        return category_ == Category::Shape || category_ == Category::Arrow ||
               category_ == Category::Pen || category_ == Category::Text;
    }

    int DesiredHeight() const {
        return HasSecondaryOptions() ? kToolbarHeight : kPrimaryHeight;
    }

    bool SelectionDragActive() const {
        return parent_ && GetCapture() == parent_ && snip_ && snip_->UiActiveTool() < 0;
    }

    void ApplyRoundedRegion() {
        if (!toolbar_) return;
        RECT client{};
        if (!GetClientRect(toolbar_, &client)) return;
        const int width = std::max(1L, client.right - client.left);
        const int height = std::max(1L, client.bottom - client.top);
        const int ellipse = height <= kPrimaryHeight ? 18 : 16;
        HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, ellipse, ellipse);
        if (region && SetWindowRgn(toolbar_, region, TRUE) == 0) {
            DeleteObject(region);
        }
    }

    void HideToolbarAndRestoreBackground() {
        if (!toolbar_ || !IsWindowVisible(toolbar_)) return;
        RECT screenRect{};
        if (!GetWindowRect(toolbar_, &screenRect)) {
            ShowWindow(toolbar_, SW_HIDE);
            return;
        }
        POINT topLeft{screenRect.left, screenRect.top};
        POINT bottomRight{screenRect.right, screenRect.bottom};
        ScreenToClient(parent_, &topLeft);
        ScreenToClient(parent_, &bottomRight);
        RECT dirty{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
        ShowWindow(toolbar_, SW_HIDE);
        InflateRect(&dirty, 3, 3);
        RedrawWindow(parent_, &dirty, nullptr,
                     RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOCHILDREN);
    }

    void UpdatePosition() {
        if (!toolbar_ || !snip_) return;
        if (!snip_->UiHasSelection()) {
            HideToolbarAndRestoreBackground();
            return;
        }
        if (SelectionDragActive()) {
            HideToolbarAndRestoreBackground();
            return;
        }

        const RECT selection = snip_->UiSelectionRect();
        const RECT legacy = snip_->UiLegacyToolbarRect();
        RECT client{};
        GetClientRect(parent_, &client);
        const int height = DesiredHeight();

        int x = static_cast<int>(legacy.left);
        int y = static_cast<int>(legacy.top);
        if (legacy.top < selection.top) {
            y = static_cast<int>(legacy.bottom) - height;
        }
        x = std::clamp(x, 6, std::max(6, static_cast<int>(client.right) - kToolbarWidth - 6));
        y = std::clamp(y, 6, std::max(6, static_cast<int>(client.bottom) - height - 6));

        SetWindowPos(toolbar_, HWND_TOP, x, y, kToolbarWidth, height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ApplyRoundedRegion();
    }

    void SyncCategoryFromTool() {
        if (!snip_) return;
        const int tool = snip_->UiActiveTool();
        if (tool >= 0 && tool <= 4) {
            const Category next = static_cast<Category>(tool + 1);
            if (next != category_) {
                category_ = next;
                UpdatePosition();
                InvalidateRect(toolbar_, nullptr, FALSE);
            }
        }
    }

    bool HandleParentBefore(UINT message, WPARAM wParam, LPARAM lParam) {
        if (!snip_ || !snip_->UiHasSelection()) return false;

        if (message == WM_KEYDOWN) {
            // ESC layered: cancel current edit first; otherwise fall through so
            // the parent snip window can destroy itself.
            if (wParam == VK_ESCAPE) {
                if (selectedText_ && selectedText_->edit) {
                    CancelTextEdit(selectedText_);
                    return true;
                }
                // No active edit — let the parent snip handle it.
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'Z') {
                UnifiedUndo();
                return true;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'Y') {
                UnifiedRedo();
                return true;
            }
            if (wParam == VK_DELETE && selectedText_) {
                DeleteText(selectedText_);
                return true;
            }
            if (wParam == VK_RETURN) {
                // If a text edit is active, Enter must insert a newline into
                // the multiline EDIT, not finish the snip. Click-outside or
                // Esc still drives commit/cancel.
                if (selectedText_ && selectedText_->edit) {
                    return false;
                }
                FinishWithText(SnipWindow::FinishAction::Copy);
                return true;
            }
        }

        if (message == WM_LBUTTONDBLCLK) {
            // Text annotations handle their own DBLCLK via TextWindowProc.
            // Clicks that reach the parent are in empty space — finish snip.
            POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT selection = snip_->UiSelectionRect();
            if (PtInRect(&selection, p)) {
                FinishWithText(SnipWindow::FinishAction::Copy);
                return true;
            }
        }

        if (message == WM_LBUTTONDOWN) {
            // Text annotations own their own mouse handling via TextWindowProc.
            // The parent only sees clicks that hit empty space in the selection.
            POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT selection = snip_->UiSelectionRect();
            const int tool = snip_->UiActiveTool();

            if (tool == 4 && PtInRect(&selection, p)) {
                BeginCreateText(p);
                return true;
            }

            if (tool < 0) {
                DeselectText();
            }

            if (tool >= 0 && tool <= 3 && PtInRect(&selection, p)) {
                rasterPending_ = true;
            }
        }

        return false;
    }

    void HandleParentAfter(UINT message) {
        if (message == WM_LBUTTONUP && rasterPending_) {
            rasterPending_ = false;
            HistoryAction action;
            action.kind = HistoryAction::Kind::Raster;
            undoActions_.push_back(std::move(action));
            redoActions_.clear();
        }

        if (message == WM_PAINT || message == WM_LBUTTONDOWN || message == WM_MOUSEMOVE ||
            message == WM_LBUTTONUP || message == WM_KEYDOWN || message == WM_SIZE ||
            message == WM_DPICHANGED) {
            SyncCategoryFromTool();
            UpdatePosition();
        }
    }

    LRESULT HandleToolbar(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            UpdateHover(p);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (hoverTool_ != -1 || hoverAction_ != -1 || hoverSecondary_ != -1) {
                hoverTool_ = hoverAction_ = hoverSecondary_ = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN:
            SetFocus(parent_);
            return 0;
        case WM_LBUTTONUP: {
            POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int tool = HitTool(p);
            if (tool >= 0) {
                SelectCategory(static_cast<Category>(tool));
                return 0;
            }
            const int action = HitAction(p);
            if (action >= 0) {
                ExecutePrimaryAction(static_cast<PrimaryAction>(action));
                return 0;
            }
            const int secondary = HitSecondary(p);
            if (secondary >= 0) ClickSecondary(secondary);
            return 0;
        }
        case WM_PAINT:
            PaintToolbar(hwnd);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd, message, 0, lParam);
    }

    LRESULT HandleText(TextItem* item, HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        if (!item) return DefWindowProcW(hwnd, message, wParam, lParam);

        switch (message) {
        case WM_ERASEBKGND: {
            // Repaint the real capture pixels under a transparent EDIT control.
            // This makes Backspace/Delete erase glyphs immediately instead of
            // leaving stale text until editing ends.
            HDC dc = reinterpret_cast<HDC>(wParam);
            RECT client{};
            GetClientRect(hwnd, &client);
            HBITMAP capture = snip_ ? snip_->UiCaptureBitmap() : nullptr;
            HDC source = capture ? CreateCompatibleDC(dc) : nullptr;
            if (source) {
                const HGDIOBJ old = SelectObject(source, capture);
                BitBlt(dc, 0, 0, client.right, client.bottom,
                       source, item->origin.x, item->origin.y, SRCCOPY);
                SelectObject(source, old);
                DeleteDC(source);
            }
            return 1;
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, item->color);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
        }
        case WM_SETFOCUS:
            SelectText(item);
            return 0;
        case WM_LBUTTONDOWN:
            // Threshold-based: a single click selects, a click-then-drag moves.
            if (!item->edit) {
                SelectText(item);
                SetFocus(hwnd);
                POINT screen{};
                GetCursorPos(&screen);
                pendingDrag_ = item;
                pendingDragStart_ = screen;
                pendingDragOrigin_ = item->origin;
            }
            return 0;
        case WM_MOUSEMOVE:
            if (pendingDrag_ == item && (wParam & MK_LBUTTON)) {
                POINT screen{};
                GetCursorPos(&screen);
                const int dx = screen.x - pendingDragStart_.x;
                const int dy = screen.y - pendingDragStart_.y;
                if (dx * dx + dy * dy >= kDragThreshold * kDragThreshold) {
                    item->dragging = true;
                    item->dragStartScreen = pendingDragStart_;
                    item->dragOrigin = pendingDragOrigin_;
                    item->dragBefore = SnapshotTextStates();
                    SetCapture(hwnd);
                    pendingDrag_ = nullptr;
                }
            }
            if (item->dragging && (wParam & MK_LBUTTON)) {
                POINT screen{};
                GetCursorPos(&screen);
                const int dx = screen.x - item->dragStartScreen.x;
                const int dy = screen.y - item->dragStartScreen.y;
                MoveTextItem(item, {item->dragOrigin.x + dx, item->dragOrigin.y + dy});
            }
            return 0;
        case WM_LBUTTONUP:
            if (pendingDrag_ == item) {
                // Released without exceeding drag threshold — it was a click.
                pendingDrag_ = nullptr;
            }
            if (item->dragging) {
                item->dragging = false;
                ReleaseCapture();
                RecordTextAction(item->dragBefore, SnapshotTextStates());
                item->dragBefore.clear();
            }
            return 0;
        case WM_LBUTTONDBLCLK:
            BeginTextEdit(item, false);
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_DELETE) {
                DeleteText(item);
                return 0;
            }
            if (wParam == VK_RETURN || wParam == VK_F2) {
                BeginTextEdit(item, false);
                return 0;
            }
            if (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_UP || wParam == VK_DOWN) {
                const auto before = SnapshotTextStates();
                const int step = (GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1;
                POINT next = item->origin;
                if (wParam == VK_LEFT) next.x -= step;
                if (wParam == VK_RIGHT) next.x += step;
                if (wParam == VK_UP) next.y -= step;
                if (wParam == VK_DOWN) next.y += step;
                MoveTextItem(item, next);
                RecordTextAction(before, SnapshotTextStates());
                return 0;
            }
            break;
        case WM_COMMAND:
            // EN_UPDATE is intentionally not handled here: edit_repaint_fix.h's
            // TextEditRefreshProc re-layouts and repaints deterministically after
            // any content-mutating message (WM_CHAR / WM_PASTE / WM_CUT / etc.).
            if (item->edit && reinterpret_cast<HWND>(lParam) == item->edit &&
                HIWORD(wParam) == EN_KILLFOCUS) {
                CommitTextEdit(item);
                return 0;
            }
            break;
        case WM_PAINT:
            PaintTextItem(item, hwnd);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void CommitTextEdit(TextItem* item) {
        if (!item || !item->edit) return;

        const int length = std::max(0, GetWindowTextLengthW(item->edit));
        std::wstring text(static_cast<size_t>(length) + 1, L'\0');
        if (length > 0) GetWindowTextW(item->edit, text.data(), length + 1);
        text.resize(static_cast<size_t>(length));

        HWND edit = item->edit;
        item->edit = nullptr;
        DestroyWindow(edit);

        if (text.empty()) {
            const auto before = item->editBefore;
            const bool wasNew = item->editWasNew;
            item->editWasNew = false;
            RemoveTextItem(item);
            if (!wasNew || !before.empty()) {
                RecordTextAction(before, SnapshotTextStates());
            }
            return;
        }

        item->text = text;
        item->editWasNew = false;
        ResizeTextItem(item);
        InvalidateRect(item->hwnd, nullptr, FALSE);
        RecordTextAction(item->editBefore, SnapshotTextStates());
        item->editBefore.clear();
    }

    void CancelTextEdit(TextItem* item) {
        if (!item || !item->edit) return;
        const bool wasNew = item->editWasNew;
        const COLORREF backupColor = item->editBackupColor;
        const int backupSize = item->editBackupSizePt;
        HWND edit = item->edit;
        item->edit = nullptr;
        RemoveWindowSubclass(edit, EditSubclassProc, kEditSubclassId);
        DestroyWindow(edit);
        item->editWasNew = false;
        item->editBefore.clear();

        if (wasNew) {
            RemoveTextItem(item);
            return;
        }
        item->text = item->editBackup;
        item->color = backupColor;
        item->sizePt = backupSize;
        RecreateTextFont(item);
        ResizeTextItem(item);
        InvalidateRect(item->hwnd, nullptr, FALSE);
    }

    void CommitAllTextEdits() {
        std::vector<TextItem*> editing;
        for (auto& item : texts_) {
            if (item->edit) editing.push_back(item.get());
        }
        for (TextItem* item : editing) {
            if (FindText(item)) CommitTextEdit(item);
        }
    }

private:
    void EnsureFonts() {
        const UINT dpi = GetDpiForWindow(parent_);
        if (!font_) {
            font_ = CreateFontW(-MulDiv(10, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL,
                FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        }
        if (!smallFont_) {
            smallFont_ = CreateFontW(-MulDiv(9, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL,
                FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        }
    }

    RECT ToolRect(int index) const {
        return {kPad + index * kToolWidth, 5,
                kPad + (index + 1) * kToolWidth - 4, kPrimaryHeight - 5};
    }

    RECT ActionRect(int index) const {
        int x = kActionStart;
        for (int i = 0; i < index; ++i) x += kActionWidths[i] + kActionGap;
        return {x, 5, x + kActionWidths[index], kPrimaryHeight - 5};
    }

    std::vector<RECT> SecondaryRects() const {
        const auto items = BuildSecondary(category_);
        std::vector<RECT> rects;
        int x = kPad;
        const int top = kPrimaryHeight + 5;
        for (const auto& item : items) {
            if (x + item.width > kToolbarWidth - kPad) break;
            rects.push_back({x, top, x + item.width, top + 32});
            x += item.width + kGap;
        }
        return rects;
    }

    int HitTool(POINT p) const {
        if (p.y >= kPrimaryHeight) return -1;
        for (int i = 0; i < kToolCount; ++i) {
            RECT r = ToolRect(i);
            if (PtInRect(&r, p)) return i;
        }
        return -1;
    }

    int HitAction(POINT p) const {
        if (p.y >= kPrimaryHeight) return -1;
        for (int i = 0; i < static_cast<int>(kActionLabels.size()); ++i) {
            RECT r = ActionRect(i);
            if (PtInRect(&r, p)) return i;
        }
        return -1;
    }

    int HitSecondary(POINT p) const {
        const auto rects = SecondaryRects();
        for (size_t i = 0; i < rects.size(); ++i) {
            RECT r = rects[i];
            if (PtInRect(&r, p)) return static_cast<int>(i);
        }
        return -1;
    }

    void UpdateHover(POINT p) {
        const int tool = HitTool(p);
        const int action = tool < 0 ? HitAction(p) : -1;
        const int secondary = (tool < 0 && action < 0) ? HitSecondary(p) : -1;
        if (tool != hoverTool_ || action != hoverAction_ || secondary != hoverSecondary_) {
            hoverTool_ = tool;
            hoverAction_ = action;
            hoverSecondary_ = secondary;
            InvalidateRect(toolbar_, nullptr, FALSE);
        }
    }

    bool IsSelected(const SecondaryItem& item) const {
        if (!snip_) return false;
        switch (item.action) {
        case ItemAction::ShapeKind: return snip_->UiShapeKind() == item.value;
        case ItemAction::ShapeFill: return snip_->UiShapeFillMode() == item.value;
        case ItemAction::ArrowKind: return snip_->UiArrowKind() == item.value;
        case ItemAction::StrokeWidth: return snip_->UiStrokeWidth() == item.value;
        case ItemAction::TextSize:
            return selectedText_ ? selectedText_->sizePt == item.value : snip_->UiTextSize() == item.value;
        case ItemAction::ColorPreset:
            return selectedText_ ? selectedText_->color == item.color : snip_->UiColor() == item.color;
        default:
            return false;
        }
    }

    std::wstring HintText() const {
        if (hoverAction_ >= 0) {
            switch (static_cast<PrimaryAction>(hoverAction_)) {
            case PrimaryAction::Pin: return L"图钉：把当前截图固定到桌面";
            case PrimaryAction::Undo: return L"撤销 Ctrl+Z";
            case PrimaryAction::Redo: return L"重做 Ctrl+Y";
            case PrimaryAction::Copy: return L"复制到剪贴板";
            case PrimaryAction::Save: return L"保存到默认截图目录";
            case PrimaryAction::SaveAs: return L"另存为：选择文件名和目录";
            case PrimaryAction::Cancel: return L"取消截图 Esc";
            }
        }
        switch (category_) {
        case Category::Shape: return L"形状：选择形状、填充方式、线宽和颜色后拖动绘制";
        case Category::Arrow: return L"箭头：支持直箭头、双向、弯曲、折线和阶梯箭头";
        case Category::Pen: return L"画笔：选择粗细和颜色后自由绘制";
        case Category::Mosaic: return L"马赛克：按住鼠标左键涂抹需要隐藏的区域";
        case Category::Text: return L"文字：单击创建；选中文字可改颜色/字号；双击继续编辑";
        default: return selectedText_ ? L"已选中文字：拖动移动，双击编辑，Delete 删除" : L"选择：拖动截图选区；单击文字对象可选中并移动";
        }
    }

    void PaintToolbar(HWND hwnd) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        if (!dc) return;
        EnsureFonts();

        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, kToolbarWidth, kToolbarHeight);
        if (!mem || !bitmap) {
            if (bitmap) DeleteObject(bitmap);
            if (mem) DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return;
        }
        const HGDIOBJ oldBitmap = SelectObject(mem, bitmap);
        RECT all{0, 0, kToolbarWidth, kToolbarHeight};
        HBRUSH background = CreateSolidBrush(RGB(252, 250, 247));
        FillRect(mem, &all, background);
        DeleteObject(background);

        Gdiplus::Graphics g(mem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen divider(Gdiplus::Color(255, 232, 226, 219), 1.0f);
        g.DrawLine(&divider, 10.0f, static_cast<float>(kPrimaryHeight),
                   static_cast<float>(kToolbarWidth - 10), static_cast<float>(kPrimaryHeight));
        g.DrawLine(&divider, 10.0f, static_cast<float>(kPrimaryHeight + kSecondaryHeight),
                   static_cast<float>(kToolbarWidth - 10), static_cast<float>(kPrimaryHeight + kSecondaryHeight));
        g.DrawLine(&divider, static_cast<float>(kActionStart - 7), 10.0f,
                   static_cast<float>(kActionStart - 7), static_cast<float>(kPrimaryHeight - 10));

        const HGDIOBJ oldFont = SelectObject(mem, font_);
        SetBkMode(mem, TRANSPARENT);

        for (int i = 0; i < kToolCount; ++i) {
            RECT r = ToolRect(i);
            const bool active = category_ == static_cast<Category>(i);
            const bool hovered = hoverTool_ == i;
            if (active || hovered) {
                FillRoundRect(g,
                    Gdiplus::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                                   static_cast<float>(r.right-r.left), static_cast<float>(r.bottom-r.top)),
                    7.0f, active ? Gdiplus::Color(255, 255, 232, 240)
                                 : Gdiplus::Color(255, 255, 246, 249));
            }
            SetTextColor(mem, active ? RGB(99, 82, 62) : RGB(58, 56, 53));
            toolbaricons_gdi::DrawTextOrIcon(mem, kToolLabels[i], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        for (int i = 0; i < static_cast<int>(kActionLabels.size()); ++i) {
            RECT r = ActionRect(i);
            const bool hovered = hoverAction_ == i;
            if (hovered) {
                FillRoundRect(g,
                    Gdiplus::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                                   static_cast<float>(r.right-r.left), static_cast<float>(r.bottom-r.top)),
                    7.0f, static_cast<PrimaryAction>(i) == PrimaryAction::Cancel
                              ? Gdiplus::Color(255, 255, 237, 239)
                              : Gdiplus::Color(255, 255, 246, 249));
            }
            SetTextColor(mem, static_cast<PrimaryAction>(i) == PrimaryAction::Cancel
                                  ? RGB(196, 76, 72) : RGB(70, 67, 63));
            toolbaricons_gdi::DrawTextOrIcon(mem, kActionLabels[i], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        const auto items = BuildSecondary(category_);
        const auto rects = SecondaryRects();
        SelectObject(mem, smallFont_);
        for (size_t i = 0; i < rects.size() && i < items.size(); ++i) {
            RECT r = rects[i];
            const auto& item = items[i];
            const bool selected = IsSelected(item);
            const bool hovered = hoverSecondary_ == static_cast<int>(i);

            if (item.action == ItemAction::ColorPreset) {
                const float cx = static_cast<float>(r.left + r.right) / 2.0f;
                const float cy = static_cast<float>(r.top + r.bottom) / 2.0f;
                Gdiplus::SolidBrush swatch(Gdiplus::Color(
                    255, GetRValue(item.color), GetGValue(item.color), GetBValue(item.color)));
                g.FillEllipse(&swatch, cx - 6.0f, cy - 6.0f, 12.0f, 12.0f);
                Gdiplus::Pen border(selected ? Gdiplus::Color(255, 224, 119, 151)
                                             : Gdiplus::Color(255, 218, 204, 210),
                                    selected ? 2.0f : 1.0f);
                g.DrawEllipse(&border, cx - 7.0f, cy - 7.0f, 14.0f, 14.0f);
                continue;
            }

            if (selected || hovered) {
                FillRoundRect(g,
                    Gdiplus::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                                   static_cast<float>(r.right-r.left), static_cast<float>(r.bottom-r.top)),
                    10.0f, selected ? Gdiplus::Color(255, 255, 235, 242)
                                     : Gdiplus::Color(255, 255, 248, 250));
            }
            SetTextColor(mem, selected ? RGB(91, 60, 72) : RGB(79, 70, 76));
            toolbaricons_gdi::DrawTextOrIcon(mem, item.label.c_str(), -1, &r,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        RECT hint{kPad, kPrimaryHeight + kSecondaryHeight, kToolbarWidth-kPad, kToolbarHeight};
        SetTextColor(mem, RGB(137, 119, 128));
        DrawTextW(mem, HintText().c_str(), -1, &hint,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        SelectObject(mem, oldFont);
        BitBlt(dc, 0, 0, kToolbarWidth, kToolbarHeight, mem, 0, 0, SRCCOPY);
        RECT client{};
        GetClientRect(hwnd, &client);
        HPEN border = CreatePen(PS_SOLID, 1, RGB(229, 223, 216));
        const HGDIOBJ oldPen = SelectObject(dc, border);
        const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, 0, 0, client.right, client.bottom, 18, 18);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(border);
        SelectObject(mem, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
    }

    void SelectCategory(Category category) {
        category_ = category;
        if (!snip_) return;
        switch (category) {
        case Category::Shape: snip_->UiSetTool(0); DeselectText(); break;
        case Category::Arrow: snip_->UiSetTool(1); DeselectText(); break;
        case Category::Pen: snip_->UiSetTool(2); DeselectText(); break;
        case Category::Mosaic: snip_->UiSetTool(3); DeselectText(); break;
        case Category::Text: snip_->UiSetTool(4); break;
        case Category::Select: snip_->UiSetTool(-1); break;
        }
        UpdatePosition();
        InvalidateRect(toolbar_, nullptr, FALSE);
    }

    void ExecutePrimaryAction(PrimaryAction action) {
        switch (action) {
        case PrimaryAction::Pin: FinishWithText(SnipWindow::FinishAction::Pin); break;
        case PrimaryAction::Undo: UnifiedUndo(); break;
        case PrimaryAction::Redo: UnifiedRedo(); break;
        case PrimaryAction::Copy: FinishWithText(SnipWindow::FinishAction::Copy); break;
        case PrimaryAction::Save: FinishWithText(SnipWindow::FinishAction::Save); break;
        case PrimaryAction::SaveAs: FinishWithText(SnipWindow::FinishAction::SaveAs); break;
        case PrimaryAction::Cancel: snip_->UiCancel(); break;
        }
    }

    void OpenColorPicker() {
        static COLORREF custom[16]{};
        CHOOSECOLORW choose{};
        choose.lStructSize = sizeof(choose);
        choose.hwndOwner = parent_;
        choose.rgbResult = selectedText_ ? selectedText_->color : snip_->UiColor();
        choose.lpCustColors = custom;
        choose.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (ChooseColorW(&choose)) ApplyColor(choose.rgbResult);
    }

    void ApplyColor(COLORREF color) {
        if (selectedText_) {
            selectedText_->color = color;
            if (selectedText_->edit) {
                RedrawWindow(selectedText_->hwnd, nullptr, nullptr,
                             RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            } else {
                const auto before = SnapshotTextStates();
                InvalidateRect(selectedText_->hwnd, nullptr, FALSE);
                RecordTextAction(before, SnapshotTextStates());
            }
        }
        snip_->UiSetColor(color);
        InvalidateRect(toolbar_, nullptr, FALSE);
    }

    void ApplyTextSize(int points) {
        if (selectedText_) {
            selectedText_->sizePt = std::clamp(points, 10, 72);
            RecreateTextFont(selectedText_);
            ResizeTextItem(selectedText_);
            if (selectedText_->edit && selectedText_->font) {
                SendMessageW(selectedText_->edit, WM_SETFONT,
                             reinterpret_cast<WPARAM>(selectedText_->font), TRUE);
                RedrawWindow(selectedText_->hwnd, nullptr, nullptr,
                             RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            } else {
                const auto before = SnapshotTextStates();
                RecordTextAction(before, SnapshotTextStates());
            }
        }
        snip_->UiSetTextSize(points);
        InvalidateRect(toolbar_, nullptr, FALSE);
    }

    void ClickSecondary(int index) {
        const auto rects = SecondaryRects();
        const auto items = BuildSecondary(category_);
        const size_t n = std::min(rects.size(), items.size());
        if (index < 0 || static_cast<size_t>(index) >= n || !snip_) return;
        const auto& item = items[index];
        switch (item.action) {
        case ItemAction::ShapeKind: snip_->UiSetShapeKind(item.value); break;
        case ItemAction::ShapeFill: snip_->UiSetShapeFillMode(item.value); break;
        case ItemAction::ArrowKind: snip_->UiSetArrowKind(item.value); break;
        case ItemAction::StrokeWidth: snip_->UiSetStrokeWidth(item.value); break;
        case ItemAction::TextSize: ApplyTextSize(item.value); break;
        case ItemAction::ColorPreset: ApplyColor(item.color); break;
        case ItemAction::ColorCustom: OpenColorPicker(); break;
        case ItemAction::None: default: break;
        }
        InvalidateRect(toolbar_, nullptr, FALSE);
    }

    bool FindText(TextItem* target) const {
        for (const auto& item : texts_) if (item.get() == target) return true;
        return false;
    }

    void EnsureTextFont(TextItem* item) {
        if (!item || item->font) return;
        const UINT dpi = GetDpiForWindow(parent_);
        item->font = CreateFontW(
            -MulDiv(std::clamp(item->sizePt, 10, 72), static_cast<int>(dpi), 72),
            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    }

    void RecreateTextFont(TextItem* item) {
        if (!item) return;
        if (item->font) {
            DeleteObject(item->font);
            item->font = nullptr;
        }
        EnsureTextFont(item);
    }

    SIZE MeasureText(TextItem* item) {
        SIZE size{40, 24};
        if (!item) return size;
        EnsureTextFont(item);
        // Use a memory DC so device-specific padding on the screen DC does
        // not bleed into text-width measurements (regression that showed up
        // when the parent snip window was maximised on a high-DPI display).
        HDC dc = CreateCompatibleDC(nullptr);
        if (!dc) return size;
        const HGDIOBJ oldFont = item->font ? SelectObject(dc, item->font) : nullptr;

        TEXTMETRICW metrics{};
        if (oldFont) GetTextMetricsW(dc, &metrics);
        const LONG lineHeight = std::max<LONG>(1, metrics.tmHeight);

        if (!item->text.empty()) {
            // Multiline text uses explicit \r\n separators (the EDIT control
            // gives us \r\n). Split on \n so a missing \r still renders as a
            // break, and measure each line independently so the host rect
            // grows vertically and the widest line dictates the width.
            LONG maxWidth = 0;
            int lineCount = 0;
            size_t start = 0;
            const std::wstring& text = item->text;
            while (start <= text.size()) {
                const size_t end = text.find(L'\n', start);
                const size_t chunkLen = (end == std::wstring::npos) ? (text.size() - start)
                                                                    : (end - start);
                const std::wstring line = text.substr(start, chunkLen);
                SIZE lineSize{};
                if (!line.empty()) {
                    GetTextExtentPoint32W(dc, line.c_str(), static_cast<int>(line.size()),
                                          &lineSize);
                }
                maxWidth = std::max<LONG>(maxWidth, lineSize.cx);
                ++lineCount;
                if (end == std::wstring::npos) break;
                start = end + 1;
            }
            size.cx = maxWidth;
            size.cy = static_cast<LONG>(lineCount) * lineHeight;
        }

        if (oldFont) SelectObject(dc, oldFont);
        DeleteDC(dc);
        size.cx = std::max<LONG>(40, size.cx + 12);
        size.cy = std::max<LONG>(24, size.cy + 8);
        return size;
    }

    void ClampTextOrigin(TextItem* item, POINT& origin, SIZE size) const {
        if (!item || !snip_) return;
        RECT selection = snip_->UiSelectionRect();
        const int maxX = std::max(static_cast<int>(selection.left),
                                  static_cast<int>(selection.right) - static_cast<int>(size.cx));
        const int maxY = std::max(static_cast<int>(selection.top),
                                  static_cast<int>(selection.bottom) - static_cast<int>(size.cy));
        origin.x = std::clamp(static_cast<int>(origin.x), static_cast<int>(selection.left), maxX);
        origin.y = std::clamp(static_cast<int>(origin.y), static_cast<int>(selection.top), maxY);
    }

    void ResizeTextItem(TextItem* item) {
        if (!item || !item->hwnd) return;
        SIZE size = MeasureText(item);
        POINT origin = item->origin;
        ClampTextOrigin(item, origin, size);
        item->origin = origin;
        SetWindowPos(item->hwnd, toolbar_ ? toolbar_ : HWND_TOP,
                     origin.x, origin.y, static_cast<int>(size.cx), static_cast<int>(size.cy),
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (item->edit) MoveWindow(item->edit, 0, 0, size.cx, size.cy, TRUE);
        InvalidateRect(item->hwnd, nullptr, FALSE);
    }

    TextItem* CreateTextItem(const TextState& state) {
        auto item = std::make_unique<TextItem>();
        item->host = this;
        item->text = state.text;
        item->origin = state.origin;
        item->color = state.color;
        item->sizePt = std::clamp(state.sizePt, 10, 72);
        EnsureTextFont(item.get());
        TextItem* raw = item.get();
        texts_.push_back(std::move(item));

        // Measure the initial size from the actual font/text so the host window
        // is created at the right dimensions on the first paint — no 80x28 flash
        // and no out-of-bounds origin near the bottom of the selection.
        SIZE size = MeasureText(raw);
        POINT origin = raw->origin;
        ClampTextOrigin(raw, origin, size);
        raw->origin = origin;

        raw->hwnd = CreateWindowExW(
            0,
            kTextClass,
            L"",
            WS_CHILD | WS_VISIBLE,
            raw->origin.x,
            raw->origin.y,
            static_cast<int>(size.cx),
            static_cast<int>(size.cy),
            parent_,
            nullptr,
            instance_,
            raw);

        if (!raw->hwnd) {
            texts_.pop_back();
            return nullptr;
        }
        return raw;
    }

    void BeginCreateText(POINT origin) {
        const auto before = SnapshotTextStates();
        TextState state;
        state.origin = origin;
        state.color = snip_->UiColor();
        state.sizePt = snip_->UiTextSize();
        TextItem* item = CreateTextItem(state);
        if (!item) return;
        item->editBefore = before;
        item->editWasNew = true;
        // BeginTextEdit internally calls SelectText — that keeps the category
        // and snip tool alone (continuous creation in Text mode).
        BeginTextEdit(item, true);
    }

    void BeginTextEdit(TextItem* item, bool preserveBefore) {
        if (!item || item->edit) return;
        SelectText(item);
        if (!preserveBefore) item->editBefore = SnapshotTextStates();
        item->editBackup = item->text;
        item->editBackupColor = item->color;
        item->editBackupSizePt = item->sizePt;
        EnsureTextFont(item);

        RECT client{};
        GetClientRect(item->hwnd, &client);
        item->edit = CreateWindowExW(
            0,
            L"EDIT",
            item->text.c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_AUTOVSCROLL |
                ES_MULTILINE | ES_WANTRETURN,
            0, 0, client.right, client.bottom,
            item->hwnd,
            nullptr,
            instance_,
            nullptr);
        if (!item->edit) return;

        if (item->font) SendMessageW(item->edit, WM_SETFONT, reinterpret_cast<WPARAM>(item->font), TRUE);
        SetWindowSubclass(item->edit, EditSubclassProc, kEditSubclassId,
                          reinterpret_cast<DWORD_PTR>(item));
        SendMessageW(item->edit, EM_SETSEL, 0, -1);
        SetFocus(item->edit);
        InvalidateRect(item->hwnd, nullptr, FALSE);
    }

    void MoveTextItem(TextItem* item, POINT origin) {
        if (!item || !item->hwnd) return;
        SIZE size = MeasureText(item);
        ClampTextOrigin(item, origin, size);
        item->origin = origin;
        SetWindowPos(item->hwnd, toolbar_ ? toolbar_ : HWND_TOP,
                     origin.x, origin.y, static_cast<int>(size.cx), static_cast<int>(size.cy),
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    // Hit-test: returns the topmost TextItem whose host rect contains `p` in
    // parent-client coordinates. NULL if none.
    TextItem* HitTestTexts(POINT p) {
        TextItem* found = nullptr;
        for (auto& item : texts_) {
            if (!item->hwnd) continue;
            RECT r{};
            if (!GetWindowRect(item->hwnd, &r)) continue;
            POINT pts[2]{{r.left, r.top}, {r.right, r.bottom}};
            MapWindowPoints(nullptr, parent_, pts, 2);
            RECT inParent{pts[0].x, pts[0].y, pts[1].x, pts[1].y};
            if (PtInRect(&inParent, p)) {
                found = item.get();
                // Later items paint on top — keep iterating to find the real top.
            }
        }
        return found;
    }

    void PaintTextItem(TextItem* item, HWND hwnd) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        if (!dc) return;
        RECT client{};
        GetClientRect(hwnd, &client);
        SetBkMode(dc, TRANSPARENT);
        EnsureTextFont(item);

        if (!item->edit && !item->text.empty()) {
            const HGDIOBJ oldFont = item->font ? SelectObject(dc, item->font) : nullptr;
            SetTextColor(dc, item->color);

            // Render line by line so explicit \r\n separators produce hard
            // breaks without soft-wrapping to the host rect width (per
            // user choice: Enter-only wrap, no auto-wrap).
            TEXTMETRICW metrics{};
            if (oldFont) GetTextMetricsW(dc, &metrics);
            const int lineHeight = std::max<int>(1, static_cast<int>(metrics.tmHeight));
            int y = client.top + kTextInsetY;
            const int x = client.left + kTextInsetX;

            size_t start = 0;
            const std::wstring& text = item->text;
            while (start <= text.size()) {
                const size_t end = text.find(L'\n', start);
                const size_t chunkLen = (end == std::wstring::npos)
                                            ? (text.size() - start)
                                            : (end - start);
                const std::wstring line = text.substr(start, chunkLen);
                if (!line.empty()) {
                    TextOutW(dc, x, y, line.c_str(), static_cast<int>(line.size()));
                }
                y += lineHeight;
                if (end == std::wstring::npos) break;
                start = end + 1;
            }

            if (oldFont) SelectObject(dc, oldFont);
        }

        if (item->selected && !item->edit) {
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::Pen pen(Gdiplus::Color(210, 96, 124, 180), 1.0f);
            pen.SetDashStyle(Gdiplus::DashStyleDash);
            g.DrawRectangle(&pen, 0.5f, 0.5f,
                            static_cast<float>(std::max(1L, client.right - 1)),
                            static_cast<float>(std::max(1L, client.bottom - 1)));
        }
        EndPaint(hwnd, &ps);
    }

    void SelectText(TextItem* item) {
        if (selectedText_ == item) return;
        if (selectedText_ && selectedText_->hwnd) {
            selectedText_->selected = false;
            InvalidateRect(selectedText_->hwnd, nullptr, FALSE);
        }
        selectedText_ = item;
        if (selectedText_) {
            selectedText_->selected = true;
            // Do not touch category_ or snip_->UiSetTool here. Selecting an
            // existing text annotation must not silently switch the user back
            // to the Select tool — that breaks continuous text creation.
            InvalidateRect(selectedText_->hwnd, nullptr, FALSE);
        }
        InvalidateRect(toolbar_, nullptr, FALSE);
    }

    void DeselectText() {
        if (!selectedText_) return;
        if (selectedText_->hwnd) {
            selectedText_->selected = false;
            InvalidateRect(selectedText_->hwnd, nullptr, FALSE);
        }
        selectedText_ = nullptr;
        InvalidateRect(toolbar_, nullptr, FALSE);
    }

    void RemoveTextItem(TextItem* target) {
        if (!target) return;
        // Snapshot before destruction so callers cannot forget to record undo.
        const auto before = SnapshotTextStates();
        if (selectedText_ == target) selectedText_ = nullptr;
        for (auto it = texts_.begin(); it != texts_.end(); ++it) {
            if (it->get() == target) {
                HWND hwnd = target->hwnd;
                target->hwnd = nullptr;
                target->edit = nullptr;
                if (hwnd) DestroyWindow(hwnd);
                texts_.erase(it);
                break;
            }
        }
        InvalidateRect(toolbar_, nullptr, FALSE);
        RecordTextAction(before, SnapshotTextStates());
    }

    void DeleteText(TextItem* item) {
        if (!item) return;
        // RemoveTextItem records the undo action internally.
        RemoveTextItem(item);
    }

    std::vector<TextState> SnapshotTextStates() const {
        std::vector<TextState> states;
        states.reserve(texts_.size());
        for (const auto& item : texts_) {
            if (item->text.empty() && item->editWasNew) continue;
            states.push_back({item->text, item->origin, item->color, item->sizePt});
        }
        return states;
    }

    void RestoreTextStates(const std::vector<TextState>& states) {
        selectedText_ = nullptr;
        for (auto& item : texts_) {
            if (item->hwnd) DestroyWindow(item->hwnd);
            item->hwnd = nullptr;
            item->edit = nullptr;
        }
        texts_.clear();
        for (const auto& state : states) CreateTextItem(state);
        InvalidateRect(parent_, nullptr, FALSE);
        InvalidateRect(toolbar_, nullptr, FALSE);
    }

    void RecordTextAction(const std::vector<TextState>& before, const std::vector<TextState>& after) {
        if (SameTextStates(before, after)) return;
        HistoryAction action;
        action.kind = HistoryAction::Kind::Text;
        action.before = before;
        action.after = after;
        undoActions_.push_back(std::move(action));
        redoActions_.clear();
    }

    void UnifiedUndo() {
        CommitAllTextEdits();
        if (undoActions_.empty()) {
            snip_->UiUndo();
            return;
        }
        HistoryAction action = std::move(undoActions_.back());
        undoActions_.pop_back();
        if (action.kind == HistoryAction::Kind::Raster) snip_->UiUndo();
        else RestoreTextStates(action.before);
        redoActions_.push_back(std::move(action));
    }

    void UnifiedRedo() {
        CommitAllTextEdits();
        if (redoActions_.empty()) {
            snip_->UiRedo();
            return;
        }
        HistoryAction action = std::move(redoActions_.back());
        redoActions_.pop_back();
        if (action.kind == HistoryAction::Kind::Raster) snip_->UiRedo();
        else RestoreTextStates(action.after);
        undoActions_.push_back(std::move(action));
    }

    std::vector<TextOverlay> BuildTextOverlays() const {
        std::vector<TextOverlay> overlays;
        overlays.reserve(texts_.size());
        for (const auto& item : texts_) {
            if (item->text.empty()) continue;
            // PaintTextItem renders committed text with this content inset.
            // Bake using the same coordinates so saved/copied images match preview.
            overlays.push_back({item->text,
                                {item->origin.x + kTextInsetX, item->origin.y + kTextInsetY},
                                item->color, item->sizePt});
        }
        return overlays;
    }

    void FinishWithText(SnipWindow::FinishAction action) {
        CommitAllTextEdits();
        if (!snip_) return;
        const auto overlays = BuildTextOverlays();
        if (!overlays.empty()) snip_->UiBakeTextOverlays(overlays);
        snip_->UiFinish(action);
    }

    HINSTANCE instance_{};
    HWND parent_{};
    HWND toolbar_{};
    SnipWindow* snip_{};
    HFONT font_{};
    HFONT smallFont_{};
    Category category_{Category::Select};
    int hoverTool_{-1};
    int hoverAction_{-1};
    int hoverSecondary_{-1};
    bool rasterPending_{false};
    TextItem* selectedText_{};
    // Pending click on a text annotation. Resolved to a real drag in the
    // host's WM_MOUSEMOVE once movement exceeds kDragThreshold pixels.
    TextItem* pendingDrag_{};
    POINT pendingDragStart_{};
    POINT pendingDragOrigin_{};
    std::vector<std::unique_ptr<TextItem>> texts_;
    std::vector<HistoryAction> undoActions_;
    std::vector<HistoryAction> redoActions_;
};

LRESULT CALLBACK ToolbarWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* host = reinterpret_cast<ToolbarHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        host = static_cast<ToolbarHost*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
        if (host) host->AttachToolbar(hwnd);
    }
    return host ? host->HandleToolbar(hwnd, message, wParam, lParam)
                : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK TextWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* item = reinterpret_cast<TextItem*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        item = static_cast<TextItem*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(item));
    }
    return (item && item->host) ? item->host->HandleText(item, hwnd, message, wParam, lParam)
                                : DefWindowProcW(hwnd, message, wParam, lParam);
}

namespace {

// Reposition the IME composition / candidate window so it follows the EDIT
// caret in screen coordinates. Without this, Chinese/Japanese IMEs can show
// the candidate list far from the cursor when the host window has unusual
// position/size attributes (transparent ancestors, high DPI, etc.).
void PositionImeCompositionWindow(HWND edit) {
    HIMC context = ImmGetContext(edit);
    if (!context) return;

    POINT caret{};
    if (GetCaretPos(&caret)) {
        COMPOSITIONFORM form{};
        form.dwStyle = CFS_POINT;
        ClientToScreen(edit, &caret);
        // 4 px below the caret baseline — matches Windows default heuristic.
        caret.y += 4;
        form.ptCurrentPos = caret;
        ImmSetCompositionWindow(context, &form);

        CANDIDATEFORM candidate{};
        candidate.dwStyle = CFS_CANDIDATEPOS;
        candidate.ptCurrentPos = caret;
        candidate.dwIndex = 0;
        ImmSetCandidateWindow(context, &candidate);
    }
    ImmReleaseContext(edit, context);
}

}  // namespace

LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                 UINT_PTR, DWORD_PTR refData) {
    auto* item = reinterpret_cast<TextItem*>(refData);
    if (!item || !item->host) return DefSubclassProc(hwnd, message, wParam, lParam);

    if (message == WM_KEYDOWN) {
        if (wParam == VK_ESCAPE) {
            item->host->CancelTextEdit(item);
            return 0;
        }
        // VK_RETURN is intentionally NOT intercepted here: with ES_MULTILINE |
        // ES_WANTRETURN, the EDIT inserts a newline on its own and we must
        // not commit on every Enter. Click-outside or Esc drives commit.
    }
    // Reposition IME composition window whenever the caret might have moved.
    if (message == WM_IME_COMPOSITION || message == WM_IME_STARTCOMPOSITION ||
        message == WM_IME_ENDCOMPOSITION || message == WM_SETFOCUS ||
        message == WM_KEYUP || message == WM_LBUTTONUP) {
        PositionImeCompositionWindow(hwnd);
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, EditSubclassProc, kEditSubclassId);
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK ParentSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                   UINT_PTR, DWORD_PTR refData) {
    auto* host = reinterpret_cast<ToolbarHost*>(refData);
    if (!host) return DefSubclassProc(hwnd, message, wParam, lParam);

    if (message == WM_NCDESTROY) {
        const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        RemovePropW(hwnd, kHostProp);
        RemoveWindowSubclass(hwnd, ParentSubclassProc, kParentSubclassId);
        delete host;
        return result;
    }

    if (host->HandleParentBefore(message, wParam, lParam)) return 0;
    const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
    host->HandleParentAfter(message);
    return result;
}

}  // namespace

bool AnimeToolbar::Register(HINSTANCE instance) {
    WNDCLASSEXW toolbar{};
    toolbar.cbSize = sizeof(toolbar);
    toolbar.lpfnWndProc = ToolbarWindowProc;
    toolbar.hInstance = instance;
    toolbar.hCursor = LoadCursorW(nullptr, IDC_HAND);
    toolbar.hbrBackground = nullptr;
    toolbar.lpszClassName = kToolbarClass;
    toolbar.style = CS_DBLCLKS;
    const bool toolbarOk = RegisterClassExW(&toolbar) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;

    WNDCLASSEXW text{};
    text.cbSize = sizeof(text);
    text.lpfnWndProc = TextWindowProc;
    text.hInstance = instance;
    text.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    text.hbrBackground = nullptr;
    text.lpszClassName = kTextClass;
    text.style = CS_DBLCLKS;
    const bool textOk = RegisterClassExW(&text) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;

    return toolbarOk && textOk;
}

void AnimeToolbar::ShowForSnip(HINSTANCE instance) {
    HWND parent = FindWindowW(kSnipClass, nullptr);
    if (!parent || GetPropW(parent, kHostProp)) return;
    auto* snip = reinterpret_cast<SnipWindow*>(GetWindowLongPtrW(parent, GWLP_USERDATA));
    if (!snip) return;

    const LONG_PTR style = GetWindowLongPtrW(parent, GWL_STYLE);
    SetWindowLongPtrW(parent, GWL_STYLE, style | WS_CLIPCHILDREN);

    auto* host = new ToolbarHost(instance, parent, snip);
    HWND child = CreateWindowExW(
        WS_EX_NOACTIVATE,
        kToolbarClass,
        L"",
        WS_CHILD | WS_CLIPSIBLINGS,
        0, 0, kToolbarWidth, kPrimaryHeight,
        parent,
        nullptr,
        instance,
        host);
    if (!child) {
        delete host;
        return;
    }

    HRGN region = CreateRoundRectRgn(0, 0, kToolbarWidth + 1, kToolbarHeight + 1, 18, 18);
    if (region) SetWindowRgn(child, region, TRUE);

    if (!SetWindowSubclass(parent, ParentSubclassProc, kParentSubclassId,
                           reinterpret_cast<DWORD_PTR>(host))) {
        DestroyWindow(child);
        delete host;
        return;
    }
    SetPropW(parent, kHostProp, reinterpret_cast<HANDLE>(host));
    host->UpdatePosition();
    InvalidateRect(child, nullptr, FALSE);
}

}  // namespace snaplite
