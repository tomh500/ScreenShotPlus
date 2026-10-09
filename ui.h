#pragma once
#include "framework.h"
#include "animation.h"
namespace ui {
inline const Gdiplus::Color background(255, 25, 29, 36), surface(255, 39, 44, 53);
inline const Gdiplus::Color accent(255, 105, 232, 187), foreground(255, 240, 245, 250);
inline const Gdiplus::Color muted(255, 163, 179, 195), danger(255, 255, 162, 149);
struct Paint {
    HWND window;
    PAINTSTRUCT ps{};
    HDC dc = nullptr, buffer = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old = nullptr;
    int width = 0, height = 0;
    bool printing = false, cached = false, reveal = true;
    explicit Paint(HWND hwnd, HDC target = nullptr, bool fade = true);
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
void Button(Gdiplus::Graphics& g, HWND hwnd, Gdiplus::RectF rect, const std::wstring& text,
    bool primary = false, bool hovered = false);
void Toggle(Gdiplus::Graphics& g, HWND hwnd, Gdiplus::RectF rect, const std::wstring& text,
    bool enabled, bool hovered);
void Border(Gdiplus::Graphics& g, Gdiplus::RectF rect, float radius, Gdiplus::Color color);
void ClipRound(Gdiplus::Graphics& g, Gdiplus::RectF rect, float radius);
void ReleasePaintBuffer(HWND hwnd);
struct ImageCache {
    std::unique_ptr<Gdiplus::Bitmap> bitmap;
    Gdiplus::Bitmap* source = nullptr;
    void Reset() { bitmap.reset(); source = nullptr; }
    void Draw(Gdiplus::Graphics& g, HWND hwnd, Gdiplus::Bitmap& image, Gdiplus::RectF rect);
};
void Prepare(Gdiplus::Graphics& g, HWND hwnd);
void Place(HWND hwnd, int width, int height, bool corner = false, POINT point = {});
void DpiChanged(HWND hwnd, LPARAM rect, int width, int height);
void Corners(HWND hwnd);
std::filesystem::path KnownFolder(REFKNOWNFOLDERID id);
bool OpenFolder(const std::filesystem::path& path);
bool PickFolder(HWND owner, std::filesystem::path& path);
}
