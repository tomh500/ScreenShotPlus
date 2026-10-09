#include "app.h"
int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\ScreenShotPlus.Singleton");
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND panel = FindWindowW(PANEL_CLASS, nullptr)) PostMessageW(panel, WM_SHOW_PANEL, 0, 0);
        CloseHandle(mutex); return 0;
    }
    const HRESULT ole = OleInitialize(nullptr);
    if (FAILED(ole)) { CloseHandle(mutex); return 2; }
    Gdiplus::GdiplusStartupInput input; ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
        OleUninitialize(); CloseHandle(mutex); return 3;
    }
    const int result = RunApp(instance);
    Gdiplus::GdiplusShutdown(token); OleUninitialize(); CloseHandle(mutex);
    return result;
}
