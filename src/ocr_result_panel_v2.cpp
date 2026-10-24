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
constexpr int kPanelWidth = 430;
constexpr int kPanelHeight = 380;
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
};

HWND g_activePanel = nullptr;

int CountVisualLines(const std::wstring& text) {
    if (text.empty()) return 0;
    int lines = 1;
    for (wchar_t ch : text) {
        if (ch == L'\n') ++lines;
    }
    return lines;
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

    HRGN region = CreateRoundRectRgn(
        0,
        0,
        rect.right - rect.left + 1,
        rect.bottom - rect.top + 1,
        kCornerRadius,
        kCornerRadius);
    if (region) {
        // SetWindowRgn 成功后区域的所有权交给系统；失败时必须自己释放，
        // 否则每次调用都会泄漏一个 HRGN。
        if (SetWindowRgn(hwnd, region, TRUE) == 0) {
            DeleteObject(region);
        }
    }
}

void SetEditFormattingRect(HWND edit) {
    if (!edit) return;
    RECT client{};
    GetClientRect(edit, &client);
    RECT format{
        12,
        10,
        std::max(13L, client.right - 12),
        std::max(11L, client.bottom - 10),
    };
    SendMessageW(edit, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&format));
}

void LayoutPanel(HWND hwnd, PanelState* state) {
    if (!state) return;

    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = std::max(320, static_cast<int>(client.right - client.left));
    const int height = std::max(260, static_cast<int>(client.bottom - client.top));
    const int pad = 18;
    const int buttonHeight = 36;
    const int buttonGap = 10;
    const int buttonWidth = 112;

    MoveWindow(state->closeTop, width - 50, 12, 32, 32, TRUE);

    const int buttonsTop = height - pad - buttonHeight;
    const int editTop = kHeaderHeight;
    const int editHeight = std::max(100, buttonsTop - editTop - 14);
    state->editFrame = {pad, editTop, width - pad, editTop + editHeight};

    MoveWindow(
        state->edit,
        state->editFrame.left + 1,
        state->editFrame.top + 1,
        state->editFrame.right - state->editFrame.left - 2,
        state->editFrame.bottom - state->editFrame.top - 2,
        TRUE);
    SetEditFormattingRect(state->edit);

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

void DrawOwnerButton(const DRAWITEMSTRUCT* draw) {
    if (!draw) return;

    RECT rect = draw->rcItem;
    const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;

    wchar_t text[64]{};
    GetWindowTextW(draw->hwndItem, text, static_cast<int>(std::size(text)));

    COLORREF fill = kSecondaryBg;
    COLORREF border = kBorder;
    COLORREF textColor = kText;

    if (draw->CtlID == kButtonCopyAll) {
        fill = pressed ? ui::kAccentActionPressed : ui::kAccentAction;
        border = fill;
        textColor = RGB(255, 255, 255);
    } else if (draw->CtlID == kButtonCloseTop) {
        fill = pressed ? RGB(245, 224, 230) : kPanelBg;
        border = fill;
        textColor = ui::kTextSecondary;
    } else if (pressed) {
        fill = kSecondaryPressed;
    }

    if (disabled) {
        textColor = RGB(180, 170, 175);
    }

    DrawRoundRectFill(draw->hDC, rect, 10, fill, border);

    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, textColor);
    DrawTextW(draw->hDC, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void PaintPanel(HWND hwnd, PanelState* state) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd, &paint);

    RECT client{};
    GetClientRect(hwnd, &client);

    HBRUSH background = CreateSolidBrush(kPanelBg);
    FillRect(dc, &client, background);
    DeleteObject(background);

    HPEN outline = CreatePen(PS_SOLID, 1, kBorder);
    HBRUSH hollow = static_cast<HBRUSH>(GetStockObject(HOLLOW_BRUSH));
    const HGDIOBJ oldPen = SelectObject(dc, outline);
    const HGDIOBJ oldBrush = SelectObject(dc, hollow);
    RoundRect(dc, 0, 0, client.right, client.bottom, kCornerRadius, kCornerRadius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(outline);

    const RECT dotRect{18, 18, 28, 28};
    ui::FillRoundRect(dc, dotRect, 4, kAccent, kAccent);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kText);
    if (state && state->titleFont) SelectObject(dc, state->titleFont);
    RECT titleRect{36, 10, client.right - 58, 38};
    DrawTextW(dc, L"文字识别结果", -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SetTextColor(dc, ui::kTextSecondary);
    if (state && state->smallFont) SelectObject(dc, state->smallFont);
    const int countBadgeRight = static_cast<int>(client.right) - 18;
    const int countBadgeLeft = countBadgeRight - 68;
    RECT subtitleRect{18, 46, countBadgeLeft - 8, 68};
    const std::wstring subtitle = L"保留换行 · 可拖选并复制";
    DrawTextW(dc, subtitle.c_str(), -1, &subtitleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (state) {
        RECT countBadge{countBadgeLeft, 46, countBadgeRight, 68};
        DrawRoundRectFill(dc, countBadge, 10, RGB(255, 238, 243), kBorder);
        SetTextColor(dc, ui::kTextSecondary);
        const std::wstring lineCount = std::to_wstring(CountVisualLines(state->text)) + L" 行";
        DrawTextW(dc, lineCount.c_str(), -1, &countBadge, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    HPEN divider = CreatePen(PS_SOLID, 1, RGB(246, 226, 231));
    if (divider) {
        const HGDIOBJ dividerPreviousPen = SelectObject(dc, divider);
        const HGDIOBJ dividerPreviousBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        MoveToEx(dc, 16, kHeaderHeight - 1, nullptr);
        LineTo(dc, client.right - 16, kHeaderHeight - 1);
        SelectObject(dc, dividerPreviousBrush);
        SelectObject(dc, dividerPreviousPen);
        DeleteObject(divider);
    }

    if (state) {
        DrawRoundRectFill(dc, state->editFrame, 12, kCardBg, kBorder);
    }

    EndPaint(hwnd, &paint);
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

        const UINT dpi = GetDpiForWindow(hwnd);
        state->titleFont = CreateFontW(
            -MulDiv(14, static_cast<int>(dpi), 72), 0, 0, 0, FW_SEMIBOLD,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        state->bodyFont = CreateFontW(
            -MulDiv(11, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        state->smallFont = CreateFontW(
            -MulDiv(9, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
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
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonCopySelected)),
            GetModuleHandleW(nullptr), nullptr);

        state->copyAll = CreateWindowExW(
            0, L"BUTTON", L"复制全部",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonCopyAll)),
            GetModuleHandleW(nullptr), nullptr);

        state->closeTop = CreateWindowExW(
            0, L"BUTTON", L"×",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonCloseTop)),
            GetModuleHandleW(nullptr), nullptr);

        ApplyFont(state->edit, state->bodyFont);
        ApplyFont(state->copySelected, state->bodyFont);
        ApplyFont(state->copyAll, state->bodyFont);
        ApplyFont(state->closeTop, state->titleFont);

        SendMessageW(state->edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(12, 12));
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
    case WM_DRAWITEM:
        DrawOwnerButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
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
        if (point.y >= 0 && point.y < kHeaderHeight && point.x < kPanelWidth - 58) {
            return HTCAPTION;
        }
        return HTCLIENT;
    }
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
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = PanelProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kPanelClass;

    registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
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

    const int workLeft = static_cast<int>(info.rcWork.left);
    const int workTop = static_cast<int>(info.rcWork.top);
    const int workRight = static_cast<int>(info.rcWork.right);
    const int workBottom = static_cast<int>(info.rcWork.bottom);

    int x = rightTop.x + 12;
    if (x + kPanelWidth > workRight) {
        x = leftTop.x - kPanelWidth - 12;
    }
    x = std::clamp(x, workLeft, std::max(workLeft, workRight - kPanelWidth));

    int y = rightTop.y;
    y = std::clamp(y, workTop, std::max(workTop, workBottom - kPanelHeight));

    return {x, y, x + kPanelWidth, y + kPanelHeight};
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
