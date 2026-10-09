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
using namespace std;
using namespace Gdiplus;
namespace {
struct App {
    enum Phase { Idle, Selecting, Saving, Dragging } phase = Idle;
    HINSTANCE instance = nullptr;
    HWND panel = nullptr;
    NOTIFYICONDATAW tray{sizeof(tray)};
    wstring notice = L"截图后自动保存，拖动缩略图即可分享。";
    POINT mouse{-1, -1}, pressed{};
    bool mouseDown = false;
    Config config;
    filesystem::path configFile;
    Hotkeys hotkeys;
    bool exiting = false;
    bool startup = false;
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
void ShowPanel() {
    if (app.exiting || app.phase != App::Idle) return;
    if (SettingsVisible()) { ShowSettings(app.config); return; }
    ui::Place(app.panel, PANEL_WIDTH, PANEL_HEIGHT); ShowWindow(app.panel, SW_SHOW);
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
bool ApplyConfig(const Config& next, wstring& error) {
    if (!ApplyConfiguredSettings(app.panel, app.hotkeys, app.config, next, app.configFile, error)) return false;
    if (PreviewVisible()) LatestPreview();
    return true;
}
void AddTray() {
    Shell_NotifyIconW(NIM_ADD, &app.tray); app.tray.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &app.tray);
}
void DrawPanel(HWND hwnd) {
    ui::Paint paint(hwnd); Graphics g(paint.buffer); ui::Prepare(g, hwnd);
    ui::Round(g, {24, 24, 40, 40}, 12, ui::accent);
    ui::Text(g, L"S", {35, 29, 30, 34}, 24, ui::background, true);
    ui::Text(g, L"ScreenShotPlus", {78, 27, 278, 36}, 23, ui::foreground, true);
    ui::Button(g, closeButton, L"×", false, ui::Inside(app.mouse, closeButton));
    ui::Text(g, L"截取 · 保存 · 拖动分享", {24, 92, 372, 28}, 17);
    ui::Text(g, app.notice, {24, 128, 372, 40}, 12, ui::muted);
    const array<wstring, 5> labels{L"开始区域截图   " + HotkeyText(app.config), L"显示最新截图",
        L"打开保存目录", L"设置", L"退出程序"};
    for (size_t i = 0; i < buttons.size(); ++i)
        ui::Button(g, buttons[i], labels[i], i == 0, ui::Inside(app.mouse, buttons[i]));
    if (app.pending) ui::Button(g, retryButton, L"重试保存未保存的截图", false, ui::Inside(app.mouse, retryButton));
    ui::Text(g, L"关闭此面板后，程序仍在托盘运行", {24, 530, 372, 24}, 12, ui::muted);
}
LRESULT CALLBACK PanelProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (app.taskbarCreated && message == app.taskbarCreated) { AddTray(); return 0; }
    switch (message) {
    case WM_PAINT: DrawPanel(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SHOW_PANEL: ShowPanel(); return 0;
    case WM_CLOSE: ShowWindow(hwnd, SW_HIDE); return 0;
    case WM_TRAY:
        if (LOWORD(lparam) == NIN_SELECT || LOWORD(lparam) == NIN_KEYSELECT || LOWORD(lparam) == WM_CONTEXTMENU) ShowPanel();
        return 0;
    case WM_HOTKEY:
        if (SettingsVisible()) {
            if (wparam == static_cast<WPARAM>(app.hotkeys.primaryId)) RecordSettingsHotkey(app.config);
        } else if (wparam == static_cast<WPARAM>(app.hotkeys.primaryId) || (wparam == 2 && app.hotkeys.printRegistered)) StartCapture();
        return 0;
    case WM_SETTINGS_SAVED:
        if (app.pending) SavePending();
        else Notice(L"设置已保存，下次启动仍然有效。");
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
        if (ui::Inside(p, closeButton) && ui::Inside(down, closeButton)) { ShowWindow(hwnd, SW_HIDE); return 0; }
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
    case WM_KEYDOWN: if (wparam == VK_ESCAPE) ShowWindow(hwnd, SW_HIDE); return 0;
    case WM_DPICHANGED: {
        ui::DpiChanged(hwnd, lparam, PANEL_WIDTH, PANEL_HEIGHT);
        return 0;
    }
    case WM_DESTROY:
        app.exiting = true; CancelSelection();
        Shell_NotifyIconW(NIM_DELETE, &app.tray);
        ReleaseHotkeys(hwnd, app.hotkeys);
        DestroySettings(); DestroyPreview(); app.latest.reset(); app.pending.reset(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
}
int RunApp(HINSTANCE instance, bool startup, const filesystem::path& configFile) {
    app.instance = instance;
    app.startup = startup;
    app.configFile = configFile.empty() ? ConfigFile() : configFile;
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
    if (!InitPreview(instance, DragLatest) || !InitSettings(instance, app.panel, ApplyConfig)) {
        DestroyWindow(app.panel); return 7;
    }
    ui::Corners(app.panel);
    app.tray.hWnd = app.panel; app.tray.uID = 1;
    app.tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    app.tray.uCallbackMessage = WM_TRAY; app.tray.hIcon = cls.hIcon;
    wcscpy_s(app.tray.szTip, L"ScreenShotPlus · 区域截图与拖动分享"); AddTray();
    const auto hotkeyWarning = RegisterConfiguredHotkeys(app.panel, app.hotkeys, app.config);
    if (!hotkeyWarning.empty()) app.notice += L" " + hotkeyWarning;
    // 开机自启动时静默驻留托盘，不弹出面板；此时托盘气泡仍说明程序已就绪。
    if (app.startup) app.notice = L"已随开机启动，正在托盘待命。点击托盘图标可打开面板。";
    else ShowPanel();
    MSG message{}; BOOL result;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    if (IsWindow(app.panel)) DestroyWindow(app.panel);
    return result == -1 ? 6 : static_cast<int>(message.wParam);
}
