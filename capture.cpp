#include "capture.h"
#include "ui.h"
#include "visual_effects.h"
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
    HBITMAP dimmed = nullptr;
    HDC frozenDC = nullptr, dimDC = nullptr;
    HGDIOBJ frozenOld = nullptr, dimOld = nullptr;
    unique_ptr<Bitmap> image;
    RECT desktop{};
    RECT lastCapture{};
    POINT begin{}, end{};
    bool active = false, finishing = false;
    bool layered = false;
    int alpha = -1;
    RECT chrome{};
    POINT paintedPointer{};
    CaptureDone done;
} selection;
unsigned captureShade = 60;
bool captureGuides = true, captureAnimation = true;

RectF HintRect(float scale) {
    POINT point{selection.end.x + selection.desktop.left, selection.end.y + selection.desktop.top};
    MONITORINFO info{sizeof(info)}; GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &info);
    const float width = max(100.0f, min(464 * scale, static_cast<float>(info.rcWork.right - info.rcWork.left) - 24 * scale));
    return {info.rcWork.left - selection.desktop.left + (info.rcWork.right - info.rcWork.left - width) / 2,
        info.rcWork.top - selection.desktop.top + 24 * scale, width, 66 * scale};
}
RECT ChromeRect(HWND hwnd) {
    const float s = ui::Scale(hwnd);
    if (!selection.active) {
        const auto hint = HintRect(s);
        return {static_cast<LONG>(floor(hint.X - 8 * s)), static_cast<LONG>(floor(hint.Y - 24 * s)),
            static_cast<LONG>(ceil(hint.GetRight() + 8 * s)), static_cast<LONG>(ceil(hint.GetBottom() + 8 * s))};
    }
    RECT r = SelectionRect(selection.begin, selection.end, selection.desktop.right - selection.desktop.left,
        selection.desktop.bottom - selection.desktop.top);
    const int desktopWidth = selection.desktop.right - selection.desktop.left, desktopHeight = selection.desktop.bottom - selection.desktop.top;
    const float labelWidth = 252 * s, labelHeight = 42 * s;
    const float x = clamp(static_cast<float>(r.left), 0.0f, max(0.0f, desktopWidth - labelWidth));
    RECT label{};
    // The pill can change sides while its 8 px reveal offset settles near
    // the bottom edge. Include both endpoints to erase and repaint both sides.
    for (const float offset : {0.0f, 8.0f}) {
        float y = r.bottom + (10 + offset) * s;
        if (y + labelHeight > desktopHeight) y = r.top - labelHeight - 10 * s;
        y = clamp(y, 0.0f, max(0.0f, desktopHeight - labelHeight));
        RECT endpoint{static_cast<LONG>(floor(x - 8 * s)), static_cast<LONG>(floor(y - 8 * s)),
            static_cast<LONG>(ceil(x + labelWidth + 8 * s)), static_cast<LONG>(ceil(y + labelHeight + 8 * s))};
        RECT combined{}; UnionRect(&combined, &label, &endpoint); label = combined;
    }
    const LONG margin = static_cast<LONG>(ceil(8 * s)); InflateRect(&r, margin, margin);
    RECT result{}; UnionRect(&result, &r, &label); return result;
}
void InvalidateSelection(HWND hwnd) {
    const RECT next = ChromeRect(hwnd);
    if (selection.active || !EqualRect(&selection.chrome, &next)) {
        InvalidateRect(hwnd, &selection.chrome, FALSE); InvalidateRect(hwnd, &next, FALSE);
    }
    if (captureGuides) {
        const LONG gap = static_cast<LONG>(ceil(30 * ui::Scale(hwnd)));
        RECT old{selection.paintedPointer.x - gap, selection.paintedPointer.y - gap,
            selection.paintedPointer.x + gap, selection.paintedPointer.y + gap};
        RECT current{selection.end.x - gap, selection.end.y - gap, selection.end.x + gap, selection.end.y + gap};
        InvalidateRect(hwnd, &old, FALSE); InvalidateRect(hwnd, &current, FALSE);
    }
}

void ClearImage() {
    selection.image.reset();
    if (selection.frozenOld && selection.frozenOld != HGDI_ERROR) SelectObject(selection.frozenDC, selection.frozenOld);
    if (selection.dimOld && selection.dimOld != HGDI_ERROR) SelectObject(selection.dimDC, selection.dimOld);
    if (selection.frozenDC) DeleteDC(selection.frozenDC);
    if (selection.dimDC) DeleteDC(selection.dimDC);
    if (selection.dimmed) DeleteObject(selection.dimmed);
    if (selection.handle) DeleteObject(selection.handle);
    selection.handle = selection.dimmed = nullptr;
    selection.frozenDC = selection.dimDC = nullptr; selection.frozenOld = selection.dimOld = nullptr;
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
    selection.lastCapture = cropped ? RECT{selection.desktop.left + r.left, selection.desktop.top + r.top,
        selection.desktop.left + r.right, selection.desktop.top + r.bottom} : RECT{};
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
void DrawSelection(HWND hwnd, HDC target = nullptr) {
    ui::Paint paint(hwnd, target, false);
    const float entrance = captureAnimation ? ui::Entrance(hwnd) : 1, scale = ui::Scale(hwnd);
    if (selection.layered) {
        const int alpha = clamp(static_cast<int>(lround(entrance * 255)), 1, 255);
        if (selection.alpha != alpha) {
            if (SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(alpha), LWA_ALPHA)) selection.alpha = alpha;
            else {
                selection.layered = false; selection.alpha = 255;
                SetWindowLongPtrW(hwnd, GWL_EXSTYLE, GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & ~WS_EX_LAYERED);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
    }
    // DWM composites the cached desktop fade. CPU work is bounded by the HUD
    // and dirty selection area rather than blending the whole desktop each frame.
    const RECT dirty = paint.printing ? RECT{0, 0, paint.width, paint.height} : paint.ps.rcPaint;
    BitBlt(paint.buffer, dirty.left, dirty.top, dirty.right - dirty.left, dirty.bottom - dirty.top,
        selection.dimDC, dirty.left, dirty.top, SRCCOPY);
    const RECT r = SelectionRect(selection.begin, selection.end, paint.width, paint.height);
    const int w = r.right - r.left, h = r.bottom - r.top;
    RECT crop{};
    if (selection.active && w > 0 && h > 0 && IntersectRect(&crop, &r, &dirty))
        BitBlt(paint.buffer, crop.left, crop.top, crop.right - crop.left, crop.bottom - crop.top,
            selection.frozenDC, crop.left, crop.top, SRCCOPY);
    Graphics g(paint.buffer);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.SetClip(Rect(dirty.left, dirty.top, dirty.right - dirty.left, dirty.bottom - dirty.top));
    const float reveal = captureAnimation ? ui::AnimateValue(hwnd, 1, selection.active ? 1.0f : 0.0f, 0.18) : (selection.active ? 1.0f : 0.0f);
    if (selection.active && w > 0 && h > 0) {
        const RectF bounds{static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(w - 1), static_cast<float>(h - 1)};
        Pen halo(Color(40, ui::accent.GetR(), ui::accent.GetG(), ui::accent.GetB()), 6 * scale);
        Pen outline(Color(200, ui::accent.GetR(), ui::accent.GetG(), ui::accent.GetB()), max(1.0f, scale));
        g.DrawRectangle(&halo, bounds); g.DrawRectangle(&outline, bounds);
        const float length = min({(8 + 10 * reveal) * scale, w * 0.35f, h * 0.35f});
        Pen corner(ui::accent, 2.5f * scale); corner.SetStartCap(LineCapRound); corner.SetEndCap(LineCapRound);
        for (int i = 0; i < 4; ++i) {
            const float x = i & 1 ? bounds.GetRight() : bounds.X, y = i & 2 ? bounds.GetBottom() : bounds.Y;
            g.DrawLine(&corner, x, y, x + (i & 1 ? -length : length), y);
            g.DrawLine(&corner, x, y, x, y + (i & 2 ? -length : length));
        }
        const float labelWidth = 252 * scale, labelHeight = 42 * scale;
        const float x = clamp(static_cast<float>(r.left), 0.0f, max(0.0f, paint.width - labelWidth));
        float y = r.bottom + (10 + (1 - reveal) * 8) * scale;
        if (y + labelHeight > paint.height) y = r.top - labelHeight - 10 * scale;
        y = clamp(y, 0.0f, max(0.0f, paint.height - labelHeight));
        ui::Round(g, {x, y + 2 * scale, labelWidth, labelHeight}, 12 * scale, Color(50, 0, 0, 0));
        ui::Round(g, {x, y, labelWidth, labelHeight}, 12 * scale, ui::background);
        ui::Border(g, {x, y, labelWidth, labelHeight}, 12 * scale, Color(130, ui::accent.GetR(), ui::accent.GetG(), ui::accent.GetB()));
        ui::Text(g, to_wstring(w) + L" × " + to_wstring(h) + L" px", {x + 14 * scale, y + 10 * scale,
            170 * scale, labelHeight}, 14 * scale, ui::accent, true);
        ui::Text(g, L"松开保存", {x + 172 * scale, y + 12 * scale, 72 * scale, 24 * scale}, 11 * scale, ui::muted);
    } else {
        const auto hint = HintRect(scale);
        const float width = hint.Width, x = hint.X, y = hint.Y - 14 * ui::MotionAmount() * (1 - entrance) * scale;
        const auto saved = g.Save();
        g.TranslateTransform(x + width / 2, y + 33 * scale);
        const float amount = captureAnimation ? ui::MotionAmount() : 0;
        const float spring = 1 - 0.06f * amount + 0.06f * amount * ui::SpringReveal(ui::EntranceProgress(hwnd));
        g.ScaleTransform(spring, spring); g.TranslateTransform(-width / 2, -33 * scale);
        ui::Round(g, {0, 3 * scale, width, 66 * scale}, 18 * scale, Color(60, 0, 0, 0));
        ui::Round(g, {0, 0, width, 66 * scale}, 18 * scale, ui::background);
        ui::Border(g, {0, 0, width, 66 * scale}, 18 * scale, Color(130, ui::accent.GetR(), ui::accent.GetG(), ui::accent.GetB()));
        ui::Round(g, {16 * scale, 17 * scale, 32 * scale, 32 * scale}, 9 * scale, ui::accent);
        ui::Text(g, L"＋", {19 * scale, 18 * scale, 30 * scale, 28 * scale}, 21 * scale, ui::background, true);
        ui::Text(g, L"选择你的截图区域", {62 * scale, 11 * scale, width - 74 * scale, 27 * scale}, 16 * scale, ui::foreground, true);
        ui::Text(g, L"拖动框选 · 松开保存 · Esc / 右键取消", {62 * scale, 37 * scale, width - 74 * scale, 23 * scale}, 11 * scale, ui::muted);
        g.Restore(saved);
        if (captureGuides) {
            const float cx = static_cast<float>(selection.end.x), cy = static_cast<float>(selection.end.y);
            Pen guide(Color(110, ui::accent.GetR(), ui::accent.GetG(), ui::accent.GetB()), max(1.0f, scale));
            g.DrawLine(&guide, cx - 20 * scale, cy, cx - 11 * scale, cy);
            g.DrawLine(&guide, cx + 11 * scale, cy, cx + 20 * scale, cy);
            g.DrawLine(&guide, cx, cy - 20 * scale, cx, cy - 11 * scale);
            g.DrawLine(&guide, cx, cy + 11 * scale, cx, cy + 20 * scale);
            Pen ring(Color(180, ui::accent.GetR(), ui::accent.GetG(), ui::accent.GetB()), 1.5f * scale);
            g.DrawEllipse(&ring, cx - 8 * scale, cy - 8 * scale, 16 * scale, 16 * scale);
        }
    }
    selection.chrome = ChromeRect(hwnd); selection.paintedPointer = selection.end;
    ui::MotionRegion(hwnd, &selection.chrome);
}
LRESULT CALLBACK SelectionProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ui::MotionMessage(hwnd, msg, wparam, lparam)) return 0;
    switch (msg) {
    case WM_PAINT:
        if (selection.image) DrawSelection(hwnd);
        else { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps); }
        return 0;
    case WM_PRINTCLIENT: if (selection.image) DrawSelection(hwnd, reinterpret_cast<HDC>(wparam)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN:
        selection.begin = selection.end = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        selection.active = true; SetCapture(hwnd); InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_MOUSEMOVE:
        selection.end = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ui::MotionMonitor(hwnd, MonitorFromPoint({selection.desktop.left + selection.end.x, selection.desktop.top + selection.end.y}, MONITOR_DEFAULTTONEAREST));
        InvalidateSelection(hwnd);
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
void ConfigureCapture(unsigned shade, bool guides, bool animate) {
    captureShade = clamp(shade, 25u, 80u); captureGuides = guides; captureAnimation = animate;
}
bool BeginSelection(HINSTANCE instance, CaptureDone done, wstring& error) {
    if (selection.window) { error = L"已有截图正在进行。"; return false; }
    CancelVisualEffects(); DwmFlush();
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
    // Precompute the dimmed desktop once; entry frames only blend two native
    // cached surfaces. Crop pixels always come from the untouched image.
    selection.frozenDC = CreateCompatibleDC(nullptr); selection.dimDC = CreateCompatibleDC(nullptr);
    selection.dimmed = CreateBitmap(width, height, 1, 32, nullptr);
    if (selection.frozenDC && selection.dimDC && selection.dimmed) {
        selection.frozenOld = SelectObject(selection.frozenDC, selection.handle);
        selection.dimOld = SelectObject(selection.dimDC, selection.dimmed);
    }
    if (!selection.frozenOld || !selection.dimOld || selection.frozenOld == HGDI_ERROR || selection.dimOld == HGDI_ERROR ||
        !BitBlt(selection.dimDC, 0, 0, width, height, selection.frozenDC, 0, 0, SRCCOPY)) {
        ClearImage(); error = L"无法准备截图绘图缓冲，请重试。"; return false;
    }
    { Graphics dim(selection.dimDC); SolidBrush shade(Color(static_cast<BYTE>(captureShade * 255 / 100), 0, 0, 0)); dim.FillRectangle(&shade, 0, 0, width, height); }
    WNDCLASSEXW cls{sizeof(cls)};
    cls.hInstance = instance; cls.lpfnWndProc = SelectionProc;
    cls.hCursor = LoadCursorW(nullptr, IDC_CROSS); cls.lpszClassName = L"ScreenShotPlus.Selection";
    WNDCLASSEXW existing{sizeof(existing)};
    if (!GetClassInfoExW(instance, cls.lpszClassName, &existing) && !RegisterClassExW(&cls)) {
        ClearImage(); error = L"无法初始化框选窗口。"; return false;
    }
    selection.done = move(done); selection.begin = {}; selection.active = false; selection.lastCapture = {};
    GetCursorPos(&selection.end); selection.end.x -= x; selection.end.y -= y;
    selection.layered = captureAnimation && ui::DecorativeMotion(); selection.alpha = -1; selection.chrome = {};
    selection.window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | (selection.layered ? WS_EX_LAYERED : 0), cls.lpszClassName,
        L"ScreenShotPlus 区域截图", WS_POPUP, x, y, width, height, nullptr, nullptr, instance, nullptr);
    if (!selection.window) { ClearImage(); selection.done = {}; error = L"无法创建框选窗口。"; return false; }
    if (selection.layered && !SetLayeredWindowAttributes(selection.window, 0, 1, LWA_ALPHA)) {
        selection.layered = false;
        SetWindowLongPtrW(selection.window, GWL_EXSTYLE, GetWindowLongPtrW(selection.window, GWL_EXSTYLE) & ~WS_EX_LAYERED);
    }
    ShowWindow(selection.window, SW_SHOW);
    ui::RestartMotion(selection.window, captureAnimation ? 0.26 : 0);
    SetForegroundWindow(selection.window);
    POINT cursor{}; GetCursorPos(&cursor); ui::MotionMonitor(selection.window, MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST));
    SetFocus(selection.window); UpdateWindow(selection.window);
    return true;
}
void CancelSelection() { Finish(false); }
RECT LastCaptureRect() { return selection.lastCapture; }
