#pragma once
#include "framework.h"
bool InitEditor(HINSTANCE instance, HWND owner,
    std::function<bool(std::unique_ptr<Gdiplus::Bitmap>&, std::wstring&)> save, std::function<void()> cancel);
bool ShowEditor(Gdiplus::Bitmap& image, std::wstring& error);
void FocusEditor();
bool EditorVisible();
void DestroyEditor();
