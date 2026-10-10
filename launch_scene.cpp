#include "launch_scene.h"
#include "ui.h"
using namespace std;
using namespace Gdiplus;
namespace {
constexpr float pi = 3.141592654f;
float Smooth(float t) { t = clamp(t, 0.0f, 1.0f); return t * t * (3 - 2 * t); }
BYTE Alpha(float t) { return static_cast<BYTE>(lround(clamp(t, 0.0f, 1.0f) * 255)); }
Color Tint(Color c, float a) { return Color(Alpha(a), c.GetR(), c.GetG(), c.GetB()); }
void CenterText(Graphics& g, const wstring& text, RectF rect, float size, Color color, bool bold = false) {
    Font font(L"Microsoft YaHei UI", size, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    StringFormat format; format.SetAlignment(StringAlignmentCenter); format.SetLineAlignment(StringAlignmentCenter);
    SolidBrush brush(color); g.DrawString(text.c_str(), static_cast<INT>(text.size()), &font, rect, &format, &brush);
}
void Glow(Graphics& g, PointF center, float rx, float ry, Color color, float intensity) {
    GraphicsPath shape; shape.AddEllipse(center.X - rx, center.Y - ry, 2 * rx, 2 * ry);
    PathGradientBrush brush(&shape); brush.SetCenterPoint(center); brush.SetCenterColor(Tint(color, intensity));
    Color edge(0, color.GetR(), color.GetG(), color.GetB()); INT count = 1; brush.SetSurroundColors(&edge, &count);
    g.FillPath(&brush, &shape);
}
void Spark(Graphics& g, PointF p, float radius, Color color, float opacity, bool rays) {
    SolidBrush brush(Tint(color, opacity)); g.FillEllipse(&brush, p.X - radius, p.Y - radius, 2 * radius, 2 * radius);
    if (rays) {
        Pen pen(Tint(color, opacity * .62f), .65f);
        g.DrawLine(&pen, p.X - 4 * radius, p.Y, p.X + 4 * radius, p.Y);
        g.DrawLine(&pen, p.X, p.Y - 4 * radius, p.X, p.Y + 4 * radius);
    }
}
void Signature(Graphics& g, const wchar_t* word, PointF p, float angle, float opacity) {
    if (opacity <= 0) return;
    const auto state = g.Save(); g.TranslateTransform(p.X, p.Y); g.RotateTransform(angle);
    CenterText(g, word, {-34, -5, 68, 10}, 6.8f, Color(Alpha(opacity), 176, 202, 231)); g.Restore(state);
}
}
double LaunchSeconds(unsigned level) { return level >= 4 ? 4.7 : 4.3; }
LaunchFrame LaunchScene(float progress, bool ready) {
    LaunchFrame frame;
    frame.progress = isfinite(progress) ? clamp(progress, 0.0f, 1.0f) : 0;
    if (!ready) frame.progress = min(frame.progress, .43f);
    frame.reveal = Smooth(frame.progress / .17f);
    frame.depart = Smooth((frame.progress - .84f) / .16f);
    frame.voyage = ready ? Smooth((frame.progress - .44f) / .12f) : 0;
    frame.poem = 1 - frame.voyage;
    frame.finished = ready && frame.progress >= 1;
    frame.phase = frame.finished ? 0U : frame.voyage < .5f ? 1U : 2U;
    return frame;
}
void DrawLaunchScene(Graphics& g, const LaunchFrame& f, unsigned level) {
    const float p = f.progress, t = p * static_cast<float>(LaunchSeconds(level)), fade = 1 - f.depart;
    if (fade <= 0) return;
    const bool ultimate = level >= 4;
    const Color blue = ultimate ? Color(255, 122, 113, 255) : ui::accent;
    const Color light = ultimate ? Color(255, 222, 206, 255) : Color(255, 205, 250, 244);
    const float show = f.reveal * fade, cx = 210, cy = 211 - 14 * f.depart;
    SolidBrush veil(Color(Alpha(fade), 16, 21, 32)); g.FillRectangle(&veil, 0.0f, 0.0f, 420.0f, 570.0f);
    const auto clip = g.Save(); ui::ClipRound(g, {0, 0, 420, 570}, 16);
    // A pair of bounded radial fills makes the nebula, rather than stacking
    // full-window alpha layers on every refresh.
    Glow(g, {cx - 36 + 8 * sin(t), cy + 26}, 192, 170, blue, show * .22f);
    Glow(g, {cx + 86, 448 - 30 * f.voyage}, 200, 128, ultimate ? Color(255, 84, 134, 235) : blue, show * .10f);
    for (int i = 0; i < 3; ++i) {
        GraphicsPath mist;
        const float y = 174 + i * 46.0f, drift = 12 * sin(t * .7f + i);
        mist.AddBezier(-34.0f, y + 120 + drift, 83.0f, y - 122, 236.0f, y + 172, 462.0f, y - 81);
        mist.AddBezier(462.0f, y - 81, 242.0f, y + 136, 86.0f, y - 93, -34.0f, y + 136 + drift);
        mist.CloseFigure(); SolidBrush brush(Tint(blue, show * .023f)); g.FillPath(&brush, &mist);
    }
    const int stars = ultimate ? 128 : 94;
    for (int i = 0; i < stars; ++i) {
        // Fixed seeds keep paths stable across frames, refresh rates and DPI.
        const float x0 = static_cast<float>((i * 137 + 29) % 420), y0 = static_cast<float>((i * 193 + 43) % 535) + 15;
        const float depth = .35f + (i % 7) * .09f, twinkle = .4f + .6f * pow(sin(t * (1.2f + depth) + i * 1.71f), 2.0f);
        const float spread = 1 + (1 - f.reveal) * .09f;
        PointF pos(cx + (x0 - cx) * spread + sin(t * .31f + i) * 3 * depth,
            cy + (y0 - cy) * spread - t * depth * 2);
        const float size = i % 13 == 0 ? 1.25f : .45f + (i % 3) * .18f;
        Spark(g, pos, size, light, show * twinkle * (i % 13 ? .36f : .78f), i % 13 == 0);
    }
    // A slow constellation forms around the badge, then opens like an orbit.
    for (int orbit = 0; orbit < (ultimate ? 3 : 2); ++orbit) {
        const auto state = g.Save(); g.TranslateTransform(cx, cy); g.RotateTransform(-28 + orbit * 43.0f);
        const float rx = 99 + orbit * 26.0f, ry = 34 + orbit * 13.0f;
        Pen faint(Tint(blue, show * .14f), .65f); g.DrawEllipse(&faint, -rx, -ry, 2 * rx, 2 * ry);
        const float angle = t * (orbit & 1 ? -.65f : .49f) + orbit * 2.1f;
        for (int k = 12; k >= 0; --k) {
            const float a = angle - k * .036f;
            Spark(g, {cos(a) * rx, sin(a) * ry}, k ? .8f : 1.7f, light, show * (1 - k / 13.0f) * .66f, k == 0);
        }
        g.Restore(state);
    }
    for (int i = 0; i < (ultimate ? 3 : 2); ++i) {
        const float cycle = (p - .17f - i * .23f) / .19f;
        if (cycle <= 0 || cycle >= 1) continue;
        const float opacity = show * sin(cycle * pi), x = -50 + cycle * 520, y = 54 + i * 114.0f + cycle * 112;
        LinearGradientBrush tail(PointF(x - 52, y - 11), PointF(x, y), Tint(blue, 0), Tint(light, opacity * .8f));
        Pen trail(&tail, 1.25f); g.DrawLine(&trail, x - 52, y - 11, x, y);
        Spark(g, {x, y}, 1.4f, light, opacity, true);
        if (i == 1) Signature(g, L"XuanRikka", {x - 34, y - 9}, 12, opacity * .31f);
    }
    // The horizon becomes a star-lit sea with the second line, not a fake
    // loading meter. Readiness is supplied by the application's initialization.
    const float sea = show * f.voyage;
    for (int i = 0; i < 5 && sea > 0; ++i) {
        GraphicsPath wave;
        const float y = 470 + i * 17.0f, bend = sin(t * .55f + i) * 10;
        wave.AddBezier(-10.0f, y + bend, 124.0f, y - 26, 247.0f, y + 24, 436.0f, y - bend);
        Pen stroke(Tint(blue, sea * (.11f - i * .014f)), .75f); g.DrawPath(&stroke, &wave);
    }
    const auto badgeState = g.Save(); g.TranslateTransform(cx, cy);
    const float zoom = .72f + .28f * ui::SpringReveal(clamp(p / .18f, 0.0f, 1.0f));
    g.ScaleTransform(zoom, zoom); g.RotateTransform((1 - f.reveal) * -14);
    Glow(g, {0, 0}, 71, 71, blue, show * (.30f + .04f * sin(t * 2)));
    auto fill = g.Save(); ui::ClipRound(g, {-37, -37, 74, 74}, 23);
    LinearGradientBrush jewel(PointF(-37, -37), PointF(37, 37), Tint(ultimate ? Color(255, 96, 113, 251) : blue, show),
        Tint(ultimate ? Color(255, 187, 108, 255) : Color(255, 71, 163, 208), show));
    g.FillRectangle(&jewel, -37.0f, -37.0f, 74.0f, 74.0f);
    const float sweepX = -125 + fmod(t * 62, 235.0f);
    LinearGradientBrush sweep(PointF(sweepX - 12, -37), PointF(sweepX + 12, 37), Color(0, 255, 255, 255), Color(Alpha(show * .18f), 255, 255, 255));
    g.FillRectangle(&sweep, sweepX - 12, -37.0f, 24.0f, 74.0f); g.Restore(fill);
    ui::Border(g, {-37, -37, 74, 74}, 23, Tint(light, show * .62f));
    Pen corners(Tint(Color(255, 255, 250, 255), show * .97f), 2.4f); corners.SetStartCap(LineCapRound); corners.SetEndCap(LineCapRound);
    for (int i = 0; i < 4; ++i) {
        const float x = i & 1 ? 16.0f : -16.0f, y = i & 2 ? 13.0f : -13.0f;
        const float draw = Smooth((p - .025f * i) / .12f);
        g.DrawLine(&corners, x, y, x + (i & 1 ? -9 : 9) * draw, y); g.DrawLine(&corners, x, y, x, y + (i & 2 ? -8 : 8) * draw);
    }
    const float engraving = show * Smooth((p - .31f) / .025f) * (1 - Smooth((p - .37f) / .025f));
    Signature(g, L"XuanRikka", {0, 29}, 0, engraving * .42f);
    g.Restore(badgeState);
    const float title = Smooth((p - .06f) / .12f), titleY = 290 + 12 * (1 - title) - 10 * f.depart;
    CenterText(g, L"ScreenShotPlus", {40, titleY, 340, 44}, 27, Tint(Color(255, 245, 245, 255), show * title), true);
    CenterText(g, L"截取光影 · 收藏灵感", {60, titleY + 44, 300, 22}, 11, Tint(light, show * title * .65f));
    const float quote = Smooth((p - .12f) / .07f) * show;
    CenterText(g, LaunchPoem, {20, 367 - f.voyage * 12, 380, 40}, 22,
        Tint(Color(255, 239, 239, 255), quote * f.poem));
    CenterText(g, LaunchVoyage, {20, 367 + f.poem * 12, 380, 40}, 23,
        Tint(Color(255, 239, 239, 255), quote * f.voyage), true);
    CenterText(g, f.voyage > .5f ? L"已就绪，向星海出发" : L"星光，正在汇聚", {60, 414, 300, 24}, 11,
        Tint(light, show * title * .58f));
    // One constellation has a small catalogue-like credit, visible only at
    // the moment it lights up. There is no persistent product watermark.
    const float credit = show * Smooth((p - .66f) / .035f) * (1 - Smooth((p - .76f) / .045f));
    Signature(g, L"XuanRikka", {321, 152}, -13, credit * .30f);
    CenterText(g, L"轻触任意位置，进入工作台", {40, 530, 340, 22}, 10,
        Tint(Color(255, 160, 176, 198), show * .65f));
    g.Restore(clip);
}
