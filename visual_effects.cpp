#include "visual_effects.h"
#include "ui.h"
using namespace std;
using namespace Gdiplus;
namespace {
constexpr wchar_t effectClass[] = L"ScreenShotPlus.FloatingImage";
constexpr UINT closeEffect = WM_APP + 71;
double Now() { return chrono::duration<double>(chrono::steady_clock::now().time_since_epoch()).count(); }
struct LayerSurface {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old = nullptr;
    void* bits = nullptr;
    int width = 0, height = 0;
    ~LayerSurface() { Clear(); }
    void Clear() {
        if (old && old != HGDI_ERROR) SelectObject(dc, old);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        dc = nullptr; bitmap = nullptr; old = nullptr; bits = nullptr;
    }
    bool Resize(int w, int h) {
        if (bitmap && w <= width && h <= height) return true;
        Clear(); width = w; height = h;
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w; info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        dc = CreateCompatibleDC(nullptr);
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (dc && bitmap) old = SelectObject(dc, bitmap);
        if (!old || old == HGDI_ERROR || !bits) { Clear(); return false; }
        return true;
    }
};
struct Effect {
    enum Mode { Landing, Dragging, Returning, Dropped } mode = Landing;
    HWND window = nullptr;
    unique_ptr<Bitmap> image;
    LayerSurface layer;
    RectF origin, destination, current, release;
    float scale = 1, tilt = 0, fade = 1;
    double lastTime = 0, createdTime = 0;
    POINT pointer{};
    DWORD feedback = DROPEFFECT_NONE, error = 0;
    UINT_PTR cookie = 0;
    bool presented = false;
} effect;
RectF FloatRect(RECT r) { return {static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right - r.left), static_cast<float>(r.bottom - r.top)}; }
RectF Fit(RectF r, float width, float height) {
    const float fit = min({1.0f, width / max(1.0f, r.Width), height / max(1.0f, r.Height)});
    const float w = r.Width * fit, h = r.Height * fit;
    return {r.X + (r.Width - w) / 2, r.Y + (r.Height - h) / 2, w, h};
}
RectF Interpolate(RectF a, RectF b, float t) {
    return {a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t, a.Width + (b.Width - a.Width) * t, a.Height + (b.Height - a.Height) * t};
}
void Render(HWND hwnd, HDC print = nullptr) {
    ui::BeginMotionPaint(hwnd);
    const double now = Now(), delta = clamp(now - effect.lastTime, 0.0, 0.05);
    effect.lastTime = now;
    const float progress = ui::EntranceProgress(hwnd), eased = ui::Entrance(hwnd);
    const bool animated = ui::GetMotionInfo(hwnd).enabled;
    float opacity = 1;
    if (effect.mode == Effect::Dragging) {
        POINT pointer = effect.pointer; GetCursorPos(&pointer);
        const float width = effect.destination.Width, height = effect.destination.Height;
        RectF target{pointer.x + 24 * effect.scale, pointer.y + 20 * effect.scale, width, height};
        if (progress < 1) effect.current = Interpolate(effect.origin, target, eased);
        else {
            // Time-based following, a short soft lag rather than a fixed
            // number of pixels per frame. Reduced motion follows directly.
            const float follow = animated && ui::MotionLevel() >= 2 ? static_cast<float>(1 - exp(-55 * delta)) : 1;
            effect.current = Interpolate(effect.current, target, follow);
        }
        const float desiredTilt = animated && ui::MotionLevel() >= 2 ?
            clamp((pointer.x - effect.pointer.x) * 0.16f * ui::MotionAmount(), -3.5f * ui::MotionAmount(), 3.5f * ui::MotionAmount()) : 0;
        effect.tilt += (desiredTilt - effect.tilt) * static_cast<float>(1 - exp(-18 * delta));
        effect.pointer = pointer;
    } else if (effect.mode == Effect::Landing) {
        effect.current = Interpolate(effect.origin, effect.destination, eased);
        effect.current.Y -= sin(progress * 3.14159265f) * 34 * effect.scale * ui::MotionAmount();
        opacity = clamp((1 - progress) / 0.24f, 0.0f, 1.0f);
    } else if (effect.mode == Effect::Returning) {
        effect.current = Interpolate(effect.release, effect.origin, eased);
        effect.tilt *= static_cast<float>(exp(-20 * delta));
        opacity = clamp((1 - progress) / 0.30f, 0.0f, 1.0f) * effect.fade;
    } else {
        const float shrink = 1 - 0.16f * eased;
        const auto& r = effect.release;
        effect.current = {r.X + r.Width * (1 - shrink) / 2, r.Y + r.Height * (1 - shrink) / 2 - 8 * eased * effect.scale,
            r.Width * shrink, r.Height * shrink};
        opacity = (1 - eased) * effect.fade;
    }
    if (effect.mode != Effect::Dragging && progress >= 1) {
        ui::ContinuousMotion(hwnd, false); ui::EndMotionPaint(hwnd);
        PostMessageW(hwnd, closeEffect, effect.cookie, 0); return;
    }
    const float s = effect.scale, padding = 32 * s;
    const int width = static_cast<int>(ceil(effect.current.Width + padding * 2)),
        height = static_cast<int>(ceil(effect.current.Height + padding * 2 + 38 * s));
    if (!effect.layer.Resize(max(width, static_cast<int>(ceil(484 * s))), max(height, static_cast<int>(ceil(352 * s))))) {
        effect.error = GetLastError(); ui::ContinuousMotion(hwnd, false); ui::EndMotionPaint(hwnd);
        PostMessageW(hwnd, closeEffect, effect.cookie, 0); return;
    }
    // Drawing directly into a PARGB DIB keeps transparent corners and shadow.
    Bitmap bitmap(width, height, effect.layer.width * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(effect.layer.bits));
    {
        Graphics g(&bitmap); g.Clear(Color(0, 0, 0, 0));
        g.SetSmoothingMode(SmoothingModeAntiAlias); g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        const float w = effect.current.Width, h = effect.current.Height;
        g.TranslateTransform(padding + w / 2, padding + h / 2);
        g.RotateTransform(effect.tilt); g.TranslateTransform(-w / 2, -h / 2);
        for (int i = 12; i >= 1; --i) {
            const float blur = i * s;
            ui::Round(g, {-blur, -blur + 5 * s, w + 2 * blur, h + 2 * blur}, 14 * s + blur,
                Color(static_cast<BYTE>(i < 4 ? 10 : 4), 0, 0, 0));
        }
        ui::Round(g, {-3 * s, -3 * s, w + 6 * s, h + 6 * s}, 14 * s, ui::surface);
        const auto saved = g.Save(); ui::ClipRound(g, {0, 0, w, h}, 11 * s);
        g.SetInterpolationMode(InterpolationModeBilinear); g.DrawImage(effect.image.get(), RectF(0, 0, w, h));
        g.Restore(saved);
        const float accepted = ui::AnimateValue(hwnd, 1, effect.feedback & DROPEFFECT_COPY ? 1.0f : 0.0f, 0.14);
        const float lifted = animated && ui::MotionLevel() >= 3 ? ui::AnimateValue(hwnd, 2, 1, .34) : 1;
        const float bloom = sin((1 - cbrt(max(0.0f, 1 - lifted))) * 3.14159265f);
        ui::Border(g, {-3 * s, -3 * s, w + 6 * s, h + 6 * s}, 14 * s,
            Color(220, static_cast<BYTE>(92 + (ui::accent.GetR() - 92) * accepted),
                static_cast<BYTE>(106 + (ui::accent.GetG() - 106) * accepted), static_cast<BYTE>(122 + (ui::accent.GetB() - 122) * accepted)));
        if (animated && ui::MotionLevel() >= 3) {
            for (int i = 2; i >= 1; --i) {
                const float margin = (3 + i * 2.4f) * s;
                ui::Border(g, {-margin, -margin, w + 2 * margin, h + 2 * margin}, 14 * s + margin,
                    Color(static_cast<BYTE>((bloom * .55f + accepted * .28f) * (22 - i * 5)), ui::accent.GetR(), ui::accent.GetG(), ui::accent.GetB()));
            }
            const float shine = static_cast<float>(fmod(now * 0.42, 1.0));
            Pen rim(Color(110, ui::accent.GetR(), ui::accent.GetG(), ui::accent.GetB()), 1.4f * s);
            g.DrawLine(&rim, w * shine - 24 * s, -3 * s, min(w, w * shine + 24 * s), -3 * s);
            for (int i = 0; i < 5; ++i) {
                const float phase = static_cast<float>(fmod(now * .16 + i * .213, 1.0)), twinkle = sin(phase * 3.14159265f);
                SolidBrush star(Color(static_cast<BYTE>(twinkle * (80 + accepted * 65)), ui::accent.GetR(), ui::accent.GetG(), ui::accent.GetB()));
                g.FillEllipse(&star, phase * w, -9 * s - (i % 2) * 5 * s, 1.6f * s, 1.6f * s);
            }
        }
        if (animated && ui::MotionLevel() == 4) {
            const Color highlight(255, static_cast<BYTE>((ui::accent.GetR() + 255) / 2),
                static_cast<BYTE>((ui::accent.GetG() + 255) / 2), static_cast<BYTE>((ui::accent.GetB() + 255) / 2));
            LinearGradientBrush premium(PointF(0, 0), PointF(w, 0), Color(220, ui::accent.GetR(), ui::accent.GetG(), ui::accent.GetB()),
                Color(230, highlight.GetR(), highlight.GetG(), highlight.GetB()));
            Pen ribbon(&premium, 2.0f * s); g.DrawLine(&ribbon, 10 * s, -3 * s, max(10 * s, w - 10 * s), -3 * s);
            for (int i = 0; i < 10; ++i) {
                const float phase = static_cast<float>(fmod(now * 0.25 + i * 0.173, 1.0));
                const float x = phase * w, y = -11 * s - (i % 3) * 4 * s;
                const float light = sin(phase * 3.14159265f);
                SolidBrush star(Color(static_cast<BYTE>(light * 180), highlight.GetR(), highlight.GetG(), highlight.GetB()));
                g.FillEllipse(&star, x, y, (i % 3 ? 1.6f : 2.6f) * s, (i % 3 ? 1.6f : 2.6f) * s);
            }
        }
        if (effect.mode == Effect::Dragging) {
            const float badgeWidth = min(188 * s, w + 24 * s);
            const RectF badge{max(0.0f, (w - badgeWidth) / 2), h + 10 * s, badgeWidth, 30 * s};
            ui::Round(g, badge, 15 * s, ui::background);
            ui::Border(g, badge, 15 * s, effect.feedback & DROPEFFECT_COPY ? ui::accent : Color(255, 90, 101, 118));
            ui::Text(g, effect.feedback & DROPEFFECT_COPY ? L"＋  松开即可复制" : L"选择可接收的位置", {badge.X + 12 * s, badge.Y + 6 * s,
                badge.Width - 20 * s, 22 * s}, 11 * s, effect.feedback & DROPEFFECT_COPY ? ui::accent : ui::muted, true);
            const float age = static_cast<float>(now - effect.createdTime), hidden = clamp((age - .8f) / .22f, 0.0f, 1.0f) * clamp((1.9f - age) / .55f, 0.0f, 1.0f);
            if (animated && ui::MotionLevel() >= 3 && hidden > 0) ui::Text(g, L"XuanRikka", {max(0.0f, w - 76 * s), h + 42 * s, 74 * s, 12 * s},
                7.0f * s, Color(static_cast<BYTE>(hidden * 80), ui::accent.GetR(), ui::accent.GetG(), ui::accent.GetB()));
        }
        g.Flush(FlushIntentionSync);
    }
    POINT destination{static_cast<LONG>(lround(effect.current.X - padding)), static_cast<LONG>(lround(effect.current.Y - padding))}, source{};
    SIZE size{width, height};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(clamp(lround(opacity * 255), 0L, 255L)), AC_SRC_ALPHA};
    if (UpdateLayeredWindow(hwnd, nullptr, &destination, &size, effect.layer.dc, &source, 0, &blend, ULW_ALPHA)) effect.presented = true;
    else { effect.error = GetLastError(); PostMessageW(hwnd, closeEffect, effect.cookie, 0); }
    if (print) BitBlt(print, 0, 0, width, height, effect.layer.dc, 0, 0, SRCCOPY);
    ui::EndMotionPaint(hwnd);
}
LRESULT CALLBACK EffectProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ui::MotionMessage(hwnd, message, wparam, lparam)) return 0;
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT ps{}; BeginPaint(hwnd, &ps); Render(hwnd); EndPaint(hwnd, &ps); return 0;
    }
    case WM_PRINTCLIENT: Render(hwnd, reinterpret_cast<HDC>(wparam)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_DPICHANGED: {
        const float next = static_cast<float>(HIWORD(wparam)) / 96;
        if (effect.mode == Effect::Dragging) { effect.destination.Width *= next / effect.scale; effect.destination.Height *= next / effect.scale; }
        effect.scale = next; return 0;
    }
    case WM_DISPLAYCHANGE: CancelVisualEffects(); return 0;
    case closeEffect: if (effect.window == hwnd && wparam == effect.cookie) CancelVisualEffects(); return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
bool Start(Bitmap& image, RECT origin, RECT destination, Effect::Mode mode) {
    CancelVisualEffects();
    if (!ui::DecorativeMotion(mode == Effect::Landing ? 2 : 1)) return false;
    if (image.GetWidth() == 0 || image.GetHeight() == 0 || origin.right <= origin.left || origin.bottom <= origin.top) return false;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW cls{sizeof(cls)}; cls.lpfnWndProc = EffectProc; cls.hInstance = instance; cls.lpszClassName = effectClass;
    WNDCLASSEXW existing{sizeof(existing)};
    if (!GetClassInfoExW(instance, effectClass, &existing) && !RegisterClassExW(&cls)) return false;
    effect.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        effectClass, L"ScreenShotPlus 图片动效", WS_POPUP | WS_DISABLED, origin.left, origin.top, 1, 1, nullptr, nullptr, instance, nullptr);
    if (!effect.window) return false;
    ++effect.cookie;
    effect.scale = ui::Scale(effect.window);
    const float s = effect.scale;
    effect.mode = mode; effect.origin = Fit(FloatRect(origin), 420 * s, 250 * s); effect.current = effect.origin;
    effect.destination = FloatRect(destination);
    if (mode == Effect::Dragging) {
        const float fit = min(210 * s / image.GetWidth(), 146 * s / image.GetHeight());
        effect.destination.Width = max(44 * s, image.GetWidth() * fit); effect.destination.Height = max(30 * s, image.GetHeight() * fit);
        // Keep the original aspect ratio, including very wide/tall screenshots.
        const float ratio = static_cast<float>(image.GetWidth()) / image.GetHeight();
        if (ratio > 1) effect.destination.Height = effect.destination.Width / ratio;
        else effect.destination.Width = effect.destination.Height * ratio;
    }
    const float fit = min(420 * s / image.GetWidth(), 300 * s / image.GetHeight());
    const int w = max(1, static_cast<int>(ceil(image.GetWidth() * fit))), h = max(1, static_cast<int>(ceil(image.GetHeight() * fit)));
    effect.image = make_unique<Bitmap>(w, h, PixelFormat32bppPARGB);
    if (effect.image->GetLastStatus() != Ok) { CancelVisualEffects(); return false; }
    bool copied = false;
    {
        Graphics cache(effect.image.get()); cache.Clear(Color(0, 0, 0, 0));
        cache.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        copied = cache.DrawImage(&image, 0, 0, w, h) == Ok;
    }
    if (!copied) { CancelVisualEffects(); return false; }
    effect.tilt = 0; effect.fade = 1; effect.feedback = DROPEFFECT_NONE; effect.presented = false; effect.error = 0;
    effect.lastTime = effect.createdTime = Now(); GetCursorPos(&effect.pointer);
    ShowWindow(effect.window, SW_SHOWNOACTIVATE); ui::RestartMotion(effect.window, mode == Effect::Landing ? 0.36 : 0.18);
    ui::ContinuousMotion(effect.window, true); UpdateWindow(effect.window);
    return true;
}
}
bool FlyToPreview(Bitmap& image, RECT origin, RECT destination) { return Start(image, origin, destination, Effect::Landing); }
bool BeginDragVisual(Bitmap& image, RECT origin) { return Start(image, origin, {}, Effect::Dragging); }
void DragVisualFeedback(DWORD feedback) {
    if (!effect.window || effect.mode != Effect::Dragging) return;
    effect.feedback = feedback; InvalidateRect(effect.window, nullptr, FALSE);
}
void EndDragVisual(bool accepted) {
    if (!effect.window || effect.mode != Effect::Dragging) return;
    effect.release = effect.current;
    effect.fade = ui::Entrance(effect.window);
    effect.mode = accepted ? Effect::Dropped : Effect::Returning;
    ui::RestartMotion(effect.window, accepted ? 0.16 : 0.26); ui::ContinuousMotion(effect.window, true);
    InvalidateRect(effect.window, nullptr, FALSE);
}
void CancelVisualEffects() {
    if (effect.window) { HWND hwnd = effect.window; effect.window = nullptr; DestroyWindow(hwnd); }
    effect.image.reset(); effect.layer.Clear();
}
VisualEffectInfo GetVisualEffectInfo() {
    return {effect.window, effect.window && effect.mode == Effect::Dragging, effect.window && effect.mode == Effect::Returning, effect.presented, effect.feedback, effect.error};
}
