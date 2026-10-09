#pragma once
#include "framework.h"
// drag 在拖出图片时调用；edit 在单击图片（未越过拖拽阈值）时调用，用于进入标注编辑。
bool InitPreview(HINSTANCE instance, std::function<void()> drag, std::function<void()> edit);
void ShowPreview(Gdiplus::Bitmap* image, const std::filesystem::path& file, POINT point, unsigned seconds = 0);
void HidePreview();
bool PreviewVisible();
void DestroyPreview();
