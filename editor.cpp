#include "editor.h"
#include "ui.h"
#include "storage.h"
using namespace std;
using namespace Gdiplus;
namespace editor {
namespace {
// 工具栏逻辑尺寸。画布宽度按图片比例推导，因此窗口宽度随图片变化。
constexpr int kToolbarWidth = 120, kSideMargin = 12, kTopMargin = 40;
constexpr int kMinCanvasWidth = 480, kMaxCanvasWidth = 1280;
constexpr int kMargin = kSideMargin + kToolbarWidth + kSideMargin;
constexpr int kToolbarX = kSideMargin, kToolbarContent = kToolbarWidth - 10;
constexpr int kColumns = 3, kGap = 2;
constexpr float kButtonHeight = 32, kSwatchHeight = 30;

RectF Cell(int index, float top, float height) {
    const float width = (kToolbarContent - kGap * (kColumns - 1)) / kColumns;
    return RectF(kToolbarX + (index % kColumns) * (width + kGap), top + (index / kColumns) * (height + kGap),
        width, height);
}
RectF Wide(int index, float top, float height, float width) {
    return RectF(kToolbarX + index * width + index * kGap, top, width - kGap, height);
}

struct Editor {
    HWND window = nullptr, owner = nullptr;
    HINSTANCE instance = nullptr;
    unique_ptr<Bitmap> canvas;              // 原始图片的副本，标注不直接写入
    filesystem::path file;
    vector<Shape> shapes, undone;
    Tool tool = Tool::Brush;
    bool erasing = false;
    int colorIndex = 0;
    float stroke = 4, fontSize = 32;
    Shape current;                          // 正在绘制、尚未入栈的标注
    bool drawing = false, textPending = false, dirty = false;
    bool trackingMouse = false, cancelArmed = false;
    wstring notice;
    PointF mouse{-1, -1};
    RectF canvasRect;
    int width = 0, height = 0;              // 窗口逻辑尺寸
    function<void(Bitmap*, const filesystem::path&)> done;
};
Editor editor;
bool verbose = false;
// GDI+ 的 Bitmap 重载了 operator new 且不接受 nothrow，因此统一用 try/catch 兜住构造失败。
Bitmap* NewBitmap(UINT width, UINT height, PixelFormat format) {
    try { return new Bitmap(width, height, format); } catch (...) { return nullptr; }
}
void Trace(const char* text, ...) {
    if (!verbose) return;
    va_list args;
    va_start(args, text);
    vfprintf(stderr, text, args);
    va_end(args);
    fflush(stderr);
}
Color ColorAt(int index) {
    switch (index) {
    case 0: return Color(255, 229, 57, 53);    // 赤
    case 1: return Color(255, 251, 140, 0);    // 橙
    case 2: return Color(255, 253, 216, 53);   // 黄
    case 3: return Color(255, 67, 160, 71);    // 绿
    case 4: return Color(255, 0, 172, 193);    // 青
    case 5: return Color(255, 30, 136, 229);   // 蓝
    case 6: return Color(255, 142, 36, 170);   // 紫
    case 7: return Color(255, 255, 255, 255);  // 白
    default: return Color(255, 0, 0, 0);       // 黑
    }
}
void DrawTextShape(Graphics& g, const Shape& shape) {
    if (shape.text.empty()) return;
    Font font(L"Microsoft YaHei UI", shape.size, FontStyleRegular, UnitPixel);
    SolidBrush brush(shape.color);
    StringFormat format(StringFormat::GenericTypographic());
    format.SetFormatFlags(static_cast<INT>(format.GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces));
    REAL line = shape.size * 1.3f;
    size_t start = 0;
    int row = 0;
    while (start <= shape.text.size()) {
        const size_t end = shape.text.find(L'\n', start);
        const wstring lineText = shape.text.substr(start, end == wstring::npos ? wstring::npos : end - start);
        if (!lineText.empty())
            g.DrawString(lineText.c_str(), static_cast<INT>(lineText.size()), &font,
                PointF(shape.at.X, shape.at.Y + row * line), &format, &brush);
        if (end == wstring::npos) break;
        start = end + 1;
        ++row;
    }
}
bool Pending() { return editor.textPending && !editor.current.text.empty(); }
void CommitText() {
    if (!Pending()) { editor.textPending = false; editor.current.text.clear(); return; }
    editor.shapes.push_back(editor.current);
    editor.undone.clear();
    editor.current.text.clear();
    editor.textPending = false;
    editor.dirty = true;
    Trace("EDITOR_TEXT committed=%zu\n", editor.shapes.size());
}
void CancelText() {
    editor.textPending = false;
    editor.current.text.clear();
}
void Undo() {
    if (editor.shapes.empty()) return;
    editor.undone.push_back(editor.shapes.back());
    editor.shapes.pop_back();
    editor.dirty = true;
}
void Redo() {
    if (editor.undone.empty()) return;
    editor.shapes.push_back(editor.undone.back());
    editor.undone.pop_back();
    editor.dirty = true;
}
bool InsidePoint(const PointF& point, const RectF& rect) {
    return point.X >= rect.X && point.Y >= rect.Y && point.X < rect.GetRight() && point.Y < rect.GetBottom();
}
bool InCanvas(const PointF& point) { return InsidePoint(point, editor.canvasRect); }
void Layout(Bitmap& image) {    const int imageWidth = max(1, static_cast<int>(image.GetWidth()));
    const int imageHeight = max(1, static_cast<int>(image.GetHeight()));
    int canvasWidth = clamp(imageWidth, kMinCanvasWidth, kMaxCanvasWidth);
    if (canvasWidth > imageWidth) canvasWidth = max(kMinCanvasWidth, imageWidth);
    const float scale = static_cast<float>(canvasWidth) / imageWidth;
    const float canvasHeight = clamp(imageHeight * scale, 120.0f, 900.0f);
    editor.width = kMargin + canvasWidth;
    editor.height = kTopMargin + static_cast<int>(canvasHeight);
    editor.canvasRect = RectF(static_cast<float>(kSideMargin + kToolbarWidth + kSideMargin), kTopMargin,
        static_cast<float>(canvasWidth), canvasHeight);
    editor.stroke = static_cast<float>(DefaultStrokeWidth(image));
    editor.fontSize = DefaultFontSize(image);
}
void Paint(HWND hwnd) {
    ui::Paint paint(hwnd);
    Graphics g(paint.buffer);
    ui::Prepare(g, hwnd);
    // 工具栏
    ui::Round(g, {6, 6, 108, static_cast<float>(editor.height - 12)}, 12, ui::surface);
    ui::Text(g, L"×", {84, 8, 26, 24}, 18, ui::muted, true);
    ui::Text(g, L"标注编辑", {12, 42, 96, 20}, 12, ui::foreground, true);
    for (int i = 0; i < kPresetCount; ++i) {
        const RectF cell = Cell(i, 66, kSwatchHeight);
        GraphicsPath path;
        const float d = min(cell.Width, cell.Height);
        path.AddEllipse(cell.X + (cell.Width - d) / 2, cell.Y + (cell.Height - d) / 2, d, d);
        SolidBrush brush(PresetColor(i));
        g.FillPath(&brush, &path);
        if (i == editor.colorIndex) {
            Pen ring(ui::accent, 2);
            g.DrawPath(&ring, &path);
        }
    }
    const float row1 = 66 + 3 * (kSwatchHeight + kGap) + 6;
    const float row2 = row1 + kButtonHeight + kGap + 6;
    const float row3 = row2 + kButtonHeight + kGap + 10;
    const float actionWidth = (kToolbarContent + kGap) / 2;
    const array<pair<RectF, wstring>, 5> toolButtons{{
        {Cell(0, row1, kButtonHeight), L"文字"}, {Cell(1, row1, kButtonHeight), L"擦除"},
        {Cell(2, row1, kButtonHeight), L"画笔"}, {Wide(0, row3, 34, actionWidth), L"取消"},
        {Wide(1, row3, 34, actionWidth), L"保存"}}};
    ui::Button(g, toolButtons[0].first, toolButtons[0].second, editor.tool == Tool::Text,
        InsidePoint(editor.mouse, toolButtons[0].first));
    ui::Button(g, toolButtons[1].first, toolButtons[1].second, editor.tool == Tool::Brush && editor.erasing,
        InsidePoint(editor.mouse, toolButtons[1].first));
    ui::Button(g, toolButtons[2].first, toolButtons[2].second, editor.tool == Tool::Brush && !editor.erasing,
        InsidePoint(editor.mouse, toolButtons[2].first));
    ui::Button(g, Cell(0, row2, kButtonHeight), to_wstring(static_cast<int>(editor.stroke)), false,
        InsidePoint(editor.mouse, Cell(0, row2, kButtonHeight)));
    ui::Button(g, Cell(1, row2, kButtonHeight), L"A-", false, InsidePoint(editor.mouse, Cell(1, row2, kButtonHeight)));
    ui::Button(g, Cell(2, row2, kButtonHeight), L"A+", false, InsidePoint(editor.mouse, Cell(2, row2, kButtonHeight)));
    ui::Button(g, toolButtons[3].first, toolButtons[3].second, false, InsidePoint(editor.mouse, toolButtons[3].first));
    ui::Button(g, toolButtons[4].first, toolButtons[4].second, true, InsidePoint(editor.mouse, toolButtons[4].first));
    // 画布：整体缩放显示，绘制坐标始终使用图片像素。
    ui::Round(g, editor.canvasRect, 8, Color(255, 14, 18, 24));
    if (!editor.canvas) return;
    const float imageWidth = static_cast<float>(editor.canvas->GetWidth());
    const float imageHeight = static_cast<float>(editor.canvas->GetHeight());
    const float scale = min(editor.canvasRect.Width / imageWidth, editor.canvasRect.Height / imageHeight);
    const float drawnWidth = imageWidth * scale, drawnHeight = imageHeight * scale;
    const float originX = editor.canvasRect.X + (editor.canvasRect.Width - drawnWidth) / 2;
    const float originY = editor.canvasRect.Y + (editor.canvasRect.Height - drawnHeight) / 2;
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.DrawImage(editor.canvas.get(), RectF(originX, originY, drawnWidth, drawnHeight),
        0.0f, 0.0f, imageWidth, imageHeight, UnitPixel);
    // 已有标注与正在进行的标注都按同一比例缩放绘制，保证显示与导出一致。
    const auto drawScaled = [&](Graphics& target, const Shape& shape, float offX, float offY, float factor) {
        Shape copy = shape;
        for (auto& point : copy.points) {
            point.X = offX + point.X * factor;
            point.Y = offY + point.Y * factor;
        }
        copy.at.X = offX + copy.at.X * factor;
        copy.at.Y = offY + copy.at.Y * factor;
        copy.stroke = max(1.0f, copy.stroke * factor);
        copy.size = max(1.0f, copy.size * factor);
        DrawShape(target, copy);
    };
    for (const auto& shape : editor.shapes) drawScaled(g, shape, originX, originY, scale);
    if (editor.textPending || editor.drawing) drawScaled(g, editor.current, originX, originY, scale);
    if (editor.tool == Tool::Text && editor.textPending) {
        ui::Text(g, L"输入文字，Enter 确认，Esc 取消", {editor.canvasRect.X + 8, editor.canvasRect.GetBottom() - 30,
            editor.canvasRect.Width - 16, 24}, 12, ui::accent);
    }
    if (!editor.notice.empty()) {
        ui::Round(g, {8, static_cast<float>(editor.height - 40), 104, 32}, 8, ui::background);
        ui::Text(g, editor.notice, {12, static_cast<float>(editor.height - 36), 96, 26}, 11, ui::danger);
    }
    // 鼠标到图片坐标的换算基准与上面保持一致。
    editor.canvasRect = RectF(originX, originY, drawnWidth, drawnHeight);
}
// 命中判定统一用浮点坐标，图片像素与逻辑像素都走这一条路径。
PointF ToImage(PointF raw) {
    if (!editor.canvas || editor.canvasRect.Width <= 0 || editor.canvasRect.Height <= 0) return {};
    const float scale = editor.canvasRect.Width / max(1.0f, static_cast<float>(editor.canvas->GetWidth()));
    return PointF((raw.X - editor.canvasRect.X) / scale, (raw.Y - editor.canvasRect.Y) / scale);
}
PointF ToClient(HWND hwnd, LPARAM lparam) {
    const float scale = ui::Scale(hwnd);
    return PointF(static_cast<float>(GET_X_LPARAM(lparam)) / scale, static_cast<float>(GET_Y_LPARAM(lparam)) / scale);
}
Shape NewShape() {
    Shape shape;
    shape.tool = editor.tool;
    shape.color = editor.erasing ? Color(255, 0, 0, 0) : PresetColor(editor.colorIndex);
    shape.stroke = editor.stroke;
    shape.size = editor.fontSize;
    return shape;
}
void Save() {
    if (!editor.canvas || editor.file.empty()) return;
    if (Pending()) CommitText();
    unique_ptr<Bitmap> flattened(NewBitmap(editor.canvas->GetWidth(), editor.canvas->GetHeight(),
        editor.canvas->GetPixelFormat()));
    if (!flattened || !Flatten(*editor.canvas, editor.shapes, *flattened)) {
        editor.notice = L"无法生成编辑后的图像。";
        InvalidateRect(editor.window, nullptr, FALSE);
        return;
    }
    wstring error;
    if (!ReplacePng(*flattened, editor.file, error)) {
        // 保存失败时保留编辑窗口与全部标注，避免用户丢失工作。
        editor.notice = error;
        InvalidateRect(editor.window, nullptr, FALSE);
        return;
    }
    auto done = move(editor.done);
    editor.done = {};
    editor.shapes.clear();
    editor.undone.clear();
    editor.canvas.reset();
    editor.dirty = false;
    editor.notice.clear();
    editor.cancelArmed = false;
    ShowWindow(editor.window, SW_HIDE);
    if (done) done(flattened.release(), editor.file);
}
void Cancel() {
    if (Pending()) CancelText();
    // 有未保存的标注时要求再点一次，避免误触丢弃工作。
    if (editor.dirty && !editor.cancelArmed) {
        editor.cancelArmed = true;
        editor.notice = L"再次点击“取消”将放弃本次标注。";
        InvalidateRect(editor.window, nullptr, FALSE);
        return;
    }
    auto done = move(editor.done);
    editor.done = {};
    editor.shapes.clear();
    editor.undone.clear();
    editor.canvas.reset();
    editor.dirty = false;
    editor.cancelArmed = false;
    editor.notice.clear();
    ShowWindow(editor.window, SW_HIDE);
    if (done) done(nullptr, editor.file);
}
void OnClick(const PointF& point) {
    if (InsidePoint(point, RectF(84, 8, 26, 24))) { Cancel(); return; }
    for (int i = 0; i < kPresetCount; ++i) {
        if (InsidePoint(point, Cell(i, 66, kSwatchHeight))) {
            editor.colorIndex = i; editor.erasing = false; editor.tool = Tool::Brush; return;
        }
    }
    const float row1 = 66 + 3 * (kSwatchHeight + kGap) + 6;
    const float row2 = row1 + kButtonHeight + kGap + 6;
    const float row3 = row2 + kButtonHeight + kGap + 10;
    if (InsidePoint(point, Cell(0, row1, kButtonHeight))) { if (Pending()) CommitText(); CancelText(); editor.tool = Tool::Text; return; }
    if (InsidePoint(point, Cell(1, row1, kButtonHeight))) { editor.erasing = true; editor.tool = Tool::Brush; return; }
    if (InsidePoint(point, Cell(2, row1, kButtonHeight))) { editor.erasing = false; editor.tool = Tool::Brush; return; }
    if (InsidePoint(point, Cell(0, row2, kButtonHeight))) { editor.stroke = NextStrokeWidth(editor.stroke); return; }
    if (InsidePoint(point, Cell(1, row2, kButtonHeight))) { editor.fontSize = NextFontSize(editor.fontSize, -1); return; }
    if (InsidePoint(point, Cell(2, row2, kButtonHeight))) { editor.fontSize = NextFontSize(editor.fontSize, 1); return; }
    const float actionWidth = (kToolbarContent + kGap) / 2;
    if (InsidePoint(point, Wide(0, row3, 34, actionWidth))) { Cancel(); return; }
    if (InsidePoint(point, Wide(1, row3, 34, actionWidth))) { Save(); return; }
}
LRESULT CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_PAINT: Paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CLOSE: Cancel(); return 0;
    case WM_SETCURSOR:
        if (editor.tool == Tool::Text) SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
        else SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        return TRUE;
    case WM_MOUSEMOVE: {
        editor.mouse = ToClient(hwnd, lparam);
        if (!editor.trackingMouse) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            editor.trackingMouse = true;
        }
        if (editor.drawing && editor.tool == Tool::Brush && InCanvas(editor.mouse)) {
            const PointF point = ToImage(editor.mouse);
            if (editor.current.points.empty() || hypot(point.X - editor.current.points.back().X,
                point.Y - editor.current.points.back().Y) >= 0.5f) {
                editor.current.points.push_back(point);
                editor.dirty = true;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSELEAVE: editor.mouse = {-1, -1}; editor.trackingMouse = false; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd);
        const PointF point = ToClient(hwnd, lparam);
        if (!InCanvas(point)) { OnClick(point); InvalidateRect(hwnd, nullptr, FALSE); return 0; }
        if (editor.tool == Tool::Text) {
            if (Pending()) CommitText();
            editor.current = NewShape();
            editor.current.at = ToImage(point);
            editor.textPending = true;
            editor.dirty = true;
            Trace("EDITOR_TEXT start at=(%.1f,%.1f)\n", editor.current.at.X, editor.current.at.Y);
        } else {
            editor.current = NewShape();
            editor.current.points.clear();
            const PointF image = ToImage(point);
            editor.current.points.push_back(image);
            editor.current.points.push_back(image);
            editor.drawing = true;
            SetCapture(hwnd);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
        if (editor.drawing) {
            editor.drawing = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            if (!editor.current.points.empty()) {
                editor.shapes.push_back(editor.current);
                editor.undone.clear();
                editor.current.points.clear();
                editor.dirty = true;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (editor.drawing) {
            editor.drawing = false;
            if (!editor.current.points.empty()) {
                editor.shapes.push_back(editor.current);
                editor.undone.clear();
                editor.current.points.clear();
            }
        }
        return 0;
    case WM_CHAR: {
        const wchar_t character = static_cast<wchar_t>(wparam);
        if (character == L'\r' || character == L'\n') { CommitText(); InvalidateRect(hwnd, nullptr, FALSE); return 0; }
        if (character == L'\x1b') { CancelText(); InvalidateRect(hwnd, nullptr, FALSE); return 0; }
        if (character == L'\b') {
            if (!editor.current.text.empty()) editor.current.text.pop_back();
            InvalidateRect(hwnd, nullptr, FALSE); return 0;
        }
        if (character >= L' ' && editor.textPending && !(GetKeyState(VK_CONTROL) & 0x8000)) {
            editor.current.text.push_back(character);
            editor.dirty = true;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) { Cancel(); return 0; }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && (wparam == 'S' || wparam == 's')) { Save(); return 0; }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && (wparam == 'Z' || wparam == 'z')) { Undo(); InvalidateRect(hwnd, nullptr, FALSE); return 0; }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && (wparam == 'Y' || wparam == 'y')) { Redo(); InvalidateRect(hwnd, nullptr, FALSE); return 0; }
        return 0;
    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        ui::PlaceAt(hwnd, suggested->left, suggested->top, editor.width, editor.height,
            POINT{suggested->left, suggested->top});
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}
}
Color PresetColor(int index) { return ColorAt(clamp(index, 0, kPresetCount - 1)); }
int DefaultStrokeWidth(Bitmap& image) {
    return clamp(static_cast<int>(max(image.GetWidth(), image.GetHeight()) / 240), 3, 16);
}
float DefaultFontSize(Bitmap& image) {
    return static_cast<float>(clamp(static_cast<int>(image.GetWidth() / 32), 20, 96));
}
float NextStrokeWidth(float current) {
    constexpr array<float, 5> choices{3, 5, 8, 12, 18};
    for (const float choice : choices) if (choice > current + 0.5f) return choice;
    return choices.front();
}
float NextFontSize(float current, int direction) {
    constexpr array<float, 8> choices{16, 22, 30, 40, 54, 72, 96, 128};
    int index = 0;
    for (size_t i = 0; i < choices.size(); ++i) if (choices[i] <= current + 0.5f) index = static_cast<int>(i);
    index = clamp(index + (direction >= 0 ? 1 : -1), 0, static_cast<int>(choices.size()) - 1);
    return choices[static_cast<size_t>(index)];
}
void DrawShape(Graphics& g, const Shape& shape) {
    if (shape.tool == Tool::Text) { DrawTextShape(g, shape); return; }
    if (shape.points.empty()) return;
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    Pen pen(shape.color, max(1.0f, shape.stroke));
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    if (shape.points.size() == 1) {
        SolidBrush brush(shape.color);
        const float radius = max(1.0f, shape.stroke) / 2;
        g.FillEllipse(&brush, shape.points[0].X - radius, shape.points[0].Y - radius, radius * 2, radius * 2);
        return;
    }
    g.DrawLines(&pen, shape.points.data(), static_cast<INT>(shape.points.size()));
}
bool Flatten(Bitmap& source, const vector<Shape>& shapes, Bitmap& target) {
    const UINT width = source.GetWidth(), height = source.GetHeight();
    if (target.GetWidth() != width || target.GetHeight() != height) return false;
    Graphics g(&target);
    if (g.GetLastStatus() != Ok) return false;
    const Status copied = g.DrawImage(&source, Rect(0, 0, static_cast<INT>(width), static_cast<INT>(height)),
        0, 0, static_cast<INT>(width), static_cast<INT>(height), UnitPixel);
    if (copied != Ok) return false;
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    for (const auto& shape : shapes) DrawShape(g, shape);
    return g.GetLastStatus() == Ok;
}
bool InitEditor(HINSTANCE instance, HWND owner, function<void(Bitmap*, const filesystem::path&)> done) {
    WNDCLASSEXW cls{sizeof(cls)};
    cls.hInstance = instance; cls.lpfnWndProc = EditorProc; cls.lpszClassName = L"ScreenShotPlus.Editor";
    cls.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    WNDCLASSEXW existing{sizeof(existing)};
    if (!GetClassInfoExW(instance, cls.lpszClassName, &existing) && !RegisterClassExW(&cls)) return false;
    editor = Editor{};
    editor.instance = instance; editor.owner = owner; editor.done = move(done);
    editor.window = CreateWindowExW(WS_EX_TOOLWINDOW, cls.lpszClassName, L"ScreenShotPlus 标注编辑", WS_POPUP,
        0, 0, 900, 600, owner, nullptr, instance, nullptr);
    if (editor.window) ui::Corners(editor.window);
    return editor.window != nullptr;
}
void OpenEditor(Bitmap& image, const filesystem::path& file) {
    if (!editor.window) return;
    unique_ptr<Bitmap> copy(NewBitmap(image.GetWidth(), image.GetHeight(), image.GetPixelFormat()));
    if (!copy || copy->GetLastStatus() != Ok) return;
    {
        Graphics g(copy.get());
        if (g.DrawImage(&image, 0, 0, static_cast<INT>(image.GetWidth()), static_cast<INT>(image.GetHeight())) != Ok) return;
    }
    editor.canvas = move(copy);
    editor.file = file;
    editor.shapes.clear();
    editor.undone.clear();
    editor.tool = Tool::Brush;
    editor.erasing = false;
    editor.dirty = false;
    editor.drawing = false;
    editor.textPending = false;
    editor.current = Shape{};
    Layout(*editor.canvas);
    POINT anchor{}; GetCursorPos(&anchor);
    const int x = anchor.x - editor.width / 2;
    const int y = anchor.y - editor.height / 2;
    ui::PlaceAt(editor.window, x, y, editor.width, editor.height, anchor);
    ShowWindow(editor.window, SW_SHOW);
    SetForegroundWindow(editor.window);
    SetFocus(editor.window);
    InvalidateRect(editor.window, nullptr, FALSE);
    Trace("EDITOR_OPEN size=%dx%d canvas=%.0fx%.0f stroke=%.0f font=%.0f\n", editor.width, editor.height,
        editor.canvasRect.Width, editor.canvasRect.Height, editor.stroke, editor.fontSize);
}
void HideEditor() { if (editor.window) ShowWindow(editor.window, SW_HIDE); }
bool EditorVisible() { return editor.window && IsWindowVisible(editor.window); }
void DestroyEditor() {
    if (editor.window) { DestroyWindow(editor.window); editor.window = nullptr; }
    editor.canvas.reset(); editor.shapes.clear(); editor.undone.clear(); editor.done = {};
}
void SetDiagnostics(bool enabled) { verbose = enabled; }
Bitmap* Canvas() { return editor.canvas.get(); }
RectF CanvasRect() { return editor.canvasRect; }
RectF SwatchRect(int index) { return Cell(clamp(index, 0, kPresetCount - 1), 66, kSwatchHeight); }
int SelectedColorIndex() { return editor.colorIndex; }
size_t ShapeCount() { return editor.shapes.size(); }
void UndoLastShape() { Undo(); }
void RedoLastShape() { Redo(); }
void RequestSave() { Save(); }
}
