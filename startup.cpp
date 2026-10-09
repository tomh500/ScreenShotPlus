#include "startup.h"
using namespace std;
namespace {
wstring RunCommand(const filesystem::path& executable) {
    wstring command = L"\"" + executable.wstring() + L"\" " + startup::kStartupArgument;
    return command;
}
bool RegQuery(HKEY key, const wchar_t* name, wstring& value) {
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS || type != REG_SZ || !bytes)
        return false;
    vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &bytes) != ERROR_SUCCESS)
        return false;
    value.assign(buffer.data());
    return true;
}
bool RegWrite(HKEY key, const wchar_t* name, const wstring& value, DWORD type, wstring& error) {
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    if (RegSetValueExW(key, name, 0, type, reinterpret_cast<const BYTE*>(value.c_str()), bytes) == ERROR_SUCCESS)
        return true;
    error = L"无法写入开机自启动设置；请检查当前账户对注册表的写入权限。";
    return false;
}
bool RegDelete(HKEY key, const wchar_t* name, wstring& error) {
    const LSTATUS status = RegDeleteValueW(key, name);
    if (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND) return true;
    error = L"无法关闭开机自启动；请检查当前账户对注册表的写入权限。";
    return false;
}
}
namespace startup {
bool Enabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    wstring value;
    const bool present = RegQuery(key, kValueName, value);
    RegCloseKey(key);
    return present;
}
bool Matches(const filesystem::path& executable) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    wstring value;
    const bool present = RegQuery(key, kValueName, value);
    RegCloseKey(key);
    return present && value == RunCommand(executable);
}
bool Apply(bool enabled, wstring& error) {
    const filesystem::path executable = Executable();
    if (!enabled) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return true;
        const bool ok = RegDelete(key, kValueName, error);
        RegCloseKey(key);
        return ok;
    }
    if (executable.empty() || !executable.is_absolute()) {
        error = L"无法确定程序位置，开机自启动未生效。";
        return false;
    }
    HKEY key = nullptr;
    const LSTATUS opened = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (opened != ERROR_SUCCESS) {
        error = L"无法打开启动项注册表键；请检查当前账户权限。";
        return false;
    }
    const bool ok = RegWrite(key, kValueName, RunCommand(executable), REG_SZ, error);
    RegCloseKey(key);
    return ok;
}
void Remove() { wstring ignored; Apply(false, ignored); }
filesystem::path Executable() {
    vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!length) return {};
        if (length < buffer.size() - 1) return filesystem::path(wstring(buffer.data(), length));
        if (buffer.size() >= 32768) return {};
        buffer.resize(buffer.size() * 2);
    }
}
}
