#include "app.h"
#include "capture.h"
#include "storage.h"
#include "config.h"
#include "dragdrop.h"
#include "preview.h"
#include "settings.h"
#include "ui.h"
#include "hotkeys.h"
#include "annotation.h"
#include "editor.h"
#include "instance.h"
#include "tray.h"
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
struct TestStartupKey {
    wstring key = L"Software\\ScreenShotPlus.Tests-" + to_wstring(GetCurrentProcessId()) + L"-" + to_wstring(GetTickCount64());
    ~TestStartupKey() { RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str()); }
};
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
HWND OwnWindow(const wchar_t* className) {
    struct Search { const wchar_t* name; HWND window = nullptr; } search{className};
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        auto& search = *reinterpret_cast<Search*>(param);
        DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
        wchar_t name[128]{}; GetClassNameW(hwnd, name, 128);
        if (pid == GetCurrentProcessId() && wstring_view(name) == search.name) { search.window = hwnd; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.window;
}
void Pointer(HWND hwnd, UINT message, int x, int y, WPARAM keys = 0) {
    const float scale = ui::Scale(hwnd);
    SendMessageW(hwnd, message, keys, MAKELPARAM(static_cast<int>(x * scale), static_cast<int>(y * scale)));
}
void Snapshot(HWND hwnd, const filesystem::path& folder) {
    UpdateWindow(hwnd);
    RECT rect{}; GetClientRect(hwnd, &rect);
    HDC dc = GetDC(hwnd), memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, rect.right, rect.bottom);
    HGDIOBJ old = SelectObject(memory, bitmap);
    const bool copied = SendMessageTimeoutW(hwnd, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT,
        SMTO_ABORTIFHUNG, 2000, nullptr) != 0;
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
vector<DWORD> notifyCalls;
int notifyFailures = 0;
bool failTrayVersion = false;
BOOL WINAPI FakeNotify(DWORD operation, PNOTIFYICONDATAW) {
    notifyCalls.push_back(operation);
    if (operation == NIM_SETVERSION) return !failTrayVersion;
    if (notifyFailures > 0) { --notifyFailures; return FALSE; }
    return TRUE;
}
void TrayTransitions() {
    TrayIcon tray;
    notifyFailures = 2;
    Check(!RegisterTrayIcon(tray, FakeNotify) && !tray.registered && !tray.modern, "failed tray registration is reported");
    notifyCalls.clear();
    Check(RegisterTrayIcon(tray, FakeNotify) && tray.registered && tray.modern &&
        notifyCalls == vector<DWORD>{NIM_ADD, NIM_SETVERSION}, "tray recovery negotiates version after successful add");
    notifyCalls.clear(); notifyFailures = 1;
    Check(RegisterTrayIcon(tray, FakeNotify) && notifyCalls == vector<DWORD>{NIM_MODIFY, NIM_ADD, NIM_SETVERSION},
        "lost tray icon can be re-added");
    failTrayVersion = true;
    Check(RegisterTrayIcon(tray, FakeNotify) && !tray.modern, "old tray event version remains usable");
    failTrayVersion = false;
    for (UINT message : {NIN_SELECT, NIN_KEYSELECT, WM_CONTEXTMENU, WM_LBUTTONUP, WM_RBUTTONUP})
        Check(TrayEventOpensPanel(MAKELPARAM(message, 1)), "modern and legacy tray click events recognized");
    Check(!TrayEventOpensPanel(MAKELPARAM(WM_MOUSEMOVE, 1)), "tray hover does not open panel");
}
void InstanceProcesses(const filesystem::path& executable) {
    const wstring suffix = to_wstring(GetCurrentProcessId()) + L"-" + to_wstring(GetTickCount64());
    const wstring mutex = L"Local\\ScreenShotPlus.TestMutex-" + suffix, gateName = L"Local\\ScreenShotPlus.TestGate-" + suffix;
    HANDLE gate = CreateEventW(nullptr, TRUE, FALSE, gateName.c_str());
    Check(gate != nullptr, "instance fixture gate");
    struct Children {
        HANDLE gate; vector<PROCESS_INFORMATION> processes;
        ~Children() {
            SetEvent(gate);
            for (auto& process : processes) {
                if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 95);
                CloseHandle(process.hThread); CloseHandle(process.hProcess);
            }
            CloseHandle(gate);
        }
    } children{gate, {}};
    for (int i = 0; i < 8; ++i) {
        wstring command = L"\"" + executable.wstring() + L"\" --instance-probe \"" + mutex + L"\" \"" + gateName + L"\"";
        STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process), "start concurrent instance child");
        children.processes.push_back(process);
    }
    int exited = 0;
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
        exited = 0;
        for (const auto& process : children.processes) if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) ++exited;
        if (exited >= 7) break;
        Sleep(20);
    }
    Check(exited == 7, "one instance remains alive while seven duplicates exit");
    SetEvent(gate);
    int first = 0, duplicate = 0;
    for (const auto& process : children.processes) {
        Check(WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0, "instance child exits");
        DWORD code = 0; GetExitCodeProcess(process.hProcess, &code);
        if (code == 71) ++first; else if (code == 72) ++duplicate;
    }
    Check(first == 1 && duplicate == 7, "real concurrent processes share a single named mutex");
    InstanceLock restarted;
    Check(AcquireInstanceLock(restarted, mutex.c_str()) && restarted.first, "shutdown releases instance mutex");
}
LRESULT CALLBACK ActivationProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_SHOW_PANEL) { SetPropW(hwnd, L"Activated", reinterpret_cast<HANDLE>(1)); return 0; }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}
void InstanceActivation(HINSTANCE instance) {
    constexpr wchar_t name[] = L"ScreenShotPlus.TestActivation";
    WNDCLASSW cls{}; cls.lpfnWndProc = ActivationProc; cls.hInstance = instance; cls.lpszClassName = name;
    Check(RegisterClassW(&cls) != 0, "activation fixture class");
    atomic<bool> delivered = false;
    jthread duplicate([&] { delivered = ActivateExistingInstance(3000, name); });
    Sleep(150);
    HWND window = CreateWindowExW(0, name, L"Activation fixture", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, instance, nullptr);
    Check(window != nullptr, "delayed primary window");
    SetPropW(window, INSTANCE_PANEL_MARKER, reinterpret_cast<HANDLE>(1));
    Pump(250); duplicate.join();
    Check(delivered && GetPropW(window, L"Activated"), "duplicate activation waits for primary window to exist");
    DestroyWindow(window);
    Check(!ActivateExistingInstance(50, name), "missing primary window returns failure");
}
void FileTransfer(const filesystem::path& file);
void AnnotationPixels(const filesystem::path& root) {
    Bitmap source(160, 100, PixelFormat32bppARGB);
    { Graphics g(&source); g.Clear(Color(255, 245, 245, 245)); }
    Annotation stroke; stroke.points = {{10, 15}, {100, 15}}; stroke.size = 6; stroke.color = Color(255, 255, 0, 0);
    Annotation dot; dot.points = {{25, 35}}; dot.size = 8; dot.color = Color(255, 0, 96, 255);
    Annotation text; text.kind = Annotation::Text; text.points = {{10, 50}}; text.size = 24; text.text = L"标注 Test";
    text.color = Color(255, 0, 180, 0);
    wstring error; auto result = RenderAnnotations(source, {stroke, dot, text}, error);
    Check(result && result->GetWidth() == 160 && result->GetHeight() == 100, "edited image retains physical dimensions");
    Color pixel; result->GetPixel(50, 15, &pixel); Check(pixel.GetR() == 255 && pixel.GetG() == 0 && pixel.GetA() == 255, "stroke saved in physical pixels");
    result->GetPixel(25, 35, &pixel); Check(pixel.GetB() == 255 && pixel.GetR() == 0, "single click brush dot");
    bool textFound = false;
    for (int y = 50; y < 90; ++y) for (int x = 10; x < 150; ++x) {
        result->GetPixel(x, y, &pixel); if (pixel.GetG() > pixel.GetR() + 30 && pixel.GetG() > pixel.GetB() + 30) textFound = true;
    }
    Check(textFound, "colored Unicode text rasterized");
    source.GetPixel(50, 15, &pixel); Check(pixel.GetR() == 245 && pixel.GetG() == 245, "source image unchanged");
    filesystem::path file; Check(SavePng(*result, root / L"编辑后的图片", file, error), "edited PNG saved");
    { Bitmap decoded(file.c_str()); decoded.GetPixel(50, 15, &pixel); Check(pixel.GetR() == 255 && pixel.GetG() == 0 && decoded.GetWidth() == 160, "edited PNG pixel round trip"); }
    FileTransfer(file);
}
void Configuration(const filesystem::path& root) {
    const auto file = root / L"设置" / L"config.json";
    Config c = DefaultConfig(); c.folder = root / L"截图 中文 空格";
    c.modifiers = MOD_CONTROL | MOD_ALT; c.key = VK_F9; c.printScreen = true; c.previewSeconds = 15; c.startup = true;
    wstring error;
    Check(SaveConfig(file, c, error), "save config");
    wstring warning;
    auto loaded = LoadConfig(file, warning);
    Check(warning.empty() && loaded.folder == c.folder && loaded.key == VK_F9 && loaded.previewSeconds == 15 && loaded.printScreen && loaded.startup, "Unicode config round trip");
    c.previewSeconds = 0; Check(SaveConfig(file, c, error), "replace existing config atomically");
    Check(LoadConfig(file, warning).previewSeconds == 0, "updated config persisted");
    nlohmann::json data;
    { ifstream in(file); in >> data; }
    auto legacy = data; legacy.erase("startup");
    { ofstream out(file); out << legacy; }
    Check(!LoadConfig(file, warning).startup && warning.empty(), "legacy config defaults startup off");
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
    Check(StartupCommand(L"C:\\便携 工具\\ScreenShotPlus.exe", error) == L"\"C:\\便携 工具\\ScreenShotPlus.exe\" --tray", "startup command quotes Unicode spaced paths");
    Check(StartupCommand(filesystem::path(L"C:\\" + wstring(260, L'a') + L".exe"), error).empty(), "oversized Run command rejected");
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
void EditorUi(HINSTANCE instance, const filesystem::path& root) {
    Bitmap source(640, 360, PixelFormat32bppARGB);
    { Graphics g(&source); g.Clear(Color(255, 245, 245, 245)); }
    unique_ptr<Bitmap> saved; filesystem::path file; int attempts = 0, canceled = 0;
    Check(InitEditor(instance, nullptr, [&](unique_ptr<Bitmap>& image, wstring& error) {
        ++attempts;
        if (attempts == 1) return SavePng(*image, root / L"blocked", file, error);
        if (!SavePng(*image, root / L"editor-ui-results", file, error)) return false;
        saved = move(image); return true;
    }, [&] { ++canceled; }), "initialize custom editor");
    wstring error; Check(ShowEditor(source, error), "open editor");
    HWND hwnd = OwnWindow(L"ScreenShotPlus.Editor"); Check(hwnd && EditorVisible(), "editor window visible");
    Pointer(hwnd, WM_LBUTTONDOWN, 240, 247, MK_LBUTTON);
    Pointer(hwnd, WM_MOUSEMOVE, 320, 247, MK_LBUTTON);
    Pointer(hwnd, WM_LBUTTONUP, 400, 247);
    Click(hwnd, 250, 267); Click(hwnd, 520, 96);
    Click(hwnd, 170, 96); Click(hwnd, 240, 307);
    for (const wchar_t c : wstring(L"标注 A")) SendMessageW(hwnd, WM_CHAR, c, 0);
    Click(hwnd, 442, 96); Click(hwnd, 442, 96);
    Click(hwnd, 430, 144); Snapshot(hwnd, root / L"editor-text-ui");
    SendMessageW(hwnd, WM_CHAR, L'\r', 0);
    Click(hwnd, 245, 312); Click(hwnd, 510, 144);
    Click(hwnd, 660, 96);
    Click(hwnd, 910, 144); Click(hwnd, 870, 232); Click(hwnd, 814, 280); Click(hwnd, 934, 328);
    Snapshot(hwnd, root / L"editor-rgb-ui");
    Click(hwnd, 910, 144); Click(hwnd, 70, 96);
    Pointer(hwnd, WM_LBUTTONDOWN, 240, 447, MK_LBUTTON);
    Pointer(hwnd, WM_LBUTTONUP, 400, 447);
    Snapshot(hwnd, root / L"editor-ui");
    Click(hwnd, 900, 724);
    Check(EditorVisible() && attempts == 1 && !saved, "save failure retains editor and annotations");
    Snapshot(hwnd, root / L"editor-error-ui"); Click(hwnd, 900, 724);
    Check(!EditorVisible() && saved && attempts == 2 && filesystem::exists(file), "retry saves edited PNG and closes editor");
    Color pixel; saved->GetPixel(100, 40, &pixel); Check(pixel.GetR() == 255 && pixel.GetG() == 0, "editor brush coordinates map to image pixels");
    saved->GetPixel(50, 60, &pixel); Check(pixel.GetR() == 245 && pixel.GetG() == 245, "editor undo removes latest dot");
    saved->GetPixel(100, 240, &pixel);
    Check(abs(pixel.GetR() - 128) <= 2 && abs(pixel.GetG() - 64) <= 2 && abs(pixel.GetB() - 200) <= 2, "RGB custom pen exported");
    bool purple = false;
    for (int y = 100; y < 150; ++y) for (int x = 40; x < 220; ++x) {
        saved->GetPixel(x, y, &pixel); if (pixel.GetR() > pixel.GetG() + 50 && pixel.GetB() > pixel.GetG() + 50) purple = true;
    }
    Check(purple, "existing text size and color edits exported");
    FileTransfer(file);
    Check(ShowEditor(*saved, error), "reopen edited PNG"); Click(hwnd, 750, 724);
    Check(!EditorVisible() && canceled == 1 && attempts == 2, "cancel preserves last saved image");
    Bitmap large(1984, 828, PixelFormat32bppARGB);
    { Graphics g(&large); g.Clear(Color(255, 245, 245, 245)); }
    Check(ShowEditor(large, error), "open image larger than editor canvas");
    Pointer(hwnd, WM_LBUTTONDOWN, 124, 280, MK_LBUTTON);
    Pointer(hwnd, WM_LBUTTONUP, 224, 280); Click(hwnd, 900, 724);
    saved->GetPixel(300, 200, &pixel);
    Check(saved->GetWidth() == 1984 && saved->GetHeight() == 828 && pixel.GetR() == 255 && pixel.GetG() == 0,
        "scaled-down canvas maps annotation to original physical pixels");
    DestroyEditor();
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
    bool dragged = false, edited = false;
    Check(InitPreview(instance, [&] {
        dragged = true; Pump(1300); Check(PreviewVisible(), "timer paused during drag callback");
    }, [&] { edited = true; }), "preview initialization");
    const HWND foreground = GetForegroundWindow();
    ShowPreview(result.get(), file, finished, 0);
    Check(PreviewVisible() && GetForegroundWindow() == foreground, "preview does not activate");
    Snapshot(FindWindowW(L"ScreenShotPlus.Preview", nullptr), root / L"preview-ui");
    Pump(150); Check(PreviewVisible(), "persistent preview"); HidePreview();
    Check(!PreviewVisible() && filesystem::exists(file), "closing preview preserves PNG");
    ShowPreview(result.get(), file, finished, 1);
    HWND preview = OwnWindow(L"ScreenShotPlus.Preview");
    Click(preview, 100, 100); Check(edited && !dragged, "preview click edits without dragging"); edited = false;
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
    Pointer(preview, WM_LBUTTONUP, 140, 100);
    Check(dragged && !edited && PreviewVisible(), "drag threshold and paused timer without editor");
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
    TestStartupKey startup;
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
    Check(ApplyConfiguredSettings(window, state, current, current, file, error, startup.key.c_str()), "initial hotkey setup");
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
    Check(!ApplyConfiguredSettings(window, state, current, next, file, error, startup.key.c_str()) && current.key == VK_F24,
        "hotkey conflict preserves current setting");
    Check(LoadConfig(file, warning).key == VK_F24 && !ProbeHotkey(current.modifiers, current.key),
        "conflict preserves persisted config and old registration");
    blocker.request_stop(); blocker.join();
    next.startup = true;
    const bool startupAvailable = ApplyConfiguredSettings(window, state, current, next, file, error, startup.key.c_str());
    if (!startupAvailable) {
        Check(error.find(L"错误 5") != wstring::npos, "startup skip is only allowed for real access denial");
        Check(current.key == VK_F24 && !current.startup && ProbeHotkey(current.modifiers, VK_F23), "startup failure rolls back hotkey");
        cout << "SKIP startup enable / disable / registry rollback: registry writes denied by execution environment\n";
        next.startup = false;
        Check(ApplyConfiguredSettings(window, state, current, next, file, error, startup.key.c_str()), "settings still usable after startup denial");
    }
    Check(current.key == VK_F23,
        "hotkey change succeeds once conflict removed");
    Check(ProbeHotkey(current.modifiers, VK_F24) && !ProbeHotkey(current.modifiers, VK_F23), "old registration released after change");
    StartupEntry original;
    if (startupAvailable) {
        Check(ReadStartupEntry(startup.key.c_str(), original, error) && original.exists, "startup registered in isolated key");
        vector<wchar_t> command(original.data.size() / sizeof(wchar_t));
        memcpy(command.data(), original.data.data(), original.data.size());
        Check(wstring(command.data()).starts_with(L"\"") && wstring(command.data()).ends_with(L"\" --tray"), "startup quoted path and tray argument");
    }
    next.key = VK_F22; next.startup = false;
    Check(!ApplyConfiguredSettings(window, state, current, next, root / L"blocked" / L"config.json", error, startup.key.c_str()),
        "config failure rolls back candidate key");
    Check(current.key == VK_F23 && !ProbeHotkey(current.modifiers, VK_F23) && ProbeHotkey(current.modifiers, VK_F22),
        "config write failure keeps old key and releases candidate");
    StartupEntry restored;
    Check(ReadStartupEntry(startup.key.c_str(), restored, error) && restored.exists == original.exists && restored.data == original.data,
        "config failure restores startup entry exactly");
    Check(ApplyConfiguredSettings(window, state, current, next, file, error, startup.key.c_str()) && !current.startup,
        "disable startup saves configuration");
    Check(ReadStartupEntry(startup.key.c_str(), restored, error) && !restored.exists, "disable removes only application entry");
    ReleaseHotkeys(window, state); DestroyWindow(window);
    Check(ProbeHotkey(current.modifiers, VK_F22), "shutdown releases hotkey");
}
bool AppSmoke(HINSTANCE instance, const filesystem::path& artifacts, bool isolated = false) {
    TestStartupKey startupKey;
    HANDLE mutex = isolated ? nullptr : CreateMutexW(nullptr, FALSE, L"Local\\ScreenShotPlus.Singleton");
    const DWORD created = isolated ? ERROR_SUCCESS : GetLastError();
    Check(isolated || mutex != nullptr, "singleton mutex available");
    if (created == ERROR_ALREADY_EXISTS) { CloseHandle(mutex); return false; }
    const auto file = artifacts / L"app-config" / L"config.json";
    Config initial = DefaultConfig(); initial.folder = artifacts / L"app-screens";
    if (isolated) { initial.modifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT; initial.key = VK_F20; }
    wstring error; Check(SaveConfig(file, initial, error), "isolated application config");
    const DWORD uiThread = GetCurrentThreadId(); atomic<bool> ok = false;
    jthread observer([&] {
        HWND panel = nullptr;
        for (int i = 0; i < 50 && !panel; ++i) {
            HWND candidate = OwnWindow(PANEL_CLASS);
            DWORD pid = 0; if (candidate) GetWindowThreadProcessId(candidate, &pid);
            if (pid == GetCurrentProcessId() && GetPropW(candidate, L"ScreenShotPlus.Ready") && (isolated || IsWindowVisible(candidate))) panel = candidate;
            else this_thread::sleep_for(chrono::milliseconds(50));
        }
        try {
            Check(panel != nullptr, "app panel available");
            if (isolated) {
                if (GetPropW(panel, L"ScreenShotPlus.TrayRegistered")) {
                    Check(!IsWindowVisible(panel), "successful tray startup does not show panel");
                    NOTIFYICONIDENTIFIER id{sizeof(id)}; id.hWnd = panel; id.uID = 1; RECT rect{};
                    Check(SUCCEEDED(Shell_NotifyIconGetRect(&id, &rect)) && rect.right > rect.left, "registered icon has actual Shell rectangle");
                    cout << "PASS real Shell tray registration / icon rectangle\n";
                } else {
                    Check(IsWindowVisible(panel), "failed tray startup preserves accessible panel");
                    SendMessageW(panel, WM_CLOSE, 0, 0);
                    Check(IsWindowVisible(panel), "panel refuses to hide when tray is unavailable");
                    cout << "SKIP actual tray display: Shell rejected registration; failure panel / retry verified\n";
                }
                SendMessageW(panel, WM_SHOW_PANEL, 0, 0);
                Check(IsWindowVisible(panel), "tray startup panel can be opened");
            } else {
            SetPropW(panel, INSTANCE_PANEL_MARKER, reinterpret_cast<HANDLE>(1));
            const auto executable = filesystem::current_path() / L"x64" / L"Release" / L"ScreenShotPlus.exe";
            STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION child{};
            Check(CreateProcessW(executable.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &child), "launch second instance");
            const DWORD waited = WaitForSingleObject(child.hProcess, 5000);
            if (waited != WAIT_OBJECT_0) TerminateProcess(child.hProcess, 1);
            DWORD exitCode = 1; GetExitCodeProcess(child.hProcess, &exitCode);
            CloseHandle(child.hThread); CloseHandle(child.hProcess);
            Check(waited == WAIT_OBJECT_0 && exitCode == 0, "second instance exits successfully");
            }
            Snapshot(panel, artifacts / L"panel-ui"); Click(panel, 100, 370);
            HWND settings = OwnWindow(L"ScreenShotPlus.Settings");
            Check(settings && IsWindowVisible(settings), "settings opens");
            Snapshot(settings, artifacts / L"settings-ui");
            Click(settings, 100, 472); Click(settings, 100, 674);
            this_thread::sleep_for(chrono::milliseconds(100));
            wstring warning;
            Check(!IsWindowVisible(settings) && LoadConfig(file, warning).previewSeconds == 5, "UI settings persisted");
            HWND pattern = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                L"ScreenShotPlus.TestPattern", L"Application capture test", WS_POPUP,
                100, 100, 400, 300, nullptr, nullptr, instance, nullptr);
            ShowWindow(pattern, SW_SHOWNOACTIVATE); UpdateWindow(pattern); DwmFlush();
            Click(panel, 100, 198);
            HWND overlay = OwnWindow(L"ScreenShotPlus.Selection");
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
            HWND preview = OwnWindow(L"ScreenShotPlus.Preview"); Click(preview, 100, 100);
            HWND editor = OwnWindow(L"ScreenShotPlus.Editor");
            Check(EditorVisible() && !PreviewVisible(), "preview click opens editor and hides preview");
            SendMessageW(panel, WM_HOTKEY, 1, 0); SendMessageW(panel, WM_SHOW_PANEL, 0, 0);
            Check(!OwnWindow(L"ScreenShotPlus.Selection") && !IsWindowVisible(panel) && EditorVisible(), "editing blocks capture and panel");
            Pointer(editor, WM_LBUTTONDOWN, 490, 370, MK_LBUTTON);
            Pointer(editor, WM_LBUTTONUP, 540, 370); Click(editor, 900, 724);
            Check(!EditorVisible() && PreviewVisible(), "edited save returns to preview");
            vector<filesystem::path> editedFiles;
            for (const auto& item : filesystem::directory_iterator(initial.folder))
                if (item.path().extension() == L".png" && item.path() != screenshots[0]) editedFiles.push_back(item.path());
            Check(editedFiles.size() == 1 && filesystem::exists(screenshots[0]), "edited image saved separately from original");
            { Bitmap image(editedFiles[0].c_str()); Color pixel; image.GetPixel(30, 13, &pixel);
                Check(image.GetWidth() == 80 && image.GetHeight() == 60 && pixel.GetR() == 255 && pixel.GetG() == 0, "application saves annotations at original resolution"); }
            FileTransfer(editedFiles[0]);
            Click(preview, 100, 100); Check(EditorVisible(), "edited preview can reopen editor");
            SendMessageW(editor, WM_CLOSE, 0, 0); Check(PreviewVisible(), "cancel editing restores latest preview");
            SendMessageW(panel, WM_HOTKEY, 1, 0);
            overlay = OwnWindow(L"ScreenShotPlus.Selection");
            Check(overlay != nullptr && !PreviewVisible(), "next capture hides old preview");
            SendMessageW(panel, WM_HOTKEY, 1, 0); SendMessageW(panel, WM_SHOW_PANEL, 0, 0);
            Check(OwnWindow(L"ScreenShotPlus.Selection") == overlay && !IsWindowVisible(panel), "capture state prevents duplicate capture and panel");
            SendMessageW(overlay, WM_KEYDOWN, VK_ESCAPE, 0);
            Check(PreviewVisible() && filesystem::exists(screenshots[0]), "cancel restores previous saved preview");
            SendMessageW(panel, WM_SHOW_PANEL, 0, 0);
            Click(panel, 100, 425); ok = true;
        } catch (const exception& e) { cerr << "App smoke: " << e.what() << '\n'; PostThreadMessageW(uiThread, WM_QUIT, 1, 0); }
    });
    const int result = RunApp(instance, file, isolated, startupKey.key.c_str());
    if (mutex) CloseHandle(mutex);
    Check(result == 0, "app message loop / shutdown");
    observer.join(); Check(ok, "app startup / settings / exit");
    return true;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && wstring_view(argv[1]) == L"--instance-probe") {
        InstanceLock lock;
        if (!AcquireInstanceLock(lock, argv[2])) return 91;
        if (!lock.first) return 72;
        HANDLE gate = OpenEventW(SYNCHRONIZE, FALSE, argv[3]);
        if (!gate) return 92;
        const DWORD waited = WaitForSingleObject(gate, 10000); CloseHandle(gate);
        return waited == WAIT_OBJECT_0 ? 71 : 93;
    }
    const bool isolatedUi = argc > 1 && wstring_view(argv[1]) == L"--isolated-ui";
    bool liveUi = !(argc > 1 && wstring_view(argv[1]) == L"--no-ui");
    HANDLE running = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\ScreenShotPlus.Singleton");
    if (running) { CloseHandle(running); if (!isolatedUi) liveUi = false; }
    const HRESULT ole = OleInitialize(nullptr);
    if (FAILED(ole)) return 2;
    GdiplusStartupInput input; ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &input, nullptr) != Ok) { OleUninitialize(); return 3; }
    const auto root = filesystem::current_path() / L"build" / L"test-artifacts" / to_wstring(GetCurrentProcessId());
    int code = 0;
    try {
        filesystem::create_directories(root);
        Geometry(); cout << "PASS selection geometry\n";
        InstanceProcesses(filesystem::absolute(argv[0])); cout << "PASS 8 concurrent processes / single-instance mutex / restart\n";
        InstanceActivation(GetModuleHandleW(nullptr)); cout << "PASS delayed primary window / duplicate activation\n";
        TrayTransitions(); cout << "PASS simulated Shell failure / recovery / legacy event handling (not actual tray acceptance)\n";
        Configuration(root); cout << "PASS config round trip / corrupt and unwritable paths\n";
        const auto file = PngStorage(root); cout << "PASS PNG pixels / Unicode paths / unique saves\n";
        FileTransfer(file); cout << "PASS Unicode file transfer / copy-only format\n";
        AnnotationPixels(root); cout << "PASS physical-pixel stroke / dot / Unicode text / edited PNG transfer\n";
        WritePermissions(root); cout << "PASS real ACL write denial / original config preserved / permissions restored\n";
        if (liveUi) {
            CaptureAndPreview(GetModuleHandleW(nullptr), root); cout << "PASS live capture / cancel / preview / timeout\n";
            EditorUi(GetModuleHandleW(nullptr), root); cout << "PASS editor brush / RGB / text / undo / failed save retry / cancel\n";
        } else cout << "SKIP live capture and preview UI: --no-ui or existing application instance\n";
        HotkeyTransactions(GetModuleHandleW(nullptr), root); cout << "PASS real hotkey conflict / change / write-failure rollback\n";
        if (liveUi && AppSmoke(GetModuleHandleW(nullptr), root, isolatedUi))
            cout << (isolatedUi ? "PASS isolated application tray startup / capture / edit / preview / state / shutdown (single-instance test skipped)\n" :
                "PASS application capture / edit / persistence / state / single instance / shutdown\n");
        else cout << "SKIP application UI smoke: --no-ui or existing application instance\n";
        cout << "Monitors actually present: " << GetSystemMetrics(SM_CMONITORS) << '\n';
        wcout << L"Test artifacts: " << root.wstring() << L'\n';
    } catch (const exception& e) { cerr << "FAIL: " << e.what() << '\n'; code = 1; }
    CancelSelection(); DestroyEditor(); DestroySettings(); DestroyPreview();
    GdiplusShutdown(token); OleUninitialize(); return code;
}
