#include "ocr_result_panel.h"

#include "ocr.h"

#include <algorithm>
#include <commctrl.h>
#include <string>

namespace snaplite {
namespace {

constexpr wchar_t kPanelClass[] = L"SnapLiteOcrResultPanelV2";
constexpr int kButtonCopySelected = 101;
constexpr int kButtonCopyAll = 102;
constexpr int kButtonCloseTop = 104;
constexpr UINT_PTR kCopiedTimerId = 1;
constexpr int kPanelWidth = 440;
constexpr int kPanelHeight = 390;
constexpr int kHeaderHeight = 76;
constexpr int kCornerRadius = 18;

constexpr COLORREF kPanelBg = RGB(255, 248, 250);
constexpr COLORREF kCardBg = RGB(255, 255, 255);
constexpr COLORREF kBorder = RGB(244, 216, 224);
constexpr COLORREF kAccent = RGB(239, 139, 166);
constexpr COLORREF kAccentPressed = RGB(224, 119, 151);
constexpr COLORREF kText = RGB(57, 49, 55);
constexpr COLORREF kMutedText = RGB(137, 119, 128);
constexpr COLORREF kSecondaryBg = RGB(255, 252, 253);
constexpr COLORREF kSecondaryPressed = RGB(250, 239, 243);

struct PanelState {
    HWND edit{};
    HWND copySelected{};
    HWND copyAll{};
    HWND closeTop{};
    HFONT titleFont{};
    HFONT bodyFont{};
    HFONT smallFont{};
    HBRUSH editBrush{};
    std::wstring text;
    RECT editFrame{};
    UINT dpi{96};
    int copiedButton{0};
};

HWND g_activePanel = nullptr;

int Scale(UINT dpi, int value) {
    return MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96);
}

int Scale(const PanelState* state, int value) {
    return Scale(state ? state->dpi : 96, value);
}

int CountVisualLines(const std::wstring& text) {
    if (text.empty()) return 0;
    int lines = 1;
    for (wchar_t ch : text) {
        if (ch == L'\n') ++lines;
    }
    return lines;
}

int CountVisibleChars(const std::wstring& text) {
    int count = 0;
    for (wchar_t ch : text) {
        if (ch != L'\r' && ch != L'\n') ++count;
    }
    return count;
}

void ApplyFont(HWND control, HFONT font) {
    if (control && font) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

// 面板是弹出窗口，键盘焦点落在只读文本框里。键盘消息不会自动回到父窗口，
// 所以 ESC 必须由文本框的子类截获，否则面板打开后 Esc 完全失效，
// 而工具栏仍然提示"取消截图 Esc"。
constexpr UINT_PTR kPanelEditSubclassId = 0x534C1501;

LRESULT CALLBACK PanelEditSubclassProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR subclassId,
    DWORD_PTR refData) {
    (void)subclassId;
    (void)refData;
    (void)lParam;
    if (message == WM_KEYDOWN && wParam == VK_ESCAPE) {
        if (HWND panel = GetParent(hwnd)) {
            PostMessageW(panel, WM_CLOSE, 0, 0);
        }
        return 0;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

std::wstring SelectedText(HWND edit) {
    if (!edit) return {};

    DWORD start = 0;
    DWORD end = 0;
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    if (end <= start) return {};

    const int length = GetWindowTextLengthW(edit);
    if (length <= 0) return {};

    std::wstring all(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(edit, all.data(), length + 1);
    all.resize(static_cast<size_t>(length));

    const size_t safeStart = std::min<size_t>(start, all.size());
    const size_t safeEnd = std::min<size_t>(end, all.size());
    if (safeEnd <= safeStart) return {};
    return all.substr(safeStart, safeEnd - safeStart);
}

void SetRoundedWindowRegion(HWND hwnd) {
    RECT rect{};
    GetClientRect(hwnd, &rect);
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;

    const UINT dpi = GetDpiForWindow(hwnd);
    const int radius = Scale(dpi ? dpi : 96, kCornerRadius);
    HRGN region = CreateRoundRectRgn(
        0,
        0,
        rect.right - rect.left + 1,
        rect.bottom - rect.top + 1,
        radius,
        radius);
    if (region) {
        // SetWindowRgn 成功后区域的所有权交给系统；失败时必须自己释放，
        // 否则每次调用都会泄漏一个 HRGN。
        if (SetWindowRgn(hwnd, region, TRUE) == 0) {
            DeleteObject(region);
        }
    }
}

void SetEditFormattingRect(HWND edit, const PanelState* state) {
    if (!edit) return;
    RECT client{};
    GetClientRect(edit, &client);
    const int padX = Scale(state, 12);
    const int padY = Scale(state, 10);
    RECT format{
        padX,
        padY,
        std::max<LONG>(padX + 1, client.right - padX),
        std::max<LONG>(padY + 1, client.bottom - padY),
    };
    SendMessageW(edit, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&format));
}

void LayoutPanel(HWND hwnd, PanelState* state) {
    if (!state) return;

    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = std::max(Scale(state, 320), static_cast<int>(client.right - client.left));
    const int height = std::max(Scale(state, 260), static_cast<int>(client.bottom - client.top));
    const int pad = Scale(state, 18);
    const int buttonHeight = Scale(state, 36);
    const int buttonGap = Scale(state, 10);
    const int buttonWidth = Scale(state, 112);
    const int closeSize = Scale(state, 32);

    MoveWindow(state->closeTop, width - pad - closeSize, Scale(state, 12), closeSize, closeSize, TRUE);

    const int buttonsTop = height - pad - buttonHeight;
    const int editTop = Scale(state, kHeaderHeight);
    const int editHeight = std::max(Scale(state, 100), buttonsTop - editTop - Scale(state, 14));
    state->editFrame = {pad, editTop, width - pad, editTop + editHeight};

    MoveWindow(
        state->edit,
        state->editFrame.left + 1,
        state->editFrame.top + 1,
        state->editFrame.right - state->editFrame.left - 2,
        state->editFrame.bottom - state->editFrame.top - 2,
        TRUE);
    SetEditFormattingRect(state->edit, state);

    int x = width - pad - buttonWidth;
    MoveWindow(state->copyAll, x, buttonsTop, buttonWidth, buttonHeight, TRUE);
    x -= buttonGap + buttonWidth;
    MoveWindow(state->copySelected, x, buttonsTop, buttonWidth, buttonHeight, TRUE);

    SetRoundedWindowRegion(hwnd);
    InvalidateRect(hwnd, nullptr, TRUE);
}

void DrawRoundRectFill(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    const HGDIOBJ oldBrush = SelectObject(dc, brush);
    const HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void DrawOwnerButton(const DRAWITEMSTRUCT* draw, const PanelState* state) {
    if (!draw) return;

    RECT rect = draw->rcItem;
    const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
    const bool hovered = (draw->itemState & ODS_HOTLIGHT) != 0;
    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;

    if (draw->CtlID == kButtonCloseTop) {
        const COLORREF fill = pressed ? ui::kPressed : (hovered ? ui::kHover : kPanelBg);
        const COLORREF border = hovered || pressed ? kBorder : fill;
        DrawRoundRectFill(draw->hDC, rect, Scale(state, 9), fill, border);
        const int inset = Scale(state, 10);
        HPEN crossPen = CreatePen(
            PS_SOLID,
            std::max(1, Scale(state, 1)),
            ui::kTextSecondary);
        if (crossPen) {
            const HGDIOBJ oldPen = SelectObject(draw->hDC, crossPen);
            MoveToEx(draw->hDC, rect.left + inset, rect.top + inset, nullptr);
            LineTo(draw->hDC, rect.right - inset, rect.bottom - inset);
            MoveToEx(draw->hDC, rect.right - inset, rect.top + inset, nullptr);
            LineTo(draw->hDC, rect.left + inset, rect.bottom - inset);
            SelectObject(draw->hDC, oldPen);
            DeleteObject(crossPen);
        }
        return;
    }

    wchar_t text[64]{};
    GetWindowTextW(draw->hwndItem, text, static_cast<int>(std::size(text)));

    COLORREF fill = kSecondaryBg;
    COLORREF border = kBorder;
    COLORREF textColor = kText;

    if (draw->CtlID == kButtonCopyAll) {
        fill = pressed
            ? ui::kAccentActionPressed
            : (hovered ? ui::Blend(ui::kAccentAction, ui::kAccentStrong) : ui::kAccentAction);
        border = fill;
        textColor = RGB(255, 255, 255);
    } else if (pressed) {
        fill = kSecondaryPressed;
        border = ui::kAccentStrong;
    } else if (hovered) {
        fill = ui::kHover;
        border = ui::kAccent;
    }

    if (disabled) {
        textColor = kMutedText;
    }

    DrawRoundRectFill(draw->hDC, rect, Scale(state, 10), fill, border);

    const HGDIOBJ oldFont = state && state->bodyFont ? SelectObject(draw->hDC, state->bodyFont) : nullptr;
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, textColor);
    DrawTextW(draw->hDC, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (oldFont) SelectObject(draw->hDC, oldFont);
}

void PaintPanel(HWND hwnd, PanelState* state) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd, &paint);

    RECT client{};
    GetClientRect(hwnd, &client);

    HBRUSH background = CreateSolidBrush(kPanelBg);
    FillRect(dc, &client, background);
    DeleteObject(background);

    const int cornerRadius = Scale(state, kCornerRadius);
    HPEN outline = CreatePen(PS_SOLID, 1, kBorder);
    HBRUSH hollow = static_cast<HBRUSH>(GetStockObject(HOLLOW_BRUSH));
    const HGDIOBJ oldPen = SelectObject(dc, outline);
    const HGDIOBJ oldBrush = SelectObject(dc, hollow);
    RoundRect(dc, 0, 0, client.right, client.bottom, cornerRadius, cornerRadius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(outline);

    const int pad = Scale(state, 18);
    const RECT dotRect{
        pad,
        Scale(state, 18),
        pad + Scale(state, 10),
        Scale(state, 28)};
    ui::FillRoundRect(dc, dotRect, Scale(state, 4), kAccent, kAccent);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kText);
    const HGDIOBJ oldFont = state && state->titleFont ? SelectObject(dc, state->titleFont) : nullptr;
    RECT titleRect{
        dotRect.right + Scale(state, 8),
        Scale(state, 10),
        client.right - Scale(state, 58),
        Scale(state, 38)};
    DrawTextW(dc, L"文字识别结果", -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    int countBadgeLeft = static_cast<int>(client.right) - pad - Scale(state, 106);
    if (state) {
        if (state->smallFont) SelectObject(dc, state->smallFont);
        const std::wstring statsText =
            std::to_wstring(CountVisualLines(state->text)) + L" 行 · " +
            std::to_wstring(CountVisibleChars(state->text)) + L" 字";
        SIZE statsSize{};
        GetTextExtentPoint32W(dc, statsText.c_str(), static_cast<int>(statsText.size()), &statsSize);
        const int badgeRight = static_cast<int>(client.right) - pad;
        const int badgeWidth = std::max(Scale(state, 86), static_cast<int>(statsSize.cx) + Scale(state, 18));
        countBadgeLeft = badgeRight - badgeWidth;
        RECT countBadge{countBadgeLeft, Scale(state, 46), badgeRight, Scale(state, 68)};
        DrawRoundRectFill(dc, countBadge, Scale(state, 11), ui::kCream, kBorder);
        SetTextColor(dc, ui::kAccentAction);
        DrawTextW(dc, statsText.c_str(), -1, &countBadge, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    SetTextColor(dc, ui::kTextSecondary);
    if (state && state->smallFont) SelectObject(dc, state->smallFont);
    RECT subtitleRect{pad, Scale(state, 46), countBadgeLeft - Scale(state, 8), Scale(state, 68)};
    const std::wstring subtitle = L"保留换行 · 可拖选并复制";
    DrawTextW(dc, subtitle.c_str(), -1, &subtitleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    const int headerHeight = Scale(state, kHeaderHeight);
    HPEN divider = CreatePen(PS_SOLID, 1, kBorder);
    if (divider) {
        const HGDIOBJ dividerPreviousPen = SelectObject(dc, divider);
        const HGDIOBJ dividerPreviousBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        MoveToEx(dc, Scale(state, 16), headerHeight - 1, nullptr);
        LineTo(dc, client.right - Scale(state, 16), headerHeight - 1);
        SelectObject(dc, dividerPreviousBrush);
        SelectObject(dc, dividerPreviousPen);
        DeleteObject(divider);
    }

    if (state) {
        DrawRoundRectFill(dc, state->editFrame, Scale(state, 12), kCardBg, kBorder);
        SetTextColor(dc, kMutedText);
        if (state->smallFont) SelectObject(dc, state->smallFont);
        RECT footerHint{
            pad,
            client.bottom - pad - Scale(state, 36),
            client.right - pad - Scale(state, 112) * 2 - Scale(state, 20),
            client.bottom - pad};
        if (footerHint.right > footerHint.left) {
            DrawTextW(dc, L"按 Esc 关闭", -1, &footerHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    if (oldFont) SelectObject(dc, oldFont);
    EndPaint(hwnd, &paint);
}

void RecreatePanelFonts(HWND hwnd, PanelState* state) {
    if (!state) return;
    const HFONT oldTitle = state->titleFont;
    const HFONT oldBody = state->bodyFont;
    const HFONT oldSmall = state->smallFont;
    const int dpi = static_cast<int>(state->dpi ? state->dpi : 96);
    const HFONT newTitle = CreateFontW(
        -MulDiv(14, dpi, 72), 0, 0, 0, FW_SEMIBOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    const HFONT newBody = CreateFontW(
        -MulDiv(11, dpi, 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    const HFONT newSmall = CreateFontW(
        -MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    state->titleFont = newTitle ? newTitle : oldTitle;
    state->bodyFont = newBody ? newBody : oldBody;
    state->smallFont = newSmall ? newSmall : oldSmall;
    ApplyFont(state->edit, state->bodyFont);
    ApplyFont(state->copySelected, state->bodyFont);
    ApplyFont(state->copyAll, state->bodyFont);
    ApplyFont(state->closeTop, state->titleFont);
    if (newTitle && oldTitle) DeleteObject(oldTitle);
    if (newBody && oldBody) DeleteObject(oldBody);
    if (newSmall && oldSmall) DeleteObject(oldSmall);
    InvalidateRect(hwnd, nullptr, TRUE);
}

void MarkCopiedFeedback(HWND hwnd, PanelState* state, int buttonId) {
    if (!state) return;
    state->copiedButton = buttonId;
    SetWindowTextW(state->copySelected, buttonId == 1 ? L"✓ 已复制" : L"复制选中");
    SetWindowTextW(state->copyAll, buttonId == 2 ? L"✓ 已复制" : L"复制全部");
    SetTimer(hwnd, kCopiedTimerId, 1500, nullptr);
}

LRESULT CALLBACK PanelProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<PanelState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
    case WM_NCCREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<PanelState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }
    case WM_CREATE: {
        if (!state) return -1;

        state->dpi = GetDpiForWindow(hwnd);
        if (state->dpi == 0) state->dpi = 96;
        state->editBrush = CreateSolidBrush(kCardBg);

        state->edit = CreateWindowExW(
            0,
            L"EDIT",
            state->text.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | ES_NOHIDESEL,
            0, 0, 0, 0,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);

        state->copySelected = CreateWindowExW(
            0, L"BUTTON", L"复制选中",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | BS_NOTIFY,
            0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonCopySelected)),
            GetModuleHandleW(nullptr), nullptr);

        state->copyAll = CreateWindowExW(
            0, L"BUTTON", L"复制全部",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | BS_NOTIFY,
            0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonCopyAll)),
            GetModuleHandleW(nullptr), nullptr);

        state->closeTop = CreateWindowExW(
            0, L"BUTTON", L"×",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | BS_NOTIFY,
            0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonCloseTop)),
            GetModuleHandleW(nullptr), nullptr);

        RecreatePanelFonts(hwnd, state);

        const int textMargin = Scale(state, 12);
        SendMessageW(
            state->edit,
            EM_SETMARGINS,
            EC_LEFTMARGIN | EC_RIGHTMARGIN,
            MAKELPARAM(textMargin, textMargin));
        SetWindowSubclass(state->edit, PanelEditSubclassProc, kPanelEditSubclassId, 0);
        LayoutPanel(hwnd, state);
        SetFocus(state->edit);
        SendMessageW(state->edit, EM_SETSEL, 0, 0);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        PaintPanel(hwnd, state);
        return 0;
    case WM_SIZE:
        LayoutPanel(hwnd, state);
        return 0;
    case WM_DPICHANGED: {
        if (!state) return 0;
        state->dpi = HIWORD(wParam);
        if (state->dpi == 0) state->dpi = 96;
        RecreatePanelFonts(hwnd, state);
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(
            hwnd,
            HWND_TOPMOST,
            suggested->left,
            suggested->top,
            suggested->right - suggested->left,
            suggested->bottom - suggested->top,
            SWP_NOACTIVATE);
        LayoutPanel(hwnd, state);
        return 0;
    }
    case WM_DRAWITEM:
        DrawOwnerButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam), state);
        return TRUE;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        if (state && reinterpret_cast<HWND>(lParam) == state->edit) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkColor(dc, kCardBg);
            SetTextColor(dc, kText);
            return reinterpret_cast<LRESULT>(state->editBrush);
        }
        break;
    case WM_NCHITTEST: {
        const LRESULT base = DefWindowProcW(hwnd, message, wParam, lParam);
        if (base != HTCLIENT) return base;
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &point);
        RECT client{};
        GetClientRect(hwnd, &client);
        if (point.y >= 0 && point.y < Scale(state, kHeaderHeight) &&
            point.x < client.right - Scale(state, 58)) {
            return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_TIMER:
        if (wParam == kCopiedTimerId && state) {
            KillTimer(hwnd, kCopiedTimerId);
            state->copiedButton = 0;
            SetWindowTextW(state->copySelected, L"复制选中");
            SetWindowTextW(state->copyAll, L"复制全部");
            return 0;
        }
        break;
    case WM_COMMAND:
        if (!state) break;
        switch (LOWORD(wParam)) {
        case kButtonCopySelected: {
            const std::wstring selected = SelectedText(state->edit);
            if (selected.empty()) {
                MessageBeep(MB_ICONINFORMATION);
                SetFocus(state->edit);
                return 0;
            }
            // 剪贴板被别的程序占用时不能静默失败：否则用户点了没反应。
            if (!CopyUnicodeTextToClipboard(hwnd, selected)) {
                MessageBoxW(
                    hwnd,
                    L"复制失败：剪贴板被其他程序占用，请稍后重试。",
                    L"Snap-Lite · 提取文字",
                    MB_OK | MB_ICONWARNING);
            } else {
                MarkCopiedFeedback(hwnd, state, 1);
            }
            SetFocus(state->edit);
            return 0;
        }
        case kButtonCopyAll:
            if (!CopyUnicodeTextToClipboard(hwnd, state->text)) {
                MessageBoxW(
                    hwnd,
                    L"复制失败：剪贴板被其他程序占用，请稍后重试。",
                    L"Snap-Lite · 提取文字",
                    MB_OK | MB_ICONWARNING);
            } else {
                MarkCopiedFeedback(hwnd, state, 2);
            }
            SetFocus(state->edit);
            return 0;
        case kButtonCloseTop:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
        }
        break;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_NCDESTROY:
        if (g_activePanel == hwnd) g_activePanel = nullptr;
        if (state) {
            if (state->titleFont) DeleteObject(state->titleFont);
            if (state->bodyFont) DeleteObject(state->bodyFont);
            if (state->smallFont) DeleteObject(state->smallFont);
            if (state->editBrush) DeleteObject(state->editBrush);
            delete state;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        // 面板抢走了键盘焦点；关闭后必须还给截图窗口，
        // 否则 Esc / Enter 等截图快捷键会继续失效。
        if (HWND owner = GetWindow(hwnd, GW_OWNER)) {
            if (IsWindow(owner)) {
                SetFocus(owner);
                SetForegroundWindow(owner);
            }
        }
        return 0;
    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool EnsurePanelClass() {
    static bool attempted = false;
    static bool registered = false;
    if (attempted) return registered;
    attempted = true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    wc.lpfnWndProc = PanelProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kPanelClass;

    registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

UINT MonitorDpiOrOwner(HMONITOR monitor, HWND owner) {
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static GetDpiForMonitorFn getDpiForMonitor = []() -> GetDpiForMonitorFn {
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        if (!shcore) return nullptr;
        return reinterpret_cast<GetDpiForMonitorFn>(
            reinterpret_cast<void*>(GetProcAddress(shcore, "GetDpiForMonitor")));
    }();
    if (monitor && getDpiForMonitor) {
        UINT dpiX = 0;
        UINT dpiY = 0;
        if (SUCCEEDED(getDpiForMonitor(monitor, 0, &dpiX, &dpiY)) && dpiX > 0) {
            return dpiX;
        }
    }
    const UINT ownerDpi = owner ? GetDpiForWindow(owner) : 0;
    return ownerDpi ? ownerDpi : 96;
}

RECT PanelBounds(HWND owner, const RECT& selectionClientRect) {
    POINT rightTop{selectionClientRect.right, selectionClientRect.top};
    POINT leftTop{selectionClientRect.left, selectionClientRect.top};
    ClientToScreen(owner, &rightTop);
    ClientToScreen(owner, &leftTop);

    // 截图窗口覆盖整个虚拟桌面，窗口原点不能可靠代表当前选区所在的屏幕。
    HMONITOR monitor = MonitorFromPoint(rightTop, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);

    const UINT dpi = MonitorDpiOrOwner(monitor, owner);
    const int workLeft = static_cast<int>(info.rcWork.left);
    const int workTop = static_cast<int>(info.rcWork.top);
    const int workRight = static_cast<int>(info.rcWork.right);
    const int workBottom = static_cast<int>(info.rcWork.bottom);

    const int panelWidth = std::min(Scale(dpi, kPanelWidth), std::max(1, workRight - workLeft - Scale(dpi, 16)));
    const int panelHeight = std::min(Scale(dpi, kPanelHeight), std::max(1, workBottom - workTop - Scale(dpi, 16)));
    const int gap = Scale(dpi, 12);

    int x = rightTop.x + gap;
    if (x + panelWidth > workRight) {
        x = leftTop.x - panelWidth - gap;
    }
    x = std::clamp(x, workLeft, std::max(workLeft, workRight - panelWidth));

    int y = rightTop.y;
    y = std::clamp(y, workTop, std::max(workTop, workBottom - panelHeight));

    return {x, y, x + panelWidth, y + panelHeight};
}

}  // namespace

void ShowOcrResultPanel(HWND owner, const RECT& selectionClientRect, const std::wstring& text) {
    if (!owner || text.empty() || !EnsurePanelClass()) return;

    if (g_activePanel && IsWindow(g_activePanel)) {
        DestroyWindow(g_activePanel);
        g_activePanel = nullptr;
    }

    auto* state = new PanelState();
    state->text = text;

    const RECT bounds = PanelBounds(owner, selectionClientRect);
    HWND panel = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kPanelClass,
        L"Snap-Lite · 文字识别结果",
        WS_POPUP,
        bounds.left,
        bounds.top,
        bounds.right - bounds.left,
        bounds.bottom - bounds.top,
        owner,
        nullptr,
        GetModuleHandleW(nullptr),
        state);

    if (!panel) {
        delete state;
        return;
    }

    g_activePanel = panel;
    ShowWindow(panel, SW_SHOWNORMAL);
    SetForegroundWindow(panel);
}

}  // namespace snaplite
