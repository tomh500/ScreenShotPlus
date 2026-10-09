#include "app.h"
#include "ui.h"
#include "Resource.h"
#include "capture.h"
#include "storage.h"
#include "preview.h"
#include "config.h"
#include "settings.h"
#include "dragdrop.h"
#include "hotkeys.h"
#include "editor.h"
#include "tray.h"
#include "instance.h"
using namespace std;
using namespace Gdiplus;
namespace {
struct App {
    enum Phase { Idle, Selecting, Saving, Dragging, Editing } phase = Idle;
    HINSTANCE instance = nullptr;
    HWND panel = nullptr;
    TrayIcon tray;
    HICON smallIcon = nullptr;
    wstring notice = L"截图后自动保存，拖动缩略图即可分享。";
    wstring trayWarning;
    POINT mouse{-1, -1}, pressed{};
    bool mouseDown = false;
    Config config;
    filesystem::path configFile;
    bool allowPortableConfig = true;
    wstring startupKey;
    Hotkeys hotkeys;
    bool exiting = false;
    UINT taskbarCreated = 0;
    unique_ptr<Bitmap> latest, pending;
    filesystem::path latestFile;
    POINT latestPoint{}, pendingPoint{};
    bool restorePreview = false;
} app;
const array<RectF, 5> buttons{{{24, 172, 372, 52}, {24, 236, 372, 44},
    {24, 292, 372, 44}, {24, 348, 372, 44}, {24, 404, 372, 44}}};
const RectF closeButton{372, 16, 28, 28};
const RectF retryButton{24, 460, 372, 44};
constexpr UINT_PTR trayRetryTimer = 4;
void ShowPanel() {
    if (app.exiting) return;
    if (app.phase == App::Editing) { FocusEditor(); return; }
    if (app.phase != App::Idle) return;
    if (SettingsVisible()) { ShowSettings(app.config); return; }
    ui::Place(app.panel, 420, 570); ShowWindow(app.panel, SW_SHOW);
    SetForegroundWindow(app.panel); InvalidateRect(app.panel, nullptr, FALSE);
}
void Notice(const wstring& text) { app.notice = text; ShowPanel(); }
void LatestPreview() {
    if (app.latest) ShowPreview(app.latest.get(), app.latestFile, app.latestPoint, app.config.previewSeconds);
    else Notice(L"还没有截图。先点击“开始区域截图”。");
}
void SavePending() {
    if (!app.pending || app.phase != App::Idle) return;
    app.phase = App::Saving;
    filesystem::path file; wstring error;
    const bool ok = SavePng(*app.pending, app.config.folder, file, error);
    app.phase = App::Idle;
    if (!ok) { Notice(error); return; }
    HidePreview(); app.latest = move(app.pending); app.latestFile = move(file);
    app.latestPoint = app.pendingPoint;
    app.notice = L"已自动保存 PNG，可直接拖动缩略图分享。";
    ShowWindow(app.panel, SW_HIDE); LatestPreview();
}
void StartCapture() {
    if (app.phase != App::Idle || SettingsVisible() || app.exiting) return;
    app.restorePreview = PreviewVisible();
    ShowWindow(app.panel, SW_HIDE); HidePreview(); DwmFlush();
    app.phase = App::Selecting;
    wstring error;
    if (!BeginSelection(app.instance, [](unique_ptr<Bitmap> image, POINT point, wstring failure) {
        if (app.exiting) return;
        app.phase = App::Idle;
        if (!image) {
            if (app.restorePreview && app.latest) LatestPreview();
            if (!failure.empty()) Notice(failure);
            return;
        }
        app.pending = move(image); app.pendingPoint = point; SavePending();
    }, error)) {
        app.phase = App::Idle;
        if (app.restorePreview && app.latest) LatestPreview();
        Notice(error);
    }
}
void DragLatest() {
    if (app.phase != App::Idle || !app.latest || SettingsVisible()) return;
    app.phase = App::Dragging;
    DWORD effect = DROPEFFECT_NONE;
    const HRESULT result = DragFile(app.latestFile, effect);
    app.phase = App::Idle;
    if (FAILED(result)) Notice(L"拖放失败；请确认截图文件仍存在，且接收程序支持文件拖入。");
    else {
        app.notice = result == DRAGDROP_S_DROP && effect == DROPEFFECT_COPY ? L"文件已拖出，原截图仍保留。" : L"拖放已取消，原截图仍保留。";
        InvalidateRect(app.panel, nullptr, FALSE);
    }
}
void EditLatest() {
    if (app.phase != App::Idle || !app.latest || SettingsVisible() || app.exiting) return;
    app.phase = App::Editing;
    ShowWindow(app.panel, SW_HIDE); HidePreview();
    wstring error;
    if (!ShowEditor(*app.latest, error)) { app.phase = App::Idle; LatestPreview(); Notice(error); }
}
bool SaveEdited(unique_ptr<Bitmap>& image, wstring& error) {
    if (app.exiting || app.phase != App::Editing || !image) return false;
    filesystem::path file;
    if (!SavePng(*image, app.config.folder, file, error)) return false;
    HidePreview(); app.latest = move(image); app.latestFile = move(file);
    app.phase = App::Idle; app.notice = L"编辑后的 PNG 已保存，拖动缩略图即可分享。";
    LatestPreview(); return true;
}
void CancelEdit() {
    if (app.exiting) return;
    app.phase = App::Idle; LatestPreview();
}
bool ApplyConfig(const Config& next, wstring& error) {
    filesystem::path destination;
    if (!ApplyConfiguredSettings(app.panel, app.hotkeys, app.config, next, app.configFile, error, app.startupKey.c_str(),
        app.allowPortableConfig ? PortableConfigFile() : filesystem::path{}, &destination)) return false;
    app.configFile = move(destination); SetSettingsConfigFile(app.configFile);
    if (PreviewVisible()) LatestPreview();
    return true;
}
void AddTray() {
    if (RegisterTrayIcon(app.tray)) {
        SetPropW(app.panel, L"ScreenShotPlus.TrayRegistered", reinterpret_cast<HANDLE>(1));
        KillTimer(app.panel, trayRetryTimer);
        if (!app.trayWarning.empty() && app.notice == app.trayWarning) app.notice = L"托盘已恢复，关闭面板后可继续截图。";
        app.trayWarning.clear();
    } else {
        RemovePropW(app.panel, L"ScreenShotPlus.TrayRegistered");
        SetTimer(app.panel, trayRetryTimer, 2000, nullptr);
        app.trayWarning = TrayFailureMessage(app.tray); app.notice = app.trayWarning;
        if (!app.exiting && app.phase == App::Idle && !SettingsVisible()) ShowPanel();
    }
    InvalidateRect(app.panel, nullptr, FALSE);
}
void HidePanel() {
    AddTray(); if (!app.tray.registered) return;
    ShowWindow(app.panel, SW_HIDE);
}
void DrawPanel(HWND hwnd, HDC target = nullptr) {
    ui::Paint paint(hwnd, target); Graphics g(paint.buffer); ui::Prepare(g, hwnd);
    ui::Round(g, {24, 24, 40, 40}, 12, ui::accent);
    ui::Text(g, L"S", {35, 29, 30, 34}, 24, ui::background, true);
    ui::Text(g, L"ScreenShotPlus", {78, 27, 278, 36}, 23, ui::foreground, true);
    ui::Button(g, hwnd, closeButton, L"×", false, ui::Inside(app.mouse, closeButton));
    ui::Text(g, L"截取 · 保存 · 拖动分享", {24, 92, 372, 28}, 17);
    ui::Text(g, L"v1.2.0", {328, 98, 68, 22}, 12, ui::accent);
    ui::Text(g, app.notice, {24, 128, 372, 40}, 12, ui::muted);
    const array<wstring, 5> labels{L"开始区域截图   " + HotkeyText(app.config), L"显示最新截图",
        L"打开保存目录", L"设置", L"退出程序"};
    for (size_t i = 0; i < buttons.size(); ++i)
        ui::Button(g, hwnd, buttons[i], labels[i], i == 0, ui::Inside(app.mouse, buttons[i]));
    if (app.pending) ui::Button(g, hwnd, retryButton, L"重试保存未保存的截图", false, ui::Inside(app.mouse, retryButton));
    ui::Text(g, app.tray.registered ? L"关闭此面板后，程序仍在托盘运行" : L"托盘不可用，面板会保留；可在上方退出程序",
        {24, 530, 372, 24}, 12, ui::muted);
}
LRESULT CALLBACK PanelProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ui::MotionMessage(hwnd, message, wparam, lparam)) return 0;
    if (app.taskbarCreated && message == app.taskbarCreated) { app.tray.registered = false; AddTray(); return 0; }
    switch (message) {
    case WM_PAINT: DrawPanel(hwnd); return 0;
    case WM_PRINTCLIENT: DrawPanel(hwnd, reinterpret_cast<HDC>(wparam)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SHOW_PANEL: AddTray(); ShowPanel(); return 0;
    case WM_CLOSE: HidePanel(); return 0;
    case WM_TIMER: if (wparam == trayRetryTimer) AddTray(); return 0;
    case WM_TRAY:
        if (TrayEventOpensPanel(lparam)) ShowPanel();
        return 0;
    case WM_HOTKEY:
        if (SettingsVisible()) {
            if (wparam == static_cast<WPARAM>(app.hotkeys.primaryId)) RecordSettingsHotkey(app.config);
        } else if (wparam == static_cast<WPARAM>(app.hotkeys.primaryId) || (wparam == 2 && app.hotkeys.printRegistered)) StartCapture();
        return 0;
    case WM_SETTINGS_SAVED:
        if (app.pending) SavePending();
        else Notice(L"设置已保存，下次启动仍然有效。配置：" + app.configFile.wstring());
        return 0;
    case WM_MOUSEMOVE: {
        app.mouse = ui::Mouse(hwnd, lparam);
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&track);
        InvalidateRect(hwnd, nullptr, FALSE); return 0;
    }
    case WM_MOUSELEAVE: app.mouse = {-1, -1}; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN:
        app.pressed = ui::Mouse(hwnd, lparam); app.mouseDown = true; SetCapture(hwnd); return 0;
    case WM_CAPTURECHANGED: app.mouseDown = false; return 0;
    case WM_NCHITTEST: {
        POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; ScreenToClient(hwnd, &p);
        const auto s = ui::Scale(hwnd);
        p = {static_cast<LONG>(p.x / s), static_cast<LONG>(p.y / s)};
        return p.y < 76 && !ui::Inside(p, closeButton) ? HTCAPTION : HTCLIENT;
    }
    case WM_LBUTTONUP: {
        const bool clicked = app.mouseDown; const POINT down = app.pressed;
        app.mouseDown = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (!clicked) return 0;
        const POINT p = ui::Mouse(hwnd, lparam);
        if (ui::Inside(p, closeButton) && ui::Inside(down, closeButton)) { HidePanel(); return 0; }
        if (app.pending && ui::Inside(p, retryButton) && ui::Inside(down, retryButton)) { SavePending(); return 0; }
        for (size_t i = 0; i < buttons.size(); ++i) if (ui::Inside(p, buttons[i]) && ui::Inside(down, buttons[i])) {
            if (i == 0) StartCapture();
            else if (i == 1) LatestPreview();
            else if (i == 2) {
                if (!ui::OpenFolder(app.config.folder)) Notice(L"无法打开保存目录；请在设置中更换目录。");
            }
            else if (i == 3) {
                ShowWindow(hwnd, SW_HIDE); ShowSettings(app.config);
            }
            else if (i == 4) DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_KEYDOWN: if (wparam == VK_ESCAPE) HidePanel(); return 0;
    case WM_DPICHANGED: {
        ui::DpiChanged(hwnd, lparam, 420, 570);
        return 0;
    }
    case WM_DESTROY:
        app.exiting = true; CancelSelection();
        KillTimer(hwnd, trayRetryTimer); RemoveTrayIcon(app.tray);
        ReleaseHotkeys(hwnd, app.hotkeys);
        DestroyEditor(); DestroySettings(); DestroyPreview(); app.latest.reset(); app.pending.reset();
        if (app.smallIcon) { DestroyIcon(app.smallIcon); app.smallIcon = nullptr; }
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
}
int RunApp(HINSTANCE instance, const filesystem::path& configFile, bool trayStartup, const wchar_t* startupKey) {
    app.instance = instance;
    app.startupKey = startupKey;
    app.configFile = configFile.empty() ? ConfigFile() : configFile;
    app.allowPortableConfig = configFile.empty();
    wstring warning; app.config = LoadConfig(app.configFile, warning);
    if (!warning.empty()) app.notice = warning;
    app.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSEXW cls{sizeof(cls)};
    cls.hInstance = instance; cls.lpfnWndProc = PanelProc; cls.lpszClassName = PANEL_CLASS;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SCREENSHOTPLUS));
    if (!RegisterClassExW(&cls)) return 4;
    app.panel = CreateWindowExW(WS_EX_TOOLWINDOW, PANEL_CLASS, L"ScreenShotPlus", WS_POPUP,
        0, 0, 420, 520, nullptr, nullptr, instance, nullptr);
    if (!app.panel) return 5;
    if (configFile.empty()) SetPropW(app.panel, INSTANCE_PANEL_MARKER, reinterpret_cast<HANDLE>(1));
    if (!InitPreview(instance, DragLatest, EditLatest) || !InitSettings(instance, app.panel, ApplyConfig) ||
        !InitEditor(instance, app.panel, SaveEdited, CancelEdit)) {
        DestroyWindow(app.panel); return 7;
    }
    SetSettingsConfigFile(app.configFile);
    ui::Corners(app.panel);
    app.smallIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_SMALL), IMAGE_ICON,
        GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem()), GetSystemMetricsForDpi(SM_CYSMICON, GetDpiForSystem()), LR_DEFAULTCOLOR));
    app.tray.data.hWnd = app.panel; app.tray.data.uID = 1;
    app.tray.data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    app.tray.data.uCallbackMessage = WM_TRAY; app.tray.data.hIcon = app.smallIcon ? app.smallIcon : cls.hIcon;
    wcscpy_s(app.tray.data.szTip, L"ScreenShotPlus · 区域截图与拖动分享"); AddTray();
    const auto hotkeyWarning = RegisterConfiguredHotkeys(app.panel, app.hotkeys, app.config);
    if (!hotkeyWarning.empty()) app.notice += L" " + hotkeyWarning;
    if (!trayStartup) ShowPanel();
    SetPropW(app.panel, L"ScreenShotPlus.Ready", reinterpret_cast<HANDLE>(1));
    MSG message{}; BOOL result;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    if (IsWindow(app.panel)) DestroyWindow(app.panel);
    return result == -1 ? 6 : static_cast<int>(message.wParam);
}
