#pragma once
#include "framework.h"

inline constexpr wchar_t LaunchPoem[] = L"危楼高百尺，手可摘星辰";
inline constexpr wchar_t LaunchVoyage[] = L"我的征途是星辰大海";
struct LaunchFrame {
    float progress = 0, reveal = 0, depart = 0, poem = 1, voyage = 0;
    unsigned phase = 1;
    bool finished = false;
    bool secret = false;
};
double LaunchSeconds(unsigned level);
LaunchFrame LaunchScene(float progress, bool ready);
void DrawLaunchScene(Gdiplus::Graphics& g, const LaunchFrame& frame, unsigned level);
void DrawExitScene(Gdiplus::Graphics& g, float progress, bool secret);
