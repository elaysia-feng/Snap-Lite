#include <windows.h>

#include "app.h"

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    snaplite::App app(instance);
    return app.Run(showCommand);
}
