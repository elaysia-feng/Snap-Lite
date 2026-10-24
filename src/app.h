#pragma once

#include <windows.h>
#include <shellapi.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

#include "update_checker.h"

#include <mutex>
#include <optional>
#include <thread>

namespace snaplite {

class App {
public:
    explicit App(HINSTANCE instance);
    ~App();

    int Run(int showCommand);

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool Initialize();
    void Shutdown();
    void AddTrayIcon();
    void ShowTrayMenu();
    void ShowNotice(const wchar_t* message);
    void ShowShortcutHelp();
    void StartUpdateCheck(bool manual);
    void StartUpdateDownload();
    bool LaunchUpdateInstaller(const std::filesystem::path& installerPath);
    void StartSnip();
    void StartSnipDelayed(int seconds);
    void CaptureFullscreen();
    void PinClipboard();
    void CommitCapture(HBITMAP bitmap);

    HINSTANCE instance_{};
    HWND hwnd_{};
    NOTIFYICONDATAW tray_{};
    ULONG_PTR gdiplusToken_{};
    HANDLE singleInstance_{};
    // 是否是抢到单实例互斥量的那个进程。只有它退出时才清理 OCR 临时文件：
    // 第二个实例退出时去清理，会把主实例正在识别用的临时 PNG 删掉，
    // 导致主实例那次识别失败。
    bool primaryInstance_{false};
    bool pinHotkeyRegistered_{};
    bool pinFallbackHotkeyRegistered_{};
    bool snipHotkeyRegistered_{};
    bool fullscreenHotkeyRegistered_{};
    std::thread updateWorker_;
    std::mutex updateMutex_;
    std::optional<UpdateCheckResult> pendingUpdateCheck_;
    std::optional<UpdateDownloadResult> pendingUpdateDownload_;
    std::optional<std::wstring> updateAvailableVersion_;
    bool updateCheckInProgress_{};
    bool updateDownloadInProgress_{};
    bool updateCheckWasManual_{};
    // 是否成功开启了进程级 DPI 感知。失败时窗口会被系统虚拟化，坐标
    // 整体错位，必须明确告知用户而不是静默下去。
    bool dpiAware_{false};
};

}  // namespace snaplite
