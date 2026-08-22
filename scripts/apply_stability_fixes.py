from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def write(path: str, text: str) -> None:
    (ROOT / path).write_text(text, encoding="utf-8", newline="\n")


def replace_once(path: str, old: str, new: str) -> None:
    text = read(path)
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected exactly one literal match, got {count}: {old[:80]!r}")
    write(path, text.replace(old, new, 1))


def replace_all_checked(path: str, mapping: dict[str, str]) -> None:
    text = read(path)
    for old, new in mapping.items():
        if old not in text:
            raise RuntimeError(f"{path}: missing expected token {old!r}")
        text = text.replace(old, new)
    write(path, text)


def regex_once(path: str, pattern: str, replacement: str) -> None:
    text = read(path)
    new_text, count = re.subn(pattern, replacement, text, count=1, flags=re.S)
    if count != 1:
        raise RuntimeError(f"{path}: expected exactly one regex match, got {count}: {pattern[:100]!r}")
    write(path, new_text)


# ---------------------------------------------------------------------------
# 1. Build graph: remove dead editor implementation and compile-time UI hooks.
# ---------------------------------------------------------------------------
replace_once("CMakeLists.txt", "    src/editor_window.cpp\n", "")
regex_once(
    "CMakeLists.txt",
    r"\n    set_property\(\n        SOURCE src/editor_toolbar\.cpp\n        APPEND PROPERTY COMPILE_OPTIONS\n        \"/FI\$\{CMAKE_CURRENT_SOURCE_DIR\}/src/toolbar_visual_theme\.h\"\n        \"/FI\$\{CMAKE_CURRENT_SOURCE_DIR\}/src/toolbar_icon_render_gdi\.h\"\n        \"/FI\$\{CMAKE_CURRENT_SOURCE_DIR\}/src/toolbar_compact_behavior\.h\"\n    \)\n",
    "\n",
)

# ---------------------------------------------------------------------------
# 2. App lifecycle: one snip session, mutex cleanup, modern folder picker,
#    long Save-As buffer.
# ---------------------------------------------------------------------------
replace_once("src/app.cpp", '#include <shlobj.h>\n', '#include <shlobj.h>\n#include <shobjidl.h>\n')
replace_once("src/app.cpp", '#include <string>\n', '#include <string>\n#include <vector>\n')
regex_once(
    "src/app.cpp",
    r"int CALLBACK BrowseFolderCallback\(.*?\n\}\n\nstd::filesystem::path ChooseScreenshotFolder\(HWND owner\) \{.*?\n\}\n\n(?=std::filesystem::path ChooseSaveAsPath)",
    r'''std::filesystem::path ChooseScreenshotFolder(HWND owner) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool shouldUninitialize = SUCCEEDED(init);

    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_FileOpenDialog,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) {
        if (shouldUninitialize) CoUninitialize();
        return {};
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    dialog->SetTitle(L"选择 Snap-Lite 默认截图保存目录");

    const std::filesystem::path initial = ConfiguredScreenshotDirectory();
    IShellItem* initialItem = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&initialItem)))) {
        dialog->SetFolder(initialItem);
        initialItem->Release();
    }

    std::filesystem::path selected;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                selected = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }

    dialog->Release();
    if (shouldUninitialize) CoUninitialize();
    return selected;
}

''',
)
replace_once(
    "src/app.cpp",
    '''    wchar_t file[MAX_PATH]{};
    wcsncpy_s(file, suggested.filename().c_str(), _TRUNCATE);
''',
    '''    std::vector<wchar_t> file(32768, L'\\0');
    wcsncpy_s(file.data(), file.size(), suggested.filename().c_str(), _TRUNCATE);
''',
)
replace_once("src/app.cpp", "    dialog.lpstrFile = file;\n    dialog.nMaxFile = MAX_PATH;\n", "    dialog.lpstrFile = file.data();\n    dialog.nMaxFile = static_cast<DWORD>(file.size());\n")
replace_once("src/app.cpp", "    return std::filesystem::path(file);\n", "    return std::filesystem::path(file.data());\n")
replace_once(
    "src/app.cpp",
    '''    singleInstance_ = CreateMutexW(nullptr, TRUE, L"Local\\\\SnapLiteSingleInstance");
    if (!singleInstance_ || GetLastError() == ERROR_ALREADY_EXISTS) {
        return false;
    }
''',
    '''    singleInstance_ = CreateMutexW(nullptr, TRUE, L"Local\\\\SnapLiteSingleInstance");
    if (!singleInstance_) {
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(singleInstance_);
        singleInstance_ = nullptr;
        return false;
    }
''',
)
replace_once(
    "src/app.cpp",
    "void App::StartSnip() {\n    const bool started = SnipWindow::Start(\n",
    '''void App::StartSnip() {
    // All annotation state is session-scoped by behavior. Keep one snip window
    // alive at a time so rapid F1 presses cannot create competing full-screen
    // overlays or attach the editor toolbar to the wrong window.
    if (HWND existing = FindWindowW(L"SnapLiteSnipWindow", nullptr)) {
        ShowWindow(existing, SW_SHOW);
        SetForegroundWindow(existing);
        SetFocus(existing);
        return;
    }

    const bool started = SnipWindow::Start(
''',
)

# ---------------------------------------------------------------------------
# 3. Clipboard safety and filename collision avoidance.
# ---------------------------------------------------------------------------
replace_once(
    "src/capture.cpp",
    '''bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) {
        return false;
    }

    EmptyClipboard();
''',
    '''bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) {
        return false;
    }

    if (!EmptyClipboard()) {
        CloseClipboard();
        return false;
    }
''',
)
replace_once(
    "src/capture.cpp",
    "    return ScreenshotDirectory() / filename;\n",
    '''    const std::filesystem::path directory = ScreenshotDirectory();
    std::filesystem::path candidate = directory / filename;
    std::error_code error;
    for (int suffix = 1; std::filesystem::exists(candidate, error) && !error; ++suffix) {
        wchar_t unique[112]{};
        swprintf_s(
            unique,
            L"SnapLite_%04u-%02u-%02u_%02u-%02u-%02u-%03u-%d.png",
            now.wYear, now.wMonth, now.wDay,
            now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, suffix);
        candidate = directory / unique;
    }
    return candidate;
''',
)
replace_once(
    "src/capture_settings.cpp",
    "    return ConfiguredScreenshotDirectory() / filename;\n",
    '''    const std::filesystem::path directory = ConfiguredScreenshotDirectory();
    std::filesystem::path candidate = directory / filename;
    std::error_code error;
    for (int suffix = 1; std::filesystem::exists(candidate, error) && !error; ++suffix) {
        wchar_t unique[112]{};
        swprintf_s(
            unique,
            L"SnapLite_%04u-%02u-%02u_%02u-%02u-%02u-%03u-%d.png",
            now.wYear, now.wMonth, now.wDay,
            now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, suffix);
        candidate = directory / unique;
    }
    return candidate;
''',
)

# ---------------------------------------------------------------------------
# 4. Pin window: validate foreign DIB buffers and use the actual monitor work
#    area for initial placement and zoom while preserving aspect ratio.
# ---------------------------------------------------------------------------
replace_once("src/pin_window.cpp", '#include <filesystem>\n', '#include <filesystem>\n#include <limits>\n')
replace_once(
    "src/pin_window.cpp",
    '''    const auto* header = reinterpret_cast<const BITMAPINFOHEADER*>(base);
    if (header->biSize < sizeof(BITMAPINFOHEADER) || header->biWidth == 0 || header->biHeight == 0) {
        GlobalUnlock(memory);
        return nullptr;
    }

    SIZE_T pixelOffset = header->biSize;
''',
    '''    const auto* header = reinterpret_cast<const BITMAPINFOHEADER*>(base);
    if (header->biSize < sizeof(BITMAPINFOHEADER) ||
        static_cast<SIZE_T>(header->biSize) > totalBytes ||
        header->biPlanes != 1 ||
        header->biWidth <= 0 ||
        header->biHeight == 0 ||
        header->biHeight == std::numeric_limits<LONG>::min()) {
        GlobalUnlock(memory);
        return nullptr;
    }

    const WORD bitCount = header->biBitCount;
    const bool supportedBitCount =
        bitCount == 1 || bitCount == 4 || bitCount == 8 ||
        bitCount == 16 || bitCount == 24 || bitCount == 32;
    const bool supportedCompression =
        header->biCompression == BI_RGB || header->biCompression == BI_BITFIELDS
#ifdef BI_ALPHABITFIELDS
        || header->biCompression == BI_ALPHABITFIELDS
#endif
        ;
    if (!supportedBitCount || !supportedCompression) {
        GlobalUnlock(memory);
        return nullptr;
    }

    const std::uint64_t width64 = static_cast<std::uint64_t>(header->biWidth);
    const std::uint64_t height64 = static_cast<std::uint64_t>(
        header->biHeight < 0 ? -static_cast<std::int64_t>(header->biHeight)
                             : static_cast<std::int64_t>(header->biHeight));
    constexpr std::uint64_t kMaxClipboardDimension = 32768;
    if (width64 == 0 || height64 == 0 ||
        width64 > kMaxClipboardDimension || height64 > kMaxClipboardDimension) {
        GlobalUnlock(memory);
        return nullptr;
    }

    SIZE_T pixelOffset = header->biSize;
''',
)
replace_once("src/pin_window.cpp", '#include <cstring>\n', '#include <cstring>\n#include <cstdint>\n')
replace_once(
    "src/pin_window.cpp",
    '''    if (pixelOffset >= totalBytes) {
        GlobalUnlock(memory);
        return nullptr;
    }

    const int width = std::abs(header->biWidth);
    const int height = std::abs(header->biHeight);
    if (width <= 0 || height <= 0) {
        GlobalUnlock(memory);
        return nullptr;
    }
''',
    '''    if (pixelOffset >= totalBytes) {
        GlobalUnlock(memory);
        return nullptr;
    }

    // For uncompressed/bitfield DIBs, every scanline is DWORD aligned. Validate
    // that the advertised dimensions actually fit inside the clipboard block
    // before StretchDIBits sees the foreign buffer.
    const std::uint64_t bitsPerRow = width64 * bitCount;
    const std::uint64_t stride64 = ((bitsPerRow + 31u) / 32u) * 4u;
    if (stride64 == 0 || stride64 > totalBytes - pixelOffset ||
        height64 > (totalBytes - pixelOffset) / stride64) {
        GlobalUnlock(memory);
        return nullptr;
    }

    const int width = static_cast<int>(width64);
    const int height = static_cast<int>(height64);
''',
)
regex_once(
    "src/pin_window.cpp",
    r"    const int maxWidth = static_cast<int>\(GetSystemMetrics\(SM_CXSCREEN\) \* 0\.65\);.*?\n    const int height = std::max\(1, static_cast<int>\(self->bitmapHeight_ \* self->zoom_\)\);\n\n    POINT cursor\{\};\n    GetCursorPos\(&cursor\);\n\n    HWND hwnd = CreateWindowExW\(\n        WS_EX_TOPMOST \| WS_EX_TOOLWINDOW \| WS_EX_LAYERED,\n        kPinClass,\n        L\"Snap-Lite Pin\",\n        WS_POPUP,\n        cursor\.x \+ 16,\n        cursor\.y \+ 16,",
    '''    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(monitor, &monitorInfo);
    const int workWidth = std::max(1L, monitorInfo.rcWork.right - monitorInfo.rcWork.left);
    const int workHeight = std::max(1L, monitorInfo.rcWork.bottom - monitorInfo.rcWork.top);
    const int maxWidth = static_cast<int>(workWidth * 0.65);
    const int maxHeight = static_cast<int>(workHeight * 0.65);
    self->zoom_ = std::min({1.0,
        static_cast<double>(maxWidth) / self->bitmapWidth_,
        static_cast<double>(maxHeight) / self->bitmapHeight_});

    const int width = std::max(1, static_cast<int>(self->bitmapWidth_ * self->zoom_));
    const int height = std::max(1, static_cast<int>(self->bitmapHeight_ * self->zoom_));
    int x = cursor.x + 16;
    int y = cursor.y + 16;
    x = std::clamp(x, static_cast<int>(monitorInfo.rcWork.left),
                   std::max(static_cast<int>(monitorInfo.rcWork.left), static_cast<int>(monitorInfo.rcWork.right) - width));
    y = std::clamp(y, static_cast<int>(monitorInfo.rcWork.top),
                   std::max(static_cast<int>(monitorInfo.rcWork.top), static_cast<int>(monitorInfo.rcWork.bottom) - height));

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        kPinClass,
        L"Snap-Lite Pin",
        WS_POPUP,
        x,
        y,''',
)
regex_once(
    "src/pin_window.cpp",
    r"void PinWindow::ResizeForZoom\(\) \{.*?\n\}\n\nvoid PinWindow::AdjustZoom",
    '''void PinWindow::ResizeForZoom() {
    RECT current{};
    GetWindowRect(hwnd_, &current);
    const POINT center{
        static_cast<LONG>((current.left + current.right) / 2),
        static_cast<LONG>((current.top + current.bottom) / 2)};

    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    const HMONITOR monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(monitor, &monitorInfo);
    const int workWidth = std::max(1L, monitorInfo.rcWork.right - monitorInfo.rcWork.left);
    const int workHeight = std::max(1L, monitorInfo.rcWork.bottom - monitorInfo.rcWork.top);

    // Clamp the zoom itself, not width and height independently, so very wide
    // or very tall images never get distorted.
    const double monitorMaxZoom = std::min(
        static_cast<double>(workWidth) / bitmapWidth_,
        static_cast<double>(workHeight) / bitmapHeight_);
    const double hardMaxZoom = std::min(
        4096.0 / bitmapWidth_,
        4096.0 / bitmapHeight_);
    const double maxZoom = std::max(0.01, std::min(8.0, std::min(monitorMaxZoom, hardMaxZoom)));
    zoom_ = std::clamp(zoom_, 0.01, maxZoom);

    const int width = std::max(1, static_cast<int>(bitmapWidth_ * zoom_));
    const int height = std::max(1, static_cast<int>(bitmapHeight_ * zoom_));
    int x = center.x - width / 2;
    int y = center.y - height / 2;
    x = std::clamp(x, static_cast<int>(monitorInfo.rcWork.left),
                   std::max(static_cast<int>(monitorInfo.rcWork.left), static_cast<int>(monitorInfo.rcWork.right) - width));
    y = std::clamp(y, static_cast<int>(monitorInfo.rcWork.top),
                   std::max(static_cast<int>(monitorInfo.rcWork.top), static_cast<int>(monitorInfo.rcWork.bottom) - height));

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PinWindow::AdjustZoom''',
)

# ---------------------------------------------------------------------------
# 5. Snip core: persistent back buffer, expose capture for transparent text,
#    and completely deactivate the legacy toolbar paint/input path.
# ---------------------------------------------------------------------------
replace_once(
    "src/snip_window.h",
    "    HWND UiHwnd() const;\n",
    "    HWND UiHwnd() const;\n    HBITMAP UiCaptureBitmap() const;\n",
)
replace_once(
    "src/snip_window.h",
    "    void Paint();\n",
    "    void Paint();\n    HDC AcquireFrameBuffer(HDC reference);\n    void ReleaseFrameBuffer();\n",
)
replace_once(
    "src/snip_window.h",
    "    HBITMAP capture_{};\n",
    "    HBITMAP capture_{};\n    HDC frameDc_{};\n    HBITMAP frameBitmap_{};\n    HGDIOBJ frameOldBitmap_{};\n",
)
replace_once(
    "src/snip_window.cpp",
    "HWND SnipWindow::UiHwnd() const { return hwnd_; }\n",
    '''HWND SnipWindow::UiHwnd() const { return hwnd_; }
HBITMAP SnipWindow::UiCaptureBitmap() const { return capture_; }

HDC SnipWindow::AcquireFrameBuffer(HDC reference) {
    if (frameDc_ && frameBitmap_) {
        return frameDc_;
    }
    ReleaseFrameBuffer();
    frameDc_ = CreateCompatibleDC(reference);
    frameBitmap_ = frameDc_ ? CreateCompatibleBitmap(reference, screen_.width, screen_.height) : nullptr;
    if (!frameDc_ || !frameBitmap_) {
        ReleaseFrameBuffer();
        return nullptr;
    }
    frameOldBitmap_ = SelectObject(frameDc_, frameBitmap_);
    return frameDc_;
}

void SnipWindow::ReleaseFrameBuffer() {
    if (frameDc_ && frameOldBitmap_) {
        SelectObject(frameDc_, frameOldBitmap_);
    }
    frameOldBitmap_ = nullptr;
    if (frameBitmap_) {
        DeleteObject(frameBitmap_);
        frameBitmap_ = nullptr;
    }
    if (frameDc_) {
        DeleteDC(frameDc_);
        frameDc_ = nullptr;
    }
}
''',
)
replace_once(
    "src/snip_window_original.inc",
    "SnipWindow::~SnipWindow() {\n",
    "SnipWindow::~SnipWindow() {\n    ReleaseFrameBuffer();\n",
)
regex_once(
    "src/snip_window_original.inc",
    r"int SnipWindow::HitToolbar\(POINT point\) const \{.*?\n\}\n\nvoid SnipWindow::SetCursorForPoint",
    '''int SnipWindow::HitToolbar(POINT) const {
    // The child editor toolbar is the only interactive toolbar. Keeping the
    // legacy hit-test disabled prevents an invisible click target underneath it.
    return -1;
}

void SnipWindow::SetCursorForPoint''',
)
replace_once(
    "src/snip_window_original.inc",
    '''            const int hovered = HitToolbar(point);
            if (hovered != hoverToolbar_) {
                hoverToolbar_ = hovered;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            SetCursorForPoint(point);
''',
    '''            hoverToolbar_ = -1;
            SetCursorForPoint(point);
''',
)
replace_once(
    "src/snip_window_original.inc",
    '''        const int toolbarIndex = HitToolbar(point);
        if (toolbarIndex >= 0) {
            HandleToolbarClick(toolbarIndex);
            return 0;
        }
        // Group dividers are part of the bar: swallow the click rather than
        // letting it start a new selection behind the toolbar.
        if (selected_) {
            const RECT bar = ToolbarRect();
            if (PtInRect(&bar, point)) {
                return 0;
            }
        }

''',
    '''        // Legacy toolbar input is disabled. The visible child toolbar owns
        // every toolbar click, so the parent must never keep a hidden hot zone.

''',
)
replace_once(
    "src/snip_window_original.inc",
    "        PaintToolbar(frameDc);\n",
    "        // Legacy toolbar disabled; editor_toolbar.cpp owns the only visible toolbar.\n",
)
regex_once(
    "src/snip_window_original.inc",
    r"    HDC frameDc = CreateCompatibleDC\(dc\);\n    HBITMAP frameBitmap = CreateCompatibleBitmap\(dc, screen_\.width, screen_\.height\);\n    if \(!frameDc \|\| !frameBitmap\) \{.*?\n    \}\n    const HGDIOBJ oldFrame = SelectObject\(frameDc, frameBitmap\);\n\n    HDC captureDc = CreateCompatibleDC\(dc\);\n    if \(!captureDc\) \{\n        SelectObject\(frameDc, oldFrame\);\n        DeleteObject\(frameBitmap\);\n        DeleteDC\(frameDc\);\n        EndPaint\(hwnd_, &ps\);\n        return;\n    \}",
    '''    HDC frameDc = AcquireFrameBuffer(dc);
    if (!frameDc) {
        EndPaint(hwnd_, &ps);
        return;
    }

    HDC captureDc = CreateCompatibleDC(dc);
    if (!captureDc) {
        EndPaint(hwnd_, &ps);
        return;
    }''',
)
replace_once(
    "src/snip_window_original.inc",
    '''    SelectObject(frameDc, oldFrame);
    DeleteObject(frameBitmap);
    DeleteDC(frameDc);
    EndPaint(hwnd_, &ps);
''',
    '''    EndPaint(hwnd_, &ps);
''',
)

# window_bounds_fix should only correct DWM bounds now. Toolbar lifecycle is
# handled explicitly by editor_toolbar.cpp rather than by SetCapture macros.
window_bounds = read("src/window_bounds_fix.h")
window_bounds = re.sub(
    r"\ninline void HideEditorToolbarAndRestoreBackground\(.*?#define SetCapture\(hwnd\) \\\n    snaplite::detail::SetCaptureForSnip\(\(hwnd\)\)\n?",
    "\n",
    window_bounds,
    flags=re.S,
)
if "HideEditorToolbarAndRestoreBackground" in window_bounds or "#define SetCapture" in window_bounds:
    raise RuntimeError("src/window_bounds_fix.h: failed to remove toolbar SetCapture interception")
write("src/window_bounds_fix.h", window_bounds)

# ---------------------------------------------------------------------------
# 6. Editor toolbar: make compact/show-hide behavior explicit, use direct icon
#    rendering (no API macro interception), and repair transparent text editing.
# ---------------------------------------------------------------------------
replace_once("src/editor_toolbar.cpp", '#include "snip_window.h"\n', '#include "snip_window.h"\n#include "toolbar_icon_render_gdi.h"\n')
replace_once(
    "src/editor_toolbar.cpp",
    "    std::wstring editBackup;\n",
    "    std::wstring editBackup;\n    COLORREF editBackupColor{RGB(235, 70, 70)};\n    int editBackupSizePt{16};\n",
)
regex_once(
    "src/editor_toolbar.cpp",
    r"    void AttachToolbar\(HWND child\) \{.*?\n    bool HandleParentBefore",
    '''    void AttachToolbar(HWND child) {
        toolbar_ = child;
        ApplyRoundedRegion();
        UpdatePosition();
    }

    bool HasSecondaryOptions() const {
        return category_ == Category::Shape || category_ == Category::Arrow ||
               category_ == Category::Pen || category_ == Category::Text;
    }

    int DesiredHeight() const {
        return HasSecondaryOptions() ? kToolbarHeight : kPrimaryHeight;
    }

    bool SelectionDragActive() const {
        return parent_ && GetCapture() == parent_ && snip_ && snip_->UiActiveTool() < 0;
    }

    void ApplyRoundedRegion() {
        if (!toolbar_) return;
        RECT client{};
        if (!GetClientRect(toolbar_, &client)) return;
        const int width = std::max(1L, client.right - client.left);
        const int height = std::max(1L, client.bottom - client.top);
        const int ellipse = height <= kPrimaryHeight ? 18 : 16;
        HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, ellipse, ellipse);
        if (region && SetWindowRgn(toolbar_, region, TRUE) == 0) {
            DeleteObject(region);
        }
    }

    void HideToolbarAndRestoreBackground() {
        if (!toolbar_ || !IsWindowVisible(toolbar_)) return;
        RECT screenRect{};
        if (!GetWindowRect(toolbar_, &screenRect)) {
            ShowWindow(toolbar_, SW_HIDE);
            return;
        }
        POINT topLeft{screenRect.left, screenRect.top};
        POINT bottomRight{screenRect.right, screenRect.bottom};
        ScreenToClient(parent_, &topLeft);
        ScreenToClient(parent_, &bottomRight);
        RECT dirty{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
        ShowWindow(toolbar_, SW_HIDE);
        InflateRect(&dirty, 3, 3);
        RedrawWindow(parent_, &dirty, nullptr,
                     RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOCHILDREN);
    }

    void UpdatePosition() {
        if (!toolbar_ || !snip_) return;
        if (!snip_->UiHasSelection()) {
            HideToolbarAndRestoreBackground();
            return;
        }
        if (SelectionDragActive()) {
            HideToolbarAndRestoreBackground();
            return;
        }

        const RECT selection = snip_->UiSelectionRect();
        const RECT legacy = snip_->UiLegacyToolbarRect();
        RECT client{};
        GetClientRect(parent_, &client);
        const int height = DesiredHeight();

        int x = static_cast<int>(legacy.left);
        int y = static_cast<int>(legacy.top);
        if (legacy.top < selection.top) {
            y = static_cast<int>(legacy.bottom) - height;
        }
        x = std::clamp(x, 6, std::max(6, static_cast<int>(client.right) - kToolbarWidth - 6));
        y = std::clamp(y, 6, std::max(6, static_cast<int>(client.bottom) - height - 6));

        SetWindowPos(toolbar_, HWND_TOP, x, y, kToolbarWidth, height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ApplyRoundedRegion();
    }

    void SyncCategoryFromTool() {
        if (!snip_) return;
        const int tool = snip_->UiActiveTool();
        if (tool >= 0 && tool <= 4) {
            const Category next = static_cast<Category>(tool + 1);
            if (next != category_) {
                category_ = next;
                UpdatePosition();
                InvalidateRect(toolbar_, nullptr, FALSE);
            }
        }
    }

    bool HandleParentBefore''',
)
replace_once(
    "src/editor_toolbar.cpp",
    '''        if (message == WM_PAINT || message == WM_MOUSEMOVE || message == WM_LBUTTONUP ||
            message == WM_KEYDOWN || message == WM_SIZE || message == WM_DPICHANGED) {
''',
    '''        if (message == WM_PAINT || message == WM_LBUTTONDOWN || message == WM_MOUSEMOVE ||
            message == WM_LBUTTONUP || message == WM_KEYDOWN || message == WM_SIZE ||
            message == WM_DPICHANGED) {
''',
)
replace_once(
    "src/editor_toolbar.cpp",
    '''        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLOREDIT: {
''',
    '''        case WM_ERASEBKGND: {
            // Repaint the real capture pixels under a transparent EDIT control.
            // This makes Backspace/Delete erase glyphs immediately instead of
            // leaving stale text until editing ends.
            HDC dc = reinterpret_cast<HDC>(wParam);
            RECT client{};
            GetClientRect(hwnd, &client);
            HBITMAP capture = snip_ ? snip_->UiCaptureBitmap() : nullptr;
            HDC source = capture ? CreateCompatibleDC(dc) : nullptr;
            if (source) {
                const HGDIOBJ old = SelectObject(source, capture);
                BitBlt(dc, 0, 0, client.right, client.bottom,
                       source, item->origin.x, item->origin.y, SRCCOPY);
                SelectObject(source, old);
                DeleteDC(source);
            }
            return 1;
        }
        case WM_CTLCOLOREDIT: {
''',
)
replace_once(
    "src/editor_toolbar.cpp",
    '''        case WM_COMMAND:
            if (item->edit && reinterpret_cast<HWND>(lParam) == item->edit &&
                HIWORD(wParam) == EN_KILLFOCUS) {
                CommitTextEdit(item);
                return 0;
            }
            break;
''',
    '''        case WM_COMMAND:
            if (item->edit && reinterpret_cast<HWND>(lParam) == item->edit) {
                const WORD notify = HIWORD(wParam);
                if (notify == EN_UPDATE) {
                    RedrawWindow(hwnd, nullptr, nullptr,
                                 RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
                    return 0;
                }
                if (notify == EN_KILLFOCUS) {
                    CommitTextEdit(item);
                    return 0;
                }
            }
            break;
''',
)
replace_once(
    "src/editor_toolbar.cpp",
    '''        const int length = GetWindowTextLengthW(item->edit);
        std::wstring text(static_cast<size_t>(std::max(0, length)), L'\\0');
        if (length > 0) GetWindowTextW(item->edit, text.data(), length + 1);
''',
    '''        const int length = std::max(0, GetWindowTextLengthW(item->edit));
        std::wstring text(static_cast<size_t>(length) + 1, L'\\0');
        if (length > 0) GetWindowTextW(item->edit, text.data(), length + 1);
        text.resize(static_cast<size_t>(length));
''',
)
replace_once(
    "src/editor_toolbar.cpp",
    '''        const bool wasNew = item->editWasNew;
        HWND edit = item->edit;
''',
    '''        const bool wasNew = item->editWasNew;
        const COLORREF backupColor = item->editBackupColor;
        const int backupSize = item->editBackupSizePt;
        HWND edit = item->edit;
''',
)
replace_once(
    "src/editor_toolbar.cpp",
    '''        item->text = item->editBackup;
        ResizeTextItem(item);
        InvalidateRect(item->hwnd, nullptr, FALSE);
''',
    '''        item->text = item->editBackup;
        item->color = backupColor;
        item->sizePt = backupSize;
        RecreateTextFont(item);
        ResizeTextItem(item);
        InvalidateRect(item->hwnd, nullptr, FALSE);
''',
)
replace_once(
    "src/editor_toolbar.cpp",
    '''        item->editBackup = item->text;
        EnsureTextFont(item);
''',
    '''        item->editBackup = item->text;
        item->editBackupColor = item->color;
        item->editBackupSizePt = item->sizePt;
        EnsureTextFont(item);
''',
)
regex_once(
    "src/editor_toolbar.cpp",
    r"    void ApplyColor\(COLORREF color\) \{.*?\n    \}\n\n    void ApplyTextSize",
    '''    void ApplyColor(COLORREF color) {
        if (selectedText_) {
            const bool editing = selectedText_->edit != nullptr;
            const auto before = editing ? std::vector<TextState>{} : SnapshotTextStates();
            selectedText_->color = color;
            if (selectedText_->edit) {
                RedrawWindow(selectedText_->hwnd, nullptr, nullptr,
                             RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            } else {
                InvalidateRect(selectedText_->hwnd, nullptr, FALSE);
                RecordTextAction(before, SnapshotTextStates());
            }
        }
        snip_->UiSetColor(color);
        InvalidateRect(toolbar_, nullptr, FALSE);
    }

    void ApplyTextSize''',
)
regex_once(
    "src/editor_toolbar.cpp",
    r"    void ApplyTextSize\(int points\) \{.*?\n    \}\n\n    void ClickSecondary",
    '''    void ApplyTextSize(int points) {
        if (selectedText_) {
            const bool editing = selectedText_->edit != nullptr;
            const auto before = editing ? std::vector<TextState>{} : SnapshotTextStates();
            selectedText_->sizePt = std::clamp(points, 10, 72);
            RecreateTextFont(selectedText_);
            ResizeTextItem(selectedText_);
            if (selectedText_->edit && selectedText_->font) {
                SendMessageW(selectedText_->edit, WM_SETFONT,
                             reinterpret_cast<WPARAM>(selectedText_->font), TRUE);
                RedrawWindow(selectedText_->hwnd, nullptr, nullptr,
                             RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            } else {
                RecordTextAction(before, SnapshotTextStates());
            }
        }
        snip_->UiSetTextSize(points);
        InvalidateRect(toolbar_, nullptr, FALSE);
    }

    void ClickSecondary''',
)
replace_once(
    "src/editor_toolbar.cpp",
    '''            overlays.push_back({item->text, item->origin, item->color, item->sizePt});
''',
    '''            // PaintTextItem renders committed text with this content inset.
            // Bake using the same coordinates so saved/copied images match preview.
            overlays.push_back({item->text, {item->origin.x + 4, item->origin.y + 2},
                                item->color, item->sizePt});
''',
)
replace_once(
    "src/editor_toolbar.cpp",
    '''        InvalidateRect(toolbar_, nullptr, FALSE);
    }

    void ExecutePrimaryAction''',
    '''        UpdatePosition();
        InvalidateRect(toolbar_, nullptr, FALSE);
    }

    void ExecutePrimaryAction''',
)
# Explicit warm theme and direct icon renderer, replacing the previous forced
# macro headers.
replace_all_checked("src/editor_toolbar.cpp", {
    "RGB(250, 250, 251)": "RGB(252, 250, 247)",
    "RGB(63, 78, 104)": "RGB(99, 82, 62)",
    "RGB(65, 66, 70)": "RGB(58, 56, 53)",
    "RGB(170, 72, 72)": "RGB(196, 76, 72)",
    "RGB(72, 73, 77)": "RGB(70, 67, 63)",
    "RGB(59, 76, 108)": "RGB(105, 84, 62)",
    "RGB(78, 79, 84)": "RGB(82, 78, 73)",
    "RGB(116, 117, 122)": "RGB(137, 128, 118)",
    "Gdiplus::Color(255, 229, 230, 233)": "Gdiplus::Color(255, 232, 226, 219)",
    "Gdiplus::Color(255, 232, 236, 244)": "Gdiplus::Color(255, 240, 233, 224)",
    "Gdiplus::Color(255, 241, 242, 244)": "Gdiplus::Color(255, 247, 243, 238)",
    "Gdiplus::Color(255, 250, 236, 236)": "Gdiplus::Color(255, 251, 237, 235)",
    "Gdiplus::Color(255, 230, 235, 245)": "Gdiplus::Color(255, 239, 232, 222)",
    "Gdiplus::Color(255, 240, 241, 243)": "Gdiplus::Color(255, 247, 243, 238)",
    "Gdiplus::Color(255, 247, 247, 248)": "Gdiplus::Color(255, 252, 250, 247)",
    "Gdiplus::Color(255, 75, 94, 128)": "Gdiplus::Color(255, 151, 128, 99)",
    "Gdiplus::Color(255, 190, 192, 197)": "Gdiplus::Color(255, 198, 190, 181)",
})
replace_once(
    "src/editor_toolbar.cpp",
    "            DrawTextW(mem, kToolLabels[i], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);\n",
    "            toolbaricons_gdi::DrawTextOrIcon(mem, kToolLabels[i], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);\n",
)
replace_once(
    "src/editor_toolbar.cpp",
    "            DrawTextW(mem, kActionLabels[i], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);\n",
    "            toolbaricons_gdi::DrawTextOrIcon(mem, kActionLabels[i], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);\n",
)
replace_once(
    "src/editor_toolbar.cpp",
    '''            DrawTextW(mem, item.label.c_str(), -1, &r,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
''',
    '''            toolbaricons_gdi::DrawTextOrIcon(mem, item.label.c_str(), -1, &r,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
''',
)
replace_once(
    "src/editor_toolbar.cpp",
    '''        SelectObject(mem, oldFont);
        BitBlt(dc, 0, 0, kToolbarWidth, kToolbarHeight, mem, 0, 0, SRCCOPY);
''',
    '''        SelectObject(mem, oldFont);
        BitBlt(dc, 0, 0, kToolbarWidth, kToolbarHeight, mem, 0, 0, SRCCOPY);
        RECT client{};
        GetClientRect(hwnd, &client);
        HPEN border = CreatePen(PS_SOLID, 1, RGB(229, 223, 216));
        const HGDIOBJ oldPen = SelectObject(dc, border);
        const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, 0, 0, client.right, client.bottom, 18, 18);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(border);
''',
)
replace_once(
    "src/editor_toolbar.cpp",
    '''        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, kToolbarWidth, kToolbarHeight,
''',
    '''        WS_CHILD | WS_CLIPSIBLINGS,
        0, 0, kToolbarWidth, kPrimaryHeight,
''',
)
# Remove the API-macro tail from the icon helper; it is now called explicitly.
icon_header = read("src/toolbar_icon_render_gdi.h")
icon_header = re.sub(r"\n#define DrawTextW\(\.\.\.\).*?\n?$", "\n", icon_header, flags=re.S)
if "#define DrawTextW" in icon_header:
    raise RuntimeError("src/toolbar_icon_render_gdi.h: failed to remove DrawTextW macro")
write("src/toolbar_icon_render_gdi.h", icon_header)

# ---------------------------------------------------------------------------
# 7. CI regression guards.
# ---------------------------------------------------------------------------
replace_once(
    ".github/workflows/build.yml",
    "      - name: Configure\n",
    '''      - name: Source invariants
        shell: pwsh
        run: |
          $snip = Get-Content src/snip_window_original.inc -Raw
          if ($snip.Contains('PaintToolbar(frameDc);')) { throw 'Legacy toolbar paint path is active.' }
          $editor = Get-Content src/editor_toolbar.cpp -Raw
          if ($editor.Contains('#define DrawTextW')) { throw 'Toolbar icon macro interception returned.' }
          if (Test-Path src/editor_window.cpp) { throw 'Dead legacy EditorWindow source returned.' }
          if (Test-Path .github/workflows/fix-beta2-once.yml) { throw 'One-time source mutation workflow must not ship.' }
          if (Test-Path .github/workflows/release-beta.yml) { throw 'Beta source-mutation release workflow must not ship.' }

      - name: Configure
''',
)

# ---------------------------------------------------------------------------
# 8. Remove dead/unsafe files. The generic release workflow remains the only
#    publisher and never edits source code.
# ---------------------------------------------------------------------------
for relative in [
    "src/editor_window.cpp",
    "src/editor_window.h",
    "src/anime_toolbar.cpp",
    "src/toolbar_compact_behavior.h",
    "src/toolbar_visual_theme.h",
    ".github/workflows/release-beta.yml",
    ".github/workflows/fix-beta2-once.yml",
    ".release-beta/v1.3.3-beta.1.txt",
    ".release-beta/v1.3.3-beta.2.txt",
]:
    path = ROOT / relative
    if path.exists():
        path.unlink()

# The migration script is intentionally one-shot and must not remain in the
# resulting tree.
Path(__file__).unlink()

print("Snap-Lite stability fixes applied successfully.")
