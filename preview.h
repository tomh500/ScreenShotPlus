#pragma once
#include "framework.h"
bool InitPreview(HINSTANCE instance, std::function<void()> drag);
void ShowPreview(Gdiplus::Bitmap* image, const std::filesystem::path& file, POINT point, unsigned seconds = 0);
void HidePreview();
bool PreviewVisible();
void DestroyPreview();
