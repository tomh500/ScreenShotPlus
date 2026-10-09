#include "instance.h"
using namespace std;
InstanceLock::~InstanceLock() { if (handle) CloseHandle(handle); }
bool AcquireInstanceLock(InstanceLock& lock, const wchar_t* name) {
    if (lock.handle) return false;
    lock.handle = CreateMutexExW(nullptr, name, 0, SYNCHRONIZE);
    const DWORD status = GetLastError();
    lock.first = lock.handle && status != ERROR_ALREADY_EXISTS;
    return lock.handle != nullptr;
}
namespace {
HWND ExistingPanel(const wchar_t* name) {
    struct Search { const wchar_t* name; HWND primary = nullptr, legacy = nullptr; } search{name};
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        auto& search = *reinterpret_cast<Search*>(param);
        wchar_t name[128]{}; GetClassNameW(hwnd, name, 128);
        if (wstring_view(name) != search.name) return TRUE;
        if (GetPropW(hwnd, INSTANCE_PANEL_MARKER)) { search.primary = hwnd; return FALSE; }
        DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process) {
            wstring executable(32768, L'\0'); DWORD length = static_cast<DWORD>(executable.size());
            if (QueryFullProcessImageNameW(process, 0, executable.data(), &length)) {
                executable.resize(length);
                if (filesystem::path(executable).filename() == L"ScreenShotPlus.exe") search.legacy = hwnd;
            }
            CloseHandle(process);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.primary ? search.primary : search.legacy;
}
}
bool ActivateExistingInstance(unsigned timeoutMs, const wchar_t* panelClass) {
    const ULONGLONG end = GetTickCount64() + timeoutMs;
    do {
        if (HWND panel = ExistingPanel(panelClass)) {
            DWORD pid = 0; GetWindowThreadProcessId(panel, &pid); AllowSetForegroundWindow(pid);
            return PostMessageW(panel, WM_SHOW_PANEL, 0, 0) != FALSE;
        }
        if (GetTickCount64() >= end) break;
        Sleep(25);
    } while (true);
    return false;
}
