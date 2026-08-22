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
constexpr wchar_t kToolbarClass[] = L"SnapLiteEditorToolbarChild";
constexpr wchar_t kHostProp[] = L"SnapLiteEditorToolbarHost";
constexpr UINT_PTR kParentSubclassId = 0x534C1400;

constexpr int kToolbarWidth = 832;
constexpr int kPrimaryHeight = 42;
constexpr int kSecondaryHeight = 42;
constexpr int kHintHeight = 24;
constexpr int kToolbarHeight = kPrimaryHeight + kSecondaryHeight + kHintHeight;
constexpr int kPrimaryCount = 8;
constexpr int kPrimaryWidth = 88;
constexpr int kPad = 10;
constexpr int kGap = 4;

enum class Category { Select = 0, Shape, Arrow, Pen, Mosaic, Text, Pin, Actions };
enum class ItemAction {
    None, ShapeKind, ShapeFill, ArrowKind, StrokeWidth, TextSize,
    ColorPreset, ColorCustom, PinNow, Undo, Redo, Copy, Save, SaveAs, Cancel,
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
        L"选择", L"形状", L"箭头", L"画笔", L"马赛克", L"文字", L"图钉", L"操作"
    };
    return names[static_cast<int>(category)];
}

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
            {L"矩形", ItemAction::ShapeKind, 0, 54}, {L"圆角", ItemAction::ShapeKind, 1, 54},
            {L"圆形", ItemAction::ShapeKind, 2, 54}, {L"椭圆", ItemAction::ShapeKind, 3, 54},
            {L"直线", ItemAction::ShapeKind, 4, 54}, {L"三角", ItemAction::ShapeKind, 5, 54},
            {L"菱形", ItemAction::ShapeKind, 6, 54}, {L"六边形", ItemAction::ShapeKind, 7, 62},
            {L"描边", ItemAction::ShapeFill, 0, 52}, {L"填充", ItemAction::ShapeFill, 1, 52},
            {L"两者", ItemAction::ShapeFill, 2, 52}, {L"1", ItemAction::StrokeWidth, 1, 32},
            {L"2", ItemAction::StrokeWidth, 2, 32}, {L"4", ItemAction::StrokeWidth, 4, 32},
            {L"6", ItemAction::StrokeWidth, 6, 32},
        };
        break;
    case Category::Arrow:
        items = {
            {L"直箭头", ItemAction::ArrowKind, 0, 62}, {L"细箭头", ItemAction::ArrowKind, 1, 62},
            {L"粗箭头", ItemAction::ArrowKind, 2, 62}, {L"双向", ItemAction::ArrowKind, 3, 54},
            {L"弯曲", ItemAction::ArrowKind, 4, 54}, {L"折线", ItemAction::ArrowKind, 5, 54},
            {L"阶梯", ItemAction::ArrowKind, 6, 54}, {L"1", ItemAction::StrokeWidth, 1, 32},
            {L"2", ItemAction::StrokeWidth, 2, 32}, {L"4", ItemAction::StrokeWidth, 4, 32},
            {L"6", ItemAction::StrokeWidth, 6, 32},
        };
        break;
    case Category::Pen:
        items = {{L"细", ItemAction::StrokeWidth, 1, 42}, {L"普通", ItemAction::StrokeWidth, 3, 52},
                 {L"粗", ItemAction::StrokeWidth, 6, 42}, {L"很粗", ItemAction::StrokeWidth, 10, 52}};
        break;
    case Category::Mosaic:
        items = {{L"拖动鼠标涂抹马赛克", ItemAction::None, 0, 166}};
        break;
    case Category::Text:
        items = {{L"12", ItemAction::TextSize, 12, 38}, {L"14", ItemAction::TextSize, 14, 38},
                 {L"16", ItemAction::TextSize, 16, 38}, {L"20", ItemAction::TextSize, 20, 38},
                 {L"24", ItemAction::TextSize, 24, 38}, {L"32", ItemAction::TextSize, 32, 38},
                 {L"48", ItemAction::TextSize, 48, 38}};
        break;
    case Category::Pin:
        items = {{L"贴到桌面", ItemAction::PinNow, 0, 92}};
        break;
    case Category::Actions:
        items = {{L"撤销", ItemAction::Undo, 0, 58}, {L"重做", ItemAction::Redo, 0, 58},
                 {L"复制", ItemAction::Copy, 0, 58}, {L"保存", ItemAction::Save, 0, 58},
                 {L"另存为", ItemAction::SaveAs, 0, 68}, {L"取消", ItemAction::Cancel, 0, 58}};
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

class ToolbarHost {
public:
    ToolbarHost(HINSTANCE instance, HWND parent, SnipWindow* snip)
        : instance_(instance), parent_(parent), snip_(snip) {}

    ~ToolbarHost() {
        if (font_) DeleteObject(font_);
        if (smallFont_) DeleteObject(smallFont_);
    }

    void Attach(HWND child) { child_ = child; UpdatePosition(); }

    void UpdatePosition() {
        if (!child_ || !snip_ || !snip_->UiHasSelection()) return;
        const RECT selection = snip_->UiSelectionRect();
        const RECT legacy = snip_->UiLegacyToolbarRect();
        RECT client{};
        GetClientRect(parent_, &client);

        int x = static_cast<int>(legacy.left);
        int y = static_cast<int>(legacy.top);
        if (legacy.top < selection.top) y = static_cast<int>(legacy.bottom) - kToolbarHeight;
        x = std::clamp(x, 6, std::max(6, static_cast<int>(client.right) - kToolbarWidth - 6));
        y = std::clamp(y, 6, std::max(6, static_cast<int>(client.bottom) - kToolbarHeight - 6));

        SetWindowPos(child_, HWND_TOP, x, y, kToolbarWidth, kToolbarHeight,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void SyncCategoryFromTool() {
        const int tool = snip_ ? snip_->UiActiveTool() : -1;
        if (tool >= 0 && tool <= 4) {
            const Category next = static_cast<Category>(tool + 1);
            if (next != category_) {
                category_ = next;
                InvalidateRect(child_, nullptr, FALSE);
            }
        }
    }

    LRESULT HandleChild(HWND hwnd, UINT message, WPARAM, LPARAM lParam) {
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
            if (hoverPrimary_ != -1 || hoverSecondary_ != -1) {
                hoverPrimary_ = hoverSecondary_ = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN:
            SetFocus(parent_);
            return 0;
        case WM_LBUTTONUP: {
            POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int primary = HitPrimary(p);
            if (primary >= 0) {
                SelectCategory(static_cast<Category>(primary));
                return 0;
            }
            const int secondary = HitSecondary(p);
            if (secondary >= 0) ClickSecondary(secondary);
            return 0;
        }
        case WM_PAINT:
            Paint(hwnd);
            return 0;
        }
        return DefWindowProcW(hwnd, message, 0, lParam);
    }

private:
    void EnsureFonts() {
        const UINT dpi = GetDpiForWindow(parent_);
        if (!font_) font_ = CreateFontW(-MulDiv(10, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        if (!smallFont_) smallFont_ = CreateFontW(-MulDiv(9, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    }

    RECT PrimaryRect(int index) const {
        return {kPad + index * kPrimaryWidth, 5,
                kPad + (index + 1) * kPrimaryWidth - 4, kPrimaryHeight - 5};
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

    int HitPrimary(POINT p) const {
        if (p.y >= kPrimaryHeight) return -1;
        for (int i = 0; i < kPrimaryCount; ++i) {
            RECT r = PrimaryRect(i);
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
        const int primary = HitPrimary(p);
        const int secondary = primary < 0 ? HitSecondary(p) : -1;
        if (primary != hoverPrimary_ || secondary != hoverSecondary_) {
            hoverPrimary_ = primary;
            hoverSecondary_ = secondary;
            InvalidateRect(child_, nullptr, FALSE);
        }
    }

    bool IsSelected(const SecondaryItem& item) const {
        if (!snip_) return false;
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

    std::wstring HintText() const {
        switch (category_) {
        case Category::Shape: return L"选择形状后拖动绘制；下方可调样式、线宽和颜色";
        case Category::Arrow: return L"选择箭头样式后拖动绘制；支持直线、双向、弯曲和折线";
        case Category::Pen: return L"拖动自由绘制；下方可调粗细和颜色";
        case Category::Mosaic: return L"按住鼠标左键涂抹需要隐藏的区域";
        case Category::Text: return L"点击选区输入文字；下方可调字号和颜色";
        case Category::Pin: return L"把当前截图固定到桌面最上层";
        case Category::Actions: return L"撤销、重做、复制、保存、另存为或取消";
        default: return L"拖动选区边缘调整大小，双击选区直接复制";
        }
    }

    void Paint(HWND hwnd) {
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
        HBRUSH background = CreateSolidBrush(RGB(250, 250, 252));
        FillRect(mem, &all, background);
        DeleteObject(background);

        Gdiplus::Graphics g(mem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen divider(Gdiplus::Color(255, 231, 232, 235), 1.0f);
        g.DrawLine(&divider, 10.0f, static_cast<float>(kPrimaryHeight),
                   static_cast<float>(kToolbarWidth - 10), static_cast<float>(kPrimaryHeight));
        g.DrawLine(&divider, 10.0f, static_cast<float>(kPrimaryHeight + kSecondaryHeight),
                   static_cast<float>(kToolbarWidth - 10), static_cast<float>(kPrimaryHeight + kSecondaryHeight));

        const HGDIOBJ oldFont = SelectObject(mem, font_);
        SetBkMode(mem, TRANSPARENT);
        for (int i = 0; i < kPrimaryCount; ++i) {
            RECT r = PrimaryRect(i);
            const bool active = category_ == static_cast<Category>(i);
            const bool hovered = hoverPrimary_ == i;
            if (active || hovered) {
                FillRoundRect(g,
                    Gdiplus::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                                   static_cast<float>(r.right-r.left), static_cast<float>(r.bottom-r.top)),
                    7.0f, active ? Gdiplus::Color(255, 232, 237, 246)
                                 : Gdiplus::Color(255, 242, 243, 245));
            }
            SetTextColor(mem, active ? RGB(60, 76, 106) : RGB(67, 68, 72));
            DrawTextW(mem, CategoryName(static_cast<Category>(i)), -1, &r,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
                const float cx = (r.left + r.right) / 2.0f;
                const float cy = (r.top + r.bottom) / 2.0f;
                Gdiplus::SolidBrush swatch(Gdiplus::Color(255, GetRValue(item.color), GetGValue(item.color), GetBValue(item.color)));
                g.FillEllipse(&swatch, cx - 7.0f, cy - 7.0f, 14.0f, 14.0f);
                Gdiplus::Pen border(selected ? Gdiplus::Color(255, 75, 94, 128) : Gdiplus::Color(255, 190, 192, 197), selected ? 2.0f : 1.0f);
                g.DrawEllipse(&border, cx - 8.0f, cy - 8.0f, 16.0f, 16.0f);
                continue;
            }
            if (item.action != ItemAction::None || hovered) {
                FillRoundRect(g,
                    Gdiplus::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                                   static_cast<float>(r.right-r.left), static_cast<float>(r.bottom-r.top)),
                    6.0f, selected ? Gdiplus::Color(255, 230, 236, 247)
                                    : hovered ? Gdiplus::Color(255, 240, 241, 243)
                                              : Gdiplus::Color(255, 247, 247, 248));
            }
            SetTextColor(mem, selected ? RGB(59, 76, 108) : RGB(78, 79, 84));
            DrawTextW(mem, item.label.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        RECT hint{kPad, kPrimaryHeight + kSecondaryHeight, kToolbarWidth-kPad, kToolbarHeight};
        SetTextColor(mem, RGB(118, 119, 124));
        DrawTextW(mem, HintText().c_str(), -1, &hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        SelectObject(mem, oldFont);
        BitBlt(dc, 0, 0, kToolbarWidth, kToolbarHeight, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
    }

    void SelectCategory(Category category) {
        category_ = category;
        if (!snip_) return;
        switch (category) {
        case Category::Shape: snip_->UiSetTool(0); break;
        case Category::Arrow: snip_->UiSetTool(1); break;
        case Category::Pen: snip_->UiSetTool(2); break;
        case Category::Mosaic: snip_->UiSetTool(3); break;
        case Category::Text: snip_->UiSetTool(4); break;
        default: snip_->UiSetTool(-1); break;
        }
        InvalidateRect(child_, nullptr, FALSE);
    }

    void OpenColorPicker() {
        static COLORREF custom[16]{};
        CHOOSECOLORW choose{};
        choose.lStructSize = sizeof(choose);
        choose.hwndOwner = parent_;
        choose.rgbResult = snip_->UiColor();
        choose.lpCustColors = custom;
        choose.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (ChooseColorW(&choose)) snip_->UiSetColor(choose.rgbResult);
    }

    void ClickSecondary(int index) {
        const auto items = BuildSecondary(category_);
        if (index < 0 || index >= static_cast<int>(items.size()) || !snip_) return;
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
        InvalidateRect(child_, nullptr, FALSE);
    }

    HINSTANCE instance_{};
    HWND parent_{};
    HWND child_{};
    SnipWindow* snip_{};
    HFONT font_{};
    HFONT smallFont_{};
    Category category_{Category::Select};
    int hoverPrimary_{-1};
    int hoverSecondary_{-1};
};

LRESULT CALLBACK ToolbarWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* host = reinterpret_cast<ToolbarHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        host = static_cast<ToolbarHost*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
        if (host) host->Attach(hwnd);
    }
    return host ? host->HandleChild(hwnd, message, wParam, lParam)
                : DefWindowProcW(hwnd, message, wParam, lParam);
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

    const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
    if (message == WM_PAINT || message == WM_MOUSEMOVE || message == WM_LBUTTONUP ||
        message == WM_KEYDOWN || message == WM_SIZE || message == WM_DPICHANGED) {
        host->SyncCategoryFromTool();
        host->UpdatePosition();
    }
    return result;
}

}  // namespace

bool AnimeToolbar::Register(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ToolbarWindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kToolbarClass;
    wc.style = CS_DBLCLKS;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
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
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, kToolbarWidth, kToolbarHeight,
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
