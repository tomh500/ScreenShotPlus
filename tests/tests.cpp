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
#include "visual_effects.h"
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
struct SelectionGuard { ~SelectionGuard() { CancelSelection(); } };
void Check(bool condition, const char* message) { if (!condition) throw runtime_error(message); }
void Pump(unsigned milliseconds) {
    const auto end = GetTickCount64() + milliseconds;
    while (GetTickCount64() < end) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
            // A continuously animated high-refresh window may enqueue another
            // frame before the queue is empty. Preserve the requested sample
            // time instead of draining that queue until the whole tween ends.
            if (GetTickCount64() >= end) return;
        }
        const auto now = GetTickCount64(); if (now >= end) return;
        MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(min<ULONGLONG>(20, end - now)), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
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
void Snapshot(HWND hwnd, const filesystem::path& folder, bool settle = true) {
    // Acceptance captures show the settled layout, not the entrance veil.
    if (settle) Pump(300);
    UpdateWindow(hwnd);
    RECT rect{}; GetClientRect(hwnd, &rect);
    HDC dc = GetDC(hwnd), memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, rect.right, rect.bottom);
    HGDIOBJ old = SelectObject(memory, bitmap);
    DWORD process = 0; GetWindowThreadProcessId(hwnd, &process);
    const bool copied = process == GetCurrentProcessId() ?
        SendMessageTimeoutW(hwnd, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT, SMTO_ABORTIFHUNG, 2000, nullptr) != 0 :
        PrintWindow(hwnd, memory, PW_CLIENTONLY) != FALSE;
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
COLORREF PrintedPixel(HWND hwnd, int x, int y) {
    RECT rect{}; GetClientRect(hwnd, &rect);
    HDC dc = GetDC(hwnd), memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, rect.right, rect.bottom);
    HGDIOBJ old = bitmap && memory ? SelectObject(memory, bitmap) : nullptr;
    const bool copied = old && old != HGDI_ERROR && SendMessageTimeoutW(hwnd, WM_PRINTCLIENT,
        reinterpret_cast<WPARAM>(memory), PRF_CLIENT, SMTO_ABORTIFHUNG, 2000, nullptr) != 0;
    const auto pixel = copied ? GetPixel(memory, x, y) : CLR_INVALID;
    if (old && old != HGDI_ERROR) SelectObject(memory, old);
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    if (dc) ReleaseDC(hwnd, dc);
    Check(pixel != CLR_INVALID, "read rendered UI fixture pixel");
    return pixel;
}
unique_ptr<Bitmap> NativePixels(HWND hwnd, RECT band) {
    DWORD process = 0; GetWindowThreadProcessId(hwnd, &process);
    Check(process == GetCurrentProcessId(), "native pixel read is restricted to our own UI fixture");
    const int width = band.right - band.left, height = band.bottom - band.top;
    HDC dc = GetDC(hwnd), memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
    HGDIOBJ old = bitmap && memory ? SelectObject(memory, bitmap) : nullptr;
    GdiFlush();
    const bool copied = old && old != HGDI_ERROR && BitBlt(memory, 0, 0, width, height, dc, band.left, band.top, SRCCOPY) != FALSE;
    if (old && old != HGDI_ERROR) SelectObject(memory, old);
    if (memory) DeleteDC(memory);
    if (dc) ReleaseDC(hwnd, dc);
    unique_ptr<Bitmap> pixels(copied ? Bitmap::FromHBITMAP(bitmap, nullptr) : nullptr);
    if (bitmap) DeleteObject(bitmap);
    Check(pixels && pixels->GetLastStatus() == Ok, "read native painted pixels without forcing WM_PRINTCLIENT repaint");
    return pixels;
}
uint64_t PixelHash(Bitmap& image) {
    uint64_t result = 1469598103934665603ULL;
    for (UINT y = 0; y < image.GetHeight(); ++y) for (UINT x = 0; x < image.GetWidth(); ++x) {
        Color color; Check(image.GetPixel(x, y, &color) == Ok, "hash native fixture pixel");
        result = (result ^ color.GetValue()) * 1099511628211ULL;
    }
    return result;
}
void AnimationTiming() {
    Check(abs(ui::RefreshRateValue(60000, 1001) - 59.94005994) < 0.00001, "fractional display refresh rate preserved");
    Check(ui::RefreshRateValue(144, 1) == 144 && ui::RefreshRateValue(240, 1) == 240, "high refresh rate not capped at 120");
    for (const auto rate : {ui::RefreshRateValue(0, 1), ui::RefreshRateValue(1, 1), ui::RefreshRateValue(120, 0), ui::RefreshRateValue(2000, 1)})
        Check(rate == 120, "unknown or invalid refresh rate falls back to 120");
    ui::Tween curve;
    curve.Set(1, 10, 0.2);
    Check(curve.Value(9) == 0 && curve.Value(10) == 0 && curve.Value(10.2) == 1, "animation endpoints clamp without overshoot");
    Check(abs(curve.Value(10.1) - 0.875f) < 0.0001f, "time-based cubic easing");
    for (const double rate : {59.94, 60.0, 120.0, 144.0, 240.0}) {
        float previous = 0;
        for (int frame = 0; frame <= static_cast<int>(ceil(rate * 0.2)); ++frame) {
            const float value = curve.Value(10 + frame / rate);
            Check(value >= previous && value <= 1, "animation monotonic at all refresh rates"); previous = value;
        }
        Check(!curve.Active(10.21), "animation duration independent of refresh rate");
    }
    const float before = curve.Value(10.08);
    curve.Set(0, 10.08, 0.16);
    Check(abs(curve.Value(10.08) - before) < 0.0001f && curve.Value(10.25) == 0, "interrupted transition reverses continuously");
    curve.Set(1, 11, 0.2, false);
    Check(curve.Value(11) == 1 && !curve.Active(11), "reduced motion settles immediately");
    Check(ui::SpringReveal(0) == 0 && ui::SpringReveal(1) == 1, "spring lands at exact endpoints");
    float peak = 0;
    for (int frame = 0; frame <= 240; ++frame) {
        const float value = ui::SpringReveal(frame / 240.0f);
        Check(isfinite(value) && value >= 0 && value < 1.10f, "spring overshoot stays subtle and bounded");
        peak = max(peak, value);
    }
    Check(peak > 1.01f, "spring has a restrained settling rebound");
    const array<float, 5> strengths{0, 0.4f, 0.75f, 1, 1.3f};
    double previousDuration = 0;
    for (unsigned level = 0; level < strengths.size(); ++level) {
        ui::SetMotionOptions(level, 1);
        Check(ui::MotionLevel() == level && abs(ui::MotionAmount() - strengths[level]) < 0.001f,
            "five tiers expose distinct restrained-to-ultimate motion strength");
        const double duration = ui::MotionDuration(0.2);
        Check(isfinite(duration) && duration > previousDuration, "higher motion tiers allow progressively longer settling");
        previousDuration = duration;
    }
    ui::SetMotionOptions(0, 1);
    Check(!ui::DecorativeMotion() && ui::MotionAmount() == 0, "light tier disables ordinary decorative motion");
    ui::SetMotionOptions(2, 0); const double quick = ui::MotionDuration(0.2);
    ui::SetMotionOptions(2, 1); const double normal = ui::MotionDuration(0.2);
    ui::SetMotionOptions(2, 2); const double relaxed = ui::MotionDuration(0.2);
    Check(quick < normal && normal < relaxed && quick > 0, "three speed settings retain time-based positive duration");
    ui::SetMotionOptions(2, 1);
}
struct MotionFixture { bool hovered = false, enabled = false; };
LRESULT CALLBACK MotionFixtureProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ui::MotionMessage(hwnd, message, wparam, lparam)) return 0;
    if (message == WM_NCCREATE) SetWindowLongPtrW(hwnd, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    auto state = reinterpret_cast<MotionFixture*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_PAINT: case WM_PRINTCLIENT: {
        ui::Paint paint(hwnd, message == WM_PRINTCLIENT ? reinterpret_cast<HDC>(wparam) : nullptr);
        Graphics g(paint.buffer); ui::Prepare(g, hwnd);
        ui::Text(g, L"流畅，也从容", {24, 20, 300, 36}, 25, ui::foreground, true);
        ui::Button(g, hwnd, {24, 72, 292, 44}, L"开始区域截图", true, state->hovered);
        ui::Toggle(g, hwnd, {24, 130, 292, 44}, L"动画效果", state->enabled, state->hovered);
        return 0;
    }
    case WM_APP + 100:
        state->hovered = wparam != 0; state->enabled = lparam != 0;
        InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
void AnimationUi(HINSTANCE instance, const filesystem::path& root) {
    WNDCLASSEXW cls{sizeof(cls)}; cls.hInstance = instance; cls.lpfnWndProc = MotionFixtureProc;
    cls.lpszClassName = L"ScreenShotPlus.Tests.Motion"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    Check(RegisterClassExW(&cls) != 0, "register animation fixture");
    const auto waitForIdle = [](HWND hwnd) {
        const auto deadline = GetTickCount64() + 2000;
        while (ui::GetMotionInfo(hwnd).running && GetTickCount64() < deadline) Pump(20);
    };
    DWORD settledResources = 0;
    for (int iteration = 0; iteration < 8; ++iteration) {
        MotionFixture fixture;
        HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls.lpszClassName, L"ScreenShotPlus 动画验证",
            WS_POPUP, 24, 24, 340, 196, nullptr, nullptr, instance, &fixture);
        Check(window != nullptr, "create animated window");
        struct WindowCleanup { HWND window; ~WindowCleanup() { if (IsWindow(window)) DestroyWindow(window); } } cleanup{window};
        ui::Corners(window);
        ui::Place(window, 340, 196);
        ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window); Pump(380);
        waitForIdle(window);
        auto info = ui::GetMotionInfo(window);
        Check(abs(info.refreshRate - ui::RefreshRate(window)) < 0.01, "scheduler uses actual window monitor refresh rate");
        Check(!info.running, "entrance stops at its final frame");
        if (iteration == 0) cout << "Animation monitor target: " << info.refreshRate << " Hz, entrance frame messages: " << info.frames
            << ", system animations: " << info.enabled << '\n';
        const auto idleFrames = info.frames;
        Pump(70); Check(ui::GetMotionInfo(window).frames == idleFrames, "idle window does not continuously repaint");
        SendMessageW(window, WM_APP + 100, 1, 1); UpdateWindow(window);
        if (info.enabled) {
            Check(ui::GetMotionInfo(window).running, "hover and toggle transition starts");
            Pump(35); Check(ui::GetMotionInfo(window).frames > idleFrames, "high resolution clock delivers animation frames");
        }
        Pointer(window, WM_LBUTTONDOWN, 120, 90, MK_LBUTTON); UpdateWindow(window); Pump(30);
        Pointer(window, WM_LBUTTONUP, 120, 90); UpdateWindow(window);
        Pump(280); Check(!ui::GetMotionInfo(window).running, "press release and toggle settle");
        SendMessageW(window, WM_APP + 100, 0, 0); UpdateWindow(window);
        ShowWindow(window, SW_HIDE);
        const auto hiddenFrames = ui::GetMotionInfo(window).frames;
        Pump(80); Check(!ui::GetMotionInfo(window).running && ui::GetMotionInfo(window).frames == hiddenFrames, "hidden window cancels scheduled frames");
        ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window); Pump(320);
        waitForIdle(window);
        SendMessageW(window, WM_DISPLAYCHANGE, 32, 0); SendMessageW(window, WM_SETTINGCHANGE, 0, 0); UpdateWindow(window);
        Check(!ui::GetMotionInfo(window).running, "display and settings updates do not leave an idle animation loop");
        if (iteration == 0) Snapshot(window, root / L"animation-ui");
        DestroyWindow(window); Pump(20);
        if (iteration == 0) settledResources = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        else {
            const DWORD resources = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            cout << "Animation GDI resources after window " << iteration + 1 << ": " << resources << " (baseline " << settledResources << ")\n";
            Check(resources <= settledResources + 2, "animated window and cached buffers release GDI handles");
        }
    }
    UnregisterClassW(cls.lpszClassName, instance);
}
void RichEffectsUi(HINSTANCE instance, const filesystem::path& root) {
    Bitmap image(320, 180, PixelFormat32bppRGB);
    { Graphics g(&image); g.Clear(Color(255, 37, 122, 183)); ui::Text(g, L"ScreenShotPlus", {32, 44, 280, 38}, 25, ui::foreground, true);
        ui::Text(g, L"截图 · 拖动 · 分享", {34, 98, 280, 30}, 16); }
    filesystem::path file; wstring error;
    Check(SavePng(image, root / L"effect-source", file, error), "save effects fixture source");
    Check(InitPreview(instance, [] {}), "initialize effects preview");
    const HWND foreground = GetForegroundWindow();
    const RECT origin{300, 200, 940, 560};
    ShowPreview(&image, file, {300, 300}, 0, origin);
    Pump(70);
    auto info = GetVisualEffectInfo();
    if (info.window) {
        Check(info.presented && info.error == 0, "layered flight surface presented with alpha");
        Snapshot(info.window, root / L"capture-flight-ui", false);
    }
    Pump(430); Check(!GetVisualEffectInfo().window, "capture flight ends and releases its window");
    Check(GetForegroundWindow() == foreground, "flight and preview do not activate");
    const RECT source = PreviewImageScreenRect();
    Check(source.right > source.left && source.bottom > source.top, "preview maps its image to physical screen pixels");
    const DWORD baseline = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int iteration = 0; iteration < 3; ++iteration) {
        Check(BeginDragVisual(image, source), "start floating drag card");
        Pump(260); info = GetVisualEffectInfo();
        Check(info.window && info.dragging && info.presented && !info.error, "drag card runs while dragging");
        const auto style = GetWindowLongPtrW(info.window, GWL_EXSTYLE);
        Check((style & (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)) == (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE), "drag card is layered, transparent to input and nonactivating");
        RECT rect{}; GetWindowRect(info.window, &rect);
        Check(WindowFromPoint({(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2}) != info.window,
            "floating card does not intercept underlying drop hit testing");
        const auto frames = ui::GetMotionInfo(info.window).frames;
        DragVisualFeedback(DROPEFFECT_COPY); Pump(150);
        Check(GetVisualEffectInfo().effect == DROPEFFECT_COPY && ui::GetMotionInfo(info.window).frames > frames, "copy feedback and pointer-follow frames update");
        if (iteration == 0) Snapshot(info.window, root / L"drag-copy-ui", false);
        if (iteration == 1) {
            EndDragVisual(true); Pump(230); Check(!GetVisualEffectInfo().window, "accepted drop contracts and closes");
        } else {
            DragVisualFeedback(DROPEFFECT_NONE); Pump(150);
            if (iteration == 0) Snapshot(info.window, root / L"drag-unavailable-ui", false);
            EndDragVisual(false);
            Check(GetVisualEffectInfo().returning, "canceled drop starts return transition");
            Pump(330); Check(!GetVisualEffectInfo().window, "canceled card returns and releases resources");
        }
        Check(filesystem::exists(file), "drag visual never removes source PNG");
        Check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= baseline + 2, "layered drag DIBs and DCs released");
    }
    Check(BeginDragVisual(image, source), "start effect before new capture");
    Check(BeginSelection(instance, [](unique_ptr<Bitmap>, POINT, wstring) {}, error), "new capture cancels old visual before desktop freeze");
    Check(!GetVisualEffectInfo().window, "floating card excluded from next screenshot"); CancelSelection();
    DestroyPreview();
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
    Check(c.animationLevel == 2 && c.animationSpeed == 1 && c.accentColor == 0 && c.previewCorner == 0 &&
        c.previewSize == 1 && c.captureShade == 60 && c.captureAnimation && c.dragAnimation &&
        c.startupAnimation && c.captureGuides, "new customization defaults preserve existing behavior");
    c.animationLevel = 4; c.animationSpeed = 2; c.accentColor = 3; c.previewCorner = 2;
    c.previewSize = 0; c.captureShade = 75;
    c.captureAnimation = false; c.dragAnimation = false; c.startupAnimation = false; c.captureGuides = false;
    wstring error;
    Check(SaveConfig(file, c, error), "save config");
    wstring warning;
    auto loaded = LoadConfig(file, warning);
    Check(warning.empty() && loaded.folder == c.folder && loaded.key == VK_F9 && loaded.previewSeconds == 15 && loaded.printScreen && loaded.startup, "Unicode config round trip");
    Check(loaded.animationLevel == 4 && loaded.animationSpeed == 2 && loaded.accentColor == 3 &&
        loaded.previewCorner == 2 && loaded.previewSize == 0 && loaded.captureShade == 75 &&
        !loaded.captureAnimation && !loaded.dragAnimation && !loaded.startupAnimation && !loaded.captureGuides,
        "animation, color, preview and capture preferences persist");
    c.previewSeconds = 0; Check(SaveConfig(file, c, error), "replace existing config atomically");
    Check(LoadConfig(file, warning).previewSeconds == 0, "updated config persisted");
    nlohmann::json data;
    { ifstream in(file); in >> data; }
    auto legacy = data;
    for (const auto* key : {"startup", "animationLevel", "animationSpeed", "accentColor", "previewCorner", "previewSize",
        "captureShade", "captureAnimation", "dragAnimation", "startupAnimation", "captureGuides"}) legacy.erase(key);
    { ofstream out(file); out << legacy; }
    warning.clear(); loaded = LoadConfig(file, warning);
    Check(!loaded.startup && warning.empty() && loaded.animationLevel == 2 && loaded.animationSpeed == 1 &&
        loaded.accentColor == 0 && loaded.previewCorner == 0 && loaded.previewSize == 1 && loaded.captureShade == 60 &&
        loaded.captureAnimation && loaded.dragAnimation && loaded.startupAnimation && loaded.captureGuides,
        "legacy config loads safe customization defaults without migration");
    for (unsigned level = 0; level < 5; ++level) {
        c.animationLevel = level;
        c.animationSpeed = level % 3; c.accentColor = level;
        c.previewCorner = level % 4; c.previewSize = level % 3; c.captureShade = level == 0 ? 25 : 80;
        Check(SaveConfig(file, c, error), "save each animation tier including range endpoints");
        warning.clear(); loaded = LoadConfig(file, warning);
        Check(warning.empty() && loaded.animationLevel == c.animationLevel && loaded.animationSpeed == c.animationSpeed &&
            loaded.accentColor == c.accentColor && loaded.previewCorner == c.previewCorner && loaded.previewSize == c.previewSize &&
            loaded.captureShade == c.captureShade, "all animation tiers and customization endpoints round trip");
    }
    const array<pair<const char*, int>, 6> limits{{{"animationLevel", 4}, {"animationSpeed", 2}, {"accentColor", 4},
        {"previewCorner", 3}, {"previewSize", 2}, {"captureShade", 80}}};
    for (const auto& [key, maximum] : limits) {
        for (const auto& bad : {nlohmann::json(-1), nlohmann::json(maximum + 1), nlohmann::json(1.5),
            nlohmann::json("bad"), nlohmann::json(true), nlohmann::json(18446744073709551615ULL)}) {
            auto invalid = data; invalid[key] = bad;
            { ofstream out(file); out << invalid; }
            warning.clear(); loaded = LoadConfig(file, warning);
            Check(!warning.empty() && loaded.animationLevel == 2 && loaded.key == 'S',
                "malformed customization values cannot wrap or partially replace configuration");
        }
    }
    { auto invalid = data; invalid["captureShade"] = 24; ofstream out(file); out << invalid; }
    warning.clear(); loaded = LoadConfig(file, warning);
    Check(!warning.empty() && loaded.captureShade == 60, "capture shade lower bound validated");
    for (const auto* key : {"captureAnimation", "dragAnimation", "startupAnimation", "captureGuides"}) {
        auto invalid = data; invalid[key] = 1;
        { ofstream out(file); out << invalid; }
        warning.clear(); loaded = LoadConfig(file, warning);
        Check(!warning.empty() && loaded.captureAnimation && loaded.dragAnimation && loaded.startupAnimation && loaded.captureGuides,
            "animation switches require boolean configuration values");
    }
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
void ConfigurationFallback(const filesystem::path& root, const filesystem::path& executable) {
    Config config = DefaultConfig(); config.folder = root / L"fallback-images"; config.previewSeconds = 61;
    const auto primary = root / L"blocked" / L"config.json", fallback = root / L"portable" / L"config.json";
    filesystem::path destination; wstring error, warning;
    Check(SaveConfigWithFallback(primary, fallback, config, destination, error) && destination == fallback && error.empty(),
        "unwritable preferred config falls back to portable config");
    Check(LoadConfig(destination, warning).previewSeconds == 61 && warning.empty(), "fallback persists chosen settings");
    destination = root / L"unchanged.json";
    Check(!SaveConfigWithFallback(primary, root / L"blocked" / L"fallback.json", config, destination, error) && destination.filename() == L"unchanged.json",
        "both failed destinations preserve active config path");
    Check(error.find(primary.wstring()) != wstring::npos && error.find(L"错误") != wstring::npos, "config failure names actual path and error");
    const auto ordinary = root / L"preferred" / L"config.json";
    Check(SaveConfigWithFallback(ordinary, fallback, config, destination, error) && destination == ordinary, "writable preferred config remains preferred");

    const auto folder = root / L"portable-restart";
    filesystem::create_directories(folder);
    const auto childFile = folder / L"ConfigProbe.exe";
    filesystem::copy_file(executable, childFile);
    Check(SaveConfig(folder / L"ScreenShotPlus-data" / L"config.json", config, error), "restart portable config fixture");
    wstring command = L"\"" + childFile.wstring() + L"\" --config-location-probe";
    STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION child{};
    Check(CreateProcessW(childFile.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &child), "launch portable config restart child");
    const DWORD waited = WaitForSingleObject(child.hProcess, 5000);
    if (waited != WAIT_OBJECT_0) TerminateProcess(child.hProcess, 95);
    DWORD code = 0; GetExitCodeProcess(child.hProcess, &code); CloseHandle(child.hThread); CloseHandle(child.hProcess);
    Check(waited == WAIT_OBJECT_0 && code == 74, "next process reads portable config next to its executable");

    // Probe a fresh, owned sibling directory; never touch the user's real config.
    const auto local = ui::KnownFolder(FOLDERID_LocalAppData);
    if (!local.empty()) {
        const auto probe = local / (L"ScreenShotPlus-Probe-" + to_wstring(GetCurrentProcessId()));
        error_code ec;
        const bool created = filesystem::create_directory(probe, ec);
        if (ec) {
            Check(SaveConfigWithFallback(probe / L"config.json", root / L"native-fallback" / L"config.json", config, destination, error) &&
                destination == root / L"native-fallback" / L"config.json", "real LocalAppData denial still permits portable save");
            cout << "PASS real LocalAppData create denial (error " << ec.value() << ") / portable save\n";
        } else if (created) {
            const bool saved = SaveConfig(probe / L"config.json", config, error);
            if (probe.parent_path() == local && probe.filename() == L"ScreenShotPlus-Probe-" + to_wstring(GetCurrentProcessId())) filesystem::remove_all(probe, ec);
            Check(saved && !ec, "owned LocalAppData probe saved and removed");
            cout << "PASS real LocalAppData write access\n";
        }
    }
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
void SettingsFailureUi(HINSTANCE instance, const filesystem::path& root) {
    const auto file = root / L"blocked" / L"config.json";
    Check(InitSettings(instance, nullptr, [&](const Config& config, wstring& error) { return SaveConfig(file, config, error); }), "settings failure fixture");
    SetSettingsConfigFile(file);
    Config config = DefaultConfig(); config.folder = root / L"settings-error-images";
    ShowSettings(config);
    HWND hwnd = OwnWindow(L"ScreenShotPlus.Settings"); Click(hwnd, 100, 768);
    Check(SettingsVisible(), "save failure keeps settings available");
    Snapshot(hwnd, root / L"settings-error-ui"); DestroySettings();
}
void SettingsMotionUi(HINSTANCE instance, const filesystem::path& root) {
    struct RestoreOptions {
        ~RestoreOptions() { DestroySettings(); ui::SetMotionOptions(2, 1); ui::SetAccent(0); }
    } restore;
    const auto file = root / L"settings-customization" / L"config.json";
    Config initial = DefaultConfig(); initial.folder = root / L"settings-customization-images";
    wstring error, warning; Check(SaveConfig(file, initial, error), "persist settings fixture baseline");
    unsigned applied = 0;
    Check(InitSettings(instance, nullptr, [&](const Config& config, wstring& failure) {
        const bool saved = SaveConfig(file, config, failure); if (saved) ++applied; return saved;
    }), "initialize settings motion fixture");
    SetSettingsConfigFile(file); ShowSettings(initial);
    HWND hwnd = OwnWindow(L"ScreenShotPlus.Settings"); Check(hwnd && SettingsVisible(), "settings fixture visible");
    UpdateWindow(hwnd); Pump(350);
    Click(hwnd, 190, 92); UpdateWindow(hwnd); Pump(260);
    Check(GetSettingsState().tab == 1 && GetSettingsState().title == L"高", "motion tab begins at persisted high tier");
    Click(hwnd, 62, 289); UpdateWindow(hwnd);
    Check(GetSettingsState().working.animationLevel == 0 && ui::MotionLevel() == 0 && !ui::GetMotionInfo(hwnd).enabled,
        "light tier applies immediately to ordinary motion");
    const auto frames = ui::GetMotionInfo(hwnd).frames;
    const auto lightSampleStart = GetTickCount64();
    Pump(35);
    auto state = GetSettingsState();
    cout << "Light selector sample: elapsed " << GetTickCount64() - lightSampleStart << " ms, frames " << frames << " -> "
        << ui::GetMotionInfo(hwnd).frames << ", position " << state.sliderPosition << ", running " << ui::GetMotionInfo(hwnd).running << '\n';
    Check(ui::GetMotionInfo(hwnd).frames > frames && state.sliderPosition > 0 && state.sliderPosition < 2,
        "selector thumb still animates smoothly while light tier disables other animation");
    Pump(400); state = GetSettingsState();
    Check(abs(state.sliderPosition) < 0.01f && state.title == L"轻度" && !ui::GetMotionInfo(hwnd).running,
        "light selector settles without continuous idle repaint");
    constexpr array<int, 5> stops{62, 156, 250, 344, 438};
    constexpr array<const wchar_t*, 5> names{L"轻度", L"中", L"高", L"极高", L"终极"};
    for (unsigned level = 1; level < 4; ++level) {
        Click(hwnd, stops[level], 289); UpdateWindow(hwnd); Pump(430);
        state = GetSettingsState();
        Check(state.working.animationLevel == level && state.title == names[level] &&
            abs(state.sliderPosition - level) < 0.01f, "accent tiers land on all five discrete selector stops");
    }
    Snapshot(hwnd, root / L"settings-extreme-ui");
    Click(hwnd, 438, 289); UpdateWindow(hwnd);
    Check(GetSettingsState().working.animationLevel == 4 && GetSettingsState().title == L"XuanRikka",
        "ultimate begins with the short XuanRikka signature");
    Pump(205); Check(GetSettingsState().title == L"XuanRikka", "ultimate signature remains briefly visible");
    Snapshot(hwnd, root / L"settings-xuanrikka-ui", false);
    Pump(430); Check(GetSettingsState().title == L"终极", "ultimate label transitions from signature to final tier");
    const auto starFrames = ui::GetMotionInfo(hwnd).frames; Pump(80);
    Check(ui::GetMotionInfo(hwnd).running && ui::GetMotionInfo(hwnd).frames > starFrames,
        "ultimate gradient and flowing stars remain animated while visible");
    Snapshot(hwnd, root / L"settings-ultimate-ui", false);
    Pump(300);
    const auto steadyStart = GetTickCount64(), steadyFrames = ui::GetMotionInfo(hwnd).frames;
    Pump(500);
    cout << "Ultimate selector steady target: " << ui::GetMotionInfo(hwnd).refreshRate << " Hz, frame messages: "
        << ui::GetMotionInfo(hwnd).frames - steadyFrames << " in " << GetTickCount64() - steadyStart << " ms (scheduler sample, not presented FPS)\n";
    Pointer(hwnd, WM_LBUTTONDOWN, 62, 289, MK_LBUTTON);
    Check(GetSettingsState().sliderDragging && GetCapture() == hwnd, "selector captures the pointer while scrubbing");
    Pointer(hwnd, WM_MOUSEMOVE, 438, 289, MK_LBUTTON);
    Check(GetSettingsState().working.animationLevel == 4 && GetSettingsState().title == L"XuanRikka", "drag reaches ultimate tier");
    Pointer(hwnd, WM_MOUSEMOVE, 156, 289, MK_LBUTTON); Pointer(hwnd, WM_LBUTTONUP, 156, 289);
    Pump(500);
    Check(!GetSettingsState().sliderDragging && GetSettingsState().working.animationLevel == 1 && GetSettingsState().title == L"中",
        "fast scrub cancels the pending ultimate label and releases capture");
    Click(hwnd, 250, 289); SendMessageW(hwnd, WM_KEYDOWN, VK_HOME, 0); SendMessageW(hwnd, WM_KEYDOWN, VK_LEFT, 0);
    Check(GetSettingsState().working.animationLevel == 0, "keyboard selection clamps at light tier");
    SendMessageW(hwnd, WM_KEYDOWN, VK_END, 0); SendMessageW(hwnd, WM_KEYDOWN, VK_RIGHT, 0);
    Check(GetSettingsState().working.animationLevel == 4 && GetSettingsState().title == L"XuanRikka", "keyboard End reaches ultimate");
    SendMessageW(hwnd, WM_KEYDOWN, VK_LEFT, 0); Pump(600);
    Check(GetSettingsState().working.animationLevel == 3 && GetSettingsState().title == L"极高", "keyboard reversal cancels signature transition");
    Pointer(hwnd, WM_LBUTTONDOWN, 250, 289, MK_LBUTTON); ReleaseCapture();
    Check(!GetSettingsState().sliderDragging && GetSettingsState().working.animationLevel == 2, "lost pointer capture cancels scrub state cleanly");
    Click(hwnd, 438, 289); Click(hwnd, 306, 92); Click(hwnd, 340, 211);
    Check(ui::MotionLevel() == 4 && ui::accent.GetValue() == ui::AccentColor(3).GetValue(), "settings provides live motion and accent preview");
    Pump(700); Check(!ui::GetMotionInfo(hwnd).running, "leaving ultimate motion page stops its star loop");
    Click(hwnd, 350, 768);
    Check(!SettingsVisible() && ui::MotionLevel() == initial.animationLevel && ui::accent.GetValue() == ui::AccentColor(initial.accentColor).GetValue(),
        "cancel rolls back live motion and accent preview");
    warning.clear(); Check(applied == 0 && LoadConfig(file, warning).animationLevel == 2 && warning.empty(), "cancel preserves persisted configuration");
    const auto hiddenFrames = ui::GetMotionInfo(hwnd).frames; Pump(90);
    Check(!ui::GetMotionInfo(hwnd).running && ui::GetMotionInfo(hwnd).frames == hiddenFrames, "hidden ultimate settings has no scheduled frames");
    ShowSettings(initial); Click(hwnd, 190, 92); Click(hwnd, 438, 289);
    Click(hwnd, 96, 448); Click(hwnd, 250, 508); Click(hwnd, 250, 566); Click(hwnd, 250, 624);
    Click(hwnd, 306, 92); Click(hwnd, 430, 211); Click(hwnd, 365, 536); Click(hwnd, 403, 627);
    Click(hwnd, 412, 92); Pointer(hwnd, WM_LBUTTONDOWN, 62, 243, MK_LBUTTON);
    Pointer(hwnd, WM_MOUSEMOVE, 438, 243, MK_LBUTTON); Pointer(hwnd, WM_LBUTTONUP, 438, 243);
    SendMessageW(hwnd, WM_KEYDOWN, VK_HOME, 0); Check(GetSettingsState().working.captureShade == 25, "shade keyboard Home reaches lightest background");
    SendMessageW(hwnd, WM_KEYDOWN, VK_END, 0); Click(hwnd, 250, 362);
    Snapshot(hwnd, root / L"settings-capture-ui"); Click(hwnd, 100, 768);
    warning.clear(); Config saved = LoadConfig(file, warning);
    Check(applied == 1 && !SettingsVisible() && warning.empty() && saved.animationLevel == 4 && saved.animationSpeed == 0 &&
        saved.accentColor == 4 && saved.previewCorner == 3 && saved.previewSize == 2 && saved.captureShade == 80 &&
        !saved.startupAnimation && !saved.captureAnimation && !saved.dragAnimation && !saved.captureGuides,
        "native settings saves all custom controls and independent animation switches");
    Check(ui::MotionLevel() == 4 && ui::accent.GetValue() == ui::AccentColor(4).GetValue(), "save keeps chosen live style as rollback baseline");
    ShowSettings(saved); Click(hwnd, 190, 92);
    Check(GetSettingsState().title == L"终极", "reopening persisted ultimate skips an artificial signature replay");
    Click(hwnd, 62, 289); Click(hwnd, 306, 92); Click(hwnd, 70, 211); SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
    Check(!SettingsVisible() && ui::MotionLevel() == 4 && ui::accent.GetValue() == ui::AccentColor(4).GetValue(),
        "Escape restores most recently saved preview baseline");
    DestroySettings(); Pump(40); const DWORD resources = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        Check(InitSettings(instance, nullptr, [](const Config&, wstring&) { return true; }), "recreate settings animation fixture");
        ShowSettings(saved); HWND next = OwnWindow(L"ScreenShotPlus.Settings"); Click(next, 190, 92); UpdateWindow(next); Pump(80);
        Check(ui::GetMotionInfo(next).running, "recreated ultimate selector animates");
        SendMessageW(next, WM_CLOSE, 0, 0); const auto stoppedFrames = ui::GetMotionInfo(next).frames; Pump(60);
        Check(!ui::GetMotionInfo(next).running && ui::GetMotionInfo(next).frames == stoppedFrames, "closing settings cancels ultimate frame loop");
        DestroySettings(); Pump(20);
        Check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= resources + 2, "settings selector releases cached buffers and GDI resources");
    }
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
        {
            Graphics g(dc); g.Clear(Color(255, 29, 137, 211));
            RECT client{}; GetClientRect(hwnd, &client);
            if (client.right > 700) {
                ui::Text(g, L"ScreenShotPlus", {static_cast<float>(client.right / 2 - 260),
                    static_cast<float>(client.bottom / 2 - 44), 540, 70}, 44, ui::foreground, true);
                ui::Text(g, L"隔离截图场景 · 原始像素保持不变", {static_cast<float>(client.right / 2 - 240),
                    static_cast<float>(client.bottom / 2 + 38), 530, 40}, 24, ui::foreground);
            }
        }
        EndPaint(hwnd, &ps); return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}
void CaptureAndPreview(HINSTANCE instance, const filesystem::path& root) {
    WNDCLASSEXW cls{sizeof(cls)}, existing{sizeof(existing)};
    cls.lpfnWndProc = PatternProc; cls.hInstance = instance; cls.lpszClassName = L"ScreenShotPlus.TestPattern";
    if (!GetClassInfoExW(instance, cls.lpszClassName, &existing)) Check(RegisterClassExW(&cls) != 0, "test pattern class");
    HWND pattern = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, cls.lpszClassName, L"Screenshot test pattern", WS_POPUP,
        GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN), nullptr, nullptr, instance, nullptr);
    Check(pattern != nullptr, "test pattern window");
    struct PatternCleanup { HWND hwnd; ~PatternCleanup() { if (IsWindow(hwnd)) DestroyWindow(hwnd); } } cleanup{pattern};
    ShowWindow(pattern, SW_SHOWNOACTIVATE); UpdateWindow(pattern); DwmFlush();
    unique_ptr<Bitmap> result;
    POINT finished{}; wstring error; bool called = false;
    SelectionGuard selectionGuard;
    Check(BeginSelection(instance, [&](unique_ptr<Bitmap> image, POINT point, wstring failure) {
        result = move(image); finished = point; error = move(failure); called = true;
    }, error), "desktop freeze");
    HWND overlay = FindWindowW(L"ScreenShotPlus.Selection", nullptr);
    Check(overlay != nullptr, "selection overlay");
    Snapshot(overlay, root / L"capture-ready-ui");
    cout << "Capture entry target: " << ui::GetMotionInfo(overlay).refreshRate << " Hz, frame messages: " << ui::GetMotionInfo(overlay).frames << '\n';
    const int x = 150 - GetSystemMetrics(SM_XVIRTUALSCREEN), y = 150 - GetSystemMetrics(SM_YVIRTUALSCREEN);
    SendMessageW(overlay, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x + 80, y + 60));
    SendMessageW(overlay, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, y));
    Snapshot(overlay, root / L"capture-selection-ui");
    SendMessageW(overlay, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    Check(called && result && error.empty() && result->GetWidth() == 80 && result->GetHeight() == 60, "live reverse selection crop");
    Color pixel; result->GetPixel(20, 20, &pixel);
    Check(pixel.GetR() == 29 && pixel.GetG() == 137 && pixel.GetB() == 211 && pixel.GetA() == 255, "frozen desktop pixel / alpha");
    Check(finished.x == 150 && finished.y == 150, "selection end physical point");
    const RECT captured = LastCaptureRect();
    Check(captured.left == 150 && captured.top == 150 && captured.right == 230 && captured.bottom == 210, "reverse selection origin for flight uses physical pixels");
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
void CustomizationUi(HINSTANCE instance, const filesystem::path& root) {
    struct RestoreOptions {
        ~RestoreOptions() { CancelSelection(); DestroyPreview(); ConfigureCapture(60, true, true); ConfigurePreview(0, 1, true); ui::SetMotionOptions(2, 1); }
    } restore;
    WNDCLASSEXW cls{sizeof(cls)}, existing{sizeof(existing)};
    cls.lpfnWndProc = PatternProc; cls.hInstance = instance; cls.lpszClassName = L"ScreenShotPlus.TestPattern";
    if (!GetClassInfoExW(instance, cls.lpszClassName, &existing)) Check(RegisterClassExW(&cls) != 0, "customization pattern class");
    HWND pattern = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, cls.lpszClassName,
        L"Isolated customization fixture", WS_POPUP, GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN), nullptr, nullptr, instance, nullptr);
    Check(pattern != nullptr, "customization pattern window");
    struct PatternCleanup { HWND hwnd; ~PatternCleanup() { if (IsWindow(hwnd)) DestroyWindow(hwnd); } } cleanup{pattern};
    ShowWindow(pattern, SW_SHOWNOACTIVATE); UpdateWindow(pattern); DwmFlush();
    COLORREF bright = 0;
    unique_ptr<Bitmap> image; wstring error;
    // Capture callbacks refer to these locals. Cancel while they are still
    // alive on both normal exit and exception unwinding.
    SelectionGuard selectionGuard;
    DWORD baseline = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (const unsigned shade : {25u, 80u}) {
        ConfigureCapture(shade, false, false);
        Check(BeginSelection(instance, [&](unique_ptr<Bitmap> captured, POINT, wstring failure) {
            image = move(captured); error = move(failure);
        }, error), "capture customized shade with entrance disabled");
        HWND overlay = OwnWindow(L"ScreenShotPlus.Selection");
        Check(overlay != nullptr, "customized selection window available");
        UpdateWindow(overlay); Pump(70);
        Check(!ui::GetMotionInfo(overlay).running, "disabled capture entrance leaves no idle animation loop");
        const COLORREF pixel = PrintedPixel(overlay, 40, 40);
        if (shade == 25) bright = pixel;
        else Check(GetBValue(pixel) < GetBValue(bright) && GetGValue(pixel) < GetGValue(bright),
            "capture shade changes frozen background brightness");
        if (shade == 80) Snapshot(overlay, root / L"capture-custom-shade-ui", false);
        const int x = 150 - GetSystemMetrics(SM_XVIRTUALSCREEN), y = 150 - GetSystemMetrics(SM_YVIRTUALSCREEN);
        SendMessageW(overlay, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        SendMessageW(overlay, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x + 80, y + 60));
        SendMessageW(overlay, WM_LBUTTONUP, 0, MAKELPARAM(x + 80, y + 60));
        Check(image && error.empty() && image->GetWidth() == 80 && image->GetHeight() == 60,
            "customized capture returns original physical dimensions");
        Color original; image->GetPixel(20, 20, &original);
        Check(original.GetR() == 29 && original.GetG() == 137 && original.GetB() == 211 && original.GetA() == 255,
            "shade and guide settings never alter original capture pixels");
        Check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= baseline + 2, "custom capture cached surfaces released");
    }
    cerr << "Native dirty fixture: begin opaque selection\n";
    ConfigureCapture(60, false, false);
    Check(BeginSelection(instance, [&](unique_ptr<Bitmap> captured, POINT, wstring failure) {
        image = move(captured); error = move(failure);
    }, error), "native dirty-region capture fixture");
    HWND nativeOverlay = OwnWindow(L"ScreenShotPlus.Selection");
    Check(nativeOverlay && !(GetWindowLongPtrW(nativeOverlay, GWL_EXSTYLE) & WS_EX_LAYERED), "native dirty-region fixture is opaque");
    RECT client{}; GetClientRect(nativeOverlay, &client);
    const float scale = ui::Scale(nativeOverlay);
    const int right = client.right - static_cast<int>(12 * scale), bottom = client.bottom - static_cast<int>(12 * scale),
        top = bottom - static_cast<int>(80 * scale);
    SendMessageW(nativeOverlay, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(right - 2, top));
    SendMessageW(nativeOverlay, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(right, bottom));
    // This fixture explicitly disables animation. Paint synchronously so a
    // queued physical WM_MOUSEMOVE cannot overwrite the synthetic endpoint.
    UpdateWindow(nativeOverlay); DwmFlush();
    const LONG labelX = static_cast<LONG>(floor(client.right - 252 * scale)),
        labelY = static_cast<LONG>(floor(top - 42 * scale - 10 * scale));
    const RECT band{labelX + static_cast<LONG>(8 * scale), labelY + static_cast<LONG>(8 * scale),
        labelX + static_cast<LONG>(162 * scale), labelY + static_cast<LONG>(34 * scale)};
    cerr << "Native dirty fixture: painted 2px ROI, band " << band.left << ',' << band.top << ',' << band.right << ',' << band.bottom << '\n';
    POINT bandCenter{(band.left + band.right) / 2, (band.top + band.bottom) / 2}; ClientToScreen(nativeOverlay, &bandCenter);
    RECT nativeWindow{}; GetWindowRect(nativeOverlay, &nativeWindow);
    const HWND hitWindow = WindowFromPoint(bandCenter);
    wchar_t hitClass[128]{}; GetClassNameW(hitWindow, hitClass, 128);
    cerr << "Native window visible=" << IsWindowVisible(nativeOverlay) << ", foreground=" << (GetForegroundWindow() == nativeOverlay)
        << ", point hit overlay=" << (hitWindow == nativeOverlay) << ", style=" << GetWindowLongPtrW(nativeOverlay, GWL_EXSTYLE)
        << ", window rect " << nativeWindow.left << ',' << nativeWindow.top << ',' << nativeWindow.right << ',' << nativeWindow.bottom << '\n';
    wcerr << L"Native point hit class: " << hitClass << L'\n';
    auto before = NativePixels(nativeOverlay, band);
    Color background; before->GetPixel(1, 1, &background);
    cerr << "Native label background RGB " << static_cast<unsigned>(background.GetR()) << ',' << static_cast<unsigned>(background.GetG()) << ','
        << static_cast<unsigned>(background.GetB()) << "; expected " << static_cast<unsigned>(ui::background.GetR()) << ','
        << static_cast<unsigned>(ui::background.GetG()) << ',' << static_cast<unsigned>(ui::background.GetB()) << '\n';
    Check(abs(static_cast<int>(background.GetR()) - ui::background.GetR()) <= 2 &&
        abs(static_cast<int>(background.GetG()) - ui::background.GetG()) <= 2 &&
        abs(static_cast<int>(background.GetB()) - ui::background.GetB()) <= 2,
        "native narrow right-bottom selection paints its clamped dimensions label");
    const auto beforeHash = PixelHash(*before);
    filesystem::path nativeFile;
    Check(SavePng(*before, root / L"capture-native-label-before", nativeFile, error), "save native dirty label evidence");
    SendMessageW(nativeOverlay, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(right + 1, bottom));
    UpdateWindow(nativeOverlay); DwmFlush();
    auto after = NativePixels(nativeOverlay, band);
    cerr << "Native dirty fixture: painted 3px ROI, before hash " << beforeHash << ", after hash " << PixelHash(*after) << '\n';
    Check(PixelHash(*after) != beforeHash,
        "native dimensions update when narrow selection changes inside the same clamped dirty rectangle");
    Check(SavePng(*after, root / L"capture-native-label-after", nativeFile, error), "save native changed label evidence");
    SendMessageW(nativeOverlay, WM_LBUTTONUP, 0, MAKELPARAM(right + 1, bottom));
    Check(image && image->GetWidth() == 3 && image->GetHeight() == static_cast<UINT>(bottom - top),
        "narrow edge selection still crops exact physical dimensions");
    Check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= baseline + 2, "native dirty-region fixture releases capture surfaces");
    DestroyWindow(pattern);
    Bitmap previewImage(320, 180, PixelFormat32bppRGB);
    { Graphics g(&previewImage); g.Clear(Color(255, 29, 137, 211)); }
    filesystem::path file; Check(SavePng(previewImage, root / L"custom-preview-source", file, error), "save customized preview fixture");
    Check(InitPreview(instance, [] {}), "customized preview initialization");
    const HWND foreground = GetForegroundWindow();
    POINT cursor{}; GetCursorPos(&cursor);
    MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
    const POINT anchor{(monitor.rcWork.left + monitor.rcWork.right) / 2, (monitor.rcWork.top + monitor.rcWork.bottom) / 2};
    const RECT origin{anchor.x - 120, anchor.y - 80, anchor.x + 120, anchor.y + 80};
    for (unsigned corner = 0; corner < 4; ++corner) {
        ConfigurePreview(corner, 1, false);
        ShowPreview(&previewImage, file, anchor, 0, origin); UpdateWindow(OwnWindow(L"ScreenShotPlus.Preview"));
        Check(!GetVisualEffectInfo().window, "capture animation switch suppresses screenshot flight");
        RECT rect{}; GetWindowRect(OwnWindow(L"ScreenShotPlus.Preview"), &rect);
        const int centerX = (rect.left + rect.right) / 2, centerY = (rect.top + rect.bottom) / 2;
        Check((corner == 1 || corner == 3) ? centerX < anchor.x : centerX > anchor.x,
            "preview honors selected horizontal monitor corner");
        Check(corner >= 2 ? centerY < anchor.y : centerY > anchor.y, "preview honors selected vertical monitor corner");
        HidePreview();
    }
    int previousWidth = 0, previousHeight = 0;
    for (unsigned size = 0; size < 3; ++size) {
        ConfigurePreview(0, size, false); ShowPreview(&previewImage, file, anchor);
        HWND hwnd = OwnWindow(L"ScreenShotPlus.Preview"); UpdateWindow(hwnd);
        RECT rect{}; GetWindowRect(hwnd, &rect);
        Check(rect.right - rect.left > previousWidth && rect.bottom - rect.top > previousHeight,
            "three preview sizes change physical window dimensions");
        previousWidth = rect.right - rect.left; previousHeight = rect.bottom - rect.top;
        const RECT fitted = PreviewImageScreenRect();
        Check(fitted.left >= rect.left && fitted.right <= rect.right && fitted.top >= rect.top && fitted.bottom <= rect.bottom &&
            abs(static_cast<float>(fitted.right - fitted.left) / (fitted.bottom - fitted.top) - 320.0f / 180) < 0.02f,
            "custom preview retains image aspect ratio and DPI-scaled physical bounds");
        if (size == 2) Snapshot(hwnd, root / L"preview-custom-size-ui");
        HidePreview();
    }
    Check(GetForegroundWindow() == foreground && filesystem::exists(file), "custom preview preserves focus and saved image");
    ui::SetMotionOptions(1, 1); ConfigurePreview(0, 1, true); ShowPreview(&previewImage, file, anchor, 0, origin);
    Check(!GetVisualEffectInfo().window, "medium tier keeps screenshot flight restrained"); HidePreview();
    ui::SetMotionOptions(0, 1);
    Check(!BeginDragVisual(previewImage, origin) && !GetVisualEffectInfo().window, "light tier suppresses decorative floating drag card");
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
    next.key = VK_F21;
    filesystem::path portableFile;
    Check(ApplyConfiguredSettings(window, state, current, next, root / L"blocked" / L"config.json", error,
        startup.key.c_str(), root / L"hotkey-portable" / L"config.json", &portableFile), "config fallback commits new hotkey only after persistence");
    Check(current.key == VK_F21 && LoadConfig(portableFile, warning).key == VK_F21 && ProbeHotkey(current.modifiers, VK_F22),
        "fallback persistence and hotkey state agree");
    ReleaseHotkeys(window, state); DestroyWindow(window);
    Check(ProbeHotkey(current.modifiers, VK_F21), "shutdown releases hotkey");
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
            Click(settings, 100, 472); Click(settings, 100, 768);
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
void ExecutableSmoke(const filesystem::path& executable) {
    HANDLE running = OpenMutexW(SYNCHRONIZE, FALSE, INSTANCE_MUTEX);
    if (running) { CloseHandle(running); cout << "SKIP standalone EXE smoke: an existing user instance owns the mutex\n"; return; }
    STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION first{};
    wstring command = L"\"" + executable.wstring() + L"\" --tray";
    Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &first), "launch actual release EXE");
    struct Cleanup {
        PROCESS_INFORMATION process;
        ~Cleanup() {
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) PostThreadMessageW(process.dwThreadId, WM_QUIT, 0, 0);
            if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 95);
            CloseHandle(process.hThread); CloseHandle(process.hProcess);
        }
    } cleanup{first};
    HWND panel = nullptr;
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (!panel && GetTickCount64() < deadline) {
        struct Search { DWORD pid; HWND window = nullptr; } search{first.dwProcessId};
        EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
            auto& search = *reinterpret_cast<Search*>(param);
            DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
            if (pid == search.pid && GetPropW(hwnd, INSTANCE_PANEL_MARKER) && GetPropW(hwnd, L"ScreenShotPlus.Ready")) {
                search.window = hwnd; return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));
        panel = search.window;
        if (!panel) Sleep(25);
    }
    Check(panel != nullptr && WaitForSingleObject(first.hProcess, 0) == WAIT_TIMEOUT, "actual EXE initializes and remains alive");
    const bool tray = GetPropW(panel, L"ScreenShotPlus.TrayRegistered") != nullptr;
    if (tray) {
        NOTIFYICONIDENTIFIER id{sizeof(id)}; id.hWnd = panel; id.uID = 1; RECT rect{};
        Check(SUCCEEDED(Shell_NotifyIconGetRect(&id, &rect)) && rect.right > rect.left, "actual EXE tray has Shell rectangle");
        cout << "PASS actual EXE notification icon registration / rectangle\n";
    } else {
        Check(IsWindowVisible(panel), "actual EXE stays accessible when Shell rejects icon");
        cout << "SKIP actual EXE tray acceptance: Shell rejected registration; accessible failure panel confirmed\n";
    }
    for (const bool quiet : {true, false}) {
        command = L"\"" + executable.wstring() + L"\"" + (quiet ? L" --tray" : L"");
        PROCESS_INFORMATION duplicate{};
        Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &duplicate), "launch actual EXE duplicate");
        const DWORD waited = WaitForSingleObject(duplicate.hProcess, 5000);
        if (waited != WAIT_OBJECT_0) TerminateProcess(duplicate.hProcess, 95);
        DWORD code = 0; GetExitCodeProcess(duplicate.hProcess, &code);
        CloseHandle(duplicate.hThread); CloseHandle(duplicate.hProcess);
        Check(waited == WAIT_OBJECT_0 && code == 0 && WaitForSingleObject(first.hProcess, 0) == WAIT_TIMEOUT,
            "actual duplicate EXE exits and preserves primary");
        if (quiet && tray) Check(!IsWindowVisible(panel), "duplicate tray launch does not show panel");
        if (!quiet) {
            // The duplicate exits after posting to the primary process. Its
            // exit does not guarantee that the primary has dispatched it yet.
            const ULONGLONG activationDeadline = GetTickCount64() + 2000;
            while (!IsWindowVisible(panel) && GetTickCount64() < activationDeadline) Pump(20);
            Check(IsWindowVisible(panel), "duplicate manual launch activates primary panel");
        }
    }
    PostThreadMessageW(first.dwThreadId, WM_QUIT, 0, 0);
    Check(WaitForSingleObject(first.hProcess, 5000) == WAIT_OBJECT_0, "actual EXE shuts down gracefully");
    cout << "PASS actual EXE startup / duplicate manual and tray launch / graceful shutdown\n";
}
void StartupUi(const filesystem::path& executable, const filesystem::path& root) {
    ui::SetMotionOptions(4, 1); const bool animationAvailable = ui::DecorativeMotion(4); ui::SetMotionOptions(2, 1);
    for (unsigned variant = 0; variant < 4; ++variant) {
        const bool enabled = variant != 1, tray = variant == 2;
        Config config = DefaultConfig(); config.folder = root / L"startup-fixture-images";
        config.modifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT; config.key = VK_F19;
        config.animationLevel = variant == 3 ? 2 : 4; config.startupAnimation = enabled;
        const auto file = root / (L"startup-" + to_wstring(variant)) / L"config.json";
        wstring error; Check(SaveConfig(file, config, error), "isolated persisted startup animation preference");
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION child{};
        wstring command = L"\"" + executable.wstring() + L"\" --startup-fixture \"" + file.wstring() + L"\"" + (tray ? L" --tray" : L"");
        Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &child),
            "launch native application startup fixture with isolated config");
        struct ChildCleanup {
            PROCESS_INFORMATION process;
            ~ChildCleanup() {
                if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) PostThreadMessageW(process.dwThreadId, WM_QUIT, 0, 0);
                if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 95);
                CloseHandle(process.hThread); CloseHandle(process.hProcess);
            }
        } cleanup{child};
        HWND panel = nullptr;
        const auto deadline = GetTickCount64() + 5000;
        while (!panel && GetTickCount64() < deadline) {
            struct Search { DWORD process; HWND window = nullptr; } search{child.dwProcessId};
            EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
                auto& search = *reinterpret_cast<Search*>(param);
                DWORD process = 0; GetWindowThreadProcessId(hwnd, &process);
                wchar_t name[128]{}; GetClassNameW(hwnd, name, 128);
                if (process == search.process && wstring_view(name) == PANEL_CLASS && GetPropW(hwnd, L"ScreenShotPlus.Ready")) {
                    search.window = hwnd; return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&search));
            panel = search.window;
            if (!panel) Pump(15);
        }
        Check(panel && WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT, "startup fixture initializes its real message loop");
        const bool expected = variant == 0 && animationAvailable;
        Check((GetPropW(panel, L"ScreenShotPlus.StartupAnimating") != nullptr) == expected,
            "startup reads ultimate preference and honors disabled, lower-tier and tray launch policies");
        if (expected) {
            Pump(240); Snapshot(panel, root / L"startup-ultimate-ui", false);
            const auto finish = GetTickCount64() + 2500;
            while (GetPropW(panel, L"ScreenShotPlus.StartupAnimating") && GetTickCount64() < finish) Pump(20);
            Check(!GetPropW(panel, L"ScreenShotPlus.StartupAnimating") && IsWindowVisible(panel), "ultimate startup settles to usable panel");
            Snapshot(panel, root / L"startup-settled-ui");
        } else {
            if (tray && GetPropW(panel, L"ScreenShotPlus.TrayRegistered")) Check(!IsWindowVisible(panel), "tray launch keeps startup silent");
            Pump(400); Check(!GetPropW(panel, L"ScreenShotPlus.StartupAnimating"), "suppressed startup never starts a delayed intro");
        }
        PostThreadMessageW(child.dwThreadId, WM_QUIT, 0, 0);
        Check(WaitForSingleObject(child.hProcess, 5000) == WAIT_OBJECT_0, "isolated startup fixture shuts down normally");
        DWORD code = 1; GetExitCodeProcess(child.hProcess, &code); Check(code == 0, "startup fixture exits successfully");
    }
    if (!animationAvailable) cout << "SKIP ultimate startup choreography: system reduced motion is enabled; startup suppression verified\n";
}
}
int wmain(int argc, wchar_t** argv) {
    if ((argc == 3 || argc == 4) && wstring_view(argv[1]) == L"--startup-fixture") {
        const HRESULT ole = OleInitialize(nullptr); if (FAILED(ole)) return 2;
        GdiplusStartupInput input; ULONG_PTR token = 0;
        if (GdiplusStartup(&token, &input, nullptr) != Ok) { OleUninitialize(); return 3; }
        TestStartupKey startup;
        const int code = RunApp(GetModuleHandleW(nullptr), filesystem::path(argv[2]), argc == 4 && wstring_view(argv[3]) == L"--tray", startup.key.c_str());
        CancelVisualEffects(); CancelSelection(); DestroyEditor(); DestroySettings(); DestroyPreview();
        GdiplusShutdown(token); OleUninitialize(); return code;
    }
    if (argc == 2 && wstring_view(argv[1]) == L"--tray-probe") {
        cout << "Process integrity RID=" << CurrentProcessIntegrity() << '\n';
        HWND shell = FindWindowW(L"Shell_TrayWnd", nullptr);
        DWORD pid = 0; GetWindowThreadProcessId(shell, &pid);
        cout << "Shell HWND=" << shell << " PID=" << pid << '\n';
        HWND window = CreateWindowExW(0, L"STATIC", L"ScreenShotPlus diagnostic", WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        TrayIcon tray;
        tray.data.hWnd = window; tray.data.uID = 99; tray.data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        tray.data.uCallbackMessage = WM_TRAY; tray.data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(tray.data.szTip, L"ScreenShotPlus diagnostic");
        cout << "Data size=" << sizeof(tray.data) << " cbSize=" << tray.data.cbSize << " HWND=" << window << " HICON=" << tray.data.hIcon << '\n';
        SetLastError(0);
        const bool result = RegisterTrayIcon(tray);
        cout << "Registered=" << result << " modern=" << tray.modern << " diagnosticError=" << tray.diagnosticError << '\n';
        if (result) RemoveTrayIcon(tray);
        DestroyWindow(window); return result ? 0 : 1;
    }
    if (argc == 2 && wstring_view(argv[1]) == L"--config-location-probe") {
        wstring warning;
        return ConfigFile() == PortableConfigFile() && LoadConfig(ConfigFile(), warning).previewSeconds == 61 && warning.empty() ? 74 : 94;
    }
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
    const bool animationOnly = argc > 1 && wstring_view(argv[1]) == L"--animation-ui";
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
        AnimationTiming(); cout << "PASS fractional refresh / 120 fallback / interrupted curves / time-based duration / five motion tiers / speeds / reduced motion\n";
        if (liveUi) {
            AnimationUi(GetModuleHandleW(nullptr), root); cout << "PASS animation frames / hover / toggle / press / idle / hide / reopen / GDI lifetime\n";
            RichEffectsUi(GetModuleHandleW(nullptr), root); cout << "PASS layered flight / drag card / copy feedback / cancel return / accepted drop / input transparency / resource lifetime (visual fixture, not client acceptance)\n";
            CustomizationUi(GetModuleHandleW(nullptr), root); cout << "PASS capture customization / original pixels / entrance off / preview corner and size / physical DPI / restrained tiers (isolated native UI fixture)\n";
            SettingsMotionUi(GetModuleHandleW(nullptr), root); cout << "PASS native five-stop selector / light-tier forced animation / signature and final label / scrub and keyboard / style rollback and persistence / hidden idle and GDI lifetime\n";
            StartupUi(filesystem::absolute(argv[0]), root); cout << "PASS persisted ultimate startup / disabled and lower-tier suppression / silent tray startup / normal shutdown (isolated child application fixture)\n";
        }
        if (!animationOnly) {
        Geometry(); cout << "PASS selection geometry\n";
        InstanceProcesses(filesystem::absolute(argv[0])); cout << "PASS 8 concurrent processes / single-instance mutex / restart\n";
        InstanceActivation(GetModuleHandleW(nullptr)); cout << "PASS delayed primary window / duplicate activation\n";
        TrayTransitions(); cout << "PASS simulated Shell failure / recovery / legacy event handling (not actual tray acceptance)\n";
        Configuration(root); cout << "PASS config round trip / legacy motion defaults / custom preferences / strict malformed ranges / corrupt and unwritable paths\n";
        ConfigurationFallback(root, filesystem::absolute(argv[0])); cout << "PASS config fallback / detailed errors / real process portable restart\n";
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
            cout << (isolatedUi ? "PASS isolated application tray status handling / capture / edit / preview / state / shutdown (single-instance UI fixture skipped)\n" :
                "PASS application capture / edit / persistence / state / single instance / shutdown\n");
        else cout << "SKIP application UI smoke: --no-ui or existing application instance\n";
        if (liveUi) ExecutableSmoke(filesystem::current_path() / L"x64" / L"Release" / L"ScreenShotPlus.exe");
        if (liveUi) { SettingsFailureUi(GetModuleHandleW(nullptr), root); cout << "PASS actual settings save error / visible config path\n"; }
        }
        cout << "Monitors actually present: " << GetSystemMetrics(SM_CMONITORS) << '\n';
        wcout << L"Test artifacts: " << root.wstring() << L'\n';
    } catch (const exception& e) { cerr << "FAIL: " << e.what() << '\n'; code = 1; }
    CancelVisualEffects(); CancelSelection(); DestroyEditor(); DestroySettings(); DestroyPreview();
    GdiplusShutdown(token); OleUninitialize(); return code;
}
