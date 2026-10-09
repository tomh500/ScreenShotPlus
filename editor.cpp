#include "editor.h"
#include "annotation.h"
#include "ui.h"
#include <imm.h>
using namespace std;
using namespace Gdiplus;
namespace {
constexpr int width = 1040, height = 760;
const RectF closeButton{988, 18, 28, 28}, penButton{24, 76, 100, 40}, textButton{134, 76, 100, 40},
    smallerButton{250, 76, 40, 40}, largerButton{422, 76, 40, 40}, undoButton{478, 76, 108, 40},
    commitButton{596, 76, 146, 40}, rgbButton{838, 124, 178, 40},
    canvas{24, 180, 992, 414}, input{24, 612, 992, 54},
    cancelButton{694, 706, 112, 38}, saveButton{818, 706, 198, 38},
    palette{700, 176, 316, 214};
const array<Color, 7> colors{Color(255, 255, 0, 0), Color(255, 255, 128, 0), Color(255, 255, 235, 0),
    Color(255, 0, 180, 0), Color(255, 0, 220, 220), Color(255, 0, 96, 255), Color(255, 160, 64, 255)};
const array<wstring, 7> colorNames{L"赤", L"橙", L"黄", L"绿", L"青", L"蓝", L"紫"};
struct Editor {
    HWND window = nullptr;
    unique_ptr<Bitmap> source;
    vector<Annotation> annotations;
    vector<vector<Annotation>> history;
    Annotation live, draft;
    Annotation::Kind tool = Annotation::Stroke;
    Color color = colors[0];
    float penSize = 4, textSize = 28;
    int editing = -1, slider = -1, channel = 0;
    bool textActive = false, drawing = false, pressing = false, rgb = false, composing = false;
    POINT mouse{-1, -1}, pressed{};
    wstring notice;
    function<bool(unique_ptr<Bitmap>&, wstring&)> save;
    function<void()> cancel;
} editor;
RectF Swatch(int i) { return {24.0f + i * 76, 124, 66, 40}; }
RectF Slider(int i) { return {758, 224.0f + i * 48, 224, 16}; }
void Refresh() { if (editor.window) InvalidateRect(editor.window, nullptr, FALSE); }
RectF ImageRect() {
    if (!editor.source) return {};
    const float scale = min({1.0f, canvas.Width / editor.source->GetWidth(), canvas.Height / editor.source->GetHeight()});
    const float w = editor.source->GetWidth() * scale, h = editor.source->GetHeight() * scale;
    return {canvas.X + (canvas.Width - w) / 2, canvas.Y + (canvas.Height - h) / 2, w, h};
}
bool ImagePoint(POINT p, PointF& point, bool clampPoint = false) {
    const RectF r = ImageRect();
    if (!editor.source || (!clampPoint && !ui::Inside(p, r))) return false;
    // Both pointer input and annotations map back to the original physical pixels.
    point = {clamp((p.x - r.X) * editor.source->GetWidth() / r.Width, 0.0f, static_cast<float>(editor.source->GetWidth() - 1)),
        clamp((p.y - r.Y) * editor.source->GetHeight() / r.Height, 0.0f, static_cast<float>(editor.source->GetHeight() - 1))};
    return true;
}
void Remember() {
    if (editor.history.size() == 24) editor.history.erase(editor.history.begin());
    editor.history.push_back(editor.annotations);
}
void CommitText() {
    if (!editor.textActive) return;
    if (editor.editing >= 0 || !editor.draft.text.empty()) {
        Remember();
        if (editor.editing >= 0) {
            if (editor.draft.text.empty()) editor.annotations.erase(editor.annotations.begin() + editor.editing);
            else editor.annotations[editor.editing] = editor.draft;
        } else editor.annotations.push_back(editor.draft);
    }
    editor.textActive = false; editor.editing = -1; editor.draft = {};
}
void FinishStroke() {
    if (!editor.drawing) return;
    editor.drawing = false; Remember(); editor.annotations.push_back(move(editor.live)); editor.live = {};
}
void Undo() {
    if (editor.textActive) {
        editor.textActive = false; editor.editing = -1; editor.draft = {};
    } else if (!editor.history.empty()) {
        editor.annotations = move(editor.history.back()); editor.history.pop_back();
    }
    editor.notice.clear(); Refresh();
}
void UpdateIme() {
    HIMC context = ImmGetContext(editor.window);
    if (!context) return;
    const float scale = ui::Scale(editor.window);
    COMPOSITIONFORM composition{}; composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos = {static_cast<LONG>((input.X + 12) * scale), static_cast<LONG>((input.Y + 12) * scale)};
    ImmSetCompositionWindow(context, &composition);
    CANDIDATEFORM candidate{}; candidate.dwStyle = CFS_CANDIDATEPOS; candidate.ptCurrentPos = composition.ptCurrentPos;
    ImmSetCandidateWindow(context, &candidate);
    ImmReleaseContext(editor.window, context);
}
void SetColor(Color color) { editor.color = color; if (editor.textActive) editor.draft.color = color; Refresh(); }
void AdjustSize(int delta) {
    float& size = editor.tool == Annotation::Stroke ? editor.penSize : editor.textSize;
    size = clamp(size + delta, editor.tool == Annotation::Stroke ? 1.0f : 8.0f, editor.tool == Annotation::Stroke ? 40.0f : 160.0f);
    if (editor.textActive) editor.draft.size = editor.textSize;
    Refresh();
}
int ChannelValue(int channel) {
    return channel == 0 ? editor.color.GetR() : channel == 1 ? editor.color.GetG() : editor.color.GetB();
}
void SetChannel(int channel, int value) {
    array<int, 3> rgb{editor.color.GetR(), editor.color.GetG(), editor.color.GetB()};
    rgb[channel] = clamp(value, 0, 255);
    SetColor(Color(255, static_cast<BYTE>(rgb[0]), static_cast<BYTE>(rgb[1]), static_cast<BYTE>(rgb[2])));
}
void MoveSlider(POINT p) {
    const auto r = Slider(editor.slider);
    SetChannel(editor.slider, static_cast<int>(lround((p.x - r.X) * 255 / r.Width)));
}
void StartText(PointF point) {
    CommitText(); editor.editing = -1;
    Graphics measure(editor.source.get());
    for (int i = static_cast<int>(editor.annotations.size()) - 1; i >= 0; --i) {
        const auto& a = editor.annotations[i];
        if (a.kind == Annotation::Text && AnnotationTextBounds(measure, a).Contains(point)) { editor.editing = i; break; }
    }
    editor.textActive = true;
    if (editor.editing >= 0) {
        editor.draft = editor.annotations[editor.editing]; editor.color = editor.draft.color; editor.textSize = editor.draft.size;
    } else {
        editor.draft = {}; editor.draft.kind = Annotation::Text; editor.draft.size = editor.textSize;
        editor.draft.color = editor.color; editor.draft.points.push_back(point);
    }
    UpdateIme(); Refresh();
}
void AddText(const wstring& text) {
    if (!editor.textActive) return;
    for (const wchar_t c : text) {
        if ((c >= L' ' || c == L'\n') && editor.draft.text.size() < 1024) editor.draft.text += c;
    }
    editor.notice.clear(); Refresh();
}
void Cancel() {
    ShowWindow(editor.window, SW_HIDE); editor.source.reset(); editor.annotations.clear(); editor.history.clear();
    editor.textActive = editor.drawing = editor.pressing = false;
    if (GetCapture() == editor.window) ReleaseCapture();
    if (editor.cancel) editor.cancel();
}
void Save() {
    if (!editor.source) return;
    CommitText(); FinishStroke();
    auto result = RenderAnnotations(*editor.source, editor.annotations, editor.notice);
    if (result && editor.save && editor.save(result, editor.notice)) {
        ShowWindow(editor.window, SW_HIDE); editor.source.reset(); editor.annotations.clear(); editor.history.clear();
    }
    Refresh();
}
void DrawEditor(HWND hwnd, HDC target = nullptr) {
    ui::Paint paint(hwnd, target); Graphics g(paint.buffer); ui::Prepare(g, hwnd);
    ui::Text(g, L"编辑截图", {24, 20, 200, 36}, 25, ui::foreground, true);
    ui::Text(g, L"原图保留，保存后回到缩略图继续拖动分享", {240, 28, 720, 26}, 13, ui::muted);
    ui::Button(g, closeButton, L"×", false, ui::Inside(editor.mouse, closeButton));
    const auto button = [&](RectF r, const wstring& text, bool active = false) { ui::Button(g, r, text, active, ui::Inside(editor.mouse, r)); };
    button(penButton, L"画笔", editor.tool == Annotation::Stroke); button(textButton, L"文字", editor.tool == Annotation::Text);
    button(smallerButton, L"−"); button(largerButton, L"+");
    ui::Text(g, (editor.tool == Annotation::Stroke ? L"粗细 " : L"字号 ") + to_wstring(static_cast<int>(editor.tool == Annotation::Stroke ? editor.penSize : editor.textSize)) + L" px",
        {302, 85, 116, 26}, 14);
    button(undoButton, L"撤销"); button(commitButton, L"完成文字", editor.textActive);
    for (int i = 0; i < 7; ++i) {
        const auto r = Swatch(i); ui::Round(g, r, 8, colors[i]);
        const bool light = i >= 1 && i <= 4;
        ui::Text(g, colorNames[i], {r.X + 21, r.Y + 8, 30, 28}, 15, light ? ui::background : ui::foreground, true);
        if (editor.color.GetValue() == colors[i].GetValue()) {
            Pen outline(ui::foreground, 2); g.DrawRectangle(&outline, RectF(r.X - 3, r.Y - 3, r.Width + 6, r.Height + 6));
        }
    }
    ui::Round(g, {580, 124, 42, 40}, 8, editor.color);
    ui::Text(g, L"当前颜色", {634, 133, 180, 26}, 13, ui::muted); button(rgbButton, L"RGB 自定义颜色", editor.rgb);
    ui::Round(g, canvas, 10, ui::surface);
    if (editor.source) {
        const auto r = ImageRect();
        const auto saved = g.Save(); g.SetClip(r);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic); g.DrawImage(editor.source.get(), r);
        g.TranslateTransform(r.X, r.Y); const float scale = r.Width / editor.source->GetWidth(); g.ScaleTransform(scale, scale);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        for (int i = 0; i < static_cast<int>(editor.annotations.size()); ++i) if (i != editor.editing) DrawAnnotation(g, editor.annotations[i]);
        if (editor.drawing) DrawAnnotation(g, editor.live);
        if (editor.textActive) {
            DrawAnnotation(g, editor.draft);
            RectF box = AnnotationTextBounds(g, editor.draft);
            if (box.Width < 4) box = {editor.draft.points[0].X, editor.draft.points[0].Y, editor.textSize, editor.textSize * 1.4f};
            Pen pen(ui::accent, 1.0f / scale); pen.SetDashStyle(DashStyleDash); g.DrawRectangle(&pen, box);
        }
        g.Restore(saved);
    }
    ui::Round(g, input, 10, ui::surface);
    ui::Text(g, editor.textActive ? editor.draft.text + L"│" : L"文字工具：先点击图片确定位置，再在此输入；点击已有文字可修改",
        {36, 622, 968, 40}, 14, editor.textActive ? ui::foreground : ui::muted);
    ui::Text(g, editor.notice.empty() ? L"Enter 完成文字 · Shift+Enter 换行 · Ctrl+V 粘贴 · Ctrl+Z 撤销 · Esc 取消编辑" : editor.notice,
        {24, 677, 992, 26}, 12, editor.notice.empty() ? ui::muted : ui::danger);
    ui::Text(g, L"画笔和字号均按原图像素保存", {24, 714, 630, 24}, 12, ui::muted);
    button(cancelButton, L"取消"); button(saveButton, L"保存并返回", true);
    if (editor.rgb) {
        ui::Round(g, palette, 12, ui::background);
        ui::Text(g, L"RGB 0–255 · 拖动或用方向键微调", {716, 188, 284, 26}, 12);
        for (int i = 0; i < 3; ++i) {
            const auto r = Slider(i); const int value = ChannelValue(i);
            ui::Text(g, array<wstring, 3>{L"R", L"G", L"B"}[i], {716, r.Y - 2, 40, 24}, 14, editor.channel == i ? ui::accent : ui::foreground, true);
            array<BYTE, 3> base{editor.color.GetR(), editor.color.GetG(), editor.color.GetB()};
            base[i] = 0; Color start(255, base[0], base[1], base[2]); base[i] = 255; Color end(255, base[0], base[1], base[2]);
            LinearGradientBrush gradient(PointF(r.X, r.Y), PointF(r.GetRight(), r.Y), start, end); g.FillRectangle(&gradient, r);
            Pen marker(ui::foreground, 3); const float x = r.X + value * r.Width / 255; g.DrawLine(&marker, x, r.Y - 3, x, r.GetBottom() + 3);
            ui::Text(g, to_wstring(value), {942, r.Y + 14, 56, 26}, 10, ui::muted);
        }
        ui::Text(g, L"再次点击 RGB 按钮关闭调色板", {716, 364, 284, 24}, 11, ui::muted);
    }
}
LRESULT CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_PAINT: DrawEditor(hwnd); return 0;
    case WM_PRINTCLIENT: DrawEditor(hwnd, reinterpret_cast<HDC>(wparam)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CLOSE: Cancel(); return 0;
    case WM_NCHITTEST: {
        POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; ScreenToClient(hwnd, &p);
        const float scale = ui::Scale(hwnd); p = {static_cast<LONG>(p.x / scale), static_cast<LONG>(p.y / scale)};
        return p.y < 62 && !ui::Inside(p, closeButton) ? HTCAPTION : HTCLIENT;
    }
    case WM_LBUTTONDOWN: {
        editor.mouse = editor.pressed = ui::Mouse(hwnd, lparam); editor.pressing = true; SetFocus(hwnd); SetCapture(hwnd);
        if (editor.rgb) {
            for (int i = 0; i < 3; ++i) {
                auto r = Slider(i); r.Y -= 8; r.Height += 16;
                if (ui::Inside(editor.mouse, r)) { editor.slider = editor.channel = i; MoveSlider(editor.mouse); return 0; }
            }
            if (ui::Inside(editor.mouse, palette)) return 0;
            if (!ui::Inside(editor.mouse, rgbButton)) { editor.rgb = false; editor.pressing = false; Refresh(); return 0; }
        }
        PointF point;
        if (ImagePoint(editor.mouse, point)) {
            editor.notice.clear();
            if (editor.tool == Annotation::Text) StartText(point);
            else {
                CommitText(); editor.drawing = true; editor.live = {}; editor.live.color = editor.color;
                editor.live.size = editor.penSize; editor.live.points.push_back(point);
            }
        }
        Refresh(); return 0;
    }
    case WM_MOUSEMOVE: {
        editor.mouse = ui::Mouse(hwnd, lparam);
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&track);
        if (editor.slider >= 0) MoveSlider(editor.mouse);
        if (editor.drawing && (wparam & MK_LBUTTON)) {
            PointF point;
            if (ImagePoint(editor.mouse, point, true) && editor.live.points.size() < 100000) {
                const auto last = editor.live.points.back();
                if (hypot(point.X - last.X, point.Y - last.Y) >= 0.5f) editor.live.points.push_back(point);
            }
        }
        Refresh(); return 0;
    }
    case WM_MOUSELEAVE: editor.mouse = {-1, -1}; Refresh(); return 0;
    case WM_LBUTTONUP: {
        const bool pressed = editor.pressing, drew = editor.drawing, slid = editor.slider >= 0;
        if (drew) { PointF point; if (ImagePoint(ui::Mouse(hwnd, lparam), point, true)) editor.live.points.push_back(point); FinishStroke(); }
        editor.pressing = false; editor.slider = -1;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (!pressed || drew || slid) { Refresh(); return 0; }
        const POINT p = ui::Mouse(hwnd, lparam);
        const auto hit = [&](const RectF& r) { return ui::Inside(p, r) && ui::Inside(editor.pressed, r); };
        if (editor.rgb && !hit(rgbButton)) return 0;
        if (hit(closeButton) || hit(cancelButton)) Cancel();
        else if (hit(saveButton)) Save();
        else if (hit(penButton)) { CommitText(); editor.tool = Annotation::Stroke; }
        else if (hit(textButton)) editor.tool = Annotation::Text;
        else if (hit(smallerButton)) AdjustSize(editor.tool == Annotation::Stroke ? -1 : -2);
        else if (hit(largerButton)) AdjustSize(editor.tool == Annotation::Stroke ? 1 : 2);
        else if (hit(undoButton)) Undo();
        else if (hit(commitButton)) CommitText();
        else if (hit(rgbButton)) editor.rgb = !editor.rgb;
        else for (int i = 0; i < 7; ++i) if (hit(Swatch(i))) SetColor(colors[i]);
        Refresh(); return 0;
    }
    case WM_CAPTURECHANGED: FinishStroke(); editor.pressing = false; editor.slider = -1; Refresh(); return 0;
    case WM_KEYDOWN:
        if (editor.composing) break;
        if (wparam == VK_ESCAPE) { if (editor.rgb) { editor.rgb = false; Refresh(); } else Cancel(); return 0; }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && wparam == 'Z') { Undo(); return 0; }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && wparam == 'V' && editor.textActive) {
            if (OpenClipboard(hwnd)) {
                HANDLE data = GetClipboardData(CF_UNICODETEXT);
                if (data) {
                    const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(data));
                    if (text) {
                        const size_t limit = min<size_t>(GlobalSize(data) / sizeof(wchar_t), 1024);
                        wstring pasted;
                        for (size_t i = 0; i < limit && text[i]; ++i) if (text[i] != L'\r') pasted += text[i];
                        AddText(pasted); GlobalUnlock(data);
                    }
                }
                CloseClipboard();
            }
            return 0;
        }
        if (editor.rgb && (wparam == VK_LEFT || wparam == VK_RIGHT)) { SetChannel(editor.channel, ChannelValue(editor.channel) + (wparam == VK_LEFT ? -1 : 1)); return 0; }
        if (editor.rgb && (wparam == VK_UP || wparam == VK_DOWN)) { editor.channel = (editor.channel + (wparam == VK_UP ? 2 : 1)) % 3; Refresh(); return 0; }
        return 0;
    case WM_CHAR:
        if (editor.composing) return 0;
        if (wparam == L'\r') {
            if (GetKeyState(VK_SHIFT) & 0x8000) AddText(L"\n"); else { CommitText(); Refresh(); }
        } else if (wparam == VK_BACK && editor.textActive && !editor.draft.text.empty()) {
            const wchar_t last = editor.draft.text.back(); editor.draft.text.pop_back();
            if (last >= 0xDC00 && last <= 0xDFFF && !editor.draft.text.empty() && editor.draft.text.back() >= 0xD800 && editor.draft.text.back() <= 0xDBFF) editor.draft.text.pop_back();
            Refresh();
        } else if (wparam >= L' ' && wparam != 127) AddText(wstring(1, static_cast<wchar_t>(wparam)));
        return 0;
    case WM_IME_STARTCOMPOSITION: editor.composing = true; UpdateIme(); break;
    case WM_IME_ENDCOMPOSITION: editor.composing = false; break;
    case WM_IME_COMPOSITION:
        if (lparam & GCS_RESULTSTR) {
            HIMC context = ImmGetContext(hwnd);
            if (context) {
                const LONG length = ImmGetCompositionStringW(context, GCS_RESULTSTR, nullptr, 0);
                if (length > 0 && length <= 8192) {
                    wstring text(static_cast<size_t>(length) / sizeof(wchar_t), L'\0');
                    if (ImmGetCompositionStringW(context, GCS_RESULTSTR, text.data(), length) == length) AddText(text);
                }
                ImmReleaseContext(hwnd, context);
            }
            return 0;
        }
        break;
    case WM_DPICHANGED: ui::DpiChanged(hwnd, lparam, width, height); UpdateIme(); return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}
}
bool InitEditor(HINSTANCE instance, HWND owner, function<bool(unique_ptr<Bitmap>&, wstring&)> save, function<void()> cancel) {
    WNDCLASSEXW cls{sizeof(cls)}; cls.hInstance = instance; cls.lpfnWndProc = EditorProc;
    cls.lpszClassName = L"ScreenShotPlus.Editor"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    WNDCLASSEXW existing{sizeof(existing)};
    if (!GetClassInfoExW(instance, cls.lpszClassName, &existing) && !RegisterClassExW(&cls)) return false;
    editor.save = move(save); editor.cancel = move(cancel);
    editor.window = CreateWindowExW(WS_EX_TOOLWINDOW, cls.lpszClassName, L"ScreenShotPlus 编辑截图", WS_POPUP,
        0, 0, width, height, owner, nullptr, instance, nullptr);
    if (editor.window) ui::Corners(editor.window);
    return editor.window != nullptr;
}
bool ShowEditor(Bitmap& image, wstring& error) {
    if (!editor.window) { error = L"编辑窗口尚未初始化。"; return false; }
    auto copy = RenderAnnotations(image, {}, error);
    if (!copy) return false;
    editor.source = move(copy); editor.annotations.clear(); editor.history.clear(); editor.live = {}; editor.draft = {};
    editor.textActive = editor.drawing = editor.pressing = editor.rgb = editor.composing = false;
    editor.editing = editor.slider = -1; editor.tool = Annotation::Stroke; editor.color = colors[0];
    editor.penSize = 4; editor.textSize = 28; editor.notice.clear();
    ui::Place(editor.window, width, height); ShowWindow(editor.window, SW_SHOW); FocusEditor(); Refresh();
    return true;
}
void FocusEditor() { if (EditorVisible()) { SetForegroundWindow(editor.window); SetFocus(editor.window); } }
bool EditorVisible() { return editor.window && IsWindowVisible(editor.window); }
void DestroyEditor() {
    if (editor.window) { DestroyWindow(editor.window); editor.window = nullptr; }
    editor.source.reset(); editor.annotations.clear(); editor.history.clear(); editor.save = {}; editor.cancel = {};
}
