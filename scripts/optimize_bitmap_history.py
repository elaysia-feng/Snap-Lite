from __future__ import annotations

import sys
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, got {count}")
    return text.replace(old, new, 1)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: optimize_bitmap_history.py <generated-src-dir>", file=sys.stderr)
        return 2

    generated = Path(sys.argv[1]).resolve()
    header_path = generated / "snip_window.h"
    inc_path = generated / "snip_window_original.inc"

    header = header_path.read_text(encoding="utf-8")
    inc = inc_path.read_text(encoding="utf-8")

    header = replace_once(
        header,
        "#include <windows.h>\n\n#include <functional>\n#include <string>\n#include <vector>",
        "#include <windows.h>\n#include <objidl.h>\n#include <gdiplus.h>\n\n#include <algorithm>\n#include <cstring>\n#include <cwchar>\n#include <functional>\n#include <string>\n#include <utility>\n#include <vector>",
        "history support includes",
    )

    start = header.index("class BitmapHistory : public std::vector<HBITMAP> {")
    end = header.index("\n\nclass SnipWindow", start)

    optimized_class = r'''class BitmapHistory {
public:
    BitmapHistory() = default;
    ~BitmapHistory() { clear(); }

    BitmapHistory(const BitmapHistory&) = delete;
    BitmapHistory& operator=(const BitmapHistory&) = delete;

    bool empty() const noexcept { return entries_.empty(); }
    size_t size() const noexcept { return entries_.size(); }

    void push_back(HBITMAP bitmap) {
        if (!bitmap) return;

        Entry entry;
        entry.bitmap = bitmap;
        entry.rawBytes = BitmapBytes(bitmap);
        entries_.push_back(std::move(entry));

        // Keep only the most recent snapshots uncompressed so Ctrl+Z remains
        // instant. Older snapshots are losslessly compressed to PNG in memory.
        CompactColdEntries();
        Trim();
    }

    HBITMAP take_back() {
        if (entries_.empty()) return nullptr;

        Entry& entry = entries_.back();
        if (entry.bitmap) {
            HBITMAP bitmap = entry.bitmap;
            entry.bitmap = nullptr;
            entries_.pop_back();
            return bitmap;
        }

        HBITMAP bitmap = DecodePng(entry.png);
        if (!bitmap) return nullptr;
        entries_.pop_back();
        return bitmap;
    }

    void clear() noexcept {
        for (Entry& entry : entries_) {
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
    };

    static constexpr size_t kHotEntries = 2;
    static constexpr size_t kMaxEntries = 128;
    static constexpr SIZE_T kMaxStoredBytes = 64ull * 1024ull * 1024ull;

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
        return static_cast<SIZE_T>(entry.png.size());
    }

    SIZE_T StoredBytes() const {
        SIZE_T total = 0;
        for (const Entry& entry : entries_) {
            total += EntryBytes(entry);
        }
        return total;
    }

    void CompactColdEntries() {
        if (entries_.size() <= kHotEntries) return;

        const size_t coldCount = entries_.size() - kHotEntries;
        for (size_t i = 0; i < coldCount; ++i) {
            Entry& entry = entries_[i];
            if (!entry.bitmap || !entry.png.empty()) continue;

            std::vector<BYTE> compressed;
            if (!EncodePng(entry.bitmap, compressed)) continue;

            // Compression is useful only when it actually saves memory.
            if (compressed.size() >= entry.rawBytes) continue;

            DeleteObject(entry.bitmap);
            entry.bitmap = nullptr;
            entry.png = std::move(compressed);
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
               (entries_.size() > kMaxEntries || StoredBytes() > kMaxStoredBytes)) {
            DropFront();
        }
    }

    std::vector<Entry> entries_;
};'''

    header = header[:start] + optimized_class + header[end:]

    inc = replace_once(
        inc,
        "void DeleteBitmaps(std::vector<HBITMAP>& bitmaps) {\n"
        "    for (HBITMAP bitmap : bitmaps) {\n"
        "        if (bitmap) {\n"
        "            DeleteObject(bitmap);\n"
        "        }\n"
        "    }\n"
        "    bitmaps.clear();\n"
        "}",
        "void DeleteBitmaps(BitmapHistory& history) {\n"
        "    history.clear();\n"
        "}",
        "history cleanup helper",
    )

    inc = replace_once(
        inc,
        "        // BitmapHistory::Trim keeps a generous 128-step / 512MB session cap.\n"
        "        // This avoids the old behavior where Ctrl+Z stopped after ~6 edits.",
        "        // BitmapHistory keeps two hot raw snapshots for fast Ctrl+Z and\n"
        "        // losslessly compresses older steps, with a 64MB cap per stack.",
        "history comment",
    )

    inc = replace_once(
        inc,
        "    HBITMAP previous = undo_.back();\n"
        "    undo_.pop_back();",
        "    HBITMAP previous = undo_.take_back();\n"
        "    if (!previous) return;",
        "undo take compressed entry",
    )

    inc = replace_once(
        inc,
        "    HBITMAP next = redo_.back();\n"
        "    redo_.pop_back();",
        "    HBITMAP next = redo_.take_back();\n"
        "    if (!next) return;",
        "redo take compressed entry",
    )

    header_path.write_text(header, encoding="utf-8", newline="\n")
    inc_path.write_text(inc, encoding="utf-8", newline="\n")

    print(
        "Optimized bitmap history: 2 hot raw snapshots + lossless PNG cold storage, "
        "128 entries / 64MB per undo-redo stack"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
