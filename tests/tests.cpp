#include "app.h"
#include "capture.h"
#include "storage.h"
#include "config.h"
#include "dragdrop.h"
#include "preview.h"
#include "settings.h"
#include "ui.h"
#include "hotkeys.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <iostream>
#include <set>
#include <thread>
#include <future>
#include <aclapi.h>
using namespace std;
using namespace Gdiplus;
namespace {
void Check(bool condition, const char* message) { if (!condition) throw runtime_error(message); }
void Pump(unsigned milliseconds) {
    const auto end = GetTickCount64() + milliseconds;
    while (GetTickCount64() < end) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}
void Click(HWND window, int x, int y) {
    const float scale = ui::Scale(window);
    SendMessageTimeoutW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(static_cast<int>(x * scale), static_cast<int>(y * scale)),
        SMTO_ABORTIFHUNG, 1000, nullptr);
    SendMessageTimeoutW(window, WM_LBUTTONUP, 0, MAKELPARAM(static_cast<int>(x * scale), static_cast<int>(y * scale)),
        SMTO_ABORTIFHUNG, 1000, nullptr);
}
void Snapshot(HWND hwnd, const filesystem::path& folder) {
    UpdateWindow(hwnd);
    RECT rect{}; GetClientRect(hwnd, &rect);
    HDC dc = GetDC(hwnd), memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, rect.right, rect.bottom);
    HGDIOBJ old = SelectObject(memory, bitmap);
    const bool copied = BitBlt(memory, 0, 0, rect.right, rect.bottom, dc, 0, 0, SRCCOPY) != FALSE;
    SelectObject(memory, old); DeleteDC(memory); ReleaseDC(hwnd, dc);
    bool saved = false;
    if (copied) {
        unique_ptr<Bitmap> image(Bitmap::FromHBITMAP(bitmap, nullptr));
        filesystem::path file; wstring error;
        saved = image && SavePng(*image, folder, file, error);
    }
    DeleteObject(bitmap);
    Check(saved, "UI snapshot failed");
}
void Geometry() {
    RECT r = SelectionRect({90, 75}, {-20, 10}, 100, 80);
    Check(r.left == 0 && r.top == 10 && r.right == 90 && r.bottom == 75, "reverse / clipped selection");
    r = SelectionRect({0, 0}, {300, 200}, 100, 80);
    Check(r.right == 100 && r.bottom == 80, "selection bounds");
    r = SelectionRect({20, 20}, {20, 20}, 100, 80);
    Check(r.left == r.right && r.top == r.bottom, "empty selection");
    // Physical points on a virtual desktop whose origin is (-1920, -1080).
    r = SelectionRect({1700, 900}, {2300, 1300}, 3840, 2160);
    Check(r.left - 1920 == -220 && r.right - 1920 == 380 && r.bottom - r.top == 400, "cross-origin geometry");
}
void Configuration(const filesystem::path& root) {
    const auto file = root / L"设置" / L"config.json";
    Config c = DefaultConfig(); c.folder = root / L"截图 中文 空格";
    c.modifiers = MOD_CONTROL | MOD_ALT; c.key = VK_F9; c.printScreen = true; c.previewSeconds = 15;
    wstring error;
    Check(SaveConfig(file, c, error), "save config");
    wstring warning;
    auto loaded = LoadConfig(file, warning);
    Check(warning.empty() && loaded.folder == c.folder && loaded.key == VK_F9 && loaded.previewSeconds == 15 && loaded.printScreen, "Unicode config round trip");
    c.previewSeconds = 0; Check(SaveConfig(file, c, error), "replace existing config atomically");
    Check(LoadConfig(file, warning).previewSeconds == 0, "updated config persisted");
    nlohmann::json data;
    { ifstream in(file); in >> data; }
    for (const auto& bad : {nlohmann::json(-1), nlohmann::json(4000), nlohmann::json(2.5), nlohmann::json("bad")}) {
        auto invalid = data; invalid["previewSeconds"] = bad;
        { ofstream out(file); out << invalid; }
        warning.clear(); loaded = LoadConfig(file, warning);
        Check(!warning.empty() && loaded.modifiers == (MOD_CONTROL | MOD_SHIFT) && loaded.key == 'S', "invalid config falls back");
    }
    { ofstream out(file); out << "{broken"; }
    warning.clear(); LoadConfig(file, warning); Check(!warning.empty(), "broken JSON warning");
    { ofstream out(root / L"blocked"); out << "original"; }
    Check(!SaveConfig(root / L"blocked" / L"config.json", c, error), "config write failure");
    c.modifiers = 0; Check(!ValidConfig(c), "bare key rejected");
}
filesystem::path PngStorage(const filesystem::path& root) {
    Bitmap image(17, 23, PixelFormat32bppRGB);
    { Graphics g(&image); g.Clear(Color(255, 13, 101, 203)); }
    set<filesystem::path> paths;
    filesystem::path latest;
    for (int i = 0; i < 12; ++i) {
        wstring error; Check(SavePng(image, root / L"图片 中文", latest, error), "save PNG");
        Check(paths.insert(latest).second, "unique filenames / no overwrite");
    }
    for (const auto& file : paths) {
        Bitmap decoded(file.c_str()); Color pixel;
        Check(decoded.GetLastStatus() == Ok && decoded.GetWidth() == 17 && decoded.GetHeight() == 23, "PNG dimensions");
        Check(decoded.GetPixel(3, 4, &pixel) == Ok && pixel.GetR() == 13 && pixel.GetG() == 101 && pixel.GetB() == 203 && pixel.GetA() == 255, "PNG pixels / opaque alpha");
    }
    wstring error;
    Check(!SavePng(image, root / L"blocked", latest, error) && !error.empty(), "PNG save failure");
    SYSTEMTIME time{}; time.wYear = 2026; time.wMonth = 10; time.wDay = 9; time.wMilliseconds = 7;
    Check(ScreenshotStem(time) == L"Screenshot_20261009_000000_007", "filename timestamp");
    return *paths.begin();
}
void FileTransfer(const filesystem::path& file) {
    IDataObject* data = nullptr;
    Check(SUCCEEDED(CreateFileDataObject(file, &data)), "create file transfer");
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    Check(data->GetData(&format, &medium) == S_OK, "CF_HDROP payload");
    const UINT size = DragQueryFileW(static_cast<HDROP>(medium.hGlobal), 0, nullptr, 0);
    vector<wchar_t> path(size + 1);
    DragQueryFileW(static_cast<HDROP>(medium.hGlobal), 0, path.data(), static_cast<UINT>(path.size()));
    Check(DragQueryFileW(static_cast<HDROP>(medium.hGlobal), 0xffffffff, nullptr, 0) == 1 && filesystem::path(path.data()) == file, "one real Unicode file");
    STGMEDIUM second{}; Check(data->GetData(&format, &second) == S_OK && second.hGlobal != medium.hGlobal, "independent transfer allocations");
    ReleaseStgMedium(&second); ReleaseStgMedium(&medium);
    format.cfFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
    Check(data->GetData(&format, &medium) == S_OK, "preferred copy format");
    const auto* effect = static_cast<DWORD*>(GlobalLock(medium.hGlobal));
    Check(effect && *effect == DROPEFFECT_COPY, "copy-only effect");
    GlobalUnlock(medium.hGlobal); ReleaseStgMedium(&medium);
    IEnumFORMATETC* enumerator = nullptr;
    Check(data->EnumFormatEtc(DATADIR_GET, &enumerator) == S_OK, "format enumeration");
    FORMATETC formats[2]{}; ULONG count = 0;
    Check(enumerator->Next(2, formats, &count) == S_OK && count == 2, "two transfer formats");
    enumerator->Release();
    format.tymed = TYMED_ISTREAM;
    Check(data->QueryGetData(&format) == DV_E_TYMED, "unsupported transfer medium");
    data->Release();
    DWORD copy = 123;
    Check(FAILED(DragFile(file.parent_path() / L"missing.png", copy)) && copy == DROPEFFECT_NONE, "missing source handled");
    Check(filesystem::exists(file), "source preserved");
}
void WritePermissions(const filesystem::path& root) {
    const auto folder = root / (L"permission-test-" + to_wstring(GetTickCount64()));
    Check(filesystem::create_directory(folder), "fresh permission test directory");
    Config config = DefaultConfig(); config.folder = folder;
    const auto file = folder / L"config.json"; wstring error, warning;
    Check(SaveConfig(file, config, error), "original configuration before access denial");
    struct RestoreDacl {
        wstring name;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        PACL original = nullptr, denied = nullptr;
        DWORD flags = DACL_SECURITY_INFORMATION;
        bool changed = false;
        ~RestoreDacl() {
            if (changed) SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, flags, nullptr, nullptr, original, nullptr);
            if (denied) LocalFree(denied);
            if (descriptor) LocalFree(descriptor);
        }
    } restore;
    restore.name = folder.wstring();
    Check(GetNamedSecurityInfoW(restore.name.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &restore.original, nullptr, &restore.descriptor) == ERROR_SUCCESS, "read fixture DACL");
    SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0;
    Check(GetSecurityDescriptorControl(restore.descriptor, &control, &revision) != FALSE, "read DACL inheritance flags");
    restore.flags |= control & SE_DACL_PROTECTED ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION;
    HANDLE token = nullptr; Check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE, "query fixture user");
    DWORD size = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    vector<char> identity(size);
    const bool queried = GetTokenInformation(token, TokenUser, identity.data(), size, &size) != FALSE;
    CloseHandle(token); Check(queried, "read fixture user SID");
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY;
    access.grfAccessMode = DENY_ACCESS; access.grfInheritance = NO_INHERITANCE;
    BuildTrusteeWithSidW(&access.Trustee, reinterpret_cast<TOKEN_USER*>(identity.data())->User.Sid);
    Check(SetEntriesInAclW(1, &access, restore.original, &restore.denied) == ERROR_SUCCESS, "construct fixture deny ACL");
    Check(SetNamedSecurityInfoW(restore.name.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, restore.denied, nullptr) == ERROR_SUCCESS, "deny new files only in owned fixture");
    restore.changed = true;
    Bitmap image(8, 8, PixelFormat32bppRGB); filesystem::path png;
    Check(!SavePng(image, folder, png, error), "PNG denied by real filesystem permissions");
    config.previewSeconds = 10;
    Check(!SaveConfig(file, config, error), "configuration denied by real filesystem permissions");
    Check(LoadConfig(file, warning).previewSeconds == 0, "write denial preserves previous configuration");
    Check(SetNamedSecurityInfoW(restore.name.data(), SE_FILE_OBJECT, restore.flags, nullptr, nullptr,
        restore.original, nullptr) == ERROR_SUCCESS, "restore fixture permissions");
    restore.changed = false;
    Check(SavePng(image, folder, png, error), "PNG succeeds after permission restoration");
}
LRESULT CALLBACK PatternProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps);
        { Graphics g(dc); g.Clear(Color(255, 29, 137, 211)); }
        EndPaint(hwnd, &ps); return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}
void CaptureAndPreview(HINSTANCE instance, const filesystem::path& root) {
    WNDCLASSW cls{}; cls.lpfnWndProc = PatternProc; cls.hInstance = instance; cls.lpszClassName = L"ScreenShotPlus.TestPattern";
    Check(RegisterClassW(&cls) != 0, "test pattern class");
    HWND pattern = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, cls.lpszClassName, L"Screenshot test pattern", WS_POPUP,
        100, 100, 400, 300, nullptr, nullptr, instance, nullptr);
    Check(pattern != nullptr, "test pattern window");
    ShowWindow(pattern, SW_SHOWNOACTIVATE); UpdateWindow(pattern); DwmFlush();
    unique_ptr<Bitmap> result;
    POINT finished{}; wstring error; bool called = false;
    Check(BeginSelection(instance, [&](unique_ptr<Bitmap> image, POINT point, wstring failure) {
        result = move(image); finished = point; error = move(failure); called = true;
    }, error), "desktop freeze");
    HWND overlay = FindWindowW(L"ScreenShotPlus.Selection", nullptr);
    Check(overlay != nullptr, "selection overlay");
    const int x = 150 - GetSystemMetrics(SM_XVIRTUALSCREEN), y = 150 - GetSystemMetrics(SM_YVIRTUALSCREEN);
    SendMessageW(overlay, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x + 80, y + 60));
    SendMessageW(overlay, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, y));
    SendMessageW(overlay, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    Check(called && result && error.empty() && result->GetWidth() == 80 && result->GetHeight() == 60, "live reverse selection crop");
    Color pixel; result->GetPixel(20, 20, &pixel);
    Check(pixel.GetR() == 29 && pixel.GetG() == 137 && pixel.GetB() == 211 && pixel.GetA() == 255, "frozen desktop pixel / alpha");
    Check(finished.x == 150 && finished.y == 150, "selection end physical point");
    const DWORD resources = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int i = 0; i < 3; ++i) {
        called = false;
        Check(BeginSelection(instance, [&](unique_ptr<Bitmap> image, POINT, wstring) { called = !image; }, error), "repeat freeze");
        CancelSelection(); Check(called, "cancel selection without image");
    }
    Check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= resources + 2, "capture GDI handles released");
    DestroyWindow(pattern);
    filesystem::path file; Check(SavePng(*result, root, file, error), "save actual cropped image");
    bool dragged = false;
    Check(InitPreview(instance, [&] {
        dragged = true; Pump(1300); Check(PreviewVisible(), "timer paused during drag callback");
    }), "preview initialization");
    const HWND foreground = GetForegroundWindow();
    ShowPreview(result.get(), file, finished, 0);
    Check(PreviewVisible() && GetForegroundWindow() == foreground, "preview does not activate");
    Snapshot(FindWindowW(L"ScreenShotPlus.Preview", nullptr), root / L"preview-ui");
    Pump(150); Check(PreviewVisible(), "persistent preview"); HidePreview();
    Check(!PreviewVisible() && filesystem::exists(file), "closing preview preserves PNG");
    ShowPreview(result.get(), file, finished, 1);
    HWND preview = FindWindowW(L"ScreenShotPlus.Preview", nullptr);
    RECT rect{}; GetWindowRect(preview, &rect); POINT cursor{}; GetCursorPos(&cursor);
    SetWindowPos(preview, nullptr, cursor.x - (rect.right - rect.left) / 2, cursor.y - (rect.bottom - rect.top) / 2,
        0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
    Pump(1300); Check(PreviewVisible(), "timer paused while hovered");
    // Move only our own window away from the physical cursor.
    SetWindowPos(preview, nullptr, cursor.x + (rect.right - rect.left) + 30,
        rect.top, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
    const float scale = ui::Scale(preview);
    SendMessageW(preview, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(static_cast<int>(100 * scale), static_cast<int>(100 * scale)));
    SendMessageW(preview, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(static_cast<int>(140 * scale), static_cast<int>(100 * scale)));
    Check(dragged && PreviewVisible(), "drag threshold and paused timer");
    Pump(1300); Check(!PreviewVisible(), "preview timer expires"); DestroyPreview();
}
bool ProbeHotkey(UINT modifiers, UINT key) {
    return async(launch::async, [=] {
        const bool registered = RegisterHotKey(nullptr, 98, modifiers | MOD_NOREPEAT, key) != FALSE;
        if (registered) UnregisterHotKey(nullptr, 98);
        return registered;
    }).get();
}
void HotkeyTransactions(HINSTANCE instance, const filesystem::path& root) {
    WNDCLASSEXW cls{sizeof(cls)}, existing{sizeof(existing)};
    cls.lpfnWndProc = PatternProc; cls.hInstance = instance; cls.lpszClassName = L"ScreenShotPlus.TestPattern";
    if (!GetClassInfoExW(instance, cls.lpszClassName, &existing)) Check(RegisterClassExW(&cls) != 0, "hotkey test class");
    HWND window = CreateWindowExW(0, L"ScreenShotPlus.TestPattern", L"Hotkey tests", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    Check(window != nullptr, "hotkey test window");
    Config current = DefaultConfig(); current.folder = root / L"hotkey-images";
    current.modifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT; current.key = VK_F24;
    Hotkeys state; wstring error, warning;
    const auto file = root / L"hotkeys" / L"config.json";
    Check(ApplyConfiguredSettings(window, state, current, current, file, error), "initial hotkey setup");
    Check(!ProbeHotkey(current.modifiers, current.key), "active primary key is reserved");
    Config next = current; next.key = VK_F23;
    promise<bool> ready; auto registered = ready.get_future();
    jthread blocker([&](stop_token stop) {
        const bool ok = RegisterHotKey(nullptr, 99, next.modifiers | MOD_NOREPEAT, next.key) != FALSE;
        ready.set_value(ok);
        while (!stop.stop_requested()) this_thread::sleep_for(chrono::milliseconds(10));
        if (ok) UnregisterHotKey(nullptr, 99);
    });
    Check(registered.get(), "conflicting key registered on another thread");
    Check(!ApplyConfiguredSettings(window, state, current, next, file, error) && current.key == VK_F24,
        "hotkey conflict preserves current setting");
    Check(LoadConfig(file, warning).key == VK_F24 && !ProbeHotkey(current.modifiers, current.key),
        "conflict preserves persisted config and old registration");
    blocker.request_stop(); blocker.join();
    Check(ApplyConfiguredSettings(window, state, current, next, file, error) && current.key == VK_F23,
        "hotkey change succeeds once conflict removed");
    Check(ProbeHotkey(current.modifiers, VK_F24) && !ProbeHotkey(current.modifiers, VK_F23), "old registration released after change");
    next.key = VK_F22;
    Check(!ApplyConfiguredSettings(window, state, current, next, root / L"blocked" / L"config.json", error),
        "config failure rolls back candidate key");
    Check(current.key == VK_F23 && !ProbeHotkey(current.modifiers, VK_F23) && ProbeHotkey(current.modifiers, VK_F22),
        "config write failure keeps old key and releases candidate");
    ReleaseHotkeys(window, state); DestroyWindow(window);
    Check(ProbeHotkey(current.modifiers, VK_F23), "shutdown releases hotkey");
}
bool AppSmoke(HINSTANCE instance, const filesystem::path& artifacts) {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\ScreenShotPlus.Singleton");
    const DWORD created = GetLastError();
    Check(mutex != nullptr, "singleton mutex available");
    if (created == ERROR_ALREADY_EXISTS) { CloseHandle(mutex); return false; }
    const auto file = artifacts / L"app-config" / L"config.json";
    Config initial = DefaultConfig(); initial.folder = artifacts / L"app-screens";
    wstring error; Check(SaveConfig(file, initial, error), "isolated application config");
    const DWORD uiThread = GetCurrentThreadId(); atomic<bool> ok = false;
    jthread observer([&] {
        HWND panel = nullptr;
        for (int i = 0; i < 50 && !panel; ++i) {
            HWND candidate = FindWindowW(PANEL_CLASS, nullptr);
            DWORD pid = 0; if (candidate) GetWindowThreadProcessId(candidate, &pid);
            if (pid == GetCurrentProcessId() && IsWindowVisible(candidate)) panel = candidate;
            else this_thread::sleep_for(chrono::milliseconds(50));
        }
        try {
            Check(panel != nullptr, "app panel available");
            const auto executable = filesystem::current_path() / L"x64" / L"Release" / L"ScreenShotPlus.exe";
            STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION child{};
            Check(CreateProcessW(executable.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &child), "launch second instance");
            const DWORD waited = WaitForSingleObject(child.hProcess, 5000);
            if (waited != WAIT_OBJECT_0) TerminateProcess(child.hProcess, 1);
            DWORD exitCode = 1; GetExitCodeProcess(child.hProcess, &exitCode);
            CloseHandle(child.hThread); CloseHandle(child.hProcess);
            Check(waited == WAIT_OBJECT_0 && exitCode == 0, "second instance exits successfully");
            Snapshot(panel, artifacts / L"panel-ui"); Click(panel, 100, 370);
            HWND settings = FindWindowW(L"ScreenShotPlus.Settings", nullptr);
            Check(settings && IsWindowVisible(settings), "settings opens");
            Snapshot(settings, artifacts / L"settings-ui");
            Click(settings, 100, 472); Click(settings, 100, 612);
            this_thread::sleep_for(chrono::milliseconds(100));
            wstring warning;
            Check(!IsWindowVisible(settings) && LoadConfig(file, warning).previewSeconds == 5, "UI settings persisted");
            HWND pattern = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                L"ScreenShotPlus.TestPattern", L"Application capture test", WS_POPUP,
                100, 100, 400, 300, nullptr, nullptr, instance, nullptr);
            ShowWindow(pattern, SW_SHOWNOACTIVATE); UpdateWindow(pattern); DwmFlush();
            Click(panel, 100, 198);
            HWND overlay = FindWindowW(L"ScreenShotPlus.Selection", nullptr);
            Check(overlay != nullptr, "application starts region capture");
            const int x = 150 - GetSystemMetrics(SM_XVIRTUALSCREEN), y = 150 - GetSystemMetrics(SM_YVIRTUALSCREEN);
            SendMessageW(overlay, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
            SendMessageW(overlay, WM_LBUTTONUP, 0, MAKELPARAM(x + 80, y + 60));
            DestroyWindow(pattern);
            Check(PreviewVisible() && !IsWindowVisible(panel), "application displays saved preview");
            vector<filesystem::path> screenshots;
            for (const auto& item : filesystem::directory_iterator(initial.folder)) if (item.path().extension() == L".png") screenshots.push_back(item.path());
            Check(screenshots.size() == 1, "exactly one screenshot auto saved");
            { Bitmap image(screenshots[0].c_str()); Check(image.GetWidth() == 80 && image.GetHeight() == 60, "application PNG dimensions"); }
            SendMessageW(panel, WM_HOTKEY, 1, 0);
            overlay = FindWindowW(L"ScreenShotPlus.Selection", nullptr);
            Check(overlay != nullptr && !PreviewVisible(), "next capture hides old preview");
            SendMessageW(panel, WM_HOTKEY, 1, 0); SendMessageW(panel, WM_SHOW_PANEL, 0, 0);
            Check(FindWindowW(L"ScreenShotPlus.Selection", nullptr) == overlay && !IsWindowVisible(panel), "capture state prevents duplicate capture and panel");
            SendMessageW(overlay, WM_KEYDOWN, VK_ESCAPE, 0);
            Check(PreviewVisible() && filesystem::exists(screenshots[0]), "cancel restores previous saved preview");
            SendMessageW(panel, WM_SHOW_PANEL, 0, 0);
            Click(panel, 100, 425); ok = true;
        } catch (const exception& e) { cerr << "App smoke: " << e.what() << '\n'; PostThreadMessageW(uiThread, WM_QUIT, 1, 0); }
    });
    const int result = RunApp(instance, file);
    CloseHandle(mutex);
    Check(result == 0, "app message loop / shutdown");
    observer.join(); Check(ok, "app startup / settings / exit");
    return true;
}
}
int wmain(int argc, wchar_t** argv) {
    bool liveUi = !(argc > 1 && wstring_view(argv[1]) == L"--no-ui");
    HANDLE running = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\ScreenShotPlus.Singleton");
    if (running) { CloseHandle(running); liveUi = false; }
    const HRESULT ole = OleInitialize(nullptr);
    if (FAILED(ole)) return 2;
    GdiplusStartupInput input; ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &input, nullptr) != Ok) { OleUninitialize(); return 3; }
    const auto root = filesystem::current_path() / L"build" / L"test-artifacts" / to_wstring(GetCurrentProcessId());
    int code = 0;
    try {
        filesystem::create_directories(root);
        Geometry(); cout << "PASS selection geometry\n";
        Configuration(root); cout << "PASS config round trip / corrupt and unwritable paths\n";
        const auto file = PngStorage(root); cout << "PASS PNG pixels / Unicode paths / unique saves\n";
        FileTransfer(file); cout << "PASS Unicode file transfer / copy-only format\n";
        WritePermissions(root); cout << "PASS real ACL write denial / original config preserved / permissions restored\n";
        if (liveUi) {
            CaptureAndPreview(GetModuleHandleW(nullptr), root); cout << "PASS live capture / cancel / preview / timeout\n";
        } else cout << "SKIP live capture and preview UI: --no-ui or existing application instance\n";
        HotkeyTransactions(GetModuleHandleW(nullptr), root); cout << "PASS real hotkey conflict / change / write-failure rollback\n";
        if (liveUi && AppSmoke(GetModuleHandleW(nullptr), root))
            cout << "PASS application capture / persistence / state / single instance / shutdown\n";
        else cout << "SKIP application UI smoke: --no-ui or existing application instance\n";
        cout << "Monitors actually present: " << GetSystemMetrics(SM_CMONITORS) << '\n';
        wcout << L"Test artifacts: " << root.wstring() << L'\n';
    } catch (const exception& e) { cerr << "FAIL: " << e.what() << '\n'; code = 1; }
    CancelSelection(); DestroySettings(); DestroyPreview();
    GdiplusShutdown(token); OleUninitialize(); return code;
}
