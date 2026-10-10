#pragma once
#include "config.h"
bool InitPreview(HINSTANCE instance, std::function<void()> drag, std::function<void()> edit = {}, std::function<void()> locate = {});
void ConfigurePreview(const Config& config);
void ConfigurePreview(unsigned corner, unsigned size, bool captureAnimation, bool dragAnimation = true);
void ShowPreview(Gdiplus::Bitmap* image, const std::filesystem::path& file, POINT point, unsigned seconds = 0, RECT origin = {});
RECT PreviewImageScreenRect();
void PreviewDragResult(bool accepted);
void HidePreview();
bool PreviewVisible();
void DestroyPreview();
