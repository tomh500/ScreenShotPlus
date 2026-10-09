#include "ui.h"
#include <new>
using namespace std;
using namespace Gdiplus;
namespace ui {
namespace {
constexpr wchar_t bufferProperty[] = L"ScreenShotPlus.PaintBuffer";
struct PaintBuffer {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old = nullptr;
    int width = 0, height = 0;
    ~PaintBuffer() {
        if (old) SelectObject(dc, old);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
};
void RoundedPath(GraphicsPath& path, RectF r, float radius) {
    const float d = max(0.1f, min(radius * 2, min(r.Width, r.Height)));
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    path.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90); path.CloseFigure();
}
Color Mix(Color a, Color b, float t) {
    const auto mix = [t](BYTE x, BYTE y) { return static_cast<BYTE>(clamp(lround(x + (y - x) * t), 0L, 255L)); };
    return Color(mix(a.GetA(), b.GetA()), mix(a.GetR(), b.GetR()), mix(a.GetG(), b.GetG()), mix(a.GetB(), b.GetB()));
}
}
Color AccentColor(unsigned choice) {
    const Color colors[]{Color(255, 105, 232, 187), Color(255, 92, 167, 255),
        Color(255, 183, 139, 255), Color(255, 255, 137, 179), Color(255, 245, 199, 108)};
    return colors[min(choice, 4u)];
}
void SetAccent(unsigned choice) { accent = AccentColor(choice); }
Paint::Paint(HWND hwnd, HDC target, bool fade) : window(hwnd), printing(target != nullptr), reveal(fade) {
    BeginMotionPaint(hwnd);
    dc = target ? target : BeginPaint(window, &ps);
    RECT r{}; GetClientRect(window, &r);
    width = max(1L, r.right); height = max(1L, r.bottom);
    auto saved = static_cast<PaintBuffer*>(GetPropW(hwnd, bufferProperty));
    if (saved && (saved->width != width || saved->height != height)) { ReleasePaintBuffer(hwnd); saved = nullptr; }
    if (saved) { buffer = saved->dc; bitmap = saved->bitmap; old = saved->old; cached = true; return; }
    buffer = CreateCompatibleDC(dc);
    bitmap = CreateCompatibleBitmap(dc, width, height);
    if (buffer && bitmap) {
        old = SelectObject(buffer, bitmap);
        auto next = GetPropW(hwnd, L"ScreenShotPlus.Motion") ? new (std::nothrow) PaintBuffer : nullptr;
        if (next) {
            next->dc = buffer; next->bitmap = bitmap; next->old = old; next->width = width; next->height = height;
            if (SetPropW(hwnd, bufferProperty, next)) cached = true;
            else { next->dc = nullptr; next->bitmap = nullptr; next->old = nullptr; delete next; }
        }
    }
    else {
        if (bitmap) DeleteObject(bitmap);
        if (buffer) DeleteDC(buffer);
        bitmap = nullptr; buffer = dc;
    }
}
Paint::~Paint() {
    // Soft reveal blends the finished frame, including text and images. The
    // hit rectangles and image pixel coordinates remain stationary.
    const float entrance = Entrance(window);
    if (reveal && entrance < 1) {
        Graphics g(buffer);
        SolidBrush veil(Color(static_cast<BYTE>((1 - entrance) * 230 * min(1.0f, MotionAmount())),
            background.GetR(), background.GetG(), background.GetB()));
        g.FillRectangle(&veil, 0, 0, width, height);
    }
    if (bitmap) {
        if (printing) BitBlt(dc, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
        else if (!IsRectEmpty(&ps.rcPaint)) {
            const auto& r = ps.rcPaint;
            BitBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, buffer, r.left, r.top, SRCCOPY);
        }
        if (!cached) { SelectObject(buffer, old); DeleteObject(bitmap); DeleteDC(buffer); }
    }
    if (!printing) EndPaint(window, &ps);
    EndMotionPaint(window);
}
void ReleasePaintBuffer(HWND hwnd) {
    delete static_cast<PaintBuffer*>(RemovePropW(hwnd, bufferProperty));
}
float Scale(HWND hwnd) {
    const auto fitted = reinterpret_cast<UINT_PTR>(GetPropW(hwnd, L"ScreenShotPlus.UiScale"));
    return fitted ? static_cast<float>(fitted) / 1024.0f : static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
}
POINT Mouse(HWND hwnd, LPARAM value) {
    const float s = Scale(hwnd);
    return {static_cast<LONG>(GET_X_LPARAM(value) / s), static_cast<LONG>(GET_Y_LPARAM(value) / s)};
}
bool Inside(POINT p, const RectF& r) {
    return p.x >= r.X && p.y >= r.Y && p.x < r.GetRight() && p.y < r.GetBottom();
}
void Text(Graphics& g, const wstring& text, RectF r, float size, Color color, bool bold) {
    Font font(L"Microsoft YaHei UI", size, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    SolidBrush brush(color); StringFormat format;
    format.SetTrimming(StringTrimmingEllipsisCharacter);
    g.DrawString(text.c_str(), static_cast<INT>(text.size()), &font, r, &format, &brush);
}
void Round(Graphics& g, RectF r, float radius, Color color) {
    GraphicsPath path;
    RoundedPath(path, r, radius); SolidBrush brush(color); g.FillPath(&brush, &path);
}
void Border(Graphics& g, RectF r, float radius, Color color) {
    GraphicsPath path; RoundedPath(path, r, radius); Pen pen(color, 1); g.DrawPath(&pen, &path);
}
void ClipRound(Graphics& g, RectF r, float radius) {
    GraphicsPath path; RoundedPath(path, r, radius); g.SetClip(&path, CombineModeIntersect);
}
void Button(Graphics& g, HWND hwnd, RectF r, const wstring& text, bool primary, bool hovered) {
    const auto motion = AnimateControl(hwnd, r, hovered, primary);
    const float insetPress = motion.press * min(1.4f, r.Width * 0.009f) * MotionAmount();
    r.Inflate(-insetPress, -insetPress);
    const auto hoverColor = text == L"×" ? Color(255, 184, 56, 63) : Color(255, 56, 65, 78);
    const auto fill = Mix(Mix(surface, hoverColor, motion.hover),
        Mix(accent, foreground, motion.hover * 0.35f), motion.selected);
    RectF shadow = r; shadow.Y += 1 + motion.hover * 1.2f * MotionAmount();
    Round(g, shadow, 8, Color(static_cast<BYTE>(18 + motion.hover * 15), 0, 0, 0));
    Round(g, r, 8, Mix(fill, background, motion.press * 0.12f));
    Border(g, {r.X + 0.5f, r.Y + 0.5f, r.Width - 1, r.Height - 1}, 8,
        Mix(Color(255, 65, 73, 85), Mix(accent, foreground, 0.50f), motion.selected * 0.60f + motion.hover * 0.25f));
    const float inset = r.Width < 64 ? 6.0f : 16.0f;
    Text(g, text, {r.X + inset, r.Y + (r.Height - 22) / 2, r.Width - inset * 2, 24},
        14, Mix(foreground, background, motion.selected), primary);
}
void Toggle(Graphics& g, HWND hwnd, RectF r, const wstring& text, bool enabled, bool hovered) {
    const auto motion = AnimateControl(hwnd, r, hovered, enabled);
    Round(g, r, 8, Mix(surface, Color(255, 53, 61, 73), motion.hover));
    Border(g, {r.X + 0.5f, r.Y + 0.5f, r.Width - 1, r.Height - 1}, 8, Color(255, 65, 73, 85));
    Text(g, text, {r.X + 16, r.Y + (r.Height - 22) / 2, r.Width - 92, 24}, 14);
    const RectF track{r.GetRight() - 60, r.Y + (r.Height - 22) / 2, 42, 22};
    Round(g, track, 11, Mix(Color(255, 76, 85, 98), accent, motion.selected));
    const float press = motion.press * MotionAmount();
    const float diameter = 14 + press * 2;
    const float x = track.X + 4 + motion.selected * (track.Width - 22);
    SolidBrush thumb(Mix(foreground, background, motion.selected));
    g.FillEllipse(&thumb, x - press, track.Y + (22 - diameter) / 2, diameter, diameter);
}
void ImageCache::Draw(Graphics& g, HWND hwnd, Bitmap& image, RectF rect) {
    const float scale = Scale(hwnd);
    const int w = max(1, static_cast<int>(ceil(rect.Width * scale))), h = max(1, static_cast<int>(ceil(rect.Height * scale)));
    if (source != &image || !bitmap || bitmap->GetWidth() != static_cast<UINT>(w) || bitmap->GetHeight() != static_cast<UINT>(h)) {
        Reset();
        auto next = make_unique<Bitmap>(w, h, PixelFormat32bppPARGB);
        if (next->GetLastStatus() == Ok) {
            Graphics cache(next.get()); cache.Clear(Color(0, 0, 0, 0));
            cache.SetInterpolationMode(InterpolationModeHighQualityBicubic); cache.SetPixelOffsetMode(PixelOffsetModeHalf);
            ImageAttributes attributes; attributes.SetWrapMode(WrapModeTileFlipXY);
            if (cache.DrawImage(&image, Rect(0, 0, w, h), 0, 0, static_cast<INT>(image.GetWidth()),
                static_cast<INT>(image.GetHeight()), UnitPixel, &attributes) == Ok) { bitmap = move(next); source = &image; }
        }
    }
    g.SetInterpolationMode(InterpolationModeBilinear);
    if (bitmap) g.DrawImage(bitmap.get(), rect, 0, 0, static_cast<REAL>(bitmap->GetWidth()), static_cast<REAL>(bitmap->GetHeight()), UnitPixel);
    else g.DrawImage(&image, rect);
}
void Prepare(Graphics& g, HWND hwnd) {
    g.Clear(background); g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    const auto s = Scale(hwnd); g.ScaleTransform(s, s);
    RECT r{}; GetClientRect(hwnd, &r);
    const float width = r.right / s, height = r.bottom / s;
    LinearGradientBrush wash(PointF(0, 0), PointF(width, height), Color(255, 32, 40, 49), background);
    g.FillRectangle(&wash, 0.0f, 0.0f, width, height);
    Border(g, {0.5f, 0.5f, width - 1, height - 1}, 10, Color(255, 63, 72, 85));
}
float FitScale(HWND hwnd, int width, int height, const RECT& work) {
    const float native = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
    const float scale = min({native, max(1.0f, static_cast<float>(work.right - work.left - 32)) / width,
        max(1.0f, static_cast<float>(work.bottom - work.top - 32)) / height});
    SetPropW(hwnd, L"ScreenShotPlus.UiScale", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(scale * 1024)));
    return Scale(hwnd);
}
void Place(HWND hwnd, int width, int height, bool corner, POINT point, unsigned cornerIndex) {
    if (!corner) GetCursorPos(&point);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &info);
    SetWindowPos(hwnd, nullptr, info.rcWork.left, info.rcWork.top, 0, 0,
        SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
    const auto s = FitScale(hwnd, width, height, info.rcWork);
    const int w = static_cast<int>(width * s), h = static_cast<int>(height * s);
    const int gap = static_cast<int>(20 * s);
    const bool left = cornerIndex == 1 || cornerIndex == 3, top = cornerIndex >= 2;
    int x = corner ? (left ? info.rcWork.left + gap : info.rcWork.right - w - gap) :
        info.rcWork.left + (info.rcWork.right - info.rcWork.left - w) / 2;
    int y = corner ? (top ? info.rcWork.top + gap : info.rcWork.bottom - h - gap) :
        info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - h) / 2;
    x = max(static_cast<int>(info.rcWork.left), x); y = max(static_cast<int>(info.rcWork.top), y);
    SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}
void DpiChanged(HWND hwnd, LPARAM value, int width, int height) {
    const RECT* suggested = reinterpret_cast<const RECT*>(value);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST), &info);
    const float scale = FitScale(hwnd, width, height, info.rcWork);
    const int w = static_cast<int>(width * scale), h = static_cast<int>(height * scale);
    const int x = clamp(static_cast<int>(suggested->left), static_cast<int>(info.rcWork.left), static_cast<int>(info.rcWork.right) - w);
    const int y = clamp(static_cast<int>(suggested->top), static_cast<int>(info.rcWork.top), static_cast<int>(info.rcWork.bottom) - h);
    SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(hwnd, nullptr, FALSE);
}
void Corners(HWND hwnd) {
    const DWORD rounded = 2; DwmSetWindowAttribute(hwnd, 33, &rounded, sizeof(rounded));
    const BOOL dark = TRUE; DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    const COLORREF border = RGB(63, 72, 85); DwmSetWindowAttribute(hwnd, 34, &border, sizeof(border));
}
filesystem::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &value))) return {};
    filesystem::path path(value); CoTaskMemFree(value); return path;
}
bool OpenFolder(const filesystem::path& path) {
    error_code ec; filesystem::create_directories(path, ec);
    if (ec) return false;
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}
bool PickFolder(HWND owner, filesystem::path& path) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return false;
    DWORD options = 0; dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"选择截图保存目录");
    IShellItem* initial = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&initial)))) {
        dialog->SetFolder(initial); initial->Release();
    }
    bool ok = false;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR name = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
                path = name; CoTaskMemFree(name); ok = true;
            }
            item->Release();
        }
    }
    dialog->Release(); return ok;
}
}
