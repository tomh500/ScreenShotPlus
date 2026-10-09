#include "config.h"
#include "ui.h"
#include <nlohmann/json.hpp>
using namespace std;
using nlohmann::json;
Config DefaultConfig() {
    Config config;
    config.folder = ui::KnownFolder(FOLDERID_Pictures) / L"ScreenShotPlus";
    return config;
}
filesystem::path ConfigFile() { return ui::KnownFolder(FOLDERID_LocalAppData) / L"ScreenShotPlus" / L"config.json"; }
bool ValidConfig(const Config& c) {
    const bool key = (c.key >= 'A' && c.key <= 'Z') || (c.key >= '0' && c.key <= '9') ||
        (c.key >= VK_F1 && c.key <= VK_F24);
    const auto path = c.folder.native();
    return c.folder.is_absolute() && !path.empty() && path.find(L'\0') == wstring::npos && key &&
        c.modifiers && !(c.modifiers & ~(MOD_CONTROL | MOD_SHIFT | MOD_ALT)) && c.previewSeconds <= 3600;
}
Config LoadConfig(const filesystem::path& file, wstring& warning) {
    Config result = DefaultConfig();
    error_code ec;
    if (!filesystem::exists(file, ec)) {
        if (ec) warning = L"无法读取配置目录，已使用默认设置。";
        return result;
    }
    try {
        if (filesystem::file_size(file) > 65536) throw runtime_error("config too large");
        ifstream input(file, ios::binary);
        const auto data = json::parse(input);
        Config parsed;
        const string path = data.at("folder").get<string>();
        parsed.folder = filesystem::path(u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
        if (!data.at("modifiers").is_number_integer() || !data.at("key").is_number_integer() ||
            !data.at("previewSeconds").is_number_integer()) throw runtime_error("non-integer config");
        // Read signed values first, so negative or oversized JSON numbers cannot wrap.
        const auto modifiers = data.at("modifiers").get<int64_t>();
        const auto key = data.at("key").get<int64_t>();
        const auto seconds = data.at("previewSeconds").get<int64_t>();
        if (modifiers <= 0 || modifiers > 7 || key < 0 || key > 255 || seconds < 0 || seconds > 3600)
            throw runtime_error("invalid config range");
        parsed.modifiers = static_cast<UINT>(modifiers); parsed.key = static_cast<UINT>(key);
        parsed.previewSeconds = static_cast<unsigned>(seconds);
        parsed.printScreen = data.at("printScreen").get<bool>();
        // 旧配置没有该字段，缺省为关闭，保证向后兼容。
        parsed.runAtStartup = data.value("runAtStartup", false);
        if (!ValidConfig(parsed)) throw runtime_error("invalid config");
        result = move(parsed);
    } catch (const exception&) { warning = L"配置损坏或无效，已使用默认设置；原文件保留到下次保存设置。"; }
    return result;
}
bool SaveConfig(const filesystem::path& file, const Config& c, wstring& error) {
    if (!ValidConfig(c)) { error = L"设置无效：请选择完整目录和带 Ctrl / Alt / Shift 的组合键。"; return false; }
    filesystem::path temporary = file; temporary += L".tmp";
    error_code ec;
    try {
        filesystem::create_directories(file.parent_path());
        const auto path = c.folder.u8string();
        const json data{{"folder", string(reinterpret_cast<const char*>(path.data()), path.size())},
            {"modifiers", c.modifiers}, {"key", c.key}, {"printScreen", c.printScreen},
            {"previewSeconds", c.previewSeconds}, {"runAtStartup", c.runAtStartup}};
        ofstream output(temporary, ios::binary | ios::trunc);
        output << data.dump(2) << '\n'; output.close();
        if (!output) throw runtime_error("config write failed");
        filesystem::rename(temporary, file);
        return true;
    } catch (const exception&) {
        filesystem::remove(temporary, ec);
        error = L"无法保存设置；原设置仍然有效，请检查配置目录权限。";
        return false;
    }
}
wstring HotkeyText(const Config& c) {
    wstring text;
    if (c.modifiers & MOD_CONTROL) text += L"Ctrl + ";
    if (c.modifiers & MOD_ALT) text += L"Alt + ";
    if (c.modifiers & MOD_SHIFT) text += L"Shift + ";
    if (c.key >= VK_F1 && c.key <= VK_F24) text += L"F" + to_wstring(c.key - VK_F1 + 1);
    else text += static_cast<wchar_t>(c.key);
    return text;
}
