#pragma once
#include "framework.h"
// 开机自启动通过 HKCU 的 Run 项实现，无需管理员权限，写入的值为带引号的 EXE 绝对路径加 --startup 参数。
namespace startup {
inline constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr wchar_t kValueName[] = L"ScreenShotPlus";
inline constexpr wchar_t kStartupArgument[] = L"--startup";
std::filesystem::path Executable();
bool Enabled();
bool Matches(const std::filesystem::path& executable);
bool Apply(bool enabled, std::wstring& error);
void Remove();
}
