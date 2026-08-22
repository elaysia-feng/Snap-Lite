#include "anime_toolbar.h"

#include "snip_window.h"

#include <commctrl.h>
#include <commdlg.h>
#include <gdiplus.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace snaplite {
namespace {

constexpr wchar_t kSnipClass[] = L"SnapLiteSnipWindow";
constexpr wchar_t kControllerProp[] = L"SnapLiteEditorToolbarController";
constexpr UINT_PTR kSubclassId = 0x534C1300;

constexpr int kToolbarWidth = 832;
constexpr int kPrimaryHeight = 42;
constexpr int kSecondaryHeight = 42;
constexpr int kHintHeight = 24;
constexpr int kToolbarHeight = kPrimaryHeight + kSecondaryHeight + kHintHeight;
constexpr int kPrimaryCount = 8;
constexpr int kPrimaryWidth = 88;
constexpr int kPad = 10;
constexpr int kGap = 4;

enum class Category {
    Select = 0,
    Shape,
    Arrow,
    Pen,
    Mosaic,
    Text,
    Pin,
    Actions,
};

enum class ItemAction {
    None,
    ShapeKind,
    ShapeFill,
    ArrowKind,
    StrokeWidth,
    TextSize,
    ColorPreset,
    ColorCustom,
    PinNow,
    Undo,
    Redo,
    Copy,
    Save,
    SaveAs,
    Cancel,
};

struct SecondaryItem {
    std::wstring label;
    ItemAction action{ItemAction::None};
    int value{};
    int width{48};
    COLORREF color{CLR_INVALID};
};

const wchar_t* CategoryName(Category category) {
    static constexpr const wchar_t* names[kPrimaryCount] = {
        L"选择", L"形状", L"箭头", L"画笔", L"马赛克", L"文字", L"图钉", L"操作",
    };
    return names[static_cast<int>(category)];
}

void AddRoundRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect, float radius) {
    path.Reset();
    const float d = radius * 2.0f;
    path.AddArc(rect.X, rect.Y, d, d, 180.0f, 90.0f);
    path.AddArc(rect.GetRight() - d, rect.Y, d, d, 270.0f, 90.0f);
    path.AddArc(rect.GetRight() - d, rect.GetBottom() - d, d, d, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.GetBottom() - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void FillRoundRect(
    Gdiplus::Graphics& graphics,
    const Gdiplus::RectF& rect,
    float radius,
    const Gdiplus::Color& color) {
    Gdiplus::GraphicsPath path;
    AddRoundRect(path, rect, radius);
    Gdiplus::SolidBrush brush(color);
    graphics.FillPath(&brush, &path);
}

std::vector<SecondaryItem> ColorItems() {
    return {
        {L"", ItemAction::ColorPreset, 0, 30, RGB(235, 70, 70)},
        {L"", ItemAction::ColorPreset, 0, 30, RGB(245, 166, 35)},
        {L"", ItemAction::ColorPreset, 0, 30, RGB(70, 170, 105)},
        {L"", ItemAction::ColorPreset, 0, 30, RGB(70, 120, 210)},
        {L"", ItemAction::ColorPreset, 0, 30, RGB(145, 105, 195)},
        {L"", ItemAction::ColorPreset, 0, 30, RGB(35, 35, 38)},
        {L"", ItemAction::ColorPreset, 0, 30, RGB(245, 245, 245)},
        {L"更多", ItemAction::ColorCustom, 0, 52},
    };
}

std::vector<SecondaryItem> BuildSecondary(Category category) {
    std::vector<SecondaryItem> items;
    switch (category) {
    case Category::Shape:
        items = {
            {L"矩形", ItemAction::ShapeKind, 0, 54},
            {L"圆角", ItemAction::ShapeKind, 1, 54},
            {L"圆形", ItemAction::ShapeKind, 2, 54},
            {L"椭圆", ItemAction::ShapeKind, 3, 54},
            {L"直线", ItemAction::ShapeKind, 4, 54},
            {L"三角", ItemAction::ShapeKind, 5, 54},
            {L"菱形", ItemAction::ShapeKind, 6, 54},
            {L"六边形", ItemAction::ShapeKind, 7, 62},
            {L"描边", ItemAction::ShapeFill, 0, 52},
            {L"填充", ItemAction::ShapeFill, 1, 52},
            {L"两者", ItemAction::ShapeFill, 2, 52},
            {L"1", ItemAction::StrokeWidth, 1, 32},
            {L"2", ItemAction::StrokeWidth, 2, 32},
            {L"4", ItemAction::StrokeWidth, 4, 32},
            {L"6", ItemAction::StrokeWidth, 6, 32},
        };
        break;
    case Category::Arrow:
        items = {
            {L"直箭头", ItemAction::ArrowKind, 0, 62},
            {L"细箭头", ItemAction::ArrowKind, 1, 62},
            {L"粗箭头", ItemAction::ArrowKind, 2, 62},
            {L"双向", ItemAction::ArrowKind, 3, 54},
            {L"弯曲", ItemAction::ArrowKind, 4, 54},
            {L"折线", ItemAction::ArrowKind, 5, 54},
            {L"阶梯", ItemAction::ArrowKind, 6, 54},
            {L"1", ItemAction::StrokeWidth, 1, 32},
            {L"2", ItemAction::StrokeWidth, 2, 32},
            {L"4", ItemAction::StrokeWidth, 4, 32},
            {L"6", ItemAction::StrokeWidth, 6, 32},
        };
        break;
    case Category::Pen:
        items = {
            {L"细", ItemAction::StrokeWidth, 1, 42},
            {L"普通", ItemAction::StrokeWidth, 3, 52},
            {L"粗", ItemAction::StrokeWidth, 6, 42},
            {L"很粗", ItemAction::StrokeWidth, 10, 52},
        };
        break;
    case Category::Mosaic:
        items = {{L"拖动鼠标涂抹马赛克", ItemAction::None, 0, 166}};
        break;
    case Category::Text:
        items = {
            {L"12", ItemAction::TextSize, 12, 38},
            {L"14", ItemAction::TextSize, 14, 38},
            {L"16", ItemAction::TextSize, 16, 38},
            {L"20", ItemAction::TextSize, 20, 38},
            {L"24", ItemAction::TextSize, 24, 38},
            {L"32", ItemAction::TextSize, 32, 38},
            {L"48", ItemAction::TextSize, 48, 38},
        };
        break;
    case Category::Pin:
        items = {{L"贴到桌面", ItemAction::PinNow, 0, 92}};
        break;
    case Category::Actions:
        items = {
            {L"撤销", ItemAction::Undo, 0, 58},
            {L"重做", ItemAction::Redo, 0, 58},
            {L"复制", ItemAction::Copy, 0, 58},
            {L"保存", ItemAction::Save, 0, 58},
            {L"另存为", ItemAction::SaveAs, 0, 68},
            {L"取消", ItemAction::Cancel, 0, 58},
        };
        break;
    case Category::Select:
        items = {{L"拖动选区边缘调整大小，双击选区直接复制", ItemAction::None, 0, 286}};
        break;
    }

    if (category == Category::Shape || category == Category::Arrow ||
        category == Category::Pen || category == Category::Text) {
        auto colors = ColorItems();
        items.insert(items.end(), colors.begin(), colors.end());
    }
    return items;
}

class ToolbarController {
public:
    ToolbarController(HWND hwnd, SnipWindow* snip) : hwnd_(hwnd), snip_(snip) {}

    ~ToolbarController() {
        if (font_) DeleteObject(font_);
        if (smallFont_) DeleteObject(smallFont_);
    }

    static LRESULT CALLBACK SubclassProc(
        HWND hwnd,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR,
        DWORD_PTR refData) {
        auto* self = reinterpret_cast<ToolbarController*>(refData);
        if (!self) return DefSubclassProc(hwnd, message, wParam, lParam);

        if (message == WM_NCDESTROY) {
            const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
            RemovePropW(hwnd, kControllerProp);
            RemoveWindowSubclass(hwnd, SubclassProc, kSubclassId);
            delete self;
            return result;
        }

        if (message == WM_CTLCOLOREDIT && self->snip_) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, self->snip_->UiColor());
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
        }

        if (message == WM_PAINT) {
            const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
            self->SyncCategoryFromTool();
            self->Paint();
            return result;
        }

        if (!self->snip_ || !self->snip_->UiHasSelection()) {
            return DefSubclassProc(hwnd, message, wParam, lParam);
        }

        if (message == WM_MOUSEMOVE) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const RECT bar = self->ToolbarRect();
            if (PtInRect(&bar, point)) {
                self->UpdateHover(point);
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return 0;
            }
            if (self->hoverPrimary_ != -1 || self->hoverSecondary_ != -1) {
                self->hoverPrimary_ = -1;
                self->hoverSecondary_ = -1;
                InvalidateRect(hwnd, &bar, FALSE);
            }
            return DefSubclassProc(hwnd, message, wParam, lParam);
        }

        if (message == WM_SETCURSOR) {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(hwnd, &point);
            const RECT bar = self->ToolbarRect();
            if (PtInRect(&bar, point)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }

        if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const RECT bar = self->ToolbarRect();
            if (PtInRect(&bar, point)) {
                SetFocus(hwnd);
                return 0;
            }
        }

        if (message == WM_LBUTTONUP) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const RECT bar = self->ToolbarRect();
            if (PtInRect(&bar, point)) {
                const int primary = self->HitPrimary(point);
                if (primary >= 0) {
                    self->SelectCategory(static_cast<Category>(primary));
                    return 0;
                }
                const int secondary = self->HitSecondary(point);
                if (secondary >= 0) {
                    self->ClickSecondary(secondary);
                    return 0;
                }
                return 0;
            }
        }

        return DefSubclassProc(hwnd, message, wParam, lParam);
    }

private:
    void EnsureFonts() {
        const UINT dpi = GetDpiForWindow(hwnd_);
        if (!font_) {
            font_ = CreateFontW(
                -MulDiv(10, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL,
                FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        }
        if (!smallFont_) {
            smallFont_ = CreateFontW(
                -MulDiv(9, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL,
                FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        }
    }

    void SyncCategoryFromTool() {
        if (categoryLocked_) return;
        const int tool = snip_->UiActiveTool();
        if (tool >= 0 && tool <= 4) {
            category_ = static_cast<Category>(tool + 1);
        }
    }

    RECT ToolbarRect() const {
        if (!snip_ || !snip_->UiHasSelection()) return {};

        const RECT selection = snip_->UiSelectionRect();
        const RECT legacy = snip_->UiLegacyToolbarRect();
        RECT client{};
        GetClientRect(hwnd_, &client);

        int x = static_cast<int>(legacy.left);
        int y = static_cast<int>(legacy.top);
        if (legacy.top < selection.top) {
            y = static_cast<int>(legacy.bottom) - kToolbarHeight;
        }

        x = std::clamp(x, 6, std::max(6, static_cast<int>(client.right) - kToolbarWidth - 6));
        y = std::clamp(y, 6, std::max(6, static_cast<int>(client.bottom) - kToolbarHeight - 6));
        return {x, y, x + kToolbarWidth, y + kToolbarHeight};
    }

    RECT PrimaryRect(const RECT& bar, int index) const {
        return {
            bar.left + kPad + index * kPrimaryWidth,
            bar.top + 5,
            bar.left + kPad + (index + 1) * kPrimaryWidth - 4,
            bar.top + kPrimaryHeight - 5,
        };
    }

    std::vector<RECT> SecondaryRects(const RECT& bar) const {
        const auto items = BuildSecondary(category_);
        std::vector<RECT> rects;
        rects.reserve(items.size());
        int x = bar.left + kPad;
        const int top = bar.top + kPrimaryHeight + 5;
        for (const auto& item : items) {
            if (x + item.width > bar.right - kPad) break;
            rects.push_back({x, top, x + item.width, top + 32});
            x += item.width + kGap;
        }
        return rects;
    }

    int HitPrimary(POINT point) const {
        const RECT bar = ToolbarRect();
        if (point.y >= bar.top + kPrimaryHeight) return -1;
        for (int i = 0; i < kPrimaryCount; ++i) {
            RECT rect = PrimaryRect(bar, i);
            if (PtInRect(&rect, point)) return i;
        }
        return -1;
    }

    int HitSecondary(POINT point) const {
        const RECT bar = ToolbarRect();
        const auto rects = SecondaryRects(bar);
        for (size_t i = 0; i < rects.size(); ++i) {
            RECT rect = rects[i];
            if (PtInRect(&rect, point)) return static_cast<int>(i);
        }
        return -1;
    }

    void UpdateHover(POINT point) {
        const int primary = HitPrimary(point);
        const int secondary = primary < 0 ? HitSecondary(point) : -1;
        if (primary != hoverPrimary_ || secondary != hoverSecondary_) {
            hoverPrimary_ = primary;
            hoverSecondary_ = secondary;
            const RECT bar = ToolbarRect();
            InvalidateRect(hwnd_, &bar, FALSE);
        }
    }

    bool IsSecondarySelected(const SecondaryItem& item) const {
        switch (item.action) {
        case ItemAction::ShapeKind: return snip_->UiShapeKind() == item.value;
        case ItemAction::ShapeFill: return snip_->UiShapeFillMode() == item.value;
        case ItemAction::ArrowKind: return snip_->UiArrowKind() == item.value;
        case ItemAction::StrokeWidth: return snip_->UiStrokeWidth() == item.value;
        case ItemAction::TextSize: return snip_->UiTextSize() == item.value;
        case ItemAction::ColorPreset: return snip_->UiColor() == item.color;
        default: return false;
        }
    }

    void PaintPrimaryIcon(Gdiplus::Graphics& graphics, Category category, const RECT& rect, bool active) {
        const Gdiplus::Color color = active
            ? Gdiplus::Color(255, 72, 95, 135)
            : Gdiplus::Color(255, 88, 90, 96);
        Gdiplus::Pen pen(color, 1.6f);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        Gdiplus::SolidBrush brush(color);
        const float x = static_cast<float>(rect.left + 14);
        const float y = static_cast<float>((rect.top + rect.bottom) / 2);

        switch (category) {
        case Category::Select:
            graphics.DrawLine(&pen, x - 5, y - 7, x + 5, y + 5);
            graphics.DrawLine(&pen, x - 5, y - 7, x - 1, y + 7);
            break;
        case Category::Shape:
            graphics.DrawRectangle(&pen, x - 6, y - 6, 12, 12);
            break;
        case Category::Arrow:
            graphics.DrawLine(&pen, x - 7, y + 5, x + 5, y - 5);
            graphics.DrawLine(&pen, x + 5, y - 5, x, y - 5);
            graphics.DrawLine(&pen, x + 5, y - 5, x + 5, y);
            break;
        case Category::Pen:
            graphics.DrawLine(&pen, x - 6, y + 6, x + 6, y - 6);
            graphics.DrawLine(&pen, x - 7, y + 7, x - 2, y + 5);
            break;
        case Category::Mosaic:
            for (int r = 0; r < 2; ++r) for (int c = 0; c < 2; ++c)
                graphics.FillRectangle(&brush, x - 6 + c * 7, y - 6 + r * 7, 5, 5);
            break;
        case Category::Text:
            graphics.DrawLine(&pen, x - 6, y - 6, x + 6, y - 6);
            graphics.DrawLine(&pen, x, y - 6, x, y + 7);
            break;
        case Category::Pin:
            graphics.FillRectangle(&brush, x - 5, y - 6, 10, 4);
            graphics.DrawLine(&pen, x, y - 2, x, y + 7);
            break;
        case Category::Actions:
            graphics.DrawArc(&pen, x - 6, y - 6, 12, 12, 200, 230);
            break;
        }
    }

    void Paint() {
        if (!snip_ || !snip_->UiHasSelection()) return;
        EnsureFonts();
        const RECT bar = ToolbarRect();
        HDC dc = GetDC(hwnd_);
        if (!dc) return;

        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        Gdiplus::RectF shadow(
            static_cast<float>(bar.left + 1), static_cast<float>(bar.top + 3),
            static_cast<float>(bar.right - bar.left), static_cast<float>(bar.bottom - bar.top));
        FillRoundRect(graphics, shadow, 10.0f, Gdiplus::Color(45, 0, 0, 0));

        Gdiplus::RectF body(
            static_cast<float>(bar.left), static_cast<float>(bar.top),
            static_cast<float>(bar.right - bar.left), static_cast<float>(bar.bottom - bar.top));
        FillRoundRect(graphics, body, 10.0f, Gdiplus::Color(255, 250, 250, 252));
        Gdiplus::GraphicsPath bodyPath;
        AddRoundRect(bodyPath, body, 10.0f);
        Gdiplus::Pen border(Gdiplus::Color(255, 218, 220, 224), 1.0f);
        graphics.DrawPath(&border, &bodyPath);

        Gdiplus::Pen divider(Gdiplus::Color(255, 232, 233, 236), 1.0f);
        graphics.DrawLine(&divider, bar.left + 10.0f, bar.top + kPrimaryHeight,
                          bar.right - 10.0f, bar.top + kPrimaryHeight);
        graphics.DrawLine(&divider, bar.left + 10.0f, bar.top + kPrimaryHeight + kSecondaryHeight,
                          bar.right - 10.0f, bar.top + kPrimaryHeight + kSecondaryHeight);

        const HGDIOBJ oldFont = SelectObject(dc, font_);
        SetBkMode(dc, TRANSPARENT);

        for (int i = 0; i < kPrimaryCount; ++i) {
            const Category category = static_cast<Category>(i);
            RECT rect = PrimaryRect(bar, i);
            const bool active = category_ == category;
            const bool hovered = hoverPrimary_ == i;
            if (active || hovered) {
                const Gdiplus::Color bg = active
                    ? Gdiplus::Color(255, 232, 237, 246)
                    : Gdiplus::Color(255, 242, 243, 245);
                FillRoundRect(
                    graphics,
                    Gdiplus::RectF(
                        static_cast<float>(rect.left), static_cast<float>(rect.top),
                        static_cast<float>(rect.right - rect.left), static_cast<float>(rect.bottom - rect.top)),
                    7.0f,
                    bg);
            }
            PaintPrimaryIcon(graphics, category, rect, active);
            RECT textRect = rect;
            textRect.left += 28;
            SetTextColor(dc, active ? RGB(62, 78, 108) : RGB(68, 69, 73));
            DrawTextW(dc, CategoryName(category), -1, &textRect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        const auto items = BuildSecondary(category_);
        const auto rects = SecondaryRects(bar);
        SelectObject(dc, smallFont_);
        for (size_t i = 0; i < rects.size() && i < items.size(); ++i) {
            const auto& item = items[i];
            RECT rect = rects[i];
            const bool selected = IsSecondarySelected(item);
            const bool hovered = hoverSecondary_ == static_cast<int>(i);

            if (item.action == ItemAction::ColorPreset) {
                const float cx = (rect.left + rect.right) / 2.0f;
                const float cy = (rect.top + rect.bottom) / 2.0f;
                Gdiplus::SolidBrush swatch(Gdiplus::Color(
                    255, GetRValue(item.color), GetGValue(item.color), GetBValue(item.color)));
                graphics.FillEllipse(&swatch, cx - 7, cy - 7, 14, 14);
                Gdiplus::Pen swatchBorder(
                    selected ? Gdiplus::Color(255, 75, 94, 128) : Gdiplus::Color(255, 190, 192, 197),
                    selected ? 2.0f : 1.0f);
                graphics.DrawEllipse(&swatchBorder, cx - 8, cy - 8, 16, 16);
                continue;
            }

            if (item.action != ItemAction::None || hovered) {
                const Gdiplus::Color bg = selected
                    ? Gdiplus::Color(255, 230, 236, 247)
                    : hovered ? Gdiplus::Color(255, 240, 241, 243)
                              : Gdiplus::Color(255, 247, 247, 248);
                FillRoundRect(
                    graphics,
                    Gdiplus::RectF(
                        static_cast<float>(rect.left), static_cast<float>(rect.top),
                        static_cast<float>(rect.right - rect.left), static_cast<float>(rect.bottom - rect.top)),
                    6.0f,
                    bg);
            }
            SetTextColor(dc, selected ? RGB(59, 76, 108) : RGB(78, 79, 84));
            DrawTextW(dc, item.label.c_str(), -1, &rect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        SelectObject(dc, smallFont_);
        SetTextColor(dc, RGB(120, 121, 126));
        RECT hint{
            bar.left + 12,
            bar.top + kPrimaryHeight + kSecondaryHeight,
            bar.right - 12,
            bar.bottom,
        };
        DrawTextW(dc, HintText().c_str(), -1, &hint,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        SelectObject(dc, oldFont);
        ReleaseDC(hwnd_, dc);
    }

    std::wstring HintText() const {
        if (hoverSecondary_ >= 0) {
            const auto items = BuildSecondary(category_);
            if (hoverSecondary_ < static_cast<int>(items.size()) && !items[hoverSecondary_].label.empty()) {
                return items[hoverSecondary_].label;
            }
        }
        switch (category_) {
        case Category::Shape: return L"先选形状，再设置填充方式、线宽和颜色";
        case Category::Arrow: return L"提供直线、双向、弯曲和折线等常用箭头";
        case Category::Pen: return L"选择画笔粗细和颜色后直接拖动绘制";
        case Category::Mosaic: return L"按住鼠标拖动即可隐藏敏感内容";
        case Category::Text: return L"选择字号和颜色，再点击截图区域输入文字";
        case Category::Pin: return L"把当前截图固定在桌面最上层";
        case Category::Actions: return L"撤销、重做、复制、保存、另存为和取消";
        case Category::Select: default: return L"拖动选区调整截图范围";
        }
    }

    void OpenColorPicker() {
        static COLORREF customColors[16] = {};
        CHOOSECOLORW choose{};
        choose.lStructSize = sizeof(choose);
        choose.hwndOwner = hwnd_;
        choose.rgbResult = snip_->UiColor();
        choose.lpCustColors = customColors;
        choose.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (::ChooseColorW(&choose)) snip_->UiSetColor(choose.rgbResult);
    }

    void SelectCategory(Category category) {
        category_ = category;
        categoryLocked_ = category == Category::Pin || category == Category::Actions || category == Category::Select;
        switch (category) {
        case Category::Shape: snip_->UiSetTool(0); break;
        case Category::Arrow: snip_->UiSetTool(1); break;
        case Category::Pen: snip_->UiSetTool(2); break;
        case Category::Mosaic: snip_->UiSetTool(3); break;
        case Category::Text: snip_->UiSetTool(4); break;
        default: snip_->UiSetTool(-1); break;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void ClickSecondary(int index) {
        const auto items = BuildSecondary(category_);
        if (index < 0 || index >= static_cast<int>(items.size())) return;
        const auto& item = items[index];

        switch (item.action) {
        case ItemAction::ShapeKind: snip_->UiSetShapeKind(item.value); break;
        case ItemAction::ShapeFill: snip_->UiSetShapeFillMode(item.value); break;
        case ItemAction::ArrowKind: snip_->UiSetArrowKind(item.value); break;
        case ItemAction::StrokeWidth: snip_->UiSetStrokeWidth(item.value); break;
        case ItemAction::TextSize: snip_->UiSetTextSize(item.value); break;
        case ItemAction::ColorPreset: snip_->UiSetColor(item.color); break;
        case ItemAction::ColorCustom: OpenColorPicker(); break;
        case ItemAction::PinNow: snip_->UiFinish(SnipWindow::FinishAction::Pin); return;
        case ItemAction::Undo: snip_->UiUndo(); break;
        case ItemAction::Redo: snip_->UiRedo(); break;
        case ItemAction::Copy: snip_->UiFinish(SnipWindow::FinishAction::Copy); return;
        case ItemAction::Save: snip_->UiFinish(SnipWindow::FinishAction::Save); return;
        case ItemAction::SaveAs: snip_->UiFinish(SnipWindow::FinishAction::SaveAs); return;
        case ItemAction::Cancel: snip_->UiCancel(); return;
        case ItemAction::None: default: break;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    HWND hwnd_{};
    SnipWindow* snip_{};
    HFONT font_{};
    HFONT smallFont_{};
    Category category_{Category::Select};
    bool categoryLocked_{true};
    int hoverPrimary_{-1};
    int hoverSecondary_{-1};
};

}  // namespace

bool AnimeToolbar::Register(HINSTANCE) {
    return true;
}

void AnimeToolbar::ShowForSnip(HINSTANCE) {
    HWND snipHwnd = FindWindowW(kSnipClass, nullptr);
    if (!snipHwnd || GetPropW(snipHwnd, kControllerProp)) return;

    auto* snip = reinterpret_cast<SnipWindow*>(
        GetWindowLongPtrW(snipHwnd, GWLP_USERDATA));
    if (!snip) return;

    auto* controller = new ToolbarController(snipHwnd, snip);
    if (!SetWindowSubclass(
            snipHwnd,
            ToolbarController::SubclassProc,
            kSubclassId,
            reinterpret_cast<DWORD_PTR>(controller))) {
        delete controller;
        return;
    }

    SetPropW(snipHwnd, kControllerProp, reinterpret_cast<HANDLE>(controller));
    InvalidateRect(snipHwnd, nullptr, FALSE);
}

}  // namespace snaplite
