#include "settings.h"
#include "motion_selector.h"
#include <cmath>
using namespace std;
using namespace Gdiplus;
namespace {
struct Settings {
    HWND window = nullptr, owner = nullptr;
    Config working, saved;
    function<bool(const Config&, wstring&)> apply;
    POINT mouse{-1, -1}, pressed{};
    bool listening = false, mouseDown = false, sliderDragging = false, shadeDragging = false;
    unsigned tab = 0, focused = 0;
    MotionSelector selector;
    ui::Tween pageReveal;
    wstring notice;
    filesystem::path configFile;
} settings;
const RectF closeButton{452, 16, 28, 28}, folderButton{24, 180, 452, 44},
    hotkeyButton{24, 272, 452, 44}, printButton{24, 336, 452, 44},
    secondsButton{24, 450, 452, 44}, startupButton{24, 542, 452, 44},
    saveButton{24, 746, 218, 44}, cancelButton{254, 746, 222, 44},
    startupMotionButton{24, 486, 452, 44}, captureMotionButton{24, 544, 452, 44},
    dragMotionButton{24, 602, 452, 44}, guidesButton{24, 340, 452, 44}, shadeSliderHit{44, 222, 412, 52};
constexpr array<const wchar_t*, 5> tabNames{L"常规", L"动效", L"外观", L"截图", L"交互"};
const array<RectF, 4> interactionButtons{{{24, 166, 452, 44}, {24, 278, 452, 44},
    {24, 390, 452, 44}, {24, 502, 452, 44}}};
double Now() { return GetTickCount64() / 1000.0; }
void Touch() {
    if (settings.window) ui::MotionRegion(settings.window, nullptr);
}
RectF Tab(unsigned index) { return {24 + 90.4f * index, 76, 90.4f, 32}; }
RectF Choice(unsigned index, unsigned count, float y) {
    const float width = (452.0f - (count - 1) * 8) / count;
    return {24 + (width + 8) * index, y, width, 40};
}
Color Alpha(Color color, BYTE opacity) { return Color(opacity, color.GetR(), color.GetG(), color.GetB()); }
void CenterText(Graphics& g, const wstring& text, RectF rect, float size, Color color, bool bold = false) {
    Font font(L"Microsoft YaHei UI", size, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    StringFormat format; format.SetAlignment(StringAlignmentCenter); format.SetLineAlignment(StringAlignmentCenter);
    SolidBrush brush(color); g.DrawString(text.c_str(), static_cast<INT>(text.size()), &font, rect, &format, &brush);
}
void LivePreview() {
    ui::SetMotionOptions(settings.working.animationLevel, settings.working.animationSpeed);
    ui::SetAccent(settings.working.accentColor);
    if (settings.owner) InvalidateRect(settings.owner, nullptr, FALSE);
}
void Rollback() {
    ui::SetMotionOptions(settings.saved.animationLevel, settings.saved.animationSpeed);
    ui::SetAccent(settings.saved.accentColor);
    if (settings.owner) InvalidateRect(settings.owner, nullptr, FALSE);
}
void SetLevel(unsigned level) {
    level = min(level, 4U); if (settings.working.animationLevel == level) return;
    settings.working.animationLevel = level; settings.selector.Select(level);
    LivePreview(); InvalidateRect(settings.window, nullptr, FALSE);
}
void SetShade(int x) {
    settings.working.captureShade = static_cast<unsigned>(clamp(lround((25 + clamp((x - 62) / 376.0, 0.0, 1.0) * 55) / 5) * 5, 25L, 80L));
    InvalidateRect(settings.window, nullptr, FALSE);
}
void SwitchTab(unsigned tab) {
    if (settings.tab == tab) return;
    settings.tab = tab; settings.listening = false; settings.focused = tab == 1 ? 1 : 0; settings.notice.clear();
    settings.pageReveal.from = settings.pageReveal.target = 0;
    settings.pageReveal.Set(1, Now(), ui::MotionDuration(.20), ui::DecorativeMotion());
    InvalidateRect(settings.window, nullptr, FALSE);
}
RECT SelectorRegion(HWND hwnd) {
    const float scale = ui::Scale(hwnd);
    return {static_cast<LONG>(floor(22 * scale)), static_cast<LONG>(floor(150 * scale)),
        static_cast<LONG>(ceil(478 * scale)), static_cast<LONG>(ceil(386 * scale))};
}
void TouchMotionControls(HWND hwnd) {
    // Keep settled controls in the common animation table while their already
    // painted pixels remain in ui::Paint's persistent window buffer.
    ui::AnimateControl(hwnd, closeButton, ui::Inside(settings.mouse, closeButton));
    ui::AnimateValue(hwnd, 6200, static_cast<float>(settings.tab), .18, static_cast<float>(settings.tab));
    for (unsigned i = 0; i < 3; ++i) {
        const auto rect = Choice(i, 3, 428);
        ui::AnimateControl(hwnd, rect, ui::Inside(settings.mouse, rect), settings.working.animationSpeed == i);
    }
    ui::AnimateControl(hwnd, startupMotionButton, ui::Inside(settings.mouse, startupMotionButton), settings.working.startupAnimation);
    ui::AnimateControl(hwnd, captureMotionButton, ui::Inside(settings.mouse, captureMotionButton), settings.working.captureAnimation);
    ui::AnimateControl(hwnd, dragMotionButton, ui::Inside(settings.mouse, dragMotionButton), settings.working.dragAnimation);
    ui::AnimateControl(hwnd, saveButton, ui::Inside(settings.mouse, saveButton), true);
    ui::AnimateControl(hwnd, cancelButton, ui::Inside(settings.mouse, cancelButton));
}
void DrawGeneral(Graphics& g, HWND hwnd) {
    ui::Text(g, L"保存目录", {24, 116, 452, 24}, 12, ui::muted);
    ui::Round(g, {24, 143, 452, 29}, 8, ui::surface);
    ui::Text(g, settings.working.folder.wstring(), {36, 147, 428, 24}, 11.5f);
    ui::Button(g, hwnd, folderButton, L"选择文件夹…", false, ui::Inside(settings.mouse, folderButton));
    ui::Text(g, L"截图快捷键", {24, 238, 452, 26}, 13, ui::muted);
    ui::Button(g, hwnd, hotkeyButton, settings.listening ? L"请按组合键（Esc 取消）" : HotkeyText(settings.working) + L"   点击修改",
        settings.listening, ui::Inside(settings.mouse, hotkeyButton));
    ui::Toggle(g, hwnd, printButton, L"同时使用 PrintScreen", settings.working.printScreen, ui::Inside(settings.mouse, printButton));
    ui::Text(g, L"若 PrintScreen 仍打开系统截图，请在 Windows 设置中\n关闭“使用 Print Screen 键打开屏幕捕获”。", {24, 390, 452, 44}, 12, ui::muted);
    const auto seconds = settings.working.previewSeconds;
    ui::Button(g, hwnd, secondsButton, seconds ? L"缩略图：" + to_wstring(seconds) + L" 秒后收起   点击切换" :
        L"缩略图：保留至关闭或下一次截图   点击切换", false, ui::Inside(settings.mouse, secondsButton));
    ui::Text(g, L"悬停和拖拽期间暂停倒计时。截图文件始终保留。", {24, 506, 452, 24}, 12, ui::muted);
    ui::Toggle(g, hwnd, startupButton, L"开机自启动（直接进入托盘）", settings.working.startup, ui::Inside(settings.mouse, startupButton));
    ui::Text(g, L"仅对当前用户生效；移动 EXE 后请重新保存设置。", {24, 595, 452, 24}, 12, ui::muted);
    ui::Text(g, L"配置文件：" + settings.configFile.wstring(), {24, 624, 452, 40}, 11, ui::muted);
}
bool DrawMotion(Graphics& g, HWND hwnd) {
    ui::Text(g, L"让每一次操作，都有恰到好处的回应。", {24, 122, 452, 22}, 12, ui::muted);
    const bool continuous = settings.selector.Draw(g, hwnd, ui::Inside(settings.mouse, motionSliderHit), settings.focused == 1);
    ui::Text(g, L"动画节奏", {24, 397, 452, 22}, 12, ui::muted);
    constexpr array<const wchar_t*, 3> speeds{L"敏捷", L"均衡", L"舒缓"};
    for (unsigned i = 0; i < 3; ++i) {
        const auto rect = Choice(i, 3, 428); ui::Button(g, hwnd, rect, speeds[i], settings.working.animationSpeed == i, ui::Inside(settings.mouse, rect));
    }
    ui::Toggle(g, hwnd, startupMotionButton, L"启动动画", settings.working.startupAnimation, ui::Inside(settings.mouse, startupMotionButton));
    ui::Toggle(g, hwnd, captureMotionButton, L"唤醒截图与完成转场", settings.working.captureAnimation, ui::Inside(settings.mouse, captureMotionButton));
    ui::Toggle(g, hwnd, dragMotionButton, L"拖动图片与落点反馈", settings.working.dragAnimation, ui::Inside(settings.mouse, dragMotionButton));
    ui::Text(g, L"极高与终极拥有专属启动演绎。滑块始终丝滑响应。\n系统关闭动画时，应用动效会尊重系统设置。", {24, 660, 452, 40}, 11, ui::muted);
    return continuous;
}
void DrawAppearance(Graphics& g, HWND hwnd) {
    ui::Text(g, L"强调色", {24, 128, 452, 22}, 13, ui::muted);
    ui::Round(g, {24, 158, 452, 130}, 16, Color(255, 33, 39, 49));
    constexpr array<const wchar_t*, 5> colors{L"薄荷", L"晴蓝", L"鸢紫", L"玫瑰", L"流金"};
    for (unsigned i = 0; i < 5; ++i) {
        const RectF hit{34 + 90.0f * i, 175, 72, 89};
        const auto motion = ui::AnimateControl(hwnd, hit, ui::Inside(settings.mouse, hit), settings.working.accentColor == i);
        const float center = hit.X + 36, size = 35 + motion.hover * 3;
        SolidBrush brush(ui::AccentColor(i)); g.FillEllipse(&brush, center - size / 2, 211 - size / 2, size, size);
        Pen ring(Alpha(ui::AccentColor(i), static_cast<BYTE>(motion.selected * 225)), 1.5f);
        g.DrawEllipse(&ring, center - 24, 187.0f, 48.0f, 48.0f);
        if (motion.selected > .01f) {
            Pen check(Color(static_cast<BYTE>(motion.selected * 255), 25, 29, 36), 2.2f); check.SetStartCap(LineCapRound); check.SetEndCap(LineCapRound);
            g.DrawLine(&check, center - 6, 211.0f, center - 1, 216.0f); g.DrawLine(&check, center - 1, 216.0f, center + 7, 206.0f);
        }
        CenterText(g, colors[i], {hit.X, 243, hit.Width, 24}, 12, settings.working.accentColor == i ? ui::accent : ui::muted);
    }
    ui::Round(g, {24, 308, 452, 112}, 16, Color(255, 32, 39, 49));
    ui::Border(g, {24.5f, 308.5f, 451, 111}, 16, Color(255, 58, 71, 85));
    ui::Round(g, {44, 332, 104, 64}, 9, Alpha(ui::accent, 28));
    Pen line(Alpha(ui::accent, 180), 1.5f); g.DrawLine(&line, 58.0f, 382.0f, 84.0f, 358.0f); g.DrawLine(&line, 84.0f, 358.0f, 109.0f, 379.0f); g.DrawLine(&line, 109.0f, 379.0f, 134.0f, 347.0f);
    ui::Text(g, L"你的截图，你的风格", {170, 332, 275, 30}, 16, ui::foreground, true);
    ui::Text(g, L"颜色会即时预览，保存后长期生效。", {170, 373, 275, 24}, 11.5f, ui::muted);
    ui::Text(g, L"缩略图停靠位置", {24, 438, 452, 24}, 12, ui::muted);
    constexpr array<const wchar_t*, 4> corners{L"↘  右下", L"↙  左下", L"↗  右上", L"↖  左上"};
    for (unsigned i = 0; i < 4; ++i) {
        const auto rect = Choice(i % 2, 2, i < 2 ? 468.0f : 516.0f);
        ui::Button(g, hwnd, rect, corners[i], settings.working.previewCorner == i, ui::Inside(settings.mouse, rect));
    }
    ui::Text(g, L"缩略图大小", {24, 577, 452, 22}, 12, ui::muted);
    constexpr array<const wchar_t*, 3> sizes{L"紧凑", L"标准", L"宽阔"};
    for (unsigned i = 0; i < 3; ++i) {
        const auto rect = Choice(i, 3, 607); ui::Button(g, hwnd, rect, sizes[i], settings.working.previewSize == i, ui::Inside(settings.mouse, rect));
    }
    ui::Text(g, L"缩略图保留图片比例；尺寸与位置应用于下一次截图。", {24, 661, 452, 28}, 11, ui::muted);
}
void DrawScreenshot(Graphics& g, HWND hwnd) {
    ui::Text(g, L"把注意力留给想捕获的内容。", {24, 124, 452, 22}, 12, ui::muted);
    ui::Round(g, {24, 158, 452, 158}, 16, Color(255, 33, 39, 49));
    ui::Text(g, L"截图背景暗度", {44, 177, 260, 26}, 14, ui::foreground, true);
    ui::Text(g, to_wstring(settings.working.captureShade) + L"%", {387, 177, 66, 26}, 14, ui::accent, true);
    const float target = (settings.working.captureShade - 25) / 55.0f;
    const float value = ui::AnimateValue(hwnd, 6300, target, .14, target, true), x = 62 + value * 376;
    ui::Round(g, {48, 234, 404, 18}, 9, Color(255, 57, 64, 78));
    ui::Round(g, {48, 234, max(14.0f, x - 48), 18}, 9, ui::accent);
    ui::Round(g, {x - 13, 230, 26, 26}, 13, ui::foreground);
    if (settings.focused == 2) ui::Border(g, {x - 17, 226, 34, 34}, 17, ui::accent);
    ui::Text(g, L"柔和 25%", {48, 271, 184, 24}, 11, ui::muted);
    ui::Text(g, L"专注 80%", {379, 271, 78, 24}, 11, ui::muted);
    ui::Toggle(g, hwnd, guidesButton, L"显示截图准星", settings.working.captureGuides, ui::Inside(settings.mouse, guidesButton));
    ui::Round(g, {24, 408, 452, 230}, 16, Color(255, 31, 38, 47));
    const RectF view{44, 428, 412, 188};
    const auto state = g.Save(); ui::ClipRound(g, view, 10);
    LinearGradientBrush field(PointF(44, 428), PointF(456, 616), Color(255, 64, 104, 126), Color(255, 40, 59, 86)); g.FillRectangle(&field, view);
    ui::Round(g, {68, 447, 248, 128}, 13, Color(255, 71, 115, 124));
    ui::Round(g, {195, 474, 219, 150}, 17, Color(255, 55, 80, 114));
    SolidBrush shade(Color(static_cast<BYTE>(settings.working.captureShade * 255 / 100), 0, 0, 0)); g.FillRectangle(&shade, view);
    LinearGradientBrush selected(PointF(124, 473), PointF(373, 567), Color(255, 76, 127, 138), Color(255, 65, 94, 139));
    g.FillRectangle(&selected, 123.0f, 471.0f, 252.0f, 96.0f);
    ui::Border(g, {123.5f, 471.5f, 251, 95}, 3, ui::accent);
    if (settings.working.captureGuides) {
        Pen guide(Alpha(ui::accent, 150), 1); g.DrawLine(&guide, 103.0f, 471.0f, 143.0f, 471.0f); g.DrawLine(&guide, 123.0f, 451.0f, 123.0f, 491.0f);
    }
    ui::Round(g, {209, 575, 83, 27}, 8, Color(240, 29, 36, 46)); CenterText(g, L"252 × 96", {209, 575, 83, 27}, 10, ui::foreground);
    CenterText(g, L"截图外观预览", {139, 503, 222, 30}, 14, ui::foreground, true); g.Restore(state);
    ui::Text(g, L"背景暗度与辅助线仅改变界面，原始截图像素保持完整。", {24, 661, 452, 28}, 11, ui::muted);
}
void DrawInteraction(Graphics& g, HWND hwnd) {
    ui::Text(g, L"按你的习惯，调整图片预览与分享。", {24, 124, 452, 22}, 12, ui::muted);
    const array<bool, 4> values{settings.working.rightClickLocation, settings.working.previewFilename,
        settings.working.previewHints, settings.working.hideAfterDrag};
    constexpr array<const wchar_t*, 4> titles{L"右键图片打开所在位置", L"显示图片文件名", L"显示图片操作提示", L"分享成功后收起缩略图"};
    constexpr array<const wchar_t*, 4> descriptions{
        L"右键缩略图中的图片，在资源管理器中选中该 PNG。",
        L"在缩略图底部显示已保存图片的文件名。",
        L"显示单击编辑、拖动分享与右键定位的提示。",
        L"接收程序确认复制后自动收起；取消或失败时保留。"};
    for (unsigned i = 0; i < interactionButtons.size(); ++i) {
        const auto rect = interactionButtons[i];
        ui::Toggle(g, hwnd, rect, titles[i], values[i], ui::Inside(settings.mouse, rect));
        ui::Text(g, descriptions[i], {24, rect.Y + 54, 452, 40}, 11.5f, ui::muted);
    }
    ui::Round(g, {24, 614, 452, 72}, 12, ui::surface);
    ui::Text(g, L"保存后应用于预览。收起缩略图始终保留原文件，\n可从操作面板再次显示最新截图。", {40, 628, 420, 44}, 12, ui::muted);
}
void DrawSettings(HWND hwnd, HDC target = nullptr) {
    ui::Paint paint(hwnd, target); Graphics g(paint.buffer);
    const double now = Now(); const RECT region = SelectorRegion(hwnd);
    const bool local = settings.tab == 1 && !settings.pageReveal.Active(now) && ui::Entrance(hwnd) >= 1;
    if (local) ui::MotionRegion(hwnd, &region); else ui::MotionRegion(hwnd, nullptr);
    const auto& dirty = paint.ps.rcPaint;
    if (local && !target && paint.cached && dirty.left >= region.left && dirty.top >= region.top &&
        dirty.right <= region.right && dirty.bottom <= region.bottom) {
        g.SetSmoothingMode(SmoothingModeAntiAlias); g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        const float scale = ui::Scale(hwnd); g.ScaleTransform(scale, scale);
        // Restore just this card's background before painting translucent
        // antialiasing and shadows; its edge must not accumulate old frames.
        LinearGradientBrush wash(PointF(0, 0), PointF(static_cast<float>(paint.width) / scale,
            static_cast<float>(paint.height) / scale), Color(255, 32, 40, 49), ui::background);
        g.FillRectangle(&wash, 22.0f, 150.0f, 456.0f, 236.0f);
        TouchMotionControls(hwnd);
        ui::ContinuousMotion(hwnd, settings.selector.Draw(g, hwnd, ui::Inside(settings.mouse, motionSliderHit), settings.focused == 1));
        return;
    }
    ui::Prepare(g, hwnd);
    ui::Text(g, L"设置", {24, 22, 240, 38}, 25, ui::foreground, true);
    ui::Text(g, L"SCREENSHOTPLUS", {301, 33, 133, 24}, 9.5f, ui::muted);
    ui::Button(g, hwnd, closeButton, L"×", false, ui::Inside(settings.mouse, closeButton));
    ui::Round(g, {24, 76, 452, 32}, 9, Color(255, 31, 37, 46));
    const float tab = ui::AnimateValue(hwnd, 6200, static_cast<float>(settings.tab), .18, static_cast<float>(settings.tab));
    ui::Round(g, {25 + 90.4f * tab, 77, 88.4f, 30}, 8, Color(255, 56, 66, 79));
    for (unsigned i = 0; i < tabNames.size(); ++i) CenterText(g, tabNames[i], Tab(i), 13, settings.tab == i ? ui::accent : ui::muted, settings.tab == i);
    const float reveal = settings.pageReveal.Value(now);
    const auto state = g.Save(); g.TranslateTransform(0, (1 - reveal) * 7 * ui::MotionAmount());
    bool continuous = settings.pageReveal.Active(now);
    if (settings.tab == 0) DrawGeneral(g, hwnd);
    else if (settings.tab == 1) continuous = DrawMotion(g, hwnd) || continuous;
    else if (settings.tab == 2) DrawAppearance(g, hwnd);
    else if (settings.tab == 3) DrawScreenshot(g, hwnd);
    else DrawInteraction(g, hwnd);
    g.Restore(state);
    ui::Text(g, settings.notice, {24, 702, 452, 36}, 11.5f, ui::danger);
    Pen divider(Color(255, 58, 68, 82), 1); g.DrawLine(&divider, 24.0f, 736.0f, 476.0f, 736.0f);
    ui::Button(g, hwnd, saveButton, L"保存设置", true, ui::Inside(settings.mouse, saveButton));
    ui::Button(g, hwnd, cancelButton, L"取消", false, ui::Inside(settings.mouse, cancelButton));
    ui::ContinuousMotion(hwnd, continuous);
}
void Cancel() { HideSettings(); PostMessageW(settings.owner, WM_SHOW_PANEL, 0, 0); }
void HandleClick(POINT down, POINT p) {
    const auto hit = [&](const RectF& r) { return ui::Inside(p, r) && ui::Inside(down, r); };
    if (hit(closeButton) || hit(cancelButton)) { Cancel(); return; }
    if (hit(saveButton)) {
        settings.listening = false;
        if (settings.apply && settings.apply(settings.working, settings.notice)) {
            settings.saved = settings.working; HideSettings(); PostMessageW(settings.owner, WM_SETTINGS_SAVED, 0, 0);
        }
        return;
    }
    for (unsigned i = 0; i < tabNames.size(); ++i) if (hit(Tab(i))) { SwitchTab(i); return; }
    settings.focused = 0;
    if (settings.tab == 0) {
        if (hit(folderButton)) { settings.listening = false; ui::PickFolder(settings.window, settings.working.folder); }
        else if (hit(hotkeyButton)) {
            settings.listening = true; settings.notice = L"支持 Ctrl / Alt / Shift + 字母、数字或 F1–F24。"; SetFocus(settings.window);
        } else if (hit(printButton)) { settings.listening = false; settings.working.printScreen = !settings.working.printScreen; }
        else if (hit(secondsButton)) {
            settings.listening = false; constexpr array<unsigned, 7> choices{0, 5, 10, 15, 30, 60, 120};
            const auto it = find(choices.begin(), choices.end(), settings.working.previewSeconds);
            settings.working.previewSeconds = it == choices.end() || next(it) == choices.end() ? choices[0] : *next(it);
        } else if (hit(startupButton)) { settings.listening = false; settings.working.startup = !settings.working.startup; }
    } else if (settings.tab == 1) {
        for (unsigned i = 0; i < 3; ++i) if (hit(Choice(i, 3, 428))) { settings.working.animationSpeed = i; LivePreview(); }
        if (hit(startupMotionButton)) settings.working.startupAnimation = !settings.working.startupAnimation;
        if (hit(captureMotionButton)) settings.working.captureAnimation = !settings.working.captureAnimation;
        if (hit(dragMotionButton)) settings.working.dragAnimation = !settings.working.dragAnimation;
    } else if (settings.tab == 2) {
        for (unsigned i = 0; i < 5; ++i) if (hit({34 + 90.0f * i, 175, 72, 89})) { settings.working.accentColor = i; LivePreview(); }
        for (unsigned i = 0; i < 4; ++i) if (hit(Choice(i % 2, 2, i < 2 ? 468.0f : 516.0f))) settings.working.previewCorner = i;
        for (unsigned i = 0; i < 3; ++i) if (hit(Choice(i, 3, 607))) settings.working.previewSize = i;
    } else if (settings.tab == 3) {
        if (hit(guidesButton)) settings.working.captureGuides = !settings.working.captureGuides;
    } else if (settings.tab == 4) {
        const array<bool*, 4> values{&settings.working.rightClickLocation, &settings.working.previewFilename,
            &settings.working.previewHints, &settings.working.hideAfterDrag};
        for (unsigned i = 0; i < values.size(); ++i) if (hit(interactionButtons[i])) *values[i] = !*values[i];
    }
}
LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ui::MotionMessage(hwnd, msg, wparam, lparam)) return 0;
    switch (msg) {
    case WM_PAINT: DrawSettings(hwnd); return 0;
    case WM_PRINTCLIENT: DrawSettings(hwnd, reinterpret_cast<HDC>(wparam)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CLOSE: Cancel(); return 0;
    case WM_MOUSEMOVE: {
        Touch();
        settings.mouse = ui::Mouse(hwnd, lparam);
        if (settings.sliderDragging) SetLevel(MotionStop(settings.mouse.x));
        if (settings.shadeDragging) SetShade(settings.mouse.x);
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&track);
        InvalidateRect(hwnd, nullptr, FALSE); return 0;
    }
    case WM_MOUSELEAVE: Touch(); settings.mouse = {-1, -1}; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN:
        Touch();
        settings.pressed = settings.mouse = ui::Mouse(hwnd, lparam); settings.mouseDown = true; SetCapture(hwnd); SetFocus(hwnd);
        if (settings.tab == 1 && ui::Inside(settings.pressed, motionSliderHit)) {
            settings.sliderDragging = true; settings.focused = 1; settings.listening = false; SetLevel(MotionStop(settings.pressed.x));
        } else if (settings.tab == 3 && ui::Inside(settings.pressed, shadeSliderHit)) {
            settings.shadeDragging = true; settings.focused = 2; SetShade(settings.pressed.x);
        }
        return 0;
    case WM_CAPTURECHANGED: Touch(); settings.mouseDown = settings.sliderDragging = settings.shadeDragging = false; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_NCHITTEST: {
        POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; ScreenToClient(hwnd, &p);
        const float s = ui::Scale(hwnd); p = {static_cast<LONG>(p.x / s), static_cast<LONG>(p.y / s)};
        return p.y < 68 && !ui::Inside(p, closeButton) ? HTCAPTION : HTCLIENT;
    }
    case WM_LBUTTONUP: {
        Touch();
        const bool clicked = settings.mouseDown, slider = settings.sliderDragging, shade = settings.shadeDragging;
        const POINT down = settings.pressed, p = ui::Mouse(hwnd, lparam);
        settings.mouseDown = settings.sliderDragging = settings.shadeDragging = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (clicked) {
            if (slider) SetLevel(MotionStop(p.x));
            else if (shade) SetShade(p.x);
            else HandleClick(down, p);
        }
        InvalidateRect(hwnd, nullptr, FALSE); return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        Touch();
        if (wparam == VK_ESCAPE) {
            if (settings.listening) { settings.listening = false; settings.notice.clear(); InvalidateRect(hwnd, nullptr, FALSE); }
            else Cancel();
            return 0;
        }
        if (settings.listening) {
            Config candidate = settings.working; candidate.modifiers = 0;
            if (GetKeyState(VK_CONTROL) & 0x8000) candidate.modifiers |= MOD_CONTROL;
            if (GetKeyState(VK_SHIFT) & 0x8000) candidate.modifiers |= MOD_SHIFT;
            if (GetKeyState(VK_MENU) & 0x8000) candidate.modifiers |= MOD_ALT;
            candidate.key = static_cast<UINT>(wparam);
            if (ValidConfig(candidate) && !(GetKeyState(VK_LWIN) & 0x8000) && !(GetKeyState(VK_RWIN) & 0x8000)) {
                settings.working = candidate; settings.listening = false; settings.notice.clear(); InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (wparam == VK_TAB) {
            const bool reverse = (GetKeyState(VK_SHIFT) & 0x8000) != 0; SwitchTab((settings.tab + (reverse ? 4 : 1)) % 5);
        } else if (settings.tab == 1 && settings.focused == 1) {
            unsigned level = settings.working.animationLevel;
            if (wparam == VK_LEFT || wparam == VK_DOWN) level = level ? level - 1 : 0;
            else if (wparam == VK_RIGHT || wparam == VK_UP) level = min(level + 1, 4U);
            else if (wparam == VK_HOME) level = 0;
            else if (wparam == VK_END) level = 4;
            else return 0;
            SetLevel(level);
        } else if (settings.tab == 3 && settings.focused == 2) {
            int value = static_cast<int>(settings.working.captureShade);
            if (wparam == VK_LEFT || wparam == VK_DOWN) value -= 5;
            else if (wparam == VK_RIGHT || wparam == VK_UP) value += 5;
            else if (wparam == VK_HOME) value = 25;
            else if (wparam == VK_END) value = 80;
            else return 0;
            settings.working.captureShade = static_cast<unsigned>(clamp(value, 25, 80)); InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_DPICHANGED: Touch(); ui::DpiChanged(hwnd, lparam, 500, 812); return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}
}
bool InitSettings(HINSTANCE instance, HWND owner, function<bool(const Config&, wstring&)> apply) {
    WNDCLASSEXW cls{sizeof(cls)}; cls.hInstance = instance; cls.lpfnWndProc = SettingsProc;
    cls.lpszClassName = L"ScreenShotPlus.Settings"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    WNDCLASSEXW existing{sizeof(existing)};
    if (!GetClassInfoExW(instance, cls.lpszClassName, &existing) && !RegisterClassExW(&cls)) return false;
    settings.owner = owner; settings.apply = move(apply);
    settings.window = CreateWindowExW(WS_EX_TOOLWINDOW, cls.lpszClassName, L"ScreenShotPlus 设置", WS_POPUP,
        0, 0, 500, 812, owner, nullptr, instance, nullptr);
    if (settings.window) ui::Corners(settings.window);
    return settings.window != nullptr;
}
void ShowSettings(const Config& config) {
    if (!settings.window) return;
    if (!SettingsVisible()) {
        settings.working = settings.saved = config; settings.listening = false; settings.notice.clear();
        settings.mouse = {-1, -1}; settings.focused = settings.tab = 0; settings.selector.Reset(config.animationLevel);
        settings.pageReveal.from = settings.pageReveal.target = 1; settings.pageReveal.duration = 0;
        Touch();
    }
    ui::Place(settings.window, 500, 812); ShowWindow(settings.window, SW_SHOW);
    SetForegroundWindow(settings.window); SetFocus(settings.window); InvalidateRect(settings.window, nullptr, FALSE);
}
void SetSettingsConfigFile(const filesystem::path& file) {
    settings.configFile = file; if (settings.window) InvalidateRect(settings.window, nullptr, FALSE);
}
void HideSettings() {
    if (!settings.window) return;
    if (SettingsVisible()) Rollback();
    settings.listening = settings.mouseDown = settings.sliderDragging = settings.shadeDragging = false;
    if (GetCapture() == settings.window) ReleaseCapture();
    ui::ContinuousMotion(settings.window, false); ShowWindow(settings.window, SW_HIDE);
}
void RecordSettingsHotkey(const Config& config) {
    if (!SettingsVisible() || !settings.listening) return;
    settings.working.modifiers = config.modifiers; settings.working.key = config.key;
    settings.listening = false; settings.notice.clear(); InvalidateRect(settings.window, nullptr, FALSE);
}
bool SettingsVisible() { return settings.window && IsWindowVisible(settings.window); }
void DestroySettings() {
    if (settings.window) { HideSettings(); DestroyWindow(settings.window); settings.window = nullptr; }
    settings.apply = {};
}
SettingsState GetSettingsState() {
    return {settings.working, settings.tab, settings.sliderDragging, settings.selector.position, settings.selector.CurrentTitle()};
}
