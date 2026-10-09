#pragma once
#include "config.h"
#include "startup.h"
struct Hotkeys {
    int primaryId = 1;
    bool primaryRegistered = false, printRegistered = false;
};
std::wstring RegisterConfiguredHotkeys(HWND window, Hotkeys& state, const Config& config);
bool ApplyConfiguredSettings(HWND window, Hotkeys& state, Config& current, const Config& next,
    const std::filesystem::path& file, std::wstring& error, const wchar_t* startupKey = STARTUP_KEY,
    const std::filesystem::path& fallback = {}, std::filesystem::path* savedFile = nullptr);
void ReleaseHotkeys(HWND window, const Hotkeys& state);
