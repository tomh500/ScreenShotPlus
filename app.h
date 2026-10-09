#pragma once
#include "framework.h"
#include "startup.h"
int RunApp(HINSTANCE instance, const std::filesystem::path& configFile = {}, bool trayStartup = false,
    const wchar_t* startupKey = STARTUP_KEY);
