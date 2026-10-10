#pragma once
#include "ui.h"

// The selector has its own motion: reducing application animation never freezes
// the control that the user uses to choose that preference.
struct MotionSelector {
    unsigned level = 3;
    float position = 3;
    double labelTime = 0, ultimateTime = 0, engravingTime = 0;
    bool wasHovered = false;
    std::wstring previousTitle = L"极高", title = L"极高";
    void Reset(unsigned value);
    void Select(unsigned value);
    std::wstring CurrentTitle();
    bool Draw(Gdiplus::Graphics& graphics, HWND window, bool hovered, bool focused);
};

inline const Gdiplus::RectF motionSliderHit{44, 262, 412, 54};
unsigned MotionStop(int logicalX);
