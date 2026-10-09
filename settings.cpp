#include "settings.h"
#include "ui.h"
using namespace std;
using namespace Gdiplus;
namespace {
struct Settings {
    HWND window = nullptr, owner = nullptr;
    Config working;
    function<bool(const Config&, wstring&)> apply;
    POINT mouse{-1, -1}, pressed{};
    bool listening = false, mouseDown = false;
    wstring notice;
    filesystem::path configFile;
} settings;
const RectF closeButton{452, 16, 28, 28}, folderButton{24, 180, 452, 44},
    hotkeyButton{24, 272, 452, 44}, printButton{24, 336, 452, 44},
    secondsButton{24, 450, 452, 44}, startupButton{24, 542, 452, 44},
    saveButton{24, 746, 218, 44}, cancelButton{254, 746, 222, 44};
void DrawSettings(HWND hwnd, HDC target = nullptr) {
    ui::Paint paint(hwnd, target); Graphics g(paint.buffer); ui::Prepare(g, hwnd);
    ui::Text(g, L"设置 · v1.1.2", {24, 25, 390, 40}, 25, ui::foreground, true);
    ui::Button(g, closeButton, L"×", false, ui::Inside(settings.mouse, closeButton));
    ui::Text(g, L"保存目录", {24, 90, 452, 26}, 14, ui::muted);
    ui::Round(g, {24, 122, 452, 46}, 10, ui::surface);
    ui::Text(g, settings.working.folder.wstring(), {36, 131, 428, 30}, 12);
    ui::Button(g, folderButton, L"选择文件夹…", false, ui::Inside(settings.mouse, folderButton));
    ui::Text(g, L"截图快捷键", {24, 238, 452, 26}, 14, ui::muted);
    ui::Button(g, hotkeyButton, settings.listening ? L"请按组合键（Esc 取消）" : HotkeyText(settings.working) + L"   点击修改",
        settings.listening, ui::Inside(settings.mouse, hotkeyButton));
    ui::Button(g, printButton, settings.working.printScreen ? L"[已开启] 同时使用 PrintScreen" : L"[已关闭] 同时使用 PrintScreen",
        false, ui::Inside(settings.mouse, printButton));
    ui::Text(g, L"若 PrintScreen 仍打开系统截图，请在 Windows 设置中\n关闭“使用 Print Screen 键打开屏幕捕获”。", {24, 390, 452, 44}, 12, ui::muted);
    const auto seconds = settings.working.previewSeconds;
    ui::Button(g, secondsButton, seconds ? L"缩略图：" + to_wstring(seconds) + L" 秒后收起   点击切换" :
        L"缩略图：保留至关闭或下一次截图   点击切换", false, ui::Inside(settings.mouse, secondsButton));
    ui::Text(g, L"悬停和拖拽期间暂停倒计时。截图文件始终保留。", {24, 506, 452, 24}, 12, ui::muted);
    ui::Button(g, startupButton, settings.working.startup ? L"☑ 开机自启动（直接进入托盘）" : L"☐ 开机自启动（直接进入托盘）",
        settings.working.startup, ui::Inside(settings.mouse, startupButton));
    ui::Text(g, L"仅对当前用户生效；移动 EXE 后请重新保存设置。", {24, 595, 452, 24}, 12, ui::muted);
    ui::Text(g, L"配置文件：" + settings.configFile.wstring(), {24, 624, 452, 40}, 11, ui::muted);
    ui::Text(g, settings.notice, {24, 676, 452, 60}, 12, ui::danger);
    ui::Button(g, saveButton, L"保存设置", true, ui::Inside(settings.mouse, saveButton));
    ui::Button(g, cancelButton, L"取消", false, ui::Inside(settings.mouse, cancelButton));
}
void Cancel() { HideSettings(); PostMessageW(settings.owner, WM_SHOW_PANEL, 0, 0); }
LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_PAINT: DrawSettings(hwnd); return 0;
    case WM_PRINTCLIENT: DrawSettings(hwnd, reinterpret_cast<HDC>(wparam)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CLOSE: Cancel(); return 0;
    case WM_MOUSEMOVE: {
        settings.mouse = ui::Mouse(hwnd, lparam);
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&track);
        InvalidateRect(hwnd, nullptr, FALSE); return 0;
    }
    case WM_MOUSELEAVE: settings.mouse = {-1, -1}; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN:
        settings.pressed = ui::Mouse(hwnd, lparam); settings.mouseDown = true; SetCapture(hwnd); return 0;
    case WM_CAPTURECHANGED: settings.mouseDown = false; return 0;
    case WM_NCHITTEST: {
        POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; ScreenToClient(hwnd, &p);
        const float s = ui::Scale(hwnd); p = {static_cast<LONG>(p.x / s), static_cast<LONG>(p.y / s)};
        return p.y < 76 && !ui::Inside(p, closeButton) ? HTCAPTION : HTCLIENT;
    }
    case WM_LBUTTONUP: {
        const bool clicked = settings.mouseDown; const POINT down = settings.pressed;
        settings.mouseDown = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (!clicked) return 0;
        const POINT p = ui::Mouse(hwnd, lparam);
        const auto hit = [&](const RectF& r) { return ui::Inside(p, r) && ui::Inside(down, r); };
        if (hit(closeButton) || hit(cancelButton)) { Cancel(); return 0; }
        if (hit(folderButton)) {
            settings.listening = false; ui::PickFolder(hwnd, settings.working.folder);
        } else if (hit(hotkeyButton)) {
            settings.listening = true;
            settings.notice = L"支持 Ctrl / Alt / Shift + 字母、数字或 F1–F24。";
            SetFocus(hwnd);
        } else if (hit(printButton)) {
            settings.listening = false; settings.working.printScreen = !settings.working.printScreen;
        } else if (hit(secondsButton)) {
            settings.listening = false;
            constexpr array<unsigned, 7> choices{0, 5, 10, 15, 30, 60, 120};
            const auto it = find(choices.begin(), choices.end(), settings.working.previewSeconds);
            settings.working.previewSeconds = it == choices.end() || next(it) == choices.end() ? choices[0] : *next(it);
        } else if (hit(startupButton)) {
            settings.listening = false; settings.working.startup = !settings.working.startup;
        } else if (hit(saveButton)) {
            settings.listening = false;
            if (settings.apply && settings.apply(settings.working, settings.notice)) {
                HideSettings(); PostMessageW(settings.owner, WM_SETTINGS_SAVED, 0, 0);
            }
        }
        InvalidateRect(hwnd, nullptr, FALSE); return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (wparam == VK_ESCAPE) {
            if (settings.listening) { settings.listening = false; settings.notice.clear(); InvalidateRect(hwnd, nullptr, FALSE); }
            else Cancel();
            return 0;
        }
        if (settings.listening) {
            Config candidate = settings.working;
            candidate.modifiers = 0;
            if (GetKeyState(VK_CONTROL) & 0x8000) candidate.modifiers |= MOD_CONTROL;
            if (GetKeyState(VK_SHIFT) & 0x8000) candidate.modifiers |= MOD_SHIFT;
            if (GetKeyState(VK_MENU) & 0x8000) candidate.modifiers |= MOD_ALT;
            candidate.key = static_cast<UINT>(wparam);
            if (ValidConfig(candidate) && !(GetKeyState(VK_LWIN) & 0x8000) && !(GetKeyState(VK_RWIN) & 0x8000)) {
                settings.working = candidate; settings.listening = false; settings.notice.clear();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    case WM_DPICHANGED: {
        ui::DpiChanged(hwnd, lparam, 500, 812); return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}
}
bool InitSettings(HINSTANCE instance, HWND owner, function<bool(const Config&, wstring&)> apply) {
    WNDCLASSEXW cls{sizeof(cls)}; cls.hInstance = instance; cls.lpfnWndProc = SettingsProc;
    cls.lpszClassName = L"ScreenShotPlus.Settings"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    WNDCLASSEXW existing{sizeof(existing)};
    if (!GetClassInfoExW(instance, cls.lpszClassName, &existing) && !RegisterClassExW(&cls)) return false;
    settings.owner = owner; settings.apply = move(apply);
    settings.window = CreateWindowExW(WS_EX_TOOLWINDOW, cls.lpszClassName, L"ScreenShotPlus 设置", WS_POPUP,
        0, 0, 500, 812, owner, nullptr, instance, nullptr);
    if (settings.window) ui::Corners(settings.window);
    return settings.window != nullptr;
}
void ShowSettings(const Config& config) {
    if (!settings.window) return;
    if (!SettingsVisible()) { settings.working = config; settings.listening = false; settings.notice.clear(); }
    ui::Place(settings.window, 500, 812); ShowWindow(settings.window, SW_SHOW);
    SetForegroundWindow(settings.window); SetFocus(settings.window); InvalidateRect(settings.window, nullptr, FALSE);
}
void SetSettingsConfigFile(const filesystem::path& file) {
    settings.configFile = file;
    if (settings.window) InvalidateRect(settings.window, nullptr, FALSE);
}
void HideSettings() { if (settings.window) { settings.listening = false; ShowWindow(settings.window, SW_HIDE); } }
void RecordSettingsHotkey(const Config& config) {
    if (!SettingsVisible() || !settings.listening) return;
    settings.working.modifiers = config.modifiers; settings.working.key = config.key;
    settings.listening = false; settings.notice.clear(); InvalidateRect(settings.window, nullptr, FALSE);
}
bool SettingsVisible() { return settings.window && IsWindowVisible(settings.window); }
void DestroySettings() {
    if (settings.window) { DestroyWindow(settings.window); settings.window = nullptr; }
    settings.apply = {};
}
