#include "preview.h"
#include "ui.h"
using namespace std;
using namespace Gdiplus;
namespace {
struct Preview {
    HWND window = nullptr;
    Bitmap* image = nullptr;
    filesystem::path file;
    POINT mouse{-1, -1}, pressed{};
    bool pressing = false, dragging = false;
    ULONGLONG remaining = 0, lastTick = 0;
    function<void()> drag, edit;
} preview;
const RectF closeButton{278, 12, 28, 28}, imageRect{16, 52, 288, 166};
void DrawPreview(HWND hwnd, HDC target = nullptr) {
    ui::Paint paint(hwnd, target); Graphics g(paint.buffer); ui::Prepare(g, hwnd);
    ui::Text(g, L"已保存 PNG", {16, 17, 240, 26}, 14, ui::accent, true);
    ui::Button(g, closeButton, L"×", false, ui::Inside(preview.mouse, closeButton));
    ui::Round(g, imageRect, 10, ui::surface);
    if (preview.image) {
        const float scale = min(imageRect.Width / preview.image->GetWidth(), imageRect.Height / preview.image->GetHeight());
        const float w = preview.image->GetWidth() * scale, h = preview.image->GetHeight() * scale;
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.DrawImage(preview.image, RectF(imageRect.X + (imageRect.Width - w) / 2,
            imageRect.Y + (imageRect.Height - h) / 2, w, h));
    }
    ui::Text(g, L"单击编辑 · 拖动到聊天窗口或文件夹", {16, 231, 288, 24}, 12);
    ui::Text(g, preview.file.filename().wstring(), {16, 259, 288, 22}, 10, ui::muted);
}
LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_PAINT: DrawPreview(hwnd); return 0;
    case WM_PRINTCLIENT: DrawPreview(hwnd, reinterpret_cast<HDC>(wparam)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_CLOSE: HidePreview(); return 0;
    case WM_LBUTTONDOWN: {
        preview.mouse = ui::Mouse(hwnd, lparam);
        if (ui::Inside(preview.mouse, closeButton)) { HidePreview(); return 0; }
        if (ui::Inside(preview.mouse, imageRect)) {
            preview.pressing = true; preview.pressed = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; SetCapture(hwnd);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const bool clicked = preview.pressing && !preview.dragging;
        preview.pressing = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        const POINT p = ui::Mouse(hwnd, lparam);
        const int dx = abs(GET_X_LPARAM(lparam) - preview.pressed.x), dy = abs(GET_Y_LPARAM(lparam) - preview.pressed.y);
        if (clicked && ui::Inside(p, imageRect) &&
            dx < GetSystemMetricsForDpi(SM_CXDRAG, GetDpiForWindow(hwnd)) &&
            dy < GetSystemMetricsForDpi(SM_CYDRAG, GetDpiForWindow(hwnd)) && preview.edit) preview.edit();
        return 0;
    }
    case WM_CAPTURECHANGED: preview.pressing = false; return 0;
    case WM_MOUSEMOVE: {
        preview.mouse = ui::Mouse(hwnd, lparam);
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&track);
        InvalidateRect(hwnd, nullptr, FALSE);
        if (preview.pressing && !preview.dragging && (wparam & MK_LBUTTON)) {
            const int dx = abs(GET_X_LPARAM(lparam) - preview.pressed.x), dy = abs(GET_Y_LPARAM(lparam) - preview.pressed.y);
            if (dx >= GetSystemMetricsForDpi(SM_CXDRAG, GetDpiForWindow(hwnd)) ||
                dy >= GetSystemMetricsForDpi(SM_CYDRAG, GetDpiForWindow(hwnd))) {
                preview.pressing = false; ReleaseCapture(); preview.dragging = true;
                if (preview.drag) preview.drag();
                preview.dragging = false; preview.lastTick = GetTickCount64();
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE: preview.mouse = {-1, -1}; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_TIMER: {
        POINT p{}; GetCursorPos(&p); RECT r{}; GetWindowRect(hwnd, &r);
        const ULONGLONG now = GetTickCount64(), elapsed = now - preview.lastTick;
        preview.lastTick = now;
        if (!preview.dragging && !preview.pressing && !PtInRect(&r, p)) {
            if (elapsed >= preview.remaining) HidePreview();
            else preview.remaining -= elapsed;
        }
        return 0;
    }
    case WM_DPICHANGED: {
        ui::DpiChanged(hwnd, lparam, 320, 294); return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}
}
bool InitPreview(HINSTANCE instance, function<void()> drag, function<void()> edit) {
    WNDCLASSEXW cls{sizeof(cls)};
    cls.hInstance = instance; cls.lpfnWndProc = PreviewProc; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = L"ScreenShotPlus.Preview";
    WNDCLASSEXW existing{sizeof(existing)};
    if (!GetClassInfoExW(instance, cls.lpszClassName, &existing) && !RegisterClassExW(&cls)) return false;
    preview.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        cls.lpszClassName, L"ScreenShotPlus 最新截图", WS_POPUP, 0, 0, 320, 294, nullptr, nullptr, instance, nullptr);
    preview.drag = move(drag); preview.edit = move(edit);
    if (preview.window) ui::Corners(preview.window);
    return preview.window != nullptr;
}
void ShowPreview(Bitmap* image, const filesystem::path& file, POINT point, unsigned seconds) {
    if (!preview.window || !image) return;
    preview.image = image; preview.file = file; preview.pressing = false;
    preview.remaining = static_cast<ULONGLONG>(seconds) * 1000; preview.lastTick = GetTickCount64();
    KillTimer(preview.window, 1);
    if (seconds) SetTimer(preview.window, 1, 100, nullptr);
    ui::Place(preview.window, 320, 294, true, point);
    ShowWindow(preview.window, SW_SHOWNOACTIVATE); InvalidateRect(preview.window, nullptr, FALSE);
}
void HidePreview() {
    if (!preview.window) return;
    KillTimer(preview.window, 1); preview.pressing = false;
    if (GetCapture() == preview.window) ReleaseCapture();
    ShowWindow(preview.window, SW_HIDE);
}
bool PreviewVisible() { return preview.window && IsWindowVisible(preview.window); }
void DestroyPreview() {
    if (preview.window) { KillTimer(preview.window, 1); DestroyWindow(preview.window); preview.window = nullptr; }
    preview.image = nullptr; preview.drag = {}; preview.edit = {};
}
