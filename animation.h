#pragma once
#include "framework.h"

namespace ui {
// Time-based curves keep the same duration at 60, 120, 144 and fractional Hz.
struct Tween {
    float from = 0, target = 0;
    double start = 0, duration = 0;
    float Value(double now) const;
    bool Active(double now) const;
    void Set(float value, double now, double seconds, bool animate = true);
};
struct Interaction {
    float hover = 0, press = 0, selected = 0;
};
struct MotionInfo {
    double refreshRate = 120;
    unsigned long long frames = 0;
    bool enabled = false, running = false;
};
double RefreshRate(HWND hwnd);
double RefreshRateValue(UINT32 numerator, UINT32 denominator);
bool MotionMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
void RestartMotion(HWND hwnd);
void BeginMotionPaint(HWND hwnd);
void EndMotionPaint(HWND hwnd);
float Entrance(HWND hwnd);
Interaction AnimateControl(HWND hwnd, Gdiplus::RectF rect, bool hovered, bool selected = false);
MotionInfo GetMotionInfo(HWND hwnd);
}
