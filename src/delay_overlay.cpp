#include "delay_overlay.h"

#include "capture.h"
#include "ui_theme.h"

#include <string>
#include <vector>

namespace snaplite {
namespace {

constexpr wchar_t kOverlayClass[] = L"SnapLiteDelayOverlay";
constexpr UINT_PTR kCountdownTimer = 1;
constexpr int kOverlayWidth = 148;
constexpr int kOverlayHeight = 104;
// 圆角半径与顶部偏移，按 96 DPI 定的，实际要随 DPI 缩放
// （尺寸缩放了、圆角不缩放的话，高 DPI 下面板会显得很"方"）。
constexpr int kOverlayCornerRadius = 20;
constexpr int kOverlayTopOffset = 48;
constexpr int kTickMs = 200;
// 覆盖层销毁后等桌面重绘的时长。桌面窗口属于 explorer 进程，重绘由它自己
// 调度，这里只能给一小段等待窗口，确保截图时那块像素已经恢复。
constexpr DWORD kRepaintWaitMs = 120;
// 等重绘的轮询节奏与上限。恢复检测失败时（例如抓不到那块区域的画面）
// 退回固定等待；有基线可对比时改用"盯像素"的轮询。
constexpr DWORD kRepaintPollMs = 30;
constexpr DWORD kRepaintTimeoutMs = 800;
// 催桌面重绘的最小间隔。
//
// 催得太勤会被放大成闪屏：桌面窗口横跨所有显示器，一串重绘请求下去，
// 别的显示器也会跟着重画（用户实测到"在桌面一测试，桌面二也闪"）。
// 800ms 的上限内按 200ms 催一次，最多四次，够用又不闹。
constexpr DWORD kRepaintNudgeIntervalMs = 200;
// 覆盖层边缘是圆角 + 抗锯齿，重绘回来可能还剩几个同色像素，容忍一下。
//
// 这个容忍度是按 96 DPI 定的。高 DPI 下圆角更大、抗锯齿边缘更长，
// 照搬同一个像素数就会**永远等不满**——每次延时截图都吃满 800ms 上限，
// 界面被冻住。所以实际用的值要按 DPI 缩放（见 Start 里的 MulDiv）。
constexpr long kOverlayResidueTolerance = 64;

struct OverlayState {
    int secondsLeft{};
    ULONGLONG deadline{};
    HWND notifyWindow{};
    UINT notifyMessage{};
    // 每个覆盖层实例各自记录 Esc 是否已按下，避免跨实例串状态
    // （上一个实例按 Esc 取消后，新实例会把「仍按着」当成「刚按下」）。
    bool escapeWasDown{false};
    // 覆盖层占据的屏幕区域，以及这块区域在覆盖层出现之前本来就有的
    // "覆盖层同色像素"数量。后者是基线：桌面壁纸里也可能正好有一样的
    // 颜色，没有基线就分不清「残留的是覆盖层」和「本来就有的」。
    RECT covered{};
    long baselineOverlayColours{0};
    // 残留容忍度（按覆盖层所在显示器的 DPI 缩放）。
    long residueTolerance{kOverlayResidueTolerance};
};

HWND g_overlay = nullptr;
OverlayState* g_state = nullptr;

#ifdef SNAPLITE_DELAY_TEST_HOOKS
// ===== 测试钩子的记录位（只在测试编译里存在）=====
//
// 记的是"产品做了什么决定"，不是"某个可以随手写死的数字"：
// 等待走的是哪条分支、实际等了多久、催了几次重绘。
long g_lastBaselineColours = -1;
unsigned g_waitCallCount = 0;
DWORD g_lastWaitElapsedMs = 0;
bool g_lastWaitTimedOut = false;
bool g_lastWaitUsedFixedSleepFallback = false;
unsigned g_lastDestroyRepaintCount = 0;
unsigned g_lastPollRepaintCount = 0;
int g_lastStartFailure = 0;
// 最近一次**真的发出去**的重绘请求，及其区域。
//
// 区域这一项不只是给测试看的：它是"产品到底让桌面重画了哪一块"的
// 唯一外部证据。整块重绘（区域为空）会让别的显示器跟着闪，而这件事
// 从结果上完全看不出来——只有"请求了哪一块"能被问出来。
RECT g_lastRepaintRegion{};
bool g_lastRepaintTargetWasNull = false;
bool g_lastRepaintErased = false;
unsigned g_lastRepaintCallCount = 0;
unsigned g_lastRepaintEraseFlagCount = 0;
// 最近一次等待用的上限与残留容忍度（都由产品决定，测试不该写死）。
DWORD g_lastWaitTimeoutMs = 0;
long g_lastResidueTolerance = 0;
// 催重绘的限频间隔，以及最近一次等待里的催重绘情况。
//
// 记的是"产品催了几次、间隔多久"，不是"某段代码里写着 200"——
// 上一轮验收实测：把限频判断整个去掉（改成无条件催），运行时 68 项、
// 静态 75 项全绿，800ms 窗口内的催重绘从 4 次变成 18 次，却没有任何
// 一道门会拦。限频一旦回退，用户实测过的"桌面二跟着闪"就回来了。
DWORD g_repaintNudgeIntervalMs = 0;
unsigned g_lastWaitPollRepaintCount = 0;
DWORD g_lastPollNudgeGapMs = 0;
// 轮询歇息的决策与实际行为。
//
// 歇息时长是**量出来**的（Sleep 前后的时钟差），不是数出来的：
// 把那行 Sleep 删掉，计数照样在涨，但量出来的时长会归零。
DWORD g_lastWaitRestIntervalMs = 0;
DWORD g_lastWaitRestMs = 0;
unsigned g_lastWaitSampleCount = 0;
// 让"Windows 拒绝了这次重绘请求"这件事**可以被造出来**。
//
// 为什么需要它：光靠静态形状断言，只能保证"重绘函数看起来是照着 API
// 的返回值写的"；挡不住"照着形状写、但实际上无条件 return true"
// （上一轮验收实测的漏判 M11b：调 API 但忽略返回值，守卫全绿）。
//
// 有了这个开关，运行时测试就能问出产品自己才能回答的问题：
// "如果那次请求被拒绝了，你还算催过重绘吗？"
// 答"不算"——计数就必须被 API 的返回值支配。
bool g_forceRepaintFailure = false;
#endif

void PaintOverlay(HWND hwnd, OverlayState* state) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    if (!dc) {
        EndPaint(hwnd, &ps);
        return;
    }

    RECT client{};
    GetClientRect(hwnd, &client);
    // 圆角半径随 DPI 缩放：面板尺寸缩了、圆角没缩的话，高 DPI 下面板
    // 看起来会"方"一圈，和系统其它控件不是一路。
    const int radius = MulDiv(
        kOverlayCornerRadius, static_cast<int>(GetDpiForWindow(hwnd)), 96);
    ui::FillRoundRect(dc, client, radius, ui::kSurface, ui::kBorder);

    const int seconds = state ? state->secondsLeft : 0;
    wchar_t number[16]{};
    swprintf_s(number, L"%d", seconds);

    HFONT big = ui::MakeFont(GetDpiForWindow(hwnd), 30, FW_SEMIBOLD);
    // 变量名不能叫 small：capture.h 会拉进 <objidl.h>，而那个头文件把
    // small 定义成了 char 的宏（Windows SDK 的老毛病），一写就变成
    // "HFONT char = ..." 编译不过。
    HFONT hintFont = ui::MakeFont(GetDpiForWindow(hwnd), 9);

    if (big) {
        const HGDIOBJ oldFont = SelectObject(dc, big);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ui::kText);
        RECT numberRect{client.left, client.top + 6, client.right, client.top + 62};
        DrawTextW(dc, number, -1, &numberRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, oldFont);
        DeleteObject(big);
    }

    if (hintFont) {
        const HGDIOBJ oldFont = SelectObject(dc, hintFont);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ui::kMutedText);
        RECT hintRect{client.left, client.top + 60, client.right, client.bottom - 6};
        DrawTextW(dc, L"秒后开始截图 · 点击取消", -1, &hintRect,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, oldFont);
        DeleteObject(hintFont);
    }

    EndPaint(hwnd, &ps);
}

// 取出（并清空）某个覆盖层窗口自己的状态指针。
// 状态必须绑定在窗口上，而不是只放在全局变量里：倒计时进行中再次触发
// "延时截图"时，旧窗口的取消消息会晚于新窗口的创建才被消息泵处理，
// 那时全局 g_state 已经指向新实例。若按全局指针释放，就会释放新实例的
// 状态（悬垂指针 + 泄漏旧实例）。
OverlayState* TakeOverlayState(HWND hwnd) {
    auto* state = reinterpret_cast<OverlayState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    return state;
}

// 数一数区域里还有多少像素是覆盖层自己的颜色。
// 返回 -1 表示这块画面拿不到（抓屏失败），此时无法判断，只能退回固定等待。
long CountOverlayColours(const RECT& region) {
    const int width = region.right - region.left;
    const int height = region.bottom - region.top;
    if (width <= 0 || height <= 0) {
        return -1;
    }
    HBITMAP bitmap = CaptureRect(region.left, region.top, width, height);
    if (!bitmap) {
        return -1;
    }
    std::vector<BYTE> pixels;
    const bool read = ReadBitmapPixels(bitmap, width, height, pixels);
    DeleteObject(bitmap);
    if (!read) {
        return -1;
    }
    long count = 0;
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
    for (int y = 0; y < height; ++y) {
        const BYTE* row = pixels.data() + static_cast<std::size_t>(y) * rowBytes;
        for (int x = 0; x < width; ++x) {
            const BYTE* pixel = row + static_cast<std::size_t>(x) * 4;
            // 读出来的字节是 B、G、R、保留位的顺序。
            const COLORREF colour = RGB(pixel[2], pixel[1], pixel[0]);
            if (colour == ui::kSurface || colour == ui::kBorder) {
                ++count;
            }
        }
    }
    return count;
}

// 这次重绘请求是为什么发的。
enum class RepaintReason {
    // 覆盖层刚被销毁：截图马上就要发生，必须让那块像素立刻退回去。
    kAfterDestroy,
    // 轮询等待期间：那块区域还没退干净，再催一次。
    kDuringPoll,
};

// 交给 Win32 的那个矩形：顺带把"这次请求发生过"记下来。
//
// 它是**实参表达式的一部分**，不是调用点旁边的旁注。这一点很关键：
//  - 计数写在调用点旁边的话，把 API 调用整段删掉，计数照样在涨
//    （上一轮验收实测：删掉调用 → 49 项全绿）；
//  - 记录写成"函数入参"的话，把**实参**换成 nullptr / erase=TRUE 时，
//    运行时测试照样全绿（上一轮验收实测的 M3b / M3c 两条假绿：
//    记下来的入参非空、erase=FALSE，而真正传给 Win32 的却是
//    整块桌面 + 擦背景）。
//
// 绕开这个函数，就等于没有这次请求——而那正是要被抓住的。
const RECT* RepaintTarget(const RECT* region) {
#ifdef SNAPLITE_DELAY_TEST_HOOKS
    ++g_lastRepaintCallCount;
    g_lastRepaintTargetWasNull = (region == nullptr);
    g_lastRepaintRegion = (region != nullptr) ? *region : RECT{};
#endif
    return region;
}

// 交给 Win32 的"要不要擦背景"标志：同样在实参表达式里。
//
// 单独一个计数器，是为了让"这次请求有没有走这条路"能被验证：
// 擦背景的标志如果被直接写成 TRUE，这个函数就不会被调用，
// 两边的计数对不上，运行时测试当场红。
//
// 记下来的必须是**这个函数真正返回的那个值**，不是写死的 FALSE：
// 写死的话，"把返回值改成 TRUE"这种改动守卫完全看不见
// （上一轮验收实测的假绿类型之一）。
BOOL RepaintEraseFlag() {
    // 传给 Win32 的就是这个值：擦背景会让别的显示器跟着闪（用户实测）。
    const BOOL eraseBackground = FALSE;
#ifdef SNAPLITE_DELAY_TEST_HOOKS
    ++g_lastRepaintEraseFlagCount;
    g_lastRepaintErased = (eraseBackground != FALSE);
#endif
    return eraseBackground;
}

// 真正向桌面发出一次重绘请求。返回"系统有没有接受这次请求"。
//
// 单独拆成函数、并且**由 API 的真实返回值**决定成败，是为了堵住一个很隐蔽的
// 假绿：以前计数是"调用之后无条件 ++"，写在调用点旁边。于是把
// InvalidateRect 整段删掉之后，计数照样在涨，运行时测试 49 项全过——
// 它验的是"计数器涨了"，不是"API 真被调用过"（上一轮验收实测）。
//
// 这里返回的就是 API 的真实返回值：把调用删掉（或让它失败），返回
// false，计数不涨，测试当场红。
bool RepaintDesktopRegion(const RECT& region) {
    if (region.right <= region.left || region.bottom <= region.top) {
        // 空区域：等于让 explorer 重绘整块虚拟桌面（别的屏会跟着闪）。
        return false;
    }
    // 只作废这一小块，而且**不擦背景**。
    //
    // 桌面窗口横跨所有显示器：整块重绘或者带擦除的重绘，explorer 会把
    // 整块虚拟桌面（含别的屏）重画一遍——用户在另一块屏上干活时会被闪到
    // （用户实测：测试在桌面一跑，桌面二跟着闪）。擦背景还会引起壁纸黑闪。
    //
    // 以前这里是 InvalidateRect + RedrawWindow 两个调用，功能重复
    // （RDW_INVALIDATE 等价于 InvalidateRect(..., FALSE)），对同一块区域
    // 作废两次只会让 explorer 多干一遍活。留一个就够。
    //
    // 两个实参都走记录器：见 RepaintTarget / RepaintEraseFlag 的说明。
    const BOOL invalidated = InvalidateRect(
        GetDesktopWindow(), RepaintTarget(&region), RepaintEraseFlag());
#ifdef SNAPLITE_DELAY_TEST_HOOKS
    // 模拟"Windows 拒绝了这次请求"：**返回值**必须跟着变。
    //
    // 注意这里不跳过真正的 API 调用——要验的是"返回值会不会传到计数上"，
    // 不是"调用还发不发出"。真正的调用照发，桌面照常重画，测试不会
    // 因为这一下而超时。
    const BOOL effective = g_forceRepaintFailure ? FALSE : invalidated;
#else
    const BOOL effective = invalidated;
#endif
    return effective != FALSE;
}

// 催桌面把那一块区域重绘回来。
//
// reason 用来区分"销毁之后催的那一次"和"轮询期间催的那些"：
// 计数器必须记在**函数体内**，不能记在调用点旁边——记在旁边的话，
// 把这里的请求整段删掉，计数照样在涨，运行时测试就看不出产品到底
// 有没有真的催过。
void RequestDesktopRepaint(const RECT& region, RepaintReason reason) {
    const bool requested = RepaintDesktopRegion(region);
#ifdef SNAPLITE_DELAY_TEST_HOOKS
    // 只有请求**真的发出去了**才计数。
    if (!requested) {
        return;
    }
    if (reason == RepaintReason::kDuringPoll) {
        ++g_lastPollRepaintCount;
        // 本次等待里催成了几次。
        //
        // 必须**由 API 的返回值驱动**（和 g_lastPollRepaintCount 同一个
        // 闸门后面）：数"打算催"而不是"催成了"，就会出现"请求被 Windows
        // 拒绝了、计数照样涨"这种自欺（上一轮验收实测：这一条没有任何
        // 交叉校验，审阅 agent 指出的 P7）。
        ++g_lastWaitPollRepaintCount;
    } else {
        ++g_lastDestroyRepaintCount;
    }
    // 这里**不**再记区域：区域由 RepaintTarget 记——它记的是**真正交给
    // Win32 的那个矩形**。在这里再记一次，等于把"传参"重新盖回"实参"
    // 上面：把实参换成整块桌面时，记录会被改回参数值，运行时断言又变成
    // 恒真（上一轮验收指出的那条问题）。
#else
    (void)reason;
    (void)requested;
#endif
}

// 等桌面把那一块像素重绘回来。
//
// 原来这里是"睡固定 120ms 去赌 explorer 已经画完"：机器繁忙时那点时间
// 经常不够，截图就会把覆盖层拍进去（实测负载高时出现过）。现在改成有
// 上限的轮询——不靠赌，靠验证：那块区域里覆盖层自己的颜色退干净了才算完。
//
// 返回 true 表示已经退干净；false 表示等到超时还有残留。
bool WaitForOverlayColoursGoneImpl(
    const RECT& region,
    long baselineColours,
    DWORD timeoutMs,
    long residueTolerance) {
#ifdef SNAPLITE_DELAY_TEST_HOOKS
    // 记下"这一次等得怎么样"。产品路径走的是哪条分支（验证 / 赌 / 超时）
    // 从外面完全看不出来，测试只能问它。
    ++g_waitCallCount;
    g_lastWaitElapsedMs = 0;
    g_lastWaitTimedOut = false;
    g_lastWaitUsedFixedSleepFallback = false;
    g_lastWaitTimeoutMs = timeoutMs;
    // 本次等待的计数一律从零开始：留上一轮的数字会让判据永远成立。
    g_lastWaitPollRepaintCount = 0;
    g_lastPollNudgeGapMs = 0;
    g_lastWaitRestMs = 0;
    g_lastWaitSampleCount = 0;
    g_repaintNudgeIntervalMs = kRepaintNudgeIntervalMs;
    g_lastWaitRestIntervalMs = kRepaintPollMs;
    const ULONGLONG waitStartedAt = GetTickCount64();
#endif
    if (region.right <= region.left || region.bottom <= region.top || baselineColours < 0) {
        // 基线拿不到就没法判断残留，退回固定等待。
#ifdef SNAPLITE_DELAY_TEST_HOOKS
        g_lastWaitUsedFixedSleepFallback = true;
#endif
        Sleep(kRepaintWaitMs);
#ifdef SNAPLITE_DELAY_TEST_HOOKS
        g_lastWaitElapsedMs = static_cast<DWORD>(GetTickCount64() - waitStartedAt);
#endif
        return true;
    }
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    ULONGLONG lastRepaintAt = 0;
#ifdef SNAPLITE_DELAY_TEST_HOOKS
    // 相邻两次催重绘隔了多久：限频有没有真的生效，就看这个数。
    ULONGLONG lastNudgeAt = 0;
    bool sawNudge = false;
#endif
    for (;;) {
#ifdef SNAPLITE_DELAY_TEST_HOOKS
        // 每轮看一次像素，就记一次——次数是"有没有空转"的直接证据。
        ++g_lastWaitSampleCount;
#endif
        const long remaining = CountOverlayColours(region);
        if (remaining >= 0 && remaining <= baselineColours + residueTolerance) {
#ifdef SNAPLITE_DELAY_TEST_HOOKS
            g_lastWaitElapsedMs = static_cast<DWORD>(GetTickCount64() - waitStartedAt);
#endif
            return true;
        }
        if (remaining < 0) {
            // 画面抓不到：判断不了残留，也**不该在这儿空转到超时**。
            // 抓不到画面的话，随后那次抓屏一样拿不到东西，
            // 干等 800ms 只是白把界面冻住。
#ifdef SNAPLITE_DELAY_TEST_HOOKS
            g_lastWaitUsedFixedSleepFallback = true;
#endif
            Sleep(kRepaintWaitMs);
#ifdef SNAPLITE_DELAY_TEST_HOOKS
            g_lastWaitElapsedMs = static_cast<DWORD>(GetTickCount64() - waitStartedAt);
#endif
            return true;
        }
        if (GetTickCount64() >= deadline) {
            // 兜底：不能无限等下去，否则截图永远不发生。
#ifdef SNAPLITE_DELAY_TEST_HOOKS
            g_lastWaitTimedOut = true;
            g_lastWaitElapsedMs = static_cast<DWORD>(GetTickCount64() - waitStartedAt);
#endif
            return false;
        }
        // 再催一次桌面重绘，然后歇一下再看。
        //
        // 催重绘必须**限频**：每 30ms 催一次的话，800ms 上限内能催二十多次。
        // 那一串重绘在多屏或远程桌面下会被放大成明显的闪屏（用户能看到
        // 另一块显示器跟着闪）。所以改成"距上次催至少 200ms 才再催一次"。
        const ULONGLONG now = GetTickCount64();
        if (now - lastRepaintAt >= kRepaintNudgeIntervalMs) {
            lastRepaintAt = now;
#ifdef SNAPLITE_DELAY_TEST_HOOKS
            // 限频有没有生效，就看相邻两次催重绘隔了多久。
            //
            // 记的是**间隔本身**（now - lastNudgeAt），而且用的是同一个
            // now：产品那条判断就是 `now - lastRepaintAt >= 间隔`，
            // 所以量出来的间隔必然 >= 那个间隔（上一轮验收实测：去掉
            // 限频之后这里会掉到 30ms 上下，也就是轮询歇息的时长）。
            if (sawNudge) {
                const ULONGLONG gap = now - lastNudgeAt;
                if (g_lastPollNudgeGapMs == 0 || gap < g_lastPollNudgeGapMs) {
                    g_lastPollNudgeGapMs = static_cast<DWORD>(gap);
                }
            }
            lastNudgeAt = now;
            sawNudge = true;
#endif
            RequestDesktopRepaint(region, RepaintReason::kDuringPoll);
        }
#ifdef SNAPLITE_DELAY_TEST_HOOKS
        // 歇息时长是**量出来**的：记在 Sleep 的前后，而不是"调用旁边
        // 无条件 ++"。把那行 Sleep 删掉，这里量到的时长就是 0，而计数
        // 照样在涨——这正是上一轮验收实测到的那种假绿。
        const ULONGLONG restStartedAt = GetTickCount64();
#endif
        Sleep(kRepaintPollMs);
#ifdef SNAPLITE_DELAY_TEST_HOOKS
        const ULONGLONG restFinishedAt = GetTickCount64();
        g_lastWaitRestMs += static_cast<DWORD>(restFinishedAt - restStartedAt);
#endif
    }
}

// 销毁覆盖层，并等桌面把那一块像素重绘回来。
//
// 顺序很关键：截图一定发生在覆盖层消失之后。覆盖层不是分层窗口
// （WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE），屏幕抓取一定会
// 把它拍进去——延时时按 F1 走普通截图路径时，截出来的图顶部中央会多一个
// 倒计时数字框。桌面窗口属于 explorer 进程，重绘由它自己调度，所以这里
// 只能主动作废那一块区域，再等它真的画回来。
void DestroyOverlayAndWaitForRepaint(
    HWND hwnd,
    const RECT& covered,
    long baselineColours,
    long residueTolerance) {
    DestroyWindow(hwnd);
    // 只请求重绘覆盖层原来占的那一小块。原来这里传的是 nullptr，等于
    // 让 explorer 重绘**整块桌面**：机器繁忙时这一下反而更慢，截图就
    // 可能把覆盖层拍进去（实测负载高时出现过）。
    RequestDesktopRepaint(covered, RepaintReason::kAfterDestroy);
    WaitForOverlayColoursGoneImpl(
        covered, baselineColours, kRepaintTimeoutMs, residueTolerance);
}

void FinishOverlay(HWND hwnd, bool notify) {
    OverlayState* state = TakeOverlayState(hwnd);
    const HWND notifyWindow = state ? state->notifyWindow : nullptr;
    const UINT notifyMessage = state ? state->notifyMessage : 0;
    // 区域与基线要在释放状态之前取出来：等重绘时还要用它们判断
    // "覆盖层真的退干净了没有"。
    const RECT covered = state ? state->covered : RECT{};
    const long baselineColours = state ? state->baselineOverlayColours : -1;
    const long residueTolerance = state ? state->residueTolerance : kOverlayResidueTolerance;

    // 只有当全局指针指向的就是本窗口的状态时才清空，
    // 否则会把后建的覆盖层实例的指针误清掉。
    if (g_state == state) {
        g_state = nullptr;
    }
    if (g_overlay == hwnd) {
        g_overlay = nullptr;
    }
    delete state;

    // 先销毁覆盖层并等桌面重绘，最后才通知调用方去截图。
    DestroyOverlayAndWaitForRepaint(hwnd, covered, baselineColours, residueTolerance);

    if (notify && notifyWindow && IsWindow(notifyWindow)) {
        PostMessageW(notifyWindow, notifyMessage, 0, 0);
    }
}

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    OverlayState* state = reinterpret_cast<OverlayState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
    case WM_NCCREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_TIMER: {
        if (state) {
            // 覆盖层是 WS_EX_NOACTIVATE，键盘焦点不在它身上，收不到按键消息。
            // 这里用异步按键状态侦测 Esc：只读不消费，不会影响用户正在操作的
            // 其它窗口，但仍然可以取消倒计时。
            const bool escapeDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
            if (escapeDown && !state->escapeWasDown) {
                state->escapeWasDown = true;
                FinishOverlay(hwnd, false);
                return 0;
            }
            state->escapeWasDown = escapeDown;

            // 按截止时间算剩余秒数，不靠"每次 tick 减一"，否则计时频率一变
            // 倒计时就失准。
            const ULONGLONG now = GetTickCount64();
            const ULONGLONG remaining = state->deadline > now ? state->deadline - now : 0;
            const int seconds = static_cast<int>((remaining + 999) / 1000);
            if (seconds <= 0) {
                FinishOverlay(hwnd, true);
                return 0;
            }
            if (seconds != state->secondsLeft) {
                state->secondsLeft = seconds;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_PAINT:
        PaintOverlay(hwnd, state);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
    case WM_KEYDOWN:
        // 点击或按 Esc 取消倒计时。
        if (message == WM_KEYDOWN && wParam != VK_ESCAPE && wParam != VK_SPACE) break;
        FinishOverlay(hwnd, false);
        return 0;
    case WM_DESTROY: {
        // 覆盖层被外部销毁（例如 Cancel 直接销毁窗口）时在这里兜底释放状态，
        // 避免泄漏；状态已经取走时 state 为空，不做二次释放。
        OverlayState* dead = TakeOverlayState(hwnd);
        if (g_state == dead) {
            g_state = nullptr;
        }
        if (g_overlay == hwnd) {
            g_overlay = nullptr;
        }
        delete dead;
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// 取某块显示器的 DPI。
//
// GetDpiForWindow 只能问"某个窗口所在屏"的 DPI，这里要问的是光标所在
// 屏。用 GetDpiForMonitor（shcore.dll，Win8.1+）：动态加载，老系统上
// 拿不到就返回 0，调用方退回自己的来源。
constexpr int kMdtEffectiveDpi = 0;

UINT MonitorDpi(HMONITOR monitor) {
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static GetDpiForMonitorFn getDpiForMonitor = []() -> GetDpiForMonitorFn {
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        if (!shcore) return nullptr;
        return reinterpret_cast<GetDpiForMonitorFn>(
            reinterpret_cast<void*>(GetProcAddress(shcore, "GetDpiForMonitor")));
    }();
    if (!getDpiForMonitor) return 0;

    UINT dpiX = 0;
    UINT dpiY = 0;
    if (FAILED(getDpiForMonitor(monitor, kMdtEffectiveDpi, &dpiX, &dpiY))) {
        return 0;
    }
    return dpiX;
}

bool EnsureOverlayClass(HINSTANCE instance) {
    static bool registered = false;
    if (registered) return true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kOverlayClass;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    registered = true;
    return true;
}

}  // namespace

#ifdef SNAPLITE_DELAY_TEST_HOOKS
COLORREF DelayOverlay::SurfaceColour() { return ui::kSurface; }

COLORREF DelayOverlay::BorderColour() { return ui::kBorder; }

long DelayOverlay::LastBaselineColours() { return g_lastBaselineColours; }

bool DelayOverlay::WaitForOverlayColoursGone(
    const RECT& region,
    long baselineColours,
    DWORD timeoutMs) {
    return WaitForOverlayColoursGoneImpl(
        region, baselineColours, timeoutMs, kOverlayResidueTolerance);
}

unsigned DelayOverlay::LastWaitCallCount() { return g_waitCallCount; }

DWORD DelayOverlay::LastWaitElapsedMs() { return g_lastWaitElapsedMs; }

bool DelayOverlay::LastWaitTimedOut() { return g_lastWaitTimedOut; }

bool DelayOverlay::LastWaitUsedFixedSleepFallback() {
    return g_lastWaitUsedFixedSleepFallback;
}

unsigned DelayOverlay::LastDestroyRepaintCount() { return g_lastDestroyRepaintCount; }

unsigned DelayOverlay::LastPollRepaintCount() { return g_lastPollRepaintCount; }

RECT DelayOverlay::LastRepaintRegion() { return g_lastRepaintRegion; }

unsigned DelayOverlay::LastRepaintCallCount() { return g_lastRepaintCallCount; }

unsigned DelayOverlay::LastRepaintEraseFlagCount() {
    return g_lastRepaintEraseFlagCount;
}

bool DelayOverlay::LastRepaintTargetWasNull() { return g_lastRepaintTargetWasNull; }

bool DelayOverlay::LastRepaintErased() { return g_lastRepaintErased; }

void DelayOverlay::ForceRepaintFailure(bool forced) { g_forceRepaintFailure = forced; }

DWORD DelayOverlay::LastWaitTimeoutMs() { return g_lastWaitTimeoutMs; }

long DelayOverlay::LastResidueTolerance() { return g_lastResidueTolerance; }

DWORD DelayOverlay::LastRepaintNudgeIntervalMs() {
    return g_repaintNudgeIntervalMs;
}

unsigned DelayOverlay::LastWaitPollRepaintCount() {
    return g_lastWaitPollRepaintCount;
}

DWORD DelayOverlay::LastPollNudgeGapMs() { return g_lastPollNudgeGapMs; }

DWORD DelayOverlay::LastWaitRestIntervalMs() { return g_lastWaitRestIntervalMs; }

DWORD DelayOverlay::LastWaitRestMs() { return g_lastWaitRestMs; }

unsigned DelayOverlay::LastWaitSampleCount() { return g_lastWaitSampleCount; }

DelayOverlay::StartFailure DelayOverlay::LastStartFailure() {
    return static_cast<StartFailure>(g_lastStartFailure);
}
#endif

bool DelayOverlay::Start(
    HINSTANCE instance,
    int seconds,
    HWND notifyWindow,
    UINT notifyMessage) {
#ifdef SNAPLITE_DELAY_TEST_HOOKS
    // 每次启动都把上一轮的记录清掉：测试断言的是"这一次"的表现，
    // 留着上一轮的旧值会让判据永远成立。
    g_lastBaselineColours = -1;
    g_waitCallCount = 0;
    g_lastWaitElapsedMs = 0;
    g_lastWaitTimedOut = false;
    g_lastWaitUsedFixedSleepFallback = false;
    g_lastDestroyRepaintCount = 0;
    g_lastPollRepaintCount = 0;
    g_lastStartFailure = static_cast<int>(StartFailure::kNone);
    g_lastRepaintRegion = RECT{};
    g_lastRepaintTargetWasNull = false;
    g_lastRepaintErased = false;
    g_lastRepaintCallCount = 0;
    g_lastRepaintEraseFlagCount = 0;
    g_lastWaitTimeoutMs = 0;
    g_lastResidueTolerance = 0;
    g_repaintNudgeIntervalMs = 0;
    g_lastWaitPollRepaintCount = 0;
    g_lastPollNudgeGapMs = 0;
    g_lastWaitRestIntervalMs = 0;
    g_lastWaitRestMs = 0;
    g_lastWaitSampleCount = 0;
#endif
    if (seconds < 1 || seconds > 60 || !notifyWindow) {
#ifdef SNAPLITE_DELAY_TEST_HOOKS
        g_lastStartFailure = static_cast<int>(StartFailure::kInvalidArguments);
#endif
        return false;
    }

    Cancel();

    if (!EnsureOverlayClass(instance)) {
#ifdef SNAPLITE_DELAY_TEST_HOOKS
        g_lastStartFailure = static_cast<int>(StartFailure::kClassRegistration);
#endif
        return false;
    }

    auto* state = new OverlayState();
    state->secondsLeft = seconds;
    state->deadline = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000ULL;
    state->notifyWindow = notifyWindow;
    state->notifyMessage = notifyMessage;
    // 记录启动瞬间 Esc 是否已经按着：用户本来就按着 Esc 时不该被当成
    // "刚按下 Esc"而立刻取消倒计时。
    state->escapeWasDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;

    // DPI 与落位必须来自**同一块**显示器。
    //
    // 用**光标所在**的那块屏：延时截图是全局快捷键触发的，用户在哪块屏
    // 干活、鼠标通常就在哪块屏，倒计时面板就该出现在那儿。
    //
    // 以前这里用的是 notifyWindow 所在屏，而那个窗口是钉在 (0,0) 的隐藏
    // 窗口、永远属于主屏——用户在副屏干活时，倒计时跑到了另一块屏上
    // （用户双屏环境实测）。DPI 也从同一块屏取：两块屏 DPI 不同时，
    // 按 A 屏的 DPI 量尺寸、摆到 B 屏上，面板会大一号或小一号。
    HMONITOR monitor = nullptr;
    POINT cursor{};
    if (GetCursorPos(&cursor)) {
        monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    }
    UINT dpi = monitor ? MonitorDpi(monitor) : 0;
    if (!monitor || dpi == 0) {
        // 拿不到光标位置（或那块屏的 DPI）：退回通知窗口所在屏。
        // 这条路径上 DPI 与落位同样同源。
        monitor = MonitorFromWindow(notifyWindow, MONITOR_DEFAULTTOPRIMARY);
        dpi = GetDpiForWindow(notifyWindow);
    }
    const int width = MulDiv(kOverlayWidth, static_cast<int>(dpi), 96);
    const int height = MulDiv(kOverlayHeight, static_cast<int>(dpi), 96);

    // 残留容忍度按 DPI 缩放。照搬 96 DPI 下的 64 像素，高 DPI 下就永远
    // 等不满（圆角更大、抗锯齿边缘更长），后果是每次延时截图都吃满
    // 800ms 上限，把界面冻住。
    state->residueTolerance =
        MulDiv(kOverlayResidueTolerance, static_cast<int>(dpi), 96);

    // 定位到那块显示器顶部居中：延时截图常用于截某个刚出现的界面，
    // 覆盖层不能挡住用户正在看的内容。
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);
    const int x = info.rcWork.left + (info.rcWork.right - info.rcWork.left - width) / 2;
    const int y = info.rcWork.top + MulDiv(kOverlayTopOffset, static_cast<int>(dpi), 96);

    // 覆盖层出现之前，先把这块区域"本来是什么样"记下来（基线）。
    // 桌面壁纸里可能正好有和覆盖层一样的颜色，没有基线就分不清
    // "残留的是覆盖层"和"本来就有的"。基线要在窗口创建之前取，
    // 否则取到的已经是覆盖层自己的颜色了。
    const RECT covered{x, y, x + width, y + height};
    state->covered = covered;
    state->baselineOverlayColours = CountOverlayColours(covered);

    HWND overlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kOverlayClass,
        L"Snap-Lite · 延时截图",
        WS_POPUP,
        x,
        y,
        width,
        height,
        nullptr,
        nullptr,
        instance,
        state);
    if (!overlay) {
#ifdef SNAPLITE_DELAY_TEST_HOOKS
        g_lastStartFailure = static_cast<int>(StartFailure::kWindowCreation);
#endif
        delete state;
        return false;
    }

#ifdef SNAPLITE_DELAY_TEST_HOOKS
    // 基线**只在覆盖层真的建出来之后**才对外可见：启动失败时留着一个
    // "半途"的基线，语义含糊（读的人分不清那是这次的还是上次的）。
    g_lastBaselineColours = state->baselineOverlayColours;
    // 残留容忍度也是产品按 DPI 算出来的，测试不该自己写死一个数
    // （高 DPI 上产品容忍 96、测试只允许 64，合法结果会被判红）。
    g_lastResidueTolerance = state->residueTolerance;
#endif

    g_overlay = overlay;
    g_state = state;
    ui::SetRoundedWindowRegion(
        overlay, MulDiv(kOverlayCornerRadius, static_cast<int>(dpi), 96));
    ShowWindow(overlay, SW_SHOWNOACTIVATE);
    UpdateWindow(overlay);
    SetTimer(overlay, kCountdownTimer, kTickMs, nullptr);
    return true;
}

void DelayOverlay::Cancel() {
    HWND overlay = g_overlay;
    OverlayState* state = g_state;
    g_overlay = nullptr;
    g_state = nullptr;
    if (overlay && IsWindow(overlay)) {
        // 直接销毁而不是投递消息：投递的消息要等消息泵才处理，期间旧实例还活着，
        // 而新的倒计时已经开始，两个实例会同时占用全局状态。
        // 销毁会同步走到 WM_DESTROY，状态在那里释放。
        //
        // 同样要等桌面重绘：取消后如果马上截图（延时倒计时期间按 F1），
        // 覆盖层还在屏幕上就会被拍进截图里。
        const RECT covered = state ? state->covered : RECT{};
        const long baselineColours = state ? state->baselineOverlayColours : -1;
        const long residueTolerance = state ? state->residueTolerance : kOverlayResidueTolerance;
        DestroyOverlayAndWaitForRepaint(overlay, covered, baselineColours, residueTolerance);
        // 状态由 WM_DESTROY 释放，这里不能 delete。
    } else {
        // 窗口已经不在了（异常路径），这里兜底释放状态，避免泄漏。
        delete state;
    }
}

}  // namespace snaplite
