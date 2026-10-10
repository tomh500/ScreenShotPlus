#include "preview.h"
#include "ui.h"
#include "visual_effects.h"
using namespace std;
using namespace Gdiplus;
namespace {
struct Preview {
    HWND window = nullptr;
    Bitmap* image = nullptr;
    ui::ImageCache thumbnail;
    filesystem::path file;
    POINT mouse{-1, -1}, pressed{};
    bool pressing = false, dragging = false;
    ULONGLONG remaining = 0, lastTick = 0;
    function<void()> drag, edit, locate;
    wstring dragStatus;
    unsigned corner = 0, size = 1;
    bool captureAnimation = true, dragAnimation = true;
    bool rightClickLocation = true, filename = true, hints = true, hideAfterDrag = false;
} preview;
int viewWidth = 320, viewHeight = 294;
RectF closeButton{278, 12, 28, 28}, imageRect{16, 52, 288, 166};
void Layout() {
    constexpr array<int, 3> widths{280, 320, 400}, heights{270, 294, 338};
    viewWidth = widths[preview.size]; viewHeight = heights[preview.size];
    closeButton.X = static_cast<float>(viewWidth - 42);
    imageRect.Width = static_cast<float>(viewWidth - 32); imageRect.Height = static_cast<float>(viewHeight - 128);
}
void DrawPreview(HWND hwnd, HDC target = nullptr) {
    ui::Paint paint(hwnd, target); Graphics g(paint.buffer); ui::Prepare(g, hwnd);
    const float progress = ui::EntranceProgress(hwnd);
    const float check = clamp((progress - 0.15f) / 0.70f, 0.0f, 1.0f);
    Pen tick(ui::accent, 2.5f); tick.SetStartCap(LineCapRound); tick.SetEndCap(LineCapRound);
    if (check > 0) {
        const float first = min(1.0f, check * 3);
        g.DrawLine(&tick, 17.0f, 25.0f, 17 + first * 4, 25 + first * 4);
        if (check > 1.0f / 3) { const float second = (check - 1.0f / 3) * 1.5f; g.DrawLine(&tick, 21.0f, 29.0f, 21 + second * 9, 29 - second * 10); }
    }
    ui::Text(g, preview.dragStatus.empty() ? L"已保存 PNG" : preview.dragStatus, {40, 17, static_cast<float>(viewWidth - 92), 26}, 14, ui::accent, true);
    ui::Button(g, hwnd, closeButton, L"×", false, ui::Inside(preview.mouse, closeButton));
    ui::Round(g, imageRect, 10, ui::surface);
    const auto motion = ui::AnimateControl(hwnd, imageRect, ui::Inside(preview.mouse, imageRect));
    const float lifted = ui::AnimateValue(hwnd, 10, preview.dragging && preview.dragAnimation ? 1.0f : 0.0f, 0.20);
    const bool rich = preview.captureAnimation && ui::DecorativeMotion(3);
    const float frameReveal = rich ? ui::AnimateValue(hwnd, 11, 1, 0.52) : 1;
    const float phase = 1 - cbrt(max(0.0f, 1 - frameReveal));
    const float bloom = max(0.0f, sin(phase * 3.14159265f));
    if (preview.image) {
        const float scale = min(imageRect.Width / preview.image->GetWidth(), imageRect.Height / preview.image->GetHeight());
        const float w = preview.image->GetWidth() * scale, h = preview.image->GetHeight() * scale;
        const auto saved = g.Save(); ui::ClipRound(g, imageRect, 10);
        const float amount = ui::MotionAmount();
        const float zoom = (1 - 0.08f * amount + 0.08f * amount * ui::SpringReveal(progress)) *
            (1 + motion.hover * 0.012f * amount - motion.press * 0.008f * amount - lifted * 0.025f * amount);
        const float cx = imageRect.X + imageRect.Width / 2, cy = imageRect.Y + imageRect.Height / 2;
        g.TranslateTransform(cx, cy + 10 * amount * (1 - ui::Entrance(hwnd))); g.ScaleTransform(zoom, zoom); g.TranslateTransform(-cx, -cy);
        preview.thumbnail.Draw(g, hwnd, *preview.image, RectF(imageRect.X + (imageRect.Width - w) / 2,
            imageRect.Y + (imageRect.Height - h) / 2, w, h));
        g.Restore(saved);
    }
    if (lifted > 0) ui::Round(g, imageRect, 10, Color(static_cast<BYTE>(lifted * 100), 25, 29, 36));
    ui::Border(g, imageRect, 10, Color(static_cast<BYTE>(70 + motion.hover * 150),
        ui::accent.GetR(), ui::accent.GetG(), ui::accent.GetB()));
    if (rich && frameReveal < 1) {
        // One finite reveal: a soft frame bloom and a light that travels along
        // the four edges. The original thumbnail and hit rectangle stay fixed.
        for (int i = 3; i >= 1; --i) {
            RectF halo = imageRect; halo.Inflate(i * 1.4f, i * 1.4f);
            ui::Border(g, halo, 10 + i * 1.4f, Color(static_cast<BYTE>(bloom * (28 - i * 5)),
                ui::accent.GetR(), ui::accent.GetG(), ui::accent.GetB()));
        }
        const array<PointF, 5> edge{{{imageRect.X + 9, imageRect.Y}, {imageRect.GetRight() - 9, imageRect.Y},
            {imageRect.GetRight() - 9, imageRect.GetBottom()}, {imageRect.X + 9, imageRect.GetBottom()}, {imageRect.X + 9, imageRect.Y}}};
        const float perimeter = 2 * (imageRect.Width - 18 + imageRect.Height), head = phase * (perimeter + 76);
        Pen glint(Color(static_cast<BYTE>(bloom * 200), ui::accent.GetR(), ui::accent.GetG(), ui::accent.GetB()), 1.8f);
        glint.SetStartCap(LineCapRound); glint.SetEndCap(LineCapRound);
        float traversed = 0;
        for (int i = 0; i < 4; ++i) {
            const float dx = edge[i + 1].X - edge[i].X, dy = edge[i + 1].Y - edge[i].Y, length = hypot(dx, dy);
            const float begin = clamp(head - 76 - traversed, 0.0f, length), end = clamp(head - traversed, 0.0f, length);
            if (end > begin) g.DrawLine(&glint, edge[i].X + dx * begin / length, edge[i].Y + dy * begin / length,
                edge[i].X + dx * end / length, edge[i].Y + dy * end / length);
            traversed += length;
        }
        if (!preview.dragging) ui::Text(g, L"XuanRikka", {static_cast<float>(viewWidth - 113),
            viewHeight - 58 + 3 * (1 - frameReveal), 94, 18}, 7.5f,
            Color(static_cast<BYTE>(bloom * 85), ui::accent.GetR(), ui::accent.GetG(), ui::accent.GetB()));
    }
    if (preview.hints) ui::Text(g, preview.dragging ? L"移动到目标位置，松开即可复制" :
        preview.rightClickLocation ? L"单击编辑 · 拖动分享 · 右键定位" : L"单击编辑 · 拖动分享", {16, static_cast<float>(viewHeight - 63), imageRect.Width, 24}, 12);
    if (preview.filename) ui::Text(g, preview.file.filename().wstring(), {16, static_cast<float>(viewHeight - 35), imageRect.Width, 22}, 10, ui::muted);
}
LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ui::MotionMessage(hwnd, msg, wparam, lparam)) return 0;
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
    case WM_RBUTTONUP:
        if (preview.rightClickLocation && !preview.pressing && !preview.dragging &&
            ui::Inside(ui::Mouse(hwnd, lparam), imageRect) && preview.locate) preview.locate();
        return 0;
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
                InvalidateRect(hwnd, nullptr, FALSE); UpdateWindow(hwnd);
                if (preview.drag) preview.drag();
                preview.dragging = false; preview.lastTick = GetTickCount64();
                InvalidateRect(hwnd, nullptr, FALSE);
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
        ui::DpiChanged(hwnd, lparam, viewWidth, viewHeight); return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}
}
bool InitPreview(HINSTANCE instance, function<void()> drag, function<void()> edit, function<void()> locate) {
    WNDCLASSEXW cls{sizeof(cls)};
    cls.hInstance = instance; cls.lpfnWndProc = PreviewProc; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = L"ScreenShotPlus.Preview";
    WNDCLASSEXW existing{sizeof(existing)};
    if (!GetClassInfoExW(instance, cls.lpszClassName, &existing) && !RegisterClassExW(&cls)) return false;
    preview.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        cls.lpszClassName, L"ScreenShotPlus 最新截图", WS_POPUP, 0, 0, 320, 294, nullptr, nullptr, instance, nullptr);
    preview.drag = move(drag); preview.edit = move(edit); preview.locate = move(locate);
    if (preview.window) ui::Corners(preview.window);
    return preview.window != nullptr;
}
void ConfigurePreview(unsigned corner, unsigned size, bool captureAnimation, bool dragAnimation) {
    preview.corner = min(corner, 3u); preview.size = min(size, 2u); preview.captureAnimation = captureAnimation;
    preview.dragAnimation = dragAnimation;
    Layout(); preview.thumbnail.Reset();
}
void ConfigurePreview(const Config& config) {
    ConfigurePreview(config.previewCorner, config.previewSize, config.captureAnimation, config.dragAnimation);
    preview.rightClickLocation = config.rightClickLocation; preview.filename = config.previewFilename;
    preview.hints = config.previewHints; preview.hideAfterDrag = config.hideAfterDrag;
}
void ShowPreview(Bitmap* image, const filesystem::path& file, POINT point, unsigned seconds, RECT origin) {
    if (!preview.window || !image) return;
    CancelVisualEffects(); preview.dragStatus.clear();
    preview.thumbnail.Reset(); preview.image = image; preview.file = file; preview.pressing = false;
    preview.remaining = static_cast<ULONGLONG>(seconds) * 1000; preview.lastTick = GetTickCount64();
    KillTimer(preview.window, 1);
    if (seconds) SetTimer(preview.window, 1, 100, nullptr);
    Layout(); ui::Place(preview.window, viewWidth, viewHeight, true, point, preview.corner);
    ui::RestartMotion(preview.window);
    ShowWindow(preview.window, SW_SHOWNOACTIVATE); InvalidateRect(preview.window, nullptr, FALSE);
    if (preview.captureAnimation && ui::DecorativeMotion(2) && origin.right > origin.left && origin.bottom > origin.top)
        FlyToPreview(*image, origin, PreviewImageScreenRect());
}
RECT PreviewImageScreenRect() {
    if (!preview.window || !preview.image) return {};
    const float scale = min(imageRect.Width / preview.image->GetWidth(), imageRect.Height / preview.image->GetHeight());
    const float w = preview.image->GetWidth() * scale, h = preview.image->GetHeight() * scale, dpi = ui::Scale(preview.window);
    RECT window{}; GetWindowRect(preview.window, &window);
    const LONG x = window.left + static_cast<LONG>(lround((imageRect.X + (imageRect.Width - w) / 2) * dpi)),
        y = window.top + static_cast<LONG>(lround((imageRect.Y + (imageRect.Height - h) / 2) * dpi));
    return {x, y, x + static_cast<LONG>(lround(w * dpi)), y + static_cast<LONG>(lround(h * dpi))};
}
void PreviewDragResult(bool accepted) {
    if (accepted && preview.hideAfterDrag) { HidePreview(); return; }
    preview.dragStatus = accepted ? L"已复制 · 原图保留" : L"已返回 · 原图保留";
    if (preview.window) InvalidateRect(preview.window, nullptr, FALSE);
}
void HidePreview() {
    if (!preview.window) return;
    CancelVisualEffects();
    KillTimer(preview.window, 1); preview.pressing = false;
    if (GetCapture() == preview.window) ReleaseCapture();
    ShowWindow(preview.window, SW_HIDE);
}
bool PreviewVisible() { return preview.window && IsWindowVisible(preview.window); }
void DestroyPreview() {
    CancelVisualEffects();
    if (preview.window) { KillTimer(preview.window, 1); DestroyWindow(preview.window); preview.window = nullptr; }
    preview.thumbnail.Reset(); preview.image = nullptr; preview.drag = {}; preview.edit = {}; preview.locate = {};
}
