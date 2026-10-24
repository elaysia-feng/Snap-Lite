#include "snip_window.h"
#include "anime_toolbar.h"
#include "ocr.h"

#include <gdiplus.h>
#include <iostream>
#include <stdexcept>
#include <vector>

// 测试链接真实工具栏；本机没有 C++/WinRT SDK，未触发的 OCR 入口显式报错。
namespace snaplite {
bool StartTextExtractionFromBitmapRegion(
    HBITMAP, const RECT&, HWND, unsigned long long, OcrCompletionCallback) {
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

DWORD MinGdiObjects() {
    // 一段时间内的最小 GDI 对象数。抖动来自后台线程的临时句柄；
    // 真泄漏是单调的，最小值压不下去。
    DWORD lowest = 0xFFFFFFFFu;
    for (int attempt = 0; attempt < 30; ++attempt) {
        const DWORD current = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        if (current < lowest) lowest = current;
        Sleep(10);
    }
    return lowest;
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
        // 拖动途中重建一次合成图。若缓存键没有覆盖"拖动取消还原"，这次重建
        // 会把"拖到一半"的图缓存下来，取消之后用户读到的仍是旧标注。
        HBITMAP midDrag = snaplite::CloneBitmap(snip->UiCaptureBitmap());
        Check(midDrag != nullptr, "mid-drag composite rebuild succeeds");
        DeleteObject(midDrag);
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
        Mouse(hwnd, WM_LBUTTONDOWN, 400, 540);
        Mouse(hwnd, WM_MOUSEMOVE, 425, 540);
        // 擦除途中重建一次合成图：橡皮移除的几何标注必须从重建结果里消失，
        // 不能因为缓存没失效而继续留在图上。
        HBITMAP midErase = snaplite::CloneBitmap(snip->UiCaptureBitmap());
        Check(midErase != nullptr, "mid-erase composite rebuild succeeds");
        DeleteObject(midErase);
        Mouse(hwnd, WM_MOUSEMOVE, 450, 540);
        Mouse(hwnd, WM_LBUTTONUP, 450, 540);
        Check(!Red(snip->UiCaptureBitmap(), 400, 540), "eraser removes editable arrow");
        History(hwnd, 'Z');
        Check(Red(snip->UiCaptureBitmap(), 400, 540), "undo eraser restores editable arrow");

        // 画笔是逐段写进 capture_ 的。若缓存键没有覆盖"笔画中途的写入"，
        // 笔画中途重建出来的半截图会被缓存住，笔画结束后用户看到的仍是半截。
        snip->UiSetArrowKind(0);
        snip->UiSetTool(2);
        snip->UiSetColor(RGB(235, 70, 70));
        snip->UiSetStrokeWidth(5);
        Mouse(hwnd, WM_LBUTTONDOWN, 200, 580);
        Mouse(hwnd, WM_MOUSEMOVE, 300, 580);
        HBITMAP midStroke = snaplite::CloneBitmap(snip->UiCaptureBitmap());
        Check(midStroke != nullptr, "mid-stroke composite rebuild succeeds");
        DeleteObject(midStroke);
        Mouse(hwnd, WM_MOUSEMOVE, 400, 580);
        Mouse(hwnd, WM_LBUTTONUP, 400, 580);
        Check(Red(snip->UiCaptureBitmap(), 400, 580), "pen stroke keeps its tail after mid-stroke rebuild");
        // 内容再次变化后重建：合成图必须同时包含之前的笔画和后来新增的箭头，
        // 缓存不能因为"上一次重建过"就把后来的标注挡在旧图后面。
        snip->UiSetTool(1);
        snip->UiSetColor(RGB(235, 70, 70));
        Drag(hwnd, 200, 640, 400, 640);
        Check(Red(snip->UiCaptureBitmap(), 300, 640), "arrow added after the pen stroke appears in composite");
        Check(Red(snip->UiCaptureBitmap(), 400, 580), "pen stroke is still present after the arrow was added");
        History(hwnd, 'Z');
        History(hwnd, 'Z');
        Check(!Red(snip->UiCaptureBitmap(), 400, 580), "undo removes the pen stroke from the composite");
        snip->UiSetTool(-1);

        // 橡皮把原始像素写回 capture_，这条路径也必须让合成图缓存失效。
        //
        // 断言要落在"中途重建之后才被擦掉"的那一段上：如果只断言中途重建
        // 之前就已被擦掉的位置，旧图也同样满足，断言就没有鉴别力。
        snip->UiSetTool(2);
        snip->UiSetColor(RGB(235, 70, 70));
        snip->UiSetStrokeWidth(5);
        Mouse(hwnd, WM_LBUTTONDOWN, 200, 620);
        Mouse(hwnd, WM_MOUSEMOVE, 400, 620);
        Mouse(hwnd, WM_LBUTTONUP, 400, 620);
        Check(Red(snip->UiCaptureBitmap(), 300, 620), "pen stroke for the eraser check");
        snip->UiSetTool(3);
        Mouse(hwnd, WM_LBUTTONDOWN, 200, 620);
        Mouse(hwnd, WM_MOUSEMOVE, 300, 620);
        // 中途重建：此时只有 200..300 被擦回原始像素。
        HBITMAP midErase2 = snaplite::CloneBitmap(snip->UiCaptureBitmap());
        Check(midErase2 != nullptr, "mid-erase rebuild of a pen stroke succeeds");
        DeleteObject(midErase2);
        Mouse(hwnd, WM_MOUSEMOVE, 400, 620);
        Mouse(hwnd, WM_LBUTTONUP, 400, 620);
        // 300..400 是在中途重建之后才被擦掉的：缓存没失效的话这里仍然是红的。
        Check(!Red(snip->UiCaptureBitmap(), 350, 620), "erase after mid-erase rebuild reaches the composite");
        History(hwnd, 'Z');
        History(hwnd, 'Z');
        Check(!Red(snip->UiCaptureBitmap(), 350, 620), "undo removes the erased pen stroke");
        snip->UiSetTool(-1);
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
        // 进程级 GDI 计数会被后台 PNG 压缩线程**同时**占用而抖动：
        // 第 14 轮验收实测 16 次运行里误红 1 次（红在一个只改了文案字符串的
        // 等价变异上，与被测代码无关）。
        //
        // 这里把测量做成"取整段采样里的最小值"：后台线程的临时句柄迟早会
        // 释放，最小值更接近"这个对象自己占了多少"；真泄漏是单调的
        // （40 次刷新涨 40），最小值也压不下去。
        //
        // 刷新后再测两轮、取较小的那个：后台线程的占用要是正好横跨第一轮
        // 的采样窗口，第二轮能把它滤掉。
        const DWORD gdiBefore = MinGdiObjects();
        for (int repeat = 0; repeat < 40; ++repeat) snip->UiCaptureBitmap();
        const DWORD gdiAfter = MinGdiObjects();
        for (int repeat = 0; repeat < 40; ++repeat) snip->UiCaptureBitmap();
        const DWORD gdiAfter2 = MinGdiObjects();
        Check(
            (gdiAfter < gdiAfter2 ? gdiAfter : gdiAfter2) <= gdiBefore + 1,
            "repeated composite refresh does not leak GDI objects");
        HWND toolbar = FindWindowExW(hwnd, nullptr, L"SnapLiteEditorToolbarChild", nullptr);
        snip->UiSetTool(0);
        Mouse(hwnd, WM_LBUTTONDOWN, 730, 220);
        Mouse(hwnd, WM_LBUTTONUP, 730, 220);
        History(hwnd, 'D');
        Check(snip->UiHitArrow({746, 236}) && snip->UiHitArrow({730, 220}), "duplicate preserves original and offsets copy");
        // 使用真实工具栏按钮验证样式编辑及统一历史；坐标对应生成后的次级工具行。
        UpdateWindow(hwnd);
        Mouse(toolbar, WM_LBUTTONUP, 728, 73);
        const COLORREF blue = RGB(70, 120, 210);
        Check(snip->UiColor() == blue, "toolbar changes selected object color");
        auto color = snaplite::ReadBitmapPixel(snip->UiCaptureBitmap(), 746, 236);
        Check(GetBValue(color) > 180 && GetRValue(color) < 130, "selected copy is painted blue");
        Check(Red(snip->UiCaptureBitmap(), 730, 220), "style edit preserves original object");
        Mouse(toolbar, WM_LBUTTONUP, 435, 73);
        color = snaplite::ReadBitmapPixel(snip->UiCaptureBitmap(), 746, 346);
        Check(GetBValue(color) > 180 && GetRValue(color) < 130, "toolbar fills existing shape");
        History(hwnd, 'Z');
        Mouse(hwnd, WM_LBUTTONDOWN, 746, 236);
        Mouse(hwnd, WM_LBUTTONUP, 746, 236);
        Check(snip->UiShapeFillMode() == 0, "undo restores fill style and selection synchronizes toolbar");
        auto overlap = [&]() { return snaplite::ReadBitmapPixel(snip->UiCaptureBitmap(), 815, 261); };
        History(hwnd, VK_END);
        Check(Red(snip->UiCaptureBitmap(), 815, 261), "send to back reveals original overlap");
        History(hwnd, VK_HOME);
        Check(GetBValue(overlap()) > GetRValue(overlap()), "bring to front restores copy overlap");
        History(hwnd, VK_NEXT);
        Check(Red(snip->UiCaptureBitmap(), 815, 261), "lower layer by one");
        History(hwnd, VK_PRIOR);
        Check(GetBValue(overlap()) > GetRValue(overlap()), "raise layer by one");
        const auto topRevision = snip->UiEditRevision();
        History(hwnd, VK_HOME);
        Check(snip->UiEditRevision() == topRevision, "already frontmost does not add history");
        for (int step = 0; step < 4; ++step) History(hwnd, 'Z');
        History(hwnd, 'Z');
        Check(Red(snip->UiCaptureBitmap(), 746, 236), "undo color edit restores red copy");
        History(hwnd, 'Z');
        Check(!snip->UiHitArrow({746, 236}), "undo duplication removes only copy");
        Mouse(hwnd, WM_LBUTTONDOWN, 730, 220);
        Mouse(hwnd, WM_LBUTTONUP, 730, 220);
        SetKeyboardState(shiftKeys);
        Drag(hwnd, 730, 220, 760, 228);
        SetKeyboardState(oldKeys);
        Check(Red(snip->UiCaptureBitmap(), 760, 220), "shift drag locks horizontal axis");
        History(hwnd, 'Z');
        SendMessageW(hwnd, WM_KEYDOWN, VK_TAB, 0);
        SendMessageW(hwnd, WM_KEYDOWN, VK_DELETE, 0);
        Check(!snip->UiHitArrow({400, 540}) && snip->UiHitArrow({730, 220}), "tab selects first object without touching others");
        History(hwnd, 'Z');
        SetKeyboardState(shiftKeys);
        SendMessageW(hwnd, WM_KEYDOWN, VK_TAB, 0);
        SetKeyboardState(oldKeys);
        SendMessageW(hwnd, WM_KEYDOWN, VK_DELETE, 0);
        Check(!snip->UiHitArrow({730, 220}) && snip->UiHitArrow({400, 540}), "shift tab selects last object");
        History(hwnd, 'Z');
        Mouse(hwnd, WM_LBUTTONDOWN, 730, 220);
        Mouse(hwnd, WM_LBUTTONUP, 730, 220);
        revision = snip->UiEditRevision();
        SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        SendMessageW(hwnd, WM_KEYDOWN, VK_DELETE, 0);
        Check(IsWindow(hwnd) && snip->UiEditRevision() == revision, "escape deselects without deleting or closing");

        snip->UiSetTool(5);
        snip->UiSetColor(RGB(235, 70, 70));
        UpdateWindow(hwnd);
        Mouse(hwnd, WM_LBUTTONDOWN, 180, 400);
        HWND textWindow = FindWindowExW(hwnd, nullptr, L"SnapLiteTextAnnotationChild", nullptr);
        Check(textWindow != nullptr, "create real text annotation");
        HWND edit = FindWindowExW(textWindow, nullptr, L"EDIT", nullptr);
        Check(edit != nullptr, "text editor exists");
        SetWindowTextW(edit, L"Text history");
        SetFocus(hwnd);
        Check(!IsWindow(edit), "text committed on focus loss");
        RECT oldTextBounds{};
        GetWindowRect(textWindow, &oldTextBounds);
        Mouse(toolbar, WM_LBUTTONUP, 382, 73);
        auto textColor = [&]() {
            HWND text = FindWindowExW(hwnd, nullptr, L"SnapLiteTextAnnotationChild", nullptr);
            HDC dc = GetDC(text);
            SendMessageW(text, WM_CTLCOLOREDIT, reinterpret_cast<WPARAM>(dc), 0);
            COLORREF ink = GetTextColor(dc);
            ReleaseDC(text, dc);
            return ink;
        };
        Check(textColor() == blue, "change committed text color");
        History(hwnd, 'Z');
        Check(textColor() == RGB(235, 70, 70), "undo text color restores actual previous value");
        textWindow = FindWindowExW(hwnd, nullptr, L"SnapLiteTextAnnotationChild", nullptr);
        Mouse(textWindow, WM_LBUTTONDOWN, 10, 10);
        Mouse(textWindow, WM_LBUTTONUP, 10, 10);
        Mouse(toolbar, WM_LBUTTONUP, 261, 73);
        RECT largeTextBounds{};
        GetWindowRect(textWindow, &largeTextBounds);
        Check(largeTextBounds.bottom - largeTextBounds.top > oldTextBounds.bottom - oldTextBounds.top, "change committed text size");
        History(hwnd, 'Z');
        textWindow = FindWindowExW(hwnd, nullptr, L"SnapLiteTextAnnotationChild", nullptr);
        RECT restoredTextBounds{};
        GetWindowRect(textWindow, &restoredTextBounds);
        Check(SameRect(oldTextBounds, restoredTextBounds), "undo text size restores layout");
        History(hwnd, 'Z');
        Check(!FindWindowExW(hwnd, nullptr, L"SnapLiteTextAnnotationChild", nullptr), "mixed history returns to before text creation");
        snip->UiSetTool(1);
        Mouse(hwnd, WM_LBUTTONDOWN, 400, 540);
        Mouse(hwnd, WM_LBUTTONUP, 400, 540);
        SendMessageW(hwnd, WM_LBUTTONDBLCLK, 0, MAKELPARAM(400, 540));
        Check(IsWindow(hwnd), "double click arrow must not finish screenshot");
        UpdateWindow(hwnd);
        HBITMAP visible = snaplite::CaptureVirtualScreen();
        Check(snaplite::SaveBitmapPng(visible, output / L"native-window.png"), "save native window evidence");
        DeleteObject(visible);
        // 产品级的"裁剪 → 编辑 → 撤销"路径（第 14 批补的覆盖缺口 F1）。
        //
        // 之前那条裁剪配对测试用的是 BitmapHistory<int>，而产品用的是
        // BitmapHistory<std::vector<ArrowAnnotation>>。测试钩子是
        // **按模板实例化**存的静态量，两个实例化各有一份——测试压的
        // 上限根本落不到产品那一份上。实测：把平行标注栈原样装回产品
        // （变异 M3），259 项运行时断言全绿，只有源码文本守卫会红。
        //
        // 现在由产品自己提供一个入口，压的就是产品真正用的那一份：
        // 上限压到 2 → 连做 3 次带标注的编辑 → 撤销 → 恢复出来的标注
        // 必须逐条对得上（第一个、第二个在，第三个不在）。
        {
            const RECT area = snip->UiSelectionRect();
            const int arrowY = area.top + 24;
            const POINT first{area.left + 24, arrowY};
            const POINT second{area.left + 124, arrowY};
            const POINT third{area.left + 224, arrowY};
            // 这三个点必须本来是空的：否则"箭头在不在"的断言会被
            // 别的标注干扰（命中测试是从后往前找，先撞上哪个算哪个）。
            Check(
                !snip->UiHitArrow(first) && !snip->UiHitArrow(second) &&
                    !snip->UiHitArrow(third),
                "the product level history test uses empty spots");
            snaplite::SnipWindow::SetHistoryEntryLimitForTest(2);
            snip->UiSetTool(1);
            snip->UiSetColor(RGB(235, 70, 70));
            snip->UiSetStrokeWidth(5);
            snip->UiSetArrowKind(0);
            Drag(hwnd, first.x, first.y, first.x + 40, first.y);
            Check(snip->UiHitArrow(first), "first product arrow is drawn");
            Drag(hwnd, second.x, second.y, second.x + 40, second.y);
            Check(snip->UiHitArrow(second), "second product arrow is drawn");
            Drag(hwnd, third.x, third.y, third.x + 40, third.y);
            Check(snip->UiHitArrow(third), "third product arrow is drawn");
            // 撤销一次：恢复出来的必须是"第三个箭头画出来之前"的那一套标注。
            History(hwnd, 'Z');
            Check(
                snip->UiHitArrow(first) && snip->UiHitArrow(second),
                "after trimming, undo restores the annotations of that very step");
            Check(
                !snip->UiHitArrow(third),
                "after trimming, undo drops the annotation created after the kept step");
            // 探针：确认"裁剪**真的发生了**"。
            //
            // 前面那几条只能证明"撤销恢复出来的标注是对的那一套"，证明不了
            // "裁剪真的发生过"——上限钩子要是没落到产品那一份栈上（或者
            // 上限写错了），栈里一条都没丢，撤销当然不会错位，断言照样全绿。
            //
            // 上限是 2 时最老那条早就被裁掉了：撤三次也回不去，第一个箭头
            // 必须还在。钩子没生效时栈里有 3 条，撤第三次正好回到"第一个
            // 箭头还没画"的那一刻，它就不在了。
            History(hwnd, 'Z');
            History(hwnd, 'Z');
            Check(
                snip->UiHitArrow(first),
                "the trimmed oldest step is really gone: the first arrow survives three undos");
            snaplite::SnipWindow::SetHistoryEntryLimitForTest(0);
        }
        RECT selection = snip->UiSelectionRect();
        snip->UiFinish(snaplite::SnipWindow::FinishAction::Copy);
        hwnd = nullptr;
        Check(exported != nullptr, "finish exports bitmap");
        Check(Red(exported, 400 - selection.left, 540 - selection.top), "export includes moved arrow");
        Check(Red(exported, 730 - selection.left, 220 - selection.top), "export includes moved and resized circle");
        Check(snaplite::SaveBitmapPng(exported, output / L"export.png"), "save export evidence");
        DeleteObject(baseline);
        // 撤销历史的后台压缩：压完的条目取回时走的是"PNG 解码"那条路，
        // 不是"直接拿原始位图"。压缩改成异步之后，不主动等它的话这条路径
        // 在测试里根本不会被走到——后台压缩一坏，撤销就静默失效。
        {
            snaplite::BitmapHistory<int> history;
            std::vector<HBITMAP> references;
            for (int index = 0; index < 6; ++index) {
                HDC screen = GetDC(nullptr);
                HBITMAP bitmap = CreateCompatibleBitmap(screen, 8, 8);
                ReleaseDC(nullptr, screen);
                HDC dc = CreateCompatibleDC(nullptr);
                const HGDIOBJ old = SelectObject(dc, bitmap);
                const HBRUSH brush = CreateSolidBrush(RGB(10 + index * 20, 30, 40));
                RECT rect{0, 0, 8, 8};
                FillRect(dc, &rect, brush);
                DeleteObject(brush);
                SelectObject(dc, old);
                DeleteDC(dc);
                // 自己留一份原始像素做对照：判据是"压过再取回，内容不变"，
                // 而不是某个写死的颜色值。
                references.push_back(snaplite::CloneBitmap(bitmap));
                history.push_back(bitmap, index);
            }
            for (int spin = 0; spin < 300 && history.IsCompressing(); ++spin) {
                Sleep(20);
            }
            Check(!history.IsCompressing(), "background history compression finished");
            for (int index = 5; index >= 0; --index) {
                int companion = -1;
                HBITMAP restored = history.take_back(&companion);
                Check(restored != nullptr, "compressed history entry is restored");
                // 取出来的这一帧必须带着**它自己**的那份状态。
                //
                // 这一条不是装饰：状态存在历史条目里就是为了这个。位图与
                // 状态如果住在两个平行的栈里，裁剪（丢队首）之后配对就会
                // 错开一位，撤销恢复出来的是另一套标注。
                Check(
                    companion == index,
                    "a restored frame keeps its own annotation state");
                Check(
                    snaplite::ReadBitmapPixel(restored, 4, 4) ==
                        snaplite::ReadBitmapPixel(references[static_cast<size_t>(index)], 4, 4),
                    "a compressed frame comes back with the same pixels");
                DeleteObject(restored);
            }
            for (HBITMAP reference : references) {
                DeleteObject(reference);
            }
        }
        // 历史有长度上限，超了会从**队首**丢老条目。丢的时候必须连着
        // "与这一帧配对的状态"一起丢。
        //
        // 只丢位图、留着状态，两边从此对不齐：位图栈少了队首的，状态栈
        // 还留着，于是下一次编辑时"把多出来的状态从队尾扔掉"会把**新的**
        // 那一套扔掉——之后每次撤销恢复出来的都是旧的那一套（实测表现：
        // 撤销之后箭头还在，而且越撤越错）。
        //
        // 裁剪平时几乎测不到（要攒够 64MB 或 128 条），所以这里把上限
        // 压到 2 条，几秒钟就把这条路走一遍。
        {
            snaplite::BitmapHistory<int> history;
            snaplite::BitmapHistory<int>::SetMaxEntriesOverrideForTest(2);
            std::vector<HBITMAP> references;
            for (int index = 0; index < 4; ++index) {
                HDC screen = GetDC(nullptr);
                HBITMAP bitmap = CreateCompatibleBitmap(screen, 8, 8);
                ReleaseDC(nullptr, screen);
                HDC dc = CreateCompatibleDC(nullptr);
                const HGDIOBJ old = SelectObject(dc, bitmap);
                const HBRUSH brush = CreateSolidBrush(RGB(200 - index * 20, 60, 90));
                RECT rect{0, 0, 8, 8};
                FillRect(dc, &rect, brush);
                DeleteObject(brush);
                SelectObject(dc, old);
                DeleteDC(dc);
                references.push_back(snaplite::CloneBitmap(bitmap));
                history.push_back(bitmap, index);
            }
            for (int spin = 0; spin < 300 && history.IsCompressing(); ++spin) {
                Sleep(20);
            }
            Check(
                history.size() == 2,
                "the history is trimmed down to the configured limit");
            // 剩下的必须是**最后压进去的那两条**，各自的编号必须对得上。
            for (int index = 3; index >= 2; --index) {
                int companion = -1;
                HBITMAP restored = history.take_back(&companion);
                Check(restored != nullptr, "a trimmed history entry is restored");
                Check(
                    companion == index,
                    "a trimmed frame keeps its own annotation state");
                if (restored) {
                    Check(
                        snaplite::ReadBitmapPixel(restored, 4, 4) ==
                            snaplite::ReadBitmapPixel(
                                references[static_cast<size_t>(index)], 4, 4),
                        "a trimmed frame comes back with the same pixels");
                    DeleteObject(restored);
                }
            }
            for (HBITMAP reference : references) {
                DeleteObject(reference);
            }
            snaplite::BitmapHistory<int>::SetMaxEntriesOverrideForTest(0);
        }
        // 用 endl 立即刷新：stdout 是带缓冲的，不刷新的话 CI 日志里可能看不到
        // 这一行，测试就变成"跑了但不知道跑了多少断言"。
        std::cout << "PASS: " << checks << " native interaction checks" << std::endl;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        result = 1;
    }
    if (hwnd) DestroyWindow(hwnd);
    if (exported) DeleteObject(exported);
    Gdiplus::GdiplusShutdown(token);
    return result;
}
