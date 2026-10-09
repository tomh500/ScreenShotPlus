#pragma once
#include "framework.h"
struct Config {
    std::filesystem::path folder;
    UINT modifiers = MOD_CONTROL | MOD_SHIFT, key = 'S';
    bool printScreen = false;
    unsigned previewSeconds = 0;
    // 开机自启动：写入 HKCU 的 Run 项，启动时带 --startup 直接驻留托盘。
    bool runAtStartup = false;
};
Config DefaultConfig();
std::filesystem::path ConfigFile();
bool ValidConfig(const Config& config);
Config LoadConfig(const std::filesystem::path& file, std::wstring& warning);
bool SaveConfig(const std::filesystem::path& file, const Config& config, std::wstring& error);
std::wstring HotkeyText(const Config& config);
