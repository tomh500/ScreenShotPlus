#pragma once
#include "framework.h"
namespace ui {
inline const Gdiplus::Color background(255, 20, 26, 35), surface(255, 33, 42, 54);
inline const Gdiplus::Color accent(255, 105, 232, 187), foreground(255, 240, 245, 250);
inline const Gdiplus::Color muted(255, 163, 179, 195), danger(255, 255, 162, 149);
struct Paint {
    HWND window;
    PAINTSTRUCT ps{};
    HDC dc = nullptr, buffer = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old = nullptr;
    int width = 0, height = 0;
    explicit Paint(HWND hwnd);
    ~Paint();
    Paint(const Paint&) = delete;
    Paint& operator=(const Paint&) = delete;
};
float Scale(HWND hwnd);
POINT Mouse(HWND hwnd, LPARAM value);
bool Inside(POINT p, const Gdiplus::RectF& rect);
void Text(Gdiplus::Graphics& g, const std::wstring& text, Gdiplus::RectF rect,
    float size = 14, Gdiplus::Color color = foreground, bool bold = false);
void Round(Gdiplus::Graphics& g, Gdiplus::RectF rect, float radius, Gdiplus::Color color);
void Button(Gdiplus::Graphics& g, Gdiplus::RectF rect, const std::wstring& text,
    bool primary = false, bool hovered = false);
void Prepare(Gdiplus::Graphics& g, HWND hwnd);
void Place(HWND hwnd, int width, int height, bool corner = false, POINT point = {});
// 按逻辑尺寸定位窗口，自动适应显示器工作区与 DPI，并夹取到可见范围内。
void PlaceAt(HWND hwnd, int x, int y, int width, int height, POINT anchor);
void DpiChanged(HWND hwnd, LPARAM rect, int width, int height);
void Corners(HWND hwnd);
std::filesystem::path KnownFolder(REFKNOWNFOLDERID id);
bool OpenFolder(const std::filesystem::path& path);
bool PickFolder(HWND owner, std::filesystem::path& path);
}
