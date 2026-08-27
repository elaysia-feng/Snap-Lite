#include "ocr_result_panel.h"

#include "ocr.h"

#include <algorithm>
#include <string>

namespace snaplite {
namespace {

constexpr wchar_t kPanelClass[] = L"SnapLiteOcrResultPanel";
constexpr int kButtonCopySelected = 101;
constexpr int kButtonCopyAll = 102;
constexpr int kButtonClose = 103;
constexpr int kPanelWidth = 390;
constexpr int kPanelHeight = 340;

struct PanelState {
    HWND edit{};
    HWND hint{};
    HWND copySelected{};
    HWND copyAll{};
    HWND close{};
    HFONT font{};
    std::wstring text;
};

HWND g_activePanel = nullptr;

void ApplyFont(HWND control, HFONT font) {
    if (control && font) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
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

void LayoutPanel(HWND hwnd, PanelState* state) {
    if (!state) return;

    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = std::max(240, static_cast<int>(client.right - client.left));
    const int height = std::max(180, static_cast<int>(client.bottom - client.top));
    const int pad = 12;
    const int hintHeight = 24;
    const int buttonHeight = 30;
    const int buttonGap = 8;
    const int buttonWidth = 92;

    MoveWindow(state->hint, pad, pad, width - pad * 2, hintHeight, TRUE);

    const int buttonsTop = height - pad - buttonHeight;
    const int editTop = pad + hintHeight + 6;
    const int editHeight = std::max(70, buttonsTop - editTop - 10);
    MoveWindow(state->edit, pad, editTop, width - pad * 2, editHeight, TRUE);

    int x = width - pad - buttonWidth;
    MoveWindow(state->close, x, buttonsTop, buttonWidth, buttonHeight, TRUE);
    x -= buttonGap + buttonWidth;
    MoveWindow(state->copyAll, x, buttonsTop, buttonWidth, buttonHeight, TRUE);
    x -= buttonGap + buttonWidth;
    MoveWindow(state->copySelected, x, buttonsTop, buttonWidth, buttonHeight, TRUE);
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
        state->font = CreateFontW(
            -MulDiv(10, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

        state->hint = CreateWindowExW(
            0, L"STATIC", L"拖选需要的文字后 Ctrl+C，或使用下方按钮复制。",
            WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);

        state->edit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", state->text.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL |
                ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
            0, 0, 0, 0, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);

        state->copySelected = CreateWindowExW(
            0, L"BUTTON", L"复制选中",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonCopySelected)),
            GetModuleHandleW(nullptr), nullptr);

        state->copyAll = CreateWindowExW(
            0, L"BUTTON", L"复制全部",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonCopyAll)),
            GetModuleHandleW(nullptr), nullptr);

        state->close = CreateWindowExW(
            0, L"BUTTON", L"关闭",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonClose)),
            GetModuleHandleW(nullptr), nullptr);

        ApplyFont(state->hint, state->font);
        ApplyFont(state->edit, state->font);
        ApplyFont(state->copySelected, state->font);
        ApplyFont(state->copyAll, state->font);
        ApplyFont(state->close, state->font);

        LayoutPanel(hwnd, state);
        SetFocus(state->edit);
        SendMessageW(state->edit, EM_SETSEL, 0, 0);
        return 0;
    }
    case WM_SIZE:
        LayoutPanel(hwnd, state);
        return 0;
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
            CopyUnicodeTextToClipboard(hwnd, selected);
            return 0;
        }
        case kButtonCopyAll:
            CopyUnicodeTextToClipboard(hwnd, state->text);
            return 0;
        case kButtonClose:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_NCDESTROY:
        if (g_activePanel == hwnd) g_activePanel = nullptr;
        if (state) {
            if (state->font) DeleteObject(state->font);
            delete state;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
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
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kPanelClass;

    registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

RECT PanelBounds(HWND owner, const RECT& selectionClientRect) {
    POINT rightTop{selectionClientRect.right, selectionClientRect.top};
    POINT leftTop{selectionClientRect.left, selectionClientRect.top};
    ClientToScreen(owner, &rightTop);
    ClientToScreen(owner, &leftTop);

    HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);

    const RECT work = info.rcWork;
    const int workLeft = static_cast<int>(work.left);
    const int workTop = static_cast<int>(work.top);
    const int workRight = static_cast<int>(work.right);
    const int workBottom = static_cast<int>(work.bottom);

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
        L"Snap-Lite · 提取文字",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
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
