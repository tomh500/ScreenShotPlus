#include "motion_selector.h"
#include <cmath>
using namespace std;
using namespace Gdiplus;
namespace {
constexpr array<const wchar_t*, 5> names{L"轻度", L"中", L"高", L"极高", L"终极"};
constexpr array<const wchar_t*, 5> descriptions{
    L"安静、直接。保留必要的状态反馈。", L"轻盈的淡入与滑动，日常使用更从容。",
    L"层次、回弹与跟随，流畅而有分寸。", L"星轨、流光与专属启动，让每一步更鲜活。",
    L"完整的星光动效，专属启动与细节演绎。"};
double Now() { return GetTickCount64() / 1000.0; }
Color Mix(Color a, Color b, float t) {
    const auto channel = [t](BYTE x, BYTE y) { return static_cast<BYTE>(clamp(lround(x + (y - x) * t), 0L, 255L)); };
    return Color(channel(a.GetA(), b.GetA()), channel(a.GetR(), b.GetR()), channel(a.GetG(), b.GetG()), channel(a.GetB(), b.GetB()));
}
Color Alpha(Color c, float alpha) { return Color(static_cast<BYTE>(clamp(alpha, 0.0f, 1.0f) * c.GetA()), c.GetR(), c.GetG(), c.GetB()); }
void CenterText(Graphics& g, const wstring& text, RectF r, float size, Color color, bool bold = false) {
    Font font(L"Microsoft YaHei UI", size, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    StringFormat format; format.SetAlignment(StringAlignmentCenter); format.SetLineAlignment(StringAlignmentCenter);
    format.SetFormatFlags(StringFormatFlagsNoWrap); SolidBrush brush(color);
    g.DrawString(text.c_str(), static_cast<INT>(text.size()), &font, r, &format, &brush);
}
float Smooth(float t) { t = clamp(t, 0.0f, 1.0f); return 1 - (1 - t) * (1 - t) * (1 - t); }
void AccentStarlight(Graphics& g, RectF r, double now, float opacity) {
    const auto state = g.Save(); g.SetClip(r, CombineModeIntersect);
    const float sweep = r.X - 65 + static_cast<float>(fmod(now * .22, 1.0)) * (r.Width + 130);
    const RectF streak{sweep - 48, r.Y, 96, r.Height};
    array<Color, 3> light{Color(0, 255, 255, 255), Color(static_cast<BYTE>(opacity * 85), 255, 255, 255), Color(0, 255, 255, 255)};
    const array<REAL, 3> stops{0, .5f, 1};
    LinearGradientBrush sheen(PointF(streak.X, streak.Y), PointF(streak.GetRight(), streak.Y), light.front(), light.back());
    sheen.SetInterpolationColors(light.data(), stops.data(), 3); g.FillRectangle(&sheen, streak);
    for (unsigned i = 0; i < 17; ++i) {
        const double phase = fmod(i * .61803398875 + now * (.025 + (i % 3) * .009), 1.0);
        const float x = r.X + static_cast<float>(phase) * r.Width;
        const float y = r.Y + 5 + static_cast<float>((i * 17 + 3) % 21);
        const float twinkle = static_cast<float>(.35 + .65 * pow((sin(now * 1.8 + i * 2.71) + 1) / 2, 2));
        const float diameter = i % 6 == 0 ? 2.1f : 1.25f;
        SolidBrush glow(Color(static_cast<BYTE>(opacity * twinkle * 22), 255, 255, 255));
        g.FillEllipse(&glow, x - diameter * 2, y - diameter * 2, diameter * 4, diameter * 4);
        SolidBrush dot(Color(static_cast<BYTE>(opacity * twinkle * 210), 255, 255, 255));
        g.FillEllipse(&dot, x - diameter / 2, y - diameter / 2, diameter, diameter);
        if (i % 6 == 0) {
            Pen ray(Color(static_cast<BYTE>(opacity * twinkle * 130), 255, 255, 255), .6f);
            g.DrawLine(&ray, x - 2.7f, y, x + 2.7f, y); g.DrawLine(&ray, x, y - 2.7f, x, y + 2.7f);
        }
    }
    g.Restore(state);
}
void AccentOrbit(Graphics& g, float x, double now, float opacity) {
    const float angle = static_cast<float>(fmod(now * 39, 360.0));
    Pen trail(Alpha(ui::accent, opacity * .42f), 1.1f); trail.SetStartCap(LineCapRound); trail.SetEndCap(LineCapRound);
    g.DrawArc(&trail, x - 24, 265.0f, 48.0f, 48.0f, angle, 77.0f);
    Pen back(Alpha(ui::accent, opacity * .14f), .8f); g.DrawArc(&back, x - 27, 262.0f, 54.0f, 54.0f, angle + 174, 113.0f);
    for (unsigned i = 0; i < 2; ++i) {
        const double phase = now * .68 + i * 3.141592653589793;
        const float radius = i ? 27.0f : 24.0f;
        const float px = x + radius * static_cast<float>(cos(phase)), py = 289 + radius * static_cast<float>(sin(phase));
        SolidBrush glow(Alpha(ui::accent, opacity * .12f)); g.FillEllipse(&glow, px - 4, py - 4, 8.0f, 8.0f);
        SolidBrush star(Alpha(Mix(ui::accent, ui::foreground, .55f), opacity * .95f)); g.FillEllipse(&star, px - 1.45f, py - 1.45f, 2.9f, 2.9f);
    }
}
void Premium(Graphics& g, RectF r, double now, float opacity) {
    const auto state = g.Save(); ui::ClipRound(g, r, 15);
    const float wave = static_cast<float>((sin(now * 0.65) + 1) / 2);
    array<Color, 5> colors{Color(255, 43, 59, 226), Color(255, 75, 62, 245),
        Mix(Color(255, 136, 86, 255), Color(255, 166, 113, 255), wave), Color(255, 188, 132, 255), Color(255, 112, 62, 233)};
    for (auto& c : colors) c = Alpha(c, opacity);
    const array<REAL, 5> positions{0, .23f, .5f, .74f, 1};
    LinearGradientBrush gradient(PointF(r.X, r.Y), PointF(r.GetRight(), r.GetBottom()), colors.front(), colors.back());
    gradient.SetInterpolationColors(colors.data(), positions.data(), 5); g.FillRectangle(&gradient, r);
    LinearGradientBrush shine(PointF(r.X, r.Y), PointF(r.X, r.GetBottom()), Color(static_cast<BYTE>(opacity * 55), 255, 255, 255), Color(0, 255, 255, 255));
    g.FillRectangle(&shine, r);
    for (unsigned i = 0; i < 29; ++i) {
        const double phase = fmod(i * .61803398875 + now * (.018 + (i % 3) * .008), 1.0);
        const float x = r.X + static_cast<float>(phase) * r.Width;
        const float y = r.Y + 5 + static_cast<float>((i * 37 + 11) % 21);
        const float twinkle = static_cast<float>(.48 + .52 * pow((sin(now * 1.4 + i * 2.13) + 1) / 2, 2));
        const float diameter = i % 7 == 0 ? 2.6f : i % 3 == 0 ? 1.8f : 1.2f;
        SolidBrush halo(Color(static_cast<BYTE>(opacity * twinkle * 24), 236, 223, 255));
        g.FillEllipse(&halo, x - diameter * 1.7f, y - diameter * 1.7f, diameter * 3.4f, diameter * 3.4f);
        SolidBrush star(Color(static_cast<BYTE>(opacity * twinkle * 220), 251, 244, 255));
        g.FillEllipse(&star, x - diameter / 2, y - diameter / 2, diameter, diameter);
        if (i % 9 == 0) { Pen ray(Color(static_cast<BYTE>(opacity * twinkle * 100), 255, 250, 255), .65f); g.DrawLine(&ray, x - 3, y, x + 3, y); g.DrawLine(&ray, x, y - 3, x, y + 3); }
    }
    g.Restore(state);
}
}
unsigned MotionStop(int logicalX) { return static_cast<unsigned>(clamp(lround((logicalX - 62) / 94.0), 0L, 4L)); }
void MotionSelector::Reset(unsigned value) {
    level = min(value, 4U); position = static_cast<float>(level);
    previousTitle = title = names[level]; labelTime = ultimateTime = engravingTime = 0; wasHovered = false;
}
wstring MotionSelector::CurrentTitle() {
    const double now = Now();
    if (ultimateTime && now >= ultimateTime) { previousTitle = title; title = L"终极"; labelTime = now; ultimateTime = 0; }
    return title;
}
void MotionSelector::Select(unsigned value) {
    value = min(value, 4U); if (value == level) return;
    CurrentTitle(); previousTitle = title; level = value;
    title = value == 4 ? L"XuanRikka" : names[value];
    labelTime = Now(); ultimateTime = value == 4 ? labelTime + .38 : 0;
}
bool MotionSelector::Draw(Graphics& g, HWND hwnd, bool hovered, bool focused) {
    CurrentTitle(); const double now = Now();
    if (level < 3) engravingTime = 0;
    else if (hovered && !wasHovered) engravingTime = now;
    wasHovered = hovered;
    const float premium = ui::AnimateValue(hwnd, 6001, level == 4 ? 1.0f : 0.0f, .30, level == 4 ? 1.0f : 0.0f, true);
    const float stellar = ui::AnimateValue(hwnd, 6003, level == 3 ? 1.0f : 0.0f, .28, level == 3 ? 1.0f : 0.0f, true);
    position = ui::AnimateValue(hwnd, 6000, static_cast<float>(level), .22, position, true);
    const float hover = ui::AnimateValue(hwnd, 6002, hovered ? 1.0f : 0.0f, .16, 0, true);
    ui::Round(g, {24, 152, 452, 232}, 18, Color(255, 33, 39, 49));
    ui::Border(g, {24.5f, 152.5f, 451, 231}, 18, Mix(Mix(Color(255, 64, 73, 88), ui::accent, stellar * .25f), Color(255, 126, 102, 200), premium * .48f));
    ui::Text(g, L"动画强度", {44, 170, 250, 24}, 13, ui::muted);
    const auto info = ui::GetMotionInfo(hwnd);
    ui::Round(g, {378, 168, 76, 25}, 12, Color(255, 44, 51, 65));
    CenterText(g, to_wstring(static_cast<unsigned>(lround(info.refreshRate))) + L" Hz", {378, 168, 76, 25}, 11, ui::muted);
    const float textProgress = labelTime ? Smooth(static_cast<float>((now - labelTime) / .18)) : 1;
    const Color titleColor = Mix(ui::accent, Color(255, 185, 146, 255), premium);
    const auto textState = g.Save(); g.SetClip(RectF(52, 199, 396, 37), CombineModeIntersect);
    if (textProgress < 1) CenterText(g, previousTitle, {52, 200 - textProgress * 10, 396, 32}, 22, Alpha(titleColor, 1 - textProgress), true);
    CenterText(g, title, {52, 200 + (1 - textProgress) * 10, 396, 32}, title == L"XuanRikka" ? 21.0f : 23.0f, Alpha(titleColor, textProgress), true);
    g.Restore(textState);
    CenterText(g, level == 4 ? L"ULTIMATE · 星光随行" : level == 3 ? L"STELLAR MOTION · 流光与星轨" : L"FLUENT MOTION · 连贯每一步",
        {44, 237, 412, 24}, 10.5f, Mix(Mix(ui::muted, ui::accent, stellar * .25f), Color(255, 185, 169, 225), premium));
    const RectF track{48, 274, 404, 30};
    ui::Round(g, track, 15, Color(255, 57, 64, 78));
    const float knob = 62 + position * 94;
    const auto clip = g.Save(); ui::ClipRound(g, track, 15);
    ui::Round(g, {48, 274, max(14.0f, knob - 48), 30}, 15, ui::accent);
    if (stellar > .001f) AccentStarlight(g, {48, 274, max(14.0f, knob - 48), 30}, now, stellar);
    if (premium > .001f) Premium(g, {48, 274, max(30.0f, knob - 48 + 15), 30}, now, premium);
    for (unsigned i = 0; i < 5; ++i) {
        const float x = 62 + 94.0f * i;
        SolidBrush dot(i <= position ? Color(85, 255, 255, 255) : Color(255, 117, 126, 143));
        g.FillEllipse(&dot, x - 2, 287.0f, 4.0f, 4.0f);
    }
    const float engravingAge = engravingTime ? static_cast<float>(now - engravingTime) : 2;
    const float engraving = hover * Smooth(engravingAge / .16f) * (1 - Smooth((engravingAge - .55f) / .50f));
    if (engraving > .001f) CenterText(g, L"XuanRikka", {78, 281, 70, 15}, 8.5f,
        Alpha(level == 4 ? ui::foreground : Color(255, 24, 45, 44), engraving * .48f));
    g.Restore(clip);
    if (stellar > .001f) AccentOrbit(g, knob, now, stellar);
    for (int i = 3; i >= 1; --i) {
        const float spread = static_cast<float>(i);
        ui::Round(g, {knob - 16 - spread, 277 - spread, 32 + spread * 2, 32 + spread * 2}, 18 + spread,
            Color(static_cast<BYTE>(5 + hover * 2), 0, 0, 0));
    }
    const float diameter = 32 + hover;
    ui::Round(g, {knob - diameter / 2, 289 - diameter / 2, diameter, diameter}, diameter / 2, Color(255, 251, 253, 255));
    ui::Border(g, {knob - diameter / 2 + .5f, 289 - diameter / 2 + .5f, diameter - 1, diameter - 1}, diameter / 2,
        Mix(Color(255, 201, 209, 221), Color(255, 218, 192, 255), premium));
    if (focused) ui::Border(g, {knob - 20, 269, 40, 40}, 20, Mix(ui::accent, Color(255, 181, 141, 255), premium));
    for (unsigned i = 0; i < 5; ++i) CenterText(g, names[i], {30 + 94.0f * i, 318, 64, 26}, 12,
        level == i ? (i == 4 ? Color(255, 192, 160, 255) : ui::accent) : ui::muted, level == i);
    CenterText(g, descriptions[level], {40, 351, 420, 22}, 11.5f, ui::muted);
    return level >= 3 || premium > .001f || stellar > .001f || ultimateTime || textProgress < 1 || engraving > .001f;
}
