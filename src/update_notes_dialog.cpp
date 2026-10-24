#include "update_notes_dialog.h"

#include "ui_theme.h"

#include <algorithm>
#include <iterator>
#include <string>

#include <richedit.h>
#include <windowsx.h>

namespace snaplite {
namespace {

constexpr wchar_t kDialogClass[] = L"SnapLiteUpdateNotesDialog";
constexpr int kDialogWidth = 560;
constexpr int kDialogHeight = 480;
constexpr int kInstallButton = 101;
constexpr int kLaterButton = 102;
constexpr int kCloseButton = 103;
constexpr COLORREF kUpdateSecondaryText = RGB(124, 107, 116);

struct DialogState {
    HWND notes{};
    HWND install{};
    HWND later{};
    HWND close{};
    HFONT titleFont{};
    HFONT bodyFont{};
    HFONT smallFont{};
    HBRUSH cardBrush{};
    RECT notesFrame{};
    std::wstring version;
    std::wstring notesText;
    UINT dpi{96};
    int footerTop{};
    bool notesScrollVisible{true};
    UpdateNotesDialogChoice choice{UpdateNotesDialogChoice::Later};
};

int Scale(const DialogState* state, int value) {
    return MulDiv(value, static_cast<int>(state ? state->dpi : 96), 96);
}

void ApplyFont(HWND control, HFONT font) {
    if (control && font) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

void UpdateNotesScrollBar(DialogState* state) {
    if (!state || !state->notes) return;

    RECT client{};
    GetClientRect(state->notes, &client);
    HDC dc = GetDC(state->notes);
    TEXTMETRICW metrics{};
    HGDIOBJ oldFont = dc && state->bodyFont ? SelectObject(dc, state->bodyFont) : nullptr;
    const bool measured = dc && GetTextMetricsW(dc, &metrics) != FALSE;
    if (oldFont) SelectObject(dc, oldFont);
    if (dc) ReleaseDC(state->notes, dc);
    if (!measured || metrics.tmHeight <= 0) return;

    const int visibleLines = std::max(
        1,
        static_cast<int>(client.bottom - client.top) / static_cast<int>(metrics.tmHeight));
    const int lineCount = static_cast<int>(SendMessageW(state->notes, EM_GETLINECOUNT, 0, 0));
    const bool shouldShow = lineCount > visibleLines;
    if (shouldShow != state->notesScrollVisible) {
        ShowScrollBar(state->notes, SB_VERT, shouldShow);
        state->notesScrollVisible = shouldShow;
    }
}

void RecreateFonts(HWND hwnd, DialogState* state) {
    if (!state) return;

    const HFONT oldTitle = state->titleFont;
    const HFONT oldBody = state->bodyFont;
    const HFONT oldSmall = state->smallFont;
    const HFONT newTitle = ui::MakeFont(state->dpi, 16, FW_SEMIBOLD);
    const HFONT newBody = ui::MakeFont(state->dpi, 10, FW_NORMAL);
    const HFONT newSmall = ui::MakeFont(state->dpi, 9, FW_NORMAL);
    state->titleFont = newTitle ? newTitle : oldTitle;
    state->bodyFont = newBody ? newBody : oldBody;
    state->smallFont = newSmall ? newSmall : oldSmall;
    ApplyFont(state->notes, state->bodyFont);
    ApplyFont(state->install, state->bodyFont);
    ApplyFont(state->later, state->bodyFont);
    ApplyFont(state->close, state->titleFont);
    if (newTitle && oldTitle) DeleteObject(oldTitle);
    if (newBody && oldBody) DeleteObject(oldBody);
    if (newSmall && oldSmall) DeleteObject(oldSmall);
    InvalidateRect(hwnd, nullptr, TRUE);
}

std::wstring PrepareNotes(const std::wstring& version, const std::wstring& notes) {
    std::wstring body = notes;
    if (!body.empty() && body.front() == L'\uFEFF') body.erase(body.begin());

    const auto firstBreak = body.find_first_of(L"\r\n");
    const std::wstring title = L"Snap-Lite v" + version;
    if (body.substr(0, firstBreak) == title && firstBreak != std::wstring::npos) {
        body.erase(0, firstBreak);
        while (!body.empty() && (body.front() == L'\r' || body.front() == L'\n')) {
            body.erase(body.begin());
        }
    }

    std::wstring normalized;
    normalized.reserve(body.size() + 16);
    for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] == L'\r') {
            if (i + 1 < body.size() && body[i + 1] == L'\n') ++i;
            normalized += L"\r\n";
        } else if (body[i] == L'\n') {
            normalized += L"\r\n";
        } else {
            normalized.push_back(body[i]);
        }
    }
    if (normalized.empty()) return L"此版本没有提供更新说明。";
    return normalized;
}

void LayoutDialog(HWND hwnd, DialogState* state) {
    if (!state) return;

    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = static_cast<int>(client.right - client.left);
    const int height = static_cast<int>(client.bottom - client.top);
    const int margin = Scale(state, 24);
    const int buttonWidth = Scale(state, 124);
    const int buttonHeight = Scale(state, 38);
    const int buttonGap = Scale(state, 10);
    const int buttonTop = height - margin - buttonHeight;
    const int footerHeight = Scale(state, 30);

    state->footerTop = buttonTop - Scale(state, 10) - footerHeight;
    state->notesFrame = {
        margin,
        Scale(state, 126),
        width - margin,
        state->footerTop - Scale(state, 12),
    };

    const int notesInset = Scale(state, 13);
    MoveWindow(
        state->notes,
        state->notesFrame.left + notesInset,
        state->notesFrame.top + notesInset,
        state->notesFrame.right - state->notesFrame.left - notesInset * 2,
        state->notesFrame.bottom - state->notesFrame.top - notesInset * 2,
        TRUE);
    const int textInset = Scale(state, 12);
    SendMessageW(
        state->notes,
        EM_SETMARGINS,
        EC_LEFTMARGIN | EC_RIGHTMARGIN,
        MAKELPARAM(textInset, textInset));
    UpdateNotesScrollBar(state);

    const int closeSize = Scale(state, 34);
    MoveWindow(state->close, width - margin - closeSize, Scale(state, 16), closeSize, closeSize, TRUE);

    int buttonLeft = width - margin - buttonWidth;
    MoveWindow(state->install, buttonLeft, buttonTop, buttonWidth, buttonHeight, TRUE);
    buttonLeft -= buttonGap + buttonWidth;
    MoveWindow(state->later, buttonLeft, buttonTop, buttonWidth, buttonHeight, TRUE);

    InvalidateRect(hwnd, nullptr, TRUE);
}

void DrawDialog(HWND hwnd, DialogState* state) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd, &paint);

    RECT client{};
    GetClientRect(hwnd, &client);
    ui::FillSolid(dc, client, ui::kSurface);

    HPEN border = CreatePen(PS_SOLID, 1, ui::kBorder);
    const HGDIOBJ oldPen = SelectObject(dc, border);
    const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    RoundRect(dc, 0, 0, client.right, client.bottom, Scale(state, 18), Scale(state, 18));
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(border);

    const int margin = Scale(state, 24);
    const int iconSize = Scale(state, 40);
    RECT icon{margin, Scale(state, 22), margin + iconSize, Scale(state, 22) + iconSize};
    ui::FillRoundRect(dc, icon, iconSize / 2, ui::kCream, ui::kBorder);

    HPEN checkPen = CreatePen(PS_SOLID, std::max(1, Scale(state, 2)), ui::kAccentStrong);
    const HGDIOBJ oldCheckPen = SelectObject(dc, checkPen);
    const int iconCenterX = (icon.left + icon.right) / 2;
    const int iconCenterY = (icon.top + icon.bottom) / 2;
    MoveToEx(dc, iconCenterX - Scale(state, 7), iconCenterY, nullptr);
    LineTo(dc, iconCenterX - Scale(state, 2), iconCenterY + Scale(state, 5));
    LineTo(dc, iconCenterX + Scale(state, 8), iconCenterY - Scale(state, 7));
    SelectObject(dc, oldCheckPen);
    DeleteObject(checkPen);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ui::kText);
    if (state && state->titleFont) SelectObject(dc, state->titleFont);
    RECT titleRect{Scale(state, 76), Scale(state, 19), client.right - Scale(state, 72), Scale(state, 48)};
    DrawTextW(dc, L"发现新版本", -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SetTextColor(dc, kUpdateSecondaryText);
    if (state && state->smallFont) SelectObject(dc, state->smallFont);
    const std::wstring subtitle = L"v" + state->version + L"  ·  安装包已通过校验";
    RECT subtitleRect{Scale(state, 76), Scale(state, 48), client.right - Scale(state, 70), Scale(state, 74)};
    DrawTextW(dc, subtitle.c_str(), -1, &subtitleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    HPEN divider = CreatePen(PS_SOLID, 1, ui::kBorder);
    const HGDIOBJ oldDivider = SelectObject(dc, divider);
    MoveToEx(dc, margin, Scale(state, 86), nullptr);
    LineTo(dc, client.right - margin, Scale(state, 86));
    SelectObject(dc, oldDivider);
    DeleteObject(divider);

    SetTextColor(dc, ui::kText);
    if (state && state->bodyFont) SelectObject(dc, state->bodyFont);
    RECT sectionRect{margin, Scale(state, 96), client.right - margin, Scale(state, 120)};
    DrawTextW(dc, L"更新内容", -1, &sectionRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    ui::FillRoundRect(dc, state->notesFrame, Scale(state, 12), ui::kCard, ui::kBorder);

    SetTextColor(dc, kUpdateSecondaryText);
    RECT footerRect{margin, state->footerTop, client.right - margin * 2 - Scale(state, 124) * 2 - Scale(state, 10), state->footerTop + Scale(state, 30)};
    DrawTextW(
        dc,
        L"立即更新将启动安装程序；Windows 可能会请求管理员授权。",
        -1,
        &footerRect,
        DT_LEFT | DT_VCENTER | DT_WORDBREAK);

    EndPaint(hwnd, &paint);
}

void DrawButton(const DRAWITEMSTRUCT* draw, DialogState* state) {
    if (!draw || !state) return;

    RECT rect = draw->rcItem;
    const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
    const bool hot = (draw->itemState & ODS_HOTLIGHT) != 0;
    const bool focused = (draw->itemState & ODS_FOCUS) != 0;
    const bool primary = draw->CtlID == kInstallButton;
    const bool close = draw->CtlID == kCloseButton;

    if (close) {
        const COLORREF fill = pressed ? ui::kPressed : (hot ? ui::kHover : ui::kSurface);
        ui::FillRoundRect(draw->hDC, rect, Scale(state, 10), fill, fill);
        const int inset = Scale(state, 11);
        HPEN pen = CreatePen(PS_SOLID, std::max(1, Scale(state, 1)), ui::kMutedText);
        const HGDIOBJ oldPen = SelectObject(draw->hDC, pen);
        MoveToEx(draw->hDC, rect.left + inset, rect.top + inset, nullptr);
        LineTo(draw->hDC, rect.right - inset, rect.bottom - inset);
        MoveToEx(draw->hDC, rect.right - inset, rect.top + inset, nullptr);
        LineTo(draw->hDC, rect.left + inset, rect.bottom - inset);
        SelectObject(draw->hDC, oldPen);
        DeleteObject(pen);
        return;
    }

    COLORREF fill = ui::kCream;
    COLORREF outline = ui::kBorder;
    COLORREF textColor = ui::kText;
    if (primary) {
        fill = pressed ? ui::kAccentStrong : ui::kAccent;
        outline = fill;
        textColor = RGB(45, 40, 44);
    } else if (pressed) {
        fill = ui::kPressed;
        outline = ui::kAccentStrong;
    } else if (hot) {
        fill = ui::kHover;
        outline = ui::kAccent;
    }
    ui::FillRoundRect(draw->hDC, rect, Scale(state, 10), fill, outline);

    wchar_t label[64]{};
    GetWindowTextW(draw->hwndItem, label, static_cast<int>(std::size(label)));
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, textColor);
    if (state->bodyFont) SelectObject(draw->hDC, state->bodyFont);
    DrawTextW(draw->hDC, label, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if (focused) {
        InflateRect(&rect, -Scale(state, 5), -Scale(state, 5));
        DrawFocusRect(draw->hDC, &rect);
    }
}

LRESULT CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<DialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
    case WM_CREATE: {
        if (!state) return -1;
        state->dpi = GetDpiForWindow(hwnd);
        if (state->dpi == 0) state->dpi = 96;
        state->cardBrush = CreateSolidBrush(ui::kCard);
        RecreateFonts(hwnd, state);

        state->notes = CreateWindowExW(
            0,
            MSFTEDIT_CLASS,
            state->notesText.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | ES_NOHIDESEL,
            0, 0, 0, 0,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        state->later = CreateWindowExW(
            0, L"BUTTON", L"稍后",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | BS_NOTIFY,
            0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLaterButton)),
            GetModuleHandleW(nullptr), nullptr);
        state->install = CreateWindowExW(
            0, L"BUTTON", L"立即更新",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | BS_DEFPUSHBUTTON | BS_NOTIFY,
            0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInstallButton)),
            GetModuleHandleW(nullptr), nullptr);
        state->close = CreateWindowExW(
            0, L"BUTTON", L"关闭",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | BS_NOTIFY,
            0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCloseButton)),
            GetModuleHandleW(nullptr), nullptr);
        if (!state->notes || !state->later || !state->install || !state->close) return -1;

        ApplyFont(state->notes, state->bodyFont);
        ApplyFont(state->later, state->bodyFont);
        ApplyFont(state->install, state->bodyFont);
        ApplyFont(state->close, state->titleFont);
        SendMessageW(state->notes, EM_SETTYPOGRAPHYOPTIONS, TO_ADVANCEDTYPOGRAPHY, TO_ADVANCEDTYPOGRAPHY);
        SendMessageW(state->notes, EM_SETBKGNDCOLOR, 0, ui::kCard);
        LayoutDialog(hwnd, state);
        ui::SetRoundedWindowRegion(hwnd, Scale(state, 18));
        SetFocus(state->install);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        DrawDialog(hwnd, state);
        return 0;
    case WM_SIZE:
        LayoutDialog(hwnd, state);
        return 0;
    case WM_DRAWITEM:
        DrawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam), state);
        return TRUE;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        if (state && state->cardBrush) {
            SetTextColor(reinterpret_cast<HDC>(wParam), ui::kText);
            SetBkColor(reinterpret_cast<HDC>(wParam), ui::kCard);
            return reinterpret_cast<LRESULT>(state->cardBrush);
        }
        break;
    case WM_NCHITTEST: {
        const LRESULT hit = DefWindowProcW(hwnd, message, wParam, lParam);
        if (hit != HTCLIENT) return hit;
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &point);
        RECT client{};
        GetClientRect(hwnd, &client);
        if (point.y < Scale(state, 86) && point.x < client.right - Scale(state, 56)) {
            return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_DPICHANGED: {
        if (!state) return 0;
        state->dpi = HIWORD(wParam);
        RecreateFonts(hwnd, state);
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(
            hwnd,
            HWND_TOPMOST,
            suggested->left,
            suggested->top,
            suggested->right - suggested->left,
            suggested->bottom - suggested->top,
            SWP_NOACTIVATE);
        LayoutDialog(hwnd, state);
        ui::SetRoundedWindowRegion(hwnd, Scale(state, 18));
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wParam) != BN_CLICKED || !state) break;
        if (LOWORD(wParam) == kInstallButton) {
            state->choice = UpdateNotesDialogChoice::Install;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wParam) == kLaterButton || LOWORD(wParam) == kCloseButton) {
            state->choice = UpdateNotesDialogChoice::Later;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (state) state->choice = UpdateNotesDialogChoice::Later;
        DestroyWindow(hwnd);
        return 0;
    case WM_NCDESTROY:
        if (state) {
            if (state->titleFont) DeleteObject(state->titleFont);
            if (state->bodyFont) DeleteObject(state->bodyFont);
            if (state->smallFont) DeleteObject(state->smallFont);
            if (state->cardBrush) DeleteObject(state->cardBrush);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool EnsureDialogClass(HINSTANCE instance) {
    static bool registered = false;
    static HMODULE richEditLibrary = LoadLibraryW(L"Msftedit.dll");
    if (!richEditLibrary) return false;
    if (registered) return true;

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = DialogProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    windowClass.lpszClassName = kDialogClass;
    registered = RegisterClassExW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

}  // namespace

UpdateNotesDialogChoice ShowUpdateNotesDialog(
    HWND owner,
    const std::wstring& version,
    const std::wstring& notes) {
    if (!owner || !IsWindow(owner)) return UpdateNotesDialogChoice::Failed;

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!EnsureDialogClass(instance)) return UpdateNotesDialogChoice::Failed;

    DialogState state;
    state.version = version;
    state.notesText = PrepareNotes(version, notes);
    state.dpi = GetDpiForWindow(owner);
    if (state.dpi == 0) state.dpi = GetDpiForSystem();
    if (state.dpi == 0) state.dpi = 96;

    HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    RECT work{};
    if (GetMonitorInfoW(monitor, &monitorInfo)) {
        work = monitorInfo.rcWork;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    }

    const int availableWidth = std::max(
        1,
        static_cast<int>(work.right - work.left) - Scale(&state, 32));
    const int availableHeight = std::max(
        1,
        static_cast<int>(work.bottom - work.top) - Scale(&state, 32));
    const int maxDpiByWidth = MulDiv(availableWidth, 96, kDialogWidth);
    const int maxDpiByHeight = MulDiv(availableHeight, 96, kDialogHeight);
    state.dpi = static_cast<UINT>(std::max(
        72,
        std::min({static_cast<int>(state.dpi), maxDpiByWidth, maxDpiByHeight})));

    const int width = Scale(&state, kDialogWidth);
    const int height = Scale(&state, kDialogHeight);
    const int x = work.left + std::max(0L, (work.right - work.left - width) / 2);
    const int y = work.top + std::max(0L, (work.bottom - work.top - height) / 2);

    HWND dialog = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT,
        kDialogClass,
        L"Snap-Lite 更新",
        WS_POPUP,
        x,
        y,
        width,
        height,
        owner,
        nullptr,
        instance,
        &state);
    if (!dialog) return UpdateNotesDialogChoice::Failed;

    const bool ownerWasEnabled = IsWindowEnabled(owner) != FALSE;
    if (ownerWasEnabled) EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOWNORMAL);
    UpdateWindow(dialog);
    SetForegroundWindow(dialog);

    MSG message{};
    while (IsWindow(dialog)) {
        const BOOL received = GetMessageW(&message, nullptr, 0, 0);
        if (received <= 0) {
            if (received == 0) PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            SendMessageW(dialog, WM_CLOSE, 0, 0);
            continue;
        }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    if (IsWindow(dialog)) DestroyWindow(dialog);
    if (ownerWasEnabled) EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    return state.choice;
}

}  // namespace snaplite
