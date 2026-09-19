#include "snip_window.h"
#include "anime_toolbar.h"
#include "ocr.h"

#include <gdiplus.h>
#include <iostream>
#include <stdexcept>

// 测试链接真实工具栏；本机没有 C++/WinRT SDK，未触发的 OCR 入口显式报错。
namespace snaplite {
OcrResult ExtractTextFromBitmapRegion(HBITMAP, const RECT&) {
    throw std::runtime_error("OCR is outside this interaction test");
}
void ShowOcrResultPanel(HWND, const RECT&, const std::wstring&) {
    throw std::runtime_error("OCR is outside this interaction test");
}
}

namespace {
int checks = 0;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

void Mouse(HWND hwnd, UINT message, int x, int y) {
    SendMessageW(hwnd, message, message == WM_MOUSEMOVE ? MK_LBUTTON : 0, MAKELPARAM(x, y));
}

void Drag(HWND hwnd, int x, int y, int toX, int toY) {
    Mouse(hwnd, WM_LBUTTONDOWN, x, y);
    Mouse(hwnd, WM_MOUSEMOVE, toX, toY);
    Mouse(hwnd, WM_LBUTTONUP, toX, toY);
    UpdateWindow(hwnd);
}

bool SameRect(RECT a, RECT b) {
    return EqualRect(&a, &b) != FALSE;
}

bool Red(HBITMAP bitmap, int x, int y) {
    COLORREF color = snaplite::ReadBitmapPixel(bitmap, x, y);
    return GetRValue(color) > 180 && GetGValue(color) < 130 && GetBValue(color) < 130;
}

void History(HWND hwnd, int key) {
    BYTE previous[256]{};
    GetKeyboardState(previous);
    BYTE pressed[256]{};
    CopyMemory(pressed, previous, sizeof(pressed));
    pressed[VK_CONTROL] = 0x80;
    SetKeyboardState(pressed);
    SendMessageW(hwnd, WM_KEYDOWN, key, 0);
    SetKeyboardState(previous);
}

bool RedNear(HBITMAP bitmap, int x, int y) {
    // 填充多边形的尖角会抗锯齿，验证小邻域，避免依赖单个半透明像素。
    for (int dy = -3; dy <= 3; ++dy) {
        for (int dx = -3; dx <= 3; ++dx) {
            if (Red(bitmap, x + dx, y + dy)) return true;
        }
    }
    return false;
}
}

int wmain(int argc, wchar_t** argv) {
    const std::filesystem::path output = argc > 1 ? argv[1] : L"build/interaction";
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &input, nullptr);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND hwnd = nullptr;
    HBITMAP exported = nullptr;
    int result = 0;
    try {
        Check(snaplite::SnipWindow::Register(instance), "register window");
        Check(snaplite::SnipWindow::Start(instance, nullptr,
            [&](HBITMAP bitmap, snaplite::SnipWindow::FinishAction) { exported = bitmap; }), "start window");
        hwnd = FindWindowW(L"SnapLiteSnipWindow", nullptr);
        Check(hwnd != nullptr, "real native window exists");
        auto* snip = reinterpret_cast<snaplite::SnipWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        Check(snip != nullptr, "window owns capture engine");
        Check(snaplite::AnimeToolbar::Register(instance), "register real toolbar");
        snaplite::AnimeToolbar::ShowForSnip(instance);
        Check(FindWindowExW(hwnd, nullptr, L"SnapLiteEditorToolbarChild", nullptr) != nullptr, "real toolbar attached");
        Drag(hwnd, 100, 100, 900, 700);
        Check(SameRect(snip->UiSelectionRect(), {100, 100, 900, 700}), "initial selection");

        // 命中点刻意避开四角和边中点，同时验证热区内按下不会吸附跳动。
        Drag(hwnd, 260, 105, 260, 135);
        Check(SameRect(snip->UiSelectionRect(), {100, 130, 900, 700}), "top full edge with offset");
        Drag(hwnd, 895, 260, 865, 260);
        Check(SameRect(snip->UiSelectionRect(), {100, 130, 870, 700}), "right full edge with offset");
        Drag(hwnd, 260, 705, 260, 675);
        Check(SameRect(snip->UiSelectionRect(), {100, 130, 870, 670}), "bottom outside edge");
        snip->UiSetTool(1);
        Drag(hwnd, 105, 260, 135, 260);
        Check(SameRect(snip->UiSelectionRect(), {130, 130, 870, 670}), "edge takes priority over drawing tool");
        Drag(hwnd, 135, 260, 1200, 260);
        Check(SameRect(snip->UiSelectionRect(), {862, 130, 870, 670}), "minimum width keeps opposite edge fixed");
        Drag(hwnd, 862, 260, 130, 260);
        Check(SameRect(snip->UiSelectionRect(), {130, 130, 870, 670}), "small selection can expand again");
        Drag(hwnd, 130, 130, -100, -100);
        Check(SameRect(snip->UiSelectionRect(), {0, 0, 870, 670}), "corner clamps at screen origin");
        Drag(hwnd, 0, 0, 130, 130);

        snip->UiSetColor(RGB(235, 70, 70));
        snip->UiSetStrokeWidth(5);
        snip->UiSetArrowKind(0);
        HBITMAP baseline = snaplite::CloneBitmap(snip->UiCaptureBitmap());
        Drag(hwnd, 250, 250, 550, 250);
        Check(Red(snip->UiCaptureBitmap(), 350, 250), "arrow is visible in composited capture");
        Drag(hwnd, 400, 250, 440, 310);
        Check(Red(snip->UiCaptureBitmap(), 390, 310), "body drag moves arrow");
        Check(snaplite::ReadBitmapPixel(snip->UiCaptureBitmap(), 350, 250) ==
            snaplite::ReadBitmapPixel(baseline, 350, 250), "moving leaves no old arrow pixels");
        History(hwnd, 'Z');
        Check(Red(snip->UiCaptureBitmap(), 350, 250), "undo restores arrow position");
        const auto revisionBeforeCancel = snip->UiEditRevision();
        Mouse(hwnd, WM_LBUTTONDOWN, 400, 250);
        Mouse(hwnd, WM_MOUSEMOVE, 440, 280);
        SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        Check(snip->UiEditRevision() == revisionBeforeCancel, "cancel does not commit history");
        History(hwnd, 'Y');
        Check(Red(snip->UiCaptureBitmap(), 390, 310), "cancel preserves redo of moved arrow");
        Drag(hwnd, 590, 310, 590, 510);
        Check(Red(snip->UiCaptureBitmap(), 440, 410), "tip drag changes direction");
        Drag(hwnd, 290, 310, 290, 510);
        Check(Red(snip->UiCaptureBitmap(), 400, 510), "tail drag keeps tip fixed");

        Mouse(hwnd, WM_LBUTTONDOWN, 400, 510);
        Mouse(hwnd, WM_MOUSEMOVE, 430, 540);
        SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        Check(Red(snip->UiCaptureBitmap(), 400, 510), "escape cancels active arrow drag");
        Check(GetCapture() != hwnd, "escape releases mouse capture");
        Mouse(hwnd, WM_LBUTTONDOWN, 400, 510);
        Mouse(hwnd, WM_MOUSEMOVE, 420, 530);
        ReleaseCapture();
        Check(Red(snip->UiCaptureBitmap(), 420, 530), "capture loss keeps last position");
        History(hwnd, 'Z');
        Check(Red(snip->UiCaptureBitmap(), 400, 510), "capture-loss movement can be undone");

        // 多箭头、不同样式及压缩后的历史不能丢失对象元数据。
        for (int kind = 0; kind <= 6; ++kind) {
            snip->UiSetArrowKind(kind);
            Drag(hwnd, 250, 220, 550, 320);
            int hitX = kind == 6 ? 350 : 400;
            int hitY = kind == 4 ? 320 : 270;
            if (kind == 6) hitY = 250;
            Drag(hwnd, hitX, hitY, hitX + 25, hitY + 25);
            History(hwnd, 'Z');
            History(hwnd, 'Y');
            History(hwnd, 'Z');
            History(hwnd, 'Z');
            Check(!Red(snip->UiCaptureBitmap(), 250, 220), "styled arrow removed by undo after move");
        }
        snip->UiSetTool(-1);
        Drag(hwnd, 400, 510, 400, 540);
        Check(Red(snip->UiCaptureBitmap(), 400, 540), "selection tool can move old arrow");
        snip->UiSetTool(3);
        Drag(hwnd, 400, 540, 450, 540);
        Check(!Red(snip->UiCaptureBitmap(), 400, 540), "eraser removes editable arrow");
        History(hwnd, 'Z');
        Check(Red(snip->UiCaptureBitmap(), 400, 540), "undo eraser restores editable arrow");
        // 所有几何形状及三种填充模式共用可编辑对象，验证命中、移动和历史。
        for (int kind = 0; kind < 8; ++kind) {
            for (int fill = 0; fill < 3; ++fill) {
                snip->UiSetShapeKind(kind);
                snip->UiSetShapeFillMode(fill);
                Drag(hwnd, 250, 220, 550, 420);
                const int x = 400;
                const int y = kind == 4 ? 320 : 220;
                Check(snip->UiHitArrow({x, y}), "shape contour is draggable");
                if (kind != 4) Check(snip->UiHitArrow({400, 320}) == (fill != 0), "hollow and filled shape interior hit");
                snip->UiSetTool(-1);
                Drag(hwnd, x, y, x + 35, y + 25);
                Check(RedNear(snip->UiCaptureBitmap(), x + 35, y + 25), "shape moves with original style");
                History(hwnd, 'Z');
                Check(RedNear(snip->UiCaptureBitmap(), x, y), "undo shape movement");
                History(hwnd, 'Y');
                Check(RedNear(snip->UiCaptureBitmap(), x + 35, y + 25), "redo shape movement");
                History(hwnd, 'Z');
                History(hwnd, 'Z');
                Check(!snip->UiHitArrow({x, y}), "undo shape creation removes editable object");
            }
        }
        snip->UiSetShapeKind(2);
        snip->UiSetShapeFillMode(0);
        Drag(hwnd, 600, 200, 800, 400);
        Drag(hwnd, 700, 200, 720, 220);
        Check(Red(snip->UiCaptureBitmap(), 720, 220), "circle moves in shape tool");
        Drag(hwnd, 820, 420, 840, 440);
        Check(snip->UiHitArrow({730, 220}), "selected circle control point resizes shape");
        auto revision = snip->UiEditRevision();
        Drag(hwnd, 180, 160, 180, 160);
        Check(snip->UiEditRevision() == revision, "click does not add empty shape history");
        Check(!snip->UiHitArrow({180, 160}), "click does not create invisible draggable object");
        Mouse(hwnd, WM_LBUTTONDOWN, 180, 160);
        Mouse(hwnd, WM_MOUSEMOVE, 240, 190);
        SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        Check(snip->UiEditRevision() == revision && !snip->UiInteractionActive(), "escape cancels new shape without history");
        Mouse(hwnd, WM_LBUTTONDOWN, 730, 220);
        Mouse(hwnd, WM_LBUTTONUP, 730, 220);
        SendMessageW(hwnd, WM_KEYDOWN, VK_RIGHT, 0);
        Check(snip->UiEditRevision() == revision + 1, "keyboard nudge records one edit");
        Check(Red(snip->UiCaptureBitmap(), 731, 220), "composited pixels refresh after nudge");
        BYTE oldKeys[256]{};
        GetKeyboardState(oldKeys);
        BYTE shiftKeys[256]{};
        CopyMemory(shiftKeys, oldKeys, sizeof(shiftKeys));
        shiftKeys[VK_SHIFT] = 0x80;
        SetKeyboardState(shiftKeys);
        SendMessageW(hwnd, WM_KEYDOWN, VK_DOWN, 0);
        SetKeyboardState(oldKeys);
        Check(Red(snip->UiCaptureBitmap(), 731, 230), "shift moves selected shape ten pixels");
        History(hwnd, 'Z');
        History(hwnd, 'Z');
        Check(Red(snip->UiCaptureBitmap(), 730, 220), "unified undo restores keyboard movements");
        Mouse(hwnd, WM_LBUTTONDOWN, 730, 220);
        Mouse(hwnd, WM_LBUTTONUP, 730, 220);
        SendMessageW(hwnd, WM_KEYDOWN, VK_DELETE, 0);
        Check(!snip->UiHitArrow({730, 220}), "delete removes selected shape only");
        Check(Red(snip->UiCaptureBitmap(), 400, 540), "delete preserves other annotations");
        History(hwnd, 'Z');
        Check(Red(snip->UiCaptureBitmap(), 730, 220), "undo deletion restores shape and composite");
        History(hwnd, 'Y');
        Check(!snip->UiHitArrow({730, 220}), "redo deletion uses toolbar unified history");
        History(hwnd, 'Z');
        const DWORD gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        for (int repeat = 0; repeat < 40; ++repeat) snip->UiCaptureBitmap();
        Check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= gdiBefore + 1, "repeated composite refresh does not leak GDI objects");
        snip->UiSetTool(1);
        Mouse(hwnd, WM_LBUTTONDOWN, 400, 540);
        Mouse(hwnd, WM_LBUTTONUP, 400, 540);
        SendMessageW(hwnd, WM_LBUTTONDBLCLK, 0, MAKELPARAM(400, 540));
        Check(IsWindow(hwnd), "double click arrow must not finish screenshot");
        UpdateWindow(hwnd);
        HBITMAP visible = snaplite::CaptureVirtualScreen();
        Check(snaplite::SaveBitmapPng(visible, output / L"native-window.png"), "save native window evidence");
        DeleteObject(visible);
        RECT selection = snip->UiSelectionRect();
        snip->UiFinish(snaplite::SnipWindow::FinishAction::Copy);
        hwnd = nullptr;
        Check(exported != nullptr, "finish exports bitmap");
        Check(Red(exported, 400 - selection.left, 540 - selection.top), "export includes moved arrow");
        Check(Red(exported, 730 - selection.left, 220 - selection.top), "export includes moved and resized circle");
        Check(snaplite::SaveBitmapPng(exported, output / L"export.png"), "save export evidence");
        DeleteObject(baseline);
        std::cout << "PASS: " << checks << " native interaction checks\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        result = 1;
    }
    if (hwnd) DestroyWindow(hwnd);
    if (exported) DeleteObject(exported);
    Gdiplus::GdiplusShutdown(token);
    return result;
}
