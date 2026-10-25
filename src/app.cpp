#include "app.h"

#include "anime_toolbar.h"
#include "capture.h"
#include "delay_overlay.h"
#include "ocr.h"
#include "pin_window.h"
#include "resource.h"
#include "snip_window.h"
#include "update_version.h"
#include "update_notes_dialog.h"
#include "update_status_toast.h"

#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <cwchar>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace snaplite {
namespace {
constexpr wchar_t kMainClass[] = L"SnapLiteMainWindow";
constexpr wchar_t kAppName[] = L"Snap-Lite";
constexpr UINT WM_TRAYICON = WM_APP + 1;
// 第二次启动时通知已在运行的实例，而不是静默退出。
constexpr UINT WM_SNAPLITE_SHOW_EXISTING = WM_APP + 2;
// 延时截图的倒计时结束。
constexpr UINT WM_SNAPLITE_DELAY_ELAPSED = WM_APP + 3;
constexpr UINT WM_SNAPLITE_UPDATE_CHECKED = WM_APP + 4;
constexpr UINT WM_SNAPLITE_UPDATE_READY = WM_APP + 5;

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
constexpr UINT CMD_SHORTCUTS = 1008;
constexpr UINT CMD_DELAY_3 = 1009;
constexpr UINT CMD_DELAY_5 = 1010;
constexpr UINT CMD_DELAY_10 = 1011;
constexpr UINT CMD_AUTO_UPDATE = 1012;

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"Snap-Lite";

// 读回进程**实际**的 DPI 感知状态。
//
// 三个设置 API 全都失败时，不等于"DPI 感知没开"：exe 的清单里可能已经
// 声明了 dpiAwareness（PerMonitorV2 之类）。那种情况下
// SetProcessDpiAwarenessContext 会以 ERROR_ACCESS_DENIED 失败——清单的
// 设置已经生效、不能再改，但感知是**开着**的。
//
// 这时如果照着"三个 API 都失败"报"未能开启 DPI 感知"，用户会看到一句
// 完全不成立的警告（坐标其实是对的）。
bool IsDpiAwarenessEffective() {
    const DPI_AWARENESS_CONTEXT context = GetThreadDpiAwarenessContext();
    // 感知开着：坐标不会被系统虚拟化（只是缩放不随显示器变化）。
    if (context == DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) return true;
    if (context == DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE) return true;
    if (context == DPI_AWARENESS_CONTEXT_SYSTEM_AWARE) return true;
    return false;
}

// 打开进程级 DPI 感知。返回 false 表示所有方式都失败——进程会被系统
// 虚拟化，选区、命中半径、贴图尺寸会整体错位，而且完全静默（用户只
//会觉得"鼠标点不准"，没有任何线索指向 DPI 感知）。
//
// 逐级回退：PMv2 → PM aware → 进程级 SetProcessDPIAware → 读回实际状态。
bool EnablePerMonitorDpiAwareness() {
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        return true;
    }
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)) {
        return true;
    }
    if (SetProcessDPIAware() != FALSE) {
        return true;
    }
    // 三个 API 都失败：可能清单已经声明了感知（见 IsDpiAwarenessEffective）。
    return IsDpiAwarenessEffective();
}

HICON LoadAppIcon(HINSTANCE instance) {
    HICON icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SNAPLITE));
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

// 从 "SnapLite-OCR2-<pid>-<tick>-<counter>.png" 里取出归属进程的 PID。
// 名字不是这个格式就返回 0（不是我们产生的文件，不动）。
DWORD ParseOcrTempFilePid(const wchar_t* fileName) {
    static const wchar_t kPrefix[] = L"SnapLite-OCR2-";
    const size_t prefixLength = wcslen(kPrefix);
    if (!fileName || wcsncmp(fileName, kPrefix, prefixLength) != 0) return 0;

    const wchar_t* cursor = fileName + prefixLength;
    if (*cursor < L'0' || *cursor > L'9') return 0;
    // PID 位数必须封顶：DWORD 上限 4294967295（10 位）。不封顶时
    // "99999999999999" 会在 DWORD 里**静默回绕**成另一个 PID，
    // 于是"这是谁的临时文件"会被判错：回绕到 0 时文件被当成别人的留下，
    // 回绕到某个还活着的 PID 时则可能误删别人正在用的文件。
    unsigned long long pid = 0;
    while (*cursor >= L'0' && *cursor <= L'9') {
        pid = pid * 10 + static_cast<unsigned long long>(*cursor - L'0');
        ++cursor;
        if (pid > 0xFFFFFFFFULL) return 0;
    }
    if (*cursor != L'-') return 0;
    return static_cast<DWORD>(pid);
}

// 进程状态的三态判定。
enum class ProcessState {
    kAlive,    // 进程还在
    kDead,     // 进程确实已经不在了
    kUnknown,  // 判不出来（宁可留下文件，也不要误删别人的）
};

// 当前所有还活着的进程 PID（**一次**快照）。
//
// 以前是**每个文件**做一次全进程快照：文件一多就是 N 次（系统里几百个
// 进程时每次都要把整张进程表枚举一遍），退出明显变慢。现在一次就够。
//
// reliable=false 表示快照没拿到——这时**一律不删**别人的文件：
// "判不出来"不等于"已经死了"。
struct AliveProcessSnapshot {
    std::set<DWORD> ids;
    bool reliable{false};
};

AliveProcessSnapshot SnapshotAliveProcessIds() {
    AliveProcessSnapshot result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        result.reliable = true;
        do {
            result.ids.insert(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

// 判断 PID 对应的进程是否还存在。
//
// 不用 OpenProcess：它对受保护进程（System、杀毒服务等）会因为
// ERROR_ACCESS_DENIED 失败，而那**不是**"进程没了"。把它一律当成"已死"，
// 就会把还活着的进程正在用的临时 PNG 删掉，那次识别直接失败。
// 进程快照枚举能直接回答"这个 PID 在不在"，没有访问权限这层歧义。
ProcessState QueryProcessState(DWORD pid, const AliveProcessSnapshot& snapshot) {
    if (pid == 0) return ProcessState::kUnknown;
    if (!snapshot.reliable) return ProcessState::kUnknown;
    return snapshot.ids.count(pid) ? ProcessState::kAlive : ProcessState::kDead;
}

// OCR 识别会把预处理后的图像落盘到临时目录。进程退出时若识别还在后台跑，
// 这些文件会残留（识别线程来不及清理）。退出时清一遍，避免临时目录堆积。
//
// 但不能一把全删：单实例互斥量是 Local\（按会话），同一用户开两个会话时
// 可以同时存在两个"主实例"，其中一个退出会把另一个**正在识别**用的 PNG
// 删掉，那次识别就失败了。文件名里带着 PID，按 PID 判定归属：
//   - 属于自己这个进程的 → 删（本次运行留下的残留）；
//   - 属于已经不存在的进程的 → 删（上次崩溃留下的垃圾）；
//   - 属于还活着的别的进程的 → 不动（它可能正在识别）。
void CleanupLeftoverOcrImages() {
    wchar_t tempDirectory[MAX_PATH + 1]{};
    const DWORD length = GetTempPathW(MAX_PATH, tempDirectory);
    if (length == 0 || length > MAX_PATH) return;

    // 先把文件**全部**列出来，再取那一次进程快照。
    //
    // 顺序有讲究：快照必须在枚举**之后**。反过来会出现竞态——新实例在
    // 快照之后建了文件并开始识别，它的文件会被当成"主人已经死了"而删掉，
    // 那次识别直接失败。枚举在前则新文件根本不在候选里，碰不到。
    struct Candidate {
        std::wstring path;
        DWORD ownerPid;
    };
    std::vector<Candidate> candidates;

    WIN32_FIND_DATAW findData{};
    const std::wstring pattern = std::wstring(tempDirectory) + L"SnapLite-OCR2-*.png";
    HANDLE find = FindFirstFileW(pattern.c_str(), &findData);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const DWORD ownerPid = ParseOcrTempFilePid(findData.cFileName);
        if (ownerPid == 0) continue;
        candidates.push_back(
            Candidate{std::wstring(tempDirectory) + findData.cFileName, ownerPid});
    } while (FindNextFileW(find, &findData));
    FindClose(find);

    if (candidates.empty()) return;

    const AliveProcessSnapshot alive = SnapshotAliveProcessIds();
    const DWORD selfPid = GetCurrentProcessId();
    for (const Candidate& candidate : candidates) {
        if (candidate.ownerPid != selfPid) {
            // 别人的文件：只有**确认**进程已经不在了才删。
            // 判不出来就留着——多留一个临时文件只是垃圾，
            // 误删别人正在用的文件会直接让那次 OCR 识别失败。
            if (QueryProcessState(candidate.ownerPid, alive) != ProcessState::kDead) {
                continue;
            }
        }
        DeleteFileW(candidate.path.c_str());
    }
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
    // 必须在创建任何窗口之前设置，否则系统会按虚拟化 DPI 处理它们。
    dpiAware_ = EnablePerMonitorDpiAwareness();

    singleInstance_ = CreateMutexW(nullptr, TRUE, L"Local\\SnapLiteSingleInstance");
    if (!singleInstance_) {
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // 已有实例在运行：通知它给用户一个提示，而不是静默退出让用户以为
        // 程序没反应。
        if (HWND existing = FindWindowW(kMainClass, nullptr)) {
            PostMessageW(existing, WM_SNAPLITE_SHOW_EXISTING, 0, 0);
        }
        CloseHandle(singleInstance_);
        singleInstance_ = nullptr;
        return false;
    }
    // 抢到互斥量的才是主实例。只有主实例退出时才允许清理 OCR 临时文件。
    primaryInstance_ = true;

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
    snipHotkeyRegistered_ = snipHotkeyRegistered;
    fullscreenHotkeyRegistered_ = fullscreenHotkeyRegistered;

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
    if (!dpiAware_) {
        // 非阻塞提示：无人值守启动时不能弹模态框把程序卡住。
        ShowNotice(L"未能开启 DPI 感知，窗口坐标可能被系统缩放，截图选区可能偏移");
    }

    const std::wstring cachedVersion = CachedLatestVersion();
    const auto cachedComparison = CompareReleaseVersions(cachedVersion, SNAP_LITE_VERSION);
    if (cachedComparison && *cachedComparison > 0) {
        updateAvailableVersion_ = cachedVersion;
    }
    StartUpdateCheck(false);

    TrimWorkingSet();
    return true;
}

void App::Shutdown() {
    if (updateWorker_.joinable()) {
        updateWorker_.join();
    }
    CloseUpdateStatusToast(hwnd_);

    // 退出时先把倒计时覆盖层收掉：它是顶层窗口，不销毁的话会在进程真正
    // 结束前留在屏幕上闪一下。
    DelayOverlay::Cancel();

    if (hwnd_) {
        UnregisterHotKey(hwnd_, HOTKEY_SNIP);
        UnregisterHotKey(hwnd_, HOTKEY_PIN);
        UnregisterHotKey(hwnd_, HOTKEY_FULLSCREEN);
        UnregisterHotKey(hwnd_, HOTKEY_PIN_FALLBACK);
    }
    pinHotkeyRegistered_ = false;
    pinFallbackHotkeyRegistered_ = false;

    // 退出时可能还有 OCR 识别在后台跑，它来不及删掉自己落盘的临时 PNG。
    // 这里顺手清一遍，避免临时目录越积越多。
    //
    // 只有主实例能清：第二个实例退出时去删，会把主实例正在识别用的临时
    // PNG 删掉，主实例那次识别就会失败。
    if (primaryInstance_) {
        CleanupLeftoverOcrImages();
    }

    // 还没被处理的识别结果也要回收：完成消息是 PostMessage 过去的，
    // 进程在消息被处理之前退出，那份结果就泄漏在内存里了。
    ReleasePendingOcrCompletions();

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

void App::ShowShortcutHelp() {
    std::wstring text;
    text += L"全局快捷键\n";
    text += snipHotkeyRegistered_
        ? L"  F1             截图\n"
        : L"  F1             截图（注册失败，可能被其他程序占用）\n";
    text += fullscreenHotkeyRegistered_
        ? L"  Ctrl+Shift+F   全屏截图\n"
        : L"  Ctrl+Shift+F   全屏截图（注册失败，可能被其他程序占用）\n";
    if (pinFallbackHotkeyRegistered_) {
        text += L"  Ctrl+Alt+P     贴图（F3 被占用，已自动切换）\n";
    } else if (pinHotkeyRegistered_) {
        text += L"  F3             贴图\n";
    } else {
        text += L"  F3             贴图（注册失败，请用托盘菜单）\n";
    }

    text += L"\n选区阶段\n";
    text += L"  单击 / 拖动    选择窗口控件或自由框选\n";
    text += L"  C              取色（复制 HEX）\n";
    text += L"  Enter          完成并复制\n";
    text += L"  Esc            取消\n";

    text += L"\n标注阶段\n";
    text += L"  R A P E M T    形状 / 箭头 / 画笔 / 橡皮 / 马赛克 / 文字\n";
    text += L"  Ctrl+Z / Y     撤销 / 重做\n";
    text += L"  Ctrl+D         复制选中的标注\n";
    text += L"  Tab            切换选中的标注（Shift+Tab 反向）\n";
    text += L"  Ctrl+Home/End  置于最上层 / 最下层\n";
    text += L"  Ctrl+PgUp/PgDn 上移一层 / 下移一层\n";
    text += L"  方向键         微调 1 像素（Shift 为 10 像素）\n";
    text += L"  Delete         删除选中的标注\n";

    MessageBoxW(hwnd_, text.c_str(), L"Snap-Lite · 快捷键", MB_OK | MB_ICONINFORMATION);
}

void App::StartUpdateCheck(bool manual) {
    if (updateCheckInProgress_) {
        if (manual) {
            updateCheckWasManual_ = true;
            ShowUpdateStatusToast(
                hwnd_, UpdateStatusToastState::Checking, SNAP_LITE_VERSION);
        }
        return;
    }
    if (updateDownloadInProgress_) {
        if (manual) {
            ShowUpdateStatusToast(
                hwnd_, UpdateStatusToastState::Downloading, SNAP_LITE_VERSION,
                updateAvailableVersion_.value_or(L""));
        }
        return;
    }
    if (!manual && !IsUpdateCheckDueToday()) return;
    if (updateWorker_.joinable()) updateWorker_.join();

    RecordUpdateCheckToday();
    updateCheckInProgress_ = true;
    updateCheckWasManual_ = manual;
    const HWND target = hwnd_;
    try {
        updateWorker_ = std::thread([this, target] {
            UpdateCheckResult result = CheckLatestRelease();
            {
                std::lock_guard lock(updateMutex_);
                pendingUpdateCheck_ = std::move(result);
            }
            if (!PostMessageW(target, WM_SNAPLITE_UPDATE_CHECKED, 0, 0)) {
                std::lock_guard lock(updateMutex_);
                pendingUpdateCheck_.reset();
            }
        });
    } catch (...) {
        updateCheckInProgress_ = false;
        if (manual) {
            ShowUpdateStatusToast(
                hwnd_, UpdateStatusToastState::CheckFailed, SNAP_LITE_VERSION,
                {}, L"无法启动更新检查，请稍后重试。");
        }
        return;
    }
    if (manual) {
        ShowUpdateStatusToast(
            hwnd_, UpdateStatusToastState::Checking, SNAP_LITE_VERSION);
    }
}

void App::StartUpdateDownload() {
    if (updateDownloadInProgress_) {
        ShowUpdateStatusToast(
            hwnd_, UpdateStatusToastState::Downloading, SNAP_LITE_VERSION,
            updateAvailableVersion_.value_or(L""));
        return;
    }
    if (updateCheckInProgress_) {
        ShowUpdateStatusToast(
            hwnd_, UpdateStatusToastState::Checking, SNAP_LITE_VERSION);
        return;
    }
    if (!updateAvailableVersion_) {
        StartUpdateCheck(true);
        return;
    }
    if (updateWorker_.joinable()) updateWorker_.join();

    updateDownloadInProgress_ = true;
    const std::wstring version = *updateAvailableVersion_;
    const HWND target = hwnd_;
    ShowUpdateStatusToast(
        hwnd_, UpdateStatusToastState::Downloading, SNAP_LITE_VERSION, version);
    try {
        updateWorker_ = std::thread([this, target, version] {
            UpdateDownloadResult result = DownloadVerifiedUpdate(version);
            {
                std::lock_guard lock(updateMutex_);
                pendingUpdateDownload_ = std::move(result);
            }
            if (!PostMessageW(target, WM_SNAPLITE_UPDATE_READY, 0, 0)) {
                std::lock_guard lock(updateMutex_);
                pendingUpdateDownload_.reset();
            }
        });
    } catch (...) {
        updateDownloadInProgress_ = false;
        ShowUpdateStatusToast(
            hwnd_, UpdateStatusToastState::DownloadFailed, SNAP_LITE_VERSION,
            version, L"无法启动更新下载，请稍后重试。");
    }
}

bool App::LaunchUpdateInstaller(const std::filesystem::path& installerPath) {
    std::vector<wchar_t> executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) return false;
    const std::filesystem::path installDirectory =
        std::filesystem::path(executable.data()).parent_path();
    const std::wstring parameters =
        L"/SP- /SILENT /NORESTART /DIR=\"" + installDirectory.wstring() + L"\"";

    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOCLOSEPROCESS;
    launch.hwnd = hwnd_;
    launch.lpVerb = L"runas";
    launch.lpFile = installerPath.c_str();
    launch.lpParameters = parameters.c_str();
    launch.lpDirectory = installDirectory.c_str();
    launch.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&launch)) {
        if (GetLastError() == ERROR_CANCELLED) {
            ShowNotice(L"已取消管理员授权，更新没有安装");
            return true;
        }
        return false;
    }
    if (launch.hProcess) CloseHandle(launch.hProcess);

    updateAvailableVersion_.reset();
    DestroyWindow(hwnd_);
    return true;
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
    HMENU delayMenu = CreatePopupMenu();
    if (delayMenu) {
        AppendMenuW(delayMenu, MF_STRING, CMD_DELAY_3, L"3 秒后截图");
        AppendMenuW(delayMenu, MF_STRING, CMD_DELAY_5, L"5 秒后截图");
        AppendMenuW(delayMenu, MF_STRING, CMD_DELAY_10, L"10 秒后截图");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(delayMenu), L"延时截图");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_OPEN_FOLDER, L"打开截图目录");
    AppendMenuW(menu, MF_STRING, CMD_SET_FOLDER, L"设置默认保存目录...");
    AppendMenuW(menu, MF_STRING, CMD_SHORTCUTS, L"快捷键说明...");
    std::wstring updateLabel = L"自动更新\t当前版本 v";
    updateLabel += SNAP_LITE_VERSION;
    if (updateCheckInProgress_) {
        updateLabel += L" · 检查中";
    } else if (updateDownloadInProgress_) {
        updateLabel += L" · 下载中";
    } else if (updateAvailableVersion_) {
        updateLabel += L" · 有新版本";
    }
    AppendMenuW(menu, MF_STRING, CMD_AUTO_UPDATE, updateLabel.c_str());
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
    if (delayMenu) DestroyMenu(delayMenu);
    DestroyMenu(menu);
}

void App::StartSnipDelayed(int seconds) {
    // 延时截图用来抓那些"刚出现、马上又消失"的界面（右键菜单、吐司提示、
    // 悬停展开的卡片）。倒计时覆盖层显示剩余秒数，点击或 Esc 可取消。
    if (FindWindowW(L"SnapLiteSnipWindow", nullptr)) {
        // 已经有一个截图窗口开着（用户正在标注）。到点时新截图无处可去——
        // 截图窗口是独占的，到点只会被"已有窗口就返回"的早退吃掉，变成
        // 什么都没发生。这里直接说清楚，不要让用户对着空气等。
        ShowNotice(L"当前还有一个截图窗口没结束，先完成或取消它再使用延时截图");
        return;
    }
    if (!DelayOverlay::Start(instance_, seconds, hwnd_, WM_SNAPLITE_DELAY_ELAPSED)) {
        // 覆盖层创建失败时直接开始截图，绝不能让用户面对"点了没反应"。
        StartSnip();
    }
}

void App::StartSnip() {
    // 延时倒计时进行中，用户又按了 F1 / 点了托盘图标 / 选了托盘菜单的
    // 「截图」：先把倒计时取消掉。倒计时覆盖层不是分层窗口，屏幕抓取一定
    // 会把它拍进去——不取消的话，这张截图顶部中央会多一个倒计时数字框。
    //
    // 放在"已有截图窗口就提前返回"的早退之前：否则倒计时会继续浮在标注
    // 窗口上，到点之后又因为早退而无声无息。
    DelayOverlay::Cancel();

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
                    const std::wstring notice = L"截图已保存：\n" + path.wstring();
                    ShowNotice(notice.c_str());
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
    // 全屏截图抓的是整块屏幕，延时倒计时覆盖层同样会被拍进去，先取消。
    DelayOverlay::Cancel();
    HBITMAP bitmap = CaptureVirtualScreen();
    if (!bitmap) {
        ShowNotice(L"全屏截图失败");
        return;
    }
    CommitCapture(bitmap);
}

void App::PinClipboard() {
    // 按细分结果给不同提示。"剪贴板被占用"和"没有图片"的处理方式完全不同，
    // 混成一句话会让用户以为自己没复制好。
    switch (PinWindow::CreateFromClipboard(instance_)) {
    case PinClipboardResult::Ok:
        break;
    case PinClipboardResult::Dismissed:
        ShowNotice(L"这张图片刚刚已经贴过并关闭了，换一张或重新复制后再按贴图");
        break;
    case PinClipboardResult::NoImage:
        ShowNotice(L"剪贴板中没有可贴出的图片，支持截图、PNG/DIB 图片和复制的图片文件");
        break;
    case PinClipboardResult::ClipboardBusy:
        ShowNotice(L"剪贴板正被其它程序占用，等一下再按贴图");
        break;
    default:
        ShowNotice(L"贴图失败");
        break;
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
        const std::wstring notice = L"已复制，可直接粘贴到支持图片的 CLI：\n" + path.wstring();
        ShowNotice(notice.c_str());
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
                    const std::wstring folderNotice = L"默认截图目录已设置为：\n" + directory.wstring();
                    ShowNotice(folderNotice.c_str());
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
        case CMD_SHORTCUTS:
            ShowShortcutHelp();
            return 0;
        case CMD_AUTO_UPDATE:
            StartUpdateCheck(true);
            return 0;
        case CMD_DELAY_3:
            StartSnipDelayed(3);
            return 0;
        case CMD_DELAY_5:
            StartSnipDelayed(5);
            return 0;
        case CMD_DELAY_10:
            StartSnipDelayed(10);
            return 0;
        case CMD_EXIT:
            DestroyWindow(hwnd_);
            return 0;
        default:
            break;
        }
        break;

    case WM_SNAPLITE_UPDATE_TOAST_ACTION:
        if (static_cast<UpdateStatusToastAction>(wParam) == UpdateStatusToastAction::Check) {
            StartUpdateCheck(true);
        } else if (static_cast<UpdateStatusToastAction>(wParam) == UpdateStatusToastAction::Download) {
            StartUpdateDownload();
        }
        return 0;

    case WM_SNAPLITE_SHOW_EXISTING:
        // 用户第二次启动了本程序：明确告诉他已经在运行，而不是静默退出。
        ShowNotice(L"Snap-Lite 已经在运行，托盘图标可以找到它");
        return 0;

    case WM_SNAPLITE_DELAY_ELAPSED:
        StartSnip();
        return 0;

    case WM_SNAPLITE_UPDATE_CHECKED: {
        if (updateWorker_.joinable()) updateWorker_.join();
        std::optional<UpdateCheckResult> result;
        {
            std::lock_guard lock(updateMutex_);
            result = std::move(pendingUpdateCheck_);
            pendingUpdateCheck_.reset();
        }
        updateCheckInProgress_ = false;
        if (!result || !result->succeeded) {
            if (updateCheckWasManual_) {
                const std::wstring error = result && !result->error.empty()
                    ? result->error
                    : L"检查更新失败，请稍后重试。";
                ShowUpdateStatusToast(
                    hwnd_, UpdateStatusToastState::CheckFailed, SNAP_LITE_VERSION,
                    {}, error);
            }
            return 0;
        }
        CacheLatestVersion(result->latestVersion);
        const auto comparison = CompareReleaseVersions(result->latestVersion, SNAP_LITE_VERSION);
        if (!comparison) {
            if (updateCheckWasManual_) {
                ShowUpdateStatusToast(
                    hwnd_, UpdateStatusToastState::CheckFailed, SNAP_LITE_VERSION,
                    {}, result->error.empty()
                            ? L"GitHub 返回的版本号无法识别。"
                            : result->error);
            }
            return 0;
        }
        if (*comparison > 0) {
            updateAvailableVersion_ = result->latestVersion;
            if (updateCheckWasManual_) {
                ShowUpdateStatusToast(
                    hwnd_, UpdateStatusToastState::Available, SNAP_LITE_VERSION,
                    result->latestVersion);
            } else {
                const std::wstring notice =
                    L"发现新版本 v" + result->latestVersion + L"，右键托盘图标可自动更新";
                ShowNotice(notice.c_str());
            }
        } else {
            updateAvailableVersion_.reset();
            if (updateCheckWasManual_) {
                ShowUpdateStatusToast(
                    hwnd_, UpdateStatusToastState::UpToDate, SNAP_LITE_VERSION,
                    result->latestVersion);
            }
        }
        return 0;
    }

    case WM_SNAPLITE_UPDATE_READY: {
        if (updateWorker_.joinable()) updateWorker_.join();
        std::optional<UpdateDownloadResult> result;
        {
            std::lock_guard lock(updateMutex_);
            result = std::move(pendingUpdateDownload_);
            pendingUpdateDownload_.reset();
        }
        updateDownloadInProgress_ = false;
        if (!result || !result->succeeded) {
            const std::wstring error = result && !result->error.empty()
                ? result->error
                : L"更新包准备失败，请稍后重试。";
            ShowUpdateStatusToast(
                hwnd_, UpdateStatusToastState::DownloadFailed, SNAP_LITE_VERSION,
                updateAvailableVersion_.value_or(L""), error);
            return 0;
        }

        CloseUpdateStatusToast(hwnd_);
        const UpdateNotesDialogChoice choice = ShowUpdateNotesDialog(
            hwnd_, *updateAvailableVersion_, result->releaseNotes);
        if (choice == UpdateNotesDialogChoice::Install &&
            !LaunchUpdateInstaller(result->installerPath)) {
            ShowNotice(L"无法启动更新安装程序");
        } else if (choice != UpdateNotesDialogChoice::Install) {
            std::error_code ignored;
            std::filesystem::remove(result->installerPath, ignored);
            if (choice == UpdateNotesDialogChoice::Failed) {
                ShowNotice(L"无法显示更新说明，请重试更新");
            }
        }
        return 0;
    }

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
