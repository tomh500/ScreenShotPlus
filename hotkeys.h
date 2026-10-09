#pragma once
#include "config.h"
struct Hotkeys {
    int primaryId = 1;
    bool primaryRegistered = false, printRegistered = false;
};
std::wstring RegisterConfiguredHotkeys(HWND window, Hotkeys& state, const Config& config);
bool ApplyConfiguredSettings(HWND window, Hotkeys& state, Config& current, const Config& next,
    const std::filesystem::path& file, std::wstring& error);
void ReleaseHotkeys(HWND window, const Hotkeys& state);
