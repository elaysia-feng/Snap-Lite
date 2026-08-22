$ErrorActionPreference = 'Stop'

function Replace-Exact([string]$Path, [string]$Old, [string]$New) {
    $text = [System.IO.File]::ReadAllText($Path)
    if (-not $text.Contains($Old)) {
        throw "Expected text was not found in $Path`n--- expected ---`n$Old"
    }
    $text = $text.Replace($Old, $New)
    [System.IO.File]::WriteAllText($Path, $text, [System.Text.UTF8Encoding]::new($false))
}

# -----------------------------------------------------------------------------
# 1. Anime-tech screenshot theme: match the blue/white Snap-Lite app icon.
# -----------------------------------------------------------------------------
$snip = 'src/snip_window.cpp'

Replace-Exact $snip @'
constexpr COLORREF kInk = RGB(236, 239, 244);

constexpr BYTE kDimSelected = 148;  // 58% scrim: the selection is the only lit area
constexpr BYTE kDimIdle = 66;       // lighter scrim before a selection exists

Gdiplus::Color Chrome(BYTE alpha = 242) { return {alpha, 27, 30, 35}; }
Gdiplus::Color ChromeLine() { return {56, 255, 255, 255}; }   // bright half of a hairline
Gdiplus::Color ChromeShade() { return {110, 0, 0, 0}; }       // dark half of a hairline
Gdiplus::Color Accent(BYTE alpha = 255) { return {alpha, 255, 197, 61}; }
Gdiplus::Color Ink(BYTE alpha = 255) { return {alpha, 236, 239, 244}; }
Gdiplus::Color Danger() { return {255, 255, 107, 91}; }

constexpr int kHandleRadius = 4;
constexpr int kHandleHitRadius = 9;
constexpr int kToolbarButton = 36;
constexpr int kToolbarHeight = 40;
'@ @'
constexpr COLORREF kInk = RGB(230, 247, 255);

// Blue-violet glass scrim inspired by the Snap-Lite anime icon. The selected
// pixels stay untouched while the surrounding desktop gets a cool tint.
constexpr BYTE kDimSelected = 124;
constexpr BYTE kDimIdle = 72;

Gdiplus::Color Chrome(BYTE alpha = 242) { return {alpha, 18, 25, 43}; }
Gdiplus::Color ChromeLine() { return {82, 143, 232, 255}; }
Gdiplus::Color ChromeShade() { return {125, 7, 14, 31}; }
Gdiplus::Color Accent(BYTE alpha = 255) { return {alpha, 111, 231, 255}; }
Gdiplus::Color Ink(BYTE alpha = 255) { return {alpha, 230, 247, 255}; }
Gdiplus::Color Danger() { return {255, 255, 111, 164}; }

constexpr int kHandleRadius = 5;
constexpr int kHandleHitRadius = 10;
constexpr int kToolbarButton = 36;
constexpr int kToolbarHeight = 42;
'@

Replace-Exact $snip 'constexpr int kToolbarRadius = 10;' 'constexpr int kToolbarRadius = 14;'
Replace-Exact $snip 'Gdiplus::SolidBrush brush(Gdiplus::Color(13, 0, 0, 0));' 'Gdiplus::SolidBrush brush(Gdiplus::Color(16, 43, 93, 170));'
Replace-Exact $snip 'Gdiplus::SolidBrush dimBrush(Gdiplus::Color(kDimSelected, 0, 0, 0));' 'Gdiplus::SolidBrush dimBrush(Gdiplus::Color(kDimSelected, 19, 24, 48));'
Replace-Exact $snip 'Gdiplus::SolidBrush dimBrush(Gdiplus::Color(kDimIdle, 0, 0, 0));' 'Gdiplus::SolidBrush dimBrush(Gdiplus::Color(kDimIdle, 19, 24, 48));'
Replace-Exact $snip 'AddRoundRect(path, box, 1.5f);' 'AddRoundRect(path, box, static_cast<float>(kHandleRadius));'
Replace-Exact $snip 'FillRoundRect(graphics, Gdiplus::Color(26, 255, 255, 255), box, 6.0f);' 'FillRoundRect(graphics, Gdiplus::Color(36, 111, 231, 255), box, 8.0f);'

Replace-Exact $snip @'
        Gdiplus::Pen frame(Accent(), 1.0f);
        graphics.DrawRectangle(&frame, left, top, width - 1.0f, height - 1.0f);

        // Small squared handles read as tooling marks rather than UI bubbles,
'@ @'
        Gdiplus::Pen frame(Accent(), 2.0f);
        graphics.DrawRectangle(&frame, left, top, width - 1.0f, height - 1.0f);

        // Viewfinder corners give the selection a light anime-camera identity
        // without putting decorative artwork over the captured pixels.
        constexpr float corner = 15.0f;
        Gdiplus::Pen cornerPen(Accent(), 3.0f);
        cornerPen.SetStartCap(Gdiplus::LineCapRound);
        cornerPen.SetEndCap(Gdiplus::LineCapRound);
        graphics.DrawLine(&cornerPen, left, top, left + corner, top);
        graphics.DrawLine(&cornerPen, left, top, left, top + corner);
        graphics.DrawLine(&cornerPen, left + width - corner, top, left + width, top);
        graphics.DrawLine(&cornerPen, left + width, top, left + width, top + corner);
        graphics.DrawLine(&cornerPen, left, top + height - corner, left, top + height);
        graphics.DrawLine(&cornerPen, left, top + height, left + corner, top + height);
        graphics.DrawLine(&cornerPen, left + width - corner, top + height, left + width, top + height);
        graphics.DrawLine(&cornerPen, left + width, top + height - corner, left + width, top + height);

        // Soft circular handles keep the controls readable without the old
        // industrial yellow square look.
'@

Replace-Exact $snip @'
    // Active tools get an amber rule under the glyph instead of a filled chip,
    // so the glyph keeps the same weight whether the tool is on or off.
    if (active) {
        Gdiplus::SolidBrush accent(Accent());
        graphics.FillRectangle(&accent, cx - 9.0f, static_cast<float>(rect.bottom) - 6.0f, 18.0f, 2.0f);
    }
'@ @'
    // Active tool: translucent cyan glass chip plus a short luminous rule.
    if (active) {
        Gdiplus::RectF activeBox(
            static_cast<float>(rect.left) + 3.0f,
            static_cast<float>(rect.top) + 3.0f,
            static_cast<float>(rect.right - rect.left) - 6.0f,
            static_cast<float>(rect.bottom - rect.top) - 6.0f);
        FillRoundRect(graphics, Accent(34), activeBox, 8.0f);
        Gdiplus::SolidBrush accent(Accent());
        graphics.FillRectangle(&accent, cx - 8.0f, static_cast<float>(rect.bottom) - 6.0f, 16.0f, 2.0f);
    }
'@

# -----------------------------------------------------------------------------
# 2. Real memory guard: annotation history used to keep up to 20 full bitmaps.
#    Cap both count and total bitmap bytes so a large crop cannot explode RAM.
# -----------------------------------------------------------------------------
Replace-Exact $snip @'
void DeleteBitmaps(std::vector<HBITMAP>& bitmaps) {
    for (HBITMAP bitmap : bitmaps) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
    }
    bitmaps.clear();
}
'@ @'
void DeleteBitmaps(std::vector<HBITMAP>& bitmaps) {
    for (HBITMAP bitmap : bitmaps) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
    }
    bitmaps.clear();
}

SIZE_T BitmapBytes(HBITMAP bitmap) {
    if (!bitmap) {
        return 0;
    }
    BITMAP info{};
    if (GetObjectW(bitmap, sizeof(info), &info) == 0) {
        return 0;
    }
    const SIZE_T stride = static_cast<SIZE_T>(info.bmWidthBytes >= 0 ? info.bmWidthBytes : -info.bmWidthBytes);
    const SIZE_T height = static_cast<SIZE_T>(info.bmHeight >= 0 ? info.bmHeight : -info.bmHeight);
    return stride * height;
}

SIZE_T BitmapListBytes(const std::vector<HBITMAP>& bitmaps) {
    SIZE_T total = 0;
    for (HBITMAP bitmap : bitmaps) {
        total += BitmapBytes(bitmap);
    }
    return total;
}
'@

Replace-Exact $snip @'
    if (snapshot) {
        undo_.push_back(snapshot);
        if (undo_.size() > 20) {
            DeleteObject(undo_.front());
            undo_.erase(undo_.begin());
        }
    }
'@ @'
    if (snapshot) {
        undo_.push_back(snapshot);

        // A 2560x1600 crop is roughly 16 MB per 32-bit snapshot. Keeping the
        // old 20 snapshots could therefore consume hundreds of MB. Preserve a
        // useful undo depth for normal crops while hard-capping bitmap memory.
        constexpr size_t kMaxUndoEntries = 6;
        constexpr SIZE_T kMaxUndoBytes = 32ull * 1024ull * 1024ull;
        while (undo_.size() > 1 &&
               (undo_.size() > kMaxUndoEntries || BitmapListBytes(undo_) > kMaxUndoBytes)) {
            DeleteObject(undo_.front());
            undo_.erase(undo_.begin());
        }
    }
'@

# -----------------------------------------------------------------------------
# 3. User-selectable Windows startup + idle working-set trim.
# -----------------------------------------------------------------------------
$app = 'src/app.cpp'
Replace-Exact $app 'constexpr UINT CMD_EXIT = 1005;' @'
constexpr UINT CMD_EXIT = 1005;
constexpr UINT CMD_AUTOSTART = 1006;
'@

Replace-Exact $app @'
HICON LoadAppIcon(HINSTANCE instance) {
    HICON icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SNAPLITE));
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}
'@ @'
HICON LoadAppIcon(HINSTANCE instance) {
    HICON icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SNAPLITE));
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"Snap-Lite";

bool IsStartupEnabled() {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    const LONG result = RegQueryValueExW(key, kRunValue, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

bool SetStartupEnabled(bool enabled) {
    HKEY key{};
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
            KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    LONG result = ERROR_SUCCESS;
    if (enabled) {
        wchar_t exePath[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
        if (length == 0 || length >= std::size(exePath)) {
            RegCloseKey(key);
            return false;
        }
        const std::wstring command = L"\"" + std::wstring(exePath, length) + L"\"";
        result = RegSetValueExW(
            key, kRunValue, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, kRunValue);
        if (result == ERROR_FILE_NOT_FOUND) {
            result = ERROR_SUCCESS;
        }
    }
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

void TrimWorkingSet() {
    // This does not discard live allocations. It asks Windows to reclaim
    // already-unused resident pages after screenshot work has finished.
    SetProcessWorkingSetSize(
        GetCurrentProcess(),
        static_cast<SIZE_T>(-1),
        static_cast<SIZE_T>(-1));
}
'@

Replace-Exact $app @'
    AppendMenuW(menu, MF_STRING, CMD_OPEN_FOLDER, L"打开截图目录");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_EXIT, L"退出 Snap-Lite");
'@ @'
    AppendMenuW(menu, MF_STRING, CMD_OPEN_FOLDER, L"打开截图目录");
    AppendMenuW(
        menu,
        MF_STRING | (IsStartupEnabled() ? MF_CHECKED : MF_UNCHECKED),
        CMD_AUTOSTART,
        L"开机自启动");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_EXIT, L"退出 Snap-Lite");
'@

Replace-Exact $app @'
        case CMD_OPEN_FOLDER:
            ShellExecuteW(hwnd_, L"open", ScreenshotDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        case CMD_EXIT:
'@ @'
        case CMD_OPEN_FOLDER:
            ShellExecuteW(hwnd_, L"open", ScreenshotDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        case CMD_AUTOSTART: {
            const bool next = !IsStartupEnabled();
            if (SetStartupEnabled(next)) {
                ShowNotice(next ? L"已开启开机自启动" : L"已关闭开机自启动");
            } else {
                ShowNotice(L"修改开机自启动失败");
            }
            return 0;
        }
        case CMD_EXIT:
'@

Replace-Exact $app @'
                if (saved) {
                    const std::wstring message = L"截图已保存：\n" + path.wstring();
                    ShowNotice(message.c_str());
                } else {
                    ShowNotice(L"截图保存失败");
                }
                return;
'@ @'
                if (saved) {
                    const std::wstring message = L"截图已保存：\n" + path.wstring();
                    ShowNotice(message.c_str());
                } else {
                    ShowNotice(L"截图保存失败");
                }
                TrimWorkingSet();
                return;
'@

Replace-Exact $app @'
    } else {
        ShowNotice(L"截图处理失败");
    }
}

LRESULT App::HandleMessage'@ @'
    } else {
        ShowNotice(L"截图处理失败");
    }
    TrimWorkingSet();
}

LRESULT App::HandleMessage'@

# Advapi32 provides the HKCU Run-key APIs used by the optional startup toggle.
$cmake = 'CMakeLists.txt'
Replace-Exact $cmake 'target_link_libraries(SnapLite PRIVATE user32 gdi32 gdiplus shell32 ole32 uuid)' 'target_link_libraries(SnapLite PRIVATE user32 gdi32 gdiplus shell32 ole32 uuid advapi32)'

# Remove this one-shot migration from the repository after it has done its job.
Remove-Item 'scripts/upgrade-once.ps1' -Force
Remove-Item '.github/workflows/upgrade-once.yml' -Force
