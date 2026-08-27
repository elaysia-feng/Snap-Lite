#pragma once

#include <windows.h>
#include <windowsx.h>

#include <string>

namespace snaplite {

// Show a modeless OCR result window beside the current screenshot selection.
// The result text remains selectable so users can copy only the fragment they need.
void ShowOcrResultPanel(HWND owner, const RECT& selectionClientRect, const std::wstring& text);

}  // namespace snaplite
