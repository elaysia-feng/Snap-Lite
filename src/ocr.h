#pragma once

#include <windows.h>

#include <string>

namespace snaplite {

struct OcrResult {
    bool success{false};
    std::wstring text;
    std::wstring error;
};

// Extract text from a selected region of the current capture. A packaged
// PaddleOCR worker is preferred; Windows built-in OCR remains the fallback.
// The region uses capture/client coordinates.
OcrResult ExtractTextFromBitmapRegion(HBITMAP bitmap, const RECT& region);

// Put UTF-16 text on the Windows clipboard.
bool CopyUnicodeTextToClipboard(HWND owner, const std::wstring& text);

}  // namespace snaplite
