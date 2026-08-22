#include "anime_toolbar.h"

#include "snip_window.h"

#include <commctrl.h>
#include <commdlg.h>
#include <gdiplus.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cwchar>

namespace snaplite {
namespace {

constexpr wchar_t kSnipClass[] = L"SnapLiteSnipWindow";
constexpr wchar_t kControllerProp[] = L"SnapLiteAnimeToolbarController";
constexpr UINT_PTR kSubclassId = 0x534C1200;

constexpr int kButtonSize = 38;
constexpr int kTopHeight = 46;
constexpr int kHintHeight = 26;
constexpr int kToolbarHeight = kTopHeight + kHintHeight;
constexpr int kButtonCount = 14;
constexpr int kPadX = 8;
constexpr int kGroupGap = 8;

constexpr bool GroupBreakAfter(int index) {
    return index == 4 || index == 6 || index == 8 || index == 12;
}

constexpr int ButtonOffset(int index) {
    int x = kPadX;
    for (int i = 0; i < index; ++i) {
        x += kButtonSize;
        if (GroupBreakAfter(i)) {
            x += kGroupGap;
        }
    }
    return x;
}

constexpr int kToolbarWidth = ButtonOffset(kButtonCount) + kPadX;

const wchar_t* HintForButton(int index) {
    static constexpr const wchar_t* hints[kButtonCount] = {
        L"矩形 R · 拖动绘制矩形标注",
        L"箭头 A · 拖动绘制箭头",
        L"画笔 P · 自由绘制",
        L"马赛克 M · 涂抹隐藏内容",
        L"文字 T · 点击选区输入透明背景文字",
        L"颜色 · 设置矩形 / 箭头 / 画笔 / 文字颜色",
        L"字号 · 设置文字大小",
        L"撤销 Ctrl+Z · 撤销上一步标注",
        L"重做 Ctrl+Y · 恢复刚撤销的标注",
        L"贴图 · 将当前截图直接贴到桌面",
        L"保存 · 保存到默认截图目录",
        L"另存为 · 自己选择文件名和位置",
        L"复制 · 复制截图到剪贴板",
        L"取消 Esc · 关闭本次截图",
    };
    return index >= 0 && index < kButtonCount
        ? hints[index]
        : L"悬停查看功能说明 · 颜色和字号可以直接调整";
}

void AddRoundRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect, float radius) {
    path.Reset();
    const float diameter = radius * 2.0f;
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(
        rect.GetRight() - diameter,
        rect.GetBottom() - diameter,
        diameter,
        diameter,
        0.0f,
        90.0f);
    path.AddArc(
        rect.X,
        rect.GetBottom() - diameter,
        diameter,
        diameter,
        90.0f,
        90.0f);
    path.CloseFigure();
}

class ToolbarController {
public:
    ToolbarController(HWND hwnd, SnipWindow* snip) : hwnd_(hwnd), snip_(snip) {}

    ~ToolbarController() {
        if (font_) {
            DeleteObject(font_);
        }
        if (smallFont_) {
            DeleteObject(smallFont_);
        }
    }

    static LRESULT CALLBACK SubclassProc(
        HWND hwnd,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR,
        DWORD_PTR refData) {
        auto* self = reinterpret_cast<ToolbarController*>(refData);
        if (!self) {
            return DefSubclassProc(hwnd, message, wParam, lParam);
        }

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
                const int hovered = self->HitTest(point);
                if (hovered != self->hover_) {
                    self->hover_ = hovered;
                    InvalidateRect(hwnd, &bar, FALSE);
                }
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return 0;
            }

            if (self->hover_ != -1) {
                self->hover_ = -1;
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
                const int index = self->HitTest(point);
                if (index >= 0) {
                    self->Click(index);
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
                -MulDiv(10, static_cast<int>(dpi), 72),
                0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        }
        if (!smallFont_) {
            smallFont_ = CreateFontW(
                -MulDiv(9, static_cast<int>(dpi), 72),
                0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        }
    }

    RECT ToolbarRect() const {
        if (!snip_ || !snip_->UiHasSelection()) {
            return {};
        }

        const RECT selection = snip_->UiSelectionRect();
        const RECT legacy = snip_->UiLegacyToolbarRect();

        RECT client{};
        GetClientRect(hwnd_, &client);

        int x = static_cast<int>(legacy.left);
        int y = static_cast<int>(legacy.top);

        if (legacy.top < selection.top) {
            y = static_cast<int>(legacy.bottom) - kToolbarHeight;
        }

        x = std::clamp(
            x,
            6,
            std::max(6, static_cast<int>(client.right) - kToolbarWidth - 6));
        y = std::clamp(
            y,
            6,
            std::max(6, static_cast<int>(client.bottom) - kToolbarHeight - 6));

        return {x, y, x + kToolbarWidth, y + kToolbarHeight};
    }

    RECT ButtonRect(const RECT& bar, int index) const {
        const int left = bar.left + ButtonOffset(index);
        return {
            left,
            bar.top + 4,
            left + kButtonSize,
            bar.top + kTopHeight - 4,
        };
    }

    int HitTest(POINT point) const {
        const RECT bar = ToolbarRect();
        if (!PtInRect(&bar, point) || point.y >= bar.top + kTopHeight) {
            return -1;
        }

        for (int i = 0; i < kButtonCount; ++i) {
            const RECT button = ButtonRect(bar, i);
            if (PtInRect(&button, point)) {
                return i;
            }
        }
        return -1;
    }

    void PaintVectorIcon(
        Gdiplus::Graphics& graphics,
        int index,
        const RECT& rect,
        bool active) {
        const float cx = (rect.left + rect.right) / 2.0f;
        const float cy = (rect.top + rect.bottom) / 2.0f;
        const Gdiplus::Color tone = active
            ? Gdiplus::Color(255, 122, 231, 255)
            : Gdiplus::Color(235, 224, 241, 255);

        Gdiplus::Pen pen(tone, 1.8f);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        Gdiplus::SolidBrush brush(tone);

        switch (index) {
        case 0:
            graphics.DrawRectangle(&pen, cx - 8.0f, cy - 6.0f, 16.0f, 12.0f);
            break;
        case 1: {
            graphics.DrawLine(&pen, cx - 8.0f, cy + 7.0f, cx + 5.0f, cy - 5.0f);
            const Gdiplus::PointF head[3] = {
                {cx + 8.0f, cy - 8.0f},
                {cx + 1.0f, cy - 6.5f},
                {cx + 6.5f, cy - 0.5f},
            };
            graphics.FillPolygon(&brush, head, 3);
            break;
        }
        case 2:
            graphics.DrawLine(&pen, cx + 5.0f, cy - 8.0f, cx + 8.0f, cy - 5.0f);
            graphics.DrawLine(&pen, cx + 5.0f, cy - 8.0f, cx - 6.0f, cy + 3.0f);
            graphics.DrawLine(&pen, cx + 8.0f, cy - 5.0f, cx - 3.0f, cy + 6.0f);
            graphics.DrawLine(&pen, cx - 6.0f, cy + 3.0f, cx - 8.0f, cy + 8.0f);
            break;
        case 3:
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 3; ++col) {
                    if ((row + col) % 2 == 0) {
                        graphics.FillRectangle(
                            &brush,
                            cx - 8.0f + col * 5.5f,
                            cy - 8.0f + row * 5.5f,
                            4.5f,
                            4.5f);
                    }
                }
            }
            break;
        case 4:
            graphics.DrawLine(&pen, cx - 7.0f, cy - 7.0f, cx + 7.0f, cy - 7.0f);
            graphics.DrawLine(&pen, cx, cy - 7.0f, cx, cy + 8.0f);
            break;
        case 7: {
            graphics.DrawArc(&pen, cx - 8.0f, cy - 6.0f, 16.0f, 14.0f, 180.0f, 200.0f);
            const Gdiplus::PointF head[3] = {
                {cx - 11.0f, cy - 1.0f},
                {cx - 4.0f, cy - 1.0f},
                {cx - 8.0f, cy + 5.0f},
            };
            graphics.FillPolygon(&brush, head, 3);
            break;
        }
        case 8: {
            graphics.DrawArc(&pen, cx - 8.0f, cy - 6.0f, 16.0f, 14.0f, 0.0f, -200.0f);
            const Gdiplus::PointF head[3] = {
                {cx + 4.0f, cy - 1.0f},
                {cx + 11.0f, cy - 1.0f},
                {cx + 8.0f, cy + 5.0f},
            };
            graphics.FillPolygon(&brush, head, 3);
            break;
        }
        case 9: {
            graphics.FillRectangle(&brush, cx - 6.0f, cy - 8.0f, 12.0f, 4.0f);
            const Gdiplus::PointF body[4] = {
                {cx - 4.0f, cy - 4.0f},
                {cx + 4.0f, cy - 4.0f},
                {cx + 6.0f, cy + 1.0f},
                {cx - 6.0f, cy + 1.0f},
            };
            graphics.FillPolygon(&brush, body, 4);
            graphics.DrawLine(&pen, cx, cy + 1.0f, cx, cy + 8.0f);
            break;
        }
        case 10:
            graphics.DrawLine(&pen, cx, cy - 8.0f, cx, cy + 2.0f);
            graphics.DrawLine(&pen, cx - 4.0f, cy - 2.0f, cx, cy + 2.0f);
            graphics.DrawLine(&pen, cx + 4.0f, cy - 2.0f, cx, cy + 2.0f);
            graphics.DrawLine(&pen, cx - 8.0f, cy + 7.0f, cx + 8.0f, cy + 7.0f);
            break;
        case 11:
            graphics.DrawRectangle(&pen, cx - 8.0f, cy - 6.0f, 16.0f, 12.0f);
            graphics.DrawLine(&pen, cx - 4.0f, cy - 1.0f, cx + 5.0f, cy - 1.0f);
            graphics.DrawLine(&pen, cx + 5.0f, cy - 1.0f, cx + 2.0f, cy - 4.0f);
            graphics.DrawLine(&pen, cx + 5.0f, cy - 1.0f, cx + 2.0f, cy + 2.0f);
            break;
        case 12:
            graphics.DrawRectangle(&pen, cx - 2.0f, cy - 3.0f, 10.0f, 10.0f);
            graphics.DrawLine(&pen, cx - 6.0f, cy + 1.0f, cx - 6.0f, cy - 7.0f);
            graphics.DrawLine(&pen, cx - 6.0f, cy - 7.0f, cx + 2.0f, cy - 7.0f);
            break;
        case 13:
            graphics.DrawLine(&pen, cx - 6.0f, cy - 6.0f, cx + 6.0f, cy + 6.0f);
            graphics.DrawLine(&pen, cx + 6.0f, cy - 6.0f, cx - 6.0f, cy + 6.0f);
            break;
        default:
            break;
        }
    }

    void PaintButton(HDC dc, Gdiplus::Graphics& graphics, const RECT& bar, int index) {
        const RECT rect = ButtonRect(bar, index);
        const bool hovered = hover_ == index;
        const bool active = snip_ && index < 5 && snip_->UiActiveTool() == index;

        Gdiplus::RectF box(
            static_cast<float>(rect.left + 2),
            static_cast<float>(rect.top + 2),
            static_cast<float>(rect.right - rect.left - 4),
            static_cast<float>(rect.bottom - rect.top - 4));
        Gdiplus::GraphicsPath path;
        AddRoundRect(path, box, 8.0f);

        if (active) {
            Gdiplus::SolidBrush fill(Gdiplus::Color(255, 50, 76, 126));
            graphics.FillPath(&fill, &path);
            Gdiplus::Pen ring(Gdiplus::Color(220, 111, 231, 255), 1.0f);
            graphics.DrawPath(&ring, &path);
        } else if (hovered) {
            Gdiplus::SolidBrush fill(Gdiplus::Color(255, 42, 58, 101));
            graphics.FillPath(&fill, &path);
        }

        if (index == 5 && snip_) {
            const COLORREF color = snip_->UiColor();
            const float cx = (rect.left + rect.right) / 2.0f;
            const float cy = (rect.top + rect.bottom) / 2.0f;
            Gdiplus::SolidBrush swatch(Gdiplus::Color(
                255, GetRValue(color), GetGValue(color), GetBValue(color)));
            graphics.FillEllipse(&swatch, cx - 8.0f, cy - 8.0f, 16.0f, 16.0f);
            Gdiplus::Pen ring(Gdiplus::Color(235, 230, 247, 255), 1.5f);
            graphics.DrawEllipse(&ring, cx - 8.0f, cy - 8.0f, 16.0f, 16.0f);
            return;
        }

        if (index == 6 && snip_) {
            EnsureFonts();
            wchar_t text[16]{};
            swprintf_s(text, L"%d", snip_->UiTextSize());
            const HGDIOBJ oldFont = SelectObject(dc, smallFont_);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(226, 242, 255));

            RECT upper = rect;
            upper.bottom = (rect.top + rect.bottom) / 2 + 2;
            DrawTextW(dc, L"Aa", -1, &upper, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            RECT lower = rect;
            lower.top = upper.bottom - 1;
            DrawTextW(dc, text, -1, &lower, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, oldFont);
            return;
        }

        PaintVectorIcon(graphics, index, rect, active);
    }

    void Paint() {
        if (!snip_ || !snip_->UiHasSelection()) {
            return;
        }

        const RECT bar = ToolbarRect();
        HDC dc = GetDC(hwnd_);
        if (!dc) {
            return;
        }

        {
            Gdiplus::Graphics graphics(dc);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

            Gdiplus::RectF shadow(
                static_cast<float>(bar.left - 3),
                static_cast<float>(bar.top - 1),
                static_cast<float>(kToolbarWidth + 6),
                static_cast<float>(kToolbarHeight + 6));
            Gdiplus::GraphicsPath shadowPath;
            AddRoundRect(shadowPath, shadow, 15.0f);
            Gdiplus::SolidBrush shadowBrush(Gdiplus::Color(95, 0, 0, 0));
            graphics.FillPath(&shadowBrush, &shadowPath);

            Gdiplus::RectF body(
                static_cast<float>(bar.left) + 0.5f,
                static_cast<float>(bar.top) + 0.5f,
                static_cast<float>(kToolbarWidth) - 1.0f,
                static_cast<float>(kToolbarHeight) - 1.0f);
            Gdiplus::GraphicsPath bodyPath;
            AddRoundRect(bodyPath, body, 13.0f);

            Gdiplus::LinearGradientBrush background(
                Gdiplus::PointF(static_cast<float>(bar.left), static_cast<float>(bar.top)),
                Gdiplus::PointF(static_cast<float>(bar.right), static_cast<float>(bar.bottom)),
                Gdiplus::Color(255, 17, 25, 48),
                Gdiplus::Color(255, 30, 35, 67));
            graphics.FillPath(&background, &bodyPath);

            Gdiplus::Pen outline(Gdiplus::Color(190, 111, 225, 255), 1.0f);
            graphics.DrawPath(&outline, &bodyPath);

            Gdiplus::Pen topGlow(Gdiplus::Color(110, 193, 236, 255), 1.0f);
            graphics.DrawLine(
                &topGlow,
                static_cast<float>(bar.left + 16),
                static_cast<float>(bar.top + 1),
                static_cast<float>(bar.right - 16),
                static_cast<float>(bar.top + 1));

            Gdiplus::Pen separator(Gdiplus::Color(55, 194, 230, 255), 1.0f);
            graphics.DrawLine(
                &separator,
                static_cast<float>(bar.left + 10),
                static_cast<float>(bar.top + kTopHeight),
                static_cast<float>(bar.right - 10),
                static_cast<float>(bar.top + kTopHeight));

            for (int i = 0; i < kButtonCount; ++i) {
                PaintButton(dc, graphics, bar, i);
            }
        }

        EnsureFonts();
        const HGDIOBJ oldFont = SelectObject(dc, font_);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(194, 222, 245));

        RECT hintRect{
            bar.left + 14,
            bar.top + kTopHeight,
            bar.right - 14,
            bar.bottom,
        };
        DrawTextW(
            dc,
            HintForButton(hover_),
            -1,
            &hintRect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, oldFont);

        ReleaseDC(hwnd_, dc);
    }

    void OpenColorPicker() {
        if (!snip_) {
            return;
        }

        static COLORREF customColors[16] = {
            RGB(255, 105, 180),
            RGB(111, 231, 255),
            RGB(139, 125, 255),
            RGB(255, 92, 92),
            RGB(255, 191, 71),
            RGB(104, 222, 143),
            RGB(255, 255, 255),
            RGB(30, 30, 30),
        };

        CHOOSECOLORW choose{};
        choose.lStructSize = sizeof(choose);
        choose.hwndOwner = hwnd_;
        choose.rgbResult = snip_->UiColor();
        choose.lpCustColors = customColors;
        choose.Flags = CC_FULLOPEN | CC_RGBINIT;

        if (::ChooseColorW(&choose)) {
            snip_->UiSetColor(choose.rgbResult);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void OpenFontSizeMenu() {
        if (!snip_) {
            return;
        }

        static constexpr std::array<int, 7> sizes{12, 14, 16, 20, 24, 32, 48};
        HMENU menu = CreatePopupMenu();
        if (!menu) {
            return;
        }

        for (size_t i = 0; i < sizes.size(); ++i) {
            wchar_t label[32]{};
            swprintf_s(label, L"%d pt", sizes[i]);
            AppendMenuW(
                menu,
                MF_STRING | (snip_->UiTextSize() == sizes[i] ? MF_CHECKED : 0),
                static_cast<UINT>(2000 + i),
                label);
        }

        const RECT bar = ToolbarRect();
        const RECT button = ButtonRect(bar, 6);
        POINT point{button.left, button.bottom};
        ClientToScreen(hwnd_, &point);

        const UINT command = TrackPopupMenu(
            menu,
            TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
            point.x,
            point.y,
            0,
            hwnd_,
            nullptr);
        DestroyMenu(menu);

        if (command >= 2000 && command < 2000 + sizes.size()) {
            snip_->UiSetTextSize(sizes[command - 2000]);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void Click(int index) {
        if (!snip_) {
            return;
        }

        if (index >= 0 && index <= 4) {
            snip_->UiSetTool(index);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        switch (index) {
        case 5:
            OpenColorPicker();
            break;
        case 6:
            OpenFontSizeMenu();
            break;
        case 7:
            snip_->UiUndo();
            break;
        case 8:
            snip_->UiRedo();
            break;
        case 9:
            snip_->UiFinish(SnipWindow::FinishAction::Pin);
            break;
        case 10:
            snip_->UiFinish(SnipWindow::FinishAction::Save);
            break;
        case 11:
            snip_->UiFinish(SnipWindow::FinishAction::SaveAs);
            break;
        case 12:
            snip_->UiFinish(SnipWindow::FinishAction::Copy);
            break;
        case 13:
            snip_->UiCancel();
            break;
        default:
            break;
        }
    }

    HWND hwnd_{};
    SnipWindow* snip_{};
    HFONT font_{};
    HFONT smallFont_{};
    int hover_{-1};
};

}  // namespace

bool AnimeToolbar::Register(HINSTANCE) {
    // The themed toolbar is painted directly inside the snip window.
    // No second popup window is created, so DPI and selection coordinates stay unified.
    return true;
}

void AnimeToolbar::ShowForSnip(HINSTANCE) {
    HWND snipHwnd = FindWindowW(kSnipClass, nullptr);
    if (!snipHwnd || GetPropW(snipHwnd, kControllerProp)) {
        return;
    }

    auto* snip = reinterpret_cast<SnipWindow*>(
        GetWindowLongPtrW(snipHwnd, GWLP_USERDATA));
    if (!snip) {
        return;
    }

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
