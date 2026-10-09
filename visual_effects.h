#pragma once
#include "framework.h"

// UI-thread effects never own the screenshot file or alter OLE hit testing.
bool FlyToPreview(Gdiplus::Bitmap& image, RECT origin, RECT destination);
bool BeginDragVisual(Gdiplus::Bitmap& image, RECT origin);
void DragVisualFeedback(DWORD effect);
void EndDragVisual(bool accepted);
void CancelVisualEffects();
struct VisualEffectInfo {
    HWND window = nullptr;
    bool dragging = false, returning = false, presented = false;
    DWORD effect = DROPEFFECT_NONE, error = 0;
};
VisualEffectInfo GetVisualEffectInfo();
