#pragma once
#include "framework.h"
RECT SelectionRect(POINT a, POINT b, int width, int height);
using CaptureDone = std::function<void(std::unique_ptr<Gdiplus::Bitmap>, POINT, std::wstring)>;
void ConfigureCapture(unsigned shade, bool guides, bool animate);
bool BeginSelection(HINSTANCE instance, CaptureDone done, std::wstring& error);
void CancelSelection();
RECT LastCaptureRect();
