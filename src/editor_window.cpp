#include "editor_window.h"

#include "capture.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace snaplite {
namespace {
constexpr wchar_t kEditorClass[] = L"SnapLiteEditorWindow";
constexpr int kToolbarHeight = 44;
constexpr int kButtonWidth = 72;
constexpr int kToolbarButtons = 9;

const wchar_t* ToolLabel(EditorWindow::Tool tool) {
    switch (tool) {
    case EditorWindow::Tool::Rectangle: return L"矩形";
    case EditorWindow::Tool::Ellipse: return L"椭圆";
    case EditorWindow::Tool::Arrow: return L"箭头";
    case EditorWindow::Tool::Pen: return L"画笔";
    case EditorWindow::Tool::Mosaic: return L"马赛克";
    }
    return L"";
}

void DeleteBitmaps(std::vector<HBITMAP>& bitmaps) {
    for (HBITMAP bitmap : bitmaps) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
    }
    bitmaps.clear();
}

}  // namespace

EditorWindow::EditorWindow(HINSTANCE instance, HBITMAP bitmap, DoneCallback callback)
    : instance_(instance), bitmap_(bitmap), callback_(std::move(callback)) {
    BITMAP info{};
    if (bitmap_) {
        GetObjectW(bitmap_, sizeof(info), &info);
        imageWidth_ = info.bmWidth;
        imageHeight_ = info.bmHeight;
    }
    windowWidth_ = std::max(imageWidth_, kToolbarButtons * kButtonWidth);
}

EditorWindow::~EditorWindow() {
    if (bitmap_) {
        DeleteObject(bitmap_);
        bitmap_ = nullptr;
    }
    DeleteBitmaps(undo_);
    DeleteBitmaps(redo_);
}

bool EditorWindow::Register(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kEditorClass;
    wc.style = CS_DBLCLKS;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool EditorWindow::Open(HINSTANCE instance, HBITMAP bitmap, DoneCallback callback) {
    if (!bitmap) {
        return false;
    }

    auto* self = new EditorWindow(instance, bitmap, std::move(callback));
    if (self->imageWidth_ <= 0 || self->imageHeight_ <= 0) {
        delete self;
        return false;
    }

    POINT cursor{};
    GetCursorPos(&cursor);

    const int x = std::max(0, cursor.x - self->windowWidth_ / 2);
    const int y = std::max(0, cursor.y - self->imageHeight_ / 2);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kEditorClass,
        L"Snap-Lite Editor",
        WS_POPUP,
        x,
        y,
        self->windowWidth_,
        self->imageHeight_ + kToolbarHeight,
        nullptr,
        nullptr,
        instance,
        self);

    if (!hwnd) {
        delete self;
        return false;
    }

    self->hwnd_ = hwnd;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    return true;
}

void EditorWindow::BeginEdit() {
    HBITMAP snapshot = CloneBitmap(bitmap_);
    if (snapshot) {
        undo_.push_back(snapshot);
        if (undo_.size() > 30) {
            DeleteObject(undo_.front());
            undo_.erase(undo_.begin());
        }
    }
    DeleteBitmaps(redo_);
}

void EditorWindow::Undo() {
    if (undo_.empty()) {
        return;
    }

    HBITMAP current = CloneBitmap(bitmap_);
    if (current) {
        redo_.push_back(current);
    }

    DeleteObject(bitmap_);
    bitmap_ = undo_.back();
    undo_.pop_back();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void EditorWindow::Redo() {
    if (redo_.empty()) {
        return;
    }

    HBITMAP current = CloneBitmap(bitmap_);
    if (current) {
        undo_.push_back(current);
    }

    DeleteObject(bitmap_);
    bitmap_ = redo_.back();
    redo_.pop_back();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void EditorWindow::DrawShape(HDC dc, Tool tool, POINT from, POINT to) {
    HPEN pen = CreatePen(PS_SOLID, 3, RGB(255, 72, 72));
    const HGDIOBJ oldPen = SelectObject(dc, pen);
    const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));

    if (tool == Tool::Rectangle) {
        Rectangle(dc, from.x, from.y, to.x, to.y);
    } else if (tool == Tool::Ellipse) {
        Ellipse(dc, from.x, from.y, to.x, to.y);
    } else if (tool == Tool::Arrow) {
        MoveToEx(dc, from.x, from.y, nullptr);
        LineTo(dc, to.x, to.y);

        const double angle = std::atan2(static_cast<double>(to.y - from.y), static_cast<double>(to.x - from.x));
        constexpr double spread = 0.55;
        constexpr double length = 18.0;
        POINT arrow[3] = {
            to,
            {static_cast<LONG>(to.x - length * std::cos(angle - spread)),
             static_cast<LONG>(to.y - length * std::sin(angle - spread))},
            {static_cast<LONG>(to.x - length * std::cos(angle + spread)),
             static_cast<LONG>(to.y - length * std::sin(angle + spread))},
        };
        HBRUSH brush = CreateSolidBrush(RGB(255, 72, 72));
        SelectObject(dc, brush);
        Polygon(dc, arrow, 3);
        SelectObject(dc, oldBrush);
        DeleteObject(brush);
    }

    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
}

void EditorWindow::DrawPenSegment(POINT from, POINT to) {
    HDC dc = CreateCompatibleDC(nullptr);
    const HGDIOBJ oldBitmap = SelectObject(dc, bitmap_);
    HPEN pen = CreatePen(PS_SOLID, 4, RGB(255, 72, 72));
    const HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, from.x, from.y, nullptr);
    LineTo(dc, to.x, to.y);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBitmap);
    DeleteObject(pen);
    DeleteDC(dc);
}

void EditorWindow::ApplyMosaic(POINT point) {
    constexpr int block = 14;
    const int left = std::clamp((point.x / block) * block, 0, std::max(0, imageWidth_ - 1));
    const int top = std::clamp((point.y / block) * block, 0, std::max(0, imageHeight_ - 1));
    const int right = std::min(imageWidth_, left + block);
    const int bottom = std::min(imageHeight_, top + block);

    HDC dc = CreateCompatibleDC(nullptr);
    const HGDIOBJ oldBitmap = SelectObject(dc, bitmap_);
    const COLORREF color = GetPixel(dc, std::min(imageWidth_ - 1, left + block / 2),
                                   std::min(imageHeight_ - 1, top + block / 2));
    HBRUSH brush = CreateSolidBrush(color == CLR_INVALID ? RGB(128, 128, 128) : color);
    RECT rect{left, top, right, bottom};
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
    SelectObject(dc, oldBitmap);
    DeleteDC(dc);
}

void EditorWindow::DrawPreview(HDC dc) {
    if (!drawing_ || tool_ == Tool::Pen || tool_ == Tool::Mosaic) {
        return;
    }
    DrawShape(dc, tool_, drawStart_, drawCurrent_);
}

void EditorWindow::PaintToolbar(HDC dc) {
    RECT bar{0, imageHeight_, windowWidth_, imageHeight_ + kToolbarHeight};
    HBRUSH background = CreateSolidBrush(RGB(28, 28, 28));
    FillRect(dc, &bar, background);
    DeleteObject(background);

    const wchar_t* labels[kToolbarButtons] = {
        L"完成", L"矩形", L"椭圆", L"箭头", L"画笔", L"马赛克", L"撤销", L"重做", L"取消"};

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(245, 245, 245));

    for (int i = 0; i < kToolbarButtons; ++i) {
        RECT button{i * kButtonWidth, imageHeight_, (i + 1) * kButtonWidth, imageHeight_ + kToolbarHeight};

        const bool selected =
            (i == 1 && tool_ == Tool::Rectangle) ||
            (i == 2 && tool_ == Tool::Ellipse) ||
            (i == 3 && tool_ == Tool::Arrow) ||
            (i == 4 && tool_ == Tool::Pen) ||
            (i == 5 && tool_ == Tool::Mosaic);

        if (selected) {
            HBRUSH accent = CreateSolidBrush(RGB(0, 120, 215));
            FillRect(dc, &button, accent);
            DeleteObject(accent);
        }

        DrawTextW(dc, labels[i], -1, &button, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

void EditorWindow::Paint() {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd_, &ps);

    RECT client{};
    GetClientRect(hwnd_, &client);
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    HDC memory = CreateCompatibleDC(dc);
    const HGDIOBJ old = SelectObject(memory, bitmap_);
    BitBlt(dc, 0, 0, imageWidth_, imageHeight_, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old);
    DeleteDC(memory);

    DrawPreview(dc);
    PaintToolbar(dc);
    EndPaint(hwnd_, &ps);
}

void EditorWindow::Finish() {
    HBITMAP result = bitmap_;
    bitmap_ = nullptr;
    auto callback = callback_;
    DestroyWindow(hwnd_);
    if (callback) {
        callback(result);
    } else if (result) {
        DeleteObject(result);
    }
}

void EditorWindow::HandleToolbarClick(int x) {
    const int index = x / kButtonWidth;
    switch (index) {
    case 0: Finish(); break;
    case 1: tool_ = Tool::Rectangle; break;
    case 2: tool_ = Tool::Ellipse; break;
    case 3: tool_ = Tool::Arrow; break;
    case 4: tool_ = Tool::Pen; break;
    case 5: tool_ = Tool::Mosaic; break;
    case 6: Undo(); break;
    case 7: Redo(); break;
    case 8: DestroyWindow(hwnd_); break;
    default: break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT EditorWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        Paint();
        return 0;

    case WM_LBUTTONDOWN: {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (point.y >= imageHeight_) {
            HandleToolbarClick(point.x);
            return 0;
        }

        drawing_ = true;
        drawStart_ = point;
        drawCurrent_ = point;
        BeginEdit();
        SetCapture(hwnd_);

        if (tool_ == Tool::Mosaic) {
            ApplyMosaic(point);
        }
        return 0;
    }

    case WM_MOUSEMOVE:
        if (drawing_) {
            const POINT next{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (tool_ == Tool::Pen) {
                DrawPenSegment(drawCurrent_, next);
            } else if (tool_ == Tool::Mosaic) {
                ApplyMosaic(next);
            }
            drawCurrent_ = next;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONUP:
        if (drawing_) {
            drawCurrent_ = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ReleaseCapture();

            if (tool_ == Tool::Rectangle || tool_ == Tool::Ellipse || tool_ == Tool::Arrow) {
                HDC dc = CreateCompatibleDC(nullptr);
                const HGDIOBJ oldBitmap = SelectObject(dc, bitmap_);
                DrawShape(dc, tool_, drawStart_, drawCurrent_);
                SelectObject(dc, oldBitmap);
                DeleteDC(dc);
            }

            drawing_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDBLCLK:
        Finish();
        return 0;

    case WM_KEYDOWN:
        switch (wParam) {
        case VK_RETURN: Finish(); return 0;
        case VK_ESCAPE: DestroyWindow(hwnd_); return 0;
        case 'R': tool_ = Tool::Rectangle; break;
        case 'O': tool_ = Tool::Ellipse; break;
        case 'A': tool_ = Tool::Arrow; break;
        case 'P': tool_ = Tool::Pen; break;
        case 'M': tool_ = Tool::Mosaic; break;
        case 'Z':
            if (GetKeyState(VK_CONTROL) & 0x8000) Undo();
            break;
        case 'Y':
            if (GetKeyState(VK_CONTROL) & 0x8000) Redo();
            break;
        default:
            break;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;

    case WM_NCDESTROY:
        hwnd_ = nullptr;
        delete this;
        return 0;
    }

    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

LRESULT CALLBACK EditorWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    EditorWindow* self = reinterpret_cast<EditorWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<EditorWindow*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    return self ? self->HandleMessage(message, wParam, lParam)
                : DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace snaplite
