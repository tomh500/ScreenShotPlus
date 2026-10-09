#include "app.h"
#include "startup.h"
#include "instance.h"
int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    InstanceLock lock;
    if (!AcquireInstanceLock(lock)) return 1;
    const bool tray = TrayStartupRequested();
    if (!lock.first) {
        if (!tray && !ActivateExistingInstance()) return 8;
        return 0;
    }
    const HRESULT ole = OleInitialize(nullptr);
    if (FAILED(ole)) return 2;
    Gdiplus::GdiplusStartupInput input; ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
        OleUninitialize(); return 3;
    }
    const int result = RunApp(instance, {}, tray);
    Gdiplus::GdiplusShutdown(token); OleUninitialize();
    return result;
}
