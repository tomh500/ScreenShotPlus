#include "startup.h"
using namespace std;
namespace {
constexpr wchar_t valueName[] = L"ScreenShotPlus";
bool RegistryError(LSTATUS status, wstring& error) {
    if (status == ERROR_SUCCESS) return true;
    error = L"无法更新当前用户的开机启动设置（错误 " + to_wstring(status) + L"）。";
    return false;
}
}
bool ReadStartupEntry(const wchar_t* key, StartupEntry& entry, wstring& error) {
    entry = {};
    HKEY handle = nullptr;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_QUERY_VALUE, &handle);
    if (status == ERROR_FILE_NOT_FOUND) return true;
    if (!RegistryError(status, error)) { error += L" 读取启动项。"; return false; }
    DWORD size = 0;
    status = RegQueryValueExW(handle, valueName, nullptr, &entry.type, nullptr, &size);
    if (status == ERROR_FILE_NOT_FOUND) { RegCloseKey(handle); return true; }
    if (status == ERROR_SUCCESS) {
        entry.data.resize(size);
        status = RegQueryValueExW(handle, valueName, nullptr, &entry.type, entry.data.data(), &size);
        if (status == ERROR_SUCCESS) { entry.data.resize(size); entry.exists = true; }
    }
    RegCloseKey(handle); return RegistryError(status, error);
}
bool WriteStartupEntry(const wchar_t* key, const StartupEntry& entry, wstring& error) {
    HKEY handle = nullptr;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_SET_VALUE, &handle);
    if (entry.exists && status == ERROR_FILE_NOT_FOUND)
        status = RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr);
    if (!entry.exists && status == ERROR_FILE_NOT_FOUND) return true;
    if (!RegistryError(status, error)) return false;
    status = entry.exists ? RegSetValueExW(handle, valueName, 0, entry.type, entry.data.data(),
        static_cast<DWORD>(entry.data.size())) : RegDeleteValueW(handle, valueName);
    RegCloseKey(handle);
    return RegistryError(status == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : status, error);
}
wstring StartupCommand(const filesystem::path& executable, wstring& error) {
    const wstring command = L"\"" + executable.wstring() + L"\" --tray";
    if (!executable.is_absolute() || command.size() > 260 || executable.native().find_first_of(L"\"\r\n") != wstring::npos ||
        executable.native().find(L'\0') != wstring::npos) {
        error = L"程序路径无效或过长，请将 EXE 移到较短的完整路径后启用开机启动。";
        return {};
    }
    return command;
}
bool SetStartup(const wchar_t* key, bool enabled, wstring& error) {
    StartupEntry entry;
    if (enabled) {
        wstring executable(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!length || length >= executable.size()) { error = L"无法取得程序路径，开机启动设置未更改。"; return false; }
        executable.resize(length);
        const wstring command = StartupCommand(filesystem::path(executable), error);
        if (command.empty()) return false;
        entry.exists = true; entry.data.resize((command.size() + 1) * sizeof(wchar_t));
        memcpy(entry.data.data(), command.c_str(), entry.data.size());
    }
    return WriteStartupEntry(key, entry, error);
}
bool TrayStartupRequested() {
    int count = 0; LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return false;
    bool tray = false;
    for (int i = 1; i < count; ++i) if (wstring_view(args[i]) == L"--tray") tray = true;
    LocalFree(args); return tray;
}
