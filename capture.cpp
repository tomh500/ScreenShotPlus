#include "capture.h"
#include "ui.h"
using namespace std;
using namespace Gdiplus;
RECT SelectionRect(POINT a, POINT b, int width, int height) {
    a.x = clamp(a.x, 0L, static_cast<LONG>(width));
    b.x = clamp(b.x, 0L, static_cast<LONG>(width));
    a.y = clamp(a.y, 0L, static_cast<LONG>(height));
    b.y = clamp(b.y, 0L, static_cast<LONG>(height));
    return {min(a.x, b.x), min(a.y, b.y), max(a.x, b.x), max(a.y, b.y)};
}
namespace {
struct Selection {
    HWND window = nullptr;
    HBITMAP handle = nullptr;
    unique_ptr<Bitmap> image;
    RECT desktop{};
    POINT begin{}, end{};
    bool active = false, finishing = false;
    CaptureDone done;
} selection;

void ClearImage() {
    selection.image.reset();
    if (selection.handle) DeleteObject(selection.handle);
    selection.handle = nullptr;
}
void Finish(bool accept, wstring error = {}) {
    if (selection.finishing || !selection.window) return;
    selection.finishing = true;
    unique_ptr<Bitmap> cropped;
    const RECT r = SelectionRect(selection.begin, selection.end,
        selection.desktop.right - selection.desktop.left, selection.desktop.bottom - selection.desktop.top);
    if (accept && r.right > r.left && r.bottom > r.top) {
        cropped.reset(selection.image->Clone(Rect(r.left, r.top, r.right - r.left, r.bottom - r.top), PixelFormat32bppRGB));
        if (!cropped || cropped->GetLastStatus() != Ok) { cropped.reset(); error = L"裁剪图像失败，请重试。"; }
    }
    POINT point{selection.desktop.left + selection.end.x, selection.desktop.top + selection.end.y};
    CaptureDone done = move(selection.done);
    selection.active = false;
    if (GetCapture() == selection.window) ReleaseCapture();
    HWND hwnd = selection.window;
    selection.window = nullptr;
    ShowWindow(hwnd, SW_HIDE);
    DestroyWindow(hwnd);
    ClearImage();
    selection.finishing = false;
    if (done) done(move(cropped), point, move(error));
}
void DrawSelection(HWND hwnd) {
    ui::Paint paint(hwnd); Graphics g(paint.buffer);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.DrawImage(selection.image.get(), 0, 0, paint.width, paint.height);
    SolidBrush shade(Color(155, 0, 0, 0));
    g.FillRectangle(&shade, 0, 0, paint.width, paint.height);
    const RECT r = SelectionRect(selection.begin, selection.end, paint.width, paint.height);
    if (selection.active && r.right > r.left && r.bottom > r.top) {
        const int w = r.right - r.left, h = r.bottom - r.top;
        g.DrawImage(selection.image.get(), Rect(r.left, r.top, w, h), r.left, r.top, w, h, UnitPixel);
        Pen pen(ui::accent, 2); g.DrawRectangle(&pen, r.left, r.top, w - 1, h - 1);
        const float scale = ui::Scale(hwnd);
        const float labelWidth = 220 * scale, labelHeight = 38 * scale;
        const float x = clamp(static_cast<float>(r.left), 0.0f, max(0.0f, paint.width - labelWidth));
        const float y = clamp(static_cast<float>(r.bottom + 10), 0.0f, max(0.0f, paint.height - labelHeight));
        ui::Round(g, {x, y, labelWidth, labelHeight}, 8 * scale, ui::background);
        ui::Text(g, to_wstring(w) + L" × " + to_wstring(h) + L" px", {x + 12 * scale, y + 7 * scale,
            labelWidth - 24 * scale, labelHeight}, 14 * scale, ui::accent);
    } else {
        const float scale = ui::Scale(hwnd);
        ui::Round(g, {20 * scale, 20 * scale, 400 * scale, 52 * scale}, 12 * scale, ui::background);
        ui::Text(g, L"拖动框选区域 · 松开保存 · Esc / 右键取消",
            {36 * scale, 35 * scale, 368 * scale, 28 * scale}, 14 * scale);
    }
}
LRESULT CALLBACK SelectionProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_PAINT:
        if (selection.image) DrawSelection(hwnd);
        else { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps); }
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN:
        selection.begin = selection.end = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        selection.active = true; SetCapture(hwnd); InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_MOUSEMOVE:
        if (selection.active) {
            selection.end = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (selection.active) { selection.end = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; Finish(true); }
        return 0;
    case WM_RBUTTONDOWN: case WM_CLOSE: Finish(false); return 0;
    case WM_KEYDOWN: if (wparam == VK_ESCAPE) Finish(false); return 0;
    case WM_CAPTURECHANGED:
        if (selection.active && !selection.finishing) Finish(false);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE && selection.window == hwnd && !selection.finishing) Finish(false);
        return 0;
    case WM_DISPLAYCHANGE: Finish(false, L"显示器布局发生变化，请重新截图。" ); return 0;
    case WM_DPICHANGED: return 0; // Frozen desktop and selection always use physical pixels.
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}
}
bool BeginSelection(HINSTANCE instance, CaptureDone done, wstring& error) {
    if (selection.window) { error = L"已有截图正在进行。"; return false; }
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width <= 0 || height <= 0) { error = L"无法获取桌面尺寸。"; return false; }
    selection.desktop = {x, y, x + width, y + height};
    HDC screen = GetDC(nullptr), memory = screen ? CreateCompatibleDC(screen) : nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    selection.handle = screen ? CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    bool ok = false;
    if (memory && selection.handle) {
        HGDIOBJ old = SelectObject(memory, selection.handle);
        ok = BitBlt(memory, 0, 0, width, height, screen, x, y, SRCCOPY | CAPTUREBLT) != FALSE;
        GdiFlush(); SelectObject(memory, old);
    }
    if (memory) DeleteDC(memory);
    if (screen) ReleaseDC(nullptr, screen);
    if (ok) selection.image.reset(Bitmap::FromHBITMAP(selection.handle, nullptr));
    if (!ok || !selection.image || selection.image->GetLastStatus() != Ok) {
        ClearImage(); error = L"无法捕获桌面，请检查会话是否锁定。"; return false;
    }
    WNDCLASSEXW cls{sizeof(cls)};
    cls.hInstance = instance; cls.lpfnWndProc = SelectionProc;
    cls.hCursor = LoadCursorW(nullptr, IDC_CROSS); cls.lpszClassName = L"ScreenShotPlus.Selection";
    WNDCLASSEXW existing{sizeof(existing)};
    if (!GetClassInfoExW(instance, cls.lpszClassName, &existing) && !RegisterClassExW(&cls)) {
        ClearImage(); error = L"无法初始化框选窗口。"; return false;
    }
    selection.done = move(done); selection.begin = {}; selection.end = {}; selection.active = false;
    selection.window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, cls.lpszClassName,
        L"ScreenShotPlus 区域截图", WS_POPUP, x, y, width, height, nullptr, nullptr, instance, nullptr);
    if (!selection.window) { ClearImage(); selection.done = {}; error = L"无法创建框选窗口。"; return false; }
    ShowWindow(selection.window, SW_SHOW); SetForegroundWindow(selection.window);
    SetFocus(selection.window); UpdateWindow(selection.window);
    return true;
}
void CancelSelection() { Finish(false); }
