#pragma once
#include "framework.h"
struct Config {
    std::filesystem::path folder;
    UINT modifiers = MOD_CONTROL | MOD_SHIFT, key = 'S';
    bool printScreen = false;
    unsigned previewSeconds = 0;
};
Config DefaultConfig();
std::filesystem::path ConfigFile();
bool ValidConfig(const Config& config);
Config LoadConfig(const std::filesystem::path& file, std::wstring& warning);
bool SaveConfig(const std::filesystem::path& file, const Config& config, std::wstring& error);
std::wstring HotkeyText(const Config& config);
