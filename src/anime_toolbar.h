#pragma once

#include <windows.h>

namespace snaplite {

class AnimeToolbar {
public:
    static bool Register(HINSTANCE instance);
    static void ShowForSnip(HINSTANCE instance);
};

}  // namespace snaplite
