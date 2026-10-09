#pragma once
#include "config.h"
inline constexpr UINT WM_SETTINGS_SAVED = WM_APP + 3;
// 自绘设置面板的逻辑尺寸；高 DPI 下由 ui::Place / ui::DpiChanged 等比缩放。
inline constexpr int SETTINGS_WIDTH = 500, SETTINGS_HEIGHT = 706;
// 缩略图计时行的逻辑 Y 坐标；保存按钮紧贴面板底部，测试据此换算点击位置。
inline constexpr int SETTINGS_SECONDS_ROW_Y = 450;
// 自绘按钮的逻辑矩形。测试据此定位按钮，避免在别处重复实现缩放换算。
enum class SettingsButton { Seconds, Startup, Save, Cancel };
Gdiplus::RectF SettingsButtonRect(SettingsButton button);
bool InitSettings(HINSTANCE instance, HWND owner, std::function<bool(const Config&, std::wstring&)> apply);
void ShowSettings(const Config& config);
void RecordSettingsHotkey(const Config& config);
void HideSettings();
bool SettingsVisible();
void DestroySettings();
