#pragma once
#include "framework.h"
inline constexpr wchar_t STARTUP_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
struct StartupEntry {
    bool exists = false;
    DWORD type = REG_SZ;
    std::vector<BYTE> data;
};
bool ReadStartupEntry(const wchar_t* key, StartupEntry& entry, std::wstring& error);
bool WriteStartupEntry(const wchar_t* key, const StartupEntry& entry, std::wstring& error);
bool SetStartup(const wchar_t* key, bool enabled, std::wstring& error);
std::wstring StartupCommand(const std::filesystem::path& executable, std::wstring& error);
bool TrayStartupRequested();
