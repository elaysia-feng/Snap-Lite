#include "update_status_toast.h"

#include "ui_theme.h"

#include <windowsx.h>

#include <algorithm>
#include <string>
#include <string_view>

namespace snaplite {
namespace {

constexpr wchar_t kToastClass[] = L"SnapLiteUpdateStatusToast";
constexpr int kToastWidth = 380;
constexpr int kToastHeight = 194;
constexpr int kToastRadius = 16;

enum class HitTarget { None, Close, Primary, Secondary };

struct ToastState {
    HWND owner{};
    UpdateStatusToastState status{UpdateStatusToastState::Checking};
    std::wstring currentVersion;
    std::wstring latestVersion;
    std::wstring detail;
    UINT dpi{96};
    HFONT titleFont{};
    HFONT statusFont{};
    HFONT bodyFont{};
    RECT closeRect{};
    RECT primaryRect{};
    RECT secondaryRect{};
    HitTarget hovered{HitTarget::None};
    HitTarget pressed{HitTarget::None};
    bool trackingMouse{};
};

struct ToastCreateContext {
    ToastState* state{};
    bool attached{};
};

HWND g_toast = nullptr;
bool g_suppressResultAfterDismiss = false;

int Scale(const ToastState* state, int value) {
    const UINT dpi = state && state->dpi ? state->dpi : 96;
    return MulDiv(value, static_cast<int>(dpi), 96);
}

bool HasPrimaryAction(UpdateStatusToastState status) {
    return status == UpdateStatusToastState::Available ||
           status == UpdateStatusToastState::CheckFailed ||
           status == UpdateStatusToastState::DownloadFailed;
}

std::wstring StatusTitle(const ToastState* state) {
    if (!state) return {};
    switch (state->status) {
    case UpdateStatusToastState::Checking: return L"正在检查更新";
    case UpdateStatusToastState::UpToDate: return L"当前已是最新版本";
    case UpdateStatusToastState::Available:
        return L"发现新版本 v" + state->latestVersion;
    case UpdateStatusToastState::CheckFailed: return L"暂时无法检查更新";
    case UpdateStatusToastState::Downloading: return L"正在准备更新";
    case UpdateStatusToastState::DownloadFailed: return L"更新准备失败";
    }
    return {};
}

std::wstring StatusDetail(const ToastState* state) {
    if (!state) return {};
    if (!state->detail.empty()) return state->detail;
    switch (state->status) {
    case UpdateStatusToastState::Checking:
        return L"正在连接 GitHub 查询最新版本，请稍候。";
    case UpdateStatusToastState::UpToDate:
        return L"你使用的版本已经是最新版本。";
    case UpdateStatusToastState::Available:
        return L"下载安装包后会先校验完整性，再显示更新说明。";
    case UpdateStatusToastState::CheckFailed:
        return L"请确认网络连接后重新检查。";
    case UpdateStatusToastState::Downloading:
        return L"正在下载并验证安装包。";
    case UpdateStatusToastState::DownloadFailed:
        return L"请检查网络后重试下载。";
    }
    return {};
}

std::wstring PrimaryLabel(UpdateStatusToastState status) {
    switch (status) {
    case UpdateStatusToastState::Available: return L"立即更新";
    case UpdateStatusToastState::CheckFailed: return L"重新检查";
    case UpdateStatusToastState::DownloadFailed: return L"重试下载";
    default: return {};
    }
}

HitTarget HitTest(const ToastState* state, POINT point) {
    if (!state) return HitTarget::None;
    if (PtInRect(&state->closeRect, point)) return HitTarget::Close;
    if (HasPrimaryAction(state->status) && PtInRect(&state->primaryRect, point)) {
        return HitTarget::Primary;
    }
    if (PtInRect(&state->secondaryRect, point)) return HitTarget::Secondary;
    return HitTarget::None;
}

void DrawTextLine(
    HDC dc,
    HFONT font,
    COLORREF color,
    std::wstring_view text,
    RECT rect,
    UINT format) {
    const HGDIOBJ previousFont = font ? SelectObject(dc, font) : nullptr;
    const COLORREF previousColor = SetTextColor(dc, color);
    const int previousMode = SetBkMode(dc, TRANSPARENT);
    std::wstring copy(text);
    DrawTextW(dc, copy.c_str(), -1, &rect, format);
    SetBkMode(dc, previousMode);
    SetTextColor(dc, previousColor);
    if (previousFont) SelectObject(dc, previousFont);
}

void DrawButton(
    HDC dc,
    const ToastState* state,
    const RECT& rect,
    std::wstring_view label,
    bool primary,
    HitTarget target) {
    if (!state || rect.right <= rect.left) return;

    const bool pressed = state->pressed == target;
    const bool hovered = state->hovered == target;
    COLORREF fill = primary ? ui::kAccentAction : ui::kCream;
    COLORREF border = primary ? ui::kAccentAction : ui::kBorder;
    COLORREF text = primary ? RGB(255, 255, 255) : ui::kText;
    if (pressed) {
        fill = primary ? ui::kAccentActionPressed : ui::kPressed;
        border = fill;
    } else if (hovered) {
        fill = primary ? ui::kAccentActionPressed : ui::kHover;
        border = fill;
    }

    ui::FillRoundRect(dc, rect, Scale(state, 9), fill, border);
    DrawTextLine(
        dc,
        state->bodyFont,
        text,
        label,
        rect,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

COLORREF StatusColor(UpdateStatusToastState status) {
    switch (status) {
    case UpdateStatusToastState::UpToDate: return RGB(77, 137, 105);
    case UpdateStatusToastState::CheckFailed:
    case UpdateStatusToastState::DownloadFailed: return ui::kDanger;
    default: return ui::kAccentStrong;
    }
}

void LayoutToast(HWND hwnd, ToastState* state) {
    if (!state) return;
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = static_cast<int>(client.right - client.left);
    const int height = static_cast<int>(client.bottom - client.top);
    const int margin = Scale(state, 16);
    const int buttonHeight = Scale(state, 32);
    const int buttonTop = height - margin - buttonHeight;
    const int primaryWidth = Scale(state, 106);
    const int secondaryWidth = Scale(state, 76);
    const int buttonGap = Scale(state, 8);

    const int closeSize = Scale(state, 28);
    state->closeRect = {
        width - margin - closeSize,
        Scale(state, 12),
        width - margin,
        Scale(state, 12) + closeSize};
    state->primaryRect = {};
    state->secondaryRect = {
        width - margin - secondaryWidth,
        buttonTop,
        width - margin,
        buttonTop + buttonHeight};

    if (HasPrimaryAction(state->status)) {
        state->primaryRect = {
            width - margin - primaryWidth,
            buttonTop,
            width - margin,
            buttonTop + buttonHeight};
        state->secondaryRect = {
            state->primaryRect.left - buttonGap - secondaryWidth,
            buttonTop,
            state->primaryRect.left - buttonGap,
            buttonTop + buttonHeight};
    }
}

void PaintToast(HWND hwnd, ToastState* state) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd, &paint);
    if (!state) {
        EndPaint(hwnd, &paint);
        return;
    }
    RECT client{};
    GetClientRect(hwnd, &client);

    ui::FillRoundRect(dc, client, Scale(state, kToastRadius), ui::kSurface, ui::kBorder);

    const int margin = Scale(state, 18);
    RECT titleRect{margin, Scale(state, 12), client.right - Scale(state, 52), Scale(state, 34)};
    DrawTextLine(
        dc,
        state ? state->titleFont : nullptr,
        ui::kText,
        L"Snap-Lite 自动更新",
        titleRect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    if (state && state->hovered == HitTarget::Close) {
        ui::FillRoundRect(dc, state->closeRect, Scale(state, 8), ui::kHover, ui::kHover);
    }
    HPEN closePen = CreatePen(PS_SOLID, std::max(1, Scale(state, 1)), ui::kTextSecondary);
    if (closePen) {
        const HGDIOBJ previousPen = SelectObject(dc, closePen);
        const int centerX = (state->closeRect.left + state->closeRect.right) / 2;
        const int centerY = (state->closeRect.top + state->closeRect.bottom) / 2;
        const int cross = Scale(state, 4);
        MoveToEx(dc, centerX - cross, centerY - cross, nullptr);
        LineTo(dc, centerX + cross + 1, centerY + cross + 1);
        MoveToEx(dc, centerX + cross, centerY - cross, nullptr);
        LineTo(dc, centerX - cross - 1, centerY + cross + 1);
        SelectObject(dc, previousPen);
        DeleteObject(closePen);
    }

    const std::wstring version = L"当前版本 v" + state->currentVersion;
    RECT versionRect{margin, Scale(state, 38), client.right - margin, Scale(state, 56)};
    DrawTextLine(
        dc,
        state ? state->bodyFont : nullptr,
        ui::kTextSecondary,
        version,
        versionRect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    HPEN divider = CreatePen(PS_SOLID, 1, ui::kBorder);
    if (divider) {
        const HGDIOBJ previousPen = SelectObject(dc, divider);
        MoveToEx(dc, margin, Scale(state, 62), nullptr);
        LineTo(dc, client.right - margin, Scale(state, 62));
        SelectObject(dc, previousPen);
        DeleteObject(divider);
    }

    const int dotSize = Scale(state, 8);
    const int dotLeft = margin;
    const int dotTop = Scale(state, 76);
    HBRUSH dot = CreateSolidBrush(StatusColor(state->status));
    if (dot) {
        const HGDIOBJ previousBrush = SelectObject(dc, dot);
        const HGDIOBJ previousPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, dotLeft, dotTop, dotLeft + dotSize, dotTop + dotSize);
        SelectObject(dc, previousPen);
        SelectObject(dc, previousBrush);
        DeleteObject(dot);
    }

    RECT statusRect{
        margin + Scale(state, 16),
        Scale(state, 68),
        client.right - margin,
        Scale(state, 91)};
    const std::wstring statusTitle = StatusTitle(state);
    DrawTextLine(dc, state->statusFont, ui::kText, statusTitle,
                 statusRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT detailRect{
        margin,
        Scale(state, 96),
        client.right - margin,
        state->secondaryRect.top - Scale(state, 8)};
    const std::wstring detail = StatusDetail(state);
    DrawTextLine(dc, state->bodyFont, ui::kTextSecondary, detail,
                 detailRect, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);

    if (state) {
        const std::wstring primary = PrimaryLabel(state->status);
        if (!primary.empty()) {
            DrawButton(dc, state, state->primaryRect, primary, true, HitTarget::Primary);
        }
        DrawButton(
            dc,
            state,
            state->secondaryRect,
            HasPrimaryAction(state->status) ? L"稍后" : L"关闭",
            false,
            HitTarget::Secondary);
    }

    EndPaint(hwnd, &paint);
}

void CloseFromUser(HWND hwnd, ToastState* state) {
    if (state && (state->status == UpdateStatusToastState::Checking ||
                  state->status == UpdateStatusToastState::Downloading)) {
        g_suppressResultAfterDismiss = true;
    }
    DestroyWindow(hwnd);
}

void ActivateButton(HWND hwnd, ToastState* state, HitTarget target) {
    if (!state) return;
    if (target == HitTarget::Close || target == HitTarget::Secondary) {
        CloseFromUser(hwnd, state);
        return;
    }
    if (target != HitTarget::Primary) return;

    if (state->status == UpdateStatusToastState::CheckFailed) {
        state->status = UpdateStatusToastState::Checking;
        state->detail = L"正在连接 GitHub 查询最新版本，请稍候。";
        g_suppressResultAfterDismiss = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        PostMessageW(
            state->owner,
            WM_SNAPLITE_UPDATE_TOAST_ACTION,
            static_cast<WPARAM>(UpdateStatusToastAction::Check),
            0);
        return;
    }

    state->status = UpdateStatusToastState::Downloading;
    state->detail = L"正在下载并验证安装包。";
    InvalidateRect(hwnd, nullptr, FALSE);
    PostMessageW(
        state->owner,
        WM_SNAPLITE_UPDATE_TOAST_ACTION,
        static_cast<WPARAM>(UpdateStatusToastAction::Download),
        0);
}

LRESULT CALLBACK ToastProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<ToastState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* context = reinterpret_cast<ToastCreateContext*>(create->lpCreateParams);
        if (!context || !context->state) return FALSE;
        state = context->state;
        context->attached = true;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }
    case WM_CREATE: {
        if (!state) return -1;
        state->dpi = GetDpiForWindow(hwnd);
        state->titleFont = ui::MakeFont(state->dpi, 11, FW_SEMIBOLD);
        state->statusFont = ui::MakeFont(state->dpi, 12, FW_SEMIBOLD);
        state->bodyFont = ui::MakeFont(state->dpi, 9, FW_NORMAL);
        LayoutToast(hwnd, state);
        ui::SetRoundedWindowRegion(hwnd, Scale(state, kToastRadius));
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        PaintToast(hwnd, state);
        return 0;
    case WM_SIZE:
        LayoutToast(hwnd, state);
        if (state) ui::SetRoundedWindowRegion(hwnd, Scale(state, kToastRadius));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        if (!state) break;
        if (!state->trackingMouse) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tracking);
            state->trackingMouse = true;
        }
        const HitTarget hover = HitTest(state, {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        if (state->hovered != hover) {
            state->hovered = hover;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (state) {
            state->trackingMouse = false;
            state->hovered = HitTarget::None;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (state) {
            state->pressed = HitTest(state, {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            if (state->pressed != HitTarget::None) {
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (state) {
            const HitTarget pressed = state->pressed;
            const HitTarget released = HitTest(state, {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            state->pressed = HitTarget::None;
            if (GetCapture() == hwnd) ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
            if (pressed != HitTarget::None && pressed == released) {
                ActivateButton(hwnd, state, released);
            }
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (state && state->pressed != HitTarget::None) {
            state->pressed = HitTarget::None;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_SETCURSOR:
        if (state) {
            POINT cursor{};
            GetCursorPos(&cursor);
            ScreenToClient(hwnd, &cursor);
            if (HitTest(state, cursor) != HitTarget::None) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    case WM_CLOSE:
        CloseFromUser(hwnd, state);
        return 0;
    case WM_NCDESTROY:
        if (g_toast == hwnd) g_toast = nullptr;
        if (state) {
            if (state->titleFont) DeleteObject(state->titleFont);
            if (state->statusFont) DeleteObject(state->statusFont);
            if (state->bodyFont) DeleteObject(state->bodyFont);
            delete state;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool EnsureToastClass(HINSTANCE instance) {
    static bool attempted = false;
    static bool registered = false;
    if (attempted) return registered;
    attempted = true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    wc.lpfnWndProc = ToastProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kToastClass;
    registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

void PlaceToast(HWND hwnd, UINT dpi) {
    POINT anchor{};
    GetCursorPos(&anchor);
    HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return;

    const int margin = MulDiv(16, static_cast<int>(dpi), 96);
    int width = MulDiv(kToastWidth, static_cast<int>(dpi), 96);
    int height = MulDiv(kToastHeight, static_cast<int>(dpi), 96);
    const int workWidth = info.rcWork.right - info.rcWork.left;
    const int workHeight = info.rcWork.bottom - info.rcWork.top;
    width = std::min(width, std::max(1, workWidth - margin * 2));
    height = std::min(height, std::max(1, workHeight - margin * 2));
    const int x = std::max(static_cast<int>(info.rcWork.left),
                           static_cast<int>(info.rcWork.right) - width - margin);
    const int y = std::max(static_cast<int>(info.rcWork.top),
                           static_cast<int>(info.rcWork.bottom) - height - margin);
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

}  // namespace

void ShowUpdateStatusToast(
    HWND owner,
    UpdateStatusToastState status,
    const std::wstring& currentVersion,
    const std::wstring& latestVersion,
    const std::wstring& detail) {
    if (!owner || !IsWindow(owner)) return;
    if (status == UpdateStatusToastState::Checking) {
        g_suppressResultAfterDismiss = false;
    } else if (g_suppressResultAfterDismiss) {
        return;
    }

    if (g_toast && IsWindow(g_toast)) {
        auto* state = reinterpret_cast<ToastState*>(GetWindowLongPtrW(g_toast, GWLP_USERDATA));
        if (state && state->owner == owner) {
            state->status = status;
            state->currentVersion = currentVersion;
            state->latestVersion = latestVersion;
            state->detail = detail;
            state->hovered = HitTarget::None;
            state->pressed = HitTarget::None;
            InvalidateRect(g_toast, nullptr, FALSE);
            SetWindowPos(g_toast, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            return;
        }
        DestroyWindow(g_toast);
    }

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!EnsureToastClass(instance)) return;

    auto* state = new ToastState();
    state->owner = owner;
    state->status = status;
    state->currentVersion = currentVersion;
    state->latestVersion = latestVersion;
    state->detail = detail;
    state->dpi = GetDpiForWindow(owner);
    if (!state->dpi) state->dpi = 96;
    ToastCreateContext createContext{state};

    const int width = MulDiv(kToastWidth, static_cast<int>(state->dpi), 96);
    const int height = MulDiv(kToastHeight, static_cast<int>(state->dpi), 96);
    HWND toast = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kToastClass,
        L"Snap-Lite · 自动更新",
        WS_POPUP,
        0,
        0,
        width,
        height,
        owner,
        nullptr,
        instance,
        &createContext);
    if (!toast) {
        if (!createContext.attached) delete state;
        return;
    }

    g_toast = toast;
    PlaceToast(toast, state->dpi);
    ShowWindow(toast, SW_SHOWNOACTIVATE);
}

void CloseUpdateStatusToast(HWND owner) {
    if (!g_toast || !IsWindow(g_toast)) {
        g_toast = nullptr;
        return;
    }
    auto* state = reinterpret_cast<ToastState*>(GetWindowLongPtrW(g_toast, GWLP_USERDATA));
    if (!owner || (state && state->owner == owner)) {
        DestroyWindow(g_toast);
    }
}

}  // namespace snaplite
