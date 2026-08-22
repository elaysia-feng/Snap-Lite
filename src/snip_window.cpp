// Theme wrapper for the original snip implementation.
//
// The screenshot engine itself stays untouched in snip_window_original.inc.
// This file remaps only GDI+ UI colors so visual changes cannot accidentally
// break selection, annotation, undo/redo or capture behavior.

#include "snip_window.h"

#include <windowsx.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <string>
#include <utility>

namespace Gdiplus {

class SnapLiteThemeColor : public Color {
public:
    SnapLiteThemeColor() : Color() {}
    explicit SnapLiteThemeColor(ARGB value) : Color(value) {}

    SnapLiteThemeColor(BYTE alpha, BYTE red, BYTE green, BYTE blue)
        : Color(ThemeArgb(alpha, red, green, blue)) {}

    SnapLiteThemeColor(BYTE red, BYTE green, BYTE blue)
        : Color(ThemeArgb(255, red, green, blue)) {}

private:
    static ARGB Pack(BYTE alpha, BYTE red, BYTE green, BYTE blue) {
        return (static_cast<ARGB>(alpha) << 24) |
               (static_cast<ARGB>(red) << 16) |
               (static_cast<ARGB>(green) << 8) |
               static_cast<ARGB>(blue);
    }

    static ARGB ThemeArgb(BYTE alpha, BYTE red, BYTE green, BYTE blue) {
        // Graphite chrome -> cool blue-black glass.
        if (red == 27 && green == 30 && blue == 35) {
            return Pack(alpha, 18, 25, 43);
        }

        // Amber accent -> cyan, matching the anime camera icon.
        if (red == 255 && green == 197 && blue == 61) {
            return Pack(alpha, 111, 231, 255);
        }

        // Main icon/text ink -> slightly blue white.
        if (red == 236 && green == 239 && blue == 244) {
            return Pack(alpha, 230, 247, 255);
        }

        // Cancel hover -> soft anime pink instead of alarm red.
        if (red == 255 && green == 107 && blue == 91) {
            return Pack(alpha, 255, 111, 164);
        }

        // Overlay dimming: preserve alpha, add a subtle blue-violet tint.
        if (red == 0 && green == 0 && blue == 0) {
            if (alpha == 148 || alpha == 66) {
                return Pack(alpha, 19, 24, 48);
            }
            if (alpha == 110) {
                return Pack(alpha, 7, 14, 31);
            }
            if (alpha == 13) {
                return Pack(alpha, 43, 93, 170);
            }
        }

        // Bright hairlines / separators / hover chips.
        if (red == 255 && green == 255 && blue == 255) {
            if (alpha == 56) {
                return Pack(82, 143, 232, 255);
            }
            if (alpha == 38) {
                return Pack(52, 111, 231, 255);
            }
            if (alpha == 26) {
                return Pack(36, 111, 231, 255);
            }
        }

        return Pack(alpha, red, green, blue);
    }
};

}  // namespace Gdiplus

// All headers used by the implementation are already included above. Remap
// only implementation-time Gdiplus::Color tokens, then immediately undo the
// macro after inclusion so it cannot leak to other translation units.
#define Color SnapLiteThemeColor
#include "snip_window_original.inc"
#undef Color
