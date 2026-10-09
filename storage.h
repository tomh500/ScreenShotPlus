#pragma once
#include "framework.h"
std::wstring ScreenshotStem(const SYSTEMTIME& time);
// 把 PNG 写进新文件；文件名包含日期时间，已存在的文件不会被覆盖。
bool SavePng(Gdiplus::Bitmap& image, const std::filesystem::path& folder,
    std::filesystem::path& file, std::wstring& error);
// 把 PNG 原子写回既有路径：先写入同目录临时文件，成功后再替换，失败时原文件保持不变。
bool ReplacePng(Gdiplus::Bitmap& image, const std::filesystem::path& file, std::wstring& error);
