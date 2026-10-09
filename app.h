#pragma once
#include "framework.h"
// 自绘主面板的逻辑尺寸；高 DPI 下由 ui::Place / ui::DpiChanged 等比缩放。
inline constexpr int PANEL_WIDTH = 420, PANEL_HEIGHT = 570;
// startup 为真时（--startup）不弹出面板，直接驻留托盘。
int RunApp(HINSTANCE instance, bool startup, const std::filesystem::path& configFile = {});
