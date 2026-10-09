#pragma once
#include "framework.h"
HRESULT CreateFileDataObject(const std::filesystem::path& file, IDataObject** result);
HRESULT DragFile(const std::filesystem::path& file, DWORD& effect, std::function<void(DWORD)> feedback = {});
