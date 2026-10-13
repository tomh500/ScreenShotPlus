#pragma once
#include "framework.h"
struct Config {
    std::filesystem::path folder;
    UINT modifiers = MOD_CONTROL | MOD_SHIFT, key = 'S';
    bool printScreen = false;
    bool startup = false;
    unsigned previewSeconds = 0;
    unsigned animationLevel = 3, animationSpeed = 1, accentColor = 0;
    unsigned previewCorner = 0, previewSize = 1, captureShade = 60;
    bool rightClickLocation = true, previewFilename = true, previewHints = true, hideAfterDrag = false;
    bool previewTopmost = true, previewMovable = true;
    bool captureAnimation = true, dragAnimation = true, startupAnimation = true, captureGuides = true;
};
Config DefaultConfig();
std::filesystem::path ConfigFile();
std::filesystem::path PortableConfigFile();
bool ValidConfig(const Config& config);
Config LoadConfig(const std::filesystem::path& file, std::wstring& warning);
bool SaveConfig(const std::filesystem::path& file, const Config& config, std::wstring& error);
bool SaveConfigWithFallback(const std::filesystem::path& file, const std::filesystem::path& fallback,
    const Config& config, std::filesystem::path& savedFile, std::wstring& error);
std::wstring HotkeyText(const Config& config);
