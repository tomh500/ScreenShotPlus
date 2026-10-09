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
void SetMotionOptions(unsigned level, unsigned speed);
unsigned MotionLevel();
float MotionAmount();
double MotionDuration(double seconds);
bool DecorativeMotion(unsigned minimum = 1);
bool MotionMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
void RestartMotion(HWND hwnd, double seconds = 0.26);
void BeginMotionPaint(HWND hwnd);
void EndMotionPaint(HWND hwnd);
float Entrance(HWND hwnd);
float EntranceProgress(HWND hwnd);
float SpringReveal(float progress);
float AnimateValue(HWND hwnd, unsigned key, float target, double seconds, float initial = 0, bool always = false);
void ContinuousMotion(HWND hwnd, bool active);
void MotionMonitor(HWND hwnd, HMONITOR monitor);
// Physical client pixels. A null region restores full-window invalidation.
void MotionRegion(HWND hwnd, const RECT* region);
Interaction AnimateControl(HWND hwnd, Gdiplus::RectF rect, bool hovered, bool selected = false);
MotionInfo GetMotionInfo(HWND hwnd);
}
