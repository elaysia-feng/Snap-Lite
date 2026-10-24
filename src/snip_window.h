#pragma once

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "capture.h"

namespace snaplite {

class SnipWindow;

namespace detail {

// Forward declarations for the themed GDI shims so SnipWindow can friend
// them and grant access to the per-instance shape drawing state. The actual
// implementations live in snip_window.cpp.
HPEN ThemedCreatePen(SnipWindow& owner, int style, int width, COLORREF color);
BOOL ThemedRectangle(SnipWindow& owner, HDC dc, int left, int top, int right, int bottom);
void DrawAdvancedArrow(HDC dc, POINT from, POINT to);

}  // namespace detail

// Stable, macro-immune annotation red. RGB() is hijacked inside snip_window.cpp
// during the .inc include, but this helper always produces the same COLORREF
// regardless of any macro state. Use it for baked-text defaults and other
// cross-TU color constants so the value doesn't drift when annotation themes
// change.
inline constexpr COLORREF kDefaultAnnotationRed() {
    // (r=235, g=70, b=70) packed as COLORREF (0x00BBGGRR).
    return static_cast<COLORREF>((70u << 16) | (70u << 8) | 235u);
}

// 窗口实例序号。
//
// 异步 OCR 回来时必须判断"这份结果还属于当初发起识别的那个窗口"。HWND 会被
// 系统回收复用：旧截图窗口销毁后，新截图窗口完全可能拿到同一个句柄，只比
// HWND 会把上一次的识别结果挂到新窗口上。因此给每个窗口实例发一个进程内
// 唯一的序号，并挂在窗口属性上——窗口销毁时属性自动失效，不会残留。
inline constexpr wchar_t kSnipInstanceIdProp[] = L"SnapLite.InstanceId";

inline unsigned long long NextSnipInstanceId() {
    static unsigned long long counter = 0;
    return ++counter;
}

inline unsigned long long ReadSnipInstanceId(HWND hwnd) {
    if (!hwnd) return 0;
    HANDLE value = GetPropW(hwnd, kSnipInstanceIdProp);
    return value ? static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(value)) : 0;
}

struct TextOverlay {
    std::wstring text;
    POINT origin{};
    COLORREF color{kDefaultAnnotationRed()};
    int sizePt{14};
};

// Undo/redo snapshot stack.
//
// The two most recent snapshots stay as raw HBITMAPs so Ctrl+Z is instant;
// older snapshots are losslessly compressed to PNG in memory. That keeps deep
// undo history without letting raw bitmaps grow toward hundreds of megabytes.
//
// 压缩必须放在后台线程：GDI+ 编码一张 4K 位图要七百多毫秒，1920x1080 也要
// 两百毫秒（实测）。放在 UI 线程上就是"每落一笔冻结零点几秒"——用户能直接
// 感觉到，本来是这里引入的回归。
//
// 后台线程拿到的是位图的**副本**：条目本身必须一直留着可用的位图，
// 否则用户撤销到这一条时既拿不到位图也没有 PNG，撤销就静默失效了。
// UI 线程复制 4K 的一帧约十几毫秒，远好过在 UI 线程上同步编码的七百毫秒。
//
// 每条历史还带着一份"与这一帧一一对应的状态"（Companion，这里是标注
// 列表）。它**存在条目里**，不是另开一个平行的栈。
//
// 以前是两个平行的栈（位图栈 + 标注栈），靠"两边同时 push、同时 pop"
// 保持配对。那样迟早会对不齐：位图栈有 64MB/128 条的裁剪，裁剪是从
// **队首**丢老条目的，而标注栈不会跟着丢——两栈长度可能仍然相等，
// 配对却整体错开一位（实测表现：撤销之后箭头还在，而且越撤越错）。
//
// 现在标注状态跟着条目一起生、一起死：裁剪丢掉的是"位图+标注"一整条，
// 取回失败丢掉的也是一整条。配对从此是结构性的，不靠两边同步。
template <typename Companion>
class BitmapHistory {
public:
    BitmapHistory() = default;
    ~BitmapHistory() {
        StopWorker();
        clear();
    }

    BitmapHistory(const BitmapHistory&) = delete;
    BitmapHistory& operator=(const BitmapHistory&) = delete;

    bool empty() const noexcept { return entries_.empty(); }
    size_t size() const noexcept { return entries_.size(); }

#ifdef SNAPLITE_EDIT_TEST_HOOKS
    // 测试专用：把"最多留多少条历史"压到指定值（0 = 恢复产品默认值）。
    //
    // 裁剪路径平时几乎测不到：它要攒够 64MB 或 128 条才触发一次。
    // 而它恰恰是最容易让"位图与标注的配对"错开的地方——裁剪丢的是
    // **队首**，取回用的是**队尾**。把它压到 2~3 条，测试几秒钟就能
    // 把这条路走一遍。
    //
    // 只在定义了 SNAPLITE_EDIT_TEST_HOOKS 的编译单元里存在：产品二进制
    // 不该带着"为测试而存在的开关"。
    static void SetMaxEntriesOverrideForTest(unsigned limit) {
        s_maxEntriesOverride = limit;
    }
#endif

    // 后台压缩还有没有没压完的条目（会顺带把已经压完的结果装回来）。
    //
    // 正常流程不需要知道，但测试要靠它确认"压完之后再取回"这条 PNG 解码
    // 路径真的被走到过：压缩改成异步之后，不主动等它的话，撤销拿到的
    // 永远是原始位图，解码路径就没人验了。
    // 它必须顺带 DrainFinished，否则在途计数只减不消，调用方会一直等下去。
    bool IsCompressing() {
        DrainFinished();
        return jobsInFlight_ > 0;
    }

    void push_back(HBITMAP bitmap, Companion companion) {
        if (!bitmap) return;

        Entry entry;
        entry.bitmap = bitmap;
        entry.rawBytes = BitmapBytes(bitmap);
        entry.id = nextId_++;
        entry.companion = std::move(companion);
        entries_.push_back(std::move(entry));

        // 先把后台压好的结果装回来，再决定要不要派新活。
        DrainFinished();
        QueueColdEntry();
        Trim();
    }

    // 取出并弹出最上面一条。
    //
    // companion（可空）拿到的是**与这一帧配对**的那份状态。位图取不到时
    // （只剩 PNG、而那张 PNG 解不开）这一整条会被丢掉——位图和状态一起
    // 丢，两边因此不可能对不齐。
    HBITMAP take_back(Companion* companion) {
        if (entries_.empty()) return nullptr;
        DrainFinished();

        Entry& entry = entries_.back();
        if (entry.bitmap) {
            HBITMAP bitmap = entry.bitmap;
            entry.bitmap = nullptr;
            if (companion) *companion = std::move(entry.companion);
            entries_.pop_back();
            return bitmap;
        }

        HBITMAP bitmap = DecodePng(entry.png);
        if (bitmap) {
            // 解码成功：这一帧和**与它配对的状态**一起交出去。
            if (companion) *companion = std::move(entry.companion);
        } else if (companion) {
            // 解码失败也要把条目弹掉：留着它会让撤销栈卡在这一条上，
            // 之后每次 Ctrl+Z 都拿不回东西，而且永远回不去这一条之前。
            //
            // 弹掉的是**整条**（位图 + 状态）：状态就存在条目里，所以调用方
            // 不必再去同步另一个栈（以前要同步，漏一次就越撤越错）。
            *companion = Companion{};
        }
        entries_.pop_back();
        return bitmap;
    }

    void clear() noexcept {
        for (Entry& entry : entries_) {
            // 位图归**这一侧**所有，必须在这里删掉。
            //
            // 注释原来写的是"位图归后台线程，这里绝对不能删"——那是反的：
            // 后台线程拿到的是**副本**（CopyBitmap），原图一直在这儿。
            // 按那条注释改代码的人会把原图泄漏掉，或者把后台的副本
            // 二次释放。真相是：原图归 UI 线程，后台只拿副本。
            if (entry.bitmap) {
                DeleteObject(entry.bitmap);
                entry.bitmap = nullptr;
            }
        }
        entries_.clear();
    }

private:
    struct Entry {
        HBITMAP bitmap{};
        std::vector<BYTE> png;
        SIZE_T rawBytes{};
        unsigned long long id{0};
        // 与这一帧一一对应的状态（标注列表）。跟条目同生共死：
        // 裁剪丢掉的是"位图+状态"一整条，不存在"位图丢了、状态还在"
        // 这种把配对错开一位的情况。
        Companion companion;
        // 编码失败过就不再派活，免得每几十毫秒重试同一个坏条目。
        bool compressFailed{false};
        // 已经派给后台线程了。
        //
        // 这一条是必须的：条目在后台压完之前，UI 线程一直留着原始位图，
        // 于是它看起来仍然"可以压"。不标记的话，同一条目会被反复排队，
        // 而结果表是以条目 id 为键的——后一个结果把前一个覆盖掉，
        // 在途计数就再也降不下来了（表现是撤销历史永远压不完）。
        bool queued{false};
    };

    // UI 线程与后台线程之间只共享这一个结构：它有自己的锁，也不碰
    // entries_ 里的任何东西。
    struct WorkerState {
        struct Job {
            unsigned long long id{0};
            HBITMAP bitmap{nullptr};
            SIZE_T rawBytes{0};
        };
        struct Result {
            std::vector<BYTE> png;
        };
        std::mutex mutex;
        std::deque<Job> queue;
        std::unordered_map<unsigned long long, Result> finished;
        std::atomic<bool> stopping{false};
    };

    static constexpr size_t kHotEntries = 2;
    static constexpr size_t kMaxEntries = 128;
    static constexpr SIZE_T kMaxStoredBytes = 64ull * 1024ull * 1024ull;
    // 后台同时在压的条目数。留一个空位让后台别闲着。
    static constexpr size_t kMaxJobsInFlight = 2;

    static SIZE_T BitmapBytes(HBITMAP bitmap) {
        if (!bitmap) return 0;

        BITMAP info{};
        if (GetObjectW(bitmap, sizeof(info), &info) == 0) return 0;

        const SIZE_T stride = static_cast<SIZE_T>(
            info.bmWidthBytes >= 0 ? info.bmWidthBytes : -info.bmWidthBytes);
        const SIZE_T height = static_cast<SIZE_T>(
            info.bmHeight >= 0 ? info.bmHeight : -info.bmHeight);
        return stride * height;
    }

    static bool PngEncoder(CLSID& clsid) {
        static bool initialized = false;
        static bool available = false;
        static CLSID cached{};

        if (!initialized) {
            initialized = true;
            UINT count = 0;
            UINT bytes = 0;
            if (Gdiplus::GetImageEncodersSize(&count, &bytes) == Gdiplus::Ok &&
                count > 0 && bytes > 0) {
                std::vector<BYTE> buffer(bytes);
                auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
                if (Gdiplus::GetImageEncoders(count, bytes, codecs) == Gdiplus::Ok) {
                    for (UINT i = 0; i < count; ++i) {
                        if (codecs[i].MimeType &&
                            wcscmp(codecs[i].MimeType, L"image/png") == 0) {
                            cached = codecs[i].Clsid;
                            available = true;
                            break;
                        }
                    }
                }
            }
        }

        if (!available) return false;
        clsid = cached;
        return true;
    }

    static bool EncodePng(HBITMAP bitmap, std::vector<BYTE>& bytes) {
        if (!bitmap) return false;

        CLSID encoder{};
        if (!PngEncoder(encoder)) return false;

        IStream* stream = nullptr;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream) {
            return false;
        }

        bool success = false;
        {
            Gdiplus::Bitmap image(bitmap, nullptr);
            if (image.GetLastStatus() == Gdiplus::Ok &&
                image.Save(stream, &encoder, nullptr) == Gdiplus::Ok) {
                STATSTG stat{};
                HGLOBAL global = nullptr;
                if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) &&
                    stat.cbSize.QuadPart > 0 &&
                    SUCCEEDED(GetHGlobalFromStream(stream, &global)) && global) {
                    const SIZE_T size = static_cast<SIZE_T>(stat.cbSize.QuadPart);
                    const void* data = GlobalLock(global);
                    if (data) {
                        const BYTE* first = static_cast<const BYTE*>(data);
                        bytes.assign(first, first + size);
                        GlobalUnlock(global);
                        success = true;
                    }
                }
            }
        }

        stream->Release();
        return success;
    }

    static HBITMAP DecodePng(const std::vector<BYTE>& bytes) {
        if (bytes.empty()) return nullptr;

        HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
        if (!global) return nullptr;

        void* data = GlobalLock(global);
        if (!data) {
            GlobalFree(global);
            return nullptr;
        }
        std::memcpy(data, bytes.data(), bytes.size());
        GlobalUnlock(global);

        IStream* stream = nullptr;
        if (FAILED(CreateStreamOnHGlobal(global, TRUE, &stream)) || !stream) {
            GlobalFree(global);
            return nullptr;
        }

        HBITMAP bitmap = nullptr;
        {
            Gdiplus::Bitmap image(stream, FALSE);
            if (image.GetLastStatus() == Gdiplus::Ok) {
                image.GetHBITMAP(Gdiplus::Color(255, 255, 255), &bitmap);
            }
        }
        stream->Release();
        return bitmap;
    }

    static SIZE_T EntryBytes(const Entry& entry) {
        if (entry.bitmap) return entry.rawBytes;
        if (!entry.png.empty()) return static_cast<SIZE_T>(entry.png.size());
        // 正在后台编码：位图由后台线程持有，内存还是原始大小。
        return entry.rawBytes;
    }

    SIZE_T StoredBytes() const {
        SIZE_T total = 0;
        for (const Entry& entry : entries_) {
            total += EntryBytes(entry);
        }
        return total;
    }

    // 找一条"够冷"的条目（不是最后两条热条目），把它的**副本**交给后台线程。
    //
    // 副本很重要：条目本身必须一直留着可用的位图，否则用户撤销到这一条时
    // 会既拿不到位图也没有 PNG——撤销就静默失效了（第一版就是这么写错的，
    // snip_interaction 的"undo color edit restores red copy"当场就红了）。
    // 复制 4K 的一帧约十几毫秒，远好过在 UI 线程上同步编码的七百毫秒。
    void QueueColdEntry() {
        if (entries_.size() <= kHotEntries) return;
        if (jobsInFlight_ >= kMaxJobsInFlight) return;

        for (size_t i = 0; i + kHotEntries < entries_.size(); ++i) {
            Entry& entry = entries_[i];
            if (!entry.bitmap || !entry.png.empty() || entry.compressFailed) continue;
            if (entry.queued) continue;

            HBITMAP copy = CopyBitmap(entry.bitmap);
            if (!copy) {
                entry.compressFailed = true;
                continue;
            }

            if (!EnsureWorker()) {
                // 后台起不来：这一条不压了。原始位图继续留着，撤销照常可用。
                // 副本是我们自己复制的，没人接手就得在这儿放掉。
                DeleteObject(copy);
                entry.compressFailed = true;
                continue;
            }
            typename WorkerState::Job job{};
            job.id = entry.id;
            job.bitmap = copy;
            job.rawBytes = entry.rawBytes;
            entry.queued = true;
            ++jobsInFlight_;
            {
                std::lock_guard<std::mutex> lock(workerState_->mutex);
                workerState_->queue.push_back(job);
            }
            return;
        }
    }

    // 把后台压好的结果装回历史。
    void DrainFinished() {
        if (!workerStarted_) return;

        std::unordered_map<unsigned long long, typename WorkerState::Result> finished;
        {
            std::lock_guard<std::mutex> lock(workerState_->mutex);
            if (workerState_->finished.empty()) return;
            finished.swap(workerState_->finished);
        }

        for (auto& item : finished) {
            if (jobsInFlight_ > 0) --jobsInFlight_;
            std::vector<BYTE>& png = item.second.png;

            for (Entry& entry : entries_) {
                if (entry.id != item.first) continue;
                entry.queued = false;
                if (!png.empty() && png.size() < entry.rawBytes) {
                    // 压成功了：换成 PNG，原始位图可以放掉。
                    DeleteObject(entry.bitmap);
                    entry.bitmap = nullptr;
                    entry.png = std::move(png);
                } else {
                    // 压不动：原始位图一直留着，继续能用，只是不再重试。
                    entry.compressFailed = true;
                }
                break;
            }
        }
    }

    // 复制一份位图交给后台线程去编码。
    //
    // 方向不需要额外处理：位图一旦被 SelectObject 进 DC，DC 的 (0,0) 就
    // 对应它的**视觉左上角**，BitBlt 拷的是设备坐标，方向天然一致——
    // 源位图是"自上而下"的 DIB section 还是"自下而上"的 DDB 都一样。
    //
    // 曾经这里有一段"自上而下就逐行翻转"的分支，理由是"直接照抄会上下颠倒"。
    // 那段分支是**死代码**：`GetObject` 填 BITMAP 时 bmHeight 永远是正的
    // （方向信息在那个结构里根本不保留，实测自上而下的 DIB section 拿到的
    // 也是正高度），于是分支永远走不到；更要命的是它一旦被"修好"变成可达，
    // 就会把图真的翻过来——比不做处理更糟。现在删掉它，方向交给 BitBlt。
    static HBITMAP CopyBitmap(HBITMAP source) {
        if (!source) return nullptr;

        BITMAP info{};
        if (GetObjectW(source, sizeof(info), &info) == 0) return nullptr;
        const int width = info.bmWidth;
        const int height = info.bmHeight < 0 ? -info.bmHeight : info.bmHeight;
        if (width <= 0 || height <= 0) return nullptr;

        HDC screenDc = GetDC(nullptr);
        if (!screenDc) return nullptr;
        HDC sourceDc = CreateCompatibleDC(screenDc);
        HDC targetDc = CreateCompatibleDC(screenDc);
        HBITMAP copy = CreateCompatibleBitmap(screenDc, width, height);
        if (!sourceDc || !targetDc || !copy) {
            if (copy) DeleteObject(copy);
            if (targetDc) DeleteDC(targetDc);
            if (sourceDc) DeleteDC(sourceDc);
            ReleaseDC(nullptr, screenDc);
            return nullptr;
        }

        const HGDIOBJ oldSource = SelectObject(sourceDc, source);
        const HGDIOBJ oldTarget = SelectObject(targetDc, copy);
        BitBlt(targetDc, 0, 0, width, height, sourceDc, 0, 0, SRCCOPY);
        SelectObject(sourceDc, oldSource);
        SelectObject(targetDc, oldTarget);
        DeleteDC(sourceDc);
        DeleteDC(targetDc);
        ReleaseDC(nullptr, screenDc);
        return copy;
    }

    // 起后台压缩线程。返回 false 表示起不来（资源耗尽等）。
    //
    // 异常安全是这里最要紧的事：调用方是窗口过程，那条路上**没有**
    // try/catch，异常一路穿出去就是 std::terminate——整个截图工具当场崩掉。
    // 以前这里是"先置 workerStarted_ = true，再 make_shared、再起线程"：
    // 线程创建失败时异常会穿出 push_back()，而且 workerStarted_ 已经置上了，
    // 后续 StopWorker() 还会拿着空的 workerState_ 去解引用。
    //
    // 现在：什么都包在 try 里，失败就老老实实返回 false，让调用方放弃
    // 压缩这一条（原始位图继续留着，撤销照常可用）。
    bool EnsureWorker() {
        if (workerStarted_) return true;
        std::shared_ptr<WorkerState> state;
        try {
            state = std::make_shared<WorkerState>();
            worker_ = std::thread([state]() { CompressLoop(state); });
        } catch (...) {
            // 线程没起起来：worker_ 保持未连接状态（析构时不会 terminate），
            // workerState_ 保持空，workerStarted_ 保持 false。
            return false;
        }
        workerState_ = state;
        workerStarted_ = true;
        return true;
    }

    // 后台线程：只碰 workerState_，绝不碰 entries_。
    static void CompressLoop(std::shared_ptr<WorkerState> state) {
        // GDI+ 每个使用它的线程各自初始化。
        ULONG_PTR gdiplusToken = 0;
        Gdiplus::GdiplusStartupInput gdiplusInput;
        const bool gdiplusReady =
            Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr) == Gdiplus::Ok;

        for (;;) {
            if (state->stopping.load()) break;

            typename WorkerState::Job job{};
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!state->queue.empty()) {
                    job = state->queue.front();
                    state->queue.pop_front();
                }
            }
            if (!job.bitmap) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            // 这一步最慢（4K 约七百毫秒），必须留在锁外面，
            // 否则 UI 线程会被拖住。
            std::vector<BYTE> png;
            const bool ok = EncodePng(job.bitmap, png);

            typename WorkerState::Result result;
            if (ok && !png.empty() && png.size() < job.rawBytes) {
                // 解码一次再交出去。
                //
                // PNG 解不开时，失败会推迟到**用户按 Ctrl+Z 那一刻**才出现：
                // 撤销拿不到位图，位图栈与箭头栈当场失同步（表现为"撤销之后
                // 箭头还在"，而且之后每次撤销都是错位的）。在这里验一遍，
                // 解不开就整条丢掉——原始位图继续留着，撤销照常可用。
                // 验证放在后台线程做，不占用 UI 线程。
                HBITMAP probe = DecodePng(png);
                if (probe) {
                    DeleteObject(probe);
                    result.png = std::move(png);
                }
            }
            // 这份副本是我们自己复制的，用完就删：UI 线程那边留着原图。
            DeleteObject(job.bitmap);
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->finished[job.id] = std::move(result);
            }
        }

        // 退出前把队列里剩下的副本清掉：所有权已经移交给我们，
        // 没人会替我们释放。
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            for (const typename WorkerState::Job& job : state->queue) {
                if (job.bitmap) DeleteObject(job.bitmap);
            }
            state->queue.clear();
        }
        if (gdiplusReady) {
            Gdiplus::GdiplusShutdown(gdiplusToken);
        }
    }

    void StopWorker() {
        if (!workerStarted_) return;
        workerState_->stopping.store(true);
        if (worker_.joinable()) {
            // 不等它做完：关闭截图窗口不该被一次七百毫秒的编码拖住。
            // workerState_ 由 shared_ptr 续命，后台线程照样能安全跑完并退出。
            worker_.detach();
        }
    }

    void DropFront() {
        if (entries_.empty()) return;
        Entry& entry = entries_.front();
        if (entry.bitmap) DeleteObject(entry.bitmap);
        entries_.erase(entries_.begin());
    }

    void Trim() {
        while (entries_.size() > 1 &&
               (entries_.size() > MaxEntries() || StoredBytes() > kMaxStoredBytes)) {
            DropFront();
        }
    }

    // 历史最多留多少条。裁剪从**队首**丢老条目。
    static size_t MaxEntries() {
#ifdef SNAPLITE_EDIT_TEST_HOOKS
        if (s_maxEntriesOverride > 0) return s_maxEntriesOverride;
#endif
        return kMaxEntries;
    }

#ifdef SNAPLITE_EDIT_TEST_HOOKS
    // 见 SetMaxEntriesOverrideForTest：测试专用的上限覆盖。
    //
    // 静态成员是**按类型**存的：撤销栈与重做栈是同一个实例化类型的两个
    // 对象，所以这一份上限对两边同时生效——测试要的就是"一次把两边都
    // 压下去"。
    static inline unsigned s_maxEntriesOverride = 0;
#endif

    std::vector<Entry> entries_;
    unsigned long long nextId_{1};
    bool workerStarted_{false};
    size_t jobsInFlight_{0};
    std::shared_ptr<WorkerState> workerState_;
    std::thread worker_;
};

class SnipWindow {
public:
    // 工具栏与截图窗口共用的工具索引，避免两边用数字约定工具顺序。
    enum class UiToolIndex : int {
        None = -1,
        Rectangle = 0,
        Arrow,
        Pen,
        Eraser,
        Mosaic,
        Text,
    };

    enum class FinishAction {
        Copy,
        Save,
        SaveAs,
        Pin,
    };

    using CaptureCallback = std::function<void(HBITMAP, FinishAction)>;

    static bool Register(HINSTANCE instance);
    static bool Start(HINSTANCE instance, HWND owner, CaptureCallback callback);

    bool UiHasSelection() const;
    RECT UiSelectionRect() const;
    RECT UiLegacyToolbarRect() const;
    int UiActiveTool() const;
    void UiSetTool(int toolIndex);

    int UiShapeKind() const;
    void UiSetShapeKind(int kind);
    int UiShapeFillMode() const;
    void UiSetShapeFillMode(int mode);
    int UiArrowKind() const;
    void UiSetArrowKind(int kind);
    int UiStrokeWidth() const;
    void UiSetStrokeWidth(int width);

    COLORREF UiColor() const;
    void UiSetColor(COLORREF color);
    int UiTextSize() const;
    void UiSetTextSize(int points);
    void UiUndo();
    void UiRedo();
    void UiBakeTextOverlays(const std::vector<TextOverlay>& overlays);
    void UiFinish(FinishAction action);
    void UiCancel();
    HWND UiHwnd() const;
    HBITMAP UiCaptureBitmap() const;
    bool UiInteractionActive() const;
    bool UiSelectionDragActive() const;
    bool UiHitSelectionBorder(POINT point) const;
    bool UiHitArrow(POINT point) const;
    unsigned long long UiEditRevision() const;
    bool UiHandleAnnotationKey(WPARAM key);
    void UiDeselectAnnotation();

#ifdef SNAPLITE_EDIT_TEST_HOOKS
    // 测试专用：把撤销/重做栈的条目上限压到指定值（0 = 恢复产品默认值）。
    //
    // 必须走**产品真正用的那个实例化**：
    // BitmapHistory 的测试钩子是**按模板实例化**存的静态量，每一份
    // 实例化各有一份。测试自己造一个 BitmapHistory<int> 再设上限，
    // 压不到产品这一份（上一轮验收实测：两个实例化互不生效，
    // 于是"裁剪配对"这条产品路径根本没有运行时覆盖）。
    //
    // 裁剪路径平时要攒够 64MB 或 128 条才触发一次，测试里压不到，
    // 只能靠这个开关把它压到 2 条——所以它必须作用在产品自己那份上。
    static void SetHistoryEntryLimitForTest(unsigned limit) {
        BitmapHistory<std::vector<ArrowAnnotation>>::SetMaxEntriesOverrideForTest(
            limit);
    }
#endif

private:
    enum class Tool {
        None,
        Rectangle,
        Arrow,
        Pen,
        Eraser,
        Mosaic,
        Text,
    };

    enum class DragMode {
        None,
        NewSelection,
        Move,
        ResizeTopLeft,
        ResizeTop,
        ResizeTopRight,
        ResizeRight,
        ResizeBottomRight,
        ResizeBottom,
        ResizeBottomLeft,
        ResizeLeft,
    };

    SnipWindow(HINSTANCE instance, HWND owner, CaptureCallback callback);
    ~SnipWindow();

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void UpdateHover();
    void EnsureFonts();
    void Paint();
    HDC AcquireFrameBuffer(HDC reference);
    void ReleaseFrameBuffer();
    void PaintMagnifier(HDC dc, POINT clientPoint);
    void PaintSelection(HDC dc);
    void PaintHint(HDC dc);
    void PaintToolbar(HDC dc);
    void PaintToolbarIcon(HDC dc, int index, const RECT& rect, bool active, bool hovered);
    void PaintPreview(HDC dc);

    RECT ToolbarRect() const;
    RECT ToolbarButtonRect(const RECT& bar, int index) const;
    RECT NormalizedSelection() const;
    DragMode HitSelection(POINT point) const;
    int HitToolbar(POINT point) const;
    void SetCursorForPoint(POINT point);

    void BeginSelectionDrag(POINT point);
    void UpdateSelectionDrag(POINT point);
    void FinishSelectionDrag(POINT point);
    void ClampSelection();
    void ClearHistory();

    void BeginEdit();
    void Undo();
    void Redo();
    HBITMAP SnapshotSelection() const;
    void RestoreSelection(HBITMAP snapshot);
    void DrawShape(HDC dc, Tool tool, POINT from, POINT to);
    struct ArrowAnnotation {
        POINT from{};
        POINT to{};
        COLORREF color{};
        int kind{};
        int width{};
        Tool tool{Tool::Arrow};
        int fillMode{};
    };
    void PaintArrows(HDC dc) const;
    int HitArrow(POINT point, int* part = nullptr) const;
    void UpdateArrowDrag(POINT point);
    void FinishArrowDrag(bool cancel);
    void EraseArrows(POINT from, POINT to);
    void SelectAnnotation(int index);
    void UpdateSelectedAnnotationStyle();
    void DrawPenSegment(POINT from, POINT to);
    void ApplyMosaic(POINT point);

    void HandleToolbarClick(int index);
    void Finish(FinishAction action);
    void CopyCurrentColor();

    // The themed rectangle shim needs access to the per-instance shape state
    // because the legacy source is included through a small macro adapter.
    friend HPEN detail::ThemedCreatePen(SnipWindow& owner, int style, int width, COLORREF color);
    friend BOOL detail::ThemedRectangle(SnipWindow& owner, HDC dc, int left, int top, int right, int bottom);

    HINSTANCE instance_{};
    HWND owner_{};
    HWND hwnd_{};
    HBITMAP capture_{};
    mutable HBITMAP arrowComposite_{};
    // 合成图（capture_ + 几何标注）的缓存键。三元组任一变化都必须重建，
    // 缺一个都会让用户看到上一帧的标注。
    mutable unsigned long long compositeEditRevision_{0};
    mutable unsigned long long compositeContentRevision_{0};
    mutable RECT compositeSelection_{};
    HDC frameDc_{};
    HBITMAP frameBitmap_{};
    HGDIOBJ frameOldBitmap_{};
    VirtualScreen screen_{};
    CaptureCallback callback_;

    bool selected_{false};
    bool hasHover_{false};
    bool edited_{false};
    bool dragging_{false};
    bool drawing_{false};
    RECT selection_{};
    RECT hoverRect_{};
    RECT pressedHoverRect_{};
    RECT dragOrigin_{};
    POINT dragStart_{};
    POINT dragCurrent_{};
    DragMode dragMode_{DragMode::None};

    Tool tool_{Tool::None};
    POINT drawStart_{};
    POINT drawCurrent_{};
    // 撤销/重做历史。每条历史带着与那一帧配对的标注状态，
    // 位图与状态同生共死——不存在"两个平行的栈对不齐"这回事。
    BitmapHistory<std::vector<ArrowAnnotation>> undo_;
    BitmapHistory<std::vector<ArrowAnnotation>> redo_;
    // 沿用箭头编辑状态保存全部几何标注，共享层级与历史，移动时不擦伤其他标注。
    std::vector<ArrowAnnotation> arrows_;
    int selectedArrow_{-1};
    int arrowDragPart_{0}; // 1：整体，2：起点，3：终点。
    POINT arrowDragStart_{};
    ArrowAnnotation arrowDragOrigin_{};
    bool arrowDragChanged_{false};
    unsigned long long editRevision_{0};
    // 捕获像素或几何标注内容发生变化时递增。editRevision_ 覆盖所有
    // BeginEdit 驱动的改动；这个计数器补上撤销/重做与拖动中途这些
    // 不经过 BeginEdit 的路径。
    unsigned long long contentRevision_{0};

    // Per-instance shape drawing state used by the themed rectangle shim.
    bool shapeDrawing_{false};

    int hoverToolbar_{-1};

    COLORREF annotationColor_{RGB(235, 70, 70)};
    int textSizePt_{14};

    HFONT uiFont_{};
    HFONT monoFont_{};
};

}  // namespace snaplite
