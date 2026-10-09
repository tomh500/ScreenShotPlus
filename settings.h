#pragma once
#include "config.h"
inline constexpr UINT WM_SETTINGS_SAVED = WM_APP + 3;
bool InitSettings(HINSTANCE instance, HWND owner, std::function<bool(const Config&, std::wstring&)> apply);
void ShowSettings(const Config& config);
void SetSettingsConfigFile(const std::filesystem::path& file);
void RecordSettingsHotkey(const Config& config);
void HideSettings();
bool SettingsVisible();
void DestroySettings();
struct SettingsState {
    Config working;
    unsigned tab = 0;
    bool sliderDragging = false;
    float sliderPosition = 0;
    std::wstring title;
};
SettingsState GetSettingsState();
