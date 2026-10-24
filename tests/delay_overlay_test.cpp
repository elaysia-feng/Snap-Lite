// 延时截图覆盖层的运行时回归测试。
//
// 这个文件以前完全没有运行时覆盖：延时覆盖层与 app.cpp 的行为改动只有
// 源码级静态守卫，而"倒计时归零后截图里会不会残留覆盖层"这种问题，
// 静态断言断不了——它取决于桌面重绘的真实时序。
//
// 这里把那条链做成可执行断言，分成三段：
//
//   A. 窗口生命周期（与可见桌面无关，到处都能跑）
//      1. Cancel() 必须同步销毁窗口（返回后立刻就查不到窗口）；
//      2. 重复启动倒计时不能留下两个覆盖层；
//      3. 没有覆盖层时 Cancel() 不能崩、也不该白跑一次"等重绘"。
//
//   B. 等待逻辑本身（需要可抓屏的交互式桌面）
//      4. 对着一块永远退不干净的区域问产品"退干净了吗"，它必须说
//         "没有"，而且必须等满超时才回答——等待逻辑要是被摘掉
//         （直接返回 true），这里当场就红。
//
//   C. 产品路径（需要可抓屏的交互式桌面）
//      5. 基线必须**真的采自屏幕**：事先在那块区域贴一块与覆盖层同色的
//         补丁，产品报出来的基线必须把这块补丁算进去。写死 0、
//         或者基线没采到却报 0，都会被这里抓住。
//      6. 产品必须**真的等过**：让那块区域永远退不干净（盖一块自己
//         的遮挡窗口），产品就必须等满上限；它压根不等、或者把上限
//         改成 0，这里当场就红。
//
// B 段验的是"等待实现这个函数的性质"，C 段验的是"产品用了它"。
// 两者缺一不可：只验 B 的话，把产品路径里的等待整段摘掉，B 段照样全绿
// （上一轮验收就是这样判 FAIL 的）；只验 C 的话，等待实现内部的像素
// 判定被改坏就没人发现。
//
// 判据为什么用"基线"而不是"比值"：
// 桌面上可能留着上一次运行没被重绘掉的覆盖层像素（实测见过 14099 个同色
// 像素留在屏幕上）。那种情况下"覆盖层可见时"和"抓屏那一刻"量到的数完全
// 相等，比值法会把环境问题误报成产品缺陷。所以先把那块区域擦干净、记下
// 基线，再用"是不是回到了基线"来判断。擦不干净就跳过像素断言，只跑
// 窗口生命周期断言——环境问题不该算成产品缺陷。
//
// 覆盖层自绘的颜色**从产品里取**，不在测试里另抄一份：常量一旦分叉，
// 测试的判据就失效了，而"一直绿的失效判据"比没有判据更糟。
//
// 顺带说明"产品到底做了什么决定"是怎么问出来的：产品侧在
// SNAPLITE_DELAY_TEST_HOOKS 下暴露了一组只读计数器（等了几次、等了多久、
// 是不是等满了上限、是不是退回了"睡固定时长去赌"、催了几次重绘）。
// 那些符号**不进产品二进制**——产品目标不定义那个宏。

#include "delay_overlay.h"

#include "capture.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int checks = 0;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

// 跳过整个用例（约定退出码 77）。
struct SkipTest {
    std::string reason;
};

// 当前是不是"没有可见桌面"的运行环境。
//
// 可以用环境变量 SNAPLITE_DELAY_TEST_HEADLESS=1 强制走这条路径：
// 目的是在没有服务 window station 的机器上也能验证 CI 分支本身是通的
// （否则那条分支只能等 CI 跑一次才知道有没有问题）。它只能**减少**覆盖
// ——跳过像素断言——不能让一个坏掉的产品蒙混过关。
bool g_headless = false;

// 这一趟跳过了哪些断言。
//
// 跳过不是"通过"：前提不满足时，被跳过的那些断言等于没有跑。把它们
// 记下来，结尾按**跳过**报（退出码 77），而不是报"PASS"——
// 否则一个彻底坏掉的产品在 CI 上也能拿到"PASS"（上一轮验收实测：
// 等待逻辑整个坏掉、销毁后不催重绘，HEADLESS 下照样 0 退出）。
std::vector<std::string> g_skipped;

void RecordSkip(const std::string& what) { g_skipped.push_back(what); }

// 建不出窗口只能跳过，建出来了就必须严格断言。
//
// 这条区分很重要：服务 window station 里窗口照常能建，只是屏幕抓不到；
// 因此"建不出窗口"是环境问题（跳过），而"窗口建出来了却没被销毁"是
// 产品缺陷（必须红）。
void CheckOrSkip(bool condition, const char* message) {
    if (condition) {
        ++checks;
        return;
    }
    if (g_headless) {
        throw SkipTest{message};
    }
    throw std::runtime_error(message);
}

// 失败时把 Win32 错误码带上，否则"窗口没建出来"只有一句没用的断言。
std::string LastErrorText(const char* what) {
    return std::string(what) + " (GetLastError=" + std::to_string(GetLastError()) + ")";
}

constexpr UINT WM_TEST_DELAY_ELAPSED = WM_APP + 77;
constexpr wchar_t kOverlayClass[] = L"SnapLiteDelayOverlay";
constexpr wchar_t kBlockerClass[] = L"SnapLiteDelayOverlayBlocker";

// 约定的跳过退出码。CMake 侧配了 SKIP_RETURN_CODE 77。
constexpr int kSkipExitCode = 77;

// 一块 148x104 的圆角面板，涂满这两种颜色大约一万四千像素。
// 超过这个量就说明这块区域"看起来还盖着一个覆盖层"。
constexpr long kOverlaySignature = 5000;
// 产品"等桌面重绘"的窗口至少要这么久。
//
// 这是**对产品的要求**：桌面重绘由 explorer 调度，窗口太短就来不及，
// 截图会把覆盖层的残影拍进去。产品把上限从 800ms 调到 500ms 是合法的
// （合法的调参不该让测试假红），但调到 1ms 就是回归。
constexpr DWORD kMinimumProductWaitTimeoutMs = 200;
// 催桌面重绘的最小间隔（毫秒）。
//
// 这是**对产品的要求**：桌面窗口横跨所有显示器，密集的重绘请求会被
// 放大成别的显示器跟着闪（用户实测："在桌面一测试居然能影响到我桌面
// 二"）。原来的实现是每 30ms 催一次，800ms 窗口内二十多次。
//
// 产品把间隔从 200ms 调到 300ms 是合法的（更安静，不假红）；调到
// 100ms 以下就是回归——又回到"一串重绘下去别的屏跟着闪"的状态。
constexpr DWORD kMinimumNudgeIntervalMs = 100;
// 清理残留时允许的误差（抗锯齿边缘）。
//
// 注意：真正的容忍度由**产品**按 DPI 算（高 DPI 下圆角更大、抗锯齿边缘
// 更长，照搬 96 DPI 的 64 会让合法结果被判红），所以运行时用
// DelayOverlay::LastResidueTolerance() 拿产品算出来的那个值，
// 这个常量只当"产品没报出来时的兜底"。

// 覆盖层自绘用的两个颜色，由产品提供（见 delay_overlay.h 的说明）。
COLORREF g_overlayBorder = RGB(0, 0, 0);
COLORREF g_overlaySurface = RGB(0, 0, 0);

// 残留容忍度：由产品按它所在显示器的 DPI 算出来。
//
// 测试不该自己写死这个数：高 DPI 下圆角更大、抗锯齿边缘更长，产品的
// 容忍度是 96 DPI 的 64 乘以缩放系数，测试若仍按 64 判，合法结果会被
// 判红（推演：150% DPI 上会假红）。
long g_residueTolerance = 64;

// 运行环境探测。
//
// GitHub 的 windows-latest 跑在会话 0 的服务 window station 上，那里没有
// 可抓屏的交互式桌面。这种环境下抓屏**不会失败**——返回的是一张黑图，
// 所以"抓不到就跳过"的保护没有用，像素数会是 0，断言照样红，
// 会把 PR 与发布一起挡住。必须先问清楚有没有可用的交互式桌面。
bool HasInteractiveDesktop() {
    HWINSTA station = GetProcessWindowStation();
    if (!station) return false;
    USEROBJECTFLAGS flags{};
    DWORD needed = 0;
    if (!GetUserObjectInformation(station, UOI_FLAGS, &flags, sizeof(flags), &needed)) {
        return false;
    }
    return (flags.dwFlags & WSF_VISIBLE) != 0;
}

// 环境变量 SNAPLITE_DELAY_TEST_HEADLESS 有没有被设置。
//
// 不能用 "GetEnvironmentVariableW(name, nullptr, 0) > 0" 判断：值是**空串**
// 时它返回 0（长度算 0），于是"显式要求走无头分支"这件事被悄悄忽略，
// 黑屏会话里直接撞上像素断言变红。正确做法是看 GetLastError()——
// 只要变量**存在**（哪怕值是空），它就不是 ERROR_ENVVAR_NOT_FOUND。
bool HeadlessForcedByEnvironment() {
    SetLastError(ERROR_SUCCESS);
    GetEnvironmentVariableW(L"SNAPLITE_DELAY_TEST_HEADLESS", nullptr, 0);
    return GetLastError() != ERROR_ENVVAR_NOT_FOUND;
}

// 采一块小画面，看它里面是不是"有东西"。
//
// 只看"API 有没有成功"是不够的：断开的 RDP、锁屏、某些服务 window
// station 里，BitBlt 会**成功**并返回一张全黑的图。0 像素既可能是
// "覆盖层没画出来"，也可能是"屏幕根本没抓到"——从数字上分不出来。
// 所以这里额外要求画面里有非黑像素，或者至少有不止一种颜色。
bool RegionHasContent(int left, int top, int width, int height) {
    HBITMAP probe = snaplite::CaptureRect(left, top, width, height);
    if (!probe) return false;
    std::vector<BYTE> pixels;
    const bool read = snaplite::ReadBitmapPixels(probe, width, height, pixels);
    DeleteObject(probe);
    if (!read) return false;
    if (pixels.size() < static_cast<std::size_t>(width) * height * 4) return false;

    bool sawContent = false;
    bool sawVariety = false;
    const BYTE firstR = pixels[0];
    const BYTE firstG = pixels[1];
    const BYTE firstB = pixels[2];
    for (std::size_t index = 0; index + 3 < pixels.size(); index += 4) {
        const BYTE* pixel = pixels.data() + index;
        if (pixel[0] > 8 || pixel[1] > 8 || pixel[2] > 8) {
            sawContent = true;
        }
        if (pixel[0] != firstR || pixel[1] != firstG || pixel[2] != firstB) {
            sawVariety = true;
        }
    }
    return sawContent || sawVariety;
}

// 这一块画面到底能不能抓到。
//
// "有可见桌面"不等于"抓得到画面"：服务 window station 里可能既标记着
// WSF_VISIBLE，又没有可抓屏的显示表面；抓到的还可能是一张全黑的图。
// 抓屏失败时量到的像素数永远是 0，而 0 和"覆盖层根本没画出来"从数字上
// 分不出来——当成缺陷就是误报，而且会把 CI 与发布一起挡住。所以像素断言
// 之前必须先探一次，而且要探**内容**：黑屏会话不能当成"能抓屏"。
bool CanCaptureScreen() {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int workWidth = work.right - work.left;
    const int workHeight = work.bottom - work.top;
    if (workWidth <= 0 || workHeight <= 0) return false;

    // 采样五个位置：整块工作区被一个纯黑窗口盖住时也能找到有内容的地方；
    // 整屏都黑（断连的 RDP / 锁屏）时五处全黑，才判定为"抓不到画面"。
    const int sample = 24;
    const int xs[3] = {work.left + 8,
                       work.left + workWidth / 2 - sample / 2,
                       work.right - sample - 8};
    const int ys[3] = {work.top + 8,
                       work.top + workHeight / 2 - sample / 2,
                       work.bottom - sample - 8};
    for (int yi = 0; yi < 3; ++yi) {
        for (int xi = 0; xi < 3; ++xi) {
            if (xi == 1 && yi == 1) continue;  // 中心只采一次
            if (RegionHasContent(xs[xi], ys[yi], sample, sample)) {
                return true;
            }
        }
    }
    return false;
}

// 一个覆盖层窗口，连同它属于哪个进程。
struct OverlayWindow {
    HWND hwnd{nullptr};
    DWORD pid{0};
    bool visible{false};
};

// 枚举全系统所有 class == SnapLiteDelayOverlay 的顶层窗口。
//
// 必须能区分"自己的覆盖层"和"别的进程举着的覆盖层"：两者位置、大小、
// 颜色完全一样。混在一起时，"取消后覆盖层还在不在"失败既可能是产品
// 的 bug，也可能是旁边恰好有个用户开着的延时截图——不区分就分不清。
std::vector<OverlayWindow> ListOverlayWindows() {
    std::vector<OverlayWindow> found;
    HWND current = nullptr;
    while ((current = FindWindowExW(nullptr, current, kOverlayClass, nullptr)) != nullptr) {
        OverlayWindow entry{};
        entry.hwnd = current;
        entry.visible = IsWindowVisible(current) != FALSE;
        DWORD pid = 0;
        GetWindowThreadProcessId(current, &pid);
        entry.pid = pid;
        found.push_back(entry);
    }
    return found;
}

// 本进程自己的覆盖层窗口。
//
// 别人的覆盖层不该算在账上：这台机器上完全可能有另一个 SnapLite 实例
// 正在倒计时（用户自己开着的、或者另一个测试进程正在跑），它的窗口
// 和本测试的覆盖层位置、大小、颜色一模一样。把它算进来，"只留下一个
// 覆盖层"这种断言就会在产品毫无问题的时候红。
std::vector<OverlayWindow> ListOurOverlayWindows() {
    std::vector<OverlayWindow> ours;
    for (const OverlayWindow& entry : ListOverlayWindows()) {
        if (entry.pid == GetCurrentProcessId()) {
            ours.push_back(entry);
        }
    }
    return ours;
}

// 本进程自己的那个覆盖层窗口。
//
// 不能用 FindWindowW 按类名找：它返回的是**第一个**同类窗口，别的进程
// 举着的覆盖层完全可能被它先选中。于是"我们的覆盖层"就变成了"别人的"，
// 后面的断言全部对着别人的窗口做。
HWND FindOurOverlayWindow() {
    for (const OverlayWindow& entry : ListOurOverlayWindows()) {
        return entry.hwnd;
    }
    return nullptr;
}

// 失败时把现场打印出来——否则"取消后窗口还在"这种问题根本无从下手。
std::string OverlayWindowSummary(HWND expected = nullptr) {
    std::string summary;
    if (expected) {
        summary += "expected=" +
                   std::to_string(reinterpret_cast<uintptr_t>(expected)) + " ";
    }
    for (const OverlayWindow& entry : ListOverlayWindows()) {
        RECT rect{};
        GetWindowRect(entry.hwnd, &rect);
        summary += " hwnd=" + std::to_string(reinterpret_cast<uintptr_t>(entry.hwnd));
        summary += " pid=" + std::to_string(entry.pid);
        summary += " rect=(" + std::to_string(rect.left) + "," +
                   std::to_string(rect.top) + ")-(" +
                   std::to_string(rect.right) + "," +
                   std::to_string(rect.bottom) + ")";
        summary += entry.visible ? " visible" : " hidden";
        summary += ";";
    }
    return summary.empty() ? std::string("(no overlay window)") : summary;
}

struct PixelCount {
    long border = 0;
    long surface = 0;

    long total() const { return border + surface; }
};

HWND g_notifyWindow = nullptr;
RECT g_overlayRect{};
PixelCount g_atNotification{};
bool g_notified = false;

PixelCount CountOverlayPixels(const RECT& region) {
    PixelCount result{};
    const int width = region.right - region.left;
    const int height = region.bottom - region.top;
    if (width <= 0 || height <= 0) {
        return result;
    }
    // 一次抓整块区域再批量读像素：逐像素调 ReadBitmapPixel 每次都要
    // 建一个临时 DC，扫这块区域要建一万五千次，测试会慢到没法用。
    HBITMAP screen = snaplite::CaptureRect(region.left, region.top, width, height);
    if (!screen) {
        return result;
    }
    std::vector<BYTE> pixels;
    const bool read = snaplite::ReadBitmapPixels(screen, width, height, pixels);
    DeleteObject(screen);
    if (!read) {
        return result;
    }
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
    for (int y = 0; y < height; ++y) {
        const BYTE* row = pixels.data() + static_cast<std::size_t>(y) * rowBytes;
        for (int x = 0; x < width; ++x) {
            const BYTE* pixel = row + static_cast<std::size_t>(x) * 4;
            const COLORREF colour = RGB(pixel[2], pixel[1], pixel[0]);
            if (colour == g_overlayBorder) {
                ++result.border;
            }
            if (colour == g_overlaySurface) {
                ++result.surface;
            }
        }
    }
    return result;
}

LRESULT CALLBACK NotifyProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_TEST_DELAY_ELAPSED) {
        // App 收到这条通知就会立刻抓屏，所以必须在这一刻统计覆盖层像素，
        // 才和真实行为对齐。
        g_atNotification = CountOverlayPixels(g_overlayRect);
        g_notified = true;
    }
    // wParam / lParam 必须原样传下去：WM_NCCREATE 的 lParam 是
    // CREATESTRUCT 指针，丢掉它 DefWindowProc 会返回 FALSE，窗口根本
    // 建不出来（而且不报任何错误码）。
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// 等到倒计时结束、通知送达。
//
// 通知是 PostMessage 过来的，必须跑消息泵才会被派发；测试主线程在这段
// 等待里还要处理覆盖层的 WM_TIMER（产品就是在那里销毁覆盖层并等重绘的）。
bool WaitForNotification(DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (!g_notified && GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(20);
    }
    return g_notified;
}

// 等到别的进程的覆盖层窗口消失（最多等 timeoutMs）。
//
// 上一轮测试进程如果还没退干净，它举着的覆盖层正好挡在本测试要量的
// 位置上——等一小会儿通常就够了；等不到就立刻说清楚是谁挡着。
bool WaitForForeignOverlays(DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        bool foreign = false;
        for (const OverlayWindow& entry : ListOverlayWindows()) {
            if (entry.pid != GetCurrentProcessId()) {
                foreign = true;
                break;
            }
        }
        if (!foreign) {
            return true;
        }
        if (GetTickCount64() >= deadline) {
            return false;
        }
        Sleep(50);
    }
}

// 等覆盖层的颜色真的出现在屏幕上。
//
// 覆盖层刚创建时，合成器未必已经把它合进屏幕画面：立刻抓一次很可能得到
// "一块什么都没有的区域"（实测在强制整屏重绘之后紧接着就抓，拿到的是 0）。
// 这里轮询到它出现为止。
PixelCount WaitForOverlayPixels(const RECT& region, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    PixelCount last{};
    for (;;) {
        last = CountOverlayPixels(region);
        if (last.total() > kOverlaySignature) {
            return last;
        }
        if (GetTickCount64() >= deadline) {
            return last;
        }
        Sleep(20);
    }
}

// 强制桌面重绘**目标区域**，再等那块区域里的覆盖层同色像素退下去。
//
// 上一次运行留下的覆盖层像素会让这个测试的结论随桌面历史翻转：同一个
// 二进制可能先 PASS 后 FAIL。测之前先把桌面擦干净；擦不干净就说明环境
// 本身有问题，后面的像素断言应当跳过，而不是误报成产品缺陷。
//
// 只重绘**这一块**，而且不擦背景：桌面窗口横跨所有显示器，对它做整块
// 或带擦除的重绘，别的显示器会跟着闪——用户在另一块屏上干活时会被闪到
// （实测踩过：这个测试在桌面一跑，桌面二跟着闪）。
bool TryCleanRegion(const RECT& region, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        if (CountOverlayPixels(region).total() < kOverlaySignature) {
            return true;
        }
        if (GetTickCount64() >= deadline) {
            return false;
        }
        // 只作废**这一块**，而且不擦背景：桌面窗口横跨所有显示器，对它做
        // 整块或带擦除的重绘，别的显示器会跟着闪——用户在另一块屏上干活时
        // 会被闪到（实测踩过：这个测试在桌面一跑，桌面二跟着闪）。
        InvalidateRect(GetDesktopWindow(), &region, FALSE);
        Sleep(50);
    }
}

// ---- 造一块"永远退不干净"的区域 ----
//
// 测试自己的顶层窗口盖在上面，刷成覆盖层自己的面板色。有了它，等待逻辑
// 就能被**确定性地**验证：对着这块区域问产品"退干净了吗"，它必须说
// "没有"，而且必须等满超时才回答——等待逻辑要是被摘掉（直接返回 true），
// 这里当场就红。
//
// 同一个窗口还有第二个用途：在覆盖层**启动之后**盖上去，产品的等待就必然
// 超时——用来验证"产品路径真的等了"。

LRESULT CALLBACK BlockerProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        if (dc) {
            RECT client{};
            GetClientRect(hwnd, &client);
            HBRUSH brush = CreateSolidBrush(g_overlaySurface);
            if (brush) {
                FillRect(dc, &client, brush);
                DeleteObject(brush);
            }
            EndPaint(hwnd, &ps);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// 选一块不与覆盖层重叠的位置：覆盖层在屏幕顶部居中，这里取左下角。
RECT PickBlockerRect() {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int width = 200;
    const int height = 100;
    RECT rect{};
    rect.left = work.left + 40;
    rect.right = rect.left + width;
    rect.bottom = work.bottom - 40;
    rect.top = rect.bottom - height;
    return rect;
}

HWND CreateBlocker(const RECT& rect) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = BlockerProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kBlockerClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return nullptr;
        }
        registered = true;
    }
    HWND blocker = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kBlockerClass,
        L"",
        WS_POPUP,
        rect.left,
        rect.top,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    if (blocker) {
        // 和产品一样的显示顺序：先建（隐藏），再显示并**立刻**画一次。
        // UpdateWindow 会同步发一次 WM_PAINT，不等消息泵——测试主线程在
        // 轮询像素时并不跑消息循环，等消息就会永远等不到。
        ShowWindow(blocker, SW_SHOWNOACTIVATE);
        UpdateWindow(blocker);
    }
    return blocker;
}

// 临时窗口的自动清理。
//
// 这些遮挡窗口是 TOPMOST 的：异常路径上忘了销毁，屏幕上就会**一直**留着
// 一块面板（用户看到的就是"有个东西在那儿一直不走"）。用 RAII 兜住，
// 无论从哪条路径退出都会收掉。
class ScopedWindow {
public:
    explicit ScopedWindow(HWND hwnd = nullptr) : hwnd_(hwnd) {}
    ~ScopedWindow() {
        if (hwnd_ && IsWindow(hwnd_)) {
            DestroyWindow(hwnd_);
        }
    }
    ScopedWindow(const ScopedWindow&) = delete;
    ScopedWindow& operator=(const ScopedWindow&) = delete;

    HWND get() const { return hwnd_; }
    // 已经自己销毁过了就别再管它。
    void release() { hwnd_ = nullptr; }

private:
    HWND hwnd_{nullptr};
};
// 收掉本进程还活着的临时窗口。
//
// 兜底措施：即便某条异常路径把 ScopedWindow 漏掉了（或者进程被强杀前
// 窗口已经建出来），也不能在用户屏幕上留一个 TOPMOST 面板——那看着就像
// "有个东西一直在那儿循环"。
void CleanupOurTemporaryWindows() {
    HWND current = nullptr;
    while ((current = FindWindowExW(nullptr, current, kBlockerClass, nullptr)) != nullptr) {
        DWORD pid = 0;
        GetWindowThreadProcessId(current, &pid);
        if (pid == GetCurrentProcessId() && IsWindow(current)) {
            DestroyWindow(current);
        }
    }
}
}  // namespace

int wmain() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 覆盖层自绘的颜色从产品取，不在测试里另抄一份。
    g_overlayBorder = snaplite::DelayOverlay::BorderColour();
    g_overlaySurface = snaplite::DelayOverlay::SurfaceColour();

    // 有没有可用的交互式桌面，决定了哪一类断言能跑。
    //
    // CI（GitHub windows-latest）跑在会话 0 的服务 window station 上，那里
    // 没有可抓屏的交互式桌面。抓屏**不会失败**——返回的是一张黑图，所以
    // "抓不到就跳过"的保护没有用，像素数会是 0，像素断言照样红。
    //
    // 但"窗口生命周期"那部分断言（起→取消→窗口没了、重启只留一个）
    // 与可见桌面无关，在服务 window station 里同样成立。所以两类断言
    // 分开跑：没有可见桌面时只跑生命周期那部分，连窗口都建不出来才整体
    // 跳过。否则 CI 上这个用例恒为"跳过"，等于零覆盖。
    // 两种"没有像素覆盖"的原因要分开报：环境变量强制是一种，
    // 窗口站不可见是另一种。文案混在一起会让人以为窗口站真不可见，
    // 而实际上只是自己设了个环境变量。
    const bool forcedHeadless = HeadlessForcedByEnvironment();
    const bool visibleStation = HasInteractiveDesktop();
    g_headless = forcedHeadless || !visibleStation;
    if (g_headless) {
        std::cout << "WARN: "
                  << (forcedHeadless
                          ? "SNAPLITE_DELAY_TEST_HEADLESS is set; "
                          : "no interactive desktop (window station is not visible); ")
                  << "running the window-lifecycle assertions only"
                  << std::endl;
        // 这一趟**没有**跑像素与产品路径的断言：必须记下来，结尾按
        // "跳过"报，而不是按"通过"报（否则等待逻辑整个坏掉时 CI 照样绿）。
        RecordSkip(
            "the pixel and product-path assertions (no usable desktop in this session)");
    }

    int result = 0;
    try {
        const HINSTANCE instance = GetModuleHandleW(nullptr);

        // 启动覆盖层。
        //
        // "产品拒绝启动"和"环境建不出窗口"必须分开：前者是产品缺陷
        // （硬失败），后者才是环境问题（无头时跳过）。原先这里一律走
        // CheckOrSkip，于是"把 Start() 改成永远返回 false"在 CI 上会变成
        // SKIP——CI 照样是绿的，缺陷就这么漏过去了。
        const auto start_overlay = [&](int seconds) {
            if (snaplite::DelayOverlay::Start(
                    instance, seconds, g_notifyWindow, WM_TEST_DELAY_ELAPSED)) {
                return true;
            }
            const int reason = static_cast<int>(snaplite::DelayOverlay::LastStartFailure());
            if (g_headless &&
                reason == static_cast<int>(snaplite::DelayOverlay::StartFailure::kWindowCreation)) {
                throw SkipTest{
                    "the overlay window cannot be created in this session "
                    "(environment problem, not a product defect)"};
            }
            throw std::runtime_error(
                "the delay overlay refuses to start (reason=" +
                std::to_string(reason) + ")");
        };

        // 1) 环境必须干净。别的进程举着延时覆盖层时，它和本测试的覆盖层
        //    位置、大小、颜色完全一样，量到的像素没有意义。
        //
        //    这台机器上完全可能有另一个 SnapLite 实例正在倒计时（用户自己
        //    开着的，或者另一个测试进程正在跑）。那不是本产品的缺陷，所以
        //    不能直接判失败——但像素断言必须跳过。
        const bool foreignOverlays = !WaitForForeignOverlays(2000);
        if (foreignOverlays) {
            std::cout << "WARN: another process is running a delay overlay ("
                      << OverlayWindowSummary()
                      << "); skipping the pixel and product-path assertions" << std::endl;
            RecordSkip(
                "the pixel and product-path assertions (another process is running a "
                "delay overlay, so the measured pixels are not ours and the "
                "product-path checks would judge someone else's overlay)");
        }

        // 这一块画面到底抓不抓得到。抓不到时像素断言一律跳过：
        // 抓屏失败（或抓到一张全黑的图）时量到的永远是 0，"覆盖层没画出来"
        // 和"屏幕抓不到"从数字上分不出来，当成缺陷就是误报，而且会把 CI
        // 与发布一起挡住。
        const bool screenReadable = CanCaptureScreen();
        if (!screenReadable) {
            std::cout << "WARN: the screen cannot be captured in this session; "
                         "skipping the pixel and product-path assertions"
                      << std::endl;
            RecordSkip(
                "the pixel and product-path assertions (the screen cannot be "
                "captured in this session)");
        }

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = NotifyProc;
        wc.hInstance = instance;
        wc.lpszClassName = L"SnapLiteDelayOverlayTestNotify";
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            if (g_headless) {
                throw SkipTest{LastErrorText("register notify window class")};
            }
            throw std::runtime_error(LastErrorText("register notify window class"));
        }

        g_notifyWindow = CreateWindowExW(
            0,
            L"SnapLiteDelayOverlayTestNotify",
            L"",
            WS_OVERLAPPED,
            0,
            0,
            0,
            0,
            nullptr,
            nullptr,
            instance,
            nullptr);
        if (!g_notifyWindow) {
            // 服务 window station 里连通知窗口都建不出来时，整体跳过：
            // 这是环境问题，不是产品缺陷。
            if (g_headless) {
                throw SkipTest{LastErrorText("create notify window")};
            }
            throw std::runtime_error(LastErrorText("create notify window"));
        }
        Check(g_notifyWindow != nullptr, "notify window created");

        // ===== A. 窗口生命周期（与可见桌面无关）=====

        // 2) 先起一次再取消：既验证 Cancel() 同步销毁窗口，也拿到这块
        //    区域的位置（覆盖层每次都落在同一个地方）。
        Check(start_overlay(30), "delay overlay starts for the cancel check");
        HWND overlay = FindOurOverlayWindow();
        CheckOrSkip(overlay != nullptr, "overlay window exists before cancel");
        GetWindowRect(overlay, &g_overlayRect);
        Check(
            g_overlayRect.right > g_overlayRect.left && g_overlayRect.bottom > g_overlayRect.top,
            "overlay window has a real rect");

        // 覆盖层出现之前，这块区域"本来是什么样"要记下来：桌面壁纸里也可能
        // 正好有和覆盖层一样的颜色，没有基线就分不清"残留的是覆盖层"和
        // "本来就有的"。
        //
        // 但**不能在这里量**：此刻覆盖层还挂在屏幕上，量到的就是它自己的
        // 颜色（≈15392），拿它当基线等于拿覆盖层跟覆盖层比——断言的上限
        // 比基线还大，永远不可能失败（上一轮验收实测：这是个空断言）。
        // 真正的基线在 B/C 段量：先把那块区域擦干净，再记。
        //
        // 残留容忍度由产品按 DPI 算（高 DPI 上圆角更大、抗锯齿边缘更长），
        // 这里从产品取，不在测试里写死。
        g_residueTolerance = snaplite::DelayOverlay::LastResidueTolerance();
        Check(
            g_residueTolerance > 0,
            "the product reports the residue tolerance it uses");

        snaplite::DelayOverlay::Cancel();
        Check(
            IsWindow(overlay) == FALSE,
            ("cancel destroys the overlay window synchronously; left=" +
             OverlayWindowSummary(overlay))
                .c_str());
        Check(
            ListOurOverlayWindows().empty(),
            ("cancel leaves no overlay window on screen; left=" + OverlayWindowSummary())
                .c_str());

        // 产品路径必须**真的调用过**"等重绘"——不是"源码里应该有这段"，
        // 而是"这一趟真的调用过"。少了这一条，CI（没有可抓屏桌面的环境）
        // 对等待逻辑就是零覆盖：把产品路径里的等待整段摘掉，测试照样全绿。
        Check(
            snaplite::DelayOverlay::LastWaitCallCount() >= 1,
            "the product really called the repaint wait after destroying the overlay");

        // 3) 重复启动倒计时只能留下一个覆盖层。
        //
        //    这一组与像素无关：覆盖层窗口的生命周期在服务 window station 里
        //    同样成立，所以没有可见桌面时它照样要跑。
        Check(start_overlay(30), "delay overlay starts again");
        HWND first = FindOurOverlayWindow();
        CheckOrSkip(first != nullptr, "first overlay exists");
        Check(start_overlay(30), "delay overlay starts a second time");
        HWND second = FindOurOverlayWindow();
        CheckOrSkip(second != nullptr, "second overlay exists");
        Check(second != first, "starting again replaces the previous overlay");
        Check(
            IsWindow(first) == FALSE,
            ("the previous overlay is gone after restarting; left=" +
             OverlayWindowSummary(first))
                .c_str());
        Check(
            ListOurOverlayWindows().size() == 1,
            ("only one overlay is left on screen; left=" + OverlayWindowSummary(first))
                .c_str());

        // 4) 没有覆盖层时取消不能崩，而且不该白跑一次"等重绘"。
        snaplite::DelayOverlay::Cancel();
        Check(
            IsWindow(second) == FALSE,
            ("cancel clears the last overlay; left=" + OverlayWindowSummary(second))
                .c_str());
        Check(
            ListOurOverlayWindows().empty(),
            ("no overlay window is left after the final cancel; left=" +
             OverlayWindowSummary())
                .c_str());
        const unsigned waitsBeforeIdleCancel =
            snaplite::DelayOverlay::LastWaitCallCount();
        snaplite::DelayOverlay::Cancel();
        Check(
            snaplite::DelayOverlay::LastWaitCallCount() == waitsBeforeIdleCancel,
            "cancel without an overlay does not run a repaint wait");
        Check(
            ListOurOverlayWindows().empty(),
            "cancel without an overlay leaves no overlay window behind");

        // ===== B/C. 像素、等待与产品路径（需要可抓屏的桌面）=====

        // 没有可见桌面时到第 4 步为止：上面那些窗口生命周期的断言是真跑过的，
        // 不是整体跳过。
        if (!g_headless && screenReadable) {
            // 5) 先把那块区域擦干净，并记下**覆盖层出现之前**的基线。
            //
            //    基线必须在覆盖层**没出现**的时候量：覆盖层可见时量到的
            //    就是它自己的颜色（≈15392），拿它当基线等于拿覆盖层跟
            //    覆盖层比——断言的上限比基线还大，永远不可能失败。
            //    原来那条就是这么写的，所以它其实是个**空断言**
            //    （上一轮验收实测：expected=15392，断言却怎么都绿）。
            //
            //    擦不干净就只跑窗口生命周期的断言：环境脏了不是产品的错。
            //    但这一趟必须按**跳过**报，不能按"通过"报。
            const bool regionClean = !foreignOverlays && TryCleanRegion(g_overlayRect, 1000);
            if (!regionClean) {
                // 打印的是**脏像素数**，不是清理后的基线——打印基线会得到
                // "not clean (0)" 这种自相矛盾的文案。
                std::cout << "WARN: overlay region is not clean ("
                          << CountOverlayPixels(g_overlayRect).total()
                          << " overlay-coloured pixels left); skipping the pixel and "
                             "product-path assertions (desktop is dirty)"
                          << std::endl;
                RecordSkip(
                    "the pixel and product-path assertions (the overlay region could "
                    "not be cleaned; the desktop is dirty)");
            }
            const PixelCount baseline = CountOverlayPixels(g_overlayRect);
            const long preOverlayColours = baseline.total();

            // 6) 取消之后，那块区域必须回到覆盖层出现之前的样子。
            //
            //    要在**擦干净之后**重新起一次覆盖层再取消：擦桌面会把
            //    产品留下的残留一起擦掉，擦完再测就等于替产品把问题擦没了。
            if (regionClean) {
                Check(start_overlay(30),
                      "delay overlay starts for the cancel-cleanliness check");
                snaplite::DelayOverlay::Cancel();
                const long afterCancel = CountOverlayPixels(g_overlayRect).total();
                Check(
                    afterCancel <= preOverlayColours + g_residueTolerance,
                    ("the overlay region is clean right after cancel; before=" +
                     std::to_string(preOverlayColours) + " after=" +
                     std::to_string(afterCancel) + " tolerance=" +
                     std::to_string(g_residueTolerance))
                        .c_str());
            }

            // 7) 覆盖层可见时，它自绘的颜色真的落在屏幕上。
            Check(start_overlay(2), "delay overlay starts");
            overlay = FindOurOverlayWindow();
            Check(overlay != nullptr, "overlay window exists");
            Check(IsWindowVisible(overlay) != FALSE, "overlay window is visible");
            // 基线必须真的采到了。采不到时产品会退回"睡固定时长去赌桌面
            // 画完了"——那正是这次改动要消灭的行为，可从外面看没有任何
            // 区别，只能直接问它。
            Check(
                snaplite::DelayOverlay::LastBaselineColours() >= 0,
                "the overlay captured a baseline for the repaint check");
            // 基线必须采自**覆盖层出现之前**的屏幕内容。
            //
            // 上一轮验收实测：把取样挪到"窗口建好、还没显示"的位置，
            // 运行时照样全绿——而那一版其实无害（窗口还没显示，屏幕上
            // 还是原来的内容）；真正坏掉的是"显示之后才取样"（采到的
            // 已经是覆盖层自己的颜色）。
            //
            // 所以这里问的是"采到的是不是显示之前那块画面的内容"，
            // 而不是"在窗口创建之前还是之后"：前者才是真正的性质。
            if (regionClean) {
                const long reportedBaseline =
                    snaplite::DelayOverlay::LastBaselineColours();
                const long expectedBaseline = baseline.total();
                const long baselineDifference =
                    reportedBaseline > expectedBaseline
                        ? reportedBaseline - expectedBaseline
                        : expectedBaseline - reportedBaseline;
                Check(
                    baselineDifference <= g_residueTolerance + expectedBaseline / 4,
                    ("the baseline was sampled from the screen before the overlay "
                     "appeared; reported=" +
                     std::to_string(reportedBaseline) + " expected=" +
                     std::to_string(expectedBaseline))
                        .c_str());
            }
            RECT liveRect{};
            GetWindowRect(overlay, &liveRect);
            Check(
                liveRect.left == g_overlayRect.left && liveRect.top == g_overlayRect.top &&
                    liveRect.right == g_overlayRect.right &&
                    liveRect.bottom == g_overlayRect.bottom,
                "the overlay is placed at the same spot every time");

            PixelCount whileVisible{};
            if (regionClean) {
                // 覆盖层可见时，它自绘的颜色真的落在屏幕上。
                whileVisible = WaitForOverlayPixels(g_overlayRect, 2000);
                Check(
                    whileVisible.total() > kOverlaySignature,
                    ("overlay paints its own colours onto the screen; got=" +
                     std::to_string(whileVisible.total()))
                        .c_str());
            }

            // 8) 等倒计时归零。App 抓屏的时机就是收到通知这一刻。
            g_notified = false;
            Check(WaitForNotification(10000), "countdown posts the elapsed notification");

            if (regionClean) {
                // 覆盖层的像素必须在抓屏时刻就已经从屏幕上消失。
                Check(
                    g_atNotification.total() < kOverlaySignature,
                    ("overlay pixels are gone by the time the app captures the screen; "
                     "whileVisible=" +
                     std::to_string(whileVisible.total()) + " atNotify=" +
                     std::to_string(g_atNotification.total()) + " baseline=" +
                     std::to_string(baseline.total()) + " left=" +
                     OverlayWindowSummary(overlay))
                        .c_str());
                // 而且那块区域要回到覆盖层出现之前的样子。
                Check(
                    g_atNotification.total() <= baseline.total() + g_residueTolerance,
                    ("the overlay region returns to its pre-overlay state; baseline=" +
                     std::to_string(baseline.total()) + " atNotify=" +
                     std::to_string(g_atNotification.total()))
                        .c_str());
            }

            // 9) 抓屏那一刻，覆盖层窗口本身也必须已经没了。
            Check(
                IsWindow(overlay) == FALSE,
                ("the overlay window is destroyed before the app captures the screen; left=" +
                 OverlayWindowSummary(overlay))
                    .c_str());

            // 10) 等待实现本身必须真的在等。
            //
            //     上面那些断言都发生在"产品已经等完之后"，所以哪怕产品
            //     **完全不等**（把等待整段摘掉），它们照样全绿。
            //     这里换一种问法：造一块永远退不干净的区域，问产品
            //     "退干净了吗"，它必须说"没有"，而且必须**等满超时**
            //     才回答。等待逻辑被摘掉时它会立刻返回 true，当场就红。
            const RECT blockerRect = PickBlockerRect();
            const long blockerBaseline = CountOverlayPixels(blockerRect).total();
            ScopedWindow blocker(CreateBlocker(blockerRect));
            Check(blocker.get() != nullptr, "blocker window created");
            if (blocker.get()) {
                const PixelCount blocked = WaitForOverlayPixels(blockerRect, 2000);
                Check(
                    blocked.total() > kOverlaySignature,
                    ("the blocker window really covers the region; got=" +
                     std::to_string(blocked.total()))
                        .c_str());

                const ULONGLONG waitStart = GetTickCount64();
                const bool cleared = snaplite::DelayOverlay::WaitForOverlayColoursGone(
                    blockerRect, blockerBaseline, 400);
                const ULONGLONG waited = GetTickCount64() - waitStart;
                Check(
                    !cleared,
                    "the wait reports that the overlay colours are still there");
                Check(
                    waited >= 300,
                    ("the wait really waited instead of returning at once; waited=" +
                     std::to_string(waited))
                        .c_str());

                // 限频：这一次等待的墙钟是**测试自己量的**（waited），
                // 上限用**测试侧**的下限（100ms）算——不看产品自报的间隔。
                //
                // 上一轮验收实测：只按产品自报的间隔算上限的话，
                // "把限频彻底拆掉、再把上报的间隔/次数/间隔值一起造假"
                // 这一整套串通谎言，运行时 76 项照样全绿。墙钟是测试量的，
                // 谎言盖不住。
                const unsigned pollNudges =
                    snaplite::DelayOverlay::LastWaitPollRepaintCount();
                const unsigned maxExternalNudges =
                    1 + static_cast<unsigned>(waited) / kMinimumNudgeIntervalMs;
                Check(
                    pollNudges <= maxExternalNudges,
                    ("the repaint nudges are rate-limited even by an outside "
                     "measurement; nudges=" +
                     std::to_string(pollNudges) + " max=" +
                     std::to_string(maxExternalNudges) + " waited=" +
                     std::to_string(waited) + "ms floor=" +
                     std::to_string(kMinimumNudgeIntervalMs) + "ms")
                        .c_str());
                // 一次等满上限的等待至少要催两次：只催一次的话，
                // "让桌面重画那块像素"这条依托就退化成了摆设
                // （上一轮验收实测：限频调大到 60000ms 时两层都看不见）。
                Check(
                    pollNudges >= 2,
                    ("a wait that runs the whole window must nudge the desktop more "
                     "than once; nudges=" +
                     std::to_string(pollNudges))
                        .c_str());

                DestroyWindow(blocker.get());
                blocker.release();
                const bool clearedAfter = snaplite::DelayOverlay::WaitForOverlayColoursGone(
                    blockerRect, blockerBaseline, 2000);
                Check(
                    clearedAfter,
                    "the wait reports the region clean once the blocker is gone");
            }

            // 11) 产品路径：基线必须**真的采自屏幕**。
            //
            //     事先在那块区域贴一块与覆盖层同色的补丁，再启动覆盖层：
            //     产品报出来的基线必须把这块补丁算进去。只断言
            //     "基线 >= 0"是拦不住的——写死一个 0 也能满足。
            if (regionClean) {
                ScopedWindow patch(CreateBlocker(g_overlayRect));
                Check(patch.get() != nullptr, "the pre-existing patch window was created");
                if (patch.get()) {
                    const PixelCount patched = WaitForOverlayPixels(g_overlayRect, 2000);
                    Check(
                        patched.total() > kOverlaySignature,
                        ("the pre-existing patch really covers the region; got=" +
                         std::to_string(patched.total()))
                            .c_str());
                    const long expectedBaseline = CountOverlayPixels(g_overlayRect).total();
                    Check(start_overlay(30), "delay overlay starts over the patch");
                    const long reportedBaseline =
                        snaplite::DelayOverlay::LastBaselineColours();
                    Check(
                        reportedBaseline > 0,
                        ("the baseline is not a hard-coded zero; reported=" +
                         std::to_string(reportedBaseline) + " expected=" +
                         std::to_string(expectedBaseline))
                            .c_str());
                    const long difference = reportedBaseline > expectedBaseline
                                                ? reportedBaseline - expectedBaseline
                                                : expectedBaseline - reportedBaseline;
                    Check(
                        difference <= g_residueTolerance + expectedBaseline / 4,
                        ("the reported baseline matches what is on the screen; reported=" +
                         std::to_string(reportedBaseline) + " expected=" +
                         std::to_string(expectedBaseline))
                            .c_str());
                    snaplite::DelayOverlay::Cancel();
                }
            }

            // 12) 产品路径：产品必须**真的等过**。
            //
            //     这次让那块区域永远退不干净：覆盖层**启动之后**再盖一块
            //     遮挡窗口（它是 TOPMOST，explorer 重绘桌面不会覆盖它），
            //     于是基线是"遮挡之前"的，而覆盖层销毁后那块区域里仍然是
            //     遮挡窗口的颜色——产品的等待必然等满上限。
            //
            //     于是"产品压根没等"或"等待上限被改成 0"都会让耗时塌成 0，
            //     这里当场就红。这条断言盯的是**产品路径**，不是某条旁路
            //     helper 的性质——上一轮验收就是在这里判的 FAIL。
            if (regionClean) {
                const bool cleanAgain = TryCleanRegion(g_overlayRect, 1000);
                if (!cleanAgain) {
                    std::cout << "WARN: the overlay region is still dirty; skipping the "
                                 "product-path wait assertions"
                              << std::endl;
                    RecordSkip(
                        "the product-path wait assertions (the overlay region stayed dirty)");
                } else {
                    Check(start_overlay(2), "delay overlay starts before the blocker");
                    ScopedWindow regionBlocker(CreateBlocker(g_overlayRect));
                    Check(regionBlocker.get() != nullptr, "the region blocker was created");
                    if (regionBlocker.get()) {
                        const PixelCount blocked =
                            WaitForOverlayPixels(g_overlayRect, 2000);
                        Check(
                            blocked.total() > kOverlaySignature,
                            ("the blocker really covers the overlay region; got=" +
                             std::to_string(blocked.total()))
                                .c_str());

                        g_notified = false;
                        Check(
                            WaitForNotification(15000),
                            "the countdown still notifies even though the region stays dirty");

                        Check(
                            snaplite::DelayOverlay::LastWaitCallCount() >= 1,
                            "the product called the repaint wait on its own path");
                        Check(
                            snaplite::DelayOverlay::LastWaitTimedOut(),
                            "the product reported that the region never became clean");
                        // 等了多久，要跟**产品自己用的上限**比，不能写死：
                        // 写死 800ms 的话，产品合法地把上限调到 500 就会假红。
                        const DWORD productTimeout =
                            snaplite::DelayOverlay::LastWaitTimeoutMs();
                        Check(
                            productTimeout > 0,
                            "the product reports the wait timeout it used");
                        // 但也必须有**绝对下限**：等重绘的窗口太短，桌面根本
                        // 来不及重画，截图就会拍到覆盖层的残影。
                        //
                        // 这一条是**对产品的要求**，不是测试自己写死一个数：
                        // 上限从 800 调到 500 是合法的（不假红），调到 1ms
                        // 就是回归（残影），必须抓住。
                        Check(
                            productTimeout >= kMinimumProductWaitTimeoutMs,
                            ("the product's wait timeout is too short to let the desktop "
                             "repaint; timeout=" +
                             std::to_string(productTimeout) + "ms minimum=" +
                             std::to_string(kMinimumProductWaitTimeoutMs) + "ms")
                                .c_str());
                        Check(
                            snaplite::DelayOverlay::LastWaitElapsedMs() >=
                                productTimeout * 3 / 4,
                            ("the product really waited instead of returning at once; elapsed=" +
                             std::to_string(snaplite::DelayOverlay::LastWaitElapsedMs()) +
                             "ms timeout=" + std::to_string(productTimeout) + "ms")
                                .c_str());
                        Check(
                            !snaplite::DelayOverlay::LastWaitUsedFixedSleepFallback(),
                            "the product did not fall back to a fixed sleep");
                        Check(
                            snaplite::DelayOverlay::LastDestroyRepaintCount() >= 1,
                            "the product asked the desktop to repaint after destroying the overlay");
                        Check(
                            snaplite::DelayOverlay::LastPollRepaintCount() >= 1,
                            "the product kept asking the desktop to repaint while waiting");

                        // ===== 催重绘必须限频（F1）=====
                        //
                        // 这是用户实测过的那个问题的直接修复点："你在桌面一
                        // 测试的时候竟然还能影响到我桌面二"——桌面窗口横跨
                        // 所有显示器，一串密集的重绘请求下去，别的屏也跟着
                        // 重画。产品对催重绘限频：距上次催至少隔一段时间。
                        //
                        // 上一轮验收实测：这条限频**一条断言都没有**，
                        // 去掉限频之后运行时 68 项、静态 75 项全绿
                        // （800ms 窗口内从 4 次变成 18 次）。
                        //
                        // 判据不能写死一个次数：产品把间隔从 200ms 调到
                        // 300ms 是合法的（那样反而更安静）。所以上限由
                        // **产品自己报的间隔**算出来。
                        const DWORD nudgeInterval =
                            snaplite::DelayOverlay::LastRepaintNudgeIntervalMs();
                        Check(
                            nudgeInterval > 0,
                            "the product reports the repaint nudge interval it uses");
                        // 光"报了数"不够：产品可以把间隔报成 1ms 来骗过
                        // 上面那条次数上限（上限会被算得很大）。所以间隔
                        // 本身也有**绝对下限**——这是对产品的要求，不是
                        // 测试写死一个数。
                        Check(
                            nudgeInterval >= kMinimumNudgeIntervalMs,
                            ("the repaint nudge interval is too short to keep the "
                             "other monitors from flickering; interval=" +
                             std::to_string(nudgeInterval) + "ms minimum=" +
                             std::to_string(kMinimumNudgeIntervalMs) + "ms")
                                .c_str());
                        const unsigned waitPollRepaints =
                            snaplite::DelayOverlay::LastWaitPollRepaintCount();
                        const unsigned maxWaitPollRepaints =
                            productTimeout / nudgeInterval + 2;
                        Check(
                            waitPollRepaints <= maxWaitPollRepaints,
                            ("the product rate-limits the repaint nudges; nudges=" +
                             std::to_string(waitPollRepaints) + " max=" +
                             std::to_string(maxWaitPollRepaints) + " interval=" +
                             std::to_string(nudgeInterval) + "ms timeout=" +
                             std::to_string(productTimeout) + "ms")
                                .c_str());
                        // 相邻两次催重绘的间隔必须真的达到限频值。
                        //
                        // 上面那条只断"次数没超"，产品可以改成"每次都催，
                        // 只在最后一次之前停手"来骗过它；这条断的是
                        // **间隔**本身。
                        const DWORD nudgeGap =
                            snaplite::DelayOverlay::LastPollNudgeGapMs();
                        Check(
                            waitPollRepaints < 2 || nudgeGap >= nudgeInterval,
                            ("the product waits the nudge interval between repaints; "
                             "gap=" +
                             std::to_string(nudgeGap) + "ms interval=" +
                             std::to_string(nudgeInterval) + "ms nudges=" +
                             std::to_string(waitPollRepaints))
                                .c_str());

                        // ===== 轮询必须歇息（F2）=====
                        //
                        // 轮询不能空转：每 30ms 看一次像素，之间必须歇一下。
                        // 歇息歇没了，等待就变成烧 CPU 的忙等——上一轮验收
                        // 实测：删掉那次 Sleep 之后运行时照样全绿。
                        const DWORD restInterval =
                            snaplite::DelayOverlay::LastWaitRestIntervalMs();
                        Check(
                            restInterval > 0,
                            "the product reports the poll rest interval it uses");
                        const DWORD restMs = snaplite::DelayOverlay::LastWaitRestMs();
                        const unsigned sampleCount =
                            snaplite::DelayOverlay::LastWaitSampleCount();
                        const DWORD waitElapsedMs =
                            snaplite::DelayOverlay::LastWaitElapsedMs();
                        if (snaplite::DelayOverlay::LastWaitTimedOut()) {
                            // 等待跑满了整个上限：这段时间里它一直在轮询，
                            // 歇息时长必须是**真的**。
                            //
                            // 上界：声称的歇息不可能超过等待本身的墙钟。
                            // 这一条**不依赖产品自报的间隔**——把歇息改成
                            // "轮数 × 声称的间隔"（数出来的而不是量出来的）
                            // 时，轮数是真的，歇息就会超出墙钟
                            // （上一轮验收实测：两条一起造假 → 运行时全绿）。
                            Check(
                                restMs <= waitElapsedMs,
                                ("the reported rest time cannot exceed the wait "
                                 "itself; rested=" + std::to_string(restMs) +
                                 "ms elapsed=" + std::to_string(waitElapsedMs) +
                                 "ms")
                                    .c_str());
                            // 下界：至少歇过一个完整的间隔。
                            //
                            // 写成"至少歇了两次"太紧：等待偶尔提前结束
                            // （区域意外变干净、或抓屏失败走兜底分支）时
                            // 歇息自然很小——那是**合法**的行为，不该判红
                            // （上一轮验收实测：同一个二进制两跑结论不同）。
                            Check(
                                restMs >= restInterval,
                                ("the wait really rested between the pixel checks; "
                                 "rested=" + std::to_string(restMs) +
                                 "ms interval=" + std::to_string(restInterval) +
                                 "ms")
                                    .c_str());
                        }
                        Check(
                            sampleCount <= waitElapsedMs / restInterval + 6,
                            ("the wait did not busy-poll the screen; samples=" +
                             std::to_string(sampleCount) + " elapsed=" +
                             std::to_string(waitElapsedMs) + "ms rest=" +
                             std::to_string(restInterval) + "ms")
                                .c_str());

                        // 催重绘的**区域**必须就是覆盖层那一小块。
                        //
                        // 区域是空的 = 让 explorer 重绘整块虚拟桌面 = 别的
                        // 显示器跟着闪（用户实测：测试在桌面一跑，桌面二闪）。
                        // 这件事从结果上完全看不出来（画面最后都会恢复），
                        // 只有"请求了哪一块"能被问出来。
                        //
                        // 而且这里问的是**真正交给 Win32 的**实参，不是
                        // 产品的入参：记入参的话，把实参换成 nullptr /
                        // erase=TRUE 时这条断言恒成立（上一轮验收实测的
                        // 两条假绿）。
                        Check(
                            snaplite::DelayOverlay::LastRepaintCallCount() >= 1,
                            "the product really asked the desktop to repaint");
                        Check(
                            snaplite::DelayOverlay::LastRepaintEraseFlagCount() ==
                                snaplite::DelayOverlay::LastRepaintCallCount(),
                            ("every repaint request went through the recorded path; "
                             "calls=" +
                             std::to_string(snaplite::DelayOverlay::LastRepaintCallCount()) +
                             " eraseFlags=" +
                             std::to_string(
                                 snaplite::DelayOverlay::LastRepaintEraseFlagCount()))
                                .c_str());
                        Check(
                            !snaplite::DelayOverlay::LastRepaintTargetWasNull(),
                            "the rectangle handed to Windows is not empty "
                            "(an empty one means the whole virtual desktop)");
                        Check(
                            !snaplite::DelayOverlay::LastRepaintErased(),
                            "the product did not ask Windows to erase the background "
                            "(that makes the other monitor flicker)");
                        const RECT repaintRegion =
                            snaplite::DelayOverlay::LastRepaintRegion();
                        Check(
                            repaintRegion.right > repaintRegion.left &&
                                repaintRegion.bottom > repaintRegion.top,
                            ("the product asked the desktop to repaint a bounded region; "
                             "region=(" +
                             std::to_string(repaintRegion.left) + "," +
                             std::to_string(repaintRegion.top) + ")-(" +
                             std::to_string(repaintRegion.right) + "," +
                             std::to_string(repaintRegion.bottom) + ")")
                                .c_str());
                        Check(
                            repaintRegion.left >= g_overlayRect.left - 4 &&
                                repaintRegion.top >= g_overlayRect.top - 4 &&
                                repaintRegion.right <= g_overlayRect.right + 4 &&
                                repaintRegion.bottom <= g_overlayRect.bottom + 4,
                            ("the repaint region is the overlay's own area, not the whole "
                             "desktop; repaint=(" +
                             std::to_string(repaintRegion.left) + "," +
                             std::to_string(repaintRegion.top) + ")-(" +
                             std::to_string(repaintRegion.right) + "," +
                             std::to_string(repaintRegion.bottom) + ") overlay=(" +
                             std::to_string(g_overlayRect.left) + "," +
                             std::to_string(g_overlayRect.top) + ")-(" +
                             std::to_string(g_overlayRect.right) + "," +
                             std::to_string(g_overlayRect.bottom) + ")")
                                .c_str());
                    }
                }
            }
        }

        // 13) 计数必须**由 Win32 API 的返回值驱动**。
        //
        //     做法：让"Windows 拒绝了这次重绘请求"这件事可以被造出来
        //     （ForceRepaintFailure），然后跑一轮完整的"启动 → 取消"流程。
        //
        //     对照组（不强制失败）：销毁之后一定催过一次重绘。
        //     实验组（强制失败）：请求被拒绝了，就**不算催过**。
        //
        //     如果计数是无条件 ++ 的（或者重绘函数假装永远成功），
        //     实验组里那两个计数照样会涨——这条断言当场就红。
        //     上一轮验收就是在这儿判的 FAIL：只查"计数被某个变量支配"
        //     的话，"调用 API 但忽略返回值"照样能过。
        Check(
            start_overlay(30),
            "delay overlay starts for the repaint-gating control run");
        snaplite::DelayOverlay::Cancel();
        const unsigned controlDestroyRepaints =
            snaplite::DelayOverlay::LastDestroyRepaintCount();
        Check(
            controlDestroyRepaints >= 1,
            ("the control run really asked the desktop to repaint after destroying "
             "the overlay; count=" + std::to_string(controlDestroyRepaints))
                .c_str());

        snaplite::DelayOverlay::ForceRepaintFailure(true);
        Check(
            start_overlay(30),
            "delay overlay starts for the repaint-gating experiment run");
        snaplite::DelayOverlay::Cancel();
        snaplite::DelayOverlay::ForceRepaintFailure(false);
        const unsigned refusedDestroyRepaints =
            snaplite::DelayOverlay::LastDestroyRepaintCount();
        const unsigned refusedPollRepaints =
            snaplite::DelayOverlay::LastPollRepaintCount();
        Check(
            refusedDestroyRepaints == 0,
            ("a refused repaint request must not be counted as a repaint; count=" +
             std::to_string(refusedDestroyRepaints))
                .c_str());
        Check(
            refusedPollRepaints == 0,
            ("a refused repaint request must not be counted as a poll repaint; "
             "count=" + std::to_string(refusedPollRepaints))
                .c_str());

        // 14) Start() 的参数检查。
        //
        //     秒数不在 1..60 内必须**明确失败**，而且不能顺手把上一次
        //     的覆盖层拆掉（参数检查在 Cancel 之前）。这一条不需要可抓屏
        //     的桌面，所以放在 B/C 段之外——无头环境下它照样跑。
        //
        //     它会清掉计数器，所以必须放在最后：不能打断上面那些断言。
        const long baselineBeforeRefusal =
            snaplite::DelayOverlay::LastBaselineColours();
        Check(
            !snaplite::DelayOverlay::Start(
                instance, 0, g_notifyWindow, WM_TEST_DELAY_ELAPSED),
            "the overlay refuses an invalid duration");
        Check(
            snaplite::DelayOverlay::LastStartFailure() ==
                snaplite::DelayOverlay::StartFailure::kInvalidArguments,
            "the refused start reports the invalid arguments");
        Check(
            snaplite::DelayOverlay::LastBaselineColours() == -1,
            ("a refused start leaves no baseline from the previous run; before=" +
             std::to_string(baselineBeforeRefusal))
                .c_str());
        Check(
            ListOurOverlayWindows().empty(),
            "a refused start does not create an overlay window");

        // 结尾按**跳过**还是按**通过**报，要看这一趟有没有把断言跳掉。
        //
        // 跳过不等于通过：前提不满足时，被跳过的那些断言等于没跑。
        // 报 "PASS" 的话，一个彻底坏掉的产品在 CI 上也能拿到"PASS"
        // （上一轮验收实测：等待逻辑整个坏掉，HEADLESS 下照样 0 退出）。
        // 退出码 77 是 ctest 认识的"跳过"，会被显示成 Skipped 而不是 Passed。
        if (!g_skipped.empty()) {
            std::cout << "SKIP: " << checks
                      << " delay overlay checks ran, but " << g_skipped.size()
                      << " group(s) of assertions were skipped:" << std::endl;
            for (const std::string& item : g_skipped) {
                std::cout << "  - " << item << std::endl;
            }
            result = kSkipExitCode;
        } else {
            std::cout << "PASS: " << checks << " delay overlay checks" << std::endl;
        }
    } catch (const SkipTest& skip) {
        std::cout << "SKIP: " << skip.reason << std::endl;
        result = kSkipExitCode;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        result = 1;
    }
    snaplite::DelayOverlay::Cancel();
    CleanupOurTemporaryWindows();
    if (g_notifyWindow) DestroyWindow(g_notifyWindow);
    return result;
}
