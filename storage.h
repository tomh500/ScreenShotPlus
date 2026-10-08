#pragma once
#include "framework.h"
std::wstring ScreenshotStem(const SYSTEMTIME& time);
bool SavePng(Gdiplus::Bitmap& image, const std::filesystem::path& folder,
    std::filesystem::path& file, std::wstring& error);
