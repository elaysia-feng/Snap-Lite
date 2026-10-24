#pragma once

#include <windows.h>

namespace snaplite {

// 延时截图的倒计时覆盖层。
//
// 屏幕顶部居中显示剩余秒数；点击覆盖层或按 Esc 取消；倒计时归零后向
// notifyWindow 发送 notifyMessage，由调用方开始截图。
class DelayOverlay {
public:
    // seconds 取值 1..60。notifyMessage 会带 0 参数发给 notifyWindow。
    // 返回 false 表示覆盖层无法创建（调用方应直接开始截图，不要静默失败）。
    static bool Start(HINSTANCE instance, int seconds, HWND notifyWindow, UINT notifyMessage);

    // 取消正在进行的倒计时（不会发送 notifyMessage）。
    static void Cancel();

#ifdef SNAPLITE_DELAY_TEST_HOOKS
    // ===== 测试钩子 =====
    //
    // 这些接口只为运行时回归测试存在，**只在定义了 SNAPLITE_DELAY_TEST_HOOKS
    // 的编译单元里出现**：产品目标（SnapLite.exe）不定义这个宏，所以这些
    // 符号不会进产品二进制，也就不会在产品上留下"为测试而存在的公开 API"。
    //
    // 它们存在的理由不是"方便写测试"，而是**可观测性**：
    // 没有它们，"产品到底有没有等重绘""基线到底采没采到""是不是退回了
    // 睡固定时长去赌"这些事从外面完全看不出来。测试只能验一条旁路
    // helper 的性质，验不出产品的行为——把产品路径上的等待整段摘掉，
    // 旧测试照样全绿。
    //
    // 所以这里暴露的是**决策**（走了哪条分支、等了多久），不是可以随手
    // 写死的数值。

    // 启动失败的原因。
    enum class StartFailure {
        kNone = 0,
        kInvalidArguments = 1,
        kClassRegistration = 2,
        kWindowCreation = 3,
    };

    // 覆盖层自绘用的两个颜色。
    //
    // 由实现返回、而不是让测试各写一份常量：常量一旦分叉，测试的判据
    // 就失效了——而"一直绿的失效判据"比没有判据更糟。
    static COLORREF SurfaceColour();
    static COLORREF BorderColour();

    // 最近一次 Start() 采到的基线（覆盖层同色像素数）。
    //
    // 返回 -1 表示基线**没采到**（抓不到覆盖层那块画面），此时产品会
    // 退回"睡固定时长去赌桌面画完了"。暴露出来是为了让测试能断言
    // "基线确实采到了"——采不到时，"靠验证代替赌"这条改动等于没生效，
    // 而从外面看不出任何区别。
    static long LastBaselineColours();

    // 直接驱动等待实现（绕开产品路径），供测试验证等待逻辑本身的性质。
    static bool WaitForOverlayColoursGone(
        const RECT& region,
        long baselineColours,
        DWORD timeoutMs);

    // 最近一次等待的实测结果。
    //
    // - LastWaitCallCount：等待实现被调用过几次（产品路径删掉调用 → 0）；
    // - LastWaitElapsedMs：最近一次等待实际花了多久（不等待 → 0）；
    // - LastWaitTimedOut：最近一次等待是不是等满了上限才放弃；
    // - LastWaitUsedFixedSleepFallback：是不是退回了"睡固定时长去赌"
    //   （基线拿不到、或画面抓不到时才会走这条路）。
    static unsigned LastWaitCallCount();
    static DWORD LastWaitElapsedMs();
    static bool LastWaitTimedOut();
    static bool LastWaitUsedFixedSleepFallback();

    // 催桌面重绘的次数。
    //
    // - LastDestroyRepaintCount：覆盖层销毁后主动催的那几次；
    // - LastPollRepaintCount：轮询等待期间催的那几次。
    //
    // 这两个计数只在**请求真的发出去了**之后才涨：计数由 Win32 API 的
    // 返回值驱动，不是"调用之后无条件 ++"。把重绘调用整段删掉，计数
    // 就不涨，运行时测试当场红——否则它验的只是"计数器涨了"，
    // 不是"桌面真的被要求重画了"（上一轮验收实测：删掉 API 调用，
    // 49 项断言照样全过）。
    static unsigned LastDestroyRepaintCount();
    static unsigned LastPollRepaintCount();

    // 最近一次**成功发出**的重绘请求所针对的区域。
    //
    // 这是"产品到底让桌面重画了哪一块"的外部证据：结果上完全看不出
    // 区别（画面最后都会恢复正常），但区域若是空的，就意味着 explorer
    // 被要求重画整块虚拟桌面——别的显示器会跟着闪（用户实测过）。
    // 没成功发过请求时返回全 0。
    static RECT LastRepaintRegion();

    // 这次重绘请求**真正交给 Win32 的是什么**。
    //
    // 只报"入参"是不够的：入参与真正传下去的可以是两个东西——把实参
    // 换成 nullptr（整块桌面重画）或 erase=TRUE（擦背景）时，记下来的
    // 入参仍然"非空、erase=FALSE"，运行时测试于是全绿（上一轮验收实测
    // 的两条假绿）。
    //
    // 所以产品把记录器**放在实参表达式里**：
    //  - LastRepaintCallCount：真正发起过多少次重绘请求；
    //  - LastRepaintEraseFlagCount：其中多少次是"带着被记录的擦除标志"
    //    发出的——它必须与上面那个计数相等，相等就说明每次请求都走了
    //    被记录的那条路（直接写死 TRUE 绕过记录器时，两者会不等）；
    //  - LastRepaintTargetWasNull：交给 Win32 的矩形是不是空的
    //    （空 = 整块桌面重画 = 别的显示器跟着闪）；
    //  - LastRepaintErased：有没有让系统擦背景（跨屏闪屏的直接原因）。
    static unsigned LastRepaintCallCount();
    static unsigned LastRepaintEraseFlagCount();
    static bool LastRepaintTargetWasNull();
    static bool LastRepaintErased();

    // 让"Windows 拒绝了这次重绘请求"可以被造出来（只对测试开放）。
    //
    // 打开之后，重绘请求**照发**，但产品会把它当成"被拒绝了"：返回值变成
    // 失败，计数因此不涨。这样测试就能问出一个只有产品能回答的问题：
    // "如果那次请求被拒绝了，你还算催过重绘吗？"
    //
    // 这条是"计数由 API 返回值驱动"的运行时证据。静态断言只能看出
    // "代码长得像那么回事"，看不出"实际上无条件 return true"——上一轮
    // 验收实测的漏判（调 API 但忽略返回值，守卫全绿）就是这么漏过去的。
    static void ForceRepaintFailure(bool forced);

    // 最近一次等待用的上限（毫秒）与残留容忍度（像素）。
    //
    // 都由产品决定：测试写死一个数（比如 800ms / 64 像素）的话，产品
    // 合法地调一下参数就会让测试假红。
    // - LastWaitTimeoutMs：等待的上限；
    // - LastResidueTolerance：残留容忍度（按 DPI 缩放过的那个值）。
    static DWORD LastWaitTimeoutMs();
    static long LastResidueTolerance();

    // 催桌面重绘的**限频间隔**（毫秒）与最近一次等待里的催重绘情况。
    //
    // 这是用户实测过的那个问题的直接修复点："在桌面一测试居然能影响到
    // 我桌面二，桌面二在闪"。桌面窗口横跨所有显示器，一串密集的重绘
    // 请求下去，别的屏也会跟着重画。所以产品对催重绘限频：距上次催至少
    // 隔一段时间才再催一次。
    //
    // 上一轮验收实测：这条限频**一条断言都没有**，去掉限频之后运行时
    // 68 项、静态 75 项照样全绿（800ms 窗口内从 4 次变成 18 次）。
    // 所以这里把限频的**决策**和**实际行为**都报出来，让测试能问出
    // "你到底催了几次、间隔多久"：
    // - LastRepaintNudgeIntervalMs：产品用的限频间隔（决策参数);
    // - LastWaitPollRepaintCount：**最近一次等待**里**催成了**几次
    //   （由 Win32 的返回值驱动——请求被拒绝了就不算催成；
    //   数"打算催"会出现"没催成却照样计数"的自欺）；
    // - LastPollNudgeGapMs：最近一次等待里，相邻两次催重绘的最小间隔
    //   （催的次数不足两次时为 0）。
    static DWORD LastRepaintNudgeIntervalMs();
    static unsigned LastWaitPollRepaintCount();
    static DWORD LastPollNudgeGapMs();

    // 轮询期间"歇息"的决策与实际行为。
    //
    // 轮询不能空转：每 30ms 看一次像素，之间必须歇一下。歇息歇没了，
    // 等待就变成烧 CPU 的忙等（上一轮验收实测：删掉那次 Sleep 之后
    // 运行时照样全绿）。
    //
    // 这里报三样：
    // - LastWaitRestIntervalMs：产品打算歇多久（决策参数）；
    // - LastWaitRestMs：最近一次等待里**真的**花在歇息上的总时长
    //   （量出来的，不是数出来的——删掉 Sleep 它就归零）；
    // - LastWaitSampleCount：最近一次等待里取了几次样（每轮一次）。
    static DWORD LastWaitRestIntervalMs();
    static DWORD LastWaitRestMs();
    static unsigned LastWaitSampleCount();

    // 最近一次 Start() 失败的原因（成功时是 kNone）。
    static StartFailure LastStartFailure();
#endif
};

}  // namespace snaplite
