#include "app.h"

#include "anime_toolbar.h"
#include "capture.h"
#include "pin_window.h"
#include "resource.h"
#include "snip_window.h"

#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <filesystem>
#include <string>
#include <vector>

namespace snaplite {
namespace {
constexpr wchar_t kMainClass[] = L"SnapLiteMainWindow";
constexpr wchar_t kAppName[] = L"Snap-Lite";
constexpr UINT WM_TRAYICON = WM_APP + 1;

constexpr int HOTKEY_SNIP = 1;
constexpr int HOTKEY_PIN = 2;
constexpr int HOTKEY_FULLSCREEN = 3;
constexpr int HOTKEY_PIN_FALLBACK = 4;

constexpr UINT CMD_SNIP = 1001;
constexpr UINT CMD_FULLSCREEN = 1002;
constexpr UINT CMD_PIN = 1003;
constexpr UINT CMD_OPEN_FOLDER = 1004;
constexpr UINT CMD_EXIT = 1005;
constexpr UINT CMD_AUTOSTART = 1006;
constexpr UINT CMD_SET_FOLDER = 1007;

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"Snap-Lite";

HICON LoadAppIcon(HINSTANCE instance) {
    HICON icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SNAPLITE));
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

std::wstring CurrentExecutableCommand() {
    wchar_t path[32768]{};
    constexpr DWORD capacity = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    const DWORD length = GetModuleFileNameW(nullptr, path, capacity);
    if (length == 0 || length >= capacity) {
        return {};
    }
    return L"\"" + std::wstring(path, length) + L"\"";
}

bool IsStartupEnabled() {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }

    wchar_t value[32768]{};
    DWORD type = 0;
    DWORD bytes = sizeof(value);
    const LONG result = RegQueryValueExW(
        key,
        kRunValue,
        nullptr,
        &type,
        reinterpret_cast<BYTE*>(value),
        &bytes);
    RegCloseKey(key);

    if (result != ERROR_SUCCESS || type != REG_SZ) {
        return false;
    }

    const std::wstring expected = CurrentExecutableCommand();
    return !expected.empty() && _wcsicmp(value, expected.c_str()) == 0;
}

bool SetStartupEnabled(bool enabled) {
    HKEY key{};
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER,
            kRunKey,
            0,
            nullptr,
            0,
            KEY_SET_VALUE | KEY_QUERY_VALUE,
            nullptr,
            &key,
            nullptr) != ERROR_SUCCESS) {
        return false;
    }

    LONG result = ERROR_SUCCESS;
    if (enabled) {
        const std::wstring command = CurrentExecutableCommand();
        if (command.empty()) {
            RegCloseKey(key);
            return false;
        }
        result = RegSetValueExW(
            key,
            kRunValue,
            0,
            REG_SZ,
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

std::filesystem::path ChooseScreenshotFolder(HWND owner) {
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

std::filesystem::path ChooseSaveAsPath(HWND owner) {
    const std::filesystem::path suggested = NextConfiguredScreenshotPath();
    const std::wstring initialDirectory = ConfiguredScreenshotDirectory().wstring();

    std::vector<wchar_t> file(32768, L'\0');
    wcsncpy_s(file.data(), file.size(), suggested.filename().c_str(), _TRUNCATE);

    static constexpr wchar_t filter[] =
        L"PNG 图片 (*.png)\0*.png\0"
        L"所有文件 (*.*)\0*.*\0\0";

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = file.data();
    dialog.nMaxFile = static_cast<DWORD>(file.size());
    dialog.lpstrInitialDir = initialDirectory.c_str();
    dialog.lpstrDefExt = L"png";
    dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

    if (!GetSaveFileNameW(&dialog)) {
        return {};
    }
    return std::filesystem::path(file.data());
}

void TrimWorkingSet() {
    SetProcessWorkingSetSize(
        GetCurrentProcess(),
        static_cast<SIZE_T>(-1),
        static_cast<SIZE_T>(-1));
}
}

App::App(HINSTANCE instance) : instance_(instance) {}

App::~App() {
    Shutdown();
}

bool App::Initialize() {
    singleInstance_ = CreateMutexW(nullptr, TRUE, L"Local\\SnapLiteSingleInstance");
    if (!singleInstance_) {
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(singleInstance_);
        singleInstance_ = nullptr;
        return false;
    }

    Gdiplus::GdiplusStartupInput gdiplusInput;
    if (Gdiplus::GdiplusStartup(&gdiplusToken_, &gdiplusInput, nullptr) != Gdiplus::Ok) {
        return false;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance_;
    wc.hIcon = LoadAppIcon(instance_);
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kMainClass;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    if (!SnipWindow::Register(instance_) ||
        !PinWindow::Register(instance_) ||
        !AnimeToolbar::Register(instance_)) {
        return false;
    }

    hwnd_ = CreateWindowExW(
        0,
        kMainClass,
        kAppName,
        WS_OVERLAPPED,
        0,
        0,
        0,
        0,
        nullptr,
        nullptr,
        instance_,
        this);
    if (!hwnd_) {
        return false;
    }

    const bool snipHotkeyRegistered =
        RegisterHotKey(hwnd_, HOTKEY_SNIP, MOD_NOREPEAT, VK_F1) != FALSE;
    pinHotkeyRegistered_ =
        RegisterHotKey(hwnd_, HOTKEY_PIN, MOD_NOREPEAT, VK_F3) != FALSE;
    const bool fullscreenHotkeyRegistered =
        RegisterHotKey(hwnd_, HOTKEY_FULLSCREEN, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'F') != FALSE;
    pinFallbackHotkeyRegistered_ = !pinHotkeyRegistered_ &&
        RegisterHotKey(hwnd_, HOTKEY_PIN_FALLBACK, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'P') != FALSE;

    AddTrayIcon();

    if (!pinHotkeyRegistered_) {
        if (pinFallbackHotkeyRegistered_) {
            ShowNotice(L"F3 热键被其他程序占用，贴图备用快捷键已切换为 Ctrl+Alt+P");
        } else {
            ShowNotice(L"F3 热键注册失败，可右键托盘图标选择“贴图”");
        }
    }
    if (!snipHotkeyRegistered) {
        ShowNotice(L"F1 截图热键注册失败，可能被其他程序占用");
    }
    if (!fullscreenHotkeyRegistered) {
        ShowNotice(L"Ctrl+Shift+F 全屏截图热键注册失败，可能被其他程序占用");
    }

    TrimWorkingSet();
    return true;
}

void App::Shutdown() {
    if (hwnd_) {
        UnregisterHotKey(hwnd_, HOTKEY_SNIP);
        UnregisterHotKey(hwnd_, HOTKEY_PIN);
        UnregisterHotKey(hwnd_, HOTKEY_FULLSCREEN);
        UnregisterHotKey(hwnd_, HOTKEY_PIN_FALLBACK);
    }
    pinHotkeyRegistered_ = false;
    pinFallbackHotkeyRegistered_ = false;

    if (tray_.hWnd) {
        Shell_NotifyIconW(NIM_DELETE, &tray_);
        tray_ = {};
    }

    if (gdiplusToken_) {
        Gdiplus::GdiplusShutdown(gdiplusToken_);
        gdiplusToken_ = 0;
    }

    if (singleInstance_) {
        ReleaseMutex(singleInstance_);
        CloseHandle(singleInstance_);
        singleInstance_ = nullptr;
    }
}

void App::AddTrayIcon() {
    tray_ = {};
    tray_.cbSize = sizeof(tray_);
    tray_.hWnd = hwnd_;
    tray_.uID = 1;
    tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    tray_.uCallbackMessage = WM_TRAYICON;
    tray_.hIcon = LoadAppIcon(instance_);
    const wchar_t* pinShortcut = pinFallbackHotkeyRegistered_ ? L"Ctrl+Alt+P"
        : pinHotkeyRegistered_ ? L"F3" : L"托盘菜单";
    swprintf_s(tray_.szTip, L"Snap-Lite · F1 截图 · %s 贴图", pinShortcut);
    Shell_NotifyIconW(NIM_ADD, &tray_);
}

void App::ShowNotice(const wchar_t* message) {
    if (!tray_.hWnd) {
        return;
    }

    tray_.uFlags = NIF_INFO | NIF_MESSAGE | NIF_ICON | NIF_TIP;
    wcscpy_s(tray_.szInfoTitle, kAppName);
    wcsncpy_s(tray_.szInfo, message, _TRUNCATE);
    tray_.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &tray_);
    tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
}

void App::ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    AppendMenuW(menu, MF_STRING, CMD_SNIP, L"截图\tF1");
    std::wstring pinLabel = L"贴图\t";
    if (pinFallbackHotkeyRegistered_) {
        pinLabel += L"Ctrl+Alt+P";
    } else if (pinHotkeyRegistered_) {
        pinLabel += L"F3";
    }
    AppendMenuW(menu, MF_STRING, CMD_PIN, pinLabel.c_str());
    AppendMenuW(menu, MF_STRING, CMD_FULLSCREEN, L"全屏截图\tCtrl+Shift+F");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_OPEN_FOLDER, L"打开截图目录");
    AppendMenuW(menu, MF_STRING, CMD_SET_FOLDER, L"设置默认保存目录...");
    AppendMenuW(
        menu,
        MF_STRING | (IsStartupEnabled() ? MF_CHECKED : MF_UNCHECKED),
        CMD_AUTOSTART,
        L"开机自启动");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_EXIT, L"退出 Snap-Lite");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(hwnd_);
    // TrackPopupMenu returns only after the menu dismisses; the WM_COMMAND for
    // the selected item is posted to hwnd_'s queue and dispatched on the next
    // message-pump turn. WM_COMMAND carries only the command id (no menu
    // handle), so DestroyMenu here is safe today. Keep this ordering if you
    // later adopt TPM_RETURNCMD or asynchronous menu handling.
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN, cursor.x, cursor.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

void App::StartSnip() {
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
        instance_,
        hwnd_,
        [this](HBITMAP bitmap, SnipWindow::FinishAction action) {
            if (!bitmap) {
                ShowNotice(L"截图失败");
                return;
            }

            if (action == SnipWindow::FinishAction::Save ||
                action == SnipWindow::FinishAction::SaveAs) {
                const std::filesystem::path path =
                    action == SnipWindow::FinishAction::Save
                        ? NextConfiguredScreenshotPath()
                        : ChooseSaveAsPath(hwnd_);

                if (path.empty()) {
                    DeleteObject(bitmap);
                    TrimWorkingSet();
                    return;
                }

                const bool saved = SaveBitmapPng(bitmap, path);
                DeleteObject(bitmap);
                if (saved) {
                    const std::wstring message = L"截图已保存：\n" + path.wstring();
                    ShowNotice(message.c_str());
                } else {
                    ShowNotice(L"截图保存失败");
                }
                TrimWorkingSet();
                return;
            }

            const bool pinAfter = action == SnipWindow::FinishAction::Pin;
            CommitCapture(bitmap);
            if (pinAfter) {
                PinClipboard();
            }
        });

    if (!started) {
        ShowNotice(L"无法启动截图");
        return;
    }

    AnimeToolbar::ShowForSnip(instance_);
}

void App::CaptureFullscreen() {
    HBITMAP bitmap = CaptureVirtualScreen();
    if (!bitmap) {
        ShowNotice(L"全屏截图失败");
        return;
    }
    CommitCapture(bitmap);
}

void App::PinClipboard() {
    if (!PinWindow::CreateFromClipboard(instance_)) {
        ShowNotice(L"剪贴板中没有可贴出的图片，支持截图、PNG/DIB 图片和复制的图片文件");
    }
}

void App::CommitCapture(HBITMAP bitmap) {
    if (!bitmap) {
        ShowNotice(L"截图失败");
        return;
    }

    const auto path = NextConfiguredScreenshotPath();
    const bool saved = SaveBitmapPng(bitmap, path);
    const bool copied = CopyBitmapToClipboard(
        hwnd_,
        bitmap,
        saved ? path : std::filesystem::path{});
    DeleteObject(bitmap);

    if (copied && saved) {
        const std::wstring message = L"已复制，可直接粘贴到支持图片的 CLI：\n" + path.wstring();
        ShowNotice(message.c_str());
    } else if (copied) {
        ShowNotice(L"已复制到图片剪贴板");
    } else if (saved) {
        ShowNotice(L"截图已保存");
    } else {
        ShowNotice(L"截图处理失败");
    }

    TrimWorkingSet();
}

LRESULT App::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_HOTKEY:
        if (wParam == HOTKEY_SNIP) {
            StartSnip();
        } else if (wParam == HOTKEY_PIN || wParam == HOTKEY_PIN_FALLBACK) {
            PinClipboard();
        } else if (wParam == HOTKEY_FULLSCREEN) {
            CaptureFullscreen();
        }
        return 0;

    case WM_TRAYICON:
        if (lParam == WM_LBUTTONUP) {
            StartSnip();
        } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
            ShowTrayMenu();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case CMD_SNIP:
            StartSnip();
            return 0;
        case CMD_PIN:
            PinClipboard();
            return 0;
        case CMD_FULLSCREEN:
            CaptureFullscreen();
            return 0;
        case CMD_OPEN_FOLDER:
            ShellExecuteW(
                hwnd_,
                L"open",
                ConfiguredScreenshotDirectory().c_str(),
                nullptr,
                nullptr,
                SW_SHOWNORMAL);
            return 0;
        case CMD_SET_FOLDER: {
            const auto directory = ChooseScreenshotFolder(hwnd_);
            if (!directory.empty()) {
                if (SetScreenshotDirectory(directory)) {
                    const std::wstring message = L"默认截图目录已设置为：\n" + directory.wstring();
                    ShowNotice(message.c_str());
                } else {
                    ShowNotice(L"设置截图目录失败");
                }
            }
            return 0;
        }
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
            DestroyWindow(hwnd_);
            return 0;
        default:
            break;
        }
        break;

    case WM_DESTROY:
        hwnd_ = nullptr;
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

LRESULT CALLBACK App::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    App* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<App*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    return self ? self->HandleMessage(message, wParam, lParam)
                : DefWindowProcW(hwnd, message, wParam, lParam);
}

int App::Run(int) {
    if (!Initialize()) {
        return 0;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

}  // namespace snaplite
