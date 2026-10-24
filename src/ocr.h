#pragma once

#include <windows.h>

#include <string>

namespace snaplite {

struct OcrResult {
    bool success{false};
    std::wstring text;
    std::wstring error;
};

// 一次异步 OCR 请求的上下文，回调期间保证有效。
struct OcrRequest {
    // 发起请求的截图窗口。回调时会检查它是否仍然有效。
    HWND parent{};
    // 发起请求时窗口实例的序号（见 snip_window.h 的 kSnipInstanceIdProp）。
    //
    // 识别是异步的，回来时窗口可能早已销毁；而 HWND 会被系统回收复用，
    // 新截图窗口完全可能拿到同一个句柄。只比 HWND 会把上一次的识别结果
    // 挂到新窗口上，所以还要比实例序号：窗口销毁后属性随之失效，
    // 新窗口会挂上属于它自己的新序号。
    unsigned long long instanceId{};
    // 触发请求时的选区（客户端坐标）。识别的是这份选区的内容，
    // 因此面板会定位在"当初那块选区"旁边，而不是识别结束时的当前选区。
    RECT selection{};
};

// OCR 完成回调：在发起请求的线程（UI 线程）上被调用。
using OcrCompletionCallback = void (*)(const OcrResult& result, const OcrRequest& request);

// 异步提取选区文字：图像预处理与识别都在后台线程执行，完成后回到调用线程
// 触发回调，UI 不会卡住。
//
// 调用方约束：必须从 UI 线程调用；完成回调也在该线程上执行。
// instanceId 是发起窗口的实例序号，回调时会用它校验窗口是否还是同一个实例。
// 返回 false 表示无法启动（例如选区为空或父窗口已失效）。
bool StartTextExtractionFromBitmapRegion(
    HBITMAP bitmap,
    const RECT& region,
    HWND parent,
    unsigned long long instanceId,
    OcrCompletionCallback callback);

// Put UTF-16 text on the Windows clipboard.
bool CopyUnicodeTextToClipboard(HWND owner, const std::wstring& text);

// 回收还没被处理的 OCR 完成消息。
//
// 完成结果是 PostMessage 过去的：进程在消息被处理之前退出，那份结果
// 就泄漏了。退出流程调用一次即可。UI 线程随后就算收到那条消息，
// 也拿不到已经被回收的结果（不会出现二次释放）。
void ReleasePendingOcrCompletions();

}  // namespace snaplite
