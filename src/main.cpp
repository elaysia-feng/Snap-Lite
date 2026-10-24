#include <windows.h>

#include "app.h"

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand) {
    // DPI 感知在 App::Initialize() 里开启：必须早于任何窗口创建，而且
    // 失败时要能通过托盘气泡告诉用户，而不是静默地坐标错位。
    snaplite::App app(instance);
    return app.Run(showCommand);
}
