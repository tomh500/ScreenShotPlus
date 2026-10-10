#include "launch_scene.h"
#include "ui.h"
using namespace std;
using namespace Gdiplus;
namespace {
constexpr float pi = 3.141592654f;
float Smooth(float t) { t = clamp(t, 0.0f, 1.0f); return t * t * (3 - 2 * t); }
BYTE Alpha(float t) { return static_cast<BYTE>(lround(clamp(t, 0.0f, 1.0f) * 255)); }
Color Tint(Color c, float a) { return Color(Alpha(a), c.GetR(), c.GetG(), c.GetB()); }
Color Blend(Color a, Color b, float t) {
    const auto channel = [t](BYTE x, BYTE y) { return static_cast<BYTE>(lround(x + (y - x) * t)); };
    return Color(255, channel(a.GetR(), b.GetR()), channel(a.GetG(), b.GetG()), channel(a.GetB(), b.GetB()));
}
Color Starlight() { return Blend(ui::accent, Color(255, 255, 255, 255), .72f); }
Color DeepAccent() { return Blend(ui::accent, Color(255, 19, 30, 51), .36f); }
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
void Constellation(Graphics& g, PointF center, float scale, float opacity, float time) {
    // Two connected letter constellations, rather than text stamped on images.
    const array<PointF, 13> points{{{-33, -15}, {-9, 15}, {-21, 0}, {-9, -15}, {-33, 15},
        {7, 15}, {7, -15}, {24, -15}, {33, -7}, {24, 0}, {7, 0}, {20, 0}, {34, 15}}};
    constexpr array<pair<int, int>, 12> links{{{0, 2}, {2, 1}, {3, 2}, {2, 4}, {5, 6},
        {6, 7}, {7, 8}, {8, 9}, {9, 10}, {10, 6}, {11, 12}, {10, 11}}};
    const auto state = g.Save(); g.TranslateTransform(center.X, center.Y); g.ScaleTransform(scale, scale);
    Pen thread(Tint(ui::accent, opacity * .45f), .75f);
    for (const auto [a, b] : links) g.DrawLine(&thread, points[a], points[b]);
    for (size_t i = 0; i < points.size(); ++i)
        Spark(g, points[i], i % 3 ? 1.0f : 1.5f, Starlight(), opacity * (.65f + .35f * pow(sin(time + i), 2.0f)), i % 3 == 0);
    g.Restore(state);
}
void Aurora(Graphics& g, float time, float show) {
    // Narrow ribbons leave the quotes clear and keep per-frame drawing bounded.
    for (int i = 0; i < 7; ++i) {
        GraphicsPath ribbon;
        const float x = 22 + i * 14.0f, drift = sin(time * .48f + i * .32f) * 18;
        ribbon.AddBezier(x - 38, 50.0f, x + 185 + drift, 100.0f, x - 90, 220.0f, x + 335, 285.0f);
        Pen pen(Tint(i % 2 ? Starlight() : ui::accent, show * (.025f + i * .003f)), 6.0f - i * .5f);
        g.DrawPath(&pen, &ribbon);
    }
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
    const Color blue = ui::accent;
    const Color light = ultimate ? Starlight() : Color(255, 205, 250, 244);
    const float show = f.reveal * fade, cx = 210, cy = 211 - 14 * f.depart;
    SolidBrush veil(Color(Alpha(fade), 16, 21, 32)); g.FillRectangle(&veil, 0.0f, 0.0f, 420.0f, 570.0f);
    const auto clip = g.Save(); ui::ClipRound(g, {0, 0, 420, 570}, 16);
    // A pair of bounded radial fills makes the nebula, rather than stacking
    // full-window alpha layers on every refresh.
    Glow(g, {cx - 36 + 8 * sin(t), cy + 26}, 192, 170, blue, show * .22f);
    Glow(g, {cx + 86, 448 - 30 * f.voyage}, 200, 128, ultimate ? DeepAccent() : blue, show * .10f);
    if (ultimate) {
        Aurora(g, t, show);
        const float formed = show * Smooth((p - .20f) / .13f);
        Constellation(g, {321, 132}, .68f, formed * .65f, t);
        if (f.secret) {
            Glow(g, {cx, cy}, 167, 151, blue, show * .18f);
            Constellation(g, {cx, 472}, 1.24f, show, t);
        }
    }
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
    if (ultimate) {
        const auto state = g.Save(); g.TranslateTransform(cx, cy); g.RotateTransform(t * 9);
        for (int i = 0; i < 48; ++i) {
            const float angle = i * 2 * pi / 48, r = 78 + 3 * sin(t + i * .3f);
            Pen tick(Tint(i % 4 ? blue : light, show * (i % 4 ? .18f : .57f)), i % 4 ? .7f : 1.2f);
            g.DrawLine(&tick, cos(angle) * r, sin(angle) * r, cos(angle) * (r + (i % 4 ? 2 : 5)), sin(angle) * (r + (i % 4 ? 2 : 5)));
        }
        g.Restore(state);
        const float pulse = fmod(t * .45f, 1.0f), radius = 82 + pulse * 72;
        Pen ripple(Tint(blue, show * (1 - pulse) * .13f), .8f);
        g.DrawEllipse(&ripple, cx - radius, cy - radius, radius * 2, radius * 2);
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
    LinearGradientBrush jewel(PointF(-37, -37), PointF(37, 37), Tint(ultimate ? Blend(blue, light, .22f) : blue, show),
        Tint(ultimate ? DeepAccent() : Color(255, 71, 163, 208), show));
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
    CenterText(g, ultimate && f.secret ? L"XuanRikka · 星海频道已连接" : L"截取光影 · 收藏灵感", {60, titleY + 44, 300, 22}, 11, Tint(light, show * title * .65f));
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
    CenterText(g, ultimate ? L"轻触徽标寻星 · 其他位置进入工作台" : L"轻触任意位置，进入工作台", {40, 530, 340, 22}, 10,
        Tint(Color(255, 160, 176, 198), show * .65f));
    g.Restore(clip);
}
void DrawExitScene(Graphics& g, float progress, bool secret) {
    const float p = isfinite(progress) ? clamp(progress, 0.0f, 1.0f) : 0;
    const float appear = Smooth(p / .20f), collapse = Smooth((p - .35f) / .65f);
    const Color light = Starlight();
    SolidBrush background(Color(Alpha(Smooth(p / .14f)), 16, 21, 32)); g.FillRectangle(&background, 0.0f, 0.0f, 420.0f, 570.0f);
    const auto clip = g.Save(); ui::ClipRound(g, {0, 0, 420, 570}, 16);
    Glow(g, {210, 223}, 160 * (1 - collapse) + 8, 140 * (1 - collapse) + 8, ui::accent, appear * (1 - collapse) * .35f);
    for (int i = 0; i < 72; ++i) {
        const float a = i * 2.399963f + p * .32f, radius = (48 + (i * 31 % 154)) * (1 - collapse);
        Spark(g, {210 + cos(a) * radius, 223 + sin(a) * radius * .83f}, .55f + (i % 3) * .22f,
            light, appear * (1 - collapse) * (.35f + (i % 5) * .10f), i % 11 == 0);
    }
    Constellation(g, {210, 223}, 1 - collapse * .8f, appear * (1 - collapse), p * 5);
    for (int i = 0; i < 3; ++i) {
        const float radius = (74 + i * 30.0f) * (1 - collapse);
        Pen ring(Tint(ui::accent, appear * (1 - collapse) * .23f), .8f);
        g.DrawEllipse(&ring, 210 - radius, 223 - radius * .38f, radius * 2, radius * .76f);
    }
    const float words = appear * (1 - Smooth((p - .78f) / .22f));
    CenterText(g, L"星光暂歇，灵感长存", {30, 328, 360, 40}, 23, Tint(light, words), true);
    CenterText(g, secret ? L"XuanRikka：下次见，星海旅人" : L"XuanRikka · 愿每一帧都值得珍藏", {30, 380, 360, 26}, 11, Tint(light, words * .70f));
    CenterText(g, L"轻触或按 Esc 立即退出", {40, 530, 340, 22}, 10, Tint(light, words * .45f));
    g.Restore(clip);
}
