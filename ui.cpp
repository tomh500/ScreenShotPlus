#include "ui.h"
using namespace std;
using namespace Gdiplus;
namespace ui {
Paint::Paint(HWND hwnd) : window(hwnd) {
    dc = BeginPaint(window, &ps);
    RECT r{}; GetClientRect(window, &r);
    width = max(1L, r.right); height = max(1L, r.bottom);
    buffer = CreateCompatibleDC(dc);
    bitmap = CreateCompatibleBitmap(dc, width, height);
    if (buffer && bitmap) old = SelectObject(buffer, bitmap);
    else {
        if (bitmap) DeleteObject(bitmap);
        if (buffer) DeleteDC(buffer);
        bitmap = nullptr; buffer = dc;
    }
}
Paint::~Paint() {
    if (bitmap) {
        BitBlt(dc, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, old); DeleteObject(bitmap); DeleteDC(buffer);
    }
    EndPaint(window, &ps);
}
float Scale(HWND hwnd) { return static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f; }
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
    const float d = min(radius * 2, min(r.Width, r.Height));
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    path.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure(); SolidBrush brush(color); g.FillPath(&brush, &path);
}
void Button(Graphics& g, RectF r, const wstring& text, bool primary, bool hovered) {
    Round(g, r, 10, primary ? accent : (hovered ? Color(255, 47, 61, 76) : surface));
    const float inset = r.Width < 64 ? 6.0f : 16.0f;
    Text(g, text, {r.X + inset, r.Y + (r.Height - 22) / 2, r.Width - inset * 2, 24},
        14, primary ? background : foreground, primary);
}
void Prepare(Graphics& g, HWND hwnd) {
    g.Clear(background); g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    const auto s = Scale(hwnd); g.ScaleTransform(s, s);
}
void Place(HWND hwnd, int width, int height, bool corner, POINT point) {
    if (!corner) GetCursorPos(&point);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &info);
    SetWindowPos(hwnd, nullptr, info.rcWork.left, info.rcWork.top, 0, 0,
        SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
    const auto s = Scale(hwnd);
    const int w = static_cast<int>(width * s), h = static_cast<int>(height * s);
    const int gap = static_cast<int>(20 * s);
    int x = corner ? info.rcWork.right - w - gap : info.rcWork.left + (info.rcWork.right - info.rcWork.left - w) / 2;
    int y = corner ? info.rcWork.bottom - h - gap : info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - h) / 2;
    x = max(static_cast<int>(info.rcWork.left), x); y = max(static_cast<int>(info.rcWork.top), y);
    SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}
void Corners(HWND hwnd) {
    const DWORD rounded = 2; DwmSetWindowAttribute(hwnd, 33, &rounded, sizeof(rounded));
}
filesystem::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &value))) return {};
    filesystem::path path(value); CoTaskMemFree(value); return path;
}
void OpenFolder(const filesystem::path& path) {
    error_code ec; filesystem::create_directories(path, ec);
    if (!ec) ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
