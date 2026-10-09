#include "app.h"
#include "startup.h"
using namespace std;
namespace {
// 只识别我们自己的开关，忽略 Windows 传给窗口程序的其它参数。
bool StartupRequested() {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return false;
    bool requested = false;
    for (int i = 1; i < count && !requested; ++i)
        requested = _wcsicmp(arguments[i], startup::kStartupArgument) == 0 ||
            _wcsicmp(arguments[i], L"/startup") == 0;
    LocalFree(arguments);
    return requested;
}
}
int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\ScreenShotPlus.Singleton");
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND panel = FindWindowW(PANEL_CLASS, nullptr)) PostMessageW(panel, WM_SHOW_PANEL, 0, 0);
        CloseHandle(mutex); return 0;
    }
    const bool startup = StartupRequested();
    const HRESULT ole = OleInitialize(nullptr);
    if (FAILED(ole)) { CloseHandle(mutex); return 2; }
    Gdiplus::GdiplusStartupInput input; ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
        OleUninitialize(); CloseHandle(mutex); return 3;
    }
    const int result = RunApp(instance, startup);
    Gdiplus::GdiplusShutdown(token); OleUninitialize(); CloseHandle(mutex);
    return result;
}
